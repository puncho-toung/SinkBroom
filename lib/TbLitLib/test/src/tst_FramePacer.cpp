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

#include "lit/FramePacer.h"

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
    CHECK(MinFrameInterval == std::chrono::nanoseconds{33'333'334});
  }

  SECTION("idle means no frame")
  {
    CHECK_FALSE(pacer.pending());
    CHECK_FALSE(pacer.nextFrameTime(true));
  }

  SECTION("the still timer is inert without a settle wanted (LP3.0, D151, P3.13)")
  {
    CHECK(StillTimerDelay == 200ms);
    // a camera that moved and stopped: no settle wanted (LP3.0: never), so no firing
    pacer.trigger();
    pacer.cameraMoved(t0);
    pacer.frameStarted(t0);
    CHECK_FALSE(pacer.settleWanted());
    CHECK_FALSE(pacer.stillTimerTime());
    CHECK_FALSE(pacer.pending());
    CHECK(pacer.drawReason() == FrameDrawReason::Trigger);
    CHECK(pacer.stillTimerFirings() == 0u);
  }

  SECTION("the still timer with a settle wanted fires once per stop (LP3.3's scope)")
  {
    pacer.trigger();
    pacer.cameraMoved(t0);
    pacer.frameStarted(t0);
    pacer.setSettleWanted(true);
    const auto at = pacer.stillTimerTime();
    REQUIRE(at);
    CHECK(*at == t0 + StillTimerDelay);
    pacer.stillTimerFired();
    CHECK(pacer.pending());
    CHECK(pacer.drawReason() == FrameDrawReason::StillTimer);
    CHECK(pacer.stillTimerFirings() == 1u);
    // spent until the camera moves again
    CHECK_FALSE(pacer.stillTimerTime());
    pacer.frameStarted(*at);
    CHECK_FALSE(pacer.stillTimerTime());
    pacer.cameraMoved(t0 + 1s);
    REQUIRE(pacer.stillTimerTime());
    CHECK(*pacer.stillTimerTime() == t0 + 1s + StillTimerDelay);
    // a trigger's reason is the trigger's
    pacer.trigger();
    CHECK(pacer.drawReason() == FrameDrawReason::Trigger);
    // no settle wanted: no timer
    pacer.setSettleWanted(false);
    CHECK_FALSE(pacer.stillTimerTime());
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
    CHECK(*next == t0 + MinFrameInterval);
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
    CHECK(*next == t0 + MinFrameInterval);
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

} // namespace tb::lit
