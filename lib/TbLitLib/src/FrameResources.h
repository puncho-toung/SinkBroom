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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_intrinsic.cpp,
// renderer/NVRHI/Image_NVRHI.cpp, renderer/NVRHI/Framebuffer_NVRHI.cpp and
// renderer/Passes/TonemapPass.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// The NVIDIA Donut code the engine adapted there (TonemapPass) is under the MIT licence.
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "base/Macros.h"
#include "base/Result.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// C14: how long what a pass writes is needed: the frame it is written in, or the session
enum class PassLifetime
{
  Frame,
  Session,
};

// C14, D9: whether a pass runs; a disabled pass says why ("disabled: <field>")
enum class PassState
{
  Enabled,
  Disabled,
};

/**
 * C14 (§3.8): a pass of the frame, {name, reads, writes, lifetime, queue, state}. Reads
 * and writes name FrameResources' resources; FrameResources::resolve finds their handles
 * in that order. Every pass runs on the graphics queue (D43) until a copy or compute
 * queue is shown to be the same family.
 */
struct PassDesc
{
  std::string name;
  std::vector<std::string> reads;
  std::vector<std::string> writes;
  PassLifetime lifetime = PassLifetime::Frame;
  nvrhi::CommandQueue queue = nvrhi::CommandQueue::Graphics;
  PassState state = PassState::Enabled;
  // Disabled: why
  std::string reason;
};

// the names of FrameResources' resources, the engine's where it has one (SC254, SC256,
// SC261)
namespace frame_resource
{
inline constexpr auto CurrentRenderHdr = std::string_view{"_currentRenderHDR"};
inline constexpr auto CurrentDepth = std::string_view{"_currentDepth"};
inline constexpr auto CurrentRenderLdr = std::string_view{"_currentRenderLDR"};
inline constexpr auto HdrFramebuffer = std::string_view{"hdrFBO"};
inline constexpr auto LdrFramebuffer = std::string_view{"ldrFBO"};
// LP6.5a (C69, SC807, SC808): the G-buffer's normals (Image_intrinsic.cpp:1125, 347-352)
// and the framebuffer pass 3 draws into, {_currentNormals, _currentDepth}
// (Framebuffer_NVRHI.cpp:184-187)
inline constexpr auto CurrentNormals = std::string_view{"_currentNormals"};
inline constexpr auto GbufferFramebuffer = std::string_view{"geometryBufferFBO"};
// LP6.5b (C71, D179, P8.8): the HDR copy the post-process stages sample, postProcFBO's
// colour (Image_intrinsic.cpp: R_HDR_RGBA16FImage_ResNative, Framebuffer_NVRHI.cpp:
// 150-152)
inline constexpr auto CurrentRender = std::string_view{"_currentRender"};
// LP6.5a (C70, SC808, SC811): the ambient occlusion pass 4 writes and the ambient pass
// reads at t4 (Image_intrinsic.cpp:1127, 342-345), and SsaoPass' deinterleaved arrays
// (SsaoPass.cpp:56-71)
inline constexpr auto Ao0 = std::string_view{"_ao0"};
inline constexpr auto SsaoDeinterleavedDepth =
  std::string_view{"SSAO/DeinterleavedDepth"};
inline constexpr auto SsaoDeinterleavedOcclusion =
  std::string_view{"SSAO/DeinterleavedOcclusion"};
// LP6.6 (C73, SC820, SC821): the progressive TAA's targets (Image_intrinsic.cpp:
// 1094-1097): the resolved colour the tonemap reads while a history exists, the two
// feedbacks the resolve reads and writes in turn, and the motion vectors it reads
// (cleared to 0 and never drawn: the engine's are camera-only and 0 while the camera is
// still)
inline constexpr auto TaaResolved = std::string_view{"_taaResolved"};
inline constexpr auto TaaFeedback1 = std::string_view{"_taaFeedback1"};
inline constexpr auto TaaFeedback2 = std::string_view{"_taaFeedback2"};
inline constexpr auto TaaMotionVectors = std::string_view{"_taaMotionVectors"};
inline constexpr auto HistogramBuffer = std::string_view{"HistogramBuffer"};
inline constexpr auto ExposureBuffer = std::string_view{"ExposureBuffer"};
// the frame's presentation target: the acquired swapchain image or the headless target
inline constexpr auto FrameTarget = std::string_view{"frameTarget"};
} // namespace frame_resource

// one of the registry's resources: a texture, a buffer or a framebuffer (one is set)
struct FrameResource
{
  nvrhi::ITexture* texture = nullptr;
  nvrhi::IBuffer* buffer = nullptr;
  nvrhi::IFramebuffer* framebuffer = nullptr;
  // a render target, or a framebuffer of them: a binding set that binds it carries the
  // render-target generation (C28)
  bool renderTarget = false;
};

// a pass's handles, in the order of its reads and its writes
struct PassResources
{
  std::vector<FrameResource> reads;
  std::vector<FrameResource> writes;
};

/**
 * D35, §3.10: resources the render thread no longer uses, held until every frame that
 * may have used them has finished. NVRHI's garbage collection keeps only what a command
 * list references, and a clear references nothing (NVRHI vulkan-texture.cpp:609-698), so
 * a replaced target is held here, stamped with the frame it was replaced in, and let go
 * once that frame's event query has completed. Its bytes are the budget's "released,
 * awaiting collection" (C11). Render thread; releaseAll in teardown step 2.
 */
class RetiredResources
{
private:
  struct Entry
  {
    std::uint64_t frame = 0;
    nvrhi::RefCountPtr<nvrhi::IResource> resource;
    std::uint64_t bytes = 0;
  };

  std::vector<Entry> m_entries;
  std::uint64_t m_bytes = 0;

public:
  // resource is no longer used from the frame frame on
  void retire(nvrhi::IResource* resource, std::uint64_t bytes, std::uint64_t frame);
  // lets go of every resource whose frame has completed: completedFrames is the number of
  // frames known to have finished (frames 0 to completedFrames - 1)
  void collect(std::uint64_t completedFrames);
  std::uint64_t bytes() const;
  std::size_t size() const;
  void releaseAll();
};

/**
 * The frame's resource registry (C14, C6, §3.8, SC254-SC261), owned by the render
 * thread: the HDR target _currentRenderHDR (RGBA16_FLOAT, render target and UAV as the
 * engine's one-sample image), the depth target _currentDepth (D24S8 when the device has
 * it, else D32S8, initial state DepthWrite) and the LDR target _currentRenderLDR
 * (RGBA8_UNORM), all at the frame's extent with keepInitialState and the engine's clear
 * values (0, depth 1); the framebuffers hdrFBO {HDR, depth} and ldrFBO {LDR, depth}; the
 * tonemap pass's histogram (256 × R32_UINT) and exposure (1 × R32_UINT) buffers, typed
 * UAVs kept in UnorderedAccess, at a fixed size. The engine never clears the exposure
 * buffer (TonemapPass::ResetExposure has no caller, SC261); the preview clears it once at
 * Start (D70), in the first frame's command list.
 *
 * A new extent (a swapchain generation of another size, a new headless extent) makes new
 * targets and framebuffers and bumps the render-target generation once (C28); the old
 * handles go to NVRHI's garbage collection, which the render thread runs (D35).
 *
 * LP6.5a (C69, C70, D176): a session whose SSAO is on also has the G-buffer's targets:
 * _currentNormals (RGBA16_FLOAT, render target, keepInitialState: R_GeometryBufferImage_
 * ResNative at one sample) and the framebuffer geometryBufferFBO {_currentNormals,
 * _currentDepth}, whose formats are hdrFBO's (P8.21: no new format set on the compile
 * thread); and SSAO's: _ao0 (R8_UNORM, render target and UAV, keepInitialState:
 * R_AmbientOcclusionImage_ResNative; _ao1 is not made, the new pass never reads it) and
 * the deinterleaved arrays SSAO/DeinterleavedDepth (R32_FLOAT) and
 * SSAO/DeinterleavedOcclusion (R8_UNORM), Texture2DArray ceil(w / 4) × ceil(h / 4) × 16,
 * UAVs kept in ShaderResource as SsaoPass makes them; made and remade with the others,
 * in renderTargetBytes (C68 (e)). A session without SSAO makes none of them, so its
 * targets are LP6.4b's.
 *
 * LP6.5b (C71, C72, D179, the director's readings (1) and (2)): two targets are made on
 * the render thread at the first frame that needs them and kept until teardown, remade
 * with the others on a new extent and in renderTargetBytes from then on: _currentRender
 * (RGBA16_FLOAT, a render target as postProcFBO's colour, keepInitialState: the copy's
 * destination and the post-process stages' shader resource), at the first frame that
 * copies (ensureCurrentRender); and, in a session without SSAO, _currentNormals with
 * geometryBufferFBO, at the first frame a bumpyenvironment2 stage is listed
 * (ensureNormals). Neither bumps the render-target generation when it is first made: no
 * binding set can hold a target that did not exist (a set that binds it carries the
 * generation, C28).
 *
 * LP6.6 (C73, the director's reading (1)): the progressive TAA's four targets are made
 * together at the first frame that accumulates (ensureTaaTargets) and kept until
 * teardown, remade with the others on a new extent and in renderTargetBytes from then on:
 * _taaResolved and the feedbacks _taaFeedback1 and _taaFeedback2 (RGBA16_FLOAT, render
 * targets and UAVs, keepInitialState: R_HDR_RGBA16FImage_ResNative_UAV) and
 * _taaMotionVectors (RG16_FLOAT, a render target: R_HDR_RG16FImage_ResNative), cleared to
 * 0 once after it is made (recordTaaClears) and never drawn. A session that never
 * accumulates never makes them; their first make bumps no generation (as _currentRender).
 *
 * Threads: create on the GUI thread in phase B before the render thread starts (as the
 * headless target); releaseAll on the GUI thread in teardown step 2; everything else on
 * the render thread.
 */
class FrameResources
{
private:
  struct Targets
  {
    vk::Extent2D extent;
    nvrhi::TextureHandle hdr;
    nvrhi::TextureHandle depth;
    nvrhi::TextureHandle ldr;
    nvrhi::FramebufferHandle hdrFramebuffer;
    nvrhi::FramebufferHandle ldrFramebuffer;
    // LP6.5a (C69, C70): the G-buffer's and SSAO's, when the session's SSAO is on
    nvrhi::TextureHandle normals;
    nvrhi::FramebufferHandle gbufferFramebuffer;
    nvrhi::TextureHandle ao0;
    nvrhi::TextureHandle deinterleavedDepth;
    nvrhi::TextureHandle deinterleavedOcclusion;
    // LP6.5b (C71): the HDR copy, once a frame has needed it
    nvrhi::TextureHandle currentRender;
    // LP6.6 (C73): the progressive TAA's, once a frame has accumulated
    nvrhi::TextureHandle taaResolved;
    nvrhi::TextureHandle taaFeedback1;
    nvrhi::TextureHandle taaFeedback2;
    nvrhi::TextureHandle taaMotionVectors;
  };

  nvrhi::IDevice* m_device = nullptr;
  // LP6.5a (C69, C70): the session's SSAO is on: the G-buffer's targets are made too
  bool m_ssaoTargets = false;
  // LP6.5b (C71, C72): a frame needed _currentRender, or the G-buffer's normals without
  // SSAO: made with the targets from then on
  bool m_currentRenderWanted = false;
  bool m_normalsWanted = false;
  // LP6.6 (C73): a frame accumulated a TAA history: the TAA targets are made with the
  // others from then on; the motion vectors made since their last clear
  bool m_taaWanted = false;
  bool m_taaVectorsCleared = false;
  nvrhi::Format m_depthFormat = nvrhi::Format::D24S8;
  Targets m_targets;
  nvrhi::BufferHandle m_histogram;
  nvrhi::BufferHandle m_exposure;
  bool m_exposureCleared = false;
  std::uint64_t m_generation = 0;
  // the frame's presentation target; set for each frame
  nvrhi::ITexture* m_frameTarget = nullptr;
  // replaced targets and framebuffers, until their frames have finished
  RetiredResources m_retired;

public:
  FrameResources() = default;

  /**
   * GUI thread, phase B: the buffers and the targets at extent; the generation is 1.
   * d24s8Supported is DeviceFacts::d24s8Supported (C30); ssaoTargets (LP6.5a: the
   * session's SSAO is on) adds the G-buffer's targets.
   */
  Result<void> create(
    nvrhi::IDevice& device,
    bool d24s8Supported,
    vk::Extent2D extent,
    bool ssaoTargets = false);

  /**
   * Render thread, at the start of frame frame: new targets and framebuffers when the
   * frame's extent differs from theirs, and the generation bumped once; whether they
   * changed. The old ones are retired until the frames before have finished (D35).
   */
  Result<bool> ensureExtent(vk::Extent2D extent, std::uint64_t frame);

  /**
   * LP6.5b (C71, D179, the director's reading (1)), render thread: _currentRender at the
   * targets' extent, made now when it is not, and made again with the targets from then
   * on; whether it exists.
   */
  bool ensureCurrentRender();
  /**
   * LP6.5b (C72, D176, the director's reading (2)), render thread: _currentNormals and
   * geometryBufferFBO, made now when they are not (a session without SSAO whose frame
   * lists a bumpyenvironment2 stage), and made again with the targets from then on;
   * whether they exist.
   */
  bool ensureNormals();
  /**
   * LP6.6 (C73, the director's reading (1)), render thread: the four TAA targets at the
   * targets' extent, made now when they are not, and made again with the targets from
   * then on; whether they exist.
   */
  bool ensureTaaTargets();
  // LP6.6 (C73), render thread: _taaMotionVectors cleared to 0 once after each make
  void recordTaaClears(nvrhi::ICommandList& commandList);

  // render thread: a resource of the frame's (the headless target) that frame and later
  // ones no longer use, held until the frames before have finished; and their release
  void retire(nvrhi::IResource* resource, std::uint64_t bytes, std::uint64_t frame);
  void collectRetired(std::uint64_t completedFrames);
  // C11: the bytes retired and not yet let go
  std::uint64_t retiredBytes() const;

  // render thread: the frame's presentation target (an acquired image or the headless
  // target)
  void setFrameTarget(nvrhi::ITexture* target);

  // render thread, at the start of a frame's command list, before its first pass: the
  // exposure buffer's clear at Start (D70), once per session
  void recordStartClears(nvrhi::ICommandList& commandList);

  // C28: bumped by every recreation of the targets; 1 after create, 0 before
  std::uint64_t renderTargetGeneration() const;
  vk::Extent2D extent() const;
  nvrhi::Format depthFormat() const;
  // the exposure clear has been recorded
  bool exposureCleared() const;

  // the resource of a name (frame_resource), or nullopt for one the registry has not
  std::optional<FrameResource> find(std::string_view name) const;
  // C14: a pass's reads and writes by name, or the first name the registry has not
  Result<PassResources> resolve(const PassDesc& pass) const;

  // §3.10, C11: the bytes of the render targets, and of the buffers, as allocated
  std::uint64_t renderTargetBytes() const;
  std::uint64_t bufferBytes() const;

  // GUI thread, teardown step 2: every handle
  void releaseAll();

private:
  Result<Targets> makeTargets(vk::Extent2D extent) const;
  // LP6.5b: _currentNormals and geometryBufferFBO into targets (its depth made; why
  // not, or nullopt), and _currentRender at an extent
  std::optional<std::string> makeNormals(Targets& targets) const;
  nvrhi::TextureHandle makeCurrentRender(vk::Extent2D extent) const;
  // LP6.6: the four TAA targets into targets at their extent (why not, or nullopt)
  std::optional<std::string> makeTaaTargets(Targets& targets) const;

  deleteCopyAndMove(FrameResources);
};

} // namespace tb::lit
