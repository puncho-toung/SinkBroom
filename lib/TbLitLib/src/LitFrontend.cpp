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
// renderer/tr_frontend_addmodels.cpp, renderer/RenderCommon.h,
// renderer/RenderBackend.cpp and renderer/RenderWorld_portals.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "LitFrontend.h"

#include "LitStages.h"
#include "ViewFlow.h"
#include "lit/AreaData.h"
#include "lit/EngineTables.h"
#include "lit/GridTable.h"
#include "lit/ProcFile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace tb::lit
{
namespace
{

namespace em = engine_material;

constexpr auto IdentityAxis = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};

// a deform whose source surface is drawn (tr_frontend_addmodels.cpp:788)
bool baseSurfaceDrawn(const MaterialEntry& material)
{
  return material.deform == em::DeformNone || material.deform == em::DeformParticle
         || material.deform == em::DeformParticle2;
}

} // namespace

bool engineLightUsesMathPath(const RenderLightParms& light)
{
  return light.mathLight.enabled && EngineFrame.mathLightsEnabled;
}

ViewLightKind engineViewLightKind(
  const MaterialEntry& shader, const RenderLightParms& light)
{
  if (shader.fogLight)
  {
    return ViewLightKind::Fog;
  }
  if (shader.blendLight)
  {
    return ViewLightKind::Blend;
  }
  if (engineLightUsesMathPath(light))
  {
    return ViewLightKind::Math;
  }
  return shader.ambientLight ? ViewLightKind::Ambient : ViewLightKind::Texture;
}

bool engineCullModelBoundsToLight(
  const EngineMat4& baseLightProject,
  const EngineBounds& localBounds,
  const EngineMat4& modelRenderMatrix)
{
  const auto modelLightProject =
    engineMatrixMultiply(baseLightProject, modelRenderMatrix);
  return engineCullBoundsToMvp(modelLightProject, localBounds, true);
}

bool engineSnapLightRegisters(const MaterialEntry& shader, std::vector<float>& registers)
{
  const auto snap = [&](const int index) {
    if (index >= 0 && std::size_t(index) < registers.size())
    {
      auto& value = registers[std::size_t(index)];
      // snap tiny values to zero
      if (value < 0.001f)
      {
        value = 0.0f;
      }
    }
  };
  for (const auto& stage : shader.stages)
  {
    // ignore stages that fail the condition
    if (engineRegister(registers, stage.conditionRegister) == 0.0f)
    {
      continue;
    }
    const auto& r = stage.colorRegisters;
    snap(r[0]);
    snap(r[1]);
    snap(r[2]);
    if (
      engineRegister(registers, r[0]) > 0.0f || engineRegister(registers, r[1]) > 0.0f
      || engineRegister(registers, r[2]) > 0.0f)
    {
      return true;
    }
  }
  return false;
}

std::optional<ScreenRect> engineLightScissor(
  const LitView& view,
  const EngineMat4& inverseBaseLightProject,
  const ScreenRect& flowRect)
{
  const auto both = engineLightScissorAndBox(view, inverseBaseLightProject, flowRect);
  return both ? std::optional{both->scissor} : std::nullopt;
}

std::optional<LightScissor> engineLightScissorAndBox(
  const LitView& view,
  const EngineMat4& inverseBaseLightProject,
  const ScreenRect& flowRect)
{
  // Calculate the matrix that projects the zero-to-one cube to exactly cover the light
  // frustum in clip space.
  const auto invProjectMVPMatrix =
    engineMatrixMultiply(view.worldMvp, inverseBaseLightProject);

  // r_useLightScissors 3: fully clipped
  const auto projected =
    engineProjectedFullyClippedBounds(invProjectMVPMatrix, engineZeroOneCube());
  if (projected[0][2] >= projected[1][2])
  {
    // the light was culled to the view frustum
    return std::nullopt;
  }

  // the viewport is (0, 0, width - 1, height - 1)
  const auto screenWidth = float(view.width - 1) - 0.0f;
  const auto screenHeight = float(view.height - 1) - 0.0f;

  auto lightScissorRect = ScreenRect{};
  lightScissorRect.x1 = short(engineFtoi(projected[0][0] * screenWidth));
  lightScissorRect.x2 = short(engineFtoi(projected[1][0] * screenWidth));
  lightScissorRect.y1 = short(engineFtoi(projected[0][1] * screenHeight));
  lightScissorRect.y2 = short(engineFtoi(projected[1][1] * screenHeight));
  lightScissorRect.expand();

  // vLight->scissorRect: the union of the portal chains' rects the light passed
  // (RenderWorld_portals.cpp:400), here flowRect (LP4.1, C33)
  auto scissor = flowRect;
  scissor.intersect(lightScissorRect);
  scissor.zmin = projected[0][2];
  scissor.zmax = projected[1][2];
  return LightScissor{scissor, projected};
}

InteractionList engineInteractionList(
  const MaterialEntry& material, const bool lightNoShadows)
{
  if (material.coverage == em::CoverageTranslucent)
  {
    return InteractionList::Translucent;
  }
  if (!lightNoShadows && (material.materialFlags & em::FlagNoSelfShadow) != 0)
  {
    return InteractionList::Local;
  }
  return InteractionList::Global;
}

std::vector<const LitSurface*> engineOrderInteractionSurfaces(
  const std::vector<LitSurface>& surfaces)
{
  const auto maxAll = EngineFrame.maxInteractionsPerLight;
  const auto maxComplex = EngineFrame.maxComplexInteractionsPerLight;
  auto all = std::vector<const LitSurface*>{};
  auto complex = std::vector<const LitSurface*>{};
  for (const auto& surface : surfaces)
  {
    if (surface.material && surface.material->fastPathBumpImage)
    {
      if (all.size() < maxAll)
      {
        all.push_back(&surface);
      }
    }
    else if (complex.size() < maxComplex)
    {
      complex.push_back(&surface);
    }
  }
  for (const auto* surface : complex)
  {
    if (all.size() < maxAll)
    {
      all.push_back(surface);
    }
  }
  return all;
}

namespace
{

// R_OptimizeViewLightsList's sortLight_t (tr_frontend_addlights.cpp:796-804): the light
// (here its index in the sort input) and its screen area; 16 bytes, as the engine's
struct SortLight
{
  std::size_t light = 0;
  int screenArea = 0;
};

// sortLight_t::sort: a->screenArea - b->screenArea, the int difference wrapping as
// MSVC's sub does (written without signed overflow)
int sortLightCompare(const void* a, const void* b)
{
  const auto lhs = static_cast<const SortLight*>(a)->screenArea;
  const auto rhs = static_cast<const SortLight*>(b)->screenArea;
  return int(std::uint32_t(lhs) - std::uint32_t(rhs));
}

} // namespace

std::vector<std::size_t> engineViewLightsOrder(const std::vector<int>& areas)
{
  // :805-812: one record per light, in viewLights' order
  auto sortLights = std::vector<SortLight>{};
  sortLights.reserve(areas.size());
  for (std::size_t i = 0; i < areas.size(); ++i)
  {
    sortLights.push_back(SortLight{i, areas[i]});
  }

  // :814: the C runtime's qsort, not std::sort: its order of equal areas is the engine's
  if (!sortLights.empty())
  {
    std::qsort(sortLights.data(), sortLights.size(), sizeof(SortLight), sortLightCompare);
  }

  // :816-822: rebuild the linked list in order, each prepended
  auto order = std::vector<std::size_t>{};
  order.reserve(sortLights.size());
  for (auto i = sortLights.size(); i-- > 0;)
  {
    order.push_back(sortLights[i].light);
  }
  return order;
}

int engineSortArea(
  const ScreenRect& scissor, const bool hasSurfaces, const bool shaderCastsShadows)
{
  if (!EngineFrame.useShadowSurfaceScissor || !shaderCastsShadows || hasSurfaces)
  {
    // no shrink; or the union of its surfaces' rects, each the light's (FAM:986), which
    // intersected with the light's rect leaves it as it is
    return scissor.area();
  }
  // surfRect.Clear(), no surface to unite, then vLight->scissorRect.Intersect( surfRect )
  auto surfRect = ScreenRect{};
  surfRect.clear();
  auto shrunk = scissor;
  shrunk.intersect(surfRect);
  return shrunk.area();
}

void engineSortViewLights(std::vector<ViewLight>& lights)
{
  auto areas = std::vector<int>{};
  areas.reserve(lights.size());
  for (const auto& light : lights)
  {
    areas.push_back(light.scissor.area());
  }
  auto sorted = std::vector<ViewLight>{};
  sorted.reserve(lights.size());
  for (const auto index : engineViewLightsOrder(areas))
  {
    sorted.push_back(std::move(lights[index]));
  }
  lights = std::move(sorted);
}

std::vector<const LitSurface*> stagePassOrder(
  const std::vector<LitSurface>& drawSurfs, const std::vector<LitSpace>& spaces)
{
  // SS_POST_PROCESS (Material.h:319)
  constexpr auto SortPostProcess = 100.0f;
  struct Keyed
  {
    const LitSurface* surface = nullptr;
    std::uint32_t sortBits = 0;
    std::uint16_t dist = 0;
  };
  auto keyed = std::vector<Keyed>{};
  keyed.reserve(drawSurfs.size());
  for (const auto& surface : drawSurfs)
  {
    // float sort = SS_POST_PROCESS - drawSurfs[i]->sort, its bits as the key's high dword
    const auto sort =
      SortPostProcess - (surface.material ? surface.material->sort : 0.0f);
    auto sortBits = std::uint32_t{0};
    std::memcpy(&sortBits, &sort, sizeof(sortBits));
    // the near depth of the bounds through the space's mvp, as Ftoui16( min * 0xFFFF )
    const auto depth =
      engineDepthBoundsForBounds(spaces[surface.space].mvp, surface.chunk->bounds);
    const auto dist = engineFtoui16(depth[0] * float(0xFFFF));
    keyed.push_back(Keyed{&surface, sortBits, dist});
  }
  // the engine sorts its unique keys descending: the high dword's bits larger first, then
  // the depth larger first, then the earlier surface first
  std::ranges::sort(keyed, [](const Keyed& lhs, const Keyed& rhs) {
    if (lhs.sortBits != rhs.sortBits)
    {
      return lhs.sortBits > rhs.sortBits;
    }
    if (lhs.dist != rhs.dist)
    {
      return lhs.dist > rhs.dist;
    }
    return lhs.surface->chunk->key < rhs.surface->chunk->key;
  });
  auto order = std::vector<const LitSurface*>{};
  order.reserve(keyed.size());
  for (const auto& entry : keyed)
  {
    order.push_back(entry.surface);
  }
  return order;
}

FrontendLists buildFrontendLists(const FrontendInputs& inputs)
{
  auto lists = FrontendLists{};
  if (!inputs.chunks)
  {
    return lists;
  }
  const auto& view = inputs.view;
  // viewDef->scissor
  const auto viewRect = inputs.scissorForTesting.value_or(viewScissor(view));

  // the state of a chunk's entity; null: worldspawn's
  const auto stateOf = [&](const PreviewId entity) -> const ChunkEntityState* {
    if (inputs.entities)
    {
      if (const auto it = inputs.entities->find(entity); it != inputs.entities->end())
      {
        return &it->second;
      }
    }
    return nullptr;
  };

  // the spaces (viewEntity_t) of the chunks' entities, in the order first seen
  auto spaceOf = std::unordered_map<PreviewId, std::size_t>{};
  const auto space = [&](const PreviewId entity) -> std::optional<std::size_t> {
    if (const auto it = spaceOf.find(entity); it != spaceOf.end())
    {
      return it->second;
    }
    const auto* state = stateOf(entity);
    if (state && !state->visible)
    {
      return std::nullopt;
    }
    const auto index = lists.spaces.size();
    lists.spaces.push_back(
      state
        ? litSpaceOf(
            view, entity, state->modelOrigin, state->parms.axis, state->parms.shaderParms)
        : litSpaceOf(
            view, entity, EngineVec3{0, 0, 0}, IdentityAxis, engineWorldShaderParms()));
    spaceOf.emplace(entity, index);
    return index;
  };

  // the light records in id order (M-FRONT-DIFF-01)
  auto records = std::vector<std::shared_ptr<const LightRecord>>{};
  if (inputs.lights)
  {
    inputs.lights->records.forEach(
      [&](const PreviewId&, const std::shared_ptr<const LightRecord>& record) {
        if (record)
        {
          records.push_back(record);
        }
      });
    std::ranges::sort(
      records, [](const auto& lhs, const auto& rhs) { return lhs->id < rhs->id; });
  }

  // LP4.1 (C32, D79): FindViewLightsAndEntities first, over the lights and the brush
  // entities that reference an area; without a tree nothing is flowed and LP2's rules
  // stand (D94)
  const auto hasTree = inputs.tree && !inputs.tree->nodes.empty() && inputs.areas
                       && !inputs.areas->areas.empty();
  auto flowLights = std::vector<ViewFlowLight>{};
  auto flowEntities = std::vector<ViewFlowEntity>{};
  if (hasTree)
  {
    for (const auto& record : records)
    {
      if (record->visible && record->derived.hasVolume && !record->derived.areas.empty())
      {
        flowLights.push_back(ViewFlowLight{
          record->id,
          record->derived.areaNum,
          record->derived.areas,
          record->derived.projection.inverseBaseLightProject,
          lightCastsShadows(*record).value_or(false)});
      }
    }
    if (inputs.entities)
    {
      for (const auto& [id, state] : *inputs.entities)
      {
        if (state.visible && !state.areas.empty())
        {
          flowEntities.push_back(
            ViewFlowEntity{id, state.areas, state.inverseBaseModelProject});
        }
      }
    }
  }
  // LP5.3 (C49, D123, P6.6): the fog portals' densities, from the assigned fog lights'
  // registers, evaluated before the flow as PortalIsFoggedOut evaluates them in it
  auto portalFog = std::vector<ViewFlowPortalFog>{};
  if (hasTree && inputs.portalFog && inputs.portalFog->areas == inputs.areas)
  {
    const auto& fogLights = inputs.portalFog->fogLights;
    portalFog.resize(fogLights.size());
    for (std::size_t portal = 0; portal < fogLights.size(); ++portal)
    {
      if (fogLights[portal] == NoPreviewId || !inputs.lights || !inputs.registers)
      {
        continue;
      }
      const auto record = inputs.lights->find(fogLights[portal]);
      if (!record || !record->derived.shader.material)
      {
        continue;
      }
      const auto& shader = *record->derived.shader.material;
      const auto answer =
        inputs.registers(record->id, shader, record->light.parms.shaderParms);
      if (!answer.registers)
      {
        // first-seen: the portal is left open, the frame unconverged
        portalFog[portal].pending = true;
        ++lists.portalFogPending;
        lists.pending = true;
        continue;
      }
      if (!answer.current)
      {
        lists.pending = true;
      }
      if (shader.stages.empty())
      {
        continue;
      }
      // find the current density of the fog: stage 0's alpha
      const auto alpha = engineRegister(
        answer.registers->registers, shader.stages.front().colorRegisters[3]);
      portalFog[portal].assigned = true;
      portalFog[portal].a = engineFogDensity(alpha);
    }
  }

  auto flowInputs = ViewFlowInputs{};
  flowInputs.view = view;
  flowInputs.portalFog = portalFog;
  flowInputs.tree = hasTree ? inputs.tree : nullptr;
  flowInputs.areas = hasTree ? inputs.areas : nullptr;
  flowInputs.lights = flowLights;
  flowInputs.entities = flowEntities;
  flowInputs.scissorForTesting = inputs.scissorForTesting;
  const auto flowStart = std::chrono::steady_clock::now();
  const auto flow = flowView(flowInputs);
  lists.flowMs = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - flowStart)
                   .count();
  lists.flowed = flow.flowed;
  lists.viewArea = flow.viewArea;
  lists.areasFlowed =
    std::size_t(std::count(flow.areaFlowed.begin(), flow.areaFlowed.end(), true));
  lists.portalChains = flow.chains;
  lists.portalsFogged = flow.portalsFogged;
  lists.connectedAreas = flow.connectedAreas;
  const auto numAreas = flow.flowed ? inputs.areas->areas.size() : std::size_t{0};

  // LP3.0 (D148, P3.2): each light's place in viewLights before
  // R_OptimizeViewLightsList's sort: the flow's order (the last accepted first); the
  // lights it does not hold (no tree, D94; the outside set, D81: no engine counterpart)
  // after them, in the reverse of the records' id order, as if each were prepended in id
  // order
  auto preSortRank = std::unordered_map<PreviewId, std::size_t>{};
  for (const auto id : flow.lightOrder)
  {
    preSortRank.emplace(id, preSortRank.size());
  }
  for (auto i = records.size(); i-- > 0;)
  {
    preSortRank.emplace(records[i]->id, preSortRank.size());
  }
  // the sort's input: per light still in viewLights its rank, its area after the shrink,
  // and its place in lists.lights (none: no surface, skipped after the sort)
  struct SortInput
  {
    std::size_t rank = 0;
    int area = 0;
    std::optional<std::size_t> light;
  };
  auto sortInput = std::vector<SortInput>{};

  // LP4.3 (C36, P4.11): the grid areas need a tree and a grid; a world area's model
  // surface of a material is the union of the area's world chunks of it (the _areaN
  // model's surface, RenderWorld_load.cpp:1029-1095), as an entity's materialBounds are
  const auto hasGrid =
    flow.flowed && inputs.grid && !inputs.grid->areas.empty() && inputs.gridValid;
  auto worldMaterialBounds = std::map<std::pair<int, std::string>, EngineBounds>{};
  if (hasGrid)
  {
    for (const auto& [key, chunk] : *inputs.chunks)
    {
      if (
        !chunk.indexes.empty() && !stateOf(key.entity) && key.area >= 0
        && std::size_t(key.area) < numAreas)
      {
        const auto [it, added] = worldMaterialBounds.try_emplace(
          std::pair{key.area, key.material}, engineClearedBounds());
        it->second = engineAddBounds(it->second, chunk.bounds);
      }
    }
  }
  const auto gridAreaOf = [&](
                            const ChunkKey& key,
                            const LitSurface& surface,
                            const AreaSurface& areaSurface) -> std::optional<int> {
    if (!hasGrid || !surface.material || !surface.material->receivesLighting)
    {
      return std::nullopt;
    }
    const auto& modelRenderMatrix = lists.spaces[surface.space].modelRenderMatrix;
    if (areaSurface.kind == AreaSurfaceKind::WorldArea)
    {
      const auto it = worldMaterialBounds.find(std::pair{key.area, key.material});
      if (it == worldMaterialBounds.end())
      {
        return std::nullopt;
      }
      const auto bounds = surface.material->deform != em::DeformNone
                            ? engineDeformSurfaceBounds(it->second)
                            : it->second;
      const auto refs = std::array<int, 1>{key.area};
      return surfaceGridArea(
        *inputs.tree, modelRenderMatrix, bounds, refs, inputs.gridValid);
    }
    if (areaSurface.kind == AreaSurfaceKind::Entity)
    {
      const auto* state = stateOf(key.entity);
      if (!state)
      {
        return std::nullopt;
      }
      // LP7.2 (C52): an instance's model surface is its own (tri->bounds, deforms grown
      // by FinishSurfaces), tr_frontend_addmodels.cpp:812-853
      if (surface.instance)
      {
        if (state->areas.empty())
        {
          return std::nullopt;
        }
        return surfaceGridArea(
          *inputs.tree,
          modelRenderMatrix,
          surface.instance->header.bounds,
          state->areas,
          inputs.gridValid);
      }
      // LP7.2 (C53): the model surface of the faces' own material
      const auto it = state->materialBounds.find(chunkSourceMaterial(*surface.chunk));
      if (it == state->materialBounds.end() || state->areas.empty())
      {
        return std::nullopt;
      }
      return surfaceGridArea(
        *inputs.tree, modelRenderMatrix, it->second, state->areas, inputs.gridValid);
    }
    // D81: the outside set takes the probe path
    return std::nullopt;
  };

  // R_AddModels: the surfaces directly visible, each with its material and registers;
  // with a tree, those of the entities the flow saw (C33) and of the outside set (D81).
  // LP7.2 (C52, D129): the model instances after the chunks, each through its header
  // (LitSurface::chunk) beside the instance; an instance's indexes are its shared
  // surface's
  auto visible = std::vector<LitSurface>{};
  // per visible surface, what the pairing reads of it (C33)
  auto areaSurfaces = std::vector<AreaSurface>{};
  const auto consider =
    [&](const ChunkKey& key, const Chunk& chunk, const ModelInstance* instance) {
      auto areaSurface = AreaSurface{};
      if (flow.flowed)
      {
        const auto* state = stateOf(key.entity);
        if (!state)
        {
          // a worldspawn chunk belongs to the world entity of its area, referenced in
          // that area alone (RenderWorld_load.cpp:1029-1095), directly visible when its
          // rect is not empty (tr_frontend_addmodels.cpp:340; P4.29); a piece in solid is
          // the outside set (D47, K14, D81)
          if (key.area >= 0 && std::size_t(key.area) < numAreas)
          {
            areaSurface.kind = AreaSurfaceKind::WorldArea;
            areaSurface.area = key.area;
            areaSurface.directlyVisible =
              flow.worldRects[std::size_t(key.area)].has_value();
            if (!areaSurface.directlyVisible)
            {
              return;
            }
          }
          else
          {
            areaSurface.kind = AreaSurfaceKind::WorldOutside;
            areaSurface.directlyVisible = true;
          }
        }
        else
        {
          // a brush entity: its references (C31), seen when the flow gave it a rect; one
          // without references is the outside set (D81)
          areaSurface.kind = AreaSurfaceKind::Entity;
          areaSurface.refs = state->areas;
          areaSurface.directlyVisible =
            state->areas.empty() || flow.entityRects.contains(key.entity);
          if (!areaSurface.directlyVisible)
          {
            if (state->visible)
            {
              ++lists.entitiesCulled;
            }
            return;
          }
        }
      }
      const auto index = space(key.entity);
      if (!index)
      {
        return;
      }
      auto surface = LitSurface{};
      surface.chunk = &chunk;
      surface.instance = instance;
      surface.space = *index;
      // LP5.2a (P6.14): the surface's scissorRect, its view entity's
      surface.scissor = viewRect;
      if (flow.flowed && areaSurface.kind == AreaSurfaceKind::WorldArea)
      {
        surface.scissor = *flow.worldRects[std::size_t(areaSurface.area)];
      }
      else if (flow.flowed && areaSurface.kind == AreaSurfaceKind::Entity)
      {
        if (const auto rect = flow.entityRects.find(key.entity);
            rect != flow.entityRects.end())
        {
          surface.scissor = rect->second;
        }
      }
      const auto answer =
        inputs.material ? inputs.material(key.material) : MaterialAnswer{nullptr, true};
      if (!answer.entry && !answer.pending)
      {
        // the parse failed: the engine would not have gone on
        return;
      }
      surface.material = answer.entry;
      if (surface.material && inputs.registers)
      {
        const auto registers = inputs.registers(
          key.entity, *surface.material, lists.spaces[*index].shaderParms);
        surface.registers = registers.registers;
        surface.provisional = !registers.registers || !registers.current;
      }
      else
      {
        surface.provisional = true;
      }
      if (surface.provisional)
      {
        lists.pending = true;
      }

      // view frustum culling for the precise surface bounds
      // (tr_frontend_addmodels.cpp:649)
      if (engineCullBoundsToMvp(lists.spaces[*index].mvp, chunk.bounds, false))
      {
        return;
      }
      // LP4.3 (C36): R_AddSingleModel's grid area of the surface's model surface
      surface.gridArea = gridAreaOf(key, surface, areaSurface);
      visible.push_back(surface);
      areaSurfaces.push_back(areaSurface);
    };
  for (const auto& [key, chunk] : *inputs.chunks)
  {
    if (!chunk.indexes.empty())
    {
      consider(key, chunk, nullptr);
    }
  }
  if (inputs.instances)
  {
    for (const auto& [key, instance] : *inputs.instances)
    {
      // a chunk without indexes is skipped: an instance's are its shared surface's
      if (instance.geometry && instance.geometry->indexCount() > 0)
      {
        consider(key, instance.header, &instance);
      }
    }
  }

  // the prepass list: the drawn surfaces, the leading SS_SUBVIEW ones first
  for (const auto subview : {true, false})
  {
    for (const auto& surface : visible)
    {
      const auto drawn =
        !surface.material
        || (engineMaterialIsDrawn(*surface.material) && baseSurfaceDrawn(*surface.material));
      const auto isSubview =
        surface.material && surface.material->sort == em::SortSubview;
      if (drawn && isSubview == subview)
      {
        lists.drawSurfs.push_back(surface);
        lists.gridSurfaces += surface.gridArea ? 1 : 0;
        lists.dressSurfaces += surface.chunk->dress ? 1 : 0;
      }
    }
  }

  // R_AddLights, then the light half of R_AddSingleModel, per light in id order
  if (!records.empty())
  {
    for (const auto& record : records)
    {
      if (!record->visible || !record->derived.hasVolume)
      {
        continue;
      }
      const auto& shader = record->derived.shader;
      if (shader.pending)
      {
        lists.pending = true;
        continue;
      }
      // LP5.3 (C49): a fog light needs no falloff image (FogPass binds _fog and
      // _fogEnter, never vLight->falloffImage): it is drawn whether or not its shader or
      // the default light's names one (a game root without the engine's lights.mtr has no
      // default)
      if (!shader.material || (!shader.falloffImage && !shader.material->fogLight))
      {
        continue;
      }

      // LP4.1 (C33): a viewLight_t only when the flow gave the light a rect, its
      // scissorRect (RenderWorld_portals.cpp:364-402); a light that pushed into no area
      // is the outside set's, with the view's scissor (D81); without a tree the view's
      // scissor stands (D94)
      auto flowRect = viewRect;
      if (flow.flowed && !record->derived.areas.empty())
      {
        const auto rect = flow.lightRects.find(record->id);
        if (rect != flow.lightRects.end())
        {
          flowRect = rect->second;
        }
        else if (const auto empty = flow.emptyLightRects.find(record->id);
                 empty != flow.emptyLightRects.end())
        {
          // LP3.0 (D148): accepted through empty chains only: in viewLights with an empty
          // scissor (no surface; a fog or blend light kept so)
          flowRect = empty->second;
        }
        else
        {
          continue;
        }
      }

      const auto& material = *shader.material;
      // LP5.3 (C49): fog and blend lights take the path below with no snap, kept without
      // a surface or with an empty scissor (tr_frontend_addlights.cpp:155, 727-735),
      // drawn by FogAllLights. LP5.1 (C45, SC510, SC512): math and ambientLight lights
      // take it with the snap and the scissor's test, as the engine's frontend gives them
      const auto fogOrBlend = material.fogLight || material.blendLight;

      // evaluate the light shader registers; D52 (P6.6): a light seen for the first time
      // without registers is skipped (the frame unconverged), kept ones are drawn
      auto viewLight = ViewLight{};
      viewLight.record = record;
      viewLight.shader = shader.material;
      viewLight.kind = engineViewLightKind(material, record->light.parms);
      // the fog plane is the light far clip plane (:212-221)
      viewLight.fogPlane = engineFogPlane(record->derived.projection.baseLightProject);
      if (!inputs.registers)
      {
        continue;
      }
      const auto registers =
        inputs.registers(record->id, material, record->light.parms.shaderParms);
      if (!registers.registers)
      {
        lists.pending = true;
        continue;
      }
      if (!registers.current)
      {
        viewLight.provisional = true;
        lists.pending = true;
      }
      viewLight.registers = registers.registers->registers;

      // if this is a purely additive light and no stage in the light shader evaluates
      // to a positive light value, we can completely skip the light (not a fog or blend
      // light, :155)
      if (!fogOrBlend && !engineSnapLightRegisters(material, viewLight.registers))
      {
        continue;
      }

      const auto scissorAndBox = engineLightScissorAndBox(
        view, record->derived.projection.inverseBaseLightProject, flowRect);
      if (!scissorAndBox)
      {
        // the light was culled to the view frustum (:266-270): every kind
        continue;
      }
      const auto* scissor = &scissorAndBox->scissor;
      // LP3.0 (C58, D144): the shadow LOD from the projected box, for a light that casts
      // (tr_frontend_addlights.cpp:391-466; r_useLightScissors 3, no MOC: K51)
      viewLight.projected = scissorAndBox->projected;
      if (
        !EngineFrame.skipShadows && !inputs.skipShadows
        && lightCastsShadows(*record).value_or(false))
      {
        const auto lod = engineShadowLod(
          viewLight.projected,
          record->light.parms.parallel,
          EngineFrame.shadowMapLodScale,
          EngineFrame.shadowMapLodBias);
        viewLight.shadowLod = lod.lod;
        viewLight.shadowFadeOut = lod.fadeOut;
      }
      if (scissor->isEmpty() && !fogOrBlend)
      {
        // tr_frontend_addmodels.cpp:362-367: an empty scissor contacts no entity, so the
        // light has no surface; LP5.3: a fog or blend light is kept so (its cube draws).
        // LP3.0 (D148): it stays in viewLights, so in the sort, with its shrunk rect
        sortInput.push_back(SortInput{
          preSortRank.at(record->id),
          engineSortArea(*scissor, false, engineLightCastsShadows(material)),
          std::nullopt});
        continue;
      }
      viewLight.scissor = *scissor;
      ++lists.lightsInView;

      // the interactions (tr_frontend_addmodels.cpp:920-1008), with a tree only through a
      // shared, connected area or the outside set (C33 a-c, R_AddSingleLight's area walk,
      // tr_frontend_addlights.cpp:488-582, and R_AddSingleModel's contacted lights,
      // tr_frontend_addmodels.cpp:340-381)
      const auto& projection = record->derived.projection;
      const auto areaLight = AreaLight{record->derived.areaNum, record->derived.areas};
      // an empty-scissor light (a fog or blend light here) contacts no entity
      // (tr_frontend_addmodels.cpp:362-367)
      const auto contacts = !viewLight.scissor.isEmpty();
      for (auto i = std::size_t{0}; i < visible.size() && contacts; ++i)
      {
        const auto& surface = visible[i];
        if (
          flow.flowed && !lightReachesSurface(areaLight, areaSurfaces[i], *inputs.areas))
        {
          continue;
        }
        const auto receives = !surface.material || surface.material->receivesLighting;
        if (!receives)
        {
          continue;
        }
        if (engineCullModelBoundsToLight(
              projection.baseLightProject,
              surface.chunk->bounds,
              lists.spaces[surface.space].modelRenderMatrix))
        {
          continue;
        }
        // "invisible ink" lights and shaders
        const auto spectrum = surface.material ? surface.material->spectrum : 0;
        if (spectrum != material.spectrum)
        {
          continue;
        }
        // DX.2: the static interaction's light triangles (GenerateAllInteractions makes
        // one for every static-model entity at load: the world's areas, brush entities
        // and patches)
        auto lit = surface;
        if (inputs.lightTriangles)
        {
          const auto triangles =
            inputs.lightTriangles(*record, surface, lists.spaces[surface.space]);
          if (triangles)
          {
            switch (triangles->state)
            {
            case LightTriangles::State::None:
              lists.culledTriangles += triangles->culledTriangles;
              continue;
            case LightTriangles::State::Pending:
              lists.lightTrianglesPending = true;
              break;
            case LightTriangles::State::Runs:
              lit.lightTriangles = triangles;
              ++lists.lightTriangleSurfaces;
              lists.culledTriangles += triangles->culledTriangles;
              break;
            case LightTriangles::State::All:
              break;
            }
          }
        }
        const auto list =
          surface.material
            ? engineInteractionList(*surface.material, engineLightNoShadows(*record))
            : InteractionList::Global;
        switch (list)
        {
        case InteractionList::Local:
          viewLight.localInteractions.push_back(std::move(lit));
          break;
        case InteractionList::Global:
          viewLight.globalInteractions.push_back(std::move(lit));
          break;
        case InteractionList::Translucent:
          viewLight.translucentInteractions.push_back(std::move(lit));
          break;
        }
      }

      // R_OptimizeViewLightsList: no lit surface, nothing to draw (:727-735); LP5.3: a
      // fog or blend light still draws its volume in FogAllLights, and stays
      const auto hasSurfaces = !viewLight.localInteractions.empty()
                               || !viewLight.globalInteractions.empty()
                               || !viewLight.translucentInteractions.empty();
      const auto sortArea =
        engineSortArea(viewLight.scissor, hasSurfaces, engineLightCastsShadows(material));
      if (!hasSurfaces && !fogOrBlend)
      {
        // LP3.0 (D148, P3.2): still in viewLights at the sort, with its emptied rect
        // (FAL:720-737, 750-791), skipped after it
        sortInput.push_back(
          SortInput{preSortRank.at(record->id), sortArea, std::nullopt});
        continue;
      }
      // LP5.1 (C45): tr.pc.c_mathViewLights, R_LightUsesMathPath of the lights it counts
      // in c_viewLights (tr_frontend_addlights.cpp:738-745: a fog or blend light's too),
      // and the ambientLight lights with surfaces (the preview's own count); LP5.3: the
      // fog and blend lights kept
      if (engineLightUsesMathPath(record->light.parms))
      {
        ++lists.mathLights;
      }
      if (material.ambientLight && !fogOrBlend)
      {
        ++lists.ambientLights;
      }
      if (fogOrBlend)
      {
        ++lists.fogAndBlendLights;
      }
      sortInput.push_back(
        SortInput{preSortRank.at(record->id), sortArea, lists.lights.size()});
      lists.lights.push_back(std::move(viewLight));
    }
  }

  // R_OptimizeViewLightsList (LP3.0, D148): the sort over viewLights in their order, then
  // the lights without a surface skipped
  std::ranges::sort(
    sortInput, [](const auto& lhs, const auto& rhs) { return lhs.rank < rhs.rank; });
  auto areas = std::vector<int>{};
  areas.reserve(sortInput.size());
  for (const auto& input : sortInput)
  {
    areas.push_back(input.area);
  }
  auto sorted = std::vector<ViewLight>{};
  sorted.reserve(lists.lights.size());
  for (const auto index : engineViewLightsOrder(areas))
  {
    if (const auto light = sortInput[index].light)
    {
      sorted.push_back(std::move(lists.lights[*light]));
    }
  }
  lists.lights = std::move(sorted);

  // LP3.0 (C57, P3.14, P3.16, P3.17): the casters of every light that will take tiles
  // (the atlas pass's loop 1 skips fog and blend lights, lights without an interaction
  // and LOD -1, RenderBackend.cpp:3653-3674), requested here, per casting light, as the
  // lists are made: R_AddSingleLight's area walk from the light's own area
  // (lightReachesCaster's rule), R_AddSingleModel's shadow surface rule (the cast class,
  // the light volume's cull, the spectrum, the light triangles), on screen or off. The
  // candidates (every chunk and instance of a cast class) and what does not depend on the
  // light (their areas, their space, their material) are gathered once per build
  const auto casterStart = std::chrono::steady_clock::now();
  const auto anyCaster = std::ranges::any_of(lists.lights, [](const ViewLight& light) {
    return light.shadowLod >= 0 && light.shader;
  });
  struct CasterCandidate
  {
    const ChunkKey* key = nullptr;
    const Chunk* chunk = nullptr;
    const ModelInstance* instance = nullptr;
    AreaSurface areaSurface;
    // made on first use: the space (none: an invisible entity) and the material
    bool resolved = false;
    std::optional<std::size_t> space;
    std::shared_ptr<const MaterialEntry> material;
  };
  auto candidates = std::vector<CasterCandidate>{};
  if (anyCaster)
  {
    const auto add =
      [&](const ChunkKey& key, const Chunk& chunk, const ModelInstance* instance) {
        if (chunk.castClass == CastClass::None)
        {
          return;
        }
        auto candidate = CasterCandidate{};
        candidate.key = &key;
        candidate.chunk = &chunk;
        candidate.instance = instance;
        if (const auto* state = stateOf(key.entity))
        {
          candidate.areaSurface.kind = AreaSurfaceKind::Entity;
          candidate.areaSurface.refs = state->areas;
        }
        else if (key.area >= 0 && std::size_t(key.area) < numAreas)
        {
          candidate.areaSurface.kind = AreaSurfaceKind::WorldArea;
          candidate.areaSurface.area = key.area;
        }
        else
        {
          candidate.areaSurface.kind = AreaSurfaceKind::WorldOutside;
        }
        candidates.push_back(std::move(candidate));
      };
    for (const auto& [key, chunk] : *inputs.chunks)
    {
      if (!chunk.indexes.empty())
      {
        add(key, chunk, nullptr);
      }
    }
    if (inputs.instances)
    {
      for (const auto& [key, instance] : *inputs.instances)
      {
        if (instance.geometry && instance.geometry->indexCount() > 0)
        {
          add(key, instance.header, &instance);
        }
      }
    }
  }
  for (auto& viewLight : lists.lights)
  {
    if (viewLight.shadowLod < 0 || !viewLight.shader)
    {
      continue;
    }
    const auto& record = *viewLight.record;
    const auto& projection = record.derived.projection;
    const auto areaNum = record.derived.areaNum;
    const auto spectrum = viewLight.shader->spectrum;
    ++lists.casterLights;
    // lightReachesCaster's rule per area, once per light: an area the light references,
    // connected to the light's own area unless that is -1 (FAL:488-501)
    auto reaches = std::vector<char>(numAreas, 0);
    for (const auto area : record.derived.areas)
    {
      if (
        area >= 0 && std::size_t(area) < numAreas
        && (areaNum == -1 || areasConnected(inputs.areas->components, areaNum, area)))
      {
        reaches[std::size_t(area)] = 1;
      }
    }
    const auto reachesArea = [&](const int area) {
      return area >= 0 && std::size_t(area) < numAreas && reaches[std::size_t(area)] != 0;
    };
    const auto hasReferences = !record.derived.areas.empty();
    for (auto& candidate : candidates)
    {
      if (flow.flowed)
      {
        // D81: the outside set by volume, for every light; a light without references
        // casts only on it
        const auto& areaSurface = candidate.areaSurface;
        const auto outside =
          areaSurface.kind == AreaSurfaceKind::WorldOutside
          || (areaSurface.kind == AreaSurfaceKind::Entity && areaSurface.refs.empty());
        if (!outside)
        {
          if (!hasReferences)
          {
            continue;
          }
          const auto reached = areaSurface.kind == AreaSurfaceKind::WorldArea
                                 ? reachesArea(areaSurface.area)
                                 : std::ranges::any_of(areaSurface.refs, reachesArea);
          if (!reached)
          {
            continue;
          }
        }
      }
      if (!candidate.resolved)
      {
        candidate.resolved = true;
        candidate.space = space(candidate.key->entity);
        if (candidate.space && inputs.material)
        {
          candidate.material = inputs.material(candidate.key->material).entry;
        }
      }
      if (!candidate.space)
      {
        continue;
      }
      const auto& litSpace = lists.spaces[*candidate.space];
      if (engineCullModelBoundsToLight(
            projection.baseLightProject,
            candidate.chunk->bounds,
            litSpace.modelRenderMatrix))
      {
        continue;
      }
      // "invisible ink" lights and shaders (FAM:935-938)
      if (candidate.material && candidate.material->spectrum != spectrum)
      {
        continue;
      }
      auto caster = LitSurface{};
      caster.chunk = candidate.chunk;
      caster.instance = candidate.instance;
      caster.space = *candidate.space;
      caster.material = candidate.material;
      ++lists.casterPairs;
      const auto triangles = inputs.lightTriangles
                               ? inputs.lightTriangles(record, caster, litSpace)
                               : std::shared_ptr<const LightTriangles>{};
      if (triangles && triangles->state == LightTriangles::State::None)
      {
        // no triangle of the surface reaches the light: no shadow surface (FAM:1089)
        ++lists.casterPairsResident;
        continue;
      }
      if (triangles && triangles->state == LightTriangles::State::Pending)
      {
        ++lists.casterPairsPending;
      }
      else
      {
        ++lists.casterPairsResident;
      }
      if (triangles && triangles->state != LightTriangles::State::All)
      {
        caster.lightTriangles = triangles;
      }
      viewLight.casters.push_back(std::move(caster));
      ++lists.casters;
    }
  }
  lists.casterMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - casterStart)
                     .count();
  return lists;
}

std::size_t surfaceIndexCount(const LitSurface& surface)
{
  if (surface.instance)
  {
    return surface.instance->geometry ? surface.instance->geometry->indexCount() : 0;
  }
  return surface.chunk ? surface.chunk->indexes.size() : 0;
}

ListsTableKey ListsTableKey::of(const MaterialTable* table)
{
  return table ? ListsTableKey{table->registry, table->materialsRevision}
               : ListsTableKey{};
}

RegisterAnswer RegisterCache::registers(
  const PreviewId consumer,
  const MaterialEntry& material,
  const ShaderParms& shaderParms,
  const MaterialTable& table,
  MaterialService* service)
{
  const auto id = MaterialId::of(material);
  auto found = table.findRegisters(RegisterKey{id, shaderParms, 0.0f});
  if (!found && service)
  {
    // queued ahead of the lookups and the bulk parse (D52)
    found = service->evaluate(id, shaderParms, 0.0f);
  }
  auto& kept = m_kept[{consumer, material.name}];
  kept.used = true;
  if (
    found && found->error.empty()
    && found->registers.size() >= std::size_t(std::max(material.numRegisters, 0)))
  {
    kept.registers = found;
    return RegisterAnswer{std::move(found), true};
  }
  // D52: the last registers until the new ones arrive, never the defaults; LPF.0b (C43,
  // P5.27): across a re-parse only while they cover the material's registers now, so a
  // keystroke does not blink the surface and a grown expression set is never read past
  if (
    kept.registers
    && kept.registers->registers.size() < std::size_t(std::max(material.numRegisters, 0)))
  {
    kept.registers.reset();
  }
  return RegisterAnswer{kept.registers, false};
}

void RegisterCache::beginBuild()
{
  for (auto& [key, kept] : m_kept)
  {
    kept.used = false;
  }
}

void RegisterCache::endBuild()
{
  std::erase_if(m_kept, [](const auto& entry) { return !entry.second.used; });
}

std::size_t RegisterCache::size() const
{
  return m_kept.size();
}

void RegisterCache::clear()
{
  m_kept.clear();
}

} // namespace tb::lit
