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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/GLMatrix.cpp and
// renderer/Interaction.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// DX.2 (31-LP2-FIXES.md): the static interactions' light triangles, the copied ranges of
// renderer/Interaction.cpp (R_CalcInteractionCullBits' planes and cull bits,
// R_CreateInteractionLightTris' triangle cull) with renderer/Interaction.h's
// LIGHT_CLIP_EPSILON and srfCullInfo_t, compiled here behind the facade
// lit/EngineLight.h (engineInteractionLightTris). The glue declares the variables the
// ranges name, with the engine's names, and stands for what the ranges leave out:
//
// - R_CalcInteractionCullBits' early returns (Interaction.cpp:97-100, 119-124) and its
//   allocation (:126-127): cullBits is a zeroed byte per vertex before the ranges run.
//   With every plane's front bit set, the second range sets no bit and the third keeps
//   every triangle, as the engine's LIGHT_CULL_ALL_FRONT path with includeBackFaces
//   references the surface's own indexes (:233-244);
// - R_CreateInteractionLightTris' facing cull (:208-228, 296-306): r_lightAllBackFaces 1
//   (the frame default, EngineFrame), so includeBackFaces is true and facing is never
//   read; the surface's new indexes (:286) are a vector of the surface's index count; the
//   bounds (:328) are not needed (the frontend culls with the surface's own).
//
// Pure: the ranges read only their arguments (and RenderMatrix.cpp's constant statics,
// mirror.json M-LIGHT-STATIC-*), call no common, FatalError or SIMDProcessor, allocate
// only the glue's vectors, and have no statics (C24, mirror.json M-LIGHT-POOL-02): the
// session's CPU pool runs them.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineLight.h"
// clang-format on

#include "shim/ShimInteraction.h"

#include <cstring>
#include <vector>

namespace tb::lit
{
namespace
{

// the fields of the engine's idRenderEntityLocal, idRenderLightLocal, srfTriangles_t and
// idDrawVert that the copied ranges name
struct CullEntity
{
  float modelMatrix[16];
};

struct CullLight
{
  idRenderMatrix baseLightProject;
};

struct CullVert
{
  idVec3 xyz;
};

struct CullSurface
{
  idBounds bounds;
  int numVerts = 0;
  const CullVert* verts = nullptr;
  int numIndexes = 0;
  const triIndex_t* indexes = nullptr;
};

struct CullLightTris
{
  triIndex_t* indexes = nullptr;
};

// R_GlobalPlaneToLocal as the copied range calls it: the facade's transcription
// (engineGlobalPlaneToLocal, GLMatrix.cpp:315-321; no skew or scale assumed, as there),
// not a second one
void R_GlobalPlaneToLocal(const float modelMatrix[16], const idPlane& in, idPlane& out)
{
  auto matrix = EngineGlMatrix{};
  std::memcpy(matrix.data(), modelMatrix, sizeof(float) * matrix.size());
  const auto local =
    engineGlobalPlaneToLocal(matrix, EnginePlane{in[0], in[1], in[2], in[3]});
  out[0] = local.a;
  out[1] = local.b;
  out[2] = local.c;
  out[3] = local.d;
}

std::vector<triIndex_t> interactionLightTris(
  const CullEntity* ent, const CullSurface* tri, const CullLight* light)
{
  // R_CreateInteractionLightTris' locals (Interaction.cpp:177-200), as the ranges name
  // them; includeBackFaces: r_lightAllBackFaces 1
  int i = 0;
  int numIndexes = 0;
  triIndex_t* indexes = nullptr;
  int c_backfaced = 0;
  int c_distance = 0;
  const bool includeBackFaces = true;
  int faceNum = 0;

  auto newIndexes = std::vector<triIndex_t>(std::size_t(tri->numIndexes));
  auto lightTris = CullLightTris{newIndexes.data()};
  const CullLightTris* newTri = &lightTris;

  // srfCullInfo_t cullInfo = {} with R_CalcInteractionCullBits' zeroed bits
  srfCullInfo_t cullInfo = {};
  auto cullBitsStorage = std::vector<byte>(std::size_t(tri->numVerts), 0);
  cullInfo.cullBits = cullBitsStorage.data();

#include "renderer/Interaction.cpp"

  static_cast<void>(c_backfaced);
  static_cast<void>(c_distance);
  static_cast<void>(includeBackFaces);
  newIndexes.resize(std::size_t(numIndexes));
  return newIndexes;
}

} // namespace

std::vector<std::uint16_t> engineInteractionLightTris(
  const EngineMat4& baseLightProject,
  const EngineGlMatrix& modelMatrix,
  const EngineBounds& localBounds,
  const std::vector<EngineVec3>& positions,
  const std::vector<std::uint16_t>& indexes)
{
  static_assert(sizeof(triIndex_t) == sizeof(std::uint16_t));
  auto ent = CullEntity{};
  std::memcpy(ent.modelMatrix, modelMatrix.data(), sizeof(ent.modelMatrix));
  auto light = CullLight{};
  std::memcpy(&light.baseLightProject, baseLightProject.data(), sizeof(idRenderMatrix));

  auto verts = std::vector<CullVert>{};
  verts.reserve(positions.size());
  for (const auto& p : positions)
  {
    verts.push_back(CullVert{idVec3{p[0], p[1], p[2]}});
  }
  auto surface = CullSurface{};
  surface.bounds = idBounds{
    idVec3{localBounds[0][0], localBounds[0][1], localBounds[0][2]},
    idVec3{localBounds[1][0], localBounds[1][1], localBounds[1][2]}};
  surface.numVerts = int(verts.size());
  surface.verts = verts.data();
  surface.numIndexes = int(indexes.size());
  surface.indexes = indexes.data();

  const auto kept = interactionLightTris(&ent, &surface, &light);
  return std::vector<std::uint16_t>{kept.begin(), kept.end()};
}

} // namespace tb::lit
