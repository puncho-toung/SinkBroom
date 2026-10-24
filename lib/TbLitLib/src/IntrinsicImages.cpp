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
// renderer/Image_load.cpp, renderer/BinaryImage.cpp, renderer/DXT/DXTCodec.h and
// renderer/DXT/DXTEncoder_SSE2.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/IntrinsicImages.h"

#include "lit/EngineImageData.h"
#include "lit/EngineLight.h"
#include "lit/ImageCache.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// Image_intrinsic.cpp:46 DEFAULT_SIZE; RenderCommon.h:49 FALLOFF_TEXTURE_SIZE; :628-629
constexpr int DefaultSize = 16;
constexpr int FalloffTextureSize = 64;
constexpr int QuadraticWidth = 32;
constexpr int QuadraticHeight = 4;

IntrinsicRecipe solid(
  const std::uint8_t value, const int usage, const int filter = FilterDefault)
{
  // R_WhiteImage, R_BlackImage, R_BlackDiffuseImage (:125-153): memset( data, value )
  auto recipe = IntrinsicRecipe{};
  recipe.width = DefaultSize;
  recipe.height = DefaultSize;
  recipe.filter = filter;
  recipe.repeat = RepeatRepeat;
  recipe.usage = usage;
  recipe.rgba.assign(std::size_t(DefaultSize) * DefaultSize * 4, value);
  return recipe;
}

// R_FlatNormalImage (:389-406): (128, 128, 255, 255), TD_BUMP
IntrinsicRecipe flatNormal()
{
  auto recipe = solid(0, UsageBump);
  for (std::size_t i = 0; i < recipe.rgba.size(); i += 4)
  {
    recipe.rgba[i + 0] = 128;
    recipe.rgba[i + 1] = 128;
    recipe.rgba[i + 2] = 255;
    recipe.rgba[i + 3] = 255;
  }
  return recipe;
}

// R_CreateNoFalloffImage (:415-432): white with a zero border, TR_CLAMP_TO_ZERO,
// TD_LOOKUP_TABLE_MONO
IntrinsicRecipe noFalloff()
{
  auto recipe = IntrinsicRecipe{};
  recipe.width = FalloffTextureSize;
  recipe.height = 16;
  recipe.filter = FilterDefault;
  recipe.repeat = RepeatClampToZero;
  recipe.usage = UsageLookupTableMono;
  recipe.rgba.assign(std::size_t(FalloffTextureSize) * 16 * 4, 0);
  for (int x = 1; x < FalloffTextureSize - 1; x++)
  {
    for (int y = 1; y < 15; y++)
    {
      auto* texel =
        &recipe.rgba[(std::size_t(y) * FalloffTextureSize + std::size_t(x)) * 4];
      texel[0] = 255;
      texel[1] = 255;
      texel[2] = 255;
      texel[3] = 255;
    }
  }
  return recipe;
}

// R_QuadraticImage (:631-669): a quadratic falloff across x, TR_CLAMP,
// TD_LOOKUP_TABLE_RGB1; the engine's float and double arithmetic kept as it is
IntrinsicRecipe quadratic()
{
  auto recipe = IntrinsicRecipe{};
  recipe.width = QuadraticWidth;
  recipe.height = QuadraticHeight;
  recipe.filter = FilterDefault;
  recipe.repeat = RepeatClamp;
  recipe.usage = UsageLookupTableRgb1;
  recipe.rgba.assign(std::size_t(QuadraticWidth) * QuadraticHeight * 4, 0);
  for (int x = 0; x < QuadraticWidth; x++)
  {
    for (int y = 0; y < QuadraticHeight; y++)
    {
      float d;

      d = float(x - (QuadraticWidth / 2 - 0.5));
      d = std::fabs(d);
      d = float(d - 0.5);
      d /= QuadraticWidth / 2;

      d = float(1.0 - d);
      d = d * d;

      int b = std::uint8_t(d * 255);
      if (b <= 0)
      {
        b = 0;
      }
      else if (b > 255)
      {
        b = 255;
      }
      auto* texel = &recipe.rgba[(std::size_t(y) * QuadraticWidth + std::size_t(x)) * 4];
      texel[0] = texel[1] = texel[2] = std::uint8_t(b);
      texel[3] = 255;
    }
  }
  return recipe;
}

// Image_intrinsic.cpp:442 FOG_SIZE; RenderCommon.h:54 FOG_ENTER_SIZE
constexpr int FogSize = 128;
constexpr int FogEnterSize = 64;

/**
 * LP5.0 (C49, P6.20): the engine's ( byte )( f ) of a float or double the C++ standard
 * leaves undefined above 255 (and below 0), as MSVC compiles it for x64, the engine's
 * build: cvttss2si (cvttsd2si for a double) truncates to a 32-bit int and the low byte of
 * the int is kept, so 366.4 becomes 110 (366 - 256), not 255. Only R_FogImage's texels
 * toward the corners, whose distance d reaches 256 / 255 of the radius (3,803 of them),
 * take that path; the fog shader samples _fog at T = 0.5 (between rows 63 and 64, the
 * planes p1 and p3 of C49), where d stays below it, so those texels are never sampled.
 * FogFraction stays in 0..1, so no texel of _fogEnter takes it.
 */
template <typename F>
std::uint8_t msvcByte(const F value)
{
  return std::uint8_t(std::uint32_t(std::int32_t(value)) & 0xffu);
}

// one texel (255, 255, 255, alpha) of a lookup table at x, y
void setAlphaTexel(
  IntrinsicRecipe& recipe, const int x, const int y, const std::uint8_t alpha)
{
  auto* texel =
    &recipe.rgba[(std::size_t(y) * std::size_t(recipe.width) + std::size_t(x)) * 4];
  texel[0] = texel[1] = texel[2] = 255;
  texel[3] = alpha;
}

/**
 * LP5.0 (C49): R_FogImage (Image_intrinsic.cpp:442-491): 128 × 128, the alpha a step
 * table of 0.982 per step over the distance from the centre in radii of 63, the border
 * 255; TF_LINEAR, TR_CLAMP, TD_LOOKUP_TABLE_ALPHA. The distance is idMath::Sqrt of the
 * int sum (x · InvSqrt( x ), the copy's), each type as the engine's (float steps, a
 * double 1.0 - step).
 */
IntrinsicRecipe fog()
{
  auto recipe = IntrinsicRecipe{};
  recipe.width = FogSize;
  recipe.height = FogSize;
  recipe.filter = FilterLinear;
  recipe.repeat = RepeatClamp;
  recipe.usage = UsageLookupTableAlpha;
  recipe.rgba.assign(std::size_t(FogSize) * FogSize * 4, 0);

  auto step = std::array<float, 256>{};
  float remaining = 1.0;
  for (std::size_t i = 0; i < step.size(); i++)
  {
    step[i] = remaining;
    remaining *= 0.982f;
  }

  for (int x = 0; x < FogSize; x++)
  {
    for (int y = 0; y < FogSize; y++)
    {
      float d;

      d = engineMathSqrt(float(
        (x - FogSize / 2) * (x - FogSize / 2) + (y - FogSize / 2) * (y - FogSize / 2)));
      d /= FogSize / 2 - 1;

      int b = msvcByte(d * 255);
      if (b <= 0)
      {
        b = 0;
      }
      else if (b > 255)
      {
        b = 255;
      }
      b = msvcByte(255 * (1.0 - step[std::size_t(b)]));
      if (x == 0 || x == FogSize - 1 || y == 0 || y == FogSize - 1)
      {
        b = 255; // avoid clamping issues
      }
      setAlphaTexel(recipe, x, y, std::uint8_t(b));
    }
  }
  return recipe;
}

// FogFraction (Image_intrinsic.cpp:501-577): RAMP_RANGE 8, DEEP_RANGE -30, the float and
// double arithmetic kept as it is
constexpr float RampRange = 8;
constexpr float DeepRange = -30;

float fogFraction(const float viewHeight, const float targetHeight)
{
  float total = std::fabs(targetHeight - viewHeight);

  // only ranges that cross the ramp range are special
  if (targetHeight > 0 && viewHeight > 0)
  {
    return 0.0;
  }
  if (targetHeight < -RampRange && viewHeight < -RampRange)
  {
    return 1.0;
  }

  float above;
  if (targetHeight > 0)
  {
    above = targetHeight;
  }
  else if (viewHeight > 0)
  {
    above = viewHeight;
  }
  else
  {
    above = 0;
  }

  float rampTop, rampBottom;

  if (viewHeight > targetHeight)
  {
    rampTop = viewHeight;
    rampBottom = targetHeight;
  }
  else
  {
    rampTop = targetHeight;
    rampBottom = viewHeight;
  }
  if (rampTop > 0)
  {
    rampTop = 0;
  }
  if (rampBottom < -RampRange)
  {
    rampBottom = -RampRange;
  }

  float rampSlope = float(1.0 / RampRange);

  if (!total)
  {
    return -viewHeight * rampSlope;
  }

  float ramp = float(
    (1.0 - (rampTop * rampSlope + rampBottom * rampSlope) * -0.5)
    * (rampTop - rampBottom));

  float frac = (total - above - ramp) / total;

  // after it gets moderately deep, always use full value
  float deepest = viewHeight < targetHeight ? viewHeight : targetHeight;

  float deepFrac = deepest / DeepRange;
  if (deepFrac >= 1.0)
  {
    return 1.0;
  }

  frac = float(frac * (1.0 - deepFrac) + deepFrac);

  return frac;
}

/**
 * LP5.0 (C49): R_FogEnterImage (Image_intrinsic.cpp:587-619): 64 × 64, the alpha
 * FogFraction( x - 32, y - 32 ) · 255 clamped to 0..255 (through msvcByte as the engine's
 * cast); TF_LINEAR, TR_CLAMP, TD_LOOKUP_TABLE_ALPHA.
 */
IntrinsicRecipe fogEnter()
{
  auto recipe = IntrinsicRecipe{};
  recipe.width = FogEnterSize;
  recipe.height = FogEnterSize;
  recipe.filter = FilterLinear;
  recipe.repeat = RepeatClamp;
  recipe.usage = UsageLookupTableAlpha;
  recipe.rgba.assign(std::size_t(FogEnterSize) * FogEnterSize * 4, 0);
  for (int x = 0; x < FogEnterSize; x++)
  {
    for (int y = 0; y < FogEnterSize; y++)
    {
      float d;

      d = fogFraction(float(x - (FogEnterSize / 2)), float(y - (FogEnterSize / 2)));

      int b = msvcByte(d * 255);
      if (b <= 0)
      {
        b = 0;
      }
      else if (b > 255)
      {
        b = 255;
      }
      setAlphaTexel(recipe, x, y, std::uint8_t(b));
    }
  }
  return recipe;
}

// DXTCodec.h:536-539 idDxtEncoder::ColorTo565
std::uint16_t colorTo565(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
{
  return std::uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void emitUShort(std::vector<std::uint8_t>& out, const std::uint16_t value)
{
  // EmitUShort writes the host's little-endian bytes
  out.push_back(std::uint8_t(value & 0xff));
  out.push_back(std::uint8_t(value >> 8));
}

/*
 * The fast encoders' arithmetic (DXTEncoder_SSE2.cpp at 285ea4d1,
 * image_highQualityCompression 0) for a block whose 16 texels are one colour t, lane by
 * lane: _mm_mulhi_epi16 is (a * b) >> 16 of 16-bit lanes, _mm_avg_epu8 (a + b + 1) >> 1.
 * Every texel of such a block gets the same index, so an index is computed once and
 * written 16 times.
 */
int mulhi(const int a, const int b)
{
  return (a * b) >> 16;
}

// SIMD_SSE2_word_div_by_3, _6 and _14
constexpr int DivBy3 = (1 << 16) / 3 + 1;
constexpr int DivBy6 = (1 << 16) / 6 + 1;
constexpr int DivBy14 = (1 << 16) / 14 + 1;

/**
 * EmitAlphaIndices_SSE2 (:663-795, :797-940) for one alpha: the thresholds ab1 to ab7
 * between the endpoints, the count of those the alpha reaches, the count turned into a
 * DXT5 alpha index (0 and 1 swapped).
 */
int alphaIndex(const int alpha, const int minAlpha, const int maxAlpha)
{
  static constexpr int MaxScale[7] = {13, 11, 9, 7, 5, 3, 1};
  auto reached = 0;
  for (const auto scale : MaxScale)
  {
    const auto threshold = mulhi(scale * maxAlpha + (14 - scale) * minAlpha + 7, DivBy14);
    reached += alpha >= threshold ? 1 : 0;
  }
  auto index = (8 - reached) & 7;
  return index < 2 ? index ^ 1 : index;
}

/**
 * EmitGreenIndices_SSE2 (:1424-1527) for one green: the thresholds gb1 to gb3 between
 * the endpoints, the count of those the green reaches, as a DXT colour index (0 and 1
 * swapped).
 */
int greenIndex(const int green, const int minGreen, const int maxGreen)
{
  const int thresholds[3] = {
    mulhi(5 * maxGreen + minGreen + 3, DivBy6),
    mulhi(3 * maxGreen + 3 * minGreen + 3, DivBy6),
    mulhi(maxGreen + 5 * minGreen + 3, DivBy6),
  };
  auto reached = 0;
  for (const auto threshold : thresholds)
  {
    reached += green >= threshold ? 1 : 0;
  }
  const auto index = (4 - reached) & 3;
  return index < 2 ? index ^ 1 : index;
}

// the 6 bytes of 16 three-bit alpha indices, all index
void emitAlphaIndices(std::vector<std::uint8_t>& out, const int index)
{
  auto bits = std::uint64_t{0};
  for (auto texel = 0; texel < 16; ++texel)
  {
    bits |= std::uint64_t(index) << (3 * texel);
  }
  for (auto i = 0; i < 6; ++i)
  {
    out.push_back(std::uint8_t(bits >> (8 * i)));
  }
}

// the 4 bytes of 16 two-bit colour indices, all index
void emitColorIndices(std::vector<std::uint8_t>& out, const int index)
{
  auto bits = std::uint32_t{0};
  for (auto texel = 0; texel < 16; ++texel)
  {
    bits |= std::uint32_t(index) << (2 * texel);
  }
  for (auto i = 0; i < 4; ++i)
  {
    out.push_back(std::uint8_t(bits >> (8 * i)));
  }
}

/**
 * CompressImageDXT5Fast_SSE2 (:1078-1137): the bounding box of one colour is that colour
 * and InsetColorsBBox moves it by nothing, so both endpoints are ColorTo565 of it, the
 * four palette colours are equal and EmitColorIndices gives index 0; the alpha endpoints
 * are the alpha and EmitAlphaIndices gives index 0.
 */
std::vector<std::uint8_t> constantPlainBlock(const std::array<std::uint8_t, 4>& t)
{
  auto block = std::vector<std::uint8_t>{};
  block.push_back(t[3]);
  block.push_back(t[3]);
  emitAlphaIndices(block, alphaIndex(t[3], t[3], t[3]));
  const auto color = colorTo565(t[0], t[1], t[2]);
  emitUShort(block, color);
  emitUShort(block, color);
  emitColorIndices(block, 0);
  return block;
}

/**
 * CompressYCoCgDXT5Fast_SSE2 (:1353-1422) for one (Co, Cg, 0, Y) texel: ScaleYCoCg
 * (:1139-1233) picks the scale from the CoCg range (1, 2 or 4 as the range passes 63 or
 * 31: 4 for a range of 0) and multiplies the CoCg word about 128 by it, writing the scale
 * into the endpoints' blue; InsetYCoCgBBox (:1235-1277) moves the two endpoints apart by
 * the rounding and quantises them to 5:6:5 with their bits replicated;
 * SelectYCoCgDiagonal (:1279-1351) swaps their Cg when the texels lie on the other
 * diagonal; EmitCoCgIndices (:530-661) picks the palette colour nearest the texel
 * (SAD over its bytes).
 */
std::vector<std::uint8_t> constantYCoCgBlock(const std::array<std::uint8_t, 4>& t)
{
  // ScaleYCoCg: the range, and from it the scale index s (0, 1 or 3) and the multiplier
  const auto range = std::max(std::abs(int(t[0]) - 128), std::abs(int(t[1]) - 128));
  const auto s = range > 63 ? 0 : range > 31 ? 1 : 3;
  const auto scaled = [&](std::array<int, 4> c, const bool endpoint) {
    if (endpoint)
    {
      // scale_mask3 drops blue; the scale's bits go there (scale_mask4)
      c[2] = 8 * s;
    }
    // the CoCg word minus 128 per byte, times s + 1, its bytes masked with ~s, plus 128
    const auto word = ((c[0] + 128) & 0xff) | (((c[1] + 128) & 0xff) << 8);
    const auto product = (word * (s + 1)) & 0xffff;
    c[0] = (((product & 0xff) & ~s) + 128) & 0xff;
    c[1] = ((((product >> 8) & 0xff) & ~s) + 128) & 0xff;
    return c;
  };
  const auto texel = scaled({t[0], t[1], t[2], t[3]}, false);
  const auto box = scaled({t[0], t[1], t[2], t[3]}, true);

  // InsetYCoCgBBox for min == max == box
  static constexpr int QuantMask[4] = {0xf8, 0xfc, 0xf8, 0xff};
  static constexpr int RepShift[4] = {5, 6, 5, -1};
  auto minColor = std::array<int, 4>{};
  auto maxColor = std::array<int, 4>{};
  for (auto lane = std::size_t{0}; lane < 4; ++lane)
  {
    const auto alpha = lane == 3;
    const auto shift = alpha ? 5 : 4;
    const auto round = lane == 2 ? 0 : alpha ? 15 : 7;
    auto low = std::max(0, ((box[lane] << shift) - round) >> shift);
    auto high = std::max(0, ((box[lane] << shift) + round) >> shift);
    low &= QuantMask[lane];
    high &= QuantMask[lane];
    if (RepShift[lane] > 0)
    {
      low |= low >> RepShift[lane];
      high |= high >> RepShift[lane];
    }
    minColor[lane] = low;
    maxColor[lane] = high;
  }

  // SelectYCoCgDiagonal (NVIDIA_7X_HARDWARE_BUG_FIX): all 16 texels on the other
  // diagonal swap the endpoints' Cg, unless their Co are equal
  const auto midCo = (minColor[0] + maxColor[0] + 1) >> 1;
  const auto midCg = (minColor[1] + maxColor[1] + 1) >> 1;
  if ((texel[0] >= midCo) != (texel[1] >= midCg) && minColor[0] != maxColor[0])
  {
    std::swap(minColor[1], maxColor[1]);
  }

  auto block = std::vector<std::uint8_t>{};
  block.push_back(std::uint8_t(maxColor[3]));
  block.push_back(std::uint8_t(minColor[3]));
  emitAlphaIndices(block, alphaIndex(texel[3], minColor[3], maxColor[3]));
  emitUShort(
    block,
    colorTo565(
      std::uint8_t(maxColor[0]), std::uint8_t(maxColor[1]), std::uint8_t(maxColor[2])));
  emitUShort(
    block,
    colorTo565(
      std::uint8_t(minColor[0]), std::uint8_t(minColor[1]), std::uint8_t(minColor[2])));

  // EmitCoCgIndices: the palette (colorMask2 keeps three bytes), the SAD of the texel's
  // four bytes to each colour, and the encoder's comparisons
  auto palette = std::array<std::array<int, 4>, 4>{};
  for (auto lane = std::size_t{0}; lane < 3; ++lane)
  {
    palette[0][lane] = maxColor[lane];
    palette[1][lane] = minColor[lane];
    palette[2][lane] = mulhi(2 * maxColor[lane] + minColor[lane], DivBy3);
    palette[3][lane] = mulhi(maxColor[lane] + 2 * minColor[lane], DivBy3);
  }
  auto d = std::array<int, 4>{};
  for (auto k = std::size_t{0}; k < 4; ++k)
  {
    for (auto lane = std::size_t{0}; lane < 4; ++lane)
    {
      d[k] += std::abs(texel[lane] - palette[k][lane]);
    }
  }
  const auto b0 = d[0] > d[3];
  const auto b1 = d[1] > d[2];
  const auto b2 = d[0] > d[2];
  const auto b3 = d[1] > d[3];
  const auto b4 = d[2] > d[3];
  const auto index = ((b4 && b0) ? 1 : 0) | (((b2 && b1) || (b3 && b0)) ? 2 : 0);
  emitColorIndices(block, index);
  return block;
}

/**
 * CompressNormalMapDXT5Fast_SSE2 (:1579-1635) for one (_, y, _, x) texel:
 * InsetNormalsBBoxDXT5 (:1529-1577) moves the green and alpha endpoints apart by the
 * rounding, clamps them to 0..255 and quantises the green to 6 bits with its bits
 * replicated; the alpha block carries x, the colour block y with the texel's red and blue
 * (EmitGreenIndices, EmitAlphaIndices on channel 3).
 */
std::vector<std::uint8_t> constantNormalBlock(const std::array<std::uint8_t, 4>& t)
{
  const auto clamp = [](const int v) { return std::clamp(v, 0, 255); };
  auto minGreen = clamp(((int(t[1]) << 4) - 7) >> 4) & 0xfc;
  auto maxGreen = clamp(((int(t[1]) << 4) + 7) >> 4) & 0xfc;
  minGreen |= minGreen >> 6;
  maxGreen |= maxGreen >> 6;
  const auto minAlpha = clamp(((int(t[3]) << 5) - 15) >> 5);
  const auto maxAlpha = clamp(((int(t[3]) << 5) + 15) >> 5);

  auto block = std::vector<std::uint8_t>{};
  block.push_back(std::uint8_t(maxAlpha));
  block.push_back(std::uint8_t(minAlpha));
  emitAlphaIndices(block, alphaIndex(t[3], minAlpha, maxAlpha));
  emitUShort(block, colorTo565(t[0], std::uint8_t(maxGreen), t[2]));
  emitUShort(block, colorTo565(t[0], std::uint8_t(minGreen), t[2]));
  emitColorIndices(block, greenIndex(t[1], minGreen, maxGreen));
  return block;
}

} // namespace

std::optional<IntrinsicRecipe> intrinsicRecipe(const std::string_view name)
{
  if (name == "_default")
  {
    // R_DefaultImage, MakeDefault (:56-118) with com_developer 0: zeros, TD_DEFAULT
    // (developer 0 pinned, mirror.json M-INTR-01)
    return solid(0, UsageDefault);
  }
  if (name == "_white")
  {
    return solid(255, UsageDefault);
  }
  if (name == "_black")
  {
    return solid(0, UsageDefault);
  }
  if (name == "_blackDiffuse")
  {
    return solid(0, UsageDiffuse);
  }
  if (name == "_flat")
  {
    return flatNormal();
  }
  if (name == "_noFalloff")
  {
    return noFalloff();
  }
  if (name == "_quadratic")
  {
    return quadratic();
  }
  // LP5.0 (C49, D69's deferral): the fog tables (Image_intrinsic.cpp:1071-1072 key them)
  if (name == "_fog")
  {
    return fog();
  }
  if (name == "_fogEnter")
  {
    return fogEnter();
  }
  return std::nullopt;
}

std::optional<std::shared_ptr<const CachedImage>> rawIntrinsicImage(
  const std::string_view name)
{
  if (name == "_brdfLut")
  {
    // R_CreateBrdfLutImage: GenerateImage( brfLutTexBytes, BRDFLUT_TEX_WIDTH,
    // BRDFLUT_TEX_HEIGHT, TF_LINEAR, TR_CLAMP, TD_RG16F ): one level of the array as it
    // is
    auto image = CachedImage{};
    image.file = std::string{name};
    image.textureType = Type2D;
    image.storedFormat = FormatRg16f;
    image.format = FormatRg16f;
    image.colorFormat = ColorDefault;
    image.width = engineBrdfLutWidth();
    image.height = engineBrdfLutHeight();
    image.numLevels = 1;
    const auto* bytes = engineBrdfLutData();
    image.data.assign(bytes, bytes + engineBrdfLutSize());
    image.levels.push_back(
      CachedImageLevel{0, 0, image.width, image.height, 0, image.data.size()});
    return std::make_shared<const CachedImage>(std::move(image));
  }
  if (name == "_defaultUACRadiance")
  {
    // R_CreateDefaultRadianceCube: GenerateCubeImage( six faces of zeros, 8, TF_LINEAR,
    // TD_HDRI ): one level, BC6H, each 8 × 8 face four zero blocks
    constexpr auto size = 8;
    constexpr auto faceBytes = std::size_t{(size / 4) * (size / 4) * 16};
    auto image = CachedImage{};
    image.file = std::string{name};
    image.textureType = TypeCubic;
    image.storedFormat = FormatBc6h;
    image.format = FormatBc6h;
    image.colorFormat = ColorDefault;
    image.width = size;
    image.height = size;
    image.numLevels = 1;
    image.data.assign(faceBytes * 6, 0);
    for (auto side = 0; side < 6; ++side)
    {
      image.levels.push_back(
        CachedImageLevel{0, side, size, size, std::size_t(side) * faceBytes, faceBytes});
    }
    return std::make_shared<const CachedImage>(std::move(image));
  }
  return std::nullopt;
}

std::vector<std::uint8_t> constantDxt5Blocks(
  const std::array<std::uint8_t, 4>& rgba,
  const int colorFormat,
  const int blocksWide,
  const int blocksHigh)
{
  const auto block = colorFormat == ColorYCoCgDxt5    ? constantYCoCgBlock(rgba)
                     : colorFormat == ColorNormalDxt5 ? constantNormalBlock(rgba)
                                                      : constantPlainBlock(rgba);
  auto out = std::vector<std::uint8_t>{};
  out.reserve(block.size() * std::size_t(blocksWide) * std::size_t(blocksHigh));
  for (int i = 0; i < blocksWide * blocksHigh; ++i)
  {
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

Result<std::shared_ptr<const CachedImage>> intrinsicImage(
  MaterialDecls& decls, const std::string_view name)
{
  // LP4.2a (D85): the raw path's images are ready as they are
  if (auto raw = rawIntrinsicImage(name))
  {
    return std::move(*raw);
  }
  auto recipe = intrinsicRecipe(name);
  if (!recipe)
  {
    return Error{
      fmt::format("the preview does not make the engine's intrinsic image {} yet", name)};
  }

  // Image_load.cpp:340-419 GenerateImage: DeriveOpts for the usage with numLevels 0, then
  // Load2DFromMemory
  const auto opts = engineImageOpts(recipe->usage);
  const auto numLevels =
    engineMipLevels(opts.format, recipe->filter, recipe->width, recipe->height);
  auto pixels = SourcePixels{};
  pixels.width = recipe->width;
  pixels.height = recipe->height;
  pixels.faces = 1;
  pixels.usage = recipe->usage;
  pixels.pixels = std::move(recipe->rgba);
  auto image = decls.sourceImageLevels(
    std::move(pixels), opts.format, opts.colorFormat, numLevels, opts.gammaMips);
  if (image.levels.empty())
  {
    return Error{fmt::format("the intrinsic image {} has no levels", name)};
  }

  if (opts.format == FormatDxt5)
  {
    // the bytes the encoder would write: each level is one colour, so its blocks are
    // constant (D24: endpoint arithmetic, no encoder)
    auto data = std::vector<std::uint8_t>{};
    for (auto& level : image.levels)
    {
      const auto* texels = image.data.data() + level.offset;
      const auto colour =
        std::array<std::uint8_t, 4>{texels[0], texels[1], texels[2], texels[3]};
      for (std::size_t i = 0; i < level.size; i += 4)
      {
        if (std::memcmp(texels + i, colour.data(), 4) != 0)
        {
          return Error{fmt::format(
            "level {} of the intrinsic image {} is not of one colour",
            level.level,
            name)};
        }
      }
      // Load2DFromMemory pads a level smaller than a block to one block
      const auto blocks = constantDxt5Blocks(
        colour,
        opts.colorFormat,
        std::max(1, (level.width + 3) / 4),
        std::max(1, (level.height + 3) / 4));
      level.offset = data.size();
      level.size = blocks.size();
      data.insert(data.end(), blocks.begin(), blocks.end());
    }
    image.data = std::move(data);
    image.format = FormatDxt5;
  }
  else if (
    opts.format != FormatInt8 && opts.format != FormatRgba8 && opts.format != FormatAlpha)
  {
    return Error{fmt::format(
      "the intrinsic image {} has the format {}, which the preview does not make",
      name,
      opts.format)};
  }

  // a generated image: no file, no time
  image.fromSource = false;
  image.file = std::string{name};
  image.textureType = Type2D;
  return std::make_shared<const CachedImage>(std::move(image));
}

} // namespace tb::lit
