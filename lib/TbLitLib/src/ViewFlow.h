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
struct LightTable;
struct ProcTree;

// MAX_PORTAL_PLANES (RenderWorld_portals.cpp:37): a stack's edge planes; the portal's
// own plane makes the 21st (portalStack_t::portalPlanes, :39-47)
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

/**
 * LP5.3 (C49, D123, P6.8; D83 closed): the scene's portal -> fog light table,
 * R_CreateLightDefFogPortals (RenderWorld_defs.cpp:647-687, called by R_CreateLightRefs
 * at :729 after the light's references) over every light record: per portal (its
 * doublePortal_t, AreaPortal::portal) the first fog light that assigned itself, the
 * records taken in preview-id order where the engine takes its lights in their creation
 * order (mirror.json M-FLOW-DIFF-01, restated: a portal two fog volumes hold may take
 * another one's). A record assigns itself when it is visible, has a volume, its shader
 * known, a fogLight shader without MF_NOPORTALFOG; then per area of its references and
 * per portal side of the area (the area's list) whose portal has no fog light yet, when
 * every point of the side's winding is inside the light's volume
 * (WindingCompletelyInsideLight, :627-637: !CullPointToMVP( baseLightProject, p, true ),
 * strict: a point on the volume's boundary is outside). Built by the scene
 * (SceneGeometry::portalFogTable) whenever its light table or its AreaData changes (a
 * fog light, its shader's fogLight or MF_NOPORTALFOG through a re-parse, which builds the
 * light's record again, C43/D106, or a new tree); immutable once published. Pure.
 */
struct PortalFogTable
{
  // per portal index, the fog light's preview id; NoPreviewId: none
  std::vector<PreviewId> fogLights;
  // what it was built from
  std::uint64_t lightsVersion = 0;
  const AreaData* areas = nullptr;
};

PortalFogTable buildPortalFogTable(
  const LightTable& lights, const AreaData& areas, std::uint64_t lightsVersion);

/**
 * LP5.3 (C49, D123, P6.6): a portal's fog as the view's flow tests it: its fog light's
 * density a from the registers the frontend evaluated before the flow (engineFogDensity
 * of stage 0's alpha register, as FogPass computes it), or pending (the light's
 * registers first-seen: the portal stays open and the frame unconverged).
 */
struct ViewFlowPortalFog
{
  bool assigned = false;
  bool pending = false;
  float a = 0.0f;
};

/**
 * idRenderWorldLocal::PortalIsFoggedOut (RenderWorld_portals.cpp:458-507) for a portal
 * whose fog light's density is a: forward = a · the world's modelViewMatrix column 2,
 * without FogPass' + 0.5; the portal is fogged out when every point of the winding (the
 * side's unclipped p->w) has forward.Distance( point ) >= 0.5, that is lies at least the
 * fog distance ahead in view depth. Pure.
 */
bool enginePortalIsFoggedOut(
  const LitView& view, float a, std::span<const EngineVec3> winding);

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
  // LP3.0 (D148, P3.2): in the order their references were made (the engine's
  // R_CreateLightRefs at UpdateLightDef, a map's lights in their spawn order; the
  // frontend gives the records' preview-id order, ids being minted in load order): each
  // area's list is walked newest first, as AddLightRefToArea prepends
  // (RenderWorld.cpp:1852-1876)
  std::span<const ViewFlowLight> lights;
  std::span<const ViewFlowEntity> entities;
  // LP5.3 (C49, D123): per portal index its fog (none past the span's end)
  std::span<const ViewFlowPortalFog> portalFog;
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
  // LP3.0 (D148, P3.2): tr.viewDef->viewLights' order after the flow: every light the
  // flood accepted (R_SetLightDefViewLight, RenderWorld_portals.cpp:66-90), the last
  // accepted first, as each is prepended at its first acceptance (:84-85); the areas are
  // walked in the flood's order and each area's lights in reverse reference order (:368)
  std::vector<PreviewId> lightOrder;
  // LP3.0 (D148): the lights the flood accepted only through chains of empty rects (a
  // portal's rect is intersected and never tested for emptiness, :603-606): their union,
  // empty; they are in viewLights all the same (R_AddSingleLight keeps an empty scissor),
  // so in the light sort, but light nothing
  std::map<PreviewId, ScreenRect> emptyLightRects;
  // AddAreaToView calls, and the portal sides gone through
  std::size_t chains = 0;
  std::size_t portalsVisited = 0;
  // LP5.3: the portal sides PortalIsFoggedOut closed (each time it did)
  std::size_t portalsFogged = 0;
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
 * (:364-402), ScreenRectFromWinding (:428-451) and the corner culls (:131-146, :276-291).
 * LP5.3 (D123, D83 closed): PortalIsFoggedOut on the clipped branch alone (:590-594),
 * after the winding's clip, over the side's unclipped winding, from portalFog (a pending
 * fog light's portal stays open); no portal blocks (K30). The
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
 * (RenderWorld_portals.cpp:131-146, :276-291): culled when every one of the eight corners
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
 * pending or has no entry: the caller treats the light as pending, LP2's rule. LP3.0
 * (C57, P3.16): noShadows is the engine's after UpdateLightDef, which forces it on a
 * light whose shader has a spectrum (RenderWorld.cpp:524-529, engineLightNoShadows).
 * Pure.
 */
std::optional<bool> lightCastsShadows(const LightRecord& light);

/**
 * LP3.0 (C57, P3.16): idRenderLightLocal::parms.noShadows as UpdateLightDef leaves it
 * (RenderWorld.cpp:524-529): the map's noshadows, or true for a light whose shader has a
 * spectrum ("force noShadows on spectrum lights"), which also moves its noSelfShadow
 * surfaces to the global list (tr_frontend_addmodels.cpp:999). A pending or missing
 * shader leaves the map's. Pure.
 */
bool engineLightNoShadows(const LightRecord& light);

// ----- the pairing and the grid area (C33, C36, D81; pure, wired in LP4.1) -----

// a view light as the pairing reads it: derived.areaNum and derived.areas (push order)
struct AreaLight
{
  int areaNum = -1;
  // empty: the light pushed into no area (the outside set, D81)
  std::span<const int> areas;
};

enum class AreaSurfaceKind
{
  // a world chunk of area N >= 0
  WorldArea,
  // a world chunk in area -1: a piece that reached solid (D47, K14): the outside set
  WorldOutside,
  // a brush entity's chunk
  Entity,
};

// a drawn surface as the pairing reads it
struct AreaSurface
{
  AreaSurfaceKind kind = AreaSurfaceKind::WorldArea;
  // WorldArea: the chunk's area
  int area = -1;
  // Entity: ChunkEntityState::areas; empty: the outside set (D81)
  std::span<const int> refs;
  // its entity's flow rect is not empty (the world entity N's, or the brush entity's):
  // IsDirectlyVisible (tr_frontend_addlights.cpp:578)
  bool directlyVisible = false;
};

/**
 * R_AddSingleLight's area walk (tr_frontend_addlights.cpp:488-582) with
 * R_AddSingleModel's contacted lights (tr_frontend_addmodels.cpp:340-381), as C33 (a)-(c)
 * states it: a surface is lit by the light (subject to the caller's volume test,
 * R_CullModelBoundsToLight, and LP2's other tests) when
 * - (a) a world chunk of area N: the world entity N is directly visible, N is one of the
 *   light's areas, and the light's area is -1 or connected to N
 *   (r_useAreasConnectedForShadowCulling 2, areasConnected);
 * - (b) a brush entity's chunk: the entity is directly visible and some area of its
 *   references is one of the light's areas and passes the same test (the engine decides
 *   an entity once, at the first such area, so where is irrelevant;
 *   MAX_CONTACTED_LIGHTS 128 per entity is not kept here, mirror.json M-FLOW-DIFF-03);
 * - (c) D81's outside set: a light without references lights only the outside geometry
 *   (world chunks in area -1, entities without references), and a light with references
 *   lights the outside geometry it meets: both by volume alone.
 * Only with a tree (D94: without one LP2's rules stand). Pure.
 */
bool lightReachesSurface(
  const AreaLight& light, const AreaSurface& surface, const AreaData& areas);

/**
 * LP3.0 (C57, P3.14, P3.16): R_AddSingleLight's area walk for shadows
 * (tr_frontend_addlights.cpp:488-634) as the caster set reads it: a surface may cast for
 * the light when one of its areas is one the light references and that area is connected
 * to the light's own area (AreasAreConnected( light->areaNum, area ), FAL:494-501,
 * r_useAreasConnectedForShadowCulling 2); a light of area -1 (every parallel light: its
 * origin is pushed 100000 along its direction, RenderWorld_defs.cpp:432-440, :703) skips
 * the connection test and casts from every area it references. Unlike
 * lightReachesSurface the surface need not be directly visible (a shadow-only entity,
 * FAL:585-633; the engine's view cull of R_ShadowBounds, :605-618, is not mirrored: K49).
 * LP3.2-FIX (C57 rev 2.3, D81, K14, H03): D81's outside set (world chunks in area -1,
 * brush entities without references: geometry dmap removes, so the engine never has it)
 * casts for a light without references alone, the light of the set, which lights that
 * geometry alone (a new room outside the hull keeps its own light's shadows until the
 * next dmap), and never for a light with references (every parallel light has references:
 * globalLightOrigin 100000 away, RenderWorld_defs.cpp:425-441, then PushFrustumIntoTree),
 * so no hull face in the void shadows the hull's inside. Its receivers are
 * lightReachesSurface's, unchanged. Only with a tree (D94: without one every chunk
 * casts). Pure; the same rule as the overload over lightCasterAreas' table.
 */
bool lightReachesCaster(
  const AreaLight& light, const AreaSurface& surface, const AreaData& areas);

/**
 * LP3.2-FIX: lightReachesCaster's area test tabulated for one light, as the caster
 * gathering (buildFrontendLists) makes it once per light: one entry per area of areas,
 * non-zero for an area the light references that is connected to the light's own area,
 * or for every area it references when its area is -1 (tr_frontend_addlights.cpp:488-501,
 * r_useAreasConnectedForShadowCulling 2). Pure.
 */
std::vector<char> lightCasterAreas(const AreaLight& light, const AreaData& areas);

/**
 * lightReachesCaster with its area test read from casterAreas, lightCasterAreas( light,
 * areas )'s table: the rule itself (the overload over AreaData calls this one), for the
 * gathering's loop over the candidates. Pure.
 */
bool lightReachesCaster(
  const AreaLight& light, const AreaSurface& surface, std::span<const char> casterAreas);

/**
 * The grid area of a model surface (tr_frontend_addmodels.cpp:814-853, C36, P4.11), for a
 * material that receives lighting: surfaceArea = PointInArea( modelRenderMatrix ·
 * GetCenter( materialBounds ) ) (CreateFromOriginAxis and TransformPoint; the bounds
 * ChunkEntityState::materialBounds, per (entity, material)); then the first of refs (the
 * entity's chain order; the world entity's [N] with its (area, material) bounds) equal
 * to surfaceArea whose grid is valid (points, an atlas, not defaulted: gridValid), else
 * the first with a valid grid, else none: the probe path. Pure; gridValid is the
 * caller's.
 */
std::optional<int> surfaceGridArea(
  const ProcTree& tree,
  const EngineMat4& modelRenderMatrix,
  const EngineBounds& materialBounds,
  std::span<const int> refs,
  const std::function<bool(int area)>& gridValid);

} // namespace tb::lit
