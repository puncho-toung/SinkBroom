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

#pragma once

#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// renderer/Image.h's enum values that the cache stores and checks (SC96, SC99)
namespace engine_image
{
// textureFormat_t
constexpr int FormatNone = 0;
constexpr int FormatRgba8 = 1;
constexpr int FormatAlpha = 3;
constexpr int FormatInt8 = 6;
constexpr int FormatDxt1 = 7;
constexpr int FormatDxt5 = 8;
constexpr int FormatDepth = 9;
constexpr int FormatRgb565 = 12;
constexpr int FormatEtc1 = 13;
constexpr int FormatShadowArray = 14;
constexpr int FormatRg16f = 15;
constexpr int FormatRgba16f = 16;
constexpr int FormatRgba32f = 17;
constexpr int FormatR32f = 18;
constexpr int FormatR11g11b10f = 19;
constexpr int FormatBc6h = 20;
constexpr int FormatBc7 = 21;
constexpr int FormatR8f = 22;
constexpr int FormatDepthStencil = 23;
constexpr int FormatRgba16s = 24;
// textureColor_t
constexpr int ColorDefault = 0;
constexpr int ColorNormalDxt5 = 1;
constexpr int ColorYCoCgDxt5 = 2;
constexpr int ColorGreenAlpha = 3;
// textureType_t
constexpr int Type2D = 1;
constexpr int TypeCubic = 2;
constexpr int Type2DArray = 3;
// textureUsage_t
constexpr int UsageSpecular = 0;
constexpr int UsageDiffuse = 1;
constexpr int UsageDefault = 2;
constexpr int UsageBump = 3;
constexpr int UsageFont = 4;
constexpr int UsageLight = 5;
constexpr int UsageCoverage = 10;
constexpr int UsageSpecularPbrRmao = 12;
constexpr int UsageHdrLightProbe = 21;
constexpr int UsageHdri = 22;
// textureFilter_t
constexpr int FilterLinear = 0;
constexpr int FilterNearest = 1;
constexpr int FilterDefault = 3;
// textureRepeat_t
constexpr int RepeatClamp = 1;
// cubeFiles_t
constexpr int Cube2D = 0;
constexpr int CubePackedMipChain = 5;
constexpr int CubeNativeHdr = 8;
// BinaryImageData.h: 'B' 'I' 'M' and the version, little-endian bytes of one int
constexpr std::uint32_t BimageMagicBfg =
  0x42u | (0x49u << 8) | (0x4Du << 16) | (10u << 24);
constexpr std::uint32_t BimageMagic = 0x42u | (0x49u << 8) | (0x4Du << 16) | (11u << 24);
} // namespace engine_image

// the format and colour format idImage::DeriveOpts gives a usage (SC99)
struct EngineImageOpts
{
  int format = engine_image::FormatNone;
  int colorFormat = engine_image::ColorDefault;
};

/**
 * Image_load.cpp:150-292 DeriveOpts' switch, as the engine is built (STANDALONE off: a
 * TD_LIGHT image is RGB565). A usage the switch does not name asserts in the engine and
 * gets RGBA8. Pure; any thread.
 */
EngineImageOpts engineImageOpts(int usage);

/**
 * Image_load.cpp:294-318 DeriveOpts' mip count for an image of this size: 1 for a linear
 * or nearest filter; otherwise one per halving down to 1×1, stopping at a DXT1, DXT5,
 * BC6H or ETC1 level whose size is not a multiple of 4. (TD_FONT fixes 4 instead.) Pure.
 */
int engineMipLevels(int format, int filter, int width, int height);

// Image_load.cpp:44-104 BitsForFormat; 0 for an unknown format. Pure.
int engineBitsForFormat(int format);

// one mip level (one side's, for a cube) in CachedImage::data
struct CachedImageLevel
{
  int level = 0;
  // the cube side (0-5), 0 for a 2D image
  int destZ = 0;
  int width = 0;
  int height = 0;
  std::size_t offset = 0;
  std::size_t size = 0;
};

/**
 * A .bimage as LoadFromGeneratedFile reads it (SC98): plain data for the render thread,
 * which LP2 uploads. Immutable once published.
 */
struct CachedImage
{
  // the cache file the search found, and where (C8)
  std::string file;
  GameFileProbe origin;
  // bimageFile_t: the source time the cache was made from (Unix seconds, A4), the magic
  // (BIM 10 or 11), textureType, the format as stored, the format of data (RGBA8 for a
  // stored RGB565, which is expanded as the engine expands it), colorFormat, the size and
  // the mip count
  std::int64_t sourceTime = 0;
  std::uint32_t magic = 0;
  int textureType = 0;
  int storedFormat = 0;
  int format = 0;
  int colorFormat = 0;
  int width = 0;
  int height = 0;
  int numLevels = 0;
  // numLevels records, six times for a cube (side-major, as written); each level's bytes
  // in the engine's format: BC blocks as stored (the engine pads its own copy to NVRHI's
  // row pitch; LP2 pads at upload), RGBA8 as stored, RGB565 as RGBA8
  std::vector<CachedImageLevel> levels;
  std::vector<std::uint8_t> data;
};

/**
 * BinaryImage.cpp:994-1128 LoadFromGeneratedFile's read of a .bimage's bytes, without the
 * time test: big-endian; the header (int64 source time, then magic, textureType, format,
 * colorFormat, width, height, numLevels as int32), then numLevels records (×6 for a
 * cube), each {level, destZ, width, height, dataSize} and dataSize bytes; RGB565 stored
 * high byte first and expanded to RGBA8 ((5 bits × 527 + 23) >> 6, (6 bits × 259 + 33) >>
 * 6, alpha 255). Every count and size is checked against the bytes before it is used: a
 * file that is cut short, has a negative count or an odd RGB565 level fails, where the
 * engine would read past the end or garbage (M-IMG-DIFF-01). A wrong magic fails as in
 * the engine. Pure; any thread.
 */
Result<CachedImage> readBimage(std::string_view bytes);

/**
 * BinaryImage.cpp:1016-1019, the staleness test as A3 corrects it: a cache is rejected
 * only when the source time is neither FILE_NOT_FOUND_TIMESTAMP (-1) nor 0 and differs
 * from the stored one (com_productionMode 0). Pure.
 */
bool engineCacheIsStale(std::int64_t sourceTime, std::int64_t storedTime);

/**
 * Image_load.cpp:674-680: whether a cache read for an image may be used: its colour
 * format, format (a stored RGB565 read as RGBA8 counts for RGB565; R11G11B10F counts for
 * BC6H) and texture type equal DeriveOpts' for the usage. Pure.
 */
bool engineCacheMatches(const CachedImage& image, int usage, int textureType);

// what the cache reader found for an image (C8, A3)
struct ImageCacheSlot
{
  enum class State
  {
    // the engine would use this cache
    Ready,
    // no cache file exists; LP1c.2's source fallback takes over
    Missing,
    // a cache file exists, but the engine would make a new one: its source time differs,
    // its format does not match the usage, or it cannot be read (reason says which)
    Stale,
  };

  State state = State::Missing;
  std::shared_ptr<const CachedImage> image;
  // the name and the source time the search used
  ImageCacheName name;
  // the cache file used or last rejected; "" when none was found
  std::string file;
  std::string reason;
};

/**
 * Image_load.cpp:625-695 for one image: the cache file through source's C8 search
 * (D23), read and tested as LoadFromGeneratedFile tests it (engineCacheIsStale), then,
 * when it is not loaded and resource files are in use, the BFHACK name; a loaded cache is
 * Ready when engineCacheMatches. The editor image is never touched (C8). Parse thread:
 * source belongs to it (C20).
 */
ImageCacheSlot resolveImageCache(DeclSource& source, const ImageCacheName& name);

} // namespace tb::lit
