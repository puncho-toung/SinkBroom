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

#include "TestEnvironment.h"
#include "TestParserStatus.h"
#include "fs/DiskIO.h"
#include "fs/Reader.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/FgdParser.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/overload.h"
#include "kd/result.h"
#include "kd/task_manager.h"

#include "vm/bbox.h"

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("Doom3BFGFixture")
{
  const auto gamePath = getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG";

  SECTION("Doom3BFGGameInfo")
  {
    CHECK(Doom3BFGGameInfo.gameConfig.name == "Doom 3 BFG");
    CHECK(
      Doom3BFGGameInfo.gameConfig.fileFormats
      == std::vector<MapFormatConfig>{{.format = "Doom3 (Valve)", .initialMap = ""}});
    CHECK(Doom3BFGGameInfo.gamePathPreference.defaultValue == gamePath);
    // like games/Doom3BFG/GameConfig.cfg, so that the _tb/models proxies load upright
    CHECK(
      Doom3BFGGameInfo.gameConfig.entityConfig.modelConvention
      == ModelConvention::IdTech4);
    CHECK(std::filesystem::exists(gamePath / "base" / "materials" / "tb_test.mtr"));
  }

  SECTION("Doom3BFGFixtureConfig")
  {
    auto fixture = MapFixture{};
    auto& map = fixture.create(Doom3BFGFixtureConfig);

    CHECK(map.gameInfo().gameConfig.name == "Doom 3 BFG");
    CHECK(map.worldNode().mapFormat() == MapFormat::Doom3_Valve);

    // a new map hands the game's naming and rotation keys to its nodes
    CHECK(
      map.worldNode().entityPropertyConfig()
      == makeEntityPropertyConfig(Doom3BFGGameInfo.gameConfig));
    CHECK(
      map.worldNode().entityPropertyConfig().naming
      == EntityNamingConfig{.nameKey = "name", .brushEntityModelKey = "model"});
    CHECK(
      map.worldNode().entityPropertyConfig().rotationPolicy
      == EntityRotationPolicy::IdTech4);

    // the builtin FGD is found next to the fixture's virtual config file
    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);
    CHECK(getType(*lightDefinition) == EntityDefinitionType::Point);
  }

  SECTION("test_box_valve220.map")
  {
    // a map from RBDOOM-3-BFG's unit test maps; dmap reports 9 world brushes and 12
    // entities, the worldspawn included
    const auto path = gamePath / "base" / "maps" / "test_box_valve220.map";

    auto file = fs::Disk::openFile(path) | kdl::value();
    auto reader = file->reader().buffer();

    auto status = TestParserStatus{};
    auto taskManager = kdl::task_manager{};
    auto worldReader = WorldReader{reader.stringView(), MapFormat::Doom3_Valve, {}};
    REQUIRE(worldReader.read(vm::bbox3d{8192.0}, status, taskManager));
    CHECK(status.countStatus(LogLevel::Error) == 0u);
    CHECK(status.countStatus(LogLevel::Warn) == 0u);

    auto fixture = MapFixture{};
    auto& map = fixture.load(path, Doom3BFGFixtureConfig);
    REQUIRE(map.worldNode().mapFormat() == MapFormat::Doom3_Valve);

    // a loaded map hands the game's naming and rotation keys to its nodes
    CHECK(
      map.worldNode().entityPropertyConfig()
      == makeEntityPropertyConfig(Doom3BFGGameInfo.gameConfig));

    auto worldBrushCount = size_t{0};
    auto entityCount = size_t{1}; // the worldspawn
    map.worldNode().accept(kdl::overload(
      [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, const GroupNode& node) { node.visitChildren(thisLambda); },
      [&](const EntityNode&) { ++entityCount; },
      [&](const BrushNode&) { ++worldBrushCount; },
      [](const PatchNode&) {}));

    CHECK(worldBrushCount == 9u);
    CHECK(entityCount == 12u);
  }

  SECTION("DOOM-3-tbtest.fgd")
  {
    const auto path = gamePath / "base" / "_tb" / "fgd" / "DOOM-3-tbtest.fgd";
    auto file = fs::Disk::openFile(path) | kdl::value();
    auto reader = file->reader().buffer();

    auto parser = FgdParser{reader.stringView(), RgbaF{1.0f, 1.0f, 1.0f, 1.0f}, path};

    auto status = TestParserStatus{};
    const auto definitions = parser.parseDefinitions(status);
    REQUIRE(definitions.is_success());
    CHECK(definitions.value().size() == 10u);
    CHECK(status.countStatus(LogLevel::Error) == 0u);
    CHECK(status.countStatus(LogLevel::Warn) == 0u);
  }
}

} // namespace tb::mdl
