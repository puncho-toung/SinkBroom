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

// LP4.2a step 3 (C37, D87, P4.2, P4.10, P4.18): the environment probes as the game spawns
// them (PopulateEnvironmentProbes, Game_local.cpp:1275-1336) and the renderer derives
// them (R_DeriveEnvprobeData, RenderWorld_defs.cpp:808-941), and the registry's
// publication.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/AreaData.h"
#include "lit/CpuPool.h"
#include "lit/GameDeclSource.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/LightGridFile.h"
#include "lit/MapBake.h"
#include "lit/MaterialService.h"
#include "lit/ProbeRecord.h"
#include "lit/ProcFile.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

void drain()
{
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

std::shared_ptr<const ProcTree> procOf(const std::string& text)
{
  auto tree =
    readProcFile(text) | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  drain();
  return std::make_shared<const ProcTree>(std::move(tree));
}

std::shared_ptr<const ProcTree> fixtureTree()
{
  return procOf(readTextFile(
    litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"));
}

// a .proc of two areas split by the plane x = 0 (x > 0 area 0), _area0 one triangle in
// x 16..64, _area1 without surfaces; with solidWest the side x <= 0 is solid instead
std::shared_ptr<const ProcTree> surfacelessProc(const bool solidWest)
{
  return procOf(
    "mapProcFile004\n"
    "model { \"_area0\" 1\n"
    "{ \"textures/test/plain\" 3 3 0\n"
    "( 16 0 0 0 0 0 0 1 ) ( 64 0 0 0 0 0 0 1 ) ( 16 32 16 0 0 0 0 1 )\n"
    "0 1 2 }\n"
    "}\n"
    "model { \"_area1\" 0 }\n"
    "interAreaPortals { 2 0 }\n"
    + std::string{
      solidWest ? "nodes { 1 ( 1 0 0 0 ) -1 0 }\n" : "nodes { 1 ( 1 0 0 0 ) -1 -2 }\n"});
}

ProbeJobInput inputOf(
  std::shared_ptr<const ProcTree> tree,
  const std::string& bakeBase,
  const EnvProbeDefState def = EnvProbeDefState::Found)
{
  auto input = ProbeJobInput{};
  input.areas = std::make_shared<const AreaData>(buildAreaData(*tree));
  input.tree = std::move(tree);
  input.bakeBase = bakeBase;
  input.envProbeDef = def;
  return input;
}

ShProbe shEntry(const int area, const EngineVec3& origin, const float first)
{
  auto entry = ShProbe{};
  entry.area = area;
  entry.origin = origin;
  for (std::size_t i = 0; i < entry.sh.size(); ++i)
  {
    entry.sh[i] = first + float(i);
  }
  return entry;
}

} // namespace

TEST_CASE("ProbeRecord: SnapInt (Vector.h:860-864)")
{
  CHECK(engineSnapInt({132.9f, -124.5f, 64.0f}) == EngineVec3{132.0f, -124.0f, 64.0f});
  CHECK(engineSnapInt({-0.75f, 0.75f, 0.0f})[0] == 0.0f);
  // a guard: a value beyond an int is left as it is
  CHECK(engineSnapInt({1e30f, 0, 0})[0] == 1e30f);
}

TEST_CASE("ProbeRecord: the fixture tree's automatic probes (C37, P4.10)")
{
  const auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
  REQUIRE(input.areas->areas.size() == 2u);
  const auto output = buildProbeRecords(input);
  CHECK(output.warnings.empty());
  CHECK(output.automaticSkipped == 0u);
  REQUIRE(output.records.size() == 2u);

  // area 0 (8 -128 0)..(256 128 128) and area 1 (-256 -128 0)..(8 128 128) (LP4.0's
  // golden): the snapped centres (132 0 64) and (-124 0 64)
  const auto& east = output.records[0];
  const auto& west = output.records[1];
  CHECK(east.automatic);
  CHECK(east.id == automaticProbeId(0));
  CHECK(east.spawnArea == 0);
  CHECK(east.origin == EngineVec3{132.0f, 0.0f, 64.0f});
  CHECK(east.areaNum == 0);
  CHECK(east.point == std::array<int, 3>{132, 0, 64});
  CHECK(east.globalProbeBounds == input.areas->areas[0].globalBounds);
  CHECK(west.origin == EngineVec3{-124.0f, 0.0f, 64.0f});
  CHECK(west.areaNum == 1);
  CHECK(west.id == automaticProbeId(1));
  CHECK(west.globalProbeBounds == input.areas->areas[1].globalBounds);
  // no .shprobes: the SH is zero
  CHECK_FALSE(east.shFound);
  CHECK(east.shIrradiance == std::array<EngineVec3, 9>{});

  // the image keys of RenderWorld_defs.cpp:853-867, '-' made '_' (the west probe's x)
  CHECK(east.images[0].name == "env/maps/lp1b_geometry/area0_envprobe_132_0_64_amb");
  CHECK(east.images[1].name == "env/maps/lp1b_geometry/area0_envprobe_132_0_64_spec");
  CHECK(east.images[2].name == "env/maps/lp1b_geometry/area0_envprobe_132_0_64_depth");
  CHECK(west.images[1].name == "env/maps/lp1b_geometry/area1_envprobe__124_0_64_spec");
  CHECK(west.images[2].name == "env/maps/lp1b_geometry/area1_envprobe__124_0_64_depth");
  CHECK(east.images[1].usage == 21);
  CHECK(east.images[1].cube == 8);
  CHECK(east.images[2].usage == 22);

  // inverseBaseProbeProject: the unit cube onto the doubled bounds, from the identity
  const auto& bounds = east.globalProbeBounds;
  auto doubled = bounds;
  for (std::size_t i = 0; i < 3; ++i)
  {
    doubled[0][i] = bounds[0][i] * 2;
    doubled[1][i] = bounds[1][i] * 2;
  }
  CHECK(
    east.inverseBaseProbeProject
    == engineOffsetScaleForBounds(engineIdentityMatrix(), doubled));
  std::cout << fmt::format(
    "LP4.2a fixture probes: ({} {} {}) area {}, ({} {} {}) area {}\n",
    east.origin[0],
    east.origin[1],
    east.origin[2],
    east.areaNum,
    west.origin[0],
    west.origin[1],
    west.origin[2],
    west.areaNum);
}

TEST_CASE("ProbeRecord: an area without surfaces spawns at the origin (P4.10)")
{
  SECTION("the origin in an area: the probe is kept")
  {
    const auto input = inputOf(surfacelessProc(false), "maps/test/surfaceless");
    // Zero() model bounds: the round trip keeps the point at the origin
    CHECK(input.areas->areas[1].globalBounds == EngineBounds{});
    const auto output = buildProbeRecords(input);
    REQUIRE(output.records.size() == 2u);
    CHECK(output.records[0].origin == EngineVec3{40.0f, 16.0f, 8.0f});
    CHECK(output.records[0].areaNum == 0);
    // the plane x = 0 itself goes to child 1: area 1
    CHECK(output.records[1].origin == EngineVec3{0.0f, 0.0f, 0.0f});
    CHECK(output.records[1].areaNum == 1);
    CHECK(output.records[1].globalProbeBounds == EngineBounds{});
    CHECK(output.warnings.empty());
  }

  SECTION("the origin in solid: skipped with the engine's warning")
  {
    const auto input = inputOf(surfacelessProc(true), "maps/test/surfaceless");
    const auto output = buildProbeRecords(input);
    REQUIRE(output.records.size() == 1u);
    CHECK(output.records[0].areaNum == 0);
    CHECK(output.automaticSkipped == 1u);
    REQUIRE(output.warnings.size() == 1u);
    CHECK(
      output.warnings[0]
      == "PopulateEnvironmentProbes: location '1' is not in a valid area");
  }
}

TEST_CASE("ProbeRecord: placed probes and the entityDef (C37, P4.2)")
{
  SECTION("one placed probe switches every automatic one off")
  {
    auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
    input.placed.push_back(ProbeEntity{
      42,
      EntityKeys{
        {"classname", "env_probe"},
        {"origin", "-100.5 20.75 33"},
        {"_color", "0.5 0.25 1"},
        {"spawnclass", "EnvironmentProbe"}}});
    const auto output = buildProbeRecords(input);
    REQUIRE(output.records.size() == 1u);
    const auto& probe = output.records[0];
    CHECK_FALSE(probe.automatic);
    CHECK(probe.id == 42u);
    CHECK(probe.spawnArea == -1);
    // the parsed origin (M-PROBE-DIFF-01), its snapped point and area
    CHECK(probe.origin == EngineVec3{-100.5f, 20.75f, 33.0f});
    CHECK(probe.point == std::array<int, 3>{-100, 20, 33});
    CHECK(probe.areaNum == 1);
    CHECK(
      probe.images[1].name == "env/maps/lp1b_geometry/area1_envprobe__100_20_33_spec");
    // ParseSpawnArgsToRenderEnvprobe's parms, kept
    CHECK(probe.shaderParms[0] == 0.5f);
    CHECK(probe.shaderParms[1] == 0.25f);
    CHECK(probe.shaderParms[2] == 1.0f);
    CHECK(probe.shaderParms[3] == 1.0f);
    CHECK(probe.shaderParms[4] == 0.0f);
  }

  SECTION("a placed probe outside the world: area -1, bounds cleared")
  {
    auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
    input.placed.push_back(ProbeEntity{7, EntityKeys{{"origin", "5000 0 0"}}});
    const auto output = buildProbeRecords(input);
    REQUIRE(output.records.size() == 1u);
    CHECK(output.records[0].areaNum == -1);
    CHECK(engineBoundsIsCleared(output.records[0].globalProbeBounds));
    CHECK(
      output.records[0].images[1].name
      == "env/maps/lp1b_geometry/area_1_envprobe_5000_0_0_spec");
  }

  SECTION("the env_probe entityDef missing: no probes, no message (the engine's Printf)")
  {
    const auto input =
      inputOf(fixtureTree(), "maps/lp1b_geometry", EnvProbeDefState::Missing);
    const auto output = buildProbeRecords(input);
    CHECK(output.records.empty());
    CHECK(output.warnings.empty());
    CHECK(buildProbeRecords(inputOf(fixtureTree(), "x", EnvProbeDefState::Pending))
            .records.empty());
  }

  SECTION("no tree: no automatic probes")
  {
    auto input = ProbeJobInput{};
    input.envProbeDef = EnvProbeDefState::Found;
    CHECK(buildProbeRecords(input).records.empty());
  }
}

TEST_CASE("ProbeRecord: the .shprobes match (RenderWorld_defs.cpp:836-851, P4.18)")
{
  auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
  auto entries = std::vector<ShProbe>{
    // another area at the east probe's point: not it
    shEntry(1, {132.0f, 0.0f, 64.0f}, 100.0f),
    // the first entry of area 0 whose int x, y, z are 132 0 64 wins
    shEntry(0, {132.9f, 0.4f, 64.8f}, 200.0f),
    shEntry(0, {132.0f, 0.0f, 64.0f}, 300.0f),
    // the west probe's point
    shEntry(1, {-124.0f, 0.0f, 64.0f}, 400.0f),
  };
  input.shProbes = std::make_shared<const std::vector<ShProbe>>(std::move(entries));
  const auto output = buildProbeRecords(input);
  REQUIRE(output.records.size() == 2u);
  CHECK(output.records[0].shFound);
  CHECK(output.records[0].shIrradiance[0] == EngineVec3{200.0f, 201.0f, 202.0f});
  CHECK(output.records[0].shIrradiance[8] == EngineVec3{224.0f, 225.0f, 226.0f});
  CHECK(output.records[1].shFound);
  CHECK(output.records[1].shIrradiance[0] == EngineVec3{400.0f, 401.0f, 402.0f});
}

TEST_CASE("ProbeRegistry: the table's version moves only when a record changes (D87)")
{
  auto publication = std::make_shared<ProbeTablePublication>();
  auto registry = ProbeRegistry{nullptr, publication};
  CHECK(registry.table()->version == 0u);
  CHECK(registry.table()->records.empty());

  auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
  input.provisional = true;
  registry.update(input);
  registry.finishForTesting();
  const auto first = publication->table();
  CHECK(first->version == 1u);
  CHECK(first->records.size() == 2u);
  CHECK(first->provisional);
  CHECK(first->envProbeDef == EnvProbeDefState::Found);

  // the same input: no build
  registry.update(input);
  CHECK_FALSE(registry.busy());
  CHECK(publication->table() == first);

  // the same records, the entityDefs answered: a new table of the same version
  input.provisional = false;
  registry.update(input);
  registry.finishForTesting();
  CHECK(publication->table() != first);
  CHECK(publication->table()->version == 1u);
  CHECK_FALSE(publication->table()->provisional);

  // a .shprobes that matches: the records changed
  input.shProbes = std::make_shared<const std::vector<ShProbe>>(
    std::vector<ShProbe>{shEntry(0, {132.0f, 0.0f, 64.0f}, 1.0f)});
  registry.update(input);
  registry.finishForTesting();
  CHECK(publication->table()->version == 2u);
  CHECK(publication->table()->records[0].shFound);

  // a placed probe
  input.placed.push_back(ProbeEntity{9, EntityKeys{{"origin", "100 0 32"}}});
  registry.update(input);
  registry.finishForTesting();
  CHECK(publication->table()->version == 3u);
  CHECK(publication->table()->records.size() == 1u);
  CHECK(publication->table()->placed == 1u);
  CHECK(registry.takeWarnings().empty());
}

TEST_CASE("SceneGeometry: the placed probes by their merged spawnclass (C37, C22)")
{
  using namespace std::chrono_literals;
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
        "def/_rbdoom_env_probe.def",
        "entityDef env_probe\n{\n\t\"editor_color\" \"0 0 1\"\n\t\"spawnclass\" "
        "\"EnvironmentProbe\"\n}\n"
        "entityDef sb_probe_child\n{\n\t\"inherit\" \"env_probe\"\n}\n"
        "entityDef info_null\n{\n\t\"spawnclass\" \"idEntity\"\n}\n",
        0}})};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      "{\n\"classname\" \"worldspawn\"\n}\n"
      "{\n\"classname\" \"env_probe\"\n\"origin\" \"10 20 30\"\n}\n"
      "{\n\"classname\" \"info_null\"\n\"origin\" \"0 0 0\"\n}\n"
      "{\n\"classname\" \"sb_probe_child\"\n\"origin\" \"-5 0 0\"\n}\n"
      "{\n\"classname\" \"info_null\"\n\"spawnclass\" \"EnvironmentProbe\"\n"
      "\"origin\" \"7 7 7\"\n}\n");
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);

    // the probes are built again once the table has every classname's entityDef
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto table = std::shared_ptr<const ProbeTable>{};
    while (std::chrono::steady_clock::now() < deadline)
    {
      scene.collect();
      table = scene.probeTable();
      if (table->version > 0 && !table->provisional)
      {
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    scene.finishForTesting();
    REQUIRE(table);
    CHECK_FALSE(table->provisional);
    CHECK(table->envProbeDef == EnvProbeDefState::Found);
    // env_probe, the def that inherits it, and the entity whose own key names the
    // spawnclass; not the plain info_null; no tree: area -1, no automatic probe
    REQUIRE(table->records.size() == 3u);
    CHECK(table->placed == 3u);
    auto origins = std::vector<EngineVec3>{};
    for (const auto& record : table->records)
    {
      CHECK_FALSE(record.automatic);
      CHECK(record.areaNum == -1);
      origins.push_back(record.origin);
    }
    CHECK(std::ranges::find(origins, EngineVec3{10.0f, 20.0f, 30.0f}) != origins.end());
    CHECK(std::ranges::find(origins, EngineVec3{-5.0f, 0.0f, 0.0f}) != origins.end());
    CHECK(std::ranges::find(origins, EngineVec3{7.0f, 7.0f, 7.0f}) != origins.end());
    // preview-id order
    CHECK(table->records[0].id < table->records[1].id);
    CHECK(table->records[1].id < table->records[2].id);
    CHECK(scene.takeWarnings().empty());
  }
  pool->stop();
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("ProbeRecord: 100 rebuilds on the debug heap")
{
  auto input = inputOf(fixtureTree(), "maps/lp1b_geometry");
  input.shProbes = std::make_shared<const std::vector<ShProbe>>(
    std::vector<ShProbe>{shEntry(0, {132.0f, 0.0f, 64.0f}, 1.0f)});
  auto registry = ProbeRegistry{nullptr};
  auto round = 0;
  const auto rebuild = [&] {
    // a different bake base each time, so every update builds again
    input.bakeBase = fmt::format("maps/round{}", round++ % 2);
    registry.update(input);
    registry.finishForTesting();
  };
  rebuild();
  rebuild();
  CHECK_FALSE(heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      rebuild();
    }
  }));
  CHECK(registry.table()->records.size() == 2u);
}
#endif


// LP4.2a (C37, D85, A29): admin's probes over the retail game (TB_LIT_GAME_PATH, read
// only): its bake (the .bproc tree, no .shprobes), its entities' classnames, the
// env_probe entityDef, the automatic probes, and the states of their images as the parse
// thread resolves them: no _spec at the engine's #__2108 (the GI .pk4 holds an older
// bake's
// #__2105), no _depth, so every probe is defaulted and the probe path is black on retail.
TEST_CASE(
  "Retail probes: admin's automatic probes and their images (LP4.2a)", "[.][retail]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the probes need the game folder");
  }
  const auto gamePath = std::filesystem::path{gamePathValue};
  auto source = GameDeclSource{gamePath};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  // the areas' deforms from the copied registry, as the service reads the bake (C31)
  const auto mapName = std::string{"maps/game/admin"};
  const auto materials =
    [&](const std::string& material) -> std::shared_ptr<const MaterialEntry> {
    auto found = decls.lookup(material);
    return found ? *found.value : nullptr;
  };
  const auto bake = readMapBake(source, mapName, materials);
  drain();
  REQUIRE(bake.tree);
  REQUIRE(bake.areas);
  CHECK_FALSE(bake.shProbes);

  // the map's entities by classname (the .map's "classname" keys): none is a placed probe
  auto classnames = std::map<std::string, int>{};
  auto mapRead = false;
  if (const auto map = source.readGameFile(mapName + ".map", nullptr))
  {
    mapRead = true;
    const auto text = std::string_view{map->data(), map->size()};
    constexpr auto key = std::string_view{R"("classname" ")"};
    for (auto at = text.find(key); at != std::string_view::npos; at = text.find(key, at))
    {
      at += key.size();
      const auto end = text.find('"', at);
      ++classnames[std::string{text.substr(at, end - at)}];
    }
  }
  auto placed = 0;
  auto unknownClasses = 0;
  for (const auto& [classname, count] : classnames)
  {
    const auto def = decls.entityDef(classname);
    if (!def || !*def.value)
    {
      ++unknownClasses;
      continue;
    }
    const auto* spawnclass = findKey((*def.value)->keys, "spawnclass");
    if (spawnclass && *spawnclass == EnvProbeSpawnclass)
    {
      placed += count;
    }
  }
  drain();
  CHECK(placed == 0);
  const auto envProbe = decls.entityDef(EnvProbeClassname);
  drain();
  REQUIRE(envProbe);

  auto input = ProbeJobInput{};
  input.tree = bake.tree;
  input.areas = bake.areas;
  input.shProbes = bake.shProbes;
  input.bakeBase = bake.bakeBase;
  input.envProbeDef = EnvProbeDefState::Found;
  const auto output = buildProbeRecords(input);
  CHECK(output.records.size() + output.automaticSkipped == bake.areas->areas.size());

  // the image states: the cache as the engine would look for it (resolveImageCache) and
  // the preview's answer (resolveImage: the source fallback after a missing cache)
  struct Count
  {
    int ready = 0;
    int missing = 0;
    int stale = 0;
    int defaulted = 0;
  };
  auto counts = std::array<Count, 3>{};
  auto olderSpec = 0;
  for (const auto& probe : output.records)
  {
    CHECK(probe.automatic);
    CHECK_FALSE(probe.shFound);
    for (std::size_t i = 0; i < 3; ++i)
    {
      const auto name = decls.imageCacheName(probe.images[i]);
      REQUIRE(name);
      const auto cache = resolveImageCache(source, *name.value);
      auto& count = counts[i];
      count.ready += cache.state == ImageCacheSlot::State::Ready ? 1 : 0;
      count.missing += cache.state == ImageCacheSlot::State::Missing ? 1 : 0;
      count.stale += cache.state == ImageCacheSlot::State::Stale ? 1 : 0;
      if (i > 0)
      {
        const auto answer = resolveImage(decls, source, probe.images[i]);
        count.defaulted += answer.image ? 0 : 1;
      }
    }
    // the GI .pk4's _spec of this probe under the older bake's usage digits
    auto older = probe.images[1];
    older.cube = engine_image::CubePackedMipChain;
    older.filter = engine_image::FilterLinear;
    if (const auto name = decls.imageCacheName(older))
    {
      olderSpec +=
        resolveImageCache(source, *name.value).state != ImageCacheSlot::State::Missing
          ? 1
          : 0;
    }
  }
  drain();
  source.closeFiles();
  std::cout << fmt::format(
    "LP4.2a retail probes of {}: {} areas, {} automatic probes ({} skipped), {} placed; "
    "map read {} ({} classnames, {} without an entityDef); .shprobes {}; _amb caches "
    "{} Ready / {} Missing / {} Stale; _spec {} Ready / {} Missing / {} Stale ({} "
    "defaulted; {} found at #__2105); _depth {} Ready / {} Missing / {} Stale ({} "
    "defaulted)\n",
    mapName,
    bake.areas->areas.size(),
    output.records.size(),
    output.automaticSkipped,
    placed,
    mapRead,
    classnames.size(),
    unknownClasses,
    bake.shProbes ? "found" : "absent",
    counts[0].ready,
    counts[0].missing,
    counts[0].stale,
    counts[1].ready,
    counts[1].missing,
    counts[1].stale,
    counts[1].defaulted,
    olderSpec,
    counts[2].ready,
    counts[2].missing,
    counts[2].stale,
    counts[2].defaulted);
  for (const auto& warning : output.warnings)
  {
    std::cout << "  " << warning << "\n";
  }
  // the areas whose snapped centre is in solid, the engine's skip (Game_local.cpp:
  // 1313-1318): their points
  for (std::size_t i = 0; i < bake.areas->areas.size(); ++i)
  {
    const auto point =
      engineSnapInt(engineBoundsCenter(bake.areas->areas[i].globalBounds));
    if (pointInArea(*bake.tree, point) < 0)
    {
      std::cout << fmt::format(
        "  area {} centre ({} {} {}) in solid\n", i, point[0], point[1], point[2]);
    }
  }
  // the plan's §1 (A29): every probe defaulted on retail. 72 areas, of which three
  // (4, 12, 62) have their snapped centre in solid: 69 automatic probes, the 72 caches of
  // the GI .pk4 coming from an older bake's probe set (area 45 holds four of them, areas
  // 4, 13 and 62 none)
  const auto probes = int(output.records.size());
  CHECK(bake.areas->areas.size() == 72u);
  CHECK(output.automaticSkipped == 3u);
  CHECK(probes == 69);
  // the _amb caches of the older bake match the preview's keys for every probe (the keys
  // as the engine makes them), never requested (D85)
  CHECK(counts[0].ready == probes);
  CHECK(counts[1].missing == probes);
  CHECK(counts[2].missing == probes);
  CHECK(counts[1].defaulted == probes);
  CHECK(counts[2].defaulted == probes);
}

} // namespace tb::lit
