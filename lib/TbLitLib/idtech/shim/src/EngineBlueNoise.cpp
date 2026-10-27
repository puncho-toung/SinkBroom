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
// This file includes, unaltered, the RBDOOM-3-BFG data header
// neo/renderer/Image_blueNoiseVC_2.h (copied whole into
// lib/TbLitLib/idtech/engine/renderer) and a range of neo/renderer/Image_intrinsic.cpp
// (R_CreateBlueNoise256Image, copied verbatim into the partial file of that name there),
// code of the Doom 3 BFG Edition GPL Source Code and RBDOOM-3-BFG:
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP3.0 (C60, P3.24a): the facade of lit/EngineImageData.h over the copied blue-noise
// table and its generator. The header defines its table static and const, so this file
// holds the one copy of it (constant-initialised, never written: any thread may read it).
// The generator's own buffer is a function-local static it writes on every call, so the
// generator runs on the parse thread alone (C20), as intrinsicImage does.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineImageData.h"
#include "lit/EngineImageEnums.h"
#include "renderer/Image_blueNoiseVC_2.h"
// clang-format on

#include <cstring>

static_assert(sizeof(blueNoiseTexBytes) == BLUENOISE_TEX_HEIGHT * BLUENOISE_TEX_PITCH);
static_assert(BLUENOISE_TEX_PITCH == BLUENOISE_TEX_WIDTH * 3);
static_assert(tb::lit::engine_image::UsageLookupTableRgba == TD_LOOKUP_TABLE_RGBA);
static_assert(tb::lit::engine_image::FilterNearest == TF_NEAREST);
static_assert(tb::lit::engine_image::RepeatRepeat == TR_REPEAT);

namespace tbLitBlueNoise
{

// The image the copied generator fills: renderer/Image.h's idImage reduced to the one
// call R_CreateBlueNoise256Image makes, GenerateImage( pic, width, height, filter,
// repeat, usage, commandList ) (Image_load.cpp: the pixels go to DeriveOpts and
// Load2DFromMemory, which lit/IntrinsicImages.h's raw path mirrors); it keeps a copy of
// the pixels and the options. Declared in this namespace, so the range's idImage names
// it.
class idImage
{
public:
  explicit idImage(tb::lit::EngineGeneratedImage& out)
    : m_out{out}
  {
  }

  void GenerateImage(
    const byte* pic,
    const int width,
    const int height,
    const textureFilter_t filter,
    const textureRepeat_t repeat,
    const textureUsage_t usage,
    nvrhi::ICommandList* /* commandList */)
  {
    m_out.width = width;
    m_out.height = height;
    m_out.filter = int(filter);
    m_out.repeat = int(repeat);
    m_out.usage = int(usage);
    m_out.rgba.assign(pic, pic + std::size_t(width) * std::size_t(height) * 4);
  }

private:
  tb::lit::EngineGeneratedImage& m_out;
};

#include "renderer/Image_intrinsic.cpp"

} // namespace tbLitBlueNoise

namespace tb::lit
{

EngineGeneratedImage engineBlueNoise256Image()
{
  auto out = EngineGeneratedImage{};
  auto image = tbLitBlueNoise::idImage{out};
  tbLitBlueNoise::R_CreateBlueNoise256Image(&image, nullptr);
  return out;
}

const std::uint8_t* engineBlueNoiseTable()
{
  return blueNoiseTexBytes;
}

std::size_t engineBlueNoiseTableSize()
{
  return sizeof(blueNoiseTexBytes);
}

int engineBlueNoiseTableWidth()
{
  return BLUENOISE_TEX_WIDTH;
}

int engineBlueNoiseTableHeight()
{
  return BLUENOISE_TEX_HEIGHT;
}

int engineBlueNoiseTablePitch()
{
  return BLUENOISE_TEX_PITCH;
}

} // namespace tb::lit
