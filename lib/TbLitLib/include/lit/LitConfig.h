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
#include <cstdint>

namespace tb::lit
{

// D35: frames in flight; before recording frame N the render thread waits on frame N - 2
constexpr auto FramesInFlight = std::size_t{2};

// D48: the longest wait for a swapchain image; a timeout skips the frame until next time
constexpr auto AcquireTimeout = std::chrono::milliseconds{50};

// §3.1: the most frames per second (Hz) while the camera moves
constexpr auto MovingCameraFrameRate = 30;

// S8, D36 (LP2.1): the size of one geometry arena buffer, vertex or index; another of the
// kind is added when none can hold a chunk (one larger than this gets one of its size)
constexpr auto ArenaBytes = std::uint64_t{64} * 1024 * 1024;

// D78 (LP2.1): the render thread's command list's upload chunks (the engine uses 64 MB,
// r_vkUploadBufferSizeMB), and §3.10's cap on what one frame uploads; what does not fit
// waits for the next frame, which the unconverged frame triggers (D77)
constexpr auto UploadChunkBytes = std::size_t{32} * 1024 * 1024;
constexpr auto UploadBytesPerFrame = std::uint64_t{32} * 1024 * 1024;

// U2.3 (31-LP2-FIXES.md): the device memory allocations kept free below the device's
// maxMemoryAllocationCount for NVRHI's upload chunks and staging: once the live count
// passes the limit minus this, no new texture or geometry arena is made
constexpr auto AllocationHeadroom = std::uint64_t{64};

// LP6.4a (C68 (a), D171, Q47): the size of one texture heap, the DeviceLocal memory the
// slots' textures are placed in; another is made when none holds a texture, and a texture
// larger than this is made dedicated
constexpr auto HeapBytes = std::uint64_t{128} * 1024 * 1024;

// D77 (LP2.1): texture arrivals trigger a frame at most this often (10 Hz)
constexpr auto TextureArrivalInterval = std::chrono::milliseconds{100};

// C28, D37 (LP2.1): the binding sets the cache keeps; beyond it the least recently used
// are released
constexpr auto BindingSetCacheSize = std::size_t{4096};

// C11 (LP2.1): the texture budget's default (the panel setting comes later), which covers
// the session's whole VRAM (§3.10); LP6.4b (C68 (b), A57):
// SessionOptions::vramBudgetBytes, the eviction's budget
constexpr auto DefaultVramBudgetBytes = std::uint64_t{1024} * 1024 * 1024;

// LP6.4b (C68 (b), D172): a slot no frame has bound for this many frames may be evicted;
// a frame evicts, or floors, at most EvictionsPerFrame slots (the rest wait for the next
// frame, which it triggers); the mip floor keeps an image's levels from the first whose
// width and height are both at most MipFloorExtent (at least its last level)
constexpr auto EvictionIdleFrames = std::uint64_t{4};
constexpr auto EvictionsPerFrame = std::size_t{64};
constexpr auto MipFloorExtent = 256;

// LP6.4b (C68 (d), D174, A60): a compaction a settled frame marked runs in the next frame
// drawn for another reason, or, when none comes within this, as an uploads-only idle
// submission
constexpr auto CompactionIdleDelay = std::chrono::milliseconds{1000};

} // namespace tb::lit
