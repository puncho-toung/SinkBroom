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
#include <string_view>

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

bool GeometryArenas::syncInstances(
  const std::map<ChunkKey, ModelInstance>& instances, const std::uint64_t frame)
{
  // the instances naming each key now
  auto counts = std::map<SharedGeometryKey, SharedPlacement>{};
  m_instanceKeys.clear();
  for (const auto& [key, instance] : instances)
  {
    if (!instance.geometry || instance.geometry->indexCount() == 0)
    {
      continue;
    }
    auto& count = counts[instance.geometry->key];
    count.geometry = instance.geometry;
    ++count.instances;
    m_instanceKeys.emplace(key, instance.geometry->key);
  }
  // D35, D36: a key no instance names goes, its ranges after the frames in flight
  for (auto it = m_shared.begin(); it != m_shared.end();)
  {
    if (counts.contains(it->first))
    {
      ++it;
      continue;
    }
    if (it->second.placement)
    {
      freePlacement(*it->second.placement, frame);
    }
    m_sharedQueued.erase(it->first);
    it = m_shared.erase(it);
  }
  for (auto& [key, count] : counts)
  {
    auto [it, added] = m_shared.try_emplace(key);
    it->second.instances = count.instances;
    if (added)
    {
      it->second.geometry = std::move(count.geometry);
      m_sharedQueued.insert(key);
    }
  }
  return pending();
}

void GeometryArenas::setAllocationRoom(std::function<bool()> room)
{
  m_allocationRoom = std::move(room);
}

std::uint64_t GeometryArenas::headroomRefusals() const
{
  return m_headroomRefusals;
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
  return upload(&commandList, chunks, budget);
}

void GeometryArenas::setCpuArenasForTesting(const std::uint64_t arenaBytes)
{
  m_cpuArenasForTesting = true;
  m_arenaBytes = arenaBytes;
}

Result<std::uint64_t> GeometryArenas::placeForTesting(
  const std::map<ChunkKey, Chunk>& chunks, const std::uint64_t budget)
{
  return upload(nullptr, chunks, budget);
}

std::optional<ChunkPlacement> GeometryArenas::place(
  const std::uint64_t vertexBytes, const std::uint64_t indexBytes, std::string& error)
{
  m_refusedForHeadroom = false;
  const auto vertices = allocate(m_vertexArenas, vertexBytes, true);
  const auto indexes = vertices ? allocate(m_indexArenas, indexBytes, false)
                                : std::optional<std::pair<std::size_t, ArenaRange>>{};
  if (!vertices || !indexes)
  {
    if (vertices)
    {
      m_vertexArenas[vertices->first].ranges.free(vertices->second, 0);
    }
    // U2.3: refused for the headroom, the rest wait, queued, until allocations are free
    if (!m_refusedForHeadroom)
    {
      error = fmt::format(
        "Cannot create a geometry arena for {} bytes", std::max(vertexBytes, indexBytes));
    }
    return std::nullopt;
  }
  auto placement = ChunkPlacement{};
  placement.vertexArena = vertices->first;
  placement.vertices = vertices->second;
  placement.indexArena = indexes->first;
  placement.indexes = indexes->second;
  return placement;
}

Result<std::uint64_t> GeometryArenas::upload(
  nvrhi::ICommandList* commandList,
  const std::map<ChunkKey, Chunk>& chunks,
  const std::uint64_t budget)
{
  auto written = std::uint64_t{0};
  if (budget == 0)
  {
    return written;
  }
  // D43, D78: written through the list's upload chunks on the graphics queue; the index
  // range is padded to whole words
  const auto write = [&](
                       const ChunkPlacement& placement,
                       const std::vector<EngineDrawVert>& verts,
                       const std::vector<std::uint16_t>& indexes,
                       const std::uint64_t vertexBytes,
                       const std::uint64_t indexBytes) {
    if (!commandList)
    {
      return;
    }
    commandList->writeBuffer(
      m_vertexArenas[placement.vertexArena].buffer,
      verts.data(),
      vertexBytes,
      placement.vertices.offset);
    m_indexScratch.assign(indexes.begin(), indexes.end());
    m_indexScratch.resize(std::size_t(indexBytes / 2), 0);
    commandList->writeBuffer(
      m_indexArenas[placement.indexArena].buffer,
      m_indexScratch.data(),
      indexBytes,
      placement.indexes.offset);
  };
  const auto indexBytesOf = [](const std::size_t count) {
    return (std::uint64_t(count) * 2 + IndexAlignment - 1) / IndexAlignment
           * IndexAlignment;
  };

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
    const auto indexBytes = indexBytesOf(chunk.indexes.size());
    // §3.10: the cap; a first chunk larger than it still goes
    if (written > 0 && written + vertexBytes + indexBytes > budget)
    {
      break;
    }
    auto error = std::string{};
    auto placement = place(vertexBytes, indexBytes, error);
    if (!placement)
    {
      if (!error.empty())
      {
        return Error{std::move(error)};
      }
      break;
    }
    write(*placement, chunk.verts, chunk.indexes, vertexBytes, indexBytes);
    placement->vertexCount = std::uint32_t(chunk.verts.size());
    placement->indexCount = std::uint32_t(chunk.indexes.size());
    placement->casterVersion = chunk.casterVersion;
    placement->appearanceVersion = chunk.appearanceVersion;
    m_placed.insert_or_assign(*it, *placement);

    written += vertexBytes + indexBytes;
    ++m_uploadedChunks;
    it = m_queued.erase(it);
  }

  // LP7.2 (D129): the shared model surfaces after the chunks, in key order, in the same
  // budget; each written once for every instance that names it
  for (auto it = m_sharedQueued.begin(); it != m_sharedQueued.end() && m_queued.empty();)
  {
    const auto found = m_shared.find(*it);
    if (found == m_shared.end() || !found->second.geometry)
    {
      it = m_sharedQueued.erase(it);
      continue;
    }
    const auto& surface = found->second.geometry->surface();
    const auto vertexBytes = std::uint64_t(surface.verts.size() * sizeof(EngineDrawVert));
    const auto indexBytes = indexBytesOf(surface.indexes.size());
    if (written > 0 && written + vertexBytes + indexBytes > budget)
    {
      break;
    }
    auto error = std::string{};
    auto placement = place(vertexBytes, indexBytes, error);
    if (!placement)
    {
      if (!error.empty())
      {
        return Error{std::move(error)};
      }
      break;
    }
    write(*placement, surface.verts, surface.indexes, vertexBytes, indexBytes);
    placement->vertexCount = std::uint32_t(surface.verts.size());
    placement->indexCount = std::uint32_t(surface.indexes.size());
    placement->shared = true;
    found->second.placement = *placement;

    written += vertexBytes + indexBytes;
    ++m_uploadedShared;
    it = m_sharedQueued.erase(it);
  }
  m_uploadedBytes += written;
  return written;
}

bool GeometryArenas::pending() const
{
  return !m_queued.empty() || !m_sharedQueued.empty();
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
  if (const auto it = m_placed.find(key); it != m_placed.end())
  {
    return it->second;
  }
  // LP7.2 (D129): an instance's shared surface
  const auto instance = m_instanceKeys.find(key);
  if (instance == m_instanceKeys.end())
  {
    return std::nullopt;
  }
  const auto shared = m_shared.find(instance->second);
  return shared != m_shared.end() ? shared->second.placement : std::nullopt;
}

std::size_t GeometryArenas::queuedShared() const
{
  return m_sharedQueued.size();
}

std::size_t GeometryArenas::placedShared() const
{
  return std::size_t(std::ranges::count_if(
    m_shared, [](const auto& entry) { return entry.second.placement.has_value(); }));
}

std::size_t GeometryArenas::sharedInstances(const SharedGeometryKey& key) const
{
  const auto it = m_shared.find(key);
  return it != m_shared.end() ? it->second.instances : 0;
}

std::uint64_t GeometryArenas::uploadedShared() const
{
  return m_uploadedShared;
}

std::size_t GeometryArenas::sharedRecords() const
{
  auto records = std::set<std::string_view>{};
  for (const auto& [key, shared] : m_shared)
  {
    records.insert(key.record);
  }
  return records.size();
}

std::uint64_t GeometryArenas::placedSharedBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& [key, shared] : m_shared)
  {
    if (shared.placement)
    {
      bytes += shared.placement->vertices.size + shared.placement->indexes.size;
    }
  }
  return bytes;
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
  m_shared.clear();
  m_sharedQueued.clear();
  m_instanceKeys.clear();
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
  if (!m_device && !m_cpuArenasForTesting)
  {
    return std::nullopt;
  }
  // U2.3: no new arena within the allocation headroom
  if (m_allocationRoom && !m_allocationRoom())
  {
    m_refusedForHeadroom = true;
    ++m_headroomRefusals;
    return std::nullopt;
  }
  // S8: one more arena of the kind; a chunk larger than an arena gets one of its size
  const auto alignment = vertices ? VertexAlignment : IndexAlignment;
  const auto size =
    std::max(m_arenaBytes, (bytes + alignment - 1) / alignment * alignment);
  auto buffer =
    m_device ? m_device->createBuffer(arenaDesc(size, vertices)) : nvrhi::BufferHandle{};
  if (!buffer && !m_cpuArenasForTesting)
  {
    return std::nullopt;
  }
  arenas.push_back(Arena{std::move(buffer), RangeAllocator{size, alignment}});
  const auto range = arenas.back().ranges.allocate(bytes);
  return range ? std::optional{std::pair{arenas.size() - 1, *range}} : std::nullopt;
}

} // namespace tb::lit
