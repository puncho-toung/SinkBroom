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
#include "lit/MaterialDecls.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tb::lit
{
class StackThread;

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
 * What a job's future holds (B0.1, D66): its result, and what the copied engine code
 * reported on the job's thread while it ran (the sink of D63, drained for that thread
 * after the job), in their order. The result is an error when the job threw, was
 * abandoned or refused, or when an Error or FatalError is among the messages: the engine
 * would have stopped there.
 */
template <typename T>
struct CpuJobResult
{
  Result<T> result;
  std::vector<DeclMessage> messages;
};

namespace detail
{
template <typename R>
struct CpuJobValue;

template <typename T>
struct CpuJobValue<Result<T>>
{
  using type = T;
};
} // namespace detail

/**
 * A session's private CPU pool (§3.1, S3, R5.12, R5.22, D66): two worker threads of its
 * own, each a StackThread with an 8 MB stack on every platform (G13; so K19's macOS
 * stack limit does not apply to the pool), and one task queue, never the document's
 * thread pool. The session makes it at Start phase B. Teardown step 1 cancels it when
 * the stop is requested (R2.1: its generation bumped, no join, so the queued jobs return
 * at once and the running ones at their next check of stale) and stops it after the
 * render thread is joined; stopping bumps its generation again, drops the queued tasks
 * and joins the threads. A pool whose second thread cannot be made stops and joins the
 * first and is not created: nothing ends the process.
 *
 * Jobs hold their inputs by shared_ptr and never the session or the pool; they look at
 * context.stale() and return early when it is set. They never touch the NVRHI device:
 * they only fill CPU memory (C20). Whatever a job throws is caught and returned as an
 * error, so every job ends in its CpuJobResult. A job that had not started when the pool
 * stopped is dropped, which breaks its future: an abandoned future is only ever wait()ed,
 * never get() (R5.22).
 *
 * The sink (B0.1): before a job the pool drops what its thread left in the sink of D63,
 * after the job it drains the thread's messages into the result. Each message the copied
 * code reports takes MAX_PRINT_MSG (16 KB) of the thread's stack while it is formatted.
 *
 * Threads: run on any thread; it can throw std::bad_alloc. stop on the thread that owns
 * the pool, never from a job, since it joins the pool's threads.
 */
class CpuPool
{
public:
  static constexpr std::size_t ThreadCount = 2;
  static constexpr std::size_t StackBytes = 8u * 1024u * 1024u;

private:
  class Task
  {
  public:
    virtual ~Task() = default;
    // runs the job and sets the promise; nothing leaves it
    virtual void run() noexcept = 0;
  };

  template <typename T, typename F>
  class JobTask : public Task
  {
  private:
    F m_job;
    std::promise<CpuJobResult<T>> m_promise;
    CpuJobContext m_context;

  public:
    JobTask(F job, std::promise<CpuJobResult<T>> promise, CpuJobContext context)
      : m_job{std::move(job)}
      , m_promise{std::move(promise)}
      , m_context{std::move(context)}
    {
    }

    void run() noexcept override
    {
      try
      {
        const auto thread = std::this_thread::get_id();
        discardEngineMessagesForThread(thread);
        auto output = CpuJobResult<T>{runJob(), {}};
        output.messages = drainEngineMessagesForThread(thread);
        failOnEngineError(output);
        m_promise.set_value(std::move(output));
      }
      catch (...)
      {
        try
        {
          m_promise.set_exception(std::current_exception());
        }
        catch (...)
        {
        }
      }
    }

  private:
    Result<T> runJob()
    {
      if (m_context.stale())
      {
        return Error{"The CPU job was abandoned"};
      }
      try
      {
        return m_job(static_cast<const CpuJobContext&>(m_context));
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

    static void failOnEngineError(CpuJobResult<T>& output)
    {
      if (!output.result.is_success())
      {
        return;
      }
      for (const auto& message : output.messages)
      {
        if (
          message.kind == DeclMessageKind::Error
          || message.kind == DeclMessageKind::FatalError)
        {
          output.result = Error{"The CPU job reached an engine error: " + message.text};
          return;
        }
      }
    }
  };

  // what the workers share with the pool, under m_mutex
  struct Shared
  {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<Task>> queue;
    bool stopping = false;
    // cancel: the queued tasks still run (abandoned at once), no new task is taken
    bool cancelled = false;
    std::array<std::thread::id, ThreadCount> threads{};
  };

  std::shared_ptr<std::atomic<std::uint64_t>> m_generation;
  std::unique_ptr<Shared> m_shared;
  std::array<std::unique_ptr<StackThread>, ThreadCount> m_threads;

  CpuPool();

  Result<void> startThreads();
  void work(std::size_t index) noexcept;
  // queues the task; false once the pool is cancelled or stopped
  bool post(std::unique_ptr<Task> task);

public:
  // starts the threads; fails when they cannot be made
  static Result<std::unique_ptr<CpuPool>> create();

  // stop
  ~CpuPool();

  /**
   * Queues job, a callable taking const CpuJobContext& and returning Result<T>, which
   * may hold move-only captures (B0.11); any thread. Its future holds the job's
   * CpuJobResult, an error for a job of a stopped pool, or, when not even the error could
   * be made, the exception.
   */
  template <typename F>
  auto run(F job)
  {
    using R = std::invoke_result_t<F&, const CpuJobContext&>;
    using T = typename detail::CpuJobValue<R>::type;
    auto promise = std::promise<CpuJobResult<T>>{};
    auto future = promise.get_future();
    auto context = CpuJobContext{m_generation, m_generation->load()};
    auto task = std::make_unique<JobTask<T, F>>(
      std::move(job), std::move(promise), std::move(context));
    if (!post(std::move(task)))
    {
      // the task was destroyed with its promise unset: answer through a new one
      auto refused = std::promise<CpuJobResult<T>>{};
      future = refused.get_future();
      refused.set_value(CpuJobResult<T>{Error{"The CPU pool is stopped"}, {}});
    }
    return future;
  }

  /**
   * R2.1: bumps the generation without joining, so every job made so far is stale: the
   * queued ones return "abandoned" without running their callable, the running ones at
   * their next check, and every future of them is ready soon without a join; later jobs
   * are refused. Any thread, also one that a job waits for.
   */
  void cancel();
  // bumps the generation, drops the queued tasks and joins the threads
  void stop();
  bool stopped() const;
  std::uint64_t generation() const;
  // the ids of the pool's threads while they run
  std::array<std::thread::id, ThreadCount> threadIdsForTesting() const;

  deleteCopyAndMove(CpuPool);
};

} // namespace tb::lit
