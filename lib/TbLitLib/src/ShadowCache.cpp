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

void ShadowCache::beginFrame(const std::span<const PreviewId> casting)
{
  std::erase_if(m_records, [&](const auto& entry) {
    return std::ranges::find(casting, entry.first) == casting.end();
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
