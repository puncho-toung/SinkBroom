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

#include "LitTestUtils.h"
#include "RangeAllocator.h"

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("RangeAllocator")
{
  SECTION("allocate: aligned sizes, the best fit, nothing larger than what is free")
  {
    auto ranges = RangeAllocator{1024, 32};
    CHECK(ranges.freeBytes() == 1024u);
    CHECK_FALSE(ranges.allocate(0));
    CHECK_FALSE(ranges.allocate(1025));

    const auto a = ranges.allocate(1);
    REQUIRE(a);
    CHECK(*a == ArenaRange{0, 32});
    const auto b = ranges.allocate(100);
    REQUIRE(b);
    CHECK(*b == ArenaRange{32, 128});
    const auto c = ranges.allocate(64);
    REQUIRE(c);
    CHECK(*c == ArenaRange{160, 64});
    CHECK(ranges.usedBytes() == 224u);
    CHECK(ranges.freeBytes() == 800u);

    // free a (32 at 0) and c (64 at 160): a 32-byte request takes a's range, the best
    // fit, not c's or the large rest
    ranges.free(*a, 0);
    ranges.free(*c, 0);
    ranges.reclaim(1);
    const auto d = ranges.allocate(20);
    REQUIRE(d);
    CHECK(*d == ArenaRange{0, 32});
    const auto e = ranges.allocate(64);
    REQUIRE(e);
    CHECK(*e == ArenaRange{160, 64});

    // the capacity's last partial unit is never handed out
    auto odd = RangeAllocator{100, 32};
    CHECK(odd.freeBytes() == 96u);
    CHECK_FALSE(odd.allocate(97));
  }

  SECTION("free: a range is handed out again only once its frame has finished")
  {
    auto ranges = RangeAllocator{256, 4};
    const auto a = ranges.allocate(256);
    REQUIRE(a);
    CHECK_FALSE(ranges.allocate(4));

    // freed in frame 7: frames 0 to 6 finished is not enough
    ranges.free(*a, 7);
    CHECK(ranges.usedBytes() == 0u);
    CHECK(ranges.pendingBytes() == 256u);
    CHECK_FALSE(ranges.allocate(4));
    ranges.reclaim(7);
    CHECK_FALSE(ranges.allocate(4));
    CHECK(ranges.pendingBytes() == 256u);

    // frame 7 finished
    ranges.reclaim(8);
    CHECK(ranges.pendingBytes() == 0u);
    CHECK(ranges.allocate(256) == ArenaRange{0, 256});
  }

  SECTION("reclaim merges free neighbours: everything freed is one range again")
  {
    auto ranges = RangeAllocator{4096, 16};
    auto taken = std::vector<ArenaRange>{};
    for (auto i = 0; i < 16; ++i)
    {
      const auto range = ranges.allocate(256);
      REQUIRE(range);
      taken.push_back(*range);
    }
    CHECK(ranges.freeRanges() == 0u);
    // every other one, then the rest
    for (std::size_t i = 0; i < taken.size(); i += 2)
    {
      ranges.free(taken[i], 1);
    }
    ranges.reclaim(2);
    CHECK(ranges.freeRanges() == 8u);
    CHECK(ranges.largestFree() == 256u);
    CHECK_FALSE(ranges.allocate(512));
    for (std::size_t i = 1; i < taken.size(); i += 2)
    {
      ranges.free(taken[i], 2);
    }
    ranges.reclaim(3);
    CHECK(ranges.freeRanges() == 1u);
    CHECK(ranges.largestFree() == 4096u);
    CHECK(ranges.allocate(4096) == ArenaRange{0, 4096});
  }

  SECTION("a fragmentation bound: random churn never splits the free space further")
  {
    // coalesced free ranges are separated by used or pending ones, so there is at most
    // one more free range than live and pending ranges, however the churn goes
    auto ranges = RangeAllocator{std::uint64_t{1} << 24, 32};
    auto random = std::mt19937{20261022};
    auto size = std::uniform_int_distribution<std::uint64_t>{1, 65536};
    auto live = std::vector<ArenaRange>{};
    auto frame = std::uint64_t{0};
    auto failed = 0;
    for (auto step = 0; step < 20000; ++step)
    {
      if (live.empty() || random() % 3 != 0)
      {
        if (const auto range = ranges.allocate(size(random)))
        {
          live.push_back(*range);
        }
        else
        {
          ++failed;
        }
      }
      else
      {
        const auto at = std::size_t(random() % live.size());
        ranges.free(live[at], frame);
        live[at] = live.back();
        live.pop_back();
      }
      // two frames in flight
      ++frame;
      ranges.reclaim(frame >= 2 ? frame - 1 : 0);
      REQUIRE(ranges.freeRanges() <= live.size() + ranges.pendingRanges() + 1);
    }
    INFO("allocations that found no range: " << failed);
    auto used = std::uint64_t{0};
    for (const auto& range : live)
    {
      used += range.size;
    }
    CHECK(ranges.usedBytes() == used);
    CHECK(
      ranges.usedBytes() + ranges.pendingBytes() + ranges.freeBytes()
      == ranges.capacity());

    for (const auto& range : live)
    {
      ranges.free(range, frame);
    }
    ranges.reclaim(frame + 1);
    CHECK(ranges.freeRanges() == 1u);
    CHECK(ranges.freeBytes() == ranges.capacity());
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
// U2.4: 100 cycles of allocate, free and reclaim leave the CRT's debug heap as two
// warm-up cycles left it (the free list and the pending list keep their bound)
TEST_CASE("RangeAllocator: 100 cycles on the debug heap (U2.4)")
{
  auto ranges = RangeAllocator{1u << 20, 32};
  auto random = std::mt19937{7};
  auto frame = std::uint64_t{0};
  const auto cycle = [&] {
    auto live = std::vector<ArenaRange>{};
    for (auto i = 0; i < 24; ++i)
    {
      if (const auto range = ranges.allocate(1 + random() % 4096))
      {
        live.push_back(*range);
      }
    }
    for (const auto& range : live)
    {
      ranges.free(range, frame);
    }
    ++frame;
    ranges.reclaim(frame);
  };
  cycle();
  cycle();
  CHECK_FALSE(heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      cycle();
    }
  }));
  CHECK(ranges.freeRanges() == 1u);
  CHECK(ranges.pendingRanges() == 0u);
}
#endif

} // namespace tb::lit
