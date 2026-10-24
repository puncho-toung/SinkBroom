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
#include "lit/EngineImageEnums.h"
#include "lit/MaterialDecls.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// the format and colour format idImage::DeriveOpts gives a usage (SC99), and whether its
// mips are made with gamma correction (LP1c.2's source fallback)
struct EngineImageOpts
{
  int format = engine_image::FormatNone;
  int colorFormat = engine_image::ColorDefault;
  bool gammaMips = false;
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

/**
 * BinaryImage.cpp:994-1128 LoadFromGeneratedFile's read of a .bimage's bytes, without the
 * time test: big-endian; the header (int64 source time, then magic, textureType, format,
 * colorFormat, width, height, numLevels as int32), then numLevels records (×6 for a
 * cube), each {level, destZ, width, height, dataSize} and dataSize bytes; RGB565 stored
 * high byte first and expanded to RGBA8 ((5 bits × 527 + 23) >> 6, (6 bits × 259 + 33) >>
 * 6, alpha 255). Every count and size is checked against the bytes before it is used: a
 * file that is cut short, has a negative count or an odd RGB565 level fails, where the
 * engine would read past the end or garbage (M-IMG-DIFF-01). A wrong magic fails as in
 * the engine. LP1c fix (S3.5): the header's sides must be 1 to 16384 and its level count
 * 1 to the levels its larger side halves into, and each level record must be the one the
 * engine's binarizer writes at its place (record k of a 2D image level k; of a cube level
 * k / 6, side k % 6), of that level's size (a block format's cube level below 4 padded to
 * 4) and with at least the bytes its format needs for that size (whole 4 × 4 blocks for
 * DXT1, DXT5, BC6H, BC7, ETC1), else the read fails and the cache is Stale: the shape LP2
 * uploads is the header's. Pure; any thread.
 */
Result<CachedImage> readBimage(std::string_view bytes);

// LP1c.2 (C21): readBimage's header only (the first 36 bytes), without the level records:
// the size, formats and time, with readBimage's header checks. Pure; any thread.
Result<CachedImage> readBimageHeader(std::string_view bytes);

/**
 * BinaryImage.cpp:1085-1110: RGB565 levels stored high byte first, expanded to RGBA8 with
 * Option 1 ((5 bits × 527 + 23) >> 6, (6 bits × 259 + 33) >> 6, alpha 255), appended to
 * out; a trailing odd byte is dropped. The cache reader and the source fallback share it,
 * so a light image reads alike from either (D61). Pure; any thread.
 */
void expandEngineRgb565(std::string_view stored, std::vector<std::uint8_t>& out);

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
  // Ready: the cache; Missing or Stale: the source fallback's image (fromSource), or null
  // when the source could not be loaded (sourceReason) and the image is the default
  std::shared_ptr<const CachedImage> image;
  // the name and the source time the search used
  ImageCacheName name;
  // the cache file used or last rejected; "" when none was found
  std::string file;
  std::string reason;
  // LP1c.2: why the source fallback made no image, "" when it made one or did not run
  std::string sourceReason;
  // LP1c.2 (C21): every file the slot was read from or the time call found: the cache
  // files read, the sources the fallback read; the service re-reads the slot when a
  // loose one changes
  std::vector<FoundGameFile> files;
  // LPF.0a (C39, C41): the image is an override's, converted from the material service's
  // store (MaterialService::setImageOverride): Ready, no files, never polled nor dropped
  // at a build; the store is its truth
  bool override = false;
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
