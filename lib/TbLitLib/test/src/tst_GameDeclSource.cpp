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
#include "lit/GameDeclSource.h"
#include "lit/GameFile.h"
#include "lit/MaterialDecls.h"
#include "lit/ResourceContainer.h"

#include <algorithm>
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
 * by their image which source a read found.
 */
class FakeGame
{
private:
  TempFolder m_folder{"tb-lit-game"};

public:
  FakeGame()
  {
    const auto base = path() / "base";
    std::filesystem::copy(
      getFixtureRoot() / "test" / "lit" / "GameDeclSource",
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
      }));
  }

  const std::filesystem::path& path() const { return m_folder.path(); }
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

void writeAt(const std::filesystem::path& path, const std::string& bytes)
{
  writeTextFile(path, bytes);
}

void setBig32(std::string& bytes, const std::size_t at, const std::uint32_t value)
{
  bytes[at] = char((value >> 24) & 0xFF);
  bytes[at + 1] = char((value >> 16) & 0xFF);
  bytes[at + 2] = char((value >> 8) & 0xFF);
  bytes[at + 3] = char(value & 0xFF);
}

} // namespace

TEST_CASE("GameDeclSource")
{
  const auto game = FakeGame{};
  auto source = GameDeclSource{game.path()};

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
    CHECK(source.origin("materials/shared.mtr")->kind == GameFileOrigin::Kind::Loose);

    CHECK(contains(source.readFile("materials/pk4_only.mtr", &time), "pk4_only_pk4"));
    CHECK(time == 0);
    CHECK(source.origin("materials/pk4_only.mtr")->container == "aa_first.pk4");

    // the last registered .pk4 first
    CHECK(contains(source.readFile("materials/both_pk4.mtr", &time), "both_pk4_second"));
    CHECK(contains(source.readFile("MATERIALS/UPPER_CASE.MTR", &time), "upper_case_pk4"));

    // the resources: base/maps, then base backwards; in one container the later entry
    CHECK(contains(source.readFile("materials/res_only.mtr", &time), "res_only_res"));
    CHECK(contains(source.readFile("materials/both_res.mtr", &time), "both_res_second"));
    CHECK(contains(source.readFile("materials/maps_wins.mtr", &time), "maps_wins_maps"));
    CHECK(source.origin("materials/maps_wins.mtr")->container == "maps/map1.resources");
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

  SECTION("a read batch keeps containers open until closeFiles")
  {
    CHECK(source.openContainers() == 0u);
    CHECK(source.readFile("materials/res_only.mtr", nullptr));
    CHECK(source.readFile("materials/pk4_only.mtr", nullptr));
    CHECK(source.openContainers() == 2u);

#if defined(_WIN32)
    // the open container shares delete access: the user may replace it (R4-3.26); the
    // source then refuses to read from its stale table
    const auto container = game.base() / "_first.resources";
    auto ec = std::error_code{};
    std::filesystem::rename(container, game.base() / "_first.old", ec);
    CHECK_FALSE(ec);
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

    CHECK(decls.declFileCount() == 9);
    CHECK(decls.declFolders().size() == 8u);
  }
}

TEST_CASE("GameDeclSource with a corrupt container")
{
  const auto game = FakeGame{};
  writeAt(game.base() / "broken.resources", "not a container");

  auto source = GameDeclSource{game.path()};
  const auto warnings = source.takeWarnings();
  REQUIRE(warnings.size() == 1u);
  CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("broken.resources"));
  const auto order = source.searchOrder();
  CHECK(std::count(order.begin(), order.end(), "broken.resources") == 0);
  CHECK(contains(source.readFile("materials/res_only.mtr", nullptr), "res_only_res"));
}

TEST_CASE("ResourceContainer")
{
  const auto folder = TempFolder{"tb-lit-resources"};
  const auto file = folder.path() / "test.resources";
  const auto good = resourcesBytes({
    {"Materials\\One.mtr", "one"},
    {"materials/two.mtr", "two"},
    {"materials/one.mtr", "one again"},
  });

  const auto open = [&](const std::string& bytes, std::vector<std::string>& warnings) {
    writeAt(file, bytes);
    return ResourceContainer::open(file, "test.resources", warnings);
  };

  SECTION("reads the engine's format")
  {
    auto warnings = std::vector<std::string>{};
    const auto container = open(good, warnings);
    REQUIRE(container);
    CHECK(warnings.empty());
    REQUIRE(container->entries().size() == 3u);
    CHECK(container->entries()[0].name == "materials/one.mtr");

    // case-insensitive, and the later of two entries with one name
    const auto* one = container->find("MATERIALS/ONE.MTR");
    REQUIRE(one);
    CHECK(one->length == 9u);

    auto handle = GameFile::open(file);
    REQUIRE(handle);
    CHECK(container->read(*one, *handle, warnings) == "one again");
    CHECK(
      container->read(*container->find("materials/two.mtr"), *handle, warnings) == "two");
    CHECK(warnings.empty());
  }

  SECTION("a corrupt container is never read out of bounds")
  {
    const auto refused = [&](const std::string& bytes, const std::string& reason) {
      auto warnings = std::vector<std::string>{};
      const auto container = open(bytes, warnings);
      CHECK_FALSE(container);
      REQUIRE(warnings.size() == 1u);
      CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring(reason));
    };

    // truncated: no header, then the table cut off at each length
    refused(good.substr(0, 5), "too short for its header");
    refused(good.substr(0, good.size() - 3), "does not fit");

    auto wrongMagic = good;
    setBig32(wrongMagic, 0, 0xDEADBEEFu);
    refused(wrongMagic, "RESOURCE_FILE_MAGIC");

    auto tablePastEnd = good;
    setBig32(tablePastEnd, 4, std::uint32_t(good.size()));
    refused(tablePastEnd, "does not fit");

    auto hugeTable = good;
    setBig32(hugeTable, 8, 0x7FFFFFF0u);
    refused(hugeTable, "does not fit");

    // the entry count, then a name length, larger than the table holds
    const auto tableOffset = good.size() - (4 + 3 * 12 + 17 + 17 + 17);
    auto countPastTable = good;
    setBig32(countPastTable, tableOffset, 1000000u);
    refused(countPastTable, "truncated");

    auto namePastTable = good;
    namePastTable[tableOffset + 4] = char(0x7F);
    namePastTable[tableOffset + 7] = char(0x7F);
    refused(namePastTable, "truncated");

    auto negativeCount = good;
    setBig32(negativeCount, tableOffset, 0xFFFFFFFFu);
    refused(negativeCount, "truncated");
  }

  SECTION("an entry whose data does not fit is skipped, the others are kept")
  {
    const auto tableOffset = good.size() - (4 + 3 * 12 + 17 + 17 + 17);
    auto entryPastEnd = good;
    // the first entry's offset: after the count, its name length and its name
    setBig32(entryPastEnd, tableOffset + 4 + 4 + 17, std::uint32_t(good.size()));

    auto warnings = std::vector<std::string>{};
    const auto container = open(entryPastEnd, warnings);
    REQUIRE(container);
    REQUIRE(warnings.size() == 1u);
    CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("does not fit"));
    CHECK(container->entries().size() == 2u);
  }

  SECTION("a container that changed after its table was read is not read from")
  {
    auto warnings = std::vector<std::string>{};
    const auto container = open(good, warnings);
    REQUIRE(container);
    writeAt(file, good.substr(0, 20));
    auto handle = GameFile::open(file);
    REQUIRE(handle);
    CHECK_FALSE(
      container->read(*container->find("materials/two.mtr"), *handle, warnings));
    REQUIRE(warnings.size() == 1u);
    CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("changed"));
  }
}

} // namespace tb::lit
