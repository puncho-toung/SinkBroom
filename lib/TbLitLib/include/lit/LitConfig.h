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

#include <chrono>
#include <cstddef>

namespace tb::lit
{

// D35: frames in flight; before recording frame N the render thread waits on frame N - 2
constexpr auto kFramesInFlight = std::size_t{2};

// D48: the longest wait for a swapchain image; a timeout skips the frame until next time
constexpr auto kAcquireTimeout = std::chrono::milliseconds{50};

// §3.1: the most frames per second (Hz) while the camera moves
constexpr auto kMovingCameraFrameRate = 30;

} // namespace tb::lit
