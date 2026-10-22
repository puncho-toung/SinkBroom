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

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace tb::lit
{

// a byte range of an arena
struct ArenaRange
{
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  bool operator==(const ArenaRange&) const = default;
};

/**
 * S8, D36, §3.4 "Memory": the ranges of one arena buffer. A range is handed out best fit
 * (the smallest free range that holds it, the lowest offset among equals) and rounded up
 * to the arena's alignment. A freed range is not free at once: it waits in a list stamped
 * with the frame it was freed in, and joins the free ranges (merged with its neighbours)
 * only once that frame has finished on the GPU (reclaim), because a frame in flight may
 * still read it. Plain CPU state; the render thread's (one arena per instance).
 */
class RangeAllocator
{
private:
  struct Pending
  {
    ArenaRange range;
    std::uint64_t frame = 0;
  };

  std::uint64_t m_capacity = 0;
  std::uint64_t m_alignment = 1;
  // the free ranges by offset, and as (size, offset), for the best fit
  std::map<std::uint64_t, std::uint64_t> m_freeByOffset;
  std::set<std::pair<std::uint64_t, std::uint64_t>> m_freeBySize;
  std::vector<Pending> m_pending;
  std::uint64_t m_used = 0;
  std::uint64_t m_pendingBytes = 0;

public:
  // capacity bytes, handed out in multiples of alignment (at least 1)
  RangeAllocator(std::uint64_t capacity, std::uint64_t alignment);

  // a free range of at least size bytes (size rounded up to the alignment), or nullopt
  // when no free range holds it; a size of 0 has no range
  std::optional<ArenaRange> allocate(std::uint64_t size);

  // range, handed out by allocate, is no longer used from frame frame on: it is free
  // again once completedFrames > frame (reclaim)
  void free(const ArenaRange& range, std::uint64_t frame);

  // the freed ranges whose frames have finished (frames 0 to completedFrames - 1) join
  // the free ranges
  void reclaim(std::uint64_t completedFrames);

  std::uint64_t capacity() const;
  std::uint64_t alignment() const;
  // bytes handed out and not freed
  std::uint64_t usedBytes() const;
  // bytes freed whose frames have not finished
  std::uint64_t pendingBytes() const;
  // bytes free now
  std::uint64_t freeBytes() const;
  // the largest free range now, and how many there are (fragmentation)
  std::uint64_t largestFree() const;
  std::size_t freeRanges() const;
  // the freed ranges waiting for their frames
  std::size_t pendingRanges() const;

private:
  void insertFree(std::uint64_t offset, std::uint64_t size);
  void eraseFree(std::map<std::uint64_t, std::uint64_t>::iterator it);
};

} // namespace tb::lit
