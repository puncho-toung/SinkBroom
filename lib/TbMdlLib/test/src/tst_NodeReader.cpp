/*
 Copyright (C) 2021 Kristian Duske

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

#include "TestParserStatus.h"
#include "mdl/BezierPatch.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/GroupNode.h"
#include "mdl/NodeReader.h"
#include "mdl/ParallelUvCoordSystem.h"
#include "mdl/ParaxialUvCoordSystem.h"
#include "mdl/PatchNode.h"
#include "mdl/SurfaceAttributes.h"

#include "kd/task_manager.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{

TEST_CASE("NodeReader")
{
  auto taskManager = kdl::task_manager{};
  const auto worldBounds = vm::bbox3d{4096.0};
  auto status = TestParserStatus{};

  SECTION("parseFaceAsNode")
  {
    const auto data = R"(
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
)";

    CHECK(NodeReader::read(data, MapFormat::Valve, worldBounds, {}, status, taskManager)
            .is_error());
  }

  SECTION("convertValveToStandardMapFormat")
  {
    const auto data = R"(
// entity 0
{
"classname" "worldspawn"
"mapversion" "220"
// brush 0
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) __TB_empty [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) __TB_empty [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) __TB_empty [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) __TB_empty [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) __TB_empty [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Standard, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);

    auto* brushNode = dynamic_cast<BrushNode*>(nodes.value().at(0)->children().at(0));
    REQUIRE(brushNode != nullptr);

    auto brush = brushNode->brush();
    CHECK(brush.face(0).uvCoordSystem().is<ParaxialUvCoordSystem>());
  }

  SECTION("convertValveToStandardMapFormatInGroups")
  {
    // Data comes from copying a Group in 2020.2
    const auto data = R"(// entity 0
{
"classname" "func_group"
"_tb_type" "_tb_group"
"_tb_name" "Unnamed"
"_tb_id" "3"
// brush 0
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) __TB_empty [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) __TB_empty [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) __TB_empty [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) __TB_empty [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) __TB_empty [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Standard, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);

    auto* groupNode = dynamic_cast<GroupNode*>(nodes.value().at(0));
    REQUIRE(groupNode != nullptr);

    auto* brushNode = dynamic_cast<BrushNode*>(groupNode->children().at(0));
    REQUIRE(brushNode != nullptr);

    const auto brush = brushNode->brush();
    CHECK(brush.face(0).uvCoordSystem().is<ParaxialUvCoordSystem>());
  }

  SECTION("reportsBrushErrorsWithLocation")
  {
    // the brush is missing its top face, so it isn't closed
    const auto data = R"(
{
"classname" "worldspawn"
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) __TB_empty 0 0 0 1 1
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Standard, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);

    CHECK(
      status.messages(LogLevel::Error)
      == std::vector<std::string>{"At line 4, column 1: Brush is incomplete"});
  }

  SECTION("skipsAndLogsFaceWithInvalidUvAttributes")
  {
    // brush 0's last face has a scale so large that its UV coordinate system matrix
    // isn't invertible; the face gets skipped, which leaves the brush incomplete (this
    // is otherwise the same brush geometry as reportsBrushErrorsWithLocation). brush 1
    // is unaffected and should still load.
    const auto data = R"(
{
"classname" "worldspawn"
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) __TB_empty 0 0 0 1e30 1e30
}
{
( -256 -256 -256 ) ( -256 -255 -256 ) ( -256 -256 -255 ) __TB_empty 0 0 0 1 1
( -256 -256 -256 ) ( -256 -256 -255 ) ( -255 -256 -256 ) __TB_empty 0 0 0 1 1
( -256 -256 -256 ) ( -255 -256 -256 ) ( -256 -255 -256 ) __TB_empty 0 0 0 1 1
( 256 256 256 ) ( 256 257 256 ) ( 257 256 256 ) __TB_empty 0 0 0 1 1
( 256 256 256 ) ( 257 256 256 ) ( 256 256 257 ) __TB_empty 0 0 0 1 1
( 256 256 256 ) ( 256 256 257 ) ( 256 257 256 ) __TB_empty 0 0 0 1 1
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Standard, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);

    CHECK(
      status.messages(LogLevel::Error)
      == std::vector<std::string>{
        "At line 9, column 60: Skipping face: UV coordinate system is not invertible",
        "At line 4, column 1: Brush is incomplete",
      });

    const auto& children = nodes.value().at(0)->children();
    REQUIRE(children.size() == 1u);

    auto* brushNode = dynamic_cast<BrushNode*>(children.at(0));
    REQUIRE(brushNode != nullptr);
    CHECK(brushNode->brush().bounds() == vm::bbox3d{{-256, -256, -256}, {256, 256, 256}});
  }

  SECTION("loadsFaceWithZeroUvScale")
  {
    // a scale of 0 is invalid for editing (InvalidUvScaleValidator flags it and offers a
    // quick fix), but it must still load rather than being rejected outright
    const auto data = R"(
{
"classname" "worldspawn"
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) __TB_empty 0 0 0 1 1
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) __TB_empty 0 0 0 1 1
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) __TB_empty 0 0 0 0 0
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Standard, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);
    CHECK(status.messages(LogLevel::Error).empty());

    auto* brushNode = dynamic_cast<BrushNode*>(nodes.value().at(0)->children().at(0));
    REQUIRE(brushNode != nullptr);
  }

  SECTION("readScientificNotation")
  {
    // https://github.com/TrenchBroom/TrenchBroom/issues/4270

    const auto data = R"(
{
"classname" "worldspawn"
"sounds" "1"
"MaxRange" "4096"
"mapversion" "220"
{
( 112 16 16 ) ( 112 16 17 ) ( 112 15 16 ) __TB_empty [ -1.8369701E-16 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1 
( 128 0 32 ) ( 128 0 33 ) ( 129 0 32 ) __TB_empty [ 1 -1.8369701e-16 0 0 ] [ 0 0 -1 0 ] 0 1 1 
( 112 16 16 ) ( 112 15 16 ) ( 113 16 16 ) __TB_empty [ 1.8369701e-16 1 0 0 ] [ -1 1.8369701E-16 0 0 ] 270 1 1 
( 128 0 80 ) ( 129 0 80 ) ( 128 -1 80 ) __TB_empty [ -1.8369701e-16 -1 0 0 ] [ -1 1.8369701E-16 0 0 ] 90 1 1 
( 112 16 16 ) ( 113 16 16 ) ( 112 16 17 ) __TB_empty [ -1 1.8369701E-16 0 0 ] [ 0 0 -1 0 ] 0 1 1 
( 128 0 32 ) ( 128 -1 32 ) ( 128 0 33 ) __TB_empty [ 1.8369701e-16 1 0 0 ] [ 0 0 -1 0 ] 0 1 1 
}
}
)";

    auto nodes =
      NodeReader::read(data, MapFormat::Valve, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);
    CHECK(nodes.value().size() == 1);
  }

  SECTION("pasteDoom3ValveBrushAndPatch")
  {
    // a brush and an explicitly subdivided patch as RBDOOM-3-BFG writes them
    const auto data = R"(
// brush 0
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) tb_test/wall [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) tb_test/wall [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) tb_test/wall [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) tb_test/wall [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) tb_test/wall [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) tb_test/wall [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
}
// primitive 1
{
 patchDef3
 {
  "textures/tb_test/wall"
  ( 3 3 4 2 0 0 0 )
  (
   (  ( 0.000000 0.000000 0.000000 0.000000 0.000000 ) ( 0.000000 64.000000 0.000000 0.000000 0.500000 ) ( 0.000000 128.000000 0.000000 0.000000 1.000000 ) )
   (  ( 64.000000 0.000000 32.000000 0.500000 0.000000 ) ( 64.000000 64.000000 32.000000 0.500000 0.500000 ) ( 64.000000 128.000000 32.000000 0.500000 1.000000 ) )
   (  ( 128.000000 0.000000 0.000000 1.000000 0.000000 ) ( 128.000000 64.000000 0.000000 1.000000 0.500000 ) ( 128.000000 128.000000 0.000000 1.000000 1.000000 ) )
  )
 }
}
)";

    auto nodes = NodeReader::read(
      data, MapFormat::Doom3_Valve, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);
    REQUIRE(nodes.value().size() == 2u);
    CHECK(status.countStatus(LogLevel::Warn) == 0u);
    CHECK(status.countStatus(LogLevel::Error) == 0u);

    const auto* brushNode = dynamic_cast<BrushNode*>(nodes.value().at(0));
    REQUIRE(brushNode != nullptr);
    CHECK(brushNode->brush().face(0).uvCoordSystem().is<ParallelUvCoordSystem>());
    CHECK(brushNode->brush().face(0).surfaceAttributes().empty());

    const auto* patchNode = dynamic_cast<PatchNode*>(nodes.value().at(1));
    REQUIRE(patchNode != nullptr);
    CHECK(patchNode->patch().materialName() == "tb_test/wall");
    CHECK(patchNode->patch().explicitSubdivisions() == PatchSubdivisions{4, 2});
    CHECK(
      patchNode->patch().controlPoint(1, 2) == BezierPatch::Point{64, 128, 32, 0.5, 1});
  }

  SECTION("pasteDoom3ValveBrushIntoOtherFormats")
  {
    // compatibleFormats governs which source formats a paste tries, and Doom3 (Valve)
    // shares no entry with any other format; this pins what happens to a brush copied
    // from a map RBDOOM-3-BFG wrote when it is pasted into a map of another format
    const auto data = R"(
// brush 0
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) tb_test/wall [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) tb_test/wall [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) tb_test/wall [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) tb_test/wall [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) tb_test/wall [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) tb_test/wall [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
}
)";

    SECTION("Standard and Valve maps refuse it")
    {
      // neither the Valve nor the Standard face syntax allows the trailing numbers
      const auto format = GENERATE(MapFormat::Standard, MapFormat::Valve);
      CAPTURE(format);

      CHECK(
        NodeReader::read(data, format, worldBounds, {}, status, taskManager).is_error());
    }

    SECTION("Quake 2 and Quake 3 Valve maps read the numbers as surface attributes")
    {
      const auto format = GENERATE(MapFormat::Quake2_Valve, MapFormat::Quake3_Valve);
      CAPTURE(format);

      auto nodes = NodeReader::read(data, format, worldBounds, {}, status, taskManager);
      REQUIRE(nodes);
      REQUIRE(nodes.value().size() == 1u);

      const auto* brushNode = dynamic_cast<BrushNode*>(nodes.value().at(0));
      REQUIRE(brushNode != nullptr);
      for (const auto& face : brushNode->brush().faces())
      {
        CHECK(face.uvCoordSystem().is<ParallelUvCoordSystem>());
        CHECK(
          face.surfaceAttributes()
          == SurfaceAttributes{.contents = 0, .flags = 0, .value = 0.0f});
      }
    }
  }

  SECTION("pasteDoom3ValvePatchSmallerThan3x3")
  {
    const auto data = R"(
{
 patchDef2
 {
  "textures/tb_test/wall"
  ( 2 3 0 0 0 )
  (
   (  ( 0 0 0 0 0 ) ( 0 64 0 0 0.5 ) ( 0 128 0 0 1 ) )
   (  ( 64 0 0 1 0 ) ( 64 64 0 1 0.5 ) ( 64 128 0 1 1 ) )
  )
 }
}
{
( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 ) tb_test/wall [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -64 -64 -15 ) ( -63 -64 -16 ) tb_test/wall [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( -64 -64 -16 ) ( -63 -64 -16 ) ( -64 -63 -16 ) tb_test/wall [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 65 16 ) ( 65 64 16 ) tb_test/wall [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 65 64 16 ) ( 64 64 17 ) tb_test/wall [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
( 64 64 16 ) ( 64 64 17 ) ( 64 65 16 ) tb_test/wall [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0
}
)";

    auto nodes = NodeReader::read(
      data, MapFormat::Doom3_Valve, worldBounds, {}, status, taskManager);
    REQUIRE(nodes);
    REQUIRE(nodes.value().size() == 1u);
    CHECK(dynamic_cast<BrushNode*>(nodes.value().at(0)) != nullptr);

    CHECK(
      status.messages(LogLevel::Error)
      == std::vector<std::string>{
        "At line 2, column 1: Skipping patch: patch must have at least 3x3 control "
        "points, but has 2x3"});
  }
}

} // namespace tb::mdl
