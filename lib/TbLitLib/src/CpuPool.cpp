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

#include "lit/CpuPool.h"

#include "kd/task_manager.h"

#include <system_error>

namespace tb::lit
{

CpuJobContext::CpuJobContext(
  std::shared_ptr<const std::atomic<std::uint64_t>> current,
  const std::uint64_t generation)
  : m_current{std::move(current)}
  , m_generation{generation}
{
}

bool CpuJobContext::stale() const
{
  return m_current->load() != m_generation;
}

std::uint64_t CpuJobContext::generation() const
{
  return m_generation;
}

CpuPool::CpuPool()
  : m_generation{std::make_shared<std::atomic<std::uint64_t>>(0)}
  , m_tasks{std::make_unique<kdl::task_manager>(ThreadCount)}
{
}

Result<std::unique_ptr<CpuPool>> CpuPool::create()
{
  try
  {
    return std::unique_ptr<CpuPool>{new CpuPool{}};
  }
  catch (const std::system_error& e)
  {
    // a thread that cannot be made; the task manager joins the ones it made
    return Error{std::string{"The CPU pool's threads could not be started: "} + e.what()};
  }
}

CpuPool::~CpuPool()
{
  stop();
}

bool CpuPool::post(std::function<bool()> task)
{
  const auto lock = std::scoped_lock{m_mutex};
  if (!m_tasks)
  {
    return false;
  }
  // the task sets its own promise; the task manager's future is not needed
  static_cast<void>(m_tasks->run_task(std::move(task)));
  return true;
}

void CpuPool::stop()
{
  auto tasks = std::unique_ptr<kdl::task_manager>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    // every job made so far is stale from here on
    ++*m_generation;
    tasks = std::move(m_tasks);
  }
  // outside the lock: joins the threads once their running jobs return; the jobs that
  // had not started are dropped with the task manager, which breaks their futures
  tasks.reset();
}

bool CpuPool::stopped() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_tasks == nullptr;
}

std::uint64_t CpuPool::generation() const
{
  return m_generation->load();
}

} // namespace tb::lit
