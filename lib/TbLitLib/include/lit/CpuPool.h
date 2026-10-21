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
#include "base/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace kdl
{
class task_manager;
}

namespace tb::lit
{

// What a job of the pool knows of it: whether the pool stopped after the job was made.
class CpuJobContext
{
private:
  std::shared_ptr<const std::atomic<std::uint64_t>> m_current;
  std::uint64_t m_generation;

public:
  CpuJobContext(
    std::shared_ptr<const std::atomic<std::uint64_t>> current, std::uint64_t generation);

  // the job is abandoned: it returns at once, and no one reads its result
  bool stale() const;
  std::uint64_t generation() const;
};

/**
 * A session's private CPU pool (§3.1, S3, R5.12, R5.22): a kdl::task_manager with two
 * threads, never the document's. The session makes it at Start phase B and stops it in
 * teardown step 1, after the render thread is joined; stopping bumps its generation, then
 * destroys the task manager, which joins its threads.
 *
 * Jobs hold their inputs by shared_ptr and never the session or the pool; they look at
 * context.stale() as PrefabLibrary's tasks look at their generation, and return early
 * when it is set. They never touch the NVRHI device: they only fill CPU memory (C20).
 * Whatever a job throws is caught and returned as an error, so every job ends in its
 * Result. A job that had not started when the pool stopped is dropped, which breaks its
 * future: an abandoned future is only ever wait()ed, never get() (R5.22).
 *
 * Threads: run on any thread; stop on the thread that owns the pool, never from a job,
 * since it joins the pool's threads.
 */
class CpuPool
{
public:
  static constexpr std::size_t ThreadCount = 2;

private:
  std::shared_ptr<std::atomic<std::uint64_t>> m_generation;
  mutable std::mutex m_mutex;
  std::unique_ptr<kdl::task_manager> m_tasks;

  CpuPool();

  // queues task; false once the pool is stopped
  bool post(std::function<bool()> task);

  template <typename T>
  static Result<T> runJob(
    const std::function<Result<T>(const CpuJobContext&)>& job,
    const CpuJobContext& context)
  {
    if (context.stale())
    {
      return Error{"The CPU job was abandoned"};
    }
    try
    {
      return job(context);
    }
    catch (const std::exception& e)
    {
      return Error{std::string{"A CPU job failed: "} + e.what()};
    }
    catch (...)
    {
      return Error{"A CPU job failed"};
    }
  }

public:
  // starts the threads; fails when they cannot be made
  static Result<std::unique_ptr<CpuPool>> create();

  // stop
  ~CpuPool();

  /**
   * Queues job; any thread. Its future holds the job's Result, an error for a job of a
   * stopped pool, or, when not even the error could be made, the exception.
   */
  template <typename T>
  std::future<Result<T>> run(std::function<Result<T>(const CpuJobContext&)> job)
  {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    auto context = CpuJobContext{m_generation, m_generation->load()};
    const auto posted = post([promise, job = std::move(job), context]() noexcept {
      try
      {
        promise->set_value(runJob(job, context));
      }
      catch (...)
      {
        promise->set_exception(std::current_exception());
      }
      return true;
    });
    if (!posted)
    {
      promise->set_value(Error{"The CPU pool is stopped"});
    }
    return future;
  }

  // bumps the generation and joins the threads; jobs not started are dropped
  void stop();
  bool stopped() const;
  std::uint64_t generation() const;

  deleteCopyAndMove(CpuPool);
};

} // namespace tb::lit
