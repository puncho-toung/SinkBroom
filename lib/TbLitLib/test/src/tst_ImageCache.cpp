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

// LP1c.1: the image cache reader (C8, A3, D23, D61). The engine-written caches of the
// LP1b fixture images (goldens/lp1c, from capture_fixtures.py's run) are read exactly; a
// fake game folder holds a name per rule of the cache search and the staleness test.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFile.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;
using namespace std::chrono_literals;

// the time capture_fixtures.py's staged copies of the fixture images had (the caches
// store it)
constexpr std::int64_t FixtureSourceTime = 1792636960;

struct Level
{
  int level = 0;
  int destZ = 0;
  int width = 0;
  int height = 0;
  std::string data;
};

void putBig(std::string& out, const std::uint64_t value, const int bytes)
{
  for (auto i = bytes - 1; i >= 0; --i)
  {
    out.push_back(char((value >> (8 * i)) & 0xFF));
  }
}

// a .bimage as WriteGeneratedFile writes it
std::string bimageBytes(
  const std::int64_t sourceTime,
  const int textureType,
  const int format,
  const int colorFormat,
  const int width,
  const int height,
  const int numLevels,
  const std::vector<Level>& levels,
  const std::uint32_t magic = BimageMagic)
{
  auto out = std::string{};
  putBig(out, std::uint64_t(sourceTime), 8);
  for (const auto value :
       {int(magic), textureType, format, colorFormat, width, height, numLevels})
  {
    putBig(out, std::uint32_t(value), 4);
  }
  for (const auto& level : levels)
  {
    for (const auto value :
         {level.level, level.destZ, level.width, level.height, int(level.data.size())})
    {
      putBig(out, std::uint32_t(value), 4);
    }
    out += level.data;
  }
  return out;
}

// a 4×4 DXT5 cache of one level, its first byte telling the copies apart
std::string dxt5Cache(const char tag, const std::int64_t sourceTime = 0)
{
  auto data = std::string(16, '\0');
  data[0] = tag;
  return bimageBytes(
    sourceTime, Type2D, FormatDxt5, ColorDefault, 4, 4, 1, {{0, 0, 4, 4, data}});
}

MaterialImageKey keyOf(
  const std::string& name,
  const int usage,
  const int cube = Cube2D,
  const int filter = FilterDefault,
  const int repeat = 0)
{
  auto key = MaterialImageKey{};
  key.name = name;
  key.usage = usage;
  key.filter = filter;
  key.repeat = repeat;
  key.cube = cube;
  return key;
}

std::pair<int, int> tgaSize(const std::filesystem::path& path)
{
  const auto bytes = readTextFile(path);
  REQUIRE(bytes.size() >= 18u);
  const auto le16 = [&](const std::size_t at) {
    return int(std::uint8_t(bytes[at])) | (int(std::uint8_t(bytes[at + 1])) << 8);
  };
  return {le16(12), le16(14)};
}

std::filesystem::path fixtureCaches()
{
  return litFixtureFolder() / "goldens" / "lp1c";
}

// the fixture images' caches and usages: the LP1b materials' stage images, and glass's
// editor image (TD_DIFFUSE, Material.cpp:261)
const std::vector<std::pair<std::string, int>>& fixtureCacheNames()
{
  static const auto names = std::vector<std::pair<std::string, int>>{
    {"crate#__0100", UsageDiffuse},
    {"floor#__0100", UsageDiffuse},
    {"glass#__0100", UsageDiffuse},
    {"glass#__0200", UsageDefault},
    {"grate#__0100", UsageDiffuse},
    {"grate#__1000", UsageCoverage},
    {"patch#__0100", UsageDiffuse},
    {"wall#__0100", UsageDiffuse},
  };
  return names;
}

// a DeclSource that counts what it reads, around a real one (A5)
class CountingSource : public DeclSource
{
private:
  DeclSource& m_inner;

public:
  int bodyReads = 0;
  int probes = 0;

  explicit CountingSource(DeclSource& inner)
    : m_inner{inner}
  {
  }

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override
  {
    return m_inner.listFiles(folder, extension);
  }
  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override
  {
    ++bodyReads;
    return m_inner.readFile(path, timestamp);
  }
  GameFileProbe probe(const std::string& path) override
  {
    ++probes;
    return m_inner.probe(path);
  }
  std::optional<std::string> readGameFile(
    const std::string& path, GameFileProbe* found) override
  {
    ++bodyReads;
    return m_inner.readGameFile(path, found);
  }
  bool usingResourceFiles() const override { return m_inner.usingResourceFiles(); }
  void closeFiles() override { m_inner.closeFiles(); }
};

/**
 * A game folder for the image caches. The sources (textures/lp1c), loose or in
 * containers, and caches of one name in several sources, so that each rule of the C8
 * search has a name:
 * - loose over .pk4 over .resources: all_three
 * - the last .pk4 registered (sorted) first: both_pk4
 * - the last base .resources registered first: both_res
 * - base/maps .resources over base .resources: maps_wins
 */
class ImageGame
{
private:
  fs::TestEnvironment m_env;

public:
  ImageGame()
  {
    const auto images = std::string{"generated/images/textures/lp1c/"};
    writeTextFile(base() / (images + "all_three#__0200.bimage"), dxt5Cache('L'));
    writePk4(
      base() / "aa_first.pk4",
      {
        {images + "all_three#__0200.bimage", dxt5Cache('P')},
        {images + "both_pk4#__0200.bimage", dxt5Cache('1')},
        {"textures/lp1c/packed.tga", "a packed source"},
      });
    writePk4(
      base() / "zz_second.pk4", {{images + "both_pk4#__0200.bimage", dxt5Cache('2')}});
    writeTextFile(
      base() / "_first.resources",
      resourcesBytes({
        {images + "all_three#__0200.bimage", dxt5Cache('R')},
        {images + "both_res#__0200.bimage", dxt5Cache('a')},
        {images + "maps_wins#__0200.bimage", dxt5Cache('b')},
      }));
    writeTextFile(
      base() / "second.resources",
      resourcesBytes({{images + "both_res#__0200.bimage", dxt5Cache('c')}}));
    writeTextFile(
      base() / "maps" / "map1.resources",
      resourcesBytes({{images + "maps_wins#__0200.bimage", dxt5Cache('m')}}));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }
  const std::filesystem::path& path() const { return m_env.dir(); }

  // a loose source file at a Unix time
  void source(const std::string& path, const std::int64_t time) const
  {
    writeTextFile(base() / path, "a source image's bytes");
    REQUIRE(setUnixModificationTime(base() / path, time));
  }
};

// the first data byte of a Ready slot's image
char tagOf(const ImageCacheSlot& slot)
{
  REQUIRE(slot.state == ImageCacheSlot::State::Ready);
  REQUIRE(slot.image);
  REQUIRE(!slot.image->data.empty());
  return char(slot.image->data.front());
}

} // namespace

TEST_CASE("ImageCache, the engine's options per usage (DeriveOpts, SC99)")
{
  SECTION("format and colour format")
  {
    const auto opts = [](const int usage) {
      const auto o = engineImageOpts(usage);
      return std::pair{o.format, o.colorFormat};
    };
    CHECK(opts(UsageDiffuse) == std::pair{FormatDxt5, ColorYCoCgDxt5});
    CHECK(opts(UsageSpecular) == std::pair{FormatDxt1, ColorDefault});
    CHECK(opts(UsageSpecularPbrRmao) == std::pair{FormatDxt1, ColorDefault});
    CHECK(opts(UsageDefault) == std::pair{FormatDxt5, ColorDefault});
    CHECK(opts(UsageBump) == std::pair{FormatDxt5, ColorNormalDxt5});
    CHECK(opts(UsageLight) == std::pair{FormatRgb565, ColorDefault});
    CHECK(opts(UsageCoverage) == std::pair{FormatDxt1, ColorGreenAlpha});
    CHECK(opts(UsageHdrLightProbe) == std::pair{FormatBc6h, ColorDefault});
    CHECK(opts(UsageHdri) == std::pair{FormatBc6h, ColorDefault});
  }

  SECTION("the mip count stops at a block-compressed level that is not a multiple of 4")
  {
    // the fixture images' own counts below agree
    CHECK(engineMipLevels(FormatDxt5, FilterDefault, 64, 32) == 4);
    CHECK(engineMipLevels(FormatDxt5, FilterDefault, 16, 16) == 3);
    CHECK(engineMipLevels(FormatDxt5, FilterDefault, 64, 16) == 3);
    CHECK(engineMipLevels(FormatRgba8, FilterDefault, 64, 16) == 7);
    CHECK(engineMipLevels(FormatDxt5, FilterLinear, 64, 64) == 1);
    CHECK(engineMipLevels(FormatDxt5, FilterNearest, 64, 64) == 1);
  }

  SECTION("a cache matches the usage's format, colour format and type")
  {
    auto image = CachedImage{};
    image.textureType = Type2D;
    image.format = FormatDxt5;
    image.colorFormat = ColorYCoCgDxt5;
    CHECK(engineCacheMatches(image, UsageDiffuse, Type2D));
    CHECK_FALSE(engineCacheMatches(image, UsageDefault, Type2D));
    CHECK_FALSE(engineCacheMatches(image, UsageDiffuse, TypeCubic));
    // RGB565 is read as RGBA8; R11G11B10F stands for BC6H
    image.format = FormatRgba8;
    image.colorFormat = ColorDefault;
    CHECK(engineCacheMatches(image, UsageLight, Type2D));
    image.format = FormatR11g11b10f;
    CHECK(engineCacheMatches(image, UsageHdrLightProbe, Type2D));
  }

  SECTION("staleness: only a real source time that differs rejects (A3)")
  {
    CHECK_FALSE(engineCacheIsStale(1000, 1000));
    CHECK_FALSE(engineCacheIsStale(0, 1000));
    CHECK_FALSE(engineCacheIsStale(-1, 1000));
    CHECK(engineCacheIsStale(1001, 1000));
  }
}

TEST_CASE("ImageCache, the engine-written caches read back exactly (D61)")
{
  const auto folder = fixtureCaches() / "generated" / "images" / "textures" / "lp1b";
  for (const auto& [name, usage] : fixtureCacheNames())
  {
    CAPTURE(name);
    const auto bytes = readTextFile(folder / (name + ".bimage"));
    const auto image = readBimage(bytes) | kdl::value();
    const auto source = name.substr(0, name.find('#'));
    const auto [width, height] =
      tgaSize(litFixtureFolder() / "textures" / "lp1b" / (source + ".tga"));

    // the header: BIM version 11, 2D, DeriveOpts' format for the usage digits
    CHECK(image.magic == BimageMagic);
    CHECK(image.sourceTime == FixtureSourceTime);
    CHECK(image.textureType == Type2D);
    CHECK(image.storedFormat == engineImageOpts(usage).format);
    CHECK(image.format == image.storedFormat);
    CHECK(image.colorFormat == engineImageOpts(usage).colorFormat);
    CHECK(image.width == width);
    CHECK(image.height == height);
    CHECK(image.numLevels == engineMipLevels(image.format, FilterDefault, width, height));
    CHECK(engineCacheMatches(image, usage, Type2D));

    // each level: its size halved, its bytes in 4×4 blocks of 8 (DXT1) or 16 (DXT5)
    REQUIRE(image.levels.size() == std::size_t(image.numLevels));
    auto total = std::size_t{36};
    const auto blockBytes = image.format == FormatDxt1 ? 8 : 16;
    for (auto i = 0; i < image.numLevels; ++i)
    {
      const auto& level = image.levels[std::size_t(i)];
      CHECK(level.level == i);
      CHECK(level.destZ == 0);
      CHECK(level.width == std::max(1, width >> i));
      CHECK(level.height == std::max(1, height >> i));
      CHECK(
        level.size
        == std::size_t(((level.width + 3) / 4) * ((level.height + 3) / 4) * blockBytes));
      total += 20 + level.size;
    }
    // every byte of the file is read, as the engine reads it
    CHECK(total == bytes.size());
    CHECK(image.data.size() == total - 36 - 20 * image.levels.size());
  }
}

TEST_CASE("ImageCache, the .bimage layout")
{
  SECTION(
    "RGB565 is stored high byte first and expanded to RGBA8 as the engine expands it")
  {
    // red, green and a middle grey, then one pixel of the 1×1 level
    const auto bytes = bimageBytes(
      5,
      Type2D,
      FormatRgb565,
      ColorDefault,
      3,
      1,
      2,
      {
        {0, 0, 3, 1, std::string{"\xF8\x00\x07\xE0\x84\x10", 6}},
        {1, 0, 1, 1, std::string{"\x00\x1F", 2}},
      });
    const auto image = readBimage(bytes) | kdl::value();
    CHECK(image.storedFormat == FormatRgb565);
    CHECK(image.format == FormatRgba8);
    REQUIRE(image.levels.size() == 2u);
    CHECK(image.levels[0].size == 12u);
    CHECK(image.levels[1].offset == 12u);
    CHECK(image.levels[1].size == 4u);
    CHECK(
      image.data
      == std::vector<std::uint8_t>{
        255, 0, 0, 255, 0, 255, 0, 255, 132, 130, 132, 255, 0, 0, 255, 255});
    CHECK(engineCacheMatches(image, UsageLight, Type2D));
    CHECK_FALSE(engineCacheMatches(image, UsageDefault, Type2D));
  }

  SECTION("a cube has six records per level, level-major (images[ level * 6 + side ])")
  {
    auto levels = std::vector<Level>{};
    for (auto side = 0; side < 6; ++side)
    {
      levels.push_back({0, side, 4, 4, std::string(16, char('0' + side))});
    }
    const auto image =
      readBimage(bimageBytes(0, TypeCubic, FormatBc6h, ColorDefault, 4, 4, 1, levels))
      | kdl::value();
    REQUIRE(image.levels.size() == 6u);
    CHECK(image.levels[5].destZ == 5);
    CHECK(image.data[image.levels[5].offset] == std::uint8_t('5'));
  }

  SECTION("BIM version 10 reads too; anything else, or a cut file, does not")
  {
    CHECK(readBimage(
            bimageBytes(
              0, Type2D, FormatRgba8, 0, 1, 1, 1, {{0, 0, 1, 1, "abcd"}}, BimageMagicBfg))
            .is_success());
    CHECK(readBimage(
            bimageBytes(
              0, Type2D, FormatRgba8, 0, 1, 1, 1, {{0, 0, 1, 1, "abcd"}}, 0x12345678u))
            .is_error());
    const auto good = dxt5Cache('x');
    for (const auto size :
         {std::size_t{0}, std::size_t{35}, std::size_t{40}, good.size() - 1})
    {
      CAPTURE(size);
      CHECK(readBimage(good.substr(0, size)).is_error());
    }
    // a negative level count, an odd RGB565 level, a negative data size
    CHECK(readBimage(bimageBytes(0, Type2D, FormatDxt5, 0, 4, 4, -1, {})).is_error());
    CHECK(
      readBimage(bimageBytes(0, Type2D, FormatRgb565, 0, 1, 1, 1, {{0, 0, 1, 1, "abc"}}))
        .is_error());
    auto negative = bimageBytes(0, Type2D, FormatRgba8, 0, 1, 1, 1, {{0, 0, 1, 1, ""}});
    negative[36 + 16] = static_cast<char>(-1);
    CHECK(readBimage(negative).is_error());
  }

  SECTION("the level records must be the header's (S3.5)")
  {
    const auto problem = [](const std::string& bytes) {
      return readBimage(bytes) | kdl::transform([](const auto&) { return std::string{}; })
             | kdl::transform_error([](const auto& e) { return e.msg; }) | kdl::value();
    };
    // no level, and more levels than a side halves into
    CHECK_THAT(
      problem(bimageBytes(0, Type2D, FormatRgba8, 0, 1, 1, 0, {})),
      Catch::Matchers::ContainsSubstring("bad level count 0"));
    auto many = std::vector<Level>{};
    for (auto i = 0; i < 200; ++i)
    {
      many.push_back({i, 0, 1, 1, "abcd"});
    }
    CHECK_THAT(
      problem(bimageBytes(0, Type2D, FormatRgba8, 0, 1, 1, 200, many)),
      Catch::Matchers::ContainsSubstring("bad level count 200"));
    // a header beyond the limits, over small levels
    CHECK_THAT(
      problem(
        bimageBytes(0, Type2D, FormatRgba8, 0, 65536, 65536, 1, {{0, 0, 1, 1, "abcd"}})),
      Catch::Matchers::ContainsSubstring("bad size"));
    // a level of fewer bytes than its size needs: 8 × 8 DXT5 is four blocks
    CHECK_THAT(
      problem(bimageBytes(
        0, Type2D, FormatDxt5, 0, 8, 8, 1, {{0, 0, 8, 8, std::string(32, 'x')}})),
      Catch::Matchers::ContainsSubstring("fewer than the 64"));
    // a record of another level, or of another size
    CHECK_THAT(
      problem(bimageBytes(
        0,
        Type2D,
        FormatRgba8,
        0,
        2,
        2,
        2,
        {{0, 0, 2, 2, std::string(16, 'x')}, {0, 0, 1, 1, "abcd"}})),
      Catch::Matchers::ContainsSubstring("is level 0 side 0, not level 1 side 0"));
    CHECK_THAT(
      problem(bimageBytes(
        0, Type2D, FormatRgba8, 0, 2, 2, 1, {{0, 0, 4, 4, std::string(64, 'x')}})),
      Catch::Matchers::ContainsSubstring("is 4 x 4, not 2 x 2"));
    // a cube with five faces' records
    auto five = std::vector<Level>{};
    for (auto side = 0; side < 5; ++side)
    {
      five.push_back({0, side, 4, 4, std::string(16, 'x')});
    }
    CHECK_THAT(
      problem(bimageBytes(0, TypeCubic, FormatBc6h, ColorDefault, 4, 4, 1, five)),
      Catch::Matchers::ContainsSubstring("level record 5 is cut short"));
    // a cube's sides in another order
    auto swapped = std::vector<Level>{};
    for (auto side = 0; side < 6; ++side)
    {
      swapped.push_back({0, 5 - side, 4, 4, std::string(16, 'x')});
    }
    CHECK_THAT(
      problem(bimageBytes(0, TypeCubic, FormatBc6h, ColorDefault, 4, 4, 1, swapped)),
      Catch::Matchers::ContainsSubstring("not level 0 side 0"));
    // a block format's cube level below 4 is padded to 4 (PadImageTo4x4)
    auto padded = std::vector<Level>{};
    for (auto level = 0; level < 2; ++level)
    {
      for (auto side = 0; side < 6; ++side)
      {
        padded.push_back({level, side, 4, 4, std::string(16, 'x')});
      }
    }
    CHECK(problem(bimageBytes(0, TypeCubic, FormatDxt5, ColorDefault, 4, 4, 2, padded))
            .empty());
  }
}

TEST_CASE("ImageCache, the cache name of an image (C8, R5.5)")
{
  const auto game = ImageGame{};
  game.source("textures/lp1c/plain.tga", 1700000100);
  game.source("textures/lp1c/a_local.tga", 1700000200);
  game.source("textures/lp1c/a_h.tga", 1700000300);
  game.source("textures/lp1c/metal_s.tga", 1700000400);
  game.source("textures/lp1c/metal_rmao.png", 1700000500);
  game.source("textures/lp1c/other_s.tga", 1700000600);
  game.source("textures/lp1c/other_rmao.tga", 1700000700);
  game.source("textures/lp1c/plain_s.tga", 1700000800);
  for (const auto* side : {"_px", "_nx", "_py", "_ny", "_pz", "_nz"})
  {
    game.source(std::string{"env/lp1c/sky"} + side + ".tga", 1700000900);
  }
  game.source("env/lp1c/sky_nz.tga", 1700000950);

  auto files = GameFileSource{game.path()};
  auto source = CountingSource{files};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto nameOf = [&](const MaterialImageKey& key) {
    auto result = decls.imageCacheName(key);
    INFO(result.error);
    REQUIRE(result);
    return *result.value;
  };

  SECTION("the usage and cube digits before the extension (GetGeneratedName, SC96)")
  {
    const auto name = nameOf(keyOf("textures/lp1c/plain", UsageDiffuse));
    CHECK(name.generatedName == "textures/lp1c/plain#__0100");
    CHECK(name.file == "generated/images/textures/lp1c/plain#__0100.bimage");
    CHECK(name.textureType == Type2D);
    CHECK_FALSE(name.generated);
    // an extension the record kept stays after the digits
    CHECK(
      nameOf(keyOf("textures/lp1c/plain.x", UsageBump)).file
      == "generated/images/textures/lp1c/plain#__0300.x.bimage");
  }

  SECTION("an image program's ( and , become folders, ) and spaces go (SC96)")
  {
    const auto name = nameOf(keyOf(
      "addnormals(textures/lp1c/a_local, heightmap(textures/lp1c/a_h, 4))", UsageBump));
    CHECK(
      name.file
      == "generated/images/addnormals/textures/lp1c/a_local/heightmap/textures/lp1c/"
         "a_h/4#__0300.bimage");
    // the program's time is its newest leaf's (Image_program.cpp:815-821)
    CHECK(name.sourceTime == 1700000300);
  }

  SECTION("a loose source's time in Unix seconds, a missing one 0, a container's 0 (A4)")
  {
    CHECK(nameOf(keyOf("textures/lp1c/plain", UsageDiffuse)).sourceTime == 1700000100);
    CHECK(nameOf(keyOf("textures/lp1c/none", UsageDiffuse)).sourceTime == 0);
    CHECK(nameOf(keyOf("textures/lp1c/packed", UsageDiffuse)).sourceTime == 0);
  }

  SECTION(
    "a specular <base>_s with a <base>_rmao.png sibling is RMAO, named _rmao (SC94)")
  {
    const auto name = nameOf(keyOf("textures/lp1c/metal_s", UsageSpecular));
    CHECK(name.usage == UsageSpecularPbrRmao);
    CHECK(name.imageName == "textures/lp1c/metal_rmao");
    CHECK(name.generatedName == "textures/lp1c/metal_rmao#__1200");
    // the time is the sibling's, which the call found
    CHECK(name.sourceTime == 1700000500);
  }

  SECTION("an _rmao sibling of another loader's extension counts too")
  {
    const auto name = nameOf(keyOf("textures/lp1c/other_s", UsageSpecular));
    CHECK(name.generatedName == "textures/lp1c/other_rmao#__1200");
    CHECK(name.sourceTime == 1700000700);
  }

  SECTION("a specular _s without a sibling, or another usage, keeps its name")
  {
    const auto plain = nameOf(keyOf("textures/lp1c/plain_s", UsageSpecular));
    CHECK(plain.usage == UsageSpecular);
    CHECK(plain.generatedName == "textures/lp1c/plain_s#__0000");
    CHECK(plain.sourceTime == 1700000800);
    CHECK(
      nameOf(keyOf("textures/lp1c/metal_s", UsageDiffuse)).generatedName
      == "textures/lp1c/metal_s#__0100");
  }

  SECTION("a cube's time is its sides' newest; CF_2D_ARRAY makes no call (SC92)")
  {
    const auto cube = nameOf(keyOf("env/lp1c/sky", UsageDefault, 1));
    CHECK(cube.textureType == TypeCubic);
    CHECK(cube.sourceTime == 1700000950);
    CHECK(cube.generatedName == "env/lp1c/sky#__0201");
    const auto array = nameOf(keyOf("textures/lp1c/plain", UsageDefault, 4));
    CHECK(array.textureType == Type2DArray);
    CHECK(array.sourceTime == -1);
  }

  SECTION("an intrinsic or the default image has no cache")
  {
    auto key = keyOf("_white", UsageDefault);
    key.intrinsic = true;
    CHECK(nameOf(key).generated);
    key = keyOf("_default", UsageDefault);
    key.isDefault = true;
    CHECK(nameOf(key).generated);
  }

  SECTION("the BFHACK names (SC100)")
  {
    CHECK(
      nameOf(keyOf("guis/assets/white", UsageSpecular)).bfhackFile
      == "generated/images/guis/assets/white#__0200.bimage");
    CHECK(
      nameOf(keyOf("guis/assets/white", UsageDiffuse)).bfhackFile
      == "generated/images/guis/assets/white#__0200.bimage");
    CHECK(
      nameOf(keyOf("textures/black", UsageDiffuse)).bfhackFile
      == "generated/images/textures/black#__0200.bimage");
    CHECK(
      nameOf(keyOf("textures/decals/bulletglass1_d", UsageDiffuse)).bfhackFile
      == "generated/images/textures/decals/bulletglass1_d#__0200.bimage");
    CHECK(
      nameOf(keyOf("models/monsters/skeleton/skeleton01_d", UsageCoverage)).bfhackFile
      == "generated/images/models/monsters/skeleton/skeleton01_d#__0100.bimage");
    CHECK(nameOf(keyOf("textures/black", UsageDefault)).bfhackFile.empty());
  }

  SECTION("the timestamp call reads no file body (A5)")
  {
    source.bodyReads = 0;
    source.probes = 0;
    static_cast<void>(nameOf(keyOf("textures/lp1c/metal_s", UsageSpecular)));
    static_cast<void>(nameOf(keyOf("textures/lp1c/packed", UsageDiffuse)));
    static_cast<void>(nameOf(keyOf("env/lp1c/sky", UsageDefault, 1)));
    CHECK(source.bodyReads == 0);
    CHECK(source.probes > 0);
    CHECK(files.openContainers() == 0u);
  }
}

TEST_CASE("ImageCache, the cache reader (C8, A3, D23)")
{
  const auto game = ImageGame{};
  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};
  const auto resolve = [&](const MaterialImageKey& key) {
    // the files the sections write are seen as the service's poll sees them (S2.2)
    source.looseFilesMayHaveChanged();
    auto name = decls.imageCacheName(key);
    REQUIRE(name);
    return resolveImageCache(source, *name.value);
  };

  SECTION("each kind of cache from its last registered container, loose first")
  {
    CHECK(tagOf(resolve(keyOf("textures/lp1c/all_three", UsageDefault))) == 'L');
    CHECK(tagOf(resolve(keyOf("textures/lp1c/both_pk4", UsageDefault))) == '2');
    CHECK(tagOf(resolve(keyOf("textures/lp1c/both_res", UsageDefault))) == 'c');
    CHECK(tagOf(resolve(keyOf("textures/lp1c/maps_wins", UsageDefault))) == 'm');

    const auto slot = resolve(keyOf("textures/lp1c/all_three", UsageDefault));
    REQUIRE(tagOf(slot) == 'L');
    CHECK(slot.file == "generated/images/textures/lp1c/all_three#__0200.bimage");
    CHECK(slot.image->origin.kind == GameFileProbe::Kind::Loose);
    std::filesystem::remove(
      game.base() / "generated" / "images" / "textures" / "lp1c"
      / "all_three#__0200.bimage");
    const auto packed = resolve(keyOf("textures/lp1c/all_three", UsageDefault));
    REQUIRE(tagOf(packed) == 'P');
    CHECK(packed.image->origin.container == "aa_first.pk4");
  }

  SECTION("no cache is Missing; a cache of another format is Stale")
  {
    const auto missing = resolve(keyOf("textures/lp1c/none", UsageDefault));
    CHECK(missing.state == ImageCacheSlot::State::Missing);
    CHECK_THAT(
      missing.reason, Catch::Matchers::ContainsSubstring("binary file not found"));
    // the DXT5 cache of a TD_DEFAULT name read for TD_DIFFUSE: same digits are not asked,
    // so the name is made to clash on purpose
    writeTextFile(
      game.base() / "generated" / "images" / "textures" / "lp1c" / "clash#__0100.bimage",
      dxt5Cache('x'));
    const auto clash = resolve(keyOf("textures/lp1c/clash", UsageDiffuse));
    CHECK(clash.state == ImageCacheSlot::State::Stale);
    CHECK_THAT(
      clash.reason, Catch::Matchers::ContainsSubstring("mismatched color format"));

    // S3.5: a cache whose records are not its header's is Stale with the reason
    writeTextFile(
      game.base() / "generated" / "images" / "textures" / "lp1c"
        / "records#__0200.bimage",
      bimageBytes(0, Type2D, FormatDxt5, ColorDefault, 4, 4, 0, {}));
    const auto records = resolve(keyOf("textures/lp1c/records", UsageDefault));
    CHECK(records.state == ImageCacheSlot::State::Stale);
    CHECK_FALSE(records.image);
    CHECK_THAT(records.reason, Catch::Matchers::ContainsSubstring("bad level count"));
  }

  SECTION("staleness: an equal time accepts, 0 accepts, a different time rejects (A3)")
  {
    const auto cache =
      game.base() / "generated" / "images" / "textures" / "lp1c" / "timed#__0200.bimage";
    writeTextFile(cache, dxt5Cache('t', 1700001000));
    // no source: 0
    CHECK(tagOf(resolve(keyOf("textures/lp1c/timed", UsageDefault))) == 't');
    game.source("textures/lp1c/timed.tga", 1700001000);
    CHECK(tagOf(resolve(keyOf("textures/lp1c/timed", UsageDefault))) == 't');
    REQUIRE(setUnixModificationTime(game.base() / "textures/lp1c/timed.tga", 1700001001));
    const auto stale = resolve(keyOf("textures/lp1c/timed", UsageDefault));
    CHECK(stale.state == ImageCacheSlot::State::Stale);
    CHECK(stale.name.sourceTime == 1700001001);
    CHECK_THAT(stale.reason, Catch::Matchers::ContainsSubstring("differs"));
  }

  SECTION("a cut cache is Stale, never read past its end")
  {
    const auto good = dxt5Cache('c');
    writeTextFile(
      game.base() / "generated" / "images" / "textures" / "lp1c" / "cut#__0200.bimage",
      good.substr(0, good.size() - 3));
    const auto cut = resolve(keyOf("textures/lp1c/cut", UsageDefault));
    CHECK(cut.state == ImageCacheSlot::State::Stale);
    CHECK_THAT(cut.reason, Catch::Matchers::ContainsSubstring("cut short"));
  }

  SECTION("the BFHACK name is tried when resource files are in use (SC100)")
  {
    writeTextFile(
      game.base() / "generated" / "images" / "textures" / "black#__0200.bimage",
      dxt5Cache('B'));
    REQUIRE(source.usingResourceFiles());
    const auto slot = resolve(keyOf("textures/black", UsageDiffuse));
    CHECK(slot.file == "generated/images/textures/black#__0200.bimage");
    // the BFHACK cache is TD_DEFAULT's, which DeriveOpts for TD_DIFFUSE does not match:
    // the engine binarizes it as well
    CHECK(slot.state == ImageCacheSlot::State::Stale);

    // without resource files the name is not tried
    const auto env = fs::TestEnvironment{};
    writeTextFile(
      env.dir() / "base" / "generated" / "images" / "textures" / "black#__0200.bimage",
      dxt5Cache('B'));
    auto plain = GameFileSource{env.dir()};
    REQUIRE_FALSE(plain.usingResourceFiles());
    auto name = decls.imageCacheName(keyOf("textures/black", UsageDiffuse));
    REQUIRE(name);
    CHECK(resolveImageCache(plain, *name.value).state == ImageCacheSlot::State::Missing);
  }
}

TEST_CASE("ImageCache, the fixture materials' images resolve to the engine's caches")
{
  // the LP1b fixture as the capture staged it: its materials, its images at the staged
  // time, and the caches the engine wrote for them
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  writeTextFile(
    base / "materials" / "lp1bc.mtr",
    readTextFile(litFixtureFolder() / "materials" / "lp1bc.mtr"));
  for (const auto* name : {"crate", "floor", "glass", "grate", "patch", "wall"})
  {
    const auto tga = std::string{name} + ".tga";
    const auto path = base / "textures" / "lp1b" / tga;
    writeTextFile(path, readTextFile(litFixtureFolder() / "textures" / "lp1b" / tga));
    REQUIRE(setUnixModificationTime(path, FixtureSourceTime));
  }
  std::filesystem::copy(
    fixtureCaches() / "generated",
    base / "generated",
    std::filesystem::copy_options::recursive);

  auto source = GameFileSource{env.dir()};
  auto decls = MaterialDecls{source};
  auto found = std::map<std::string, std::pair<int, int>>{};
  auto generated = 0;
  for (const auto* material :
       {"textures/lp1b/floor",
        "textures/lp1b/wall",
        "textures/lp1b/crate",
        "textures/lp1b/grate",
        "textures/lp1b/glass",
        "textures/lp1b/patch"})
  {
    CAPTURE(material);
    const auto entry = decls.lookup(material);
    REQUIRE(entry);
    for (const auto& stage : (*entry.value)->stages)
    {
      REQUIRE(stage.image);
      const auto name = decls.imageCacheName(*stage.image);
      REQUIRE(name);
      const auto slot = resolveImageCache(source, *name.value);
      CAPTURE(stage.image->name, name.value->file, slot.reason);
      if (name.value->generated)
      {
        // the flat bump stage the parser adds to a lit material: an intrinsic, no cache
        CHECK(stage.image->intrinsic);
        CHECK(slot.state == ImageCacheSlot::State::Missing);
        ++generated;
        continue;
      }
      REQUIRE(slot.state == ImageCacheSlot::State::Ready);
      found[name.value->generatedName] = {slot.image->width, slot.image->height};
    }
  }
  // every stage image of the six materials: the diffuse ones, grate's coverage map and
  // glass's blended stage (its #__0100 is the editor image, which the preview never asks)
  CHECK(
    found
    == std::map<std::string, std::pair<int, int>>{
      {"textures/lp1b/crate#__0100", {32, 32}},
      {"textures/lp1b/floor#__0100", {64, 32}},
      {"textures/lp1b/glass#__0200", {16, 16}},
      {"textures/lp1b/grate#__0100", {32, 32}},
      {"textures/lp1b/grate#__1000", {32, 32}},
      {"textures/lp1b/patch#__0100", {64, 16}},
      {"textures/lp1b/wall#__0100", {32, 64}},
    });
  CHECK(generated > 0);

  // a source saved after the capture makes the cache stale (A3)
  REQUIRE(setUnixModificationTime(
    base / "textures" / "lp1b" / "wall.tga", FixtureSourceTime + 1));
  const auto wall = decls.lookup("textures/lp1b/wall");
  REQUIRE(wall);
  const auto& stages = (*wall.value)->stages;
  const auto diffuse = std::find_if(
    stages.begin(), stages.end(), [](const auto& stage) { return stage.lighting == 2; });
  REQUIRE(diffuse != stages.end());
  const auto name = decls.imageCacheName(*diffuse->image);
  REQUIRE(name);
  CHECK(name.value->generatedName == "textures/lp1b/wall#__0100");
  CHECK(resolveImageCache(source, *name.value).state == ImageCacheSlot::State::Stale);
}

TEST_CASE("MaterialService serves image caches on the parse thread (C8, C20)")
{
  const auto game = ImageGame{};
  auto options = MaterialServiceOptions{};
  options.makeSource = [path = game.path()](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();

  const auto ready = keyOf("textures/lp1c/both_pk4", UsageDefault);
  const auto missing = keyOf("textures/lp1c/none", UsageDefault);
  CHECK(service->image(ready).state == ImageLookup::State::Pending);
  CHECK(service->image(missing).state == ImageLookup::State::Pending);
  const auto table = service->waitForTableForTesting(
    [&](const MaterialTable& t) { return t.findImage(ready) && t.findImage(missing); },
    10s);
  REQUIRE(table->findImage(ready));

  const auto found = service->image(ready);
  CHECK(found.state == ImageLookup::State::Ready);
  REQUIRE(found.image);
  CHECK(found.image->data.front() == std::uint8_t('2'));
  // no cache and no source (LP1c.2): the default image, the cache Missing
  const auto none = service->image(missing);
  CHECK(none.state == ImageLookup::State::Default);
  REQUIRE(none.slot);
  CHECK(none.slot->state == ImageCacheSlot::State::Missing);
  CHECK_THAT(
    none.slot->sourceReason, Catch::Matchers::ContainsSubstring("Couldn't load"));

  // LP1c fix (S2.5): a reload drops the image slots, so that the new source's containers
  // and caches are seen; asked again, the image is read again, alike
  service->reload();
  const auto reloaded = service->waitForTableForTesting(
    [&](const MaterialTable& t) { return t.registry != table->registry; }, 10s);
  CHECK_FALSE(reloaded->findImage(ready));
  CHECK(service->image(ready).state == ImageLookup::State::Pending);
  const auto again = service->waitForTableForTesting(
    [&](const MaterialTable& t) { return t.findImage(ready) != nullptr; }, 10s);
  REQUIRE(again->findImage(ready));
  REQUIRE(again->findImage(ready)->image);
  CHECK(again->findImage(ready)->image->data == table->findImage(ready)->image->data);
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
