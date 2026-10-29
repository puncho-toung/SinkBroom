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
#include <cstdint>
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

// LP3.0 (D151, C62, P3.13): the still timer's delay, the camera still this long
constexpr auto StillTimerDelay = std::chrono::milliseconds{200};

/**
 * LP3.0 (A49): the reasons a frame is started, §3.10's list as the pacer sees them: a
 * trigger (a camera change, requestFrame, an arrival, an unconverged pass), or the still
 * timer's firing (D151). LP3.3: the still timer's frame is a settling one
 * (cameraMoving).
 */
enum class FrameDrawReason
{
  Trigger,
  StillTimer,
};

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

  // ----- LP3.0 (D151, C62, P3.13): the still timer; LP3.3 wants it -----

  // the camera moved at this time: the still timer counts from it again
  void cameraMoved(FrameClock::time_point at);
  /**
   * whether a settle wants the still timer (LitFrameStats::shadowSettleWanted, after each
   * frame): some light's held LOD differs from its exact LOD, a cascade budget is
   * pending (C62's scope), or the held layout is not the engine's packing (LP3.3's term,
   * D145)
   */
  void setSettleWanted(bool wanted);
  bool settleWanted() const;
  /**
   * When the still timer fires: StillTimerDelay after the last camera move, while a
   * settle is wanted and the timer has not fired since that move; empty otherwise.
   */
  std::optional<FrameClock::time_point> stillTimerTime() const;
  /**
   * LP3.3 (C62, D152): whether a frame started at now is drawn while the camera moves:
   * the last camera move is younger than StillTimerDelay (a camera trigger's frame always
   * is) and the frame is not the still timer's; such a frame's pass 6 holds the layout
   * and the LODs, any other settles them.
   */
  bool cameraMoving(FrameClock::time_point now) const;
  // LP6.6 (C73): when a frame started then is no longer cameraMoving: StillTimerDelay
  // after the last camera move; empty without one
  std::optional<FrameClock::time_point> movingUntil() const;
  // the still timer fired: a frame trigger whose reason is StillTimer, spent until the
  // camera moves again
  void stillTimerFired();
  std::uint64_t stillTimerFirings() const;
  // the reason of the pending (or last started) frame
  FrameDrawReason drawReason() const;

private:
  FrameClock::duration m_minInterval;
  std::optional<FrameClock::time_point> m_lastFrameStart;
  bool m_pending = false;
  // LP3.0 (D151)
  std::optional<FrameClock::time_point> m_lastCameraMove;
  bool m_settleWanted = false;
  bool m_stillTimerSpent = false;
  std::uint64_t m_stillTimerFirings = 0;
  FrameDrawReason m_drawReason = FrameDrawReason::Trigger;
};

} // namespace tb::lit
