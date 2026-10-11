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

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

EntityNode* makeEntityNode(std::vector<EntityProperty> properties)
{
  return new EntityNode{Entity{std::move(properties)}};
}

EntityNode* addEntity(Map& map, std::vector<EntityProperty> properties)
{
  auto* entityNode = makeEntityNode(std::move(properties));
  addNodes(map, {{&parentForNodes(map), {entityNode}}});
  return entityNode;
}

const std::string* nameOf(const EntityNode& entityNode)
{
  return entityNode.entity().property("name");
}

} // namespace

TEST_CASE("EntityNaming")
{
  auto fixture = MapFixture{};
  auto& map = fixture.create(NamingFixtureConfig);

  SECTION("EntityNameAllocator")
  {
    SECTION("allocate")
    {
      SECTION("an empty map hands out N = 0")
      {
        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_0");
      }

      SECTION("the lowest free N is handed out")
      {
        addEntity(map, {{"classname", "light"}, {"name", "light_0"}});
        addEntity(map, {{"classname", "light"}, {"name", "light_2"}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_1");
        CHECK(allocator.allocate("light") == "light_3");
      }

      SECTION("names are compared without regard to case")
      {
        addEntity(map, {{"classname", "light"}, {"name", "Light_0"}});
        addEntity(map, {{"classname", "light"}, {"name", "LIGHT_1"}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_2");
        CHECK(allocator.allocate("Light") == "Light_3");
      }

      SECTION("only the naming key holds a name")
      {
        // a link source value, a group name and a _tb_ key do not take a name
        addEntity(map, {{"classname", "trigger_once"}, {"target", "light_0"}});
        addEntity(map, {{"classname", "info_null"}, {"_tb_name", "light_0"}});
        addEntity(map, {{"classname", "info_null"}, {"light_0", "value"}});
        addNodes(map, {{&parentForNodes(map), {new GroupNode{Group{"light_0"}}}}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_0");
      }

      SECTION("names with leading zeros or other suffixes do not block an N")
      {
        addEntity(map, {{"classname", "light"}, {"name", "light_00"}});
        addEntity(map, {{"classname", "light"}, {"name", "light_0a"}});
        addEntity(map, {{"classname", "light"}, {"name", "light_"}});
        addEntity(map, {{"classname", "light"}, {"name", "lights_0"}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_0");
      }

      SECTION("special characters in the classname are not patterns")
      {
        addEntity(map, {{"classname", "a*b"}, {"name", "a*b_0"}});
        addEntity(map, {{"classname", "axb"}, {"name", "axb_1"}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("a*b") == "a*b_1");
        CHECK(allocator.allocate("a?b") == "a?b_0");
      }

      SECTION("a batch gets distinct names")
      {
        addEntity(map, {{"classname", "light"}, {"name", "light_1"}});

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.allocate("light") == "light_0");
        CHECK(allocator.allocate("light") == "light_2");
        CHECK(allocator.allocate("light") == "light_3");
        CHECK(allocator.allocate("info_null") == "info_null_0");
      }

      SECTION("reserved names are skipped")
      {
        auto allocator = EntityNameAllocator{map};
        allocator.reserve("LIGHT_0");
        CHECK(allocator.allocate("light") == "light_1");
      }

      SECTION("the worldspawn's name is taken")
      {
        REQUIRE(setEntityProperty(map, "name", "light_0"));
        REQUIRE(map.worldNode().entity().property("name"));

        auto allocator = EntityNameAllocator{map};
        CHECK(allocator.isTaken("light_0"));
        CHECK(allocator.allocate("light") == "light_1");
      }

      SECTION("entities in excluded subtrees do not take a name")
      {
        auto* groupNode = new GroupNode{Group{"group"}};
        groupNode->addChild(
          makeEntityNode({{"classname", "light"}, {"name", "light_0"}}));
        addNodes(map, {{&parentForNodes(map), {groupNode}}});
        auto* entityNode = addEntity(map, {{"classname", "light"}, {"name", "light_1"}});

        auto allocator = EntityNameAllocator{map, {groupNode, entityNode}};
        CHECK_FALSE(allocator.isTaken("light_0"));
        CHECK_FALSE(allocator.isTaken("light_1"));
        CHECK(allocator.allocate("light") == "light_0");
      }
    }

    SECTION("isTaken")
    {
      addEntity(map, {{"classname", "func_door"}, {"name", "Big_Door"}});
      addEntity(map, {{"classname", "info_null"}, {"target", "small_door"}});

      auto allocator = EntityNameAllocator{map};
      CHECK(allocator.isTaken("Big_Door"));
      CHECK(allocator.isTaken("big_door"));
      CHECK(allocator.isTaken("BIG_DOOR"));
      CHECK_FALSE(allocator.isTaken("big_doors"));
      CHECK_FALSE(allocator.isTaken("small_door"));

      allocator.reserve("small_door");
      CHECK(allocator.isTaken("Small_Door"));

      // the engine refuses to spawn entities with these names
      CHECK(allocator.isTaken("NULL"));
      CHECK(allocator.isTaken("null_entity"));
    }
  }

  SECTION("isBrushEntityModelClass")
  {
    CHECK(isBrushEntityModelClass("func_static"));
    CHECK(isBrushEntityModelClass("light"));
    CHECK_FALSE(isBrushEntityModelClass("func_group"));
    CHECK_FALSE(isBrushEntityModelClass("worldspawn"));
  }

  SECTION("assignUniqueEntityNames")
  {
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    const auto makeBrushNode = [&]() {
      return new BrushNode{builder.createCube(64.0, "material") | kdl::value()};
    };

    addEntity(map, {{"classname", "light"}, {"name", "light_0"}});
    addEntity(map, {{"classname", "func_static"}, {"name", "door"}});

    auto* lightNode = makeEntityNode({{"classname", "light"}, {"name", "light_0"}});
    auto* namedNode = makeEntityNode({{"classname", "info_null"}, {"name", "free_name"}});
    auto* unnamedNode = makeEntityNode({{"classname", "info_null"}});

    auto* brushEntityNode =
      makeEntityNode({{"classname", "func_static"}, {"name", "door"}, {"model", "door"}});
    brushEntityNode->addChild(makeBrushNode());

    auto* funcGroupNode = makeEntityNode({{"classname", "func_group"}});
    funcGroupNode->addChild(makeBrushNode());

    auto* groupNode = new GroupNode{Group{"group"}};
    groupNode->addChildren({unnamedNode, brushEntityNode, funcGroupNode});

    const auto roots = std::vector<Node*>{lightNode, namedNode, groupNode};
    auto nodes = std::vector<std::unique_ptr<Node>>{};
    for (auto* root : roots)
    {
      nodes.emplace_back(root);
    }

    SECTION("Create mode names every entity")
    {
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::Create);

      CHECK(*nameOf(*lightNode) == "light_1");
      CHECK(*nameOf(*namedNode) == "info_null_0");
      CHECK(*nameOf(*unnamedNode) == "info_null_1");
      CHECK(*nameOf(*brushEntityNode) == "func_static_0");
      CHECK(*brushEntityNode->entity().property("model") == "func_static_0");

      // func_group is named, but gets no model
      CHECK(*nameOf(*funcGroupNode) == "func_group_0");
      CHECK(funcGroupNode->entity().property("model") == nullptr);

      CHECK(
        result.renamed
        == std::vector<std::pair<std::string, std::string>>{
          {"light_0", "light_1"},
          {"free_name", "info_null_0"},
          {"", "info_null_1"},
          {"door", "func_static_0"},
          {"", "func_group_0"},
        });
      CHECK(result.kept.empty());
    }

    SECTION("KeepIfFree mode keeps names that no other entity holds")
    {
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);

      CHECK(*nameOf(*lightNode) == "light_1");
      CHECK(*nameOf(*namedNode) == "free_name");
      CHECK(*nameOf(*unnamedNode) == "info_null_0");
      CHECK(*nameOf(*brushEntityNode) == "func_static_0");
      CHECK(*brushEntityNode->entity().property("model") == "func_static_0");

      CHECK(
        result.renamed
        == std::vector<std::pair<std::string, std::string>>{
          {"light_0", "light_1"},
          {"", "info_null_0"},
          {"door", "func_static_0"},
          {"", "func_group_0"},
        });
      CHECK(result.kept == std::vector<std::string>{"free_name"});
    }

    SECTION("KeepIfFree mode renames the second of two entities with one name")
    {
      auto* firstNode = makeEntityNode({{"classname", "info_null"}, {"name", "twin"}});
      auto* secondNode = makeEntityNode({{"classname", "info_null"}, {"name", "Twin"}});
      nodes.emplace_back(firstNode);
      nodes.emplace_back(secondNode);

      auto allocator = EntityNameAllocator{map};
      assignUniqueEntityNames(
        {firstNode, secondNode}, allocator, EntityNamingMode::KeepIfFree);

      CHECK(*nameOf(*firstNode) == "twin");
      CHECK(*nameOf(*secondNode) == "info_null_0");
    }

    SECTION("KeepIfFree mode sets a missing or wrong model")
    {
      auto* entityNode =
        makeEntityNode({{"classname", "func_static"}, {"name", "free"}, {"model", "x"}});
      entityNode->addChild(makeBrushNode());
      nodes.emplace_back(entityNode);

      auto allocator = EntityNameAllocator{map};
      assignUniqueEntityNames({entityNode}, allocator, EntityNamingMode::KeepIfFree);

      CHECK(*nameOf(*entityNode) == "free");
      CHECK(*entityNode->entity().property("model") == "free");
    }

    SECTION("without a brush entity model key, no model is set")
    {
      auto otherFixture = MapFixture{};
      auto& mapWithoutModels = otherFixture.create(makeNamingFixtureConfig(std::nullopt));

      auto allocator = EntityNameAllocator{mapWithoutModels};
      assignUniqueEntityNames(roots, allocator, EntityNamingMode::Create);

      CHECK(*nameOf(*brushEntityNode) == "func_static_0");
      CHECK(*brushEntityNode->entity().property("model") == "door");
    }

    SECTION("names are handed out in the given order")
    {
      auto allocator = EntityNameAllocator{map};
      assignUniqueEntityNames(
        {groupNode, namedNode, lightNode}, allocator, EntityNamingMode::Create);

      CHECK(*nameOf(*unnamedNode) == "info_null_0");
      CHECK(*nameOf(*namedNode) == "info_null_1");
    }
  }

  SECTION("remapLinkSourceValues")
  {
    map.entityDefinitionManager().setDefinitions({
      {"light",
       Color{},
       "",
       {
         PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
         PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
       },
       PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
      {"trigger_once",
       Color{},
       "",
       {
         PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
         PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
       }},
    });

    addEntity(map, {{"classname", "light"}, {"name", "light_0"}});

    auto* lightNode = makeEntityNode({{"classname", "light"}, {"name", "light_0"}});
    auto* triggerNode = makeEntityNode({
      {"classname", "trigger_once"},
      {"name", "trigger_once_0"},
      {"target", "light_0"},
      {"target2", "light_0"},
      {"target3", "other"},
      {"message", "light_0"},
    });
    auto nodes = std::vector<std::unique_ptr<Node>>{};
    nodes.emplace_back(lightNode);
    nodes.emplace_back(triggerNode);

    SECTION("references to renamed entities of the batch are changed")
    {
      const auto roots = std::vector<Node*>{lightNode, triggerNode};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result);

      CHECK(*nameOf(*lightNode) == "light_1");
      CHECK(*triggerNode->entity().property("target") == "light_1");
      CHECK(*triggerNode->entity().property("target2") == "light_1");
      CHECK(*triggerNode->entity().property("target3") == "other");
      // not a link source
      CHECK(*triggerNode->entity().property("message") == "light_0");
    }

    SECTION("references to entities outside the batch are kept")
    {
      const auto roots = std::vector<Node*>{triggerNode};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result);

      CHECK(*triggerNode->entity().property("target") == "light_0");
    }

    SECTION("a name that an entity of the batch kept is not remapped")
    {
      auto* otherLightNode = makeEntityNode({{"classname", "light"}, {"name", "lamp"}});
      auto* secondLightNode = makeEntityNode({{"classname", "light"}, {"name", "lamp"}});
      auto* otherTriggerNode =
        makeEntityNode({{"classname", "trigger_once"}, {"target", "lamp"}});
      nodes.emplace_back(otherLightNode);
      nodes.emplace_back(secondLightNode);
      nodes.emplace_back(otherTriggerNode);

      const auto roots =
        std::vector<Node*>{otherLightNode, secondLightNode, otherTriggerNode};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result);

      CHECK(*nameOf(*otherLightNode) == "lamp");
      CHECK(*nameOf(*secondLightNode) == "light_1");
      CHECK(*otherTriggerNode->entity().property("target") == "lamp");
    }

    SECTION("references are matched without regard to case, as the engine does")
    {
      auto* upperCaseTriggerNode =
        makeEntityNode({{"classname", "trigger_once"}, {"target", "LIGHT_0"}});
      nodes.emplace_back(upperCaseTriggerNode);

      const auto roots = std::vector<Node*>{lightNode, upperCaseTriggerNode};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result);

      CHECK(*nameOf(*lightNode) == "light_1");
      CHECK(*upperCaseTriggerNode->entity().property("target") == "light_1");
    }

    SECTION("renames that form a chain follow each entity")
    {
      // the map has light_0, so the batch's light_0 becomes light_1, which moves the
      // batch's light_1 on to light_2; each trigger must follow its own light
      auto* light0Node = makeEntityNode({{"classname", "light"}, {"name", "light_0"}});
      auto* trigger0Node =
        makeEntityNode({{"classname", "trigger_once"}, {"target", "light_0"}});
      auto* light1Node = makeEntityNode({{"classname", "light"}, {"name", "light_1"}});
      auto* trigger1Node =
        makeEntityNode({{"classname", "trigger_once"}, {"target", "light_1"}});
      nodes.emplace_back(light0Node);
      nodes.emplace_back(trigger0Node);
      nodes.emplace_back(light1Node);
      nodes.emplace_back(trigger1Node);

      const auto roots =
        std::vector<Node*>{light0Node, trigger0Node, light1Node, trigger1Node};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result);

      const auto light0Name = *nameOf(*light0Node);
      const auto light1Name = *nameOf(*light1Node);
      CHECK(light0Name != "light_0");
      CHECK(light0Name != light1Name);
      CHECK(*trigger0Node->entity().property("target") == light0Name);
      CHECK(*trigger1Node->entity().property("target") == light1Name);
    }

    SECTION("groups that stay linked are skipped")
    {
      auto* groupNode = new GroupNode{Group{"group"}};
      auto* groupedTriggerNode =
        makeEntityNode({{"classname", "trigger_once"}, {"target", "light_0"}});
      groupNode->addChild(groupedTriggerNode);
      nodes.emplace_back(groupNode);

      const auto roots = std::vector<Node*>{lightNode, triggerNode, groupNode};
      auto allocator = EntityNameAllocator{map};
      const auto result =
        assignUniqueEntityNames(roots, allocator, EntityNamingMode::KeepIfFree);
      remapLinkSourceValues(map.entityDefinitionManager(), roots, result, {groupNode});

      CHECK(*nameOf(*lightNode) == "light_1");
      CHECK(*triggerNode->entity().property("target") == "light_1");
      CHECK(*groupedTriggerNode->entity().property("target") == "light_0");
    }
  }

  SECTION("assignUniqueEntityNamesBeforeAdding")
  {
    SECTION("names and remaps for a game with naming rules")
    {
      auto& namingMap = map;
      namingMap.entityDefinitionManager().setDefinitions({
        {"trigger_once",
         Color{},
         "",
         {
           PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
         }},
      });
      addEntity(namingMap, {{"classname", "light"}, {"name", "light_0"}});

      auto lightNode = std::unique_ptr<EntityNode>{
        makeEntityNode({{"classname", "light"}, {"name", "light_0"}})};
      auto triggerNode = std::unique_ptr<EntityNode>{
        makeEntityNode({{"classname", "trigger_once"}, {"target", "light_0"}})};

      SECTION("with remapping")
      {
        assignUniqueEntityNamesBeforeAdding(
          namingMap, {lightNode.get(), triggerNode.get()}, true);

        CHECK(*nameOf(*lightNode) == "light_1");
        CHECK(*nameOf(*triggerNode) == "trigger_once_0");
        CHECK(*triggerNode->entity().property("target") == "light_1");
      }

      SECTION("without remapping")
      {
        assignUniqueEntityNamesBeforeAdding(
          namingMap, {lightNode.get(), triggerNode.get()}, false);

        CHECK(*nameOf(*lightNode) == "light_1");
        CHECK(*triggerNode->entity().property("target") == "light_0");
      }
    }
  }

  SECTION("findEntitiesWithName")
  {
    auto* entityNode1 = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    auto* entityNode2 = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    auto* entityNode3 = addEntity(map, {{"classname", "light"}, {"name", "Lamp"}});
    addEntity(map, {{"classname", "light"}, {"target", "lamp"}});

    CHECK_THAT(
      findEntitiesWithName(map, "LAMP"),
      Catch::Matchers::UnorderedEquals(
        std::vector<EntityNodeBase*>{entityNode1, entityNode2, entityNode3}));
    CHECK(findEntitiesWithName(map, "").empty());

    SECTION("the index follows renames and removals")
    {
      deselectAll(map);
      selectNodes(map, {entityNode1});
      REQUIRE(setEntityProperty(map, "name", "lantern"));
      CHECK(
        findEntitiesWithName(map, "lantern")
        == std::vector<EntityNodeBase*>{entityNode1});

      deselectAll(map);
      selectNodes(map, {entityNode2});
      removeSelectedNodes(map);
      CHECK(
        findEntitiesWithName(map, "lamp") == std::vector<EntityNodeBase*>{entityNode3});
    }

    SECTION("a game without naming rules has no index")
    {
      auto otherFixture = MapFixture{};
      auto& defaultMap = otherFixture.create();
      addEntity(defaultMap, {{"classname", "light"}, {"name", "lamp"}});

      CHECK(defaultMap.entityNameIndex() == nullptr);
      CHECK(findEntitiesWithName(defaultMap, "lamp").empty());
    }
  }
}

} // namespace tb::mdl
