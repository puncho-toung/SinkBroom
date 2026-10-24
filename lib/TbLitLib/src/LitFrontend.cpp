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
#include <map>
#include <string>
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
  return scissor;
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

void engineSortViewLights(std::vector<ViewLight>& lights)
{
  // qsort ascending by screen area, then each prepended: the largest first
  std::stable_sort(lights.begin(), lights.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.scissor.area() < rhs.scissor.area();
  });
  std::reverse(lights.begin(), lights.end());
}

FrontendLists buildFrontendLists(const FrontendInputs& inputs)
{
  auto lists = FrontendLists{};
  if (!inputs.chunks)
  {
    return lists;
  }
  const auto& view = inputs.view;

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
  auto flowInputs = ViewFlowInputs{};
  flowInputs.view = view;
  flowInputs.tree = hasTree ? inputs.tree : nullptr;
  flowInputs.areas = hasTree ? inputs.areas : nullptr;
  flowInputs.lights = flowLights;
  flowInputs.entities = flowEntities;
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
  lists.connectedAreas = flow.connectedAreas;
  const auto numAreas = flow.flowed ? inputs.areas->areas.size() : std::size_t{0};

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
      const auto it = state->materialBounds.find(key.material);
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
  // with a tree, those of the entities the flow saw (C33) and of the outside set (D81)
  auto visible = std::vector<LitSurface>{};
  // per visible surface, what the pairing reads of it (C33)
  auto areaSurfaces = std::vector<AreaSurface>{};
  for (const auto& [key, chunk] : *inputs.chunks)
  {
    if (chunk.indexes.empty())
    {
      continue;
    }
    auto areaSurface = AreaSurface{};
    if (flow.flowed)
    {
      const auto* state = stateOf(key.entity);
      if (!state)
      {
        // a worldspawn chunk belongs to the world entity of its area, referenced in that
        // area alone (RenderWorld_load.cpp:1029-1095), directly visible when its rect is
        // not empty (tr_frontend_addmodels.cpp:340; P4.29); a piece in solid is the
        // outside set (D47, K14, D81)
        if (key.area >= 0 && std::size_t(key.area) < numAreas)
        {
          areaSurface.kind = AreaSurfaceKind::WorldArea;
          areaSurface.area = key.area;
          areaSurface.directlyVisible =
            flow.worldRects[std::size_t(key.area)].has_value();
          if (!areaSurface.directlyVisible)
          {
            continue;
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
          continue;
        }
      }
    }
    const auto index = space(key.entity);
    if (!index)
    {
      continue;
    }
    auto surface = LitSurface{};
    surface.chunk = &chunk;
    surface.space = *index;
    const auto answer =
      inputs.material ? inputs.material(key.material) : MaterialAnswer{nullptr, true};
    if (!answer.entry && !answer.pending)
    {
      // the parse failed: the engine would not have gone on
      continue;
    }
    surface.material = answer.entry;
    if (surface.material && inputs.registers)
    {
      const auto registers =
        inputs.registers(key.entity, *surface.material, lists.spaces[*index].shaderParms);
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

    // view frustum culling for the precise surface bounds (tr_frontend_addmodels.cpp:649)
    if (engineCullBoundsToMvp(lists.spaces[*index].mvp, chunk.bounds, false))
    {
      continue;
    }
    // LP4.3 (C36): R_AddSingleModel's grid area of the surface's model surface
    surface.gridArea = gridAreaOf(key, surface, areaSurface);
    visible.push_back(surface);
    areaSurfaces.push_back(areaSurface);
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
      if (!shader.material || !shader.falloffImage)
      {
        continue;
      }

      // LP4.1 (C33): a viewLight_t only when the flow gave the light a rect, its
      // scissorRect (RenderWorld_portals.cpp:364-402); a light that pushed into no area
      // is the outside set's, with the view's scissor (D81); without a tree the view's
      // scissor stands (D94)
      auto flowRect = viewScissor(view);
      if (flow.flowed && !record->derived.areas.empty())
      {
        const auto rect = flow.lightRects.find(record->id);
        if (rect == flow.lightRects.end())
        {
          continue;
        }
        flowRect = rect->second;
      }

      const auto& material = *shader.material;
      if (material.fogLight || material.blendLight)
      {
        ++lists.fogAndBlendLights;
        continue;
      }
      if (engineLightUsesMathPath(record->light.parms))
      {
        ++lists.mathLights;
        continue;
      }
      if (material.ambientLight)
      {
        ++lists.ambientLights;
        continue;
      }

      // evaluate the light shader registers
      auto viewLight = ViewLight{};
      viewLight.record = record;
      viewLight.shader = shader.material;
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
      // to a positive light value, we can completely skip the light
      if (!engineSnapLightRegisters(material, viewLight.registers))
      {
        continue;
      }

      const auto scissor = engineLightScissor(
        view, record->derived.projection.inverseBaseLightProject, flowRect);
      if (!scissor || scissor->isEmpty())
      {
        // tr_frontend_addmodels.cpp:362-367: an empty scissor contacts no entity
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
      for (auto i = std::size_t{0}; i < visible.size(); ++i)
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
            ? engineInteractionList(*surface.material, record->light.parms.noShadows)
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

      // R_OptimizeViewLightsList: no lit surface, nothing to draw
      if (
        viewLight.localInteractions.empty() && viewLight.globalInteractions.empty()
        && viewLight.translucentInteractions.empty())
      {
        continue;
      }
      lists.lights.push_back(std::move(viewLight));
    }
  }

  engineSortViewLights(lists.lights);
  return lists;
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
  // D52: the last registers until the new ones arrive, never the defaults
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
