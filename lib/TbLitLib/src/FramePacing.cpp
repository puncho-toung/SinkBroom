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

#include <algorithm>

namespace tb::lit
{

FramePacer::FramePacer(const FrameClock::duration minInterval)
  : m_minInterval{minInterval}
{
}

void FramePacer::trigger()
{
  m_pending = true;
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

std::array<float, 4> clearColorFor(const vm::vec3f& direction)
{
  const auto channel = [](const float d) {
    return std::clamp(d * 0.5f + 0.5f, 0.0f, 1.0f);
  };
  return {channel(direction.x()), channel(direction.y()), channel(direction.z()), 1.0f};
}

} // namespace tb::lit
