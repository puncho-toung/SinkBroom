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

#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/AreaData.h"
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/EntityState.h"
#include "lit/GameFileSource.h"
#include "lit/ImageOverride.h"
#include "lit/LightData.h"
#include "lit/LightRecord.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <iostream>
#include <latch>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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

// the fixture's materials, the stand-ins and the test's own decls
std::vector<DeclFile> fixtureDecls()
{
  return {
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
  };
}

// a material service over the fixture decls, whose parse thread waits at the gate before
// every request
std::unique_ptr<MaterialService> gatedService(const std::shared_ptr<ParseGate>& gate)
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(fixtureDecls())};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

/**
 * C10: a game folder with the fixture decls and the fixture map's .proc as
 * maps/lp1b_geometry.proc, where the bake search finds it for the map document
 * base/maps/lp1b_geometry.map.
 */
class BakeGame
{
private:
  fs::TestEnvironment m_env;

public:
  BakeGame()
  {
    for (const auto& file : fixtureDecls())
    {
      writeTextFile(base() / file.path, *file.text);
    }
    writeProc("lp1b_geometry", readTextFile(goldenProc()));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }

  static std::filesystem::path goldenProc()
  {
    return litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc";
  }

  // the document path of a map below base/maps
  std::string mapPath(const std::string& name) const
  {
    return (base() / "maps" / (name + ".map")).string();
  }

  void writeProc(const std::string& name, const std::string& text) const
  {
    writeTextFile(base() / "maps" / (name + ".proc"), text);
  }

  // a material service over this game folder, gated as gatedService
  std::unique_ptr<MaterialService> service(
    const std::shared_ptr<ParseGate>& gate,
    const std::chrono::milliseconds pollInterval = std::chrono::milliseconds{0}) const
  {
    auto options = MaterialServiceOptions{};
    options.makeSource = [path = m_env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    };
    options.pollInterval = pollInterval;
    options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
    return MaterialService::create(std::move(options))
           | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  }
};

// collects until nothing is in flight and done holds, or 10 s
template <typename Done>
bool settle(SceneGeometry& scene, const Done& done)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline)
  {
    scene.collect();
    if (!scene.busy() && done())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  return false;
}

/**
 * Closes the gate once the parse thread stands at it: the thread, waiting for a request,
 * takes one more (ParseGate), so it is given a lookup of name, and once that is published
 * it stops before its next request; a request made after this stays queued until open.
 */
void closeGateWhenIdle(MaterialService& service, ParseGate& gate, const std::string& name)
{
  gate.close();
  static_cast<void>(service.lookup(name));
  REQUIRE(service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.find(name) != nullptr; },
    std::chrono::seconds{10}));
}

// the snapshot's first light node
const SnapshotNode& lightNodeOf(const Snapshot& snapshot)
{
  const auto light = std::find_if(
    snapshot.nodes.begin(), snapshot.nodes.end(), [](const SnapshotNode& node) {
      return node.kind == SnapshotNodeKind::Light;
    });
  REQUIRE(light != snapshot.nodes.end());
  return *light;
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

TEST_CASE("SceneGeometry, a resized editor image builds its cells again (S2.5)")
{
  using namespace std::chrono_literals;
  // the texture is 32 × 16, then 64 × 16, then deleted: its cells are built with each
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  writeTextFile(
    base / "materials" / "resized.mtr",
    materialText("textures/fix/resized", "textures/fix/resized"));
  const auto tga = [&](const int width, const int height) {
    writeTextFile(
      base / "textures" / "fix" / "resized.tga",
      test::tgaFile(test::gradient(width, height), 32));
  };
  tga(32, 16);

  auto options = MaterialServiceOptions{};
  options.makeSource = [path = env.dir()](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
  };
  options.pollInterval = 20ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      "{\n\"classname\" \"worldspawn\"\n" + box(0, 0, 0, 64, 64, 64, "fix/resized")
      + "}\n");
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);

    // the material's chunk once it is built again after the table's size changed: other
    // vertices (st divided by another size), provisional for a Default size
    auto previous = std::vector<EngineDrawVert>{};
    const auto builtWith = [&](const int width, const EditorImageSize::State state) {
      static_cast<void>(service->lookup("textures/fix/resized"));
      REQUIRE(service->waitForTableForTesting(
        [&](const MaterialTable& t) {
          const auto slot = t.find("textures/fix/resized");
          return slot && slot->entry && slot->sizeMeasured
                 && slot->entry->editorImageSize.width == width
                 && slot->entry->editorImageSize.state == state;
        },
        10s));
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < deadline)
      {
        scene.collect();
        scene.finishForTesting();
        const auto& chunks = scene.chunks();
        const auto chunk =
          std::find_if(chunks.begin(), chunks.end(), [](const auto& entry) {
            return entry.first.material == "textures/fix/resized";
          });
        if (
          chunk != chunks.end()
          && chunk->second.provisional == (state == EditorImageSize::State::Default)
          && chunk->second.verts != previous)
        {
          previous = chunk->second.verts;
          return true;
        }
        std::this_thread::sleep_for(5ms);
      }
      return false;
    };
    CHECK(builtWith(32, EditorImageSize::State::Ready));
    tga(64, 16);
    bumpModificationTime(base / "textures" / "fix" / "resized.tga");
    CHECK(builtWith(64, EditorImageSize::State::Ready));
    std::filesystem::remove(base / "textures" / "fix" / "resized.tga");
    CHECK(builtWith(8, EditorImageSize::State::Default));
  }
  pool->stop();
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
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

TEST_CASE(
  "SceneGeometry: a table published while a batch runs is adopted when it applies (V.1)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  auto gate = std::make_shared<ParseGate>();
  gate->close();
  auto service = gatedService(gate);
  const auto atExit = OpenAtExit{gate};
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.registry != 0; }, 10s));
  auto pool = CpuPool::create() | kdl::value();

  // both pool threads held, so that the batch stays in flight while the table arrives
  auto held = std::make_shared<ParseGate>();
  held->close();
  auto started = std::make_shared<std::latch>(std::ptrdiff_t(CpuPool::ThreadCount));
  auto blockers = std::vector<std::future<CpuJobResult<bool>>>{};
  for (auto i = std::size_t{0}; i < CpuPool::ThreadCount; ++i)
  {
    blockers.push_back(pool->run([held, started](const CpuJobContext&) -> Result<bool> {
      started->count_down();
      held->pass();
      return true;
    }));
  }
  started->wait();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    // a failed REQUIRE releases the pool before the scene waits for its jobs
    const auto releaseAtExit = OpenAtExit{held};
    auto snapshot = snapshotOfMapText(mapText("", ""));
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);
    REQUIRE(scene.busy());

    gate->open();
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.find("textures/lp1b/wall") != nullptr; },
      10s));
    // a pass while the batch runs cannot adopt it
    CHECK_FALSE(scene.collect());
    held->open();

    // the batch applies, and with it the table is adopted: its cells are built again at
    // once, with no snapshot, no table and no further pass
    auto applied = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!applied && std::chrono::steady_clock::now() < deadline)
    {
      applied = scene.collect();
      if (!applied)
      {
        std::this_thread::sleep_for(1ms);
      }
    }
    REQUIRE(applied);
    CHECK(scene.busy());
    scene.finishForTesting();
  }
  for (auto& blocker : blockers)
  {
    blocker.wait();
  }
  pool->stop();
}

TEST_CASE("SceneGeometry: a material reload builds every chunk and light again (V.2)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  // version 1: an opaque material, and a cube map that fails (its image was referenced
  // as 2D first); version 2: the first translucent, the second parses
  auto version = std::make_shared<std::atomic<int>>(1);
  auto options = MaterialServiceOptions{};
  options.makeSource = [version](const std::atomic<bool>&) {
    const auto second = version->load() == 2;
    auto text = std::string{
      "textures/lp1b/conflict2d\n{\n\t{\n\t\tblend add\n\t\tmap "
      "env/lp1b_conflict\n\t}\n}\n"};
    text += second ? "textures/lp1b/conflictcube\n{\n\t{\n\t\tblend add\n\t\tcubeMap "
                     "env/lp1b_other\n\t}\n}\n"
                   : "textures/lp1b/conflictcube\n{\n\t{\n\t\tblend add\n\t\tcubeMap "
                     "env/lp1b_conflict\n\t}\n}\n";
    text += second
              ? "textures/lp1b/reload\n{\n\ttranslucent\n\t{\n\t\tblend blend\n\t\tmap "
                "textures/lp1b/reload.tga\n\t}\n}\n"
              : "textures/lp1b/reload\n{\n\tdiffusemap textures/lp1b/reload.tga\n}\n";
    text +=
      "lights/lp1b_reloadlight\n{\n\t{\n\t\tblend add\n\t\tmap "
      "lights/squarelight1.tga\n\t}\n}\n";
    return std::unique_ptr<DeclSource>{std::make_unique<MemoryDeclSource>(
      std::vector<DeclFile>{DeclFile{"materials/zz_reload.mtr", text, 0}})};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      "{\n\"classname\" \"worldspawn\"\n" + box(0, 0, 0, 32, 32, 32, "lp1b/conflict2d")
      + box(64, 0, 0, 96, 32, 32, "lp1b/conflictcube")
      + box(128, 0, 0, 160, 32, 32, "lp1b/reload")
      + "}\n{\n\"classname\" \"light\"\n\"origin\" \"64 64 64\"\n"
        "\"light_radius\" \"64 64 64\"\n\"texture\" \"lights/lp1b_reloadlight\"\n}\n");
    snapshot.sequence = 1;
    const auto light = std::find_if(
      snapshot.nodes.begin(), snapshot.nodes.end(), [](const SnapshotNode& node) {
        return node.kind == SnapshotNodeKind::Light;
      });
    REQUIRE(light != snapshot.nodes.end());
    const auto lightId = light->entity;
    scene.takeSnapshot(snapshot);

    const auto castOf = [&](const std::string& material) {
      for (const auto& [key, chunk] : scene.chunks())
      {
        if (key.material == material)
        {
          return std::optional{chunk.castClass};
        }
      }
      return std::optional<CastClass>{};
    };
    const auto lightRegistry = [&] {
      const auto record = scene.lightTable()->find(lightId);
      return record && record->material ? record->material->registry : 0u;
    };
    const auto settle = [&](const auto& done) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < deadline)
      {
        scene.collect();
        if (!scene.busy() && done())
        {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return false;
    };

    const auto first = service->waitForTableForTesting(
      [](const MaterialTable& t) {
        const auto cube = t.find("textures/lp1b/conflictcube");
        return cube && !cube->entry && t.find("textures/lp1b/reload")
               && t.find("lights/lp1b_reloadlight");
      },
      10s);
    REQUIRE(first);
    CHECK(settle([&] {
      return castOf("textures/lp1b/conflictcube") == CastClass::Opaque
             && castOf("textures/lp1b/reload") == CastClass::Opaque
             && lightRegistry() == first->registry;
    }));

    // the edited decls are read again: a new registry answers every name at once
    version->store(2);
    service->reload();
    const auto second = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto cube = t.find("textures/lp1b/conflictcube");
        return t.registry != first->registry && cube && cube->entry;
      },
      10s);
    REQUIRE(second);
    CHECK(settle([&] {
      return castOf("textures/lp1b/conflictcube") == CastClass::None
             && castOf("textures/lp1b/reload") == CastClass::None
             && lightRegistry() == second->registry;
    }));
    scene.finishForTesting();
  }
  pool->stop();
}

TEST_CASE("SceneGeometry: worldspawn's overrideMaterial is asked for and drawn (V.5)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  auto gate = std::make_shared<ParseGate>();
  auto service = gatedService(gate);
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot =
      snapshotOfMapText(mapText("\"overrideMaterial\" \"textures/lp1b/glass\"\n", ""));
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);
    // nothing in the map names the override; it replaces every side and is translucent
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto drawn = false;
    while (!drawn && std::chrono::steady_clock::now() < deadline)
    {
      scene.collect();
      drawn = !scene.busy() && !scene.chunks().empty()
              && std::all_of(
                scene.chunks().begin(), scene.chunks().end(), [](const auto& entry) {
                  return entry.first.material == "textures/lp1b/glass"
                         && entry.second.castClass == CastClass::None;
                });
      std::this_thread::sleep_for(2ms);
    }
    CHECK(drawn);
    scene.finishForTesting();
  }
  pool->stop();
}

TEST_CASE(
  "SceneGeometry, lights are built once when the .proc comes with the snapshot (V.8)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  const auto game = BakeGame{};
  auto gate = std::make_shared<ParseGate>();
  auto service = game.service(gate);
  const auto atExit = OpenAtExit{gate};
  static_cast<void>(service->lookup("lights/squarelight1"));
  static_cast<void>(service->lookup(DefaultPointLightMaterial));
  static_cast<void>(service->lookup(DefaultProjectedLightMaterial));
  static_cast<void>(service->entityDef("light"));
  REQUIRE(service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.find("lights/squarelight1") && t.find(DefaultPointLightMaterial)
             && t.find(DefaultProjectedLightMaterial) && t.findEntityDef("light");
    },
    10s));
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    // the first snapshot asks for the map's bake (C10); its batch applies before the
    // answer arrives
    closeGateWhenIdle(*service, *gate, "textures/lp1b/v8_gate");
    auto first = snapshotOfMapText(mapText("", ""));
    first.sequence = 1;
    first.mapPath = game.mapPath("lp1b_geometry");
    scene.takeSnapshot(first);
    scene.finishForTesting();
    REQUIRE_FALSE(scene.procTree());
    gate->open();
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto slot = t.findMapBake(*first.mapPath);
        return slot && slot->tree;
      },
      10s));

    // the snapshot that brings a light adopts the tree before the lights take it
    auto snapshot = snapshotOfMapText(mapText(
      "",
      "{\n\"classname\" \"light\"\n\"origin\" \"64 0 96\"\n\"light_radius\" \"200 200 "
      "200\"\n}\n"));
    snapshot.sequence = 2;
    const auto light = lightNodeOf(snapshot);
    scene.takeSnapshot(snapshot);
    scene.finishForTesting();
    REQUIRE(scene.procTree());
    // C31 (LP4.0): the bake's AreaData comes with the tree
    REQUIRE(scene.areaData());
    CHECK(scene.areaData()->areas.size() == 2u);
    const auto record = scene.lightTable()->find(light.entity);
    REQUIRE(record);
    CHECK(record->derived.hasAreaTree);
    CHECK(record->derived.areas.size() == 2u);
    // built once, with the areas, not first without them
    CHECK(record->version == 1u);
  }
  pool->stop();
}

TEST_CASE(
  "SceneGeometry, a .proc that arrives with a snapshot changing a light builds it once "
  "(W.2)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  const auto game = BakeGame{};
  auto gate = std::make_shared<ParseGate>();
  auto service = game.service(gate);
  const auto atExit = OpenAtExit{gate};
  static_cast<void>(service->lookup("lights/squarelight1"));
  static_cast<void>(service->lookup(DefaultPointLightMaterial));
  static_cast<void>(service->lookup(DefaultProjectedLightMaterial));
  static_cast<void>(service->entityDef("light"));
  REQUIRE(service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.find("lights/squarelight1") && t.find(DefaultPointLightMaterial)
             && t.find(DefaultProjectedLightMaterial) && t.findEntityDef("light");
    },
    10s));
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    const auto lightAt = [](const int x) {
      return mapText(
        "",
        "{\n\"classname\" \"light\"\n\"origin\" \"" + std::to_string(x)
          + " 0 96\"\n\"light_radius\" \"200 200 200\"\n}\n");
    };

    // the light is built without the tree, which the parse thread holds back
    closeGateWhenIdle(*service, *gate, "textures/lp1b/w2_gate");
    auto first = snapshotOfMapText(lightAt(64));
    first.sequence = 1;
    first.mapPath = game.mapPath("lp1b_geometry");
    const auto light = lightNodeOf(first);
    scene.takeSnapshot(first);
    scene.finishForTesting();
    REQUIRE(scene.lightTable()->find(light.entity));
    REQUIRE(scene.lightTable()->find(light.entity)->version == 1u);
    CHECK_FALSE(scene.lightTable()->find(light.entity)->derived.hasAreaTree);
    gate->open();
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto slot = t.findMapBake(*first.mapPath);
        return slot && slot->tree;
      },
      10s));

    // the tree arrives in the pass of a snapshot that moves the light: one job, on the
    // new keys and the tree; no record of the old keys published in between
    const auto tablesBefore = scene.lightTable()->version;
    auto moved = snapshotOfMapText(lightAt(-64));
    moved.sequence = 2;
    moved.nodes = {lightNodeOf(moved)};
    scene.takeSnapshot(moved);
    scene.finishForTesting();
    const auto record = scene.lightTable()->find(light.entity);
    REQUIRE(record);
    CHECK(record->version == 2u);
    CHECK(record->derived.hasAreaTree);
    CHECK(record->derived.areaNum == 1);
    CHECK(scene.lightTable()->version == tablesBefore + 1);
  }
  pool->stop();
}

TEST_CASE("SceneGeometry: a good build after a failed one rebuilds everything (W.1)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  // the first build's source cannot be made; reload() builds the fixture decls
  auto builds = std::make_shared<std::atomic<int>>(0);
  auto options = MaterialServiceOptions{};
  options.makeSource = [builds](const std::atomic<bool>&) {
    if (builds->fetch_add(1) == 0)
    {
      throw std::runtime_error{"the first build fails"};
    }
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(fixtureDecls())};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  REQUIRE(service->waitForStatsForTesting(
    [](const MaterialServiceStats& stats) { return stats.failedWorkItems > 0; }, 10s));
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      "{\n\"classname\" \"worldspawn\"\n" + box(0, 0, 0, 32, 32, 32, "lp1b/glass")
      + box(2048, 0, 0, 2080, 32, 32, "lp1b/grate") + "}\n{\n\"classname\" \"light\"\n\"origin\" \"64 64 64\"\n"
        "\"light_radius\" \"64 64 64\"\n\"texture\" \"lights/lp1b_w1\"\n}\n");
    snapshot.sequence = 1;
    const auto light = lightNodeOf(snapshot);
    scene.takeSnapshot(snapshot);

    const auto castOfGlass = [&] {
      for (const auto& [key, chunk] : scene.chunks())
      {
        if (key.material == "textures/lp1b/glass")
        {
          return std::optional{chunk.castClass};
        }
      }
      return std::optional<CastClass>{};
    };
    const auto lightRegistry = [&] {
      const auto record = scene.lightTable()->find(light.entity);
      return record && record->material ? record->material->registry : std::uint64_t(-1);
    };
    // every name answers Failed in the failed build's table: the glass defaults to opaque
    CHECK(settle(scene, [&] {
      const auto glass = service->table()->find("textures/lp1b/glass");
      return glass && !glass->entry && castOfGlass() == CastClass::Opaque;
    }));
    const auto chunksBefore = summaryOf(scene.chunks());
    const auto lightBefore = scene.lightTable()->find(light.entity);
    REQUIRE(lightBefore);

    // a good build: every cell and every light is built again with its decls
    service->reload();
    const auto good = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.registry != 0; }, 10s);
    REQUIRE(good->registry != 0);
    CHECK(settle(scene, [&] {
      return castOfGlass() == CastClass::None && lightRegistry() == good->registry;
    }));
    // both cells: the glass translucent, the grate perforated, where the failed build's
    // defaults made them opaque
    const auto chunksAfter = summaryOf(scene.chunks());
    REQUIRE(chunksBefore.size() == 2u);
    REQUIRE(chunksAfter.size() == chunksBefore.size());
    for (const auto& [key, before] : chunksBefore)
    {
      CAPTURE(key.material);
      REQUIRE(chunksAfter.count(key) == 1u);
      CHECK(before.castClass == CastClass::Opaque);
      CHECK(chunksAfter.at(key).castClass != before.castClass);
    }
    CHECK(scene.lightTable()->find(light.entity)->version > lightBefore->version);
  }
  pool->stop();
}

TEST_CASE("SceneGeometry, a Save As back to an earlier path reads its bake again")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  const auto game = BakeGame{};
  game.writeProc("other", readTextFile(BakeGame::goldenProc()));
  auto gate = std::make_shared<ParseGate>();
  auto service = game.service(gate);
  const auto atExit = OpenAtExit{gate};
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    const auto first = game.mapPath("lp1b_geometry");
    const auto take = [&](const std::string& path, const std::uint64_t sequence) {
      auto snapshot = snapshotOfMapText(mapText("", ""));
      snapshot.sequence = sequence;
      snapshot.mapPath = path;
      scene.takeSnapshot(snapshot);
    };
    const auto nodesOf = [&] {
      return scene.procTree() ? scene.procTree()->nodes.size() : std::size_t{0};
    };

    take(first, 1);
    CHECK(settle(scene, [&] { return nodesOf() == 39u; }));
    // Save As: another path and its bake
    take(game.mapPath("other"), 2);
    CHECK(settle(scene, [&] { return nodesOf() == 39u && scene.procTree(); }));

    // dmap runs again on the first map, then a Save As goes back to it: the slot read
    // before is in the table, but only the answer to the new request is adopted
    game.writeProc(
      "lp1b_geometry",
      "mapProcFile004\n\ninterAreaPortals { 2 0 }\n\nnodes { 1\n( 1 0 0 0 ) -1 -2\n}\n");
    closeGateWhenIdle(*service, *gate, "textures/lp1b/save_as_gate");
    take(first, 3);
    scene.finishForTesting();
    CHECK_FALSE(scene.procTree());
    gate->open();
    CHECK(settle(scene, [&] { return nodesOf() == 1u; }));
  }
  pool->stop();
}

TEST_CASE("SceneGeometry, the entities' area references (LP4.0, C31, D80, P4.9, P4.11)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  const auto game = BakeGame{};
  auto gate = std::make_shared<ParseGate>();
  auto service = game.service(gate);
  const auto atExit = OpenAtExit{gate};
  auto pool = CpuPool::create() | kdl::value();

  // the entity whose name key is name
  const auto idNamed = [](const Snapshot& snapshot, const std::string& name) {
    for (const auto& entity : snapshot.entities)
    {
      for (auto k = entity.keys.first; k < entity.keys.first + entity.keys.count; ++k)
      {
        if (
          snapshot.string(snapshot.keyValues[k].key) == "name"
          && snapshot.string(snapshot.keyValues[k].value) == name)
        {
          return entity.id;
        }
      }
    }
    return NoPreviewId;
  };
  const auto setOf = [](const std::vector<int>& areas) {
    return std::set<int>{areas.begin(), areas.end()};
  };

  // the fixture map and a door across the doorway's portal plane x = 8: world x -8..24,
  // y -16..16, z 16..48, its origin (8 0 32) the pivot (model space -16..16 each way)
  const auto fixture = readTextFile(litFixtureFolder() / "maps" / "lp1b_geometry.map");
  const auto doorHead = std::string{
    "{\n\"classname\" \"func_static\"\n\"name\" \"lp4_door\"\n\"model\" \"lp4_door\"\n"
    "\"origin\" \"8 0 32\"\n"};
  const auto doorBrush = box(-8, -16, 16, 24, 16, 48, "lp1b/crate");
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(fixture + doorHead + doorBrush + "}\n");
    snapshot.sequence = 1;
    snapshot.mapPath = game.mapPath("lp1b_geometry");
    const auto rotator = idNamed(snapshot, "lp1b_rotator");
    const auto scaled = idNamed(snapshot, "lp1b_scaled");
    const auto door = idNamed(snapshot, "lp4_door");
    REQUIRE(rotator != NoPreviewId);
    REQUIRE(scaled != NoPreviewId);
    REQUIRE(door != NoPreviewId);
    scene.takeSnapshot(snapshot);
    const auto referenced = [&](const PreviewId id) {
      const auto it = scene.entityStates().find(id);
      return it != scene.entityStates().end() && !it->second.areas.empty();
    };
    REQUIRE(settle(scene, [&] {
      return scene.procTree() && scene.areaData() && referenced(rotator)
             && referenced(scaled) && referenced(door);
    }));

    // lp1b_rotator: its .proc model's bounds (-28 -28 -4)..(4 4 28) about the pivot
    // (140 76 20) through the axis rows (0 1 0) (-1 0 0) (0 0 1): world x 140 - y_local
    // 136..168, y 76 + x_local 48..80, z 16..48, inside the east room (x > 8): area 0
    const auto& r = scene.entityStates().at(rotator);
    CHECK(
      r.localReferenceBounds
      == EngineBounds{EngineVec3{-28, -28, -4}, EngineVec3{4, 4, 28}});
    CHECK(
      r.globalReferenceBounds
      == EngineBounds{EngineVec3{136, 48, 16}, EngineVec3{168, 80, 48}});
    CHECK(r.areas == std::vector<int>{0});
    REQUIRE(r.materialBounds.size() == 1u);
    CHECK(r.materialBounds.at("textures/lp1b/crate") == r.localReferenceBounds);
    CHECK(
      r.inverseBaseModelProject
      == engineOffsetScaleForBounds(
        engineMatrixFromOriginAxis(r.modelOrigin, r.parms.axis), r.localReferenceBounds));

    // lp1b_scaled: (-16..16) scaled by 2 about (-128 -64 32): world x -160..-96, y
    // -96..-32, z 0..64, in the west room (x < 8): area 1; its bottom face on the floor
    // plane z = 0 pushes nothing into the solid below
    const auto& s = scene.entityStates().at(scaled);
    CHECK(
      s.globalReferenceBounds
      == EngineBounds{EngineVec3{-160, -96, 0}, EngineVec3{-96, -32, 64}});
    CHECK(s.areas == std::vector<int>{1});

    // the door straddles the portal: both areas, in the engine's chain order, the reverse
    // of the push order (AddEntityRefToArea prepends)
    const auto& d = scene.entityStates().at(door);
    CHECK(setOf(d.areas) == std::set<int>{0, 1});
    auto pushed = pushFrustumIntoTree(
      *scene.procTree(),
      scene.areaData()->commonAreas,
      d.inverseBaseModelProject,
      engineUnitCube());
    std::ranges::reverse(pushed);
    CHECK(d.areas == pushed);
    // P4.9: the door's chunks keep the pivot's area as a key only
    for (const auto& [key, chunk] : scene.chunks())
    {
      if (key.entity == door)
      {
        CHECK(key.area == pointInArea(*scene.procTree(), d.modelOrigin));
      }
    }

    // a brush in another 1024 cell of another material: the union over every chunk of
    // the entity grows, a second material bounds appears, the references stay (the new
    // brush is in solid, outside the baked hull)
    auto grown = snapshotOfMapText(
      fixture + doorHead + doorBrush + box(1100, -16, 16, 1132, 16, 48, "lp1b/wall")
      + "}\n");
    grown.sequence = 2;
    grown.mapPath = snapshot.mapPath;
    scene.takeSnapshot(grown);
    REQUIRE(settle(scene, [&] {
      const auto it = scene.entityStates().find(door);
      return it != scene.entityStates().end() && it->second.materialBounds.size() == 2u;
    }));
    const auto& g = scene.entityStates().at(door);
    CHECK(
      g.materialBounds.at("textures/lp1b/crate")
      == EngineBounds{EngineVec3{-16, -16, -16}, EngineVec3{16, 16, 16}});
    CHECK(
      g.materialBounds.at("textures/lp1b/wall")
      == EngineBounds{EngineVec3{1092, -16, -16}, EngineVec3{1124, 16, 16}});
    CHECK(
      g.localReferenceBounds
      == EngineBounds{EngineVec3{-16, -16, -16}, EngineVec3{1124, 16, 16}});
    CHECK(setOf(g.areas) == std::set<int>{0, 1});

    // a removed entity's state goes
    auto removed = Snapshot{};
    removed.sequence = 3;
    removed.removed.push_back(door);
    for (const auto& node : grown.nodes)
    {
      if (node.entity == door)
      {
        removed.removed.push_back(node.id);
      }
    }
    scene.takeSnapshot(removed);
    scene.finishForTesting();
    CHECK_FALSE(scene.entityStates().contains(door));
    CHECK(scene.entityStates().contains(rotator));
  }

  // without a tree (no material service, no bake): the bounds, no references, the
  // entity's chunks in area 0 (D94)
  {
    auto scene = SceneGeometry{pool.get(), nullptr};
    auto snapshot = snapshotOfMapText(mapText("", doorHead + doorBrush + "}\n"));
    snapshot.sequence = 1;
    const auto door = idNamed(snapshot, "lp4_door");
    scene.takeSnapshot(snapshot);
    scene.finishForTesting();
    REQUIRE(scene.entityStates().contains(door));
    const auto& d = scene.entityStates().at(door);
    CHECK(
      d.localReferenceBounds
      == EngineBounds{EngineVec3{-16, -16, -16}, EngineVec3{16, 16, 16}});
    CHECK(d.areas.empty());
    CHECK_FALSE(scene.areaData());
    for (const auto& [key, chunk] : scene.chunks())
    {
      CHECK(key.area == 0);
    }
  }
  pool->stop();
}

TEST_CASE(
  "ChunkBuilder: every patch triangle's corners through TriVertsFromOriginal (R3.5)")
{
  // V.9: a patch off every grid, where a corner kept whole and one through
  // TriVertsFromOriginal differ (the fixture's patch is on a grid, where they agree)
  const auto text = std::string{
    "{\n\"classname\" \"worldspawn\"\n{\npatchDef2\n{\n\"lp1b/patch\"\n( 3 3 0 0 0 )\n(\n"
    "( ( 0.37 1.11 0.13 0.031 0.017 ) ( 33.7 9.29 17.3 0.517 0.093 ) "
    "( 64.1 2.03 0.71 1.037 0.011 ) )\n"
    "( ( 1.3 31.7 33.9 0.023 0.511 ) ( 31.9 41.3 49.7 0.497 0.529 ) "
    "( 63.3 33.1 31.7 1.019 0.503 ) )\n"
    "( ( 0.91 64.3 0.29 0.013 1.021 ) ( 32.3 57.1 17.9 0.509 1.003 ) "
    "( 63.7 65.9 0.47 1.031 1.017 ) )\n"
    ")\n}\n}\n}\n"};
  const auto snapshot = snapshotOfMapText(text);
  const auto output = buildChunks(inputOf(snapshot));
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  // dmap's corners: every triangle of the subdivided patch through TriVertsFromOriginal,
  // at the position the chunk reads back (R3.9); and each vertex as it is, kept whole
  REQUIRE(snapshot.patches.size() == 1u);
  const auto& sp = snapshot.patches.front();
  auto patch = EnginePatch{};
  patch.width = int(sp.rows);
  patch.height = int(sp.columns);
  patch.controlPoints.resize(sp.controlPoints.count);
  for (auto r = 0u; r < sp.rows; ++r)
  {
    for (auto c = 0u; c < sp.columns; ++c)
    {
      const auto& p = snapshot.controlPoints[sp.controlPoints.first + r * sp.columns + c];
      auto vert = engineClearedVert();
      vert.xyz = {float(p[0]), float(p[1]), float(p[2])};
      engineSetTexCoord(vert, float(p[3]), float(p[4]));
      patch.controlPoints[c * sp.rows + r] = vert;
    }
  }
  const auto surface = engineSubdividePatch(patch);
  REQUIRE(surface);
  const auto keyOf = [](const EngineVec3& xyz) {
    return EngineVec3{procReadBack(xyz[0]), procReadBack(xyz[1]), procReadBack(xyz[2])};
  };
  auto barycentric = std::map<EngineVec3, std::vector<EngineVec2>>{};
  auto whole = std::map<EngineVec3, std::vector<EngineVec2>>{};
  for (auto i = std::size_t{0}; i + 2 < surface->indexes.size(); i += 3)
  {
    const auto original = std::array<EngineDrawVert, 3>{
      surface->verts[std::size_t(surface->indexes[i])],
      surface->verts[std::size_t(surface->indexes[i + 1])],
      surface->verts[std::size_t(surface->indexes[i + 2])]};
    for (const auto& corner : original)
    {
      const auto key = keyOf(corner.xyz);
      barycentric[key].push_back(
        engineTexCoord(engineVertFromOriginal(corner.xyz, original)));
      whole[key].push_back(engineTexCoord(corner));
    }
  }
  const auto congruent = [](const EngineVec2& a, const std::vector<EngineVec2>& list) {
    return std::any_of(list.begin(), list.end(), [&](const EngineVec2& b) {
      const auto ds = a[0] - b[0];
      const auto dt = a[1] - b[1];
      return ds == std::round(ds) && dt == std::round(dt);
    });
  };
  auto checked = 0;
  auto onlyBarycentric = 0;
  for (const auto& chunk : output.chunks)
  {
    for (const auto& vert : chunk.verts)
    {
      const auto it = barycentric.find(vert.xyz);
      REQUIRE(it != barycentric.end());
      const auto st = engineTexCoord(vert);
      CHECK(congruent(st, it->second));
      onlyBarycentric += congruent(st, whole.at(vert.xyz)) ? 0 : 1;
      ++checked;
    }
  }
  CHECK(checked > 20);
  // the test would fail with corners kept whole
  CHECK(onlyBarycentric > 0);
}

namespace
{

// LPF.0b: a service over the fixture decls and one stage-less material of the test's
std::unique_ptr<MaterialService> reparseSceneService()
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    auto decls = fixtureDecls();
    decls.push_back(DeclFile{
      "materials/zz_lpf_test.mtr", "textures/lpf/plain\n{\n\tnoshadows\n}\n", 0});
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::move(decls))};
  };
  options.pollInterval = std::chrono::milliseconds{0};
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// the service idle and the scene's batches applied with its latest table
bool quiet(MaterialService& service, SceneGeometry& scene)
{
  return service.waitForStatsForTesting(
           [](const auto& s) { return s.idle; }, std::chrono::seconds{10})
         && settle(scene, [&] { return !scene.waitsForMaterials(); });
}

// a re-parse, its table published and the scene quiet again
bool reparsed(
  MaterialService& service,
  SceneGeometry& scene,
  const std::string& name,
  const std::string& text)
{
  if (!service.reparseMaterial(name, text).is_success())
  {
    return false;
  }
  const auto requested = service.feedRequested();
  service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.feedApplied >= requested; },
    std::chrono::seconds{10});
  return quiet(service, scene);
}

const Chunk* chunkOf(const SceneGeometry& scene, const std::string& material)
{
  for (const auto& [key, chunk] : scene.chunks())
  {
    if (key.material == material)
    {
      return &chunk;
    }
  }
  return nullptr;
}

std::string reparseSceneMap()
{
  return "{\n\"classname\" \"worldspawn\"\n" + box(0, 0, 0, 64, 64, 64, "lp1b/wall")
         + box(1100, 0, 0, 1132, 32, 32, "lp1b/crate")
         + box(2100, 0, 0, 2132, 32, 32, "lp1b/test_nodraw") + "}\n"
         + "{\n\"classname\" \"func_static\"\n\"name\" \"a\"\n\"model\" \"a\"\n"
         + box(3100, 0, 0, 3132, 32, 32, "lp1b/floor")
         + box(4100, 0, 0, 4132, 32, 32, "lp1b/floor")
         + box(5200, 0, 0, 5216, 16, 16, "lpf/plain") + "}\n";
}

const auto CrateText = std::string{
  "textures/lp1b/crate\n{\n\tunsmoothedTangents\n\tqer_editorimage\ttextures/lp1b/"
  "crate.tga\n\tdiffusemap\ttextures/lp1b/crate.tga\n}\n"};
const auto TwoSidedCrateText = std::string{
  "textures/lp1b/crate\n{\n\tunsmoothedTangents\n\ttwoSided\n\tqer_editorimage\t"
  "textures/lp1b/crate.tga\n\tdiffusemap\ttextures/lp1b/crate.tga\n}\n"};

} // namespace

TEST_CASE(
  "SceneGeometry: a re-parsed material builds its cells again (LPF.0b, C43, D106)")
{
  using namespace std::chrono_literals;
  static_cast<void>(fixtureTable());
  auto service = reparseSceneService();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(reparseSceneMap());
    snapshot.sequence = 1;
    const auto world = snapshot.entities[0].id;
    const auto funcId = snapshot.entities[1].id;
    scene.takeSnapshot(snapshot);
    REQUIRE(quiet(*service, scene));

    const auto* crate = chunkOf(scene, "textures/lp1b/crate");
    const auto* wall = chunkOf(scene, "textures/lp1b/wall");
    REQUIRE(crate);
    REQUIRE(wall);
    const auto crateCell = CellRef{crate->key.entity, crate->key.cell};
    const auto crateIndexes = crate->indexes.size();
    const auto* wallVerts = wall->verts.data();
    CHECK_FALSE(chunkOf(scene, "textures/lp1b/test_nodraw"));
    REQUIRE(scene.entityStates().contains(funcId));
    CHECK(scene.entityStates().at(funcId).originOffset == EngineVec3{0, 0, 0});

    SECTION("twoSided: its cells built again with back faces, nothing else")
    {
      REQUIRE(reparsed(*service, scene, "textures/lp1b/crate", TwoSidedCrateText));
      const auto* rebuilt = chunkOf(scene, "textures/lp1b/crate");
      REQUIRE(rebuilt);
      // createBackSides: a reversed copy of every triangle
      CHECK(rebuilt->indexes.size() == 2 * crateIndexes);
      CHECK(scene.lastBatchCellsForTesting() == std::set<CellRef>{crateCell});
      // the wall's chunk is the one built before
      CHECK(chunkOf(scene, "textures/lp1b/wall")->verts.data() == wallVerts);

      // an image override changes no revision: nothing is built again
      auto image = ImageOverride{};
      auto pixels = RawImagePixels{};
      pixels.width = 2;
      pixels.height = 2;
      pixels.bytes.assign(16, 200);
      image.image = std::move(pixels);
      const auto before = service->table()->materialsRevision;
      const auto* crateVerts = rebuilt->verts.data();
      REQUIRE(
        service->setImageOverride("textures/lp1b/crate", std::move(image)).is_success());
      const auto requested = service->feedRequested();
      const auto table = service->waitForTableForTesting(
        [&](const MaterialTable& t) { return t.feedApplied >= requested; }, 10s);
      CHECK(table->materialsRevision == before);
      REQUIRE(quiet(*service, scene));
      CHECK(chunkOf(scene, "textures/lp1b/crate")->verts.data() == crateVerts);
    }

    SECTION("a stage-less material given a stage: its cells built, a chunk appears")
    {
      REQUIRE(reparsed(
        *service,
        scene,
        "textures/lp1b/test_nodraw",
        "{\n\tnoshadows\n\tdiffusemap textures/lp1b/crate.tga\n}\n"));
      const auto* appeared = chunkOf(scene, "textures/lp1b/test_nodraw");
      REQUIRE(appeared);
      CHECK(
        scene.lastBatchCellsForTesting()
        == std::set<CellRef>{CellRef{appeared->key.entity, appeared->key.cell}});
      CHECK(chunkOf(scene, "textures/lp1b/wall")->verts.data() == wallVerts);
    }

    SECTION("a material gaining CONTENTS_ORIGIN: its entity built again whole")
    {
      REQUIRE(
        reparsed(*service, scene, "textures/lpf/plain", "{\n\torigin\n\tnoshadows\n}\n"));
      // the pivot is the origin brush's centre now
      CHECK(scene.entityStates().at(funcId).originOffset == EngineVec3{5208, 8, 8});
      auto entityCells = std::set<CellRef>{};
      for (const auto& ref : scene.lastBatchCellsForTesting())
      {
        CHECK(ref.entity == funcId);
        entityCells.insert(ref);
      }
      // its three cells, the floor's two and the origin brush's
      CHECK(entityCells.size() == 3u);
      CHECK(chunkOf(scene, "textures/lp1b/wall")->verts.data() == wallVerts);
      static_cast<void>(world);

      // and back
      REQUIRE(reparsed(*service, scene, "textures/lpf/plain", "{\n\tnoshadows\n}\n"));
      CHECK(scene.entityStates().at(funcId).originOffset == EngineVec3{0, 0, 0});
    }
    scene.finishForTesting();
    static_cast<void>(scene.takeWarnings());
  }
  pool->stop();
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("SceneGeometry: 100 re-parses with the scene leave the heap (LPF.0b)")
{
  static_cast<void>(fixtureTable());
  auto service = reparseSceneService();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(reparseSceneMap());
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);
    REQUIRE(quiet(*service, scene));
    const auto cycle = [&] {
      REQUIRE(reparsed(*service, scene, "textures/lp1b/crate", TwoSidedCrateText));
      REQUIRE(reparsed(*service, scene, "textures/lp1b/crate", CrateText));
      static_cast<void>(scene.takeWarnings());
      static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    };
    // LPF.1 (C44): the warm-up fills the tables' feed records to their bound
    // (MaterialTable::FeedRecordsKept), two items a cycle
    for (auto i = 0; i < int(MaterialTable::FeedRecordsKept / 2) + 2; ++i)
    {
      cycle();
    }
    // the pool's and the service's queues may hold a few blocks more or less at either
    // checkpoint; a leak in any of the 100 re-parses shows 50 times or more
    const auto growth = heapGrowth([&] {
      for (auto i = 0; i < 50; ++i)
      {
        cycle();
      }
    });
    CAPTURE(growth.blocks, growth.bytes);
    CHECK(growth.blocks <= 64);
    CHECK(growth.bytes <= 16384);
    scene.finishForTesting();
  }
  pool->stop();
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}
#endif

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

TEST_CASE("MaterialService, map bakes on the parse thread (C10, D64)")
{
  using namespace std::chrono_literals;
  const auto game = BakeGame{};
  auto gate = std::make_shared<ParseGate>();
  auto service = game.service(gate, 20ms);
  const auto atExit = OpenAtExit{gate};

  const auto path = game.mapPath("lp1b_geometry");
  const auto missing = game.mapPath("no_such_map");
  const auto outside = (game.base().parent_path() / "outside.map").string();
  const auto ticket = service->loadMapBake(path);
  CHECK(service->loadMapBake(missing) > ticket);
  static_cast<void>(service->loadMapBake(outside));
  const auto table = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.findMapBake(path) && t.findMapBake(missing) && t.findMapBake(outside);
    },
    10s);
  const auto found = table->findMapBake(path);
  REQUIRE(found);
  CHECK(found->ticket == ticket);
  REQUIRE(found->tree);
  CHECK(found->tree->nodes.size() == 39u);
  CHECK(found->tree->numAreas == 2);
  REQUIRE(found->bake);
  CHECK(found->bake->geometry == MapBake::Geometry::Proc);
  CHECK(found->bake->mapName == "maps/lp1b_geometry");
  const auto none = table->findMapBake(missing);
  REQUIRE(none);
  CHECK_FALSE(none->tree);
  CHECK_THAT(none->error, Catch::Matchers::ContainsSubstring("no maps/no_such_map.proc"));
  const auto away = table->findMapBake(outside);
  REQUIRE(away);
  CHECK_FALSE(away->bake);
  CHECK_THAT(
    away->error, Catch::Matchers::ContainsSubstring("not below the game's base"));

  // asked again with no file changed: a new ticket, the same tree
  const auto again = service->loadMapBake(path);
  const auto same = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.findMapBake(path);
      return slot && slot->ticket == again;
    },
    10s);
  CHECK(same->findMapBake(path)->tree == found->tree);

  // a reload keeps them
  service->reload();
  const auto reloaded = service->waitForTableForTesting(
    [&](const MaterialTable& t) { return t.registry != table->registry; }, 10s);
  CHECK(reloaded->findMapBake(path));

  // a new dmap run is seen by the poll: the tree is read again under the same ticket
  game.writeProc(
    "lp1b_geometry",
    "mapProcFile004\n\ninterAreaPortals { 2 0 }\n\nnodes { 1\n( 1 0 0 0 ) -1 -2\n}\n");
  bumpModificationTime(game.base() / "maps" / "lp1b_geometry.proc");
  const auto polled = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.findMapBake(path);
      return slot && slot->tree && slot->tree->nodes.size() == 1u;
    },
    10s);
  CHECK(polled->findMapBake(path)->ticket == again);
  service->requestStop();
  service->join();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
