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

#include <cstddef>
#include <cstdint>
#include <vector>

// LP4.2a (D85, P4.13): the engine's image data headers copied whole into TbLitIdLib
// (renderer/Image_brdfLut.h, mirror.json M-INTR-08; LP3.0:
// renderer/Image_blueNoiseVC_2.h, M-SHADOW-COPY-01), behind a facade any thread may read:
// the arrays are constant data, constant-initialised (C9), never written. LP3.0's
// blue-noise generator is the one exception: the parse thread's.

namespace tb::lit
{

/**
 * renderer/Image_brdfLut.h's brfLutTexBytes, whole: the split-sum BRDF lookup table that
 * R_CreateBrdfLutImage (Image_intrinsic.cpp:1021-1024) hands GenerateImage as TD_RG16F,
 * BRDFLUT_TEX_WIDTH × BRDFLUT_TEX_HEIGHT texels of two IEEE halves each (R then G, little
 * endian), top row first: 256 × 256 × 4 = 262,144 bytes. The header's own
 * BRDFLUT_TEX_PITCH and BRDFLUT_TEX_SIZE are half that (two bytes a texel) and used by
 * nothing: the size here is the array's. A pointer and a size, as TbLitIdLib is C++17.
 */
const std::uint8_t* engineBrdfLutData();
std::size_t engineBrdfLutSize();
// BRDFLUT_TEX_WIDTH and BRDFLUT_TEX_HEIGHT
int engineBrdfLutWidth();
int engineBrdfLutHeight();

// what an intrinsic image's generator hands idImage::GenerateImage: its pixels (RGBA8, in
// the generator's order, row by row) and its size and options (textureFilter_t,
// textureRepeat_t, textureUsage_t)
struct EngineGeneratedImage
{
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  int filter = 0;
  int repeat = 0;
  int usage = 0;
};

/**
 * LP3.0 (C60, P3.24a, SC732): _blueNoise256 as the engine makes it: the copied
 * generator R_CreateBlueNoise256Image (Image_intrinsic.cpp:793-815) run on the copied
 * table renderer/Image_blueNoiseVC_2.h (blueNoiseTexBytes, 512 × 512 RGB8 texels, 786,432
 * bytes, top row first; the header's comment "Stored in R8 format" is wrong). The
 * generator fills its static RGBA8 buffer data[x][y] from the table's texel (x, y), so
 * the image it hands GenerateImage is the table transposed: the image's texel in row r
 * and column c is the table's texel in row c and column r; its alpha is the byte 1, not
 * 255; BLUENOISE_TEX_WIDTH × BLUENOISE_TEX_HEIGHT, TF_NEAREST, TR_REPEAT,
 * TD_LOOKUP_TABLE_RGBA. The generator writes its function-local static on every call:
 * the parse thread alone (C20), as intrinsicImage.
 */
EngineGeneratedImage engineBlueNoise256Image();
// the copied table itself (BLUENOISE_TEX_WIDTH, _HEIGHT and _PITCH: 512, 512, 1536);
// constant data, any thread
const std::uint8_t* engineBlueNoiseTable();
std::size_t engineBlueNoiseTableSize();
int engineBlueNoiseTableWidth();
int engineBlueNoiseTableHeight();
int engineBlueNoiseTablePitch();

} // namespace tb::lit
