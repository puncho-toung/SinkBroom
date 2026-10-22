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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/BinaryImage.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c fix (S3.2), test only: the copied fast DXT encoders behind a std-only call, for
// the image goldens' exact comparison with the engine's caches. Linked by TbLitLibTest
// alone (TbLitIdDxtTestLib); never in the product (D24).

// clang-format off: the std-only header, then precompiled.h, then the copied codec
#include "lit/DxtEncoderForTesting.h"
#include "precompiled.h"
#include "renderer/DXT/DXTCodec.h"
// clang-format on

// The copied encoders report their progress to the binarizer's pacifier, which the
// product's shim declares and never defines: a test has none to report to
void idCommon::LoadPacifierBinarizeProgressIncrement(int) {}

namespace tb::lit
{
namespace
{

// BinaryImage.cpp:653-671 PadImageTo4x4: a level below 4 × 4 replicated into one block
void padImageTo4x4(const byte* src, int width, int height, byte dest[64])
{
  for (int y = 0; y < 4; y++)
  {
    int sy = y % height;
    for (int x = 0; x < 4; x++)
    {
      int sx = x % width;
      for (int c = 0; c < 4; c++)
      {
        dest[(y * 4 + x) * 4 + c] = src[(sy * width + sx) * 4 + c];
      }
    }
  }
}

} // namespace

std::vector<std::uint8_t> encodeDxtForTesting(
  const DxtEncoderForTesting encoder,
  const std::vector<std::uint8_t>& rgba,
  const int width,
  const int height,
  const bool cube)
{
  if (
    width < 1 || height < 1
    || rgba.size() != std::size_t(width) * std::size_t(height) * 4)
  {
    return {};
  }

  // BinaryImage.cpp:113-131 (2D: zero-padded to whole blocks) and :706-718 (cube: a level
  // below 4 replicated)
  auto padded = std::vector<std::uint8_t>{};
  const byte* pic = rgba.data();
  auto dxtWidth = width;
  auto dxtHeight = height;
  if (cube && width < 4)
  {
    padded.resize(64);
    padImageTo4x4(rgba.data(), width, height, padded.data());
    pic = padded.data();
    dxtWidth = dxtHeight = 4;
  }
  else if (!cube && ((width & 3) || (height & 3)))
  {
    dxtWidth = (width + 3) & ~3;
    dxtHeight = (height + 3) & ~3;
    padded.assign(std::size_t(dxtWidth) * std::size_t(dxtHeight) * 4, 0);
    for (int i = 0; i < height; i++)
    {
      memcpy(
        padded.data() + std::size_t(i) * std::size_t(dxtWidth) * 4,
        rgba.data() + std::size_t(i) * std::size_t(width) * 4,
        std::size_t(width) * 4);
    }
    pic = padded.data();
  }

  const auto blockBytes = encoder == DxtEncoderForTesting::Dxt1 ? 8 : 16;
  auto out = std::vector<std::uint8_t>(
    std::size_t(dxtWidth / 4) * std::size_t(dxtHeight / 4) * std::size_t(blockBytes));
  // the encoders read whole 16-byte rows with SSE2 loads: an aligned copy of the input
  auto aligned = std::vector<std::uint8_t>(std::size_t(dxtWidth) * dxtHeight * 4 + 16);
  auto* in = aligned.data()
             + ((16 - (reinterpret_cast<std::uintptr_t>(aligned.data()) & 15)) & 15);
  memcpy(in, pic, std::size_t(dxtWidth) * std::size_t(dxtHeight) * 4);

#if defined(USE_INTRINSICS_SSE)
  idDxtEncoder dxt;
  switch (encoder)
  {
  case DxtEncoderForTesting::Dxt1:
    dxt.CompressImageDXT1Fast(in, out.data(), dxtWidth, dxtHeight);
    break;
  case DxtEncoderForTesting::Dxt5:
    dxt.CompressImageDXT5Fast(in, out.data(), dxtWidth, dxtHeight);
    break;
  case DxtEncoderForTesting::YCoCgDxt5:
    dxt.CompressYCoCgDXT5Fast(in, out.data(), dxtWidth, dxtHeight);
    break;
  case DxtEncoderForTesting::NormalMapDxt5:
    dxt.CompressNormalMapDXT5Fast(in, out.data(), dxtWidth, dxtHeight);
    break;
  }
  return out;
#else
  // the copied encoders are the SSE2 ones the engine's x64 build runs
  static_cast<void>(encoder);
  static_cast<void>(in);
  return {};
#endif
}

bool dxtEncoderForTestingAvailable()
{
#if defined(USE_INTRINSICS_SSE)
  return true;
#else
  return false;
#endif
}

} // namespace tb::lit
