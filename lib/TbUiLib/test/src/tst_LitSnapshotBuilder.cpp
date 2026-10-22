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

#include <QCoreApplication>

#include "TestEnvironment.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineGeometry.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"
#include "lit/NumberText.h"
#include "lit/ProcFile.h"
#include "lit/Snapshot.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/LitDocumentObserver.h"
#include "ui/LitSnapshotBuilder.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include "kd/overload.h"
#include "kd/result.h"
#include "kd/task_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

// what the engine reads: idLexer's ParseFloat (R3.10)
float engineRead(const std::string& token)
{
  const auto value = lit::lexerFloat(token);
  REQUIRE(value);
  return *value;
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

namespace
{

std::string readFile(const std::filesystem::path& path)
{
  auto in = std::ifstream{path, std::ios::binary};
  auto text = std::stringstream{};
  text << in.rdbuf();
  return text.str();
}

// listImages.txt's sizes after "@@ map" (as TbLitLibTest's LitMapFixture reads them)
std::map<std::string, std::pair<int, int>> listedSizes(const std::string& text)
{
  auto sizes = std::map<std::string, std::pair<int, int>>{};
  auto in = std::istringstream{text};
  auto line = std::string{};
  auto inMap = false;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    if (line.rfind("@@ ", 0) == 0)
    {
      inMap = line == "@@ map";
      continue;
    }
    if (!inMap || line.find("textures/") == std::string::npos)
    {
      continue;
    }
    auto fields = std::istringstream{line.substr(line.find(':') + 1)};
    auto width = 0;
    auto height = 0;
    fields >> width >> height;
    sizes[line.substr(line.rfind(' ') + 1)] = {width, height};
  }
  return sizes;
}

// the fixture's materials and the base stand-ins, parsed by the copied parser, with the
// engine's editor-image sizes Ready (as TbLitLibTest's fixtureMaterialTable)
std::shared_ptr<const lit::MaterialTable> fixtureTable(const std::filesystem::path& root)
{
  const auto sizes =
    listedSizes(readFile(root / "goldens" / "lp1b_geometry" / "listImages.txt"));
  auto source = lit::MemoryDeclSource{std::vector<lit::DeclFile>{
    lit::DeclFile{"materials/lp1bc.mtr", readFile(root / "materials" / "lp1bc.mtr"), 0},
    lit::DeclFile{
      "materials/zz_base_stand_ins.mtr",
      "textures/editor/visportal\n{\n\tqer_editorimage textures/editor/visportal\n"
      "\tareaportal\n\tnoshadows\n}\n"
      "textures/common/origin\n{\n\tqer_editorimage textures/common/origin\n"
      "\torigin\n\tnoshadows\n}\n",
      0},
  }};
  auto decls = lit::MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  auto table = std::make_shared<lit::MaterialTable>();
  table->registry = decls.id();
  for (const auto* name :
       {"textures/lp1b/floor",
        "textures/lp1b/wall",
        "textures/lp1b/crate",
        "textures/lp1b/grate",
        "textures/lp1b/glass",
        "textures/lp1b/patch",
        "textures/editor/visportal",
        "textures/common/origin"})
  {
    auto found = decls.lookup(name);
    REQUIRE(found);
    auto entry = std::make_shared<lit::MaterialEntry>(**found.value);
    if (const auto size = sizes.find(name); size != sizes.end())
    {
      entry->editorImageSize = lit::EditorImageSize{
        size->second.first, size->second.second, lit::EditorImageSize::State::Ready, {}};
    }
    table->materials = table->materials.set(
      name,
      std::make_shared<const lit::MaterialSlot>(lit::MaterialSlot{std::move(entry), {}}));
  }
  decls.takeMessages();
  return table;
}

// the .proc's _areaN surfaces per (area, material): summed triangle area
std::map<std::pair<int, std::string>, double> procAreas(const lit::ProcTree& proc)
{
  auto areas = std::map<std::pair<int, std::string>, double>{};
  for (const auto& model : proc.models)
  {
    if (model.area < 0)
    {
      continue;
    }
    for (const auto& surface : model.surfaces)
    {
      const auto stride = std::size_t(surface.vertexFormat == 1 ? 12 : 8);
      const auto at = [&](const int i) {
        const auto* v = surface.verts.data() + std::size_t(i) * stride;
        return lit::EngineVec3{v[0], v[1], v[2]};
      };
      for (auto i = std::size_t{0}; i + 2 < surface.indexes.size(); i += 3)
      {
        areas[{model.area, surface.material}] += double(lit::engineTriangleArea(
          at(surface.indexes[i]),
          at(surface.indexes[i + 1]),
          at(surface.indexes[i + 2])));
      }
    }
  }
  return areas;
}

bool closeArea(const double a, const double b, const double relative)
{
  return std::abs(a - b) <= relative * std::max(1.0, std::abs(b));
}

#if defined(_MSC_VER) && defined(_DEBUG)
// runs f between two heap checkpoints; true when the heap grew, with its statistics
template <typename F>
bool heapGrows(F&& f)
{
  _CrtMemState before;
  _CrtMemCheckpoint(&before);
  f();
  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  _CrtMemState difference;
  const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
  if (grew)
  {
    _CrtMemDumpStatistics(&difference);
  }
  return grew;
}
#endif

} // namespace

TEST_CASE("LitSnapshotBuilder: the fixture map end to end, against the .proc (golden d)")
{
  // R1.15: the pipeline the user runs: the map loaded into a document, the observer's
  // first snapshot (LitSnapshotBuilder), the chunker; checked as golden (d) checks
  // TbLitLibTest's stand-in snapshot (tst_GeometryGoldens.cpp): the world's chunk area
  // per (area, material) within 0.1 % of the engine's _areaN models (K9), every brush
  // entity's chunk bounds equal to its .proc model's, the pivots and axes
  const auto root = getFixtureRoot() / "test" / "lit";
  auto fixture = MapDocumentFixture{};
  auto& document =
    fixture.load(root / "maps" / "lp1b_geometry.map", mdl::Doom3BFGFixtureConfig);

  auto snapshot = std::shared_ptr<const lit::Snapshot>{};
  auto observer = LitDocumentObserver{
    document,
    1,
    LitSnapshotTarget{
      [] { return true; },
      [] { return true; },
      [&](std::shared_ptr<const lit::Snapshot> taken) {
        snapshot = std::move(taken);
        return true;
      },
      [] {}}};
  QCoreApplication::processEvents();
  REQUIRE(snapshot);

  auto input = lit::ChunkJobInput{};
  for (const auto& entity : lit::sceneEntitiesOf(*snapshot))
  {
    input.entities[entity->id] = entity;
    if (entity->classname == "worldspawn")
    {
      input.worldspawn = entity;
    }
  }
  REQUIRE(input.worldspawn);
  auto cells = std::set<lit::CellRef>{};
  for (const auto& node : lit::sceneNodesOf(*snapshot))
  {
    input.nodes.push_back(node);
    if (
      node->kind == lit::SnapshotNodeKind::Brush
      || node->kind == lit::SnapshotNodeKind::Patch)
    {
      const auto& entity = input.entities.at(node->entity);
      cells.insert(lit::CellRef{
        entity->classname == "func_group" ? input.worldspawn->id : node->entity,
        lit::cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.materials = fixtureTable(root);
  const auto proc = std::make_shared<const lit::ProcTree>(
    lit::readProcFile(
      readFile(root / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"), true)
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value());
  input.proc = proc;
  const auto output = lit::buildChunks(input);
  static_cast<void>(lit::drainEngineMessagesForThread(std::this_thread::get_id()));
  CHECK(output.warnings.empty());
  CHECK(output.pendingMaterials.empty());

  auto chunkAreas = std::map<std::pair<int, std::string>, double>{};
  auto entityBounds = std::map<lit::PreviewId, std::array<lit::EngineVec3, 2>>{};
  for (const auto& chunk : output.chunks)
  {
    if (chunk.key.entity == input.worldspawn->id)
    {
      const auto slot = input.materials->find(chunk.key.material);
      const auto sides = slot && slot->entry && slot->entry->createBackSides ? 2.0 : 1.0;
      auto area = 0.0;
      for (auto i = std::size_t{0}; i + 2 < chunk.indexes.size(); i += 3)
      {
        area += double(lit::engineTriangleArea(
          chunk.verts[chunk.indexes[i]].xyz,
          chunk.verts[chunk.indexes[i + 1]].xyz,
          chunk.verts[chunk.indexes[i + 2]].xyz));
      }
      if (chunk.key.area >= 0)
      {
        chunkAreas[{chunk.key.area, chunk.key.material}] += area / sides;
      }
    }
    else if (const auto it = entityBounds.find(chunk.key.entity);
             it == entityBounds.end())
    {
      entityBounds[chunk.key.entity] = chunk.bounds;
    }
    else
    {
      for (auto k = std::size_t{0}; k < 3; ++k)
      {
        it->second[0][k] = std::min(it->second[0][k], chunk.bounds[0][k]);
        it->second[1][k] = std::max(it->second[1][k], chunk.bounds[1][k]);
      }
    }
  }
  const auto engineAreas = procAreas(*proc);
  REQUIRE_FALSE(engineAreas.empty());
  for (const auto& [key, area] : engineAreas)
  {
    CAPTURE(key.first, key.second, area, chunkAreas[key]);
    CHECK(closeArea(chunkAreas[key], area, 1e-3));
  }

  auto names = std::map<std::string, lit::PreviewId>{};
  for (const auto& [id, entity] : input.entities)
  {
    if (const auto* name = lit::findKey(entity->keys, "name"))
    {
      names[*name] = id;
    }
  }
  auto models = 0;
  for (const auto& model : proc->models)
  {
    if (model.area >= 0)
    {
      continue;
    }
    CAPTURE(model.name);
    REQUIRE(names.contains(model.name));
    auto points = std::vector<lit::EngineVec3>{};
    for (const auto& surface : model.surfaces)
    {
      for (auto j = 0; j < surface.numVerts; ++j)
      {
        const auto* v = surface.verts.data() + std::size_t(j) * 8;
        points.push_back({v[0], v[1], v[2]});
      }
    }
    REQUIRE(entityBounds.contains(names.at(model.name)));
    CHECK(entityBounds.at(names.at(model.name)) == lit::engineBoundsOfPoints(points));
    ++models;
  }
  CHECK(models >= 2);

  const auto state = [&](const std::string& name) {
    const auto it =
      std::find_if(output.entities.begin(), output.entities.end(), [&](const auto& s) {
        return s.id == names.at(name);
      });
    REQUIRE(it != output.entities.end());
    return *it;
  };
  const auto rotator = state("lp1b_rotator");
  CHECK(rotator.originOffset == lit::EngineVec3{12, 12, -12});
  CHECK(rotator.modelOrigin == lit::EngineVec3{140, 76, 20});
  CHECK(rotator.parms.axis == lit::EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1});
  const auto scaled = state("lp1b_scaled");
  CHECK(scaled.modelOrigin == lit::EngineVec3{-128, -64, 32});
  CHECK(scaled.parms.axis == lit::EngineMat3{2, 0, 0, 0, 2, 0, 0, 0, 2});
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("LitSnapshotBuilder: 50 observers and 50 snapshots leave the heap (R2.14)")
{
  const auto root = getFixtureRoot() / "test" / "lit";
  auto fixture = MapDocumentFixture{};
  auto& document =
    fixture.load(root / "maps" / "lp1b_geometry.map", mdl::Doom3BFGFixtureConfig);

  // an observer made and dropped: its subscriptions, its id map and its first snapshot
  auto missed = 0;
  const auto observe = [&] {
    for (auto i = 0; i < 50; ++i)
    {
      auto taken = std::shared_ptr<const lit::Snapshot>{};
      {
        auto observer = LitDocumentObserver{
          document,
          1,
          LitSnapshotTarget{
            [] { return true; },
            [] { return true; },
            [&](std::shared_ptr<const lit::Snapshot> snapshot) {
              taken = std::move(snapshot);
              return true;
            },
            [] {}}};
        QCoreApplication::processEvents();
      }
      QCoreApplication::processEvents();
      missed += taken ? 0 : 1;
    }
  };
  // a snapshot of every brush and patch, built and freed
  const auto snapshots = [&] {
    for (auto i = 0; i < 50; ++i)
    {
      auto builder = LitSnapshotBuilder{1, std::uint64_t(i)};
      auto id = lit::PreviewId{1};
      const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
        node.accept(kdl::overload(
          [](const mdl::WorldNode&) {},
          [](const mdl::LayerNode&) {},
          [](const mdl::GroupNode&) {},
          [](const mdl::EntityNode&) {},
          [&](const mdl::BrushNode& brush) {
            builder.addBrush(id++, 1, true, false, brush);
          },
          [&](const mdl::PatchNode& patch) {
            builder.addPatch(id++, 1, true, false, patch);
          }));
        for (const auto* child : node.children())
        {
          self(self, *child);
        }
      };
      visit(visit, document.map().worldNode());
      missed += builder.take().nodes.empty() ? 1 : 0;
    }
  };
  // what the first round allocates for good (Qt's event queue, fmt's buffers)
  observe();
  snapshots();
  CHECK_FALSE(heapGrows(observe));
  CHECK_FALSE(heapGrows(snapshots));
  CHECK(missed == 0);
}
#endif

} // namespace tb::ui
