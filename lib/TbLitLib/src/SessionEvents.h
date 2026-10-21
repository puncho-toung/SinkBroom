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

#pragma once

#include "base/Macros.h"
#include "lit/LitSession.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

/**
 * A session's messages to its owner and its lost and failed states; any thread.
 *
 * Posting never throws: a message that cannot be posted is lost, not the session. Once
 * the stop is requested, warnings and errors go to the teardown report instead, because
 * no owner acts on them any more (R5.17).
 */
class SessionEvents
{
private:
  EventSink m_ownerSink;
  std::uint64_t m_generation;

  std::atomic<bool> m_lost = false;
  std::atomic<bool> m_lostReported = false;
  std::atomic<bool> m_failed = false;
  std::atomic<bool> m_stopping = false;

  std::mutex m_mutex;
  std::vector<std::string> m_teardownMessages;

public:
  SessionEvents(EventSink ownerSink, std::uint64_t generation);

  std::uint64_t generation() const;

  void post(SessionEvent event, std::string message) noexcept;

  // DeviceLost, once; nothing more is drawn
  void markLost(std::string_view reason) noexcept;
  // the loss is known but not reported yet, as NVRHI's message callback finds it
  void setLost() noexcept;
  bool lost() const;
  bool lostReported() const;

  // Error, once; nothing more is drawn
  void markFailed(std::string_view reason) noexcept;
  bool failed() const;

  // from now on warnings and errors go to the teardown report
  void beginStop() noexcept;
  std::vector<std::string> takeTeardownMessages();

  deleteCopyAndMove(SessionEvents);
};

} // namespace tb::lit
