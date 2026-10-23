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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_portals.cpp,
// renderer/RenderWorld_load.cpp, renderer/tr_frontend_main.cpp,
// renderer/tr_frontend_addlights.cpp, renderer/tr_frontend_addmodels.cpp and
// renderer/RenderCommon.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "LitView.h"
#include "lit/AreaData.h"
#include "lit/EngineLight.h"
#include "lit/Snapshot.h"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

// LP4.0 (C32, C33, C36, D79, D81, D94): the view's portal flow and the area predicates
// the frontend wires in LP4.1. Every function here is pure over immutable inputs: it
// reads its arguments, allocates only its own results and scratch on the heap, and calls
// the copied pure functions of mirror.json M-FLOW-PURE-01; it runs on the render thread
// (LP4.1) or any thread.

namespace tb::lit
{
struct LightRecord;
struct ProcTree;

// MAX_PORTAL_PLANES (RenderWorld_portals.cpp:37-38): a stack's edge planes; the portal's
// own plane makes the 21st (portalStack_t::portalPlanes, :40-47)
constexpr auto MaxPortalPlanes = std::size_t{20};

// a light as the flow reads it, from its record (LightRecord::derived, lightCastsShadows)
struct ViewFlowLight
{
  PreviewId id = NoPreviewId;
  // derived.areaNum: PointInArea of its origin, -1 outside
  int areaNum = -1;
  // derived.areas, in push order (the engine's chain holds them newest first; the flow
  // reads only which areas they are)
  std::span<const int> areas;
  // derived.projection.inverseBaseLightProject
  EngineMat4 inverseBaseLightProject = {};
  // lightCastsShadows( record )
  bool castsShadows = false;
};

// a brush entity as the flow reads it, from its ChunkEntityState (C31)
struct ViewFlowEntity
{
  PreviewId id = NoPreviewId;
  // ChunkEntityState::areas, in chain order (only which areas they are is read)
  std::span<const int> areas;
  // ChunkEntityState::inverseBaseModelProject
  EngineMat4 inverseBaseModelProject = {};
};

// a portal chain entered into an area (AddAreaToView), for the tests and the log
struct ViewFlowChain
{
  int area = -1;
  // the chain's planes (ps->numPortalPlanes): the view's 5, or a stack's edge planes and
  // its portal's plane
  std::size_t planes = 0;
  ScreenRect rect;
};

struct ViewFlowInputs
{
  LitView view;
  // the .proc's tree and its AreaData (lit/SceneGeometry.h procTree(), areaData()); a
  // tree without nodes, or without its AreaData, counts as none
  const ProcTree* tree = nullptr;
  const AreaData* areas = nullptr;
  std::span<const ViewFlowLight> lights;
  std::span<const ViewFlowEntity> entities;
  // tests: the view's scissor (viewDef->scissor) to use instead of viewScissor( view )
  std::optional<ScreenRect> scissorForTesting;
  // tests: every chain entered, in order, in ViewFlowResult::chainsForTesting
  bool recordChainsForTesting = false;
};

/**
 * What FindViewLightsAndEntities leaves (C32.6): the view's area, the areas connected to
 * it and those the flow marked (area.viewCount), per world entity, brush entity and light
 * the union of the rects of the chains it passed (its viewEntity's or viewLight's
 * scissorRect), kept only when not empty: an empty one is not directly visible
 * (tr_frontend_addmodels.cpp:340) and an empty-scissor light lights nothing
 * (tr_frontend_addmodels.cpp:362-367). The outside set (D81: lights and entities without
 * references, world pieces in area -1) is never in an area's lists, so never here: the
 * caller adds it. Without a tree nothing is flowed (D94) and the caller keeps LP2's
 * rules.
 */
struct ViewFlowResult
{
  int viewArea = -1;
  // tr.viewDef->connectedAreas, one per area
  std::vector<bool> connectedAreas;
  // the areas AddAreaToView marked, one per area
  std::vector<bool> areaFlowed;
  // per area its world entity's rect; absent: not directly visible
  std::vector<std::optional<ScreenRect>> worldRects;
  std::map<PreviewId, ScreenRect> entityRects;
  std::map<PreviewId, ScreenRect> lightRects;
  // AddAreaToView calls, and the portal sides gone through
  std::size_t chains = 0;
  std::size_t portalsVisited = 0;
  // false without a tree (D94)
  bool flowed = false;
  std::vector<ViewFlowChain> chainsForTesting;
};

/**
 * idRenderWorldLocal::FindViewLightsAndEntities (RenderWorld_portals.cpp:754-822) after
 * R_RenderView's frustum (tr_frontend_main.cpp:912-921, engineViewFrustumPlanes): the
 * view's area is PointInArea( vieworg ) (r_usePortals 1; an area past the count, the
 * engine's Error, is -1 here), BuildConnectedAreas (engineBuildConnectedAreas), then
 * FlowViewThroughPortals( vieworg, the first 5 planes, the view's scissor ) (r_singleArea
 * 0), with every piece mirrored in ViewFlow.cpp under its own engine range:
 * FlowViewThroughPortals (:658-687), FloodViewThroughArea_r (:514-647), AddAreaToView
 * (:412-421, no probes: SC306), AddAreaViewEntities (:222-267), AddAreaViewLights
 * (:364-402), ScreenRectFromWinding (:428-451) and the corner culls (:131-143, :276-288).
 * PortalIsFoggedOut is false (D83, M-FLOW-DIFF-01) and no portal blocks (K30). The
 * recursion is an explicit stack on the heap, each frame's planes and clipped winding in
 * heap memory; each portal side enters a stack once, so its depth is at most twice the
 * portal count (P4.23: the render thread is a plain std::thread). Pure.
 */
ViewFlowResult flowView(const ViewFlowInputs& inputs);

/**
 * idRenderWorldLocal::BuildConnectedAreas and BuildConnectedAreas_r
 * (RenderWorld_portals.cpp:694-738): every area true for a view area of -1; otherwise
 * the areas a flood from it reaches through every portal (none blocks, K30). Pure.
 */
std::vector<bool> engineBuildConnectedAreas(const AreaData& areas, int viewArea);

/**
 * idRenderWorldLocal::ScreenRectFromWinding (RenderWorld_portals.cpp:428-451) of a
 * winding in world space (tr.identitySpace: R_LocalPointToGlobal leaves it as it is):
 * viewWidth = viewport.x2 - x1 (width - 1) and viewHeight = viewport.y2 - y1 (height -
 * 1, P4.16), each point through engineGlobalToNdc, then AddPoint( ( ndc.x · 0.5 + 0.5 ) ·
 * viewWidth, ( ndc.y · 0.5 + 0.5 ) · viewHeight ), then Expand. Pure.
 */
ScreenRect engineScreenRectFromWinding(
  const LitView& view, const std::vector<EngineVec3>& winding);

/**
 * CullEntityByPortals and CullLightByPortals with r_use*PortalCulling 1
 * (RenderWorld_portals.cpp:131-143, :276-288): culled when every one of the eight corners
 * is in front (FRUSTUM_CULL_FRONT: the copied CullFrustumCornersToPlane's sign bits) of
 * any one of the chain's planes. The corners are GetFrustumCorners(
 * inverseBaseModelProject, bounds_unitCube ) for an entity, GetFrustumCorners(
 * inverseBaseLightProject, bounds_zeroOneCube ) for a light. Pure.
 */
bool engineCullCornersByPortals(
  const EngineFrustumCorners& corners, std::span<const EnginePlane> planes);

/**
 * idRenderLightLocal::LightCastsShadows (RenderCommon.h:181-184, C32, P4.21):
 * parms.forceShadows || ( !parms.noShadows && lightShader->LightCastsShadows() ), the
 * shader's through engineLightCastsShadows (Material.h:719-723: MF_FORCESHADOWS, which
 * the material keyword "forceShadows" sets, Material.cpp:2684-2689, or neither a fog, an
 * ambient nor a blend light and not MF_NOSHADOWS; MaterialEntry::materialFlags holds
 * every materialFlags_t bit the copied parser set). forceShadows is never set for a map
 * light: ParseSpawnArgsToRenderLight clears the renderLight_t (d3xp/Light.cpp:90) and
 * only the projectiles' lights set it (d3xp/Projectile.cpp:262, :1186), so here it is a
 * constant false (mirror.json M-LIGHT-DIFF-07). nullopt while the light's shader is
 * pending or has no entry: the caller treats the light as pending, LP2's rule. Pure.
 */
std::optional<bool> lightCastsShadows(const LightRecord& light);

} // namespace tb::lit
