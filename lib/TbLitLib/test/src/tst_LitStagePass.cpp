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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_main.cpp and
// renderer/RenderBackend.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "LitFrontend.h"
#include "LitStages.h"
#include "LitView.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineLight.h"
#include "lit/LitSession.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// a chunk of one axis-aligned box of bounds (min, max), keyed by material and part
Chunk boxChunk(
  const std::string& material,
  const EngineVec3& min,
  const EngineVec3& max,
  const int part = 0,
  const int area = 0)
{
  auto chunk = Chunk{};
  chunk.key.area = area;
  chunk.key.material = material;
  chunk.key.part = part;
  chunk.bounds = {min, max};
  return chunk;
}

std::shared_ptr<const MaterialEntry> sortedMaterial(const std::string& name, float sort)
{
  auto material = std::make_shared<MaterialEntry>();
  material->name = name;
  material->sort = sort;
  return material;
}

// the engine's R_SortDrawSurfs (tr_frontend_main.cpp:255-359) transcribed on its own: the
// 64-bit key of each surface in creation order, sorted descending (the engine's quicksort
// and selection sort leave the unique keys descending), and the surfaces read back from
// the low 16 bits
std::vector<const LitSurface*> engineOrderModel(
  const std::vector<const LitSurface*>& created, const std::vector<LitSpace>& spaces)
{
  const auto n = created.size();
  auto indices = std::vector<std::uint64_t>(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    const auto* surface = created[i];
    float sort = 100.0f - (surface->material ? surface->material->sort : 0.0f);
    auto dist = std::uint64_t{0};
    const auto depth =
      engineDepthBoundsForBounds(spaces[surface->space].mvp, surface->chunk->bounds);
    // idMath::Ftoui16: the truncation, clamped to 0..65535
    const auto i32 = int(depth[0] * 0xFFFF);
    dist = std::uint64_t(i32 < 0 ? 0 : i32 > 65535 ? 65535 : i32);
    auto bits = std::uint32_t{0};
    std::memcpy(&bits, &sort, sizeof(bits));
    indices[i] =
      ((std::uint64_t(n - i)) & 0xFFFF) | (dist << 16) | (std::uint64_t(bits) << 32);
  }
  std::sort(indices.begin(), indices.end(), std::greater<std::uint64_t>{});
  auto order = std::vector<const LitSurface*>{};
  for (const auto key : indices)
  {
    order.push_back(created[n - std::size_t(key & 0xFFFF)]);
  }
  return order;
}

} // namespace

TEST_CASE("LitStagePass: the stage pass's sorted list (LP5.2a, C46, D115, K37)")
{
  // a view from the origin looking down +x
  auto camera = CameraState{};
  camera.position = vm::vec3f{0, 0, 0};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.viewportWidth = 640;
  camera.viewportHeight = 480;
  const auto view = litViewOf(camera, 640, 480);
  const auto spaces = std::vector<LitSpace>{litSpaceOf(
    view,
    NoPreviewId,
    EngineVec3{0, 0, 0},
    EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1},
    engineWorldShaderParms())};

  // the chunks in ChunkKey order (the order buildFrontendLists makes surfaces in)
  auto chunks = std::map<ChunkKey, Chunk>{};
  const auto add = [&](Chunk chunk) {
    const auto key = chunk.key;
    chunks.emplace(key, std::move(chunk));
  };
  // a slab in front of the camera at x .. x + 4
  const auto slab = [](const float x) {
    return std::pair{EngineVec3{x, -32, -32}, EngineVec3{x + 4, 32, 32}};
  };
  const auto [nearMin, nearMax] = slab(50);
  const auto [midMin, midMax] = slab(120);
  const auto [farMin, farMax] = slab(300);
  add(boxChunk("a_opaque", midMin, midMax));
  add(boxChunk("b_medium_near", nearMin, nearMax));
  add(boxChunk("c_medium_far", farMin, farMax));
  // equal bounds and sort: creation (ChunkKey) order, the material name first, then the
  // part
  add(boxChunk("d_medium_tie", midMin, midMax, 1));
  add(boxChunk("d_medium_tie", midMin, midMax, 0));
  add(boxChunk("e_decal", nearMin, nearMax));
  add(boxChunk("f_post", nearMin, nearMax));
  add(boxChunk("g_subview", farMin, farMax));
  // a sort above SS_POST_PROCESS: its key is a negative float, whose bits sort first
  add(boxChunk("h_beyond", farMin, farMax));
  // a pending material sorts as SS_OPAQUE
  add(boxChunk("i_pending", farMin, farMax));
  // across the eye: a corner behind it gives the near depth 0, the nearest key
  add(boxChunk("j_medium_across", EngineVec3{-16, -8, -8}, EngineVec3{200, 8, 8}));

  const auto sorts = std::map<std::string, float>{
    {"a_opaque", 0.0f},
    {"b_medium_near", 4.0f},
    {"c_medium_far", 4.0f},
    {"d_medium_tie", 4.0f},
    {"e_decal", 2.0f},
    {"f_post", 100.0f},
    {"g_subview", -3.0f},
    {"h_beyond", 150.0f},
    {"j_medium_across", 4.0f},
  };
  auto drawSurfs = std::vector<LitSurface>{};
  for (const auto& [key, chunk] : chunks)
  {
    auto surface = LitSurface{};
    surface.chunk = &chunk;
    surface.space = 0;
    if (const auto it = sorts.find(key.material); it != sorts.end())
    {
      surface.material = sortedMaterial(key.material, it->second);
    }
    drawSurfs.push_back(surface);
  }

  const auto order = stagePassOrder(drawSurfs, spaces);
  REQUIRE(order.size() == drawSurfs.size());

  SECTION("the levels: the sort value, then the farthest first, then ChunkKey's order")
  {
    auto names = std::vector<std::string>{};
    for (const auto* surface : order)
    {
      names.push_back(
        surface->chunk->key.material + "#" + std::to_string(surface->chunk->key.part));
    }
    CHECK(
      names
      == std::vector<std::string>{
        "h_beyond#0",
        "g_subview#0",
        "i_pending#0",
        "a_opaque#0",
        "e_decal#0",
        "c_medium_far#0",
        "d_medium_tie#0",
        "d_medium_tie#1",
        "b_medium_near#0",
        "j_medium_across#0",
        "f_post#0",
      });
  }

  SECTION("the engine's key arithmetic gives the same order")
  {
    auto created = std::vector<const LitSurface*>{};
    for (const auto& surface : drawSurfs)
    {
      created.push_back(&surface);
    }
    CHECK(engineOrderModel(created, spaces) == order);
  }

  SECTION("the near depth: farther slabs have larger keys, one across the eye 0")
  {
    const auto depthOf = [&](const EngineVec3& min, const EngineVec3& max) {
      return engineFtoui16(
        engineDepthBoundsForBounds(spaces[0].mvp, EngineBounds{min, max})[0]
        * float(0xFFFF));
    };
    CHECK(depthOf(nearMin, nearMax) < depthOf(midMin, midMax));
    CHECK(depthOf(midMin, midMax) < depthOf(farMin, farMax));
    CHECK(depthOf(EngineVec3{-16, -8, -8}, EngineVec3{200, 8, 8}) == 0u);
    CHECK(engineFtoui16(-3.0f) == 0u);
    CHECK(engineFtoui16(65535.9f) == 65535u);
    CHECK(engineFtoui16(70000.0f) == 65535u);
    CHECK(engineFtoui16(12.75f) == 12u);
  }
}

TEST_CASE("LitStagePass: the sort's cost over a few thousand chunks", "[.][stagesort]")
{
  auto camera = CameraState{};
  camera.viewportWidth = 1280;
  camera.viewportHeight = 720;
  const auto view = litViewOf(camera, 1280, 720);
  const auto spaces = std::vector<LitSpace>{litSpaceOf(
    view,
    NoPreviewId,
    EngineVec3{0, 0, 0},
    EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1},
    engineWorldShaderParms())};
  auto chunks = std::map<ChunkKey, Chunk>{};
  const auto material = sortedMaterial("textures/x", 4.0f);
  for (int i = 0; i < 4096; ++i)
  {
    const auto x = float(16 + (i * 37) % 2000);
    const auto y = float((i * 53) % 800 - 400);
    auto chunk = boxChunk(
      "textures/m" + std::to_string(i % 64),
      EngineVec3{x, y, -16},
      EngineVec3{x + 32, y + 32, 16},
      i);
    const auto key = chunk.key;
    chunks.emplace(key, std::move(chunk));
  }
  auto drawSurfs = std::vector<LitSurface>{};
  for (const auto& [key, chunk] : chunks)
  {
    auto surface = LitSurface{};
    surface.chunk = &chunk;
    surface.material = material;
    drawSurfs.push_back(surface);
  }
  const auto start = std::chrono::steady_clock::now();
  constexpr auto Runs = 100;
  auto total = std::size_t{0};
  for (int run = 0; run < Runs; ++run)
  {
    total += stagePassOrder(drawSurfs, spaces).size();
  }
  const auto ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
  CHECK(total == drawSurfs.size() * Runs);
  std::cout << "stagePassOrder over " << drawSurfs.size() << " surfaces: " << ms / Runs
            << " ms\n";
}

} // namespace tb::lit
