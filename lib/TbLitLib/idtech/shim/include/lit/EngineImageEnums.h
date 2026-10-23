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

namespace tb::lit
{

/**
 * renderer/Image.h's and renderer/Material.h's enum values that the image cache stores
 * and checks (SC96, SC99) and the samplers and intrinsic images use. U1.5: each one is
 * static_asserted against the copied enum in ShimPipelineState.cpp, which compiles the
 * copied Image.h and Material.h; the .bimage magic is BinaryImageData.h's, which is not
 * copied (BinaryImage.cpp's reader is mirrored in ImageCache.cpp, M-IMG-*).
 */
namespace engine_image
{
// textureFormat_t
constexpr int FormatNone = 0;
constexpr int FormatRgba8 = 1;
constexpr int FormatXrgb8 = 2;
constexpr int FormatAlpha = 3;
constexpr int FormatL8a8 = 4;
constexpr int FormatLum8 = 5;
constexpr int FormatInt8 = 6;
constexpr int FormatDxt1 = 7;
constexpr int FormatDxt5 = 8;
constexpr int FormatDepth = 9;
constexpr int FormatX16 = 10;
constexpr int FormatY16X16 = 11;
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
// U1.5: the lookup tables, which only the intrinsic images use
constexpr int UsageLookupTableMono = 6;
constexpr int UsageLookupTableRgb1 = 8;
constexpr int UsageCoverage = 10;
constexpr int UsageSpecularPbrRmao = 12;
constexpr int UsageHdrLightProbe = 21;
constexpr int UsageHdri = 22;
// textureFilter_t
constexpr int FilterLinear = 0;
constexpr int FilterNearest = 1;
constexpr int FilterNearestMipmap = 2;
constexpr int FilterDefault = 3;
// textureRepeat_t
constexpr int RepeatRepeat = 0;
constexpr int RepeatClamp = 1;
constexpr int RepeatClampToZero = 2;
constexpr int RepeatClampToZeroAlpha = 3;
// cubeFiles_t
constexpr int Cube2D = 0;
constexpr int CubePackedMipChain = 5;
constexpr int CubeNativeHdr = 8;
// BinaryImageData.h: 'B' 'I' 'M' and the version, little-endian bytes of one int
constexpr std::uint32_t BimageMagicBfg =
  0x42u | (0x49u << 8) | (0x4Du << 16) | (10u << 24);
constexpr std::uint32_t BimageMagic = 0x42u | (0x49u << 8) | (0x4Du << 16) | (11u << 24);
} // namespace engine_image

} // namespace tb::lit
