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
#include "lit/CpuPool.h"
#include "lit/LightRecord.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

const auto World = std::string{"{\n\"classname\" \"worldspawn\"\n}\n"};

std::string lightText(const std::string& keys)
{
  return "{\n\"classname\" \"light\"\n" + keys + "}\n";
}

// a snapshot of one entity record with these keys, without its node (B0.4)
Snapshot keysOnly(
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
  entity.keys.first = 0;
  for (const auto& [key, value] : keys)
  {
    snapshot.keyValues.push_back(SnapshotKeyValue{add(key), add(value)});
  }
  entity.keys.count = std::uint32_t(snapshot.keyValues.size());
  snapshot.entities.push_back(entity);
  return snapshot;
}

PreviewId lightIdOf(const Snapshot& snapshot)
{
  for (const auto& node : snapshot.nodes)
  {
    if (node.kind == SnapshotNodeKind::Light)
    {
      return node.entity;
    }
  }
  return NoPreviewId;
}

std::shared_ptr<const ProcTree> fixtureTree()
{
  const auto goldens = litFixtureFolder() / "goldens" / "lp1b_geometry";
  return std::make_shared<const ProcTree>(
    readProcFile(readTextFile(goldens / "lp1b_geometry.proc"))
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value());
}

} // namespace

TEST_CASE("LightRecord: the registry's records and versions (C15)")
{
  auto pool =
    CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  auto registry = LightRegistry{pool.get(), nullptr};
  CHECK(registry.table()->records.empty());

  auto snapshot = snapshotOfMapText(
    World
    + lightText(
      "\"name\" \"a\"\n\"origin\" \"64 0 96\"\n\"light_radius\" \"200 200 200\"\n"));
  snapshot.sequence = 1;
  const auto id = lightIdOf(snapshot);
  REQUIRE(id != NoPreviewId);

  registry.takeSnapshot(snapshot, nullptr);
  CHECK_FALSE(registry.readyForSnapshot());
  registry.finishForTesting();
  CHECK(registry.readyForSnapshot());

  const auto first = registry.table();
  REQUIRE(first->records.size() == 1u);
  const auto record = first->find(id);
  REQUIRE(record);
  CHECK(record->version == 1u);
  CHECK(record->classname == "light");
  CHECK(record->light.parms.origin == EngineVec3{64, 0, 96});
  // no material service: the entityDef and the materials stay pending
  CHECK(record->provisional);
  CHECK(record->pendingEntityDef);
  CHECK_FALSE(record->material);
  CHECK(
    record->pendingMaterials
    == std::vector<std::string>{"lights/squarelight1", DefaultPointLightMaterial});
  // no .proc: no area references, the whole volume
  CHECK_FALSE(record->derived.hasAreaTree);
  CHECK(record->derived.areas.empty());

  SECTION("a key change builds a new version; the old table keeps the old record")
  {
    registry.takeSnapshot(
      keysOnly(id, "light", {{"name", "a"}, {"origin", "0 0 32"}}, 2), nullptr);
    registry.finishForTesting();
    const auto second = registry.table();
    CHECK(second->version > first->version);
    REQUIRE(second->find(id));
    CHECK(second->find(id)->version == 2u);
    CHECK(second->find(id)->light.parms.origin == EngineVec3{0, 0, 32});
    CHECK(first->find(id)->version == 1u);
  }

  SECTION("a .proc tree rebuilds every light with its areas")
  {
    registry.setProcTree(fixtureTree());
    registry.finishForTesting();
    const auto withTree = registry.table()->find(id);
    REQUIRE(withTree);
    CHECK(withTree->version == 2u);
    CHECK(withTree->derived.hasAreaTree);
    CHECK(withTree->derived.areas.size() == 2u);
    CHECK(withTree->derived.areaNum >= 0);
  }

  SECTION("a removal unpublishes the record")
  {
    auto removed = Snapshot{};
    removed.sequence = 2;
    removed.removed.push_back(id);
    registry.takeSnapshot(removed, nullptr);
    registry.finishForTesting();
    CHECK_FALSE(registry.table()->find(id));
  }

  SECTION("a light that is no longer a light goes")
  {
    auto changed = snapshotOfMapText(World + "{\n\"classname\" \"info_null\"\n}\n");
    changed.nodes.back().id = id;
    changed.nodes.back().entity = id;
    changed.entities.back().id = id;
    registry.takeSnapshot(changed, nullptr);
    registry.finishForTesting();
    CHECK_FALSE(registry.table()->find(id));
  }

  SECTION("a change while the job runs drops the stale record")
  {
    registry.takeSnapshot(keysOnly(id, "light", {{"origin", "1 1 1"}}, 2), nullptr);
    registry.takeSnapshot(keysOnly(id, "light", {{"origin", "2 2 2"}}, 3), nullptr);
    registry.finishForTesting();
    const auto latest = registry.table()->find(id);
    REQUIRE(latest);
    CHECK(latest->light.parms.origin == EngineVec3{2, 2, 2});
  }

  SECTION("R3.12: worldspawn's removeEntities* removes the light, like every entity")
  {
    const auto world = snapshot.entities.front().id;
    registry.takeSnapshot(
      keysOnly(world, "worldspawn", {{"removeEntities1", "LIGHT"}}, 2), nullptr);
    registry.finishForTesting();
    CHECK_FALSE(registry.table()->find(id));

    // the key goes: the light is back
    registry.takeSnapshot(keysOnly(world, "worldspawn", {}, 3), nullptr);
    registry.finishForTesting();
    REQUIRE(registry.table()->find(id));
    CHECK(registry.table()->find(id)->light.parms.origin == EngineVec3{64, 0, 96});
  }

  SECTION("LP7.0 (D128, P7.17): hide 1 turns the light off; the key goes, it is back")
  {
    const auto keys = std::vector<std::pair<std::string, std::string>>{
      {"name", "a"}, {"origin", "64 0 96"}, {"light_radius", "200 200 200"}};
    auto hidden = keys;
    hidden.emplace_back("hide", "1");
    registry.takeSnapshot(keysOnly(id, "light", hidden, 2), nullptr);
    registry.finishForTesting();
    CHECK_FALSE(registry.table()->find(id));
    // a value atoi reads as 0 shows it, as the key's absence does
    auto zero = keys;
    zero.emplace_back("hide", "0");
    registry.takeSnapshot(keysOnly(id, "light", zero, 3), nullptr);
    registry.finishForTesting();
    REQUIRE(registry.table()->find(id));
    CHECK(registry.table()->find(id)->version == 3u);
    registry.takeSnapshot(keysOnly(id, "light", hidden, 4), nullptr);
    registry.finishForTesting();
    CHECK_FALSE(registry.table()->find(id));
    registry.takeSnapshot(keysOnly(id, "light", keys, 5), nullptr);
    registry.finishForTesting();
    REQUIRE(registry.table()->find(id));
    CHECK(registry.table()->find(id)->light.parms.origin == EngineVec3{64, 0, 96});
  }

  SECTION("bad target info: a warning, a record without a volume")
  {
    registry.takeSnapshot(
      keysOnly(id, "light", {{"origin", "1 2 3"}, {"light_target", "0 0 -1"}}, 2),
      nullptr);
    registry.finishForTesting();
    const auto bad = registry.table()->find(id);
    REQUIRE(bad);
    CHECK(bad->light.badTarget);
    CHECK_FALSE(bad->derived.hasVolume);
    // warnings come once the entityDef is known; without a service it never is
    CHECK(registry.takeWarnings().empty());
  }
  pool->stop();
}

TEST_CASE("LightRecord: entityDefs and materials from the service")
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<
      DeclSource>{std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{
      DeclFile{
        "def/lights.def",
        "entityDef light\n{\n\t\"editor_color\" \"0 1 0\"\n\t\"spawnclass\" \"idLight\"\n"
        "\t\"_color\" \"0.5 0.5 0.5\"\n}\n",
        0},
      DeclFile{
        "materials/lights.mtr",
        "lights/squarelight1\n{\n\tlightFalloffImage makeIntensity( "
        "lights/squarelight1a.tga )\n\t{\n\t\tmap lights/squarelight1.tga\n\t}\n}\n"
        "lights/defaultPointLight\n{\n\t{\n\t\tmap lights/squarelight1.tga\n\t}\n}\n"
        "lights/defaultProjectedLight\n{\n\t{\n\t\tmap lights/squarelight1.tga\n\t}\n}\n",
        0}})};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      World + lightText("\"origin\" \"0 0 0\"\n\"light_radius\" \"8 8 8\"\n"));
    snapshot.sequence = 1;
    const auto id = lightIdOf(snapshot);
    scene.takeSnapshot(snapshot);

    // the light is built again once the table has its entityDef and materials
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto record = std::shared_ptr<const LightRecord>{};
    while (std::chrono::steady_clock::now() < deadline)
    {
      scene.collect();
      record = scene.lightTable()->find(id);
      if (record && !record->provisional)
      {
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    scene.finishForTesting();
    REQUIRE(record);
    CHECK_FALSE(record->provisional);
    CHECK(record->version >= 2u);
    // the entityDef's _color under the map's keys (A10)
    CHECK(record->light.parms.shaderParms[0] == 0.5f);
    CHECK(record->derived.shader.name == "lights/squarelight1");
    REQUIRE(record->material);
    CHECK(record->material->registry == service->table()->registry);
  }
  pool->stop();
}

TEST_CASE("LightRecord: a re-parsed light shader builds its lights again (LPF.0b, C43)")
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
        "materials/lights.mtr",
        "lights/defaultPointLight\n{\n\t{\n\t\tmap lights/squarelight1.tga\n\t}\n}\n"
        "lights/defaultProjectedLight\n{\n\t{\n\t\tmap lights/squarelight1.tga\n\t}\n}\n"
        "lights/lpf_a\n{\n\t{\n\t\tmap lights/lpf_a.tga\n\t}\n}\n"
        "lights/lpf_b\n{\n\t{\n\t\tmap lights/lpf_b.tga\n\t}\n}\n",
        0}})};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(
      World
      + lightText("\"name\" \"a\"\n\"origin\" \"0 0 0\"\n\"light_radius\" \"8 8 8\"\n"
                  "\"texture\" \"lights/lpf_a\"\n")
      + lightText("\"name\" \"b\"\n\"origin\" \"64 0 0\"\n\"light_radius\" \"8 8 8\"\n"
                  "\"texture\" \"lights/lpf_b\"\n"));
    snapshot.sequence = 1;
    auto ids = std::vector<PreviewId>{};
    for (const auto& node : snapshot.nodes)
    {
      if (node.kind == SnapshotNodeKind::Light)
      {
        ids.push_back(node.entity);
      }
    }
    REQUIRE(ids.size() == 2u);
    scene.takeSnapshot(snapshot);

    // both lights built with their shaders, the service idle
    const auto settled = [&](const auto& done) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < deadline)
      {
        scene.collect();
        if (
          !scene.busy() && service->stats().idle && done(scene.lightTable()->find(ids[0]))
          && scene.lightTable()->find(ids[1]))
        {
          scene.finishForTesting();
          return true;
        }
        std::this_thread::sleep_for(5ms);
      }
      return false;
    };
    REQUIRE(settled([](const auto& record) {
      return record && !record->provisional
             && record->derived.shader.name == "lights/lpf_a";
    }));
    const auto a = scene.lightTable()->find(ids[0]);
    const auto b = scene.lightTable()->find(ids[1]);
    REQUIRE_FALSE(b->provisional);
    CHECK(b->derived.shader.name == "lights/lpf_b");

    REQUIRE(
      service
        ->reparseMaterial("lights/lpf_a", "{\n\t{\n\t\tmap lights/lpf_new.tga\n\t}\n}\n")
        .is_success());
    REQUIRE(settled(
      [&](const auto& record) { return record && record->version > a->version; }));
    const auto rebuilt = scene.lightTable()->find(ids[0]);
    REQUIRE(rebuilt->derived.shader.material);
    CHECK(rebuilt->derived.shader.material->revision == 1u);
    REQUIRE_FALSE(rebuilt->derived.shader.material->stages.empty());
    CHECK(rebuilt->derived.shader.material->stages[0].image->name == "lights/lpf_new");
    // the other light's record is the one built before
    CHECK(scene.lightTable()->find(ids[1]) == b);
  }
  pool->stop();
}

} // namespace tb::lit
