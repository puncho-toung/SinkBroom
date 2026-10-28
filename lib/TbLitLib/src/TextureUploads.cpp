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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/NVRHI/Image_NVRHI.cpp and
// renderer/Image_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "TextureUploads.h"

#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"
#include "lit/LitConfig.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// idImage::IsCompressed: the 4 × 4 block formats
bool blockCompressed(const int format)
{
  return format == FormatDxt1 || format == FormatDxt5 || format == FormatBc6h
         || format == FormatBc7 || format == FormatEtc1;
}

// the rows writeTexture copies: a block format's are rows of 4 × 4 blocks
std::uint64_t uploadRows(const int format, const int height)
{
  return blockCompressed(format) ? std::uint64_t(std::max(1, (height + 3) / 4))
                                 : std::uint64_t(height);
}

} // namespace

Result<nvrhi::TextureDesc> engineTextureDesc(
  const CachedImage& image, const bool d24s8Supported, const std::string& name)
{
  const auto format = engineTextureFormat(image.format, d24s8Supported);
  if (!format)
  {
    return Error{fmt::format(
      "the image format {} has no texture format (the engine stops on it)",
      image.format)};
  }
  if (image.width < 1 || image.height < 1 || image.numLevels < 1)
  {
    return Error{fmt::format(
      "the image is {} x {} with {} levels", image.width, image.height, image.numLevels)};
  }

  // Image_NVRHI.cpp:473-477: a compressed image's size in whole blocks
  auto width = std::uint32_t(image.width);
  auto height = std::uint32_t(image.height);
  if (blockCompressed(image.format))
  {
    width = (width + 3) & ~3u;
    height = (height + 3) & ~3u;
  }

  // Image_NVRHI.cpp:479-488, 596-614
  auto desc = nvrhi::TextureDesc{}
                .setDebugName(name)
                .setDimension(nvrhi::TextureDimension::Texture2D)
                .setWidth(width)
                .setHeight(height)
                .setFormat(*format)
                .setMipLevels(std::uint32_t(image.numLevels))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true);
  desc.componentMapping = engineComponentMapping(image.format, image.colorFormat);
  switch (image.textureType)
  {
  case Type2D:
    break;
  case TypeCubic:
    desc.setDimension(nvrhi::TextureDimension::TextureCube).setArraySize(6);
    break;
  case Type2DArray:
    desc.setDimension(nvrhi::TextureDimension::Texture2DArray).setArraySize(6);
    break;
  default:
    return Error{
      fmt::format("the texture type {} is not one the engine makes", image.textureType)};
  }
  return desc;
}

Result<std::uint64_t> levelUploadBytes(
  const CachedImage& image, const CachedImageLevel& level)
{
  // T0.14: a format without a pitch is refused, never uploaded with a zero pitch
  const auto pitch = engineRowPitch(image.format, level.width);
  if (!pitch)
  {
    return Error{fmt::format(
      "the image format {} has no row pitch for a width of {}",
      image.format,
      level.width)};
  }
  const auto bytes = std::uint64_t(*pitch) * uploadRows(image.format, level.height);
  if (bytes > level.size || level.offset + bytes > image.data.size())
  {
    return Error{fmt::format(
      "level {} side {} holds {} bytes, its rows need {}",
      level.level,
      level.destZ,
      level.size,
      bytes)};
  }
  return bytes;
}

int mipFloorLevel(const std::uint32_t width, const std::uint32_t height, const int levels)
{
  if (levels < 1)
  {
    return 0;
  }
  for (auto level = 0; level < levels; ++level)
  {
    const auto w = std::max(width >> std::uint32_t(level), 1u);
    const auto h = std::max(height >> std::uint32_t(level), 1u);
    if (w <= std::uint32_t(MipFloorExtent) && h <= std::uint32_t(MipFloorExtent))
    {
      return level;
    }
  }
  return levels - 1;
}

TextureUploads::TextureUploads(const bool d24s8Supported)
  : m_d24s8Supported{d24s8Supported}
{
}

void TextureUploads::setD24s8Supported(const bool supported)
{
  m_d24s8Supported = supported;
}

std::uint64_t TextureUploads::DedicatedCounts::total() const
{
  return noHeaps + refusedClass + tooLarge + noHeap + bindRefused;
}

bool TextureUploads::record(
  nvrhi::IDevice& device,
  nvrhi::ICommandList& commandList,
  ImageSlots& slots,
  const std::uint64_t cap,
  std::uint64_t& frameBytes,
  const std::function<bool()>& allocationRoom,
  TextureHeaps* heaps)
{
  auto left = false;
  for (const auto id : slots.loading())
  {
    const auto* slot = slots.slot(id);
    const auto image = slot->pixels;
    if (!image)
    {
      slots.refuse(id, "the image's bytes are gone before they were written");
      continue;
    }
    // LP6.4b (the director's reading (4)): a floored slot's texture starts at the image's
    // mip floor
    const auto floorLevel = slot->floored ? mipFloorLevel(
                                              std::uint32_t(std::max(image->width, 1)),
                                              std::uint32_t(std::max(image->height, 1)),
                                              image->numLevels)
                                          : 0;

    if (!slot->texture)
    {
      auto bytes = std::uint64_t{0};
      auto problem = std::string{};
      // a level's bytes, all its sides together, as a frame writes them
      auto perLevel = std::vector<std::uint64_t>(std::size_t(image->numLevels), 0);
      for (const auto& level : image->levels)
      {
        levelUploadBytes(*image, level)
          | kdl::transform([&](const std::uint64_t levelBytes) {
              if (level.level >= floorLevel)
              {
                bytes += levelBytes;
              }
              if (level.level >= 0 && level.level < image->numLevels)
              {
                perLevel[std::size_t(level.level)] += levelBytes;
              }
            })
          | kdl::if_error([&](const auto& e) { problem = e.msg; }) | kdl::ignore();
        if (!problem.empty())
        {
          break;
        }
      }
      // U3.16: a level larger than a frame's cap would be one writeTexture through one
      // upload chunk of its size (the engine writes it whole: mirror.json M-TEX-DIFF-02)
      for (auto level = std::size_t(floorLevel);
           problem.empty() && level < perLevel.size();
           ++level)
      {
        if (perLevel[level] > UploadBytesPerFrame)
        {
          problem = fmt::format(
            "level {} is {} bytes, more than the {} MB a frame uploads (the engine "
            "writes it whole; mirror.json M-TEX-DIFF-02)",
            level,
            perLevel[level],
            UploadBytesPerFrame / (1024 * 1024));
        }
      }
      auto desc = std::optional<nvrhi::TextureDesc>{};
      if (problem.empty())
      {
        engineTextureDesc(*image, m_d24s8Supported, slot->key.name)
          | kdl::transform([&](nvrhi::TextureDesc made) { desc = std::move(made); })
          | kdl::if_error([&](const auto& e) { problem = e.msg; }) | kdl::ignore();
      }
      if (desc && floorLevel > 0)
      {
        // the floor's level as the texture's first, its size in whole blocks as the
        // engine's (Image_NVRHI.cpp:473-477)
        auto width =
          std::max(std::uint32_t(image->width) >> std::uint32_t(floorLevel), 1u);
        auto height =
          std::max(std::uint32_t(image->height) >> std::uint32_t(floorLevel), 1u);
        if (blockCompressed(image->format))
        {
          width = (width + 3) & ~3u;
          height = (height + 3) & ~3u;
        }
        desc->setWidth(width).setHeight(height).setMipLevels(
          std::uint32_t(image->numLevels - floorLevel));
      }
      auto texture = nvrhi::TextureHandle{};
      auto roomRefused = false;
      // why a texture made below is dedicated (counted once it is made)
      auto* dedicatedReason = &m_dedicated.noHeaps;
      if (problem.empty())
      {
        // LP6.4a (C68 (a), D171): a heap-able class's texture is made virtual and bound
        // into a heap's range; one larger than a heap, or with no heap made, is made
        // again dedicated below
        const auto formatClass = FormatClass{desc->format, desc->dimension};
        if (heaps && heaps->heapable(formatClass))
        {
          auto virtualDesc = *desc;
          virtualDesc.isVirtual = true;
          texture = device.createTexture(virtualDesc);
          if (!texture)
          {
            problem = "NVRHI cannot create the texture";
          }
          else
          {
            const auto requirements = device.getTextureMemoryRequirements(texture);
            if (const auto placement = heaps->place(texture.Get(), requirements))
            {
              if (device.bindTextureMemory(
                    texture, heaps->heap(placement->heap), placement->range.offset))
              {
                ++m_texturesPlaced;
                m_largestPlacedBytes = std::max(m_largestPlacedBytes, requirements.size);
              }
              else
              {
                // P8.3: a bind refused is a wrong assumption (the probe's); its range
                // goes at once (nothing used it) and the texture is made dedicated
                heaps->retire(texture.Get(), 0);
                texture = nullptr;
                dedicatedReason = &m_dedicated.bindRefused;
              }
            }
            else
            {
              switch (heaps->lastRefusal())
              {
              case TextureHeaps::Refusal::Room:
                roomRefused = true;
                break;
              case TextureHeaps::Refusal::TooLarge:
                dedicatedReason = &m_dedicated.tooLarge;
                break;
              case TextureHeaps::Refusal::NoHeap:
              case TextureHeaps::Refusal::None:
                dedicatedReason = &m_dedicated.noHeap;
                break;
              }
              texture = nullptr;
            }
          }
        }
        else if (heaps && heaps->formatClassesForTesting())
        {
          dedicatedReason = &m_dedicated.refusedClass;
        }
        // a dedicated texture is a device allocation: U2.3's room before it
        if (problem.empty() && !texture && !roomRefused)
        {
          if (allocationRoom && !allocationRoom())
          {
            roomRefused = true;
          }
          else
          {
            texture = device.createTexture(*desc);
            if (!texture)
            {
              problem = "NVRHI cannot create the texture";
            }
            else
            {
              ++*dedicatedReason;
              m_largestDedicatedBytes = std::max(m_largestDedicatedBytes, bytes);
            }
          }
        }
      }
      // U2.3: no new device allocation within the allocation headroom (a dedicated
      // texture, or a new heap); the slot keeps binding its intrinsic. LPF.1 (D102): a
      // Replacing slot keeps binding its complete texture and keeps its image, so that a
      // later frame tries again (the frame is left unconverged); counted once until its
      // texture is made
      if (roomRefused && slot->state == ImageSlots::State::Replacing)
      {
        if (slots.waitForRoom(id))
        {
          ++m_headroomRefusals;
        }
        left = true;
        continue;
      }
      if (roomRefused)
      {
        problem = fmt::format(
          "the device's memory allocations are within {} of its limit "
          "(maxMemoryAllocationCount)",
          AllocationHeadroom);
        ++m_headroomRefusals;
      }
      if (!problem.empty())
      {
        slots.refuse(id, problem);
        continue;
      }
      slots.attachTexture(id, std::move(texture), image->numLevels - floorLevel, bytes);
      ++m_texturesMade;
    }

    // P2.14c: the smallest level first; a level is all its sides
    while (slot->firstWrittenMip > 0)
    {
      const auto level = slot->firstWrittenMip - 1;
      auto records = std::vector<const CachedImageLevel*>{};
      auto levelBytes = std::uint64_t{0};
      for (const auto& record : image->levels)
      {
        // LP6.4b: a floored texture's level is the image's less the floor
        if (record.level == level + floorLevel)
        {
          records.push_back(&record);
          levelBytes +=
            levelUploadBytes(*image, record) | kdl::value_or(std::uint64_t{0});
        }
      }
      // §3.10: within the frame's cap, unless it is the frame's first upload
      if (cap == 0 || (frameBytes > 0 && frameBytes + levelBytes > cap))
      {
        return true;
      }
      for (const auto* record : records)
      {
        // Image_load.cpp:883-889: each side's level with GetRowPitch's pitch
        const auto pitch = engineRowPitch(image->format, record->width).value_or(0);
        commandList.writeTexture(
          slot->texture,
          std::uint32_t(record->destZ),
          std::uint32_t(record->level - floorLevel),
          image->data.data() + record->offset,
          std::size_t(pitch));
      }
      frameBytes += levelBytes;
      m_uploadedBytes += levelBytes;
      slots.levelsWritten(id, level);
    }

    // Image_load.cpp:890-891, as the engine ends its upload; LPF.1 (D98): a Replacing
    // slot swaps its new texture in here, in the frame that wrote its last level. LP6.4b
    // (the director's reading (5)): a fed image's texture stays tracked (non-permanent)
    const auto fed = slot->state == ImageSlots::State::Replacing ? slot->replacingOverride
                                                                 : slot->adoptedOverride;
    if (m_permanentState && !fed)
    {
      commandList.setPermanentTextureState(
        slot->texture, nvrhi::ResourceStates::ShaderResource);
      commandList.commitBarriers();
    }
    slots.dropPixels(id);
  }
  return left;
}

bool TextureUploads::fitsInPlace(
  const CachedImage& image, const nvrhi::TextureDesc& desc, const std::uint64_t cap) const
{
  auto made = std::optional<nvrhi::TextureDesc>{};
  engineTextureDesc(image, m_d24s8Supported, {})
    | kdl::transform([&](nvrhi::TextureDesc d) { made = std::move(d); }) | kdl::ignore();
  if (
    !made || made->format != desc.format || made->dimension != desc.dimension
    || made->width != desc.width || made->height != desc.height
    || made->mipLevels != desc.mipLevels || made->arraySize != desc.arraySize)
  {
    return false;
  }
  auto bytes = std::uint64_t{0};
  for (const auto& level : image.levels)
  {
    auto ok = false;
    levelUploadBytes(image, level) | kdl::transform([&](const std::uint64_t b) {
      bytes += b;
      ok = true;
    }) | kdl::ignore();
    if (!ok)
    {
      return false;
    }
  }
  return cap > 0 && bytes <= cap;
}

bool TextureUploads::recordInPlace(
  nvrhi::ICommandList& commandList,
  ImageSlots& slots,
  const std::uint64_t cap,
  std::uint64_t& frameBytes)
{
  auto left = false;
  for (const auto id : slots.inPlace())
  {
    const auto* slot = slots.slot(id);
    const auto image = slot->pixels;
    auto bytes = std::uint64_t{0};
    for (const auto& level : image->levels)
    {
      bytes += levelUploadBytes(*image, level) | kdl::value_or(std::uint64_t{0});
    }
    // the frame's first in-place write always goes (the rule kept it within the cap);
    // another that would pass the cap waits whole for the next frame (A36)
    if (cap == 0 || (frameBytes > 0 && frameBytes + bytes > cap))
    {
      left = true;
      continue;
    }
    // Image_load.cpp:883-889: each side's level with GetRowPitch's pitch, every level
    for (const auto& level : image->levels)
    {
      const auto pitch = engineRowPitch(image->format, level.width).value_or(0);
      commandList.writeTexture(
        slot->texture,
        std::uint32_t(level.destZ),
        std::uint32_t(level.level),
        image->data.data() + level.offset,
        std::size_t(pitch));
    }
    frameBytes += bytes;
    m_uploadedBytes += bytes;
    ++m_feedsInPlace;
    slots.inPlaceWritten(id);
  }
  return left;
}

std::uint64_t TextureUploads::feedsInPlace() const
{
  return m_feedsInPlace;
}

void TextureUploads::setPermanentStateForTesting(const bool permanent)
{
  m_permanentState = permanent;
}

std::uint64_t TextureUploads::uploadedBytes() const
{
  return m_uploadedBytes;
}

std::uint64_t TextureUploads::texturesMade() const
{
  return m_texturesMade;
}

std::uint64_t TextureUploads::headroomRefusals() const
{
  return m_headroomRefusals;
}

std::uint64_t TextureUploads::texturesPlaced() const
{
  return m_texturesPlaced;
}

const TextureUploads::DedicatedCounts& TextureUploads::dedicated() const
{
  return m_dedicated;
}

std::uint64_t TextureUploads::largestPlacedBytes() const
{
  return m_largestPlacedBytes;
}

std::uint64_t TextureUploads::largestDedicatedBytes() const
{
  return m_largestDedicatedBytes;
}

} // namespace tb::lit
