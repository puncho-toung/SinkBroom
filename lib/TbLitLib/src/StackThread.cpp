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

#include "StackThread.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#endif

#include "kd/result.h"

#include <fmt/format.h>

#include <cassert>
#include <cerrno>
#include <memory>
#include <utility>

namespace tb::lit
{
namespace
{

using Body = std::function<void()>;

#if defined(_WIN32)
unsigned __stdcall runBody(void* argument)
{
  const auto body = std::unique_ptr<Body>{static_cast<Body*>(argument)};
  (*body)();
  return 0;
}
#else
void* runBody(void* argument)
{
  const auto body = std::unique_ptr<Body>{static_cast<Body*>(argument)};
  (*body)();
  return nullptr;
}
#endif

} // namespace

StackThread::~StackThread()
{
  assert(!joinable());
}

Result<void> StackThread::start(Body body, const std::size_t stackBytes)
{
  auto argument = std::make_unique<Body>(std::move(body));

#if defined(_WIN32)
  const auto handle = _beginthreadex(
    nullptr,
    unsigned(stackBytes),
    runBody,
    argument.get(),
    STACK_SIZE_PARAM_IS_A_RESERVATION,
    nullptr);
  if (handle == 0)
  {
    return Error{fmt::format("_beginthreadex failed (errno {})", errno)};
  }
  m_handle = reinterpret_cast<void*>(handle);
#else
  auto attributes = pthread_attr_t{};
  if (pthread_attr_init(&attributes) != 0)
  {
    return Error{"pthread_attr_init failed"};
  }
  const auto sized = pthread_attr_setstacksize(&attributes, stackBytes) == 0;
  const auto created =
    sized && pthread_create(&m_thread, &attributes, runBody, argument.get()) == 0;
  pthread_attr_destroy(&attributes);
  if (!sized)
  {
    return Error{"pthread_attr_setstacksize failed"};
  }
  if (!created)
  {
    return Error{"pthread_create failed"};
  }
  m_started = true;
#endif

  // the thread owns it now
  static_cast<void>(argument.release());
  return kdl::void_success;
}

bool StackThread::joinable() const
{
#if defined(_WIN32)
  return m_handle != nullptr;
#else
  return m_started;
#endif
}

void StackThread::join()
{
#if defined(_WIN32)
  if (m_handle)
  {
    WaitForSingleObject(m_handle, INFINITE);
    CloseHandle(m_handle);
    m_handle = nullptr;
  }
#else
  if (m_started)
  {
    pthread_join(m_thread, nullptr);
    m_started = false;
  }
#endif
}

} // namespace tb::lit
