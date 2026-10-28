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

#include "RangeAllocator.h"

#include <algorithm>
#include <iterator>

namespace tb::lit
{

RangeAllocator::RangeAllocator(
  const std::uint64_t capacity, const std::uint64_t alignment)
  : m_capacity{capacity}
  , m_alignment{std::max(alignment, std::uint64_t{1})}
{
  // only whole aligned units are handed out
  const auto usable = m_capacity - m_capacity % m_alignment;
  if (usable > 0)
  {
    insertFree(0, usable);
  }
}

std::optional<ArenaRange> RangeAllocator::allocate(const std::uint64_t size)
{
  if (size == 0 || size > m_capacity)
  {
    return std::nullopt;
  }
  const auto aligned = (size + m_alignment - 1) / m_alignment * m_alignment;
  // the best fit: the smallest free range that holds it, the lowest offset among equals
  const auto best = m_freeBySize.lower_bound(std::pair{aligned, std::uint64_t{0}});
  if (best == m_freeBySize.end())
  {
    return std::nullopt;
  }
  const auto [freeSize, offset] = *best;
  eraseFree(m_freeByOffset.find(offset));
  if (freeSize > aligned)
  {
    insertFree(offset + aligned, freeSize - aligned);
  }
  m_used += aligned;
  return ArenaRange{offset, aligned};
}

std::optional<ArenaRange> RangeAllocator::allocate(
  const std::uint64_t size, const std::uint64_t alignment)
{
  const auto offsetAlignment = std::max(alignment, m_alignment);
  if ((offsetAlignment & (offsetAlignment - 1)) != 0)
  {
    return std::nullopt;
  }
  if (offsetAlignment == m_alignment)
  {
    return allocate(size);
  }
  if (size == 0 || size > m_capacity)
  {
    return std::nullopt;
  }
  const auto aligned = (size + m_alignment - 1) / m_alignment * m_alignment;
  // the best fit with the padding: the smallest free range that holds the padding up to
  // its first aligned offset and the size, the lowest offset among equals
  for (auto it = m_freeBySize.lower_bound(std::pair{aligned, std::uint64_t{0}});
       it != m_freeBySize.end();
       ++it)
  {
    const auto [freeSize, offset] = *it;
    const auto start = (offset + offsetAlignment - 1) & ~(offsetAlignment - 1);
    const auto padding = start - offset;
    if (padding + aligned > freeSize)
    {
      continue;
    }
    eraseFree(m_freeByOffset.find(offset));
    if (padding > 0)
    {
      insertFree(offset, padding);
    }
    if (freeSize > padding + aligned)
    {
      insertFree(start + aligned, freeSize - padding - aligned);
    }
    m_used += aligned;
    return ArenaRange{start, aligned};
  }
  return std::nullopt;
}

void RangeAllocator::free(const ArenaRange& range, const std::uint64_t frame)
{
  if (range.size == 0)
  {
    return;
  }
  m_used -= std::min(m_used, range.size);
  m_pendingBytes += range.size;
  m_pending.push_back(Pending{range, frame});
}

void RangeAllocator::reclaim(const std::uint64_t completedFrames)
{
  // frame f has finished once completedFrames > f
  auto kept = std::vector<Pending>{};
  for (const auto& pending : m_pending)
  {
    if (pending.frame < completedFrames)
    {
      m_pendingBytes -= pending.range.size;
      insertFree(pending.range.offset, pending.range.size);
    }
    else
    {
      kept.push_back(pending);
    }
  }
  m_pending = std::move(kept);
}

std::uint64_t RangeAllocator::capacity() const
{
  return m_capacity;
}

std::uint64_t RangeAllocator::alignment() const
{
  return m_alignment;
}

std::uint64_t RangeAllocator::usedBytes() const
{
  return m_used;
}

std::uint64_t RangeAllocator::pendingBytes() const
{
  return m_pendingBytes;
}

std::uint64_t RangeAllocator::freeBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& [offset, size] : m_freeByOffset)
  {
    bytes += size;
  }
  return bytes;
}

std::uint64_t RangeAllocator::largestFree() const
{
  return m_freeBySize.empty() ? 0 : m_freeBySize.rbegin()->first;
}

std::size_t RangeAllocator::freeRanges() const
{
  return m_freeByOffset.size();
}

std::size_t RangeAllocator::pendingRanges() const
{
  return m_pending.size();
}

void RangeAllocator::insertFree(std::uint64_t offset, std::uint64_t size)
{
  // merged with the free neighbours on both sides
  auto next = m_freeByOffset.lower_bound(offset);
  if (next != m_freeByOffset.end() && offset + size == next->first)
  {
    size += next->second;
    eraseFree(next);
  }
  next = m_freeByOffset.lower_bound(offset);
  if (next != m_freeByOffset.begin())
  {
    const auto previous = std::prev(next);
    if (previous->first + previous->second == offset)
    {
      offset = previous->first;
      size += previous->second;
      eraseFree(previous);
    }
  }
  m_freeByOffset.emplace(offset, size);
  m_freeBySize.emplace(size, offset);
}

void RangeAllocator::eraseFree(const std::map<std::uint64_t, std::uint64_t>::iterator it)
{
  m_freeBySize.erase(std::pair{it->second, it->first});
  m_freeByOffset.erase(it);
}

} // namespace tb::lit
