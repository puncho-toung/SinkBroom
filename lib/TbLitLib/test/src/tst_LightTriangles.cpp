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

// DX.2 (31-LP2-FIXES.md): the static interactions' light triangles: the copied cull
// (engineInteractionLightTris), its runs and the render thread's cache of them.

#include "LightTriangles.h"
#include "LitTestUtils.h"
#include "lit/CpuPool.h"
#include "lit/EngineLight.h"

#include "kd/result.h"

#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

constexpr auto IdentityAxis = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};

// a point light at the origin, its volume [-100, 100] on every axis
EngineMat4 pointLightProject()
{
  auto light = EngineLightGeometry{};
  light.lightRadius = {100, 100, 100};
  light.pointLight = true;
  return engineDeriveLightProjection(light).baseLightProject;
}

// a quad in the plane x = x0, y and z in [-50, 50], as two triangles
void addQuadAtX(
  std::vector<EngineVec3>& positions, std::vector<std::uint16_t>& indexes, const float x0)
{
  const auto first = std::uint16_t(positions.size());
  positions.push_back({x0, -50, -50});
  positions.push_back({x0, 50, -50});
  positions.push_back({x0, 50, 50});
  positions.push_back({x0, -50, 50});
  for (const auto i : {0, 1, 2, 0, 2, 3})
  {
    indexes.push_back(std::uint16_t(first + i));
  }
}

EngineBounds boundsOf(const std::vector<EngineVec3>& positions)
{
  auto bounds = EngineBounds{positions.front(), positions.front()};
  for (const auto& p : positions)
  {
    for (std::size_t i = 0; i < 3; ++i)
    {
      bounds[0][i] = std::min(bounds[0][i], p[i]);
      bounds[1][i] = std::max(bounds[1][i], p[i]);
    }
  }
  return bounds;
}

std::vector<std::uint16_t> cull(
  const EngineMat4& project,
  const EngineGlMatrix& modelMatrix,
  const std::vector<EngineVec3>& positions,
  const std::vector<std::uint16_t>& indexes)
{
  return engineInteractionLightTris(
    project, modelMatrix, boundsOf(positions), positions, indexes);
}

Chunk chunkOf(
  const std::vector<EngineVec3>& positions, std::vector<std::uint16_t> indexes)
{
  auto chunk = Chunk{};
  chunk.key.entity = 7;
  chunk.key.material = "textures/lp2/wall";
  for (const auto& p : positions)
  {
    auto vert = EngineDrawVert{};
    vert.xyz = p;
    chunk.verts.push_back(vert);
  }
  chunk.indexes = std::move(indexes);
  chunk.bounds = boundsOf(positions);
  chunk.casterVersion = 1;
  return chunk;
}

} // namespace

TEST_CASE(
  "LightTriangles: the copied cull (R_CalcInteractionCullBits, LIGHT_CLIP_EPSILON)")
{
  const auto project = pointLightProject();
  const auto identity = engineModelMatrix({0, 0, 0}, IdentityAxis);

  SECTION(
    "a face on the volume's plane, 0.05 inside and beyond it goes; 0.15 inside stays")
  {
    for (const auto& [x, kept] : std::vector<std::pair<float, bool>>{
           {100.0f, false},
           {99.95f, false},
           {99.85f, true},
           {150.0f, false},
           {0.0f, true},
           {-99.95f, false},
           {-99.85f, true}})
    {
      CAPTURE(x);
      auto positions = std::vector<EngineVec3>{};
      auto indexes = std::vector<std::uint16_t>{};
      addQuadAtX(positions, indexes, x);
      const auto result = cull(project, identity, positions, indexes);
      CHECK(result.size() == (kept ? 6u : 0u));
    }
  }

  SECTION("a surface keeps its kept triangles in their order")
  {
    auto positions = std::vector<EngineVec3>{};
    auto indexes = std::vector<std::uint16_t>{};
    addQuadAtX(positions, indexes, 100.0f);
    addQuadAtX(positions, indexes, 20.0f);
    addQuadAtX(positions, indexes, 99.95f);
    addQuadAtX(positions, indexes, 99.85f);
    const auto result = cull(project, identity, positions, indexes);
    CHECK(result == std::vector<std::uint16_t>{4, 5, 6, 4, 6, 7, 12, 13, 14, 12, 14, 15});
    const auto triangles = lightTrianglesOf(indexes, result);
    CHECK(triangles.state == LightTriangles::State::Runs);
    CHECK(triangles.culledTriangles == 4u);
    CHECK(triangles.runs == std::vector<IndexRun>{{6, 6}, {18, 6}});
  }

  SECTION("a scaled entity's planes are not normalised again (R_GlobalPlaneToLocal)")
  {
    // an entity scaled by 2: the local plane's normal is twice the world's, so its
    // distance is the world's and LIGHT_CLIP_EPSILON stays 0.1 world units (normalised
    // again it would be 0.2): local x 49.925 is world x 99.85, 0.15 inside, and stays;
    // local x 49.97 is world x 99.94, 0.06 inside, and goes
    const auto scaled = engineModelMatrix({0, 0, 0}, {2, 0, 0, 0, 2, 0, 0, 0, 2});
    for (const auto& [localX, kept] :
         std::vector<std::pair<float, bool>>{{49.925f, true}, {49.97f, false}})
    {
      CAPTURE(localX);
      auto positions = std::vector<EngineVec3>{};
      auto indexes = std::vector<std::uint16_t>{};
      addQuadAtX(positions, indexes, localX);
      for (auto& p : positions)
      {
        p[1] *= 0.5f;
        p[2] *= 0.5f;
      }
      CHECK(cull(project, scaled, positions, indexes).size() == (kept ? 6u : 0u));
    }
  }

  SECTION("an entity's origin moves the volume into its space")
  {
    // the entity at x 50: local x 50 is world x 100, on the plane
    const auto moved = engineModelMatrix({50, 0, 0}, IdentityAxis);
    auto positions = std::vector<EngineVec3>{};
    auto indexes = std::vector<std::uint16_t>{};
    addQuadAtX(positions, indexes, 50.0f);
    addQuadAtX(positions, indexes, 40.0f);
    CHECK(
      cull(project, moved, positions, indexes)
      == std::vector<std::uint16_t>{4, 5, 6, 4, 6, 7});
  }

  SECTION("a projected light's near and far planes")
  {
    // aimed down +x from the origin: the falloff runs from light_start 8 (its zNear,
    // SPOT_LIGHT_MIN_Z_NEAR) to light_end 200 (its zFar): the near plane at x = zNear,
    // the far one at x = zNear + zFar (R_ComputeSpotLightProjectionMatrix's zScale)
    auto light = EngineLightGeometry{};
    light.target = {200, 0, 0};
    light.right = {0, -100, 0};
    light.up = {0, 0, 100};
    light.start = {8, 0, 0};
    light.end = {200, 0, 0};
    const auto spot = engineDeriveLightProjection(light).baseLightProject;
    for (const auto& [x, kept] : std::vector<std::pair<float, bool>>{
           {208.0f, false},
           {207.95f, false},
           {207.5f, true},
           {8.0f, false},
           {9.0f, true}})
    {
      CAPTURE(x);
      auto positions = std::vector<EngineVec3>{};
      auto indexes = std::vector<std::uint16_t>{};
      // small quads near the axis, inside the frustum's sides
      addQuadAtX(positions, indexes, x);
      for (auto& p : positions)
      {
        p[1] *= 0.02f;
        p[2] *= 0.02f;
      }
      CHECK(cull(spot, identity, positions, indexes).size() == (kept ? 6u : 0u));
    }
  }
}

TEST_CASE("LightTriangles: the runs of the kept triangles")
{
  const auto all = std::vector<std::uint16_t>{0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
  CHECK(lightTrianglesOf(all, all).state == LightTriangles::State::All);
  const auto none = lightTrianglesOf(all, {});
  CHECK(none.state == LightTriangles::State::None);
  CHECK(none.culledTriangles == 4u);
  const auto first = lightTrianglesOf(all, {0, 1, 2, 2, 1, 3});
  CHECK(first.state == LightTriangles::State::Runs);
  CHECK(first.runs == std::vector<IndexRun>{{0, 6}});
  const auto apart = lightTrianglesOf(all, {0, 1, 2, 4, 5, 6});
  CHECK(apart.runs == std::vector<IndexRun>{{0, 3}, {6, 3}});
  CHECK(apart.culledTriangles == 2u);
  // a repeated triangle: each kept one is the next equal one
  const auto repeated = std::vector<std::uint16_t>{0, 1, 2, 0, 1, 2, 3, 4, 5};
  CHECK(
    lightTrianglesOf(repeated, {0, 1, 2, 0, 1, 2}).runs == std::vector<IndexRun>{{0, 6}});
}

TEST_CASE("LightTriangleCache")
{
  const auto project = pointLightProject();
  const auto identity = engineModelMatrix({0, 0, 0}, IdentityAxis);
  auto positions = std::vector<EngineVec3>{};
  auto indexes = std::vector<std::uint16_t>{};
  addQuadAtX(positions, indexes, 100.0f);
  addQuadAtX(positions, indexes, 20.0f);
  const auto chunk = chunkOf(positions, indexes);

  SECTION("a miss is Pending until its build ends; the answer is kept per version")
  {
    auto cache = LightTriangleCache{};
    cache.beginBuild();
    auto answer = cache.triangles(1, 1, project, chunk, identity);
    CHECK(answer->state == LightTriangles::State::Pending);
    CHECK(cache.busy());
    cache.endBuild(nullptr);
    CHECK_FALSE(cache.busy());
    CHECK(cache.version() == 1u);

    cache.beginBuild();
    answer = cache.triangles(1, 1, project, chunk, identity);
    cache.endBuild(nullptr);
    CHECK(answer->state == LightTriangles::State::Runs);
    CHECK(answer->runs == std::vector<IndexRun>{{6, 6}});
    CHECK(cache.version() == 1u);

    // a new light version, a new chunk version, another model matrix: asked again
    for (const auto& [lightVersion, casterVersion, matrix] :
         {std::tuple{2u, 1u, identity},
          std::tuple{2u, 2u, identity},
          std::tuple{2u, 2u, engineModelMatrix({-50, 0, 0}, IdentityAxis)}})
    {
      auto changed = chunk;
      changed.casterVersion = casterVersion;
      cache.beginBuild();
      CHECK(
        cache.triangles(1, lightVersion, project, changed, matrix)->state
        == LightTriangles::State::Pending);
      cache.endBuild(nullptr);
    }
    // the last one: the entity moved by -50, both quads inside the volume
    cache.beginBuild();
    auto moved = chunk;
    moved.casterVersion = 2;
    CHECK(
      cache.triangles(1, 2, project, moved, engineModelMatrix({-50, 0, 0}, IdentityAxis))
        ->state
      == LightTriangles::State::All);
    cache.endBuild(nullptr);

    // a build that no longer asks for a pair keeps it (a camera move away and back asks
    // nothing again), until the cache passes its bound
    CHECK(cache.size() == 1u);
    cache.beginBuild();
    cache.endBuild(nullptr);
    CHECK(cache.size() == 1u);
    cache.setCapacityForTesting(1);
    cache.beginBuild();
    auto another = chunk;
    another.key.material = "textures/lp2/other";
    cache.triangles(1, 2, project, another, identity);
    cache.endBuild(nullptr);
    CHECK(cache.size() == 1u);
  }

#if defined(_MSC_VER) && defined(_DEBUG)
  SECTION("V2.5: N builds at a small capacity keep the heap where it was")
  {
    // two chunks asked for in turn by two lights, one pair kept beyond the bound: every
    // build asks for misses again, so entries, requests and the shared copies churn
    auto pool = CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); })
                | kdl::value();
    auto cache = LightTriangleCache{};
    cache.setCapacityForTesting(1);
    auto other = chunk;
    other.key.material = "textures/lp2/floor";
    auto version = std::uint64_t{0};
    const auto build = [&](const Chunk& c) {
      ++version;
      cache.beginBuild();
      cache.triangles(1, version, project, c, identity);
      cache.triangles(2, version, project, c, identity);
      cache.endBuild(pool.get());
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      while (!cache.collect() && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      cache.beginBuild();
      cache.triangles(1, version, project, c, identity);
      cache.triangles(2, version, project, c, identity);
      cache.endBuild(pool.get());
    };
    build(chunk);
    build(other);
    CHECK_FALSE(heapGrows([&] {
      for (auto i = 0; i < 50; ++i)
      {
        build(chunk);
        build(other);
      }
    }));
    CHECK(cache.size() == 2u);
    CHECK(cache.jobs() == 102u);
    pool->stop();
  }
#endif

  SECTION("on the CPU pool: one job, its answers taken by collect")
  {
    auto pool = CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); })
                | kdl::value();
    auto cache = LightTriangleCache{};
    cache.beginBuild();
    CHECK(
      cache.triangles(1, 1, project, chunk, identity)->state
      == LightTriangles::State::Pending);
    auto other = chunk;
    other.key.material = "textures/lp2/floor";
    CHECK(
      cache.triangles(2, 1, project, other, identity)->state
      == LightTriangles::State::Pending);
    cache.endBuild(pool.get());
    CHECK(cache.jobs() == 1u);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!cache.collect() && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK_FALSE(cache.busy());
    CHECK(cache.answered() == 2u);
    cache.beginBuild();
    CHECK(
      cache.triangles(1, 1, project, chunk, identity)->state
      == LightTriangles::State::Runs);
    // asked while nothing changed: no second job
    cache.endBuild(pool.get());
    CHECK(cache.jobs() == 1u);
    pool->stop();
  }
}

} // namespace tb::lit
