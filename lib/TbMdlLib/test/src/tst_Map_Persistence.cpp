/*
 Copyright (C) 2026 Kristian Duske

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

#include "Observer.h"
#include "TestEnvironment.h"
#include "fs/TestEnvironment.h"
#include "mdl/BezierPatch.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "version/Version.h"

#include "kd/k.h"
#include "kd/overload.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

struct PatchSummary
{
  size_t rowCount;
  size_t columnCount;
  std::string materialName;
  std::optional<PatchSubdivisions> explicitSubdivisions;

  bool operator==(const PatchSummary&) const = default;
};

/**
 * The parts of a loaded map that a round trip through a file must preserve. Entities are
 * keyed by their name and summarized by their sorted property keys.
 */
struct MapSummary
{
  std::vector<std::string> worldspawnKeys;
  std::map<std::string, std::vector<std::string>> entityKeys;
  std::vector<std::tuple<std::string, size_t>> groups;
  size_t brushCount = 0;
  std::vector<PatchSummary> patches;

  bool operator==(const MapSummary&) const = default;
};

std::vector<std::string> sortedKeys(const Entity& entity)
{
  auto result = entity.propertyKeys();
  std::ranges::sort(result);
  return result;
}

MapSummary summarize(const WorldNode& worldNode)
{
  auto result = MapSummary{};
  result.worldspawnKeys = sortedKeys(worldNode.entity());

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const GroupNode& node) {
      result.groups.emplace_back(node.name(), node.childCount());
      node.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const EntityNode& node) {
      const auto* name = node.entity().property("name");
      result.entityKeys[name ? *name : ""] = sortedKeys(node.entity());
      node.visitChildren(thisLambda);
    },
    [&](const BrushNode&) { ++result.brushCount; },
    [&](const PatchNode& node) {
      const auto& patch = node.patch();
      result.patches.push_back(PatchSummary{
        patch.pointRowCount(),
        patch.pointColumnCount(),
        patch.materialName(),
        patch.explicitSubdivisions(),
      });
    }));

  return result;
}

std::vector<std::string> splitLines(const std::string& str)
{
  auto result = std::vector<std::string>{};
  auto stream = std::istringstream{str};
  for (auto line = std::string{}; std::getline(stream, line);)
  {
    result.push_back(line);
  }
  return result;
}

} // namespace

TEST_CASE("Map_Persistence")
{
  const auto generatorComment =
    fmt::format("// Generator: TrenchBroom {} (Build {})\n", VERSION_STR, BUILD_ID_STR);

  SECTION("save")
  {
    auto env = fs::TestEnvironment{};

    const auto filename = "test.map";
    env.createFile(filename, R"(// Game: Test
// Format: Valve
// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"name" "entity1"
}
)");

    const auto path = env.dir() / filename;

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo.gameConfig.fileFormats = {{"Valve", ""}};

    auto fixture = MapFixture{};
    auto& map = fixture.load(path, fixtureConfig);

    REQUIRE(map.persistent());
    REQUIRE(map.path() == path);

    auto* entityNode = new EntityNode{Entity{{{"name", "entity2"}}}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    auto mapWasSaved = Observer<>{map.mapWasSavedNotifier};
    auto modificationStateDidChange = Observer<>{map.modificationStateDidChangeNotifier};

    REQUIRE(map.save());

    CHECK(mapWasSaved.notifications == std::vector<std::tuple<>>{{}});
    CHECK(modificationStateDidChange.notifications == std::vector<std::tuple<>>{{}});
    CHECK(map.persistent());
    CHECK(map.path() == path);

    REQUIRE(env.fileExists(path));
    CHECK(
      env.loadFile(path)
      == fmt::format("// Game: Test\n// Format: Valve\n{}", generatorComment)
           + R"(// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"name" "entity1"
}
// entity 2
{
"name" "entity2"
}
)");
  }

  SECTION("saveAs")
  {
    auto fixture = MapFixture{};
    auto& map = fixture.create();

    auto* entityNode = new EntityNode{Entity{{{"key", "value"}}}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});
    REQUIRE(map.worldNode().defaultLayer()->children() == std::vector<Node*>{entityNode});

    auto mapWasSaved = Observer<>{map.mapWasSavedNotifier};
    auto modificationStateDidChange = Observer<>{map.modificationStateDidChangeNotifier};

    auto env = fs::TestEnvironment{};

    const auto path = env.dir() / "test.map";
    REQUIRE(map.saveAs(path));

    CHECK(mapWasSaved.notifications == std::vector<std::tuple<>>{{}});
    CHECK(modificationStateDidChange.notifications == std::vector<std::tuple<>>{{}});
    CHECK(map.persistent());
    CHECK(map.path() == path);

    REQUIRE(env.fileExists(path));
    CHECK(
      env.loadFile(path)
      == fmt::format("// Game: Test\n// Format: Standard\n{}", generatorComment)
           + R"(// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"key" "value"
}
)");
  }

  SECTION("exportAs")
  {
    auto fixture = MapFixture{};
    auto env = fs::TestEnvironment{};

    SECTION("Export as obj")
    {
      auto& map = fixture.create();

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});

      const auto objFilename = "test.obj";
      const auto mtlFilename = "test.mtl";

      REQUIRE(map.exportAs(ObjExportOptions{
        env.dir() / objFilename,
        ObjMtlPathMode::RelativeToExportPath,
      }));

      CHECK(env.fileExists(objFilename));
      CHECK(env.fileExists(mtlFilename));
      CHECK(!map.persistent());
      CHECK(map.path() == "unnamed.map");
    }

    SECTION("Export as map")
    {
      auto& map = fixture.create();

      auto* entityNode = new EntityNode{Entity{{{"key", "value"}}}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      const auto filename = "test.map";
      REQUIRE(map.exportAs(MapExportOptions{
        env.dir() / filename,
        !K(stripTbProperties),
        std::nullopt,
        std::nullopt,
      }));
      REQUIRE(env.fileExists(filename));
      CHECK(env.loadFile(filename) == R"(// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"key" "value"
}
)");
      CHECK(!map.persistent());
      CHECK(map.path() == "unnamed.map");
    }

    SECTION("Omit layers from export")
    {
      const auto newDocumentPath = std::filesystem::path{"test.map"};
      auto& map = fixture.create(QuakeFixtureConfig);

      auto layer = mdl::Layer{"Layer"};
      layer.setOmitFromExport(true);

      auto* layerNode = new mdl::LayerNode{std::move(layer)};
      addNodes(map, {{&map.worldNode(), {layerNode}}});

      REQUIRE(map.exportAs(MapExportOptions{
        env.dir() / newDocumentPath,
        !K(stripTbProperties),
        std::nullopt,
        std::nullopt,
      }));
      REQUIRE(env.fileExists(newDocumentPath));
      CHECK(env.loadFile(newDocumentPath) == R"(// entity 0
{
"classname" "worldspawn"
}
)");
    }

    SECTION("Strip TB properties")
    {
      const auto newDocumentPath = std::filesystem::path{"test.map"};

      auto& map = fixture.create(QuakeFixtureConfig);

      auto* layerNode = new mdl::LayerNode{mdl::Layer{"Layer"}};
      addNodes(map, {{&map.worldNode(), {layerNode}}});

      REQUIRE(map.exportAs(MapExportOptions{
        env.dir() / newDocumentPath,
        K(stripTbProperties),
        std::nullopt,
        std::nullopt,
      }));
      REQUIRE(env.fileExists(newDocumentPath));
      CHECK(env.loadFile(newDocumentPath) == R"(// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"classname" "func_group"
}
)");
    }

    SECTION("Strip entities")
    {
      const auto newDocumentPath = std::filesystem::path{"test.map"};

      auto& map = fixture.create(QuakeFixtureConfig);

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      REQUIRE(map.exportAs(MapExportOptions{
        env.dir() / newDocumentPath,
        !K(stripTbProperties),
        "light",
        std::nullopt,
      }));
      REQUIRE(env.fileExists(newDocumentPath));
      CHECK(env.loadFile(newDocumentPath) == R"(// entity 0
{
"classname" "worldspawn"
}
)");
    }

    SECTION("Add entity")
    {
      const auto newDocumentPath = std::filesystem::path{"test.map"};

      auto& map = fixture.create(QuakeFixtureConfig);

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      REQUIRE(map.exportAs(MapExportOptions{
        env.dir() / newDocumentPath,
        !K(stripTbProperties),
        std::nullopt,
        mdl::Entity{{{"classname", "info_player_start"}}},
      }));
      REQUIRE(env.fileExists(newDocumentPath));
      CHECK(env.loadFile(newDocumentPath) == R"(// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"classname" "light"
}
// entity 2
{
"classname" "info_player_start"
}
)");
    }
  }

  SECTION("Doom3 (Valve) round trip")
  {
    // tb_test_valve220.map has the syntax RBDOOM-3-BFG's idMapFile::Write produces
    const auto engineMapPath = getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG"
                               / "base" / "maps" / "tb_test_valve220.map";

    const auto expectedSummary = MapSummary{
      .worldspawnKeys = {"_tb_def", "_tb_textures", "classname"},
      .entityKeys =
        {
          {"func_door_1", {"angle", "classname", "model", "name"}},
          {"func_static_1", {"classname", "model", "name"}},
          {"info_player_start_1", {"angle", "classname", "name", "origin"}},
          {"light_1",
           {"_color", "angles", "classname", "light_radius", "name", "origin"}},
          {"light_2", {"classname", "light_radius", "name", "origin"}},
          {"misc_model_1",
           {"angles", "classname", "model", "name", "origin", "proxymodel"}},
          {"target_null_1", {"classname", "name", "origin"}},
          {"trigger_once_1", {"classname", "model", "name", "target"}},
        },
      // the func_group with its brush and the light that refers to it
      .groups = {{"group_1", 2}},
      .brushCount = 10,
      .patches =
        {
          {3, 5, "tb_test/wall", std::nullopt},
          {5, 3, "tb_test/wall_plain", PatchSubdivisions{4, 2}},
        },
    };

    auto env = fs::TestEnvironment{};

    auto engineFixture = MapFixture{};
    auto& engineMap = engineFixture.load(engineMapPath, Doom3BFGFixtureConfig);
    REQUIRE(engineMap.worldNode().mapFormat() == MapFormat::Doom3_Valve);
    CHECK(summarize(engineMap.worldNode()) == expectedSummary);

    const auto firstSavePath = env.dir() / "first.map";
    REQUIRE(engineMap.saveAs(firstSavePath));
    const auto firstSave = env.loadFile(firstSavePath);

    // RBDOOM-3-BFG's header, so the engine and TrenchBroom detect the saved map
    CHECK(firstSave.starts_with(
      "// Game: Doom 3 BFG\n// Format: Doom3 (Valve)\n// Generator: TrenchBroom "));

    // 10 entities: worldspawn, 8 entities and the group
    CHECK(
      std::ranges::count_if(
        splitLines(firstSave),
        [](const auto& line) { return line.starts_with("// entity "); })
      == 10);

    // patches are written quoted without textures/, which the engine adds when reading
    CHECK(
      firstSave.find("patchDef2\n{\n\"tb_test/wall\"\n( 3 5 0 0 0 )\n")
      != std::string::npos);
    CHECK(
      firstSave.find("patchDef3\n{\n\"tb_test/wall_plain\"\n( 5 3 4 2 0 0 0 )\n")
      != std::string::npos);

    // faces end with rotation and scale, without the engine's ignored "0 0 0"
    for (const auto& line : splitLines(firstSave))
    {
      if (const auto lastBracket = line.rfind(" ] "); lastBracket != std::string::npos)
      {
        CAPTURE(line);
        CHECK(kdl::str_split(line.substr(lastBracket + 3), " ").size() == 3u);
      }
    }

    auto savedFixture = MapFixture{};
    auto& savedMap = savedFixture.load(firstSavePath, Doom3BFGFixtureConfig);
    CHECK(summarize(savedMap.worldNode()) == expectedSummary);

    const auto secondSavePath = env.dir() / "second.map";
    REQUIRE(savedMap.saveAs(secondSavePath));
    CHECK(env.loadFile(secondSavePath) == firstSave);
  }
}

} // namespace tb::mdl
