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

#include "vm/vec.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tb::lit
{

using FrameClock = std::chrono::steady_clock;

// the shortest time between two frame starts: 1 / kMovingCameraFrameRate, rounded up so
// that a second holds at most kMovingCameraFrameRate starts
constexpr auto kMinFrameInterval = std::chrono::nanoseconds{
  (1'000'000'000 + kMovingCameraFrameRate - 1) / kMovingCameraFrameRate};

// §3.1: garbage is also collected, and timer queries polled, this long after the last
// frame
constexpr auto kIdleCollectInterval = std::chrono::seconds{1};

/**
 * Decides when the render thread starts a frame (§3.10, R5.25). A frame is drawn only
 * while one is pending (a camera change or requestFrame) and drawing is possible (the
 * window is exposed and has a swapchain), and frame starts are at least kMinFrameInterval
 * apart, which caps a moving camera at 30 Hz. Nothing pending means no frames at all.
 */
class FramePacer
{
public:
  explicit FramePacer(FrameClock::duration minInterval = kMinFrameInterval);

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

/**
 * The timer query of a pass in a frame, from the fixed ring of passCount ×
 * kFramesInFlight queries (R4-2.20). Frame N reuses the queries of frame N -
 * kFramesInFlight, which the render thread has waited for before it records frame N.
 */
constexpr std::size_t timerQuerySlot(
  const std::uint64_t frameIndex, const std::size_t pass, const std::size_t passCount)
{
  return std::size_t(frameIndex % kFramesInFlight) * passCount + pass;
}

/**
 * The maxVersions of a volatile constant buffer (R5.22): NVRHI drops a write once every
 * version is still in flight, so the most writes per frame are kept for every frame in
 * flight, plus a margin.
 */
constexpr std::uint32_t volatileBufferMaxVersions(const std::uint32_t mostWritesPerFrame)
{
  constexpr auto margin = std::uint32_t{16};
  return mostWritesPerFrame * std::uint32_t(kFramesInFlight) + margin;
}

/**
 * LP0's frame content: a clear colour from the camera's direction, direction * 0.5 + 0.5,
 * so that a viewer sees the preview follow the camera.
 */
std::array<float, 4> clearColorFor(const vm::vec3f& direction);

} // namespace tb::lit
