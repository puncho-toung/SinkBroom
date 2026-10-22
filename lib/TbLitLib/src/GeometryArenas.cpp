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

#include "GeometryArenas.h"

#include <fmt/format.h>

#include <algorithm>

namespace tb::lit
{
namespace
{

// C4: idDrawVert's 32 bytes, the vertex buffer's stride: a vertex range starts at a
// whole vertex, so that a draw's startVertexLocation is its offset / 32
constexpr auto VertexAlignment = std::uint64_t{sizeof(EngineDrawVert)};
static_assert(VertexAlignment == 32);
// 16-bit indexes in whole 4-byte words: Vulkan's buffer updates and copies stay aligned
constexpr auto IndexAlignment = std::uint64_t{4};

nvrhi::BufferDesc arenaDesc(const std::uint64_t bytes, const bool vertices)
{
  // keepInitialState: writeBuffer moves it to CopyDest and the list puts it back
  auto desc = nvrhi::BufferDesc{};
  desc.byteSize = bytes;
  desc.debugName = vertices ? "Lit preview vertex arena" : "Lit preview index arena";
  desc.isVertexBuffer = vertices;
  desc.isIndexBuffer = !vertices;
  desc.initialState =
    vertices ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
  desc.keepInitialState = true;
  return desc;
}

} // namespace

void GeometryArenas::setDevice(nvrhi::IDevice& device, const std::uint64_t arenaBytes)
{
  m_device = &device;
  m_arenaBytes = arenaBytes;
}

bool GeometryArenas::sync(
  const std::map<ChunkKey, Chunk>& chunks, const std::uint64_t frame)
{
  // a chunk that went or changed gives its ranges back, after the frames in flight
  for (auto it = m_placed.begin(); it != m_placed.end();)
  {
    const auto found = chunks.find(it->first);
    if (
      found == chunks.end() || found->second.casterVersion != it->second.casterVersion
      || found->second.appearanceVersion != it->second.appearanceVersion)
    {
      freePlacement(it->second, frame);
      it = m_placed.erase(it);
    }
    else
    {
      ++it;
    }
  }
  std::erase_if(m_queued, [&](const ChunkKey& key) { return !chunks.contains(key); });

  // a new or changed chunk, with geometry, is uploaded
  for (const auto& [key, chunk] : chunks)
  {
    if (!chunk.verts.empty() && !chunk.indexes.empty() && !m_placed.contains(key))
    {
      m_queued.insert(key);
    }
  }
  return !m_queued.empty();
}

void GeometryArenas::reclaim(const std::uint64_t completedFrames)
{
  for (auto& arena : m_vertexArenas)
  {
    arena.ranges.reclaim(completedFrames);
  }
  for (auto& arena : m_indexArenas)
  {
    arena.ranges.reclaim(completedFrames);
  }
}

Result<std::uint64_t> GeometryArenas::recordUploads(
  nvrhi::ICommandList& commandList,
  const std::map<ChunkKey, Chunk>& chunks,
  const std::uint64_t budget)
{
  auto written = std::uint64_t{0};
  if (budget == 0)
  {
    return written;
  }
  for (auto it = m_queued.begin(); it != m_queued.end();)
  {
    const auto found = chunks.find(*it);
    if (found == chunks.end())
    {
      it = m_queued.erase(it);
      continue;
    }
    const auto& chunk = found->second;
    const auto vertexBytes = std::uint64_t(chunk.verts.size() * sizeof(EngineDrawVert));
    const auto indexBytes = (std::uint64_t(chunk.indexes.size()) * 2 + IndexAlignment - 1)
                            / IndexAlignment * IndexAlignment;
    // §3.10: the cap; a first chunk larger than it still goes
    if (written > 0 && written + vertexBytes + indexBytes > budget)
    {
      break;
    }

    const auto vertices = allocate(m_vertexArenas, vertexBytes, true);
    const auto indexes = vertices ? allocate(m_indexArenas, indexBytes, false)
                                  : std::optional<std::pair<std::size_t, ArenaRange>>{};
    if (!vertices || !indexes)
    {
      if (vertices)
      {
        m_vertexArenas[vertices->first].ranges.free(vertices->second, 0);
      }
      return Error{fmt::format(
        "Cannot create a geometry arena for {} bytes",
        std::max(vertexBytes, indexBytes))};
    }

    // D43, D78: written through the list's upload chunks on the graphics queue; the
    // index range is padded to whole words
    commandList.writeBuffer(
      m_vertexArenas[vertices->first].buffer,
      chunk.verts.data(),
      vertexBytes,
      vertices->second.offset);
    m_indexScratch.assign(chunk.indexes.begin(), chunk.indexes.end());
    m_indexScratch.resize(std::size_t(indexBytes / 2), 0);
    commandList.writeBuffer(
      m_indexArenas[indexes->first].buffer,
      m_indexScratch.data(),
      indexBytes,
      indexes->second.offset);

    auto placement = ChunkPlacement{};
    placement.vertexArena = vertices->first;
    placement.vertices = vertices->second;
    placement.indexArena = indexes->first;
    placement.indexes = indexes->second;
    placement.vertexCount = std::uint32_t(chunk.verts.size());
    placement.indexCount = std::uint32_t(chunk.indexes.size());
    placement.casterVersion = chunk.casterVersion;
    placement.appearanceVersion = chunk.appearanceVersion;
    m_placed.insert_or_assign(*it, placement);

    written += vertexBytes + indexBytes;
    ++m_uploadedChunks;
    it = m_queued.erase(it);
  }
  m_uploadedBytes += written;
  return written;
}

bool GeometryArenas::pending() const
{
  return !m_queued.empty();
}

std::size_t GeometryArenas::queuedChunks() const
{
  return m_queued.size();
}

std::size_t GeometryArenas::placedChunks() const
{
  return m_placed.size();
}

std::optional<ChunkPlacement> GeometryArenas::placement(const ChunkKey& key) const
{
  const auto it = m_placed.find(key);
  return it != m_placed.end() ? std::optional{it->second} : std::nullopt;
}

nvrhi::IBuffer* GeometryArenas::vertexBuffer(const std::size_t arena) const
{
  return arena < m_vertexArenas.size() ? m_vertexArenas[arena].buffer.Get() : nullptr;
}

nvrhi::IBuffer* GeometryArenas::indexBuffer(const std::size_t arena) const
{
  return arena < m_indexArenas.size() ? m_indexArenas[arena].buffer.Get() : nullptr;
}

std::size_t GeometryArenas::vertexArenas() const
{
  return m_vertexArenas.size();
}

std::size_t GeometryArenas::indexArenas() const
{
  return m_indexArenas.size();
}

std::uint64_t GeometryArenas::capacityBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto* arenas : {&m_vertexArenas, &m_indexArenas})
  {
    for (const auto& arena : *arenas)
    {
      bytes += arena.ranges.capacity();
    }
  }
  return bytes;
}

std::uint64_t GeometryArenas::usedBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto* arenas : {&m_vertexArenas, &m_indexArenas})
  {
    for (const auto& arena : *arenas)
    {
      bytes += arena.ranges.usedBytes();
    }
  }
  return bytes;
}

std::uint64_t GeometryArenas::pendingFreeBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto* arenas : {&m_vertexArenas, &m_indexArenas})
  {
    for (const auto& arena : *arenas)
    {
      bytes += arena.ranges.pendingBytes();
    }
  }
  return bytes;
}

std::uint64_t GeometryArenas::uploadedBytes() const
{
  return m_uploadedBytes;
}

std::uint64_t GeometryArenas::uploadedChunks() const
{
  return m_uploadedChunks;
}

void GeometryArenas::releaseAll()
{
  m_placed.clear();
  m_queued.clear();
  m_vertexArenas.clear();
  m_indexArenas.clear();
  m_indexScratch = {};
}

void GeometryArenas::freePlacement(
  const ChunkPlacement& placement, const std::uint64_t frame)
{
  if (placement.vertexArena < m_vertexArenas.size())
  {
    m_vertexArenas[placement.vertexArena].ranges.free(placement.vertices, frame);
  }
  if (placement.indexArena < m_indexArenas.size())
  {
    m_indexArenas[placement.indexArena].ranges.free(placement.indexes, frame);
  }
}

std::optional<std::pair<std::size_t, ArenaRange>> GeometryArenas::allocate(
  std::vector<Arena>& arenas, const std::uint64_t bytes, const bool vertices)
{
  for (std::size_t i = 0; i < arenas.size(); ++i)
  {
    if (const auto range = arenas[i].ranges.allocate(bytes))
    {
      return std::pair{i, *range};
    }
  }
  if (!m_device)
  {
    return std::nullopt;
  }
  // S8: one more arena of the kind; a chunk larger than an arena gets one of its size
  const auto alignment = vertices ? VertexAlignment : IndexAlignment;
  const auto size =
    std::max(m_arenaBytes, (bytes + alignment - 1) / alignment * alignment);
  auto buffer = m_device->createBuffer(arenaDesc(size, vertices));
  if (!buffer)
  {
    return std::nullopt;
  }
  arenas.push_back(Arena{std::move(buffer), RangeAllocator{size, alignment}});
  const auto range = arenas.back().ranges.allocate(bytes);
  return range ? std::optional{std::pair{arenas.size() - 1, *range}} : std::nullopt;
}

} // namespace tb::lit
