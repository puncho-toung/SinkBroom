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
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 GPL Source
// Code and RBDOOM-3-BFG (neo/tools/compilers/dmap/dress_decal.cpp):
//   Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The dress decals' facade (lit/EngineDecal.h): dress_decal.cpp's vector arithmetic,
// transcribed on the copied idVec3, idMat3 and idFixedWinding, and the copied
// idDecalProjection (idlib/geometry/DecalProjection.cpp, its own translation unit).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineDecal.h"
// clang-format on

namespace tb::lit
{
namespace
{

static_assert(sizeof(idPlane) == sizeof(EnginePlane));

idVec3 toId(const EngineVec3& v)
{
  return idVec3{v[0], v[1], v[2]};
}

EngineVec3 fromId(const idVec3& v)
{
  return {v.x, v.y, v.z};
}

idMat3 toId(const EngineMat3& m)
{
  return idMat3{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
}

EngineMat3 fromId(const idMat3& m)
{
  return {
    m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
}

idPlane toId(const EnginePlane& plane)
{
  return idPlane{plane.a, plane.b, plane.c, plane.d};
}

EnginePlane fromId(const idPlane& plane)
{
  return {plane[0], plane[1], plane[2], plane[3]};
}

/**
 * The material CreateParms carries (DecalProjection.cpp:51-54: asserted non-null, stored,
 * never read): the address of this constant byte, never dereferenced as an
 * idMaterial (D32: the table holds no id pointer; mirror.json M-DRESS-PURE-01)
 */
// (C++17 here: constant-initialised by its literal, no constinit)
constexpr unsigned char s_materialStandIn = 0;

const idMaterial* materialStandIn()
{
  return reinterpret_cast<const idMaterial*>(&s_materialStandIn);
}

} // namespace

std::optional<EngineMat3> engineDecalTargetAxis(
  const EngineVec3& origin, const EngineVec3& target)
{
  // dress_decal.cpp:211-225
  idVec3 dir = toId(target) - toId(origin);
  if (dir.Normalize() == 0.0f)
  {
    return std::nullopt;
  }
  // aimed at the target, roll 0
  idMat3 axis;
  idVec3 left, down;
  axis[0] = -dir;
  axis[0].NormalVectors(left, down);
  axis[1] = left;
  axis[2] = -down;
  return fromId(axis);
}

std::optional<EngineDecalParms> engineDecalProjection(
  const EngineVec3& origin,
  const EngineMat3& axisRows,
  const EngineVec2& sizeXY,
  const float halfDepth,
  const float fade,
  const bool parallel)
{
  const auto originVec = toId(origin);
  const auto axis = toId(axisRows);
  const auto size = idVec2{sizeXY[0], sizeXY[1]};

  // dress_decal.cpp:249-259
  const float capScale = parallel ? 0.5f : 1.0f;
  const idVec3 farCenter = originVec - halfDepth * axis[0];
  const idVec3 right = (size.x * capScale) * axis[1];
  const idVec3 up = (size.y * capScale) * axis[2];
  const idVec3 projectionOrigin = originVec + halfDepth * axis[0];

  idFixedWinding winding;
  winding += idVec5(farCenter - right + up, idVec2(0.0f, 0.0f));
  winding += idVec5(farCenter + right + up, idVec2(1.0f, 0.0f));
  winding += idVec5(farCenter + right - up, idVec2(1.0f, 1.0f));
  winding += idVec5(farCenter - right - up, idVec2(0.0f, 1.0f));

  // :265
  decalProjectionParms_t parms;
  if (!idDecalProjection::CreateParms(
        parms, winding, projectionOrigin, parallel, fade, materialStandIn(), 0))
  {
    return std::nullopt;
  }

  auto result = EngineDecalParms{};
  for (auto i = std::size_t{0}; i < EngineDecalBoundingPlanes; ++i)
  {
    result.boundingPlanes[i] = fromId(parms.boundingPlanes[i]);
  }
  for (auto i = std::size_t{0}; i < 2; ++i)
  {
    result.fadePlanes[i] = fromId(parms.fadePlanes[i]);
    result.textureAxis[i] = fromId(parms.textureAxis[i]);
  }
  result.projectionOrigin = fromId(parms.projectionOrigin);
  result.projectionBounds = {
    fromId(parms.projectionBounds[0]), fromId(parms.projectionBounds[1])};
  result.fadeDepth = parms.fadeDepth;
  result.parallel = parms.parallel;
  return result;
}

std::uint32_t engineDecalFadeColor(const EngineDecalParms& parms, const EngineVec3& xyz)
{
  // dress_decal.cpp:465-467
  const float depthFade = idDecalProjection::ClampDepthFade(
    idDecalProjection::DepthFade(toId(parms.fadePlanes[0]), parms.fadeDepth, toId(xyz)));
  // rounded, so a vertex on the knee is exactly opaque (PackColorRound, LP7.0's
  // transcription of Lib.cpp:175-182)
  return enginePackColorRound({1.0f, 1.0f, 1.0f, 1.0f - depthFade});
}

} // namespace tb::lit
