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
  };

  nvrhi::IDevice* m_device = nullptr;
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
   * d24s8Supported is DeviceFacts::d24s8Supported (C30).
   */
  Result<void> create(nvrhi::IDevice& device, bool d24s8Supported, vk::Extent2D extent);

  /**
   * Render thread, at the start of frame frame: new targets and framebuffers when the
   * frame's extent differs from theirs, and the generation bumped once; whether they
   * changed. The old ones are retired until the frames before have finished (D35).
   */
  Result<bool> ensureExtent(vk::Extent2D extent, std::uint64_t frame);

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

  deleteCopyAndMove(FrameResources);
};

} // namespace tb::lit
