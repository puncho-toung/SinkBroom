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

#include "base/Result.h"

#include <cstddef>
#include <functional>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace tb::lit
{

/**
 * A thread with an explicit stack size (R4-1.1, R4-3.25, G13), the same on every platform
 * and whatever the executable reserves: neither std::thread nor TrenchBroom can set one,
 * and Qt is not used here (D28). On Windows it is _beginthreadex with the size as a
 * reservation, elsewhere a pthread with the size set. The owner joins it before
 * destroying it.
 *
 * An exception that leaves the body ends the thread, never the process (W.5): escaped,
 * when given, is called with its reason on the thread, and what escaped throws is
 * dropped.
 */
class StackThread
{
private:
#if defined(_WIN32)
  void* m_handle = nullptr;
#else
  pthread_t m_thread{};
  bool m_started = false;
#endif

public:
  StackThread() = default;
  ~StackThread();

  StackThread(const StackThread&) = delete;
  StackThread& operator=(const StackThread&) = delete;

  // fails when the thread cannot be created
  Result<void> start(
    std::function<void()> body,
    std::size_t stackBytes,
    std::function<void(const char* reason)> escaped = {});
  bool joinable() const;
  void join();
};

} // namespace tb::lit
