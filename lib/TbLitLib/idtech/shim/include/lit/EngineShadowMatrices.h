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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp,
// renderer/tr_frontend_main.cpp, renderer/GLMatrix.cpp and renderer/RenderCommon.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// LP3.1 (C58, D143, SC710-SC712): the facade over the shadow maps' matrices, three
// verbatim ranges compiled in shim/src/EngineShadowMatrices.cpp behind stand-ins of the
// viewDef_t and viewLight_t fields they read: idRenderBackend::SetupShadowMapMatrices
// with MatrixOrthogonalProjectionRH (renderer/RenderBackend.cpp:2748-2766, 2839-3164),
// R_SetupSplitFrustums (renderer/tr_frontend_main.cpp:397-456, which writes through a
// stand-in of the global tr's viewDef) and R_SetupProjectionMatrix2
// (renderer/GLMatrix.cpp:548-603). Standard types only; C++17 and C++20. Threads: one
// caller at a time (engineSetupSplitFrustums sets and clears the stand-in tr's viewDef;
// the render thread's pass 6, or a test with no session running); the rest is pure.

#include "lit/EngineLight.h"

#include <array>

namespace tb::lit
{

// FRUSTUM_PRIMARY .. FRUSTUM_CASCADE5, MAX_FRUSTUMS (RenderCommon.h:531-540)
inline constexpr auto EngineMaxFrustums = std::size_t{6};

/**
 * What of viewDef_t the copied ranges read (RenderCommon.h:546-616): renderView's
 * vieworg, viewaxis (rows forward, left, up), fov_x and fov_y (cramZNear,
 * stereoScreenSeparation and flipProjection the preview's false, 0 and false), the
 * viewport (0, 0, width - 1, height - 1) and the world's modelViewMatrix;
 * projectionMatrix is read only under flipProjection, never.
 */
struct EngineShadowView
{
  EngineVec3 vieworg = {0, 0, 0};
  EngineMat3 viewaxis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float fovX = 90.0f;
  float fovY = 90.0f;
  int width = 1;
  int height = 1;
  EngineGlMatrix worldModelViewMatrix = {};
  EngineGlMatrix projectionMatrix = {};
};

// R_SetupSplitFrustums' outputs: viewDef->frustumSplitDistances and frustumMVPs (1 to
// r_shadowMapSplits + 1 written; the distances past the splits idMath::INFINITUM)
struct EngineSplitFrustums
{
  std::array<float, EngineMaxFrustums> splitDistances = {};
  std::array<EngineMat4, EngineMaxFrustums> frustumMVPs = {};
};

/**
 * R_SetupSplitFrustums( viewDef ) (tr_frontend_main.cpp:397-456, called every view at
 * :924): zNearStart r_znear, zFarEnd 10000, lambda r_shadowMapSplitWeight, for i = 1 to
 * splits + 1 zNear = zFar - zFar · 0.005 past the first, zFar = 1.005 · lambda · zNear ·
 * ratio^( i / ( splits + 1 ) ) + ( 1 - lambda ) · ( zNearStart + ( zFarEnd - zNearStart )
 * · si ), frustumMVPs[i] = R_SetupProjectionMatrix2( zNear, zFar ) (GLMatrix.cpp:548-603:
 * jitter 0, r_centerX and r_centerY 0) transposed times the world's view matrix
 * transposed. The copied code; one caller at a time.
 */
EngineSplitFrustums engineSetupSplitFrustums(const EngineShadowView& view);

// the viewLight_t fields SetupShadowMapMatrices reads (RenderCommon.h:364-379)
struct EngineShadowLight
{
  EngineVec3 globalLightOrigin = {0, 0, 0};
  EngineMat4 baseLightProject = {};
  EngineMat4 inverseBaseLightProject = {};
  bool pointLight = false;
  bool parallel = false;
  EngineVec3 lightCenter = {0, 0, 0};
};

/**
 * One call of idRenderBackend::SetupShadowMapMatrices( vLight, side, ... )
 * (RenderBackend.cpp:2839-3164, SC710): the raster pair it returns (lightProjection,
 * lightView: what the tile's draws multiply, RB:3519-3555) and what it stores in
 * vLight->shadowV[ max( 0, side ) ] and shadowP[ max( 0, side ) ] (the consumer's, with
 * the NVRHI matClipToUvzw: C for parallel and point, C' · windowToClip for spot).
 */
struct EngineShadowSideMatrices
{
  EngineMat4 lightProjection = {};
  EngineMat4 lightView = {};
  EngineMat4 shadowV = {};
  EngineMat4 shadowP = {};
};

/**
 * SetupShadowMapMatrices for side (parallel 0 to splits, point 0 to 5, spot -1) of light
 * in view, whose split frustums (parallel lights only) are splits: parallel, a view at
 * vieworg along normalize( -lightCenter ) (0 0 -1 when zero) cropped to frustumMVPs[1 +
 * side]'s corners and the light volume's bounds; point, the six axis views about
 * globalLightOrigin with zNear 4 and r_shadowMapFrustumFOV on both axes; spot, the
 * identity and baseLightProject. The copied code; pure.
 */
EngineShadowSideMatrices engineShadowMapMatrices(
  const EngineShadowLight& light,
  int side,
  const EngineShadowView& view,
  const EngineSplitFrustums& splits);

// renderMatrix_windowSpaceToClipSpace and renderMatrix_clipSpaceToWindowSpace
// (RenderMatrix.cpp:86-99, the copy)
EngineMat4 engineWindowSpaceToClipSpace();
EngineMat4 engineClipSpaceToWindowSpace();

// the cvars the copied ranges read, at the defaults their stand-ins hold
// (RenderSystem_init.cpp:118, :253, :260, :261; GLMatrix.cpp:410-411), equal to
// EngineFrame's by a test
struct EngineShadowMatrixCvars
{
  float znear = 0.0f;
  float shadowMapFrustumFov = 0.0f;
  int shadowMapSplits = 0;
  float shadowMapSplitWeight = 0.0f;
  float centerX = 0.0f;
  float centerY = 0.0f;
};
EngineShadowMatrixCvars engineShadowMatrixCvars();

} // namespace tb::lit
