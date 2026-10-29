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

namespace tb::lit
{

FramePacer::FramePacer(const FrameClock::duration minInterval)
  : m_minInterval{minInterval}
{
}

void FramePacer::trigger()
{
  m_pending = true;
  m_drawReason = FrameDrawReason::Trigger;
}

bool FramePacer::pending() const
{
  return m_pending;
}

std::optional<FrameClock::time_point> FramePacer::nextFrameTime(const bool canDraw) const
{
  if (!m_pending || !canDraw)
  {
    return std::nullopt;
  }
  if (!m_lastFrameStart)
  {
    return FrameClock::time_point::min();
  }
  return *m_lastFrameStart + m_minInterval;
}

void FramePacer::frameStarted(const FrameClock::time_point now)
{
  m_pending = false;
  m_lastFrameStart = now;
}

void FramePacer::frameSkipped()
{
  m_pending = true;
}

void FramePacer::cameraMoved(const FrameClock::time_point at)
{
  m_lastCameraMove = at;
  m_stillTimerSpent = false;
}

void FramePacer::setSettleWanted(const bool wanted)
{
  m_settleWanted = wanted;
}

bool FramePacer::settleWanted() const
{
  return m_settleWanted;
}

std::optional<FrameClock::time_point> FramePacer::stillTimerTime() const
{
  if (!m_settleWanted || m_stillTimerSpent || !m_lastCameraMove)
  {
    return std::nullopt;
  }
  return *m_lastCameraMove + StillTimerDelay;
}

void FramePacer::stillTimerFired()
{
  m_stillTimerSpent = true;
  ++m_stillTimerFirings;
  m_pending = true;
  m_drawReason = FrameDrawReason::StillTimer;
}

bool FramePacer::cameraMoving(const FrameClock::time_point now) const
{
  return m_drawReason != FrameDrawReason::StillTimer && m_lastCameraMove
         && now - *m_lastCameraMove < StillTimerDelay;
}

std::optional<FrameClock::time_point> FramePacer::movingUntil() const
{
  if (!m_lastCameraMove)
  {
    return std::nullopt;
  }
  return *m_lastCameraMove + StillTimerDelay;
}

std::uint64_t FramePacer::stillTimerFirings() const
{
  return m_stillTimerFirings;
}

FrameDrawReason FramePacer::drawReason() const
{
  return m_drawReason;
}

} // namespace tb::lit
