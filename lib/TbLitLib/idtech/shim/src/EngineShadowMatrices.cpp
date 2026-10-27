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
// This file compiles, unaltered, ranges of the Doom 3 BFG Edition GPL Source Code and
// RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp, renderer/tr_frontend_main.cpp and
// renderer/GLMatrix.cpp, copied verbatim into the partial files of those names in
// lib/TbLitLib/idtech/engine/renderer), and declares, altered for SinkBroom, the fields
// of renderer/RenderCommon.h they read:
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014 Carl Kenner
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2013-2024 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP3.1 (C58, D143, SC710-SC712): the facade of lit/EngineShadowMatrices.h. The three
// partial files are compiled inside the namespace tbLitShadowMatrices, which declares,
// with the engine's names, what the ranges read and nothing else, so that no copied line
// changes (C7) and unqualified lookup finds these stand-ins before the shim's own
// viewDef_t and tr (RenderCommon.h): renderView_t, viewEntity_t (the world's
// modelViewMatrix), viewDef_t and viewLight_t reduced to their read fields,
// idScreenRect's four bounds, the frustum enumeration and frustum_t
// (RenderCommon.h:519-542), a reduced idRenderBackend whose one member is viewDef, the
// global tr with its viewDef (which R_SetupSplitFrustums writes through: P3.29), the six
// cvars at their engine defaults, and R_MatrixMultiply as the facade's own transcription
// (engineGlMatrixMultiply, GLMatrix.cpp:77-150), not a second one. The stand-ins are
// about 90 lines (D143: under 150, so the ranges are copied, not transcribed).
//
// Threads: the ranges touch no state but their arguments and tr, which
// engineSetupSplitFrustums sets for its call and clears after it (one caller at a time:
// the render thread's pass 6, or a test with no session running); tr is a
// zero-initialised global (mirror.json M-SHADOW-STATIC-02).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineShadowMatrices.h"
// clang-format on

#include <cstring>

namespace tbLitShadowMatrices
{

// RenderCommon.h:519-542: the frustum planes and the subfrustums of cascaded shadow
// mapping
enum frustumPlanes_t
{
  FRUSTUM_PLANE_LEFT,
  FRUSTUM_PLANE_RIGHT,
  FRUSTUM_PLANE_BOTTOM,
  FRUSTUM_PLANE_TOP,
  FRUSTUM_PLANE_NEAR,
  FRUSTUM_PLANE_FAR,
  FRUSTUM_PLANES = 6,
  FRUSTUM_CLIPALL = 1 | 2 | 4 | 8 | 16 | 32
};

enum
{
  FRUSTUM_PRIMARY,
  FRUSTUM_CASCADE1,
  FRUSTUM_CASCADE2,
  FRUSTUM_CASCADE3,
  FRUSTUM_CASCADE4,
  FRUSTUM_CASCADE5,
  MAX_FRUSTUMS,
};

typedef idPlane frustum_t[FRUSTUM_PLANES];

// renderer/ScreenRect.h's idScreenRect: the bounds R_SetupProjectionMatrix2 reads
struct idScreenRect
{
  short x1 = 0;
  short y1 = 0;
  short x2 = 0;
  short y2 = 0;
};

// renderer/RenderWorld.h's renderView_t: the fields the ranges read
struct renderView_t
{
  float fov_x = 0.0f;
  float fov_y = 0.0f;
  idVec3 vieworg;
  idMat3 viewaxis;
  bool cramZNear = false;
  bool flipProjection = false;
  float stereoScreenSeparation = 0.0f;
};

// viewEntity_t (RenderCommon.h:405-435): the world space's modelViewMatrix
struct viewEntity_t
{
  float modelViewMatrix[16] = {};
};

// viewDef_t (RenderCommon.h:546-616): the fields the ranges read
struct viewDef_t
{
  renderView_t renderView;
  float projectionMatrix[16] = {};
  viewEntity_t worldSpace;
  idScreenRect viewport;
  frustum_t frustums[MAX_FRUSTUMS];
  float frustumSplitDistances[MAX_FRUSTUMS] = {};
  idRenderMatrix frustumMVPs[MAX_FRUSTUMS];
};

// viewLight_t (RenderCommon.h:336-397): the fields SetupShadowMapMatrices reads and
// writes
struct viewLight_t
{
  idVec3 globalLightOrigin;
  idRenderMatrix baseLightProject;
  bool pointLight = false;
  bool parallel = false;
  idVec3 lightCenter;
  idRenderMatrix shadowV[6];
  idRenderMatrix shadowP[6];
  idRenderMatrix inverseBaseLightProject;
};

// idRenderBackend (RenderCommon.h), reduced to the member SetupShadowMapMatrices reads
class idRenderBackend
{
public:
  void SetupShadowMapMatrices(
    viewLight_t* vLight,
    int side,
    idRenderMatrix& lightProjectionRenderMatrix,
    idRenderMatrix& lightViewRenderMatrix);

  const viewDef_t* viewDef = nullptr;
};

// idRenderSystemLocal tr (RenderCommon.h), reduced to the view R_SetupSplitFrustums
// writes through; set and cleared around each call (engineSetupSplitFrustums)
struct tbLitRenderSystem
{
  viewDef_t* viewDef = nullptr;
};
static tbLitRenderSystem tr;

// the cvars the ranges read, at their engine defaults (RenderSystem_init.cpp:118, :253,
// :260, :261; GLMatrix.cpp:410-411); constexpr, so nothing is registered or allocated
constexpr idCVar r_znear("r_znear", "3", CVAR_RENDERER | CVAR_FLOAT, "");
constexpr idCVar r_shadowMapFrustumFOV(
  "r_shadowMapFrustumFOV", "92", CVAR_RENDERER | CVAR_FLOAT, "");
constexpr idCVar r_shadowMapSplits(
  "r_shadowMapSplits", "3", CVAR_RENDERER | CVAR_INTEGER, "", 0, 4);
constexpr idCVar r_shadowMapSplitWeight(
  "r_shadowMapSplitWeight", "0.9", CVAR_RENDERER | CVAR_FLOAT, "");
constexpr idCVar r_centerX("r_centerX", "0", CVAR_FLOAT, "");
constexpr idCVar r_centerY("r_centerY", "0", CVAR_FLOAT, "");

// R_MatrixMultiply as the copied range calls it: the facade's transcription
// (engineGlMatrixMultiply, GLMatrix.cpp:77-150, its SSE path), not a second one
void R_MatrixMultiply(const float a[16], const float b[16], float out[16])
{
  auto left = tb::lit::EngineGlMatrix{};
  auto right = tb::lit::EngineGlMatrix{};
  std::memcpy(left.data(), a, sizeof(float) * 16);
  std::memcpy(right.data(), b, sizeof(float) * 16);
  const auto product = tb::lit::engineGlMatrixMultiply(left, right);
  std::memcpy(out, product.data(), sizeof(float) * 16);
}

// the copied ranges, in the order they call each other
#include "renderer/GLMatrix.cpp"
#include "renderer/RenderBackend.cpp"
#include "renderer/tr_frontend_main.cpp"

} // namespace tbLitShadowMatrices

namespace tb::lit
{
namespace
{

idVec3 toId(const EngineVec3& v)
{
  return idVec3{v[0], v[1], v[2]};
}

idRenderMatrix toId(const EngineMat4& m)
{
  auto out = idRenderMatrix{};
  std::memcpy(&out, m.data(), sizeof(out));
  return out;
}

EngineMat4 fromId(const idRenderMatrix& m)
{
  auto out = EngineMat4{};
  std::memcpy(out.data(), &m, sizeof(float) * out.size());
  return out;
}

static_assert(sizeof(idRenderMatrix) == sizeof(EngineMat4));
static_assert(sizeof(idMat3) == sizeof(EngineMat3));
static_assert(tbLitShadowMatrices::MAX_FRUSTUMS == int(EngineMaxFrustums));

tbLitShadowMatrices::viewDef_t viewDefOf(const EngineShadowView& view)
{
  auto viewDef = tbLitShadowMatrices::viewDef_t{};
  viewDef.renderView.fov_x = view.fovX;
  viewDef.renderView.fov_y = view.fovY;
  viewDef.renderView.vieworg = toId(view.vieworg);
  std::memcpy(&viewDef.renderView.viewaxis, view.viewaxis.data(), sizeof(idMat3));
  // the viewport ( 0, 0, width - 1, height - 1 )
  viewDef.viewport.x1 = 0;
  viewDef.viewport.y1 = 0;
  viewDef.viewport.x2 = short(view.width - 1);
  viewDef.viewport.y2 = short(view.height - 1);
  std::memcpy(
    viewDef.worldSpace.modelViewMatrix,
    view.worldModelViewMatrix.data(),
    sizeof(viewDef.worldSpace.modelViewMatrix));
  std::memcpy(
    viewDef.projectionMatrix,
    view.projectionMatrix.data(),
    sizeof(viewDef.projectionMatrix));
  return viewDef;
}

} // namespace

EngineSplitFrustums engineSetupSplitFrustums(const EngineShadowView& view)
{
  auto viewDef = viewDefOf(view);
  // R_RenderView sets tr.viewDef to the view it then hands R_SetupSplitFrustums
  // (tr_frontend_main.cpp:886, 924)
  tbLitShadowMatrices::tr.viewDef = &viewDef;
  tbLitShadowMatrices::R_SetupSplitFrustums(&viewDef);
  tbLitShadowMatrices::tr.viewDef = nullptr;

  auto splits = EngineSplitFrustums{};
  for (std::size_t i = 0; i < EngineMaxFrustums; ++i)
  {
    splits.splitDistances[i] = viewDef.frustumSplitDistances[i];
    splits.frustumMVPs[i] = fromId(viewDef.frustumMVPs[i]);
  }
  return splits;
}

EngineShadowSideMatrices engineShadowMapMatrices(
  const EngineShadowLight& light,
  const int side,
  const EngineShadowView& view,
  const EngineSplitFrustums& splits)
{
  auto viewDef = viewDefOf(view);
  for (std::size_t i = 0; i < EngineMaxFrustums; ++i)
  {
    viewDef.frustumSplitDistances[i] = splits.splitDistances[i];
    viewDef.frustumMVPs[i] = toId(splits.frustumMVPs[i]);
  }
  auto vLight = tbLitShadowMatrices::viewLight_t{};
  vLight.globalLightOrigin = toId(light.globalLightOrigin);
  vLight.baseLightProject = toId(light.baseLightProject);
  vLight.inverseBaseLightProject = toId(light.inverseBaseLightProject);
  vLight.pointLight = light.pointLight;
  vLight.parallel = light.parallel;
  vLight.lightCenter = toId(light.lightCenter);

  auto backend = tbLitShadowMatrices::idRenderBackend{};
  backend.viewDef = &viewDef;
  auto lightProjection = idRenderMatrix{};
  auto lightView = idRenderMatrix{};
  backend.SetupShadowMapMatrices(&vLight, side, lightProjection, lightView);

  const auto slice = std::size_t(side < 0 ? 0 : side);
  auto out = EngineShadowSideMatrices{};
  out.lightProjection = fromId(lightProjection);
  out.lightView = fromId(lightView);
  out.shadowV = fromId(vLight.shadowV[slice]);
  out.shadowP = fromId(vLight.shadowP[slice]);
  return out;
}

EngineMat4 engineWindowSpaceToClipSpace()
{
  return fromId(renderMatrix_windowSpaceToClipSpace);
}

EngineMat4 engineClipSpaceToWindowSpace()
{
  return fromId(renderMatrix_clipSpaceToWindowSpace);
}

EngineShadowMatrixCvars engineShadowMatrixCvars()
{
  auto cvars = EngineShadowMatrixCvars{};
  cvars.znear = tbLitShadowMatrices::r_znear.GetFloat();
  cvars.shadowMapFrustumFov = tbLitShadowMatrices::r_shadowMapFrustumFOV.GetFloat();
  cvars.shadowMapSplits = tbLitShadowMatrices::r_shadowMapSplits.GetInteger();
  cvars.shadowMapSplitWeight = tbLitShadowMatrices::r_shadowMapSplitWeight.GetFloat();
  cvars.centerX = tbLitShadowMatrices::r_centerX.GetFloat();
  cvars.centerY = tbLitShadowMatrices::r_centerY.GetFloat();
  return cvars;
}

} // namespace tb::lit
