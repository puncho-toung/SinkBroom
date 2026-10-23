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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_load.cpp,
// renderer/BinaryImage.cpp and renderer/BinaryImageData.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ImageCache.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// big-endian reads over the cache's bytes, each checked against what is left
class BigReader
{
private:
  std::string_view m_bytes;
  std::size_t m_offset = 0;

public:
  explicit BigReader(const std::string_view bytes)
    : m_bytes{bytes}
  {
  }

  std::size_t offset() const { return m_offset; }
  std::size_t left() const { return m_bytes.size() - m_offset; }

  std::optional<std::uint64_t> big(const std::size_t size)
  {
    if (size > left())
    {
      return std::nullopt;
    }
    auto value = std::uint64_t{0};
    for (std::size_t i = 0; i < size; ++i)
    {
      value = (value << 8) | std::uint8_t(m_bytes[m_offset + i]);
    }
    m_offset += size;
    return value;
  }

  std::optional<std::int32_t> int32()
  {
    const auto value = big(4);
    return value ? std::optional{std::int32_t(std::uint32_t(*value))} : std::nullopt;
  }

  std::optional<std::string_view> bytes(const std::size_t size)
  {
    if (size > left())
    {
      return std::nullopt;
    }
    const auto view = m_bytes.substr(m_offset, size);
    m_offset += size;
    return view;
  }
};

Error cacheError(const std::string_view what)
{
  return Error{fmt::format("The image cache cannot be read: {}", what)};
}

} // namespace

// BinaryImage.cpp:1085-1110: RGB565, high byte first, to RGBA8 (Option 1)
void expandEngineRgb565(const std::string_view stored, std::vector<std::uint8_t>& out)
{
  for (std::size_t i = 0; i + 1 < stored.size(); i += 2)
  {
    const auto value =
      std::uint16_t((std::uint8_t(stored[i]) << 8) | std::uint8_t(stored[i + 1]));
    out.push_back(std::uint8_t((((value) >> 11) * 527 + 23) >> 6));
    out.push_back(std::uint8_t((((value & 0x07E0) >> 5) * 259 + 33) >> 6));
    out.push_back(std::uint8_t((((value & 0x001F)) * 527 + 23) >> 6));
    out.push_back(0xFF);
  }
}

EngineImageOpts engineImageOpts(const int usage)
{
  // Image_load.cpp:152-291
  switch (usage)
  {
  case UsageCoverage:
    return {FormatDxt1, ColorGreenAlpha, false};
  case UsageDiffuse:
    // TD_DIFFUSE gets only set to when its a diffuse texture for an interaction
    return {FormatDxt5, ColorYCoCgDxt5, true};
  case UsageSpecular:
    return {FormatDxt1, ColorDefault, true};
  case UsageSpecularPbrRmao:
    return {FormatDxt1, ColorDefault, false};
  case 13: // TD_SPECULAR_PBR_RMAOD
    return {FormatDxt5, ColorDefault, false};
  case UsageDefault:
    return {FormatDxt5, ColorDefault, true};
  case UsageBump:
    return {FormatDxt5, ColorNormalDxt5, false};
  case UsageFont:
    return {FormatDxt1, ColorGreenAlpha, true};
  case UsageLight:
    // D3 BFG assets require RGB565 (the engine is built without STANDALONE)
    return {FormatRgb565, ColorDefault, true};
  case 6: // TD_LOOKUP_TABLE_MONO
    return {FormatInt8, ColorDefault};
  case 7: // TD_LOOKUP_TABLE_ALPHA
    return {FormatAlpha, ColorDefault};
  case 8: // TD_LOOKUP_TABLE_RGB1
  case 9: // TD_LOOKUP_TABLE_RGBA
    return {FormatRgba8, ColorDefault};
  case UsageHdrLightProbe:
  case UsageHdri:
    return {FormatBc6h, ColorDefault};
  case 14: // TD_HIGHQUALITY_CUBE
    return {FormatRgba8, ColorDefault, true};
  case 11: // TD_DEPTH
    return {FormatDepth, ColorDefault};
  case 26: // TD_DEPTH_STENCIL
    return {FormatDepthStencil, ColorDefault};
  case 16: // TD_SHADOW_ARRAY
    return {FormatShadowArray, ColorDefault};
  case 17: // TD_RG16F
    return {FormatRg16f, ColorDefault};
  case 18: // TD_RGBA16F
    return {FormatRgba16f, ColorDefault};
  case 19: // TD_RGBA16S
    return {FormatRgba16s, ColorDefault};
  case 20: // TD_RGBA32F
    return {FormatRgba32f, ColorDefault};
  case 23: // TD_R32F
    return {FormatR32f, ColorDefault};
  case 24: // TD_R8F
    return {FormatR8f, ColorDefault};
  default:
    // assert( false )
    return {FormatRgba8, ColorDefault};
  }
}

int engineMipLevels(const int format, const int filter, const int width, const int height)
{
  // Image_load.cpp:294-318
  auto numLevels = 1;
  if (filter == FilterLinear || filter == FilterNearest)
  {
    // don't create mip maps if we aren't going to be using them
    return numLevels;
  }
  auto tempWidth = width;
  auto tempHeight = height;
  while (tempWidth > 1 || tempHeight > 1)
  {
    tempWidth >>= 1;
    tempHeight >>= 1;
    if (
      (format == FormatDxt1 || format == FormatDxt5 || format == FormatBc6h
       || format == FormatEtc1)
      && ((tempWidth & 0x3) != 0 || (tempHeight & 0x3) != 0))
    {
      break;
    }
    numLevels++;
  }
  return numLevels;
}

int engineBitsForFormat(const int format)
{
  // Image_load.cpp:44-104
  switch (format)
  {
  case FormatNone:
    return 0;
  case FormatRgba8:
  case 2: // FMT_XRGB8
    return 32;
  case FormatRgb565:
  case 4: // FMT_L8A8
    return 16;
  case FormatAlpha:
  case 5: // FMT_LUM8
  case FormatInt8:
    return 8;
  case FormatDxt1:
    return 4;
  case FormatDxt5:
    return 8;
  case FormatEtc1:
    return 4;
  case FormatShadowArray:
    return 32 * 6;
  case FormatRg16f:
    return 32;
  case FormatRgba16f:
  case FormatRgba16s:
    return 64;
  case FormatRgba32f:
    return 128;
  case FormatR32f:
  case FormatR11g11b10f:
    return 32;
  case FormatBc6h:
  case FormatBc7:
    return 8;
  case FormatDepth:
  case FormatDepthStencil:
    return 32;
  case 10: // FMT_X16
    return 16;
  case 11: // FMT_Y16_X16
    return 32;
  case FormatR8f:
    return 4;
  default:
    return 0;
  }
}

namespace
{

// LP1c fix (S3.5): the largest side a cache may declare (the image limits of the loader
// pre-validation, D51)
constexpr int MaxCacheSide = MaxImageFileDimension;

// floor(log2(side)) + 1: the most levels a side can halve into
int mostLevels(const int side)
{
  auto levels = 1;
  for (auto s = side; s > 1; s >>= 1)
  {
    ++levels;
  }
  return levels;
}

// the formats the engine encodes in 4 × 4 blocks, and a block's bytes
int blockBytes(const int format)
{
  switch (format)
  {
  case FormatDxt1:
  case FormatEtc1:
    return 8;
  case FormatDxt5:
  case FormatBc6h:
  case FormatBc7:
    return 16;
  default:
    return 0;
  }
}

// bimageFile_t, 36 bytes (packed); the engine's Read takes a short header as it is.
// LP1c fix (S3.5): its size and level count are checked, so that no header with sides of
// 0, beyond the limits or more levels than its sides halve into is used
Result<CachedImage> readHeader(BigReader& in)
{
  auto image = CachedImage{};
  const auto time = in.big(8);
  const auto magic = in.int32();
  const auto textureType = in.int32();
  const auto format = in.int32();
  const auto colorFormat = in.int32();
  const auto width = in.int32();
  const auto height = in.int32();
  const auto numLevels = in.int32();
  if (!numLevels)
  {
    return cacheError("the header is cut short");
  }
  image.sourceTime = std::int64_t(*time);
  image.magic = std::uint32_t(*magic);
  if (image.magic != BimageMagicBfg && image.magic != BimageMagic)
  {
    return cacheError(fmt::format("bad magic {:08x}", image.magic));
  }
  image.textureType = *textureType;
  image.storedFormat = *format;
  image.format = *format;
  image.colorFormat = *colorFormat;
  image.width = *width;
  image.height = *height;
  image.numLevels = *numLevels;
  if (
    image.width < 1 || image.height < 1 || image.width > MaxCacheSide
    || image.height > MaxCacheSide)
  {
    return cacheError(fmt::format("bad size {} x {}", image.width, image.height));
  }
  // U3.15: a cube's sides are square (Vulkan's cube-compatible images; the engine's
  // binarizer writes the six sides of a square image)
  if (image.textureType == TypeCubic && image.width != image.height)
  {
    return cacheError(fmt::format(
      "a cube of {} x {}: its sides are not square", image.width, image.height));
  }
  if (
    image.numLevels < 1
    || image.numLevels > mostLevels(std::max(image.width, image.height)))
  {
    return cacheError(fmt::format(
      "bad level count {} for {} x {}", image.numLevels, image.width, image.height));
  }
  return image;
}

/**
 * LP1c fix (S3.5): a level record as the engine's binarizer writes it
 * (BinaryImage.cpp:56-331, :679-837): record k of a 2D image is level k, of a cube level
 * k / 6 and side k % 6 (images[ level * 6 + side ]); its size is the level's, a block
 * format's cube level below 4 padded to 4 (PadImageTo4x4); and its data size at least
 * the bytes its format needs for that size (whole blocks for a block format, as
 * LoadFromGeneratedFile's assert demands).
 */
std::optional<std::string> recordProblem(
  const CachedImage& image,
  const std::int64_t index,
  const int level,
  const int destZ,
  const int width,
  const int height,
  const std::int64_t dataSize)
{
  const auto cube = image.textureType == TypeCubic;
  const auto expectedLevel = int(cube ? index / 6 : index);
  const auto expectedSide = int(cube ? index % 6 : 0);
  const auto blocks = blockBytes(image.storedFormat);
  auto expectedWidth = std::max(1, image.width >> expectedLevel);
  auto expectedHeight = std::max(1, image.height >> expectedLevel);
  if (cube && blocks > 0 && expectedWidth < 4)
  {
    expectedWidth = expectedHeight = 4;
  }
  if (level != expectedLevel || destZ != expectedSide)
  {
    return fmt::format(
      "level record {} is level {} side {}, not level {} side {}",
      index,
      level,
      destZ,
      expectedLevel,
      expectedSide);
  }
  if (width != expectedWidth || height != expectedHeight)
  {
    return fmt::format(
      "level record {} is {} x {}, not {} x {}",
      index,
      width,
      height,
      expectedWidth,
      expectedHeight);
  }
  const auto bits = engineBitsForFormat(image.storedFormat);
  if (bits == 0)
  {
    return fmt::format("its format {} is not one the engine stores", image.storedFormat);
  }
  const auto needed =
    blocks > 0 ? std::int64_t((width + 3) / 4) * std::int64_t((height + 3) / 4) * blocks
               : std::int64_t(width) * std::int64_t(height) * bits / 8;
  if (dataSize < needed)
  {
    return fmt::format(
      "level record {} holds {} bytes, fewer than the {} its {} x {} need",
      index,
      dataSize,
      needed,
      width,
      height);
  }
  return std::nullopt;
}

// the level records and their bytes after the header
Result<CachedImage> readLevels(BigReader& in, CachedImage image)
{
  // :1022-1026
  auto numImages = std::int64_t(image.numLevels);
  if (image.textureType == TypeCubic)
  {
    numImages *= 6;
  }
  // every record takes 20 bytes at least
  if (numImages < 0 || std::uint64_t(numImages) > in.left() / 20)
  {
    return cacheError(fmt::format("bad level count {}", image.numLevels));
  }

  const auto rgb565 = image.storedFormat == FormatRgb565;
  image.levels.reserve(std::size_t(numImages));
  for (auto i = std::int64_t{0}; i < numImages; ++i)
  {
    const auto level = in.int32();
    const auto destZ = in.int32();
    const auto levelWidth = in.int32();
    const auto levelHeight = in.int32();
    const auto dataSize = in.int32();
    if (!dataSize)
    {
      return cacheError(fmt::format("level record {} is cut short", i));
    }
    if (*dataSize < 0 || (rgb565 && *dataSize % 2 != 0))
    {
      return cacheError(fmt::format("level {} has a bad size {}", i, *dataSize));
    }
    if (
      const auto problem =
        recordProblem(image, i, *level, *destZ, *levelWidth, *levelHeight, *dataSize))
    {
      return cacheError(*problem);
    }
    const auto stored = in.bytes(std::size_t(*dataSize));
    if (!stored)
    {
      return cacheError(fmt::format("level {} is cut short", i));
    }

    auto record = CachedImageLevel{};
    record.level = *level;
    record.destZ = *destZ;
    record.width = *levelWidth;
    record.height = *levelHeight;
    record.offset = image.data.size();
    if (rgb565)
    {
      // SRS - Convert FMT_RGB565 16-bits to FMT_RGBA8 32-bits
      expandEngineRgb565(*stored, image.data);
    }
    else
    {
      image.data.insert(image.data.end(), stored->begin(), stored->end());
    }
    record.size = image.data.size() - record.offset;
    image.levels.push_back(record);
  }
  if (rgb565)
  {
    image.format = FormatRgba8;
  }
  return image;
}

} // namespace

Result<CachedImage> readBimageHeader(const std::string_view bytes)
{
  auto in = BigReader{bytes};
  return readHeader(in);
}

Result<CachedImage> readBimage(const std::string_view bytes)
{
  auto in = BigReader{bytes};
  return readHeader(in) | kdl::and_then([&](CachedImage image) {
           return readLevels(in, std::move(image));
         });
}

bool engineCacheIsStale(const std::int64_t sourceTime, const std::int64_t storedTime)
{
  // RB: source might be from .resources, so we ignore the time stamp
  return sourceTime != -1 && sourceTime != 0 && sourceTime != storedTime;
}

bool engineCacheMatches(const CachedImage& image, const int usage, const int textureType)
{
  const auto opts = engineImageOpts(usage);
  // SRS: handle case when image read is cached and RGB565 format conversion is already
  // done; RB: allow R11G11B10 instead of BC6
  return image.colorFormat == opts.colorFormat
         && (image.format == opts.format || (image.format == FormatRgba8 && opts.format == FormatRgb565) || (image.format == FormatR11g11b10f && opts.format == FormatBc6h))
         && image.textureType == textureType;
}

ImageCacheSlot resolveImageCache(DeclSource& source, const ImageCacheName& name)
{
  auto slot = ImageCacheSlot{};
  slot.name = name;
  if (name.generated)
  {
    slot.reason = "the engine generates this image; it has no cache";
    return slot;
  }

  auto found = false;
  // BinaryImage.cpp:971-985 LoadFromGeneratedFile( sourceFileTime ): the image, or why
  // it was not loaded
  const auto load = [&](const std::string& file) -> std::optional<CachedImage> {
    auto origin = GameFileProbe{};
    const auto bytes = source.readGameFile(file, &origin);
    // LP1c fix round 2 (X.3): a missed cache name is kept too, so that a cache the engine
    // writes later is seen by the re-probe and by a reload
    slot.files.push_back(FoundGameFile{file, origin});
    if (!bytes)
    {
      return std::nullopt;
    }
    found = true;
    slot.file = file;
    return readBimage(*bytes)
           | kdl::and_then([&](CachedImage image) -> Result<CachedImage> {
               if (engineCacheIsStale(name.sourceTime, image.sourceTime))
               {
                 return Error{fmt::format(
                   "its source time {} differs from the source's {}",
                   image.sourceTime,
                   name.sourceTime)};
               }
               image.file = file;
               image.origin = origin;
               return image;
             })
           | kdl::transform(
             [](CachedImage image) { return std::optional{std::move(image)}; })
           | kdl::transform_error([&](const auto& e) {
               slot.reason = e.msg;
               return std::optional<CachedImage>{};
             })
           | kdl::value();
  };

  auto image = load(name.file);
  // BFHACK, do not want to tweak on buildgame so catch these images here
  if (!image && source.usingResourceFiles() && !name.bfhackFile.empty())
  {
    image = load(name.bfhackFile);
  }

  if (!image)
  {
    slot.state = found ? ImageCacheSlot::State::Stale : ImageCacheSlot::State::Missing;
    if (!found)
    {
      slot.reason = "binarize: binary file not found '" + name.generatedName + "'";
    }
    return slot;
  }

  if (!engineCacheMatches(*image, name.usage, name.textureType))
  {
    const auto opts = engineImageOpts(name.usage);
    // :700-711's reasons, and the format, which the engine's message leaves out
    slot.state = ImageCacheSlot::State::Stale;
    slot.reason = image->colorFormat != opts.colorFormat
                    ? "binarize: mismatched color format '" + name.generatedName + "'"
                  : image->textureType != name.textureType
                    ? "binarize: mismatched texture type '" + name.generatedName + "'"
                    : "binarize: mismatched format '" + name.generatedName + "'";
    return slot;
  }

  slot.state = ImageCacheSlot::State::Ready;
  slot.reason.clear();
  slot.image = std::make_shared<const CachedImage>(std::move(*image));
  return slot;
}

} // namespace tb::lit
