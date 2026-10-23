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

#pragma once

#include "lit/EngineLight.h"
#include "lit/MaterialDecls.h"
#include "lit/ProbeRecord.h"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace tb::lit
{

/**
 * D88: what a probe image's slot is for the choice: loaded (IsLoaded() &&
 * !IsDefaulted(): the slot binds a written level of its own), not loaded yet (Pending:
 * the default stands, the frame is unconverged) or defaulted (the service has no image,
 * Missing being final).
 */
enum class ProbeImageState
{
  Pending,
  Ready,
  Defaulted,
};

using ProbeImageStateOf = std::function<ProbeImageState(const MaterialImageKey&)>;

/**
 * What R_FindClosestEnvironmentProbes leaves in viewDef (tr_frontend_main.cpp:487-668)
 * for the ambient pass: the probe bounds (cleared or not), the three radiance cubes, the
 * nearest probe's depth cube and the next two's (nullopt: globalImages->
 * defaultUACRadianceCube, the preview's _defaultUACRadiance), the three probe positions
 * (xyz, w = the depth cube is loaded), the radiance blends (w0, w1, w2, 0) and the
 * blended L2 SH ((x, y, z, 0) each). irradianceImage has no field: the preview binds
 * _default at t7 on the probe path (D85). candidates is the connected probes' count;
 * imagesPending says that an image the choice read was not loaded yet (D88).
 */
struct ViewProbes
{
  EngineBounds globalProbeBounds = {};
  std::array<std::optional<MaterialImageKey>, 3> radiance;
  std::optional<MaterialImageKey> depth;
  std::array<std::optional<MaterialImageKey>, 2> probeDepth;
  std::array<std::array<float, 4>, 3> probePositions = {};
  std::array<float, 4> blends = {1.0f, 0.0f, 0.0f, 0.0f};
  std::array<std::array<float, 4>, 9> sh = {};
  std::size_t candidates = 0;
  bool imagesPending = false;
};

/**
 * R_FindClosestEnvironmentProbes (tr_frontend_main.cpp:487-668), once per view, every
 * frame (D79): the defaults first (bounds cleared, blends ( 1, 0, 0, 0 ), the default
 * cube everywhere, positions zero, SH zero); an early out at view area -1 (and without a
 * tree, D94: flowed false); the candidates every probe whose areaNum is -1 or connected
 * to the view (r_useLightAreaCulling 1, :524; an area past connectedAreas counts as not
 * connected, a guard), sorted by the squared distance of the view origin to the probe,
 * equal distances by the probe's id (K32: the engine's quicksort is not stable); the
 * nearest's bounds and depth cube (Ready); probePositions[i] = ( candidate i's origin,
 * its depth cube Ready ) below the count, else ( the nearest's origin, 0 ) (:557-583);
 * the weights max( 0, 1 - d_i / dCut )² for i < 3 with d_i idVec3::Length of origin -
 * view (engineVec3Length) and dCut max( d_3, 1e-4 ) with more than three candidates,
 * else max( 1.5 · d_last, 1e-4 ), times 1 / their sum when it is above 1e-4, else
 * ( 1, 0, 0 ) (:585-621); sh[i] = Σ_p sh_p[i] · w_p in p order (:626-634); radiance[i]
 * candidate i's _spec when Ready (the engine's !IsDefaulted, every image being loaded
 * there); probeDepth[i - 1] candidate i's _depth when Ready (i = 1, 2); the bounds
 * cleared when every radiance cube or the depth cube is the default (:655-667). Pure,
 * given imageState; the render thread.
 */
ViewProbes chooseViewProbes(
  const ProbeTable* probes,
  const EngineVec3& viewOrigin,
  int viewArea,
  const std::vector<bool>& connectedAreas,
  bool flowed,
  const ProbeImageStateOf& imageState);

} // namespace tb::lit
