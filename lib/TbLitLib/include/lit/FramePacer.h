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

#include "lit/LitConfig.h"

#include <chrono>
#include <optional>

namespace tb::lit
{

using FrameClock = std::chrono::steady_clock;

// the shortest time between two frame starts: 1 / MovingCameraFrameRate, rounded up so
// that a second holds at most MovingCameraFrameRate starts
constexpr auto MinFrameInterval = std::chrono::nanoseconds{
  (1'000'000'000 + MovingCameraFrameRate - 1) / MovingCameraFrameRate};

// §3.1: garbage is also collected, and timer queries polled, this long after the last
// frame
constexpr auto IdleCollectInterval = std::chrono::seconds{1};

/**
 * Decides when the render thread starts a frame (§3.10, R5.25). A frame is drawn only
 * while one is pending (a camera change or requestFrame) and drawing is possible (the
 * window is exposed and has a swapchain), and frame starts are at least MinFrameInterval
 * apart, which caps a moving camera at 30 Hz. Nothing pending means no frames at all.
 */
class FramePacer
{
public:
  explicit FramePacer(FrameClock::duration minInterval = MinFrameInterval);

  // a camera change or requestFrame
  void trigger();
  bool pending() const;

  /**
   * When the next frame may start: empty when nothing is pending or drawing is not
   * possible, a time at or before now when it may start at once.
   */
  std::optional<FrameClock::time_point> nextFrameTime(bool canDraw) const;

  // a frame was started at now; it clears the pending trigger
  void frameStarted(FrameClock::time_point now);

  // the started frame was skipped (acquire timeout); it stays pending, so it is retried
  // at the next slot the limiter grants, or after the next trigger
  void frameSkipped();

private:
  FrameClock::duration m_minInterval;
  std::optional<FrameClock::time_point> m_lastFrameStart;
  bool m_pending = false;
};

} // namespace tb::lit
