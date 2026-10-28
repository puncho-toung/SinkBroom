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

#include "FrameResources.h"

#include "lit/EngineTables.h"
#include "lit/ImageCache.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace tb::lit
{
namespace
{

// TonemapPass::CreateParameters (TonemapPass.h:47): the histogram's bins
constexpr auto HistogramBins = std::uint32_t{256};

/**
 * Image_NVRHI.cpp:556-575 AllocImage for an image generated with isRenderTarget (SC255):
 * initial state RenderTarget, clear value 0, keepInitialState; a depth format's initial
 * state DepthWrite and clear value 1. The image is 2D with one level and one sample.
 */
nvrhi::TextureDesc renderTargetDesc(
  const char* name,
  const nvrhi::Format format,
  const vk::Extent2D extent,
  const bool depth,
  const bool uav)
{
  auto desc = nvrhi::TextureDesc{}
                .setDebugName(name)
                .setDimension(nvrhi::TextureDimension::Texture2D)
                .setWidth(extent.width)
                .setHeight(extent.height)
                .setFormat(format)
                .setIsUAV(uav)
                .setSampleCount(1)
                .setMipLevels(1)
                .setInitialState(nvrhi::ResourceStates::RenderTarget)
                .setClearValue(nvrhi::Color{0.0f})
                .setIsRenderTarget(true)
                .setKeepInitialState(true);
  if (depth)
  {
    desc.setInitialState(nvrhi::ResourceStates::DepthWrite)
      .setClearValue(nvrhi::Color{1.0f});
  }
  return desc;
}

/**
 * LP6.5a (C70, SC811): SsaoPass' deinterleaved textures (SsaoPass.cpp:56-71): a
 * Texture2DArray of ( w + 3 ) / 4 × ( h + 3 ) / 4 × 16, a UAV, initial state
 * ShaderResource, kept there; no render target
 */
nvrhi::TextureDesc deinterleavedDesc(
  const char* name, const nvrhi::Format format, const vk::Extent2D extent)
{
  return nvrhi::TextureDesc{}
    .setDebugName(name)
    .setDimension(nvrhi::TextureDimension::Texture2DArray)
    .setWidth((extent.width + 3) / 4)
    .setHeight((extent.height + 3) / 4)
    .setArraySize(16)
    .setFormat(format)
    .setIsUAV(true)
    .setSampleCount(1)
    .setMipLevels(1)
    .setInitialState(nvrhi::ResourceStates::ShaderResource)
    .setKeepInitialState(true);
}

/**
 * TonemapPass.cpp:77-96: the histogram and the exposure buffers, typed R32_UINT UAVs in
 * UnorderedAccess, kept there.
 */
nvrhi::BufferDesc storageBufferDesc(const char* name, const std::uint32_t count)
{
  auto desc = nvrhi::BufferDesc{};
  desc.byteSize = sizeof(std::uint32_t) * count;
  desc.format = nvrhi::Format::R32_UINT;
  desc.canHaveUAVs = true;
  desc.debugName = name;
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  desc.canHaveTypedViews = true;
  return desc;
}

std::uint64_t textureBytes(const nvrhi::ITexture* texture)
{
  if (!texture)
  {
    return 0;
  }
  const auto& desc = texture->getDesc();
  const auto& info = nvrhi::getFormatInfo(desc.format);
  // LP6.5a: an array's slices (the SSAO arrays)
  return std::uint64_t(desc.width) * desc.height * desc.arraySize * info.bytesPerBlock;
}

} // namespace

void RetiredResources::retire(
  nvrhi::IResource* resource, const std::uint64_t bytes, const std::uint64_t frame)
{
  if (resource)
  {
    m_entries.push_back(
      Entry{frame, nvrhi::RefCountPtr<nvrhi::IResource>{resource}, bytes});
    m_bytes += bytes;
  }
}

void RetiredResources::collect(const std::uint64_t completedFrames)
{
  // frame f has finished once completedFrames > f; the removed entries' bytes are counted
  // as they go (U2.2: after remove_if the tail holds moved-from copies of kept entries)
  std::erase_if(m_entries, [&](const Entry& entry) {
    if (entry.frame >= completedFrames)
    {
      return false;
    }
    m_bytes -= entry.bytes;
    return true;
  });
}

std::uint64_t RetiredResources::bytes() const
{
  return m_bytes;
}

std::size_t RetiredResources::size() const
{
  return m_entries.size();
}

void RetiredResources::releaseAll()
{
  m_entries.clear();
  m_bytes = 0;
}

Result<void> FrameResources::create(
  nvrhi::IDevice& device,
  const bool d24s8Supported,
  const vk::Extent2D extent,
  const bool ssaoTargets)
{
  m_device = &device;
  m_ssaoTargets = ssaoTargets;
  // SC254: _currentDepth is TD_DEPTH_STENCIL, AllocImage's FMT_DEPTH_STENCIL
  m_depthFormat = engineTextureFormat(engine_image::FormatDepthStencil, d24s8Supported)
                    .value_or(nvrhi::Format::D32S8);

  m_histogram = device.createBuffer(storageBufferDesc("HistogramBuffer", HistogramBins));
  m_exposure = device.createBuffer(storageBufferDesc("ExposureBuffer", 1));
  if (!m_histogram || !m_exposure)
  {
    return Error{"Cannot create the tonemap pass's buffers"};
  }

  return makeTargets(extent) | kdl::transform([&](Targets targets) {
           m_targets = std::move(targets);
           m_generation = 1;
         });
}

Result<bool> FrameResources::ensureExtent(
  const vk::Extent2D extent, const std::uint64_t frame)
{
  if (m_targets.hdr && m_targets.extent == extent)
  {
    return false;
  }
  return makeTargets(extent) | kdl::transform([&](Targets targets) {
           // D35: the old ones are held until the frames that used them have finished,
           // then NVRHI frees them (a clear holds no reference of its own)
           auto old = std::exchange(m_targets, std::move(targets));
           retire(old.hdrFramebuffer.Get(), 0, frame);
           retire(old.ldrFramebuffer.Get(), 0, frame);
           retire(old.gbufferFramebuffer.Get(), 0, frame);
           for (const auto& texture :
                {old.hdr,
                 old.depth,
                 old.ldr,
                 old.normals,
                 old.ao0,
                 old.deinterleavedDepth,
                 old.deinterleavedOcclusion})
           {
             retire(texture.Get(), textureBytes(texture.Get()), frame);
           }
           ++m_generation;
           return true;
         });
}

void FrameResources::retire(
  nvrhi::IResource* resource, const std::uint64_t bytes, const std::uint64_t frame)
{
  m_retired.retire(resource, bytes, frame);
}

void FrameResources::collectRetired(const std::uint64_t completedFrames)
{
  m_retired.collect(completedFrames);
}

std::uint64_t FrameResources::retiredBytes() const
{
  return m_retired.bytes();
}

void FrameResources::setFrameTarget(nvrhi::ITexture* target)
{
  m_frameTarget = target;
}

void FrameResources::recordStartClears(nvrhi::ICommandList& commandList)
{
  if (!m_exposureCleared && m_exposure)
  {
    // D70: the exposure starts from 0 in every session, so that its first frame adapts
    // from a known value (the engine's buffer holds whatever the allocation held)
    commandList.clearBufferUInt(m_exposure, 0);
    m_exposureCleared = true;
  }
}

std::uint64_t FrameResources::renderTargetGeneration() const
{
  return m_generation;
}

vk::Extent2D FrameResources::extent() const
{
  return m_targets.extent;
}

nvrhi::Format FrameResources::depthFormat() const
{
  return m_depthFormat;
}

bool FrameResources::exposureCleared() const
{
  return m_exposureCleared;
}

std::optional<FrameResource> FrameResources::find(const std::string_view name) const
{
  namespace fr = frame_resource;
  if (name == fr::CurrentRenderHdr && m_targets.hdr)
  {
    return FrameResource{m_targets.hdr.Get(), nullptr, nullptr, true};
  }
  if (name == fr::CurrentDepth && m_targets.depth)
  {
    return FrameResource{m_targets.depth.Get(), nullptr, nullptr, true};
  }
  if (name == fr::CurrentRenderLdr && m_targets.ldr)
  {
    return FrameResource{m_targets.ldr.Get(), nullptr, nullptr, true};
  }
  if (name == fr::HdrFramebuffer && m_targets.hdrFramebuffer)
  {
    return FrameResource{nullptr, nullptr, m_targets.hdrFramebuffer.Get(), true};
  }
  if (name == fr::LdrFramebuffer && m_targets.ldrFramebuffer)
  {
    return FrameResource{nullptr, nullptr, m_targets.ldrFramebuffer.Get(), true};
  }
  if (name == fr::CurrentNormals && m_targets.normals)
  {
    return FrameResource{m_targets.normals.Get(), nullptr, nullptr, true};
  }
  if (name == fr::GbufferFramebuffer && m_targets.gbufferFramebuffer)
  {
    return FrameResource{nullptr, nullptr, m_targets.gbufferFramebuffer.Get(), true};
  }
  if (name == fr::Ao0 && m_targets.ao0)
  {
    return FrameResource{m_targets.ao0.Get(), nullptr, nullptr, true};
  }
  if (name == fr::SsaoDeinterleavedDepth && m_targets.deinterleavedDepth)
  {
    return FrameResource{m_targets.deinterleavedDepth.Get(), nullptr, nullptr, true};
  }
  if (name == fr::SsaoDeinterleavedOcclusion && m_targets.deinterleavedOcclusion)
  {
    return FrameResource{m_targets.deinterleavedOcclusion.Get(), nullptr, nullptr, true};
  }
  if (name == fr::HistogramBuffer && m_histogram)
  {
    return FrameResource{nullptr, m_histogram.Get(), nullptr, false};
  }
  if (name == fr::ExposureBuffer && m_exposure)
  {
    return FrameResource{nullptr, m_exposure.Get(), nullptr, false};
  }
  if (name == fr::FrameTarget && m_frameTarget)
  {
    return FrameResource{m_frameTarget, nullptr, nullptr, true};
  }
  return std::nullopt;
}

Result<PassResources> FrameResources::resolve(const PassDesc& pass) const
{
  auto resources = PassResources{};
  const auto add = [&](
                     const std::vector<std::string>& names,
                     std::vector<FrameResource>& into) -> std::optional<std::string> {
    for (const auto& name : names)
    {
      const auto resource = find(name);
      if (!resource)
      {
        return name;
      }
      into.push_back(*resource);
    }
    return std::nullopt;
  };
  if (const auto missing = add(pass.reads, resources.reads))
  {
    return Error{
      fmt::format("the pass {} reads {}, which the frame has not", pass.name, *missing)};
  }
  if (const auto missing = add(pass.writes, resources.writes))
  {
    return Error{
      fmt::format("the pass {} writes {}, which the frame has not", pass.name, *missing)};
  }
  return resources;
}

std::uint64_t FrameResources::renderTargetBytes() const
{
  return textureBytes(m_targets.hdr.Get()) + textureBytes(m_targets.depth.Get())
         + textureBytes(m_targets.ldr.Get()) + textureBytes(m_targets.normals.Get())
         + textureBytes(m_targets.ao0.Get())
         + textureBytes(m_targets.deinterleavedDepth.Get())
         + textureBytes(m_targets.deinterleavedOcclusion.Get());
}

std::uint64_t FrameResources::bufferBytes() const
{
  return (m_histogram ? m_histogram->getDesc().byteSize : 0)
         + (m_exposure ? m_exposure->getDesc().byteSize : 0);
}

void FrameResources::releaseAll()
{
  m_frameTarget = nullptr;
  m_retired.releaseAll();
  m_targets = Targets{};
  m_histogram = nullptr;
  m_exposure = nullptr;
}

Result<FrameResources::Targets> FrameResources::makeTargets(
  const vk::Extent2D extent) const
{
  if (!m_device)
  {
    return Error{"The frame resources have no device"};
  }
  auto& device = *m_device;
  auto targets = Targets{};
  targets.extent = extent;
  // SC254: Image_intrinsic.cpp:258-276 R_LdrNativeImage, R_DepthImage,
  // R_HDR_RGBA16FImage_ResNative_MSAAOpt (one sample: a UAV too)
  targets.hdr = device.createTexture(renderTargetDesc(
    "_currentRenderHDR", nvrhi::Format::RGBA16_FLOAT, extent, false, true));
  targets.depth = device.createTexture(
    renderTargetDesc("_currentDepth", m_depthFormat, extent, true, false));
  targets.ldr = device.createTexture(renderTargetDesc(
    "_currentRenderLDR", nvrhi::Format::RGBA8_UNORM, extent, false, false));
  if (!targets.hdr || !targets.depth || !targets.ldr)
  {
    return Error{fmt::format(
      "Cannot create the frame's targets at {}x{}", extent.width, extent.height)};
  }
  // SC256: Framebuffer_NVRHI.cpp:140-148
  targets.ldrFramebuffer = device.createFramebuffer(nvrhi::FramebufferDesc{}
                                                      .addColorAttachment(targets.ldr)
                                                      .setDepthAttachment(targets.depth));
  targets.hdrFramebuffer = device.createFramebuffer(nvrhi::FramebufferDesc{}
                                                      .addColorAttachment(targets.hdr)
                                                      .setDepthAttachment(targets.depth));
  if (!targets.hdrFramebuffer || !targets.ldrFramebuffer)
  {
    return Error{"Cannot create the frame's framebuffers"};
  }
  if (m_ssaoTargets)
  {
    // LP6.5a (C69, SC808): R_GeometryBufferImage_ResNative (Image_intrinsic.cpp:347-352)
    // at one sample, and geometryBufferFBO (Framebuffer_NVRHI.cpp:184-187)
    targets.normals = device.createTexture(renderTargetDesc(
      "_currentNormals", nvrhi::Format::RGBA16_FLOAT, extent, false, false));
    if (!targets.normals)
    {
      return Error{fmt::format(
        "Cannot create the G-buffer's target at {}x{}", extent.width, extent.height)};
    }
    targets.gbufferFramebuffer =
      device.createFramebuffer(nvrhi::FramebufferDesc{}
                                 .addColorAttachment(targets.normals)
                                 .setDepthAttachment(targets.depth));
    if (!targets.gbufferFramebuffer)
    {
      return Error{"Cannot create the G-buffer's framebuffer"};
    }
    // LP6.5a (C70): R_AmbientOcclusionImage_ResNative (Image_intrinsic.cpp:342-345:
    // TD_R8F, a render target and a UAV) and SsaoPass' arrays (SsaoPass.cpp:56-71)
    targets.ao0 = device.createTexture(
      renderTargetDesc("_ao0", nvrhi::Format::R8_UNORM, extent, false, true));
    targets.deinterleavedDepth = device.createTexture(
      deinterleavedDesc("SSAO/DeinterleavedDepth", nvrhi::Format::R32_FLOAT, extent));
    targets.deinterleavedOcclusion = device.createTexture(
      deinterleavedDesc("SSAO/DeinterleavedOcclusion", nvrhi::Format::R8_UNORM, extent));
    if (!targets.ao0 || !targets.deinterleavedDepth || !targets.deinterleavedOcclusion)
    {
      return Error{fmt::format(
        "Cannot create the SSAO targets at {}x{}", extent.width, extent.height)};
    }
  }
  return targets;
}

} // namespace tb::lit
