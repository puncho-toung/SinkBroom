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
// renderer/NVRHI/RenderBackend_NVRHI.cpp and renderer/NVRHI/RenderProgs_NVRHI.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023 Stephen Saunders
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "LitPasses.h"

#include "lit/AreaData.h"
#include "lit/EngineBuiltins.h"
#include "lit/EnginePipelineState.h"
#include "lit/EntityState.h"
#include "lit/ImageCache.h"
#include "lit/LitConfig.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>

namespace tb::lit
{
namespace
{

namespace em = engine_material;
namespace gls = engine_gls;

// the layout's push constants or volatile constant buffer: the first item of set 0
bool pushesConstants(const std::vector<nvrhi::BindingLayoutHandle>& layouts)
{
  if (layouts.empty() || !layouts.front())
  {
    return false;
  }
  const auto* desc = layouts.front()->getDesc();
  return desc && !desc->bindings.empty()
         && desc->bindings.front().type == nvrhi::ResourceType::PushConstants;
}

// rpBufSize: the layout's renderparm set in bytes
std::uint32_t parmSetBytes(const EngineBindingLayout layout)
{
  return engineParmSetBlock(engineParmSetOfLayout(layout)).size;
}

Vec4 vec4Of(const EnginePlane& plane)
{
  return Vec4{plane.a, plane.b, plane.c, plane.d};
}

constexpr auto ZeroVec4 = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
constexpr auto RowS = Vec4{1.0f, 0.0f, 0.0f, 0.0f};
constexpr auto RowT = Vec4{0.0f, 1.0f, 0.0f, 0.0f};

// C14: the passes LP2.2a declares over FrameResources
PassDesc clearPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "litClears";
  pass.writes = {
    std::string{frame_resource::CurrentRenderHdr},
    std::string{frame_resource::CurrentRenderLdr},
    std::string{frame_resource::CurrentDepth}};
  return pass;
}

PassDesc prepassDesc()
{
  auto pass = PassDesc{};
  pass.name = "depthPrepass";
  pass.writes = {std::string{frame_resource::HdrFramebuffer}};
  return pass;
}

// LP4.2a (C34): AmbientPass( false ) into hdrFBO, depth EQUAL against the prepass's
PassDesc ambientPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "ambient";
  pass.reads = {std::string{frame_resource::CurrentDepth}};
  pass.writes = {std::string{frame_resource::HdrFramebuffer}};
  return pass;
}

// RenderBackend_NVRHI.cpp:698-788: set 1 t0-t2, set 2 t3 t4 t7 t8 t9 t10 t12 t13 t14
constexpr auto AmbientUnits = std::array<AmbientUnitBinding, 12>{{
  {1, 0, 0},
  {1, 1, 1},
  {1, 2, 2},
  {2, 3, 3},
  {2, 4, 4},
  {2, 7, 7},
  {2, 8, 8},
  {2, 9, 9},
  {2, 10, 10},
  {2, 12, 12},
  {2, 13, 13},
  {2, 14, 14},
}};

// RenderBackend.cpp:1051-1067 INTERACTION_TEXUNIT_*: the units of the IBL path
constexpr auto UnitBump = std::size_t{0};
constexpr auto UnitSpecular = std::size_t{1};
constexpr auto UnitBaseColor = std::size_t{2};
constexpr auto UnitBrdfLut = std::size_t{3};
constexpr auto UnitSsao = std::size_t{4};
constexpr auto UnitAmbientCube = std::size_t{7};
constexpr auto UnitSpecularCube1 = std::size_t{8};
constexpr auto UnitDepthCube = std::size_t{12};

// LP5.2a (C46): DrawShaderPasses into hdrFBO, testing the prepass's depth
PassDesc stagePassDesc()
{
  auto pass = PassDesc{};
  pass.name = "stages";
  pass.reads = {std::string{frame_resource::CurrentDepth}};
  pass.writes = {std::string{frame_resource::HdrFramebuffer}};
  return pass;
}

// LP5.3 (C49): FogAllLights into hdrFBO, testing the prepass's depth
PassDesc fogPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "fogLights";
  pass.reads = {std::string{frame_resource::CurrentDepth}};
  pass.writes = {std::string{frame_resource::HdrFramebuffer}};
  return pass;
}

PassDesc interactionPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "interactions";
  pass.reads = {std::string{frame_resource::CurrentDepth}};
  pass.writes = {std::string{frame_resource::HdrFramebuffer}};
  return pass;
}

} // namespace

LitFrameStats LitPasses::record(const LitFrameInputs& inputs)
{
  m_stats = LitFrameStats{};
  m_stats.camera = inputs.camera;
  m_frameWrites.clear();
  m_stats.constantWriteBudget = writeBudget();
  if (
    !inputs.device || !inputs.commandList || !inputs.frame || !inputs.arenas
    || !inputs.pipelines || !inputs.images || !inputs.bindingSets || !inputs.samplers)
  {
    return m_stats;
  }

  auto frame = Frame{};
  frame.inputs = &inputs;
  const auto extent = inputs.frame->extent();
  frame.view = litViewOf(inputs.camera, extent.width, extent.height);
  frame.formats =
    FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, inputs.frame->depthFormat()};

  // SC201: ZeroUniforms and the view's parms; the frame's command list starts with no
  // state, so the draw state starts again too
  m_parms.zero();
  m_state = DrawState{};
  m_units = {};
  m_currentSpace.reset();
  m_currentScissor = viewScissor(frame.view);
  m_scissor = engineScissorRect(frame.view, m_currentScissor);
  setViewParms(m_parms, frame.view);
  recordClears(frame);

  rebuildLists(inputs, frame.view);
  m_stats.lightsVisible = m_lists.lights.size();
  m_stats.gridSurfaces = m_lists.gridSurfaces;
  m_stats.dressSurfaces = m_lists.dressSurfaces;
  // LP4.1 (C16): the flow behind the lists; its time only when it ran this frame
  m_stats.viewArea = m_lists.viewArea;
  m_stats.areasFlowed = m_lists.areasFlowed;
  m_stats.areas = m_lists.flowed && inputs.areas ? inputs.areas->areas.size() : 0;
  m_stats.lightsInView = m_lists.lightsInView;
  m_stats.portalChains = m_lists.portalChains;
  m_stats.entitiesCulled = m_lists.entitiesCulled;
  m_stats.portalsFogged = m_lists.portalsFogged;
  m_stats.portalFogPending = m_lists.portalFogPending;
  m_stats.flowMs = m_stats.listsRebuilt ? m_lists.flowMs : 0.0;
  m_stats.unconverged = m_lists.pending;
  m_stats.waitsForTable = m_lists.pending;
  // DX.2: a surface drawn whole until its light triangles are known
  m_stats.lightTrianglesPending =
    m_lists.lightTrianglesPending || m_lightTriangles.busy();
  m_stats.unconverged = m_stats.unconverged || m_stats.lightTrianglesPending;
  m_stats.culledTriangles = m_lists.culledTriangles;
  // LP3.0 (C57, P3.14): the casters' requests behind the lists (their time only when the
  // lists were built this frame)
  m_stats.casterLights = m_lists.casterLights;
  m_stats.casterPairs = m_lists.casterPairs;
  m_stats.casterPairsResident = m_lists.casterPairsResident;
  m_stats.casterPairsPending = m_lists.casterPairsPending;
  m_stats.casters = m_lists.casters;
  m_stats.casterMs = m_stats.listsRebuilt ? m_lists.casterMs : 0.0;
  m_stats.lightTriangleEntries = m_lightTriangles.size();

  // LP4.2a (C37, D79, D88): R_FindClosestEnvironmentProbes, every frame, from the probe
  // table and the slots' states: Ready once a slot binds a written level of its own,
  // Pending before (its intrinsic stand-in is 2D, never a cube: P4.28), Defaulted when
  // the service has no image for it
  auto& images = *inputs.images;
  const auto probeState = [&](const MaterialImageKey& key) {
    const auto id = images.request(key);
    const auto* slot = images.slot(id);
    if (!slot || slot->state == ImageSlots::State::Pending)
    {
      return ProbeImageState::Pending;
    }
    if (slot->state == ImageSlots::State::Default)
    {
      return ProbeImageState::Defaulted;
    }
    const auto binding = images.binding(id);
    return binding.texture && !binding.intrinsic ? ProbeImageState::Ready
                                                 : ProbeImageState::Pending;
  };
  m_viewProbes = chooseViewProbes(
    inputs.probes.get(),
    frame.view.viewOrigin,
    m_lists.viewArea,
    m_lists.connectedAreas,
    m_lists.flowed,
    probeState);
  m_stats.probeCandidates = m_viewProbes.candidates;
  m_stats.probeImagesPending = m_viewProbes.imagesPending;
  m_stats.unconverged = m_stats.unconverged || m_stats.probeImagesPending;

  // LP4.3 (C36, D88): an area with grid points whose atlas the table has not answered
  // yet is "not loaded" (the engine's LoadLightGridImages loads them all at map load,
  // RenderWorld_lightgrid.cpp:415-447): the frame is unconverged until it is Ready or
  // Default (final)
  if (inputs.grid)
  {
    for (const auto& area : inputs.grid->areas)
    {
      if (area.hasPoints)
      {
        const auto* slot = images.slot(images.request(area.atlas));
        m_stats.gridImagesPending =
          m_stats.gridImagesPending || !slot || slot->state == ImageSlots::State::Pending;
      }
    }
  }
  m_stats.unconverged = m_stats.unconverged || m_stats.gridImagesPending;

  frame.framebuffer = inputs.frame->resolve(prepassDesc())
                      | kdl::transform([](const PassResources& resources) {
                          return resources.writes.front().framebuffer;
                        })
                      | kdl::value_or(nullptr);
  if (!frame.framebuffer || (m_lists.drawSurfs.empty() && m_lists.lights.empty()))
  {
    return m_stats;
  }

  // a pass boundary
  if (inputs.stopRequested && inputs.stopRequested())
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  beginTimer(frame, inputs.prepassTimer);
  recordPrepass(frame);
  m_stats.prepassTimed = endTimer(frame, inputs.prepassTimer);
  if (inputs.afterPrepass)
  {
    inputs.afterPrepass();
  }

  if (inputs.stopRequested && inputs.stopRequested())
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  // LP4.2a (C34): AmbientPass( false ) after the prepass, before the shadow atlas and the
  // interactions (RenderBackend.cpp:6057-6063); r_skipAmbient gates only
  // DrawShaderPasses, not this pass (SC348)
  const auto ambientTarget = inputs.frame->resolve(ambientPassDesc())
                             | kdl::transform([](const PassResources& resources) {
                                 return resources.writes.front().framebuffer;
                               })
                             | kdl::value_or(nullptr);
  if (ambientTarget == frame.framebuffer)
  {
    beginTimer(frame, inputs.ambientTimer);
    recordAmbient(frame);
    m_stats.ambientTimed = endTimer(frame, inputs.ambientTimer);
    if (const auto it = m_frameWrites.find(EngineBindingLayout::AmbientLightingIbl);
        it != m_frameWrites.end())
    {
      m_stats.ambientConstantWrites = it->second;
    }
  }

  if (inputs.stopRequested && inputs.stopRequested())
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  // LP3.1 (C59, D142, SC714): pass 6, ShadowAtlasPass, after AmbientPass( false ) and
  // before DrawInteractions (RenderBackend.cpp:6062-6073), inside the shot's frame
  // (SC742: no warm-up frame); r_skipInteractions does not gate it
  {
    const auto start = std::chrono::steady_clock::now();
    beginTimer(frame, inputs.shadowTimer);
    recordShadowAtlas(frame);
    m_stats.shadowAtlasTimed = endTimer(frame, inputs.shadowTimer);
    m_stats.shadowRecordMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  }

  if (inputs.stopRequested && inputs.stopRequested())
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  // C38 (LP4.2b): r_skipInteractions 1 records no interactions (RenderBackend.cpp:4181).
  // LP5.0 (P6.16, C46): it gates DrawInteractions alone, so it skips this pass and no
  // other: the passes recorded after it (LP5.2a's stage pass, LP5.3's fog and blend
  // lights) still run, as the ambient pass before it does
  if (m_skipInteractions)
  {
    m_stats.interactionsSkipped = true;
  }
  else
  {
    const auto interactionTarget = inputs.frame->resolve(interactionPassDesc())
                                   | kdl::transform([](const PassResources& resources) {
                                       return resources.writes.front().framebuffer;
                                     })
                                   | kdl::value_or(nullptr);
    if (interactionTarget == frame.framebuffer)
    {
      beginTimer(frame, inputs.interactionTimer);
      recordInteractions(frame);
      m_stats.interactionsTimed = endTimer(frame, inputs.interactionTimer);
    }
  }

  if (m_stats.stoppedEarly || (inputs.stopRequested && inputs.stopRequested()))
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  // LP5.2a (C46): after DrawInteractions, no SSR copy (r_useSSR 0, D19), the first
  // DrawShaderPasses call unless r_skipShaderPasses (RenderBackend.cpp:6107-6125); its
  // r_skipAmbient test is the test switch (D118: the panel never sets it); the second
  // call, after the fog lights and the _currentRender copy, draws the SS_POST_PROCESS
  // surfaces this one stops at (K38), which the preview does not draw
  if (m_skipShaderPasses || EngineFrame.skipShaderPasses)
  {
    m_stats.stagesSkipped = true;
  }
  else
  {
    const auto stageTarget = inputs.frame->resolve(stagePassDesc())
                             | kdl::transform([](const PassResources& resources) {
                                 return resources.writes.front().framebuffer;
                               })
                             | kdl::value_or(nullptr);
    if (stageTarget == frame.framebuffer)
    {
      beginTimer(frame, inputs.stageTimer);
      recordStages(frame);
      m_stats.stagesTimed = endTimer(frame, inputs.stageTimer);
    }
  }

  if (m_stats.stoppedEarly || (inputs.stopRequested && inputs.stopRequested()))
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
  // LP5.3 (C49): fog and blend lights, drawn after emissive surfaces so they are properly
  // dimmed down (RenderBackend.cpp:6131-6136); r_skipAmbient does not gate them (SC530),
  // nor any test switch (the parity test shoots its fog rows lit)
  const auto fogTarget = inputs.frame->resolve(fogPassDesc())
                         | kdl::transform([](const PassResources& resources) {
                             return resources.writes.front().framebuffer;
                           })
                         | kdl::value_or(nullptr);
  if (fogTarget == frame.framebuffer)
  {
    beginTimer(frame, inputs.fogTimer);
    recordFogLights(frame);
    m_stats.fogTimed = endTimer(frame, inputs.fogTimer);
  }
  return m_stats;
}

void LitPasses::beginTimer(Frame& frame, nvrhi::ITimerQuery* timer)
{
  if (timer)
  {
    frame.inputs->commandList->beginTimerQuery(timer);
    // nvrhi rule 7: a timer query ends the render pass, so the next draw sets its state
    // again
    m_state.valid = false;
  }
}

bool LitPasses::endTimer(Frame& frame, nvrhi::ITimerQuery* timer)
{
  if (!timer)
  {
    return false;
  }
  frame.inputs->commandList->endTimerQuery(timer);
  m_state.valid = false;
  return true;
}

void LitPasses::rebuildLists(const LitFrameInputs& inputs, const LitView& view)
{
  // DX.2: a finished job's light triangles are part of this frame's lists
  m_lightTriangles.collect();
  const auto* table = inputs.materials.get();
  auto key = ListsKey{};
  key.camera = inputs.camera;
  key.width = view.width;
  key.height = view.height;
  key.chunks = inputs.chunks;
  key.chunksVersion = inputs.chunksVersion;
  key.instances = inputs.instances;
  key.entities = inputs.entities;
  key.lights = inputs.lights.get();
  key.lightsVersion = inputs.lights ? inputs.lights->version : 0;
  key.tree = inputs.procTree.get();
  key.areas = inputs.areas.get();
  key.portalFog = inputs.portalFog.get();
  // LP4.3 (D79): the grid's dispatch reads the atlas slots' states, only with a grid
  const auto hasGrid = inputs.grid && !inputs.grid->areas.empty();
  key.grid = inputs.grid.get();
  key.gridVersion = inputs.grid ? inputs.grid->version : 0;
  // the areas whose grid is valid (points, an atlas Ready or Loading), as an FNV-1a hash
  // of their flags, worked out again only when a slot's state or the table moved: any
  // other slot's churn (a texture arriving, a re-request) leaves the lists alone
  if (hasGrid && inputs.images)
  {
    const auto states = inputs.images->stateVersion();
    if (states != m_gridValiditySlotStates || inputs.grid.get() != m_gridValidityTable)
    {
      m_gridValiditySlotStates = states;
      m_gridValidityTable = inputs.grid.get();
      auto hash = std::uint64_t{14695981039346656037ull};
      for (const auto& area : inputs.grid->areas)
      {
        const auto id = area.hasPoints ? inputs.images->find(area.atlas) : std::nullopt;
        const auto* slot = id ? inputs.images->slot(*id) : nullptr;
        // LPF.1 (C40): a Replacing atlas binds its complete texture
        const auto valid =
          slot
          && (slot->state == ImageSlots::State::Resident || slot->state == ImageSlots::State::Loading || slot->state == ImageSlots::State::Replacing);
        hash = (hash ^ (valid ? 2u : 1u)) * 1099511628211ull;
      }
      m_gridValidity = hash;
    }
    key.gridValidity = m_gridValidity;
  }
  key.table = ListsTableKey::of(table);
  key.lightTrianglesVersion = m_lightTriangles.version();
  key.lightTriangleCull = m_lightTriangleCull;
  key.skipShadows = m_skipShadows;
  const auto tableVersion = table ? table->version : 0;
  if (m_listsKey == key && !(m_lists.pending && tableVersion != m_listsTableVersion))
  {
    return;
  }
  // LPF.1 (C43): built again for a new registry or a re-parse (the key's table part)
  m_stats.listsTableChanged = !m_listsKey || m_listsKey->table != key.table;
  m_listsKey = key;
  m_listsTableVersion = tableVersion;
  m_stats.listsRebuilt = true;

  if (!table)
  {
    // no material service: nothing to draw, and nothing to wait for
    m_lists = FrontendLists{};
    m_stageOrder.clear();
    return;
  }

  auto frontend = FrontendInputs{};
  frontend.view = view;
  frontend.chunks = inputs.chunks;
  frontend.instances = inputs.instances;
  frontend.entities = inputs.entities;
  frontend.lights = inputs.lights.get();
  frontend.tree = inputs.procTree.get();
  frontend.areas = inputs.areas.get();
  frontend.portalFog = inputs.portalFog.get();
  // LP3.1 (C57): r_skipShadows 1's stand-in
  frontend.skipShadows = m_skipShadows;
  // LP4.3 (C36): an area's grid is valid when it has points and its atlas slot holds an
  // image (Resident, or Loading: its texture is made and written as the frame uploads);
  // Pending is not loaded yet, Default the engine's defaulted image (D88)
  if (hasGrid && inputs.images)
  {
    frontend.grid = inputs.grid.get();
    frontend.gridValid = [&](const int area) {
      const auto* grid = inputs.grid->find(area);
      if (!grid || !grid->hasPoints)
      {
        return false;
      }
      const auto id = inputs.images->find(grid->atlas);
      const auto* slot = id ? inputs.images->slot(*id) : nullptr;
      return slot
             && (slot->state == ImageSlots::State::Resident || slot->state == ImageSlots::State::Loading || slot->state == ImageSlots::State::Replacing);
    };
  }
  frontend.material = [&](const std::string& name) {
    if (const auto slot = table->find(name))
    {
      return MaterialAnswer{slot->entry, false};
    }
    if (inputs.service)
    {
      inputs.service->lookup(name);
    }
    return MaterialAnswer{nullptr, true};
  };
  frontend.registers = [&](
                         const PreviewId consumer,
                         const MaterialEntry& material,
                         const ShaderParms& shaderParms) {
    return m_registers.registers(consumer, material, shaderParms, *table, inputs.service);
  };
  if (m_lightTriangleCull)
  {
    frontend.lightTriangles =
      [&](const LightRecord& record, const LitSurface& surface, const LitSpace& space) {
        // LP7.2 (D129, A27): an instance's from its shared surface
        return surface.instance ? m_lightTriangles.triangles(
                                    record.id,
                                    record.version,
                                    record.derived.projection.baseLightProject,
                                    *surface.instance,
                                    space.modelMatrix)
                                : m_lightTriangles.triangles(
                                    record.id,
                                    record.version,
                                    record.derived.projection.baseLightProject,
                                    *surface.chunk,
                                    space.modelMatrix);
      };
  }
  m_registers.beginBuild();
  m_lightTriangles.beginBuild();
  m_lists = buildFrontendLists(frontend);
  m_registers.endBuild();
  m_lightTriangles.endBuild(inputs.pool);
  // LP5.2a (D115): R_SortDrawSurfs' order of the new lists, kept with them
  m_stageOrder = stagePassOrder(m_lists.drawSurfs, m_lists.spaces);
}

bool LitPasses::collectLightTriangles()
{
  return m_lightTriangles.collect();
}

bool LitPasses::lightTrianglesBusy() const
{
  return m_lightTriangles.busy();
}

void LitPasses::recordClears(Frame& frame)
{
  // SetBuffer with r_clear 2 (RenderBackend_NVRHI.cpp:2420-2469, SC202): hdrFBO's and
  // ldrFBO's colour to (0, 0, 0, 1); DrawViewInternal's GL_Clear (RenderBackend.cpp:
  // 5926-5931): depth 1 and stencil STENCIL_SHADOW_TEST_VALUE
  auto& commandList = *frame.inputs->commandList;
  frame.inputs->frame->resolve(clearPassDesc())
    | kdl::transform([&](const PassResources& resources) {
        const auto black = nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f};
        commandList.clearTextureFloat(
          resources.writes[0].texture, nvrhi::AllSubresources, black);
        commandList.clearTextureFloat(
          resources.writes[1].texture, nvrhi::AllSubresources, black);
        commandList.clearDepthStencilTexture(
          resources.writes[2].texture,
          nvrhi::AllSubresources,
          true,
          1.0f,
          true,
          EngineFrame.stencilClear);
      })
    | kdl::ignore();
}

void LitPasses::setSpace(Frame&, const std::size_t space)
{
  // change the matrix if needed: RB_SetMVP( space->mvp )
  if (m_currentSpace != space)
  {
    setMvp(m_parms, m_lists.spaces[space].mvp);
    m_currentSpace = space;
  }
}

void LitPasses::recordPrepass(Frame& frame)
{
  // FillDepthBufferFast (RenderBackend.cpp:934-1041): GLS_DEFAULT, the MVP on a space
  // change, no polygon offset
  m_currentSpace.reset();
  const auto& surfaces = m_lists.drawSurfs;

  // the subview surfaces, at the start of the sorted list, through the generic path
  auto i = std::size_t{0};
  for (; i < surfaces.size(); ++i)
  {
    const auto& surface = surfaces[i];
    if (!surface.material || surface.material->sort != em::SortSubview)
    {
      break;
    }
    if (!genericDepth(frame, {&surface}))
    {
      return;
    }
  }

  // the opaque surfaces now, the perforated ones after them
  auto perforated = std::vector<const LitSurface*>{};
  for (; i < surfaces.size(); ++i)
  {
    const auto& surface = surfaces[i];
    if (surface.material)
    {
      // translucent surfaces don't put anything in the depth buffer
      if (surface.material->coverage == em::CoverageTranslucent)
      {
        continue;
      }
      if (surface.material->coverage == em::CoveragePerforated)
      {
        perforated.push_back(&surface);
        continue;
      }
    }
    setSpace(frame, surface.space);
    const auto outcome =
      draw(frame, EngineBuiltinDepth, *surface.chunk, gls::Default, 0.0f, 0.0f);
    if (outcome == DrawOutcome::Disabled)
    {
      m_stats.prepassDisabled = true;
      return;
    }
    if (outcome == DrawOutcome::Overflowed)
    {
      drawOverflowed(frame);
    }
    if (outcome == DrawOutcome::Drawn)
    {
      ++m_stats.prepassDraws;
      log(frame, LitDrawRecord::Pass::Prepass, *surface.chunk, EngineBuiltinDepth, 0);
    }
  }

  if (!perforated.empty())
  {
    genericDepth(frame, perforated);
  }
}

bool LitPasses::genericDepth(Frame& frame, const std::vector<const LitSurface*>& surfaces)
{
  // FillDepthBufferGeneric (RenderBackend.cpp:706-914)
  for (const auto* surface : surfaces)
  {
    auto draws = std::vector<PrepassDraw>{};
    if (surface->material && surface->registers)
    {
      draws = engineGenericDepthDraws(*surface->material, surface->registers->registers);
    }
    else if (!surface->material || surface->material->coverage != em::CoverageTranslucent)
    {
      // D77: registers pending: the surface is drawn solid until they arrive
      auto solid = PrepassDraw{};
      solid.program = PrepassProgram::Depth;
      solid.state = gls::AlphaMask;
      draws.push_back(solid);
    }
    if (draws.empty())
    {
      continue;
    }

    setSpace(frame, surface->space);
    for (const auto& prepassDraw : draws)
    {
      if (prepassDraw.color)
      {
        // GL_Color
        m_parms.set(RenderParm::Color, *prepassDraw.color);
      }
      if (prepassDraw.alphaTest)
      {
        m_parms.set(RenderParm::AlphaTest, *prepassDraw.alphaTest);
      }

      auto program = EngineBuiltinDepth;
      switch (prepassDraw.program)
      {
      case PrepassProgram::TextureColor:
      case PrepassProgram::Environment:
      case PrepassProgram::BumpyEnvironment:
      case PrepassProgram::SkyBox:
      case PrepassProgram::WobbleSky: {
        // :832-850: BindShader_TextureVertexColor, SVC_IGNORE, unit 0 the stage's image;
        // LP5.2b (C48, SC528): PrepareStageTexturing then replaces the program for the
        // cube and sky texgens (ALPHA_TEST stays written and unread: the key names the
        // replacing program, which never clips), so their stage writes the whole
        // surface's depth; LOCALVIEWORIGIN and MODELMATRIX are what the frame's
        // ZeroUniforms left (0: no space set them before the prepass), as the engine's
        program = EngineBuiltinTextureVertexColor;
        const auto vertexColor = engineVertexColorParms(em::VertexColorIgnore);
        m_parms.set(RenderParm::VertexColorModulate, vertexColor[0]);
        m_parms.set(RenderParm::VertexColorAdd, vertexColor[1]);
        // bind the texture: unit 0 is the stage's image
        const auto& stage = surface->material->stages[std::size_t(prepassDraw.stage)];
        if (prepassDraw.program == PrepassProgram::TextureColor)
        {
          if (!stage.image)
          {
            continue;
          }
          m_units[0] = *stage.image;
        }
        else
        {
          program =
            prepassDraw.program == PrepassProgram::Environment ? EngineBuiltinEnvironment
            : prepassDraw.program == PrepassProgram::BumpyEnvironment
              ? EngineBuiltinBumpyEnvironment
            : prepassDraw.program == PrepassProgram::SkyBox ? EngineBuiltinSkyBox
                                                            : EngineBuiltinWobbleSky;
          // the cube by its slot's state (the zero cube while not written:
          // M-STAGE-DIFF-04); the frame unconverged until it is
          auto& images = *frame.inputs->images;
          const auto [cube, pendingCube] = cubeImage(images, stage.image);
          m_units[0] = cube;
          if (pendingCube)
          {
            m_stats.unconverged = true;
            m_stats.retrigger = true;
          }
          if (prepassDraw.program == PrepassProgram::BumpyEnvironment)
          {
            // :366-368: the bump stage's image raw at unit 1 (_flat for a Default one,
            // M-STAGE-DIFF-06)
            const auto& bump =
              surface->material->stages[std::size_t(prepassDraw.bumpStage)];
            m_units[1] = bumpImage(images, bump.image);
            if (!m_units[1])
            {
              ++m_stats.skippedDraws;
              m_stats.unconverged = true;
              m_stats.retrigger = true;
              continue;
            }
          }
          if (prepassDraw.program == PrepassProgram::WobbleSky)
          {
            m_parms.set(RenderParm::WobbleSkyX, prepassDraw.wobbleSky[0]);
            m_parms.set(RenderParm::WobbleSkyY, prepassDraw.wobbleSky[1]);
            m_parms.set(RenderParm::WobbleSkyZ, prepassDraw.wobbleSky[2]);
          }
        }
        // PrepareStageTexturing: the texture matrix and TEXGEN_0_ENABLED
        m_parms.set(RenderParm::TextureMatrixS, prepassDraw.textureMatrix[0]);
        m_parms.set(RenderParm::TextureMatrixT, prepassDraw.textureMatrix[1]);
        if (prepassDraw.screenTexgen)
        {
          const auto& space = m_lists.spaces[surface->space];
          const auto mat =
            engineGlMatrixMultiply(space.modelViewMatrix, frame.view.projectionMatrix);
          m_parms.set(
            RenderParm::TexGen0S,
            Vec4{mat[0 * 4 + 0], mat[1 * 4 + 0], mat[2 * 4 + 0], mat[3 * 4 + 0]});
          m_parms.set(
            RenderParm::TexGen0T,
            Vec4{mat[0 * 4 + 1], mat[1 * 4 + 1], mat[2 * 4 + 1], mat[3 * 4 + 1]});
          m_parms.set(
            RenderParm::TexGen0Q,
            Vec4{mat[0 * 4 + 3], mat[1 * 4 + 3], mat[2 * 4 + 3], mat[3 * 4 + 3]});
        }
        m_parms.set(RenderParm::TexGen0Enabled, prepassDraw.texGenEnabled);
        break;
      }
      case PrepassProgram::Color:
        program = EngineBuiltinColor;
        break;
      case PrepassProgram::Depth:
        program = EngineBuiltinDepth;
        break;
      }

      const auto outcome = draw(
        frame,
        program,
        *surface->chunk,
        prepassDraw.state,
        prepassDraw.slopeBias,
        prepassDraw.depthBias);
      if (outcome == DrawOutcome::Disabled)
      {
        m_stats.prepassDisabled = true;
        return false;
      }
      if (outcome == DrawOutcome::Overflowed)
      {
        drawOverflowed(frame);
      }
      if (outcome == DrawOutcome::Drawn)
      {
        ++m_stats.prepassDraws;
        log(
          frame,
          LitDrawRecord::Pass::Prepass,
          *surface->chunk,
          program,
          prepassDraw.state);
      }
    }
  }

  m_parms.set(RenderParm::AlphaTest, ZeroVec4);
  return true;
}

std::uint64_t LitPasses::writeBudget() const
{
  // the versions of the frames in flight and of the one before them may still be held
  // (nvrhi rule 2: they are recycled once NVRHI knows a frame finished)
  return m_constantBufferVersions / (FramesInFlight + 1);
}

void LitPasses::drawOverflowed(Frame& frame)
{
  // U2.11: the surface has no depth (or no light) where the engine draws it, so the frame
  // is not the engine's picture: unconverged, and said once, as an overflowed light is
  ++m_stats.overflowedDraws;
  m_stats.unconverged = true;
  if (!m_drawOverflowWarned && frame.inputs->warn)
  {
    m_drawOverflowWarned = true;
    frame.inputs->warn(fmt::format(
      "The lit preview's draws overflowed its constant buffer ({} writes a frame): some "
      "surfaces are not drawn",
      writeBudget()));
  }
}

std::uint64_t LitPasses::worstCaseWrites(const ViewLight& light) const
{
  // each interaction draw writes the buffer at most once; a surface's stage loop draws
  // at most once per bump, diffuse or specular stage and once more at its end
  auto stagesOn = std::uint64_t{0};
  for (const auto& stage : light.shader->stages)
  {
    if (engineRegister(light.registers, stage.conditionRegister) != 0.0f)
    {
      ++stagesOn;
    }
  }
  auto draws = std::uint64_t{0};
  for (const auto* list :
       {&light.localInteractions,
        &light.globalInteractions,
        &light.translucentInteractions})
  {
    for (const auto* surface : engineOrderInteractionSurfaces(*list))
    {
      auto perSurface = std::uint64_t{1};
      if (surface->material && surface->registers)
      {
        for (const auto& stage : surface->material->stages)
        {
          if (
            stage.lighting == em::LightingBump || stage.lighting == em::LightingDiffuse
            || stage.lighting == em::LightingSpecular)
          {
            ++perSurface;
          }
        }
      }
      draws += perSurface;
    }
  }
  return stagesOn * draws;
}

void LitPasses::recordInteractions(Frame& frame)
{
  // DrawInteractions (RenderBackend.cpp:4179-4251): r_skipInteractions and no view lights
  // return (the caller's test and this one), then two ordered phases over the one sorted
  // list, DrawInteractionsPass( false ) for the texture lights and DrawInteractionsPass(
  // true ) for the math lights (LP5.1, C45), each keeping the largest-first order
  if (m_lists.lights.empty())
  {
    return;
  }
  const auto budget = writeBudget();
  // D76 counts the writes of a volatile constant buffer; a pushed layout writes none
  const auto& interactionLayouts =
    frame.inputs->pipelines->layouts(EngineBindingLayout::DrawInteraction);
  const auto counted = interactionLayouts.state != BindingLayoutLookup::State::Ready
                       || !pushesConstants(interactionLayouts.layouts);
  // D76 (P6.13): one budget and one overflow over both phases, in their draw order: once
  // a light overflows, no light after it is drawn, the rest of its phase and every light
  // of the later phase
  auto overflowing = false;
  for (const auto mathPass : {false, true})
  {
    // DrawInteractionsPass (RenderBackend.cpp:3984-4171)
    for (const auto& light : m_lists.lights)
    {
      // do fogging later (:3999-4007): FogAllLights draws the fog and blend lights
      // (LP5.3, recordFogLights)
      if (light.kind == ViewLightKind::Fog || light.kind == ViewLightKind::Blend)
      {
        continue;
      }
      if (
        light.localInteractions.empty() && light.globalInteractions.empty()
        && light.translucentInteractions.empty())
      {
        continue;
      }
      // filter to this phase (:4014-4021) and count (:4022-4029): the lights with
      // interactions that entered the phase; ambientLights is the preview's own count
      const auto isMathLight = engineLightUsesMathPath(light.record->light.parms);
      if (isMathLight != mathPass)
      {
        continue;
      }
      ++(isMathLight ? m_stats.mathLights : m_stats.textureLights);
      if (light.shader->ambientLight)
      {
        ++m_stats.ambientLights;
      }

      if (overflowing)
      {
        ++m_stats.lightsOverflowed;
        continue;
      }
      // a pass boundary between lights
      if (frame.inputs->stopRequested && frame.inputs->stopRequested())
      {
        m_stats.stoppedEarly = true;
        return;
      }
      // D76: no light whose worst case would pass the frame's writes is added, nor any
      // after it
      const auto writes = m_frameWrites[EngineBindingLayout::DrawInteraction];
      if (counted && writes + worstCaseWrites(light) > budget)
      {
        overflowing = true;
        ++m_stats.lightsOverflowed;
        if (!m_overflowWarned && frame.inputs->warn)
        {
          m_overflowWarned = true;
          frame.inputs->warn(fmt::format(
            "The lit preview's interactions overflowed its constant buffer ({} writes a "
            "frame): some lights are not drawn",
            budget));
        }
        continue;
      }

      // per light, before its lists (:4122-4127): GL_State( GLS_DEFAULT ) (each draw's
      // pipeline key carries its whole state, D75, so it changes no draw), ALPHA_TEST 0,
      // ResetViewportAndScissorToDefaultCamera
      m_parms.set(RenderParm::AlphaTest, ZeroVec4);
      m_currentScissor = viewScissor(frame.view);
      m_scissor = engineScissorRect(frame.view, m_currentScissor);
      m_logLight = light.record->id;

      for (const auto& [surfaces, list, depthFunc] :
           {std::tuple{
              &light.localInteractions, InteractionList::Local, gls::DepthFuncEqual},
            std::tuple{
              &light.globalInteractions, InteractionList::Global, gls::DepthFuncEqual},
            std::tuple{
              &light.translucentInteractions,
              InteractionList::Translucent,
              gls::DepthFuncLess}})
      {
        // r_skipTranslucent (:4145)
        if (list == InteractionList::Translucent && EngineFrame.skipTranslucent)
        {
          continue;
        }
        if (!renderInteractions(frame, light, *surfaces, list, depthFunc))
        {
          m_stats.interactionsDisabled = true;
          return;
        }
      }
      ++m_stats.lightsDrawn;
    }
  }
  // disable stencil shadow test: GL_State( GLS_DEFAULT ) after both phases (:4237), which
  // the next pass's draws replace by their own pipelines
}

bool LitPasses::renderInteractions(
  Frame& frame,
  const ViewLight& light,
  const std::vector<LitSurface>& surfaces,
  const InteractionList list,
  const std::uint64_t depthFunc)
{
  // RenderInteractions (RenderBackend.cpp:1703-2247)
  if (surfaces.empty())
  {
    return true;
  }
  m_logList = list;

  // change the scissor if needed, it will be constant across all the surfaces lit by the
  // light (r_useScissor 1)
  if (!m_currentScissor.equals(light.scissor))
  {
    m_scissor = engineScissorRect(frame.view, light.scissor);
    m_currentScissor = light.scissor;
  }

  // additive RGB, alpha overwritten, no depth write, stencil ALWAYS (D75)
  const auto stateBits = interactionStateBits(depthFunc);

  const auto& shader = *light.shader;
  const auto& record = *light.record;
  const auto& lightRegs = light.registers;
  const auto noSpecular = record.light.parms.noSpecular;
  // LP5.1 (C45): R_LightUsesMathPath( vLight ) (RenderBackend.cpp:1914, 1937)
  const auto mathPath = engineLightUsesMathPath(record.light.parms);
  const auto ordered = engineOrderInteractionSurfaces(surfaces);
  auto& images = *frame.inputs->images;
  const auto usage =
    ImageUsage{[&](const MaterialImageKey& key) { return imageUsage(images, key); }};

  // drawInteraction_t inter = {}: the images null; the matrices, which the engine leaves
  // indeterminate, zero (mirror.json M-PASS-DIFF-02); din->vLight's ambientLight (LP5.1,
  // SC513: its specular _black, its program interactionAmbient)
  auto inter = InteractionState{};
  inter.ambientLight = shader.ambientLight;
  const auto lightScale = EngineFrame.lightScale;

  for (const auto& lightStage : shader.stages)
  {
    // ignore stages that fail the condition
    if (engineRegister(lightRegs, lightStage.conditionRegister) == 0.0f)
    {
      continue;
    }

    const auto& c = lightStage.colorRegisters;
    const auto lightColor = Vec4{
      lightScale * engineRegister(lightRegs, c[0]),
      lightScale * engineRegister(lightRegs, c[1]),
      lightScale * engineRegister(lightRegs, c[2]),
      engineRegister(lightRegs, c[3])};
    const auto diffuseColor = lightColor;
    auto specularColor = Vec4{
      lightColor[0] * 1.0f,
      lightColor[1] * 1.0f,
      lightColor[2] * 1.0f,
      lightColor[3] * 1.0f};
    if (noSpecular)
    {
      specularColor = ZeroVec4;
    }

    auto lightTextureMatrix = std::optional<EngineGlMatrix>{};
    if (lightStage.hasTextureMatrix)
    {
      lightTextureMatrix = engineShaderTextureMatrix(lightRegs, lightStage);
    }

    // texture 3 the light falloff, texture 4 the light projection; LP5.1 (C45, D112):
    // _white at both for a math light, which never reads its stage image, so a stage
    // without one is drawn there (RenderBackend.cpp:1906-1931)
    const auto lightUnits =
      engineLightStageUnits(mathPath, record.derived.shader.falloffImage, lightStage);
    if (!lightUnits)
    {
      // a texture light's cinematic stage: no image to bind (mirror.json M-PASS-GUARD-01)
      continue;
    }
    m_units[3] = (*lightUnits)[0];
    m_units[4] = (*lightUnits)[1];

    // USER0-2 (:1934-1965)
    const auto user = engineMathLightParms(record.light.parms.mathLight, mathPath);
    m_parms.set(RenderParm::User0, user[0]);
    m_parms.set(RenderParm::User1, user[1]);
    m_parms.set(RenderParm::User2, user[2]);

    // RB_SetupForFastPathInteractions (RenderBackend.cpp:1673-1694)
    m_parms.set(RenderParm::BumpMatrixS, RowS);
    m_parms.set(RenderParm::BumpMatrixT, RowT);
    m_parms.set(RenderParm::DiffuseMatrixS, RowS);
    m_parms.set(RenderParm::DiffuseMatrixT, RowT);
    m_parms.set(RenderParm::SpecularMatrixS, RowS);
    m_parms.set(RenderParm::SpecularMatrixT, RowT);
    const auto ignore = engineVertexColorParms(em::VertexColorIgnore);
    m_parms.set(RenderParm::VertexColorModulate, ignore[0]);
    m_parms.set(RenderParm::VertexColorAdd, ignore[1]);
    m_parms.set(RenderParm::DiffuseModifier, diffuseColor);
    m_parms.set(RenderParm::SpecularModifier, specularColor);

    // even if the space does not change between light stages, each light stage may need
    // a different lightTextureMatrix baked in
    m_currentSpace.reset();

    for (const auto* surface : ordered)
    {
      if (m_currentSpace != surface->space)
      {
        setInteractionSpace(frame, surface->space, light, lightStage, lightTextureMatrix);
      }

      auto disabled = false;
      const auto drawOne = [&](const InteractionDraw& interaction) {
        if (disabled)
        {
          return;
        }
        // DrawSingleInteraction's parms
        m_parms.set(RenderParm::BumpMatrixS, interaction.bumpMatrix[0]);
        m_parms.set(RenderParm::BumpMatrixT, interaction.bumpMatrix[1]);
        m_parms.set(RenderParm::DiffuseMatrixS, interaction.diffuseMatrix[0]);
        m_parms.set(RenderParm::DiffuseMatrixT, interaction.diffuseMatrix[1]);
        m_parms.set(RenderParm::SpecularMatrixS, interaction.specularMatrix[0]);
        m_parms.set(RenderParm::SpecularMatrixT, interaction.specularMatrix[1]);
        const auto vertexColor = engineVertexColorParms(interaction.vertexColor);
        m_parms.set(RenderParm::VertexColorModulate, vertexColor[0]);
        m_parms.set(RenderParm::VertexColorAdd, vertexColor[1]);
        m_parms.set(RenderParm::DiffuseModifier, interaction.diffuseColor);
        m_parms.set(RenderParm::SpecularModifier, interaction.specularColor);

        // texture 0 the bump map, 1 the specular map, 2 the diffuse map
        m_units[0] = interaction.bumpImage;
        m_units[1] = interaction.specularImage;
        m_units[2] = interaction.diffuseImage;

        const auto program =
          interaction.program == InteractionProgram::PbrInteraction
            ? EngineBuiltinPbrInteraction
          : interaction.program == InteractionProgram::InteractionAmbient
            ? EngineBuiltinInteractionAmbient
            : EngineBuiltinInteraction;
        switch (draw(
          frame,
          program,
          *surface->chunk,
          stateBits,
          0.0f,
          0.0f,
          surface->lightTriangles.get()))
        {
        case DrawOutcome::Drawn:
          ++m_stats.interactionDraws;
          log(
            frame, LitDrawRecord::Pass::Interaction, *surface->chunk, program, stateBits);
          break;
        case DrawOutcome::Disabled:
          disabled = true;
          break;
        case DrawOutcome::Overflowed:
          drawOverflowed(frame);
          break;
        case DrawOutcome::Skipped:
          break;
        }
      };

      if (surface->material && surface->registers)
      {
        engineSurfaceInteractions(
          *surface->material,
          surface->registers->registers,
          diffuseColor,
          specularColor,
          noSpecular,
          inter,
          usage,
          drawOne);
      }
      else if (
        const auto pending =
          pendingSurfaceInteraction(diffuseColor, usage, shader.ambientLight))
      {
        drawOne(*pending);
      }
      if (disabled)
      {
        return false;
      }
    }
  }
  return true;
}

void LitPasses::setInteractionSpace(
  Frame& frame,
  const std::size_t index,
  const ViewLight& light,
  const MaterialStage&,
  const std::optional<EngineGlMatrix>& lightTextureMatrix)
{
  // RenderBackend.cpp:2007-2128
  m_currentSpace = index;
  const auto& space = m_lists.spaces[index];

  // model-view-projection
  setMvp(m_parms, space.mvp);

  // MODELMATRIX and MODELVIEWMATRIX: the float[16]s transposed
  m_parms.set(RenderParm::ModelMatrixX, engineMatrixTranspose(space.modelMatrix));
  m_parms.set(RenderParm::ModelViewMatrixX, engineMatrixTranspose(space.modelViewMatrix));

  const auto& projection = light.record->derived.projection;
  const auto& origin = projection.globalLightOrigin;
  m_parms.set(RenderParm::GlobalLightOrigin, Vec4{origin[0], origin[1], origin[2], 1.0f});

  // tranform the light/view origin into model local space
  const auto localLight = engineGlobalPointToLocal(space.modelMatrix, origin);
  const auto localView =
    engineGlobalPointToLocal(space.modelMatrix, frame.view.viewOrigin);
  m_parms.set(
    RenderParm::LocalLightOrigin,
    Vec4{localLight[0], localLight[1], localLight[2], 0.0f});
  m_parms.set(
    RenderParm::LocalViewOrigin, Vec4{localView[0], localView[1], localView[2], 1.0f});

  // transform the light project into model local space
  auto lightProjection = std::array<EnginePlane, 4>{};
  for (std::size_t i = 0; i < 4; ++i)
  {
    lightProjection[i] =
      engineGlobalPlaneToLocal(space.modelMatrix, projection.lightProject[i]);
  }

  // optionally multiply the local light projection by the light texture matrix
  if (lightTextureMatrix)
  {
    engineBakeTextureMatrixIntoTexgen(lightProjection, *lightTextureMatrix);
  }

  m_parms.set(RenderParm::LightProjectionS, vec4Of(lightProjection[0]));
  m_parms.set(RenderParm::LightProjectionT, vec4Of(lightProjection[1]));
  m_parms.set(RenderParm::LightProjectionQ, vec4Of(lightProjection[2]));
  m_parms.set(RenderParm::LightFalloffS, vec4Of(lightProjection[3]));
}

std::uint64_t interactionStateBits(const std::uint64_t depthFunc)
{
  return gls::SrcBlendOne | gls::DstBlendOne | gls::DepthMask | depthFunc
         | gls::StencilFuncAlways;
}

std::uint64_t ambientStateBits(const int vertexColor)
{
  // RenderBackend.cpp:2603-2611 (and 2636, 2668, 2692): SVC_IGNORE overwrites, any
  // other vertex colour adds ("a bit hacky: use additive blending to blend the
  // normals")
  return (vertexColor == em::VertexColorIgnore ? gls::DstBlendZero : gls::DstBlendOne)
         | gls::SrcBlendOne | gls::DepthMask | gls::DepthFuncEqual
         | gls::StencilFuncAlways;
}

std::span<const AmbientUnitBinding> ambientUnitBindings()
{
  return AmbientUnits;
}

void LitPasses::setAmbientSpace(Frame& frame, const std::size_t index)
{
  // RenderBackend.cpp:2480-2502: change the matrix if needed
  m_currentSpace = index;
  const auto& space = m_lists.spaces[index];
  setMvp(m_parms, space.mvp);

  // tranform the view origin into model local space
  const auto localView =
    engineGlobalPointToLocal(space.modelMatrix, frame.view.viewOrigin);
  m_parms.set(
    RenderParm::LocalViewOrigin, Vec4{localView[0], localView[1], localView[2], 1.0f});

  // the model -> world matrix and the model -> camera matrix, transposed
  m_parms.set(RenderParm::ModelMatrixX, engineMatrixTranspose(space.modelMatrix));
  m_parms.set(RenderParm::ModelViewMatrixX, engineMatrixTranspose(space.modelViewMatrix));
}

void LitPasses::recordAmbient(Frame& frame)
{
  // AmbientPass( drawSurfs, numDrawSurfs, fillGbuffer false ) (RenderBackend.cpp:
  // 2264-2731) on the light-grid path (DrawSingleInteraction's useIBL branch with a grid
  // area, :1196-1305, LP4.3) or the probe path (:1306-1394).
  // The early outs (:2268-2287): no surface; a view without entities and RDF_NOAMBIENT
  // never reach the preview's 3D view
  const auto& surfaces = m_lists.drawSurfs;
  if (surfaces.empty())
  {
    return;
  }
  const auto skippedBefore = m_stats.skippedDraws;
  const auto pendingBefore = m_stats.pendingPipelines;

  // make sure rpWindowCoord is set even without post processing surfaces in the view
  // (:2292-2304): w = x2 - x1 + 1 of the viewport, the whole target here
  const auto w = float(frame.view.width);
  const auto h = float(frame.view.height);
  m_parms.set(RenderParm::WindowCoord, Vec4{1.0f / w, 1.0f / h, w, h});

  // force MVP change on first surface (:2362)
  m_currentSpace.reset();

  // :2366-2375: GL_State( GLS_DEFAULT ), then ONE / ZERO | DEPTHMASK | DEPTHFUNC_EQUAL,
  // which each draw's pipeline key carries (ambientStateBits); GL_Color( colorWhite )
  m_parms.set(RenderParm::Color, engineClampedColor(Vec4{1.0f, 1.0f, 1.0f, 1.0f}));

  // :2401-2418: the light colour white × r_lightScale for diffuse and specular, and
  // AMBIENT_COLOR ( r_forceAmbient, same, same, 1 ) (ambientBoost 1)
  const auto lightScale = EngineFrame.lightScale;
  const auto lightColor = Vec4{lightScale, lightScale, lightScale, lightScale};
  const auto diffuseColor = lightColor;
  const auto specularColor = lightColor;
  const auto ambient = EngineFrame.forceAmbient;
  m_parms.set(RenderParm::AmbientColor, Vec4{ambient, ambient, ambient, 1.0f});

  // :2420-2423: useIBL, and RB_SetupForFastPathInteractions( diffuseColor,
  // specularColor ) (:1673-1694), whose modifiers every draw sets again (:1189-1190)
  m_parms.set(RenderParm::BumpMatrixS, RowS);
  m_parms.set(RenderParm::BumpMatrixT, RowT);
  m_parms.set(RenderParm::DiffuseMatrixS, RowS);
  m_parms.set(RenderParm::DiffuseMatrixT, RowT);
  m_parms.set(RenderParm::SpecularMatrixS, RowS);
  m_parms.set(RenderParm::SpecularMatrixT, RowT);
  const auto ignore = engineVertexColorParms(em::VertexColorIgnore);
  m_parms.set(RenderParm::VertexColorModulate, ignore[0]);
  m_parms.set(RenderParm::VertexColorAdd, ignore[1]);
  m_parms.set(RenderParm::DiffuseModifier, diffuseColor);
  m_parms.set(RenderParm::SpecularModifier, specularColor);

  // the probe path's view parms (:1308-1335), the same for every draw of the view: the
  // probe bounds with their validity, the three probe positions, the radiance blends and
  // the blended SH (SetVertexParms of nine contiguous float4s)
  const auto& probes = m_viewProbes;
  const auto& bounds = probes.globalProbeBounds;
  const auto cleared = engineBoundsIsCleared(bounds);
  const auto centre = engineBoundsCenter(bounds);
  const auto probeParms = std::array<std::pair<RenderParm, Vec4>, 7>{{
    {RenderParm::WobbleSkyX,
     Vec4{bounds[0][0], bounds[0][1], bounds[0][2], cleared ? 0.0f : 1.0f}},
    {RenderParm::WobbleSkyY, Vec4{bounds[1][0], bounds[1][1], bounds[1][2], 0.0f}},
    {RenderParm::WobbleSkyZ, Vec4{centre[0], centre[1], centre[2], 1.0f}},
    {RenderParm::TexGen0S, probes.probePositions[0]},
    {RenderParm::TexGen0T, probes.probePositions[1]},
    {RenderParm::TexGen0Q, probes.probePositions[2]},
    {RenderParm::LocalLightOrigin, probes.blends},
  }};
  auto sh = std::array<float, 36>{};
  for (std::size_t i = 0; i < 9; ++i)
  {
    for (std::size_t c = 0; c < 4; ++c)
    {
      sh[i * 4 + c] = probes.sh[i][c];
    }
  }

  // the view's images (:1361-1393): t3 the BRDF LUT, t4 _white (K31: r_useSSAO 0 until
  // LP6), t7 _default for the irradiance image no probe-path shader reads (D85), t8-t10
  // the radiance cubes, t12 the nearest's depth cube, t13 and t14 the next two's; a probe
  // image that is not Ready is the default cube, never a slot's 2D stand-in (P4.28)
  const auto defaultCube = intrinsicImageKey("_defaultUACRadiance");
  const auto cubeOr = [&](const std::optional<MaterialImageKey>& key) {
    return key ? *key : defaultCube;
  };

  auto& images = *frame.inputs->images;
  const auto usage =
    ImageUsage{[&](const MaterialImageKey& key) { return imageUsage(images, key); }};

  for (const auto& surface : surfaces)
  {
    // a pass boundary is not taken between surfaces: the pass is one GL state's draws
    // M-AMB-DIFF-02: a surface whose material or registers are pending gets no ambient
    // draw (the interactions draw it with _default until they arrive); the frame is
    // unconverged already (FrontendLists::pending)
    if (!surface.material || !surface.registers)
    {
      continue;
    }
    const auto& material = *surface.material;

    // translucent surfaces don't put anything in the depth buffer and don't test
    // against it (:2430-2435)
    if (material.coverage == em::CoverageTranslucent)
    {
      continue;
    }

    // if all stages of a material have been conditioned off, don't do anything
    // (:2440-2454: surfaceRegs[ pStage->conditionRegister ] != 0 for some stage)
    const auto& registers = surface.registers->registers;
    if (std::ranges::none_of(material.stages, [&](const MaterialStage& stage) {
          return engineRegister(registers, stage.conditionRegister) != 0.0f;
        }))
    {
      continue;
    }

    // :2480-2502
    if (m_currentSpace != surface.space)
    {
      setAmbientSpace(frame, surface.space);
    }

    // LP4.3 (C36): a surface with a grid area takes the light-grid path when
    // r_useLightGrid is 1 (:1196, useIBL && din->surf->area && r_useLightGrid), else the
    // probe path (:1306)
    const auto* grid = EngineFrame.useLightGrid && surface.gridArea && frame.inputs->grid
                         ? frame.inputs->grid->find(*surface.gridArea)
                         : nullptr;
    if (grid)
    {
      ++m_stats.ambientGridSurfaces;
    }

    // :2538-2703: inter = {} (diffuse 1, specular 0), the fast path dead
    // (r_skipInteractionFastPath 1, :2545), the stage loop and the final draw
    auto disabled = false;
    const auto drawOne = [&](const InteractionDraw& interaction) {
      if (disabled)
      {
        return;
      }
      // :2603-2611 and alike: the group's blend, by the vertexColor its diffuse or
      // specular stage set (kept across a bump stage, P4.19)
      const auto stateBits = ambientStateBits(interaction.vertexColor);

      // DrawSingleInteraction (:1175-1190): the matrices, the vertex colour, the group's
      // colours as the modifiers
      m_parms.set(RenderParm::BumpMatrixS, interaction.bumpMatrix[0]);
      m_parms.set(RenderParm::BumpMatrixT, interaction.bumpMatrix[1]);
      m_parms.set(RenderParm::DiffuseMatrixS, interaction.diffuseMatrix[0]);
      m_parms.set(RenderParm::DiffuseMatrixT, interaction.diffuseMatrix[1]);
      m_parms.set(RenderParm::SpecularMatrixS, interaction.specularMatrix[0]);
      m_parms.set(RenderParm::SpecularMatrixT, interaction.specularMatrix[1]);
      const auto vertexColor = engineVertexColorParms(interaction.vertexColor);
      m_parms.set(RenderParm::VertexColorModulate, vertexColor[0]);
      m_parms.set(RenderParm::VertexColorAdd, vertexColor[1]);
      m_parms.set(RenderParm::DiffuseModifier, interaction.diffuseColor);
      m_parms.set(RenderParm::SpecularModifier, interaction.specularColor);

      // both paths' probe parms (:1198-1222, :1308-1331), the radiance blends among
      // them (:1246, :1333)
      for (const auto& [parm, value] : probeParms)
      {
        m_parms.set(parm, value);
      }
      auto program = 0;
      if (grid)
      {
        // the light-grid path (:1224-1276): the area's grid parms, the atlas texture's
        // size (GetUploadResolution); no SH (the grid shader reads none). The atlas
        // binds only its own written levels: Loading without one is not drawn yet
        const auto binding = images.binding(images.request(grid->atlas));
        if (!binding.texture || binding.intrinsic)
        {
          ++m_stats.skippedDraws;
          m_stats.unconverged = true;
          m_stats.retrigger = true;
          return;
        }
        const auto& desc = binding.texture->getDesc();
        const auto gridParms = engineGridParms(*grid, int(desc.width), int(desc.height));
        m_parms.set(RenderParm::GlobalLightOrigin, gridParms.globalLightOrigin);
        m_parms.set(RenderParm::JitterTexScale, gridParms.jitterTexScale);
        m_parms.set(RenderParm::JitterTexOffset, gridParms.jitterTexOffset);
        m_parms.set(RenderParm::ScreenCorrectionFactor, gridParms.screenCorrectionFactor);
        m_parms.set(RenderParm::CascadeDistances, gridParms.cascadeDistances);
        // :1248-1264: the PBR program by the specular image's usage (no skinning)
        program = interaction.program == InteractionProgram::PbrInteraction
                    ? EngineBuiltinAmbientLightgridIblPbr
                    : EngineBuiltinAmbientLightgridIbl;
      }
      else
      {
        // the probe path (:1306-1335): the view's SH
        m_parms.set(RenderParm::ShIrradiance0, sh);
        // :1337-1359: the PBR program by the specular image's usage (no skinning)
        program = interaction.program == InteractionProgram::PbrInteraction
                    ? EngineBuiltinAmbientLightingIblPbr
                    : EngineBuiltinAmbientLightingIbl;
      }

      // the units: the interaction's three, then the view's (:1361-1393)
      m_units[UnitBump] = interaction.bumpImage;
      m_units[UnitSpecular] = interaction.specularImage;
      m_units[UnitBaseColor] = interaction.diffuseImage;
      m_units[UnitBrdfLut] = intrinsicImageKey("_brdfLut");
      static_assert(!EngineFrame.useSsao, "K31: _ao0 is LP6's");
      m_units[UnitSsao] = engineWhiteImageKey();
      // t7: the area's irradiance atlas on the grid path (:1270-1271), _default on the
      // probe path (D85)
      m_units[UnitAmbientCube] = grid ? grid->atlas : engineDefaultImageKey();
      for (std::size_t i = 0; i < 3; ++i)
      {
        m_units[UnitSpecularCube1 + i] = cubeOr(probes.radiance[i]);
      }
      m_units[UnitDepthCube] = cubeOr(probes.depth);
      m_units[UnitDepthCube + 1] = cubeOr(probes.probeDepth[0]);
      m_units[UnitDepthCube + 2] = cubeOr(probes.probeDepth[1]);

      // no polygon offset: :2504-2536 are commented out (P4.19)
      switch (draw(frame, program, *surface.chunk, stateBits, 0.0f, 0.0f))
      {
      case DrawOutcome::Drawn:
        ++m_stats.ambientDraws;
        log(frame, LitDrawRecord::Pass::Ambient, *surface.chunk, program, stateBits);
        break;
      case DrawOutcome::Disabled:
        disabled = true;
        break;
      case DrawOutcome::Overflowed:
        // D86: set 9's own buffer and budget, U2.11's accounting
        ++m_stats.ambientOverflowed;
        drawOverflowed(frame);
        break;
      case DrawOutcome::Skipped:
        break;
      }
    };
    engineAmbientSurfaceInteractions(
      material, registers, diffuseColor, specularColor, usage, drawOne);
    if (disabled)
    {
      // D9: the pass is disabled; programReport() says why
      m_stats.ambientDisabled = true;
      break;
    }
  }

  // :2708-2712: disable blending (the next pass's pipeline sets its own state),
  // ALPHA_TEST 0, GL_SelectTexture( 0 ); renderProgManager.Unbind() has no NVRHI
  // counterpart (the next draw's pipeline replaces the program)
  m_parms.set(RenderParm::AlphaTest, ZeroVec4);
  // draw() counts an overflowed draw as skipped too
  m_stats.ambientSkipped =
    m_stats.skippedDraws - skippedBefore - m_stats.ambientOverflowed;
  m_stats.ambientPendingPipelines = m_stats.pendingPipelines - pendingBefore;
}

namespace
{

// LP3.1 (C59): the atlas's format and its framebuffer's (FBN:136-138: depth only)
constexpr auto AtlasFormat = nvrhi::Format::D32;
constexpr auto AtlasFormats = FramebufferFormats{nvrhi::Format::UNKNOWN, AtlasFormat};
// NUM_QUAD_TREE_LEVELS (RenderBackend_NVRHI.cpp:207-215)
constexpr auto AtlasQuadTreeLevels = 8u;

ShadowLightType shadowLightType(const ViewLight& light)
{
  // RenderBackend.cpp:3678-3700: vLight->parallel, then vLight->pointLight
  const auto& parms = light.record->light.parms;
  return parms.parallel     ? ShadowLightType::Parallel
         : parms.pointLight ? ShadowLightType::Point
                            : ShadowLightType::Spot;
}

// the side and its stop of ShadowAtlasPass' loops (:3676-3700, :3835-3859)
std::pair<int, int> shadowSides(const ShadowLightType type)
{
  switch (type)
  {
  case ShadowLightType::Parallel:
    return {0, EngineFrame.shadowMapSplits + 1};
  case ShadowLightType::Point:
    if (EngineFrame.shadowMapSingleSide != -1)
    {
      return {EngineFrame.shadowMapSingleSide, EngineFrame.shadowMapSingleSide + 1};
    }
    return {0, 6};
  case ShadowLightType::Spot:
    break;
  }
  return {-1, 0};
}

// LP3.1 (C58): what of viewDef_t the shadow matrices read: the view's origin, axis, fov,
// viewport and world view matrix
EngineShadowView shadowViewOf(const LitView& view)
{
  auto shadowView = EngineShadowView{};
  shadowView.vieworg = view.viewOrigin;
  shadowView.viewaxis = view.viewAxis;
  shadowView.fovX = view.fovX;
  shadowView.fovY = view.fovY;
  shadowView.width = view.width;
  shadowView.height = view.height;
  shadowView.worldModelViewMatrix = view.worldModelViewMatrix;
  shadowView.projectionMatrix = view.projectionMatrix;
  return shadowView;
}

// LP3.1 (C58): the viewLight_t fields SetupShadowMapMatrices reads (R_AddSingleLight:
// the light's globalLightOrigin, baseLightProject and its inverse, pointLight, parallel,
// lightCenter)
EngineShadowLight shadowLightOf(const ViewLight& light)
{
  const auto& record = *light.record;
  const auto& projection = record.derived.projection;
  auto shadowLight = EngineShadowLight{};
  shadowLight.globalLightOrigin = projection.globalLightOrigin;
  shadowLight.baseLightProject = projection.baseLightProject;
  shadowLight.inverseBaseLightProject = projection.inverseBaseLightProject;
  shadowLight.pointLight = record.light.parms.pointLight;
  shadowLight.parallel = record.light.parms.parallel;
  shadowLight.lightCenter = record.light.parms.lightCenter;
  return shadowLight;
}

// the pixels a and b share
std::int64_t overlapArea(const AtlasRect& a, const AtlasRect& b)
{
  const auto x0 = std::max(a.x, b.x);
  const auto y0 = std::max(a.y, b.y);
  const auto x1 = std::min(a.x + a.size, b.x + b.size);
  const auto y1 = std::min(a.y + a.size, b.y + b.size);
  return x1 > x0 && y1 > y0 ? std::int64_t(x1 - x0) * std::int64_t(y1 - y0) : 0;
}

} // namespace

void LitPasses::recordShadowAtlas(Frame& frame)
{
  m_shadowRecords.clear();
  m_shadowRecordOf.assign(m_lists.lights.size(), -1);
  // ShadowAtlasPass (RenderBackend.cpp:3605): r_skipShadows, r_useShadowAtlas (always 1
  // here: the arrays are K50's), no view lights
  if (m_skipShadows || EngineFrame.skipShadows || m_lists.lights.empty())
  {
    return;
  }
  static_assert(EngineFrame.useShadowAtlas == 1, "K50: the non-atlas path");

  // loop 1 (:3653-3716): the sides' sizes in viewLights order
  auto sizes = std::vector<EngineAtlasSize>{};
  for (std::size_t i = 0; i < m_lists.lights.size(); ++i)
  {
    const auto& light = m_lists.lights[i];
    if (light.kind == ViewLightKind::Fog || light.kind == ViewLightKind::Blend)
    {
      continue;
    }
    if (
      light.localInteractions.empty() && light.globalInteractions.empty()
      && light.translucentInteractions.empty())
    {
      continue;
    }
    // light doesn't cast shadows: LOD -1, or no shadow surface (globalShadows NULL: C57's
    // caster set is empty)
    if (light.shadowLod < 0 || light.casters.empty())
    {
      continue;
    }
    auto record = ShadowLightRecord{};
    record.light = light.record->id;
    record.index = i;
    record.type = shadowLightType(light);
    record.lod = light.shadowLod;
    const auto [first, stop] = shadowSides(record.type);
    record.sides = stop - first;
    const auto resolution = EngineShadowMapResolutions[std::size_t(light.shadowLod)];
    for (auto side = first; side < stop; ++side)
    {
      sizes.push_back(EngineAtlasSize{resolution, resolution});
    }
    m_shadowRecordOf[i] = int(m_shadowRecords.size());
    m_shadowRecords.push_back(record);
  }
  m_stats.lightsCasting = m_shadowRecords.size();

  // :3718-3783: the engine's packing (D145)
  if (!m_tileMapReady)
  {
    m_tileMapReady = m_tileMap.init(
      unsigned(EngineFrame.shadowMapAtlasSize),
      unsigned(EngineShadowMapResolutions[0]),
      AtlasQuadTreeLevels);
  }
  auto placements = std::vector<EngineAtlasPlacement>{};
  if (!sizes.empty() && m_tileMapReady)
  {
    placements =
      engineShadowAtlasLayout(m_tileMap, sizes, EngineFrame.shadowMapAtlasSize);
  }
  else
  {
    placements.resize(sizes.size());
  }

  // loop 2's placements (:3798-3895): imageSize the light's first side's output size,
  // imageAtlasOffset[ max( 0, side ) ] per side, x -1 when a side did not fit
  auto tiles = std::vector<std::pair<std::size_t, int>>{};
  auto current = std::vector<AtlasRect>{};
  {
    auto shadowIndex = std::size_t{0};
    for (auto r = std::size_t{0}; r < m_shadowRecords.size(); ++r)
    {
      auto& record = m_shadowRecords[r];
      const auto [first, stop] = shadowSides(record.type);
      record.imageSize = {placements[shadowIndex].size, placements[shadowIndex].size};
      auto fits = true;
      for (auto side = first; side < stop; ++side)
      {
        const auto slice = std::size_t(std::max(0, side));
        const auto& placement = placements[shadowIndex];
        record.placements[slice] = placement;
        record.atlasOffsets[slice] = {placement.x, placement.y};
        ++shadowIndex;
        if (placement.x == -1 || placement.y == -1)
        {
          // didn't fit into atlas anymore
          fits = false;
          continue;
        }
        tiles.emplace_back(r, side);
        current.push_back(AtlasRect{placement.x, placement.y, record.imageSize[0]});
      }
      if (!fits)
      {
        record.imageSize[0] = -1;
      }
    }
  }

  // P3.3: the rects last run's tiles occupied that this run's tiles do not cover
  auto vacated = std::vector<AtlasRect>{};
  for (const auto& rect : m_atlasOccupied)
  {
    auto covered = std::int64_t{0};
    for (const auto& tile : current)
    {
      covered += overlapArea(rect, tile);
    }
    if (covered < std::int64_t(rect.size) * std::int64_t(rect.size))
    {
      vacated.push_back(rect);
    }
  }
  // the perforated casters a light that takes tiles leaves out until LP3.4 (D155)
  for (const auto& record : m_shadowRecords)
  {
    for (const auto& caster : m_lists.lights[record.index].casters)
    {
      if (caster.chunk && caster.chunk->castClass == CastClass::Perforated)
      {
        ++m_stats.perforatedCastersSkipped;
      }
    }
  }

  // the lights placed and not, the latter by name (D157), at every way out
  const auto countRecords = [&] {
    for (const auto& record : m_shadowRecords)
    {
      if (record.placed())
      {
        ++m_stats.lightsPlaced;
      }
      else
      {
        ++m_stats.lightsUnplaced;
        m_stats.unplacedLights.emplace_back(
          spawnString(m_lists.lights[record.index].record->spawnArgs, "name"));
      }
    }
  };
  const auto unplaceAll = [&] {
    for (auto& record : m_shadowRecords)
    {
      record.imageSize[0] = -1;
    }
  };
  if (tiles.empty() && vacated.empty())
  {
    m_atlasOccupied.clear();
    countRecords();
    return;
  }

  // the atlas, made at the first frame that places a tile (A49: 256 MB from then on)
  if (!ensureShadowAtlas(frame))
  {
    m_stats.shadowAtlasDisabled = true;
    m_atlasOccupied.clear();
    unplaceAll();
    countRecords();
    return;
  }

  auto atlasFrame = frame;
  atlasFrame.framebuffer = m_atlasFramebuffer.Get();
  atlasFrame.formats = AtlasFormats;
  m_currentSpace.reset();
  auto disabled = false;

  // D147, P3.3: the vacated rects first, each by a far-plane quad
  for (const auto& rect : vacated)
  {
    if (!clearAtlasRect(atlasFrame, rect, NoPreviewId, -1))
    {
      disabled = true;
      break;
    }
    ++m_stats.tilesVacated;
  }

  // R_SetupSplitFrustums (tr_frontend_main.cpp:924: every view; a parallel light's
  // cascades and the consumer's rpCascadeDistances read it)
  const auto shadowView = shadowViewOf(frame.view);
  m_splitFrustums = engineSetupSplitFrustums(shadowView);

  // then each placed side, in loop 2's order: its far-plane clear, then its draws
  for (auto t = std::size_t{0}; t < tiles.size() && !disabled; ++t)
  {
    const auto [r, side] = tiles[t];
    auto& record = m_shadowRecords[r];
    const auto slice = std::size_t(std::max(0, side));
    // ShadowMapPassFast's SetupShadowMapMatrices (RB:3451-3453): its stores kept on the
    // light's record as drawn (C61), its raster pair for the side's draws
    const auto matrices = engineShadowMapMatrices(
      shadowLightOf(m_lists.lights[record.index]), side, shadowView, m_splitFrustums);
    record.shadowV[slice] = matrices.shadowV;
    record.shadowP[slice] = matrices.shadowP;
    const auto rect = AtlasRect{
      record.atlasOffsets[slice][0], record.atlasOffsets[slice][1], record.imageSize[1]};
    if (!clearAtlasRect(atlasFrame, rect, record.light, side))
    {
      disabled = true;
      break;
    }
    ++m_stats.tilesDrawn;
    m_stats.atlasBytesWritten +=
      std::uint64_t(rect.size) * std::uint64_t(rect.size) * sizeof(float);
    if (!shadowMapPass(atlasFrame, record, side, matrices))
    {
      disabled = true;
      break;
    }
  }
  m_atlasOccupied = std::move(current);

  if (disabled)
  {
    // D9: the pass is disabled; every light draws the plain interaction (programReport()
    // says why)
    m_stats.shadowAtlasDisabled = true;
    unplaceAll();
  }
  countRecords();

  // :3910-3915: GL_State( GLS_DEFAULT ) (each draw's key carries its state), ALPHA_TEST
  // 0, ResetViewportAndScissorToDefaultCamera
  m_parms.set(RenderParm::AlphaTest, ZeroVec4);
  m_currentScissor = viewScissor(frame.view);
  m_scissor = engineScissorRect(frame.view, m_currentScissor);
  m_currentSpace.reset();
}

bool LitPasses::ensureShadowAtlas(Frame& frame)
{
  if (m_atlas && m_atlasFramebuffer)
  {
    return true;
  }
  if (m_atlasFailed)
  {
    return false;
  }
  // _shadowMapAtlas: GenerateImage( NULL, r_shadowMapAtlasSize, r_shadowMapAtlasSize,
  // TF_LINEAR, TR_CLAMP_TO_ZERO_ALPHA, TD_DEPTH, isRenderTarget ) (Image_intrinsic.cpp:
  // 672-675, 1077): FMT_DEPTH, nvrhi::Format::D32 (IN:400-402), initial state DepthWrite
  // and clear value 1 (IN:556-566); nvrhi rule 3: that state kept
  auto& device = *frame.inputs->device;
  const auto size = std::uint32_t(EngineFrame.shadowMapAtlasSize);
  auto desc = nvrhi::TextureDesc{};
  desc.width = size;
  desc.height = size;
  desc.format = AtlasFormat;
  desc.debugName = "_shadowMapAtlas";
  desc.isRenderTarget = true;
  desc.initialState = nvrhi::ResourceStates::DepthWrite;
  desc.keepInitialState = true;
  desc.setClearValue(nvrhi::Color{1.0f});
  auto atlas = device.createTexture(desc);
  auto framebuffer =
    atlas ? device.createFramebuffer(nvrhi::FramebufferDesc{}.setDepthAttachment(atlas))
          : nvrhi::FramebufferHandle{};
  if (!atlas || !framebuffer)
  {
    m_atlasFailed = true;
    if (!m_atlasWarned && frame.inputs->warn)
    {
      m_atlasWarned = true;
      frame.inputs->warn(
        "The lit preview cannot make its shadow atlas (8192 × 8192 D32): no light casts "
        "shadows in this session");
    }
    return false;
  }
  // cleared whole once, at its creation (D147)
  frame.inputs->commandList->clearDepthStencilTexture(
    atlas, nvrhi::AllSubresources, true, 1.0f, false, 0);
  m_state.valid = false;
  m_atlas = std::move(atlas);
  m_atlasFramebuffer = std::move(framebuffer);
  m_atlasOccupied.clear();
  return true;
}

void LitPasses::setAtlasViewport(Frame& frame, const AtlasRect& rect)
{
  // GL_ViewportAndScissor( x, y, w, h ): currentViewport and the scissor [x, x + w] × [y,
  // y + h], taken by NVRHI top-left and unflipped (RenderBackend_NVRHI.cpp:520-535,
  // 2176-2193; SC719)
  frame.viewport = nvrhi::Viewport{
    float(rect.x),
    float(rect.x + rect.size),
    float(rect.y),
    float(rect.y + rect.size),
    0.0f,
    1.0f};
  m_scissor = nvrhi::Rect{rect.x, rect.x + rect.size, rect.y, rect.y + rect.size};
}

bool LitPasses::clearAtlasRect(
  Frame& frame, const AtlasRect& rect, const PreviewId light, const int side)
{
  // D147: NVRHI has no rect clear (§3.9); a quad at z = w = 1 through the identity MVP,
  // DEPTHFUNC_ALWAYS, depth written, both sides, no polygon offset, writes exactly 1.0
  // under the rect's scissor
  setAtlasViewport(frame, rect);
  const auto geometry = clearQuadGeometry(frame);
  if (!geometry)
  {
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    m_stats.retrigger = true;
    return true;
  }
  setMvp(m_parms, engineIdentityMatrix());
  m_currentSpace.reset();
  constexpr auto state = gls::DepthFuncAlways | gls::CullTwoSided;
  const auto outcome =
    drawGeometry(frame, EngineBuiltinDepth, *geometry, state, 0.0f, 0.0f, nullptr);
  if (outcome == DrawOutcome::Disabled)
  {
    return false;
  }
  if (outcome == DrawOutcome::Drawn && m_logDraws)
  {
    auto record = LitDrawRecord{};
    record.pass = LitDrawRecord::Pass::ShadowClear;
    record.light = light;
    record.program = EngineBuiltinDepth;
    record.state = state;
    record.scissor = m_scissor;
    record.side = side;
    m_drawLog.push_back(std::move(record));
  }
  return true;
}

bool engineShadowCasterCulled(const EngineMat4& mvp, const EngineBounds& bounds)
{
  // the side's clip volume as the device rasterizes it: x and y in [-w, w] (the planes
  // of GetFrustumPlanes( zeroToOne false )), z in [0, w] (its zeroToOne near plane: row
  // 2, NVRHI's pipelines clip depth, RasterState::depthClipEnable unset), the far plane
  // row 3 - row 2; the planes point inside. A box is culled by a plane its corner
  // farthest inside lies outside of by more than CullMargin (model units), so that a
  // caster the device would rasterize at a tile's edge is never left out (D149)
  constexpr auto CullMargin = 0.01f;
  auto planes = engineFrustumPlanes(mvp, false, false);
  planes[4] = engineFrustumPlanes(mvp, true, false)[4];
  for (const auto& plane : planes)
  {
    const auto length =
      std::sqrt(plane.a * plane.a + plane.b * plane.b + plane.c * plane.c);
    if (!(length > 1e-12f))
    {
      continue;
    }
    const auto x = plane.a >= 0.0f ? bounds[1][0] : bounds[0][0];
    const auto y = plane.b >= 0.0f ? bounds[1][1] : bounds[0][1];
    const auto z = plane.c >= 0.0f ? bounds[1][2] : bounds[0][2];
    const auto distance = (plane.a * x + plane.b * y + plane.c * z + plane.d) / length;
    if (distance < -CullMargin)
    {
      return true;
    }
  }
  return false;
}

bool LitPasses::shadowMapPass(
  Frame& frame,
  const ShadowLightRecord& record,
  const int side,
  const EngineShadowSideMatrices& matrices)
{
  // ShadowMapPassFast (RenderBackend.cpp:3395-3584) into the side's tile (its viewport
  // and scissor set by the tile's clear): GLS_POLYGON_OFFSET, then
  // r_shadowMapOccluderFacing 2's TWOSIDED with GL_PolygonOffset(
  // r_shadowMapPolygonFactor, r_vkShadowMapPolygonOffset ) (the key's bias an int,
  // RBN:460, SC722); 0 FRONTSIDED and 1 BACKSIDED with the negated offsets as written
  // (:3433-3449)
  auto stateBits = gls::PolygonOffset;
  auto slope = EngineFrame.shadowMapPolygonFactor;
  auto bias = EngineFrame.vkShadowMapPolygonOffset;
  switch (EngineFrame.shadowMapOccluderFacing)
  {
  case 0:
    stateBits |= gls::CullFrontSided;
    break;
  case 1:
    stateBits |= gls::CullBackSided;
    slope = -slope;
    bias = -bias;
    break;
  default:
    stateBits |= gls::CullTwoSided;
    break;
  }
  const auto& light = m_lists.lights[record.index];
  m_logLight = record.light;
  m_currentSpace.reset();
  for (const auto& caster : light.casters)
  {
    if (!caster.chunk || surfaceIndexCount(caster) == 0)
    {
      // a job may have created an empty shadow geometry
      continue;
    }
    // translucent surfaces don't put anything in the depth buffer (:3506-3509, P3.17);
    // perforated ones go through ShadowMapPassPerforated, LP3.4's (D155): left out, and
    // counted
    if (caster.chunk->castClass != CastClass::Opaque)
    {
      continue;
    }
    // :3519-3555: model -> world, world -> light, then the clip MVP; a parallel light's
    // and a point light's clipMVP, a spot light's windowToClip · clipMVP
    const auto& space = m_lists.spaces[caster.space];
    const auto modelRenderMatrix = engineMatrixTranspose(space.modelMatrix);
    const auto modelToLight = engineMatrixMultiply(matrices.lightView, modelRenderMatrix);
    const auto clipMvp = engineMatrixMultiply(matrices.lightProjection, modelToLight);
    const auto mvp = record.type == ShadowLightType::Spot
                       ? engineMatrixMultiply(engineWindowSpaceToClipSpace(), clipMvp)
                       : clipMvp;
    // D149 (P3.19): not inside the side's true frustum, so it adds no fragment
    if (engineShadowCasterCulled(mvp, caster.chunk->bounds))
    {
      ++m_stats.shadowCastersCulled;
      continue;
    }
    setMvp(m_parms, mvp);
    // :3559-3572: BindShader_Depth (never the skinned form: no skinned model, P3.24e);
    // DrawElementsWithCounters on the caster's light triangles (a Pending one whole)
    const auto outcome = draw(
      frame,
      EngineBuiltinDepth,
      *caster.chunk,
      stateBits,
      slope,
      bias,
      caster.lightTriangles.get());
    switch (outcome)
    {
    case DrawOutcome::Drawn:
      ++m_stats.shadowDraws;
      if (m_logDraws)
      {
        auto drawn = LitDrawRecord{};
        drawn.pass = LitDrawRecord::Pass::Shadow;
        drawn.light = record.light;
        drawn.chunk = caster.chunk->key;
        drawn.program = EngineBuiltinDepth;
        drawn.state = stateBits;
        drawn.scissor = m_scissor;
        drawn.side = side;
        m_drawLog.push_back(std::move(drawn));
      }
      break;
    case DrawOutcome::Disabled:
      return false;
    case DrawOutcome::Overflowed:
      drawOverflowed(frame);
      break;
    case DrawOutcome::Skipped:
      break;
    }
  }
  return true;
}

std::optional<LitPasses::DrawGeometry> LitPasses::clearQuadGeometry(Frame& frame)
{
  if (!m_clearQuadVertices || !m_clearQuadIndexes)
  {
    // the clip-space square at the far plane: (±1, ±1, 1), w = 1 through the identity
    auto verts = std::array<EngineDrawVert, 4>{};
    verts[0].xyz = {-1.0f, -1.0f, 1.0f};
    verts[1].xyz = {1.0f, -1.0f, 1.0f};
    verts[2].xyz = {1.0f, 1.0f, 1.0f};
    verts[3].xyz = {-1.0f, 1.0f, 1.0f};
    constexpr auto indexes = std::array<std::uint16_t, 6>{0, 1, 2, 0, 2, 3};
    // nvrhi rule 3: a known entry state kept; written once through the frame's list
    auto vertexDesc = nvrhi::BufferDesc{};
    vertexDesc.byteSize = sizeof(verts);
    vertexDesc.debugName = "Lit preview atlas clear quad vertices";
    vertexDesc.isVertexBuffer = true;
    vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertexDesc.keepInitialState = true;
    auto indexDesc = nvrhi::BufferDesc{};
    indexDesc.byteSize = sizeof(indexes);
    indexDesc.debugName = "Lit preview atlas clear quad indexes";
    indexDesc.isIndexBuffer = true;
    indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
    indexDesc.keepInitialState = true;
    auto& device = *frame.inputs->device;
    auto vertices = device.createBuffer(vertexDesc);
    auto indexBuffer = device.createBuffer(indexDesc);
    if (!vertices || !indexBuffer)
    {
      return std::nullopt;
    }
    auto& commandList = *frame.inputs->commandList;
    commandList.writeBuffer(vertices, verts.data(), sizeof(verts));
    commandList.writeBuffer(indexBuffer, indexes.data(), sizeof(indexes));
    // nvrhi rules 6 and 7: the writes end the render pass
    m_state.valid = false;
    m_clearQuadVertices = std::move(vertices);
    m_clearQuadIndexes = std::move(indexBuffer);
  }
  return DrawGeometry{m_clearQuadVertices.Get(), m_clearQuadIndexes.Get(), 6, 0, 0};
}

std::optional<MaterialImageKey> LitPasses::stageImage(
  ImageSlots& images, const MaterialImageKey& key)
{
  const auto* slot = images.slot(images.request(key));
  if (!slot)
  {
    return std::nullopt;
  }
  switch (slot->state)
  {
  case ImageSlots::State::Default:
    // the engine's image is not loaded or is defaulted: globalImages->defaultImage
    return engineDefaultImageKey();
  case ImageSlots::State::Pending:
    // not answered yet: the engine has its image loaded before it draws
    return std::nullopt;
  case ImageSlots::State::Loading: {
    // its written levels, or none yet (the intrinsic would stand in)
    const auto binding = images.binding(images.request(key));
    return binding.texture && !binding.intrinsic ? std::optional{key} : std::nullopt;
  }
  case ImageSlots::State::Resident:
  case ImageSlots::State::Replacing:
    // Replacing binds its complete texture while the new one is written (LPF.1, C40)
    return key;
  }
  return std::nullopt;
}

std::pair<MaterialImageKey, bool> LitPasses::cubeImage(
  ImageSlots& images, const std::optional<MaterialImageKey>& key)
{
  // the preview's zero cube (M-STAGE-DIFF-04): never a slot's 2D stand-in (P4.28)
  const auto zeroCube = intrinsicImageKey("_defaultUACRadiance");
  if (!key || key->cube == 0)
  {
    return {zeroCube, false};
  }
  const auto id = images.request(*key);
  const auto* slot = images.slot(id);
  if (!slot)
  {
    return {zeroCube, true};
  }
  switch (slot->state)
  {
  case ImageSlots::State::Default:
    return {zeroCube, false};
  case ImageSlots::State::Pending:
    return {zeroCube, true};
  case ImageSlots::State::Loading: {
    const auto binding = images.binding(id);
    return binding.texture && !binding.intrinsic ? std::pair{*key, false}
                                                 : std::pair{zeroCube, true};
  }
  case ImageSlots::State::Resident:
  case ImageSlots::State::Replacing:
    return {*key, false};
  }
  return {zeroCube, true};
}

std::optional<MaterialImageKey> LitPasses::bumpImage(
  ImageSlots& images, const std::optional<MaterialImageKey>& key)
{
  if (!key)
  {
    return engineFlatImageKey();
  }
  const auto* slot = images.slot(images.request(*key));
  if (slot && slot->state == ImageSlots::State::Default)
  {
    return engineFlatImageKey();
  }
  return stageImage(images, *key);
}

const EngineMaterialProgram* LitPasses::materialProgram(
  Frame& frame, const MaterialStage& stage)
{
  // FindProgram( name, vertexProgram, fragmentProgram, POST_PROCESS_INGAME )
  // (Material.cpp:2252-2262): a "program" keyword names both shaders; the table's row is
  // the fragment program's (the vertex program's when only it is named)
  const auto& name =
    !stage.fragmentProgram.empty() ? stage.fragmentProgram : stage.vertexProgram;
  auto [it, added] = m_materialPrograms.try_emplace(name);
  if (added)
  {
    it->second = engineMaterialProgram(frame.inputs->gamePath, name);
  }
  if (!it->second && m_programWarnings.insert(name).second && frame.inputs->warn)
  {
    // D9: the stage is disabled, said once per program
    frame.inputs->warn(fmt::format(
      "The lit preview has no material program '{}' (not in its table, or a blob "
      "missing): its newStage stages are not drawn",
      name));
  }
  return it->second ? &*it->second : nullptr;
}

void LitPasses::recordStages(Frame& frame)
{
  // DrawShaderPasses (RenderBackend.cpp:4271-4716): r_skipAmbient is the caller's test
  // (the test switch, D118); targetRender is null for the 3D view; GL_SelectTexture( 0 );
  // currentSpace = 1 forces the first surface's space
  const auto& inputs = *frame.inputs;
  auto& images = *inputs.images;
  m_currentSpace.reset();
  auto disabledPrograms = std::set<int>{};

  for (std::size_t i = 0; i < m_stageOrder.size(); ++i)
  {
    const auto& surface = *m_stageOrder[i];
    // D124 (P6.6): a material or registers pending: the engine never draws one; kept
    // registers (D52) are drawn
    if (!surface.material || !surface.registers)
    {
      ++m_stats.stagePending;
      m_stats.unconverged = true;
      m_stats.waitsForTable = true;
      continue;
    }
    const auto& material = *surface.material;

    if (!material.hasAmbient)
    {
      continue;
    }
    if (material.portalSky)
    {
      continue;
    }
    // some deforms may disable themselves by setting numIndexes = 0 (LP7.2: an
    // instance's are its shared surface's)
    if (surfaceIndexCount(surface) == 0)
    {
      continue;
    }
    if (material.suppressInSubview)
    {
      continue;
    }
    // the preview's view is no xray subview, and every surface's stereo eye is 0

    // we need to draw the post process shaders after we have drawn the fog lights (:4329,
    // currentRenderCopied false at the first call): K38, the pass stops here
    if (material.sort >= EngineSortPostProcess)
    {
      for (std::size_t j = i; j < m_stageOrder.size(); ++j)
      {
        const auto* left = m_stageOrder[j];
        m_stats.stagePostProcess += left->material && left->material->hasAmbient ? 1 : 0;
      }
      break;
    }

    // change the matrix and other space related vars if needed (:4352-4382): MVP,
    // LOCALVIEWORIGIN, MODELMATRIX_X..W and MODELVIEWMATRIX_X..W transposed
    if (m_currentSpace != surface.space)
    {
      setAmbientSpace(frame, surface.space);
    }

    // change the scissor if needed (:4385-4394, r_useScissor 1): the surface's engine
    // scissor (P6.14), with GL_Scissor's Y flip
    if (!m_currentScissor.equals(surface.scissor))
    {
      m_scissor = engineScissorRect(frame.view, surface.scissor);
      m_currentScissor = surface.scissor;
    }

    const auto& registers = surface.registers->registers;
    const auto surfaceDraws = engineShaderPassDraws(material, registers);
    m_stats.stageSkipped += surfaceDraws.skipped;
    m_stats.stageTexgenLp6 += surfaceDraws.texgenLp6;
    m_stats.stageCinematic += surfaceDraws.cinematic;
    if (surfaceDraws.texgenLp6 > 0 && !m_texgenLp6Warned && inputs.warn)
    {
      // C48, P6.18 (mirror.json M-STAGE-DIFF-05)
      m_texgenLp6Warned = true;
      inputs.warn(
        "The lit preview does not draw texGen reflect2 stages (their programs need the "
        "G-buffer, Hi-Z and probe inputs of a later package): some material stages are "
        "not drawn");
    }

    for (const auto& stageDraw : surfaceDraws.draws)
    {
      const auto& stage = material.stages[std::size_t(stageDraw.stage)];
      auto program = EngineBuiltinTextureVertexColor;
      auto drawable = true;
      if (stageDraw.program == StageProgram::Material)
      {
        // C47 (:4509-4570): BindProgram( glslProgram ), USER0 + j, the fragment images at
        // units 0-2 where not null (C47: a null unit binds _default where the engine's is
        // stale, mirror.json M-STAGE-DIFF-03); rpEnableSkinning stays 0 (no joints);
        // WINDOWCOORD, PROJMATRIX_Y, MODELVIEWMATRIX_Z and ALPHA_TEST are what the engine
        // leaves in them: the view's, the space's and 0
        const auto* row = materialProgram(frame, stage);
        if (!row || disabledPrograms.contains(row->row.index))
        {
          ++m_stats.stageProgramMissing;
          ++m_stats.stageSkipped;
          continue;
        }
        program = row->row.index;
        for (std::size_t j = 0; j < stageDraw.userParms.size() && j < 8; ++j)
        {
          m_parms.set(
            RenderParm(int(RenderParm::User0) + int(j)), stageDraw.userParms[j]);
        }
        for (std::size_t unit = 0; unit < 3; ++unit)
        {
          const auto& fragment = unit < stage.fragmentImages.size()
                                   ? stage.fragmentImages[unit]
                                   : std::optional<MaterialImageKey>{};
          if (!fragment)
          {
            m_units[unit] = engineDefaultImageKey();
            continue;
          }
          m_units[unit] = stageImage(images, *fragment);
          drawable = drawable && m_units[unit].has_value();
        }
      }
      else
      {
        // GL_Color, the program, RB_SetVertexColorParms (:4598-4673)
        m_parms.set(RenderParm::Color, stageDraw.color);
        switch (stageDraw.program)
        {
        case StageProgram::TextureColorTexgen:
          program = EngineBuiltinTextureTexgenVertexColor;
          break;
        case StageProgram::Environment:
          program = EngineBuiltinEnvironment;
          break;
        case StageProgram::BumpyEnvironment:
          program = EngineBuiltinBumpyEnvironment;
          break;
        case StageProgram::SkyBox:
          program = EngineBuiltinSkyBox;
          break;
        case StageProgram::WobbleSky:
          program = EngineBuiltinWobbleSky;
          break;
        case StageProgram::TextureColor:
        case StageProgram::Material:
          program = EngineBuiltinTextureVertexColor;
          break;
        }
        m_parms.set(RenderParm::VertexColorModulate, stageDraw.vertexColor[0]);
        m_parms.set(RenderParm::VertexColorAdd, stageDraw.vertexColor[1]);

        if (stageDraw.texgenWarning && !m_texgenWarned && inputs.warn)
        {
          // PrepareStageTexturing (:625-635): the engine's own warning, nothing else
          m_texgenWarned = true;
          inputs.warn(
            "A material stage uses texGen normal or glassWarp, which the engine warns "
            "about and draws as a plain stage (\"Please contact Brian!\")");
        }

        if (engineStageProgramSamplesCube(stageDraw.program))
        {
          // LP5.2b (C48): BindVariableStageImage binds the stage's cube image at unit 0
          // (its _default when not loaded or defaulted, a 2D image in the TextureCube
          // slot); the preview's zero cube stands in for an unwritten, defaulted or 2D
          // one, a cube not written yet keeping the frame unconverged (M-STAGE-DIFF-04)
          const auto [cube, pendingCube] = cubeImage(images, stage.image);
          m_units[0] = cube;
          if (pendingCube)
          {
            ++m_stats.stagePending;
            m_stats.unconverged = true;
            m_stats.retrigger = true;
          }
          if (stageDraw.program == StageProgram::BumpyEnvironment)
          {
            // :366-368: GL_SelectTexture( 1 ); bumpStage->texture.image->Bind()
            const auto& bump = material.stages[std::size_t(stageDraw.bumpStage)];
            m_units[1] = bumpImage(images, bump.image);
            drawable = m_units[1].has_value();
          }
          if (stageDraw.program == StageProgram::WobbleSky)
          {
            // :561: SetVertexParms( RENDERPARM_WOBBLESKY_X, transform, 3 )
            m_parms.set(RenderParm::WobbleSkyX, stageDraw.wobbleSky[0]);
            m_parms.set(RenderParm::WobbleSkyY, stageDraw.wobbleSky[1]);
            m_parms.set(RenderParm::WobbleSkyZ, stageDraw.wobbleSky[2]);
          }
        }
        else
        {
          // BindVariableStageImage (:268-345): a stage without an image binds none in the
          // engine (a dynamic image's stage, K25), and a cube image in texture_color's
          // Texture2D (a cubeMap stage of TG_EXPLICIT, TG_SCREEN, TG_DIFFUSE_CUBE or
          // TG_GLASSWARP) is undefined there: neither is drawn (M-STAGE-GUARD-01)
          if (!stage.image || stage.image->cube != 0)
          {
            ++m_stats.stageSkipped;
            if (!m_stageImageWarned && inputs.warn)
            {
              m_stageImageWarned = true;
              inputs.warn(
                "The lit preview does not draw material stages without an image, or with "
                "a cube image on texture_color: some material stages are not drawn");
            }
            continue;
          }
          m_units[0] = stageImage(images, *stage.image);
          drawable = m_units[0].has_value();
        }

        // PrepareStageTexturing (:352-638): the texture matrix always, TG_SCREEN's planes
        // from modelViewMatrix × projectionMatrix (rows 0, 1 and 3), TEXGEN_0_ENABLED
        m_parms.set(RenderParm::TextureMatrixS, stageDraw.textureMatrix[0]);
        m_parms.set(RenderParm::TextureMatrixT, stageDraw.textureMatrix[1]);
        if (stageDraw.screenTexgen)
        {
          const auto& space = m_lists.spaces[surface.space];
          const auto mat =
            engineGlMatrixMultiply(space.modelViewMatrix, frame.view.projectionMatrix);
          m_parms.set(
            RenderParm::TexGen0S,
            Vec4{mat[0 * 4 + 0], mat[1 * 4 + 0], mat[2 * 4 + 0], mat[3 * 4 + 0]});
          m_parms.set(
            RenderParm::TexGen0T,
            Vec4{mat[0 * 4 + 1], mat[1 * 4 + 1], mat[2 * 4 + 1], mat[3 * 4 + 1]});
          m_parms.set(
            RenderParm::TexGen0Q,
            Vec4{mat[0 * 4 + 3], mat[1 * 4 + 3], mat[2 * 4 + 3], mat[3 * 4 + 3]});
        }
        const auto enabled = stageDraw.screenTexgen ? 1.0f : 0.0f;
        m_parms.set(RenderParm::TexGen0Enabled, Vec4{enabled, enabled, enabled, enabled});
      }

      if (!drawable)
      {
        // D124: an image not answered or not written yet: no draw, the frame unconverged
        ++m_stats.stageSkipped;
        ++m_stats.skippedDraws;
        m_stats.unconverged = true;
        m_stats.retrigger = true;
        continue;
      }

      // GL_State( stageGLState ), the draw, FinishStageTexturing (for the reflect cubes
      // renderProgManager.Unbind(), :645-669, which has no NVRHI counterpart: the next
      // draw's pipeline replaces the program) and the restored offset, which the next
      // draw's key carries
      switch (draw(
        frame,
        program,
        *surface.chunk,
        stageDraw.state,
        stageDraw.slopeBias,
        stageDraw.depthBias))
      {
      case DrawOutcome::Drawn:
        ++m_stats.stageDraws;
        log(frame, LitDrawRecord::Pass::Stage, *surface.chunk, program, stageDraw.state);
        break;
      case DrawOutcome::Disabled:
        ++m_stats.stageSkipped;
        if (stageDraw.program == StageProgram::Material)
        {
          // D9: a material program whose pipeline failed disables its stages alone
          disabledPrograms.insert(program);
        }
        else
        {
          // D9: the pass is disabled; programReport() says why
          m_stats.stagesDisabled = true;
          m_parms.set(RenderParm::Color, Vec4{1.0f, 1.0f, 1.0f, 1.0f});
          return;
        }
        break;
      case DrawOutcome::Overflowed:
        ++m_stats.stageSkipped;
        drawOverflowed(frame);
        break;
      case DrawOutcome::Skipped:
        ++m_stats.stageSkipped;
        break;
      }
    }
  }

  // :4707-4712: GL_Color( 1, 1, 1 ) (alpha 1); GL_State( GLS_DEFAULT ) disables the
  // stencil test, which the next pass's pipeline keys carry as their own state
  m_parms.set(RenderParm::Color, Vec4{1.0f, 1.0f, 1.0f, 1.0f});
}

void LitPasses::recordFogLights(Frame& frame)
{
  // FogAllLights (RenderBackend.cpp:5038-5076): r_skipFogLights, r_showOverDraw (0), no
  // view lights, an xray subview (none here)
  if (EngineFrame.skipFogLights)
  {
    m_stats.fogLightsSkipped = true;
    return;
  }
  if (m_lists.lights.empty())
  {
    return;
  }
  // force fog plane to recalculate
  m_currentSpace.reset();
  for (const auto& light : m_lists.lights)
  {
    // a pass boundary between lights
    if (frame.inputs->stopRequested && frame.inputs->stopRequested())
    {
      m_stats.stoppedEarly = true;
      return;
    }
    if (light.kind == ViewLightKind::Fog)
    {
      if (!fogPass(frame, light))
      {
        m_stats.fogDisabled = true;
        return;
      }
    }
    else if (light.kind == ViewLightKind::Blend)
    {
      if (!blendLight(frame, light))
      {
        m_stats.fogDisabled = true;
        return;
      }
    }
  }
}

bool LitPasses::blendLight(Frame& frame, const ViewLight& light)
{
  if (light.globalInteractions.empty())
  {
    return true;
  }
  if (EngineFrame.skipBlendLights)
  {
    return true;
  }
  ++m_stats.blendLights;
  m_logLight = light.record->id;
  auto& images = *frame.inputs->images;
  const auto& shader = *light.shader;
  const auto& regs = light.registers;

  // texture 1 will get the falloff texture
  const auto& falloff = light.record->derived.shader.falloffImage;
  const auto falloffUnit =
    falloff ? stageImage(images, *falloff) : std::optional<MaterialImageKey>{};

  // texture 0 will get the projected texture
  for (const auto& stage : shader.stages)
  {
    if (engineRegister(regs, stage.conditionRegister) == 0.0f)
    {
      continue;
    }
    const auto stateBits = gls::DepthMask | stage.drawStateBits | gls::DepthFuncEqual;
    if (!stage.image)
    {
      // a stage without an image (a cinematic's): the engine binds nothing there
      // (stage->texture.image is NULL); the preview draws nothing for it
      ++m_stats.fogSkipped;
      continue;
    }
    const auto stageUnit = stageImage(images, *stage.image);
    if (!stageUnit || !falloffUnit)
    {
      // D124: an image not answered or not written yet: no draw, the frame unconverged
      ++m_stats.fogSkipped;
      ++m_stats.skippedDraws;
      m_stats.unconverged = true;
      m_stats.retrigger = true;
      continue;
    }
    m_units[0] = *stageUnit;
    m_units[1] = *falloffUnit;

    // RB_LoadShaderTextureMatrix: set 7 has no TEXTUREMATRIX, so it is never read
    if (stage.hasTextureMatrix)
    {
      const auto matrix = engineLoadShaderTextureMatrix(regs, stage);
      m_parms.set(RenderParm::TextureMatrixS, matrix[0]);
      m_parms.set(RenderParm::TextureMatrixT, matrix[1]);
    }

    // get the modulate values from the light, including alpha, unlike normal lights
    auto lightColor = Vec4{};
    for (std::size_t i = 0; i < 4; ++i)
    {
      lightColor[i] = engineRegister(regs, stage.colorRegisters[i]);
    }
    m_parms.set(RenderParm::Color, engineClampedColor(lightColor));

    if (
      !tBlendLight(frame, light, light.globalInteractions, stateBits)
      || !tBlendLight(frame, light, light.localInteractions, stateBits))
    {
      return false;
    }
  }
  return true;
}

bool LitPasses::tBlendLight(
  Frame& frame,
  const ViewLight& light,
  const std::vector<LitSurface>& surfaces,
  const std::uint64_t stateBits)
{
  m_currentSpace.reset();
  const auto& projection = light.record->derived.projection;
  for (const auto& surface : surfaces)
  {
    // the interaction surface's scissorRect is its light's: an empty one is skipped
    const auto& scissor = light.scissor;
    if (scissor.isEmpty())
    {
      continue;
    }
    if (!m_currentScissor.equals(scissor))
    {
      m_scissor = engineScissorRect(frame.view, scissor);
      m_currentScissor = scissor;
    }
    if (m_currentSpace != surface.space)
    {
      // change the matrix, and the light projection matrix
      const auto& space = m_lists.spaces[surface.space];
      setMvp(m_parms, space.mvp);
      auto lightProjectInCurrentSpace = std::array<EnginePlane, 4>{};
      for (std::size_t i = 0; i < 4; ++i)
      {
        lightProjectInCurrentSpace[i] =
          engineGlobalPlaneToLocal(space.modelMatrix, projection.lightProject[i]);
      }
      m_parms.set(RenderParm::TexGen0S, vec4Of(lightProjectInCurrentSpace[0]));
      m_parms.set(RenderParm::TexGen0T, vec4Of(lightProjectInCurrentSpace[1]));
      m_parms.set(RenderParm::TexGen0Q, vec4Of(lightProjectInCurrentSpace[2]));
      // falloff
      m_parms.set(RenderParm::TexGen1S, vec4Of(lightProjectInCurrentSpace[3]));
      m_currentSpace = surface.space;
    }
    // BindShader_BlendLight (the skinned form never: no joints, LP7)
    const auto outcome = draw(
      frame,
      EngineBuiltinBlendLight,
      *surface.chunk,
      stateBits,
      0.0f,
      0.0f,
      surface.lightTriangles.get());
    if (!countFogDraw(frame, outcome, surface.chunk, EngineBuiltinBlendLight, stateBits))
    {
      return false;
    }
  }
  return true;
}

bool LitPasses::fogPass(Frame& frame, const ViewLight& light)
{
  // find the current color and density of the fog (stage 0, no r_lightScale), and the
  // falloff planes
  const auto parms = engineFogParms(
    *light.shader,
    light.registers,
    light.fogPlane,
    frame.view.worldModelViewMatrix,
    frame.view.viewOrigin);
  if (!parms)
  {
    // a fog shader without a stage: the engine would read past its stage list
    return true;
  }
  ++m_stats.fogLights;
  m_logLight = light.record->id;
  m_parms.set(RenderParm::Color, parms->color);

  // texture 0 is the falloff image, texture 1 the entering plane fade correction
  m_units[0] = intrinsicImageKey("_fog");
  m_units[1] = intrinsicImageKey("_fogEnter");

  // draw it
  const auto blend =
    gls::DepthMask | gls::SrcBlendSrcAlpha | gls::DstBlendOneMinusSrcAlpha;
  if (
    !basicFog(
      frame, light, light.globalInteractions, parms->planes, blend | gls::DepthFuncEqual)
    || !basicFog(
      frame, light, light.localInteractions, parms->planes, blend | gls::DepthFuncEqual))
  {
    return false;
  }

  // the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less
  // instead of depthfunc_equal
  if (!fogCube(
        frame, light, parms->planes, blend | gls::DepthFuncLess | gls::CullBackSided))
  {
    return false;
  }

  // GL_State( ( glStateBits & ~GLS_CULL_MASK ) | GLS_CULL_FRONTSIDED ): the next draw's
  // key carries its own cull
  return true;
}

void LitPasses::setFogPlanes(const std::array<EnginePlane, 4>& planes)
{
  m_parms.set(RenderParm::TexGen0S, vec4Of(planes[0]));
  m_parms.set(RenderParm::TexGen0T, vec4Of(planes[1]));
  m_parms.set(RenderParm::TexGen1T, vec4Of(planes[2]));
  m_parms.set(RenderParm::TexGen1S, vec4Of(planes[3]));
}

bool LitPasses::basicFog(
  Frame& frame,
  const ViewLight& light,
  const std::vector<LitSurface>& surfaces,
  const std::array<EnginePlane, 4>& planes,
  const std::uint64_t stateBits)
{
  // T_BasicFog (RenderBackend.cpp:4868-4933) with inverseBaseLightProject NULL
  m_currentSpace.reset();
  for (const auto& surface : surfaces)
  {
    // the interaction surface's scissorRect is its light's (tr_frontend_addmodels.cpp:
    // 981): an empty one is skipped
    const auto& scissor = light.scissor;
    if (scissor.isEmpty())
    {
      continue;
    }
    if (!m_currentScissor.equals(scissor))
    {
      m_scissor = engineScissorRect(frame.view, scissor);
      m_currentScissor = scissor;
    }
    if (m_currentSpace != surface.space)
    {
      const auto& space = m_lists.spaces[surface.space];
      setMvp(m_parms, space.mvp);
      auto localFogPlanes = std::array<EnginePlane, 4>{};
      for (std::size_t i = 0; i < 4; ++i)
      {
        localFogPlanes[i] = engineGlobalPlaneToLocal(space.modelMatrix, planes[i]);
      }
      setFogPlanes(localFogPlanes);
      m_currentSpace = surface.space;
    }
    // BindShader_Fog (the skinned form never: the preview has no joints, LP7)
    const auto outcome = draw(
      frame,
      EngineBuiltinFog,
      *surface.chunk,
      stateBits,
      0.0f,
      0.0f,
      surface.lightTriangles.get());
    if (outcome == DrawOutcome::Drawn)
    {
      ++m_stats.fogSurfaceDraws;
    }
    if (!countFogDraw(frame, outcome, surface.chunk, EngineBuiltinFog, stateBits))
    {
      return false;
    }
  }
  return true;
}

bool LitPasses::fogCube(
  Frame& frame,
  const ViewLight& light,
  const std::array<EnginePlane, 4>& planes,
  const std::uint64_t stateBits)
{
  // zeroOneCubeSurface: space worldSpace, scissorRect the view's scissor
  const auto geometry = fogCubeGeometry(frame);
  if (!geometry)
  {
    ++m_stats.fogSkipped;
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    return true;
  }
  const auto scissor = viewScissor(frame.view);
  if (!m_currentScissor.equals(scissor))
  {
    m_scissor = engineScissorRect(frame.view, scissor);
    m_currentScissor = scissor;
  }
  // T_BasicFog with inverseBaseLightProject: MVP = worldSpace.mvp ×
  // inverseBaseLightProject, the planes InverseTransformPlane( normalize false )
  const auto& inverseBaseLightProject =
    light.record->derived.projection.inverseBaseLightProject;
  setMvp(m_parms, engineMatrixMultiply(frame.view.worldMvp, inverseBaseLightProject));
  auto localFogPlanes = std::array<EnginePlane, 4>{};
  for (std::size_t i = 0; i < 4; ++i)
  {
    localFogPlanes[i] =
      engineInverseTransformPlane(inverseBaseLightProject, planes[i], false);
  }
  setFogPlanes(localFogPlanes);
  m_currentSpace.reset();
  const auto outcome =
    drawGeometry(frame, EngineBuiltinFog, *geometry, stateBits, 0.0f, 0.0f, nullptr);
  if (outcome == DrawOutcome::Drawn)
  {
    ++m_stats.fogCubeDraws;
  }
  return countFogDraw(frame, outcome, nullptr, EngineBuiltinFog, stateBits);
}

std::optional<LitPasses::DrawGeometry> LitPasses::fogCubeGeometry(Frame& frame)
{
  if (!m_fogCubeVertices || !m_fogCubeIndexes)
  {
    // R_MakeZeroOneCubeTris (RenderSystem_init.cpp:2035-2120) as idDrawVerts: the
    // corners, the first four's colour 0xffffffff
    auto verts = std::array<EngineDrawVert, 8>{};
    const auto corners = engineZeroOneCubeVertices();
    for (std::size_t i = 0; i < verts.size(); ++i)
    {
      verts[i].xyz = corners[i];
      verts[i].color = i < 4 ? std::array<std::uint8_t, 4>{255, 255, 255, 255}
                             : std::array<std::uint8_t, 4>{0, 0, 0, 0};
    }
    const auto indexes = engineZeroOneCubeIndexes();
    // nvrhi rule 3: a known entry state kept; written once through the frame's list
    auto vertexDesc = nvrhi::BufferDesc{};
    vertexDesc.byteSize = sizeof(verts);
    vertexDesc.debugName = "Lit preview fog cube vertices";
    vertexDesc.isVertexBuffer = true;
    vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertexDesc.keepInitialState = true;
    auto indexDesc = nvrhi::BufferDesc{};
    indexDesc.byteSize = sizeof(indexes);
    indexDesc.debugName = "Lit preview fog cube indexes";
    indexDesc.isIndexBuffer = true;
    indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
    indexDesc.keepInitialState = true;
    auto& device = *frame.inputs->device;
    auto vertices = device.createBuffer(vertexDesc);
    auto indexBuffer = device.createBuffer(indexDesc);
    if (!vertices || !indexBuffer)
    {
      return std::nullopt;
    }
    auto& commandList = *frame.inputs->commandList;
    commandList.writeBuffer(vertices, verts.data(), sizeof(verts));
    commandList.writeBuffer(indexBuffer, indexes.data(), sizeof(indexes));
    // nvrhi rules 6 and 7: the writes end the render pass, so the next draw sets its
    // state again
    m_state.valid = false;
    m_fogCubeVertices = std::move(vertices);
    m_fogCubeIndexes = std::move(indexBuffer);
  }
  return DrawGeometry{m_fogCubeVertices.Get(), m_fogCubeIndexes.Get(), 36, 0, 0};
}

bool LitPasses::countFogDraw(
  Frame& frame,
  const DrawOutcome outcome,
  const Chunk* chunk,
  const int program,
  const std::uint64_t state)
{
  const auto fog = program == EngineBuiltinFog;
  switch (outcome)
  {
  case DrawOutcome::Drawn:
    ++(fog ? m_stats.fogDraws : m_stats.blendDraws);
    if (m_logDraws)
    {
      auto record = LitDrawRecord{};
      record.pass = fog ? LitDrawRecord::Pass::Fog : LitDrawRecord::Pass::Blend;
      record.light = m_logLight;
      record.chunk = chunk ? chunk->key : ChunkKey{};
      record.program = program;
      record.state = state;
      record.scissor = m_scissor;
      m_drawLog.push_back(std::move(record));
    }
    return true;
  case DrawOutcome::Disabled:
    // D9: the pass is disabled; programReport() says why
    m_parms.set(RenderParm::Color, Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    return false;
  case DrawOutcome::Overflowed:
    ++m_stats.fogSkipped;
    drawOverflowed(frame);
    return true;
  case DrawOutcome::Skipped:
    ++m_stats.fogSkipped;
    return true;
  }
  return true;
}

std::vector<PipelineKey> litStartPipelineKeys(const nvrhi::Format depthFormat)
{
  const auto formats = FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, depthFormat};
  const auto key = [&](const int program, const std::uint64_t state) {
    return PipelineKey{PipelineRecipe::Draw, program, state, 0, 0.0f, formats};
  };
  return {
    key(EngineBuiltinDepth, gls::Default),
    // V2.7: the solid prepass draw of a surface whose registers are pending (D77)
    key(EngineBuiltinDepth, gls::AlphaMask),
    key(EngineBuiltinTextureVertexColor, gls::Default),
    key(EngineBuiltinInteraction, interactionStateBits(gls::DepthFuncEqual)),
    key(EngineBuiltinInteraction, interactionStateBits(gls::DepthFuncLess)),
    key(EngineBuiltinPbrInteraction, interactionStateBits(gls::DepthFuncEqual)),
    key(EngineBuiltinPbrInteraction, interactionStateBits(gls::DepthFuncLess)),
    // LP4.2a (C34): the ambient pass's probe path, overwriting and adding
    key(EngineBuiltinAmbientLightingIbl, ambientStateBits(em::VertexColorIgnore)),
    key(EngineBuiltinAmbientLightingIbl, ambientStateBits(em::VertexColorModulate)),
    key(EngineBuiltinAmbientLightingIblPbr, ambientStateBits(em::VertexColorIgnore)),
    key(EngineBuiltinAmbientLightingIblPbr, ambientStateBits(em::VertexColorModulate)),
  };
}

int LitPasses::imageUsage(ImageSlots& images, const MaterialImageKey& key) const
{
  // the record's usage once loaded (the PBR rename), the key's until then
  const auto* slot = images.slot(images.request(key));
  return slot && slot->adoptedUsage ? *slot->adoptedUsage : key.usage;
}

nvrhi::IBuffer* LitPasses::constantBuffer(Frame& frame, const EngineBindingLayout layout)
{
  // RenderProgs.cpp:752-774: CreateVolatileConstantBufferDesc( rpBufSize,
  // "RenderParams_<layout>", 8192 )
  auto& buffer = m_constantBuffers[layout];
  if (!buffer || buffer->getDesc().maxVersions != m_constantBufferVersions)
  {
    // U1.10: the engine's own helper
    const auto name = fmt::format("RenderParams_{}", int(layout));
    buffer =
      frame.inputs->device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        parmSetBytes(layout), name.c_str(), m_constantBufferVersions));
  }
  return buffer.Get();
}

bool LitPasses::bindingSets(
  Frame& frame,
  const EngineBindingLayout layout,
  const std::vector<nvrhi::BindingLayoutHandle>& layouts,
  const bool pushConstants,
  std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts>& sets,
  std::size_t& count)
{
  using Item = nvrhi::BindingSetItem;
  const auto& inputs = *frame.inputs;
  const auto frameIndex = inputs.frameIndex;
  auto& cache = *inputs.bindingSets;
  auto& device = *inputs.device;

  const auto expected = layout == EngineBindingLayout::DrawInteraction      ? 4u
                        : layout == EngineBindingLayout::AmbientLightingIbl ? 4u
                        : layout == EngineBindingLayout::Texture            ? 3u
                        : layout == EngineBindingLayout::Texgen             ? 3u
                        : layout == EngineBindingLayout::Default            ? 3u
                        : layout == EngineBindingLayout::Legacy             ? 3u
                        : layout == EngineBindingLayout::NormalCube         ? 3u
                        : layout == EngineBindingLayout::Fog                ? 3u
                        : layout == EngineBindingLayout::Blendlight         ? 3u
                        : layout == EngineBindingLayout::PostProcessIngame  ? 2u
                                                                            : 1u;
  if (layouts.size() != expected)
  {
    return false;
  }
  count = layouts.size();

  // a unit's binding: its slot's written levels or its intrinsic's (C28)
  auto& images = *inputs.images;
  const auto unitBinding =
    [&](const std::size_t unit) -> std::optional<ImageSlots::Binding> {
    if (!m_units[unit])
    {
      return std::nullopt;
    }
    auto binding = images.binding(images.request(*m_units[unit]));
    if (!binding.texture)
    {
      return std::nullopt;
    }
    return binding;
  };
  // a set of textures, each unit at its slot, in the layout's item order (nvrhi rule 4)
  const auto slottedSet =
    [&](
      const std::size_t set,
      const std::vector<std::pair<std::size_t, std::uint32_t>>& unitSlots) -> bool {
    auto bindings = std::vector<ImageSlots::Binding>{};
    auto key = BindingSetKey{};
    key.layout = layouts[set].Get();
    for (const auto& [unit, slot] : unitSlots)
    {
      auto binding = unitBinding(unit);
      if (!binding)
      {
        return false;
      }
      key.slots.insert(
        key.slots.end(), binding->versions.begin(), binding->versions.end());
      bindings.push_back(std::move(*binding));
    }
    sets[set] = cache
                  .get(
                    key,
                    frameIndex,
                    [&] {
                      auto desc = nvrhi::BindingSetDesc{};
                      for (std::size_t i = 0; i < bindings.size(); ++i)
                      {
                        desc.addItem(Item::Texture_SRV(
                          unitSlots[i].second,
                          bindings[i].texture,
                          nvrhi::Format::UNKNOWN,
                          bindings[i].subresources));
                      }
                      return device.createBindingSet(desc, layouts[set]);
                    })
                  .Get();
    return sets[set] != nullptr;
  };
  // consecutive units from firstSlot on
  const auto textureSet = [&](
                            const std::size_t set,
                            const std::vector<std::size_t>& units,
                            const std::uint32_t firstSlot) -> bool {
    auto unitSlots = std::vector<std::pair<std::size_t, std::uint32_t>>{};
    for (std::size_t i = 0; i < units.size(); ++i)
    {
      unitSlots.emplace_back(units[i], firstSlot + std::uint32_t(i));
    }
    return slottedSet(set, unitSlots);
  };

  // set 0: the renderparms, pushed or a volatile constant buffer
  // (GetCurrentBindingLayout, RenderBackend_NVRHI.cpp:576-584); LP5.2a (C47):
  // POST_PROCESS_INGAME's also holds the units 0-2 at t0-t2 (:1464-1512)
  {
    auto key = BindingSetKey{};
    key.layout = layouts[0].Get();
    nvrhi::IBuffer* buffer = nullptr;
    if (!pushConstants)
    {
      buffer = constantBuffer(frame, layout);
      if (!buffer)
      {
        return false;
      }
      key.resources = {buffer};
    }
    auto units = std::vector<ImageSlots::Binding>{};
    if (layout == EngineBindingLayout::PostProcessIngame)
    {
      for (std::size_t unit = 0; unit < 3; ++unit)
      {
        auto binding = unitBinding(unit);
        if (!binding)
        {
          return false;
        }
        key.slots.insert(
          key.slots.end(), binding->versions.begin(), binding->versions.end());
        units.push_back(std::move(*binding));
      }
    }
    const auto bytes = parmSetBytes(layout);
    sets[0] = cache
                .get(
                  key,
                  frameIndex,
                  [&] {
                    auto desc = nvrhi::BindingSetDesc{};
                    desc.addItem(
                      pushConstants ? Item::PushConstants(0, bytes)
                                    : Item::ConstantBuffer(0, buffer));
                    for (std::size_t unit = 0; unit < units.size(); ++unit)
                    {
                      desc.addItem(Item::Texture_SRV(
                        std::uint32_t(unit),
                        units[unit].texture,
                        nvrhi::Format::UNKNOWN,
                        units[unit].subresources));
                    }
                    return device.createBindingSet(desc, layouts[0]);
                  })
                .Get();
    if (!sets[0])
    {
      return false;
    }
  }

  switch (layout)
  {
  case EngineBindingLayout::Texture:
  case EngineBindingLayout::Texgen:
  case EngineBindingLayout::Default:
  case EngineBindingLayout::Legacy: {
    // TEXTURE: t0 the unit 0 image, s0 its own sampler (SC238, SC269); LP5.2a: TEXGEN
    // the same, LP5.2b: DEFAULT and LEGACY too (RenderBackend_NVRHI.cpp:586-623), whose
    // unit 0 is a cube: a cube image's repeat is TR_CLAMP from its load on
    // (Image_load.cpp:587-591)
    if (!textureSet(1, {0}, 0))
    {
      return false;
    }
    const auto& key = *m_units[0];
    const auto* slot = images.slot(images.request(key));
    const auto format =
      slot && slot->adoptedFormat ? *slot->adoptedFormat : engine_image::FormatRgba8;
    const auto repeat =
      key.cube != engine_image::Cube2D ? engine_image::RepeatClamp : key.repeat;
    auto* sampler = inputs.samplers->imageSampler(device, format, key.filter, repeat)
                    | kdl::value_or(nullptr);
    if (!sampler)
    {
      return false;
    }
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[2].Get();
    samplerKey.samplers = 2;
    samplerKey.resources = {sampler};
    sets[2] =
      cache
        .get(
          samplerKey,
          frameIndex,
          [&] {
            return device.createBindingSet(
              nvrhi::BindingSetDesc{}.addItem(Item::Sampler(0, sampler)), layouts[2]);
          })
        .Get();
    return sets[2] != nullptr;
  }
  case EngineBindingLayout::DrawInteraction: {
    // DRAW_INTERACTION (RenderBackend_NVRHI.cpp:916-996): t0-t2 the units 0-2, t3-t4
    // the units 3-4, s0 AnisotropicWrap and s1 LinearBorder (D68)
    if (!textureSet(1, {0, 1, 2}, 0) || !textureSet(2, {3, 4}, 3))
    {
      return false;
    }
    const auto* common = inputs.samplers->common(device)
                         | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));
    if (!common)
    {
      return false;
    }
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[3].Get();
    samplerKey.samplers = 1;
    sets[3] = cache
                .get(
                  samplerKey,
                  frameIndex,
                  [&] {
                    return device.createBindingSet(
                      nvrhi::BindingSetDesc{}
                        .addItem(Item::Sampler(0, common->anisotropicWrap))
                        .addItem(Item::Sampler(1, common->linearBorder)),
                      layouts[3]);
                  })
                .Get();
    return sets[3] != nullptr;
  }
  case EngineBindingLayout::AmbientLightingIbl: {
    // LP4.2a (C34, C35): AMBIENT_LIGHTING_IBL (RenderBackend_NVRHI.cpp:698-788): t0-t2
    // the units 0-2; t3 t4 t7 t8 t9 t10 t12 t13 t14 the units of the same number; s0
    // AnisotropicWrap (compare-enabled, D68; PointWrap only under R_UsePixelatedLook) and
    // s1 LinearClamp
    for (const auto set : {std::size_t{1}, std::size_t{2}})
    {
      auto unitSlots = std::vector<std::pair<std::size_t, std::uint32_t>>{};
      for (const auto& binding : AmbientUnits)
      {
        if (binding.set == set)
        {
          unitSlots.emplace_back(binding.unit, binding.slot);
        }
      }
      if (!slottedSet(set, unitSlots))
      {
        return false;
      }
    }
    const auto* common = inputs.samplers->common(device)
                         | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));
    if (!common)
    {
      return false;
    }
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[3].Get();
    samplerKey.samplers = 3;
    samplerKey.resources = {common->anisotropicWrap.Get(), common->linearClamp.Get()};
    sets[3] = cache
                .get(
                  samplerKey,
                  frameIndex,
                  [&] {
                    return device.createBindingSet(
                      nvrhi::BindingSetDesc{}
                        .addItem(Item::Sampler(0, common->anisotropicWrap))
                        .addItem(Item::Sampler(1, common->linearClamp)),
                      layouts[3]);
                  })
                .Get();
    return sets[3] != nullptr;
  }
  case EngineBindingLayout::NormalCube: {
    // LP5.2b (C48): NORMAL_CUBE (RenderBackend_NVRHI.cpp:1677-1734): t0 the unit 0 cube,
    // t1 the unit 1 bump image, s0 LinearWrap, which carries the Comparison reduction of
    // CommonPasses' leaked desc (D68, mirrored: P6.19's validation answer is recorded on
    // mirror.json M-STAGE-08); PointWrap only under R_UsePixelatedLook
    if (!textureSet(1, {0, 1}, 0))
    {
      return false;
    }
    const auto* common = inputs.samplers->common(device)
                         | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));
    if (!common)
    {
      return false;
    }
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[2].Get();
    samplerKey.samplers = 5;
    samplerKey.resources = {common->linearWrap.Get()};
    sets[2] =
      cache
        .get(
          samplerKey,
          frameIndex,
          [&] {
            return device.createBindingSet(
              nvrhi::BindingSetDesc{}.addItem(Item::Sampler(0, common->linearWrap)),
              layouts[2]);
          })
        .Get();
    return sets[2] != nullptr;
  }
  case EngineBindingLayout::Fog:
  case EngineBindingLayout::Blendlight: {
    // LP5.3 (C49): FOG (RenderBackend_NVRHI.cpp:1286-1329): t0 t1 the units 0 and 1 (_fog
    // and _fogEnter), s0 = s1 = LinearClamp; BLENDLIGHT (:1378-1418): t0 the stage image,
    // t1 the falloff image, s0 LinearBorder (border ( 0, 0, 0, 1 ), no compare)
    if (!textureSet(1, {0, 1}, 0))
    {
      return false;
    }
    const auto* common = inputs.samplers->common(device)
                         | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));
    if (!common)
    {
      return false;
    }
    const auto fog = layout == EngineBindingLayout::Fog;
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[2].Get();
    samplerKey.samplers = fog ? 6 : 7;
    samplerKey.resources = {fog ? common->linearClamp.Get() : common->linearBorder.Get()};
    sets[2] = cache
                .get(
                  samplerKey,
                  frameIndex,
                  [&] {
                    auto desc = nvrhi::BindingSetDesc{};
                    if (fog)
                    {
                      desc.addItem(Item::Sampler(0, common->linearClamp))
                        .addItem(Item::Sampler(1, common->linearClamp));
                    }
                    else
                    {
                      desc.addItem(Item::Sampler(0, common->linearBorder));
                    }
                    return device.createBindingSet(desc, layouts[2]);
                  })
                .Get();
    return sets[2] != nullptr;
  }
  case EngineBindingLayout::PostProcessIngame: {
    // LP5.2a (C47): s0 LinearClamp (RenderBackend_NVRHI.cpp:1464-1512; PointClamp only
    // under R_UsePixelatedLook)
    const auto* common = inputs.samplers->common(device)
                         | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));
    if (!common)
    {
      return false;
    }
    auto samplerKey = BindingSetKey{};
    samplerKey.layout = layouts[1].Get();
    samplerKey.samplers = 4;
    samplerKey.resources = {common->linearClamp.Get()};
    sets[1] =
      cache
        .get(
          samplerKey,
          frameIndex,
          [&] {
            return device.createBindingSet(
              nvrhi::BindingSetDesc{}.addItem(Item::Sampler(0, common->linearClamp)),
              layouts[1]);
          })
        .Get();
    return sets[1] != nullptr;
  }
  default:
    return true;
  }
}

LitPasses::DrawOutcome LitPasses::draw(
  Frame& frame,
  const int program,
  const Chunk& chunk,
  const std::uint64_t stateBits,
  const float slopeBias,
  const float depthBias,
  const LightTriangles* lightTriangles)
{
  // DrawElementsWithCounters (RenderBackend_NVRHI.cpp:315-565)
  const auto& inputs = *frame.inputs;
  if (!engineProgram(program))
  {
    return DrawOutcome::Disabled;
  }

  // the vertex and index buffers: the chunk's arena ranges (SC220)
  const auto placement = inputs.arenas->placement(chunk.key);
  auto* vertexBuffer =
    placement ? inputs.arenas->vertexBuffer(placement->vertexArena) : nullptr;
  auto* indexBuffer =
    placement ? inputs.arenas->indexBuffer(placement->indexArena) : nullptr;
  if (!placement || !vertexBuffer || !indexBuffer || placement->indexCount == 0)
  {
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    m_stats.retrigger = true;
    return DrawOutcome::Skipped;
  }
  const auto outcome = drawGeometry(
    frame,
    program,
    DrawGeometry{
      vertexBuffer,
      indexBuffer,
      placement->indexCount,
      std::uint32_t(placement->indexes.offset / 2),
      std::uint32_t(placement->vertices.offset / 32)},
    stateBits,
    slopeBias,
    depthBias,
    lightTriangles);
  // LP7.2 (D129): an instance's draw reads its shared placement
  if (outcome == DrawOutcome::Drawn && placement->shared)
  {
    ++m_stats.modelDraws;
  }
  return outcome;
}

LitPasses::DrawOutcome LitPasses::drawGeometry(
  Frame& frame,
  const int program,
  const DrawGeometry& geometry,
  const std::uint64_t stateBits,
  const float slopeBias,
  const float depthBias,
  const LightTriangles* lightTriangles)
{
  const auto& inputs = *frame.inputs;
  const auto* builtin = engineProgram(program);
  if (!builtin)
  {
    return DrawOutcome::Disabled;
  }
  const auto layout = builtin->bindingLayout;
  auto* vertexBuffer = geometry.vertexBuffer;
  auto* indexBuffer = geometry.indexBuffer;

  // the pipeline (RenderBackend_NVRHI.cpp:459-461): the key's bias only with
  // GLS_POLYGON_OFFSET, the only state GetRenderState reads it in (M-PASS-DIFF-01)
  const auto offset = (stateBits & gls::PolygonOffset) != 0;
  const auto key = PipelineKey{
    PipelineRecipe::Draw,
    program,
    stateBits,
    offset ? int(depthBias) : 0,
    offset ? slopeBias : 0.0f,
    frame.formats};
  if (m_logDraws)
  {
    m_requestedKeys.insert(key);
  }
  const auto& lookup = inputs.pipelines->graphics(key);
  if (lookup.state == GraphicsPipelineLookup::State::Pending)
  {
    // D77: skipped, and the compile thread triggers a frame when it is made (LP2.2b:
    // its onReady is the trigger, so the frame does not re-trigger itself for it)
    ++m_stats.pendingPipelines;
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    return DrawOutcome::Skipped;
  }
  if (lookup.state == GraphicsPipelineLookup::State::Failed || !lookup.pipeline)
  {
    // D9: the pass is disabled; programReport() says why
    if (m_stats.disabledReason.empty())
    {
      m_stats.disabledReason = lookup.error;
    }
    return DrawOutcome::Disabled;
  }
  auto* pipeline = lookup.pipeline.Get();
  const auto layouts = lookup.bindingLayouts;
  const auto pushConstants = pushesConstants(layouts);

  // the binding sets of the layout
  auto sets = std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts>{};
  auto setCount = std::size_t{0};
  if (!bindingSets(frame, layout, layouts, pushConstants, sets, setCount))
  {
    // D77: a texture with no written level yet (neither the slot nor its intrinsic)
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    m_stats.retrigger = true;
    return DrawOutcome::Skipped;
  }

  // LP3.1: pass 6's tile viewport, else the view's
  const auto viewport = frame.viewport ? *frame.viewport : engineViewport(frame.view);
  auto changeState = !m_state.valid || m_state.pipeline != pipeline
                     || m_state.framebuffer != frame.framebuffer
                     || m_state.vertexBuffer != vertexBuffer
                     || m_state.indexBuffer != indexBuffer || m_state.setCount != setCount
                     || !(m_state.viewport == viewport) || m_state.scissor != m_scissor;
  for (std::size_t i = 0; i < setCount && !changeState; ++i)
  {
    changeState = m_state.sets[i] != sets[i];
  }

  // CommitConstantBuffer (RenderProgs_NVRHI.cpp:536-565): the subset written only when
  // the layout's uniforms changed or the layout did; on Vulkan a pushed layout's change,
  // and a written buffer's new layout, reset the state
  const auto layoutChanged = layout != m_state.layout;
  auto data = std::array<float, MaxParmSetFloat4s * 4>{};
  if (pushConstants)
  {
    changeState = changeState || m_parms.changed(layout) || layoutChanged;
  }
  else if (m_parms.changed(layout) || layoutChanged)
  {
    auto* buffer = constantBuffer(frame, layout);
    auto& writes = m_frameWrites[layout];
    if (!buffer || writes >= writeBudget())
    {
      // D76: never past the frame's budget, so NVRHI's maxVersions error is never met
      ++m_stats.skippedDraws;
      return DrawOutcome::Overflowed;
    }
    const auto floats = m_parms.select(layout, data);
    inputs.commandList->writeBuffer(buffer, data.data(), floats * sizeof(float));
    ++writes;
    ++m_stats.constantWrites;
    m_stats.constantBytes += floats * sizeof(float);
    m_parms.clearChanged(layout);
    changeState = changeState || layoutChanged;
  }

  if (changeState)
  {
    auto state = nvrhi::GraphicsState{};
    state.pipeline = pipeline;
    state.framebuffer = frame.framebuffer;
    for (std::size_t i = 0; i < setCount; ++i)
    {
      state.bindings.push_back(sets[i]);
    }
    state.indexBuffer = nvrhi::IndexBufferBinding{}
                          .setBuffer(indexBuffer)
                          .setFormat(nvrhi::Format::R16_UINT)
                          .setOffset(0);
    state.vertexBuffers = {
      nvrhi::VertexBufferBinding{}.setBuffer(vertexBuffer).setSlot(0).setOffset(0)};
    state.viewport.addViewport(viewport);
    state.viewport.addScissorRect(m_scissor);
    inputs.commandList->setGraphicsState(state);

    // CommitPushConstants (:567-579)
    if (pushConstants)
    {
      const auto floats = m_parms.select(layout, data);
      inputs.commandList->setPushConstants(data.data(), floats * sizeof(float));
      m_stats.pushConstantBytes += floats * sizeof(float);
      m_parms.clearChanged(layout);
    }

    m_state.valid = true;
    m_state.pipeline = pipeline;
    m_state.framebuffer = frame.framebuffer;
    m_state.vertexBuffer = vertexBuffer;
    m_state.indexBuffer = indexBuffer;
    m_state.sets = sets;
    m_state.setCount = setCount;
    m_state.viewport = viewport;
    m_state.scissor = m_scissor;
  }

  // the draw command: the chunk's indexes, its first vertex the base
  auto args = nvrhi::DrawArguments{};
  args.vertexCount = geometry.indexCount;
  args.instanceCount = 1;
  args.startIndexLocation = geometry.firstIndex;
  args.startVertexLocation = geometry.baseVertex;
  if (lightTriangles && lightTriangles->state == LightTriangles::State::Runs)
  {
    // DX.2: the engine draws its light triangles' index subset in one draw; the runs of
    // that subset are the same triangles in the same order, a draw call each
    const auto first = args.startIndexLocation;
    for (const auto& run : lightTriangles->runs)
    {
      args.vertexCount = run.indexCount;
      args.startIndexLocation = first + run.firstIndex;
      inputs.commandList->drawIndexed(args);
      ++m_stats.lightTriangleRuns;
    }
    ++m_stats.lightTriangleDraws;
  }
  else
  {
    inputs.commandList->drawIndexed(args);
  }

  m_state.layout = layout;
  return DrawOutcome::Drawn;
}

void LitPasses::log(
  const Frame&,
  const LitDrawRecord::Pass pass,
  const Chunk& chunk,
  const int program,
  const std::uint64_t state)
{
  if (!m_logDraws)
  {
    return;
  }
  auto record = LitDrawRecord{};
  record.pass = pass;
  record.light = pass == LitDrawRecord::Pass::Interaction ? m_logLight : NoPreviewId;
  record.list = m_logList;
  record.chunk = chunk.key;
  record.program = program;
  record.state = state;
  record.scissor = m_scissor;
  m_drawLog.push_back(std::move(record));
}

const FrontendLists& LitPasses::lists() const
{
  return m_lists;
}

const ViewProbes& LitPasses::viewProbes() const
{
  return m_viewProbes;
}

const LitFrameStats& LitPasses::lastStats() const
{
  return m_stats;
}

std::uint64_t LitPasses::volatileBufferBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& [layout, buffer] : m_constantBuffers)
  {
    if (buffer)
    {
      bytes += buffer->getDesc().byteSize * buffer->getDesc().maxVersions;
    }
  }
  return bytes;
}

std::uint64_t LitPasses::shadowAtlasBytes() const
{
  if (!m_atlas)
  {
    return 0;
  }
  const auto& desc = m_atlas->getDesc();
  return std::uint64_t(desc.width) * std::uint64_t(desc.height) * sizeof(float);
}

const std::vector<ShadowLightRecord>& LitPasses::shadowLights() const
{
  return m_shadowRecords;
}

nvrhi::ITexture* LitPasses::shadowAtlas() const
{
  return m_atlas.Get();
}

const RenderParms& LitPasses::parms() const
{
  return m_parms;
}

void LitPasses::releaseAll()
{
  // LP3.1 (C59): the shadow atlas goes with the session's device objects (teardown step
  // 2), so that the stop row's allocations are back to the start's
  m_atlasFramebuffer = nullptr;
  m_atlas = nullptr;
  m_atlasFailed = false;
  m_atlasOccupied.clear();
  m_shadowRecords.clear();
  m_shadowRecordOf.clear();
  m_clearQuadVertices = nullptr;
  m_clearQuadIndexes = nullptr;
  m_constantBuffers.clear();
  m_lists = FrontendLists{};
  m_stageOrder.clear();
  m_materialPrograms.clear();
  m_viewProbes = ViewProbes{};
  m_listsKey.reset();
  m_registers.clear();
  m_lightTriangles.clear();
  m_state = DrawState{};
  m_units = {};
  m_fogCubeVertices = nullptr;
  m_fogCubeIndexes = nullptr;
  m_drawLog.clear();
  m_requestedKeys.clear();
}

void LitPasses::setConstantBufferVersionsForTesting(const std::uint32_t versions)
{
  m_constantBufferVersions = std::max(versions, 1u);
}

void LitPasses::setLightTriangleCullForTesting(const bool on)
{
  m_lightTriangleCull = on;
}

void LitPasses::setSkipInteractionsForTesting(const bool on)
{
  m_skipInteractions = on;
}

void LitPasses::setSkipShaderPassesForTesting(const bool on)
{
  m_skipShaderPasses = on;
}

bool LitPasses::skipShaderPassesForTesting() const
{
  return m_skipShaderPasses;
}

void LitPasses::setSkipShadowsForTesting(const bool on)
{
  // the lists' key holds it: they are built again when it changes
  m_skipShadows = on;
}

void LitPasses::setDrawLogForTesting(const bool on)
{
  m_logDraws = on;
  m_drawLog.clear();
  m_requestedKeys.clear();
}

const std::vector<LitDrawRecord>& LitPasses::drawLogForTesting() const
{
  return m_drawLog;
}

const std::set<PipelineKey, PipelineKeyLess>& LitPasses::requestedKeysForTesting() const
{
  return m_requestedKeys;
}

} // namespace tb::lit
