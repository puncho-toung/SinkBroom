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

// LP7.3 (C54, SC628-SC633): the facade over the dress decals' projection: the copied
// idDecalProjection (idlib/geometry/DecalProjection.{h,cpp}, whole files, C7) and the
// lines of dmap's ParseDecalProjector and DressDecalsPostTjunction that do vector
// arithmetic (dress_decal.cpp:211-225, 249-265, 465-467), transcribed in the engine's
// float arithmetic (idVec3, idMat3, idFixedWinding). Standard types only; compiles as
// C++17 and C++20.
//
// Threading (C24): every function here is pure and may run on any thread; the render
// thread's scan runs them (SceneGeometry's dress pass). The copied CreateParms reads its
// arguments only, allocates nothing (an idFixedWinding on the stack), calls no common and
// holds no FatalError site (mirror.json M-DRESS-PURE-01).

#include "lit/EngineGeometry.h"

#include <array>
#include <cstdint>
#include <optional>

namespace tb::lit
{

// DecalProjection.h:50 NUM_DECAL_BOUNDING_PLANES
inline constexpr auto EngineDecalBoundingPlanes = std::size_t{6};

/**
 * decalProjectionParms_t (DecalProjection.h:52-64) as plain data, without its material
 * (only carried by the engine: the preview keeps the projector's material by name),
 * startTime (0 for dmap's) and force (false): the six bounding planes (positive sides
 * outside the volume), the two fade planes, the texture axes, the projection origin and
 * bounds, the fade depth and whether the projection is parallel.
 */
struct EngineDecalParms
{
  std::array<EnginePlane, EngineDecalBoundingPlanes> boundingPlanes = {};
  std::array<EnginePlane, 2> fadePlanes = {};
  std::array<EnginePlane, 2> textureAxis = {};
  EngineVec3 projectionOrigin = {0, 0, 0};
  std::array<EngineVec3, 2> projectionBounds = {};
  float fadeDepth = 0;
  bool parallel = false;
};

/**
 * dress_decal.cpp:211-225, a projector aimed at its target: dir = target - origin,
 * dir.Normalize() (idVec3::Normalize: InvSqrt of the squared length, Vector.h:795-805),
 * axis[0] = -dir, axis[0].NormalVectors( left, down ) (:892-915), axis[1] = left,
 * axis[2] = -down (roll 0). nullopt when Normalize returns 0, where the engine warns and
 * uses the angles: a target at the origin (InvSqrt( 0 ) is INFINITUM, 1e30, so the
 * length returned is 1e30 times 0, Math.h:527-532, Math.cpp:50). Pure.
 */
std::optional<EngineMat3> engineDecalTargetAxis(
  const EngineVec3& origin, const EngineVec3& target);

/**
 * dress_decal.cpp:249-265: the far cap winding depth / 2 behind the origin (capScale 0.5
 * when parallel, else 1; farCenter = origin - halfDepth axis[0], right = ( size.x
 * capScale ) axis[1], up = ( size.y capScale ) axis[2]; its four points with st ( 0 0 )
 * ( 1 0 ) ( 1 1 ) ( 0 1 )) and the projection origin origin + halfDepth axis[0], through
 * the copied idDecalProjection::CreateParms( parms, winding, projectionOrigin, parallel,
 * fade, material, 0 ) (DecalProjection.cpp:43-138). The material argument, which
 * CreateParms only carries (and asserts non-null, :51), is a constant stand-in of the
 * facade's (the table holds no id pointer, D32). nullopt when CreateParms returns false
 * (it cannot with four points). axis is an idMat3, row by row. Pure.
 */
std::optional<EngineDecalParms> engineDecalProjection(
  const EngineVec3& origin,
  const EngineMat3& axis,
  const EngineVec2& size,
  float halfDepth,
  float fade,
  bool parallel);

/**
 * dress_decal.cpp:465-467, the colour DressDecalsPostTjunction gives a corner of a
 * projector with a fade: PackColorRound( ( 1, 1, 1, 1 - ClampDepthFade( DepthFade(
 * fadePlanes[0], fadeDepth, xyz ) ) ) ) (DecalProjection.h:98-121, Lib.cpp:175-182).
 * Pure.
 */
std::uint32_t engineDecalFadeColor(const EngineDecalParms& parms, const EngineVec3& xyz);

} // namespace tb::lit
