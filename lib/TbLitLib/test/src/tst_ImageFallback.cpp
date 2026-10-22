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

// LP1c.2 (S14, D24): the source fallback. Where the engine would binarize, the copied
// loaders read the source and the levels are the engine's encoder input: its channel
// layouts as RGBA8 (R11G11B10F for HDR usages), checked exactly here against layouts
// computed independently from Load2DFromMemory's rules; the engine's own caches are
// compared in tst_ImageGoldens.cpp (D61).

#include "ImageTestFiles.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/EditorImageSize.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;
using namespace test;
using namespace std::chrono_literals;

constexpr int CubeNative = 1;
constexpr int CubePanorama = 3;
constexpr int CubeSingle = 6;

MaterialImageKey keyOf(
  const std::string& name,
  const int usage,
  const int cube = Cube2D,
  const int filter = FilterDefault)
{
  auto key = MaterialImageKey{};
  key.name = name;
  key.usage = usage;
  key.filter = filter;
  key.cube = cube;
  return key;
}

int clampByte(const int x)
{
  return std::clamp(x, 0, 255);
}

// ColorSpace.cpp:55-57 and ConvertRGBToCoCg_Y, written out again
std::vector<std::uint8_t> coCgY(const Rgba& image)
{
  auto out = std::vector<std::uint8_t>{};
  for (auto i = 0; i < image.width * image.height; ++i)
  {
    const int r = image.pixels[std::size_t(4 * i)];
    const int g = image.pixels[std::size_t(4 * i + 1)];
    const int b = image.pixels[std::size_t(4 * i + 2)];
    out.push_back(std::uint8_t(clampByte(((((r << 1) - (b << 1)) + 2) >> 2) + 128)));
    out.push_back(std::uint8_t(clampByte((((-r + (g << 1) - b) + 2) >> 2) + 128)));
    out.push_back(0);
    out.push_back(std::uint8_t(clampByte(((r + 2 * g + b) + 2) >> 2)));
  }
  return out;
}

// the box filter of R_MipMap for even sizes
std::vector<std::uint8_t> boxMip(
  const std::vector<std::uint8_t>& in, const int width, const int height)
{
  auto out = std::vector<std::uint8_t>{};
  const auto row = width * 4;
  for (auto y = 0; y < height / 2; ++y)
  {
    for (auto x = 0; x < width / 2; ++x)
    {
      const auto at = std::size_t(2 * y * row + 2 * x * 4);
      for (auto c = 0; c < 4; ++c)
      {
        out.push_back(std::uint8_t(
          (in[at + std::size_t(c)] + in[at + 4 + std::size_t(c)]
           + in[at + std::size_t(row + c)] + in[at + std::size_t(row + 4 + c)])
          >> 2));
      }
    }
  }
  return out;
}

std::vector<std::uint8_t> levelBytes(const CachedImage& image, const std::size_t record)
{
  const auto& level = image.levels.at(record);
  return {
    image.data.begin() + std::ptrdiff_t(level.offset),
    image.data.begin() + std::ptrdiff_t(level.offset + level.size)};
}

class FallbackGame
{
private:
  fs::TestEnvironment m_env;

public:
  void file(const std::string& name, const std::string& bytes) const
  {
    writeTextFile(m_env.dir() / "base" / "textures" / "lp1c" / name, bytes);
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }
  const std::filesystem::path& path() const { return m_env.dir(); }
};

} // namespace

TEST_CASE("ImageFallback, the engine's channel layouts before its encoder (D24)")
{
  const auto game = FallbackGame{};
  const auto image = gradient(16, 8);
  game.file("grad.tga", tgaFile(image, 32));
  game.file("flat.tga", tgaFile(flat(8, 8, 200, 100, 50, 25), 32));
  game.file("metal_s.tga", tgaFile(flat(8, 8, 1, 2, 3), 32));
  game.file("metal_rmao.png", pngFile(image));
  for (const auto* side : {"_px", "_nx", "_py", "_ny", "_pz", "_nz"})
  {
    game.file(
      std::string{"sky"} + side + ".tga",
      tgaFile(flat(8, 8, std::uint8_t(side[1]), std::uint8_t(side[2]), 7), 32));
  }
  auto pano = std::vector<float>{};
  for (auto i = 0; i < 8 * 4; ++i)
  {
    pano.insert(pano.end(), {2.0f, 0.5f, 1.0f});
  }
  game.file("pano.exr", exrFile(8, 4, pano));

  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto made = [&](const MaterialImageKey& key) {
    const auto slot = resolveImage(decls, source, key);
    INFO(slot.sourceReason);
    REQUIRE(slot.state == ImageCacheSlot::State::Missing);
    REQUIRE(slot.image);
    REQUIRE(slot.image->fromSource);
    return slot;
  };

  SECTION("TD_DIFFUSE: YCoCg (Co, Cg, 0, Y) through DXT5's mip chain")
  {
    const auto slot = made(keyOf("textures/lp1c/grad", UsageDiffuse));
    const auto& out = *slot.image;
    CHECK(out.storedFormat == FormatDxt5);
    CHECK(out.format == FormatRgba8);
    CHECK(out.colorFormat == ColorYCoCgDxt5);
    CHECK(out.textureType == Type2D);
    CHECK(out.numLevels == engineMipLevels(FormatDxt5, FilterDefault, 16, 8));
    REQUIRE(out.levels.size() == std::size_t(out.numLevels));
    CHECK(out.levels[0].width == 16);
    CHECK(out.levels[0].height == 8);
    CHECK(levelBytes(out, 0) == coCgY(image));
    CHECK(out.levels[1].width == 8);
    CHECK(out.levels[1].height == 4);
  }

  SECTION("TD_BUMP: x in alpha, y in green (0, G, 0, R), box-filtered mips")
  {
    const auto slot = made(keyOf("textures/lp1c/grad", UsageBump));
    const auto& out = *slot.image;
    CHECK(out.colorFormat == ColorNormalDxt5);
    auto expected = std::vector<std::uint8_t>{};
    for (auto i = 0; i < 16 * 8; ++i)
    {
      expected.insert(
        expected.end(),
        {0, image.pixels[std::size_t(4 * i + 1)], 0, image.pixels[std::size_t(4 * i)]});
    }
    CHECK(levelBytes(out, 0) == expected);
    // no gamma mips for a bump map: R_MipMap's box filter of the swizzled level
    CHECK(levelBytes(out, 1) == boxMip(expected, 16, 8));
  }

  SECTION(
    "TD_COVERAGE: alpha into green (0, A, 0, 0); TD_DEFAULT and TD_SPECULAR as read")
  {
    const auto coverage = made(keyOf("textures/lp1c/grad", UsageCoverage));
    CHECK(coverage.image->storedFormat == FormatDxt1);
    CHECK(coverage.image->colorFormat == ColorGreenAlpha);
    auto expected = std::vector<std::uint8_t>{};
    for (auto i = 0; i < 16 * 8; ++i)
    {
      expected.insert(expected.end(), {0, image.pixels[std::size_t(4 * i + 3)], 0, 0});
    }
    CHECK(levelBytes(*coverage.image, 0) == expected);

    const auto plain = made(keyOf("textures/lp1c/grad", UsageDefault));
    CHECK(plain.image->colorFormat == ColorDefault);
    CHECK(levelBytes(*plain.image, 0) == image.pixels);
    const auto specular = made(keyOf("textures/lp1c/grad", UsageSpecular));
    CHECK(specular.image->storedFormat == FormatDxt1);
    CHECK(levelBytes(*specular.image, 0) == image.pixels);
  }

  SECTION("gamma mips of a flat image stay flat, at most one step below")
  {
    // R_MipMapWithGamma goes through mip_gammaTable and back with Pow and a truncating
    // Ftob, so a channel may lose one step, as in the engine
    const auto slot = made(keyOf("textures/lp1c/flat", UsageDefault));
    const auto& out = *slot.image;
    CHECK(out.numLevels == 2);
    const auto mip = levelBytes(out, 1);
    REQUIRE(mip.size() == 4u * 4u * 4u);
    const auto input = std::vector<int>{200, 100, 50, 25};
    for (std::size_t i = 0; i < mip.size(); ++i)
    {
      CAPTURE(i);
      CHECK(mip[i] == mip[i % 4]);
      CHECK(int(mip[i]) <= input[i % 4]);
      CHECK(int(mip[i]) >= input[i % 4] - 1);
    }
  }

  SECTION("TD_LIGHT: RGB565, expanded as the cache reader expands it")
  {
    const auto slot = made(keyOf("textures/lp1c/grad", UsageLight));
    const auto& out = *slot.image;
    CHECK(out.storedFormat == FormatRgb565);
    CHECK(out.format == FormatRgba8);
    // not block-compressed: a level per halving down to 1 x 1
    CHECK(out.numLevels == 5);
    auto expected = std::vector<std::uint8_t>{};
    for (auto i = 0; i < 16 * 8; ++i)
    {
      const auto r5 = image.pixels[std::size_t(4 * i)] >> 3;
      const auto g6 = image.pixels[std::size_t(4 * i + 1)] >> 2;
      const auto b5 = image.pixels[std::size_t(4 * i + 2)] >> 3;
      expected.insert(
        expected.end(),
        {std::uint8_t((r5 * 527 + 23) >> 6),
         std::uint8_t((g6 * 259 + 33) >> 6),
         std::uint8_t((b5 * 527 + 23) >> 6),
         255});
    }
    CHECK(levelBytes(out, 0) == expected);
  }

  SECTION("an _s with an _rmao.png sibling loads the sibling as TD_SPECULAR_PBR_RMAO")
  {
    const auto slot = made(keyOf("textures/lp1c/metal_s", UsageSpecular));
    CHECK(slot.name.usage == UsageSpecularPbrRmao);
    CHECK(slot.name.imageName == "textures/lp1c/metal_rmao");
    CHECK(slot.image->storedFormat == FormatDxt1);
    CHECK(levelBytes(*slot.image, 0) == image.pixels);
    CHECK(slot.image->file == "textures/lp1c/metal_rmao#__1200");
  }

  SECTION("a six-sided cube: CFM_DEFAULT, six records per level")
  {
    const auto slot = made(keyOf("textures/lp1c/sky", UsageDefault, CubeNative));
    const auto& out = *slot.image;
    CHECK(out.textureType == TypeCubic);
    CHECK(out.colorFormat == ColorDefault);
    CHECK(out.width == 8);
    REQUIRE(out.levels.size() == std::size_t(6 * out.numLevels));
    const auto sides = std::vector<std::string>{"px", "nx", "py", "ny", "pz", "nz"};
    for (std::size_t side = 0; side < 6; ++side)
    {
      CAPTURE(side);
      CHECK(out.levels[side].level == 0);
      CHECK(out.levels[side].destZ == int(side));
      CHECK(
        levelBytes(out, side)
        == flat(8, 8, std::uint8_t(sides[side][0]), std::uint8_t(sides[side][1]), 7)
             .pixels);
      CHECK(out.levels[6 + side].level == 1);
      CHECK(out.levels[6 + side].width == 4);
    }
  }

  SECTION("an HDR panorama (hdriMap, TD_HDRI): six 1024 sides of R11G11B10F")
  {
    const auto slot = made(keyOf("textures/lp1c/pano", UsageHdri, CubePanorama));
    const auto& out = *slot.image;
    CHECK(out.storedFormat == FormatBc6h);
    CHECK(out.format == FormatR11g11b10f);
    CHECK(out.textureType == TypeCubic);
    CHECK(out.width == 1024);
    // 2.0, 0.5, 1.0 packed: exact in R11G11B10F
    const auto texel = levelBytes(out, 0);
    const auto value = std::uint32_t(texel[0]) | (std::uint32_t(texel[1]) << 8)
                       | (std::uint32_t(texel[2]) << 16)
                       | (std::uint32_t(texel[3]) << 24);
    CHECK((value & 0x7FF) == ((16u << 6) | 0u));
    CHECK(((value >> 11) & 0x7FF) == ((14u << 6) | 0u));
    CHECK(((value >> 22) & 0x3FF) == ((15u << 5) | 0u));
  }

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

namespace
{

// S2.1: a game source that sets the stop flag the first time a file is read whole (a
// Stop between two sides of a cube); every other call goes to the game's GameFileSource
class StoppingSource : public DeclSource
{
private:
  GameFileSource& m_files;
  std::atomic<bool>& m_stop;
  std::string m_stopAt;

public:
  StoppingSource(GameFileSource& files, std::atomic<bool>& stop, std::string stopAt)
    : m_files{files}
    , m_stop{stop}
    , m_stopAt{std::move(stopAt)}
  {
  }

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override
  {
    return m_files.listFiles(folder, extension);
  }
  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override
  {
    return m_files.readFile(path, timestamp);
  }
  GameFileProbe probe(const std::string& path) override { return m_files.probe(path); }
  std::optional<std::string> readGameFile(
    const std::string& path, GameFileProbe* found) override
  {
    if (path == m_stopAt)
    {
      m_stop = true;
      m_stopAt.clear();
    }
    return m_files.readGameFile(path, found);
  }
  std::optional<std::string> readGameFileHead(
    const std::string& path, const std::size_t maxBytes, GameFileProbe* found) override
  {
    return m_files.readGameFileHead(path, maxBytes, found);
  }
  bool usingResourceFiles() const override { return m_files.usingResourceFiles(); }
  // sets the stop flag when this file is read next
  void stopAt(std::string path) { m_stopAt = std::move(path); }
  std::vector<std::string> takeWarnings() override { return m_files.takeWarnings(); }
  void closeFiles() override { m_files.closeFiles(); }
};

// the six sides of a CF_NATIVE cube, each flat in its own colour
void cubeSides(
  const FallbackGame& game,
  const std::string& name,
  const std::vector<std::string>& sides,
  const int size)
{
  for (const auto& side : sides)
  {
    game.file(
      name + "_" + side + ".tga",
      tgaFile(flat(size, size, std::uint8_t(side[0]), std::uint8_t(side[1]), 7), 32));
  }
}

} // namespace

TEST_CASE("ImageFallback, a cube that fails part-way frees each side once (S2.1)")
{
  const auto all = std::vector<std::string>{"px", "nx", "py", "ny", "pz", "nz"};
  const auto game = FallbackGame{};
  // _nz missing; one side of another size; a side whose header passes and whose
  // run-length data runs out (the pre-validation refuses it as the copy reads it); six
  // good sides, read with a stop after the fourth
  cubeSides(game, "five", {"px", "nx", "py", "ny", "pz"}, 8);
  cubeSides(game, "mixed", all, 8);
  game.file("mixed_ny.tga", tgaFile(flat(4, 4, 1, 2, 3), 32));
  cubeSides(game, "refused", all, 8);
  auto garbage = rleTgaFile(flat(8, 8, 9, 9, 9), 32).substr(0, 18);
  garbage += std::string{"\x05\x01\x02\x03", 4};
  game.file("refused_py.tga", garbage);
  cubeSides(game, "stopped", all, 8);
  // X.2: side 0 with a good header and a body refused (run-length data running out) or
  // undecodable (a PNG whose deflate data stb_image rejects)
  cubeSides(game, "refused0", all, 8);
  game.file("refused0_px.tga", garbage);
  cubeSides(game, "broken0", {"nx", "py", "ny", "pz", "nz"}, 8);
  auto broken = pngFile(flat(8, 8, 1, 2, 3));
  // the zlib header's check bits broken: stb_image refuses the stream, the pre-validation
  // (chunks and sizes) passes it
  broken[broken.find("IDAT") + 5] ^= 0x55;
  game.file("broken0_px.png", broken);
  // Y.1: a side whose .tga has a good 8 x 8 header over run-length data that runs out,
  // beside a 4 x 4 .png the copied loop falls through to: side 0, and side 3
  cubeSides(game, "sib0", all, 8);
  game.file("sib0_px.tga", garbage);
  game.file("sib0_px.png", pngFile(flat(4, 4, 1, 2, 3)));
  cubeSides(game, "sib3", all, 8);
  game.file("sib3_ny.tga", garbage);
  game.file("sib3_ny.png", pngFile(flat(4, 4, 1, 2, 3)));
  // X.1: one picture cut into six sides (cubeMapSingle with a size, a panorama): missing,
  // refused (16 bits), undecodable, and a good one
  auto sixteen = tgaFile(flat(16, 12, 1, 2, 3), 32);
  sixteen[16] = 16;
  game.file("single_refused.tga", sixteen);
  game.file("pano_refused.tga", sixteen);
  game.file("single_broken.png", broken);
  game.file("pano_broken.png", broken);
  game.file("single_good.tga", tgaFile(gradient(16, 12), 32));
  game.file("pano_good.tga", tgaFile(gradient(16, 8), 32));

  auto files = GameFileSource{game.path()};
  auto stop = std::atomic<bool>{false};
  auto source = StoppingSource{files, stop, "textures/lp1c/stopped_ny.tga"};
  auto decls = MaterialDecls{source, &stop};
  REQUIRE(decls.constructionError().empty());

  const auto failed = [&](const std::string& name) {
    CAPTURE(name);
    source.stopAt("textures/lp1c/stopped_ny.tga");
    const auto key = keyOf("textures/lp1c/" + name, UsageDefault, CubeNative);
    const auto slot = resolveImage(decls, source, key);
    stop = false;
    CHECK_FALSE(slot.image);
    static_cast<void>(decls.takeMessages());
    static_cast<void>(source.takeWarnings());
    return slot.sourceReason;
  };
  const auto sized = [&](const std::string& name) {
    const auto key = keyOf("textures/lp1c/" + name, UsageDefault, CubeNative);
    auto size = imageUploadSize(decls, source, key);
    static_cast<void>(decls.takeMessages());
    return size;
  };

  SECTION("a missing side: the cube is the default, its size Default")
  {
    CHECK_THAT(failed("five"), Catch::Matchers::ContainsSubstring("five_nz"));
    const auto size = sized("five");
    CHECK(size.state == EditorImageSize::State::Default);
    CHECK_THAT(size.reason, Catch::Matchers::ContainsSubstring("five_nz"));
  }

  SECTION("sides of two sizes: the engine's warning, nothing leaked")
  {
    CHECK_THAT(failed("mixed"), Catch::Matchers::ContainsSubstring("Mismatched sizes"));
    CHECK(sized("mixed").state == EditorImageSize::State::Default);
  }

  SECTION("a side refused as the copy reads it: the copy frees what it loaded")
  {
    CHECK_THAT(failed("refused"), Catch::Matchers::ContainsSubstring("refused_py.tga"));
    // its header is a good 8 × 8 one: the size is measured by headers (M-IMG-DIFF-07)
    CHECK(sized("refused").state == EditorImageSize::State::Ready);
  }

  SECTION("X.2: side 0 refused or undecodable behind a good header: nothing leaked")
  {
    CHECK_THAT(failed("refused0"), Catch::Matchers::ContainsSubstring("refused0_px"));
    CHECK_THAT(
      failed("broken0"), Catch::Matchers::ContainsSubstring("Couldn't load cube image"));
  }

  SECTION("Y.1: a side's .tga refused, a sibling of another size: every side freed")
  {
    CHECK_THAT(failed("sib0"), Catch::Matchers::ContainsSubstring("sib0_px.tga"));
    CHECK_THAT(failed("sib3"), Catch::Matchers::ContainsSubstring("sib3_ny.tga"));
  }

  SECTION("X.1: a picture cut into sides that is missing, refused or undecodable")
  {
    for (const auto* kind : {"single", "pano"})
    {
      for (const auto* what : {"missing", "refused", "broken"})
      {
        const auto name = std::string{kind} + "_" + what;
        CAPTURE(name);
        auto key = keyOf(
          "textures/lp1c/" + name,
          std::string{kind} == "pano" ? UsageHdri : UsageDefault,
          std::string{kind} == "pano" ? CubePanorama : CubeSingle);
        key.cubeMapSize = 4;
        const auto slot = resolveImage(decls, source, key);
        CHECK_FALSE(slot.image);
        // a refused TGA names its file; the others the cube's failure
        CHECK_THAT(
          slot.sourceReason,
          Catch::Matchers::ContainsSubstring(
            std::string{what} == "refused" ? name + ".tga" : "Couldn't load cube image"));
        static_cast<void>(decls.takeMessages());
        static_cast<void>(source.takeWarnings());
      }
    }
    // a stop before the picture is read: every read fails, the load refuses cleanly
    for (const auto* kind : {"single", "pano"})
    {
      auto key = keyOf(
        "textures/lp1c/" + std::string{kind} + "_good",
        std::string{kind} == "pano" ? UsageHdri : UsageDefault,
        std::string{kind} == "pano" ? CubePanorama : CubeSingle);
      key.cubeMapSize = 4;
      const auto name = decls.imageCacheName(key);
      REQUIRE(name);
      stop = true;
      const auto stopped = decls.loadImagePixels(*name.value, key.cubeMapSize, true);
      stop = false;
      CHECK_FALSE(stopped.pixels);
      // and without the stop it loads: six sides of the cube size, or of 1024
      const auto loaded = decls.loadImagePixels(*name.value, key.cubeMapSize, true);
      REQUIRE(loaded.pixels);
      CHECK(loaded.pixels->faces == 6);
      CHECK(loaded.pixels->width == (std::string{kind} == "pano" ? 1024 : 4));
    }
  }

  SECTION("a stop between sides: the image reads after it fail, nothing is freed twice")
  {
    CHECK_THAT(
      failed("stopped"), Catch::Matchers::ContainsSubstring("Couldn't load cube image"));
    // without the stop the same cube loads
    const auto slot = resolveImage(
      decls, source, keyOf("textures/lp1c/stopped", UsageDefault, CubeNative));
    stop = false;
    REQUIRE(slot.image);
    CHECK(slot.image->levels.size() == std::size_t(6 * slot.image->numLevels));
    CHECK(sized("stopped").width == 8);
  }

#if defined(_MSC_VER) && defined(_DEBUG)
  SECTION("under the debug heap, 20 times over (S2.6)")
  {
    const auto cycle = [&] {
      for (const auto* name :
           {"five", "mixed", "refused", "stopped", "refused0", "broken0", "sib0", "sib3"})
      {
        static_cast<void>(failed(name));
        static_cast<void>(sized(name));
      }
      // X.1: the picture cut into sides, missing, refused, undecodable and stopped
      for (const auto* kind : {"single", "pano"})
      {
        for (const auto* what : {"missing", "refused", "broken", "good"})
        {
          auto key = keyOf(
            "textures/lp1c/" + std::string{kind} + "_" + what,
            std::string{kind} == "pano" ? UsageHdri : UsageDefault,
            std::string{kind} == "pano" ? CubePanorama : CubeSingle);
          key.cubeMapSize = 4;
          if (std::string{what} != "good")
          {
            static_cast<void>(resolveImage(decls, source, key));
          }
          const auto name = decls.imageCacheName(key);
          REQUIRE(name);
          stop = true;
          static_cast<void>(decls.loadImagePixels(*name.value, key.cubeMapSize, true));
          stop = false;
          static_cast<void>(decls.takeMessages());
          static_cast<void>(source.takeWarnings());
        }
      }
    };
    cycle();
    const auto grew = heapGrows([&] {
      for (auto i = 0; i < 20; ++i)
      {
        cycle();
      }
    });
    CHECK_FALSE(grew);
  }
#endif

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("ImageFallback, the fallback runs only where the engine would binarize")
{
  const auto game = FallbackGame{};
  const auto image = gradient(8, 8);
  game.file("plain.tga", tgaFile(image, 32));
  auto truncated = tgaFile(image, 32);
  truncated.resize(truncated.size() - 5);
  game.file("half.tga", truncated);

  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};

  SECTION("no cache: the source")
  {
    const auto slot =
      resolveImage(decls, source, keyOf("textures/lp1c/plain", UsageDefault));
    CHECK(slot.state == ImageCacheSlot::State::Missing);
    REQUIRE(slot.image);
    CHECK(slot.image->fromSource);
    CHECK(slot.image->origin.kind == GameFileProbe::Kind::Loose);
    CHECK(slot.sourceReason.empty());
  }

  SECTION("a refused file: no image, the reason names it")
  {
    static_cast<void>(decls.takeMessages());
    const auto slot =
      resolveImage(decls, source, keyOf("textures/lp1c/half", UsageDefault));
    CHECK_FALSE(slot.image);
    CHECK_THAT(
      slot.sourceReason, Catch::Matchers::ContainsSubstring("textures/lp1c/half.tga"));
    auto warnings = 0;
    for (const auto& message : decls.takeMessages())
    {
      warnings += message.kind == DeclMessageKind::Warning ? 1 : 0;
    }
    CHECK(warnings == 1);
  }

  SECTION("an intrinsic: generated as the engine's cache would hold it (D69, LP2.1)")
  {
    auto key = keyOf("_white", UsageDefault);
    key.intrinsic = true;
    const auto slot = resolveImage(decls, source, key);
    CHECK(slot.name.generated);
    REQUIRE(slot.image);
    CHECK(slot.image->storedFormat == FormatDxt5);
    CHECK(slot.image->format == FormatDxt5);
    CHECK(slot.image->width == 16);
    CHECK(slot.sourceReason.empty());

    // one LP2 does not draw with: no image, a reason that names it
    auto fog = keyOf("_fog", UsageDefault);
    fog.intrinsic = true;
    const auto none = resolveImage(decls, source, fog);
    CHECK(none.name.generated);
    CHECK_FALSE(none.image);
    CHECK(
      none.sourceReason
      == "the preview does not make the engine's intrinsic image _fog yet");
  }

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("MaterialService serves source images (LP1c.2)")
{
  const auto game = FallbackGame{};
  game.file("plain.tga", tgaFile(gradient(8, 8), 32));
  auto options = MaterialServiceOptions{};
  options.makeSource = [path = game.path()](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();

  const auto plain = keyOf("textures/lp1c/plain", UsageDefault);
  const auto none = keyOf("textures/lp1c/none", UsageDefault);
  auto white = keyOf("_white", UsageDefault);
  white.intrinsic = true;
  auto cyan = keyOf("_cyan", UsageDefault);
  cyan.intrinsic = true;
  CHECK(service->image(plain).state == ImageLookup::State::Pending);
  static_cast<void>(service->image(none));
  static_cast<void>(service->image(white));
  static_cast<void>(service->image(cyan));
  REQUIRE(service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.findImage(plain) && t.findImage(none) && t.findImage(white)
             && t.findImage(cyan);
    },
    10s));
  const auto found = service->image(plain);
  CHECK(found.state == ImageLookup::State::Source);
  REQUIRE(found.image);
  CHECK(found.image->fromSource);
  CHECK(service->image(none).state == ImageLookup::State::Default);
  // D69 (LP2.1): LP2's intrinsics are made; any other is the default image
  const auto generated = service->image(white);
  CHECK(generated.state == ImageLookup::State::Generated);
  CHECK(generated.image);
  const auto notMade = service->image(cyan);
  CHECK(notMade.state == ImageLookup::State::Default);
  REQUIRE(notMade.slot);
  CHECK(notMade.slot->sourceReason.find("_cyan") != std::string::npos);
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
