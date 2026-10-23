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
// renderer/NVRHI/RenderBackend_NVRHI.cpp, renderer/ScreenRect.cpp,
// renderer/tr_frontend_main.cpp and renderer/tr_frontend_addmodels.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "RenderParms.h"
#include "lit/EngineLight.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/Snapshot.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace tb::lit
{

/**
 * LP2.2a (SC207): what of viewDef_t the frame's passes read. The viewport and the view's
 * scissor are the whole target, (0, 0, width - 1, height - 1); the matrices are the
 * engine's: R_SetupProjectionMatrix and R_SetupViewMatrix as float[16]s
 * (lit/Projection.h), projectionRenderMatrix their transpose and the world's mvp
 * projectionRenderMatrix · Transpose( modelViewMatrix ) through the copied idRenderMatrix
 * (tr_frontend_main.cpp:904-907), so that the float order is the engine's.
 */
struct LitView
{
  int width = 1;
  int height = 1;
  // renderView.vieworg
  EngineVec3 viewOrigin = {0, 0, 0};
  EngineGlMatrix projectionMatrix = {};
  EngineGlMatrix worldModelViewMatrix = {};
  EngineMat4 projectionRenderMatrix = {};
  EngineMat4 worldMvp = {};
};

/**
 * The view of the camera drawn at width × height: the camera's viewport is the target's
 * (CalcFov's aspect, R_SetupProjectionMatrix's size). Pure.
 */
LitView litViewOf(const CameraState& camera, std::uint32_t width, std::uint32_t height);

/**
 * LP2.2a (SC207): a viewEntity_t's matrices for an entity of origin and axis
 * (tr_frontend_addmodels.cpp:502-507): modelMatrix R_AxisToModelMatrix, modelViewMatrix
 * R_MatrixMultiply( modelMatrix, the world's modelViewMatrix ), mvp
 * projectionRenderMatrix · Transpose( modelViewMatrix ); modelRenderMatrix is
 * CreateFromOriginAxis (RenderWorld_defs.cpp:53), which the light cull takes; and the
 * entity's shaderParms.
 */
struct LitSpace
{
  PreviewId entity = NoPreviewId;
  EngineGlMatrix modelMatrix = {};
  EngineMat4 modelRenderMatrix = {};
  EngineGlMatrix modelViewMatrix = {};
  EngineMat4 mvp = {};
  ShaderParms shaderParms = {};
};

// Pure.
LitSpace litSpaceOf(
  const LitView& view,
  PreviewId entity,
  const EngineVec3& origin,
  const EngineMat3& axis,
  const ShaderParms& shaderParms);

/**
 * The world's render entity parms (RenderWorld_load.cpp:1083-1086 AddWorldModelEntities):
 * the identity axis at the origin and shaderParms 0 to 3 at 1, the rest 0. Pure.
 */
ShaderParms engineWorldShaderParms();

/**
 * idScreenRect (ScreenRect.h:42-83, ScreenRect.cpp:67-184): inclusive pixel bounds in
 * GL's unflipped convention (y up, row 0 the bottom of the target), as shorts, with the
 * depth bounds the engine computes and no NVRHI test uses (D67, K22). It is the space of
 * the view's scissor, of engineLightScissor's rect before engineScissorRect's flip, and
 * of the portal flow's rects (lit/ViewFlow.h, C32.4). Pure; any thread.
 */
struct ScreenRect
{
  short x1 = 32000;
  short y1 = 32000;
  short x2 = -32000;
  short y2 = -32000;
  float zmin = 0.0f;
  float zmax = 1.0f;

  // Clear: backwards values (32000, 32000, -32000, -32000), zmin 0, zmax 1
  void clear();
  /**
   * AddPoint (ScreenRect.cpp:81-104, LP4.0, P4.16): x and y through idMath::Ftoi
   * (_mm_cvttss_si32: truncation toward zero, Math.h:1331-1338; INT_MIN for a value
   * beyond an int) and compared as ints; a new bound is stored into the short field as
   * the engine's int-to-short assignment stores it (modulo 2^16).
   */
  void addPoint(float x, float y);
  // IsEmpty: x1 > x2 || y1 > y2
  bool isEmpty() const;
  // GetArea: ( x2 - x1 + 1 ) · ( y2 - y1 + 1 )
  int area() const;
  // Expand: one pixel each way
  void expand();
  void intersect(const ScreenRect& rect);
  void unite(const ScreenRect& rect);
  bool equals(const ScreenRect& rect) const;

  bool operator==(const ScreenRect&) const = default;
};

// the view's scissor, (0, 0, width - 1, height - 1) (viewDef->scissor)
ScreenRect viewScissor(const LitView& view);

/**
 * GL_Scissor of a rect in GL's convention, with the callers' Y flip (SC211:
 * RenderBackend.cpp:684-687 for the view, :1715-1718 for a light): x = viewport.x1 +
 * x1, y = viewport.y2 - y2, w = x2 + 1 - x1, h = y2 + 1 - y1, stored as [x, x + w] × [y,
 * y + h] (RenderBackend_NVRHI.cpp:2176-2181) and applied as nvrhi::Rect( x1, x2, y1, y2
 * ), or the whole viewport when the stored rect is empty (:528-535); an empty rect, which
 * the engine never passes, is the whole viewport too. Pure.
 */
nvrhi::Rect engineScissorRect(const LitView& view, const ScreenRect& rect);

// the view's viewport as GL_Viewport stores it and NVRHI takes it (:2188-2193, :520-525)
nvrhi::Viewport engineViewport(const LitView& view);

/**
 * DrawViewInternal's per-view parms (RenderBackend.cpp:5934-5990, SC201), after
 * ZeroUniforms: GLOBALEYEPOS (vieworg, 1), OVERBRIGHT r_lightScale · 0.5 in all four,
 * PROJMATRIX_X..W the rows of projectionRenderMatrix, PSX_DISTORTIONS 0 (r_renderMode 0),
 * WINDOWCOORD (1 / w, 1 / h, w, h).
 */
void setViewParms(RenderParms& parms, const LitView& view);

// RB_SetMVP (RenderBackend.cpp:102-105): MVPMATRIX_X..W the rows of mvp
void setMvp(RenderParms& parms, const EngineMat4& mvp);

} // namespace tb::lit
