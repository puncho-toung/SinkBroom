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

#pragma once

#include "BindingSetCache.h"
#include "FrameResources.h"
#include "ImageSlots.h"
#include "PipelineCompiler.h"
#include "SamplerSet.h"
#include "base/Macros.h"
#include "lit/EngineBuiltins.h"
#include "lit/EngineTables.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace tb::lit
{

/**
 * ToneMappingConstants (TonemapPass_cb.h:25-47, shaders/builtin/post/tonemapping_cb.h),
 * the 80 bytes the histogram, exposure and tonemapping shaders read: idVec2i is the
 * shaders' uint2, idVec2 their float2.
 */
struct EngineToneMappingConstants
{
  std::array<std::int32_t, 2> viewOrigin = {0, 0};
  std::array<std::int32_t, 2> viewSize = {0, 0};

  float logLuminanceScale = 0.0f;
  float logLuminanceBias = 0.0f;
  float histogramLowPercentile = 0.0f;
  float histogramHighPercentile = 0.0f;

  float eyeAdaptationSpeedUp = 0.0f;
  float eyeAdaptationSpeedDown = 0.0f;
  float minAdaptedLuminance = 0.0f;
  float maxAdaptedLuminance = 0.0f;

  float frameTime = 0.0f;
  float exposureScale = 0.0f;
  float whitePointInvSquared = 0.0f;
  std::uint32_t sourceSlice = 0;

  std::array<float, 2> colorLutTextureSize = {0.0f, 0.0f};
  std::array<float, 2> colorLutTextureSizeInv = {0.0f, 0.0f};
};
static_assert(sizeof(EngineToneMappingConstants) == 80);

// BlitConstants (CommonPasses.h:63-72, shaders/blit.cb.h), the 36 bytes rect.vs pushes
struct EngineBlitConstants
{
  std::array<float, 2> sourceOrigin = {0.0f, 0.0f};
  std::array<float, 2> sourceSize = {0.0f, 0.0f};
  std::array<float, 2> targetOrigin = {0.0f, 0.0f};
  std::array<float, 2> targetSize = {0.0f, 0.0f};
  float sharpenFactor = 0.0f;
};
static_assert(sizeof(EngineBlitConstants) == 36);

/**
 * D70, K24: the exposure pass's frameTime. The engine passes Sys_Milliseconds() / 1000,
 * the seconds since it started, so exp2( -frameTime · speed ) is next to 0 tens of
 * seconds in and each frame's exposure is its own clamped target; the preview passes a
 * constant so large that it is 0 from the first frame (exp2( -500 ) underflows in
 * float), because frames drawn on demand would otherwise never converge.
 */
inline constexpr auto SteadyStateFrameTime = 1000.0f;

// TonemapPass::CreateParameters::numConstantBufferVersions (TonemapPass.h:50): the
// versions of the one volatile constant buffer the passes share where a pass has no push
// constants
inline constexpr auto TonemapConstantBufferVersions = std::uint32_t{32};

// DX.5: the tonemap pass's pipeline (into the LDR target, with the HDR pass's depth
// format as TonemapPass's framebuffer has it) and the blit's (into the frame's target
// alone), as record asks for them; and the two compute programs
PipelineKey tonemapPipelineKey(nvrhi::Format depthFormat);
PipelineKey blitPipelineKey(nvrhi::Format targetFormat);
inline constexpr auto TonemapComputePrograms =
  std::array{EngineBuiltinHistogram, EngineBuiltinExposure};

// Pure functions of the engine's arithmetic, for any thread.

/**
 * AddFrameToHistogram's viewport (TonemapPass.cpp:399-406): the view's viewport x1..x2,
 * y1..y2 without the + 1 the other passes add, so that its scissor, and the histogram's
 * viewSize, leave out the last column and row. The preview's view is the whole target,
 * (0, 0, width - 1, height - 1) (LitView).
 */
nvrhi::Viewport engineHistogramViewport(std::uint32_t width, std::uint32_t height);

/**
 * AddFrameToHistogram's constants (TonemapPass.cpp:410-417): logLuminanceScale
 * 1 / (r_hdrMinLuminance - r_hdrMaxLuminance) and logLuminanceBias -min · scale as the
 * engine writes them (D20: its encode range is not the exposure's decode range, SC286),
 * viewOrigin and viewSize from the scissor of engineHistogramViewport, sourceSlice 0.
 */
EngineToneMappingConstants engineHistogramConstants(
  std::uint32_t width,
  std::uint32_t height,
  const EngineTonemapDefaults& defaults = EngineTonemap);

// the histogram's thread groups: ( viewSize + 15 ) / 16 (TonemapPass.cpp:434-435)
std::array<std::uint32_t, 2> engineHistogramGroups(
  const EngineToneMappingConstants& constants);

/**
 * ComputeExposure's constants (TonemapPass.cpp:439-453): the decode range scale
 * maxLuminance - minLuminance (4 - -10) and bias minLuminance, the percentiles clamped
 * as the engine clamps them, the adaptation speeds r_hdrAdaptionRate and half of it, the
 * adapted-luminance bounds r_hdrMinLuminance and r_hdrMaxLuminance; frameTime is
 * SteadyStateFrameTime (D70) instead of the engine's seconds since start.
 */
EngineToneMappingConstants engineExposureConstants(
  const EngineTonemapDefaults& defaults = EngineTonemap,
  float frameTime = SteadyStateFrameTime);

// Render's viewport (TonemapPass.cpp:299-305): the view's viewport with the + 1
nvrhi::Viewport engineTonemapViewport(std::uint32_t width, std::uint32_t height);

/**
 * Render's constants (TonemapPass.cpp:309-316): exposureScale exp2( r_exposure ),
 * whitePointInvSquared 1 / whitePoint², the adapted-luminance bounds, sourceSlice 0, and
 * no colour LUT (colorLutSize is 0: the LUT is _black, TonemapPass.cpp:99), so its size
 * and inverse are 0 and the shader applies ACESFilm.
 */
EngineToneMappingConstants engineTonemapConstants(
  const EngineTonemapDefaults& defaults = EngineTonemap);

/**
 * The swapchain blit of DrawViewInternal (RenderBackend.cpp:6329-6333): BlitParameters'
 * defaults (CommonPasses.h:47-61) with the target viewport Viewport( width, height ),
 * through BlitTexture (CommonPasses.cpp:217-324): source and target boxes (0, 0, 1, 1),
 * sharpenFactor 0, the LinearClamp sampler (BlitSampler::Linear), the scissor
 * Rect( viewport ), blending off.
 */
EngineBlitConstants engineBlitConstants();
nvrhi::Viewport engineBlitViewport(std::uint32_t width, std::uint32_t height);

// what a frame's tonemap passes read and record into (render thread)
struct TonemapFrameInputs
{
  nvrhi::IDevice* device = nullptr;
  nvrhi::ICommandList* commandList = nullptr;
  std::uint64_t frameIndex = 0;
  FrameResources* frame = nullptr;
  RenderPipelines* pipelines = nullptr;
  ImageSlots* images = nullptr;
  BindingSetCache* bindingSets = nullptr;
  SamplerSet* samplers = nullptr;
  // the frame's presentation target (an acquired swapchain image or the headless
  // target), at the frame's extent, and a framebuffer of it alone
  nvrhi::ITexture* target = nullptr;
  nvrhi::IFramebuffer* targetFramebuffer = nullptr;
  // the HDR target holds a picture this frame: the lit passes drew into it, or a test
  // filled it
  bool hdrDrawn = false;
  // C16: the queries the passes are timed with, or null
  nvrhi::ITimerQuery* tonemapTimer = nullptr;
  nvrhi::ITimerQuery* blitTimer = nullptr;
  // a pass boundary: a stop or a park was asked for
  std::function<bool()> stopRequested;
};

// C16's columns of the tonemap passes, per frame
struct TonemapFrameStats
{
  // histogram, exposure and tonemapping were recorded into the LDR target
  bool tonemapped = false;
  // the LDR target was blitted into the frame's target
  bool blitted = false;
  // the frame's target was cleared to (0, 0, 0, 1) instead (an empty, pending or
  // disabled frame)
  bool targetCleared = false;
  // D77: a pipeline of the passes is pending (the compile thread's onReady is the next
  // frame's trigger), or the colour LUT has no written level yet (its arrival is)
  std::size_t pendingPipelines = 0;
  bool lutPending = false;
  // D9: a pipeline of the passes failed: they are disabled for the session, and the
  // frame converged as far as it can
  bool disabled = false;
  std::string disabledReason;
  // a stop or a park came between the passes
  bool stoppedEarly = false;
  // draw calls (the tonemap's and the blit's) and compute dispatches
  std::size_t draws = 0;
  std::size_t dispatches = 0;
  // D76: the shared volatile constant buffer's writes and bytes, and the push-constant
  // bytes
  std::uint64_t constantWrites = 0;
  std::uint64_t constantBytes = 0;
  std::uint64_t pushConstantBytes = 0;
  // C16: the passes' timer queries were begun and ended
  bool tonemapTimed = false;
  bool blitTimed = false;
  // what the frame's three passes were given (tests)
  EngineToneMappingConstants histogramConstants;
  EngineToneMappingConstants exposureConstants;
  EngineToneMappingConstants tonemapConstants;

  // D77: the frame waits for a pipeline or the LUT
  bool pending() const;
};

/**
 * LP2.2b (§3.8 pass 10, C14, C27, D70): the frame's tonemap and its presentation, as
 * DrawViewInternal ends a 3D view (RenderBackend.cpp:6222-6250, 6281-6337, SC201) without
 * TAA and MSAA: TonemapPass::SimpleRender (TonemapPass.cpp:337-345) over the HDR target
 * into ldrFBO — ResetHistogram (clearBufferUInt 0), AddFrameToHistogram (histogram.cs,
 * 16 × 16 groups over the x2 viewport), ComputeExposure (exposure.cs, one group, D70's
 * frameTime), Render (tonemapping, four TriangleStrip vertices over the x2 + 1 viewport,
 * the source read with t_Source[pos.xy], the exposure, _black as the colour LUT, the
 * LinearClamp sampler) — then CommonRenderPasses::BlitTexture of the LDR target into the
 * frame's target (rect.vs and blit.ps, the 36-byte push constants, LinearClamp). The
 * engine blits a second time in PostProcess (RenderBackend.cpp:6912-6920, SC203), the
 * same source into the same target: drawn once here (mirror.json M-TONE-DIFF-02).
 *
 * The uniforms of each pass are push constants where the device's rule says so (C30;
 * the compile thread's layouts carry it), else the one volatile constant buffer of
 * TonemapConstantBufferVersions versions they share, written before the state that binds
 * it (TonemapPass.cpp:66-75). Binding sets come from the session's cache (C28): the HDR
 * and LDR targets by the render-target generation, the colour LUT by its slot.
 *
 * A frame whose HDR target holds nothing (nothing drawn: an empty scene, or lit passes
 * still pending) skips the passes and clears the frame's target to (0, 0, 0, 1), the
 * picture the passes would give of a black HDR target (mirror.json M-TONE-DIFF-03); so
 * does a frame whose pipelines are pending or failed (D9), until they are made.
 *
 * Threads: the render thread; releaseAll in teardown step 2.
 */
class TonemapPasses
{
private:
  nvrhi::BufferHandle m_constantBuffer;
  TonemapFrameStats m_stats;
  // the engine's values; LP2.3's pinned-exposure diagnostic replaces them (Q21)
  EngineTonemapDefaults m_defaults = EngineTonemap;

public:
  TonemapPasses() = default;

  // render thread: the values the next frames' passes compute their constants from
  void setDefaults(const EngineTonemapDefaults& defaults);

  // render thread: the frame's tonemap passes and its blit; what the frame did
  TonemapFrameStats record(const TonemapFrameInputs& inputs);

  const TonemapFrameStats& lastStats() const;
  // U2.6, render thread: the volatile constant buffer's bytes, every version (NVRHI's
  // aligned size times maxVersions)
  std::uint64_t volatileBufferBytes() const;

  // teardown step 2: every handle
  void releaseAll();

private:
  void clearTarget(const TonemapFrameInputs& inputs);
  nvrhi::IBuffer* constantBuffer(nvrhi::IDevice& device);
  // writes constants into the shared buffer (pushed is false) before the state
  void writeConstants(
    const TonemapFrameInputs& inputs,
    bool pushed,
    const EngineToneMappingConstants& constants);
  void pushConstants(
    const TonemapFrameInputs& inputs,
    bool pushed,
    const EngineToneMappingConstants& constants);

  deleteCopyAndMove(TonemapPasses);
};

} // namespace tb::lit
