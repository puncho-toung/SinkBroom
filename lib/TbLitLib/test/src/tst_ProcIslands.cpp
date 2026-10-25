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
#include "lit/EngineGeometry.h"
#include "lit/MaterialDecls.h"
#include "lit/ProcFile.h"
#include "lit/ProcIslands.h"

#include "kd/result.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

} // namespace tb::lit
