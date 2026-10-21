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

#include "kd/result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Timeout = 10s;

std::unique_ptr<CpuPool> makePool()
{
  return CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); })
         | kdl::value();
}

// a gate that jobs wait in until the test opens it
struct Gate
{
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  bool open = false;

  void enterAndWait()
  {
    auto lock = std::unique_lock{mutex};
    ++entered;
    changed.notify_all();
    changed.wait(lock, [&] { return open; });
  }

  bool waitUntilEntered(const int count)
  {
    auto lock = std::unique_lock{mutex};
    return changed.wait_for(lock, Timeout, [&] { return entered >= count; });
  }

  void openGate()
  {
    auto lock = std::scoped_lock{mutex};
    open = true;
    changed.notify_all();
  }
};

} // namespace

TEST_CASE("CpuPool")
{
  SECTION("jobs return their results, on the pool's two threads")
  {
    auto pool = makePool();
    // both threads hold a job at once, so two different threads ran them
    auto gate = std::make_shared<Gate>();
    auto ids = std::make_shared<std::vector<std::thread::id>>(2);
    auto futures = std::vector<std::future<Result<int>>>{};
    for (auto i = 0; i < 2; ++i)
    {
      futures.push_back(pool->run(std::function<Result<int>(const CpuJobContext&)>{
        [gate, ids, i](const CpuJobContext& context) -> Result<int> {
          (*ids)[std::size_t(i)] = std::this_thread::get_id();
          gate->enterAndWait();
          return context.stale() ? -1 : i * 10;
        }}));
    }
    REQUIRE(gate->waitUntilEntered(2));
    gate->openGate();
    CHECK(futures[0].get() == Result<int>{0});
    CHECK(futures[1].get() == Result<int>{10});
    CHECK((*ids)[0] != (*ids)[1]);
    CHECK((*ids)[0] != std::this_thread::get_id());
    CHECK(CpuPool::ThreadCount == 2u);
  }

  SECTION("what a job throws is returned as an error")
  {
    auto pool = makePool();
    auto future = pool->run(std::function<Result<int>(const CpuJobContext&)>{
      [](const CpuJobContext&) -> Result<int> {
        throw std::runtime_error{"an injected job failure"};
      }});
    CHECK(
      future.get() == Result<int>{Error{"A CPU job failed: an injected job failure"}});

    auto unknown = pool->run(std::function<Result<int>(const CpuJobContext&)>{
      [](const CpuJobContext&) -> Result<int> { throw 42; }});
    CHECK(unknown.get() == Result<int>{Error{"A CPU job failed"}});
  }

  SECTION("stop: running jobs see stale, queued ones are dropped, later ones refused")
  {
    auto pool = makePool();
    const auto generation = pool->generation();
    auto gate = std::make_shared<Gate>();
    auto sawStale = std::make_shared<std::atomic<int>>(0);
    auto ran = std::make_shared<std::atomic<int>>(0);
    const auto job = std::function<Result<int>(const CpuJobContext&)>{
      [gate, sawStale, ran](const CpuJobContext& context) -> Result<int> {
        ++*ran;
        gate->enterAndWait();
        if (context.stale())
        {
          ++*sawStale;
          return Error{"stale"};
        }
        return 1;
      }};

    // two run and wait in the gate, the third waits in the queue
    auto running = std::vector<std::future<Result<int>>>{};
    running.push_back(pool->run(job));
    running.push_back(pool->run(job));
    REQUIRE(gate->waitUntilEntered(2));
    auto queued = pool->run(job);

    // the stop joins the threads, so the gate opens from another thread once the
    // generation has moved on
    auto opener = std::thread{[&] {
      while (pool->generation() == generation)
      {
        std::this_thread::sleep_for(1ms);
      }
      gate->openGate();
    }};
    pool->stop();
    opener.join();

    CHECK(pool->stopped());
    CHECK(pool->generation() == generation + 1);
    CHECK(sawStale->load() == 2);
    CHECK(ran->load() == 2);
    // R5.22: an abandoned future is only waited for; the dropped job's is ready, broken
    for (auto& future : running)
    {
      CHECK(future.wait_for(0s) == std::future_status::ready);
    }
    CHECK(queued.wait_for(0s) == std::future_status::ready);

    auto refused = pool->run(job);
    CHECK(refused.get() == Result<int>{Error{"The CPU pool is stopped"}});
    CHECK(ran->load() == 2);

    // a second stop only moves the generation on
    pool->stop();
    CHECK(pool->generation() == generation + 2);
  }

  SECTION("a job still queued when the pool stops never runs")
  {
    auto pool = makePool();
    auto gate = std::make_shared<Gate>();
    auto entered = std::make_shared<std::atomic<int>>(0);
    const auto blocker = std::function<Result<int>(const CpuJobContext&)>{
      [gate](const CpuJobContext&) -> Result<int> {
        gate->enterAndWait();
        return 0;
      }};
    auto first = pool->run(blocker);
    auto second = pool->run(blocker);
    REQUIRE(gate->waitUntilEntered(2));

    // queued behind the two: dropped, or, if a thread takes it before the task manager
    // stops, abandoned by its stale context before it calls the job
    auto late = pool->run(std::function<Result<int>(const CpuJobContext&)>{
      [entered](const CpuJobContext&) -> Result<int> {
        ++*entered;
        return 0;
      }});
    auto opener = std::thread{[&] {
      std::this_thread::sleep_for(50ms);
      gate->openGate();
    }};
    pool->stop();
    opener.join();
    CHECK(entered->load() == 0);
    CHECK(late.wait_for(0s) == std::future_status::ready);
  }
}

} // namespace tb::lit
