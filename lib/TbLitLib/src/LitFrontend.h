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
  std::size_t space = 0;
  std::shared_ptr<const MaterialEntry> material;
  std::shared_ptr<const EvaluatedRegisters> registers;
  // D77: the material or its registers are pending, or the registers are the
  // consumer's last (D52)
  bool provisional = false;
  // DX.2: in a light's lists, the runs of the chunk's triangles the static interaction
  // lights (R_CreateInteractionLightTris); null: all of them
  std::shared_ptr<const LightTriangles> lightTriangles;
  // LP4.3 (C36): drawSurf->area, the area whose light grid lights the surface in the
  // ambient pass (RenderBackend.cpp:1196); never set in LP4.2a, so every surface takes
  // the probe path (mirror.json M-AMB-DIFF-03)
  std::optional<int> gridArea;
};

/**
 * LP2.2a (SC207): a viewLight_t: the record, its shader and registers (snapped as
 * R_AddSingleLight snaps them), the scissor, and the three interaction lists in their
 * chain order.
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
};

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
  // lights skipped: math lights (LP5), fog and blend lights (LP5), ambient lights (no LP2
  // program), and lights whose shader or falloff the table does not have; with a tree,
  // only those the view's flow gave a rect are counted
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
  // the view lights that went on to the pairing: a flow rect (or the outside set's
  // projected rect alone, D81), a texture light whose registers add light, and a scissor
  // that is not empty (before R_OptimizeViewLightsList drops those without a surface)
  std::size_t lightsInView = 0;
  // brush-entity chunks left out because their entity got no flow rect
  std::size_t entitiesCulled = 0;
  // flowView's own time in this build (steady clock), in milliseconds
  double flowMs = 0.0;
};

// what the frontend reads; the functions answer from the material table and the
// service (LitPasses) or from a test's tables
struct FrontendInputs
{
  LitView view;
  const std::map<ChunkKey, Chunk>* chunks = nullptr;
  // the entities whose chunks are not worldspawn's; a chunk of an entity without a state
  // is drawn as worldspawn (identity, engineWorldShaderParms)
  const std::unordered_map<PreviewId, ChunkEntityState>* entities = nullptr;
  const LightTable* lights = nullptr;
  // LP4.1 (D79, D94): the scene's .proc tree and its AreaData (lit/SceneGeometry.h
  // procTree(), areaData()), which the view's flow runs on; either null (or a tree
  // without nodes): no flow, LP2's rules
  const ProcTree* tree = nullptr;
  const AreaData* areas = nullptr;
  std::function<MaterialAnswer(const std::string&)> material;
  std::function<RegisterAnswer(PreviewId, const MaterialEntry&, const ShaderParms&)>
    registers;
  // DX.2: the light triangles of a surface lit by a light, in the surface's space; none:
  // every triangle
  std::function<std::shared_ptr<const LightTriangles>(
    const LightRecord&, const LitSurface&, const LitSpace&)>
    lightTriangles;
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
 * R_OptimizeViewLightsList's sort (tr_frontend_addlights.cpp:794-822): qsort ascending
 * by scissor area, then the list rebuilt by prepending, so the largest light is first.
 * The engine's qsort is not stable; here equal areas keep the reverse of their input
 * order (mirror.json M-FRONT-DIFF-01).
 */
void engineSortViewLights(std::vector<ViewLight>& lights);

/**
 * LP2.2a (C27, A1, D67): the per-light lists of a view. Chunks of an invisible entity
 * state are left out. A surface is directly visible when its chunk's bounds are not
 * culled by its space's mvp (CullBoundsToMVP); a visible surface whose material is
 * drawn (IsDrawn, a deform of none or a particle one) or pending is a prepass surface.
 * Each light record that is visible, has a volume, is a texture light (not math, fog,
 * blend or ambient) with its shader and falloff known, whose registers are known (its
 * last ones while a new evaluation is pending, D52) and add light, and whose scissor is
 * not empty, gets every directly visible surface that receives lighting (a pending one
 * does), is not culled to the light volume by its bounds through its space's model
 * matrix (R_CullModelBoundsToLight), and has the light shader's spectrum, in the list of
 * engineInteractionList, in chunk order, with its light triangles (DX.2: the static
 * interaction's cull to the light volume; a surface with none is left out, as the engine
 * makes no interaction surface for it). A light with no surface in any list is dropped
 * (R_OptimizeViewLightsList); the rest are sorted (engineSortViewLights).
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
 * before LP2's tests. The lights stay in id order before the sort (M-FRONT-DIFF-01).
 * Without a tree (D94) every rule is LP2's: no flow, every chunk a candidate, the view's
 * scissor. Pure, given the inputs' functions; the render thread (the flow's copied
 * functions are mirror.json M-FLOW-PURE-01's).
 */
FrontendLists buildFrontendLists(const FrontendInputs& inputs);

/**
 * D52 (§3.5 Registers, C27's last sentence): the render thread's registers per consumer
 * (an entity's or a light's preview id) and material. An answer is the latest table's
 * evaluation of (material, shaderParms, time 0), else the service's (evaluate queues it
 * ahead of the lookups), else the consumer's last registers of that material, never the
 * defaults: a light-colour drag never flickers. Consumers not asked for in a build are
 * forgotten at its end. Render thread.
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
