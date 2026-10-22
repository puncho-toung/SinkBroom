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

// LP1c.2 (D61): the source fallback against the caches the engine wrote for the same
// sources (TOOLS/lit-preview/capture_fixtures.py: touch material per fixture material,
// the LP1b caches from its dmap and map). An uncompressed cache (RGB565, read as RGBA8 by
// the cache reader) equals the fallback exactly; a DXT1 or DXT5 cache is decoded by the
// BC1 and BC3 decoder below and compared within a per-texel bound derived from the
// engine's fast encoders (endpointBound); the channel layout is checked exactly where the
// encoder keeps a channel whole (the normal map's red and blue are 0, the YCoCg alpha
// holds Y). The HDR panoramas' caches are BC6H, 8 MB each: their headers and level
// records are compared, their texels are not (no BC6H decoder here).

#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/MaterialDecls.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;

std::filesystem::path goldenImages()
{
  return litFixtureFolder() / "goldens" / "lp1c" / "generated" / "images";
}

// ---- a reference BC1/BC3 decoder (D3D's rules: BC3's colour block is always 4-colour)
// ----

std::array<int, 3> expand565(const std::uint32_t c)
{
  const auto r = int((c >> 11) & 31);
  const auto g = int((c >> 5) & 63);
  const auto b = int(c & 31);
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

// one 4×4 block's 16 texels, RGBA8, row-major
std::array<std::array<int, 4>, 16> decodeBlock(const std::uint8_t* block, const bool dxt5)
{
  auto out = std::array<std::array<int, 4>, 16>{};
  const auto* color = dxt5 ? block + 8 : block;
  const auto c0 = std::uint32_t(color[0]) | (std::uint32_t(color[1]) << 8);
  const auto c1 = std::uint32_t(color[2]) | (std::uint32_t(color[3]) << 8);
  const auto e0 = expand565(c0);
  const auto e1 = expand565(c1);
  auto palette = std::array<std::array<int, 4>, 4>{};
  const auto fourColour = dxt5 || c0 > c1;
  for (auto ch = 0; ch < 3; ++ch)
  {
    palette[0][ch] = e0[ch];
    palette[1][ch] = e1[ch];
    palette[2][ch] = fourColour ? (2 * e0[ch] + e1[ch]) / 3 : (e0[ch] + e1[ch]) / 2;
    palette[3][ch] = fourColour ? (e0[ch] + 2 * e1[ch]) / 3 : 0;
  }
  palette[0][3] = palette[1][3] = palette[2][3] = 255;
  palette[3][3] = fourColour ? 255 : 0;
  const auto indices = std::uint32_t(color[4]) | (std::uint32_t(color[5]) << 8)
                       | (std::uint32_t(color[6]) << 16)
                       | (std::uint32_t(color[7]) << 24);
  for (auto i = 0; i < 16; ++i)
  {
    out[std::size_t(i)] = palette[(indices >> (2 * i)) & 3];
  }
  if (dxt5)
  {
    const auto a0 = int(block[0]);
    const auto a1 = int(block[1]);
    auto alphas = std::array<int, 8>{a0, a1};
    if (a0 > a1)
    {
      for (auto i = 2; i < 8; ++i)
      {
        alphas[std::size_t(i)] = ((8 - i) * a0 + (i - 1) * a1) / 7;
      }
    }
    else
    {
      for (auto i = 2; i < 6; ++i)
      {
        alphas[std::size_t(i)] = ((6 - i) * a0 + (i - 1) * a1) / 5;
      }
      alphas[6] = 0;
      alphas[7] = 255;
    }
    auto bits = std::uint64_t{0};
    for (auto i = 0; i < 6; ++i)
    {
      bits |= std::uint64_t(block[2 + i]) << (8 * i);
    }
    for (auto i = 0; i < 16; ++i)
    {
      out[std::size_t(i)][3] = alphas[std::size_t((bits >> (3 * i)) & 7)];
    }
  }
  return out;
}

// a DXT level decoded to RGBA8 texels, cut to its width and height
std::vector<std::array<int, 4>> decodeLevel(
  const std::uint8_t* data, const int width, const int height, const bool dxt5)
{
  const auto blocksWide = (width + 3) / 4;
  const auto blockBytes = dxt5 ? 16 : 8;
  auto out = std::vector<std::array<int, 4>>(std::size_t(width * height));
  for (auto by = 0; by < (height + 3) / 4; ++by)
  {
    for (auto bx = 0; bx < blocksWide; ++bx)
    {
      const auto texels =
        decodeBlock(data + std::size_t((by * blocksWide + bx) * blockBytes), dxt5);
      for (auto i = 0; i < 16; ++i)
      {
        const auto x = bx * 4 + i % 4;
        const auto y = by * 4 + i / 4;
        if (x < width && y < height)
        {
          out[std::size_t(y * width + x)] = texels[std::size_t(i)];
        }
      }
    }
  }
  return out;
}

/**
 * The bound of the engine's fast DXT encoders on one channel of a 4×4 block whose values
 * span range (derived from DXTEncoder.cpp at the pinned commit, Fast paths, which
 * image_highQualityCompression 0 selects):
 *
 * - every endpoint is truncated to its bits (ColorTo565: off by at most 7 for 5 bits and
 *   3 for 6 bits; the alpha endpoints are whole bytes);
 * - a channel fitted alone (DXT5's alpha, the normal map's green, EmitGreenIndices) has
 *   its own bounding box inset by range / 16 (colour) or range / 32 (alpha), and each
 *   texel the nearest of 4 colours or 8 alphas, at most half their spacing away (range /
 *   6, range / 14): so range / 16 + range / 6 + quantisation, or range / 32 + range / 14;
 * - the colour channels of a BC1 block share one index per texel, chosen along the
 *   bounding box's diagonal from its minimum to its maximum corner, which the fast
 *   encoders never flip: a channel that falls where the others rise is fitted backwards,
 *   so a texel can be off by the channel's whole range in the block, plus quantisation
 *   (the gradient fixtures show it: their green falls as their red rises).
 *
 * Each bound adds 2 for the interpolation's and the decoder's rounding.
 */
int endpointBound(const int range, const int bits, const bool fittedAlone)
{
  if (bits == 8)
  {
    return range / 32 + range / 14 + 2;
  }
  const auto quantisation = bits == 5 ? 7 : 3;
  return (fittedAlone ? range / 16 + range / 6 : range) + quantisation + 2;
}

struct Comparison
{
  int texels = 0;
  int maxError = 0;
  std::vector<int> levelErrors;
  int worstBound = 0;
  int overBound = 0;
};

/**
 * One DXT level of the fallback (RGBA8 in the engine's channel layout) against the
 * engine's decoded blocks. Each channel's bound comes from the fallback's own range in
 * the block; YCoCg's colour channels are compared after the block's scale (stored in blue
 * as (scale - 1) << 3) is undone, and its blue is the scale, not compared.
 */
void compareDxtLevel(
  const std::uint8_t* ours,
  const std::vector<std::array<int, 4>>& engine,
  const int width,
  const int height,
  const int colorFormat,
  const bool dxt5,
  Comparison& result)
{
  for (auto by = 0; by < (height + 3) / 4; ++by)
  {
    for (auto bx = 0; bx < (width + 3) / 4; ++bx)
    {
      // the channel ranges of the block, in the fallback's bytes
      auto low = std::array<int, 4>{255, 255, 255, 255};
      auto high = std::array<int, 4>{0, 0, 0, 0};
      for (auto i = 0; i < 16; ++i)
      {
        const auto x = bx * 4 + i % 4;
        const auto y = by * 4 + i / 4;
        if (x >= width || y >= height)
        {
          continue;
        }
        for (auto c = 0; c < 4; ++c)
        {
          const auto v = int(ours[std::size_t((y * width + x) * 4 + c)]);
          low[std::size_t(c)] = std::min(low[std::size_t(c)], v);
          high[std::size_t(c)] = std::max(high[std::size_t(c)], v);
        }
      }
      for (auto i = 0; i < 16; ++i)
      {
        const auto x = bx * 4 + i % 4;
        const auto y = by * 4 + i / 4;
        if (x >= width || y >= height)
        {
          continue;
        }
        const auto& texel = engine[std::size_t(y * width + x)];
        auto decoded = texel;
        auto scale = 1;
        if (colorFormat == ColorYCoCgDxt5)
        {
          scale = texel[2] / 8 + 1;
          for (auto c = 0; c < 2; ++c)
          {
            decoded[std::size_t(c)] =
              int(std::lround(double(texel[std::size_t(c)] - 128) / scale)) + 128;
          }
        }
        ++result.texels;
        for (auto c = 0; c < 4; ++c)
        {
          const auto v = int(ours[std::size_t((y * width + x) * 4 + c)]);
          if (c == 3 && !dxt5)
          {
            continue;
          }
          if (colorFormat == ColorYCoCgDxt5 && c == 2)
          {
            continue;
          }
          const auto range = high[std::size_t(c)] - low[std::size_t(c)];
          // the normal map's red and blue, and YCoCg's alpha, are kept whole by the
          // encoder's layout: 0, 0 and Y within the alpha bound
          auto bound = 0;
          if (colorFormat == ColorNormalDxt5 && (c == 0 || c == 2))
          {
            bound = 0;
          }
          else if (c == 3)
          {
            bound = endpointBound(range, 8, true);
          }
          else
          {
            const auto alone = colorFormat == ColorNormalDxt5;
            bound = endpointBound(range * scale, c == 1 ? 6 : 5, alone) / scale + 1;
          }
          const auto error = std::abs(decoded[std::size_t(c)] - v);
          result.maxError = std::max(result.maxError, error);
          result.levelErrors.back() = std::max(result.levelErrors.back(), error);
          result.worstBound = std::max(result.worstBound, bound);
          result.overBound += error > bound ? 1 : 0;
        }
      }
    }
  }
}

} // namespace

TEST_CASE("ImageGoldens, the source fallback against the engine's caches (D61)")
{
  // the fixture sources, as the capture staged them, without their caches
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  for (const auto* folder : {"textures/lp1b", "textures/lp1c", "lights/lp1c"})
  {
    for (const auto& file :
         std::filesystem::directory_iterator{litFixtureFolder() / folder})
    {
      const auto target = base / folder / file.path().filename();
      writeTextFile(target, readTextFile(file.path()));
    }
  }
  for (const auto* mtr : {"lp1bc.mtr", "lp1c.mtr"})
  {
    writeTextFile(
      base / "materials" / mtr, readTextFile(litFixtureFolder() / "materials" / mtr));
  }

  auto source = GameFileSource{env.dir()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  // every image of the fixture materials that has a golden cache, by its generated name
  auto keys = std::map<std::string, MaterialImageKey>{};
  for (const auto& name : decls.declNames("material"))
  {
    if (name.rfind("textures/lp1b/", 0) != 0 && name.rfind("textures/lp1c/", 0) != 0)
    {
      continue;
    }
    const auto entry = decls.lookup(name);
    REQUIRE(entry);
    for (const auto& stage : (*entry.value)->stages)
    {
      if (!stage.image || stage.image->intrinsic)
      {
        continue;
      }
      const auto cacheName = decls.imageCacheName(*stage.image);
      REQUIRE(cacheName);
      keys.emplace(cacheName.value->generatedName, *stage.image);
    }
  }

  auto lines = std::vector<std::string>{};
  auto compared = 0;
  for (const auto& [generatedName, key] : keys)
  {
    CAPTURE(generatedName);
    const auto goldenPath = goldenImages() / (generatedName + ".bimage");
    if (!std::filesystem::exists(goldenPath))
    {
      continue;
    }
    const auto golden = readBimage(readTextFile(goldenPath)) | kdl::value();
    const auto slot = resolveImage(decls, source, key);
    INFO(slot.sourceReason);
    REQUIRE(slot.image);
    const auto& ours = *slot.image;
    REQUIRE(ours.fromSource);

    // the shape: type, size, levels, records in the engine's order, the format the
    // engine stored and the channel layout
    CHECK(ours.textureType == golden.textureType);
    CHECK(ours.width == golden.width);
    CHECK(ours.height == golden.height);
    CHECK(ours.numLevels == golden.numLevels);
    CHECK(ours.storedFormat == golden.storedFormat);
    CHECK(ours.colorFormat == golden.colorFormat);
    REQUIRE(ours.levels.size() == golden.levels.size());

    auto result = Comparison{};
    auto exact = true;
    for (std::size_t i = 0; i < ours.levels.size(); ++i)
    {
      const auto& level = ours.levels[i];
      const auto& engineLevel = golden.levels[i];
      CHECK(level.level == engineLevel.level);
      CHECK(level.destZ == engineLevel.destZ);
      CHECK(level.width == engineLevel.width);
      CHECK(level.height == engineLevel.height);
      const auto* data = ours.data.data() + level.offset;
      const auto* engineData = golden.data.data() + engineLevel.offset;
      if (golden.format == FormatDxt1 || golden.format == FormatDxt5)
      {
        exact = false;
        result.levelErrors.push_back(0);
        const auto dxt5 = golden.format == FormatDxt5;
        compareDxtLevel(
          data,
          decodeLevel(engineData, engineLevel.width, engineLevel.height, dxt5),
          level.width,
          level.height,
          golden.colorFormat,
          dxt5,
          result);
      }
      else
      {
        // RGB565 expanded alike, R11G11B10F, RGBA8: byte for byte
        REQUIRE(level.size == engineLevel.size);
        CHECK(std::equal(data, data + level.size, engineData));
        result.texels += int(level.size / 4);
      }
    }
    CHECK(result.overBound == 0);
    lines.push_back(fmt::format(
      "  {}: stored format {}, colour {}, {} levels, {} texels: {}",
      generatedName,
      golden.storedFormat,
      golden.colorFormat,
      golden.numLevels,
      result.texels,
      exact ? std::string{"exact"}
            : fmt::format(
                "max error {} (bound at most {}), {} over the bound; per level {}",
                result.maxError,
                result.worstBound,
                result.overBound,
                fmt::join(result.levelErrors, " "))));
    ++compared;
  }

  SECTION("the HDR panoramas: BC6H caches of six 1024 sides, R11G11B10F from the source")
  {
    for (const auto* name : {"textures/lp1c/pano_exr", "textures/lp1c/pano_hdr"})
    {
      CAPTURE(name);
      auto key = MaterialImageKey{};
      key.name = name;
      key.usage = UsageHdri;
      key.filter = FilterDefault;
      key.cube = 3;
      const auto cacheName = decls.imageCacheName(key);
      REQUIRE(cacheName);
      const auto header = readTextFile(
        goldenImages() / (cacheName.value->generatedName + ".bimage-header"));
      const auto engine = readBimageHeader(header) | kdl::value();
      const auto slot = resolveImage(decls, source, key);
      REQUIRE(slot.image);
      const auto& ours = *slot.image;
      CHECK(engine.storedFormat == FormatBc6h);
      CHECK(ours.storedFormat == FormatBc6h);
      CHECK(ours.format == FormatR11g11b10f);
      CHECK(ours.textureType == engine.textureType);
      CHECK(ours.width == engine.width);
      CHECK(ours.numLevels == engine.numLevels);
      REQUIRE(ours.levels.size() == std::size_t(6 * engine.numLevels));
      // the engine's level records, in its order
      for (std::size_t i = 0; i < ours.levels.size(); ++i)
      {
        const auto at = 36 + i * 20;
        const auto field = [&](const std::size_t k) {
          auto value = 0;
          for (auto b = 0; b < 4; ++b)
          {
            value = (value << 8) | std::uint8_t(header[at + k * 4 + std::size_t(b)]);
          }
          return value;
        };
        CHECK(ours.levels[i].level == field(0));
        CHECK(ours.levels[i].destZ == field(1));
        CHECK(ours.levels[i].width == field(2));
        CHECK(ours.levels[i].height == field(3));
      }
      // 2.0, 0.5, 1.0 packed: exact in R11G11B10F
      const auto* texel = ours.data.data();
      const auto value = std::uint32_t(texel[0]) | (std::uint32_t(texel[1]) << 8)
                         | (std::uint32_t(texel[2]) << 16)
                         | (std::uint32_t(texel[3]) << 24);
      CHECK(value == ((15u << 27) | (14u << 17) | (16u << 6)));
    }
  }

  SECTION("the 16-bit TGA the engine stopped on is the default here")
  {
    auto key = MaterialImageKey{};
    key.name = "textures/lp1c/sixteen";
    key.usage = UsageDiffuse;
    key.filter = FilterDefault;
    const auto slot = resolveImage(decls, source, key);
    CHECK_FALSE(slot.image);
    CHECK(slot.sourceReason.find("pixel size 16") != std::string::npos);
  }

  std::cout << fmt::format("Image goldens: {} caches compared\n", compared);
  for (const auto& line : lines)
  {
    std::cout << line << "\n";
  }
  std::cout << std::flush;
  // 7 of the 8 LP1b caches (glass#__0100 is glass's editor image, no stage's) and the 10
  // LP1c caches (the panoramas apart)
  CHECK(compared == 17);
  static_cast<void>(decls.takeMessages());
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
