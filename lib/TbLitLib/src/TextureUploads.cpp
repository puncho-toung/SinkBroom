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

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
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

TextureUploads::TextureUploads(const bool d24s8Supported)
  : m_d24s8Supported{d24s8Supported}
{
}

void TextureUploads::setD24s8Supported(const bool supported)
{
  m_d24s8Supported = supported;
}

bool TextureUploads::record(
  nvrhi::IDevice& device,
  nvrhi::ICommandList& commandList,
  ImageSlots& slots,
  const std::uint64_t cap,
  std::uint64_t& frameBytes)
{
  for (const auto id : slots.loading())
  {
    const auto* slot = slots.slot(id);
    const auto image = slot->pixels;
    if (!image)
    {
      slots.refuse(id, "the image's bytes are gone before they were written");
      continue;
    }

    if (!slot->texture)
    {
      auto bytes = std::uint64_t{0};
      auto problem = std::string{};
      for (const auto& level : image->levels)
      {
        levelUploadBytes(*image, level)
          | kdl::transform([&](const std::uint64_t levelBytes) { bytes += levelBytes; })
          | kdl::if_error([&](const auto& e) { problem = e.msg; }) | kdl::ignore();
        if (!problem.empty())
        {
          break;
        }
      }
      auto texture = nvrhi::TextureHandle{};
      if (problem.empty())
      {
        engineTextureDesc(*image, m_d24s8Supported, slot->key.name)
          | kdl::transform([&](const nvrhi::TextureDesc& desc) {
              texture = device.createTexture(desc);
              if (!texture)
              {
                problem = "NVRHI cannot create the texture";
              }
            })
          | kdl::if_error([&](const auto& e) { problem = e.msg; }) | kdl::ignore();
      }
      if (!problem.empty())
      {
        slots.refuse(id, problem);
        continue;
      }
      slots.attachTexture(id, std::move(texture), image->numLevels, bytes);
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
        if (record.level == level)
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
          std::uint32_t(record->level),
          image->data.data() + record->offset,
          std::size_t(pitch));
      }
      frameBytes += levelBytes;
      m_uploadedBytes += levelBytes;
      slots.levelsWritten(id, level);
    }

    // Image_load.cpp:890-891, as the engine ends its upload
    if (m_permanentState)
    {
      commandList.setPermanentTextureState(
        slot->texture, nvrhi::ResourceStates::ShaderResource);
      commandList.commitBarriers();
    }
    slots.dropPixels(id);
  }
  return false;
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

} // namespace tb::lit
