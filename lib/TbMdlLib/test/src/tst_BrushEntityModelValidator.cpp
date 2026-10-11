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

#include "fs/TestEnvironment.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/string_utils.h"

#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

std::string propertyOf(const EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  return value ? *value : std::string{};
}

} // namespace

TEST_CASE("BrushEntityModelValidator")
{
  auto fixture = MapFixture{};
  auto& map = fixture.create(NamingFixtureConfig);

  const auto* validator =
    findValidator(map.worldNode(), "Brush entity model differs from its name");
  REQUIRE(validator != nullptr);
  const auto type = validator->type();

  auto* entityNode = new EntityNode{Entity{{
    {"classname", "func_static"},
    {"name", "door"},
    {"model", "door"},
  }}};
  auto* brushNode = createBrushNode(map);
  addNodes(map, {{&parentForNodes(map), {entityNode}}});
  addNodes(map, {{entityNode, {brushNode}}});
  REQUIRE(collectIssues(map.worldNode(), type).empty());

  // the properties of a brush entity are edited by selecting its brushes
  const auto editEntity = [&](const auto& edit) {
    deselectAll(map);
    selectNodes(map, {brushNode});
    edit();
    deselectAll(map);
  };

  const auto applyQuickFix = [&](const std::vector<const Issue*>& issues) {
    const auto quickFixes = map.worldNode().quickFixes(type);
    REQUIRE(quickFixes.size() == 1);

    auto transaction = Transaction{map, "Apply Quick Fix"};
    quickFixes.front()->apply(map, issues);
    CHECK(transaction.commit());
  };

  SECTION("A missing model is reported and set to the name")
  {
    editEntity([&]() { REQUIRE(removeEntityProperty(map, "model")); });

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 1u);
    CHECK(
      issues.front()->description()
      == "func_static 'door' has brushes or patches but no 'model' property");
    CHECK(
      static_cast<const EntityPropertyIssue*>(issues.front())->propertyKey() == "model");

    applyQuickFix(issues);
    CHECK(propertyOf(*entityNode, "model") == "door");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Renaming an entity does not change its model, which is reported")
  {
    editEntity([&]() { REQUIRE(setEntityProperty(map, "name", "gate")); });
    CHECK(propertyOf(*entityNode, "model") == "door");

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 1u);
    CHECK(
      issues.front()->description()
      == "func_static 'gate' has the model 'door', which differs from its name");

    applyQuickFix(issues);
    CHECK(propertyOf(*entityNode, "name") == "gate");
    CHECK(propertyOf(*entityNode, "model") == "gate");
    CHECK(collectIssues(map.worldNode(), type).empty());

    SECTION("Undo restores the model")
    {
      map.undoCommand();
      CHECK(propertyOf(*entityNode, "model") == "door");
    }
  }

  SECTION("A model that differs from the name only in case is no issue")
  {
    editEntity([&]() { REQUIRE(setEntityProperty(map, "model", "DOOR")); });
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("A missing name is reported and replaced by a unique one")
  {
    editEntity([&]() { REQUIRE(removeEntityProperty(map, "name")); });

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 1u);
    CHECK(
      issues.front()->description()
      == "func_static has brushes or patches but no 'name' property");

    applyQuickFix(issues);
    CHECK(propertyOf(*entityNode, "name") == "func_static_0");
    CHECK(propertyOf(*entityNode, "model") == "func_static_0");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Two entities without names get distinct names in one fix")
  {
    auto* entityNode1 = new EntityNode{Entity{{{"classname", "func_static"}}}};
    auto* entityNode2 = new EntityNode{Entity{{{"classname", "func_static"}}}};
    addNodes(map, {{&parentForNodes(map), {entityNode1, entityNode2}}});
    addNodes(map, {{entityNode1, {createBrushNode(map)}}});
    addNodes(map, {{entityNode2, {createBrushNode(map)}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 2u);

    applyQuickFix(issues);
    CHECK(propertyOf(*entityNode1, "name") == "func_static_0");
    CHECK(propertyOf(*entityNode1, "model") == "func_static_0");
    CHECK(propertyOf(*entityNode2, "name") == "func_static_1");
    CHECK(propertyOf(*entityNode2, "model") == "func_static_1");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("The fix works across the instances of a linked group")
  {
    deselectAll(map);
    selectNodes(map, {entityNode});
    auto* groupNode = groupSelectedNodes(map, "group");
    REQUIRE(groupNode != nullptr);

    auto linkedGroupNodes = std::vector<GroupNode*>{groupNode};
    for (auto i = 0; i < 2; ++i)
    {
      deselectAll(map);
      selectNodes(map, {groupNode});
      linkedGroupNodes.push_back(createLinkedDuplicate(map));
      REQUIRE(linkedGroupNodes.back() != nullptr);
    }
    deselectAll(map);

    // let the models drift without updating the linked groups
    auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
    for (auto* linkedGroupNode : linkedGroupNodes)
    {
      auto* linkedEntityNode = linkedGroupNode->children().front();
      auto entity = static_cast<EntityNode*>(linkedEntityNode)->entity();
      entity.addOrUpdateProperty("model", "drift");
      nodesToSwap.emplace_back(linkedEntityNode, NodeContents{std::move(entity)});
    }
    REQUIRE(updateNodeContents(map, "Drift", std::move(nodesToSwap), {}));

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 3u);

    applyQuickFix(issues);

    auto names = std::vector<std::string>{};
    for (const auto* linkedGroupNode : linkedGroupNodes)
    {
      const auto* linkedEntityNode =
        static_cast<const EntityNode*>(linkedGroupNode->children().front());
      CHECK(
        propertyOf(*linkedEntityNode, "model") == propertyOf(*linkedEntityNode, "name"));
      names.push_back(propertyOf(*linkedEntityNode, "name"));
    }
    CHECK(names == std::vector<std::string>{"door", "func_static_0", "func_static_1"});
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Loading and saving a map changes no names and no models")
  {
    // an unnamed point entity and a brush entity without a model
    const auto mapText = std::string{R"(// Game: Test
// Format: Valve
// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"classname" "info_null"
"origin" "0 0 0"
}
// entity 2
{
"classname" "func_static"
"name" "door"
// brush 0
{
( -16 -16 -16 ) ( -16 -15 -16 ) ( -16 -16 -15 ) material [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -16 -16 -16 ) ( -16 -16 -15 ) ( -15 -16 -16 ) material [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -16 -16 -16 ) ( -15 -16 -16 ) ( -16 -15 -16 ) material [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 16 16 16 ) ( 16 17 16 ) ( 17 16 16 ) material [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 16 16 16 ) ( 17 16 16 ) ( 16 16 17 ) material [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 16 16 16 ) ( 16 16 17 ) ( 16 17 16 ) material [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
}
}
)"};

    auto env = fs::TestEnvironment{};
    env.createFile("unnamed.map", mapText);

    auto fixtureConfig = NamingFixtureConfig;
    fixtureConfig.gameInfo.gameConfig.fileFormats = {
      {.format = "Valve", .initialMap = ""}};

    auto loadFixture = MapFixture{};
    auto& loadedMap = loadFixture.load(env.dir() / "unnamed.map", fixtureConfig);
    CHECK_FALSE(loadedMap.modified());
    CHECK_FALSE(loadedMap.canUndoCommand());

    const auto* loadedValidator =
      findValidator(loadedMap.worldNode(), "Brush entity model differs from its name");
    REQUIRE(loadedValidator != nullptr);
    CHECK(collectIssues(loadedMap.worldNode(), loadedValidator->type()).size() == 1u);
    CHECK_FALSE(loadedMap.canUndoCommand());

    REQUIRE(loadedMap.saveAs(env.dir() / "saved.map"));

    const auto propertyLines = [](const std::string& text) {
      return kdl::str_split(text, "\n")
             | std::views::filter([](const auto& line) { return line.starts_with("\""); })
             | kdl::ranges::to<std::vector>();
    };
    CHECK(propertyLines(env.loadFile("saved.map")) == propertyLines(mapText));
  }

  SECTION("Point entities, func_group and the world are not checked")
  {
    auto* pointEntityNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"name", "light_0"},
    }}};
    auto* funcGroupNode = new EntityNode{Entity{{
      {"classname", "func_group"},
    }}};
    addNodes(map, {{&parentForNodes(map), {pointEntityNode, funcGroupNode}}});
    addNodes(map, {{funcGroupNode, {createBrushNode(map)}}});
    addNodes(map, {{&parentForNodes(map), {createBrushNode(map)}}});

    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Without a brush entity model key, there is no model validator")
  {
    auto& mapWithoutModels = fixture.create(makeNamingFixtureConfig(std::nullopt));
    CHECK(
      findValidator(
        mapWithoutModels.worldNode(), "Brush entity model differs from its name")
      == nullptr);
    CHECK(
      findValidator(mapWithoutModels.worldNode(), "Duplicate or invalid entity name")
      != nullptr);
  }
}

} // namespace tb::mdl
