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

#include "LitSnapshotBuilder.h"
#include "lit/Snapshot.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include "kd/result.h"
#include "kd/task_manager.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

// what the engine reads: idLexer's double, then ParseFloat's float
float engineRead(const std::string& token)
{
  return float(std::strtod(token.c_str(), nullptr));
}

std::vector<std::string> tokens(const std::string& line)
{
  auto result = std::vector<std::string>{};
  auto stream = std::istringstream{line};
  auto token = std::string{};
  while (stream >> token)
  {
    result.push_back(token);
  }
  return result;
}

// the Valve 220 numbers of one face line: [ ux uy uz ou ] [ vx vy vz ov ] rotation su sv
struct FaceLineNumbers
{
  std::vector<float> u;
  std::vector<float> v;
  float scaleU = 0.0f;
  float scaleV = 0.0f;
};

FaceLineNumbers readFaceLine(const std::string& line)
{
  const auto words = tokens(line);
  auto result = FaceLineNumbers{};
  auto bracket = std::size_t{0};
  for (std::size_t i = 0; i < words.size(); ++i)
  {
    if (words[i] == "[")
    {
      auto& axis = bracket++ == 0 ? result.u : result.v;
      for (std::size_t j = 1; j <= 4; ++j)
      {
        axis.push_back(engineRead(words[i + j]));
      }
    }
  }
  REQUIRE(words.size() >= 2u);
  result.scaleU = engineRead(words[words.size() - 2]);
  result.scaleV = engineRead(words[words.size() - 1]);
  return result;
}

const lit::SnapshotNode& onlyNode(const lit::Snapshot& snapshot)
{
  REQUIRE(snapshot.nodes.size() == 1u);
  return snapshot.nodes.front();
}

} // namespace

TEST_CASE("LitSnapshotBuilder")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(mdl::Doom3BFGFixtureConfig);
  auto& map = document.map();

  SECTION("a serialised face reads back to the same floats (C23, P.19)")
  {
    // uneven values, so that the axes are not axial and the text is not short
    auto* brushNode = mdl::createBrushNode(map, "lp1b/uneven", [](mdl::Brush& brush) {
      for (std::size_t i = 0; i < brush.faceCount(); ++i)
      {
        auto attributes = brush.face(i).uvAttributes();
        attributes.offset = vm::vec2f{13.3f + float(i), -7.1f};
        attributes.scale = vm::vec2f{0.37f, 1.7f + 0.01f * float(i)};
        attributes.rotation = 17.0f + float(i) * 11.0f;
        REQUIRE(brush.face(i).setUvAttributes(attributes) | kdl::is_success());
      }
    });
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});

    auto stream = std::stringstream{};
    auto taskManager = kdl::task_manager{1};
    mdl::NodeWriter{map.worldNode(), stream}.writeBrushFaces(
      brushNode->brush().faces(), taskManager);
    auto lines = std::vector<std::string>{};
    for (auto line = std::string{}; std::getline(stream, line);)
    {
      if (line.find('[') != std::string::npos)
      {
        lines.push_back(line);
      }
    }

    auto builder = LitSnapshotBuilder{1, 1};
    builder.addBrush(2, 1, true, false, *brushNode);
    const auto snapshot = builder.take();
    REQUIRE(snapshot.faces.size() == brushNode->brush().faceCount());
    REQUIRE(lines.size() == snapshot.faces.size());

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
      CAPTURE(i, lines[i]);
      const auto numbers = readFaceLine(lines[i]);
      REQUIRE(numbers.u.size() == 4u);
      REQUIRE(numbers.v.size() == 4u);
      const auto& face = snapshot.faces[i];
      CHECK(face.uAxis == vm::vec3f{numbers.u[0], numbers.u[1], numbers.u[2]});
      CHECK(face.vAxis == vm::vec3f{numbers.v[0], numbers.v[1], numbers.v[2]});
      CHECK(face.offset == vm::vec2f{numbers.u[3], numbers.v[3]});
      CHECK(face.scale == vm::vec2f{numbers.scaleU, numbers.scaleV});
    }

    // a double and a float each through their own shortest text
    CHECK(serialisedFloat(0.1) == 0.1f);
    CHECK(serialisedFloat(0.1f) == 0.1f);
    CHECK(serialisedFloat(-0.0) == 0.0f);
  }

  SECTION("a brush: polygons in world space, points, plane, material")
  {
    auto* brushNode = mdl::createBrushNode(map, "lp1b/floor");
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    const auto& faces = brushNode->brush().faces();

    auto builder = LitSnapshotBuilder{3, 9};
    builder.addBrush(5, 4, false, true, *brushNode);
    const auto snapshot = builder.take();
    CHECK(snapshot.generation == 3u);
    CHECK(snapshot.sequence == 9u);

    const auto& node = onlyNode(snapshot);
    CHECK(node.id == 5u);
    CHECK(node.entity == 4u);
    CHECK(node.kind == lit::SnapshotNodeKind::Brush);
    CHECK_FALSE(node.visible);
    CHECK(node.active);
    REQUIRE(node.elements.first == 0u);
    REQUIRE(node.elements.count == faces.size());

    for (std::size_t i = 0; i < faces.size(); ++i)
    {
      CAPTURE(i);
      const auto& face = snapshot.faces[i];
      const auto polygon = faces[i].polygon();
      REQUIRE(face.polygon.count == polygon.vertices().size());
      for (std::size_t v = 0; v < polygon.vertices().size(); ++v)
      {
        CHECK(snapshot.vertices[face.polygon.first + v] == polygon.vertices()[v]);
      }
      CHECK(face.points == faces[i].points());
      CHECK(face.plane == faces[i].boundary());
      CHECK(snapshot.string(face.material) == "lp1b/floor");
    }

    // one material, pooled once
    CHECK(snapshot.strings == "lp1b/floor");
  }

  SECTION("a face without a material is written as the serialiser writes it")
  {
    auto* brushNode = mdl::createBrushNode(map, "");
    auto builder = LitSnapshotBuilder{1, 1};
    builder.addBrush(2, 1, true, false, *brushNode);
    const auto snapshot = builder.take();
    REQUIRE(!snapshot.faces.empty());
    CHECK(
      snapshot.string(snapshot.faces.front().material) == mdl::BrushFace::NoMaterialName);
    delete brushNode;
  }

  SECTION("a patch: control points row by row, counts and patchDef3 subdivisions")
  {
    auto points = std::vector<mdl::BezierPatch::Point>{};
    for (std::size_t row = 0; row < 3; ++row)
    {
      for (std::size_t column = 0; column < 5; ++column)
      {
        points.push_back({double(column), double(row), 1.0, 0.25 * double(column), 0.5});
      }
    }
    auto* patchNode = new mdl::PatchNode{
      mdl::BezierPatch{3, 5, points, "lp1b/curve", mdl::PatchSubdivisions{4, 6}}};
    auto* plainNode = mdl::createPatchNode("lp1b/plain");

    auto builder = LitSnapshotBuilder{1, 1};
    builder.addPatch(7, 1, true, false, *patchNode);
    builder.addPatch(8, 1, true, false, *plainNode);
    const auto snapshot = builder.take();
    REQUIRE(snapshot.nodes.size() == 2u);
    REQUIRE(snapshot.patches.size() == 2u);

    const auto& node = snapshot.nodes[0];
    CHECK(node.kind == lit::SnapshotNodeKind::Patch);
    CHECK(node.elements.first == 0u);
    CHECK(node.elements.count == 1u);
    const auto& patch = snapshot.patches[0];
    CHECK(snapshot.string(patch.material) == "lp1b/curve");
    CHECK(patch.rows == 3u);
    CHECK(patch.columns == 5u);
    CHECK(patch.explicitSubdivisions);
    CHECK(patch.horizontalSubdivisions == 4u);
    CHECK(patch.verticalSubdivisions == 6u);
    REQUIRE(patch.controlPoints.count == 15u);
    for (std::size_t i = 0; i < points.size(); ++i)
    {
      CHECK(snapshot.controlPoints[patch.controlPoints.first + i] == points[i]);
    }

    CHECK(snapshot.nodes[1].elements.first == 1u);
    CHECK_FALSE(snapshot.patches[1].explicitSubdivisions);
    CHECK(snapshot.patches[1].controlPoints.first == 15u);
    CHECK(snapshot.patches[1].controlPoints.count == 9u);

    delete patchNode;
    delete plainNode;
  }

  SECTION("entities: keys in map order; a point entity's node is its entity record")
  {
    const auto world = mdl::Entity{{
      {"classname", "worldspawn"},
      {"overrideMaterial", "textures/lp1b/override"},
    }};
    const auto light = mdl::Entity{{
      {"classname", "light"},
      {"origin", "1 2 3"},
      {"_color", "1 0.5 0"},
    }};

    auto builder = LitSnapshotBuilder{1, 1};
    CHECK(builder.addEntity(1, world) == 0u);
    builder.addPointEntity(9, pointEntityKind(light), true, false, light);
    builder.addRemoved(4);
    builder.setMapPath("D:/maps/lp1b.map");
    const auto snapshot = builder.take();

    REQUIRE(snapshot.entities.size() == 2u);
    CHECK(snapshot.entities[0].id == 1u);
    CHECK(snapshot.string(snapshot.entities[0].classname) == "worldspawn");
    REQUIRE(snapshot.entities[1].keys.count == 3u);
    const auto keyOf = [&](const std::size_t i) {
      const auto& keyValue = snapshot.keyValues[snapshot.entities[1].keys.first + i];
      return std::pair{
        std::string{snapshot.string(keyValue.key)},
        std::string{snapshot.string(keyValue.value)}};
    };
    CHECK(keyOf(0) == std::pair{std::string{"classname"}, std::string{"light"}});
    CHECK(keyOf(1) == std::pair{std::string{"origin"}, std::string{"1 2 3"}});
    CHECK(keyOf(2) == std::pair{std::string{"_color"}, std::string{"1 0.5 0"}});

    // worldspawn has no node; the light's node points at its record
    const auto& node = onlyNode(snapshot);
    CHECK(node.id == 9u);
    CHECK(node.entity == 9u);
    CHECK(node.kind == lit::SnapshotNodeKind::Light);
    CHECK(node.elements.first == 1u);
    CHECK(node.elements.count == 1u);

    CHECK(snapshot.removed == std::vector<lit::PreviewId>{4});
    CHECK(snapshot.mapPath == "D:/maps/lp1b.map");
    CHECK(
      pointEntityKind(mdl::Entity{{{"classname", "info_player_start"}}})
      == lit::SnapshotNodeKind::PointEntity);
  }

  SECTION("an empty builder makes an empty snapshot")
  {
    auto builder = LitSnapshotBuilder{1, 1};
    CHECK(builder.empty());
    builder.addRemoved(3);
    CHECK_FALSE(builder.empty());
  }

  SECTION("visibleInPreview: EditorContext::visible without the selection override (D65)")
  {
    auto* brushNode = mdl::createBrushNode(map);
    auto* patchNode = mdl::createPatchNode();
    auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode, patchNode, entityNode}}});
    auto& context = map.editorContext();

    CHECK(visibleInPreview(context, *brushNode));
    CHECK(visibleInPreview(context, *patchNode));
    CHECK(visibleInPreview(context, *entityNode));

    mdl::selectNodes(map, {brushNode, patchNode, entityNode});
    context.setShowBrushes(false);
    context.setShowPatches(false);
    context.setShowPointEntities(false);

    // the editor shows selected nodes; the preview, like the game, does not
    CHECK(context.visible(*brushNode));
    CHECK(context.visible(*patchNode));
    CHECK(context.visible(*entityNode));
    CHECK_FALSE(visibleInPreview(context, *brushNode));
    CHECK_FALSE(visibleInPreview(context, *patchNode));
    CHECK_FALSE(visibleInPreview(context, *entityNode));
    // the world and its layers know no filter
    CHECK(visibleInPreview(context, map.worldNode()));

    context.setShowBrushes(true);
    context.setShowPatches(true);
    context.setShowPointEntities(true);
    mdl::deselectAll(map);
    CHECK(visibleInPreview(context, *brushNode));
    CHECK(visibleInPreview(context, *patchNode));
    CHECK(visibleInPreview(context, *entityNode));
  }
}

} // namespace tb::ui
