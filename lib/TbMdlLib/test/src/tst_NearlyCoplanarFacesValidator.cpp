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
#include "mdl/CircleShape.h"
#include "mdl/EntityProperties.h"
#include "mdl/Issue.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestUtils.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/util.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

const auto Description = std::string{"Nearly coplanar faces"};
const auto IssueDescription = std::string{
  "Brush has adjacent faces within 6 degrees of each other; id Tech 4's dmap moves the "
  "vertices on such a crease and leaves cracks, so rebuild it with planar sides or split "
  "it"};

MapFixtureConfig makeIdTech4FixtureConfig()
{
  auto config = MapFixtureConfig{};
  config.gameInfo.gameConfig.entityConfig.rotationPolicy = EntityRotationPolicy::IdTech4;
  return config;
}

BrushBuilder makeBrushBuilder(const Map& map)
{
  return BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
}

BrushNode* createBrushNodeFromPoints(const Map& map, const std::vector<vm::vec3d>& points)
{
  return new BrushNode{
    makeBrushBuilder(map).createBrush(points, "material") | kdl::value()};
}

// one top corner of a 64-unit cube lifted by half a unit, as rounding a sweep station can
// do; the hull splits the top into two faces less than a degree apart
BrushNode* createNonPlanarPrismBrushNode(const Map& map)
{
  return createBrushNodeFromPoints(
    map,
    {
      {0, 0, 0},
      {64, 0, 0},
      {64, 64, 0},
      {0, 64, 0},
      {0, 0, 64},
      {64, 0, 64},
      {64, 64, 64.5},
      {0, 64, 64},
    });
}

// the bottom of a 64-unit cube with its top split into two faces along x = 32 that meet
// at a ridge, each tilted by half the given angle
BrushNode* createRidgeBrushNode(const Map& map, const double angle)
{
  const auto h0 = 64.0;
  const auto h1 = h0 + 32.0 * std::tan(vm::to_radians(angle) / 2.0);
  return createBrushNodeFromPoints(
    map,
    {
      {0, 0, 0},
      {64, 0, 0},
      {64, 64, 0},
      {0, 64, 0},
      {0, 0, h0},
      {0, 64, h0},
      {32, 0, h1},
      {32, 64, h1},
      {64, 0, h0},
      {64, 64, h0},
    });
}

} // namespace

TEST_CASE("NearlyCoplanarFacesValidator")
{
  auto fixture = MapFixture{};

  SECTION("The validator is registered only under the id Tech 4 rotation policy")
  {
    auto& map = fixture.create();
    CHECK(findValidator(map.worldNode(), Description) == nullptr);

    // a scan with every registered validator reports nothing about the crease
    addNodes(map, {{&parentForNodes(map), {createNonPlanarPrismBrushNode(map)}}});

    const auto issues =
      collectIssues(map.worldNode(), map.worldNode().registeredValidators());
    CHECK(std::ranges::none_of(issues, [](const auto* issue) {
      return issue->description() == IssueDescription;
    }));
  }

  SECTION("Under the id Tech 4 rotation policy")
  {
    auto& map = fixture.create(makeIdTech4FixtureConfig());

    const auto* validator = findValidator(map.worldNode(), Description);
    REQUIRE(validator != nullptr);
    CHECK(validator->quickFixes().empty());

    const auto type = validator->type();

    const auto addCylinder = [&](const size_t numSides) {
      addNodes(
        map,
        {{&parentForNodes(map),
          {new BrushNode{
            makeBrushBuilder(map).createCylinder(
              vm::bbox3d{{-64, -64, 0}, {64, 64, 64}},
              EdgeAlignedCircle{numSides},
              vm::axis::z,
              "material")
            | kdl::value()}}}});
    };

    SECTION("A cube is no issue")
    {
      addNodes(
        map,
        {{&parentForNodes(map),
          {new BrushNode{
            makeBrushBuilder(map).createCube(64.0, "material") | kdl::value()}}}});
      CHECK(collectIssues(map.worldNode(), type).empty());
    }

    SECTION("A cylinder with 16 sides is no issue")
    {
      // its sides are 22.5 degrees apart
      addCylinder(16);
      CHECK(collectIssues(map.worldNode(), type).empty());
    }

    SECTION("A cylinder with 64 sides is reported")
    {
      // its sides are 5.625 degrees apart
      addCylinder(64);
      CHECK(collectIssues(map.worldNode(), type).size() == 1u);
    }

    SECTION("A prism with a non-planar top is reported")
    {
      auto* brushNode = createNonPlanarPrismBrushNode(map);
      addNodes(map, {{&parentForNodes(map), {brushNode}}});

      const auto issues = collectIssues(map.worldNode(), type);
      REQUIRE(issues.size() == 1u);
      CHECK(&issues.front()->node() == brushNode);
      CHECK(issues.front()->description() == IssueDescription);
    }

    SECTION("A ridge of 7.5 degrees is no issue")
    {
      addNodes(map, {{&parentForNodes(map), {createRidgeBrushNode(map, 7.5)}}});
      CHECK(collectIssues(map.worldNode(), type).empty());
    }

    SECTION("A ridge of 6.5 degrees is no issue")
    {
      addNodes(map, {{&parentForNodes(map), {createRidgeBrushNode(map, 6.5)}}});
      CHECK(collectIssues(map.worldNode(), type).empty());
    }

    SECTION("A ridge of 5.5 degrees is reported")
    {
      addNodes(map, {{&parentForNodes(map), {createRidgeBrushNode(map, 5.5)}}});
      CHECK(collectIssues(map.worldNode(), type).size() == 1u);
    }

    SECTION("A ridge of 4.5 degrees is reported")
    {
      addNodes(map, {{&parentForNodes(map), {createRidgeBrushNode(map, 4.5)}}});
      CHECK(collectIssues(map.worldNode(), type).size() == 1u);
    }
  }
}

} // namespace tb::mdl
