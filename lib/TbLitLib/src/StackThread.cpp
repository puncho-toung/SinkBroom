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

#include <cassert>
#include <exception>
#include <memory>
#include <system_error>
#include <utility>

namespace tb::lit
{
namespace
{

#if !defined(_WIN32)
void* runBody(void* argument)
{
  const auto body =
    std::unique_ptr<std::function<void()>>{static_cast<std::function<void()>*>(argument)};
  (*body)();
  return nullptr;
}
#endif

} // namespace

StackThread::~StackThread()
{
  assert(!joinable());
}

bool StackThread::start(
  std::function<void()> body,
  [[maybe_unused]] const std::size_t posixStackBytes,
  std::string* error)
{
#if defined(_WIN32)
  try
  {
    m_thread = std::thread{std::move(body)};
    return true;
  }
  catch (const std::system_error& e)
  {
    if (error)
    {
      *error = e.what();
    }
    return false;
  }
#else
  auto attributes = pthread_attr_t{};
  if (pthread_attr_init(&attributes) != 0)
  {
    if (error)
    {
      *error = "pthread_attr_init failed";
    }
    return false;
  }
  auto ok = pthread_attr_setstacksize(&attributes, posixStackBytes) == 0;
  if (!ok && error)
  {
    *error = "pthread_attr_setstacksize failed";
  }
  if (ok)
  {
    auto argument = std::make_unique<std::function<void()>>(std::move(body));
    ok = pthread_create(&m_thread, &attributes, runBody, argument.get()) == 0;
    if (ok)
    {
      // the thread owns it now
      static_cast<void>(argument.release());
      m_started = true;
    }
    else if (error)
    {
      *error = "pthread_create failed";
    }
  }
  pthread_attr_destroy(&attributes);
  return ok;
#endif
}

bool StackThread::joinable() const
{
#if defined(_WIN32)
  return m_thread.joinable();
#else
  return m_started;
#endif
}

void StackThread::join()
{
#if defined(_WIN32)
  if (m_thread.joinable())
  {
    m_thread.join();
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
