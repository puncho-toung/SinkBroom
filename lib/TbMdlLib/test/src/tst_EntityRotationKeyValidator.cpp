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

#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto Description =
  std::string{"Entity rotation property that TrenchBroom does not write"};
const auto Yaw90 = std::string{"0 1 0 -1 0 0 0 0 1"};

MapFixtureConfig makeIdTech4FixtureConfig()
{
  auto config = MapFixtureConfig{};
  config.gameInfo.gameConfig.entityConfig.rotationPolicy = EntityRotationPolicy::IdTech4;
  return config;
}

std::vector<std::string> propertyKeys(const std::vector<const Issue*>& issues)
{
  auto result = std::vector<std::string>{};
  for (const auto* issue : issues)
  {
    result.push_back(static_cast<const EntityPropertyIssue*>(issue)->propertyKey());
  }
  std::ranges::sort(result);
  return result;
}

void applyQuickFix(
  Map& map,
  const IssueType type,
  const std::vector<const Issue*>& issues,
  const std::string& description)
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

TEST_CASE("EntityRotationKeyValidator")
{
  auto fixture = MapFixture{};

  SECTION("The validator is registered only under the id Tech 4 rotation policy")
  {
    CHECK(findValidator(fixture.create().worldNode(), Description) == nullptr);
    CHECK(
      findValidator(fixture.create(makeIdTech4FixtureConfig()).worldNode(), Description)
      != nullptr);
  }

  auto& map = fixture.create(makeIdTech4FixtureConfig());

  const auto* validator = findValidator(map.worldNode(), Description);
  REQUIRE(validator != nullptr);
  const auto type = validator->type();

  SECTION("An entity with brushes and angles or rotation")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "func_static"},
      {"rotation", Yaw90},
      {"angles", "0 90 0"},
      {"angle", "90"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});
    addNodes(map, {{entityNode, {createBrushNode(map)}}});

    // angle is a mover's direction, which TrenchBroom writes itself
    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(propertyKeys(issues) == std::vector<std::string>{"angles", "rotation"});
    CHECK(
      std::ranges::count_if(
        issues,
        [](const auto* issue) {
          return issue->description()
                 == "func_static has brushes or patches, which the game rotates again "
                    "by its 'angles' property";
        })
      == 1);

    SECTION("Delete Property deletes both")
    {
      applyQuickFix(map, type, issues, "Delete Property");
      CHECK(!entityNode->entity().hasProperty("rotation"));
      CHECK(!entityNode->entity().hasProperty("angles"));
      CHECK(*entityNode->entity().property("angle") == "90");
      CHECK(collectIssues(map.worldNode(), type).empty());

      map.undoCommand();
      CHECK(*entityNode->entity().property("rotation") == Yaw90);
      CHECK(*entityNode->entity().property("angles") == "0 90 0");
    }

    SECTION("Convert to Angles leaves the entity alone")
    {
      applyQuickFix(map, type, issues, "Convert to Angles");
      CHECK(*entityNode->entity().property("rotation") == Yaw90);
      CHECK(*entityNode->entity().property("angles") == "0 90 0");
      CHECK(collectIssues(map.worldNode(), type).size() == 2u);
    }
  }

  SECTION("A point entity with rotation")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "some_class"},
      {"rotation", Yaw90},
      {"angle", "45"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 1u);
    CHECK(
      issues.front()->description()
      == "some_class has its rotation in 'rotation', which TrenchBroom replaces by "
         "'angles' when the entity is rotated");
    CHECK(propertyKeys(issues) == std::vector<std::string>{"rotation"});

    SECTION("Convert to Angles keeps the rotation and removes rotation and angle")
    {
      applyQuickFix(map, type, issues, "Convert to Angles");
      CHECK(
        entityNode->entity().properties()
        == std::vector<EntityProperty>{
          {"classname", "some_class"},
          {"angles", "0 90 0"},
        });
      CHECK(collectIssues(map.worldNode(), type).empty());

      map.undoCommand();
      CHECK(*entityNode->entity().property("rotation") == Yaw90);
      CHECK(*entityNode->entity().property("angle") == "45");
      CHECK(!entityNode->entity().hasProperty("angles"));
    }

    SECTION("Delete Property deletes only the reported property")
    {
      applyQuickFix(map, type, issues, "Delete Property");
      CHECK(!entityNode->entity().hasProperty("rotation"));
      CHECK(*entityNode->entity().property("angle") == "45");
      CHECK(collectIssues(map.worldNode(), type).empty());
    }
  }

  SECTION("A point entity with a degenerate rotation")
  {
    // forward and left are both +Y, so the rotation has no angles
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"rotation", "0 1 0"},
      {"light_angles", "0 90 0"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 2u);
    const auto descriptions = [&]() {
      auto result = std::vector<std::string>{};
      for (const auto* issue : issues)
      {
        result.push_back(issue->description());
      }
      std::ranges::sort(result);
      return result;
    }();
    CHECK(
      descriptions
      == std::vector<std::string>{
        "light has its rotation in 'light_angles', which cannot be converted to 'angles' "
        "because the rotation in 'rotation' is degenerate; edit it by hand",
        "light has its rotation in 'rotation', which is degenerate and cannot be "
        "converted to 'angles'; edit it by hand",
      });

    // Convert to Angles leaves the entity alone
    applyQuickFix(map, type, issues, "Convert to Angles");
    CHECK(*entityNode->entity().property("rotation") == "0 1 0");
    CHECK(*entityNode->entity().property("light_angles") == "0 90 0");
    CHECK(!entityNode->entity().hasProperty("angles"));
    CHECK(collectIssues(map.worldNode(), type).size() == 2u);
  }

  SECTION("A light with light_rotation and light_angles")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"light_rotation", Yaw90},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(
      propertyKeys(issues) == std::vector<std::string>{"light_angles", "light_rotation"});

    // both issues of the light are fixed at once
    applyQuickFix(map, type, issues, "Convert to Angles");
    CHECK(
      entityNode->entity().properties()
      == std::vector<EntityProperty>{
        {"classname", "light"},
        {"angles", "0 90 0"},
      });
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Properties that TrenchBroom writes or the engine ignores are no issue")
  {
    auto* pointEntityNode = new EntityNode{Entity{{
      {"classname", "some_class"},
      {"angles", "0 90 0"},
      {"angle", "90"},
      // only lights read these
      {"light_rotation", Yaw90},
      {"light_angles", "0 90 0"},
    }}};
    auto* lightNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"angles", "0 90 0"},
      {"angle", "90"},
    }}};
    auto* brushEntityNode = new EntityNode{Entity{{
      {"classname", "func_door"},
      {"angle", "90"},
    }}};
    addNodes(
      map, {{&parentForNodes(map), {pointEntityNode, lightNode, brushEntityNode}}});
    addNodes(map, {{brushEntityNode, {createBrushNode(map)}}});

    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("An entity that gains brushes is reported")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "func_static"},
      {"angles", "0 90 0"},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});
    REQUIRE(collectIssues(map.worldNode(), type).empty());

    auto* brushNode = createBrushNode(map);
    addNodes(map, {{entityNode, {brushNode}}});
    CHECK(collectIssues(map.worldNode(), type).size() == 1u);
  }

  SECTION("Converting every group of a link set")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "some_class"},
      {"rotation", Yaw90},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

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

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 3u);

    // changing two groups of one link set at once would fail the transaction
    applyQuickFix(map, type, issues, "Convert to Angles");

    for (const auto* linkedGroupNode : linkedGroupNodes)
    {
      const auto* linkedEntityNode =
        static_cast<const EntityNode*>(linkedGroupNode->children().front());
      CHECK(*linkedEntityNode->entity().property("angles") == "0 90 0");
      CHECK(!linkedEntityNode->entity().hasProperty("rotation"));
    }
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("Converting an entity in a group nested in linked groups")
  {
    auto* entityNode = new EntityNode{Entity{{
      {"classname", "some_class"},
      {"rotation", Yaw90},
    }}};
    addNodes(map, {{&parentForNodes(map), {entityNode}}});

    selectNodes(map, {entityNode});
    auto* innerGroupNode = groupSelectedNodes(map, "inner");
    REQUIRE(innerGroupNode != nullptr);
    deselectAll(map);
    selectNodes(map, {innerGroupNode});
    auto* outerGroupNode = groupSelectedNodes(map, "outer");
    REQUIRE(outerGroupNode != nullptr);

    deselectAll(map);
    selectNodes(map, {outerGroupNode});
    auto* linkedOuterGroupNode = createLinkedDuplicate(map);
    REQUIRE(linkedOuterGroupNode != nullptr);
    deselectAll(map);

    // the fix keeps one entity per link set of the entity's own group, which is the inner
    // group, and the linked group update copies the change into the other inner group
    const auto linkedInnerGroupNode = [&]() {
      return static_cast<const GroupNode*>(linkedOuterGroupNode->children().front());
    };
    REQUIRE(linkedInnerGroupNode()->linkId() == innerGroupNode->linkId());

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(issues.size() == 2u);

    applyQuickFix(map, type, issues, "Convert to Angles");

    // the update replaced the children of the other inner group
    const auto* linkedEntityNode =
      static_cast<const EntityNode*>(linkedInnerGroupNode()->children().front());
    CHECK(*entityNode->entity().property("angles") == "0 90 0");
    CHECK(*linkedEntityNode->entity().property("angles") == "0 90 0");
    CHECK(!linkedEntityNode->entity().hasProperty("rotation"));
    CHECK(collectIssues(map.worldNode(), type).empty());
  }

  SECTION("A light with only rotation or only light_angles")
  {
    auto* rotationLightNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"rotation", Yaw90},
    }}};
    auto* lightAnglesLightNode = new EntityNode{Entity{{
      {"classname", "light"},
      {"light_angles", "0 90 0"},
    }}};
    addNodes(map, {{&parentForNodes(map), {rotationLightNode, lightAnglesLightNode}}});

    const auto issues = collectIssues(map.worldNode(), type);
    REQUIRE(propertyKeys(issues) == std::vector<std::string>{"light_angles", "rotation"});

    applyQuickFix(map, type, issues, "Convert to Angles");
    CHECK(
      rotationLightNode->entity().properties()
      == std::vector<EntityProperty>{{"classname", "light"}, {"angles", "0 90 0"}});
    CHECK(
      lightAnglesLightNode->entity().properties()
      == std::vector<EntityProperty>{{"classname", "light"}, {"angles", "0 90 0"}});
    CHECK(collectIssues(map.worldNode(), type).empty());
  }
}

} // namespace tb::mdl
