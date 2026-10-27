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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_addlights.cpp,
// renderer/tr_frontend_addmodels.cpp, renderer/RenderCommon.h and
// renderer/RenderBackend.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "LightTriangles.h"
#include "LitView.h"
#include "lit/ChunkBuilder.h"
#include "lit/LightRecord.h"
#include "lit/MaterialService.h"
#include "lit/ModelInstance.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::lit
{
struct AreaData;
struct GridTable;
struct PortalFogTable;
struct ProcTree;

// a chunk's material as the frontend finds it: an entry, pending (not in the table yet),
// or neither (the lookup failed: the chunk is not drawn)
struct MaterialAnswer
{
  std::shared_ptr<const MaterialEntry> entry;
  bool pending = false;
};

// registers for a consumer: the current evaluation, the consumer's last one kept (D52),
// or none yet
struct RegisterAnswer
{
  std::shared_ptr<const EvaluatedRegisters> registers;
  bool current = false;
};

/**
 * LP2.2a (SC207): a chunk as the view's drawSurf_t: the chunk, its space (an index into
 * FrontendLists::spaces), its material and registers. A surface whose material (null) or
 * registers (null) are pending (D77) is drawn with _default images until they arrive: in
 * the prepass solid (unless its known material is translucent), in the interactions the
 * bump _flat and the diffuse _default (pendingSurfaceInteraction); its pointer is into
 * the chunk map the lists were built from, which a new chunk version replaces.
 */
struct LitSurface
{
  const Chunk* chunk = nullptr;
  // LP7.2 (C52, D129): the model instance whose header chunk is (null: a chunk); its
  // indexes are its shared surface's
  const ModelInstance* instance = nullptr;
  std::size_t space = 0;
  std::shared_ptr<const MaterialEntry> material;
  std::shared_ptr<const EvaluatedRegisters> registers;
  // D77: the material or its registers are pending, or the registers are the
  // consumer's last (D52)
  bool provisional = false;
  // DX.2: in a light's lists, the runs of the chunk's triangles the static interaction
  // lights (R_CreateInteractionLightTris); null: all of them
  std::shared_ptr<const LightTriangles> lightTriangles;
  // LP4.3 (C36, P4.11): drawSurf->area, the area whose light grid lights the surface in
  // the ambient pass (RenderBackend.cpp:1196), from surfaceGridArea when the lists are
  // built (tr_frontend_addmodels.cpp:814-853); none: the probe path
  std::optional<int> gridArea;
  // LP5.2a (C46, P6.14): drawSurf->scissorRect, the stage pass's scissor: a world chunk's
  // the rect of its area's world entity (the union of the rects of the portal chains
  // that reached the area, RenderWorld_portals.cpp:262-265: the flow's worldRects), a
  // brush entity's its entity's (entityRects); the view's scissor for the outside set
  // (D81) and without a tree (D94)
  ScreenRect scissor;
};

/**
 * LP5.3 (C45, C49): what a view light is to the passes. Fog and blend come from the
 * shader's fogLight and blendLight flags (Material.h IsFogLight, IsBlendLight), which
 * FogAllLights draws and both DrawInteractionsPass phases skip (RenderBackend.cpp:
 * 3999-4007); of the others a light on R_LightUsesMathPath is a math light, then an
 * ambientLight shader's an ambient light (a math light whose shader is an ambientLight
 * one is Math here and still counted among the ambient lights by its flag), else a
 * texture light.
 */
enum class ViewLightKind
{
  Texture,
  Math,
  Ambient,
  Fog,
  Blend,
};

/**
 * LP2.2a (SC207): a viewLight_t: the record, its shader and registers (snapped as
 * R_AddSingleLight snaps them; LP5.3: a fog or blend light's never, :155), the scissor,
 * and the three interaction lists in their chain order. LP5.3 (C49): its kind and
 * fogPlane (:212-221, engineFogPlane: normalize( baseLightProject row 2 - row 3 ), the
 * volume's far plane), which FogPass reads.
 */
struct ViewLight
{
  std::shared_ptr<const LightRecord> record;
  std::shared_ptr<const MaterialEntry> shader;
  std::vector<float> registers;
  ScreenRect scissor;
  std::vector<LitSurface> localInteractions;
  std::vector<LitSurface> globalInteractions;
  std::vector<LitSurface> translucentInteractions;
  // the registers are the light's last (D52)
  bool provisional = false;
  ViewLightKind kind = ViewLightKind::Texture;
  EnginePlane fogPlane = {};
  // LP3.0 (C58, D144): the light volume's window-space box (LightScissor::projected) and
  // viewLight_t::shadowLOD and shadowFadeOut (RenderCommon.h), engineShadowLod's for a
  // light that casts (lightCastsShadows, r_skipShadows 0), else -1 and 0; LP3.1 reads
  // them
  EngineBounds projected = {};
  int shadowLod = -1;
  float shadowFadeOut = 0.0f;
  /**
   * LP3.0 (C57, P3.14): the light's casters, for a light that will take tiles (it casts,
   * shadowLod > -1, it has surfaces): every chunk and model instance of a cast class
   * (engineCastClass) whose areas reach the light by lightReachesCaster, whose entity is
   * visible, whose bounds the light volume does not cull (R_CullModelBoundsToLight),
   * whose material's spectrum is the light shader's (FAM:935-938) and whose light
   * triangles for this light are not None (INT:658, FAM:1089), on screen or off, in
   * chunk order then instance order; each with its space (a shadow-only entity's too:
   * R_SetEntityDefViewEntity, FAL:694-698), its material and its light triangles (null:
   * all; Pending: not known yet). A TranslucentCaster counts and is never drawn (P3.17).
   * Nothing draws them before LP3.1.
   */
  std::vector<LitSurface> casters;
};

// LP5.3 (C45, C49): the kind of a light of this shader (ViewLightKind's rule). Pure.
ViewLightKind engineViewLightKind(
  const MaterialEntry& shader, const RenderLightParms& light);

/**
 * LP2.2a (C27, A1, D67): what R_AddLights, R_AddModels and R_OptimizeViewLightsList
 * leave for the backend: the spaces, the surfaces the depth prepass draws (the leading
 * SS_SUBVIEW ones first), and the lights that have interactions, largest scissor first.
 */
struct FrontendLists
{
  std::vector<LitSpace> spaces;
  std::vector<LitSurface> drawSurfs;
  std::vector<ViewLight> lights;
  // D77: a material, an evaluation or a light's registers is pending
  bool pending = false;
  // DX.2: a lit surface's light triangles are not known yet (it is drawn whole); the
  // surfaces drawn in runs, and the triangles culled from them
  bool lightTrianglesPending = false;
  std::size_t lightTriangleSurfaces = 0;
  std::size_t culledTriangles = 0;
  // LP5.1 (C45): the math lights in the lists (tr.pc.c_mathViewLights,
  // tr_frontend_addlights.cpp:738-745: R_LightUsesMathPath of every light it counts in
  // c_viewLights, a fog or blend light's too), and the ambientLight lights with surfaces
  // (the preview's own count); LP5.3 (C49): the fog and blend lights in the lists, every
  // one kept with or without a surface (:727-735)
  std::size_t mathLights = 0;
  std::size_t fogAndBlendLights = 0;
  std::size_t ambientLights = 0;

  // ----- LP4.1 (C16, C32, D79, D94): the view's portal flow (lit/ViewFlow.h) -----

  // the flow ran: the scene has a tree (false without one: LP2's rules, D94)
  bool flowed = false;
  // PointInArea( vieworg ): -1 outside the world, and without a tree
  int viewArea = -1;
  // LP4.2a (C37, D79): tr.viewDef->connectedAreas, one per area (all true at view area
  // -1; empty without a tree), which the per-frame probe choice reads from the lists
  std::vector<bool> connectedAreas;
  // the areas AddAreaToView marked (area.viewCount), and the chains it was called with
  std::size_t areasFlowed = 0;
  std::size_t portalChains = 0;
  // LP5.3 (C49, D123): the portal sides the flow found fogged out, and the portals whose
  // fog light's registers were first-seen (left open; the lists pending)
  std::size_t portalsFogged = 0;
  std::size_t portalFogPending = 0;
  // LP7.3 (C54): the dress surfaces in drawSurfs (a chunk each, Chunk::dress)
  std::size_t dressSurfaces = 0;
  // the view lights that went on to the pairing: a flow rect (or the outside set's
  // projected rect alone, D81), registers that add light, and a scissor that is not
  // empty (before R_OptimizeViewLightsList drops those without a surface): the viewLights
  // R_AddLights keeps (removeFromList false, tr_frontend_addlights.cpp:474), no engine
  // counter, less a texture, math or ambient light whose scissor is empty (it contacts no
  // entity, tr_frontend_addmodels.cpp:362-367, so it has no surface and no counter counts
  // it). LP5.3 (C49): a fog or blend light counts with an empty scissor too and without
  // the snap's test (:155): R_AddLights keeps it, and its cube draws
  std::size_t lightsInView = 0;
  // brush-entity chunks left out because their entity got no flow rect
  std::size_t entitiesCulled = 0;
  // flowView's own time in this build (steady clock), in milliseconds
  double flowMs = 0.0;

  // LP4.3 (C36): the drawn surfaces (drawSurfs) that have a grid area
  std::size_t gridSurfaces = 0;

  // ----- LP3.0 (C57, P3.14, P3.24f): the caster light-triangle requests -----

  // the lights whose casters were gathered (ViewLight::casters)
  std::size_t casterLights = 0;
  // the (light, chunk or instance) pairs whose light triangles were asked for as
  // casters, those the cache answered (All, Runs or None) and those Pending, and the
  // casters kept (not None)
  std::size_t casterPairs = 0;
  std::size_t casterPairsResident = 0;
  std::size_t casterPairsPending = 0;
  std::size_t casters = 0;
  // the gathering's own time in this build (steady clock), in milliseconds
  double casterMs = 0.0;
};

// what the frontend reads; the functions answer from the material table and the
// service (LitPasses) or from a test's tables
struct FrontendInputs
{
  LitView view;
  const std::map<ChunkKey, Chunk>* chunks = nullptr;
  // LP7.2 (C52, D129): the scene's model instances (SceneGeometry::instances), listed
  // after the chunks; null: none
  const std::map<ChunkKey, ModelInstance>* instances = nullptr;
  // the entities whose chunks are not worldspawn's; a chunk of an entity without a state
  // is drawn as worldspawn (identity, engineWorldShaderParms)
  const std::unordered_map<PreviewId, ChunkEntityState>* entities = nullptr;
  const LightTable* lights = nullptr;
  // LP4.1 (D79, D94): the scene's .proc tree and its AreaData (lit/SceneGeometry.h
  // procTree(), areaData()), which the view's flow runs on; either null (or a tree
  // without nodes): no flow, LP2's rules
  const ProcTree* tree = nullptr;
  const AreaData* areas = nullptr;
  // LP5.3 (C49, D123): the scene's portal -> fog light table (SceneGeometry::
  // portalFogTable) of these areas; null, or another AreaData's: no portal is fogged
  const PortalFogTable* portalFog = nullptr;
  // LP4.3 (C36, D79, P4.11): the bake's light grid (null or empty: none) and whether an
  // area's grid is valid now: it has points and its atlas is Ready or Loading (the
  // engine's lightGridPoints.Num() && image && !image->IsDefaulted(),
  // tr_frontend_addmodels.cpp:826-852); the caller answers it from the slots' states
  const GridTable* grid = nullptr;
  std::function<bool(int area)> gridValid;
  std::function<MaterialAnswer(const std::string&)> material;
  std::function<RegisterAnswer(PreviewId, const MaterialEntry&, const ShaderParms&)>
    registers;
  // DX.2: the light triangles of a surface lit by a light, in the surface's space; none:
  // every triangle
  std::function<std::shared_ptr<const LightTriangles>(
    const LightRecord&, const LitSurface&, const LitSpace&)>
    lightTriangles;
  // LP5.3, tests: the view's scissor (viewDef->scissor) to use instead of viewScissor(
  // view ), as ViewFlowInputs::scissorForTesting (a subview's would be smaller)
  std::optional<ScreenRect> scissorForTesting;
  // LP3.1 (C57), tests: r_skipShadows 1's stand-in (LitPasses::setSkipShadowsForTesting):
  // no light gets a shadow LOD or casters, as with EngineFrame.skipShadows
  bool skipShadows = false;
};

/**
 * R_LightUsesMathPath (RenderCommon.h:1592-1600): math_enabled and r_mathLightsEnabled 1.
 */
bool engineLightUsesMathPath(const RenderLightParms& light);

/**
 * R_CullModelBoundsToLight (RenderCommon.h:1573-1578): the model's local bounds outside
 * the light volume: CullBoundsToMVP( baseLightProject · modelRenderMatrix, bounds, true
 * ).
 */
bool engineCullModelBoundsToLight(
  const EngineMat4& baseLightProject,
  const EngineBounds& localBounds,
  const EngineMat4& modelRenderMatrix);

/**
 * R_AddSingleLight's skip of a purely additive light (tr_frontend_addlights.cpp:152-197):
 * for every stage whose condition is on, its colour registers below 0.001 are snapped to
 * 0 in registers; whether one of them is then above 0.
 */
bool engineSnapLightRegisters(const MaterialEntry& shader, std::vector<float>& registers);

/**
 * The light's scissor (tr_frontend_addlights.cpp:244-284, r_useLightScissors 3, D67):
 * the zero-to-one cube through the world's mvp times inverseBaseLightProject, fully
 * clipped and projected; nullopt when its z range is empty (the light is outside the
 * view frustum). Else the projected bounds times ( viewport.x2 - x1, y2 - y1 ) through
 * idMath::Ftoi, expanded by a pixel, intersected with flowRect, the viewLight's
 * scissorRect the portal flow left (LP4.1, C33: the union of the rects of the chains the
 * light passed, lit/ViewFlow.h; the view's scissor without a tree, D94, and for the
 * outside set's lights, D81), with zmin and zmax (no depth-bounds test, SC212). Pure.
 */
std::optional<ScreenRect> engineLightScissor(
  const LitView& view,
  const EngineMat4& inverseBaseLightProject,
  const ScreenRect& flowRect);

// LP3.0 (C58): the light scissor with the window-space box it was made from
struct LightScissor
{
  ScreenRect scissor;
  // ProjectedFullyClippedBounds( worldMvp · inverseBaseLightProject, the zero-to-one
  // cube ): x, y and depth in 0..1, before the flow-rect intersection
  // (tr_frontend_addlights.cpp:248-264), the shadow LOD's input (engineShadowLod)
  EngineBounds projected = {};
};

// engineLightScissor's rect beside its projected box; nullopt where it is. Pure.
std::optional<LightScissor> engineLightScissorAndBox(
  const LitView& view,
  const EngineMat4& inverseBaseLightProject,
  const ScreenRect& flowRect);

// the interaction list a surface goes to (tr_frontend_addmodels.cpp:991-1006)
enum class InteractionList
{
  Local,
  Global,
  Translucent,
};

/**
 * MC_TRANSLUCENT: translucent; a light that casts shadows (not noShadows) on a material
 * with MF_NOSELFSHADOW: local; else global.
 */
InteractionList engineInteractionList(const MaterialEntry& material, bool lightNoShadows);

/**
 * RenderInteractions' split (RenderBackend.cpp:1756-1779): the surfaces whose material
 * has a fast-path bump image first, in list order, then the others, with the caps of
 * SC222 (2048 in all, 256 complex): idStaticList::Append drops a surface beyond them
 * silently. A pending surface counts as complex.
 */
std::vector<const LitSurface*> engineOrderInteractionSurfaces(
  const std::vector<LitSurface>& surfaces);

/**
 * LP3.0 (D148, P3.2, K39 restated): R_OptimizeViewLightsList's sort
 * (tr_frontend_addlights.cpp:794-822) of the sort input, areas: the viewLights in their
 * order before it (the flow's, ViewFlowResult::lightOrder) with each one's scissor area
 * after the r_useShadowSurfaceScissor shrink, every light still in the list counted (one
 * with no surface too: engineSortArea). The C runtime's qsort itself (the UCRT's on
 * Windows, as the engine's MSVC build: its ties are the engine's; glibc's elsewhere, K48)
 * sorts { index, area } records ascending by the engine's comparator, the int difference
 * a - b (wrapping as MSVC's sub does), then the list is rebuilt by prepending: the
 * indices in the new viewLights order, the largest first. Pure.
 */
std::vector<std::size_t> engineViewLightsOrder(const std::vector<int>& areas);

/**
 * LP3.0 (D148): the area a light enters the sort with (FAL:750-791, 810):
 * r_useShadowSurfaceScissor 1 shrinks the rect of a light whose shader casts shadows
 * (Material.h LightCastsShadows, the light's own noShadows not consulted) to the union of
 * its surfaces' rects; every interaction surface's rect is the light's
 * (tr_frontend_addmodels.cpp:986), so a light with a surface keeps its rect, and one
 * without is intersected with a cleared rect (32000, 32000, -32000, -32000), whose area
 * wraps to -199,095,295 (ScreenRect::area, ScreenRect.h:69-72): it sorts after every
 * light. A light whose shader does not cast (fog, blend, ambient, MF_NOSHADOWS) keeps
 * its rect. Pure.
 */
int engineSortArea(const ScreenRect& scissor, bool hasSurfaces, bool shaderCastsShadows);

/**
 * engineViewLightsOrder over lights already in the engine's pre-sort order, each with
 * its scissor's area (lights that all have surfaces, or whose areas need no shrink).
 * Equal areas fall in the C runtime qsort's order. Pure.
 */
void engineSortViewLights(std::vector<ViewLight>& lights);

/**
 * LP5.2a (C46, D115, K37): the stage pass's order of the view's drawn surfaces,
 * R_SortDrawSurfs (tr_frontend_main.cpp:255-359): the engine packs a 64-bit key per
 * surface, the bits of float( SS_POST_PROCESS - sort ) in its high dword, Ftoui16( min ·
 * 0xFFFF ) of DepthBoundsForBounds( space->mvp, the surface's bounds ) above the low 16
 * bits, and ( numDrawSurfs - i ) in them, and sorts the keys descending: the sort value
 * ascending (as the float's bits order it: a sort above SS_POST_PROCESS, whose key is a
 * negative float, comes first), then the farthest first within one sort value (the
 * engine's "depth smallest first" comment is wrong, SC522), then creation order. Here
 * the same three levels are compared one by one, creation order being ChunkKey's order
 * (its operator<: entity, cell, area, material, part; ChunkBuilder.cpp), the order
 * buildFrontendLists makes the surfaces in (the leading SS_SUBVIEW ones first, which sort
 * first anyway). A surface whose material is pending sorts as SS_OPAQUE (the stage pass
 * skips it).
 *
 * K37: the engine sorts dmap's model surfaces, one per area and material, each drawn in
 * its triangle order; the preview sorts its chunks (a material's faces of one area may be
 * several chunks, in cells), so two overlapping translucent pieces of one material in one
 * area may blend in another order than the engine's one surface draws them, and the
 * depth key is the chunk's bounds', not the model surface's.
 *
 * Pure; the render thread, once per frame (the lists may be kept).
 */
std::vector<const LitSurface*> stagePassOrder(
  const std::vector<LitSurface>& drawSurfs, const std::vector<LitSpace>& spaces);

/**
 * LP2.2a (C27, A1, D67): the per-light lists of a view. Chunks of an invisible entity
 * state are left out. A surface is directly visible when its chunk's bounds are not
 * culled by its space's mvp (CullBoundsToMVP); a visible surface whose material is
 * drawn (IsDrawn, a deform of none or a particle one) or pending is a prepass surface.
 * Each light record that is visible, has a volume (LP5.1: a math or ambientLight light is
 * a light as any, C45) with its shader and falloff known, whose registers are known (its
 * last ones while a new evaluation is pending, D52; a light seen first without registers
 * is skipped) and add light, and whose scissor is
 * not empty, gets every directly visible surface that receives lighting (a pending one
 * does), is not culled to the light volume by its bounds through its space's model
 * matrix (R_CullModelBoundsToLight), and has the light shader's spectrum, in the list of
 * engineInteractionList, in chunk order, with its light triangles (DX.2: the static
 * interaction's cull to the light volume; a surface with none is left out, as the engine
 * makes no interaction surface for it). A light with no surface in any list is dropped
 * (R_OptimizeViewLightsList) after the sort; LP3.0 (D148): the sort runs on every light
 * the engine's viewLights hold then, one without a surface or with an empty scissor
 * among them (engineSortArea), in the flow's order (ViewFlowResult::lightOrder; without
 * a tree, and for the outside set after the flowed lights, the reverse of the records'
 * id order, as if each were prepended in id order), through engineViewLightsOrder.
 *
 * LP5.3 (C45, C49): a fog or blend light takes the same lists by the same rules (the
 * dead noFog and ReceivesFog are not consulted: tr_frontend_addmodels.cpp:949-1008 asks
 * ReceivesLighting alone; its local list holds the noSelfShadow surfaces of a light
 * without noShadows), with no snap of its registers (tr_frontend_addlights.cpp:155), and
 * is kept with an empty scissor (no surface: an empty-scissor light contacts no entity,
 * tr_frontend_addmodels.cpp:362-367) and without a surface (R_OptimizeViewLightsList
 * keeps it, :727-735: FogAllLights draws its cube); a light culled to the view frustum
 * (the projected z range empty) is dropped as every kind is (:266-270). Each view light
 * carries its kind and fogPlane.
 * r_useShadowSurfaceScissor's shrink is the light's own rect here (every surface's rect
 * is the light's), so it changes nothing.
 *
 * LP4.1 (C32, C33, D79, D81, D94): with a tree, flowView runs first, over the visible
 * light records that pushed into an area (areaNum, derived.areas,
 * inverseBaseLightProject, lightCastsShadows; a pending shader counts as not casting
 * shadows, the light is pending anyway) and the visible entity states that reference an
 * area (their areas and inverseBaseModelProject). Then a world chunk (one of an entity
 * without a state) of area N is a candidate only when the flow gave the world entity N a
 * rect (P4.29), a brush entity's chunk only when its entity got one (else it counts in
 * entitiesCulled), and the outside set always (D81: world chunks in area -1 or past the
 * area count, entities without references); the frustum test follows as before
 * (tr_frontend_addmodels.cpp :649). A light record is a view light only when the flow
 * gave it a rect, which its scissor is intersected with (engineLightScissor), or when it
 * pushed into no area (the outside set: the view's scissor, the projected rect alone); a
 * light the flow left out asks for no registers. Each candidate surface of a view light
 * must pass lightReachesSurface (C33 a-c: a shared, connected area; an outside light
 * lights only the outside set, and every light the outside set's surfaces it meets)
 * before LP2's tests. The lights enter the sort in the flow's order (LP3.0, D148).
 * Without a tree (D94) every rule is LP2's: no flow, every chunk a candidate, the view's
 * scissor.
 *
 * LP5.3 (C49, D123, P6.6, P6.8): with a tree and a portal -> fog light table of its
 * AreaData, before flowView the registers of each assigned fog light are asked for (its
 * record's shader and shaderParms, the consumer its preview id: the same answer its view
 * light gets) and the flow gets per portal the density of stage 0's alpha
 * (engineFogDensity), or pending when they are first-seen: the portal stays open and the
 * lists are pending; kept registers (D52) are used, the lists pending as for any light.
 *
 * LP4.3 (C36, P4.11, D81): with a tree and a grid, every visible surface whose material
 * receives lighting gets its grid area from surfaceGridArea (lit/ViewFlow.h): a world
 * chunk of area N with refs [N] and the union of the area's world chunks of its material
 * (FinishSurfaces' deform growth applied, Model.cpp:1346-1360: the centre is the same),
 * a brush entity's chunk with its entity's materialBounds of the material and areas
 * (C31) through its space's modelRenderMatrix; the outside set and a pending material
 * have none (the probe path). Pure, given the inputs' functions; the render thread (the
 * flow's copied functions are mirror.json M-FLOW-PURE-01's).
 */
FrontendLists buildFrontendLists(const FrontendInputs& inputs);

// LP7.2 (D129): a surface's index count: its chunk's, or its instance's shared surface's
std::size_t surfaceIndexCount(const LitSurface& surface);

/**
 * LPF.0b (C43, D106): what of the material table the frontend lists depend on beyond its
 * version: the registry, and materialsRevision, which a re-parse moves once (an image
 * override moves neither), so that LitPasses builds its lists again once per re-parse.
 * Any thread.
 */
struct ListsTableKey
{
  std::uint64_t registry = 0;
  std::uint64_t materialsRevision = 0;

  static ListsTableKey of(const MaterialTable* table);
  bool operator==(const ListsTableKey&) const = default;
};

/**
 * D52 (§3.5 Registers, C27's last sentence): the render thread's registers per consumer
 * (an entity's or a light's preview id) and material. An answer is the latest table's
 * evaluation of (material, shaderParms, time 0), else the service's (evaluate queues it
 * ahead of the lookups), else the consumer's last registers of that material, never the
 * defaults: a light-colour drag never flickers. LPF.0b (C43, P5.27): after a re-parse
 * (whose register deltas the service erased) the last registers stand only while they
 * cover the material's numRegisters, and are dropped otherwise. Consumers not asked for
 * in a build are forgotten at its end. Render thread.
 */
class RegisterCache
{
private:
  struct Kept
  {
    std::shared_ptr<const EvaluatedRegisters> registers;
    bool used = false;
  };

  std::map<std::pair<PreviewId, std::string>, Kept> m_kept;

public:
  RegisterAnswer registers(
    PreviewId consumer,
    const MaterialEntry& material,
    const ShaderParms& shaderParms,
    const MaterialTable& table,
    MaterialService* service);

  // a build begins: every kept entry is unused until asked for
  void beginBuild();
  // the build ended: the entries not asked for go
  void endBuild();
  std::size_t size() const;
  void clear();
};

} // namespace tb::lit
