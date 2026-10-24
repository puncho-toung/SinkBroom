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

// LPF.0a (C39, C40, C41, D96-D103, D111): the material service's image override: the
// Feed work kind and its counters, the matching, the conversion and the store with its
// one publication per hand-over. CPU only: the render side is LPF.1's.

#include "ImageTestFiles.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/EngineImageEnums.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/ImageOverride.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Timeout = 30s;

std::unique_ptr<DeclSource> fixtureSource(const std::atomic<bool>&)
{
  return std::make_unique<MemoryDeclSource>(materialDeclsFixture());
}

MaterialServiceOptions options(
  std::function<std::unique_ptr<DeclSource>(const std::atomic<bool>&)> makeSource)
{
  auto result = MaterialServiceOptions{};
  result.makeSource = std::move(makeSource);
  result.pollInterval = 0ms;
  return result;
}

std::unique_ptr<MaterialService> start(MaterialServiceOptions serviceOptions)
{
  auto service = MaterialService::create(std::move(serviceOptions));
  REQUIRE(service.is_success());
  return std::move(service).value();
}

// holds the parse thread before it takes its next request, and tells when it is held
struct Hold
{
  std::mutex mutex;
  std::condition_variable changed;
  bool closed = false;
  bool held = false;

  void pass()
  {
    auto lock = std::unique_lock{mutex};
    held = true;
    changed.notify_all();
    changed.wait(lock, [&] { return !closed; });
    held = false;
  }

  void close()
  {
    const auto lock = std::scoped_lock{mutex};
    closed = true;
  }

  bool waitUntilHeld()
  {
    auto lock = std::unique_lock{mutex};
    return changed.wait_for(lock, Timeout, [&] { return held; });
  }

  void open()
  {
    {
      const auto lock = std::scoped_lock{mutex};
      closed = false;
    }
    changed.notify_all();
  }
};

// opens the hold when the test leaves, so that the service's join never waits for it
struct OpenHoldAtExit
{
  std::shared_ptr<Hold> hold;
  ~OpenHoldAtExit() { hold->open(); }
};

// the work items the parse thread ran, in order
struct Kinds
{
  std::mutex mutex;
  std::vector<std::string> kinds;

  void add(const std::string_view kind)
  {
    const auto lock = std::scoped_lock{mutex};
    kinds.emplace_back(kind);
  }

  std::vector<std::string> taken()
  {
    const auto lock = std::scoped_lock{mutex};
    return std::exchange(kinds, {});
  }
};

// a w × h RGBA8 picture of one colour
RawImagePixels rgba8(
  const int width,
  const int height,
  const std::uint8_t r,
  const std::uint8_t g,
  const std::uint8_t b,
  const std::uint8_t a = 255,
  const int faces = 1)
{
  auto pixels = RawImagePixels{};
  pixels.width = width;
  pixels.height = height;
  pixels.faces = faces;
  pixels.format = ImagePixelFormat::RGBA8;
  // one texel, then doubled: the large ones are tens of megabytes
  const auto size = std::size_t(width) * std::size_t(height) * std::size_t(faces) * 4;
  pixels.bytes.resize(size);
  const auto texel = std::array<std::uint8_t, 4>{r, g, b, a};
  std::memcpy(pixels.bytes.data(), texel.data(), std::min(size, texel.size()));
  for (auto filled = std::min(size, texel.size()); filled < size; filled *= 2)
  {
    std::memcpy(
      pixels.bytes.data() + filled, pixels.bytes.data(), std::min(filled, size - filled));
  }
  return pixels;
}

ImageOverride overrideOf(RawImagePixels pixels)
{
  auto image = ImageOverride{};
  image.image = std::move(pixels);
  return image;
}

std::shared_ptr<const MaterialTable> waitForFeeds(MaterialService& service)
{
  const auto requested = service.feedRequested();
  return service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.feedApplied >= requested; }, Timeout);
}

std::shared_ptr<const MaterialEntry> waitForEntry(
  MaterialService& service, const std::string& name)
{
  static_cast<void>(service.lookup(name));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.find(name);
      return slot && (!slot->entry || slot->sizeMeasured);
    },
    Timeout);
  const auto slot = table->find(name);
  REQUIRE(slot);
  REQUIRE(slot->entry);
  return slot->entry;
}

bool waitUntilIdle(MaterialService& service)
{
  return service.waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout);
}

/**
 * Holds the parse thread before its next request: the hold closes, then an evaluation
 * the table lacks wakes the thread, which runs it and is held when it comes back for
 * the next one.
 */
void holdThread(MaterialService& service, Hold& hold, const MaterialEntry& entry)
{
  static auto parm = 0.0f;
  hold.close();
  auto parms = ShaderParms{};
  parm += 1.0f;
  parms[0] = parm;
  static_cast<void>(service.evaluate(MaterialId::of(entry), parms, 0.0f));
  REQUIRE(hold.waitUntilHeld());
}

} // namespace

TEST_CASE("feedKeyName folds a name as ImageFromFile does (C39, P5.1)")
{
  // ImageManager.cpp:418-425 and :435: the extensions anywhere, backslashes, no case
  CHECK(feedKeyName("TEXTURES/Foo.tga") == "textures/foo");
  CHECK(feedKeyName("textures\\lp1c\\Foo.png") == "textures/lp1c/foo");
  CHECK(feedKeyName("textures/foo.jpg.exr") == "textures/foo");
  // idStr::Replace compares with case: an upper-case extension stays, as in the engine,
  // whose record of "TEXTURES/Foo.TGA" is not the record of textures/foo
  CHECK(feedKeyName("TEXTURES/Foo.TGA") == "textures/foo.tga");
  // a program's leaves are folded inside its text, which stays whole
  CHECK(
    feedKeyName(
      "addnormals ( textures/a_local.tga , heightmap ( textures/a_h.tga , 4 ) )")
    == "addnormals ( textures/a_local , heightmap ( textures/a_h , 4 ) )");

  auto key = MaterialImageKey{};
  key.name = "Textures/LP1C/Foo";
  CHECK(feedMatchName(key) == feedKeyName("TEXTURES/lp1c/foo.tga"));

  CHECK(feedNameRefusal("textures/foo").empty());
  CHECK_FALSE(feedNameRefusal("").empty());
  CHECK_FALSE(feedNameRefusal("default").empty());
  CHECK_FALSE(feedNameRefusal("_flat").empty());
}

TEST_CASE("MaterialService image overrides, the built-ins are refused at hand-over")
{
  auto service = start(options(fixtureSource));
  for (const auto* name : {"_default", "_flat", "_white", "_FLAT.tga", "default"})
  {
    CAPTURE(name);
    const auto handed = service->setImageOverride(name, overrideOf(rgba8(2, 2, 1, 2, 3)));
    REQUIRE(handed.is_error());
    CHECK_THAT(
      std::get<Error>(handed.error()).msg, Catch::Matchers::ContainsSubstring("refused"));
  }
  CHECK(service->feedRequested() == 0u);
  CHECK(service->imageOverrideStatus("_flat").state == ImageOverrideStatus::State::None);
}

TEST_CASE("MaterialService image overrides, the Feed work kind (D96, D100)")
{
  auto hold = std::make_shared<Hold>();
  auto kinds = std::make_shared<Kinds>();
  auto serviceOptions = options(fixtureSource);
  serviceOptions.beforeTakeWorkForTesting = [hold] { hold->pass(); };
  // the image hook a section sets, called on the parse thread before each item
  auto onItem = std::make_shared<std::function<void(std::string_view)>>();
  serviceOptions.beforeWorkItemForTesting = [kinds, onItem](const std::string_view kind) {
    kinds->add(kind);
    if (*onItem)
    {
      (*onItem)(kind);
    }
  };
  auto service = start(std::move(serviceOptions));
  const auto openAtExit = OpenHoldAtExit{hold};
  const auto entry = waitForEntry(*service, "textures/lp1a/wall");
  REQUIRE(waitUntilIdle(*service));

  SECTION("served after the evaluations and ahead of the lookups")
  {
    holdThread(*service, *hold, *entry);
    static_cast<void>(kinds->taken());
    static_cast<void>(service->lookup("textures/lp1a/expr"));
    REQUIRE(service->setImageOverride("textures/feed/a", overrideOf(rgba8(4, 4, 1, 2, 3)))
              .is_success());
    auto parms = ShaderParms{};
    parms[1] = 0.5f;
    static_cast<void>(service->evaluate(MaterialId::of(*entry), parms, 0.0f));
    hold->open();
    waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    const auto ran = kinds->taken();
    REQUIRE(ran.size() >= 3u);
    CHECK(ran[0] == "evaluate");
    CHECK(ran[1] == "feed");
    CHECK(ran[2] == "lookup");
  }

  SECTION("one item per name: the newest hand-over wins, the older is dropped")
  {
    holdThread(*service, *hold, *entry);
    static_cast<void>(kinds->taken());
    const auto before = service->feedRequested();
    REQUIRE(service->setImageOverride("textures/feed/b", overrideOf(rgba8(4, 4, 1, 2, 3)))
              .is_success());
    REQUIRE(service->setImageOverride("textures/feed/b", overrideOf(rgba8(4, 4, 4, 5, 6)))
              .is_success());
    CHECK(service->feedRequested() == before + 2);
    hold->open();
    const auto table = waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    CHECK(table->feedApplied == service->feedRequested());
    CHECK(service->stats().droppedFeeds == 1u);
    const auto ran = kinds->taken();
    CHECK(std::ranges::count(ran, std::string{"feed"}) == 1);
  }

  SECTION("a running image batch yields after the image in hand when a feed arrives")
  {
    holdThread(*service, *hold, *entry);
    for (auto i = 0; i < 256; ++i)
    {
      auto key = MaterialImageKey{};
      key.name = fmt::format("textures/batch/i{}", i);
      key.usage = 1;
      key.filter = 3;
      static_cast<void>(service->image(key));
    }
    auto imagesBeforeFeed = std::make_shared<std::atomic<std::size_t>>(0);
    auto handedOver = std::make_shared<std::atomic<bool>>(false);
    auto* raw = service.get();
    *onItem = [raw, imagesBeforeFeed, handedOver](const std::string_view kind) {
      if (kind == "image" && !handedOver->exchange(true))
      {
        // a hand-over while the image batch is about to run
        static_cast<void>(
          raw->setImageOverride("textures/feed/c", overrideOf(rgba8(4, 4, 1, 2, 3))));
      }
      else if (kind == "feed")
      {
        auto count = std::size_t{0};
        raw->table()->images.forEach([&](const auto&, const auto&) { ++count; });
        *imagesBeforeFeed = count;
      }
    };
    hold->open();
    waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    *onItem = {};
    CHECK(handedOver->load());
    CHECK(imagesBeforeFeed->load() == 1u);
  }

  SECTION("feedRequested and feedApplied agree after every item")
  {
    for (auto i = 0; i < 5; ++i)
    {
      if (i % 2 == 0)
      {
        REQUIRE(service
                  ->setImageOverride(
                    fmt::format("textures/feed/d{}", i), overrideOf(rgba8(2, 2, 1, 2, 3)))
                  .is_success());
      }
      else
      {
        service->clearImageOverride(fmt::format("textures/feed/d{}", i - 1));
      }
      const auto table = waitForFeeds(*service);
      CHECK(table->feedApplied == service->feedRequested());
    }
    CHECK(service->feedRequested() == 5u);
  }
}

namespace
{

using namespace engine_image;

constexpr int CubeNative = 1;

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

RawImagePixels rawOf(const test::Rgba& picture)
{
  auto pixels = RawImagePixels{};
  pixels.width = picture.width;
  pixels.height = picture.height;
  pixels.bytes = picture.pixels;
  return pixels;
}

std::vector<std::uint8_t> recordBytes(const CachedImage& image, const std::size_t record)
{
  const auto& level = image.levels.at(record);
  return {
    image.data.begin() + std::ptrdiff_t(level.offset),
    image.data.begin() + std::ptrdiff_t(level.offset + level.size)};
}

// what the source fallback makes of the same pixels: sourceImageLevels with DeriveOpts
// for the usage, called directly
CachedImage directLevels(
  MaterialDecls& decls,
  const RawImagePixels& raw,
  const int usage,
  const int filter = FilterDefault)
{
  const auto opts = engineImageOpts(usage);
  auto pixels = SourcePixels{};
  pixels.width = raw.width;
  pixels.height = raw.height;
  pixels.faces = raw.faces;
  pixels.usage = usage;
  pixels.pixels = raw.bytes;
  return decls.sourceImageLevels(
    std::move(pixels),
    opts.format,
    opts.colorFormat,
    engineMipLevels(opts.format, filter, raw.width, raw.height),
    opts.gammaMips);
}

// a game folder with the LP1c fixture's _s + _rmao pair and a material whose stages name
// an _rmao image and an image program
class OverrideGame
{
private:
  fs::TestEnvironment m_env;

public:
  OverrideGame()
  {
    file("textures/lp1c/metal_s.tga", test::tgaFile(test::flat(8, 8, 1, 2, 3), 32));
    file("textures/lp1c/metal_rmao.png", test::pngFile(test::gradient(16, 8)));
    file(
      "materials/ovr.mtr",
      "textures/ovr/pbr\n{\n\tdiffusemap\ttextures/ovr/base.tga\n\tspecularmap\t"
      "textures/lp1c/metal_rmao\n\tbumpmap\taddnormals( textures/ovr/base_local.tga, "
      "heightmap( textures/ovr/base_h.tga, 4 ) )\n}\n");
  }

  void file(const std::string& path, const std::string& bytes) const
  {
    writeTextFile(m_env.dir() / "base" / path, bytes);
  }

  const std::filesystem::path& path() const { return m_env.dir(); }
};

// the key of the stage whose lighting is this one (1 bump, 2 diffuse, 3 specular)
MaterialImageKey stageKey(const MaterialEntry& entry, const int lighting)
{
  auto key = std::optional<MaterialImageKey>{};
  for (const auto& stage : entry.stages)
  {
    if (stage.lighting == lighting && stage.image)
    {
      key = *stage.image;
      break;
    }
  }
  REQUIRE(key);
  return *key;
}

std::string refusalOf(const Result<std::shared_ptr<const CachedImage>>& result)
{
  return result.is_error() ? std::get<Error>(result.error()).msg : std::string{};
}

double medianMs(std::vector<double> times)
{
  std::ranges::sort(times);
  return times[times.size() / 2];
}

} // namespace

TEST_CASE("overrideImage converts as the source fallback does (C39, D97, D103)")
{
  const auto game = OverrideGame{};
  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto convert = [&](const MaterialImageKey& key, const ImageOverride& image) {
    const auto name = decls.imageCacheName(key);
    INFO(name.error);
    REQUIRE(name);
    return overrideImage(decls, *name.value, key, image);
  };
  const auto made = [&](const MaterialImageKey& key, const ImageOverride& image) {
    auto result = convert(key, image);
    INFO(refusalOf(result));
    REQUIRE(result.is_success());
    return std::move(result).value();
  };
  const auto picture = test::gradient(16, 8);

  SECTION("a diffuse key: sourceImageLevels' YCoCg and gamma mips of the same pixels")
  {
    const auto key = keyOf("textures/ovr/a", UsageDiffuse);
    const auto image = made(key, overrideOf(rawOf(picture)));
    const auto expected = directLevels(decls, rawOf(picture), UsageDiffuse);
    CHECK(image->colorFormat == ColorYCoCgDxt5);
    CHECK(image->storedFormat == FormatDxt5);
    CHECK(image->format == FormatRgba8);
    CHECK(image->numLevels == expected.numLevels);
    REQUIRE(image->levels.size() == expected.levels.size());
    for (std::size_t i = 0; i < expected.levels.size(); ++i)
    {
      CAPTURE(i);
      CHECK(recordBytes(*image, i) == recordBytes(expected, i));
    }
    CHECK(image->data == expected.data);
  }

  SECTION("a bump key: the normal swizzle (0, G, 0, R)")
  {
    const auto image =
      made(keyOf("textures/ovr/a", UsageBump), overrideOf(rawOf(picture)));
    CHECK(image->colorFormat == ColorNormalDxt5);
    auto swizzled = std::vector<std::uint8_t>{};
    for (std::size_t i = 0; i < picture.pixels.size(); i += 4)
    {
      swizzled.insert(swizzled.end(), {0, picture.pixels[i + 1], 0, picture.pixels[i]});
    }
    CHECK(recordBytes(*image, 0) == swizzled);
  }

  SECTION("a 6-face cube: six records a level, level-major, in R_LoadCubeImages' order")
  {
    auto raw = RawImagePixels{};
    raw.width = 8;
    raw.height = 8;
    raw.faces = 6;
    for (auto side = 0; side < 6; ++side)
    {
      const auto face = test::flat(8, 8, std::uint8_t(10 * side), 20, 30);
      raw.bytes.insert(raw.bytes.end(), face.pixels.begin(), face.pixels.end());
    }
    const auto image =
      made(keyOf("textures/ovr/sky", UsageDefault, CubeNative), overrideOf(raw));
    CHECK(image->textureType == TypeCubic);
    REQUIRE(image->levels.size() == std::size_t(image->numLevels) * 6);
    for (std::size_t k = 0; k < image->levels.size(); ++k)
    {
      CAPTURE(k);
      CHECK(image->levels[k].level == int(k / 6));
      CHECK(image->levels[k].destZ == int(k % 6));
    }
    for (auto side = 0; side < 6; ++side)
    {
      CHECK(
        recordBytes(*image, std::size_t(side))
        == test::flat(8, 8, std::uint8_t(10 * side), 20, 30).pixels);
    }
    const auto expected = directLevels(decls, raw, UsageDefault);
    CHECK(image->data == expected.data);
  }

  SECTION("RGBA32F into TD_HDRI: R11G11B10F packed as LoadEXR packs, the alpha dropped")
  {
    auto raw = RawImagePixels{};
    raw.width = 4;
    raw.height = 4;
    raw.format = ImagePixelFormat::RGBA32F;
    auto floats = std::vector<float>{};
    for (auto i = 0; i < 16; ++i)
    {
      floats.insert(floats.end(), {1.0f, 1.0f, 1.0f, 0.25f});
    }
    floats[4] = 2.0f;
    floats[5] = 0.5f;
    raw.bytes.resize(floats.size() * sizeof(float));
    std::memcpy(raw.bytes.data(), floats.data(), raw.bytes.size());
    const auto image =
      made(keyOf("textures/ovr/hdr", UsageHdri, Cube2D, FilterLinear), overrideOf(raw));
    CHECK(image->format == FormatR11g11b10f);
    CHECK(image->storedFormat == FormatBc6h);
    REQUIRE(image->levels.size() == 1u);
    CHECK(recordBytes(*image, 0) == packEngineR11G11B10F(raw.bytes));
    // f32_to_uf11( 1.0f ) = 15 << 6, f32_to_uf10( 1.0f ) = 15 << 5
    auto first = std::uint32_t{0};
    std::memcpy(&first, image->data.data(), sizeof(first));
    CHECK(first == 0x781E03C0u);
  }

  SECTION("the cross pairs of pixel format and usage are refused")
  {
    auto floats = RawImagePixels{};
    floats.width = 2;
    floats.height = 2;
    floats.format = ImagePixelFormat::RGBA32F;
    floats.bytes.assign(2 * 2 * 16, 0);
    CHECK_THAT(
      refusalOf(convert(keyOf("textures/ovr/a", UsageDiffuse), overrideOf(floats))),
      Catch::Matchers::ContainsSubstring("RGBA32F")
        && Catch::Matchers::ContainsSubstring("TD_HDRI"));
    CHECK_THAT(
      refusalOf(convert(
        keyOf("textures/ovr/hdr", UsageHdri),
        overrideOf(rawOf(test::flat(2, 2, 1, 2, 3))))),
      Catch::Matchers::ContainsSubstring("RGBA8")
        && Catch::Matchers::ContainsSubstring("8-bit"));
  }

  SECTION("GPU-ready levels: the usage's format, colour format and face count")
  {
    const auto dxt5 = [](const int colorFormat, const int textureType) {
      auto image = CachedImage{};
      image.textureType = textureType;
      image.storedFormat = FormatDxt5;
      image.format = FormatDxt5;
      image.colorFormat = colorFormat;
      image.width = 8;
      image.height = 8;
      image.numLevels = 2;
      const auto faces = textureType == TypeCubic ? 6 : 1;
      for (auto level = 0; level < 2; ++level)
      {
        for (auto side = 0; side < faces; ++side)
        {
          const auto size = std::size_t(level == 0 ? 64 : 16);
          image.levels.push_back(CachedImageLevel{
            level, side, 8 >> level, 8 >> level, image.data.size(), size});
          image.data.resize(image.data.size() + size, std::uint8_t(level + side));
        }
      }
      auto override = ImageOverride{};
      override.image = std::make_shared<const CachedImage>(std::move(image));
      return override;
    };
    const auto diffuse = keyOf("textures/ovr/a", UsageDiffuse);
    const auto right = dxt5(ColorYCoCgDxt5, Type2D);
    CHECK(
      made(diffuse, right) == std::get<std::shared_ptr<const CachedImage>>(right.image));
    CHECK_THAT(
      refusalOf(convert(diffuse, dxt5(ColorDefault, Type2D))),
      Catch::Matchers::ContainsSubstring("colour format"));
    CHECK_THAT(
      refusalOf(convert(diffuse, dxt5(ColorYCoCgDxt5, TypeCubic))),
      Catch::Matchers::ContainsSubstring("texture type"));
    CHECK_THAT(
      refusalOf(convert(
        keyOf("textures/ovr/sky", UsageDefault, CubeNative), dxt5(ColorDefault, Type2D))),
      Catch::Matchers::ContainsSubstring("texture type"));
    // a format that is neither the usage's nor its uncompressed twin
    auto wrong = dxt5(ColorDefault, Type2D);
    CHECK_THAT(
      refusalOf(convert(keyOf("textures/ovr/s", UsageSpecular), wrong)),
      Catch::Matchers::ContainsSubstring("format 8"));
  }

  SECTION("the cap: a level beyond UploadBytesPerFrame over all its faces (D103)")
  {
    // one level each (a linear filter), so that the accepted sizes convert quickly
    const auto flat = keyOf("textures/ovr/big", UsageDefault, Cube2D, FilterLinear);
    CHECK_THAT(
      refusalOf(convert(flat, overrideOf(rgba8(4096, 4096, 1, 2, 3)))),
      Catch::Matchers::ContainsSubstring("M-TEX-DIFF-02"));
    CHECK(convert(flat, overrideOf(rgba8(2048, 2048, 1, 2, 3))).is_success());
    CHECK(convert(flat, overrideOf(rgba8(2896, 2896, 1, 2, 3))).is_success());
    CHECK_FALSE(convert(flat, overrideOf(rgba8(2897, 2897, 1, 2, 3))).is_success());

    const auto cube =
      keyOf("textures/ovr/bigsky", UsageDefault, CubeNative, FilterLinear);
    const auto cubeOf = [](const int side) {
      auto raw = rgba8(side, side, 1, 2, 3, 255, 6);
      return overrideOf(std::move(raw));
    };
    CHECK_THAT(
      refusalOf(convert(cube, cubeOf(2048))),
      Catch::Matchers::ContainsSubstring("6 face(s)")
        && Catch::Matchers::ContainsSubstring("M-TEX-DIFF-02"));
    CHECK(convert(cube, cubeOf(1024)).is_success());
    CHECK(convert(cube, cubeOf(1182)).is_success());
    CHECK_FALSE(convert(cube, cubeOf(1183)).is_success());
  }

  SECTION("the cache name's usage: _rmao stages, _s with and without the sibling (P5.1)")
  {
    const auto entry = decls.lookup("textures/ovr/pbr");
    INFO(entry.error);
    REQUIRE(entry);
    const auto raw = rawOf(picture);

    // Material.cpp:2279-2285: a stage naming _rmao is TD_SPECULAR_PBR_RMAO at parse
    const auto rmao = stageKey(**entry.value, 3);
    CHECK(rmao.usage == UsageSpecularPbrRmao);
    const auto stage = made(rmao, overrideOf(raw));
    CHECK(stage->storedFormat == FormatDxt1);
    CHECK(stage->data == directLevels(decls, raw, UsageSpecularPbrRmao).data);

    // an _s whose _rmao file exists: the time call renames it, and the override converts
    // with RMAO's options although its bytes replace the files'
    const auto withSibling = keyOf("textures/lp1c/metal_s", UsageSpecular);
    const auto name = decls.imageCacheName(withSibling);
    REQUIRE(name);
    CHECK(name.value->usage == UsageSpecularPbrRmao);
    CHECK(name.value->imageName == "textures/lp1c/metal_rmao");
    CHECK(
      made(withSibling, overrideOf(raw))->data
      == directLevels(decls, raw, UsageSpecularPbrRmao).data);

    // an _s without the sibling stays TD_SPECULAR, with its gamma mips
    const auto alone = keyOf("textures/lp1c/plain_s", UsageSpecular);
    const auto plain = made(alone, overrideOf(raw));
    CHECK(plain->data == directLevels(decls, raw, UsageSpecular).data);
    CHECK(plain->data != directLevels(decls, raw, UsageSpecularPbrRmao).data);

    // a program key converts with the program's usage (TD_BUMP)
    const auto program = stageKey(**entry.value, 1);
    CHECK_THAT(program.name, Catch::Matchers::ContainsSubstring("addnormals"));
    CHECK(made(program, overrideOf(raw))->colorFormat == ColorNormalDxt5);
  }
}

TEST_CASE("overrideImage's conversion time, 1024 x 1024 RGBA8 (P5.13, Q29)")
{
  auto source = MemoryDeclSource{std::vector<DeclFile>{}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto image = overrideOf(rawOf(test::gradient(1024, 1024)));
  for (const auto& [label, usage] :
       {std::pair{"diffuse (YCoCg, gamma mips)", UsageDiffuse},
        std::pair{"bump (normal swizzle, box mips)", UsageBump}})
  {
    const auto key = keyOf("textures/ovr/timed", usage);
    const auto name = decls.imageCacheName(key);
    REQUIRE(name);
    auto times = std::vector<double>{};
    for (auto i = 0; i < 10; ++i)
    {
      const auto started = std::chrono::steady_clock::now();
      const auto made = overrideImage(decls, *name.value, key, image);
      times.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count());
      REQUIRE(made.is_success());
    }
#ifdef NDEBUG
    constexpr auto build = "Release";
#else
    constexpr auto build = "Debug";
#endif
    std::cout << fmt::format(
      "LPF.0a conversion ({}): 1024 x 1024 RGBA8 {}: median {:.1f} ms, min {:.1f}, max "
      "{:.1f} over 10\n",
      build,
      label,
      medianMs(times),
      *std::ranges::min_element(times),
      *std::ranges::max_element(times));
  }
}

namespace
{

// counts the tables published
struct Publications
{
  std::atomic<std::uint64_t> count = 0;

  MaterialListener listener()
  {
    return [this](const MaterialServiceEvent& event) {
      if (event.kind == MaterialServiceEvent::Kind::Published)
      {
        ++count;
      }
    };
  }
};

// a game folder of materials whose images the overrides replace
class FeedGame
{
private:
  fs::TestEnvironment m_env;

public:
  FeedGame()
  {
    auto text = materialText("textures/feed/wall", "textures/feed/wall");
    text += materialText("textures/feed/sized", "textures/feed/sized");
    text +=
      "textures/feed/prog\n{\n\tbumpmap\taddnormals( textures/feed/wall_local.tga, "
      "heightmap( textures/feed/wall_h.tga, 4 ) "
      ")\n\tdiffusemap\ttextures/feed/wall.tga\n}"
      "\n";
    writeTextFile(base() / "materials" / "feed.mtr", text);
    image("wall", 16, 16);
    image("sized", 32, 16);
  }

  void image(const std::string& name, const int width, const int height) const
  {
    writeTextFile(
      base() / "textures" / "feed" / (name + ".tga"),
      test::tgaFile(test::gradient(width, height), 32));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }

  MaterialServiceOptions options(
    MaterialListener listener = {},
    const std::chrono::milliseconds pollInterval = std::chrono::milliseconds{0}) const
  {
    auto result = MaterialServiceOptions{};
    result.makeSource = [path = m_env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    };
    result.listener = std::move(listener);
    result.pollInterval = pollInterval;
    return result;
  }
};

MaterialImageKey feedKey(const std::string& name, const int usage = UsageDiffuse)
{
  return keyOf("textures/feed/" + name, usage);
}

// the slot of a key once a table has it
std::shared_ptr<const ImageCacheSlot> waitForImage(
  MaterialService& service, const MaterialImageKey& key)
{
  static_cast<void>(service.image(key));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.findImage(key) != nullptr; }, Timeout);
  auto slot = table->findImage(key);
  REQUIRE(slot);
  return slot;
}

// hands an override over and waits for the table that includes it, the thread idle
std::shared_ptr<const MaterialTable> handOver(
  MaterialService& service, const std::string& name, ImageOverride image)
{
  REQUIRE(service.setImageOverride(name, std::move(image)).is_success());
  auto table = waitForFeeds(service);
  REQUIRE(waitUntilIdle(service));
  return table;
}

std::shared_ptr<const MaterialTable> clearAndWait(
  MaterialService& service, const std::string& name)
{
  service.clearImageOverride(name);
  auto table = waitForFeeds(service);
  REQUIRE(waitUntilIdle(service));
  return table;
}

std::uint64_t changeOf(const MaterialTable& table, const MaterialImageKey& key)
{
  const auto* change = table.changedImages.find(key);
  return change ? *change : 0;
}

// the measured editor-image size of a material, once it satisfies predicate
EditorImageSize waitForMeasure(
  MaterialService& service,
  const std::string& name,
  const std::function<bool(const EditorImageSize&)>& predicate)
{
  static_cast<void>(service.lookup(name));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.find(name);
      return slot && slot->entry && slot->sizeMeasured
             && predicate(slot->entry->editorImageSize);
    },
    Timeout);
  const auto slot = table->find(name);
  REQUIRE(slot);
  REQUIRE(slot->entry);
  return slot->entry->editorImageSize;
}

using State = ImageOverrideStatus::State;

} // namespace

TEST_CASE("MaterialService image overrides, the store and its publications (C40, C41)")
{
  const auto game = FeedGame{};
  auto publications = std::make_shared<Publications>();
  auto hold = std::make_shared<Hold>();
  auto serviceOptions = game.options(publications->listener());
  serviceOptions.beforeTakeWorkForTesting = [hold] { hold->pass(); };
  auto service = start(std::move(serviceOptions));
  const auto openAtExit = OpenHoldAtExit{hold};
  const auto wall = feedKey("wall");
  const auto fromFile = waitForImage(*service, wall);
  REQUIRE(fromFile->image);
  CHECK_FALSE(fromFile->override);
  CHECK(fromFile->image->width == 16);
  REQUIRE(waitUntilIdle(*service));

  SECTION("the whole name under ImageFromFile's folding matches; one publication")
  {
    const auto before = service->table();
    const auto published = publications->count.load();
    const auto table =
      handOver(*service, "TEXTURES/Feed/Wall.tga", overrideOf(rgba8(8, 8, 200, 100, 50)));
    // the drop and the new slot in one table, with the hand-over counted
    CHECK(publications->count.load() == published + 1);
    CHECK(table->feedApplied == before->feedApplied + 1);
    CHECK(table->imageChanges > before->imageChanges);
    CHECK(changeOf(*table, wall) > changeOf(*before, wall));
    const auto slot = table->findImage(wall);
    REQUIRE(slot);
    CHECK(slot->override);
    CHECK(slot->state == ImageCacheSlot::State::Ready);
    CHECK(slot->files.empty());
    REQUIRE(slot->image);
    CHECK(slot->image->width == 8);
    CHECK(slot->image->colorFormat == ColorYCoCgDxt5);
    CHECK(slot->name.usage == UsageDiffuse);
    CHECK(service->image(wall).state == ImageLookup::State::Ready);
    CHECK(service->imageOverrideStatus("textures/feed/wall").state == State::Applied);
    CHECK(table->overrideBytes == 8u * 8u * 4u);

    // an upper-case extension is not removed (idStr::Replace compares with case)
    const auto other =
      handOver(*service, "TEXTURES/Feed/Wall.TGA", overrideOf(rgba8(4, 4, 1, 2, 3)));
    CHECK(other->findImage(wall) == slot);
    CHECK(service->imageOverrideStatus("TEXTURES/Feed/Wall.TGA").state == State::Pending);
  }

  SECTION("a second override: one table holds the drop and the new slot")
  {
    handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    const auto before = service->table();
    const auto published = publications->count.load();
    const auto conversions = service->stats().overrideConversions;
    const auto table =
      handOver(*service, "textures/feed/wall", overrideOf(rgba8(4, 4, 9, 8, 7)));
    CHECK(publications->count.load() == published + 1);
    CHECK(table->feedApplied == before->feedApplied + 1);
    CHECK(changeOf(*table, wall) > changeOf(*before, wall));
    CHECK(table->imageChanges > before->imageChanges);
    REQUIRE(table->findImage(wall)->image);
    CHECK(table->findImage(wall)->image->width == 4);
    CHECK(service->stats().overrideConversions == conversions + 1);
    CHECK(table->overrideBytes == 4u * 4u * 4u);
  }

  SECTION("two hand-overs before the item runs: one conversion, the newest bytes")
  {
    const auto entry = waitForEntry(*service, "textures/feed/wall");
    holdThread(*service, *hold, *entry);
    const auto conversions = service->stats().overrideConversions;
    REQUIRE(
      service->setImageOverride("textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)))
        .is_success());
    REQUIRE(
      service->setImageOverride("textures/feed/wall", overrideOf(rgba8(2, 2, 4, 5, 6)))
        .is_success());
    hold->open();
    const auto table = waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    CHECK(table->feedApplied == service->feedRequested());
    CHECK(service->stats().droppedFeeds == 1u);
    CHECK(service->stats().overrideConversions == conversions + 1);
    REQUIRE(table->findImage(wall)->image);
    CHECK(table->findImage(wall)->image->width == 2);
  }

  SECTION("a refused override: one publication, the reason, the old slot standing")
  {
    auto floats = RawImagePixels{};
    floats.width = 2;
    floats.height = 2;
    floats.format = ImagePixelFormat::RGBA32F;
    floats.bytes.assign(2 * 2 * 16, 0);
    const auto before = service->table();
    const auto published = publications->count.load();
    const auto table = handOver(*service, "textures/feed/wall", overrideOf(floats));
    CHECK(publications->count.load() == published + 1);
    CHECK(table->feedApplied == before->feedApplied + 1);
    CHECK(table->feedApplied == service->feedRequested());
    CHECK(table->findImage(wall) == fromFile);
    CHECK(table->imageChanges == before->imageChanges);
    const auto status = service->imageOverrideStatus("textures/feed/wall");
    CHECK(status.state == State::Refused);
    CHECK_THAT(status.reason, Catch::Matchers::ContainsSubstring("RGBA32F"));
    CHECK(table->overrideBytes == 0u);

    // a refused replacement leaves the earlier override applied
    const auto applied =
      handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    const auto kept = handOver(*service, "textures/feed/wall", overrideOf(floats));
    CHECK(kept->findImage(wall) == applied->findImage(wall));
    CHECK(kept->overrideBytes == 8u * 8u * 4u);
  }

  SECTION("a clear: one publication with the file's slot, never a pending one")
  {
    handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    const auto before = service->table();
    const auto published = publications->count.load();
    const auto table = clearAndWait(*service, "textures/feed/wall");
    CHECK(publications->count.load() == published + 1);
    CHECK(table->feedApplied == before->feedApplied + 1);
    CHECK(changeOf(*table, wall) > changeOf(*before, wall));
    const auto slot = table->findImage(wall);
    REQUIRE(slot);
    CHECK_FALSE(slot->override);
    REQUIRE(slot->image);
    CHECK(slot->image->width == 16);
    CHECK(service->image(wall).state == ImageLookup::State::Source);
    CHECK(service->imageOverrideStatus("textures/feed/wall").state == State::None);
    CHECK(table->overrideBytes == 0u);
  }

  SECTION("a clear of a name with no override publishes once, nothing else changed")
  {
    const auto before = service->table();
    const auto published = publications->count.load();
    const auto table = clearAndWait(*service, "textures/feed/nothing");
    CHECK(publications->count.load() == published + 1);
    CHECK(table->feedApplied == before->feedApplied + 1);
    CHECK(table->imageChanges == before->imageChanges);
    CHECK(table->findImage(wall) == fromFile);
  }

  SECTION("an override no key uses yet is converted at the key's first lookup")
  {
    handOver(*service, "textures/feed/later", overrideOf(rgba8(4, 4, 1, 2, 3)));
    CHECK(service->imageOverrideStatus("textures/feed/later").state == State::Pending);
    const auto later = feedKey("later");
    CHECK_FALSE(service->table()->findImage(later));
    const auto slot = waitForImage(*service, later);
    CHECK(slot->override);
    REQUIRE(slot->image);
    CHECK(slot->image->width == 4);
    CHECK(service->imageOverrideStatus("textures/feed/later").state == State::Applied);
  }

  SECTION("a program key takes only an override of its whole text (P5.1)")
  {
    const auto entry = waitForEntry(*service, "textures/feed/prog");
    const auto program = stageKey(*entry, 1);
    const auto programSlot = waitForImage(*service, program);
    REQUIRE(waitUntilIdle(*service));
    CHECK_FALSE(programSlot->override);
    // the program's leaf is another name
    const auto leaf =
      handOver(*service, "textures/feed/wall_local", overrideOf(rgba8(4, 4, 1, 2, 3)));
    CHECK(leaf->findImage(program) == programSlot);
    // the whole text, its extensions folded as ImageFromFile folds them
    CAPTURE(program.name);
    const auto whole = handOver(*service, program.name, overrideOf(rgba8(4, 4, 1, 2, 3)));
    const auto slot = whole->findImage(program);
    REQUIRE(slot);
    CHECK(slot->override);
    REQUIRE(slot->image);
    CHECK(slot->image->colorFormat == ColorNormalDxt5);
  }

  SECTION("reload(): the override slot stays, not converted again (D99, P5.7)")
  {
    const auto applied =
      handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    const auto conversions = service->stats().overrideConversions;
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u && s.idle; }, Timeout));
    const auto reloaded = service->table();
    CHECK(reloaded->findImage(wall) == applied->findImage(wall));
    CHECK(service->stats().overrideConversions == conversions);
    CHECK(reloaded->feedApplied == applied->feedApplied);
    CHECK(reloaded->overrideBytes == applied->overrideBytes);
  }
}

TEST_CASE(
  "MaterialService image overrides, the budget, the poll and the sizes (D99, D111)")
{
  using SizeState = EditorImageSize::State;
  const auto game = FeedGame{};

  SECTION("the budget drops a taken override slot; the next request converts it again")
  {
    auto serviceOptions = game.options();
    serviceOptions.imageBudgetBytes = std::size_t{1024} * 1024;
    auto service = start(std::move(serviceOptions));
    const auto wall = feedKey("wall");
    static_cast<void>(waitForImage(*service, wall));
    // 512 x 512 RGBA8 with its mips, about 1.4 MB: beyond the budget alone
    handOver(*service, "textures/feed/wall", overrideOf(rgba8(512, 512, 1, 2, 3)));
    REQUIRE(service->table()->findImage(wall)->override);
    REQUIRE(service->image(wall).image);
    const auto conversions = service->stats().overrideConversions;
    // another image's batch runs the budget, which drops the taken override slot
    static_cast<void>(waitForImage(*service, feedKey("sized")));
    REQUIRE(waitUntilIdle(*service));
    CHECK_FALSE(service->table()->findImage(wall));
    CHECK(service->stats().evictedImages >= 1u);
    // the store is the truth: asked again, it is converted from the store
    CHECK(service->image(wall).state == ImageLookup::State::Pending);
    const auto again = waitForImage(*service, wall);
    CHECK(again->override);
    REQUIRE(again->image);
    CHECK(again->image->width == 512);
    CHECK(service->stats().overrideConversions == conversions + 1);
  }

  SECTION("the poll never probes an overridden key's files (C41)")
  {
    auto service = start(game.options({}, std::chrono::milliseconds{20}));
    const auto wall = feedKey("wall");
    const auto sized = feedKey("sized");
    static_cast<void>(waitForImage(*service, wall));
    const auto control = waitForImage(*service, sized);
    const auto applied =
      handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    const auto slot = applied->findImage(wall);
    REQUIRE(slot->override);
    // both files change; the poll reads the control again, never the override's
    game.image("wall", 64, 64);
    game.image("sized", 64, 16);
    bumpModificationTime(game.base() / "textures" / "feed" / "wall.tga");
    bumpModificationTime(game.base() / "textures" / "feed" / "sized.tga");
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto read = t.findImage(sized);
        return read && read != control && read->image && read->image->width == 64;
      },
      Timeout));
    // and some more polls
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK(service->table()->findImage(wall) == slot);
  }

  SECTION("editorSize: C21's size of the name, the materials that hold it remeasured")
  {
    auto service = start(game.options());
    CHECK(
      waitForMeasure(*service, "textures/feed/sized", [](const auto&) { return true; })
      == EditorImageSize{32, 16, SizeState::Ready, {}});
    auto image = overrideOf(rgba8(4, 4, 1, 2, 3));
    image.editorSize = EditorImageSize{1024, 1024, SizeState::Ready, {}};
    REQUIRE(
      service->setImageOverride("textures/feed/sized", std::move(image)).is_success());
    CHECK(
      waitForMeasure(
        *service,
        "textures/feed/sized",
        [](const auto& size) { return size.width == 1024; })
      == EditorImageSize{1024, 1024, SizeState::Ready, {}});
    // another material's size is untouched
    CHECK(
      waitForMeasure(*service, "textures/feed/wall", [](const auto&) { return true; })
      == EditorImageSize{16, 16, SizeState::Ready, {}});
    // a clear: the file's size again
    service->clearImageOverride("textures/feed/sized");
    CHECK(
      waitForMeasure(
        *service,
        "textures/feed/sized",
        [](const auto& size) { return size.width == 32; })
      == EditorImageSize{32, 16, SizeState::Ready, {}});
  }

  SECTION("without a registry an override waits, Pending; the next build applies it")
  {
    // a source that throws when the first build ends its read batch (W.4)
    class FailOnceSource : public GameFileSource
    {
    private:
      std::shared_ptr<std::atomic<bool>> m_throws;

    public:
      FailOnceSource(
        const std::filesystem::path& path, std::shared_ptr<std::atomic<bool>> throws)
        : GameFileSource{path}
        , m_throws{std::move(throws)}
      {
      }

      void closeFiles() override
      {
        GameFileSource::closeFiles();
        if (m_throws->exchange(false))
        {
          throw std::runtime_error{"an injected build failure"};
        }
      }
    };
    auto throws = std::make_shared<std::atomic<bool>>(true);
    auto serviceOptions = game.options();
    serviceOptions.makeSource = [path = game.base().parent_path(),
                                 throws](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{std::make_unique<FailOnceSource>(path, throws)};
    };
    auto service = start(std::move(serviceOptions));
    const auto wall = feedKey("wall");
    const auto failed = waitForImage(*service, wall);
    CHECK_FALSE(failed->image);
    REQUIRE(waitUntilIdle(*service));
    handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 1, 2, 3)));
    CHECK(service->imageOverrideStatus("textures/feed/wall").state == State::Pending);
    CHECK_FALSE(service->table()->findImage(wall)->override);

    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 1u && s.idle; }, Timeout));
    // the build dropped the failed slot: the consumer's lookup converts the override
    const auto applied = waitForImage(*service, wall);
    CHECK(applied->override);
    CHECK(service->imageOverrideStatus("textures/feed/wall").state == State::Applied);
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("MaterialService image overrides leak nothing")
{
  const auto game = FeedGame{};
  auto service = start(game.options());
  const auto wall = feedKey("wall");
  static_cast<void>(waitForImage(*service, wall));
  const auto cycle = [&](const int i) {
    handOver(
      *service, "textures/feed/wall", overrideOf(rgba8(8, 8, std::uint8_t(i), 2, 3)));
    static_cast<void>(service->image(wall));
    clearAndWait(*service, "textures/feed/wall");
    static_cast<void>(service->image(wall));
    handOver(*service, "textures/feed/wall", overrideOf(rgba8(8, 8, 4, 5, 6)));
    static_cast<void>(service->image(wall));
    REQUIRE(waitUntilIdle(*service));
  };
  // LPF.1 (C44): the warm-up fills the tables' feed records to their bound
  // (MaterialTable::FeedRecordsKept), three items a cycle
  for (auto i = 0; i < int(MaterialTable::FeedRecordsKept / 3) + 2; ++i)
  {
    cycle(i);
  }
  const auto grew = heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      cycle(i);
    }
  });
  CHECK_FALSE(grew);
}
#endif

} // namespace tb::lit
