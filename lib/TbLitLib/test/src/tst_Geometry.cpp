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
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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
  return boxBrush(x0, y0, z0, x1, y1, z1, material);
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

// the nodes of full whose ids are given, without entity records or removals
Snapshot nodesOnly(
  const Snapshot& full, const std::set<PreviewId>& ids, const std::uint64_t sequence)
{
  auto snapshot = full;
  snapshot.sequence = sequence;
  snapshot.entities.clear();
  snapshot.removed.clear();
  std::erase_if(
    snapshot.nodes, [&](const SnapshotNode& node) { return !ids.contains(node.id); });
  return snapshot;
}

// the snapshot of text with every node and entity id moved by offset; the nodes of the
// first entity (worldspawn) keep entity 1, the scene's worldspawn
Snapshot withIdOffset(Snapshot snapshot, const PreviewId offset)
{
  for (auto& node : snapshot.nodes)
  {
    node.id += offset;
    node.entity = node.entity == 1 ? 1 : node.entity + offset;
  }
  for (auto& entity : snapshot.entities)
  {
    entity.id += offset;
  }
  return snapshot;
}

// one entity record with these keys, without its nodes (B0.4)
Snapshot entityRecord(
  const PreviewId id,
  const std::string& classname,
  const std::vector<std::pair<std::string, std::string>>& keys,
  const std::uint64_t sequence)
{
  auto snapshot = Snapshot{};
  snapshot.sequence = sequence;
  const auto add = [&](const std::string& text) {
    const auto offset = std::uint32_t(snapshot.strings.size());
    snapshot.strings += text;
    return SnapshotString{offset, std::uint32_t(text.size())};
  };
  auto entity = SnapshotEntity{};
  entity.id = id;
  entity.classname = add(classname);
  for (const auto& [key, value] : keys)
  {
    snapshot.keyValues.push_back(SnapshotKeyValue{add(key), add(value)});
  }
  entity.keys = SnapshotRange{0, std::uint32_t(snapshot.keyValues.size())};
  snapshot.entities.push_back(entity);
  return snapshot;
}

// the node whose first face has this material
PreviewId nodeWithMaterial(const Snapshot& snapshot, const std::string& material)
{
  for (const auto& node : snapshot.nodes)
  {
    if (
      node.kind == SnapshotNodeKind::Brush && node.elements.count > 0
      && snapshot.string(snapshot.faces[node.elements.first].material) == material)
    {
      return node.id;
    }
  }
  return NoPreviewId;
}

// what a chunk is, for comparing two builds of it
struct ChunkSummary
{
  CastClass castClass = CastClass::Opaque;
  std::uint64_t appearanceVersion = 0;
  std::uint64_t casterVersion = 0;
  bool operator==(const ChunkSummary&) const = default;
};

std::map<ChunkKey, ChunkSummary> summaryOf(const std::map<ChunkKey, Chunk>& chunks)
{
  auto summary = std::map<ChunkKey, ChunkSummary>{};
  for (const auto& [key, chunk] : chunks)
  {
    summary[key] = {chunk.castClass, chunk.appearanceVersion, chunk.casterVersion};
  }
  return summary;
}

std::map<ChunkKey, ChunkSummary> summaryOf(const std::vector<Chunk>& chunks)
{
  auto summary = std::map<ChunkKey, ChunkSummary>{};
  for (const auto& chunk : chunks)
  {
    summary[chunk.key] = {chunk.castClass, chunk.appearanceVersion, chunk.casterVersion};
  }
  return summary;
}

std::map<ChunkKey, ChunkSummary> ofEntity(
  std::map<ChunkKey, ChunkSummary> summary, const PreviewId entity)
{
  std::erase_if(summary, [&](const auto& entry) { return entry.first.entity != entity; });
  return summary;
}

// a material service over the fixture's materials, the stand-ins and the test's own
// decls, whose parse thread waits at the gate before every request
std::unique_ptr<MaterialService> gatedService(const std::shared_ptr<ParseGate>& gate)
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{
        DeclFile{
          "materials/lp1bc.mtr",
          readTextFile(litFixtureFolder() / "materials" / "lp1bc.mtr"),
          0},
        DeclFile{
          "materials/zz_lp1b_test.mtr",
          "textures/common/origin\n{\n\torigin\n\tnoshadows\n}\n"
          "textures/lp1b/test_nodraw\n{\n\tnoshadows\n}\n"
          "textures/lp1b/test_origin\n{\n\torigin\n\tnoshadows\n}\n",
          0},
        DeclFile{
          "def/lp1b_test.def",
          "entityDef func_static\n{\n\t\"noshadows\" \"1\"\n}\n"
          "entityDef func_lp1b_late\n{\n\t\"noshadows\" \"1\"\n}\n",
          0},
      })};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
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

  // R2.4: 200 brush entities beside the 2000 boxes; a snapshot that changes their 200
  // entity records alone is merged by the entity -> nodes index, not by a scan of every
  // node per entity
  auto withEntities = text;
  for (auto i = 0; i < 200; ++i)
  {
    const auto x = 5000 + (i % 20) * 64;
    const auto y = (i / 20) * 64;
    withEntities += fmt::format(
      "{{\n\"classname\" \"func_static\"\n\"name\" \"f{}\"\n\"model\" \"f{}\"\n", i, i);
    withEntities += box(x, y, 0, x + 32, y + 32, 32, "lp1b/crate") + "}\n";
  }
  auto full = snapshotOfMapText(withEntities);
  full.sequence = 1;
  auto changed = Snapshot{};
  changed.sequence = 2;
  for (const auto& entity : full.entities)
  {
    if (full.string(entity.classname) == "worldspawn")
    {
      continue;
    }
    const auto name =
      std::string{full.string(full.keyValues[entity.keys.first + 1].value)};
    auto one = entityRecord(
      entity.id,
      "func_static",
      {{"classname", "func_static"},
       {"name", name},
       {"model", name},
       {"_color", "1 0 0"}},
      2);
    const auto base = std::uint32_t(changed.strings.size());
    const auto firstKey = std::uint32_t(changed.keyValues.size());
    changed.strings += one.strings;
    for (auto keyValue : one.keyValues)
    {
      keyValue.key.offset += base;
      keyValue.value.offset += base;
      changed.keyValues.push_back(keyValue);
    }
    auto record = one.entities.front();
    record.classname.offset += base;
    record.keys.first = firstKey;
    changed.entities.push_back(record);
  }
  REQUIRE(changed.entities.size() == 200u);
  auto pool2 = CpuPool::create() | kdl::value();
  {
    auto entities = SceneGeometry{pool2.get(), nullptr};
    const auto mergeFull = std::chrono::steady_clock::now();
    entities.takeSnapshot(full);
    const auto fullTook = std::chrono::duration<double, std::milli>{
      std::chrono::steady_clock::now() - mergeFull};
    entities.finishForTesting();
    const auto mergeChanged = std::chrono::steady_clock::now();
    entities.takeSnapshot(changed);
    const auto changedTook = std::chrono::duration<double, std::milli>{
      std::chrono::steady_clock::now() - mergeChanged};
    entities.finishForTesting();
    std::cout << fmt::format(
      "R2.4: merging the snapshot of 2000 boxes and 200 brush entities took {:.2f} ms on "
      "the render thread, merging 200 changed entity records {:.2f} ms\n",
      fullTook.count(),
      changedTook.count());
    CHECK(entities.appliedSequence() == 2u);
    CHECK(entities.entityStates().size() == 200u);
  }
  pool2->stop();
}

TEST_CASE("ChunkBuilder: the tangent path of a material (C25, R3.11)")
{
  auto entry = MaterialEntry{};
  CHECK(tangentPath(nullptr) == EngineTangentPath::WithoutNormals);
  CHECK(tangentPath(&entry) == EngineTangentPath::WithoutNormals);
  // the engine's failure path: MikkTSpace is not copied
  entry.mikktspace = true;
  CHECK(tangentPath(&entry) == EngineTangentPath::Mikktspace);
  // unsmoothedTangents comes first (tr_trisurf.cpp:1831-1839)
  entry.unsmoothedTangents = true;
  CHECK(tangentPath(&entry) == EngineTangentPath::Unsmoothed);
}

TEST_CASE("SceneGeometry: what a later material table brings is built (R3.1, R3.2)")
{
  using namespace std::chrono_literals;
  // the stand-in table (a registry of its own) before the service's registry exists:
  // only one lives at a time, and the tests run in any order
  static_cast<void>(fixtureTable());
  auto gate = std::make_shared<ParseGate>();
  gate->close();
  auto service = gatedService(gate);
  // destroyed before the service, so that its join never waits at the gate
  const auto atExit = OpenAtExit{gate};
  // the first registry is built before the thread waits at the gate
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.registry != 0; }, 10s));
  auto pool = CpuPool::create() | kdl::value();

  const auto world = std::string{"{\n\"classname\" \"worldspawn\"\n"}
                     + box(0, 0, 0, 64, 64, 64, "lp1b/wall")
                     + box(200, 0, 0, 232, 32, 32, "lp1b/grate")
                     + box(300, 0, 0, 332, 32, 32, "lp1b/test_nodraw")
                     + box(400, 0, 0, 432, 32, 32, "lp1b/crate") + "}\n";
  const auto func = std::string{"{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n"}
                    + "\"model\" \"a\"\n" + box(1100, 0, 0, 1132, 32, 32, "lp1b/floor")
                    + box(1200, 0, 0, 1216, 16, 16, "lp1b/test_origin") + "}\n";
  auto snapshot = snapshotOfMapText(world + func);
  snapshot.sequence = 1;
  const auto funcId = snapshot.entities[1].id;
  // a far brush of worldspawn, the only node of a later snapshot
  const auto far = [&](const PreviewId offset, const std::uint64_t sequence) {
    return nodesOnly(
      withIdOffset(
        snapshotOfMapText(
          "{\n\"classname\" \"worldspawn\"\n" + box(5000, 0, 0, 5032, 32, 32, "lp1b/wall")
          + "}\n"),
        offset),
      {2 + offset},
      sequence);
  };
  const auto withoutFar = [](std::map<ChunkKey, ChunkSummary> summary) {
    std::erase_if(summary, [](const auto& entry) { return entry.first.cell[0] >= 4; });
    return summary;
  };

  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    scene.takeSnapshot(snapshot);
    scene.finishForTesting();

    // built while every material and the entityDef were pending: the defaults
    const auto pending = scene.chunks();
    CHECK(std::any_of(pending.begin(), pending.end(), [](const auto& entry) {
      return entry.first.material == "textures/lp1b/test_nodraw";
    }));
    REQUIRE(scene.entityStates().contains(funcId));
    CHECK(scene.entityStates().at(funcId).originOffset == EngineVec3{0, 0, 0});

    // the table arrives; a later snapshot for another cell adopts it first (R3.1's
    // failing sequence), and every pending cell is built again in its batch
    gate->open();
    const auto table = service->waitForTableForTesting(
      [](const MaterialTable& t) {
        return t.find("textures/lp1b/grate") && t.find("textures/lp1b/test_nodraw")
               && t.find("textures/lp1b/test_origin") && t.find("textures/lp1b/crate")
               && t.find("textures/lp1b/floor") && t.find("textures/lp1b/wall")
               && t.findEntityDef("func_static");
      },
      10s);
    REQUIRE(table);
    scene.takeSnapshot(far(1000, 2));
    scene.finishForTesting();

    auto expectedInput = inputOf(snapshot);
    expectedInput.materials = table;
    expectedInput.entityDefs["func_static"] = table->findEntityDef("func_static")->entry;
    const auto expected = buildChunks(expectedInput);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    CHECK(withoutFar(summaryOf(scene.chunks())) == summaryOf(expected.chunks));

    // what that means: no face of a material neither drawn nor casting, the grate
    // perforated, the origin brush found by its material, the entityDef's noshadows
    const auto& chunks = scene.chunks();
    CHECK(std::none_of(chunks.begin(), chunks.end(), [](const auto& entry) {
      return entry.first.material == "textures/lp1b/test_nodraw"
             || entry.first.material == "textures/lp1b/test_origin";
    }));
    for (const auto& [key, chunk] : chunks)
    {
      CAPTURE(key.material, key.entity);
      if (key.material == "textures/lp1b/grate")
      {
        CHECK(chunk.castClass == CastClass::Perforated);
      }
      if (key.entity == funcId)
      {
        CHECK(chunk.castClass == CastClass::None);
      }
    }
    REQUIRE(scene.entityStates().contains(funcId));
    CHECK(scene.entityStates().at(funcId).originOffset == EngineVec3{1208, 8, 8});

    // R3.2 alone: an entity whose classname's entityDef is asked for only now, while the
    // materials are known; the gate holds the parse thread after one more request
    gate->close();
    static_cast<void>(service->lookup("textures/lp1b/patch"));
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.find("textures/lp1b/patch") != nullptr; },
      10s));
    auto late = withIdOffset(
      snapshotOfMapText(
        "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"func_lp1b_late\"\n"
        "\"name\" \"late\"\n\"model\" \"late\"\n"
        + box(3100, 0, 0, 3132, 32, 32, "lp1b/crate") + "}\n"),
      2000);
    std::erase_if(
      late.entities, [](const SnapshotEntity& entity) { return entity.id == 2001; });
    late.sequence = 3;
    const auto lateId = PreviewId{2002};
    scene.takeSnapshot(late);
    scene.finishForTesting();
    const auto lateCast = [&] {
      for (const auto& [key, chunk] : scene.chunks())
      {
        if (key.entity == lateId)
        {
          return std::optional{chunk.castClass};
        }
      }
      return std::optional<CastClass>{};
    };
    CHECK(lateCast() == CastClass::Opaque);

    gate->open();
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.findEntityDef("func_lp1b_late") != nullptr; },
      10s));
    scene.takeSnapshot(far(3000, 4));
    scene.finishForTesting();
    CHECK(lateCast() == CastClass::None);
  }
  pool->stop();
}

TEST_CASE("SceneGeometry: a classname change to and from func_group re-keys (R2.3)")
{
  auto pool = CpuPool::create() | kdl::value();
  auto scene = SceneGeometry{pool.get(), nullptr};
  auto snapshot = snapshotOfMapText(mapText(
    "",
    "{\n\"classname\" \"func_group\"\n" + box(100, 0, 0, 132, 32, 32, "lp1b/crate")
      + "}\n"));
  snapshot.sequence = 1;
  const auto world = snapshot.entities[0].id;
  const auto group = snapshot.entities[1].id;
  const auto crateUnder = [&](const PreviewId entity) {
    return std::count_if(
      scene.chunks().begin(), scene.chunks().end(), [&](const auto& e) {
        return e.first.entity == entity && e.first.material == "textures/lp1b/crate";
      });
  };

  scene.takeSnapshot(snapshot);
  scene.finishForTesting();
  CHECK(crateUnder(world) > 0);
  CHECK(crateUnder(group) == 0);

  // only the entity record comes: its brushes move to its own key
  scene.takeSnapshot(entityRecord(
    group,
    "func_static",
    {{"classname", "func_static"}, {"name", "g"}, {"model", "g"}},
    2));
  scene.finishForTesting();
  CHECK(crateUnder(world) == 0);
  CHECK(crateUnder(group) > 0);

  // and back into worldspawn
  scene.takeSnapshot(entityRecord(group, "func_group", {{"classname", "func_group"}}, 3));
  scene.finishForTesting();
  CHECK(crateUnder(world) > 0);
  CHECK(crateUnder(group) == 0);
  pool->stop();
}

TEST_CASE("SceneGeometry: a moved origin brush rebuilds every cell of its entity (R3.4)")
{
  const auto mapWith = [](const int originX) {
    return mapText(
      "",
      std::string{"{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n\"model\" \"a\"\n"}
        + box(100, 0, 0, 132, 32, 32, "lp1b/crate")
        + box(1100, 0, 0, 1132, 32, 32, "lp1b/crate")
        + box(2100, 0, 0, 2132, 32, 32, "lp1b/crate")
        + box(originX, 0, 0, originX + 16, 16, 16, "common/origin") + "}\n");
  };
  auto before = snapshotOfMapText(mapWith(1200));
  before.sequence = 1;
  const auto after = snapshotOfMapText(mapWith(2200));
  const auto entity = before.entities[1].id;
  const auto originBrush = nodeWithMaterial(before, "common/origin");
  REQUIRE(originBrush != NoPreviewId);

  auto pool = CpuPool::create() | kdl::value();
  auto scene = SceneGeometry{pool.get(), nullptr};
  scene.takeSnapshot(before);
  scene.finishForTesting();
  auto cells = std::set<CellCoord>{};
  for (const auto& [key, chunk] : scene.chunks())
  {
    if (key.entity == entity)
    {
      cells.insert(key.cell);
    }
  }
  CHECK(cells.size() == 3u);

  // the origin brush moves from cell 1 to cell 2: cell 0's chunks are relative to the
  // new pivot too
  scene.takeSnapshot(nodesOnly(after, {originBrush}, 2));
  scene.finishForTesting();
  auto input = inputOf(after);
  input.materials = nullptr;
  const auto expected = buildChunks(input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  CHECK(
    ofEntity(summaryOf(scene.chunks()), entity)
    == ofEntity(summaryOf(expected.chunks), entity));
  REQUIRE(scene.entityStates().contains(entity));
  CHECK(scene.entityStates().at(entity).originOffset == EngineVec3{2208, 8, 8});
  pool->stop();
}

TEST_CASE("SceneGeometry: a cancelled pool's batch ends at once (R2.1)")
{
  auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
  for (auto i = 0; i < 2000; ++i)
  {
    const auto x = (i % 4) * 1024 + ((i / 16) % 10) * 96 + 32;
    const auto y = ((i / 4) % 4) * 1024 + ((i / 160) % 10) * 96 + 32;
    const auto z = (i / 1600) * 96;
    text += box(x, y, z, x + 48, y + 48, z + 48, "lp1b/wall");
  }
  text += "}\n";
  auto snapshot = snapshotOfMapText(text);
  snapshot.sequence = 1;

  auto pool = CpuPool::create() | kdl::value();
  auto scene = SceneGeometry{pool.get(), nullptr};
  scene.takeSnapshot(snapshot);
  // the stop's first step: the queued jobs return without building, the running ones at
  // their next node, and nothing is joined
  pool->cancel();
  const auto start = std::chrono::steady_clock::now();
  scene.finishForTesting();
  const auto took =
    std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - start};
  std::cout << fmt::format(
    "a cancelled 2000-brush batch of 16 cells ended in {:.2f} ms\n", took.count());
  const auto warnings = scene.takeWarnings();
  CHECK(
    std::count_if(
      warnings.begin(),
      warnings.end(),
      [](const std::string& w) { return w.find("abandoned") != std::string::npos; })
    >= 14);
  CHECK(scene.chunks().size() <= 2u);
  pool->stop();
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE(
  "SceneGeometry: 50 snapshots, chunk batches and light jobs leave the heap (R2.14)")
{
  const auto heapGrows = [](const auto& f) {
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
  };
  const auto mapWith = [](const int x) {
    return mapText(
      "",
      "{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n\"model\" \"a\"\n"
        + box(x, 0, 0, x + 32, 32, 32, "lp1b/crate")
        + box(1100, 0, 0, 1132, 32, 32, "lp1b/grate") + "}\n"
        + "{\n\"classname\" \"light\"\n\"origin\" \"" + std::to_string(x)
        + " 0 64\"\n\"light_radius\" \"64 64 64\"\n}\n");
  };
  const auto first = snapshotOfMapText(mapWith(100));
  const auto moved = snapshotOfMapText(mapWith(164));
  const auto crate = nodeWithMaterial(first, "lp1b/crate");
  const auto light =
    std::find_if(first.nodes.begin(), first.nodes.end(), [](const SnapshotNode& node) {
      return node.kind == SnapshotNodeKind::Light;
    });
  REQUIRE(light != first.nodes.end());
  // a node and a light record that move back and forth: every batch replaces chunks,
  // entity states and a light record, and the state stays bounded
  auto back = nodesOnly(first, {crate, light->id}, 0);
  back.entities = first.entities;
  auto forth = nodesOnly(moved, {crate, light->id}, 0);
  forth.entities = moved.entities;

  const auto run = [&] {
    auto pool = CpuPool::create() | kdl::value();
    {
      auto scene = SceneGeometry{pool.get(), nullptr};
      auto start = first;
      start.sequence = 1;
      scene.takeSnapshot(start);
      scene.finishForTesting();
      for (auto i = 0; i < 50; ++i)
      {
        auto next = i % 2 ? back : forth;
        next.sequence = std::uint64_t(i + 2);
        scene.takeSnapshot(next);
        scene.finishForTesting();
      }
      static_cast<void>(scene.takeWarnings());
    }
    pool->stop();
  };
  // what the first round allocates for good
  run();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  CHECK_FALSE(heapGrows(run));
}
#endif

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
