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

// LP7.1b step 0 (49-START-STOP-HEAP-FLAKE.md §5): the heap measure's own proofs

#include "LitTestUtils.h"
#include "StackThread.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>

#if defined(_MSC_VER) && defined(_DEBUG)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// after windows.h
#include <crtdbg.h>
#endif

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
#if defined(_MSC_VER) && defined(_DEBUG)
namespace
{

constexpr auto Bound = DWORD{10000};

// the measure before LP7.1b step 0 (LitTestUtils.cpp at 48e971df8, :331-351), its dump
// left out: any normal- or client-block difference fails
bool oldMeasureGrows(const std::function<void()>& f)
{
  _CrtMemState before;
  _CrtMemCheckpoint(&before);
  f();
  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  _CrtMemState difference;
  return _CrtMemDifference(&difference, &before, &after) != 0;
}

/**
 * A thread started the way the OS starts its own (CreateThread: std::thread's callable
 * would be a block of the window for as long as the thread runs), which signals that it
 * runs, its TLS callbacks done, and waits to be released. Kernel events only: it costs
 * the heap nothing but what any thread costs.
 */
class HeldThread
{
public:
  HeldThread() = default;
  HeldThread(const HeldThread&) = delete;
  HeldThread& operator=(const HeldThread&) = delete;

  ~HeldThread()
  {
    static_cast<void>(release());
    CloseHandle(m_started);
    CloseHandle(m_release);
  }

  // the thread runs; nothing here touches the heap or Catch2
  bool start()
  {
    m_thread = CreateThread(nullptr, 0, &HeldThread::body, this, 0, nullptr);
    return m_thread != nullptr && WaitForSingleObject(m_started, Bound) == WAIT_OBJECT_0;
  }

  // the thread released and joined, its thread-locals destroyed
  bool release()
  {
    if (m_thread == nullptr)
    {
      return true;
    }
    SetEvent(m_release);
    const auto ended = WaitForSingleObject(m_thread, Bound) == WAIT_OBJECT_0;
    CloseHandle(m_thread);
    m_thread = nullptr;
    return ended;
  }

private:
  static DWORD WINAPI body(void* self)
  {
    auto& held = *static_cast<HeldThread*>(self);
    SetEvent(held.m_started);
    return WaitForSingleObject(held.m_release, Bound) == WAIT_OBJECT_0 ? 0 : 1;
  }

  HANDLE m_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE m_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE m_thread = nullptr;
};

} // namespace

TEST_CASE("heapGrows counts only what the product owns (LP7.1b step 0)")
{
  SECTION("a thread alive across the second checkpoint passes; the old measure fails it")
  {
    const auto carriers = carrierThreadsForTesting();
    auto started = false;

    auto first = HeldThread{};
    CHECK_FALSE(heapGrows([&] { started = first.start(); }));
    REQUIRE(started);
    CHECK(carrierThreadsForTesting() == carriers + 1);
    REQUIRE(first.release());
    CHECK(carrierThreadsForTesting() == carriers);

    // the same window under the old arithmetic, and the cost it saw: Catch2's pair
    auto second = HeldThread{};
    CHECK(oldMeasureGrows([&] { started = second.start(); }));
    REQUIRE(started);
    REQUIRE(second.release());

    auto third = HeldThread{};
    const auto growth = heapGrowth([&] { started = third.start(); });
    REQUIRE(started);
    CHECK(growth.blocks == CatchThreadBlocks);
    CHECK(growth.bytes == CatchThreadBytes);
    REQUIRE(third.release());

    auto fourth = HeldThread{};
    const auto born = measureHeap([&] { started = fourth.start(); }, false);
    REQUIRE(started);
    CHECK(born.threadsBorn == 1);
    CHECK(born.threadsGone == 0);
    CHECK(born.blocks == CatchThreadBlocks);
    CHECK(born.bytes == CatchThreadBytes);
    CHECK_FALSE(born.grew());
    CHECK_FALSE(born.threadLeak());

    // alive at the first checkpoint, gone at the second: minus the pair
    auto released = false;
    const auto gone = measureHeap([&] { released = fourth.release(); }, false);
    REQUIRE(released);
    CHECK(gone.threadsBorn == 0);
    CHECK(gone.threadsGone == 1);
    CHECK(gone.blocks == -CatchThreadBlocks);
    CHECK(gone.bytes == -CatchThreadBytes);
    CHECK_FALSE(gone.grew());
  }

  SECTION("a window that leaks one block still fails, with a thread alive or not")
  {
    const auto dumped = DumpCounter{};
    auto* leaked = static_cast<int*>(nullptr);
    CHECK(heapGrows([&] { leaked = new int(7); }));
    CHECK(dumped.blocks() == 1);
    delete leaked;

    auto thread = HeldThread{};
    auto started = false;
    const auto measure = measureHeap(
      [&] {
        started = thread.start();
        leaked = new int(8);
      },
      false);
    REQUIRE(started);
    CHECK(measure.threadsBorn == 1);
    CHECK(measure.blocks == CatchThreadBlocks + 1);
    CHECK(measure.grew());
    delete leaked;
    REQUIRE(thread.release());
  }

  SECTION("a library thread that outlives the window fails the thread check")
  {
    // the dump of the window (with the thread's own blocks) kept off stderr
    const auto quiet = DumpCounter{};
    const auto release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(release != nullptr);
    const auto body = [release] { WaitForSingleObject(release, Bound); };

    auto outliving = StackThread{};
    auto started = false;
    const auto measure =
      measureHeap([&] { started = outliving.start(body, 64 * 1024).is_success(); }, true);
    REQUIRE(started);
    CHECK(measure.threadLeak());
    CHECK(measure.libraryThreadsAfter == measure.libraryThreadsBefore + 1);
    SetEvent(release);
    outliving.join();

    // started and joined inside the window: neither a thread leak nor growth
    auto joined = StackThread{};
    const auto inside = measureHeap(
      [&] {
        started = joined.start([] {}, 64 * 1024).is_success();
        joined.join();
      },
      false);
    REQUIRE(started);
    CHECK_FALSE(inside.threadLeak());
    CHECK_FALSE(inside.grew());
    CloseHandle(release);
  }

  SECTION("the dump lists the window's blocks and nothing older")
  {
    const auto dumped = DumpCounter{};
    // older than the window and freed inside it: without the sentinel a freed newest
    // block at the checkpoint sent the walk down to the heap's oldest block
    auto* const older = new std::array<char, 200>{};
    auto* a = static_cast<std::array<char, 24>*>(nullptr);
    auto* b = static_cast<std::array<char, 40>*>(nullptr);
    auto* c = static_cast<std::array<char, 56>*>(nullptr);
    CHECK(heapGrows([&] {
      delete older;
      a = new std::array<char, 24>{};
      b = new std::array<char, 40>{};
      c = new std::array<char, 56>{};
    }));
    CHECK(dumped.blocks() == 3);
    delete a;
    delete b;
    delete c;
  }
}
#endif

} // namespace tb::lit
