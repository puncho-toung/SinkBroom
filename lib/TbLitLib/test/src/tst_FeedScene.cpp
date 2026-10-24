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

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/EngineGeometry.h"
#include "lit/FeedScene.h"
#include "lit/LightRecord.h"
#include "lit/MaterialService.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

// the feed fixture's materials, a light entityDef and the default light materials
std::unique_ptr<MaterialService> feedSceneService()
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{
        DeclFile{
          "materials/lp5_feed.mtr",
          readTextFile(litFixtureFolder() / "materials" / "lp5_feed.mtr"),
          0},
        DeclFile{
          "materials/zz_lp5_lights.mtr",
          "lights/defaultPointLight\n{\n\tlightFalloffImage _noFalloff\n\t{\n\t\tmap "
          "_white\n\t\tcolored\n\t}\n}\n"
          "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n\t{\n\t\tmap "
          "_white\n\t\tcolored\n\t}\n}\n",
          0},
        DeclFile{
          "def/lp5_feed.def", "entityDef light\n{\n\t\"spawnclass\" \"idLight\"\n}\n", 0},
      })};
  };
  options.pollInterval = 0ms;
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// collects until nothing is in flight and nothing is waited for, or 10 s
bool settled(MaterialService& service, SceneGeometry& scene)
{
  if (!service.waitForStatsForTesting([](const auto& s) { return s.idle; }, 10s))
  {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline)
  {
    scene.collect();
    if (
      !scene.busy() && !scene.waitsForMaterials() && !scene.waitsForEntityDefs()
      && !scene.lightTable()->records.empty())
    {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

// the triangles of every chunk of a material
std::vector<std::array<EngineVec3, 3>> trianglesOf(
  const SceneGeometry& scene, const std::string& material)
{
  auto triangles = std::vector<std::array<EngineVec3, 3>>{};
  for (const auto& [key, chunk] : scene.chunks())
  {
    if (key.material != material)
    {
      continue;
    }
    for (auto i = std::size_t{0}; i + 2 < chunk.indexes.size(); i += 3)
    {
      triangles.push_back(
        {chunk.verts[chunk.indexes[i]].xyz,
         chunk.verts[chunk.indexes[i + 1]].xyz,
         chunk.verts[chunk.indexes[i + 2]].xyz});
    }
  }
  return triangles;
}

vm::vec3d toVec(const EngineVec3& v)
{
  return {double(v[0]), double(v[1]), double(v[2])};
}

// the faces of a brush node
std::vector<SnapshotFace> facesOf(const Snapshot& snapshot, const PreviewId node)
{
  for (const auto& record : snapshot.nodes)
  {
    if (record.id == node && record.kind == SnapshotNodeKind::Brush)
    {
      return {
        snapshot.faces.begin() + record.elements.first,
        snapshot.faces.begin() + record.elements.first + record.elements.count};
    }
  }
  return {};
}

std::vector<vm::vec3d> polygonOf(const Snapshot& snapshot, const SnapshotFace& face)
{
  return {
    snapshot.vertices.begin() + face.polygon.first,
    snapshot.vertices.begin() + face.polygon.first + face.polygon.count};
}

// snapshotOfMapText of text on another thread, bounded: nullopt when it did not return
std::optional<Result<Snapshot>> boundedRead(const std::string& text)
{
  auto future =
    std::async(std::launch::async, [text] { return snapshotOfMapText(text, 3, 9); });
  if (future.wait_for(5s) != std::future_status::ready)
  {
    // the thread is left behind; the test fails
    return std::nullopt;
  }
  return future.get();
}

std::string errorOf(const std::optional<Result<Snapshot>>& result)
{
  return result && result->is_error() ? std::get<Error>(result->error()).msg
                                      : std::string{};
}

} // namespace

TEST_CASE("FeedScene: the record builders (LPF.2, D107, P5.11)")
{
  const auto centre = vm::vec3d{0, 0, 0};
  const auto sphereCentre = vm::vec3d{700, -300, 0};
  const auto lightColour = vm::vec3f{1.0f, 0.5f, 0.25f};
  auto snapshot = Snapshot{};
  snapshot.generation = 5;
  snapshot.sequence = 1;
  feed::worldspawn(snapshot, 1);
  REQUIRE(feed::quadBrush(
            snapshot,
            1,
            2,
            centre,
            {0, -1, 0},
            512,
            "lp5/feed_pbr",
            "lp5/feed_side",
            1.0,
            {512, 512})
            .is_success());
  REQUIRE(feed::patchDef3Sphere(snapshot, 1, 3, sphereCentre, 128, "lp5/feed_sphere")
            .is_success());
  feed::pointLight(snapshot, 4, {0, -300, 100}, 1000, lightColour);

  SECTION("the records")
  {
    REQUIRE(snapshot.entities.size() == 2u);
    CHECK(snapshot.string(snapshot.entities[0].classname) == "worldspawn");
    CHECK(snapshot.string(snapshot.entities[1].classname) == "light");
    REQUIRE(snapshot.nodes.size() == 3u);
    // a light is its own entity, as the editor writes a point entity
    CHECK(snapshot.nodes[2].id == 4u);
    CHECK(snapshot.nodes[2].entity == 4u);
    CHECK(snapshot.nodes[2].kind == SnapshotNodeKind::Light);
    const auto keys = snapshot.entities[1].keys;
    auto text = std::vector<std::string>{};
    for (auto k = keys.first; k < keys.first + keys.count; ++k)
    {
      text.push_back(
        std::string{snapshot.string(snapshot.keyValues[k].key)} + "="
        + std::string{snapshot.string(snapshot.keyValues[k].value)});
    }
    CHECK(
      text
      == std::vector<std::string>{
        "classname=light",
        "origin=0 -300 100",
        "light_radius=1000 1000 1000",
        "_color=1 0.5 0.25"});

    // the quad: one face toward the camera of the material, five of the other, Valve
    // axes as the editor writes them, (p3 - p1) x (p2 - p1) outward
    const auto faces = facesOf(snapshot, 2);
    REQUIRE(faces.size() == 6u);
    auto front = 0;
    for (const auto& face : faces)
    {
      const auto outward =
        vm::cross(face.points[2] - face.points[0], face.points[1] - face.points[0]);
      CHECK(vm::dot(outward, face.plane.normal) > 0.0);
      CHECK(polygonOf(snapshot, face).size() == 4u);
      if (snapshot.string(face.material) == "lp5/feed_pbr")
      {
        ++front;
        CHECK(face.plane.normal == vm::vec3d{0, -1, 0});
        CHECK(face.plane.distance == 0.0);
        CHECK(face.uAxis == vm::vec3f{1, 0, 0});
        CHECK(face.vAxis == vm::vec3f{0, 0, -1});
        CHECK(face.offset == vm::vec2f{512, 512});
        CHECK(face.scale == vm::vec2f{1, 1});
        // counter-clockwise seen from -y, the least vertex first
        CHECK(
          polygonOf(snapshot, face)
          == std::vector<vm::vec3d>{
            {-512, 0, -512}, {512, 0, -512}, {512, 0, 512}, {-512, 0, 512}});
      }
      else
      {
        CHECK(snapshot.string(face.material) == "lp5/feed_side");
      }
    }
    CHECK(front == 1);

    // the same brush through the map text's reader: the same planes and polygons
    const auto text2 = "{\n\"classname\" \"worldspawn\"\n"
                       + boxBrush(-512, 0, -512, 512, 16, 512, "x") + "}\n";
    const auto parsed = snapshotOfMapText(text2, 1, 1) | kdl::value();
    const auto parsedFaces = facesOf(parsed, 2);
    REQUIRE(parsedFaces.size() == 6u);
    for (const auto& face : faces)
    {
      const auto match = std::ranges::find_if(parsedFaces, [&](const auto& other) {
        return other.plane.normal == face.plane.normal;
      });
      REQUIRE(match != parsedFaces.end());
      CHECK(match->plane.distance == face.plane.distance);
      CHECK(polygonOf(parsed, *match) == polygonOf(snapshot, face));
      CHECK(match->uAxis == face.uAxis);
      CHECK(match->vAxis == face.vAxis);
    }

    // the sphere's control points: 9 × 17, the poles pinched
    REQUIRE(snapshot.patches.size() == 1u);
    const auto& patch = snapshot.patches[0];
    CHECK(patch.rows == 9u);
    CHECK(patch.columns == 17u);
    CHECK(patch.controlPoints.count == 153u);
    CHECK(patch.explicitSubdivisions);
    CHECK(patch.horizontalSubdivisions == 8u);
    CHECK(patch.verticalSubdivisions == 8u);
    for (auto c = std::size_t{0}; c < 17; ++c)
    {
      const auto& south = snapshot.controlPoints[patch.controlPoints.first + c];
      const auto& north = snapshot.controlPoints[patch.controlPoints.first + 8 * 17 + c];
      CHECK(
        vm::vec3d{south[0], south[1], south[2]} == sphereCentre - vm::vec3d{0, 0, 128});
      CHECK(
        vm::vec3d{north[0], north[1], north[2]} == sphereCentre + vm::vec3d{0, 0, 128});
    }
  }

  SECTION("the subdivided sphere: K19's bounds, the degenerate pole triangles")
  {
    // as ChunkBuilder::addPatch hands the patch to the copied subdivision
    const auto& patch = snapshot.patches[0];
    auto engine = EnginePatch{};
    engine.width = int(patch.rows);
    engine.height = int(patch.columns);
    engine.explicitSubdivisions = true;
    engine.horizontalSubdivisions = int(patch.horizontalSubdivisions);
    engine.verticalSubdivisions = int(patch.verticalSubdivisions);
    engine.controlPoints.resize(std::size_t(patch.rows) * patch.columns);
    for (auto row = std::size_t{0}; row < patch.rows; ++row)
    {
      for (auto column = std::size_t{0}; column < patch.columns; ++column)
      {
        const auto& p =
          snapshot
            .controlPoints[patch.controlPoints.first + row * patch.columns + column];
        auto vert = engineClearedVert();
        vert.xyz = {float(p[0]), float(p[1]), float(p[2])};
        engineSetTexCoord(vert, float(p[3]), float(p[4]));
        engine.controlPoints[column * patch.rows + row] = vert;
      }
    }
    auto error = std::string{};
    const auto surface = engineSubdividePatch(engine, &error);
    REQUIRE(surface);
    // (4 · 8 + 1) · (8 · 8 + 1) before welding
    CHECK(surface->verts.size() == 2145u);
    CHECK(surface->verts.size() <= std::size_t(engineBounds::MaxPatchVertices));
    auto degenerate = std::size_t{0};
    for (auto i = std::size_t{0}; i + 2 < surface->indexes.size(); i += 3)
    {
      const auto& a = surface->verts[std::size_t(surface->indexes[i])].xyz;
      const auto& b = surface->verts[std::size_t(surface->indexes[i + 1])].xyz;
      const auto& c = surface->verts[std::size_t(surface->indexes[i + 2])].xyz;
      degenerate += engineTriangleArea(a, b, c) <= 0 ? 1 : 0;
    }
    // the pole rows' triangles: one strip a pole, 64 columns of subdivisions
    CHECK(degenerate == 2u * 64u);
    std::cout << "LPF.2 sphere: " << surface->verts.size() << " vertices before welding, "
              << surface->indexes.size() / 3 << " triangles, " << degenerate
              << " degenerate\n";
  }

  SECTION("through SceneGeometry::takeSnapshot into chunks")
  {
    auto service = feedSceneService();
    auto pool = CpuPool::create() | kdl::value();
    {
      auto scene = SceneGeometry{pool.get(), service.get()};
      scene.takeSnapshot(snapshot);
      REQUIRE(settled(*service, scene));

      // a quad: 2 triangles of the material, 10 of the other
      const auto quad = trianglesOf(scene, "textures/lp5/feed_pbr");
      CHECK(quad.size() == 2u);
      CHECK(trianglesOf(scene, "textures/lp5/feed_side").size() == 10u);
      // the engine's winding as the brush's front face has it: the sense of
      // (b - a) x (c - a) against the face's outward normal (0 -1 0)
      const auto sense =
        [](const std::array<EngineVec3, 3>& triangle, const vm::vec3d& out) {
          const auto a = toVec(triangle[0]);
          return vm::dot(vm::cross(toVec(triangle[1]) - a, toVec(triangle[2]) - a), out)
                 > 0.0;
        };
      REQUIRE_FALSE(quad.empty());
      const auto frontSense = sense(quad[0], {0, -1, 0});
      CHECK(sense(quad[1], {0, -1, 0}) == frontSense);

      // the sphere: no degenerate triangle, every one facing out as the brush does
      const auto sphere = trianglesOf(scene, "textures/lp5/feed_sphere");
      CHECK(sphere.size() == 2u * 64u * 32u - 2u * 64u);
      auto outward = std::size_t{0};
      for (const auto& triangle : sphere)
      {
        CHECK(engineTriangleArea(triangle[0], triangle[1], triangle[2]) > 0);
        const auto middle =
          (toVec(triangle[0]) + toVec(triangle[1]) + toVec(triangle[2])) / 3.0;
        outward += sense(triangle, middle - sphereCentre) == frontSense ? 1 : 0;
      }
      CHECK(outward == sphere.size());

      // the light: its radius and colour
      const auto light = scene.lightTable()->find(4);
      REQUIRE(light);
      CHECK(light->classname == "light");
      CHECK(light->light.parms.pointLight);
      CHECK(light->light.parms.lightRadius == EngineVec3{1000, 1000, 1000});
      CHECK(light->light.parms.origin == EngineVec3{0, -300, 100});
      CHECK(light->light.parms.shaderParms[0] == lightColour.x());
      CHECK(light->light.parms.shaderParms[1] == lightColour.y());
      CHECK(light->light.parms.shaderParms[2] == lightColour.z());
    }
    pool->stop();
  }

  SECTION("what the builders refuse")
  {
    auto refused = Snapshot{};
    CHECK(feed::quadBrush(refused, 1, 2, centre, {0, 0.6, 0.8}, 64, "a", "b").is_error());
    CHECK(feed::quadBrush(refused, 1, 2, centre, {0, 0, 1}, 0, "a", "b").is_error());
    CHECK(feed::patchDef3Sphere(refused, 1, 3, centre, -1, "a").is_error());
    CHECK(feed::patchDef3Sphere(refused, 1, 3, centre, 64, "a", 65, 8).is_error());
    CHECK(feed::patchDef3Sphere(refused, 1, 3, centre, 64, "a", 0, 8).is_error());
    CHECK(refused.nodes.empty());
    CHECK(refused.faces.empty());
    CHECK(refused.patches.empty());
  }

  SECTION("the camera")
  {
    const auto camera = feed::cameraLookingAt({0, -100, 0}, {0, 0, 0}, 75.0f);
    CHECK(camera.position == vm::vec3f{0, -100, 0});
    CHECK(camera.direction == vm::vec3f{0, 1, 0});
    CHECK(camera.up == vm::vec3f{0, 0, 1});
    CHECK(camera.fovDegrees == 75.0f);
    const auto down = feed::cameraLookingAt({0, 0, 100}, {0, 0, 0});
    CHECK(down.direction == vm::vec3f{0, 0, -1});
    CHECK(vm::dot(down.up, down.direction) == 0.0f);
  }
}

TEST_CASE("snapshotOfMapText: bounded, and its own generation (LPF.2, D53, P5.11)")
{
  SECTION("the caller's generation and sequence, no map path unless given")
  {
    const auto text = "{\n\"classname\" \"worldspawn\"\n"
                      + boxBrush(0, 0, 0, 8, 8, 8, "a")
                      + "}\n{\n\"classname\" \"light\"\n\"origin\" \"1 2 3\"\n}\n";
    const auto snapshot = snapshotOfMapText(text, 7, 3) | kdl::value();
    CHECK(snapshot.generation == 7u);
    CHECK(snapshot.sequence == 3u);
    CHECK_FALSE(snapshot.mapPath);
    CHECK(snapshot.nodes.size() == 2u);
    const auto withPath = snapshotOfMapText(text, 7, 4, "maps/a.map") | kdl::value();
    CHECK(withPath.mapPath == "maps/a.map");
  }

  SECTION("an unterminated quote, block or number is an error, within a bound")
  {
    for (const auto& [text, reason] : std::vector<std::pair<std::string, std::string>>{
           {"{\n\"classname\" \"worldspawn\n}\n",
            "line 2: a quoted string is not closed"},
           {"{\n\"classname\" \"worldspawn\"\n{\n( 0 0 0 ) ( 0 1 0 ) ( 0 0 1 ) a [ 0 1 0 "
            "0 ] [ "
            "0 0 -1 0 ] 0 1 1\n",
            "unexpected end of the map"},
           {"{\n\"classname\" \"worldspawn\"\n{\n( 0 0 x ) ( 0 1 0 ) ( 0 0 1 ) a [ 0 1 0 "
            "0 ] [ "
            "0 0 -1 0 ] 0 1 1\n}\n}\n",
            "line 4: x is not a number the engine reads"},
           {"\"classname\" \"worldspawn\"\n", "line 1: expected an entity"},
           {"{\n\"classname\" \"worldspawn\"\n{\npatchDef3\n{\nm\n( 200 3 1 1 0 0 0 )\n",
            "line 7: a patch of 200 x 3 control points"},
           {"{\n\"classname\"", "unexpected end of the map"},
         })
    {
      CAPTURE(text);
      const auto result = boundedRead(text);
      REQUIRE(result);
      CHECK_THAT(errorOf(result), Catch::Matchers::ContainsSubstring(reason));
    }
    // the fixture's wrapper throws it
    CHECK_THROWS(snapshotOfMapText("{\n\"a"));
  }

  SECTION("a non-axial brush's polygon (the header's rule)")
  {
    // a 64 cube with its +x +z edge cut by x + z <= 96: the cut face's corners are
    // (64 0 32) (64 64 32) (32 64 64) (32 0 64), solved through planes whose normals hold
    // 1 / sqrt( 2 )
    auto box = boxBrush(0, 0, 0, 64, 64, 64, "a");
    box.insert(
      box.rfind('}'),
      "( 64 0 32 ) ( 32 0 64 ) ( 64 64 32 ) cut [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1\n");
    const auto snapshot =
      snapshotOfMapText("{\n\"classname\" \"worldspawn\"\n" + box + "}\n", 1, 1)
      | kdl::value();
    const auto faces = facesOf(snapshot, 2);
    REQUIRE(faces.size() == 7u);
    const auto& cut = faces[6];
    CHECK(snapshot.string(cut.material) == "cut");
    const auto polygon = polygonOf(snapshot, cut);
    // counter-clockwise seen from outside ((1 0 1)), the least vertex first
    const auto expected =
      std::vector<vm::vec3d>{{32, 0, 64}, {64, 0, 32}, {64, 64, 32}, {32, 64, 64}};
    REQUIRE(polygon.size() == expected.size());
    auto worst = 0.0;
    for (auto i = std::size_t{0}; i < expected.size(); ++i)
    {
      worst = std::max(worst, vm::length(polygon[i] - expected[i]));
    }
    CHECK(worst < 1e-9);
    // the +x face, cut too
    const auto side = std::ranges::find_if(
      faces, [](const auto& f) { return f.plane.normal == vm::vec3d{1, 0, 0}; });
    REQUIRE(side != faces.end());
    const auto sidePolygon = polygonOf(snapshot, *side);
    REQUIRE(sidePolygon.size() == 4u);
    for (const auto& p : sidePolygon)
    {
      CHECK(p.x() == 64.0);
      CHECK(p.z() <= 32.0 + 1e-9);
    }
    std::cout << "LPF.2 non-axial polygon: worst distance to the exact corners " << worst
              << "\n";
  }
}

} // namespace tb::lit
