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

#include "StackThread.h"

#include "kd/result.h"

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
  , m_shared{std::make_unique<Shared>()}
{
}

Result<void> CpuPool::startThreads()
{
  auto result = Result<void>{kdl::void_success};
  for (auto i = std::size_t{0}; i < ThreadCount; ++i)
  {
    result =
      std::move(result) | kdl::and_then([&]() {
        m_threads[i] = std::make_unique<StackThread>();
        return m_threads[i]->start([this, i] { work(i); }, StackBytes)
               | kdl::or_else([&](Error e) -> Result<void> {
                   // D66, B0.2: a thread that was not made is not joined
                   m_threads[i].reset();
                   return Error{"The CPU pool's threads could not be started: " + e.msg};
                 });
      });
  }
  return result;
}

Result<std::unique_ptr<CpuPool>> CpuPool::create()
{
  auto pool = std::unique_ptr<CpuPool>{new CpuPool{}};
  return pool->startThreads() | kdl::transform([&]() { return std::move(pool); })
         | kdl::or_else([&](Error e) -> Result<std::unique_ptr<CpuPool>> {
             // the threads made so far are stopped and joined; nothing terminates
             pool->stop();
             return e;
           });
}

CpuPool::~CpuPool()
{
  stop();
}

void CpuPool::work(const std::size_t index) noexcept
{
  auto lock = std::unique_lock{m_shared->mutex};
  m_shared->threads[index] = std::this_thread::get_id();
  for (;;)
  {
    m_shared->wake.wait(
      lock, [&] { return m_shared->stopping || !m_shared->queue.empty(); });
    if (m_shared->stopping)
    {
      break;
    }
    auto task = std::move(m_shared->queue.front());
    m_shared->queue.pop_front();
    lock.unlock();
    task->run();
    // the task's inputs are released before the thread waits again
    task.reset();
    lock.lock();
  }
  m_shared->threads[index] = std::thread::id{};
  lock.unlock();
  // what the thread left in the sink of D63 is not passed to a later thread with its id
  discardEngineMessagesForThread(std::this_thread::get_id());
}

bool CpuPool::post(std::unique_ptr<Task> task)
{
  {
    const auto lock = std::scoped_lock{m_shared->mutex};
    if (m_shared->stopping || m_shared->cancelled)
    {
      return false;
    }
    m_shared->queue.push_back(std::move(task));
  }
  m_shared->wake.notify_one();
  return true;
}

void CpuPool::cancel()
{
  const auto lock = std::scoped_lock{m_shared->mutex};
  // every job made so far is stale from here on; the workers keep taking the queued
  // tasks, which return at once, so no future waits for a join
  ++*m_generation;
  m_shared->cancelled = true;
}

void CpuPool::stop()
{
  auto dropped = std::deque<std::unique_ptr<Task>>{};
  {
    const auto lock = std::scoped_lock{m_shared->mutex};
    // every job made so far is stale from here on
    ++*m_generation;
    m_shared->stopping = true;
    dropped.swap(m_shared->queue);
  }
  m_shared->wake.notify_all();
  // outside the lock: the jobs that had not started are dropped, which breaks their
  // futures; then the threads are joined once their running jobs return
  dropped.clear();
  for (auto& thread : m_threads)
  {
    if (thread)
    {
      thread->join();
      thread.reset();
    }
  }
}

bool CpuPool::stopped() const
{
  const auto lock = std::scoped_lock{m_shared->mutex};
  return m_shared->stopping;
}

std::uint64_t CpuPool::generation() const
{
  return m_generation->load();
}

std::array<std::thread::id, CpuPool::ThreadCount> CpuPool::threadIdsForTesting() const
{
  const auto lock = std::scoped_lock{m_shared->mutex};
  return m_shared->threads;
}

} // namespace tb::lit
