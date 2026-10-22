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
#include "lit/EntityState.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::string box(
  const int x0,
  const int y0,
  const int z0,
  const int x1,
  const int y1,
  const int z1,
  const std::string& material)
{
  // TrenchBroom's points: (p3 - p1) x (p2 - p1) is the outward normal
  const auto face = [&](
                      const int ax,
                      const int ay,
                      const int az,
                      const int bx,
                      const int by,
                      const int bz,
                      const int cx,
                      const int cy,
                      const int cz,
                      const std::string& axes) {
    return fmt::format(
      "( {} {} {} ) ( {} {} {} ) ( {} {} {} ) {} {} 0 1 1\n",
      ax,
      ay,
      az,
      bx,
      by,
      bz,
      cx,
      cy,
      cz,
      material,
      axes);
  };
  const auto xAxes = std::string{"[ 0 1 0 0 ] [ 0 0 -1 0 ]"};
  const auto yAxes = std::string{"[ 1 0 0 0 ] [ 0 0 -1 0 ]"};
  const auto zAxes = std::string{"[ 1 0 0 0 ] [ 0 -1 0 0 ]"};
  return "{\n" + face(x0, y0, z0, x0, y0 + 1, z0, x0, y0, z0 + 1, xAxes)
         + face(x1, y1, z1, x1, y1, z1 + 1, x1, y1 + 1, z1, xAxes)
         + face(x0, y0, z0, x0, y0, z0 + 1, x0 + 1, y0, z0, yAxes)
         + face(x1, y1, z1, x1 + 1, y1, z1, x1, y1, z1 + 1, yAxes)
         + face(x0, y0, z0, x0 + 1, y0, z0, x0, y0 + 1, z0, zAxes)
         + face(x1, y1, z1, x1, y1 + 1, z1, x1 + 1, y1, z1, zAxes) + "}\n";
}

const std::shared_ptr<const MaterialTable>& fixtureTable()
{
  static const auto table = [] {
    const auto sizes = listImagesSizes(
      readTextFile(litFixtureFolder() / "goldens" / "lp1b_geometry" / "listImages.txt"));
    auto result = fixtureMaterialTable(sizes);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    return result;
  }();
  return table;
}

ChunkJobInput inputOf(const Snapshot& snapshot)
{
  auto input = ChunkJobInput{};
  for (const auto& entity : sceneEntitiesOf(snapshot))
  {
    input.entities[entity->id] = entity;
    if (entity->classname == "worldspawn")
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
      const auto& entity = input.entities.at(node->entity);
      cells.insert(CellRef{
        entity->classname == "func_group" ? input.worldspawn->id : node->entity,
        cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.materials = fixtureTable();
  return input;
}

std::set<std::string> materialsOf(const ChunkJobOutput& output)
{
  auto materials = std::set<std::string>{};
  for (const auto& chunk : output.chunks)
  {
    materials.insert(chunk.key.material);
  }
  return materials;
}

std::string mapText(const std::string& worldKeys, const std::string& rest)
{
  return "{\n\"classname\" \"worldspawn\"\n" + worldKeys
         + box(0, 0, 0, 64, 64, 64, "lp1b/wall") + "}\n" + rest;
}

} // namespace

TEST_CASE("EntityState: the spawn args as the engine reads them")
{
  SECTION("the map keys' last duplicate wins, case-insensitively, over the entityDef's")
  {
    const auto def = EntityKeys{{"ORIGIN", "9 9 9"}, {"skin", "s"}};
    const auto merged = mergeSpawnArgs(
      {{"origin", "1 2 3"}, {"Origin", "4 5 6"}, {"_color", "0.5 0.5 0.5"}}, &def);
    CHECK(spawnString(merged, "origin") == "4 5 6");
    CHECK(spawnString(merged, "skin") == "s");
    CHECK(merged.size() == 3u);
  }

  SECTION("a rotation key is used alone: no angles, no scale")
  {
    const auto parms = parseSpawnArgsToRenderEntity(
      {{"rotation", "0 1 0 -1 0 0 0 0 1"}, {"angle", "45"}, {"modelscale", "3"}});
    CHECK(parms.axis == EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1});
  }

  SECTION("angle is the yaw; all-zero angles are the identity; modelscale_vec first")
  {
    const auto yaw = parseSpawnArgsToRenderEntity({{"angle", "90"}});
    CHECK(std::abs(yaw.axis[1] - 1.0f) < 1e-6f);
    const auto zero =
      parseSpawnArgsToRenderEntity({{"angles", "0 0 0"}, {"angle", "90"}});
    CHECK(zero.axis == EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    const auto vec =
      parseSpawnArgsToRenderEntity({{"modelscale_vec", "2 3 4"}, {"modelscale", "5"}});
    CHECK(vec.axis == EngineMat3{2, 0, 0, 0, 3, 0, 0, 0, 4});
    const auto tiny = parseSpawnArgsToRenderEntity({{"modelscale", "0.005"}});
    CHECK(tiny.axis == EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
  }

  SECTION("shaderParms and flags")
  {
    const auto parms = parseSpawnArgsToRenderEntity(
      {{"_color", "0.25 0.5 1"}, {"shaderParm7", "3"}, {"noshadows", "1"}});
    CHECK(parms.shaderParms[0] == 0.25f);
    CHECK(parms.shaderParms[3] == 1.0f);
    CHECK(parms.shaderParms[7] == 3.0f);
    CHECK(parms.noShadows);
    CHECK_FALSE(parms.noSelfShadows);
  }
}

TEST_CASE("ProcFile: what the reader refuses")
{
  CHECK_FALSE(readProcFile("mapProcFile002\n").is_success());
  CHECK_FALSE(readProcFile("mapProcFile004 nodes { 1 ( 0 0 1 0 ) 5 0 }").is_success());
  CHECK_FALSE(
    readProcFile("mapProcFile004 model { \"m\" 1 { \"x\" 3 3 0 }").is_success());
  auto tree = readProcFile(
    "mapProcFile004 shadowModel { a b { c } } nodes { 1 ( 1 0 0 0 ) -1 -2 }");
  const auto t =
    std::move(tree) | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  CHECK(pointInArea(t, {1, 0, 0}) == 0);
  CHECK(pointInArea(t, {0, 0, 0}) == 1);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("ChunkBuilder: worldspawn's rules and K17")
{
  SECTION("overrideMaterial replaces every brush side's material")
  {
    const auto snapshot =
      snapshotOfMapText(mapText("\"overrideMaterial\" \"textures/lp1b/crate\"\n", ""));
    const auto output = buildChunks(inputOf(snapshot));
    CHECK(materialsOf(output) == std::set<std::string>{"textures/lp1b/crate"});
  }

  SECTION("removeEntities* removes every entity of a classname, case-insensitively")
  {
    const auto entity =
      "{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n\"model\" \"a\"\n"
      + box(100, 0, 0, 132, 32, 32, "lp1b/crate") + "}\n";
    const auto kept = buildChunks(inputOf(snapshotOfMapText(mapText("", entity))));
    CHECK(materialsOf(kept).contains("textures/lp1b/crate"));
    const auto removed = buildChunks(inputOf(
      snapshotOfMapText(mapText("\"removeEntities2\" \"FUNC_STATIC\"\n", entity))));
    CHECK_FALSE(materialsOf(removed).contains("textures/lp1b/crate"));
  }

  SECTION("K17: a brush entity without model and name is invisible")
  {
    const auto nameless = "{\n\"classname\" \"func_static\"\n"
                          + box(100, 0, 0, 132, 32, 32, "lp1b/crate") + "}\n";
    const auto output = buildChunks(inputOf(snapshotOfMapText(mapText("", nameless))));
    CHECK_FALSE(materialsOf(output).contains("textures/lp1b/crate"));
  }

  SECTION("a func_group's primitives join worldspawn, relative to its origin")
  {
    const auto group = "{\n\"classname\" \"func_group\"\n\"origin\" \"16 0 0\"\n"
                       + box(100, 0, 0, 132, 32, 32, "lp1b/crate") + "}\n";
    const auto snapshot = snapshotOfMapText(mapText("", group));
    const auto output = buildChunks(inputOf(snapshot));
    const auto crate =
      std::find_if(output.chunks.begin(), output.chunks.end(), [](const auto& c) {
        return c.key.material == "textures/lp1b/crate";
      });
    REQUIRE(crate != output.chunks.end());
    CHECK(crate->key.entity == snapshot.entities.front().id);
    CHECK(crate->bounds[0] == EngineVec3{84, 0, 0});
    CHECK(crate->bounds[1] == EngineVec3{116, 32, 32});
  }

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

TEST_CASE("SceneGeometry: batches on the pool, entity records and removals (B0.3, B0.4)")
{
  auto pool = CpuPool::create() | kdl::value();
  auto scene = SceneGeometry{pool.get(), nullptr};
  const auto entity =
    "{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n\"model\" \"a\"\n"
    + box(100, 0, 0, 132, 32, 32, "lp1b/crate") + "}\n";
  auto snapshot = snapshotOfMapText(mapText("", entity));
  snapshot.sequence = 1;
  REQUIRE(scene.readyForSnapshot());
  scene.takeSnapshot(snapshot);
  CHECK_FALSE(scene.readyForSnapshot());
  scene.finishForTesting();
  CHECK(scene.readyForSnapshot());
  CHECK(scene.appliedSequence() == 1u);
  const auto chunks = scene.chunks().size();
  CHECK(chunks >= 2u);
  CHECK(scene.entityStates().size() == 1u);

  // an entity record without its nodes re-keys its chunks: a new origin moves the pivot
  auto moved = Snapshot{};
  moved.sequence = 2;
  const auto func = snapshot.entities[1];
  moved.entities.push_back(func);
  for (auto k = func.keys.first; k < func.keys.first + func.keys.count; ++k)
  {
    const auto key = std::string{snapshot.string(snapshot.keyValues[k].key)};
    const auto value = std::string{snapshot.string(snapshot.keyValues[k].value)};
    const auto add = [&](const std::string& text) {
      const auto offset = std::uint32_t(moved.strings.size());
      moved.strings += text;
      return SnapshotString{offset, std::uint32_t(text.size())};
    };
    if (k == func.keys.first)
    {
      moved.entities.back().keys.first = 0;
      moved.entities.back().classname = add("func_static");
    }
    moved.keyValues.push_back(SnapshotKeyValue{add(key), add(value)});
  }
  {
    const auto offset = std::uint32_t(moved.strings.size());
    moved.strings += "origin";
    const auto value = std::uint32_t(moved.strings.size());
    moved.strings += "116 16 16";
    moved.keyValues.push_back(SnapshotKeyValue{{offset, 6}, {value, 9}});
    moved.entities.back().keys.count = std::uint32_t(moved.keyValues.size());
  }
  scene.takeSnapshot(moved);
  scene.finishForTesting();
  CHECK(scene.appliedSequence() == 2u);
  REQUIRE(scene.entityStates().contains(func.id));
  CHECK(scene.entityStates().at(func.id).modelOrigin == EngineVec3{116, 16, 16});

  // removed holds entity ids too: the entity's chunks go with its nodes
  auto removed = Snapshot{};
  removed.sequence = 3;
  removed.removed.push_back(func.id);
  for (const auto& node : snapshot.nodes)
  {
    if (node.entity == func.id)
    {
      removed.removed.push_back(node.id);
    }
  }
  scene.takeSnapshot(removed);
  scene.finishForTesting();
  CHECK(scene.appliedSequence() == 3u);
  CHECK(scene.chunks().size() < chunks);
  CHECK(std::none_of(scene.chunks().begin(), scene.chunks().end(), [&](const auto& c) {
    return c.first.entity == func.id;
  }));
  pool->stop();
}

TEST_CASE("SceneGeometry: a 2000-brush snapshot's changed cells on the pool")
{
  // 2000 boxes over 4 × 4 cells; then one box per cell moves (Release measures, D60)
  auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
  auto count = 0;
  for (auto i = 0; i < 2000; ++i)
  {
    const auto cx = (i % 4) * 1024;
    const auto cy = ((i / 4) % 4) * 1024;
    const auto slot = i / 16;
    const auto x = cx + (slot % 10) * 96 + 32;
    const auto y = cy + ((slot / 10) % 10) * 96 + 32;
    const auto z = (slot / 100) * 96;
    text += box(x, y, z, x + 48, y + 48, z + 48, i % 2 ? "lp1b/wall" : "lp1b/floor");
    ++count;
  }
  text += "}\n";
  const auto snapshot = snapshotOfMapText(text);
  auto input = inputOf(snapshot);
  CHECK(input.cells.size() == 16u);

  // per cell, on this thread
  auto slowest = std::chrono::duration<double, std::milli>{0};
  auto total = std::chrono::duration<double, std::milli>{0};
  for (const auto& cell : input.cells)
  {
    auto one = input;
    one.cells = {cell};
    const auto start = std::chrono::steady_clock::now();
    const auto output = buildChunks(one);
    const auto took = std::chrono::steady_clock::now() - start;
    slowest = std::max(slowest, std::chrono::duration<double, std::milli>{took});
    total += took;
    CHECK_FALSE(output.chunks.empty());
  }
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  // all 16 changed cells as one batch on the pool's two threads
  auto pool = CpuPool::create() | kdl::value();
  auto scene = SceneGeometry{pool.get(), nullptr};
  const auto start = std::chrono::steady_clock::now();
  scene.takeSnapshot(snapshot);
  scene.finishForTesting();
  const auto batch =
    std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - start};
  pool->stop();

  std::cout << fmt::format(
    "2000-brush snapshot ({} boxes, {} cells): per cell {:.2f} ms on average, {:.2f} ms "
    "at most; the batch of 16 cells on the pool {:.2f} ms ({:.2f} ms per cell)\n",
    count,
    input.cells.size(),
    total.count() / double(input.cells.size()),
    slowest.count(),
    batch.count(),
    batch.count() / 16.0);
  CHECK(scene.chunks().size() >= 16u);
}

} // namespace tb::lit

namespace tb::lit
{

TEST_CASE("MaterialService: .proc files on the parse thread (D64)")
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{})};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  auto service = MaterialService::create(std::move(options)) | kdl::value();

  const auto path =
    (litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc").string();
  const auto missing = (litFixtureFolder() / "maps" / "no_such_map.proc").string();
  service->loadProcFile(path);
  service->loadProcFile(missing);
  const auto table = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.findProcFile(path) && t.findProcFile(missing);
    },
    std::chrono::seconds{10});
  const auto found = table->findProcFile(path);
  REQUIRE(found);
  REQUIRE(found->tree);
  CHECK(found->tree->nodes.size() == 39u);
  CHECK(found->tree->numAreas == 2);
  const auto none = table->findProcFile(missing);
  REQUIRE(none);
  CHECK_FALSE(none->tree);
  CHECK_FALSE(none->error.empty());

  // a reload keeps them
  service->reload();
  const auto reloaded = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.version > table->version && t.registry != table->registry;
    },
    std::chrono::seconds{10});
  CHECK(reloaded->findProcFile(path));
}

} // namespace tb::lit
