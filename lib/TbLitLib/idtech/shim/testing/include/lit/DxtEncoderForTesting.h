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

#include <cstdint>
#include <vector>

namespace tb::lit
{

/**
 * LP1c fix (S3.2), test only (TbLitIdDxtTestLib, linked by TbLitLibTest alone; D24: the
 * product never encodes BC on the CPU): the engine's fast DXT encoders, copied verbatim
 * (engine/renderer/DXT/DXTEncoder_SSE2.cpp, the encoders the engine's x64 build calls
 * with image_highQualityCompression 0), behind a std-only call.
 */
enum class DxtEncoderForTesting
{
  // CompressImageDXT1Fast (TD_SPECULAR, TD_COVERAGE's green alpha, TD_FONT, ...)
  Dxt1,
  // CompressImageDXT5Fast (DXT5 with CFM_DEFAULT)
  Dxt5,
  // CompressYCoCgDXT5Fast (CFM_YCOCG_DXT5)
  YCoCgDxt5,
  // CompressNormalMapDXT5Fast (CFM_NORMAL_DXT5)
  NormalMapDxt5,
};

/**
 * One level's blocks as the engine's binarizer encodes them from the RGBA8 bytes its
 * Load2DFromMemory or LoadCubeFromMemory hands its encoder (the source fallback's level
 * bytes, D24): a 2D level whose size is not a multiple of 4 is first padded with zeros to
 * whole blocks (BinaryImage.cpp:113-131), a cube level below 4 × 4 replicated into one
 * block (PadImageTo4x4, :653-671). Test only.
 */
std::vector<std::uint8_t> encodeDxtForTesting(
  DxtEncoderForTesting encoder,
  const std::vector<std::uint8_t>& rgba,
  int width,
  int height,
  bool cube);

// whether this build has the copied encoders (the SSE2 ones; empty blocks elsewhere)
bool dxtEncoderForTestingAvailable();

} // namespace tb::lit
