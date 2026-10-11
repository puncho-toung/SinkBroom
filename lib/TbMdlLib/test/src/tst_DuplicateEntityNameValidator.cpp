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
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
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
#include "mdl/PropertyDefinition.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/string_utils.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

std::vector<const Node*> issueNodes(const std::vector<const Issue*>& issues)
{
  return issues | std::views::transform([](const auto* issue) -> const Node* {
           return &issue->node();
         })
         | kdl::ranges::to<std::vector>();
}

std::string nameOf(const Node* node)
{
  const auto* entityNode = dynamic_cast<const EntityNodeBase*>(node);
  REQUIRE(entityNode != nullptr);
  const auto* name = entityNode->entity().property("name");
  return name ? *name : std::string{};
}

void applyQuickFix(
  Map& map,
  const IssueType type,
  const std::vector<const Issue*>& issues,
  const std::string& description = "Make Names Unique")
{
  const auto quickFixes = map.worldNode().quickFixes(type);
  const auto quickFix = std::ranges::find_if(
    quickFixes, [&](const auto* q) { return q->description() == description; });
  REQUIRE(quickFix != quickFixes.end());

  // like the issue browser
  auto transaction = Transaction{map, "Apply Quick Fix"};
  (*quickFix)->apply(map, issues);
  CHECK(transaction.commit());
}

} // namespace

TEST_CASE("DuplicateEntityNameValidator")
{
  using namespace Catch::Matchers;

  auto fixture = MapFixture{};
  auto& map = fixture.create(NamingFixtureConfig);

  const auto* validator =
    findValidator(map.worldNode(), "Duplicate or invalid entity name");
  REQUIRE(validator != nullptr);
  const auto type = validator->type();

  auto* entityNode1 = new EntityNode{Entity{{
    {"classname", "info_null"},
    {"name", "a"},
  }}};
  auto* entityNode2 = new EntityNode{Entity{{
    {"classname", "info_null"},
    {"name", "b"},
  }}};
  addNodes(map, {{&parentForNodes(map), {entityNode1, entityNode2}}});
  REQUIRE(collectIssues(map.worldNode(), type).empty());

  const auto renameEntity2 = [&](const std::string& name) {
    deselectAll(map);
    selectNodes(map, {entityNode2});
    REQUIRE(setEntityProperty(map, "name", name));
    deselectAll(map);
  };

  SECTION("Both entities report the clash, and neither does after the fix")
  {
    renameEntity2("a");

    const auto issues = collectIssues(map.worldNode(), type);
    CHECK_THAT(
      issueNodes(issues),
      UnorderedEquals(std::vector<const Node*>{entityNode1, entityNode2}));
    CHECK(
      issues.front()->description()
      == "info_null shares the name 'a' with 1 other entity");

    applyQuickFix(map, type, issues);

    // the first entity in the map keeps the name
    CHECK(nameOf(entityNode1) == "a");
    CHECK(nameOf(entityNode2) == "info_null_0");
    CHECK(collectIssues(map.worldNode(), type).empty());

    SECTION("Undo brings the issues back")
    {
      map.undoCommand();
      CHECK(nameOf(entityNode2) == "a");
      CHECK_THAT(
        issueNodes(collectIssues(map.worldNode(), type)),
        UnorderedEquals(std::vector<const Node*>{entityNode1, entityNode2}));
    }
  }

  SECTION("Names that differ only in case clash, as the engine may find either")
  {
    auto* lightNode1 = new EntityNode{Entity{{
      {"classname", "light"},
      {"name", "Light_x"},
    }}};
    auto* lightNode2 = new EntityNode{Entity{{
      {"classname", "light"},
      {"name", "light_x"},
    }}};
    addNodes(map, {{&parentForNodes(map), {lightNode1, lightNode2}}});

    const auto issues = collectIssues(map.worldNode(), type);
    CHECK_THAT(
      issueNodes(issues),
      UnorderedEquals(std::vector<const Node*>{lightNode1, lightNode2}));
    CHECK(issues.front()->description().ends_with(
      " with 1 other entity if case is ignored, as the engine ignores it"));

    applyQuickFix(map, type, issues);

    // the first entity in the map keeps its name
    CHECK(nameOf(lightNode1) == "Light_x");
    CHECK(nameOf(lightNode2) == "light_0");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("A name that the engine reserves is reported and replaced")
  {
    renameEntity2("null_entity");

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issueNodes(issues) == std::vector<const Node*>{entityNode2});
    CHECK(
      issues.front()->description()
      == "info_null has the name 'null_entity', which the engine reserves");

    applyQuickFix(map, type, issues);
    CHECK(nameOf(entityNode2) == "info_null_0");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("A reference to the entity itself is reported and removed")
  {
    map.entityDefinitionManager().setDefinitions({
      {"trigger_once",
       Color{},
       "",
       {
         PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
         PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
       },
       PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
    });

    auto* triggerNode = new EntityNode{Entity{{
      {"classname", "trigger_once"},
      {"name", "trigger_once_0"},
      {"target", "a"},
      {"target2", "Trigger_Once_0"},
    }}};
    addNodes(map, {{&parentForNodes(map), {triggerNode}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issueNodes(issues) == std::vector<const Node*>{triggerNode});
    CHECK(
      static_cast<const EntityPropertyIssue*>(issues.front())->propertyKey()
      == "target2");
    CHECK(
      issues.front()->description()
      == "trigger_once 'trigger_once_0' refers to itself with 'target2'");

    // the names stay as they are
    applyQuickFix(map, type, issues);
    CHECK(collectIssues(map.worldNode(), type).size() == 1u);

    applyQuickFix(map, type, issues, "Remove References to Itself");
    CHECK(triggerNode->entity().property("target2") == nullptr);
    CHECK(*triggerNode->entity().property("target") == "a");
    CHECK(nameOf(triggerNode) == "trigger_once_0");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Removing one entity removes the issue of the other")
  {
    renameEntity2("a");
    REQUIRE(collectIssues(map.worldNode(), type).size() == 2u);

    selectNodes(map, {entityNode2});
    removeSelectedNodes(map);
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Adding an entity with a taken name reports both entities")
  {
    auto* entityNode3 = new EntityNode{Entity{{
      {"classname", "light"},
      {"name", "a"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode3}}});

    CHECK_THAT(
      issueNodes(collectIssues(map.worldNode(), type)),
      UnorderedEquals(std::vector<const Node*>{entityNode1, entityNode3}));
  }

  SECTION("The worldspawn takes part but keeps its name")
  {
    deselectAll(map);
    REQUIRE(setEntityProperty(map, "name", "a"));

    const auto issues = collectIssues(map.worldNode(), type);
    CHECK_THAT(
      issueNodes(issues),
      UnorderedEquals(std::vector<const Node*>{&map.worldNode(), entityNode1}));

    applyQuickFix(map, type, issues);
    CHECK(nameOf(&map.worldNode()) == "a");
    CHECK(nameOf(entityNode1) == "info_null_0");
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("A brush entity gets its new name as its model")
  {
    auto* staticNode = new EntityNode{Entity{{
      {"classname", "func_static"},
      {"name", "a"},
      {"model", "a"},
    }}};
    addNodes(map, {{&parentForNodes(map), {staticNode}}});
    addNodes(map, {{staticNode, {createBrushNode(map)}}});

    applyQuickFix(map, type, collectIssues(map.worldNode(), type));
    CHECK(nameOf(entityNode1) == "a");
    CHECK(nameOf(staticNode) == "func_static_0");
    CHECK(*staticNode->entity().property("model") == "func_static_0");
  }

  SECTION("The fix works across the instances of a linked group")
  {
    deselectAll(map);
    selectNodes(map, {entityNode1});
    auto* groupNode = groupSelectedNodes(map, "group");
    REQUIRE(groupNode != nullptr);

    deselectAll(map);
    selectNodes(map, {groupNode});
    auto* linkedGroupNode1 = createLinkedDuplicate(map);
    REQUIRE(linkedGroupNode1 != nullptr);
    deselectAll(map);
    selectNodes(map, {groupNode});
    auto* linkedGroupNode2 = createLinkedDuplicate(map);
    REQUIRE(linkedGroupNode2 != nullptr);
    deselectAll(map);

    auto* linkedEntityNode1 = linkedGroupNode1->children().front();
    auto* linkedEntityNode2 = linkedGroupNode2->children().front();
    REQUIRE(nameOf(linkedEntityNode1) != "a");
    REQUIRE(nameOf(linkedEntityNode2) != "a");

    // give all three instances the same name without updating the linked groups
    const auto withName = [](const Node* node, const std::string& name) {
      auto entity = static_cast<const EntityNode*>(node)->entity();
      entity.addOrUpdateProperty("name", name);
      return NodeContents{std::move(entity)};
    };
    auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
    nodesToSwap.emplace_back(linkedEntityNode1, withName(linkedEntityNode1, "a"));
    nodesToSwap.emplace_back(linkedEntityNode2, withName(linkedEntityNode2, "a"));
    REQUIRE(updateNodeContents(map, "Drift", std::move(nodesToSwap), {}));

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 3u);

    applyQuickFix(map, type, issues);

    CHECK(nameOf(entityNode1) == "a");
    const auto name1 = nameOf(linkedGroupNode1->children().front());
    const auto name2 = nameOf(linkedGroupNode2->children().front());
    CHECK(name1 != "a");
    CHECK(name2 != "a");
    CHECK(name1 != name2);
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Loading a map with duplicate names leaves it unchanged")
  {
    auto env = fs::TestEnvironment{};
    env.createFile("duplicates.map", R"(// Game: Test
// Format: Valve
// entity 0
{
"classname" "worldspawn"
}
// entity 1
{
"classname" "info_null"
"name" "twin"
}
// entity 2
{
"classname" "info_null"
"name" "twin"
}
)");

    auto fixtureConfig = NamingFixtureConfig;
    fixtureConfig.gameInfo.gameConfig.fileFormats = {
      {.format = "Valve", .initialMap = ""}};

    auto loadFixture = MapFixture{};
    auto& loadedMap = loadFixture.load(env.dir() / "duplicates.map", fixtureConfig);

    CHECK_FALSE(loadedMap.modified());
    CHECK_FALSE(loadedMap.canUndoCommand());

    const auto& entityNodes = loadedMap.worldNode().defaultLayer()->children();
    REQUIRE(entityNodes.size() == 2u);
    CHECK(nameOf(entityNodes[0]) == "twin");
    CHECK(nameOf(entityNodes[1]) == "twin");

    const auto* loadedValidator =
      findValidator(loadedMap.worldNode(), "Duplicate or invalid entity name");
    REQUIRE(loadedValidator != nullptr);
    CHECK(collectIssues(loadedMap.worldNode(), loadedValidator->type()).size() == 2u);

    // reporting the issues changes nothing either
    CHECK_FALSE(loadedMap.modified());
    CHECK_FALSE(loadedMap.canUndoCommand());

    REQUIRE(loadedMap.saveAs(env.dir() / "saved.map"));
    const auto savedLines = kdl::str_split(env.loadFile("saved.map"), "\n");
    CHECK(std::ranges::count(savedLines, std::string{R"("name" "twin")"}) == 2);
  }

  SECTION("No naming validators for a game without naming rules")
  {
    auto& defaultMap = fixture.create();
    CHECK(
      findValidator(defaultMap.worldNode(), "Duplicate or invalid entity name")
      == nullptr);
    CHECK(
      findValidator(defaultMap.worldNode(), "Brush entity model differs from its name")
      == nullptr);
  }
}

} // namespace tb::mdl
