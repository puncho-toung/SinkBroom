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

// LP1c.2 (C21, A6, P.4, P.11): the editor-image size, GetEditorImage's chain branch by
// branch on a fixture material each with images of known sizes, the LP1b fixture's
// sizes against the engine's listImages (the engine golden for C21), and the re-probe
// when a loose image file changes.

#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/EditorImageSize.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
using State = EditorImageSize::State;

// the time capture_fixtures.py's staged copies of the LP1b images had
// (tst_ImageCache.cpp)
constexpr std::int64_t FixtureSourceTime = 1792636960;

// one material per branch of GetEditorImage's chain (Material.cpp:223-291)
constexpr auto ChainMaterials = R"(
// qer_editorimage loads: its size
textures/lp1c/c21_editor
{
	qer_editorimage	textures/lp1c/c21_editor.tga
	diffusemap	textures/lp1c/c21_diffuse.tga
}

// qer_editorimage defaulted: the first diffuse stage's image
textures/lp1c/c21_editor_missing
{
	qer_editorimage	textures/lp1c/c21_none.tga
	bumpmap	textures/lp1c/c21_bump.tga
	diffusemap	textures/lp1c/c21_diffuse.tga
}

// qer_editorimage defaulted and no diffuse stage: the defaulted image, 8 x 8, not stage
// 0's
textures/lp1c/c21_editor_alone
{
	qer_editorimage	textures/lp1c/c21_none.tga
	{
		blend	add
		map	textures/lp1c/c21_stage0.tga
	}
}

// no qer_editorimage: the first diffuse stage's image, not stage 0's
textures/lp1c/c21_diffuse_stage
{
	bumpmap	textures/lp1c/c21_bump.tga
	diffusemap	textures/lp1c/c21_diffuse.tga
}

// no qer_editorimage, no diffuse stage: stage 0's image
textures/lp1c/c21_stage0
{
	{
		blend	add
		map	textures/lp1c/c21_stage0.tga
	}
}

// a missing image: 8 x 8, defaulted
textures/lp1c/c21_missing
{
	diffusemap	textures/lp1c/c21_missing.tga
}

// an image program: its first leaf (addnormals resamples the second to it). As the
// diffuse stage: a lit material without one gets the parser's own diffuse stage of _white
textures/lp1c/c21_program
{
	diffusemap	addnormals( textures/lp1c/c21_bump.tga, textures/lp1c/c21_stage0.tga )
}

// a lit material without a diffuse stage: the parser's own, _white, the file
// guis/assets/white, missing here
textures/lp1c/c21_bump_only
{
	bumpmap	textures/lp1c/c21_bump.tga
}

// an intrinsic: its generator's size (not _white or _black, which the image program
// parser renames to the files guis/assets/white and textures/black)
textures/lp1c/c21_intrinsic
{
	{
		blend	add
		map	_quadratic
	}
}

// _white is the file guis/assets/white: missing here, so defaulted
textures/lp1c/c21_white
{
	{
		blend	add
		map	_white
	}
}

// no stages: the default image, 16 x 16
textures/lp1c/c21_nostage
{
	nonsolid
}

// qer_editorimage naming the default image
textures/lp1c/c21_editor_default
{
	qer_editorimage	_default
	diffusemap	textures/lp1c/c21_diffuse.tga
}

// a stage image whose file the loader pre-validation refuses: defaulted
textures/lp1c/c21_refused
{
	diffusemap	textures/lp1c/c21_refused.tga
}
)";

class SizeGame
{
private:
  fs::TestEnvironment m_env;

public:
  SizeGame()
  {
    writeTextFile(base() / "materials" / "lp1c_c21.mtr", ChainMaterials);
    image("c21_editor", 32, 16);
    image("c21_diffuse", 8, 4);
    image("c21_bump", 16, 16);
    image("c21_stage0", 24, 8);
    auto refused = tgaFile(gradient(4, 4), 32);
    refused[16] = 16;
    writeTextFile(base() / "textures" / "lp1c" / "c21_refused.tga", refused);
  }

  void image(const std::string& name, const int width, const int height) const
  {
    writeTextFile(
      base() / "textures" / "lp1c" / (name + ".tga"),
      tgaFile(gradient(width, height), 32));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }
  const std::filesystem::path& path() const { return m_env.dir(); }
};

EditorImageSize sizeOf(
  MaterialDecls& decls, DeclSource& source, const std::string& material)
{
  const auto entry = decls.lookup(material);
  INFO(entry.error);
  REQUIRE(entry);
  return editorImageSize(decls, source, **entry.value).size;
}

void checkSize(
  MaterialDecls& decls,
  DeclSource& source,
  const std::string& material,
  const int width,
  const int height,
  const State state)
{
  const auto size = sizeOf(decls, source, material);
  CAPTURE(material, size.reason);
  CHECK(size.width == width);
  CHECK(size.height == height);
  CHECK(size.state == state);
}

// the engine's listImages lines after the fixture map's load: name -> (width, height)
std::map<std::string, std::pair<int, int>> engineListImages()
{
  const auto text =
    readTextFile(litFixtureFolder() / "goldens" / "lp1b_geometry" / "listImages.txt");
  auto sizes = std::map<std::string, std::pair<int, int>>{};
  auto stream = std::istringstream{text};
  auto line = std::string{};
  while (std::getline(stream, line))
  {
    // " 478:         64   32 DXT5             mip  rept    2k  textures/lp1b/floor"
    const auto colon = line.find(':');
    if (colon == std::string::npos || line.rfind('#', 0) == 0)
    {
      continue;
    }
    auto fields = std::istringstream{line.substr(colon + 1)};
    auto width = 0;
    auto height = 0;
    if (!(fields >> width >> height))
    {
      continue;
    }
    auto name = std::string{};
    for (auto field = std::string{}; fields >> field;)
    {
      name = field;
    }
    sizes.emplace(name, std::pair{width, height});
  }
  return sizes;
}

} // namespace

TEST_CASE("EditorImageSize, GetEditorImage's chain (C21, P.4)")
{
  const auto game = SizeGame{};
  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  checkSize(decls, source, "textures/lp1c/c21_editor", 32, 16, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_editor_missing", 8, 4, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_editor_alone", 8, 8, State::Default);
  checkSize(decls, source, "textures/lp1c/c21_diffuse_stage", 8, 4, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_stage0", 24, 8, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_missing", 8, 8, State::Default);
  checkSize(decls, source, "textures/lp1c/c21_program", 16, 16, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_bump_only", 8, 8, State::Default);
  checkSize(decls, source, "textures/lp1c/c21_intrinsic", 32, 4, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_white", 8, 8, State::Default);
  checkSize(decls, source, "textures/lp1c/c21_nostage", 16, 16, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_editor_default", 16, 16, State::Ready);
  checkSize(decls, source, "textures/lp1c/c21_refused", 8, 8, State::Default);

  SECTION("a Default size says why")
  {
    const auto entry = decls.lookup("textures/lp1c/c21_missing");
    REQUIRE(entry);
    const auto sizing = editorImageSize(decls, source, **entry.value);
    CHECK_THAT(
      sizing.size.reason, Catch::Matchers::ContainsSubstring("Couldn't load image"));
    const auto refused = decls.lookup("textures/lp1c/c21_refused");
    REQUIRE(refused);
    CHECK_THAT(
      editorImageSize(decls, source, **refused.value).size.reason,
      Catch::Matchers::ContainsSubstring("pixel size 16"));
  }

  SECTION("a single image is sized from its header, without its loader")
  {
    static_cast<void>(decls.takeMessages());
    const auto entry = decls.lookup("textures/lp1c/c21_editor");
    REQUIRE(entry);
    const auto sizing = editorImageSize(decls, source, **entry.value);
    REQUIRE(sizing.files.size() == 1u);
    CHECK(sizing.files.front().path == "textures/lp1c/c21_editor.tga");
    CHECK(sizing.files.front().probe.kind == GameFileProbe::Kind::Loose);
  }

  SECTION("the editor image's key is named as ImageFromFile names it")
  {
    const auto entry = decls.lookup("textures/lp1c/c21_editor");
    REQUIRE(entry);
    const auto key = decls.editorImageKey(**entry.value);
    REQUIRE(key);
    CHECK(key->name == "textures/lp1c/c21_editor");
    CHECK(key->usage == UsageDiffuse);
    CHECK(key->filter == FilterDefault);
    const auto withoutEditor = decls.lookup("textures/lp1c/c21_stage0");
    REQUIRE(withoutEditor);
    CHECK_FALSE(decls.editorImageKey(**withoutEditor.value));
  }

  SECTION("the intrinsic generator sizes")
  {
    CHECK(intrinsicImageSize("_default") == std::pair{16, 16});
    CHECK(intrinsicImageSize("_black") == std::pair{16, 16});
    CHECK(intrinsicImageSize("_quadratic") == std::pair{32, 4});
    CHECK_FALSE(intrinsicImageSize("_currentRender"));
  }

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

namespace
{

// the LP1b fixture as the capture staged it, with the caches the engine wrote or without
void stageLp1bFixture(const std::filesystem::path& base, const bool caches)
{
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
  if (caches)
  {
    std::filesystem::copy(
      litFixtureFolder() / "goldens" / "lp1c" / "generated",
      base / "generated",
      std::filesystem::copy_options::recursive);
  }
}

constexpr const char* Lp1bMaterials[] = {
  "textures/lp1b/floor",
  "textures/lp1b/wall",
  "textures/lp1b/crate",
  "textures/lp1b/grate",
  "textures/lp1b/glass",
  "textures/lp1b/patch",
};

} // namespace

TEST_CASE("EditorImageSize, the LP1b fixture against the engine's listImages (C21)")
{
  const auto env = fs::TestEnvironment{};
  stageLp1bFixture(env.dir() / "base", true);
  auto source = GameFileSource{env.dir()};
  auto decls = MaterialDecls{source};
  const auto engine = engineListImages();
  auto compared = 0;
  for (const auto* material : Lp1bMaterials)
  {
    CAPTURE(material);
    REQUIRE(engine.count(material) == 1u);
    const auto entry = decls.lookup(material);
    REQUIRE(entry);
    const auto sizing = editorImageSize(decls, source, **entry.value);
    INFO(sizing.size.reason);
    CHECK(sizing.size.state == State::Ready);
    CHECK(std::pair{sizing.size.width, sizing.size.height} == engine.at(material));
    // the size came from the cache's header (the engine's own .bimage), read without its
    // body
    auto fromCache = false;
    for (const auto& file : sizing.files)
    {
      fromCache = fromCache || file.path.rfind("generated/images/", 0) == 0;
    }
    CHECK(fromCache);
    ++compared;
  }
  CHECK(compared == 6);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("EditorImageSize, the LP1b sources' headers without the caches (C21)")
{
  const auto env = fs::TestEnvironment{};
  stageLp1bFixture(env.dir() / "base", false);
  auto source = GameFileSource{env.dir()};
  auto decls = MaterialDecls{source};
  const auto engine = engineListImages();
  for (const auto* material : Lp1bMaterials)
  {
    CAPTURE(material);
    const auto entry = decls.lookup(material);
    REQUIRE(entry);
    const auto sizing = editorImageSize(decls, source, **entry.value);
    INFO(sizing.size.reason);
    CHECK(sizing.size.state == State::Ready);
    CHECK(std::pair{sizing.size.width, sizing.size.height} == engine.at(material));
  }
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("MaterialService publishes editor-image sizes and re-probes changed images")
{
  const auto game = SizeGame{};
  auto options = MaterialServiceOptions{};
  options.makeSource = [path = game.path()](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
  };
  options.pollInterval = 50ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();

  const auto editor = std::string{"textures/lp1c/c21_editor"};
  const auto missing = std::string{"textures/lp1c/c21_missing"};
  static_cast<void>(service->lookup(editor));
  static_cast<void>(service->lookup(missing));
  auto key = MaterialImageKey{};
  key.name = "textures/lp1c/c21_diffuse";
  key.usage = UsageDiffuse;
  key.filter = FilterDefault;
  static_cast<void>(service->image(key));
  // S2.2: the sizes once measured (a lookup queues the measure)
  const auto table = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.find(editor) && t.find(editor)->sizeMeasured && t.find(missing)
             && t.find(missing)->sizeMeasured && t.findImage(key);
    },
    10s);
  REQUIRE(table->find(editor)->entry);
  CHECK(
    table->find(editor)->entry->editorImageSize
    == EditorImageSize{32, 16, State::Ready, {}});
  const auto& missingSize = table->find(missing)->entry->editorImageSize;
  CHECK(missingSize == EditorImageSize{8, 8, State::Default, {}});
  CHECK_FALSE(missingSize.reason.empty());
  const auto image = service->image(key);
  REQUIRE(image.state == ImageLookup::State::Source);
  CHECK(image.image->width == 8);

  // the files change: the size and the image slot are read again
  game.image("c21_editor", 64, 8);
  game.image("c21_diffuse", 16, 8);
  bumpModificationTime(game.base() / "textures" / "lp1c" / "c21_editor.tga");
  bumpModificationTime(game.base() / "textures" / "lp1c" / "c21_diffuse.tga");
  const auto changed = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.find(editor);
      const auto imageSlot = t.findImage(key);
      return slot && slot->entry && slot->entry->editorImageSize.width == 64 && imageSlot
             && imageSlot->image && imageSlot->image->width == 16;
    },
    10s);
  CHECK(
    changed->find(editor)->entry->editorImageSize
    == EditorImageSize{64, 8, State::Ready, {}});
  CHECK(service->image(key).image->width == 16);

  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
