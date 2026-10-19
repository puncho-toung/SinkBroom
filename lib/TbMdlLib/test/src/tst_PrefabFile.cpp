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

#include "base/Logger.h"
#include "base/SimpleParserStatus.h"
#include "fs/TestEnvironment.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/overload.h"
#include "kd/task_manager.h"

#include "vm/mat_ext.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

MapFixtureConfig makeConfig(const std::filesystem::path& gamePath)
{
  return MapFixtureConfig{
    .mapFormat = MapFormat::Doom3_Valve,
    .gameInfo = detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, gamePath),
  };
}

void setDefinitions(Map& map)
{
  // the Doom 3 BFG fixture loads an FGD; let the map release those definitions first
  map.entityDefinitionsWillChangeNotifier();
  map.entityDefinitionManager().setDefinitions({
    {"light",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
       PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
       PropertyDefinition{
         "light_radius", PropertyValueTypes::String{"300 300 300"}, "", ""},
     },
     PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
    {"monster_zombie",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
       PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
       PropertyDefinition{"cameraTarget", PropertyValueTypes::LinkSource{}, "", ""},
       PropertyDefinition{"health", PropertyValueTypes::Integer{100}, "", ""},
     },
     PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
    {"path_corner",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
     },
     PointEntityDefinition{vm::bbox3d{8.0}, {}, {}}},
    {"func_static",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
     }},
  });
}

EntityNode* createEntity(std::vector<EntityProperty> properties)
{
  return new EntityNode{Entity{std::move(properties)}};
}

BrushNode* createBrushAt(const Map& map, const vm::vec3d& center)
{
  return createBrushNode(map, "material", [&](Brush& brush) {
    REQUIRE(
      brush.transform(map.worldBounds(), vm::translation_matrix(center), false)
      | kdl::is_success());
  });
}

std::unique_ptr<WorldNode> readPrefab(const Map& map, const std::string& contents)
{
  auto logger = NullLogger{};
  auto status = SimpleParserStatus{logger};
  auto taskManager = kdl::task_manager{0};
  return WorldReader::tryRead(
           contents,
           {MapFormat::Doom3_Valve},
           map.worldBounds(),
           map.worldNode().entityPropertyConfig(),
           status,
           taskManager)
         | kdl::value();
}

std::string property(const EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  return value ? *value : "<missing>";
}

} // namespace

TEST_CASE("PrefabFile")
{
  auto env = fs::TestEnvironment{};
  auto fixture = MapFixture{};
  auto& map = fixture.create(makeConfig(env.dir()));
  map.prefabLibrary().setUserRoot(env.dir() / "user");
  setDefinitions(map);

  const auto savedFile = [&](const std::filesystem::path& path) {
    return env.loadFile(path.lexically_relative(env.dir()));
  };

  SECTION("prefabFilePath")
  {
    const auto& library = map.prefabLibrary();
    const auto userRoot = env.dir() / "user";
    const auto filePath = [&](std::string name, std::filesystem::path category = {}) {
      return prefabFilePath(
        library,
        SavePrefabOptions{
          .name = std::move(name),
          .category = std::move(category),
          .root = PrefabRoot::User});
    };

    CHECK(
      (filePath("Red torch", "lights") | kdl::value())
      == userRoot / "lights" / "Red torch.map");
    CHECK(
      (filePath("Crate", "props/wood") | kdl::value())
      == userRoot / "props" / "wood" / "Crate.map");

    // characters that a file name cannot hold on some platform are replaced; leading and
    // trailing spaces and trailing dots are removed
    CHECK(
      (filePath(" a/b\\c:d*e?f\"g<h>i|j ") | kdl::value())
      == userRoot / "a_b_c_d_e_f_g_h_i_j.map");
    CHECK((filePath("tab\there") | kdl::value()) == userRoot / "tab_here.map");
    CHECK((filePath("dots... ") | kdl::value()) == userRoot / "dots.map");
    CHECK(filePath(" . ").is_error());
    CHECK(filePath("").is_error());

    // the device names that Windows reserves get a suffix, with or without an extension
    CHECK((filePath("CON") | kdl::value()) == userRoot / "CON_.map");
    CHECK((filePath("nul") | kdl::value()) == userRoot / "nul_.map");
    CHECK((filePath("Com1") | kdl::value()) == userRoot / "Com1_.map");
    CHECK((filePath("lpt9.crate") | kdl::value()) == userRoot / "lpt9_.crate.map");
    CHECK((filePath("console") | kdl::value()) == userRoot / "console.map");
    CHECK((filePath("com10") | kdl::value()) == userRoot / "com10.map");

    // the category is a folder inside the root
    CHECK(
      (filePath("Light", "./lights/") | kdl::value())
      == userRoot / "lights" / "Light.map");
    CHECK((filePath("Light", "a/../b") | kdl::value()) == userRoot / "b" / "Light.map");
    CHECK((filePath("Light", ".") | kdl::value()) == userRoot / "Light.map");
    CHECK(filePath("Light", "../out").is_error());
    CHECK(filePath("Light", "a/../..").is_error());
    CHECK(filePath("Light", "/lights").is_error());
    CHECK(filePath("Light", env.dir() / "elsewhere").is_error());
#ifdef _WIN32
    CHECK(filePath("Light", "C:lights").is_error());
#endif

    // no root
    CHECK(
      prefabFilePath(library, SavePrefabOptions{.name = "Light", .root = PrefabRoot::Mod})
        .is_success());
    map.prefabLibrary().setUserRoot({});
    CHECK(filePath("Light").is_error());
  }

  SECTION("derivePrefabKind")
  {
    auto* lightNode = createEntity({{"classname", "light"}, {"origin", "0 0 0"}});
    auto* monsterNode =
      createEntity({{"classname", "monster_zombie"}, {"origin", "64 0 0"}});

    auto* brushEntityNode = createEntity({{"classname", "func_static"}});
    auto* brushNode1 = createBrushAt(map, {0, 0, 0});
    auto* brushNode2 = createBrushAt(map, {64, 0, 0});
    brushEntityNode->addChildren({brushNode1, brushNode2});

    auto* groupNode = new GroupNode{Group{"group"}};
    auto* groupedNode = createEntity({{"classname", "light"}, {"origin", "0 64 0"}});
    groupNode->addChild(groupedNode);

    addNodes(
      map,
      {{&parentForNodes(map), {lightNode, monsterNode, brushEntityNode, groupNode}}});

    CHECK(derivePrefabKind({lightNode}, false) == PrefabKind::Entity);
    CHECK(derivePrefabKind({brushEntityNode}, false) == PrefabKind::Entity);
    // the brushes of a brush entity stand for the entity
    CHECK(derivePrefabKind({brushNode1, brushNode2}, false) == PrefabKind::Entity);
    CHECK(derivePrefabKind({lightNode, monsterNode}, false) == PrefabKind::Assembly);
    CHECK(derivePrefabKind({groupNode}, false) == PrefabKind::Assembly);
    CHECK(derivePrefabKind({lightNode}, true) == PrefabKind::Assembly);
  }

  SECTION("findExternalLinks")
  {
    auto* monsterNode = createEntity({
      {"classname", "monster_zombie"},
      {"origin", "64 0 0"},
      {"name", "zombie_1"},
      {"target", "path_1"},
      {"target2", "Light_1"},
      {"cameraTarget", "camera_1"},
      {"bind", "mover_1"},
    });
    auto* pathNode = createEntity(
      {{"classname", "path_corner"}, {"origin", "0 0 0"}, {"name", "path_1"}});
    auto* lightNode =
      createEntity({{"classname", "light"}, {"origin", "0 0 64"}, {"name", "light_1"}});
    addNodes(map, {{&parentForNodes(map), {monsterNode, pathNode, lightNode}}});

    const auto keys = [](const auto& links) {
      auto result = std::vector<std::string>{};
      for (const auto& link : links)
      {
        result.push_back(link.key);
      }
      return result;
    };

    const auto monsterLinks = findExternalLinks({monsterNode});
    CHECK(
      keys(monsterLinks)
      == std::vector<std::string>{"target", "target2", "cameraTarget", "bind"});
    CHECK(monsterLinks.front().node == monsterNode);
    CHECK(monsterLinks.front().value == "path_1");

    // links into the selection are kept, names compare without case
    CHECK(
      keys(findExternalLinks({monsterNode, pathNode, lightNode}))
      == std::vector<std::string>{"cameraTarget", "bind"});

    // savePrefab removes the same keys from the written file
    const auto worldNode = readPrefab(
      map,
      savedFile(
        savePrefab(
          map,
          {monsterNode},
          SavePrefabOptions{.name = "Zombie", .root = PrefabRoot::User})
        | kdl::value()));
    const auto* bodyNode =
      dynamic_cast<const EntityNode*>(worldNode->defaultLayer()->children().front());
    REQUIRE(bodyNode);
    CHECK(property(*bodyNode, "name") == "zombie_1");
    for (const auto* key : {"target", "target2", "cameraTarget", "bind"})
    {
      CAPTURE(key);
      CHECK(property(*bodyNode, key) == "<missing>");
    }
  }

  SECTION("Entity snapshot round trip")
  {
    auto* lightNode = createEntity({
      {"classname", "light"},
      {"origin", "64 32 16"},
      {"name", "torch"},
      {"_color", "1 0 0"},
      {"target", "door"},
    });
    auto* doorNode =
      createEntity({{"classname", "path_corner"}, {"origin", "0 0 0"}, {"name", "door"}});
    addNodes(map, {{&parentForNodes(map), {lightNode, doorNode}}});

    auto options = SavePrefabOptions{
      .name = "Red torch",
      .category = "lights",
      .tags = {"light", " torch ", ""},
      .root = PrefabRoot::User,
    };

    SECTION("Default options")
    {
      const auto path = savePrefab(map, {lightNode}, options) | kdl::value();
      CHECK(path == env.dir() / "user" / "lights" / "Red torch.map");

      const auto contents = savedFile(path);
      CHECK(contents.starts_with("// Game: Doom 3 BFG\n// Format: Doom3 (Valve)\n"));

      const auto worldNode = readPrefab(map, contents);
      const auto metadata = readPrefabMetadata(*worldNode) | kdl::value();
      CHECK(metadata.kind == PrefabKind::Entity);
      CHECK(metadata.name == "Red torch");
      CHECK(metadata.className == "light");
      CHECK(metadata.tags == std::vector<std::string>{"light", "torch"});
      CHECK(metadata.saved.size() == 19);
      CHECK(metadata.saved[10] == 'T');
      CHECK(metadata.source == "unnamed.map");

      const auto& body = worldNode->defaultLayer()->children();
      REQUIRE(body.size() == 1);
      const auto* bodyNode = dynamic_cast<const EntityNode*>(body.front());
      REQUIRE(bodyNode);
      CHECK(property(*bodyNode, "classname") == "light");
      CHECK(property(*bodyNode, "origin") == "0 0 0");
      CHECK(property(*bodyNode, "name") == "torch");
      CHECK(property(*bodyNode, "_color") == "1 0 0");
      // the link left the selection
      CHECK(property(*bodyNode, "target") == "<missing>");
      // no definition defaults
      CHECK(property(*bodyNode, "light_radius") == "<missing>");

      // the map is not touched
      CHECK(property(*lightNode, "target") == "door");
      CHECK(property(*lightNode, "origin") == "64 32 16");

      // an entity snapshot carries no _tb_ key, protected properties included
      auto entity = lightNode->entity();
      entity.setProtectedProperties({"_color"});
      lightNode->setEntity(std::move(entity));
      const auto protectedContents =
        savedFile(savePrefab(map, {lightNode}, options) | kdl::value());
      CHECK(protectedContents.find("_tb_protected_properties") == std::string::npos);
      const auto protectedWorldNode = readPrefab(map, protectedContents);
      const auto* protectedBodyNode = dynamic_cast<const EntityNode*>(
        protectedWorldNode->defaultLayer()->children().front());
      REQUIRE(protectedBodyNode);
      CHECK(protectedBodyNode->entity().protectedProperties().empty());
      CHECK(property(*protectedBodyNode, "_color") == "1 0 0");
      CHECK(
        lightNode->entity().protectedProperties() == std::vector<std::string>{"_color"});

      // the host's definition file and mods are copied when it has them
      const auto hasDefAndMod = [](const WorldNode& prefabWorldNode) {
        const auto& entity = prefabWorldNode.entity();
        return std::pair{entity.hasProperty("_tb_def"), entity.hasProperty("_tb_mod")};
      };
      CHECK(hasDefAndMod(*worldNode) == std::pair{false, false});

      auto worldEntity = map.worldNode().entity();
      worldEntity.addOrUpdateProperty("_tb_def", "external:defs/host.fgd");
      worldEntity.addOrUpdateProperty("_tb_mod", "hostmod");
      map.worldNode().setEntity(std::move(worldEntity));

      const auto hostWorldNode =
        readPrefab(map, savedFile(savePrefab(map, {lightNode}, options) | kdl::value()));
      CHECK(hasDefAndMod(*hostWorldNode) == std::pair{true, true});
      CHECK(*hostWorldNode->entity().property("_tb_def") == "external:defs/host.fgd");
      CHECK(*hostWorldNode->entity().property("_tb_mod") == "hostmod");
    }

    SECTION("Keep external links")
    {
      options.clearExternalLinks = false;
      const auto path = savePrefab(map, {lightNode}, options) | kdl::value();
      const auto worldNode = readPrefab(map, savedFile(path));
      const auto* bodyNode =
        dynamic_cast<const EntityNode*>(worldNode->defaultLayer()->children().front());
      REQUIRE(bodyNode);
      CHECK(property(*bodyNode, "target") == "door");
    }

    SECTION("Include definition defaults")
    {
      options.includeDefaults = true;
      const auto path = savePrefab(map, {lightNode}, options) | kdl::value();
      const auto worldNode = readPrefab(map, savedFile(path));
      const auto* bodyNode =
        dynamic_cast<const EntityNode*>(worldNode->defaultLayer()->children().front());
      REQUIRE(bodyNode);
      CHECK(property(*bodyNode, "light_radius") == "300 300 300");
      // own keys win over defaults
      CHECK(property(*bodyNode, "_color") == "1 0 0");
    }

    SECTION("Overwrite an existing file")
    {
      const auto path = savePrefab(map, {lightNode}, options) | kdl::value();
      options.tags = {"red"};
      CHECK((savePrefab(map, {lightNode}, options) | kdl::value()) == path);
      const auto worldNode = readPrefab(map, savedFile(path));
      CHECK(
        (readPrefabMetadata(*worldNode) | kdl::value()).tags
        == std::vector<std::string>{"red"});
    }
  }

  SECTION("Brush entity snapshot")
  {
    auto* entityNode = createEntity(
      {{"classname", "func_static"}, {"origin", "64 0 0"}, {"name", "prop"}});
    entityNode->addChild(createBrushAt(map, {64, 0, 0}));
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    const auto path = savePrefab(
                        map,
                        entityNode->children(),
                        SavePrefabOptions{.name = "Prop", .root = PrefabRoot::User})
                      | kdl::value();
    CHECK(path == env.dir() / "user" / "Prop.map");

    const auto worldNode = readPrefab(map, savedFile(path));
    CHECK((readPrefabMetadata(*worldNode) | kdl::value()).className == "func_static");

    const auto& body = worldNode->defaultLayer()->children();
    REQUIRE(body.size() == 1);
    const auto* bodyNode = dynamic_cast<const EntityNode*>(body.front());
    REQUIRE(bodyNode);
    CHECK(property(*bodyNode, "origin") == "0 0 0");
    REQUIRE(bodyNode->childCount() == 1);
    CHECK(bodyNode->children().front()->logicalBounds().center() == vm::vec3d{0, 0, 0});
  }

  SECTION("Brush entity snapshot without an origin")
  {
    // the snapped bounds centre is the reference point (H-B-5); no origin key is added
    auto* entityNode = createEntity({{"classname", "func_static"}, {"name", "slab"}});
    entityNode->addChildren(
      {createBrushAt(map, {64, 0, 0}), createBrushAt(map, {96, 0, 0})});
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    const auto path =
      savePrefab(
        map, {entityNode}, SavePrefabOptions{.name = "Slab", .root = PrefabRoot::User})
      | kdl::value();

    const auto worldNode = readPrefab(map, savedFile(path));
    const auto& body = worldNode->defaultLayer()->children();
    REQUIRE(body.size() == 1);
    const auto* bodyNode = dynamic_cast<const EntityNode*>(body.front());
    REQUIRE(bodyNode);
    CHECK(property(*bodyNode, "origin") == "<missing>");
    REQUIRE(bodyNode->childCount() == 2);
    // the bounds are x 48..112, so the centre 80 0 0 goes to 0 0 0
    CHECK(bodyNode->logicalBounds().center() == vm::vec3d{0, 0, 0});
    CHECK(bodyNode->children()[0]->logicalBounds().center() == vm::vec3d{-16, 0, 0});
  }

  SECTION("Assembly round trip")
  {
    auto* brushNode = createBrushAt(map, {64, 0, 0});
    auto* lightNode = createEntity({
      {"classname", "light"},
      {"origin", "128 0 0"},
      {"name", "lamp"},
      {"target", "trigger"},
    });
    auto* triggerNode = createEntity(
      {{"classname", "path_corner"}, {"origin", "128 0 64"}, {"name", "trigger"}});

    auto* pillarNode1 = new GroupNode{Group{"pillar"}};
    pillarNode1->addChild(createBrushAt(map, {64, 64, 0}));
    auto* pillarNode2 =
      static_cast<GroupNode*>(pillarNode1->cloneRecursively(map.worldBounds()));

    addNodes(
      map,
      {{&parentForNodes(map),
        {brushNode, lightNode, triggerNode, pillarNode1, pillarNode2}}});
    REQUIRE(pillarNode1->linkId() == pillarNode2->linkId());
    const auto sourceLinkId = pillarNode1->linkId();

    const auto path =
      savePrefab(
        map,
        {brushNode, lightNode, triggerNode, pillarNode1, pillarNode2},
        SavePrefabOptions{.name = "Rig", .category = "rigs", .root = PrefabRoot::User})
      | kdl::value();
    CHECK(path == env.dir() / "user" / "rigs" / "Rig.map");

    const auto worldNode = readPrefab(map, savedFile(path));
    const auto metadata = readPrefabMetadata(*worldNode) | kdl::value();
    CHECK(metadata.kind == PrefabKind::Assembly);
    CHECK(metadata.className.empty());

    const auto& body = worldNode->defaultLayer()->children();
    REQUIRE(body.size() == 1);
    const auto* groupNode = dynamic_cast<const GroupNode*>(body.front());
    REQUIRE(groupNode);
    CHECK(groupNode->group().name() == "Rig");
    REQUIRE(groupNode->childCount() == 5);

    auto bodyBrushNodes = std::vector<const BrushNode*>{};
    auto bodyLightNode = static_cast<const EntityNode*>(nullptr);
    auto bodyPillarNodes = std::vector<const GroupNode*>{};
    for (const auto* child : groupNode->children())
    {
      if (const auto* childBrushNode = dynamic_cast<const BrushNode*>(child))
      {
        bodyBrushNodes.push_back(childBrushNode);
      }
      else if (const auto* childGroupNode = dynamic_cast<const GroupNode*>(child))
      {
        bodyPillarNodes.push_back(childGroupNode);
      }
      else if (const auto* childEntityNode = dynamic_cast<const EntityNode*>(child);
               childEntityNode && childEntityNode->entity().classname() == "light")
      {
        bodyLightNode = childEntityNode;
      }
    }

    // the bounds are x 48..144, y -16..80, z -16..72 (the definitions give the point
    // entities their size); the centre snapped to the grid, 96 32 32, goes to 0 0 0
    REQUIRE(bodyBrushNodes.size() == 1);
    CHECK(bodyBrushNodes.front()->logicalBounds().center() == vm::vec3d{-32, -32, -32});
    REQUIRE(bodyLightNode);
    CHECK(property(*bodyLightNode, "origin") == "32 -32 -32");
    // links inside the selection are kept
    CHECK(property(*bodyLightNode, "target") == "trigger");

    // the link set inside keeps its membership under a fresh id
    REQUIRE(bodyPillarNodes.size() == 2);
    const auto* bodyPillarNode1 = bodyPillarNodes[0];
    const auto* bodyPillarNode2 = bodyPillarNodes[1];
    CHECK(bodyPillarNode1->linkId() == bodyPillarNode2->linkId());
    CHECK(bodyPillarNode1->linkId() != sourceLinkId);
    CHECK(groupNode->linkId() != sourceLinkId);
    CHECK(groupNode->linkId() != bodyPillarNode1->linkId());

    // the map is not touched
    CHECK(pillarNode1->linkId() == sourceLinkId);
    CHECK(brushNode->logicalBounds().center() == vm::vec3d{64, 0, 0});
  }

  SECTION("Assembly of one group")
  {
    auto* pillarNode1 = new GroupNode{Group{"pillar"}};
    pillarNode1->addChild(createBrushAt(map, {64, 64, 0}));
    pillarNode1->addChild(
      createEntity({{"classname", "light"}, {"origin", "64 64 0"}, {"name", "lamp"}}));
    auto* pillarNode2 =
      static_cast<GroupNode*>(pillarNode1->cloneRecursively(map.worldBounds()));
    addNodes(map, {{&parentForNodes(map), {pillarNode1, pillarNode2}}});
    const auto sourceLinkId = pillarNode1->linkId();

    const auto path =
      savePrefab(
        map, {pillarNode1}, SavePrefabOptions{.name = "Pillar", .root = PrefabRoot::User})
      | kdl::value();

    const auto worldNode = readPrefab(map, savedFile(path));
    CHECK((readPrefabMetadata(*worldNode) | kdl::value()).kind == PrefabKind::Assembly);

    // the selected group is the prefab's group, renamed, not wrapped in another one
    const auto& body = worldNode->defaultLayer()->children();
    REQUIRE(body.size() == 1);
    const auto* groupNode = dynamic_cast<const GroupNode*>(body.front());
    REQUIRE(groupNode);
    CHECK(groupNode->group().name() == "Pillar");
    CHECK(groupNode->linkId() != sourceLinkId);
    REQUIRE(groupNode->childCount() == 2);
    CHECK(dynamic_cast<const BrushNode*>(groupNode->children()[0]));
    CHECK(dynamic_cast<const EntityNode*>(groupNode->children()[1]));
    CHECK(groupNode->logicalBounds().center() == vm::vec3d{0, 0, 0});

    // the map is not touched
    CHECK(pillarNode1->group().name() == "pillar");
    CHECK(pillarNode1->linkId() == sourceLinkId);

    SECTION("A group with something else is wrapped")
    {
      auto* lightNode =
        createEntity({{"classname", "light"}, {"origin", "0 0 0"}, {"name", "torch"}});
      addNodes(map, {{&parentForNodes(map), {lightNode}}});

      const auto wrappedWorldNode = readPrefab(
        map,
        savedFile(
          savePrefab(
            map,
            {pillarNode1, lightNode},
            SavePrefabOptions{.name = "Pillar", .root = PrefabRoot::User})
          | kdl::value()));

      const auto& wrappedBody = wrappedWorldNode->defaultLayer()->children();
      REQUIRE(wrappedBody.size() == 1);
      const auto* wrapperNode = dynamic_cast<const GroupNode*>(wrappedBody.front());
      REQUIRE(wrapperNode);
      CHECK(wrapperNode->group().name() == "Pillar");
      REQUIRE(wrapperNode->childCount() == 2);
      const auto* innerGroupNode =
        dynamic_cast<const GroupNode*>(wrapperNode->children().front());
      REQUIRE(innerGroupNode);
      CHECK(innerGroupNode->group().name() == "pillar");
    }
  }

  SECTION("Placed assemblies in the selection lose their prefab keys")
  {
    auto placedGroup = Group{"placed"};
    placedGroup.setPrefabPath("user/props/crate.map");
    placedGroup.setPrefabHash("af63dc4c8601ec8c");
    auto* placedNode = new GroupNode{std::move(placedGroup)};
    placedNode->addChild(createBrushAt(map, {0, 0, 0}));
    auto* lightNode = createEntity({{"classname", "light"}, {"origin", "64 0 0"}});
    addNodes(map, {{&parentForNodes(map), {placedNode, lightNode}}});

    const auto checkFile = [&](const std::vector<Node*>& nodes) {
      const auto contents = savedFile(
        savePrefab(map, nodes, SavePrefabOptions{.name = "Rig", .root = PrefabRoot::User})
        | kdl::value());
      CHECK(contents.find("\"_tb_prefab\"") == std::string::npos);
      CHECK(contents.find("\"_tb_prefab_hash\"") == std::string::npos);

      const auto worldNode = readPrefab(map, contents);
      auto groupNodes = std::vector<const GroupNode*>{};
      worldNode->accept(kdl::overload(
        [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
        [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
        [&](auto&& thisLambda, const GroupNode& node) {
          groupNodes.push_back(&node);
          node.visitChildren(thisLambda);
        },
        [](const EntityNode&) {},
        [](const BrushNode&) {},
        [](const PatchNode&) {}));
      CHECK(!groupNodes.empty());
      for (const auto* groupNode : groupNodes)
      {
        CHECK(groupNode->group().prefabPath() == std::nullopt);
        CHECK(groupNode->group().prefabHash() == std::nullopt);
      }
    };

    // as the prefab's own group, and inside the group that wraps a selection
    checkFile({placedNode});
    checkFile({placedNode, lightNode});

    // the map is not touched
    CHECK(placedNode->group().prefabPath() == "user/props/crate.map");
    CHECK(placedNode->group().prefabHash() == "af63dc4c8601ec8c");
  }

  SECTION("Errors")
  {
    auto* lightNode = createEntity({{"classname", "light"}, {"origin", "0 0 0"}});
    addNodes(map, {{&parentForNodes(map), {lightNode}}});

    CHECK(savePrefab(map, {}, SavePrefabOptions{.name = "Empty"}).is_error());
    // the name is made a file name
    CHECK(
      (savePrefab(map, {lightNode}, SavePrefabOptions{.name = " a/b: "}) | kdl::value())
      == env.dir() / "user" / "a_b_.map");
    CHECK(savePrefab(map, {lightNode}, SavePrefabOptions{.name = " . "}).is_error());
    CHECK(savePrefab(map, {lightNode}, SavePrefabOptions{.name = ""}).is_error());
    CHECK(savePrefab(
            map, {lightNode}, SavePrefabOptions{.name = "Light", .category = "../out"})
            .is_error());

    map.prefabLibrary().setUserRoot({});
    CHECK(savePrefab(map, {lightNode}, SavePrefabOptions{.name = "Light"}).is_error());
  }

  SECTION("readPrefabMetadata")
  {
    auto worldNode =
      WorldNode{{}, Entity{{{"classname", "worldspawn"}}}, MapFormat::Standard};
    CHECK(readPrefabMetadata(worldNode).is_error());

    worldNode.setEntity(Entity{{
      {"classname", "worldspawn"},
      {"_tb_prefab_kind", "assembly"},
      {"_tb_prefab_name", "Doorway"},
      {"_tb_prefab_class", "ignored"},
      {"_tb_prefab_tags", "door; arch;;"},
      {"_tb_prefab_saved", "2026-10-18T14:02:11"},
      {"_tb_prefab_source", "maps/game/hub.map"},
    }});
    const auto metadata = readPrefabMetadata(worldNode) | kdl::value();
    CHECK(metadata.kind == PrefabKind::Assembly);
    CHECK(metadata.name == "Doorway");
    CHECK(metadata.className.empty());
    CHECK(metadata.tags == std::vector<std::string>{"door", "arch"});
    CHECK(metadata.saved == "2026-10-18T14:02:11");
    CHECK(metadata.source == "maps/game/hub.map");

    worldNode.setEntity(
      Entity{{{"classname", "worldspawn"}, {"_tb_prefab_kind", "something"}}});
    CHECK(readPrefabMetadata(worldNode).is_error());
  }
}

} // namespace tb::mdl
