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

// LP4.0 (C31, D82, K30): the area bounds of a .proc and a .bproc as the engine's
// FinishSurfaces and AddWorldModelEntities make them, and the areas' connectivity.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/AreaData.h"
#include "lit/BprocFile.h"
#include "lit/BprocModel.h"
#include "lit/GameFileSource.h"
#include "lit/MapBake.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::filesystem::path goldens()
{
  return litFixtureFolder() / "goldens" / "lp1b_geometry";
}

std::span<const std::byte> bytesOf(const std::string& text)
{
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

void drain()
{
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

ProcTree procOf(const std::string& text)
{
  auto tree =
    readProcFile(text) | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  drain();
  return tree;
}

// the fixture's materials by name (none has a deform)
AreaMaterialLookup fixtureLookup()
{
  static const auto table = [] {
    auto made =
      fixtureMaterialTable(listImagesSizes(readTextFile(goldens() / "listImages.txt")));
    drain();
    return made;
  }();
  return [](const std::string& name) -> std::shared_ptr<const MaterialEntry> {
    const auto slot = table->find(name);
    return slot ? slot->entry : nullptr;
  };
}

// a .proc with _area0 of two triangles, the second of the material deformed, an
// _area1 without surfaces, two areas split by the plane x = 0 and no portal
std::string deformProc(const std::string& deformed)
{
  return "mapProcFile004\n"
         "model { \"_area0\" 2\n"
         "{ \"textures/test/plain\" 3 3 0\n"
         "( 0 0 0 0 0 0 0 1 ) ( 64 0 0 0 0 0 0 1 ) ( 0 32 16 0 0 0 0 1 )\n"
         "0 1 2 }\n"
         "{ \""
         + deformed
         + "\" 3 3 0\n"
           "( 100 100 100 0 0 0 0 1 ) ( 104 100 100 0 0 0 0 1 ) ( 100 108 102 0 0 0 0 1 )\n"
           "0 1 2 }\n"
           "}\n"
           "model { \"_area1\" 0 }\n"
           "interAreaPortals { 2 0 }\n"
           "nodes { 1 ( 1 0 0 0 ) -1 -2 }\n";
}

// FinishSurfaces' growth by hand: mid ( 102 104 101 ), ( bounds[0] - mid ).Length() =
// idMath::Sqrt( 21 ) = 21 · sqrtf( 1 / 21 ), plus 20
EngineBounds deformedByHand()
{
  const auto radius = 21.0f * std::sqrt(1.0f / 21.0f) + 20.0f;
  return {
    EngineVec3{102.0f - radius, 104.0f - radius, 101.0f - radius},
    EngineVec3{102.0f + radius, 104.0f + radius, 101.0f + radius}};
}

} // namespace

TEST_CASE("AreaData: the fixture's .proc and .bproc give the same areas (C31, P4.30)")
{
  // the engine wrote the .bproc from its own parse of this .proc (RenderWorld_load.cpp:
  // 905-912): its stored model bounds are FinishSurfaces' own
  const auto proc = procOf(readTextFile(goldens() / "lp1b_geometry.proc"));
  const auto bytes = readTextFile(goldens() / "lp1b_geometry.bproc");
  const auto bproc = readBprocTree(bytesOf(bytes), true) | kdl::value();
  drain();

  const auto fromProc = buildAreaData(proc, fixtureLookup());
  const auto fromBproc = buildAreaData(bproc.tree);
  REQUIRE(fromProc.areas.size() == 2u);
  REQUIRE(fromBproc.areas.size() == 2u);
  for (auto i = std::size_t{0}; i < 2; ++i)
  {
    CAPTURE(i);
    CHECK(fromProc.areas[i].modelBounds == fromBproc.areas[i].modelBounds);
    CHECK(fromProc.areas[i].globalBounds == fromBproc.areas[i].globalBounds);
    CHECK(
      fromProc.areas[i].inverseBaseModelProject
      == fromBproc.areas[i].inverseBaseModelProject);
  }
  // the _areaN models' vertices: area 0 x 8..256 (the room east of the doorway's plane),
  // area 1 x -256..8 (the west room and the doorway), both y -128..128, z 0..128
  CHECK(
    fromProc.areas[0].modelBounds
    == EngineBounds{EngineVec3{8, -128, 0}, EngineVec3{256, 128, 128}});
  CHECK(
    fromProc.areas[1].modelBounds
    == EngineBounds{EngineVec3{-256, -128, 0}, EngineVec3{8, 128, 128}});
  // globalBounds: the unit cube through OffsetScaleForBounds and back with
  // ProjectedBounds, whose divide by w goes through _mm_rcp32_ps; P4.10's ulp does not
  // show for these whole numbers (the world entity's offset 132 0 64 and scale 124 128
  // 64 in area 0, offset -124 0 64 and scale 132 128 64 in area 1)
  for (const auto& area : fromProc.areas)
  {
    CHECK(area.globalBounds == area.modelBounds);
  }
  CHECK(
    fromProc.areas[0].inverseBaseModelProject
    == EngineMat4{124, 0, 0, 132, 0, 128, 0, 0, 0, 0, 64, 64, 0, 0, 0, 1});
  CHECK(
    fromProc.areas[1].inverseBaseModelProject
    == EngineMat4{132, 0, 0, -124, 0, 128, 0, 0, 0, 0, 64, 64, 0, 0, 0, 1});
  // commonChildrenAreas, one per node
  CHECK(fromProc.commonAreas.size() == proc.nodes.size());
  CHECK(fromProc.commonAreas == fromBproc.commonAreas);

  // every model's bounds, the brush entities' too, agree with the .bproc's stored ones
  REQUIRE(bproc.tree.models.size() == proc.models.size());
  for (auto m = std::size_t{0}; m < proc.models.size(); ++m)
  {
    CAPTURE(proc.models[m].name);
    CHECK_FALSE(proc.models[m].boundsKnown);
    CHECK(bproc.tree.models[m].boundsKnown);
    CHECK(
      procModelBounds(proc.models[m], fixtureLookup())
      == EngineBounds{bproc.tree.models[m].bounds[0], bproc.tree.models[m].bounds[1]});
  }
}

TEST_CASE("AreaData: FinishSurfaces' model bounds (C31, P4.10)")
{
  SECTION("a surface whose material has a deform grows to its cube")
  {
    const auto tree = procOf(deformProc("textures/test/sprite"));
    REQUIRE(tree.models.size() == 2u);
    // R_BoundTriSurf of each surface, kept without the geometry
    CHECK(
      tree.models[0].surfaces[0].bounds
      == std::array<EngineVec3, 2>{EngineVec3{0, 0, 0}, EngineVec3{64, 32, 16}});
    CHECK(
      tree.models[0].surfaces[1].bounds
      == std::array<EngineVec3, 2>{EngineVec3{100, 100, 100}, EngineVec3{104, 108, 102}});
    CHECK(tree.models[0].surfaces[1].verts.empty());

    // a MaterialEntry made here: DFRM_SPRITE for the second surface's material
    auto sprite = std::make_shared<MaterialEntry>();
    sprite->name = "textures/test/sprite";
    sprite->deform = 1;
    auto asked = std::map<std::string, int>{};
    const auto lookup =
      [&](const std::string& name) -> std::shared_ptr<const MaterialEntry> {
      ++asked[name];
      return name == sprite->name ? sprite : nullptr;
    };
    const auto data = buildAreaData(tree, lookup);
    REQUIRE(data.areas.size() == 2u);
    const auto grown = deformedByHand();
    CHECK(
      engineDeformSurfaceBounds({EngineVec3{100, 100, 100}, EngineVec3{104, 108, 102}})
      == grown);
    // the union: the plain surface's minimum (0 0 0) is below the grown cube's (77.4
    // 79.4 76.4), whose maximum (126.6 128.6 125.6) is above the plain one's
    CHECK(data.areas[0].modelBounds == EngineBounds{EngineVec3{0, 0, 0}, grown[1]});
    // each material asked once
    CHECK(
      asked
      == std::map<std::string, int>{
        {"textures/test/plain", 1}, {"textures/test/sprite", 1}});

    // without the deform the union is of the vertices alone
    const auto plain = buildAreaData(tree);
    CHECK(
      plain.areas[0].modelBounds
      == EngineBounds{EngineVec3{0, 0, 0}, EngineVec3{104, 108, 102}});
  }

  SECTION("an area whose model has no surface is Zero(), a point at the origin")
  {
    const auto data = buildAreaData(procOf(deformProc("textures/test/plain")));
    REQUIRE(data.areas.size() == 2u);
    CHECK(data.areas[1].modelBounds == engineZeroBounds());
    CHECK_FALSE(engineBoundsIsCleared(data.areas[1].modelBounds));
    for (const auto& corner : data.areas[1].globalBounds)
    {
      CHECK(corner == EngineVec3{0, 0, 0});
    }
  }

  SECTION("an area without a model is Zero() too (a guard: the engine stops the load)")
  {
    const auto data = buildAreaData(procOf(
      "mapProcFile004\ninterAreaPortals { 2 0 }\nnodes { 1 ( 1 0 0 0 ) -1 -2 }\n"));
    REQUIRE(data.areas.size() == 2u);
    CHECK(data.areas[0].modelBounds == engineZeroBounds());
    CHECK(data.areas[1].modelBounds == engineZeroBounds());
  }

  SECTION("a surface without vertices has cleared bounds, which add nothing")
  {
    const auto tree = procOf(
      "mapProcFile004\nmodel { \"_area0\" 2\n{ \"a\" 0 0 0 }\n"
      "{ \"b\" 3 3 0 ( 1 2 3 0 0 0 0 1 ) ( 4 5 6 0 0 0 0 1 ) ( 1 5 3 0 0 0 0 1 ) 0 1 2 "
      "}\n"
      "}\ninterAreaPortals { 1 0 }\nnodes { 1 ( 1 0 0 0 ) -1 -1 }\n");
    CHECK(engineBoundsIsCleared(tree.models[0].surfaces[0].bounds));
    const auto data = buildAreaData(tree);
    CHECK(
      data.areas[0].modelBounds
      == EngineBounds{EngineVec3{1, 2, 3}, EngineVec3{4, 5, 6}});
  }
}

TEST_CASE("AreaData: the .bproc model's stored bounds (C31, Model.cpp:607-608)")
{
  // the new read replaces the 24-byte skip: every entry after it still reads
  const auto bytes = readTextFile(goldens() / "lp1b_geometry.bproc");
  const auto file = readBprocFile(bytesOf(bytes)) | kdl::value();
  drain();
  REQUIRE(file.models.size() == 4u);
  REQUIRE(file.areas.nodes.size() == 39u);
  auto bounds = std::map<std::string, std::array<EngineVec3, 2>>{};
  for (const auto& model : file.models)
  {
    bounds[model.name] = model.bounds;
  }
  CHECK(
    bounds.at("_area0")
    == std::array<EngineVec3, 2>{EngineVec3{8, -128, 0}, EngineVec3{256, 128, 128}});
  CHECK(
    bounds.at("_area1")
    == std::array<EngineVec3, 2>{EngineVec3{-256, -128, 0}, EngineVec3{8, 128, 128}});
  // the rotator's vertices are relative to its origin brush's pivot (R3.6)
  CHECK(
    bounds.at("lp1b_rotator")
    == std::array<EngineVec3, 2>{EngineVec3{-28, -28, -4}, EngineVec3{4, 4, 28}});
  CHECK(
    bounds.at("lp1b_scaled")
    == std::array<EngineVec3, 2>{EngineVec3{-16, -16, -16}, EngineVec3{16, 16, 16}});
  // the surfaces' stored bounds lie inside the model's
  for (const auto& model : file.models)
  {
    for (const auto& surface : model.surfaces)
    {
      CHECK(surface.geometry);
      for (auto k = std::size_t{0}; k < 3; ++k)
      {
        CHECK(surface.bounds[0][k] >= model.bounds[0][k]);
        CHECK(surface.bounds[1][k] <= model.bounds[1][k]);
      }
    }
  }
}

TEST_CASE("AreaData, the bake read asks the registry for the deform (C31, C20)")
{
  using namespace std::chrono_literals;
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  writeTextFile(
    base / "materials" / "lp4_area.mtr",
    "textures/test/sprite\n{\n\tdeform sprite\n\t{\n\t\tblend add\n\t\tmap "
    "_white\n\t}\n}\n");
  writeTextFile(base / "maps" / "lp4_area.proc", deformProc("textures/test/sprite"));
  writeTextFile(base / "maps" / "lp4_plain.proc", deformProc("textures/test/unknown"));

  auto options = MaterialServiceOptions{};
  options.makeSource = [path = env.dir()](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  auto service = MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  const auto path = (base / "maps" / "lp4_area.map").string();
  const auto plainPath = (base / "maps" / "lp4_plain.map").string();
  static_cast<void>(service->loadMapBake(path));
  static_cast<void>(service->loadMapBake(plainPath));
  const auto table = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto a = t.findMapBake(path);
      const auto b = t.findMapBake(plainPath);
      return a && a->bake && b && b->bake;
    },
    10s);
  REQUIRE(table);

  // the decl's deform sprite, through MaterialDecls::lookup on the parse thread
  const auto bake = table->findMapBake(path)->bake;
  REQUIRE(bake->areas);
  REQUIRE(bake->areas->areas.size() == 2u);
  const auto grown = deformedByHand();
  CHECK(bake->areas->areas[0].modelBounds[1] == grown[1]);
  CHECK(bake->areas->areas[1].modelBounds == engineZeroBounds());

  // a name without a decl is an implicit material: no deform
  const auto plain = table->findMapBake(plainPath)->bake;
  REQUIRE(plain->areas);
  CHECK(
    plain->areas->areas[0].modelBounds
    == EngineBounds{EngineVec3{0, 0, 0}, EngineVec3{104, 108, 102}});
}

TEST_CASE("AreaData: the portal sides and the connected areas (C31, D82, K30, P4.17)")
{
  SECTION("the fixture: one portal, both sides, one component of two areas")
  {
    const auto proc = procOf(readTextFile(goldens() / "lp1b_geometry.proc"));
    const auto data = buildAreaData(proc);
    REQUIRE(data.areas.size() == 2u);
    // iap 0: 4 points, positive area 0, negative area 1, the doorway's plane x = 8
    REQUIRE(data.areas[0].portals.size() == 1u);
    REQUIRE(data.areas[1].portals.size() == 1u);
    const auto& front = data.areas[0].portals[0];
    CHECK(front.portal == 0u);
    CHECK(front.side == 0);
    CHECK(front.intoArea == 1);
    CHECK(
      front.points
      == std::vector<EngineVec3>{{8, -32, 0}, {8, -32, 96}, {8, 32, 96}, {8, 32, 0}});
    // GetPlane: ( p1 - centre ) x ( p0 - centre ) normalised, through p0: the positive
    // side is area 0's, x > 8
    CHECK(front.plane.a == 1.0f);
    CHECK(front.plane.b == 0.0f);
    CHECK(front.plane.c == 0.0f);
    CHECK(front.plane.d == -8.0f);
    const auto& back = data.areas[1].portals[0];
    CHECK(back.side == 1);
    CHECK(back.intoArea == 0);
    // Reverse(): point i at n - 1 - i
    CHECK(
      back.points
      == std::vector<EngineVec3>{{8, 32, 0}, {8, 32, 96}, {8, -32, 96}, {8, -32, 0}});
    CHECK(back.plane.a == -1.0f);
    CHECK(back.plane.d == 8.0f);

    CHECK(data.components == std::vector<int>{0, 0});
    CHECK(connectedComponents(proc) == data.components);
    CHECK(areasConnected(data.components, 0, 1));
    CHECK(areasConnected(data.components, 1, 1));
  }

  SECTION("three areas and one portal: two components")
  {
    auto tree = ProcTree{};
    tree.numAreas = 3;
    tree.portals.push_back(
      ProcPortal{0, 1, {{0, -32, 0}, {0, -32, 64}, {0, 32, 64}, {0, 32, 0}}});
    const auto components = connectedComponents(tree);
    CHECK(components == std::vector<int>{0, 0, 1});
    CHECK(areasConnected(components, 0, 1));
    CHECK_FALSE(areasConnected(components, 0, 2));
    CHECK_FALSE(areasConnected(components, 1, 2));
    CHECK(areasConnected(components, 2, 2));
    // AreasAreConnected: -1 is connected to nothing, itself included
    for (const auto other : {-1, 0, 1, 2})
    {
      CHECK_FALSE(areasConnected(components, -1, other));
      CHECK_FALSE(areasConnected(components, other, -1));
    }
    // an area past the count (the engine's Error) is not connected either
    CHECK_FALSE(areasConnected(components, 0, 3));
  }

  SECTION("an area's list holds its portals in the .proc's order reversed")
  {
    // area 1 between areas 0 and 2: portal 0 to area 0 (area 1 negative), portal 1 to
    // area 2 (area 1 positive)
    auto tree = ProcTree{};
    tree.numAreas = 3;
    tree.portals.push_back(
      ProcPortal{0, 1, {{0, -32, 0}, {0, -32, 64}, {0, 32, 64}, {0, 32, 0}}});
    tree.portals.push_back(
      ProcPortal{1, 2, {{-64, 32, 0}, {-64, 32, 64}, {-64, -32, 64}, {-64, -32, 0}}});
    const auto sides = areaPortalSides(tree);
    REQUIRE(sides.size() == 3u);
    REQUIRE(sides[1].size() == 2u);
    CHECK(sides[1][0].portal == 1u);
    CHECK(sides[1][0].side == 0);
    CHECK(sides[1][0].intoArea == 2);
    CHECK(sides[1][1].portal == 0u);
    CHECK(sides[1][1].side == 1);
    CHECK(sides[1][1].intoArea == 0);
    CHECK(connectedComponents(tree) == std::vector<int>{0, 0, 0});
  }
}

} // namespace tb::lit
