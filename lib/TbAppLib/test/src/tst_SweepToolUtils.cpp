/*
 Copyright (C) 2026 Jackson Palmer

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
#include "mdl/Map.h" // IWYU pragma: keep
#include "mdl/Map_Nodes.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/SweepToolUtils.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/constants.h"
#include "vm/mat.h"
#include "vm/polygon.h"
#include "vm/quat.h"
#include "vm/vec.h"

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("SweepTransform")
{
  SECTION("destinationCenter")
  {
    const auto source = SweepSource{{}, vm::vec3d{8, 8, 8}, {}, {}};
    const auto transform = SweepTransform{vm::vec3d{16, 0, -8}};
    CHECK(transform.destinationCenter(source) == vm::vec3d{24, 8, 0});
  }

  SECTION("effectiveRotation")
  {
    auto transform = SweepTransform{};

    SECTION("returns rotations up to a half turn unchanged")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};

      const auto rotation = transform.effectiveRotation();
      CHECK(rotation.angle() == vm::approx{vm::Cd::half_pi()});
      CHECK(rotation.axis() == vm::approx{vm::vec3d{0, 0, 1}});
    }

    SECTION("replaces rotations beyond a half turn with the shorter turn")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, 3.0 * vm::Cd::half_pi()};

      const auto rotation = transform.effectiveRotation();
      CHECK(rotation.angle() == vm::approx{vm::Cd::half_pi()});
      CHECK(rotation.axis() == vm::approx{vm::vec3d{0, 0, -1}});
    }

    SECTION("treats a full turn as no rotation")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::two_pi()};

      CHECK(transform.effectiveRotation().angle() == vm::approx{0.0});
    }
  }

  SECTION("isNoOp")
  {
    auto transform = SweepTransform{};
    CHECK(transform.isNoOp());

    SECTION("translation")
    {
      transform.translation = vm::vec3d{1, 0, 0};
      CHECK(!transform.isNoOp());
    }

    SECTION("rotation")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};
      CHECK(!transform.isNoOp());
    }

    SECTION("scale")
    {
      transform.scale = vm::vec3d{2, 2, 2};
      CHECK(!transform.isNoOp());
    }

    SECTION("a full turn")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::two_pi()};
      CHECK(transform.isNoOp());
    }
  }
}

TEST_CASE("SweepToolUtils functions")
{
  SECTION("stationTransform")
  {
    auto source = SweepSource{{}, vm::vec3d{0, 0, 0}, vm::vec3d{1, 0, 0}, {}};
    auto transform = SweepTransform{};
    auto parameters = SweepParameters{};

    const auto station = [&](const double t, const vm::vec3d& point) {
      return stationTransform(
               source, transform, parameters, t, transform.effectiveRotation())
             * point;
    };

    SECTION("straight path interpolates the translation")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      parameters.pathMode = SweepPathMode::Straight;

      CHECK(station(0.0, vm::vec3d{0, 16, 0}) == vm::approx{vm::vec3d{0, 16, 0}});
      CHECK(station(0.5, vm::vec3d{0, 16, 0}) == vm::approx{vm::vec3d{32, 16, 0}});
      CHECK(station(1.0, vm::vec3d{0, 16, 0}) == vm::approx{vm::vec3d{64, 16, 0}});
    }

    SECTION("straight path scales the rotation angle with t")
    {
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};
      parameters.pathMode = SweepPathMode::Straight;

      const auto d = 16.0 / std::sqrt(2.0);
      CHECK(station(0.5, vm::vec3d{16, 0, 0}) == vm::approx{vm::vec3d{d, d, 0}});
      CHECK(station(1.0, vm::vec3d{16, 0, 0}) == vm::approx{vm::vec3d{0, 16, 0}});
    }

    SECTION("straight path interpolates the scale about the source center")
    {
      transform.scale = vm::vec3d{2, 2, 2};
      parameters.pathMode = SweepPathMode::Straight;

      CHECK(station(0.5, vm::vec3d{16, 0, 0}) == vm::approx{vm::vec3d{24, 0, 0}});
      CHECK(station(1.0, vm::vec3d{16, 0, 0}) == vm::approx{vm::vec3d{32, 0, 0}});
    }

    SECTION("arc path revolves the source about the derived pivot")
    {
      // a quarter turn from {0,0,0} to {64,64,0} fits a circle about {0,64,0}
      transform.translation = vm::vec3d{64, 64, 0};
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};
      parameters.pathMode = SweepPathMode::Arc;

      const auto d = 64.0 / std::sqrt(2.0);
      CHECK(station(0.0, source.center) == vm::approx{source.center});
      CHECK(station(0.5, source.center) == vm::approx{vm::vec3d{d, 64.0 - d, 0}});
      CHECK(station(1.0, source.center) == vm::approx{vm::vec3d{64, 64, 0}});
    }

    SECTION("arc path applies translation along the axis as a lift")
    {
      transform.translation = vm::vec3d{64, 64, 32};
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};
      parameters.pathMode = SweepPathMode::Arc;

      const auto d = 64.0 / std::sqrt(2.0);
      CHECK(station(0.5, source.center) == vm::approx{vm::vec3d{d, 64.0 - d, 16}});
      CHECK(station(1.0, source.center) == vm::approx{vm::vec3d{64, 64, 32}});
    }

    SECTION("arc path without a usable pivot behaves like the straight path")
    {
      // no travel perpendicular to the rotation axis, so no circle fits
      transform.translation = vm::vec3d{0, 0, 64};
      transform.rotation = vm::quatd{vm::vec3d{0, 0, 1}, vm::Cd::half_pi()};
      parameters.pathMode = SweepPathMode::Arc;

      CHECK(station(1.0, vm::vec3d{16, 0, 0}) == vm::approx{vm::vec3d{0, 16, 64}});
    }

    SECTION("arc path without a rotation behaves like the straight path")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      parameters.pathMode = SweepPathMode::Arc;

      CHECK(station(0.5, vm::vec3d{0, 16, 0}) == vm::approx{vm::vec3d{32, 16, 0}});
    }

    SECTION("s-bend path bulges toward the source normal")
    {
      transform.translation = vm::vec3d{0, 64, 0};
      parameters.pathMode = SweepPathMode::SBend;

      // the Hermite basis at t=1/4 weighs the tangents with 9/64 and -3/64 and the
      // translation with 5/32; both tangents are source.normal * 64
      CHECK(station(0.0, source.center) == vm::approx{source.center});
      CHECK(station(0.25, source.center) == vm::approx{vm::vec3d{6, 10, 0}});
      CHECK(station(1.0, source.center) == vm::approx{vm::vec3d{0, 64, 0}});
    }

    SECTION("s-bend path requires a non-zero source normal")
    {
      source.normal = vm::vec3d{0, 0, 0};
      transform.translation = vm::vec3d{0, 64, 0};
      parameters.pathMode = SweepPathMode::SBend;

      CHECK(station(0.25, source.center) == vm::approx{vm::vec3d{0, 16, 0}});
    }
  }

  SECTION("generateSweepBrushes")
  {
    auto fixture = MapDocumentFixture{};
    auto& document = fixture.create();
    auto& map = document.map();

    auto& defaultParent = parentForNodes(map);

    const auto squareAt = [](const double x) {
      return vm::polygon3d{
        {x, -16, -16},
        {x, -16, 16},
        {x, 16, 16},
        {x, 16, -16},
      };
    };

    // an octagon with integer vertices like the end of a small cylinder; its longest
    // edges are 4 units long
    const auto octagonAt = [](const double x) {
      return vm::polygon3d{
        {x, 4, 2},
        {x, 2, 4},
        {x, -2, 4},
        {x, -4, 2},
        {x, -4, -2},
        {x, -2, -4},
        {x, 2, -4},
        {x, 4, -2},
      };
    };

    auto source = SweepSource{
      {SweepFace{squareAt(0.0), &defaultParent}},
      vm::vec3d{0, 0, 0},
      vm::vec3d{1, 0, 0},
      vm::vec3d{0, 16, 16},
    };
    auto transform = SweepTransform{};
    auto parameters =
      SweepParameters{1, 1, SweepPathMode::Straight, SweepAlignment::Free};

    SECTION("creates one brush per segment")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      parameters.segments = 4;

      const auto result = generateSweepBrushes(map, source, transform, parameters);
      REQUIRE(result.brushes.size() == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 4);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -16, -16}, {16, 16, 16}});
      CHECK(brushes[3]->logicalBounds() == vm::bbox3d{{48, -16, -16}, {64, 16, 16}});
    }

    SECTION("iterations continue from the previous destination cap")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      parameters.segments = 2;
      parameters.iterations = 2;

      const auto result = generateSweepBrushes(map, source, transform, parameters);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 4);
      CHECK(brushes[3]->logicalBounds() == vm::bbox3d{{96, -16, -16}, {128, 16, 16}});
    }

    SECTION("groups the brushes by the parent of their source face")
    {
      auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
      addNodes(map, {{&defaultParent, {entityNode}}});

      source.faces = {
        SweepFace{squareAt(0.0), entityNode},
        SweepFace{squareAt(0.0), nullptr},
      };
      transform.translation = vm::vec3d{64, 0, 0};
      parameters.segments = 2;

      const auto result = generateSweepBrushes(map, source, transform, parameters);
      REQUIRE(result.brushes.size() == 2);
      CHECK(result.brushes.at(entityNode).size() == 2);
      // a face without a parent falls back to the map's default insertion parent
      CHECK(result.brushes.at(&defaultParent).size() == 2);
    }

    SECTION("integer alignment keeps the source station exact and rounds the rest")
    {
      source.faces = {SweepFace{squareAt(0.25), &defaultParent}};
      source.center = vm::vec3d{0.25, 0, 0};
      transform.translation = vm::vec3d{16, 0, 0};
      parameters.segments = 2;
      parameters.alignment = SweepAlignment::Integer;

      // the stations sit at x = 0.25 (exact), round(8.25) = 8 and round(16.25) = 16
      const auto result = generateSweepBrushes(map, source, transform, parameters);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 2);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0.25, -16, -16}, {8, 16, 16}});
      CHECK(brushes[1]->logicalBounds() == vm::bbox3d{{8, -16, -16}, {16, 16, 16}});
    }

    SECTION("integer alignment rounds the cap shared between iterations")
    {
      // the destination cap of iteration r continues as the source station of iteration
      // r+1 (station(r=1,s=0) below); both must round the same way or the mesh gets a
      // seam
      transform.translation = vm::vec3d{15.6, 0, 0};
      parameters.segments = 1;
      parameters.iterations = 2;
      parameters.alignment = SweepAlignment::Integer;

      const auto result = generateSweepBrushes(map, source, transform, parameters);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 2);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -16, -16}, {16, 16, 16}});
      CHECK(brushes[1]->logicalBounds() == vm::bbox3d{{16, -16, -16}, {31, 16, 16}});
    }

    SECTION("skips segments that do not form a valid brush")
    {
      transform.translation = vm::vec3d{1, 0, 0};
      parameters.segments = 2;
      parameters.alignment = SweepAlignment::Integer;

      // rounding collapses the second segment: the stations sit at x = 0, 1 and 1
      const auto result = generateSweepBrushes(map, source, transform, parameters);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 1);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -16, -16}, {1, 16, 16}});
    }

    SECTION("returns nothing if every segment is degenerate")
    {
      // an identity transform collapses every station onto the source face
      parameters.segments = 2;

      CHECK(generateSweepBrushes(map, source, transform, parameters).brushes.empty());
    }

    SECTION("integer alignment stops the run before the cap collapses to a point")
    {
      source.faces = {SweepFace{octagonAt(0.0), &defaultParent}};
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d::fill(0.01);
      parameters.segments = 8;
      parameters.iterations = 2;
      parameters.alignment = SweepAlignment::Integer;

      // station 6 rounds to a square 2 units wide, station 7 to a diamond with edges of
      // 1.41 units and station 8 to a point, where the second iteration would continue
      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 7);
      CHECK(brushes[6]->logicalBounds() == vm::bbox3d{{48, -1, -1}, {56, 1, 1}});
    }

    SECTION("free alignment stops the run before the cap gets narrower than one unit")
    {
      source.faces = {SweepFace{octagonAt(0.0), &defaultParent}};
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d::fill(0.01);
      parameters.segments = 8;
      parameters.iterations = 2;

      // the longest edge shrinks to 1.03 units at station 6 and to 0.535 at station 7;
      // the second iteration would shrink it further
      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 6);
      CHECK(brushes[5]->logicalBounds().max.x() == vm::approx{48.0});
    }

    SECTION("stops a square tapered to a point before the apex")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d::fill(0.01);
      parameters.segments = 2;
      parameters.alignment = SweepAlignment::Integer;

      // station 1 rounds to a square 16 units wide and station 2 to a point, so the
      // pyramid between them is not built
      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 1);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -16, -16}, {32, 16, 16}});
    }

    SECTION("stops the run before a cap flattened to a line")
    {
      source.faces = {SweepFace{octagonAt(0.0), &defaultParent}};
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d{1, 1, 0.01};
      parameters.segments = 2;
      parameters.alignment = SweepAlignment::Integer;

      // station 2 rounds every vertex onto z = 0, leaving four distinct vertices on a
      // line
      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 1);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -4, -4}, {32, 4, 4}});
    }

    SECTION("keeps a cap exactly one unit wide")
    {
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d::fill(1.0 / 32.0);

      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 0);
      CHECK(result.brushes.at(&defaultParent).size() == 1);
    }

    SECTION("a face whose first segment ends in an unusable cap produces nothing")
    {
      const auto bigSquare = vm::polygon3d{
        {0, -128, -128},
        {0, -128, 128},
        {0, 128, 128},
        {0, 128, -128},
      };
      source.faces = {
        SweepFace{bigSquare, &defaultParent},
        SweepFace{squareAt(0.0), &defaultParent},
      };
      transform.translation = vm::vec3d{64, 0, 0};
      transform.scale = vm::vec3d::fill(0.01);
      parameters.alignment = SweepAlignment::Integer;

      // the big square's cap rounds to a square 2 units wide, the small one's to a point
      const auto result = generateSweepBrushes(map, source, transform, parameters);
      CHECK(result.stoppedRuns == 1);

      const auto& brushes = result.brushes.at(&defaultParent);
      REQUIRE(brushes.size() == 1);
      CHECK(brushes[0]->logicalBounds() == vm::bbox3d{{0, -128, -128}, {64, 128, 128}});
    }
  }
}

} // namespace tb::ui
