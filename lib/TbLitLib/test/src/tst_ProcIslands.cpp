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

// LP7.4 (C55, D133, D138): the islands of the .proc's surfaces, against the fixture map's
// .proc (goldens/lp1b_geometry, dmap's own output) and hand-written surfaces.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/BprocFile.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineGeometry.h"
#include "lit/GameDeclSource.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"
#include "lit/ProcIslands.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

ProcTree fixtureProc()
{
  auto tree =
    readProcFile(
      readTextFile(
        litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"),
      true)
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return tree;
}

// the index of the model named name, or the model count
std::size_t modelNamed(const ProcTree& tree, const std::string& name)
{
  for (auto m = std::size_t{0}; m < tree.models.size(); ++m)
  {
    if (tree.models[m].name == name)
    {
      return m;
    }
  }
  return tree.models.size();
}

// a surface of the basic format from (x y z s t nx ny nz) rows and its indexes
ProcSurface handSurface(
  const std::vector<std::array<float, 8>>& rows, const std::vector<int>& indexes)
{
  auto surface = ProcSurface{};
  surface.material = "textures/lp74/hand";
  surface.numVerts = int(rows.size());
  surface.numIndexes = int(indexes.size());
  for (const auto& row : rows)
  {
    surface.verts.insert(surface.verts.end(), row.begin(), row.end());
  }
  surface.indexes = indexes;
  return surface;
}

// ----- step 3: the candidates and the test -----

bool sameName(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
              return std::tolower(static_cast<unsigned char>(a))
                     == std::tolower(static_cast<unsigned char>(b));
            });
}

// one island's verdict, by model, surface and island
struct IslandResult
{
  std::string model;
  std::string material;
  std::size_t surface = 0;
  std::size_t island = 0;
  IslandMatch match;
  std::vector<const AdoptionPiece*> candidates;
};

// the islands of every model the snapshot's entities have pieces of, through the test
struct Evaluation
{
  std::size_t islands = 0;
  std::size_t adopted = 0;
  std::size_t unmatched = 0;
  std::size_t curved = 0;
  std::vector<IslandResult> results;
  // what the results' candidates point into
  std::shared_ptr<const std::vector<ChunkPiece>> pieces;

  const IslandResult* find(
    const std::string& model, const std::string& material, const std::size_t index) const
  {
    auto count = std::size_t{0};
    for (const auto& result : results)
    {
      if (
        result.model == model && sameName(result.material, material) && count++ == index)
      {
        return &result;
      }
    }
    return nullptr;
  }

  std::vector<const IslandResult*> of(
    const std::string& model, const std::string& material) const
  {
    auto found = std::vector<const IslandResult*>{};
    for (const auto& result : results)
    {
      if (result.model == model && sameName(result.material, material))
      {
        found.push_back(&result);
      }
    }
    return found;
  }
};

Evaluation evaluate(
  const ProcTree& tree, const Snapshot& snapshot, std::vector<ChunkPiece> allPieces)
{
  // the world's chunk entity and the brush entities by name (K17: dmap names the model by
  // the entity's name)
  auto worldspawn = NoPreviewId;
  auto byName = std::map<std::string, PreviewId>{};
  for (const auto& entity : snapshot.entities)
  {
    if (sameName(snapshot.string(entity.classname), "worldspawn"))
    {
      worldspawn = entity.id;
    }
    for (auto k = entity.keys.first; k < entity.keys.first + entity.keys.count; ++k)
    {
      if (snapshot.string(snapshot.keyValues[k].key) == "name")
      {
        auto name = std::string{snapshot.string(snapshot.keyValues[k].value)};
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
          return char(std::tolower(c));
        });
        byName[name] = entity.id;
      }
    }
  }
  const auto islands = buildProcIslands(tree);
  auto evaluation = Evaluation{};
  evaluation.pieces =
    std::make_shared<const std::vector<ChunkPiece>>(std::move(allPieces));
  const auto& pieces = *evaluation.pieces;
  for (auto m = std::size_t{0}; m < tree.models.size(); ++m)
  {
    const auto& model = tree.models[m];
    auto entity = worldspawn;
    if (model.area < 0)
    {
      auto name = model.name;
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return char(std::tolower(c));
      });
      const auto it = byName.find(name);
      entity = it != byName.end() ? it->second : NoPreviewId;
    }
    auto modelPieces = std::vector<const AdoptionPiece*>{};
    for (const auto& piece : pieces)
    {
      if (
        piece.cell.entity == entity
        && (model.area >= 0 ? piece.piece.area == model.area : piece.piece.area == -1))
      {
        modelPieces.push_back(&piece.piece);
      }
    }
    if (modelPieces.empty())
    {
      continue;
    }
    for (auto s = std::size_t{0}; s < model.surfaces.size(); ++s)
    {
      const auto& list = islands.models[m].surfaces[s];
      for (auto i = std::size_t{0}; i < list.size(); ++i)
      {
        auto result = IslandResult{};
        result.model = model.name;
        result.material = model.surfaces[s].material;
        result.surface = s;
        result.island = i;
        result.match = matchProcIsland(model.surfaces[s], list[i], modelPieces);
        for (const auto c : result.match.candidates)
        {
          result.candidates.push_back(modelPieces[c]);
        }
        ++evaluation.islands;
        if (result.match.verdict == IslandVerdict::Adopted)
        {
          ++evaluation.adopted;
        }
        else if (result.match.verdict == IslandVerdict::Curved)
        {
          ++evaluation.curved;
        }
        else
        {
          ++evaluation.unmatched;
        }
        evaluation.results.push_back(std::move(result));
      }
    }
  }
  return evaluation;
}

// the candidates' vertices that are no island vertex: the corners dmap's optimizer
// dropped (colinear, optimize.cpp:1967-2000)
std::vector<EngineVec3> droppedCorners(const ProcTree& tree, const IslandResult& result)
{
  const auto m = modelNamed(tree, result.model);
  const auto& surface = tree.models[m].surfaces[result.surface];
  const auto island = procSurfaceIslands(surface)[result.island];
  auto dropped = std::vector<EngineVec3>{};
  for (const auto* piece : result.candidates)
  {
    for (const auto& p : piece->points)
    {
      const auto isVertex = std::ranges::any_of(island.verts, [&](const int k) {
        const auto* v = surface.verts.data() + std::size_t(k) * 8;
        return std::abs(v[0] - p[0]) < 0.01f && std::abs(v[1] - p[1]) < 0.01f
               && std::abs(v[2] - p[2]) < 0.01f;
      });
      const auto known = std::ranges::any_of(dropped, [&](const EngineVec3& d) {
        return std::abs(d[0] - p[0]) < 0.01f && std::abs(d[1] - p[1]) < 0.01f
               && std::abs(d[2] - p[2]) < 0.01f;
      });
      if (!isVertex && !known)
      {
        dropped.push_back(p);
      }
    }
  }
  return dropped;
}

bool containsPoint(const std::vector<EngineVec3>& points, const EngineVec3& point)
{
  return std::ranges::any_of(points, [&](const EngineVec3& p) {
    return std::abs(p[0] - point[0]) < 0.01f && std::abs(p[1] - point[1]) < 0.01f
           && std::abs(p[2] - point[2]) < 0.01f;
  });
}

const char* verdictName(const IslandVerdict verdict)
{
  switch (verdict)
  {
  case IslandVerdict::Adopted:
    return "adopted";
  case IslandVerdict::Curved:
    return "curved";
  case IslandVerdict::NoCandidate:
    return "no candidate";
  case IslandVerdict::IslandOutside:
    return "island outside";
  case IslandVerdict::PieceOutside:
    return "piece outside";
  case IslandVerdict::Overlap:
    return "overlap";
  case IslandVerdict::Area:
    return "area";
  }
  return "?";
}

// the fixture's snapshot through the chunk builder, every cell, its pieces collected
struct FixtureScene
{
  Snapshot snapshot;
  std::shared_ptr<const MaterialTable> materials;
  std::shared_ptr<const ProcTree> proc;
};

FixtureScene fixtureScene()
{
  const auto root = litFixtureFolder();
  auto scene = FixtureScene{};
  scene.snapshot = snapshotOfMapText(readTextFile(root / "maps" / "lp1b_geometry.map"));
  scene.materials = fixtureMaterialTable(
    listImagesSizes(readTextFile(root / "goldens" / "lp1b_geometry" / "listImages.txt")));
  scene.proc = std::make_shared<const ProcTree>(fixtureProc());
  return scene;
}

ChunkJobInput allCells(
  const Snapshot& snapshot,
  std::shared_ptr<const ProcTree> proc,
  std::shared_ptr<const MaterialTable> materials)
{
  auto input = ChunkJobInput{};
  for (const auto& entity : sceneEntitiesOf(snapshot))
  {
    input.entities[entity->id] = entity;
    if (sameName(entity->classname, "worldspawn"))
    {
      input.worldspawn = entity;
    }
  }
  auto cells = std::set<CellRef>{};
  for (const auto& node : sceneNodesOf(snapshot))
  {
    input.nodes.push_back(node);
    if (node->kind == SnapshotNodeKind::Brush || node->kind == SnapshotNodeKind::Patch)
    {
      const auto entity = input.entities.at(node->entity);
      cells.insert(CellRef{
        sameName(entity->classname, "func_group") ? input.worldspawn->id : node->entity,
        cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.proc = std::move(proc);
  input.materials = std::move(materials);
  input.collectPieces = true;
  return input;
}

std::vector<ChunkPiece> piecesOf(const ChunkJobInput& input)
{
  auto output = buildChunks(input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return std::move(output.pieces);
}

// the snapshot face whose polygon has a vertex at each of corners
SnapshotFace* faceAt(Snapshot& snapshot, const std::vector<vm::vec3d>& corners)
{
  for (auto& face : snapshot.faces)
  {
    const auto has = [&](const vm::vec3d& corner) {
      for (auto v = face.polygon.first; v < face.polygon.first + face.polygon.count; ++v)
      {
        if (vm::length(snapshot.vertices[v] - corner) < 1e-6)
        {
          return true;
        }
      }
      return false;
    };
    if (std::ranges::all_of(corners, has))
    {
      return &face;
    }
  }
  return nullptr;
}

// a face moved by offset: its polygon and its three plane points (nothing else of the
// brush), as an edit of that face alone would leave it
void moveFace(Snapshot& snapshot, SnapshotFace& face, const vm::vec3d& offset)
{
  for (auto v = face.polygon.first; v < face.polygon.first + face.polygon.count; ++v)
  {
    snapshot.vertices[v] = snapshot.vertices[v] + offset;
  }
  for (auto& point : face.points)
  {
    point = point + offset;
  }
}

/**
 * A surface as dmap writes coplanar quads: per quad (x0 y0 x1 y1 on z 0, facing +z) its
 * corners with the st its side's Valve 220 values give through dmap's rule and
 * SetTexCoord's half floats (TriListForSide), no vertex shared between quads (MatchVert
 * welds none: their st differ), and the piece each quad's face makes
 */
struct DmapQuads
{
  ProcSurface surface;
  std::vector<AdoptionPiece> pieces;
};

DmapQuads dmapQuads(
  const std::vector<std::array<float, 4>>& quads,
  const std::vector<EngineValveTexture>& textures)
{
  auto result = DmapQuads{};
  result.surface.material = "textures/lp74/quads";
  for (auto q = std::size_t{0}; q < quads.size(); ++q)
  {
    const auto [x0, y0, x1, y1] = quads[q];
    // the engine's winding: clockwise seen from +z
    const auto corners =
      std::vector<EngineVec3>{{x0, y0, 0}, {x0, y1, 0}, {x1, y1, 0}, {x1, y0, 0}};
    const auto first = int(result.surface.numVerts);
    for (const auto& c : corners)
    {
      const auto st = engineValveTexCoord(c, textures[q], 64, 64);
      auto vert = engineClearedVert();
      engineSetTexCoord(vert, st[0], st[1]);
      const auto stored = engineTexCoord(vert);
      result.surface.verts.insert(
        result.surface.verts.end(), {c[0], c[1], c[2], stored[0], stored[1], 0, 0, 1});
      ++result.surface.numVerts;
    }
    result.surface.indexes.insert(
      result.surface.indexes.end(),
      {first, first + 1, first + 2, first, first + 2, first + 3});
    auto piece = AdoptionPiece{};
    piece.key = AdoptionPieceKey{PreviewId{q + 1}, 0, 0};
    piece.material = result.surface.material;
    piece.plane = EnginePlane{0, 0, 1, 0};
    piece.texVec = engineValveTextureVectors(textures[q]);
    piece.textureWidth = 64;
    piece.textureHeight = 64;
    piece.points = corners;
    result.pieces.push_back(std::move(piece));
  }
  result.surface.numIndexes = int(result.surface.indexes.size());
  return result;
}

std::vector<const AdoptionPiece*> pointersOf(const std::vector<AdoptionPiece>& pieces)
{
  auto pointers = std::vector<const AdoptionPiece*>{};
  for (const auto& piece : pieces)
  {
    pointers.push_back(&piece);
  }
  return pointers;
}

} // namespace

TEST_CASE("ProcIslands: the fixture's .proc (LP7.4, C55)")
{
  const auto tree = fixtureProc();
  const auto islands = buildProcIslands(tree);
  REQUIRE(islands.models.size() == tree.models.size());

  SECTION("the islands per surface; the patches curved, everything else planar")
  {
    // per model, per surface: its islands (the .proc's order: the brush entities first)
    const auto expected = std::map<std::string, std::vector<std::size_t>>{
      {"lp1b_scaled", {6}},
      {"lp1b_rotator", {6}},
      {"_area0", {1, 5, 5, 1}},
      {"_area1", {1, 8, 5, 1}},
    };
    auto planar = std::size_t{0};
    auto curved = std::size_t{0};
    for (auto m = std::size_t{0}; m < tree.models.size(); ++m)
    {
      const auto& model = tree.models[m];
      CAPTURE(model.name);
      REQUIRE(expected.contains(model.name));
      const auto& counts = expected.at(model.name);
      REQUIRE(islands.models[m].surfaces.size() == counts.size());
      for (auto s = std::size_t{0}; s < counts.size(); ++s)
      {
        const auto& surface = model.surfaces[s];
        CAPTURE(surface.material);
        const auto& list = islands.models[m].surfaces[s];
        CHECK(list.size() == counts[s]);
        auto triangles = std::size_t{0};
        auto verts = std::size_t{0};
        for (const auto& island : list)
        {
          triangles += island.indexes.size() / 3;
          verts += island.verts.size();
          const auto patch = surface.material == "textures/lp1b/patch";
          CHECK(island.planar == !patch);
        }
        // every triangle and every used vertex in exactly one island
        CHECK(triangles * 3 == surface.indexes.size());
        CHECK(verts == std::size_t(surface.numVerts));
      }
      planar += islands.models[m].planarIslands;
      curved += islands.models[m].curvedIslands;
    }
    CHECK(planar == 37u);
    CHECK(curved == 2u);
    std::cout << "LP7.4 fixture islands: " << planar << " planar, " << curved
              << " curved\n";
  }

  SECTION("a planar island's plane and st map are the .proc's own")
  {
    for (auto m = std::size_t{0}; m < tree.models.size(); ++m)
    {
      const auto& model = tree.models[m];
      for (auto s = std::size_t{0}; s < model.surfaces.size(); ++s)
      {
        const auto& surface = model.surfaces[s];
        for (const auto& island : islands.models[m].surfaces[s])
        {
          if (!island.planar)
          {
            continue;
          }
          CAPTURE(model.name, surface.material, island.verts.front());
          for (const auto k : island.verts)
          {
            const auto* v = surface.verts.data() + std::size_t(k) * 8;
            // the plane faces as dmap's side normal, every vertex on it
            const auto facing =
              island.plane.a * v[5] + island.plane.b * v[6] + island.plane.c * v[7];
            CHECK(facing > 0.999f);
            CHECK(
              std::abs(enginePlaneDistance(island.plane, {v[0], v[1], v[2]})) < 1e-4f);
            // the affine st map of the reference triangle gives every vertex's st
            const auto& map = island.stMap;
            const auto s0 =
              map[0][0] * v[0] + map[0][1] * v[1] + map[0][2] * v[2] + map[0][3];
            const auto t0 =
              map[1][0] * v[0] + map[1][1] * v[1] + map[1][2] * v[2] + map[1][3];
            CHECK(std::abs(s0 - v[3]) < 1e-3f);
            CHECK(std::abs(t0 - v[4]) < 1e-3f);
          }
        }
      }
    }

    // two planes by name: the floor of area 0 (+z through z 0) and its ceiling (-z
    // through z 128, the wall material's first island)
    const auto area0Index = modelNamed(tree, "_area0");
    REQUIRE(area0Index < tree.models.size());
    const auto& floor = islands.models[area0Index].surfaces[0].front();
    CHECK(floor.verts.size() == 6u);
    CHECK(floor.plane.c == 1.0f);
    CHECK(floor.plane.d == 0.0f);
    CHECK(std::abs(floor.area - 248.0f * 256.0f) < 1.0f);
    const auto& ceiling = islands.models[area0Index].surfaces[1].front();
    CHECK(ceiling.plane.c == -1.0f);
    CHECK(ceiling.plane.d == 128.0f);
    // the doorway wall: the lintel's two top corners dropped (optimize.cpp:1967-2000)
    const auto& doorway = islands.models[area0Index].surfaces[1].back();
    CHECK(doorway.verts.size() == 8u);
    CHECK(doorway.plane.a == 1.0f);
    CHECK(doorway.plane.d == -8.0f);
  }

  SECTION("loadProcIsland is loadProcSurface's, byte for byte, island by island")
  {
    for (auto m = std::size_t{0}; m < tree.models.size(); ++m)
    {
      const auto& model = tree.models[m];
      for (auto s = std::size_t{0}; s < model.surfaces.size(); ++s)
      {
        const auto& surface = model.surfaces[s];
        const auto whole = loadProcSurface(surface);
        REQUIRE(whole.size() == std::size_t(surface.numVerts));
        for (const auto& island : islands.models[m].surfaces[s])
        {
          const auto geometry = loadProcIsland(surface, island);
          REQUIRE(geometry.verts.size() == island.verts.size());
          REQUIRE(geometry.indexes.size() == island.indexes.size());
          for (auto i = std::size_t{0}; i < island.verts.size(); ++i)
          {
            CHECK(geometry.verts[i] == whole[std::size_t(island.verts[i])]);
          }
          for (auto i = std::size_t{0}; i < island.indexes.size(); ++i)
          {
            CHECK(island.verts[geometry.indexes[i]] == island.indexes[i]);
          }
        }
      }
    }
  }

  SECTION("D138: a .bproc's finished models yield no islands")
  {
    auto in = std::ifstream{
      litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.bproc",
      std::ios::binary};
    const auto chars = std::vector<char>{
      std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto bytes = std::vector<std::byte>(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    const auto bproc = readBprocTree(bytes, true)
                       | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto finished = buildProcIslands(bproc.tree);
    REQUIRE(finished.models.size() == bproc.tree.models.size());
    REQUIRE_FALSE(finished.models.empty());
    for (const auto& model : finished.models)
    {
      CHECK(model.planarIslands == 0u);
      CHECK(model.curvedIslands == 0u);
      for (const auto& surface : model.surfaces)
      {
        CHECK(surface.empty());
      }
    }
    // and a surface read without its geometry has none either
    const auto bare =
      readProcFile(readTextFile(
        litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"))
      | kdl::value();
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    CHECK(procSurfaceIslands(bare.models.front().surfaces.front()).empty());
  }
}

TEST_CASE("ProcIslands: hand-written surfaces (LP7.4, C55)")
{
  SECTION("two disjoint coplanar triangles are two islands, one plane")
  {
    const auto surface = handSurface(
      {{0, 0, 0, 0, 0, 0, 0, 1},
       {0, 16, 0, 0, 1, 0, 0, 1},
       {16, 0, 0, 1, 0, 0, 0, 1},
       {32, 0, 0, 2, 0, 0, 0, 1},
       {32, 16, 0, 2, 1, 0, 0, 1},
       {48, 0, 0, 3, 0, 0, 0, 1}},
      {0, 1, 2, 3, 4, 5});
    const auto islands = procSurfaceIslands(surface);
    REQUIRE(islands.size() == 2u);
    CHECK(islands[0].verts == std::vector<int>{0, 1, 2});
    CHECK(islands[1].verts == std::vector<int>{3, 4, 5});
    for (const auto& island : islands)
    {
      CHECK(island.planar);
      CHECK(island.plane.c == 1.0f);
      CHECK(island.area == 128.0f);
      // s = x / 16, t = y / 16
      CHECK(std::abs(island.stMap[0][0] - 1.0f / 16.0f) < 1e-7f);
      CHECK(std::abs(island.stMap[1][1] - 1.0f / 16.0f) < 1e-7f);
    }
  }

  SECTION("a shared vertex joins two triangles into one island")
  {
    const auto surface = handSurface(
      {{0, 0, 0, 0, 0, 0, 0, 1},
       {0, 16, 0, 0, 1, 0, 0, 1},
       {16, 0, 0, 1, 0, 0, 0, 1},
       {32, 16, 0, 2, 1, 0, 0, 1},
       {32, 0, 0, 2, 0, 0, 0, 1}},
      {0, 1, 2, 2, 3, 4});
    const auto islands = procSurfaceIslands(surface);
    REQUIRE(islands.size() == 1u);
    CHECK(islands[0].verts.size() == 5u);
    CHECK(islands[0].indexes.size() == 6u);
    CHECK(islands[0].planar);
  }

  SECTION("a fold beyond 0.999 or a vertex 1/32 off the plane is curved")
  {
    // two triangles sharing an edge, the second tilted by 5 degrees about it
    const auto tilt = 16.0f * std::tan(5.0f * 3.14159265f / 180.0f);
    const auto folded = handSurface(
      {{0, 0, 0, 0, 0, 0, 0, 1},
       {0, 16, 0, 0, 1, 0, 0, 1},
       {16, 0, 0, 1, 0, 0, 0, 1},
       {16, 16, tilt, 1, 1, 0, 0, 1}},
      {0, 1, 2, 2, 1, 3});
    const auto curved = procSurfaceIslands(folded);
    REQUIRE(curved.size() == 1u);
    CHECK_FALSE(curved[0].planar);

    // a quarter of a unit off: the normals agree within 0.999, the distance does not
    const auto stepped = handSurface(
      {{0, 0, 0, 0, 0, 0, 0, 1},
       {0, 64, 0, 0, 1, 0, 0, 1},
       {64, 0, 0, 1, 0, 0, 0, 1},
       {64, 64, 0.25f, 1, 1, 0, 0, 1}},
      {0, 1, 2, 2, 1, 3});
    const auto off = procSurfaceIslands(stepped);
    REQUIRE(off.size() == 1u);
    CHECK_FALSE(off[0].planar);

    // within 1/32: planar
    const auto flat = handSurface(
      {{0, 0, 0, 0, 0, 0, 0, 1},
       {0, 64, 0, 0, 1, 0, 0, 1},
       {64, 0, 0, 1, 0, 0, 0, 1},
       {64, 64, 0.015625f, 1, 1, 0, 0, 1}},
      {0, 1, 2, 2, 1, 3});
    const auto snapped = procSurfaceIslands(flat);
    REQUIRE(snapped.size() == 1u);
    CHECK(snapped[0].planar);
  }
}

TEST_CASE("ProcIslands: the test on the fixture pair (LP7.4, C55, P7.1)")
{
  auto scene = fixtureScene();
  const auto run = [&](const Snapshot& snapshot) {
    return evaluate(
      *scene.proc, snapshot, piecesOf(allCells(snapshot, scene.proc, scene.materials)));
  };

  SECTION("every planar island adopted, the patches curved")
  {
    const auto evaluation = run(scene.snapshot);
    for (const auto& result : evaluation.results)
    {
      if (result.match.verdict != IslandVerdict::Adopted)
      {
        std::cout << "LP7.4 fixture: " << result.model << " " << result.material
                  << " island " << result.island << ": "
                  << verdictName(result.match.verdict) << "\n";
      }
    }
    CHECK(evaluation.islands == 39u);
    CHECK(evaluation.adopted == 37u);
    CHECK(evaluation.unmatched == 0u);
    CHECK(evaluation.curved == 2u);

    // the doorway wall of area 0 (brushes 6, 7 and the lintel 8 at x 8): one island of
    // three faces, the lintel's top corners dropped by the optimizer and passing by
    // containment
    const auto doorway = evaluation.of("_area0", "textures/lp1b/wall");
    REQUIRE(doorway.size() == 5u);
    CHECK(doorway.back()->match.verdict == IslandVerdict::Adopted);
    auto faces = std::set<PreviewId>{};
    for (const auto* piece : doorway.back()->candidates)
    {
      faces.insert(piece->key.node);
    }
    CHECK(faces.size() == 3u);
    const auto dropped = droppedCorners(*scene.proc, *doorway.back());
    CHECK(containsPoint(dropped, {8, -32, 128}));
    CHECK(containsPoint(dropped, {8, 32, 128}));
    // the brush entities' islands match their own entity's faces
    CHECK(evaluation.of("lp1b_rotator", "textures/lp1b/crate").size() == 6u);
    for (const auto* result : evaluation.of("lp1b_scaled", "textures/lp1b/crate"))
    {
      CHECK(result->match.verdict == IslandVerdict::Adopted);
      CHECK(result->candidates.size() == 1u);
    }
  }

  SECTION("a face moved by one unit fails its island and no other")
  {
    // off its plane: the grate's top (brush 10, z 64)
    {
      auto snapshot = scene.snapshot;
      auto* top = faceAt(snapshot, {{100, -64, 64}, {104, 64, 64}});
      REQUIRE(top);
      moveFace(snapshot, *top, {0, 0, 1});
      const auto evaluation = run(snapshot);
      CHECK(evaluation.adopted == 36u);
      CHECK(evaluation.unmatched == 1u);
      for (const auto& result : evaluation.results)
      {
        if (result.match.verdict == IslandVerdict::NoCandidate)
        {
          CHECK(result.model == "_area0");
          CHECK(result.material == "textures/lp1b/grate");
          CHECK(result.island == 0u);
        }
      }
    }
    // within its plane: the lintel's face at x 8 (brush 8) moved by one unit along y
    // leaves a strip of the doorway wall bare and overlaps brush 7's face
    {
      auto snapshot = scene.snapshot;
      auto* lintel = faceAt(snapshot, {{8, -32, 96}, {8, 32, 128}});
      REQUIRE(lintel);
      moveFace(snapshot, *lintel, {0, 1, 0});
      const auto evaluation = run(snapshot);
      CHECK(evaluation.adopted == 36u);
      CHECK(evaluation.unmatched == 1u);
      const auto doorway = evaluation.of("_area0", "textures/lp1b/wall");
      REQUIRE(doorway.size() == 5u);
      CHECK(doorway.back()->match.verdict != IslandVerdict::Adopted);
      std::cout << "LP7.4 fixture: the lintel moved by one unit: "
                << verdictName(doorway.back()->match.verdict) << "\n";
    }
  }

  SECTION("a face whose material changed fails its island alone")
  {
    auto snapshot = scene.snapshot;
    auto* top = faceAt(snapshot, {{100, -64, 64}, {104, 64, 64}});
    REQUIRE(top);
    const auto name = std::string{"lp1b/wall"};
    top->material =
      SnapshotString{std::uint32_t(snapshot.strings.size()), std::uint32_t(name.size())};
    snapshot.strings += name;
    const auto evaluation = run(snapshot);
    CHECK(evaluation.adopted == 36u);
    CHECK(evaluation.unmatched == 1u);
    const auto* grate = evaluation.find("_area0", "textures/lp1b/grate", 0);
    REQUIRE(grate);
    CHECK(grate->match.verdict == IslandVerdict::NoCandidate);
  }
}

TEST_CASE("ProcIslands: the test on hand-made dmap surfaces (LP7.4, C55, P7.1)")
{
  const auto plain = EngineValveTexture{{1, 0, 0}, 0, {0, -1, 0}, 0, 1, 1};
  const auto shifted = EngineValveTexture{{1, 0, 0}, 16, {0, -1, 0}, 0, 1, 1};

  SECTION("two coplanar faces with another alignment: two islands, two groups")
  {
    // US:62-94: a texVec beyond the epsilons is another optimize group, and MatchVert
    // welds no vertex between them (their st differ): two islands
    auto quads = dmapQuads({{0, 0, 64, 64}, {64, 0, 128, 64}}, {plain, shifted});
    // and a face over the first quad with the second's alignment (P7.1 (c))
    auto over = quads.pieces[1];
    over.key.node = 9;
    over.points = quads.pieces[0].points;
    quads.pieces.push_back(over);
    const auto islands = procSurfaceIslands(quads.surface);
    REQUIRE(islands.size() == 2u);
    const auto pieces = pointersOf(quads.pieces);
    const auto first = matchProcIsland(quads.surface, islands[0], pieces);
    CHECK(first.verdict == IslandVerdict::Adopted);
    CHECK(first.candidates == std::vector<std::size_t>{0});
    const auto second = matchProcIsland(quads.surface, islands[1], pieces);
    CHECK(second.verdict == IslandVerdict::Adopted);
    CHECK(second.candidates == std::vector<std::size_t>{1});
  }

  SECTION("a T-junction neighbour and the dropped shared corners pass")
  {
    // a wall of two faces (0..64 and 64..128) whose shared edge's corners dmap's
    // optimizer dropped, and a vertex the T-junction fix put on the first face's bottom
    // edge for a neighbour's corner at (32, 0)
    auto quads = dmapQuads({{0, 0, 64, 64}, {64, 0, 128, 64}}, {plain, plain});
    auto surface = ProcSurface{};
    surface.material = quads.surface.material;
    const auto corners = std::vector<EngineVec3>{
      {0, 64, 0}, {32, 0, 0}, {0, 0, 0}, {128, 0, 0}, {128, 64, 0}};
    for (const auto& c : corners)
    {
      const auto st = engineValveTexCoord(c, plain, 64, 64);
      auto vert = engineClearedVert();
      engineSetTexCoord(vert, st[0], st[1]);
      const auto stored = engineTexCoord(vert);
      surface.verts.insert(
        surface.verts.end(), {c[0], c[1], c[2], stored[0], stored[1], 0, 0, 1});
    }
    surface.numVerts = int(corners.size());
    // clockwise seen from +z: ( c - a ) x ( b - a ) along +z
    surface.indexes = {0, 1, 2, 0, 3, 1, 0, 4, 3};
    surface.numIndexes = int(surface.indexes.size());
    const auto islands = procSurfaceIslands(surface);
    REQUIRE(islands.size() == 1u);
    REQUIRE(islands[0].planar);
    CHECK(islands[0].plane.c == 1.0f);
    const auto pieces = pointersOf(quads.pieces);
    const auto match = matchProcIsland(surface, islands[0], pieces);
    CHECK(match.verdict == IslandVerdict::Adopted);
    CHECK(match.candidates.size() == 2u);

    // the extra vertex a unit off the edge (its st the side's there): outside the faces'
    // union
    auto off = surface;
    off.verts[1 * 8 + 1] = -1.0f;
    off.verts[1 * 8 + 4] = 1.0f / 64.0f;
    const auto offIslands = procSurfaceIslands(off);
    REQUIRE(offIslands.size() == 1u);
    CHECK(
      matchProcIsland(off, offIslands[0], pieces).verdict
      == IslandVerdict::IslandOutside);
    // a face shorter by a unit: the island's corners outside the faces' union
    auto shorter = quads.pieces;
    shorter[1].points = {{64, 0, 0}, {64, 64, 0}, {127, 64, 0}, {127, 0, 0}};
    CHECK(
      matchProcIsland(surface, islands[0], pointersOf(shorter)).verdict
      == IslandVerdict::IslandOutside);
  }
}


/**
 * LP7.4 (C55, P7.1): the sandbox's four parity pairs (TB_LIT_PARITY_ROOT, read only: each
 * .map through the snapshot path the parity case uses and the engine's .proc beside it
 * with its geometry), the materials' editor-image sizes from a material service over the
 * root: every planar island of the world and of the brush entities adopted, the curved
 * ones the patches', and the review's named cases. Hidden: it needs the root of a
 * parity.py run.
 */
TEST_CASE("ProcIslands: the sandbox's parity pairs adopted (LP7.4, P7.1)", "[.][parity]")
{
  const auto* rootValue = std::getenv("TB_LIT_PARITY_ROOT");
  if (!rootValue || !*rootValue)
  {
    SKIP(
      "TB_LIT_PARITY_ROOT is not set: the test needs the preview root of a parity run");
  }
  const auto root = std::filesystem::path{rootValue};
  auto options = MaterialServiceOptions{};
  options.makeSource = [root](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  auto service = MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

  const auto evaluateMap = [&](const std::string& map) {
    auto snapshot = snapshotOfMapText(readTextFile(root / "base" / map));
    auto procPath = root / "base" / map;
    procPath.replace_extension(".proc");
    auto tree = std::make_shared<const ProcTree>(
      readProcFile(readTextFile(procPath), true)
      | kdl::if_error([&](const auto& e) { FAIL(map << ": " << e.msg); }) | kdl::value());
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    // every face's material, its editor-image size measured (S2.2)
    auto names = std::set<std::string>{};
    for (const auto& face : snapshot.faces)
    {
      names.insert(engineSideMaterial(std::string{snapshot.string(face.material)}));
    }
    for (const auto& name : names)
    {
      static_cast<void>(service->lookup(name));
    }
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        return std::ranges::all_of(names, [&](const std::string& name) {
          const auto slot = t.find(name);
          return slot && (!slot->entry || slot->sizeMeasured);
        });
      },
      std::chrono::seconds{30});
    REQUIRE(table);
    auto patches = std::size_t{0};
    for (const auto& node : snapshot.nodes)
    {
      patches += node.kind == SnapshotNodeKind::Patch ? 1u : 0u;
    }
    auto evaluation =
      evaluate(*tree, snapshot, piecesOf(allCells(snapshot, tree, table)));
    std::cout << fmt::format(
      "LP7.4 parity pair {}: {} islands, {} adopted, {} unmatched, {} curved ({} "
      "patches)\n",
      map,
      evaluation.islands,
      evaluation.adopted,
      evaluation.unmatched,
      evaluation.curved,
      patches);
    for (const auto& result : evaluation.results)
    {
      if (
        result.match.verdict != IslandVerdict::Adopted
        && result.match.verdict != IslandVerdict::Curved)
      {
        std::cout << fmt::format(
          "  unmatched: {} {} island {}: {}\n",
          result.model,
          result.material,
          result.island,
          verdictName(result.match.verdict));
      }
    }
    CHECK(evaluation.unmatched == 0u);
    CHECK(evaluation.curved == patches);
    return std::pair{std::move(evaluation), tree};
  };

  SECTION("the doorway wall of the areas map: the lintel's dropped corners pass")
  {
    const auto [evaluation, tree] = evaluateMap("maps/test/litpreview_areas.map");
    auto found = false;
    for (const auto* result : evaluation.of("_area0", "textures/lp2/wall"))
    {
      const auto dropped = droppedCorners(*tree, *result);
      const auto lintel =
        std::ranges::any_of(
          dropped, [](const EngineVec3& p) { return p[1] == 144 && p[2] == 192; })
        && std::ranges::any_of(
          dropped, [](const EngineVec3& p) { return p[1] == 240 && p[2] == 192; });
      if (lintel)
      {
        found = true;
        CHECK(result->match.verdict == IslandVerdict::Adopted);
        std::cout << fmt::format(
          "LP7.4 areas _area0 lp2/wall: the doorway island adopted with {} faces, {} "
          "dropped corners\n",
          result->candidates.size(),
          dropped.size());
      }
    }
    CHECK(found);
  }

  SECTION("the stages map's white wall: its dropped corners pass")
  {
    const auto [evaluation, tree] = evaluateMap("maps/test/litpreview_stages.map");
    auto found = std::size_t{0};
    for (const auto& result : evaluation.results)
    {
      if (!sameName(result.material, "textures/lp5/white_wall"))
      {
        continue;
      }
      const auto dropped = droppedCorners(*tree, result);
      const auto corner = [&](const float y) {
        return std::ranges::any_of(
          dropped, [&](const EngineVec3& p) { return p[1] == y && p[2] == 16; });
      };
      if (corner(0) && corner(384))
      {
        ++found;
        CHECK(result.match.verdict == IslandVerdict::Adopted);
      }
    }
    std::cout << "LP7.4 stages lp5/white_wall: " << found
              << " islands with the dropped corners (0, 16) and (384, 16), adopted\n";
    CHECK(found > 0u);
  }

  SECTION("the parity map: area 21's two disjoint lp2/white islands, area 11's patch")
  {
    const auto [evaluation, tree] = evaluateMap("maps/test/litpreview_parity.map");
    const auto m = modelNamed(*tree, "_area21");
    REQUIRE(m < tree->models.size());
    const auto modelIslands = buildProcIslands(*tree).models[m];
    // the two islands of one material on the plane z 150 (P7.1 (b))
    auto white = std::vector<const IslandResult*>{};
    for (const auto* result : evaluation.of("_area21", "textures/lp2/white"))
    {
      const auto& island = modelIslands.surfaces[result->surface][result->island];
      if (
        std::abs(std::abs(island.plane.c) - 1.0f) < 1e-6f
        && std::abs(std::abs(island.plane.d) - 150.0f) < 1e-3f)
      {
        white.push_back(result);
      }
    }
    REQUIRE(white.size() == 2u);
    for (const auto* result : white)
    {
      CHECK(result->match.verdict == IslandVerdict::Adopted);
    }
    // each matched to its own faces, neither failing the other
    auto first = std::set<AdoptionPieceKey>{};
    for (const auto* piece : white[0]->candidates)
    {
      first.insert(piece->key);
    }
    for (const auto* piece : white[1]->candidates)
    {
      CHECK_FALSE(first.contains(piece->key));
    }
    std::cout << fmt::format(
      "LP7.4 parity _area21 lp2/white at z 150: two islands adopted with {} and {} "
      "faces\n",
      white[0]->candidates.size(),
      white[1]->candidates.size());
    const auto patch = evaluation.of("_area11", "textures/lp2/patch");
    REQUIRE(patch.size() == 1u);
    CHECK(patch.front()->match.verdict == IslandVerdict::Curved);
  }

  SECTION("the ambient map: lp4_g04_pair's islands, eight of them on four shared planes")
  {
    const auto [evaluation, tree] = evaluateMap("maps/test/litpreview_ambient.map");
    const auto m = modelNamed(*tree, "lp4_g04_pair");
    REQUIRE(m < tree->models.size());
    const auto modelIslands = buildProcIslands(*tree).models[m];
    auto perPlane = std::map<std::array<float, 4>, std::size_t>{};
    auto islands = std::size_t{0};
    for (const auto& result : evaluation.results)
    {
      if (result.model != "lp4_g04_pair")
      {
        continue;
      }
      ++islands;
      CHECK(result.match.verdict == IslandVerdict::Adopted);
      const auto& island = modelIslands.surfaces[result.surface][result.island];
      ++perPlane[{island.plane.a, island.plane.b, island.plane.c, island.plane.d}];
    }
    auto shared = std::size_t{0};
    auto onShared = std::size_t{0};
    for (const auto& [plane, count] : perPlane)
    {
      if (count > 1)
      {
        ++shared;
        onShared += count;
      }
    }
    // two boxes side by side: twelve faces, eight of them on the four planes they share
    CHECK(islands == 12u);
    CHECK(shared == 4u);
    CHECK(onShared == 8u);
    std::cout << fmt::format(
      "LP7.4 ambient lp4_g04_pair: {} islands adopted, {} on {} shared planes\n",
      islands,
      onShared,
      shared);
  }
}

} // namespace tb::lit
