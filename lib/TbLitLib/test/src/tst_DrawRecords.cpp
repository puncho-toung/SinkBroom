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

#include "BindingSetCache.h"
#include "DrawRecords.h"

#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

class FakeBindingSet : public nvrhi::RefCounter<nvrhi::IBindingSet>
{
public:
  const nvrhi::BindingSetDesc* getDesc() const override { return nullptr; }
  nvrhi::IBindingLayout* getLayout() const override { return nullptr; }
};

nvrhi::BindingSetHandle fakeSet()
{
  return nvrhi::BindingSetHandle::Create(new FakeBindingSet{});
}

// a scene of surfaces whose records the table makes: chunks, an opaque material,
// registers with a stage on
struct Scene
{
  std::array<Chunk, 6> chunks = {};
  std::shared_ptr<MaterialEntry> material = std::make_shared<MaterialEntry>();
  std::shared_ptr<EvaluatedRegisters> registers = std::make_shared<EvaluatedRegisters>();

  Scene()
  {
    material->coverage = engine_material::CoverageOpaque;
    registers->registers = {1.0f};
  }

  LitSurface surface(const std::size_t i) const
  {
    auto surface = LitSurface{};
    surface.chunk = &chunks[i];
    surface.material = material;
    surface.registers = registers;
    return surface;
  }
};

// the record holds one set per slot list (as a record's resolution holds its sets), made
// against the cache, and is indexed by the table
void resolveFake(
  DrawRecordTable& table,
  BindingSetCache& cache,
  DrawRecord& record,
  const std::vector<std::vector<SlotVersion>>& setSlots,
  const std::uint64_t frame)
{
  static auto layout = int{0};
  record.setCount = setSlots.size();
  for (std::size_t i = 0; i < setSlots.size(); ++i)
  {
    auto key = BindingSetKey{};
    key.layout = reinterpret_cast<nvrhi::IBindingLayout*>(&layout);
    key.slots.append(setSlots[i].begin(), setSlots[i].end());
    key.samplers = i;
    auto* entry = cache.hold(key, frame, [] { return fakeSet(); });
    REQUIRE(entry);
    record.entries[i] = entry;
    record.sets[i] = entry->set.Get();
    record.slots.append(key.slots.begin(), key.slots.end());
  }
  record.state = DrawRecord::State::Resolved;
  table.resolved(record);
}

std::uint32_t holdsOf(const DrawRecord& record, const std::size_t set)
{
  return record.entries[set] ? record.entries[set]->holds : 0u;
}

DrawRecordTable::Lifetime lifetimeOf(const Scene& scene)
{
  auto lifetime = DrawRecordTable::Lifetime{};
  lifetime.chunks = &scene.chunks;
  lifetime.chunksVersion = 1;
  return lifetime;
}

} // namespace

TEST_CASE("DrawRecordTable (LP6.1, C64, P8.1)")
{
  auto scene = Scene{};
  auto cache = BindingSetCache{64};
  auto table = DrawRecordTable{};
  table.beginBuild(lifetimeOf(scene));
  table.generations(1, 1);

  // three surfaces' prepass records, each resolved with sets of these slots
  auto made = false;
  auto& a = table.surface(scene.surface(0), made);
  CHECK(made);
  auto& b = table.surface(scene.surface(1), made);
  auto& c = table.surface(scene.surface(2), made);
  CHECK(table.groups() == 3u);
  CHECK(table.records() == 3u);
  // a second link of the same surface finds its group
  CHECK(&table.surface(scene.surface(0), made) == &a);
  CHECK_FALSE(made);
  resolveFake(table, cache, a.prepass, {{{1, 1}}, {{2, 1}}}, 1);
  resolveFake(table, cache, b.prepass, {{{2, 1}, {3, 1}}}, 1);
  resolveFake(table, cache, c.prepass, {{{4, 1}}}, 1);
  table.endBuild();

  SECTION("a slot's bump releases exactly the records resolved with it")
  {
    CHECK(table.recordsOf(2) == 2u);
    CHECK(table.slotChanged(2) == 2u);
    CHECK(a.prepass.state == DrawRecord::State::Unresolved);
    CHECK(b.prepass.state == DrawRecord::State::Unresolved);
    CHECK(c.prepass.state == DrawRecord::State::Resolved);
    CHECK(table.recordsOf(1) == 0u);
    CHECK(table.recordsOf(2) == 0u);
    CHECK(table.recordsOf(3) == 0u);
    CHECK(table.recordsOf(4) == 1u);
    // their holds let go: the cache erases the slot's sets without a held one
    CHECK(a.prepass.entries[0] == nullptr);
    CHECK(cache.slotChanged(2) == 2u);
    CHECK(cache.heldErased() == 0u);
    CHECK(table.slotChanged(7) == 0u);
    // a released record is resolved again at its next draw, the new version's set
    resolveFake(table, cache, b.prepass, {{{2, 2}, {3, 1}}}, 2);
    CHECK(table.recordsOf(2) == 1u);
    CHECK(table.records() == 3u);
  }

  SECTION("an adopted usage's change releases the records of that slot alone")
  {
    // the render thread passes the usage changes to slotChanged before the cache's
    // (which they do not erase)
    CHECK(table.slotChanged(4) == 1u);
    CHECK(c.prepass.state == DrawRecord::State::Unresolved);
    CHECK(a.prepass.state == DrawRecord::State::Resolved);
    CHECK(cache.setsOf(4) == 1u);
  }

  SECTION("a render-target or an arena generation clears every record")
  {
    CHECK_FALSE(table.generations(1, 1));
    CHECK(table.records() == 3u);
    CHECK(table.generations(2, 1));
    CHECK(table.records() == 0u);
    CHECK(table.groups() == 0u);
    CHECK(table.recordsOf(1) == 0u);
    CHECK(cache.renderTargetsChanged(2) == 0u);
    // every hold let go
    CHECK(cache.slotChanged(1) == 1u);
    CHECK(cache.slotChanged(2) == 2u);
    CHECK(cache.heldErased() == 0u);
    CHECK(table.renderTargetGeneration() == 2u);

    auto& again = table.surface(scene.surface(0), made);
    CHECK(made);
    resolveFake(table, cache, again.prepass, {{{1, 1}}}, 3);
    CHECK(table.generations(2, 2));
    CHECK(table.records() == 0u);
    CHECK(table.arenaGeneration() == 2u);
  }

  SECTION("a camera-only rebuild keeps every record; the lights or the grid a kind")
  {
    // the lifetime holds no camera field: a build of the same scene clears nothing, and
    // no record needs resolving
    const auto kept = table.beginBuild(lifetimeOf(scene));
    CHECK_FALSE(kept.surfaces);
    CHECK_FALSE(kept.interactions);
    CHECK(&table.surface(scene.surface(0), made) == &a);
    CHECK_FALSE(made);
    CHECK(a.prepass.state == DrawRecord::State::Resolved);
    CHECK(b.prepass.state == DrawRecord::State::Resolved);
    CHECK(table.recordsOf(2) == 2u);
    table.endBuild();

    // an interaction group of a light, then the lights' version moves
    auto light = ViewLight{};
    light.record = std::make_shared<LightRecord>();
    light.shader = scene.material;
    const auto serial = table.lightSerial(light);
    CHECK(table.lightSerial(light) == serial);
    auto key = InteractionRecordKey{};
    key.light = serial;
    key.chunk = &scene.chunks[3];
    auto& lit = table.interaction(key, scene.surface(3), made);
    CHECK(made);
    lit.draws.push_back(InteractionRecord{});
    table.filled(lit);
    resolveFake(table, cache, lit.draws[0].variants[0], {{{5, 1}}}, 4);
    CHECK(table.records() == 4u);
    // new registers: a new serial (the old groups are swept)
    light.registers = {0.5f};
    CHECK(table.lightSerial(light) != serial);

    auto lights = lifetimeOf(scene);
    lights.lightsVersion = 2;
    const auto clearedLights = table.beginBuild(lights);
    CHECK(clearedLights.interactions);
    CHECK_FALSE(clearedLights.surfaces);
    CHECK(table.records() == 3u);
    CHECK(table.recordsOf(5) == 0u);
    CHECK(a.prepass.state == DrawRecord::State::Resolved);
    table.endBuild();

    auto grid = lights;
    grid.gridValidity = 7;
    const auto clearedGrid = table.beginBuild(grid);
    CHECK(clearedGrid.surfaces);
    CHECK_FALSE(clearedGrid.interactions);
    CHECK(table.records() == 0u);
    CHECK(table.groups() == 0u);
    table.endBuild();

    auto scene2 = grid;
    scene2.chunksVersion = 2;
    auto& fresh = table.surface(scene.surface(0), made);
    CHECK(made);
    resolveFake(table, cache, fresh.prepass, {{{1, 1}}}, 5);
    const auto clearedScene = table.beginBuild(scene2);
    CHECK(clearedScene.surfaces);
    CHECK(clearedScene.interactions);
    CHECK(table.records() == 0u);
    CHECK(cache.slotChanged(1) == 1u);
    CHECK(cache.heldErased() == 0u);
  }

  SECTION("the sweep: a group unlinked for 1024 builds goes, checked every 64th")
  {
    // builds 2 .. 1088 link a and b; c is linked last by build 1
    for (auto build = 2u; build <= 1088u; ++build)
    {
      table.beginBuild(lifetimeOf(scene));
      table.surface(scene.surface(0), made);
      table.surface(scene.surface(1), made);
      table.endBuild();
      if (build < 1088u)
      {
        CHECK(table.groups() == 3u);
      }
    }
    // build 1088 is the first multiple of 64 past 1024 builds after c's last link
    CHECK(table.build() == 1088u);
    CHECK(table.groups() == 2u);
    CHECK(table.records() == 2u);
    CHECK(table.recordsOf(4) == 0u);
    CHECK(cache.slotChanged(4) == 1u);
    CHECK(cache.heldErased() == 0u);
  }

  SECTION("the bound: past the capacity the least recently linked go first")
  {
    table.setCapacityForTesting(2);
    table.beginBuild(lifetimeOf(scene));
    // this build links d, e and f; a, b and c are older
    table.surface(scene.surface(3), made);
    table.surface(scene.surface(4), made);
    table.surface(scene.surface(5), made);
    CHECK(table.records() == 6u);
    table.endBuild();
    // never a group the current build linked: the three of it stay
    CHECK(table.groups() == 3u);
    CHECK(table.records() == 3u);
    CHECK(table.recordsOf(1) == 0u);
    CHECK(table.recordsOf(4) == 0u);
  }

  SECTION("no record outlives its sets: held sets stay through trim, erases count")
  {
    // the stamp: once per frame through the entries, no hash
    DrawRecordTable::stamp(a.prepass, 7);
    CHECK(a.prepass.entries[0]->lastUsed == 7u);
    CHECK(a.prepass.entries[1]->lastUsed == 7u);
    CHECK(a.prepass.stamped == 7u);
    // a held set is never trimmed, whatever its age
    auto narrow = BindingSetCache{1};
    auto& d = table.surface(scene.surface(3), made);
    resolveFake(table, narrow, d.prepass, {{{8, 1}}, {{9, 1}}}, 1);
    narrow.get(BindingSetKey{}, 2, [] { return fakeSet(); });
    CHECK(narrow.size() == 3u);
    CHECK(narrow.trim(5) == 1u);
    CHECK(narrow.size() == 2u);
    CHECK(holdsOf(d.prepass, 0) == 1u);
    // the render thread's order releases before it erases
    CHECK(table.slotChanged(8) == 1u);
    CHECK(holdsOf(d.prepass, 0) == 0u);
    CHECK(narrow.slotChanged(8) == 1u);
    CHECK(narrow.heldErased() == 0u);
    table.clear();
    CHECK(table.records() == 0u);
    CHECK(narrow.slotChanged(9) == 1u);
    CHECK(narrow.heldErased() == 0u);
    // the other order is counted (a set erased under a holder)
    auto other = BindingSetCache{4};
    auto key = BindingSetKey{};
    key.slots = {SlotVersion{10, 1}};
    REQUIRE(other.hold(key, 1, [] { return fakeSet(); }));
    CHECK(other.slotChanged(10) == 1u);
    CHECK(other.heldErased() == 1u);
  }
}

} // namespace tb::lit
