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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ImageFallback.h"

#include <string_view>
#include <utility>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// cubeFiles_t (Image.h): the images the engine itself makes in these layouts
constexpr int Cube2DArray = 4;

} // namespace

std::shared_ptr<const CachedImage> sourceImage(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  std::string& why)
{
  if (name.generated)
  {
    why = "the engine generates this image; it has no source";
    return nullptr;
  }
  if (name.cube == Cube2DArray || name.cube == CubePackedMipChain)
  {
    why =
      "an image array or packed mip chain comes from the engine's bakes; it has no "
      "source the preview loads";
    return nullptr;
  }

  // Image_load.cpp:716-865: the source, then DeriveOpts for its size (the format, colour
  // format and gamma mips stay the ones the time call's usage gave)
  auto load = decls.loadImagePixels(name, key.cubeMapSize, false);
  if (!load.pixels)
  {
    why = std::move(load.error);
    return nullptr;
  }
  const auto& pixels = *load.pixels;
  const auto opts = engineImageOpts(name.usage);
  const auto numLevels =
    engineMipLevels(opts.format, key.filter, pixels.width, pixels.height);
  auto image = decls.sourceImageLevels(
    pixels, opts.format, opts.colorFormat, numLevels, opts.gammaMips);
  if (image.levels.empty())
  {
    why = "the source image has no levels";
    return nullptr;
  }

  if (image.format == FormatRgb565)
  {
    // the cache reader's expansion, so a light image reads alike from either (D61)
    auto data = std::vector<std::uint8_t>{};
    for (auto& level : image.levels)
    {
      const auto stored = std::string_view{
        reinterpret_cast<const char*>(image.data.data()) + level.offset, level.size};
      level.offset = data.size();
      expandEngineRgb565(stored, data);
      level.size = data.size() - level.offset;
    }
    image.data = std::move(data);
    image.format = FormatRgba8;
  }

  image.file = name.generatedName;
  if (!pixels.files.empty())
  {
    image.origin = pixels.files.front().probe;
  }
  return std::make_shared<const CachedImage>(std::move(image));
}

ImageCacheSlot resolveImage(
  MaterialDecls& decls, DeclSource& source, const MaterialImageKey& key)
{
  auto name = decls.imageCacheName(key);
  if (!name)
  {
    auto slot = ImageCacheSlot{};
    slot.reason = std::move(name.error);
    slot.sourceReason = slot.reason;
    return slot;
  }
  auto slot = resolveImageCache(source, *name.value);
  if (slot.state == ImageCacheSlot::State::Ready)
  {
    return slot;
  }
  slot.image = sourceImage(decls, *name.value, key, slot.sourceReason);
  return slot;
}

} // namespace tb::lit
