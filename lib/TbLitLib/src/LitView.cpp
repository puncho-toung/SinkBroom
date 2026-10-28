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

#include "LitView.h"

#include "lit/Projection.h"

#include <algorithm>
#include <cstdint>

namespace tb::lit
{

LitView litViewOf(
  const CameraState& camera, const std::uint32_t width, const std::uint32_t height)
{
  auto sized = camera;
  sized.viewportWidth = std::max(width, 1u);
  sized.viewportHeight = std::max(height, 1u);
  const auto projection = viewProjectionOf(sized);

  auto view = LitView{};
  view.width = int(sized.viewportWidth);
  view.height = int(sized.viewportHeight);
  view.viewOrigin = projection.viewOrigin;
  view.viewAxis = projection.viewAxis;
  view.fovX = projection.fov.x;
  view.fovY = projection.fov.y;
  view.projectionMatrix = projection.projectionMatrix;
  view.worldModelViewMatrix = projection.modelViewMatrix;
  // tr_frontend_main.cpp:904-907
  view.projectionRenderMatrix = engineMatrixTranspose(view.projectionMatrix);
  const auto viewRenderMatrix = engineMatrixTranspose(view.worldModelViewMatrix);
  view.worldMvp = engineMatrixMultiply(view.projectionRenderMatrix, viewRenderMatrix);
  return view;
}

LitSpace litSpaceOf(
  const LitView& view,
  const PreviewId entity,
  const EngineVec3& origin,
  const EngineMat3& axis,
  const ShaderParms& shaderParms)
{
  auto space = litModelSpaceOf(entity, origin, axis, shaderParms);
  // tr_frontend_addmodels.cpp:502-507
  space.modelViewMatrix =
    engineGlMatrixMultiply(space.modelMatrix, view.worldModelViewMatrix);
  space.mvp = engineMatrixMultiply(
    view.projectionRenderMatrix, engineMatrixTranspose(space.modelViewMatrix));
  return space;
}

LitSpace litModelSpaceOf(
  const PreviewId entity,
  const EngineVec3& origin,
  const EngineMat3& axis,
  const ShaderParms& shaderParms)
{
  auto space = LitSpace{};
  space.entity = entity;
  // RenderWorld_defs.cpp:51-53
  space.modelMatrix = engineModelMatrix(origin, axis);
  space.modelRenderMatrix = engineMatrixFromOriginAxis(origin, axis);
  space.shaderParms = shaderParms;
  return space;
}

ShaderParms engineWorldShaderParms()
{
  auto parms = ShaderParms{};
  parms[0] = 1.0f;
  parms[1] = 1.0f;
  parms[2] = 1.0f;
  parms[3] = 1.0f;
  return parms;
}

void ScreenRect::clear()
{
  x1 = y1 = 32000;
  x2 = y2 = -32000;
  zmin = 0.0f;
  zmax = 1.0f;
}

void ScreenRect::addPoint(const float x, const float y)
{
  const int ix = engineFtoi(x);
  const int iy = engineFtoi(y);

  if (ix < x1)
  {
    x1 = short(ix);
  }
  if (ix > x2)
  {
    x2 = short(ix);
  }
  if (iy < y1)
  {
    y1 = short(iy);
  }
  if (iy > y2)
  {
    y2 = short(iy);
  }
}

bool ScreenRect::isEmpty() const
{
  return x1 > x2 || y1 > y2;
}

int ScreenRect::area() const
{
  // the shorts promoted to int; the product wraps modulo 2^32 as MSVC's imul does (an
  // emptied rect, (32000, 32000, -32000, -32000), has 63999 * 63999 = 4,095,872,001,
  // stored as -199,095,295: LP3.0, D148), written without signed overflow
  const auto width = std::uint32_t(int(x2) - int(x1) + 1);
  const auto height = std::uint32_t(int(y2) - int(y1) + 1);
  return int(width * height);
}

void ScreenRect::expand()
{
  x1--;
  y1--;
  x2++;
  y2++;
}

void ScreenRect::intersect(const ScreenRect& rect)
{
  if (rect.x1 > x1)
  {
    x1 = rect.x1;
  }
  if (rect.x2 < x2)
  {
    x2 = rect.x2;
  }
  if (rect.y1 > y1)
  {
    y1 = rect.y1;
  }
  if (rect.y2 < y2)
  {
    y2 = rect.y2;
  }
}

void ScreenRect::unite(const ScreenRect& rect)
{
  if (rect.x1 < x1)
  {
    x1 = rect.x1;
  }
  if (rect.x2 > x2)
  {
    x2 = rect.x2;
  }
  if (rect.y1 < y1)
  {
    y1 = rect.y1;
  }
  if (rect.y2 > y2)
  {
    y2 = rect.y2;
  }
}

bool ScreenRect::equals(const ScreenRect& rect) const
{
  return x1 == rect.x1 && x2 == rect.x2 && y1 == rect.y1 && y2 == rect.y2;
}

ScreenRect viewScissor(const LitView& view)
{
  auto rect = ScreenRect{};
  rect.x1 = 0;
  rect.y1 = 0;
  rect.x2 = short(view.width - 1);
  rect.y2 = short(view.height - 1);
  return rect;
}

std::array<EnginePlane, 6> engineViewFrustumPlanes(const LitView& view)
{
  // tr_frontend_main.cpp:913
  auto planes = engineFrustumPlanes(view.worldMvp, false, true);

  // the DOOM 3 frustum planes point outside the frustum
  for (auto& plane : planes)
  {
    plane = EnginePlane{-plane.a, -plane.b, -plane.c, -plane.d};
  }
  // remove the Z-near to avoid portals from being near clipped
  planes[4].d -= EngineFrame.znear;
  return planes;
}

EngineVec3 engineGlobalToNdc(const LitView& view, const EngineVec3& global)
{
  return engineGlobalToNormalizedDeviceCoordinates(
    view.worldModelViewMatrix, view.projectionMatrix, global);
}

nvrhi::Rect engineScissorRect(const LitView& view, const ScreenRect& rect)
{
  if (rect.isEmpty())
  {
    // a guard: the engine never sets an empty one (a light's is tested before), and a
    // cleared context's scissor is the whole viewport (M-PASS-GUARD-02)
    return nvrhi::Rect{engineViewport(view)};
  }
  // the viewport is (0, 0, width - 1, height - 1)
  const auto viewportX1 = 0;
  const auto viewportY2 = view.height - 1;
  // GL_Scissor( x, y, w, h ): Clear, then AddPoint( x, y ) and AddPoint( x + w, y + h )
  const auto x = viewportX1 + rect.x1;
  const auto y = viewportY2 - rect.y2;
  const auto w = rect.x2 + 1 - rect.x1;
  const auto h = rect.y2 + 1 - rect.y1;
  auto stored = ScreenRect{};
  stored.clear();
  stored.x1 = short(std::min(x, x + w));
  stored.x2 = short(std::max(x, x + w));
  stored.y1 = short(std::min(y, y + h));
  stored.y2 = short(std::max(y, y + h));
  if (stored.isEmpty())
  {
    return nvrhi::Rect{engineViewport(view)};
  }
  return nvrhi::Rect{stored.x1, stored.x2, stored.y1, stored.y2};
}

nvrhi::Viewport engineViewport(const LitView& view)
{
  // GL_Viewport( 0, 0, w, h ) stores [0, w] × [0, h]
  return nvrhi::Viewport{0.0f, float(view.width), 0.0f, float(view.height), 0.0f, 1.0f};
}

void setViewParms(RenderParms& parms, const LitView& view)
{
  // GLOBALEYEPOS
  parms.set(
    RenderParm::GlobalEyePos,
    RenderParms::Vec4{view.viewOrigin[0], view.viewOrigin[1], view.viewOrigin[2], 1.0f});

  // OVERBRIGHT
  const auto overbright = EngineFrame.lightScale * 0.5f;
  parms.set(
    RenderParm::Overbright,
    RenderParms::Vec4{overbright, overbright, overbright, overbright});

  // PROJMATRIX_X..W: SetVertexParms( RENDERPARM_PROJMATRIX_X,
  // viewDef->projectionRenderMatrix[0], 4 )
  parms.set(RenderParm::ProjMatrixX, view.projectionRenderMatrix);

  // PSX_DISTORTIONS: r_renderMode is not RENDERMODE_PSX
  parms.set(RenderParm::PsxDistortions, RenderParms::Vec4{0.0f, 0.0f, 0.0f, 0.0f});

  // WINDOWCOORD
  const auto w = view.width;
  const auto h = view.height;
  parms.set(
    RenderParm::WindowCoord,
    RenderParms::Vec4{1.0f / float(w), 1.0f / float(h), float(w), float(h)});
}

void setMvp(RenderParms& parms, const EngineMat4& mvp)
{
  parms.set(RenderParm::MvpMatrixX, mvp);
}

} // namespace tb::lit
