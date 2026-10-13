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
#include "mdl/EntityProperties.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include "vm/bbox.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto Description = std::string{"Non-integer vertices"};

MapFixtureConfig makeIdTech4FixtureConfig()
{
  auto config = MapFixtureConfig{};
  config.gameInfo.gameConfig.entityConfig.rotationPolicy = EntityRotationPolicy::IdTech4;
  return config;
}

BrushNode* createNonIntegerBrushNode(const Map& map)
{
  const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  return new BrushNode{
    builder.createCuboid(vm::bbox3d{{0, 0, 0}, {16.5, 16, 16}}, "material")
    | kdl::value()};
}

std::vector<std::string> quickFixDescriptions(const Validator& validator)
{
  auto result = std::vector<std::string>{};
  for (const auto* quickFix : validator.quickFixes())
  {
    result.push_back(quickFix->description());
  }
  return result;
}

} // namespace

TEST_CASE("NonIntegerVerticesValidator")
{
  auto fixture = MapFixture{};

  SECTION("Other games keep the upstream description")
  {
    auto& map = fixture.create();

    const auto* validator = findValidator(map.worldNode(), Description);
    REQUIRE(validator != nullptr);
    CHECK(quickFixDescriptions(*validator) == std::vector<std::string>{"Snap Vertices"});

    addNodes(map, {{&parentForNodes(map), {createNonIntegerBrushNode(map)}}});

    const auto issues = collectIssues(map.worldNode(), validator->type());
    REQUIRE(issues.size() == 1u);
    CHECK(issues.front()->description() == "Brush has non-integer vertices");
  }

  SECTION("The id Tech 4 rotation policy appends the engine note")
  {
    auto& map = fixture.create(makeIdTech4FixtureConfig());

    const auto* validator = findValidator(map.worldNode(), Description);
    REQUIRE(validator != nullptr);
    CHECK(quickFixDescriptions(*validator) == std::vector<std::string>{"Snap Vertices"});

    auto* brushNode = createNonIntegerBrushNode(map);
    addNodes(map, {{&parentForNodes(map), {brushNode}}});

    const auto issues = collectIssues(map.worldNode(), validator->type());
    REQUIRE(issues.size() == 1u);
    CHECK(
      issues.front()->description()
      == "Brush has non-integer vertices; id Tech 4's dmap welds vertices on a 1/32 grid "
         "and leaves cracks where many faces meet at an off-grid vertex");

    SECTION("Snap Vertices fixes the brush")
    {
      // like the issue browser, which selects the issues' nodes before applying a fix
      selectNodes(map, {brushNode});

      auto transaction = Transaction{map, "Apply Quick Fix"};
      validator->quickFixes().front()->apply(map, issues);
      CHECK(transaction.commit());

      CHECK(collectIssues(map.worldNode(), validator->type()).empty());
    }
  }

  SECTION("Brushes with integer vertices are no issue")
  {
    auto& map = fixture.create(makeIdTech4FixtureConfig());

    const auto* validator = findValidator(map.worldNode(), Description);
    REQUIRE(validator != nullptr);

    addNodes(map, {{&parentForNodes(map), {createBrushNode(map)}}});
    CHECK(collectIssues(map.worldNode(), validator->type()).empty());
  }
}

} // namespace tb::mdl
