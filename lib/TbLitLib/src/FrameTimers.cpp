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

#include "FrameTimers.h"

namespace tb::lit
{

void FrameTimers::createQueries(nvrhi::IDevice& device, const bool timerQueries)
{
  if (timerQueries)
  {
    for (auto& slot : m_slots)
    {
      slot.query = device.createTimerQuery();
    }
  }
}

std::optional<std::string> FrameTimers::openLog(const std::filesystem::path& path)
{
  return m_log.open(path);
}

nvrhi::ITimerQuery* FrameTimers::query(const std::size_t slot) const
{
  return m_slots[slot].query.Get();
}

void FrameTimers::frameSubmitted(const std::size_t slot)
{
  m_slots[slot].pending = bool(m_slots[slot].query);
}

void FrameTimers::frameShown(const std::size_t slot, const FrameLogRow& row)
{
  if (m_slots[slot].pending)
  {
    m_slots[slot].row = row;
  }
  else
  {
    m_log.append(row);
  }
}

bool FrameTimers::pending(const std::size_t slot) const
{
  return m_slots[slot].pending;
}

void FrameTimers::resolve(nvrhi::IDevice& device, const std::size_t slot)
{
  auto& s = m_slots[slot];
  if (!s.pending)
  {
    return;
  }
  const auto seconds = device.getTimerQueryTime(s.query);
  s.pending = false;
  if (s.row)
  {
    s.row->gpuClearMs = double(seconds) * 1000.0;
    m_log.append(*s.row);
    s.row.reset();
  }
}

void FrameTimers::resolveAll(nvrhi::IDevice& device, const std::uint64_t nextFrameIndex)
{
  for (std::size_t i = 0; i < FramesInFlight; ++i)
  {
    resolve(device, std::size_t((nextFrameIndex + i) % FramesInFlight));
  }
}

std::optional<std::string> FrameTimers::closeLog(const std::uint64_t nextFrameIndex)
{
  for (std::size_t i = 0; i < FramesInFlight; ++i)
  {
    auto& s = m_slots[std::size_t((nextFrameIndex + i) % FramesInFlight)];
    if (s.row)
    {
      m_log.append(*s.row);
      s.row.reset();
    }
  }
  return m_log.close();
}

void FrameTimers::releaseQueries()
{
  for (auto& slot : m_slots)
  {
    slot = Slot{};
  }
}

} // namespace tb::lit
