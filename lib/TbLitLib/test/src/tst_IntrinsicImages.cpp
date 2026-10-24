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

// LP2.1 step 6 (D69, SC271, K23): the intrinsic images LP2 draws with, as the engine's
// cache would hold them, against the engine's own fast DXT encoders (the test-only copy,
// TbLitIdDxtTestLib): each level's constant blocks byte for byte against the encoder's
// blocks of the same level. LP5.0 (C49) adds the fog tables against a transcription.

#include "LitTestUtils.h"
#include "lit/DxtEncoderForTesting.h"
#include "lit/EditorImageSize.h"
#include "lit/EngineGeometry.h"
#include "lit/EngineImageData.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/IntrinsicImages.h"
#include "lit/MaterialDecls.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;

std::string hex(const std::vector<std::uint8_t>& bytes)
{
  auto text = std::string{};
  for (const auto byte : bytes)
  {
    text += fmt::format("{:02x}", byte);
  }
  return text;
}

DxtEncoderForTesting encoderOf(const int colorFormat)
{
  return colorFormat == ColorNormalDxt5  ? DxtEncoderForTesting::NormalMapDxt5
         : colorFormat == ColorYCoCgDxt5 ? DxtEncoderForTesting::YCoCgDxt5
                                         : DxtEncoderForTesting::Dxt5;
}

// BC1's 5:6:5 endpoint widened as a decoder does
std::array<int, 3> decode565(const std::uint16_t c)
{
  const auto r = (c >> 11) & 31;
  const auto g = (c >> 5) & 63;
  const auto b = c & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

} // namespace

TEST_CASE("IntrinsicImages (D69)")
{
  auto source = MemoryDeclSource{{}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  SECTION("LP2's intrinsics: the engine's sizes, mips and encoder bytes")
  {
    const auto encoders = dxtEncoderForTestingAvailable();
    if (!encoders)
    {
      WARN(
        "the engine's fast DXT encoders are its SSE2 ones, which this build has not: the "
        "blocks are not compared");
    }
    auto lines = std::vector<std::string>{};
    for (const auto* name :
         {"_default",
          "_white",
          "_black",
          "_blackDiffuse",
          "_flat",
          "_noFalloff",
          "_quadratic"})
    {
      CAPTURE(name);
      const auto recipe = intrinsicRecipe(name);
      REQUIRE(recipe);
      const auto made = intrinsicImage(decls, name);
      REQUIRE(made.is_success());
      const auto& image = *made.value();
      const auto opts = engineImageOpts(recipe->usage);

      // the size of EditorImageSize's table, the mips of DeriveOpts
      CHECK(intrinsicImageSize(name) == std::pair{image.width, image.height});
      CHECK(image.width == recipe->width);
      CHECK(
        image.numLevels
        == engineMipLevels(opts.format, recipe->filter, recipe->width, recipe->height));
      CHECK(int(image.levels.size()) == image.numLevels);
      CHECK(image.storedFormat == opts.format);
      CHECK(image.colorFormat == opts.colorFormat);
      CHECK(image.textureType == Type2D);
      CHECK_FALSE(image.fromSource);

      // the RGBA8 levels the engine's encoder is handed: the same layout conversion
      auto pixels = SourcePixels{};
      pixels.width = recipe->width;
      pixels.height = recipe->height;
      pixels.usage = recipe->usage;
      pixels.pixels = recipe->rgba;
      const auto layouts = decls.sourceImageLevels(
        std::move(pixels),
        opts.format,
        opts.colorFormat,
        image.numLevels,
        opts.gammaMips);
      REQUIRE(layouts.levels.size() == image.levels.size());

      auto exact = 0;
      for (std::size_t i = 0; i < image.levels.size(); ++i)
      {
        CAPTURE(i);
        const auto& level = image.levels[i];
        const auto ours = std::vector<std::uint8_t>(
          image.data.begin() + std::ptrdiff_t(level.offset),
          image.data.begin() + std::ptrdiff_t(level.offset + level.size));
        const auto& layout = layouts.levels[i];
        const auto rgba = std::vector<std::uint8_t>(
          layouts.data.begin() + std::ptrdiff_t(layout.offset),
          layouts.data.begin() + std::ptrdiff_t(layout.offset + layout.size));
        if (opts.format != FormatDxt5)
        {
          // INT8 and RGBA8: the stored bytes
          CHECK(ours == rgba);
          exact += ours == rgba ? 1 : 0;
          continue;
        }
        CHECK(image.format == FormatDxt5);
        if (!encoders)
        {
          continue;
        }
        const auto engine = encodeDxtForTesting(
          encoderOf(opts.colorFormat), rgba, layout.width, layout.height, false);
        INFO("ours " << hex(ours) << " engine " << hex(engine));
        CHECK(ours == engine);
        exact += ours == engine ? 1 : 0;
      }
      lines.push_back(fmt::format(
        "  {}: {} x {}, {} levels, format {}, colour {}: {} of {} levels byte-equal",
        name,
        image.width,
        image.height,
        image.numLevels,
        image.storedFormat,
        image.colorFormat,
        exact,
        image.levels.size()));
    }
    std::cout << "Intrinsic images against the engine's encoder:\n";
    for (const auto& line : lines)
    {
      std::cout << line << "\n";
    }
  }

  SECTION("the decoded values of the first block (K23)")
  {
    // a DXT5 block's colour endpoints (bytes 8-11), its first texel's colour index (bits
    // 0-1 of byte 12) and the BC1 palette colour it decodes to, in thirds: c0, c1,
    // (2 c0 + c1) / 3, (c0 + 2 c1) / 3
    struct Decoded
    {
      std::array<int, 3> c0;
      std::array<int, 3> c1;
      int index = 0;
      std::array<double, 3> texel;
    };
    const auto decode = [](const CachedImage& image) {
      const auto* block = image.data.data();
      auto decoded = Decoded{};
      decoded.c0 = decode565(std::uint16_t(block[8] | (block[9] << 8)));
      decoded.c1 = decode565(std::uint16_t(block[10] | (block[11] << 8)));
      decoded.index = block[12] & 3;
      for (std::size_t i = 0; i < 3; ++i)
      {
        const auto a = double(decoded.c0[i]);
        const auto b = double(decoded.c1[i]);
        decoded.texel[i] = decoded.index == 0   ? a
                           : decoded.index == 1 ? b
                           : decoded.index == 2 ? (2 * a + b) / 3
                                                : (a + 2 * b) / 3;
      }
      return decoded;
    };

    // P2.4 named the endpoints: Co/Cg 128 become 132/130, the green 128 becomes 130; the
    // encoder insets the box of one colour, so the texel's index is 2, two thirds of the
    // way from (132, 130) to (123, 125): Co 129, Cg 128 1/3
    const auto blackDiffuse =
      decode(*(intrinsicImage(decls, "_blackDiffuse") | kdl::value()));
    CHECK(blackDiffuse.c0 == std::array<int, 3>{132, 130, 24});
    CHECK(blackDiffuse.c1 == std::array<int, 3>{123, 125, 24});
    CHECK(blackDiffuse.index == 2);
    CHECK(blackDiffuse.texel[0] == 129.0);
    CHECK(std::abs(blackDiffuse.texel[1] - 385.0 / 3.0) < 1e-9);

    // _flat: the green endpoints 130 and 125, index 2: 128 1/3; x in alpha: 128
    const auto flatImage = intrinsicImage(decls, "_flat") | kdl::value();
    const auto flat = decode(*flatImage);
    CHECK(flat.c0[1] == 130);
    CHECK(flat.c1[1] == 125);
    CHECK(flat.index == 2);
    CHECK(std::abs(flat.texel[1] - 385.0 / 3.0) < 1e-9);
    CHECK(flatImage->data[0] == 128);
    CHECK(flatImage->data[1] == 127);
    CHECK((flatImage->data[2] & 7) == 0);

    // _white: one endpoint, index 0
    const auto white = decode(*(intrinsicImage(decls, "_white") | kdl::value()));
    CHECK(white.c0 == std::array<int, 3>{255, 255, 255});
    CHECK(white.index == 0);
    std::cout << fmt::format(
      "decoded: _blackDiffuse Co/Cg {:.3f}/{:.3f} (endpoints {}/{} and {}/{}), _flat "
      "green "
      "{:.3f}\n",
      blackDiffuse.texel[0],
      blackDiffuse.texel[1],
      blackDiffuse.c0[0],
      blackDiffuse.c0[1],
      blackDiffuse.c1[0],
      blackDiffuse.c1[1],
      flat.texel[1]);
  }

  SECTION("LP5.0 (C49): _fog and _fogEnter, FMT_ALPHA, against a transcription")
  {
    // An independent transcription of R_FogImage, FogFraction and R_FogEnterImage
    // (Image_intrinsic.cpp:442-619 at 285ea4d1), written here apart from the product's:
    // idMath::Sqrt as x · sqrtf( 1 / x ) (Math.h:527-552), and the engine's ( byte )
    // casts as MSVC's x64 code makes them: truncate to an int, keep its low byte.
    const auto castByte = [](const double value) {
      const auto truncated = static_cast<long long>(std::trunc(value));
      return int(((truncated % 256) + 256) % 256);
    };
    const auto idSqrt = [](const float x) {
      return x >= 0.0f ? x * (x > 1.1754944e-38f ? sqrtf(1.0f / x) : 1e30f) : 0.0f;
    };
    auto steps = std::array<float, 256>{};
    {
      float r = 1.0f;
      for (auto& s : steps)
      {
        s = r;
        r *= 0.982f;
      }
    }
    auto truncatedFog = std::vector<std::array<int, 3>>{};
    const auto fogAlpha = [&](const int x, const int y) {
      float d = idSqrt(float((x - 64) * (x - 64) + (y - 64) * (y - 64)));
      d /= 63;
      const float scaled = d * 255;
      auto b = castByte(double(scaled));
      if (scaled >= 256.0f)
      {
        truncatedFog.push_back({x, y, b});
      }
      b = std::clamp(b, 0, 255);
      b = castByte(255 * (1.0 - double(steps[std::size_t(b)])));
      if (x == 0 || x == 127 || y == 0 || y == 127)
      {
        b = 255;
      }
      return b;
    };
    const auto fraction = [](const float view, const float target) -> float {
      const float total = std::fabs(target - view);
      if (target > 0 && view > 0)
      {
        return 0.0f;
      }
      if (target < -8.0f && view < -8.0f)
      {
        return 1.0f;
      }
      const float above = target > 0 ? target : view > 0 ? view : 0.0f;
      float top = view > target ? view : target;
      float bottom = view > target ? target : view;
      top = top > 0 ? 0.0f : top;
      bottom = bottom < -8.0f ? -8.0f : bottom;
      const float slope = 0.125f;
      if (total == 0.0f)
      {
        return -view * slope;
      }
      const float ramp =
        float((1.0 - double(top * slope + bottom * slope) * -0.5) * double(top - bottom));
      float frac = (total - above - ramp) / total;
      const float deepest = view < target ? view : target;
      const float deep = deepest / -30.0f;
      if (deep >= 1.0f)
      {
        return 1.0f;
      }
      frac = float(double(frac) * (1.0 - double(deep)) + double(deep));
      return frac;
    };
    auto outsideFogEnter = std::vector<std::array<int, 3>>{};
    const auto fogEnterAlpha = [&](const int x, const int y) {
      const float d = fraction(float(x - 32), float(y - 32));
      const float scaled = d * 255;
      auto b = castByte(double(scaled));
      if (scaled >= 256.0f || scaled < 0.0f)
      {
        outsideFogEnter.push_back({x, y, b});
      }
      return std::clamp(b, 0, 255);
    };

    for (const auto& [name, size] :
         std::vector<std::pair<std::string, int>>{{"_fog", 128}, {"_fogEnter", 64}})
    {
      CAPTURE(name);
      const auto recipe = intrinsicRecipe(name);
      REQUIRE(recipe);
      CHECK(recipe->width == size);
      CHECK(recipe->height == size);
      CHECK(recipe->filter == FilterLinear);
      CHECK(recipe->repeat == RepeatClamp);
      CHECK(recipe->usage == 7); // TD_LOOKUP_TABLE_ALPHA
      CHECK(recipe->usage == UsageLookupTableAlpha);
      CHECK(intrinsicImageSize(name) == std::pair{size, size});
      REQUIRE(recipe->rgba.size() == std::size_t(size * size * 4));

      // every texel white with the table's alpha, top row first
      auto mismatches = 0;
      for (auto y = 0; y < size; ++y)
      {
        for (auto x = 0; x < size; ++x)
        {
          const auto* texel = &recipe->rgba[std::size_t((y * size + x) * 4)];
          const auto want = size == 128 ? fogAlpha(x, y) : fogEnterAlpha(x, y);
          mismatches +=
            texel[0] == 255 && texel[1] == 255 && texel[2] == 255 && int(texel[3]) == want
              ? 0
              : 1;
        }
      }
      CHECK(mismatches == 0);

      // DeriveOpts (Image_load.cpp:222-223, :294-301): FMT_ALPHA, one level (TF_LINEAR);
      // Load2DFromMemory stores the alpha bytes alone (BinaryImage.cpp:245-252)
      const auto opts = engineImageOpts(recipe->usage);
      CHECK(opts.format == FormatAlpha);
      CHECK(opts.colorFormat == ColorDefault);
      CHECK(engineMipLevels(opts.format, recipe->filter, size, size) == 1);
      const auto made = intrinsicImage(decls, name);
      REQUIRE(made.is_success());
      const auto& image = *made.value();
      CHECK(image.format == FormatAlpha);
      CHECK(image.colorFormat == ColorDefault);
      CHECK(image.textureType == Type2D);
      CHECK(image.width == size);
      CHECK(image.height == size);
      REQUIRE(image.levels.size() == 1u);
      REQUIRE(image.levels[0].size == std::size_t(size * size));
      REQUIRE(image.data.size() == std::size_t(size * size));
      auto alphaMismatches = 0;
      for (std::size_t i = 0; i < image.data.size(); ++i)
      {
        alphaMismatches += image.data[i] == recipe->rgba[i * 4 + 3] ? 0 : 1;
      }
      CHECK(alphaMismatches == 0);
      // R8, sampled as (1, 1, 1, R) (Image_NVRHI.cpp:538-541), with the image's sampler:
      // linear, clamped to the edge (Image_NVRHI.cpp:173-243)
      CHECK(engineTextureFormat(FormatAlpha, true) == nvrhi::Format::R8_UNORM);
      const auto mapping = engineComponentMapping(FormatAlpha, ColorDefault);
      CHECK(mapping.r == nvrhi::ComponentSwizzle::One);
      CHECK(mapping.g == nvrhi::ComponentSwizzle::One);
      CHECK(mapping.b == nvrhi::ComponentSwizzle::One);
      CHECK(mapping.a == nvrhi::ComponentSwizzle::Red);
      const auto sampler = engineImageSamplerDesc(FormatAlpha, FilterLinear, RepeatClamp);
      REQUIRE(sampler);
      CHECK(sampler->minFilter);
      CHECK(sampler->addressU == nvrhi::SamplerAddressMode::ClampToEdge);
      CHECK(sampler->addressV == nvrhi::SamplerAddressMode::ClampToEdge);
    }

    // the rows the fog shader samples at T = 0.5 (between rows 63 and 64 of _fog, 31 and
    // 32 of _fogEnter), texel by texel
    const auto fogRecipe = *intrinsicRecipe("_fog");
    const auto fogEnterRecipe = *intrinsicRecipe("_fogEnter");
    const auto alphaAt = [](const IntrinsicRecipe& recipe, const int x, const int y) {
      return int(recipe.rgba[std::size_t((y * recipe.width + x) * 4 + 3)]);
    };
    for (const auto row : {63, 64})
    {
      for (auto x = 0; x < 128; ++x)
      {
        CAPTURE(row, x);
        CHECK(alphaAt(fogRecipe, x, row) == fogAlpha(x, row));
      }
    }
    for (const auto row : {31, 32})
    {
      for (auto x = 0; x < 64; ++x)
      {
        CAPTURE(row, x);
        CHECK(alphaAt(fogEnterRecipe, x, row) == fogEnterAlpha(x, row));
      }
    }
    // a few of them by value: _fog's row 64 rises from 0 at the centre to 255 at the
    // border; _fogEnter's row 32 is 0 from the centre on (the view above the plane)
    CHECK(alphaAt(fogRecipe, 64, 64) == 0);
    CHECK(alphaAt(fogRecipe, 8, 64) == 250);
    CHECK(alphaAt(fogRecipe, 56, 64) == 112);
    CHECK(alphaAt(fogRecipe, 72, 64) == 112);
    CHECK(alphaAt(fogRecipe, 0, 64) == 255);
    CHECK(alphaAt(fogEnterRecipe, 0, 32) == 255);
    CHECK(alphaAt(fogEnterRecipe, 4, 32) == 252);
    CHECK(alphaAt(fogEnterRecipe, 28, 32) == 89);
    CHECK(alphaAt(fogEnterRecipe, 32, 32) == 0);
    CHECK(alphaAt(fogEnterRecipe, 63, 32) == 0);

    // the texels whose ( byte ) cast took MSVC's truncation: in _fog the 3,803 texels
    // outside the circle where d · 255 reaches 256 (d past 256 / 255 radii), toward the
    // corners, none of them on the sampled rows; at (1, 1) d · 255 = 360.6 is cast to
    // 104, not clamped to 255, and the alpha is 216 where a clamp would give 252; the
    // border's 255 overwrites row and column 0 and 127. In _fogEnter FogFraction stays in
    // 0..1: no texel takes it
    truncatedFog.clear();
    outsideFogEnter.clear();
    for (auto y = 0; y < 128; ++y)
    {
      for (auto x = 0; x < 128; ++x)
      {
        fogAlpha(x, y);
      }
    }
    for (auto y = 0; y < 64; ++y)
    {
      for (auto x = 0; x < 64; ++x)
      {
        fogEnterAlpha(x, y);
      }
    }
    CHECK(truncatedFog.size() == 3803u);
    CHECK(outsideFogEnter.empty());
    const auto truncatedAt = [&](const int x, const int y) {
      for (const auto& [tx, ty, b] : truncatedFog)
      {
        if (tx == x && ty == y)
        {
          return b;
        }
      }
      return -1;
    };
    CHECK(truncatedAt(0, 0) == 110);
    CHECK(alphaAt(fogRecipe, 0, 0) == 255);
    CHECK(truncatedAt(1, 1) == 104);
    CHECK(alphaAt(fogRecipe, 1, 1) == 216);
    CHECK(truncatedAt(2, 2) == 98);
    CHECK(alphaAt(fogRecipe, 2, 2) == 212);
    CHECK(truncatedAt(3, 3) == 93);
    CHECK(alphaAt(fogRecipe, 3, 3) == 207);
    // the centre is texel 64, so (126, 126) mirrors (2, 2)
    CHECK(truncatedAt(126, 126) == 98);
    CHECK(alphaAt(fogRecipe, 126, 126) == alphaAt(fogRecipe, 2, 2));
    for (const auto& [x, y, b] : truncatedFog)
    {
      CHECK_FALSE(((y == 63 || y == 64) && x > 0 && x < 127));
    }
    // a clamp in place of the cast would differ there
    CHECK(castByte(255 * (1.0 - double(steps[255]))) == 252);
  }

  SECTION("every other intrinsic answers Default with its name")
  {
    for (const auto* name :
         {"_cyan",
          "_redClay",
          "_alphaNotch",
          "_defaultUACIrradiance",
          "_blueNoise256",
          "_jitter4",
          "_shadowMapAtlas"})
    {
      CAPTURE(name);
      CHECK_FALSE(intrinsicRecipe(name));
      const auto made = intrinsicImage(decls, name);
      REQUIRE(made.is_error());
      CHECK(
        std::get<Error>(made.error()).msg
        == fmt::format(
          "the preview does not make the engine's intrinsic image {} yet", name));
    }
  }

  SECTION("LP4.2a (D85): _brdfLut, the copied header's array whole")
  {
    CHECK_FALSE(intrinsicRecipe("_brdfLut"));
    const auto raw = rawIntrinsicImage("_brdfLut");
    REQUIRE(raw);
    const auto& image = **raw;
    CHECK(image.textureType == Type2D);
    CHECK(image.format == FormatRg16f);
    CHECK(image.storedFormat == FormatRg16f);
    CHECK(image.colorFormat == ColorDefault);
    CHECK(image.width == 256);
    CHECK(image.height == 256);
    CHECK(image.numLevels == 1);
    CHECK_FALSE(image.fromSource);
    // 256 × 256 × 4 bytes: the array's size, twice the header's BRDFLUT_TEX_SIZE
    REQUIRE(image.data.size() == 262144u);
    REQUIRE(image.levels.size() == 1u);
    CHECK(image.levels[0].level == 0);
    CHECK(image.levels[0].destZ == 0);
    CHECK(image.levels[0].width == 256);
    CHECK(image.levels[0].height == 256);
    CHECK(image.levels[0].offset == 0u);
    CHECK(image.levels[0].size == 262144u);
    // GetRowPitch's 1024 bytes a row: one level of 256 rows
    CHECK(engineRowPitch(FormatRg16f, 256) == 1024);
    // the whole array byte for byte: its FNV-1a 64 as the test's script computed it from
    // the engine's own file at 285ea4d1 (neo/renderer/Image_brdfLut.h, blob c7c59142)
    auto hash = std::uint64_t{0xcbf29ce484222325ull};
    for (const auto byte : image.data)
    {
      hash ^= byte;
      hash *= 0x100000001b3ull;
    }
    CHECK(hash == 0x531975a019261c39ull);
    CHECK(engineBrdfLutSize() == 262144u);
    CHECK(std::equal(image.data.begin(), image.data.end(), engineBrdfLutData()));
    // the first texel (0xf8 0x20 0xe8 0x3b: R 0x20f8, G 0x3be8) and the last (0xf3 0x34
    // 0x00 0x00: R 0x34f3, G 0) by value
    const auto texel = [&](const std::size_t i) {
      return std::array<std::uint16_t, 2>{
        std::uint16_t(image.data[i * 4] | (image.data[i * 4 + 1] << 8)),
        std::uint16_t(image.data[i * 4 + 2] | (image.data[i * 4 + 3] << 8))};
    };
    CHECK(texel(0) == std::array<std::uint16_t, 2>{0x20f8, 0x3be8});
    CHECK(texel(65535) == std::array<std::uint16_t, 2>{0x34f3, 0x0000});
    CHECK(engineF16toF32(texel(0)[0]) == 0.00970458984375f);
    CHECK(engineF16toF32(texel(0)[1]) == 0.98828125f);
    CHECK(engineF16toF32(texel(65535)[0]) == 0.309326171875f);
    // through the service's path, as the parse thread answers it
    const auto made = intrinsicImage(decls, "_brdfLut");
    REQUIRE(made.is_success());
    CHECK(made.value()->data == image.data);
  }

  SECTION("LP4.2a (D85): _defaultUACRadiance, an 8 x 8 cube of zero BC6H blocks")
  {
    const auto raw = rawIntrinsicImage("_defaultUACRadiance");
    REQUIRE(raw);
    const auto& image = **raw;
    CHECK(image.textureType == TypeCubic);
    CHECK(image.format == FormatBc6h);
    CHECK(image.storedFormat == FormatBc6h);
    CHECK(image.width == 8);
    CHECK(image.height == 8);
    CHECK(image.numLevels == 1);
    REQUIRE(image.levels.size() == 6u);
    for (auto side = 0; side < 6; ++side)
    {
      CAPTURE(side);
      const auto& level = image.levels[std::size_t(side)];
      CHECK(level.level == 0);
      CHECK(level.destZ == side);
      CHECK(level.width == 8);
      CHECK(level.height == 8);
      CHECK(level.size == 64u);
      CHECK(level.offset == std::size_t(side) * 64u);
      // GetRowPitch: two 16-byte blocks a row of blocks, two rows of blocks
      CHECK(engineRowPitch(FormatBc6h, 8) == 32);
    }
    REQUIRE(image.data.size() == 384u);
    CHECK(std::ranges::all_of(image.data, [](const auto byte) { return byte == 0; }));
    const auto made = intrinsicImage(decls, "_defaultUACRadiance");
    REQUIRE(made.is_success());
    CHECK(made.value()->textureType == TypeCubic);
    CHECK_FALSE(rawIntrinsicImage("_defaultUACIrradiance"));
    CHECK_FALSE(rawIntrinsicImage("_white"));
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
// U2.4: the intrinsics made 100 times over leave the CRT's debug heap as two warm-up
// rounds left it (the parse thread makes them at every request of a new session)
TEST_CASE("IntrinsicImages: 100 rounds on the debug heap (U2.4)")
{
  auto source = MemoryDeclSource{{}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  auto failed = 0;
  const auto round = [&] {
    for (const auto* name :
         {"_default",
          "_white",
          "_black",
          "_blackDiffuse",
          "_flat",
          "_noFalloff",
          "_quadratic",
          "_fog",
          "_fogEnter"})
    {
      failed += intrinsicImage(decls, name).is_success() ? 0 : 1;
    }
  };
  round();
  round();
  CHECK_FALSE(heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      round();
    }
  }));
  CHECK(failed == 0);
}
#endif

} // namespace tb::lit
