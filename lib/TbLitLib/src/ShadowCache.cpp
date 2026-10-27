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

#include "ShadowCache.h"

#include "LitStages.h"

#include <algorithm>

namespace tb::lit
{
namespace
{

constexpr auto FnvBasis = std::uint64_t{14695981039346656037ull};
constexpr auto FnvPrime = std::uint64_t{1099511628211ull};

std::uint64_t fnv(std::uint64_t hash, const void* data, const std::size_t size)
{
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i)
  {
    hash = (hash ^ bytes[i]) * FnvPrime;
  }
  return hash;
}

template <typename T>
std::uint64_t fnvOf(const std::uint64_t hash, const T& value)
{
  return fnv(hash, &value, sizeof(value));
}

// splitmix64's finaliser: a term's bits spread over the word, so that the sum of terms
// is order-independent and still moves with any one of them
std::uint64_t mix(std::uint64_t x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

} // namespace

ShadowCvars shadowCvarsOf(const EngineFrameDefaults& frame)
{
  auto cvars = ShadowCvars{};
  cvars.skipShadows = frame.skipShadows;
  cvars.useShadowAtlas = frame.useShadowAtlas;
  cvars.shadowMapAtlasSize = frame.shadowMapAtlasSize;
  cvars.shadowMapFrustumFov = frame.shadowMapFrustumFov;
  cvars.shadowMapSingleSide = frame.shadowMapSingleSide;
  cvars.shadowMapJitterScale = frame.shadowMapJitterScale;
  cvars.shadowMapRandomizeJitter = frame.shadowMapRandomizeJitter;
  cvars.shadowMapSamples = frame.shadowMapSamples;
  cvars.shadowMapSplits = frame.shadowMapSplits;
  cvars.shadowMapSplitWeight = frame.shadowMapSplitWeight;
  cvars.shadowMapLodScale = frame.shadowMapLodScale;
  cvars.shadowMapLodBias = frame.shadowMapLodBias;
  cvars.shadowMapPolygonFactor = frame.shadowMapPolygonFactor;
  cvars.vkShadowMapPolygonOffset = frame.vkShadowMapPolygonOffset;
  cvars.shadowMapOccluderFacing = frame.shadowMapOccluderFacing;
  cvars.shadowMapRegularDepthBiasScale = frame.shadowMapRegularDepthBiasScale;
  cvars.shadowMapSunDepthBiasScale = frame.shadowMapSunDepthBiasScale;
  cvars.forceShadowMapsOnAlphaTestedSurfaces = frame.forceShadowMapsOnAlphaTestedSurfaces;
  cvars.useShadowSurfaceScissor = frame.useShadowSurfaceScissor;
  return cvars;
}

std::uint64_t shadowLightVersion(const LightRecord& light)
{
  const auto& parms = light.light.parms;
  const auto& derived = light.derived;
  const auto& projection = derived.projection;
  auto hash = FnvBasis;
  hash = fnvOf(hash, projection.baseLightProject);
  hash = fnvOf(hash, projection.inverseBaseLightProject);
  hash = fnvOf(hash, projection.globalLightOrigin);
  hash = fnvOf(hash, parms.origin);
  hash = fnvOf(hash, parms.axis);
  hash = fnvOf(hash, parms.lightCenter);
  hash = fnvOf(hash, parms.lightRadius);
  const auto flags = std::array<bool, 4>{
    parms.pointLight, parms.parallel, engineLightNoShadows(light), derived.hasVolume};
  hash = fnvOf(hash, flags);
  // the casting shader's flags: whether it casts (MF_NOSHADOWS, LightCastsShadows) and
  // its spectrum; none while it is pending (the light does not cast then)
  const auto* shader = derived.shader.pending ? nullptr : derived.shader.material.get();
  const auto shaderFlags = std::array<int, 3>{
    shader ? 1 : 0,
    shader && engineLightCastsShadows(*shader) ? 1 : 0,
    shader ? shader->spectrum : 0};
  hash = fnvOf(hash, shaderFlags);
  // the area references (LP3.2-FIX's items: a light crossing the hull boundary flips the
  // outside set in or out, one crossing an area portal changes its set)
  const auto areas = std::array<int, 3>{
    derived.areaNum, derived.hasAreaTree ? 1 : 0, int(derived.areas.size())};
  hash = fnvOf(hash, areas);
  hash = fnv(hash, derived.areas.data(), derived.areas.size() * sizeof(int));
  return hash;
}

std::uint64_t shadowCasterTerm(
  const ChunkKey& key,
  const bool instance,
  const std::uint64_t casterVersion,
  const EngineGlMatrix& modelMatrix,
  const CastClass castClass,
  const AreaSurface& surface)
{
  auto hash = FnvBasis;
  hash = fnvOf(hash, key.area);
  hash = fnvOf(hash, key.entity);
  hash = fnv(hash, key.material.data(), key.material.size());
  hash = fnvOf(hash, key.cell);
  hash = fnvOf(hash, key.part);
  hash = fnvOf(hash, instance);
  hash = fnvOf(hash, casterVersion);
  hash = fnvOf(hash, modelMatrix);
  hash = fnvOf(hash, castClass);
  const auto membership =
    std::array<int, 3>{int(surface.kind), surface.area, surface.refs.empty() ? 1 : 0};
  hash = fnvOf(hash, membership);
  return mix(hash);
}

std::uint64_t shadowPerforatedTerm(
  const ChunkKey& key,
  const bool instance,
  const MaterialEntry* material,
  const EvaluatedRegisters* registers)
{
  auto hash = FnvBasis;
  hash = fnvOf(hash, key.area);
  hash = fnvOf(hash, key.entity);
  hash = fnv(hash, key.material.data(), key.material.size());
  hash = fnvOf(hash, key.cell);
  hash = fnvOf(hash, key.part);
  hash = fnvOf(hash, instance);
  if (!material || !registers)
  {
    // pending: drawn solid and provisional until they arrive
    hash = fnvOf(hash, std::uint8_t{0xff});
    return mix(hash);
  }
  hash = fnvOf(hash, material->revision);
  auto carried = enginePerforatedShadowOpening();
  const auto none = Vec4{-1.0f, -1.0f, -1.0f, -1.0f};
  for (const auto& draw :
       enginePerforatedShadowDraws(*material, registers->registers, carried))
  {
    hash = fnvOf(hash, draw.program);
    hash = fnvOf(hash, draw.state);
    hash = fnvOf(hash, draw.slopeBias);
    hash = fnvOf(hash, draw.depthBias);
    hash = fnvOf(hash, draw.color.value_or(none));
    hash = fnvOf(hash, draw.alphaTest.value_or(none));
    hash = fnvOf(hash, draw.stage);
    hash = fnvOf(hash, draw.textureMatrix);
    hash = fnvOf(hash, draw.texGenEnabled);
    hash = fnvOf(hash, draw.screenTexgen);
    hash = fnvOf(hash, draw.bumpStage);
    hash = fnvOf(hash, draw.wobbleSky);
  }
  return mix(hash);
}

std::uint64_t shadowPerforatedImageTerm(
  const std::uint64_t slot, const bool complete, const std::uint64_t version)
{
  auto hash = FnvBasis;
  hash = fnvOf(hash, slot);
  hash = fnvOf(hash, complete);
  hash = fnvOf(hash, complete ? version : std::uint64_t{0});
  return mix(hash);
}

std::int64_t atlasOverlap(const AtlasRect& a, const AtlasRect& b)
{
  const auto x0 = std::max(a.x, b.x);
  const auto y0 = std::max(a.y, b.y);
  const auto x1 = std::min(a.x + a.size, b.x + b.size);
  const auto y1 = std::min(a.y + a.size, b.y + b.size);
  return x1 > x0 && y1 > y0 ? std::int64_t(x1 - x0) * std::int64_t(y1 - y0) : 0;
}

ShadowTileAllocator::ShadowTileAllocator(const int mapSize, const int minSize)
  : m_minSize{minSize}
{
  build(AtlasRect{0, 0, mapSize}, -1);
}

int ShadowTileAllocator::build(const AtlasRect& rect, const int parent)
{
  const auto index = int(m_nodes.size());
  auto node = Node{};
  node.rect = rect;
  node.parent = parent;
  m_nodes.push_back(node);
  if (rect.size / 2 >= m_minSize)
  {
    const auto c = rect.size / 2;
    // BuildTree's offsets ( -s, s ), ( -s, -s ), ( s, -s ), ( s, s ) (TileMap.cpp:93-104)
    const auto corners = std::array<std::array<int, 2>, 4>{{
      {rect.x, rect.y + c},
      {rect.x, rect.y},
      {rect.x + c, rect.y},
      {rect.x + c, rect.y + c},
    }};
    for (std::size_t i = 0; i < 4; ++i)
    {
      const auto child = build(AtlasRect{corners[i][0], corners[i][1], c}, index);
      m_nodes[std::size_t(index)].children[i] = child;
    }
  }
  return index;
}

std::optional<AtlasRect> ShadowTileAllocator::allocate(const int size)
{
  // GetTile: the level of the next power of two at or above size
  auto side = m_minSize;
  while (side < size)
  {
    side *= 2;
  }
  if (side >= m_nodes.front().rect.size)
  {
    return std::nullopt;
  }
  // FindNode: depth first in child order from the root's children; a taken node and its
  // subtree are passed over, as a node of the level with a taken node below it
  auto stack = std::vector<int>{};
  const auto push = [&](const Node& node) {
    for (auto i = std::size_t{4}; i-- > 0;)
    {
      if (node.children[i] >= 0)
      {
        stack.push_back(node.children[i]);
      }
    }
  };
  push(m_nodes.front());
  while (!stack.empty())
  {
    const auto index = stack.back();
    stack.pop_back();
    const auto& node = m_nodes[std::size_t(index)];
    if (node.taken)
    {
      continue;
    }
    if (node.rect.size == side)
    {
      if (node.takenBelow == 0)
      {
        take(index);
        return node.rect;
      }
      continue;
    }
    push(node);
  }
  return std::nullopt;
}

int ShadowTileAllocator::nodeOf(const AtlasRect& rect) const
{
  auto index = 0;
  while (index >= 0)
  {
    const auto& node = m_nodes[std::size_t(index)];
    if (node.rect == rect)
    {
      return index;
    }
    if (node.rect.size <= rect.size)
    {
      return -1;
    }
    auto next = -1;
    for (const auto child : node.children)
    {
      if (child < 0)
      {
        continue;
      }
      const auto& r = m_nodes[std::size_t(child)].rect;
      if (
        rect.x >= r.x && rect.x < r.x + r.size && rect.y >= r.y && rect.y < r.y + r.size)
      {
        next = child;
        break;
      }
    }
    index = next;
  }
  return -1;
}

void ShadowTileAllocator::take(const int node)
{
  m_nodes[std::size_t(node)].taken = true;
  ++m_taken;
  for (auto up = m_nodes[std::size_t(node)].parent; up >= 0;
       up = m_nodes[std::size_t(up)].parent)
  {
    ++m_nodes[std::size_t(up)].takenBelow;
  }
}

bool ShadowTileAllocator::reserve(const AtlasRect& rect)
{
  const auto index = nodeOf(rect);
  if (index < 0)
  {
    return false;
  }
  const auto& node = m_nodes[std::size_t(index)];
  if (node.taken || node.takenBelow != 0)
  {
    return false;
  }
  for (auto up = node.parent; up >= 0; up = m_nodes[std::size_t(up)].parent)
  {
    if (m_nodes[std::size_t(up)].taken)
    {
      return false;
    }
  }
  take(index);
  return true;
}

void ShadowTileAllocator::release(const AtlasRect& rect)
{
  const auto index = nodeOf(rect);
  if (index < 0 || !m_nodes[std::size_t(index)].taken)
  {
    return;
  }
  m_nodes[std::size_t(index)].taken = false;
  --m_taken;
  for (auto up = m_nodes[std::size_t(index)].parent; up >= 0;
       up = m_nodes[std::size_t(up)].parent)
  {
    --m_nodes[std::size_t(up)].takenBelow;
  }
}

void ShadowTileAllocator::clear()
{
  for (auto& node : m_nodes)
  {
    node.taken = false;
    node.takenBelow = 0;
  }
  m_taken = 0;
}

std::size_t ShadowTileAllocator::taken() const
{
  return m_taken;
}

std::vector<AtlasRect> uncoveredRects(
  const AtlasRect& rect, const std::span<const AtlasRect> covering, const int minSize)
{
  auto overlapped = false;
  for (const auto& other : covering)
  {
    const auto shared = atlasOverlap(rect, other);
    if (shared == std::int64_t(rect.size) * std::int64_t(rect.size))
    {
      return {};
    }
    overlapped = overlapped || shared > 0;
  }
  if (!overlapped || rect.size / 2 < minSize)
  {
    return {rect};
  }
  auto parts = std::vector<AtlasRect>{};
  const auto c = rect.size / 2;
  for (const auto& part :
       {AtlasRect{rect.x, rect.y, c},
        AtlasRect{rect.x + c, rect.y, c},
        AtlasRect{rect.x, rect.y + c, c},
        AtlasRect{rect.x + c, rect.y + c, c}})
  {
    const auto uncovered = uncoveredRects(part, covering, minSize);
    parts.insert(parts.end(), uncovered.begin(), uncovered.end());
  }
  return parts;
}

ShadowRelocationPlan planShadowRelocation(
  const std::span<const ShadowMove> moves, const int scratchSize, const int minSize)
{
  auto plan = ShadowRelocationPlan{};
  if (moves.empty())
  {
    return plan;
  }
  auto scratch = ShadowTileAllocator{scratchSize, minSize};
  const auto save = [&](const std::size_t move, const AtlasRect& slot) {
    plan.copies.push_back(ShadowCopy{true, moves[move].from, slot, move});
  };
  const auto restore = [&](const std::size_t move, const AtlasRect& slot) {
    plan.copies.push_back(ShadowCopy{false, moves[move].to, slot, move});
  };

  // every source first when they all fit
  {
    auto slots = std::vector<AtlasRect>{};
    for (const auto& move : moves)
    {
      const auto slot = scratch.allocate(move.from.size);
      if (!slot)
      {
        break;
      }
      slots.push_back(*slot);
    }
    if (slots.size() == moves.size())
    {
      for (std::size_t m = 0; m < moves.size(); ++m)
      {
        save(m, slots[m]);
      }
      for (std::size_t m = 0; m < moves.size(); ++m)
      {
        restore(m, slots[m]);
      }
      return plan;
    }
    scratch.clear();
  }

  // in batches
  auto remaining = std::vector<std::size_t>(moves.size());
  for (std::size_t m = 0; m < moves.size(); ++m)
  {
    remaining[m] = m;
  }
  // the moves whose source sits in the scratch, by their slot
  auto saved = std::vector<std::optional<AtlasRect>>(moves.size());
  const auto blocked = [&](const std::size_t m, const std::vector<bool>& inBatch) {
    for (const auto other : remaining)
    {
      if (
        other != m && !saved[other] && !inBatch[other]
        && atlasOverlap(moves[m].to, moves[other].from) > 0)
      {
        return true;
      }
    }
    return false;
  };
  while (!remaining.empty())
  {
    auto inBatch = std::vector<bool>(moves.size(), false);
    auto batch = std::vector<std::size_t>{};
    auto toSave = std::vector<std::pair<std::size_t, AtlasRect>>{};
    for (auto grew = true; grew;)
    {
      grew = false;
      for (const auto m : remaining)
      {
        if (inBatch[m] || blocked(m, inBatch))
        {
          continue;
        }
        if (!saved[m])
        {
          const auto slot = scratch.allocate(moves[m].from.size);
          if (!slot)
          {
            continue;
          }
          toSave.emplace_back(m, *slot);
        }
        inBatch[m] = true;
        batch.push_back(m);
        grew = true;
      }
    }
    if (batch.empty())
    {
      // a cycle: one move's source saved into one slot and parked
      auto parked = false;
      for (const auto m : remaining)
      {
        if (saved[m])
        {
          continue;
        }
        if (const auto slot = scratch.allocate(moves[m].from.size))
        {
          save(m, *slot);
          saved[m] = slot;
          parked = true;
          break;
        }
      }
      if (!parked)
      {
        // the scratch cannot hold what the cycles need: the rest is drawn again
        for (const auto m : remaining)
        {
          if (saved[m])
          {
            scratch.release(*saved[m]);
          }
          plan.redraw.push_back(m);
        }
        // a parked source's slot is not restored: its move is drawn again
        std::erase_if(plan.copies, [&](const ShadowCopy& copy) {
          return std::ranges::find(plan.redraw, copy.move) != plan.redraw.end();
        });
        return plan;
      }
      continue;
    }
    for (const auto& [m, slot] : toSave)
    {
      save(m, slot);
      saved[m] = slot;
    }
    for (const auto m : batch)
    {
      restore(m, *saved[m]);
      scratch.release(*saved[m]);
      saved[m].reset();
    }
    std::erase_if(remaining, [&](const std::size_t m) { return inBatch[m]; });
  }
  return plan;
}

void ShadowCache::beginFrame(
  const std::span<const PreviewId> casting, std::vector<AtlasRect>& released)
{
  std::erase_if(m_records, [&](const auto& entry) {
    if (std::ranges::find(casting, entry.first) != casting.end())
    {
      return false;
    }
    for (const auto& tile : entry.second.tiles)
    {
      if (tile.placed)
      {
        released.push_back(tile.rect);
      }
    }
    return true;
  });
}

ShadowRecord& ShadowCache::record(const PreviewId light)
{
  auto& record = m_records[light];
  record.light = light;
  return record;
}

void ShadowCache::drop(const PreviewId light)
{
  m_records.erase(light);
}

void ShadowCache::invalidateTiles()
{
  for (auto& [light, record] : m_records)
  {
    for (auto& tile : record.tiles)
    {
      tile.valid = false;
    }
  }
}

const ShadowRecord* ShadowCache::find(const PreviewId light) const
{
  const auto it = m_records.find(light);
  return it != m_records.end() ? &it->second : nullptr;
}

ShadowRecord* ShadowCache::find(const PreviewId light)
{
  const auto it = m_records.find(light);
  return it != m_records.end() ? &it->second : nullptr;
}

const ShadowCache::Records& ShadowCache::records() const
{
  return m_records;
}

std::size_t ShadowCache::size() const
{
  return m_records.size();
}

void ShadowCache::clear()
{
  m_records.clear();
}

} // namespace tb::lit
