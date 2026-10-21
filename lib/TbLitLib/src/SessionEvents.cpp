/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SessionEvents.h"

#include <fmt/format.h>

#include <utility>

namespace tb::lit
{
namespace
{

constexpr auto MaxWarnings = std::size_t{20};

} // namespace

SessionEvents::SessionEvents(EventSink ownerSink, const std::uint64_t generation)
  : m_ownerSink{std::move(ownerSink)}
  , m_generation{generation}
{
}

std::uint64_t SessionEvents::generation() const
{
  return m_generation;
}

void SessionEvents::post(const SessionEvent event, std::string message) noexcept
{
  try
  {
    if (event == SessionEvent::Warning)
    {
      const auto lock = std::scoped_lock{m_mutex};
      if (m_warnings.size() >= MaxWarnings || !m_warnings.insert(message).second)
      {
        return;
      }
      if (m_warnings.size() == MaxWarnings)
      {
        message = fmt::format(
          "{} (further warnings of this session are not shown)", std::move(message));
      }
    }

    if (!m_stopping)
    {
      if (m_ownerSink)
      {
        m_ownerSink(event, std::move(message), m_generation);
      }
      return;
    }

    if (
      event == SessionEvent::Warning || event == SessionEvent::Error
      || event == SessionEvent::DeviceLost)
    {
      const auto lock = std::scoped_lock{m_mutex};
      m_teardownMessages.push_back(std::move(message));
    }
  }
  catch (...)
  {
    // a message that cannot be posted is lost, not the session
  }
}

void SessionEvents::markLost(const std::string_view reason) noexcept
{
  m_lost = true;
  if (!m_lostReported.exchange(true))
  {
    try
    {
      post(
        SessionEvent::DeviceLost,
        fmt::format(
          "The GPU device was lost ({}). Press Start to run the preview again.", reason));
    }
    catch (...)
    {
      // the loss stands; only its message is lost
    }
  }
}

void SessionEvents::setLost() noexcept
{
  m_lost = true;
}

bool SessionEvents::lost() const
{
  return m_lost;
}

bool SessionEvents::lostReported() const
{
  return m_lostReported;
}

void SessionEvents::markFailed(const std::string_view reason) noexcept
{
  if (!m_failed.exchange(true))
  {
    try
    {
      post(
        SessionEvent::Error,
        fmt::format("The lit preview stopped: {}. Stop and Start it again.", reason));
    }
    catch (...)
    {
      // the failure stands; only its message is lost
    }
  }
}

bool SessionEvents::failed() const
{
  return m_failed;
}

void SessionEvents::beginStop() noexcept
{
  m_stopping = true;
}

std::vector<std::string> SessionEvents::takeTeardownMessages()
{
  const auto lock = std::scoped_lock{m_mutex};
  return std::exchange(m_teardownMessages, {});
}

} // namespace tb::lit
