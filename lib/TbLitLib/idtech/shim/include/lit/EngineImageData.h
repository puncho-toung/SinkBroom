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

// LP4.2a (D85, P4.13): the engine's image data headers copied whole into TbLitIdLib
// (renderer/Image_brdfLut.h, mirror.json M-INTR-08), behind a facade any thread may read:
// the arrays are constant data, constant-initialised (C9), never written.

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

} // namespace tb::lit
