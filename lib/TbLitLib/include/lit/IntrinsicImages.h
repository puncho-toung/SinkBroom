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
// renderer/DXT/DXTCodec.h and renderer/DXT/DXTEncoder_SSE2.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace tb::lit
{

/**
 * D69, SC271: what one of the engine's intrinsic generators hands GenerateImage: its
 * pixels (RGBA8, top row first) and its size, filter, repeat and usage. Mirrored from
 * Image_intrinsic.cpp at 285ea4d1 (mirror.json M-INTR-*): R_DefaultImage with developer 0
 * (zeros; the grey with a white border is com_developer's), R_WhiteImage, R_BlackImage,
 * R_BlackDiffuseImage, R_FlatNormalImage, R_CreateNoFalloffImage and R_QuadraticImage,
 * the images LP2 draws with. nullopt for every other name: the preview does not make it
 * yet (LP3-LP5 add theirs). Pure; any thread.
 */
struct IntrinsicRecipe
{
  int width = 0;
  int height = 0;
  // textureFilter_t, textureRepeat_t, textureUsage_t
  int filter = 0;
  int repeat = 0;
  int usage = 0;
  std::vector<std::uint8_t> rgba;
};
std::optional<IntrinsicRecipe> intrinsicRecipe(std::string_view name);

/**
 * D69, D24: a block-compressed level of one colour as the engine's fast DXT encoders
 * write it (CompressImageDXT5Fast, CompressYCoCgDXT5Fast, CompressNormalMapDXT5Fast with
 * image_highQualityCompression 0), by endpoint arithmetic: no encoder runs. rgba is the
 * colour the layout conversion gave every texel of the level (sourceImageLevels: the
 * YCoCg or normal-map layout already applied); colorFormat picks the encoder;
 * blocksWide × blocksHigh 16-byte DXT5 blocks. Pure.
 */
std::vector<std::uint8_t> constantDxt5Blocks(
  const std::array<std::uint8_t, 4>& rgba,
  int colorFormat,
  int blocksWide,
  int blocksHigh);

/**
 * D69 (LP2.1 step 6): an intrinsic image as the engine's cache would hold it, had the
 * engine binarized it: the recipe's pixels through GenerateImage's path, DeriveOpts for
 * its usage, the layout conversion and mips of Load2DFromMemory (decls'
 * sourceImageLevels, LP1c.2), then the bytes the engine's encoder would write: constant
 * DXT5 blocks for a DXT usage (every level of these images is one colour), the stored
 * bytes for INT8 and RGBA8. An error for a name with no recipe or a level that is not of
 * one colour. Parse thread (decls belongs to it, C20).
 */
Result<std::shared_ptr<const CachedImage>> intrinsicImage(
  MaterialDecls& decls, std::string_view name);

} // namespace tb::lit
