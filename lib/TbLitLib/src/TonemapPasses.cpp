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
// Parts of this file transcribe, altered for SinkBroom, code of RBDOOM-3-BFG
// (neo/renderer/Passes/TonemapPass.cpp, Passes/TonemapPass.h, Passes/TonemapPass_cb.h,
// Passes/CommonPasses.cpp, Passes/CommonPasses.h and renderer/RenderBackend.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// The NVIDIA Donut code the engine adapted there (TonemapPass, CommonPasses), Copyright
// (c) 2014-2021 NVIDIA CORPORATION, is under the MIT licence. That code is also subject
// to the additional terms that follow the GNU General Public License in
// lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "TonemapPasses.h"

#include "lit/EngineBuiltins.h"

#include "kd/result.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cmath>

namespace tb::lit
{
namespace
{

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

// C14: the passes LP2.2b declares over FrameResources
PassDesc histogramPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "histogram";
  pass.reads = {std::string{frame_resource::CurrentRenderHdr}};
  pass.writes = {std::string{frame_resource::HistogramBuffer}};
  return pass;
}

PassDesc exposurePassDesc()
{
  auto pass = PassDesc{};
  pass.name = "exposure";
  pass.reads = {std::string{frame_resource::HistogramBuffer}};
  pass.writes = {std::string{frame_resource::ExposureBuffer}};
  pass.lifetime = PassLifetime::Session;
  return pass;
}

PassDesc tonemapPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "tonemap";
  pass.reads = {
    std::string{frame_resource::CurrentRenderHdr},
    std::string{frame_resource::ExposureBuffer}};
  pass.writes = {
    std::string{frame_resource::LdrFramebuffer},
    std::string{frame_resource::CurrentRenderLdr}};
  return pass;
}

PassDesc blitPassDesc()
{
  auto pass = PassDesc{};
  pass.name = "blit";
  pass.reads = {std::string{frame_resource::CurrentRenderLdr}};
  pass.writes = {std::string{frame_resource::FrameTarget}};
  return pass;
}

// one-set layouts only: the tonemap passes' and CommonPasses' blit layout
bool oneSet(const std::vector<nvrhi::BindingLayoutHandle>& layouts)
{
  return layouts.size() == 1 && layouts.front();
}

} // namespace

nvrhi::Viewport engineHistogramViewport(
  const std::uint32_t width, const std::uint32_t height)
{
  // viewDef->viewport is (0, 0, width - 1, height - 1), zmin 0, zmax 1
  return nvrhi::Viewport{
    0.0f, float(width) - 1.0f, 0.0f, float(height) - 1.0f, 0.0f, 1.0f};
}

EngineToneMappingConstants engineHistogramConstants(
  const std::uint32_t width,
  const std::uint32_t height,
  const EngineTonemapDefaults& defaults)
{
  auto viewportState = nvrhi::ViewportState{};
  viewportState.addViewportAndScissorRect(engineHistogramViewport(width, height));
  const auto& scissor = viewportState.scissorRects[0];

  auto constants = EngineToneMappingConstants{};
  constants.logLuminanceScale =
    1.0f / (defaults.hdrMinLuminance - defaults.hdrMaxLuminance);
  constants.logLuminanceBias = -defaults.hdrMinLuminance * constants.logLuminanceScale;
  constants.viewOrigin = {scissor.minX, scissor.minY};
  constants.viewSize = {scissor.maxX - scissor.minX, scissor.maxY - scissor.minY};
  constants.sourceSlice = 0;
  return constants;
}

std::array<std::uint32_t, 2> engineHistogramGroups(
  const EngineToneMappingConstants& constants)
{
  return {
    std::uint32_t((constants.viewSize[0] + 15) / 16),
    std::uint32_t((constants.viewSize[1] + 15) / 16)};
}

EngineToneMappingConstants engineExposureConstants(
  const EngineTonemapDefaults& defaults, const float frameTime)
{
  auto constants = EngineToneMappingConstants{};
  constants.logLuminanceScale = defaults.exposureLogMax - defaults.exposureLogMin;
  constants.logLuminanceBias = defaults.exposureLogMin;
  constants.histogramLowPercentile =
    std::min(0.99f, std::max(0.0f, defaults.histogramLowPercentile));
  constants.histogramHighPercentile = std::min(
    1.0f, std::max(constants.histogramLowPercentile, defaults.histogramHighPercentile));
  constants.eyeAdaptationSpeedUp = defaults.adaptationRate;
  constants.eyeAdaptationSpeedDown = defaults.adaptationRate / 2.0f;
  constants.minAdaptedLuminance = defaults.hdrMinLuminance;
  constants.maxAdaptedLuminance = defaults.hdrMaxLuminance;
  constants.frameTime = frameTime;
  return constants;
}

nvrhi::Viewport engineTonemapViewport(
  const std::uint32_t width, const std::uint32_t height)
{
  return nvrhi::Viewport{0.0f, float(width), 0.0f, float(height), 0.0f, 1.0f};
}

EngineToneMappingConstants engineTonemapConstants(const EngineTonemapDefaults& defaults)
{
  auto constants = EngineToneMappingConstants{};
  constants.exposureScale = std::exp2(defaults.exposure);
  constants.whitePointInvSquared = 1.0f / std::pow(defaults.whitePoint, 2.0f);
  constants.minAdaptedLuminance = defaults.hdrMinLuminance;
  constants.maxAdaptedLuminance = defaults.hdrMaxLuminance;
  constants.sourceSlice = 0;
  // enableColorLUT && colorLutSize > 0 is false: no LUT was given
  constants.colorLutTextureSize = {0.0f, 0.0f};
  constants.colorLutTextureSizeInv = {0.0f, 0.0f};
  return constants;
}

EngineBlitConstants engineBlitConstants()
{
  auto constants = EngineBlitConstants{};
  constants.sourceOrigin = {0.0f, 0.0f};
  constants.sourceSize = {1.0f, 1.0f};
  constants.targetOrigin = {0.0f, 0.0f};
  constants.targetSize = {1.0f, 1.0f};
  constants.sharpenFactor = 0.0f;
  return constants;
}

nvrhi::Viewport engineBlitViewport(const std::uint32_t width, const std::uint32_t height)
{
  return nvrhi::Viewport{float(width), float(height)};
}

bool TonemapFrameStats::pending() const
{
  return pendingPipelines > 0 || lutPending;
}

TonemapFrameStats TonemapPasses::record(const TonemapFrameInputs& inputs)
{
  m_stats = TonemapFrameStats{};
  if (!inputs.commandList || !inputs.target)
  {
    return m_stats;
  }
  if (
    !inputs.hdrDrawn || !inputs.device || !inputs.frame || !inputs.pipelines
    || !inputs.images || !inputs.bindingSets || !inputs.samplers
    || !inputs.targetFramebuffer)
  {
    // M-TONE-DIFF-03: the passes of a black HDR target give (0, 0, 0, 1)
    clearTarget(inputs);
    return m_stats;
  }

  using Item = nvrhi::BindingSetItem;
  auto& device = *inputs.device;
  auto& commandList = *inputs.commandList;
  auto& frame = *inputs.frame;
  auto& pipelines = *inputs.pipelines;
  auto& cache = *inputs.bindingSets;
  const auto frameIndex = inputs.frameIndex;
  const auto extent = frame.extent();
  const auto generation = frame.renderTargetGeneration();

  // the pipelines (D71) and what the passes bind (C14)
  const auto histogram = pipelines.compute(EngineBuiltinHistogram);
  const auto exposure = pipelines.compute(EngineBuiltinExposure);
  const auto tonemap = pipelines.graphics(tonemapPipelineKey(frame.depthFormat()));
  const auto blit = pipelines.graphics(blitPipelineKey(inputs.target->getDesc().format));
  const auto check = [&]<typename L>(const L& lookup) {
    if (lookup.state == L::State::Pending)
    {
      ++m_stats.pendingPipelines;
    }
    else if (lookup.state == L::State::Failed || !lookup.pipeline)
    {
      // D9: the compile thread disabled the program and reported it once
      m_stats.disabled = true;
      if (m_stats.disabledReason.empty())
      {
        m_stats.disabledReason = lookup.error;
      }
    }
    else if (!oneSet(lookup.bindingLayouts))
    {
      m_stats.disabled = true;
      if (m_stats.disabledReason.empty())
      {
        m_stats.disabledReason = "a tonemap pass's binding layouts are not one set";
      }
    }
  };
  check(histogram);
  check(exposure);
  check(tonemap);
  check(blit);

  // the colour LUT is _black (TonemapPass.cpp:99) through its slot (C28)
  auto& images = *inputs.images;
  const auto lut = images.binding(images.request(intrinsicImageKey("_black")));
  m_stats.lutPending = lut.texture == nullptr;
  const auto* common = inputs.samplers->common(device)
                       | kdl::value_or(static_cast<const SamplerSet::Common*>(nullptr));

  const auto histogramPass =
    frame.resolve(histogramPassDesc()) | kdl::value_or(PassResources{});
  const auto exposurePass =
    frame.resolve(exposurePassDesc()) | kdl::value_or(PassResources{});
  const auto tonemapPass =
    frame.resolve(tonemapPassDesc()) | kdl::value_or(PassResources{});
  const auto blitPass = frame.resolve(blitPassDesc()) | kdl::value_or(PassResources{});
  const auto resolved = !histogramPass.reads.empty() && !exposurePass.reads.empty()
                        && !tonemapPass.reads.empty() && !blitPass.reads.empty();
  if (m_stats.disabled || m_stats.pending() || !common || !resolved)
  {
    clearTarget(inputs);
    return m_stats;
  }

  auto* hdr = histogramPass.reads[0].texture;
  auto* histogramBuffer = histogramPass.writes[0].buffer;
  auto* exposureBuffer = exposurePass.writes[0].buffer;
  auto* ldrFramebuffer = tonemapPass.writes[0].framebuffer;
  auto* ldr = blitPass.reads[0].texture;
  auto* linearClamp = common->linearClamp.Get();

  const auto pushHistogram = pushesConstants(histogram.bindingLayouts);
  const auto pushExposure = pushesConstants(exposure.bindingLayouts);
  const auto pushTonemap = pushesConstants(tonemap.bindingLayouts);
  // TonemapPass.cpp:66-75: one buffer the passes without push constants share
  auto* cb =
    pushHistogram && pushExposure && pushTonemap ? nullptr : constantBuffer(device);
  if (!(pushHistogram && pushExposure && pushTonemap) && !cb)
  {
    clearTarget(inputs);
    return m_stats;
  }
  const auto uniforms = [&](const bool pushed) {
    return pushed ? Item::PushConstants(0, sizeof(EngineToneMappingConstants))
                  : Item::ConstantBuffer(0, cb);
  };

  // the binding sets (TonemapPass.cpp:174-194, 266-291, 374-397; CommonPasses.cpp:
  // 272-288): the targets by the render-target generation, the LUT by its slot
  const auto& histogramLayout = histogram.bindingLayouts.front();
  auto histogramKey = BindingSetKey{};
  histogramKey.layout = histogramLayout.Get();
  histogramKey.renderTargetGeneration = generation;
  histogramKey.resources = {histogramBuffer, pushHistogram ? nullptr : cb};
  const auto histogramSet = cache.get(histogramKey, frameIndex, [&] {
    return device.createBindingSet(
      nvrhi::BindingSetDesc{}
        .addItem(uniforms(pushHistogram))
        .addItem(Item::Texture_SRV(0, hdr))
        .addItem(Item::TypedBuffer_UAV(0, histogramBuffer)),
      histogramLayout);
  });

  const auto& exposureLayout = exposure.bindingLayouts.front();
  auto exposureKey = BindingSetKey{};
  exposureKey.layout = exposureLayout.Get();
  exposureKey.resources = {histogramBuffer, exposureBuffer, pushExposure ? nullptr : cb};
  const auto exposureSet = cache.get(exposureKey, frameIndex, [&] {
    return device.createBindingSet(
      nvrhi::BindingSetDesc{}
        .addItem(uniforms(pushExposure))
        .addItem(Item::TypedBuffer_SRV(0, histogramBuffer))
        .addItem(Item::TypedBuffer_UAV(0, exposureBuffer)),
      exposureLayout);
  });

  const auto& tonemapLayout = tonemap.bindingLayouts.front();
  auto tonemapKey = BindingSetKey{};
  tonemapKey.layout = tonemapLayout.Get();
  tonemapKey.slots = lut.versions;
  tonemapKey.renderTargetGeneration = generation;
  tonemapKey.resources = {exposureBuffer, linearClamp, pushTonemap ? nullptr : cb};
  const auto tonemapSet = cache.get(tonemapKey, frameIndex, [&] {
    return device.createBindingSet(
      nvrhi::BindingSetDesc{}
        .addItem(uniforms(pushTonemap))
        .addItem(Item::Texture_SRV(0, hdr))
        .addItem(Item::TypedBuffer_SRV(1, exposureBuffer))
        .addItem(
          Item::Texture_SRV(2, lut.texture, nvrhi::Format::UNKNOWN, lut.subresources))
        .addItem(Item::Sampler(0, linearClamp)),
      tonemapLayout);
  });

  const auto& blitLayout = blit.bindingLayouts.front();
  auto blitKey = BindingSetKey{};
  blitKey.layout = blitLayout.Get();
  blitKey.renderTargetGeneration = generation;
  blitKey.resources = {linearClamp};
  const auto blitSet = cache.get(blitKey, frameIndex, [&] {
    return device.createBindingSet(
      nvrhi::BindingSetDesc{}
        .addItem(Item::PushConstants(0, sizeof(EngineBlitConstants)))
        .addItem(Item::Texture_SRV(0, ldr)
                   .setSubresources(nvrhi::TextureSubresourceSet{0, 1, 0, 1})
                   .setDimension(nvrhi::TextureDimension::Texture2D))
        .addItem(Item::Sampler(0, linearClamp)),
      blitLayout);
  });
  if (!histogramSet || !exposureSet || !tonemapSet || !blitSet)
  {
    m_stats.disabled = true;
    m_stats.disabledReason = "NVRHI cannot create a tonemap pass's binding set";
    clearTarget(inputs);
    return m_stats;
  }

  const auto stopped = [&] {
    if (inputs.stopRequested && inputs.stopRequested())
    {
      m_stats.stoppedEarly = true;
      return true;
    }
    return false;
  };

  // a pass boundary, between the lit passes and these
  if (stopped())
  {
    return m_stats;
  }

  // SimpleRender (TonemapPass.cpp:337-345)
  if (inputs.tonemapTimer)
  {
    commandList.beginTimerQuery(inputs.tonemapTimer);
  }
  const auto endTonemapTimer = [&] {
    if (inputs.tonemapTimer)
    {
      commandList.endTimerQuery(inputs.tonemapTimer);
      m_stats.tonemapTimed = true;
    }
  };

  // ResetHistogram
  commandList.clearBufferUInt(histogramBuffer, 0);

  // AddFrameToHistogram: one viewport, so one dispatch
  m_stats.histogramConstants =
    engineHistogramConstants(extent.width, extent.height, m_defaults);
  writeConstants(inputs, pushHistogram, m_stats.histogramConstants);
  {
    auto state = nvrhi::ComputeState{};
    state.pipeline = histogram.pipeline;
    state.bindings = {histogramSet};
    commandList.setComputeState(state);
  }
  pushConstants(inputs, pushHistogram, m_stats.histogramConstants);
  const auto groups = engineHistogramGroups(m_stats.histogramConstants);
  commandList.dispatch(groups[0], groups[1], 1);
  ++m_stats.dispatches;

  // a pass boundary
  if (stopped())
  {
    endTonemapTimer();
    return m_stats;
  }

  // ComputeExposure
  m_stats.exposureConstants = engineExposureConstants(m_defaults);
  writeConstants(inputs, pushExposure, m_stats.exposureConstants);
  {
    auto state = nvrhi::ComputeState{};
    state.pipeline = exposure.pipeline;
    state.bindings = {exposureSet};
    commandList.setComputeState(state);
  }
  pushConstants(inputs, pushExposure, m_stats.exposureConstants);
  commandList.dispatch(1);
  ++m_stats.dispatches;

  if (stopped())
  {
    endTonemapTimer();
    return m_stats;
  }

  // Render: into ldrFBO, the + 1 viewport, four TriangleStrip vertices
  m_stats.tonemapConstants = engineTonemapConstants(m_defaults);
  writeConstants(inputs, pushTonemap, m_stats.tonemapConstants);
  {
    auto state = nvrhi::GraphicsState{};
    state.pipeline = tonemap.pipeline;
    state.framebuffer = ldrFramebuffer;
    state.bindings = {tonemapSet};
    state.viewport.addViewportAndScissorRect(
      engineTonemapViewport(extent.width, extent.height));
    commandList.setGraphicsState(state);
  }
  pushConstants(inputs, pushTonemap, m_stats.tonemapConstants);
  {
    auto args = nvrhi::DrawArguments{};
    args.instanceCount = 1;
    args.vertexCount = 4;
    commandList.draw(args);
    ++m_stats.draws;
  }
  endTonemapTimer();
  m_stats.tonemapped = true;

  if (stopped())
  {
    return m_stats;
  }

  // LP3.4 (D158): a debug view over the tonemapped picture
  if (inputs.beforeBlit)
  {
    inputs.beforeBlit(ldrFramebuffer);
  }

  // the blit into the frame's target (RenderBackend.cpp:6329-6333, BlitTexture)
  if (inputs.blitTimer)
  {
    commandList.beginTimerQuery(inputs.blitTimer);
  }
  {
    const auto& desc = inputs.target->getDesc();
    const auto viewport = engineBlitViewport(desc.width, desc.height);
    auto state = nvrhi::GraphicsState{};
    state.pipeline = blit.pipeline;
    state.framebuffer = inputs.targetFramebuffer;
    state.bindings = {blitSet};
    state.viewport.addViewport(viewport);
    state.viewport.addScissorRect(nvrhi::Rect{viewport});
    state.blendConstantColor = nvrhi::Color{0.0f};
    commandList.setGraphicsState(state);
    const auto constants = engineBlitConstants();
    commandList.setPushConstants(&constants, sizeof(constants));
    m_stats.pushConstantBytes += sizeof(constants);
    auto args = nvrhi::DrawArguments{};
    args.instanceCount = 1;
    args.vertexCount = 4;
    commandList.draw(args);
    ++m_stats.draws;
  }
  if (inputs.blitTimer)
  {
    commandList.endTimerQuery(inputs.blitTimer);
    m_stats.blitTimed = true;
  }
  m_stats.blitted = true;
  return m_stats;
}

void TonemapPasses::setDefaults(const EngineTonemapDefaults& defaults)
{
  m_defaults = defaults;
}

const TonemapFrameStats& TonemapPasses::lastStats() const
{
  return m_stats;
}

std::uint64_t TonemapPasses::volatileBufferBytes() const
{
  return m_constantBuffer ? m_constantBuffer->getDesc().byteSize
                              * m_constantBuffer->getDesc().maxVersions
                          : 0;
}

void TonemapPasses::releaseAll()
{
  m_constantBuffer = nullptr;
}

void TonemapPasses::clearTarget(const TonemapFrameInputs& inputs)
{
  // r_clear 2's colour (SC202), the LDR target's clear
  inputs.commandList->clearTextureFloat(
    inputs.target, nvrhi::AllSubresources, nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
  m_stats.targetCleared = true;
}

PipelineKey tonemapPipelineKey(const nvrhi::Format depthFormat)
{
  return PipelineKey{
    PipelineRecipe::Tonemap,
    EngineBuiltinTonemapping,
    0,
    0,
    0.0f,
    FramebufferFormats{nvrhi::Format::RGBA8_UNORM, depthFormat}};
}

PipelineKey blitPipelineKey(const nvrhi::Format targetFormat)
{
  return PipelineKey{
    PipelineRecipe::Blit,
    EngineBuiltinBlit,
    0,
    0,
    0.0f,
    FramebufferFormats{targetFormat, nvrhi::Format::UNKNOWN}};
}

nvrhi::IBuffer* TonemapPasses::constantBuffer(nvrhi::IDevice& device)
{
  // TonemapPass.cpp:68-74
  if (!m_constantBuffer)
  {
    // U1.10: CreateVolatileConstantBufferDesc, as TonemapPass.cpp calls it
    m_constantBuffer = device.createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
      sizeof(EngineToneMappingConstants),
      "ToneMappingConstants",
      TonemapConstantBufferVersions));
  }
  return m_constantBuffer.Get();
}

void TonemapPasses::writeConstants(
  const TonemapFrameInputs& inputs,
  const bool pushed,
  const EngineToneMappingConstants& constants)
{
  // nvrhi rule 5: written before the state that binds it
  if (!pushed && m_constantBuffer)
  {
    inputs.commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    ++m_stats.constantWrites;
    m_stats.constantBytes += sizeof(constants);
  }
}

void TonemapPasses::pushConstants(
  const TonemapFrameInputs& inputs,
  const bool pushed,
  const EngineToneMappingConstants& constants)
{
  // nvrhi rule 10: after the state, before the dispatch or draw
  if (pushed)
  {
    inputs.commandList->setPushConstants(&constants, sizeof(constants));
    m_stats.pushConstantBytes += sizeof(constants);
  }
}

} // namespace tb::lit
