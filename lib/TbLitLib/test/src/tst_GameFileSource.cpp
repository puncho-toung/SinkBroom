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

#include "LitTestUtils.h"
#include "TestEnvironment.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFile.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using Files = std::vector<std::pair<std::string, std::string>>;

/**
 * A game folder with loose files, three .pk4 files (one a binary pak) and three
 * .resources files, two in base and one in base/maps, whose copies of the same names tell
 * by their image which source a read found. Retail data never puts one name in two kinds
 * of container (G8), so every rule of the engine's search order has a name here:
 *
 * - loose over .pk4 over .resources: shared (all three)
 * - .pk4 over base .resources: pk4_only; .pk4 over base/maps .resources: pk4_maps
 * - the last .pk4 registered (sorted) first: both_pk4
 * - the last base .resources registered first: both_res
 * - base/maps .resources over base .resources: maps_wins
 * - inside one container, the later of two entries: dup
 * - a binary pak is refused: binary_only
 * - a .pk4's entry name lowercased, its folders kept: upper_case, nested
 */
class FakeGame
{
private:
  fs::TestEnvironment m_folder;

public:
  FakeGame()
  {
    const auto base = path() / "base";
    std::filesystem::copy(
      getFixtureRoot() / "test" / "lit" / "GameFileSource",
      path(),
      std::filesystem::copy_options::recursive);

    const auto mtr = [](const std::string& name, const std::string& image) {
      return materialText("textures/gds/" + name, "textures/gds/" + image);
    };

    writePk4(
      base / "aa_first.pk4",
      {
        {"materials/shared.mtr", mtr("shared", "shared_pk4first")},
        {"materials/both_pk4.mtr", mtr("both_pk4", "both_pk4_first")},
        {"materials/pk4_only.mtr", mtr("pk4_only", "pk4_only_pk4")},
        {"materials/sub/nested.mtr", mtr("nested", "nested_pk4")},
      });
    writePk4(
      base / "zz_second.pk4",
      {
        {"materials/both_pk4.mtr", mtr("both_pk4", "both_pk4_second")},
        {"Materials/Upper_Case.MTR", mtr("upper_case", "upper_case_pk4")},
        {"materials/pk4_maps.mtr", mtr("pk4_maps", "pk4_maps_pk4")},
      });
    // a binary pak, which Init refuses
    writePk4(
      base / "binary.pk4",
      {
        {"binary.conf", "binary\n"},
        {"materials/binary_only.mtr", mtr("binary_only", "binary_only_pk4")},
      });

    writeTextFile(
      base / "_first.resources",
      resourcesBytes({
        {"materials/shared.mtr", mtr("shared", "shared_res")},
        {"materials/res_only.mtr", mtr("res_only", "res_only_res")},
        {"materials/pk4_only.mtr", mtr("pk4_only", "pk4_only_res")},
        {"materials/both_res.mtr", mtr("both_res", "both_res_first")},
      }));
    writeTextFile(
      base / "second.resources",
      resourcesBytes({
        {"materials/both_res.mtr", mtr("both_res", "both_res_second")},
        {"materials/dup.mtr", mtr("dup", "dup_one")},
        {"materials/dup.mtr", mtr("dup", "dup_two")},
        {"materials/maps_wins.mtr", mtr("maps_wins", "maps_wins_second")},
      }));
    writeTextFile(
      base / "maps" / "map1.resources",
      resourcesBytes({
        {"materials/maps_wins.mtr", mtr("maps_wins", "maps_wins_maps")},
        {"materials/pk4_maps.mtr", mtr("pk4_maps", "pk4_maps_maps")},
      }));
  }

  const std::filesystem::path& path() const { return m_folder.dir(); }
  std::filesystem::path base() const { return path() / "base"; }
};

bool contains(const std::optional<std::string>& text, const std::string& part)
{
  return text && text->find(part) != std::string::npos;
}

std::string stageImage(MaterialDecls& decls, const std::string& name)
{
  auto result = decls.lookup(name);
  INFO(result.error);
  REQUIRE(result);
  for (const auto& stage : (*result.value)->stages)
  {
    if (stage.image && stage.lighting == 2)
    {
      return stage.image->name;
    }
  }
  return {};
}

} // namespace

TEST_CASE("GameFileSource")
{
  const auto game = FakeGame{};
  auto source = GameFileSource{game.path()};

  SECTION("registers the containers in the engine's order and searches them backwards")
  {
    // the .pk4 files sorted, then the .resources of base and of base/maps; a read
    // searches each kind from its last registered container, and skips the binary pak
    CHECK(
      source.searchOrder()
      == std::vector<std::string>{
        "zz_second.pk4",
        "aa_first.pk4",
        "maps/map1.resources",
        "second.resources",
        "_first.resources",
      });
    CHECK(source.takeWarnings().empty());
  }

  SECTION("a listing merges every source, flat, each name once")
  {
    auto expected = std::vector<std::string>{
      "both_pk4.mtr",
      "both_res.mtr",
      "dup.mtr",
      "Loose_Only.mtr",
      "maps_wins.mtr",
      "pk4_maps.mtr",
      "pk4_only.mtr",
      "res_only.mtr",
      "shared.mtr",
      // stored as Materials/Upper_Case.MTR; the engine lowercases a container's names
      "upper_case.mtr",
    };

#if defined(_WIN32)
    // Sys_ListFiles recurses on Windows, so a loose file below the folder is listed; the
    // engine sorts folders first
    writeTextFile(
      game.base() / "materials" / "sub" / "deep.mtr",
      materialText("textures/gds/deep", "textures/gds/deep_loose"));
    expected.insert(expected.begin(), "sub/deep.mtr");
#endif

    // the .pk4 file's materials/sub/nested.mtr is not listed: containers list flat
    CHECK(sortedEnginePaths(source.listFiles("materials", ".mtr")) == expected);
    CHECK(source.listFiles("def", ".def").empty());
  }

  SECTION("a read takes the loose file, then the .pk4 files, then the resources")
  {
    auto time = std::int64_t{0};
    CHECK(contains(source.readFile("materials/shared.mtr", &time), "shared_loose"));
    CHECK(time != 0);
    CHECK(source.probe("materials/shared.mtr").kind == GameFileProbe::Kind::Loose);

    CHECK(contains(source.readFile("materials/pk4_only.mtr", &time), "pk4_only_pk4"));
    CHECK(time == 0);
    CHECK(source.probe("materials/pk4_only.mtr").container == "aa_first.pk4");
    CHECK(contains(source.readFile("materials/pk4_maps.mtr", &time), "pk4_maps_pk4"));
    CHECK(source.probe("materials/pk4_maps.mtr").container == "zz_second.pk4");

    // the last registered .pk4 first
    CHECK(contains(source.readFile("materials/both_pk4.mtr", &time), "both_pk4_second"));
    CHECK(contains(source.readFile("MATERIALS/UPPER_CASE.MTR", &time), "upper_case_pk4"));

    // the resources: base/maps, then base backwards; in one container the later entry
    CHECK(contains(source.readFile("materials/res_only.mtr", &time), "res_only_res"));
    CHECK(contains(source.readFile("materials/both_res.mtr", &time), "both_res_second"));
    CHECK(contains(source.readFile("materials/maps_wins.mtr", &time), "maps_wins_maps"));
    CHECK(source.probe("materials/maps_wins.mtr").container == "maps/map1.resources");
    CHECK(contains(source.readFile("materials/dup.mtr", &time), "dup_two"));
    CHECK(contains(source.readFile("materials\\res_only.mtr", &time), "res_only_res"));

    // a binary pak is never searched; a subfolder entry is still read by name
    CHECK_FALSE(source.readFile("materials/binary_only.mtr", &time));
    CHECK(contains(source.readFile("materials/sub/nested.mtr", &time), "nested_pk4"));

    // OpenFileReadFlags: a leading slash is dropped; a path that backs up, an OS path or
    // nothing finds nothing
    CHECK(contains(source.readFile("/materials/shared.mtr", &time), "shared_loose"));
    CHECK_FALSE(source.readFile("../base/materials/shared.mtr", &time));
    CHECK_FALSE(source.readFile("c:/materials/res_only.mtr", &time));
    CHECK_FALSE(source.readFile("", &time));
    CHECK_FALSE(source.readFile("materials/none.mtr", &time));
    CHECK(source.takeWarnings().empty());
  }

  SECTION("a probe finds what a read finds, without reading it (A5, A4)")
  {
    const auto loose = source.probe("materials/shared.mtr");
    CHECK(loose.kind == GameFileProbe::Kind::Loose);
    CHECK(loose.container.empty());
    const auto stat = statGameFile(game.base() / "materials" / "shared.mtr");
    REQUIRE(stat);
    CHECK(loose.time == stat->unixSeconds);
    CHECK(loose.changeStamp == stat->time);
    CHECK(loose.size == stat->size);

    // a container's file has time 0; a .pk4 entry's size is not known without inflating
    // it
    CHECK(
      source.probe("materials/pk4_only.mtr")
      == GameFileProbe{GameFileProbe::Kind::Pk4, "aa_first.pk4", 0, 0, 0});
    const auto resText = source.readFile("materials/res_only.mtr", nullptr);
    REQUIRE(resText);
    source.closeFiles();
    CHECK(
      source.probe("materials/res_only.mtr")
      == GameFileProbe{
        GameFileProbe::Kind::Resources, "_first.resources", 0, resText->size(), 0});
    CHECK(source.probe("MATERIALS/MAPS_WINS.MTR").container == "maps/map1.resources");

    // nothing: FILE_NOT_FOUND_TIMESTAMP
    CHECK(source.probe("materials/none.mtr") == GameFileProbe{});
    CHECK(source.probe("materials/none.mtr").time == -1);
    CHECK_FALSE(source.probe("../base/materials/shared.mtr").exists());
    CHECK_FALSE(source.probe("c:/materials/res_only.mtr").exists());
    CHECK_FALSE(source.probe("materials/binary_only.mtr").exists());

    // no file body was read, so no container was opened
    CHECK(source.openContainers() == 0u);
  }

  SECTION("readGameFile reads any game file, and says where it found it")
  {
    auto found = GameFileProbe{};
    CHECK(contains(source.readGameFile("materials/shared.mtr", &found), "shared_loose"));
    CHECK(found == source.probe("materials/shared.mtr"));
    CHECK(
      contains(source.readGameFile("materials/pk4_maps.mtr", &found), "pk4_maps_pk4"));
    CHECK(found.kind == GameFileProbe::Kind::Pk4);
    CHECK(found.container == "zz_second.pk4");
    CHECK(found.time == 0);
    CHECK(contains(source.readGameFile("materials/dup.mtr", &found), "dup_two"));
    CHECK(found.kind == GameFileProbe::Kind::Resources);
    CHECK_FALSE(source.readGameFile("materials/none.mtr", &found));
    CHECK(found == GameFileProbe{});

    // A4: a decl read's time is the engine's, Unix seconds
    auto time = std::int64_t{0};
    CHECK(source.readFile("materials/shared.mtr", &time));
    CHECK(time == source.probe("materials/shared.mtr").time);
    CHECK_FALSE(source.changedWhileRead());
  }

  SECTION("a file path below the base folder has a game-relative path")
  {
    const auto map = (game.base() / "maps" / "sub" / "x.map").string();
    CHECK(source.gameRelativePath(map) == "maps/sub/x.map");
#if defined(_WIN32)
    // the file system ignores case
    auto upper = game.base().string();
    std::transform(upper.begin(), upper.end(), upper.begin(), [](const char c) {
      return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    });
    CHECK(source.gameRelativePath(upper + "\\MAPS\\x.map") == "MAPS/x.map");
#endif
    CHECK_FALSE(source.gameRelativePath((game.path() / "x.map").string()));
    CHECK_FALSE(source.gameRelativePath(game.base().string()));
    CHECK_FALSE(source.gameRelativePath("maps/x.map"));
  }

  SECTION("resource files are in use (UsingResourceFiles)")
  {
    CHECK(source.usingResourceFiles());
  }

  SECTION("a read batch keeps containers open until closeFiles")
  {
    CHECK(source.openContainers() == 0u);
    CHECK(source.readFile("materials/res_only.mtr", nullptr));
    CHECK(source.readFile("materials/pk4_only.mtr", nullptr));
    CHECK(source.openContainers() == 2u);

#if defined(_WIN32)
    // the open container shares delete access: the user may replace it (R4-3.26); the
    // batch goes on reading the file it opened, whose table it has (LP1c fix S2.2: the
    // container is statted once per open), and the next batch refuses to read from the
    // stale table
    const auto container = game.base() / "_first.resources";
    auto ec = std::error_code{};
    std::filesystem::rename(container, game.base() / "_first.old", ec);
    CHECK_FALSE(ec);
    CHECK(source.readFile("materials/res_only.mtr", nullptr));
    source.closeFiles();
    CHECK_FALSE(source.readFile("materials/res_only.mtr", nullptr));
    CHECK_THAT(
      source.takeWarnings().front(),
      Catch::Matchers::ContainsSubstring("changed since its table was read"));
#endif

    source.closeFiles();
    CHECK(source.openContainers() == 0u);
  }

  SECTION("loose file times for polling")
  {
    const auto times = source.looseFileTimes("materials", ".mtr");
    REQUIRE(times.size() == 2u);
    CHECK(times[0].path == "materials/Loose_Only.mtr");
    CHECK(times[1].path == "materials/shared.mtr");
    CHECK(times[1].time != 0);

    const auto file = game.base() / "materials" / "shared.mtr";
    writeTextFile(file, materialText("textures/gds/shared", "textures/gds/edited"));
    bumpModificationTime(file);
    const auto later = source.looseFileTimes("materials", ".mtr");
    REQUIRE(later.size() == 2u);
    CHECK(later[1] != times[1]);
  }

  SECTION("the registry reads the game's decls through it")
  {
    auto decls = MaterialDecls{source};
    source.closeFiles();
    CHECK(source.openContainers() == 0u);

    CHECK(stageImage(decls, "textures/gds/shared") == "textures/gds/shared_loose");
    CHECK(stageImage(decls, "textures/gds/looseOnly") == "textures/gds/looseOnly_loose");
    CHECK(stageImage(decls, "textures/gds/pk4_only") == "textures/gds/pk4_only_pk4");
    CHECK(stageImage(decls, "textures/gds/pk4_maps") == "textures/gds/pk4_maps_pk4");
    CHECK(stageImage(decls, "textures/gds/both_pk4") == "textures/gds/both_pk4_second");
    CHECK(stageImage(decls, "textures/gds/upper_case") == "textures/gds/upper_case_pk4");
    CHECK(stageImage(decls, "textures/gds/res_only") == "textures/gds/res_only_res");
    CHECK(stageImage(decls, "textures/gds/both_res") == "textures/gds/both_res_second");
    CHECK(stageImage(decls, "textures/gds/maps_wins") == "textures/gds/maps_wins_maps");
    CHECK(stageImage(decls, "textures/gds/dup") == "textures/gds/dup_two");

    // the file name as the listing spelled it, and the folder's flat listing: the nested
    // .pk4 material is not read
    const auto looseOnly = decls.lookup("textures/gds/looseOnly");
    REQUIRE(looseOnly);
    CHECK((*looseOnly.value)->sourceFile == "materials/Loose_Only.mtr");
    const auto nested = decls.lookup("textures/gds/nested");
    REQUIRE(nested);
    CHECK((*nested.value)->implicit);

    CHECK(decls.declFileCount() == 10);
    CHECK(decls.declFolders().size() == 8u);
  }
}

TEST_CASE("GameFileSource with a corrupt container")
{
  const auto game = FakeGame{};
  writeTextFile(game.base() / "broken.resources", "not a container");
  // A3.12: a .pk4 that is no zip
  writeTextFile(game.base() / "broken.pk4", "not a zip either");

  auto source = GameFileSource{game.path()};
  const auto warnings = source.takeWarnings();
  REQUIRE(warnings.size() == 2u);
  CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("broken.resources"));
  CHECK_THAT(
    warnings[1],
    Catch::Matchers::ContainsSubstring("Unable to open zip file broken.pk4"));
  const auto order = source.searchOrder();
  CHECK(std::count(order.begin(), order.end(), "broken.resources") == 0);
  CHECK(std::count(order.begin(), order.end(), "broken.pk4") == 0);
  CHECK(contains(source.readFile("materials/res_only.mtr", nullptr), "res_only_res"));
  CHECK(contains(source.readFile("materials/pk4_only.mtr", nullptr), "pk4_only_pk4"));
}

TEST_CASE("GameFileSource without resource files")
{
  const auto env = fs::TestEnvironment{};
  writeTextFile(env.dir() / "base" / "materials" / "a.mtr", "a");
  writePk4(env.dir() / "base" / "one.pk4", {{"materials/b.mtr", "b"}});
  auto source = GameFileSource{env.dir()};
  CHECK_FALSE(source.usingResourceFiles());
  CHECK(source.containerCount() == 1u);
}

TEST_CASE("GameFileSource indexes a .pk4 entry by the name the zip stores (A3.13)")
{
  const auto game = FakeGame{};
  // a UTF-8 name, stored as its bytes; the engine compares the bytes, lowercasing ASCII
  const auto name = std::string{"caf\xC3\xA9.mtr"};
  writePk4(
    game.base() / "utf8.pk4",
    {{"materials/" + name, materialText("textures/gds/cafe", "textures/gds/cafe_pk4")}});
  writeTextFile(
    game.base() / "materials" / *pathFromUtf8(name),
    materialText("textures/gds/cafe", "textures/gds/cafe_loose"));

  auto source = GameFileSource{game.path()};
  const auto names = source.listFiles("materials", ".mtr");
  // listed once: the container's name and the loose one are the same bytes
  CHECK(std::count(names.begin(), names.end(), name) == 1);
  CHECK(source.probe("materials/CAF\xC3\xA9.mtr").kind == GameFileProbe::Kind::Loose);
  std::filesystem::remove(game.base() / "materials" / *pathFromUtf8(name));
  CHECK(source.probe("materials/CAF\xC3\xA9.mtr").container == "utf8.pk4");
  CHECK(contains(source.readFile("materials/" + name, nullptr), "cafe_pk4"));
  CHECK(source.takeWarnings().empty());
}

TEST_CASE("GameFileSource stops its scan once cancelled")
{
  // G4: nothing is scanned or listed after the flag is set
  const auto game = FakeGame{};
  const auto cancel = std::atomic<bool>{true};
  auto source = GameFileSource{game.path(), &cancel};
  CHECK(source.searchOrder().empty());
  CHECK(source.listFiles("materials", ".mtr").empty());
  CHECK(source.takeWarnings().empty());
}

#if defined(_WIN32)
TEST_CASE("GameFileSource skips a file name that is not valid Unicode")
{
  // G3: a lone UTF-16 surrogate never throws; the file is skipped with a warning
  const auto game = FakeGame{};
  const auto bad = game.base() / "materials" / std::filesystem::path{L"bad\xD800.mtr"};
  writeTextFile(bad, materialText("textures/gds/bad", "textures/gds/bad"));
  REQUIRE(std::filesystem::exists(bad));

  auto source = GameFileSource{game.path()};
  const auto names = source.listFiles("materials", ".mtr");
  CHECK(std::none_of(names.begin(), names.end(), [](const auto& name) {
    return name.rfind("bad", 0) == 0;
  }));
  const auto warnings = source.takeWarnings();
  REQUIRE(warnings.size() == 1u);
  CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("is not valid Unicode"));

  auto decls = MaterialDecls{source};
  CHECK(decls.constructionError().empty());
  CHECK(decls.declFileCount() == 10);
}
#endif

} // namespace tb::lit
