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
// blocks of the same level.

#include "LitTestUtils.h"
#include "lit/DxtEncoderForTesting.h"
#include "lit/EditorImageSize.h"
#include "lit/EngineGeometry.h"
#include "lit/EngineImageData.h"
#include "lit/EnginePipelineState.h"
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

  SECTION("every other intrinsic answers Default with its name")
  {
    for (const auto* name :
         {"_fog",
          "_fogEnter",
          "_cyan",
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
          "_quadratic"})
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
