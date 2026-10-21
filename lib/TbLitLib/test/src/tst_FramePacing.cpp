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

#include "lit/FramePacing.h"

#include <set>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("FramePacer")
{
  using namespace std::chrono_literals;

  const auto t0 = FrameClock::time_point{} + 1000s;
  auto pacer = FramePacer{};

  SECTION("the cap is 30 Hz")
  {
    CHECK(kMinFrameInterval == std::chrono::nanoseconds{33'333'334});
  }

  SECTION("idle means no frame")
  {
    CHECK_FALSE(pacer.pending());
    CHECK_FALSE(pacer.nextFrameTime(true));
  }

  SECTION("a trigger draws at once after idle")
  {
    pacer.trigger();
    const auto next = pacer.nextFrameTime(true);
    REQUIRE(next);
    CHECK(*next <= t0);
  }

  SECTION("not exposed or no swapchain: no frame, the trigger is kept")
  {
    pacer.trigger();
    CHECK_FALSE(pacer.nextFrameTime(false));
    CHECK(pacer.pending());
    CHECK(pacer.nextFrameTime(true));
  }

  SECTION("a started frame clears the trigger")
  {
    pacer.trigger();
    pacer.frameStarted(t0);
    CHECK_FALSE(pacer.pending());
    CHECK_FALSE(pacer.nextFrameTime(true));
  }

  SECTION("camera changes that keep arriving are capped at 30 Hz")
  {
    pacer.trigger();
    pacer.frameStarted(t0);

    pacer.trigger();
    const auto next = pacer.nextFrameTime(true);
    REQUIRE(next);
    CHECK(*next == t0 + kMinFrameInterval);
    CHECK(*next > t0 + 10ms);

    // many triggers before the slot still give one frame
    pacer.trigger();
    pacer.trigger();
    pacer.frameStarted(*next);
    CHECK_FALSE(pacer.nextFrameTime(true));
  }

  SECTION("a second of camera changes every millisecond starts 30 frames")
  {
    auto frames = 0;
    for (auto now = t0; now < t0 + 1s; now += 1ms)
    {
      pacer.trigger();
      if (*pacer.nextFrameTime(true) <= now)
      {
        pacer.frameStarted(now);
        ++frames;
      }
    }
    CHECK(frames == 30);
  }

  SECTION("a skipped frame is retried at the next slot")
  {
    pacer.trigger();
    pacer.frameStarted(t0);
    pacer.frameSkipped();
    CHECK(pacer.pending());
    const auto next = pacer.nextFrameTime(true);
    REQUIRE(next);
    CHECK(*next == t0 + kMinFrameInterval);
  }

  SECTION("a trigger long after the last frame draws at once")
  {
    pacer.trigger();
    pacer.frameStarted(t0);
    pacer.trigger();
    const auto next = pacer.nextFrameTime(true);
    REQUIRE(next);
    CHECK(*next <= t0 + 5s);
  }
}

TEST_CASE("FramePacing")
{
  SECTION("timerQuerySlot")
  {
    static_assert(kFramesInFlight == 2);

    SECTION("one pass: frames alternate between two queries")
    {
      CHECK(timerQuerySlot(0, 0, 1) == 0u);
      CHECK(timerQuerySlot(1, 0, 1) == 1u);
      CHECK(timerQuerySlot(2, 0, 1) == 0u);
      CHECK(timerQuerySlot(7, 0, 1) == 1u);
    }

    SECTION("frame N reuses frame N - kFramesInFlight's queries")
    {
      for (std::uint64_t frame = kFramesInFlight; frame < 20; ++frame)
      {
        for (std::size_t pass = 0; pass < 3; ++pass)
        {
          CHECK(
            timerQuerySlot(frame, pass, 3)
            == timerQuerySlot(frame - kFramesInFlight, pass, 3));
        }
      }
    }

    SECTION("the ring holds passes × frames in flight distinct queries")
    {
      constexpr auto passes = std::size_t{3};
      auto slots = std::set<std::size_t>{};
      for (std::uint64_t frame = 0; frame < kFramesInFlight; ++frame)
      {
        for (std::size_t pass = 0; pass < passes; ++pass)
        {
          const auto slot = timerQuerySlot(frame, pass, passes);
          CHECK(slot < passes * kFramesInFlight);
          slots.insert(slot);
        }
      }
      CHECK(slots.size() == passes * kFramesInFlight);
    }

    SECTION("large frame indices")
    {
      CHECK(timerQuerySlot(0xFFFFFFFFFFFFFFFFull, 0, 1) == 1u);
    }
  }

  SECTION("volatileBufferMaxVersions")
  {
    // writes per frame × frames in flight plus a margin (R5.22)
    CHECK(volatileBufferMaxVersions(1) > 2u);
    CHECK(volatileBufferMaxVersions(100) >= 200u);
    CHECK(volatileBufferMaxVersions(100) < 300u);
  }

  SECTION("clearColorFor")
  {
    CHECK(
      clearColorFor(vm::vec3f{1, 0, 0}) == std::array<float, 4>{1.0f, 0.5f, 0.5f, 1.0f});
    CHECK(
      clearColorFor(vm::vec3f{0, -1, 0}) == std::array<float, 4>{0.5f, 0.0f, 0.5f, 1.0f});
    CHECK(
      clearColorFor(vm::vec3f{0, 0, 1}) == std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f});
    // out-of-range input is clamped
    CHECK(
      clearColorFor(vm::vec3f{3, -3, 0}) == std::array<float, 4>{1.0f, 0.0f, 0.5f, 1.0f});
  }
}

} // namespace tb::lit
