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
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityLinkManager.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/FgdParser.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/TestUtils.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/overload.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/task_manager.h"
#include "kd/vector_utils.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <filesystem>
#include <string>
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
    CHECK(definitions.value().size() == 11u);
    CHECK(status.countStatus(LogLevel::Error) == 0u);
    CHECK(status.countStatus(LogLevel::Warn) == 0u);
  }

  SECTION("Links come from the entity definitions")
  {
    // the FGD declares name(target_source) and target(target_destination), as the FGDs
    // that RBDOOM-3-BFG exports do, so no link keys are hard-coded for Doom 3
    const auto path = gamePath / "base" / "maps" / "tb_test_valve220.map";

    auto fixture = MapFixture{};
    auto& map = fixture.load(path, Doom3BFGFixtureConfig);

    const auto* triggerDefinition =
      map.entityDefinitionManager().definition("trigger_once");
    REQUIRE(triggerDefinition != nullptr);
    CHECK(isLinkTargetProperty(triggerDefinition, "name"));
    CHECK(isLinkSourceProperty(triggerDefinition, "target"));

    // the targetname fallback for definition files without link types was not applied
    CHECK(std::ranges::none_of(
      map.entityDefinitionManager().definitions(), [](const auto& definition) {
        return getPropertyDefinition(definition, EntityPropertyKeys::Targetname)
               != nullptr;
      }));

    const auto findEntity = [&](const std::string& name) {
      auto entityNodes = findEntitiesWithName(map, name);
      REQUIRE(entityNodes.size() == 1u);
      return entityNodes.front();
    };

    const auto* triggerNode = findEntity("trigger_once_1");
    const auto* targetNode = findEntity("target_null_1");
    CHECK(map.entityLinkManager().hasLink(*triggerNode, *targetNode, "target"));

    // every entity has a name, so an unused name is no issue
    const auto* linkSourceValidator =
      findValidator(map.worldNode(), "Missing entity link source");
    REQUIRE(linkSourceValidator != nullptr);
    CHECK(collectIssues(map.worldNode(), linkSourceValidator->type()).empty());
  }

  SECTION("Editing keeps every name unique and every model equal to its name")
  {
    auto fixture = MapFixture{};
    auto& map = fixture.create(Doom3BFGFixtureConfig);

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    const auto* staticDefinition =
      map.entityDefinitionManager().definition("func_static");
    const auto* triggerDefinition =
      map.entityDefinitionManager().definition("trigger_once");
    REQUIRE(lightDefinition != nullptr);
    REQUIRE(staticDefinition != nullptr);
    REQUIRE(triggerDefinition != nullptr);

    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    const auto selectNewBrush = [&](const vm::vec3d& position) {
      auto* brushNode = new BrushNode{
        builder.createCuboid(
          vm::bbox3d{position, position + vm::vec3d{32, 32, 32}}, "wall")
        | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      deselectAll(map);
      selectNodes(map, {brushNode});
      return brushNode;
    };

    // create
    auto* lightNode = createPointEntity(map, *lightDefinition, {0, 0, 64});
    REQUIRE(lightNode != nullptr);
    selectNewBrush({0, 0, 0});
    auto* staticNode = createBrushEntity(map, *staticDefinition);
    REQUIRE(staticNode != nullptr);
    selectNewBrush({64, 0, 0});
    auto* triggerNode = createBrushEntity(map, *triggerDefinition);
    REQUIRE(triggerNode != nullptr);
    REQUIRE(setEntityProperty(map, "target", *lightNode->entity().property("name")));

    // paste twice
    deselectAll(map);
    selectNodes(map, {lightNode, staticNode, triggerNode});
    const auto str = serializeSelectedNodes(map);
    deselectAll(map);
    REQUIRE(paste(map, str) == PasteType::Node);
    REQUIRE(paste(map, str) == PasteType::Node);

    // duplicate
    deselectAll(map);
    selectNodes(map, {lightNode, staticNode, triggerNode});
    duplicateSelectedNodes(map);

    // linked duplicate, then an update of the link set that adds an entity
    deselectAll(map);
    selectNodes(map, {lightNode, staticNode, triggerNode});
    auto* groupNode = groupSelectedNodes(map, "group");
    REQUIRE(groupNode != nullptr);
    deselectAll(map);
    selectNodes(map, {groupNode});
    REQUIRE(createLinkedDuplicate(map) != nullptr);

    openGroup(map, *groupNode);
    deselectAll(map);
    REQUIRE(createPointEntity(map, *lightDefinition, {0, 32, 64}) != nullptr);
    selectNodes(map, {lightNode});
    REQUIRE(setEntityProperty(map, "light_radius", "64"));
    closeGroup(map);

    for (const auto& description :
         {"Duplicate or invalid entity name", "Brush entity model differs from its name"})
    {
      CAPTURE(description);
      const auto* validator = findValidator(map.worldNode(), description);
      REQUIRE(validator != nullptr);
      CHECK(collectIssues(map.worldNode(), validator->type()).empty());
    }

    auto names = std::vector<std::string>{};
    map.worldNode().accept(kdl::overload(
      [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, const GroupNode& node) { node.visitChildren(thisLambda); },
      [&](const EntityNode& node) {
        if (const auto* name = node.entity().property("name"))
        {
          names.push_back(kdl::str_to_lower(*name));
        }
      },
      [](const BrushNode&) {},
      [](const PatchNode&) {}));

    // 3 created, 6 pasted, 3 duplicated, 3 in the linked duplicate, 2 lights added
    CHECK(names.size() == 17u);
    names = kdl::vec_sort(std::move(names));
    CHECK(std::ranges::adjacent_find(names) == names.end());
  }

  SECTION("Rotating entities writes what the engine reads")
  {
    auto fixture = MapFixture{};
    auto& map = fixture.create(Doom3BFGFixtureConfig);

    // the shipped and the fixture FGD declare angle but not angles for func_teleporter
    const auto* teleporterDefinition =
      map.entityDefinitionManager().definition("func_teleporter");
    REQUIRE(teleporterDefinition != nullptr);
    REQUIRE(getPropertyDefinition(teleporterDefinition, "angle") != nullptr);
    REQUIRE(getPropertyDefinition(teleporterDefinition, "angles") == nullptr);

    // a light from an older map, turned by 30.6 degrees with a rotation matrix
    auto* lightNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"origin", "0 0 0"},
      {"rotation", "0.860742 0.509041 0 -0.509041 0.860742 0 0 0 1"},
      {"angle", "45"},
    }}};
    auto* teleporterNode = new EntityNode{Entity{{
      {"classname", "func_teleporter"},
      {"origin", "64 0 0"},
    }}};
    auto* staticNode = new EntityNode{Entity{{
      {"classname", "func_static"},
      {"angles", "0 30 0"},
    }}};

    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-128, -32, 0}, {-64, 32, 64}}, "wall")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {lightNode, teleporterNode, staticNode}}});
    addNodes(map, {{staticNode, {brushNode}}});
    REQUIRE(lightNode->entity().definition() != nullptr);
    REQUIRE(teleporterNode->entity().definition() == teleporterDefinition);

    selectNodes(map, {lightNode, teleporterNode, brushNode});
    REQUIRE(
      rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, vm::to_radians(90.0)));

    // the light gets angles in the engine's format and loses what the engine reads first
    CHECK(*lightNode->entity().property("angles") == "0 120.6 0");
    CHECK(!lightNode->entity().hasProperty("rotation"));
    CHECK(!lightNode->entity().hasProperty("angle"));

    // the teleporter keeps its yaw in angle, the only key idTeleporter reads
    CHECK(*teleporterNode->entity().property("angle") == "90");
    CHECK(!teleporterNode->entity().hasProperty("angles"));

    // the brush entity's brushes are turned, its angles are left alone
    CHECK(*staticNode->entity().property("angles") == "0 30 0");
    CHECK(!staticNode->entity().hasProperty("angle"));
  }
}

} // namespace tb::mdl
