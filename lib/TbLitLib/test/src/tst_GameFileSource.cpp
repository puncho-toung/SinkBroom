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
#include "lit/GameContainerIndex.h"
#include "lit/GameFile.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/ResourceContainer.h"

#include "kd/result.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

using Origin = ContainerTablesOrigin::Kind;

/**
 * LP6.3 (C67 (a)): the source as a scan makes it, or (viaIndex) as a container index
 * gives it from the tables a scan stored before, a memory hit; every case of the search
 * runs both ways, so no read, listing, probe or search order depends on where the tables
 * came from. A cancelled source's index finds nothing (a key check while the flag is set)
 */
std::unique_ptr<GameFileSource> sourceOf(
  const std::filesystem::path& gamePath,
  const bool viaIndex,
  const std::atomic<bool>* cancel = nullptr)
{
  if (!viaIndex)
  {
    return std::make_unique<GameFileSource>(gamePath, cancel);
  }
  auto index = GameContainerIndex{{}};
  const auto scanned = index.makeSource(gamePath, nullptr);
  REQUIRE(scanned->containerTablesOrigin().kind == Origin::Scan);
  REQUIRE(scanned->complete());
  auto source = index.makeSource(gamePath, cancel);
  CHECK(
    source->containerTablesOrigin().kind
    == (cancel && cancel->load() ? Origin::Scan : Origin::Memory));
  return source;
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
  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(game.path(), viaIndex);
  auto& source = *sourcePtr;

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

TEST_CASE("GameFileSource lists a folder dated ahead of the clock once (Y.3)")
{
  const auto env = fs::TestEnvironment{};
  const auto folder = env.dir() / "base" / "ahead";
  std::filesystem::create_directories(folder);
  auto ec = std::error_code{};
  std::filesystem::last_write_time(
    folder, std::filesystem::file_time_type::clock::now() + std::chrono::hours{1}, ec);
  REQUIRE_FALSE(ec);

  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(env.dir(), viaIndex);
  auto& source = *sourcePtr;
  CHECK_FALSE(source.probe("ahead/a.tga").exists());
  const auto listed = source.looseListingsForTesting();
  CHECK(listed >= 1u);
  for (auto round = 0; round < 3; ++round)
  {
    source.looseFilesMayHaveChanged();
    CHECK_FALSE(source.probe("ahead/a.tga").exists());
  }
  // its time did not change and lies ahead: never "recent", so never listed again
  CHECK(source.looseListingsForTesting() == listed);
}

TEST_CASE("GameFileSource with a corrupt container")
{
  const auto game = FakeGame{};
  writeTextFile(game.base() / "broken.resources", "not a container");
  // A3.12: a .pk4 that is no zip
  writeTextFile(game.base() / "broken.pk4", "not a zip either");

  // LP6.3: a hit replays the scan's warnings
  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(game.path(), viaIndex);
  auto& source = *sourcePtr;
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
  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(env.dir(), viaIndex);
  auto& source = *sourcePtr;
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

  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(game.path(), viaIndex);
  auto& source = *sourcePtr;
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
  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(game.path(), viaIndex, &cancel);
  auto& source = *sourcePtr;
  CHECK(source.searchOrder().empty());
  CHECK_FALSE(source.complete());
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

  const auto viaIndex = GENERATE(false, true);
  CAPTURE(viaIndex);
  const auto sourcePtr = sourceOf(game.path(), viaIndex);
  auto& source = *sourcePtr;
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

namespace
{

// LP6.3: what a read, a probe and a listing of the fake game find
const auto SearchNames = std::vector<std::string>{
  "materials/shared.mtr",
  "materials/Loose_Only.mtr",
  "materials/pk4_only.mtr",
  "materials/pk4_maps.mtr",
  "materials/both_pk4.mtr",
  "MATERIALS/UPPER_CASE.MTR",
  "materials/res_only.mtr",
  "materials/both_res.mtr",
  "materials/maps_wins.mtr",
  "materials/dup.mtr",
  "materials\\res_only.mtr",
  "materials/binary_only.mtr",
  "materials/sub/nested.mtr",
  "materials/none.mtr",
};

// LP6.3 (C67 (a)): every read, listing and probe of source finds what fresh's (a scan's)
// finds, and the search order is the same
void checkSameSearch(GameFileSource& source, GameFileSource& fresh)
{
  CHECK(source.searchOrder() == fresh.searchOrder());
  CHECK(source.containerCount() == fresh.containerCount());
  CHECK(source.usingResourceFiles() == fresh.usingResourceFiles());
  CHECK(source.containerEntryNamesForTesting() == fresh.containerEntryNamesForTesting());
  CHECK(source.listFiles("materials", ".mtr") == fresh.listFiles("materials", ".mtr"));
  CHECK(source.listFiles("def", ".def") == fresh.listFiles("def", ".def"));
  for (const auto& name : SearchNames)
  {
    CAPTURE(name);
    const auto probe = source.probe(name);
    const auto freshProbe = fresh.probe(name);
    CHECK(probe == freshProbe);
    CHECK(probe.containerStamp == freshProbe.containerStamp);
    CHECK(probe.containerSize == freshProbe.containerSize);
    auto time = std::int64_t{0};
    auto freshTime = std::int64_t{0};
    CHECK(source.readFile(name, &time) == fresh.readFile(name, &freshTime));
    CHECK(time == freshTime);
    auto found = GameFileProbe{};
    auto freshFound = GameFileProbe{};
    CHECK(source.readGameFile(name, &found) == fresh.readGameFile(name, &freshFound));
    CHECK(found == freshFound);
    CHECK(found.containerStamp == freshFound.containerStamp);
    CHECK(found.containerSize == freshFound.containerSize);
    auto head = GameFileProbe{};
    auto freshHead = GameFileProbe{};
    CHECK(
      source.readGameFileHead(name, 5, &head)
      == fresh.readGameFileHead(name, 5, &freshHead));
    CHECK(head == freshHead);
  }
  source.closeFiles();
  fresh.closeFiles();
  CHECK(source.openContainers() == 0u);
}

// the index folder's files: one per base folder, no temporary file left
std::vector<std::filesystem::path> indexFiles(const std::filesystem::path& folder)
{
  auto files = std::vector<std::filesystem::path>{};
  auto ec = std::error_code{};
  for (auto it = std::filesystem::directory_iterator{folder, ec};
       !ec && it != std::filesystem::directory_iterator{};
       it.increment(ec))
  {
    files.push_back(it->path());
  }
  return files;
}

std::string bytesOf(const std::filesystem::path& path)
{
  return readGameFile(path) | kdl::value();
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes)
{
  auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
  stream.write(bytes.data(), std::streamsize(bytes.size()));
}

} // namespace

TEST_CASE("GameContainerIndex (LP6.3)")
{
  const auto game = FakeGame{};
  // inside the test's folder, beside base (no listing of the game sees it)
  const auto folder = game.path() / "index";

  SECTION("a cold build, a memory hit, then a file hit in a new index object")
  {
    auto fresh = GameFileSource{game.path()};
    auto index = GameContainerIndex{folder};
    auto cold = index.makeSource(game.path(), nullptr);
    CHECK(cold->complete());
    const auto coldOrigin = cold->containerTablesOrigin();
    CHECK(coldOrigin.kind == Origin::Scan);
    CHECK(coldOrigin.keySeconds >= 0.0);
    CHECK(coldOrigin.writeSeconds > 0.0);
    CHECK(coldOrigin.fileBytes > 0u);
    REQUIRE(cold->scannedTables() != nullptr);
    CHECK(index.memoryEntriesForTesting() == 1u);
    const auto files = indexFiles(folder);
    REQUIRE(files.size() == 1u);
    CHECK(files[0].filename().string().starts_with("containers-"));
    CHECK(files[0].extension() == ".idx");
    CHECK(std::filesystem::file_size(files[0]) == coldOrigin.fileBytes);
    CHECK(index.filePath(cold->scannedTables()->key.base) == files[0]);
    checkSameSearch(*cold, fresh);
    CHECK(cold->takeWarnings().empty());

    // the memory copy: the same tables, nothing written again
    auto memory = index.makeSource(game.path(), nullptr);
    CHECK(memory->complete());
    CHECK(memory->containerTablesOrigin().kind == Origin::Memory);
    CHECK(memory->containerTablesOrigin().writeSeconds == 0.0);
    CHECK(memory->scannedTables() == nullptr);
    checkSameSearch(*memory, fresh);

    // a new index object (a new launch): its memory empty, the file read, then kept
    auto other = GameContainerIndex{folder};
    auto file = other.makeSource(game.path(), nullptr);
    CHECK(file->complete());
    CHECK(file->containerTablesOrigin().kind == Origin::File);
    CHECK(file->containerTablesOrigin().fileBytes == coldOrigin.fileBytes);
    CHECK(other.memoryEntriesForTesting() == 1u);
    checkSameSearch(*file, fresh);
    CHECK(file->takeWarnings().empty());
    CHECK(
      other.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::Memory);

    // the memory copy dropped (P8.18's file Start): the file again
    index.dropMemoryForTesting();
    CHECK(index.memoryEntriesForTesting() == 0u);
    CHECK(
      index.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::File);
    CHECK(indexFiles(folder).size() == 1u);
  }

  SECTION("a changed, added, removed or renamed container rebuilds the key's tables")
  {
    auto index = GameContainerIndex{folder};
    static_cast<void>(index.makeSource(game.path(), nullptr));
    const auto rebuilt = [&](const std::string& label) {
      CAPTURE(label);
      auto source = index.makeSource(game.path(), nullptr);
      CHECK(source->containerTablesOrigin().kind == Origin::Scan);
      // all-or-nothing: the base's tables replaced whole, one entry, one file
      CHECK(index.memoryEntriesForTesting() == 1u);
      CHECK(indexFiles(folder).size() == 1u);
      auto other = GameContainerIndex{folder};
      auto file = other.makeSource(game.path(), nullptr);
      CHECK(file->containerTablesOrigin().kind == Origin::File);
      auto fresh = GameFileSource{game.path()};
      checkSameSearch(*source, fresh);
      checkSameSearch(*file, fresh);
      CHECK(
        index.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
        == Origin::Memory);
    };

    // a .pk4 rewritten: its size and stamp
    writePk4(
      game.base() / "zz_second.pk4",
      {
        {"materials/both_pk4.mtr",
         materialText("textures/gds/both_pk4", "textures/gds/both_pk4_rewritten")},
        {"Materials/Upper_Case.MTR",
         materialText("textures/gds/upper_case", "textures/gds/upper_case_pk4")},
        {"materials/pk4_maps.mtr",
         materialText("textures/gds/pk4_maps", "textures/gds/pk4_maps_pk4")},
      });
    bumpModificationTime(game.base() / "zz_second.pk4");
    rebuilt("rewritten");
    CHECK(contains(
      index.makeSource(game.path(), nullptr)->readFile("materials/both_pk4.mtr", nullptr),
      "both_pk4_rewritten"));

    // the same bytes, a later stamp
    bumpModificationTime(game.base() / "second.resources");
    rebuilt("restamped");

    writePk4(
      game.base() / "mm_added.pk4",
      {{"materials/added.mtr",
        materialText("textures/gds/added", "textures/gds/added_pk4")}});
    rebuilt("added");
    auto added = index.makeSource(game.path(), nullptr);
    CHECK(contains(added->readFile("materials/added.mtr", nullptr), "added_pk4"));
    added.reset();

    std::filesystem::remove(game.base() / "mm_added.pk4");
    rebuilt("removed");
    CHECK_FALSE(
      index.makeSource(game.path(), nullptr)->readFile("materials/added.mtr", nullptr));

    // the same file under another name
    std::filesystem::rename(
      game.base() / "second.resources", game.base() / "third.resources");
    rebuilt("renamed");
    const auto order = index.makeSource(game.path(), nullptr)->searchOrder();
    CHECK(std::ranges::count(order, "third.resources") == 1);
    CHECK(std::ranges::count(order, "second.resources") == 0);
  }

  SECTION("another base folder never finds another's tables")
  {
    // a copy of the game: the same names, sizes and (copied) stamps, another folder
    const auto copy = game.path() / "copy";
    std::filesystem::create_directories(copy / "base");
    std::filesystem::copy(
      game.base(), copy / "base", std::filesystem::copy_options::recursive);
    auto index = GameContainerIndex{folder};
    static_cast<void>(index.makeSource(game.path(), nullptr));
    auto copied = index.makeSource(copy, nullptr);
    CHECK(copied->containerTablesOrigin().kind == Origin::Scan);
    CHECK(index.memoryEntriesForTesting() == 2u);
    CHECK(indexFiles(folder).size() == 2u);
    CHECK(
      index.makeSource(copy, nullptr)->containerTablesOrigin().kind == Origin::Memory);

    // the copy's tables open the copy's containers: the original's moved away, the
    // copy reads on
    std::filesystem::rename(
      game.base() / "_first.resources", game.base() / "_first.gone");
    std::filesystem::rename(game.base() / "aa_first.pk4", game.base() / "aa_first.gone");
    auto fromCopy = index.makeSource(copy, nullptr);
    CHECK(fromCopy->containerTablesOrigin().kind == Origin::Memory);
    CHECK(
      contains(fromCopy->readFile("materials/res_only.mtr", nullptr), "res_only_res"));
    CHECK(
      contains(fromCopy->readFile("materials/pk4_only.mtr", nullptr), "pk4_only_pk4"));
    CHECK(fromCopy->takeWarnings().empty());
    // and the original's key moved: a scan, without the moved containers
    auto original = index.makeSource(game.path(), nullptr);
    CHECK(original->containerTablesOrigin().kind == Origin::Scan);
    CHECK_FALSE(original->readFile("materials/res_only.mtr", nullptr));
  }

  SECTION("a cancelled scan stores nothing")
  {
    auto index = GameContainerIndex{folder};
    auto cancel = std::atomic<bool>{false};
    auto seen = std::vector<std::string>{};
    // the stop between the first container's table and the second's
    index.setAfterContainerForTesting([&](const std::string& name) {
      seen.push_back(name);
      cancel = true;
    });
    auto cancelled = index.makeSource(game.path(), &cancel);
    CHECK_FALSE(cancelled->complete());
    CHECK(seen.size() == 1u);
    // what it scanned so far (G4), never stored
    CHECK(cancelled->containerCount() == 1u);
    CHECK(cancelled->scannedTables() == nullptr);
    CHECK(index.memoryEntriesForTesting() == 0u);
    CHECK_FALSE(std::filesystem::exists(folder));

    // a stop in the .pk4 part, after the first directory
    seen.clear();
    cancel = false;
    index.setAfterContainerForTesting([&](const std::string& name) {
      seen.push_back(name);
      if (name.ends_with(".pk4"))
      {
        cancel = true;
      }
    });
    auto late = index.makeSource(game.path(), &cancel);
    CHECK_FALSE(late->complete());
    CHECK(seen.back() == "aa_first.pk4");
    CHECK(late->containerCount() == 4u);
    CHECK(index.memoryEntriesForTesting() == 0u);
    CHECK_FALSE(std::filesystem::exists(folder));

    // complete, then stored; a key check while the flag is set finds nothing
    index.setAfterContainerForTesting({});
    cancel = false;
    CHECK(index.makeSource(game.path(), &cancel)->complete());
    CHECK(index.memoryEntriesForTesting() == 1u);
    CHECK(indexFiles(folder).size() == 1u);
    cancel = true;
    const auto stopped = index.makeSource(game.path(), &cancel);
    CHECK(stopped->containerTablesOrigin().kind == Origin::Scan);
    CHECK(stopped->containerCount() == 0u);
    CHECK_FALSE(stopped->complete());
  }

  SECTION("a damaged, mis-versioned, missing or stale file rebuilds")
  {
    {
      auto index = GameContainerIndex{folder};
      static_cast<void>(index.makeSource(game.path(), nullptr));
    }
    const auto files = indexFiles(folder);
    REQUIRE(files.size() == 1u);
    const auto file = files[0];
    const auto good = bytesOf(file);
    const auto rebuildsWith = [&](const std::string& bytes, const bool warned) {
      writeBytes(file, bytes);
      auto index = GameContainerIndex{folder};
      auto source = index.makeSource(game.path(), nullptr);
      CHECK(source->containerTablesOrigin().kind == Origin::Scan);
      const auto warnings = source->takeWarnings();
      CHECK(warnings.size() == (warned ? 1u : 0u));
      if (warned && !warnings.empty())
      {
        CHECK_THAT(warnings.front(), Catch::Matchers::ContainsSubstring("is damaged"));
      }
      // the scan's tables written whole again, read by the next launch
      CHECK(bytesOf(file) == good);
      auto again = GameContainerIndex{folder};
      CHECK(
        again.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
        == Origin::File);
    };

    SECTION("truncated")
    {
      rebuildsWith(good.substr(0, good.size() / 2), true);
    }
    SECTION("empty")
    {
      rebuildsWith({}, true);
    }
    SECTION("another version")
    {
      auto other = good;
      other[8] = char(ContainerIndexFileVersion + 1);
      rebuildsWith(other, false);
    }
    SECTION("the key tampered")
    {
      // a unit of the base path, the payload's first field
      auto tampered = good;
      tampered[24 + 4] = char(tampered[24 + 4] ^ 0x20);
      rebuildsWith(tampered, true);
    }
    SECTION("missing")
    {
      std::filesystem::remove(file);
      auto index = GameContainerIndex{folder};
      auto source = index.makeSource(game.path(), nullptr);
      CHECK(source->containerTablesOrigin().kind == Origin::Scan);
      CHECK(source->takeWarnings().empty());
      CHECK(bytesOf(file) == good);
    }
    SECTION("stale: whole, of another key")
    {
      bumpModificationTime(game.base() / "_first.resources");
      auto index = GameContainerIndex{folder};
      auto source = index.makeSource(game.path(), nullptr);
      CHECK(source->containerTablesOrigin().kind == Origin::Scan);
      CHECK(source->takeWarnings().empty());
      CHECK(bytesOf(file) != good);
      CHECK(indexFiles(folder).size() == 1u);
    }
  }

  SECTION("the file's tables are read whole or not at all")
  {
    auto index = GameContainerIndex{{}};
    const auto source = index.makeSource(game.path(), nullptr);
    const auto tables = source->scannedTables();
    REQUIRE(tables != nullptr);
    const auto bytes = writeContainerIndexFile(*tables);

    // a round trip: every table as the scan read it
    const auto read = readContainerIndexFile(bytes, tables->key);
    REQUIRE(read);
    CHECK(read->key == tables->key);
    CHECK(read->warnings == tables->warnings);
    REQUIRE(read->resources.size() == tables->resources.size());
    for (std::size_t i = 0; i < read->resources.size(); ++i)
    {
      const auto& a = *read->resources[i];
      const auto& b = *tables->resources[i];
      CHECK(a.name() == b.name());
      CHECK(a.path() == b.path());
      CHECK(a.fileSize() == b.fileSize());
      CHECK(a.fileTime() == b.fileTime());
      REQUIRE(a.entries().size() == b.entries().size());
      for (std::size_t k = 0; k < a.entries().size(); ++k)
      {
        CHECK(a.entries()[k].name == b.entries()[k].name);
        CHECK(a.entries()[k].offset == b.entries()[k].offset);
        CHECK(a.entries()[k].length == b.entries()[k].length);
      }
    }
    REQUIRE(read->pk4s.size() == tables->pk4s.size());
    for (std::size_t i = 0; i < read->pk4s.size(); ++i)
    {
      CHECK(read->pk4s[i]->name == tables->pk4s[i]->name);
      CHECK(read->pk4s[i]->path == tables->pk4s[i]->path);
      CHECK(read->pk4s[i]->names == tables->pk4s[i]->names);
      CHECK(read->pk4s[i]->index == tables->pk4s[i]->index);
      CHECK(read->pk4s[i]->stamp == tables->pk4s[i]->stamp);
      CHECK(read->pk4s[i]->size == tables->pk4s[i]->size);
    }

    // every shorter file and every one-bit change is refused, never a partial table
    auto prefixesRead = 0;
    for (std::size_t n = 0; n < bytes.size(); ++n)
    {
      prefixesRead += readContainerIndexFile(bytes.substr(0, n), tables->key) ? 1 : 0;
    }
    CHECK(prefixesRead == 0);
    auto flipsRead = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
      auto flipped = bytes;
      flipped[i] = char(flipped[i] ^ 0x01);
      flipsRead += readContainerIndexFile(flipped, tables->key) ? 1 : 0;
    }
    CHECK(flipsRead == 0);
    // one byte more
    CHECK_FALSE(readContainerIndexFile(bytes + "x", tables->key));

    // another key: refused, not damaged
    auto otherKey = tables->key;
    otherKey.pk4s.back().stamp += 1;
    auto error = std::string{};
    auto damaged = true;
    CHECK_FALSE(readContainerIndexFile(bytes, otherKey, &error, &damaged));
    CHECK_FALSE(damaged);

    // tables their key does not list, or with another stamp than their key's: refused
    auto unlisted = *tables;
    unlisted.key.resources.erase(unlisted.key.resources.begin());
    CHECK_FALSE(readContainerIndexFile(
      writeContainerIndexFile(unlisted), unlisted.key, &error, &damaged));
    CHECK(damaged);
    auto restamped = *tables;
    restamped.key.pk4s.front().stamp += 1;
    CHECK_FALSE(readContainerIndexFile(
      writeContainerIndexFile(restamped), restamped.key, &error, &damaged));
    CHECK(damaged);
  }

  SECTION("a file hit replays the scan's warnings")
  {
    writeTextFile(game.base() / "broken.resources", "not a container");
    writeTextFile(game.base() / "broken.pk4", "not a zip either");
    auto index = GameContainerIndex{folder};
    const auto scanned = index.makeSource(game.path(), nullptr)->takeWarnings();
    REQUIRE(scanned.size() == 2u);
    auto other = GameContainerIndex{folder};
    auto file = other.makeSource(game.path(), nullptr);
    CHECK(file->containerTablesOrigin().kind == Origin::File);
    CHECK(file->takeWarnings() == scanned);
    // once each: taken, they are gone
    CHECK(file->takeWarnings().empty());
    CHECK(index.makeSource(game.path(), nullptr)->takeWarnings() == scanned);
  }

  SECTION("a relative game path is the same base as the absolute one")
  {
    const auto relative = std::filesystem::relative(game.path());
    REQUIRE_FALSE(relative.empty());
    REQUIRE(relative.is_relative());
    auto index = GameContainerIndex{folder};
    auto source = index.makeSource(relative, nullptr);
    CHECK(source->containerTablesOrigin().kind == Origin::Scan);
    REQUIRE(source->scannedTables() != nullptr);
    CHECK(source->scannedTables()->key.base.is_absolute());
    auto fresh = GameFileSource{game.path()};
    checkSameSearch(*source, fresh);
    CHECK(
      index.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::Memory);
    CHECK(
      index.makeSource(game.base() / "..", nullptr)->containerTablesOrigin().kind
      == Origin::Memory);
    CHECK(index.memoryEntriesForTesting() == 1u);
    CHECK(indexFiles(folder).size() == 1u);
  }

  SECTION("two indexes read and write one file")
  {
    auto first = GameContainerIndex{folder};
    auto second = GameContainerIndex{folder};
    static_cast<void>(first.makeSource(game.path(), nullptr));
    CHECK(
      second.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::File);
    // a change: the second rebuilds and writes, the first finds its own key moved and
    // reads the second's file
    bumpModificationTime(game.base() / "aa_first.pk4");
    CHECK(
      second.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::Scan);
    CHECK(
      first.makeSource(game.path(), nullptr)->containerTablesOrigin().kind
      == Origin::File);
    CHECK(indexFiles(folder).size() == 1u);
  }
}

} // namespace tb::lit
