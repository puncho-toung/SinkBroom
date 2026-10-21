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

#include <cstddef>
#include <functional>
#include <string>

#if defined(_WIN32)
#include <thread>
#else
#include <pthread.h>
#endif

namespace tb::lit
{

/**
 * A thread with a known stack (R4-1.1, R4-3.25). On Windows it is a std::thread, which
 * gets the executable's stack reserve (8 MB for TrenchBroom, /STACK in its CMakeLists).
 * Elsewhere it is a pthread with posixStackBytes, because a macOS secondary thread gets
 * only 512 KB. The owner joins it before destroying it.
 */
class StackThread
{
private:
#if defined(_WIN32)
  std::thread m_thread;
#else
  pthread_t m_thread{};
  bool m_started = false;
#endif

public:
  StackThread() = default;
  ~StackThread();

  StackThread(const StackThread&) = delete;
  StackThread& operator=(const StackThread&) = delete;

  // false, with the reason, when the thread could not be created
  bool start(std::function<void()> body, std::size_t posixStackBytes, std::string* error);
  bool joinable() const;
  void join();
};

} // namespace tb::lit
