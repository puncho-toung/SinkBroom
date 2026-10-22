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

#include "lit/MaterialDecls.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::lit
{

/**
 * LP1c.2 (C21): an image's upload size as the engine's image load gives it: a generated
 * image's generator size, a usable cache's header size, else its source's size; or the
 * 8 × 8 the engine gives an image whose load fails (defaulted).
 */
struct ImageUploadSize
{
  int width = 8;
  int height = 8;
  // Ready: the size the engine uploads; Default: the engine's defaulted 8 × 8 image, or a
  // size the preview cannot know (an intrinsic the engine sizes by its window)
  EditorImageSize::State state = EditorImageSize::State::Default;
  // idImage::IsDefaulted: the engine's load failed and made the 8 × 8 default
  bool defaulted = true;
  // why the size is Default, "" for Ready
  std::string reason;
  // every file the size was read from or found by the time call, for the re-probe
  std::vector<FoundGameFile> files;
};

/**
 * LP1c.2 (C21, A6, P.4), LP1c fix (S2.2): an image's upload size, never decoding a pixel:
 * the default image 16 × 16 and an intrinsic by its generator (Image_intrinsic.cpp);
 * otherwise the copied time call names the cache (MaterialDecls::imageCacheName), and a
 * cache the engine would use (not stale, its format matching DeriveOpts', the BFHACK name
 * too) gives its header's size, read without its body; otherwise the source, by headers
 * read through small bounded reads (readImageFileHeader): a 2D image or an image program
 * by its first leaf as the time call's leaf resolution found it (the .tga default,
 * png/tga/jpg/exr/hdr, the _rmao sibling; addnormals resamples its second leaf to the
 * first's size, and no program changes the size), when the time call found every leaf; a
 * .png or .jpg whose header is refused gives way to the next extension, as stb_image's
 * failure does in the engine (S3.3); a six-file cube by side 0 once all six exist at one
 * square size (MaterialDecls::cubeSideSizes); a CF_SINGLE cube by its cube size once the
 * picture holds the 4 × 3 sides; a panorama 1024. A missing leaf or side, or a refused
 * header, is the defaulted 8 × 8. Every file found or looked for is in files, for the
 * re-probe. Messages the copied code reports stay in the registry; the caller drops them.
 * Parse thread (C20).
 */
ImageUploadSize imageUploadSize(
  MaterialDecls& decls, DeclSource& source, const MaterialImageKey& key);

// the size of a material's editor image, and every file it was read from or looked for
struct EditorImageSizing
{
  EditorImageSize size;
  // LP1c fix (S3.15): the name of the image the size is of ("" for the default image),
  // which a kept Ready size is keyed by
  std::string image;
  std::vector<FoundGameFile> files;
};

/**
 * LP1c.2 (C21): idMaterial::GetEditorImage's upload size (Material.cpp:223-291): the
 * qer_editorimage loaded as TD_DIFFUSE (MaterialDecls::editorImageKey); when that load is
 * defaulted, the first SL_DIFFUSE stage's image, or stage 0's when that stage has none,
 * or still the defaulted editor image when no stage is diffuse; without qer_editorimage,
 * the first SL_DIFFUSE stage's image, else stage 0's; no stage or no image: the default
 * image (16 × 16). Each image is sized by imageUploadSize. Parse thread (C20).
 */
EditorImageSizing editorImageSize(
  MaterialDecls& decls, DeclSource& source, const MaterialEntry& material);

/**
 * Image_intrinsic.cpp's generators: the size an intrinsic image of this name is made at,
 * or nullopt for one the engine sizes by its window, a cvar or a font atlas. Pure; any
 * thread.
 */
std::optional<std::pair<int, int>> intrinsicImageSize(std::string_view name);

} // namespace tb::lit
