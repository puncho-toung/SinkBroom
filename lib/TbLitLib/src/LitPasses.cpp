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

#include "lit/EngineBuiltins.h"
#include "lit/EnginePipelineState.h"
#include "lit/ImageCache.h"
#include "lit/LitConfig.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>

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
  m_stats.unconverged = m_lists.pending;
  m_stats.waitsForTable = m_lists.pending;

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

  if (inputs.stopRequested && inputs.stopRequested())
  {
    m_stats.stoppedEarly = true;
    return m_stats;
  }
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
  const auto* table = inputs.materials.get();
  auto key = ListsKey{};
  key.camera = inputs.camera;
  key.width = view.width;
  key.height = view.height;
  key.chunks = inputs.chunks;
  key.chunksVersion = inputs.chunksVersion;
  key.entities = inputs.entities;
  key.lights = inputs.lights.get();
  key.lightsVersion = inputs.lights ? inputs.lights->version : 0;
  key.registry = table ? table->registry : 0;
  const auto tableVersion = table ? table->version : 0;
  if (m_listsKey == key && !(m_lists.pending && tableVersion != m_listsTableVersion))
  {
    return;
  }
  m_listsKey = key;
  m_listsTableVersion = tableVersion;
  m_stats.listsRebuilt = true;

  if (!table)
  {
    // no material service: nothing to draw, and nothing to wait for
    m_lists = FrontendLists{};
    return;
  }

  auto frontend = FrontendInputs{};
  frontend.view = view;
  frontend.chunks = inputs.chunks;
  frontend.entities = inputs.entities;
  frontend.lights = inputs.lights.get();
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
  m_registers.beginBuild();
  m_lists = buildFrontendLists(frontend);
  m_registers.endBuild();
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
      case PrepassProgram::TextureColor: {
        program = EngineBuiltinTextureVertexColor;
        const auto vertexColor = engineVertexColorParms(em::VertexColorIgnore);
        m_parms.set(RenderParm::VertexColorModulate, vertexColor[0]);
        m_parms.set(RenderParm::VertexColorAdd, vertexColor[1]);
        // bind the texture: unit 0 is the stage's image
        const auto& stage = surface->material->stages[std::size_t(prepassDraw.stage)];
        if (!stage.image)
        {
          continue;
        }
        m_units[0] = *stage.image;
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
  // DrawInteractionsPass (RenderBackend.cpp:3984-4171) for the texture lights
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
  auto overflowing = false;
  for (const auto& light : m_lists.lights)
  {
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

    // GL_State( GLS_DEFAULT ), ALPHA_TEST 0, ResetViewportAndScissorToDefaultCamera
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
      if (!renderInteractions(frame, light, *surfaces, list, depthFunc))
      {
        m_stats.interactionsDisabled = true;
        return;
      }
    }
    ++m_stats.lightsDrawn;
  }
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
  const auto stateBits = gls::SrcBlendOne | gls::DstBlendOne | gls::DepthMask | depthFunc
                         | gls::StencilFuncAlways;

  const auto& shader = *light.shader;
  const auto& record = *light.record;
  const auto& lightRegs = light.registers;
  const auto noSpecular = record.light.parms.noSpecular;
  const auto ordered = engineOrderInteractionSurfaces(surfaces);
  auto& images = *frame.inputs->images;
  const auto usage =
    ImageUsage{[&](const MaterialImageKey& key) { return imageUsage(images, key); }};

  // drawInteraction_t inter = {}: the images null; the matrices, which the engine leaves
  // indeterminate, zero (mirror.json M-PASS-DIFF-02)
  auto inter = InteractionState{};
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

    // texture 3 the light falloff, texture 4 the light projection (a texture light)
    if (!lightStage.image)
    {
      // a cinematic stage: no image to bind (mirror.json M-PASS-GUARD-01)
      continue;
    }
    m_units[3] = record.derived.shader.falloffImage;
    m_units[4] = *lightStage.image;

    // USER0-2
    const auto user = engineMathLightParms(record.light.parms.mathLight, false);
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

        const auto program = interaction.program == InteractionProgram::PbrInteraction
                               ? EngineBuiltinPbrInteraction
                               : EngineBuiltinInteraction;
        switch (draw(frame, program, *surface->chunk, stateBits, 0.0f, 0.0f))
        {
        case DrawOutcome::Drawn:
          ++m_stats.interactionDraws;
          log(
            frame, LitDrawRecord::Pass::Interaction, *surface->chunk, program, stateBits);
          break;
        case DrawOutcome::Disabled:
          disabled = true;
          break;
        case DrawOutcome::Skipped:
        case DrawOutcome::Overflowed:
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
      else if (const auto pending = pendingSurfaceInteraction(diffuseColor, usage))
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

int LitPasses::imageUsage(ImageSlots& images, const MaterialImageKey& key) const
{
  // the record's usage once loaded (the PBR rename), the key's until then
  const auto* slot = images.slot(images.request(key));
  return slot && slot->adopted ? slot->adopted->name.usage : key.usage;
}

nvrhi::IBuffer* LitPasses::constantBuffer(Frame& frame, const EngineBindingLayout layout)
{
  // RenderProgs.cpp:752-774: CreateVolatileConstantBufferDesc( rpBufSize,
  // "RenderParams_<layout>", 8192 )
  auto& buffer = m_constantBuffers[layout];
  if (!buffer || buffer->getDesc().maxVersions != m_constantBufferVersions)
  {
    auto desc = nvrhi::BufferDesc{};
    desc.byteSize = parmSetBytes(layout);
    desc.isConstantBuffer = true;
    desc.isVolatile = true;
    desc.maxVersions = m_constantBufferVersions;
    desc.debugName = fmt::format("RenderParams_{}", int(layout));
    buffer = frame.inputs->device->createBuffer(desc);
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

  const auto expected = layout == EngineBindingLayout::DrawInteraction ? 4u
                        : layout == EngineBindingLayout::Texture       ? 3u
                                                                       : 1u;
  if (layouts.size() != expected)
  {
    return false;
  }
  count = layouts.size();

  // set 0: the renderparms, pushed or a volatile constant buffer
  // (GetCurrentBindingLayout, RenderBackend_NVRHI.cpp:576-584)
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
                    return device.createBindingSet(desc, layouts[0]);
                  })
                .Get();
    if (!sets[0])
    {
      return false;
    }
  }

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
  const auto textureSet = [&](
                            const std::size_t set,
                            const std::vector<std::size_t>& units,
                            const std::uint32_t firstSlot) -> bool {
    auto bindings = std::vector<ImageSlots::Binding>{};
    auto key = BindingSetKey{};
    key.layout = layouts[set].Get();
    for (const auto unit : units)
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
                      for (std::uint32_t i = 0; i < bindings.size(); ++i)
                      {
                        desc.addItem(Item::Texture_SRV(
                          firstSlot + i,
                          bindings[i].texture,
                          nvrhi::Format::UNKNOWN,
                          bindings[i].subresources));
                      }
                      return device.createBindingSet(desc, layouts[set]);
                    })
                  .Get();
    return sets[set] != nullptr;
  };

  switch (layout)
  {
  case EngineBindingLayout::Texture: {
    // TEXTURE: t0 the unit 0 image, s0 its own sampler (SC238, SC269)
    if (!textureSet(1, {0}, 0))
    {
      return false;
    }
    const auto& key = *m_units[0];
    const auto* slot = images.slot(images.request(key));
    const auto format = slot && slot->adopted && slot->adopted->image
                          ? slot->adopted->image->format
                          : engine_image::FormatRgba8;
    auto* sampler = inputs.samplers->imageSampler(device, format, key.filter, key.repeat)
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
  const float depthBias)
{
  // DrawElementsWithCounters (RenderBackend_NVRHI.cpp:315-565)
  const auto& inputs = *frame.inputs;
  const auto* builtin = engineBuiltin(program);
  if (!builtin)
  {
    return DrawOutcome::Disabled;
  }
  const auto layout = builtin->bindingLayout;

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
    // D77: skipped, and the compile thread triggers a frame when it is made
    ++m_stats.pendingPipelines;
    ++m_stats.skippedDraws;
    m_stats.unconverged = true;
    m_stats.retrigger = true;
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

  const auto viewport = engineViewport(frame.view);
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
  args.vertexCount = placement->indexCount;
  args.instanceCount = 1;
  args.startIndexLocation = std::uint32_t(placement->indexes.offset / 2);
  args.startVertexLocation = std::uint32_t(placement->vertices.offset / 32);
  inputs.commandList->drawIndexed(args);

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

const LitFrameStats& LitPasses::lastStats() const
{
  return m_stats;
}

const RenderParms& LitPasses::parms() const
{
  return m_parms;
}

void LitPasses::releaseAll()
{
  m_constantBuffers.clear();
  m_lists = FrontendLists{};
  m_listsKey.reset();
  m_registers.clear();
  m_state = DrawState{};
  m_units = {};
  m_drawLog.clear();
  m_requestedKeys.clear();
}

void LitPasses::setConstantBufferVersionsForTesting(const std::uint32_t versions)
{
  m_constantBufferVersions = std::max(versions, 1u);
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
