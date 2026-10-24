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
#include "lit/ImageCache.h"
#include "lit/MaterialDecls.h"

#include <memory>
#include <string>
#include <vector>

namespace tb::lit
{

/**
 * LP1c.2 (S14, D24, D61): an image made from its source where the engine would binarize
 * one: the copied loaders and image programs read it (MaterialDecls::loadImagePixels,
 * every file validated first, D51), and its levels are laid out as the engine's
 * Load2DFromMemory or LoadCubeFromMemory lays them out for its encoder
 * (MaterialDecls::sourceImageLevels): with DeriveOpts' format, colour format and gamma
 * mips for the usage the time call found, the mip count for the picture's size and the
 * key's filter, RGBA8 where the engine would encode DXT, R11G11B10F where it would encode
 * BC6H, RGB565 expanded to RGBA8 as the cache reader expands it. An error saying why when
 * the source cannot be loaded (the image is then the default: the engine's 8 × 8
 * defaulted image) or the image is one the engine makes itself (an intrinsic, a packed
 * mip chain, an array). The files the loaders read or looked for are appended to files.
 * Writes no file (D11's spirit). Parse thread (C20).
 */
Result<std::shared_ptr<const CachedImage>> sourceImage(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  std::vector<FoundGameFile>& files);

/**
 * LPF.0a (C39 (a)): sourceImage's conversion of a picture already in memory (a loaded
 * source, or an override's pixels): DeriveOpts' format, colour format and gamma mips for
 * name.usage (the time call's usage, never the key's: P5.1), the mip count for the
 * picture's size and the key's filter, MaterialDecls::sourceImageLevels, and RGB565
 * expanded to RGBA8 as the cache reader expands it; the image's file is name's generated
 * name. An error when no level is made (a stop, a size of 0). Parse thread (C20).
 */
Result<CachedImage> sourceLevels(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  SourcePixels pixels);

/**
 * LP1c.2: an image as the preview has it: the engine's cache when the engine would use it
 * (resolveImageCache), else, for a Missing or Stale cache, sourceImage. A name the copied
 * time call cannot make is Missing with its error. The editor's own image is never asked
 * (C8): the editor-image sizes have their own path (C21). Parse thread (C20).
 */
ImageCacheSlot resolveImage(
  MaterialDecls& decls, DeclSource& source, const MaterialImageKey& key);

} // namespace tb::lit
