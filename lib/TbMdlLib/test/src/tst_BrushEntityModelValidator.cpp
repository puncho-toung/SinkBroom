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
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

MapFixtureConfig makeNamingFixtureConfig(std::optional<std::string> brushEntityModelKey)
{
  auto fixtureConfig = MapFixtureConfig{};
  fixtureConfig.gameInfo.gameConfig.entityConfig.naming = EntityNamingConfig{
    .nameKey = "name",
    .brushEntityModelKey = std::move(brushEntityModelKey),
  };
  return fixtureConfig;
}

const Validator* findValidator(const WorldNode& worldNode, const std::string& description)
{
  for (const auto* validator : worldNode.registeredValidators())
  {
    if (validator->description() == description)
    {
      return validator;
    }
  }
  return nullptr;
}

std::vector<const Issue*> collectIssues(WorldNode& worldNode, const IssueType type)
{
  const auto validators = worldNode.registeredValidators();

  auto issues = std::vector<const Issue*>{};
  const auto collect = [&](Node& node) {
    for (const auto* issue : node.issues(validators))
    {
      if (issue->type() == type)
      {
        issues.push_back(issue);
      }
    }
  };

  worldNode.accept(kdl::overload(
    [&](auto&& thisLambda, WorldNode& w) {
      collect(w);
      w.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, LayerNode& l) { l.visitChildren(thisLambda); },
    [&](auto&& thisLambda, GroupNode& g) { g.visitChildren(thisLambda); },
    [&](EntityNode& e) { collect(e); },
    [](BrushNode&) {},
    [](PatchNode&) {}));
  return issues;
}

std::string propertyOf(const EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  return value ? *value : std::string{};
}

} // namespace

TEST_CASE("BrushEntityModelValidator")
{
  auto fixture = MapFixture{};
  auto& map = fixture.create(makeNamingFixtureConfig("model"));

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
