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
// the cache reader) equals the fallback exactly. LP1c fix (S3.2): a DXT1 or DXT5 cache
// equals, block for block at every level, the fallback's level encoded by the engine's
// own fast encoders (copied verbatim, test only: TbLitIdDxtTestLib), so the fallback's
// mip and gamma rules and its channel layouts are checked exactly. The HDR panoramas'
// caches are BC6H, 8 MB each: their headers and level records are compared, their texels
// are not (no BC6H encoder or decoder here, deferred).

#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/DxtEncoderForTesting.h"
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

/**
 * LP1c fix (S3.2): the encoder the engine's binarizer runs on a level
 * (BinaryImage.cpp:56-331, :679-837 with image_highQualityCompression 0): DXT1's for any
 * DXT1 image, and for DXT5 the normal map's, YCoCg's or the plain one by the colour
 * format (a cube's is CFM_DEFAULT)
 */
DxtEncoderForTesting encoderOf(const CachedImage& golden)
{
  if (golden.format == FormatDxt1)
  {
    return DxtEncoderForTesting::Dxt1;
  }
  return golden.colorFormat == ColorNormalDxt5  ? DxtEncoderForTesting::NormalMapDxt5
         : golden.colorFormat == ColorYCoCgDxt5 ? DxtEncoderForTesting::YCoCgDxt5
                                                : DxtEncoderForTesting::Dxt5;
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

  // X.6: without the copied encoders (the engine's SSE2 ones; an Apple Silicon build)
  // only the DXT blocks go unchecked, never the shapes, the uncompressed caches or the
  // rest
  const auto encoders = dxtEncoderForTestingAvailable();
  if (!encoders)
  {
    WARN(
      "the engine's fast DXT encoders are its SSE2 ones, which this build has not: the "
      "DXT1 and DXT5 blocks are not compared");
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

    auto texels = 0;
    auto dxtLevels = 0;
    auto exactLevels = 0;
    for (std::size_t i = 0; i < ours.levels.size(); ++i)
    {
      CAPTURE(i);
      const auto& level = ours.levels[i];
      const auto& engineLevel = golden.levels[i];
      CHECK(level.level == engineLevel.level);
      CHECK(level.destZ == engineLevel.destZ);
      CHECK(level.width == engineLevel.width);
      CHECK(level.height == engineLevel.height);
      const auto* data = ours.data.data() + level.offset;
      const auto* engineData = golden.data.data() + engineLevel.offset;
      texels += level.width * level.height;
      if ((golden.format == FormatDxt1 || golden.format == FormatDxt5) && !encoders)
      {
        continue;
      }
      if (golden.format == FormatDxt1 || golden.format == FormatDxt5)
      {
        // S3.2: the fallback's level through the engine's own encoder, block for block
        ++dxtLevels;
        const auto encoded = encodeDxtForTesting(
          encoderOf(golden),
          std::vector<std::uint8_t>(data, data + level.size),
          level.width,
          level.height,
          golden.textureType == TypeCubic);
        REQUIRE(encoded.size() == engineLevel.size);
        const auto same = std::equal(encoded.begin(), encoded.end(), engineData);
        CHECK(same);
        exactLevels += same ? 1 : 0;
      }
      else
      {
        // RGB565 expanded alike, R11G11B10F, RGBA8: byte for byte
        REQUIRE(level.size == engineLevel.size);
        const auto same = std::equal(data, data + level.size, engineData);
        CHECK(same);
        exactLevels += same ? 1 : 0;
      }
    }
    lines.push_back(fmt::format(
      "  {}: stored format {}, colour {}, {} levels ({} through the engine's encoder), "
      "{} "
      "texels: {} of {} levels exact",
      generatedName,
      golden.storedFormat,
      golden.colorFormat,
      golden.numLevels,
      dxtLevels,
      texels,
      exactLevels,
      ours.levels.size()));
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
