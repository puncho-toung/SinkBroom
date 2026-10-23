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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_main.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "ViewProbes.h"

#include <algorithm>

namespace tb::lit
{
namespace
{

// idVec3::LengthSqr of a - b (Vector.h), the engine's float arithmetic
float distanceSquared(const EngineVec3& a, const EngineVec3& b)
{
  const auto x = a[0] - b[0];
  const auto y = a[1] - b[1];
  const auto z = a[2] - b[2];
  return x * x + y * y + z * z;
}

// idMath's Max (Math.h): ( x > y ) ? x : y
float maxOf(const float x, const float y)
{
  return x > y ? x : y;
}

} // namespace

ViewProbes chooseViewProbes(
  const ProbeTable* probes,
  const EngineVec3& viewOrigin,
  const int viewArea,
  const std::vector<bool>& connectedAreas,
  const bool flowed,
  const ProbeImageStateOf& imageState)
{
  // set safe defaults (:489-509): the default cube everywhere, blends ( 1, 0, 0, 0 ), the
  // positions and the SH zero
  auto view = ViewProbes{};
  view.globalProbeBounds = engineClearedBounds();

  // early out (:511-515): no view area (and D94: no tree), a subview (none here)
  if (!probes || !flowed || viewArea == -1)
  {
    return view;
  }

  // :517-531: every probe that is not closed off behind a door (r_useLightAreaCulling 1)
  auto candidates = std::vector<const ProbeRecord*>{};
  for (const auto& probe : probes->records)
  {
    if (probe.areaNum != -1)
    {
      const auto area = std::size_t(probe.areaNum);
      if (probe.areaNum < -1 || area >= connectedAreas.size() || !connectedAreas[area])
      {
        continue;
      }
    }
    candidates.push_back(&probe);
  }
  view.candidates = candidates.size();
  if (candidates.empty())
  {
    return view;
  }

  // sort by distance (:538-542, idSort_CompareEnvprobe: ( vieworg - origin
  // ).LengthSqr()); K32: equal distances by id, where the engine's quicksort keeps no
  // order
  std::ranges::sort(candidates, [&](const auto* a, const auto* b) {
    const auto da = distanceSquared(viewOrigin, a->origin);
    const auto db = distanceSquared(viewOrigin, b->origin);
    return da != db ? da < db : a->id < b->id;
  });

  const auto state = [&](const MaterialImageKey& key) {
    const auto s = imageState ? imageState(key) : ProbeImageState::Pending;
    if (s == ProbeImageState::Pending)
    {
      // D88: not loaded: the default stands and the frame is unconverged
      view.imagesPending = true;
    }
    return s;
  };

  // :544-555: the nearest's bounds and (loaded and not defaulted) depth cube; its
  // irradiance image is not taken (D85: t7 binds _default)
  const auto& nearest = *candidates[0];
  view.globalProbeBounds = nearest.globalProbeBounds;
  if (state(nearest.images[2]) == ProbeImageState::Ready)
  {
    view.depth = nearest.images[2];
  }

  // :557-583: the three closest probes' positions, the nearest's where there are fewer,
  // with the depth cube's validity in w
  const auto count = candidates.size();
  for (std::size_t i = 0; i < 3; ++i)
  {
    const auto& origin = i < count ? candidates[i]->origin : nearest.origin;
    auto depthValid = 0.0f;
    if (i < count && state(candidates[i]->images[2]) == ProbeImageState::Ready)
    {
      depthValid = 1.0f;
    }
    view.probePositions[i] = {origin[0], origin[1], origin[2], depthValid};
  }

  // :585-621: distance-falloff weights, the cutoff the 4th-nearest's distance, or 1.5
  // times the farthest active probe's with fewer than four
  auto probeDist = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  for (std::size_t i = 0; i < 4 && i < count; ++i)
  {
    const auto& origin = candidates[i]->origin;
    probeDist[i] = engineVec3Length(EngineVec3{
      origin[0] - viewOrigin[0], origin[1] - viewOrigin[1], origin[2] - viewOrigin[2]});
  }
  const auto dCut =
    count > 3 ? maxOf(probeDist[3], 1e-4f) : maxOf(probeDist[count - 1] * 1.5f, 1e-4f);
  auto bary = std::array<float, 3>{0.0f, 0.0f, 0.0f};
  auto weightSum = 0.0f;
  for (std::size_t i = 0; i < 3 && i < count; ++i)
  {
    auto w = maxOf(0.0f, 1.0f - probeDist[i] / dCut);
    // squared: the weight and its slope reach 0 at dCut
    w *= w;
    bary[i] = w;
    weightSum += w;
  }
  if (weightSum > 1e-4f)
  {
    // idVec3::operator/=: times the reciprocal
    const auto inva = 1.0f / weightSum;
    for (auto& w : bary)
    {
      w *= inva;
    }
  }
  else
  {
    // degenerate: the nearest probe only
    bary = {1.0f, 0.0f, 0.0f};
  }

  // :626-634: the L2 coefficients blended with the same weights
  for (std::size_t i = 0; i < 9; ++i)
  {
    auto s = EngineVec3{0.0f, 0.0f, 0.0f};
    for (std::size_t p = 0; p < 3 && p < count; ++p)
    {
      const auto& c = candidates[p]->shIrradiance[i];
      const auto x = c[0] * bary[p];
      const auto y = c[1] * bary[p];
      const auto z = c[2] * bary[p];
      s[0] += x;
      s[1] += y;
      s[2] += z;
    }
    view.sh[i] = {s[0], s[1], s[2], 0.0f};
  }

  // :636
  view.blends = {bary[0], bary[1], bary[2], 0.0f};

  // :638-644: the radiance cubes that are not defaulted
  for (std::size_t i = 0; i < count && i < 3; ++i)
  {
    if (state(candidates[i]->images[1]) == ProbeImageState::Ready)
    {
      view.radiance[i] = candidates[i]->images[1];
    }
  }

  // :646-653: the depth cubes of probes 1 and 2
  for (std::size_t i = 1; i < count && i < 3; ++i)
  {
    if (state(candidates[i]->images[2]) == ProbeImageState::Ready)
    {
      view.probeDepth[i - 1] = candidates[i]->images[2];
    }
  }

  // :655-667: no usable radiance or depth cube: the bounds cleared, which tells the
  // backend not to march
  if (!view.radiance[0] && !view.radiance[1] && !view.radiance[2])
  {
    view.globalProbeBounds = engineClearedBounds();
  }
  if (!view.depth)
  {
    view.globalProbeBounds = engineClearedBounds();
  }
  return view;
}

} // namespace tb::lit
