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

// LP7.2 (C52, C53, D127-D131, D139, D140): model entities in the scene, skins and
// shaders, on the CPU

#include "GeometryArenas.h"
#include "LightTriangles.h"
#include "LitFrontend.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "LitView.h"
#include "fs/TestEnvironment.h"
#include "lit/AreaData.h"
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/EntityState.h"
#include "lit/GameFileSource.h"
#include "lit/GridTable.h"
#include "lit/LightData.h"
#include "lit/MaterialService.h"
#include "lit/ModelInstance.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>
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

// the decls of these tests: the LP1b fixture materials, stand-ins for the origin brush,
// a not-drawn caster and a deforming flare, and the hand skins (skins/lp7_test.skin)
std::vector<DeclFile> skinDecls()
{
  return {
    DeclFile{
      "materials/lp1bc.mtr",
      readTextFile(litFixtureFolder() / "materials" / "lp1bc.mtr"),
      0},
    DeclFile{
      "materials/zz_lp7s_test.mtr",
      "textures/common/origin\n{\n\torigin\n\tnoshadows\n}\n"
      // no stage: not drawn (IsDrawn); forceShadows: casting shadows (SurfaceCastsShadow)
      "textures/lp7s/caster\n{\n\tforceShadows\n\tqer_editorimage "
      "textures/lp1b/wall.tga\n}\n"
      // a deforming surface, which a shader key skips (tr_frontend_addmodels.cpp:588)
      "textures/lp7s/flare\n{\n\tdeform sprite\n\tqer_editorimage "
      "textures/lp1b/floor.tga\n\t{\n\t\tblend add\n\t\tmap "
      "textures/lp1b/floor.tga\n\t}\n}\n",
      0},
    DeclFile{
      "skins/lp7_test.skin",
      readTextFile(litFixtureFolder() / "skins" / "lp7_test.skin"),
      0},
  };
}

const auto SkinTestMaterials = std::vector<std::string>{
  "textures/lp1b/floor",
  "textures/lp1b/wall",
  "textures/lp1b/crate",
  "textures/lp1b/grate",
  "textures/lp1b/glass",
  "textures/lp1b/patch",
  "textures/common/origin",
  "textures/lp7s/caster",
  "textures/lp7s/flare"};

const auto SkinTestSkins = std::vector<std::string>{
  "skins/lp7s/to_grate", "skins/lp7s/to_wall", "skins/lp7s/wild"};

// polls until it holds, or the timeout
template <typename Predicate>
bool waitUntil(const Predicate& predicate, const std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

// a skin of the registry as the service publishes it
std::shared_ptr<const SkinSlot> skinSlotOf(MaterialDecls& decls, const std::string& name)
{
  auto slot = std::make_shared<SkinSlot>();
  auto found = decls.skin(name);
  if (!found)
  {
    slot->error = found.error;
    return slot;
  }
  auto record = std::make_shared<SkinRecord>();
  record->name = found.value->name;
  for (const auto& [from, to] : found.value->mappings)
  {
    record->mappings.push_back(SkinRecord::Mapping{from, to});
  }
  slot->entry = std::move(record);
  return slot;
}

/**
 * A table of the test decls with the fixture's editor-image sizes (listImages.txt), its
 * skins read through MaterialDecls::skin; made once, its registry gone by the time it is
 * used (only one registry lives at a time)
 */
const std::shared_ptr<const MaterialTable>& skinTestTable()
{
  static const auto table = [] {
    const auto sizes = listImagesSizes(
      readTextFile(litFixtureFolder() / "goldens" / "lp1b_geometry" / "listImages.txt"));
    auto source = MemoryDeclSource{skinDecls()};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());
    auto result = std::make_shared<MaterialTable>();
    result->registry = decls.id();
    for (const auto& name : SkinTestMaterials)
    {
      auto found = decls.lookup(name);
      REQUIRE(found);
      auto entry = std::make_shared<MaterialEntry>(**found.value);
      const auto size = sizes.find(name);
      const auto own = size != sizes.end() ? size->second : std::pair{32, 64};
      entry->editorImageSize =
        EditorImageSize{own.first, own.second, EditorImageSize::State::Ready, {}};
      result->materials = result->materials.set(
        name, std::make_shared<const MaterialSlot>(MaterialSlot{std::move(entry), {}}));
    }
    for (const auto& name : SkinTestSkins)
    {
      result->skins = result->skins.set(name, skinSlotOf(decls, name));
    }
    static_cast<void>(decls.takeMessages());
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    return std::shared_ptr<const MaterialTable>{std::move(result)};
  }();
  return table;
}

// the LP2.2a fixture's map (lp1b_geometry.map) with keys added to its func_static
// lp1b_rotator (the crate box and its origin brush) and brushes added inside it
std::string rotatorMap(const std::string& keys, const std::string& brushes = {})
{
  auto map = readTextFile(litFixtureFolder() / "maps" / "lp1b_geometry.map");
  const auto model = std::string{"\"model\" \"lp1b_rotator\"\n"};
  const auto at = map.find(model);
  REQUIRE(at != std::string::npos);
  map.insert(at + model.size(), keys + brushes);
  return map;
}

ChunkJobInput jobInputOf(const Snapshot& snapshot)
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
      cells.insert(CellRef{node->entity, cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.materials = skinTestTable();
  for (const auto& name : SkinTestSkins)
  {
    input.skins[name] = skinTestTable()->findSkin(name);
  }
  return input;
}

PreviewId entityNamed(const Snapshot& snapshot, const std::string& name)
{
  auto id = NoPreviewId;
  for (const auto& entity : sceneEntitiesOf(snapshot))
  {
    if (const auto* value = findKey(entity->keys, "name"); value && *value == name)
    {
      id = entity->id;
    }
  }
  REQUIRE(id != NoPreviewId);
  return id;
}

// the chunks of one entity, by their own material
std::multimap<std::string, Chunk> chunksOf(
  const ChunkJobOutput& output, const PreviewId entity)
{
  auto chunks = std::multimap<std::string, Chunk>{};
  for (const auto& chunk : output.chunks)
  {
    if (chunk.key.entity == entity)
    {
      chunks.emplace(chunkSourceMaterial(chunk), chunk);
    }
  }
  return chunks;
}

std::vector<std::array<std::uint16_t, 2>> stOf(const Chunk& chunk)
{
  auto st = std::vector<std::array<std::uint16_t, 2>>{};
  for (const auto& vert : chunk.verts)
  {
    st.push_back(vert.st);
  }
  return st;
}

} // namespace

TEST_CASE("MaterialDecls: a skin's mappings from its decl (LP7.2, C53)")
{
  auto source = MemoryDeclSource{skinDecls()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  using Pairs = std::vector<std::pair<std::string, std::string>>;

  // the model line is the editor's association, not a mapping (DeclSkin.cpp:88-92)
  const auto grate = decls.skin("skins/lp7s/to_grate");
  REQUIRE(grate);
  CHECK(grate.value->name == "skins/lp7s/to_grate");
  CHECK(grate.value->mappings == Pairs{{"textures/lp1b/crate", "textures/lp1b/grate"}});
  // quoted tokens, and the name canonicalised as FindType does
  const auto wall = decls.skin("Skins\\LP7S\\To_Wall");
  REQUIRE(wall);
  CHECK(wall.value->name == "skins/lp7s/to_wall");
  CHECK(wall.value->mappings == Pairs{{"textures/lp1b/crate", "textures/lp1b/wall"}});
  // "*" is the wildcard, map.from NULL
  const auto wild = decls.skin("skins/lp7s/wild");
  REQUIRE(wild);
  CHECK(
    wild.value->mappings
    == Pairs{
      {"textures/lp1b/floor", "textures/lp1b/patch"}, {"", "textures/lp1b/glass"}});
  // FindSkin makes the engine's default for a name without a decl: a material of the
  // name maps _default to it (SetDefaultText), any other name "*" to _default
  const auto named = decls.skin("textures/lp1b/crate");
  REQUIRE(named);
  CHECK(named.value->mappings == Pairs{{"_default", "textures/lp1b/crate"}});
  const auto none = decls.skin("skins/lp7s/none");
  REQUIRE(none);
  CHECK(none.value->mappings == Pairs{{"", "_default"}});
  static_cast<void>(decls.takeMessages());
}

TEST_CASE("ChunkBuilder: a brush entity's shader and skin, the roles split (LP7.2, C53)")
{
  const auto flare = boxBrush(120, 56, 20, 124, 60, 24, "lp7s/flare");
  const auto caster = boxBrush(126, 56, 20, 130, 60, 24, "lp7s/caster");
  const auto build = [&](const std::string& keys) {
    const auto snapshot = snapshotOfMapText(rotatorMap(keys, flare + caster));
    auto output = buildChunks(jobInputOf(snapshot));
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    return std::pair{chunksOf(output, entityNamed(snapshot, "lp1b_rotator")), output};
  };
  const auto [own, ownOutput] = build("");
  REQUIRE(own.count("textures/lp1b/crate") == 1);
  REQUIRE(own.count("textures/lp7s/flare") == 1);
  REQUIRE(own.count("textures/lp7s/caster") == 1);
  const auto& crate = own.find("textures/lp1b/crate")->second;
  CHECK(crate.key.material == "textures/lp1b/crate");
  CHECK(crate.sourceMaterial == "textures/lp1b/crate");
  CHECK(crate.castClass == CastClass::Opaque);
  CHECK_FALSE(crate.provisional);

  SECTION("a skin remaps the draw material; back sides, tangents and st are the own's")
  {
    const auto [skinned, output] = build("\"skin\" \"skins/lp7s/to_grate\"\n");
    REQUIRE(skinned.count("textures/lp1b/crate") == 1);
    const auto& chunk = skinned.find("textures/lp1b/crate")->second;
    CHECK(chunk.key.material == "textures/lp1b/grate");
    // the draw material's cast class (grate is perforated)
    CHECK(chunk.castClass == CastClass::Perforated);
    // grate is two-sided (ShouldCreateBackSides), the crate is not: no back sides; the
    // crate's unsmoothed tangents (the same vertices)
    CHECK(chunk.verts == crate.verts);
    CHECK(chunk.indexes == crate.indexes);
    CHECK(chunk.appearanceVersion != crate.appearanceVersion);
    // unmapped surfaces keep their own material
    CHECK(
      skinned.find("textures/lp7s/flare")->second.key.material == "textures/lp7s/flare");
  }

  SECTION("the editor-image size is the own material's (C21)")
  {
    const auto [skinned, output] = build("\"skin\" \"skins/lp7s/to_wall\"\n");
    const auto& chunk = skinned.find("textures/lp1b/crate")->second;
    CHECK(chunk.key.material == "textures/lp1b/wall");
    // wall's image is 32 x 64, the crate's 32 x 32: the st are the crate's
    CHECK(stOf(chunk) == stOf(crate));
  }

  SECTION("a shader replaces every draw material and skips a deforming surface")
  {
    const auto [shaded, output] = build("\"shader\" \"textures/lp1b/wall\"\n");
    const auto& chunk = shaded.find("textures/lp1b/crate")->second;
    CHECK(chunk.key.material == "textures/lp1b/wall");
    CHECK(stOf(chunk) == stOf(crate));
    CHECK(shaded.count("textures/lp7s/flare") == 0);
    // a not-drawn surface takes the shader too (the frontend's test is on its own
    // material first, :579: it casts shadows, so it stays)
    CHECK(
      shaded.find("textures/lp7s/caster")->second.key.material == "textures/lp1b/wall");
    // the shader goes first: a skin beside it changes nothing
    const auto [both, bothOutput] =
      build("\"shader\" \"textures/lp1b/wall\"\n\"skin\" \"skins/lp7s/to_grate\"\n");
    CHECK(both.find("textures/lp1b/crate")->second.key.material == "textures/lp1b/wall");
  }

  SECTION("a wildcard skin: the first match wins, a not-drawn surface is never remapped")
  {
    const auto [skinned, output] = build("\"skin\" \"skins/lp7s/wild\"\n");
    CHECK(
      skinned.find("textures/lp1b/crate")->second.key.material == "textures/lp1b/glass");
    CHECK(
      skinned.find("textures/lp7s/flare")->second.key.material == "textures/lp1b/glass");
    // DeclSkin.cpp RemapShaderBySkin: !IsDrawn() keeps its own
    CHECK(
      skinned.find("textures/lp7s/caster")->second.key.material
      == "textures/lp7s/caster");
  }

  SECTION("a skin the table does not have yet: no remap, the chunks provisional (A41)")
  {
    const auto snapshot =
      snapshotOfMapText(rotatorMap("\"skin\" \"skins/lp7s/to_grate\"\n", flare + caster));
    auto input = jobInputOf(snapshot);
    input.skins.erase("skins/lp7s/to_grate");
    const auto output = buildChunks(input);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    const auto chunks = chunksOf(output, entityNamed(snapshot, "lp1b_rotator"));
    const auto& chunk = chunks.find("textures/lp1b/crate")->second;
    CHECK(chunk.key.material == "textures/lp1b/crate");
    CHECK(chunk.provisional);
  }

  SECTION("worldspawn's faces take no entity remap")
  {
    const auto snapshot =
      snapshotOfMapText(rotatorMap("\"skin\" \"skins/lp7s/wild\"\n", flare + caster));
    const auto output = buildChunks(jobInputOf(snapshot));
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    auto worldspawn = NoPreviewId;
    for (const auto& entity : sceneEntitiesOf(snapshot))
    {
      worldspawn = entity->classname == "worldspawn" ? entity->id : worldspawn;
    }
    REQUIRE(worldspawn != NoPreviewId);
    for (const auto& chunk : output.chunks)
    {
      if (chunk.key.entity == worldspawn)
      {
        CHECK(chunk.key.material == chunk.sourceMaterial);
      }
    }
  }
}

TEST_CASE("MaterialService: skins asked, published and asked again (LP7.2, C53)")
{
  static_cast<void>(skinTestTable());
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{std::make_unique<MemoryDeclSource>(skinDecls())};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.registry != 0; }, 10s));

  // Pending first, then the slot under the asked spelling and the canonical name
  const auto first = service->skin("Skins/LP7S/To_Grate");
  CHECK(first.state == SkinLookup::State::Pending);
  const auto table = service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.findSkin("Skins/LP7S/To_Grate") != nullptr; },
    10s);
  REQUIRE(table);
  const auto slot = table->findSkin("skins/lp7s/to_grate");
  REQUIRE(slot);
  REQUIRE(slot->entry);
  CHECK(slot->entry->name == "skins/lp7s/to_grate");
  REQUIRE(slot->entry->mappings.size() == 1);
  CHECK(slot->entry->mappings[0].from == "textures/lp1b/crate");
  CHECK(slot->entry->mappings[0].to == "textures/lp1b/grate");
  CHECK(service->skin("Skins/LP7S/To_Grate").state == SkinLookup::State::Ready);

  // a reload's new registry has it again, asked by the build itself
  const auto registry = table->registry;
  service->reload();
  const auto reloaded = service->waitForTableForTesting(
    [&](const MaterialTable& t) {
      return t.registry != registry && t.findSkin("Skins/LP7S/To_Grate") != nullptr;
    },
    10s);
  REQUIRE(reloaded);
  CHECK(reloaded->findSkin("skins/lp7s/to_grate")->entry->mappings.size() == 1);
}

TEST_CASE("SceneGeometry: a brush entity's skin, pending and then published (LP7.2, A41)")
{
  // the stand-in table first: one registry at a time
  static_cast<void>(skinTestTable());
  auto gate = std::make_shared<ParseGate>();
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{std::make_unique<MemoryDeclSource>(skinDecls())};
  };
  options.pollInterval = 0ms;
  options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
  auto service = MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  const auto atExit = OpenAtExit{gate};
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.registry != 0; }, 10s));
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    auto snapshot = snapshotOfMapText(rotatorMap("\"skin\" \"skins/lp7s/to_grate\"\n"));
    snapshot.sequence = 1;
    const auto rotator = entityNamed(snapshot, "lp1b_rotator");
    const auto drawn = [&] {
      auto materials = std::set<std::string>{};
      for (const auto& [key, chunk] : scene.chunks())
      {
        if (key.entity == rotator)
        {
          materials.insert(key.material);
        }
      }
      return materials;
    };

    // everything is answered and measured but the skin: the chunks are built without it
    auto names = std::set<std::string>{};
    for (const auto& node : sceneNodesOf(snapshot))
    {
      for (const auto& face : node->faces)
      {
        names.insert(engineSideMaterial(face.material));
      }
      if (node->patch)
      {
        names.insert(enginePatchMaterial(node->patch->material));
      }
    }
    for (const auto& name : names)
    {
      static_cast<void>(service->lookup(name));
    }
    for (const auto& entity : sceneEntitiesOf(snapshot))
    {
      static_cast<void>(service->entityDef(entity->classname));
    }
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        return std::ranges::all_of(
                 names,
                 [&](const auto& name) {
                   const auto slot = t.find(name);
                   return slot && slot->sizeMeasured;
                 })
               && t.findEntityDef("func_static") != nullptr
               && t.findEntityDef("light") != nullptr;
      },
      10s));
    // the gate holds the thread at its next take (an idle thread takes a new request
    // without passing it): one more lookup brings it there, and the skin's waits
    gate->close();
    static_cast<void>(service->lookup("textures/lp7s/gate"));
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.find("textures/lp7s/gate") != nullptr; },
      10s));
    scene.takeSnapshot(snapshot);
    scene.finishForTesting();
    CHECK(drawn().contains("textures/lp1b/crate"));
    CHECK(scene.waitsForMaterials());

    // the skin arrives: the entity is built again with it, and nothing waits
    gate->open();
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.findSkin("skins/lp7s/to_grate") != nullptr; },
      10s));
    const auto settled = waitUntil(
      [&] {
        scene.collect();
        scene.finishForTesting();
        return !scene.waitsForMaterials();
      },
      10000ms);
    CHECK(settled);
    CHECK(drawn().contains("textures/lp1b/grate"));
    CHECK_FALSE(drawn().contains("textures/lp1b/crate"));
  }
  pool->stop();
}

namespace
{

// a hand record of one quad surface (four vertices, two triangles) at x 0, its version
// given
std::shared_ptr<const ModelRecord> quadRecord(
  const std::string& name, const std::uint64_t version, const std::string& material)
{
  auto record = std::make_shared<ModelRecord>();
  record->name = name;
  record->state = ModelRecord::State::Ready;
  record->source = ModelRecord::SourceKind::Source;
  record->version = version;
  auto surface = ModelSurface{};
  surface.material = material;
  for (const auto& [y, z] : std::vector<std::pair<float, float>>{
         {-8.0f, -8.0f}, {8.0f, -8.0f}, {8.0f, 8.0f}, {-8.0f, 8.0f}})
  {
    auto vert = EngineDrawVert{};
    vert.xyz = {0, y, z};
    surface.verts.push_back(vert);
  }
  surface.indexes = {0, 1, 2, 0, 2, 3};
  surface.bounds = {EngineVec3{0, -8, -8}, EngineVec3{0, 8, 8}};
  record->surfaces.push_back(std::move(surface));
  record->bounds = record->surfaces.front().bounds;
  return record;
}

// an instance of a shared surface for an entity, keyed as the scene keys it
ModelInstance testInstance(
  const PreviewId entity,
  std::shared_ptr<const SharedSurface> surface,
  const std::string& drawMaterial)
{
  auto instance = ModelInstance{};
  instance.entity = entity;
  instance.surface = surface->key.surface;
  instance.header.key =
    ChunkKey{0, entity, drawMaterial, {0, 0, 0}, int(surface->key.surface)};
  instance.header.bounds = surface->surface().bounds;
  instance.header.sourceMaterial = surface->surface().material;
  instance.header.casterVersion = surface->key.version;
  instance.geometry = std::move(surface);
  return instance;
}

} // namespace

TEST_CASE("GeometryArenas: model instances share their placements (LP7.2, D129)")
{
  auto arenas = GeometryArenas{};
  arenas.setCpuArenasForTesting(1024 * 1024);
  const auto noChunks = std::map<ChunkKey, Chunk>{};
  const auto record = quadRecord("models/test/quad.obj", 1, "textures/lp1b/floor");
  const auto shared = makeSharedSurface(record, 0);
  REQUIRE(shared);
  CHECK(shared->key == SharedGeometryKey{"models/test/quad.obj", 0, 1});
  CHECK(shared->positions.size() == 4);
  CHECK_FALSE(makeSharedSurface(record, 1));
  // four 32-byte vertices and six 16-bit indexes in whole words: once per placement
  const auto placementBytes = std::uint64_t{4 * 32 + 12};

  auto instances = std::map<ChunkKey, ModelInstance>{};
  const auto a = testInstance(10, shared, "textures/lp1b/floor");
  const auto b = testInstance(11, shared, "textures/lp1b/wall");
  instances.emplace(a.key(), a);
  instances.emplace(b.key(), b);

  SECTION("two instances, one placement, its bytes once; 2, 1, 0 and the frame stamp")
  {
    CHECK(arenas.syncInstances(instances, 1));
    CHECK(arenas.queuedShared() == 1);
    CHECK(arenas.sharedInstances(shared->key) == 2);
    REQUIRE(arenas.placeForTesting(noChunks, 1 << 20).is_success());
    CHECK_FALSE(arenas.pending());
    CHECK(arenas.placedShared() == 1);
    CHECK(arenas.uploadedShared() == 1);
    CHECK(arenas.usedBytes() == placementBytes);
    const auto pa = arenas.placement(a.key());
    const auto pb = arenas.placement(b.key());
    REQUIRE(pa);
    REQUIRE(pb);
    CHECK(pa->shared);
    CHECK(pa->vertices.offset == pb->vertices.offset);
    CHECK(pa->indexes.offset == pb->indexes.offset);
    CHECK(pa->indexCount == 6);
    CHECK(pa->vertexCount == 4);

    // one instance goes: the placement stays, counted once
    instances.erase(b.key());
    CHECK_FALSE(arenas.syncInstances(instances, 2));
    CHECK(arenas.sharedInstances(shared->key) == 1);
    CHECK(arenas.usedBytes() == placementBytes);
    CHECK_FALSE(arenas.placement(b.key()));

    // the last goes: freed in frame 3, handed out again once frame 3 has finished
    instances.clear();
    arenas.syncInstances(instances, 3);
    CHECK(arenas.placedShared() == 0);
    CHECK(arenas.sharedInstances(shared->key) == 0);
    CHECK(arenas.usedBytes() == 0);
    CHECK(arenas.pendingFreeBytes() == placementBytes);
    arenas.reclaim(3);
    CHECK(arenas.pendingFreeBytes() == placementBytes);
    arenas.reclaim(4);
    CHECK(arenas.pendingFreeBytes() == 0);
  }

  SECTION("a record's new version is a new placement; the old one retires by its frame")
  {
    arenas.syncInstances(instances, 1);
    REQUIRE(arenas.placeForTesting(noChunks, 1 << 20).is_success());
    const auto old = arenas.placement(a.key());
    REQUIRE(old);

    const auto newer =
      makeSharedSurface(quadRecord("models/test/quad.obj", 2, "textures/lp1b/floor"), 0);
    instances.clear();
    instances.emplace(a.key(), testInstance(10, newer, "textures/lp1b/floor"));
    instances.emplace(b.key(), testInstance(11, newer, "textures/lp1b/wall"));
    CHECK(arenas.syncInstances(instances, 5));
    CHECK(arenas.sharedInstances(shared->key) == 0);
    CHECK(arenas.sharedInstances(newer->key) == 2);
    // the old ranges wait for frame 5: the new placement takes others
    CHECK(arenas.pendingFreeBytes() == placementBytes);
    REQUIRE(arenas.placeForTesting(noChunks, 1 << 20).is_success());
    const auto fresh = arenas.placement(a.key());
    REQUIRE(fresh);
    CHECK(fresh->vertices.offset != old->vertices.offset);
    CHECK(arenas.usedBytes() == placementBytes);
    arenas.reclaim(6);
    CHECK(arenas.pendingFreeBytes() == 0);
  }

  SECTION("an instance without indexes places nothing")
  {
    auto empty = std::make_shared<ModelRecord>(*record);
    empty->surfaces.front().indexes.clear();
    instances.clear();
    const auto none =
      testInstance(12, makeSharedSurface(empty, 0), "textures/lp1b/floor");
    instances.emplace(none.key(), none);
    CHECK_FALSE(arenas.syncInstances(instances, 1));
    CHECK_FALSE(arenas.placement(none.key()));
  }
}

TEST_CASE("Frontend: model instances in the lists, with their shared indexes (LP7.2)")
{
  const auto& table = *skinTestTable();
  const auto record = quadRecord("models/test/quad.obj", 1, "textures/lp1b/floor");
  const auto shared = makeSharedSurface(record, 0);
  auto instances = std::map<ChunkKey, ModelInstance>{};
  const auto a = testInstance(10, shared, "textures/lp1b/floor");
  const auto b = testInstance(11, shared, "textures/lp1b/wall");
  instances.emplace(a.key(), a);
  instances.emplace(b.key(), b);
  // an instance of a surface without indexes is skipped, as a chunk without them
  auto empty = std::make_shared<ModelRecord>(*record);
  empty->name = "models/test/empty.obj";
  empty->surfaces.front().indexes.clear();
  const auto none = testInstance(12, makeSharedSurface(empty, 0), "textures/lp1b/floor");
  instances.emplace(none.key(), none);

  auto entities = std::unordered_map<PreviewId, ChunkEntityState>{};
  for (const auto& [id, x] :
       std::vector<std::pair<PreviewId, float>>{{10, 100.0f}, {11, 100.0f}, {12, 100.0f}})
  {
    auto state = ChunkEntityState{};
    state.id = id;
    state.modelOrigin = {x, 0, 0};
    state.parms.origin = state.modelOrigin;
    state.parms.shaderParms[3] = 1;
    entities.emplace(id, state);
  }
  const auto chunks = std::map<ChunkKey, Chunk>{};
  auto camera = CameraState{};
  camera.viewportWidth = 160;
  camera.viewportHeight = 120;
  auto inputs = FrontendInputs{};
  inputs.view = litViewOf(camera, 160, 120);
  inputs.chunks = &chunks;
  inputs.instances = &instances;
  inputs.entities = &entities;
  inputs.material = [&](const std::string& name) {
    const auto slot = table.find(name);
    return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
  };
  const auto lists = buildFrontendLists(inputs);
  REQUIRE(lists.drawSurfs.size() == 2);
  for (const auto& surface : lists.drawSurfs)
  {
    REQUIRE(surface.instance);
    CHECK(surface.chunk == &surface.instance->header);
    CHECK(surfaceIndexCount(surface) == 6);
    CHECK(surface.instance->geometry == shared);
  }
  CHECK(lists.drawSurfs[0].instance->entity == 10);
  CHECK(lists.drawSurfs[1].instance->entity == 11);
  CHECK(lists.drawSurfs[1].chunk->key.material == "textures/lp1b/wall");
  // each entity its space, at its pivot
  CHECK(lists.drawSurfs[0].space != lists.drawSurfs[1].space);
}

TEST_CASE("LightTriangleCache: an instance's triangles from its shared surface (LP7.2)")
{
  const auto shared =
    makeSharedSurface(quadRecord("models/test/quad.obj", 1, "textures/lp1b/floor"), 0);
  const auto a = testInstance(10, shared, "textures/lp1b/floor");
  auto cache = LightTriangleCache{};
  // a light whose volume holds the quad (an identity projection of the unit cube)
  auto project = EngineMat4{};
  for (auto i = 0; i < 4; ++i)
  {
    project[std::size_t(i * 4 + i)] = 1.0f / 64.0f;
  }
  project[3] = 0.5f;
  project[7] = 0.5f;
  project[11] = 0.5f;
  project[15] = 1.0f;
  const auto identity = EngineGlMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  cache.beginBuild();
  const auto first = cache.triangles(1, 1, project, a, identity);
  REQUIRE(first);
  CHECK(first->state == LightTriangles::State::Pending);
  cache.endBuild(nullptr);
  cache.beginBuild();
  const auto answered = cache.triangles(1, 1, project, a, identity);
  REQUIRE(answered);
  CHECK(answered->state != LightTriangles::State::Pending);
  cache.endBuild(nullptr);
  // the stamp is the instance's caster version: a new record version asks again
  auto moved = a;
  moved.header.casterVersion = 2;
  cache.beginBuild();
  CHECK(
    cache.triangles(1, 1, project, moved, identity)->state
    == LightTriangles::State::Pending);
  cache.endBuild(nullptr);
}

namespace
{

// LP7.2 step 4: a quad on lp1b/floor (2015-stamped), as the service's model tests' OBJ
const auto SceneQuadObj = std::string{
  "v 0 0 0\nv 32 0 0\nv 32 32 0\nv 0 32 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
  "usemtl textures/lp1b/floor\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n"};

// the model scenes' decls: the test materials, the skins, the entityDefs of the dispatch
// (D139's spawnclasses, a dmapOnly class) and the quad
std::vector<DeclFile> modelSceneDecls()
{
  auto files = skinDecls();
  files.push_back(DeclFile{
    "def/lp7s_test.def",
    "entityDef func_static\n{\n\t\"spawnclass\" \"idStaticEntity\"\n}\n"
    "entityDef lp7s_monster\n{\n\t\"spawnclass\" \"idAI\"\n\t\"model\" "
    "\"lp7s_monster_def\"\n}\n"
    "entityDef lp7s_item\n{\n\t\"spawnclass\" \"idItem\"\n}\n"
    "entityDef func_decal\n{\n\t\"spawnclass\" \"idEntity\"\n\t\"dmapOnly\" \"1\"\n}\n"
    "entityDef light\n{\n\t\"spawnclass\" \"idLight\"\n}\n",
    0});
  files.push_back(DeclFile{"models/test/quad.obj", SceneQuadObj, 1451520000});
  return files;
}

// a scene over a material service of modelSceneDecls, settled after each snapshot
class ModelScene
{
public:
  std::unique_ptr<MaterialService> service;
  std::unique_ptr<CpuPool> pool;
  std::unique_ptr<SceneGeometry> scene;
  std::uint64_t sequence = 0;

  ModelScene()
  {
    static_cast<void>(skinTestTable());
    auto options = MaterialServiceOptions{};
    options.makeSource = [](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{
        std::make_unique<MemoryDeclSource>(modelSceneDecls())};
    };
    options.pollInterval = 0ms;
    service = MaterialService::create(std::move(options))
              | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.registry != 0; }, 10s));
    pool = CpuPool::create() | kdl::value();
    scene = std::make_unique<SceneGeometry>(pool.get(), service.get());
  }

  ~ModelScene()
  {
    scene.reset();
    pool->stop();
  }

  // the snapshot taken, then until nothing waits (or the timeout)
  Snapshot take(const std::string& mapText)
  {
    auto snapshot = snapshotOfMapText(mapText);
    snapshot.sequence = ++sequence;
    scene->takeSnapshot(snapshot);
    settle();
    return snapshot;
  }

  void settle()
  {
    const auto settled = waitUntil(
      [&] {
        scene->collect();
        scene->finishForTesting();
        return !scene->waitsForModels() && !scene->waitsForEntityDefs()
               && !scene->waitsForMaterials();
      },
      10000ms);
    CHECK(settled);
  }

  std::vector<const ModelInstance*> instancesOf(const PreviewId entity) const
  {
    auto found = std::vector<const ModelInstance*>{};
    for (const auto& [key, instance] : scene->instances())
    {
      if (key.entity == entity)
      {
        found.push_back(&instance);
      }
    }
    return found;
  }

  std::size_t chunksOf(const PreviewId entity) const
  {
    return std::size_t(std::ranges::count_if(
      scene->chunks(), [&](const auto& entry) { return entry.first.entity == entity; }));
  }
};

std::string pointEntity(const std::string& classname, const std::string& keys)
{
  return "{\n\"classname\" \"" + classname + "\"\n" + keys + "}\n";
}

const auto WorldBox = std::string{"{\n\"classname\" \"worldspawn\"\n"}
                      + boxBrush(0, 0, 0, 64, 64, 64, "lp1b/wall") + "}\n";

} // namespace

TEST_CASE("SceneGeometry: model entities, the dispatch of each kind (LP7.2, C52, D139)")
{
  auto model = ModelScene{};
  const auto quad = std::string{"\"model\" \"models/test/quad.obj\"\n"};
  const auto map =
    WorldBox
    + pointEntity("func_static", "\"name\" \"obj\"\n\"origin\" \"100 0 0\"\n" + quad)
    + "{\n\"classname\" \"func_static\"\n\"name\" \"own\"\n\"model\" \"own\"\n"
    + boxBrush(200, 0, 0, 232, 32, 32, "lp1b/crate") + "}\n"
    + "{\n\"classname\" \"func_static\"\n\"name\" \"brushfile\"\n" + quad
    + boxBrush(300, 0, 0, 332, 32, 32, "lp1b/crate") + "}\n"
    + pointEntity(
      "func_static", "\"name\" \"modeldef\"\n\"model\" \"lp7s_monster_def\"\n")
    + pointEntity("lp7s_monster", "\"name\" \"monster\"\n")
    + pointEntity("lp7s_item", "\"name\" \"item\"\n\"model\" \"lp7s_monster_def\"\n")
    + pointEntity("func_decal", "\"name\" \"decal\"\n" + quad)
    + pointEntity("func_static", "\"name\" \"hidden\"\n\"hide\" \"1\"\n" + quad)
    + pointEntity("func_static", "\"name\" \"inlined\"\n\"inline\" \"1\"\n" + quad)
    + pointEntity("light", "\"name\" \"lamp\"\n\"_color\" \"1 0.5 0.25\"\n" + quad)
    + pointEntity("light", "\"name\" \"lamp_off\"\n\"start_off\" \"1\"\n" + quad)
    + pointEntity(
      "func_static", "\"name\" \"skinned\"\n\"model\" \"models/test/n11.md5mesh\"\n")
    + pointEntity("func_static", "\"name\" \"proc\"\n\"model\" \"own\"\n");
  const auto snapshot = model.take(map);
  const auto id = [&](const std::string& name) { return entityNamed(snapshot, name); };

  // an .obj on a point entity: its record's surfaces, the entity's state at its origin
  const auto obj = model.instancesOf(id("obj"));
  REQUIRE(obj.size() == 1);
  CHECK(obj[0]->header.key.material == "textures/lp1b/floor");
  CHECK(obj[0]->geometry->record->state == ModelRecord::State::Ready);
  CHECK(obj[0]->geometry->record->name == "models/test/quad.obj");
  REQUIRE(model.scene->entityStates().contains(id("obj")));
  CHECK(model.scene->entityStates().at(id("obj")).modelOrigin == EngineVec3{100, 0, 0});
  // a brush entity of its own name draws its brushes, no instance (K17)
  CHECK(model.instancesOf(id("own")).empty());
  CHECK(model.chunksOf(id("own")) > 0);
  // a brush entity whose model names a file draws the file, not its brushes
  CHECK(model.instancesOf(id("brushfile")).size() == 1);
  CHECK(model.chunksOf(id("brushfile")) == 0);
  // a modelDef name on idStaticEntity: the default cube (Default, 24 vertices)
  const auto cube = model.instancesOf(id("modeldef"));
  REQUIRE(cube.size() == 1);
  CHECK(cube[0]->geometry->record->state == ModelRecord::State::Default);
  CHECK(cube[0]->geometry->vertexCount() == 24);
  // on idAI (its def's model) and on idItem
  CHECK(model.instancesOf(id("monster")).empty());
  CHECK(model.instancesOf(id("item")).size() == 1);
  // dmapOnly, hide 1: nothing; inline 1: drawn from its file at its place (K42)
  CHECK(model.instancesOf(id("decal")).empty());
  CHECK(model.instancesOf(id("hidden")).empty());
  CHECK(model.instancesOf(id("inlined")).size() == 1);
  // a light's model under its id, its colour the light's, 0 when it starts off
  const auto lamp = model.instancesOf(id("lamp"));
  REQUIRE(lamp.size() == 1);
  CHECK(lamp[0]->shaderParms[0] == 1.0f);
  CHECK(lamp[0]->shaderParms[1] == 0.5f);
  CHECK(lamp[0]->shaderParms[2] == 0.25f);
  const auto off = model.instancesOf(id("lamp_off"));
  REQUIRE(off.size() == 1);
  CHECK(off[0]->shaderParms[0] == 0.0f);
  // an .md5mesh: nothing (Skinned); another entity's .proc model name: nothing (K47)
  CHECK(model.instancesOf(id("skinned")).empty());
  CHECK(model.instancesOf(id("proc")).empty());

  const auto counts = model.scene->modelCounts();
  // obj, brushfile, modeldef, monster, item, inlined, lamp, lamp_off, skinned
  CHECK(counts.entities == 9);
  CHECK(counts.instances == 7);
  CHECK(counts.pending == 0);
  CHECK(counts.skinned == 2);
  CHECK_FALSE(model.scene->waitsForModels());
  // one shared surface for the five quad instances, the cube's for two
  auto placements = std::set<SharedGeometryKey>{};
  for (const auto& [key, instance] : model.scene->instances())
  {
    placements.insert(instance.placement());
  }
  CHECK(placements.size() == 2);
  // the once-notices: the cube's reason, the skinned and the .proc names
  const auto warnings = model.scene->takeWarnings();
  const auto said = [&](const std::string& part) {
    return std::ranges::any_of(warnings, [&](const std::string& warning) {
      return warning.find(part) != std::string::npos;
    });
  };
  CHECK(said("'modeldef' (func_static): 'lp7s_monster_def' draws the default cube"));
  CHECK(said("idAI spawnclass"));
  CHECK(said("K47"));
}

TEST_CASE("SceneGeometry: a model entity's keys rebuild its instances (LP7.2, C52)")
{
  auto model = ModelScene{};
  const auto entity = [](const std::string& keys) {
    return WorldBox
           + pointEntity(
             "func_static",
             "\"name\" \"obj\"\n\"model\" \"models/test/quad.obj\"\n\"origin\" \"100 0 "
             "0\"\n"
               + keys);
  };
  const auto snapshot = model.take(entity(""));
  const auto obj = entityNamed(snapshot, "obj");
  REQUIRE(model.instancesOf(obj).size() == 1);
  const auto base = *model.instancesOf(obj)[0];
  const auto versions = [&](const std::string& keys) {
    model.take(entity(keys));
    REQUIRE(model.instancesOf(obj).size() == 1);
    const auto& instance = *model.instancesOf(obj)[0];
    return std::pair{instance.header.casterVersion, instance.header.appearanceVersion};
  };
  for (const auto& keys : std::vector<std::string>{
         "\"_color\" \"1 0 0\"\n",
         "\"shaderParm5\" \"2\"\n",
         "\"angles\" \"0 90 0\"\n",
         "\"modelscale_vec\" \"2 1 1\"\n",
         "\"skin\" \"skins/lp7s/wild\"\n",
         "\"shader\" \"textures/lp1b/wall\"\n",
         "\"noshadows\" \"1\"\n"})
  {
    CAPTURE(keys);
    CHECK(versions(keys).second != base.header.appearanceVersion);
  }
  // the shadow flags move the caster version too
  CHECK(versions("\"noshadows\" \"1\"\n").first != base.header.casterVersion);
  // a skin and a shader: the draw material (C53)
  model.take(entity("\"skin\" \"skins/lp7s/wild\"\n"));
  CHECK(model.instancesOf(obj)[0]->header.key.material == "textures/lp1b/patch");
  CHECK(model.instancesOf(obj)[0]->header.sourceMaterial == "textures/lp1b/floor");
  model.take(entity("\"shader\" \"textures/lp1b/wall\"\n"));
  CHECK(model.instancesOf(obj)[0]->header.key.material == "textures/lp1b/wall");
  // hide 1 takes them away, and back
  model.take(entity("\"hide\" \"1\"\n"));
  CHECK(model.instancesOf(obj).empty());
  model.take(entity(""));
  CHECK(model.instancesOf(obj).size() == 1);
  // the model key gone: no instance, no state of the scene's
  model.take(
    WorldBox + pointEntity("func_static", "\"name\" \"obj\"\n\"origin\" \"100 0 0\"\n"));
  CHECK(model.instancesOf(obj).empty());
  CHECK_FALSE(model.scene->entityStates().contains(obj));
}

TEST_CASE("SceneGeometry: a Pending model draws nothing and waits (LP7.2, D124)")
{
  static_cast<void>(skinTestTable());
  auto gate = std::make_shared<ParseGate>();
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(modelSceneDecls())};
  };
  options.pollInterval = 0ms;
  options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
  auto service = MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  const auto atExit = OpenAtExit{gate};
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.registry != 0; }, 10s));
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    // the gate holds the thread at its next take: one lookup brings it there
    gate->close();
    static_cast<void>(service->lookup("textures/lp7s/gate"));
    REQUIRE(service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.find("textures/lp7s/gate") != nullptr; },
      10s));
    auto snapshot = snapshotOfMapText(
      WorldBox
      + pointEntity(
        "func_static", "\"name\" \"obj\"\n\"model\" \"models/test/quad.obj\"\n")
      + pointEntity(
        "func_static", "\"name\" \"none\"\n\"model\" \"models/test/none.obj\"\n"));
    snapshot.sequence = 1;
    scene.takeSnapshot(snapshot);
    scene.finishForTesting();
    CHECK(scene.instances().empty());
    CHECK(scene.waitsForModels());
    CHECK(scene.modelCounts().pending == 2);

    gate->open();
    const auto settled = waitUntil(
      [&] {
        scene.collect();
        scene.finishForTesting();
        return !scene.waitsForModels() && !scene.waitsForEntityDefs();
      },
      10000ms);
    CHECK(settled);
    CHECK(scene.modelCounts().pending == 0);
    auto states = std::map<std::string, ModelRecord::State>{};
    for (const auto& [key, instance] : scene.instances())
    {
      states[instance.geometry->record->name] = instance.geometry->record->state;
    }
    // Ready, and a missing file's cube (converged)
    CHECK(states["models/test/quad.obj"] == ModelRecord::State::Ready);
    CHECK(states["models/test/none.obj"] == ModelRecord::State::Missing);
  }
  pool->stop();
}

namespace
{

// LP7.2 step 5: a box OBJ of local x -60..-10, y -8..8, z 24..40 (its centre at x -35)
std::string spanBoxObj()
{
  auto text = std::string{"# LP7.2 step 5: a box left of its origin\n"};
  for (const auto x : {-60, -10})
  {
    for (const auto y : {-8, 8})
    {
      for (const auto z : {24, 40})
      {
        text += fmt::format("v {} {} {}\n", x, y, z);
      }
    }
  }
  text += "vt 0 0\nvn 0 0 1\nusemtl textures/lp1b/floor\n";
  // the six faces, two triangles each (the vertex index: 1 + 4 x + 2 y + z)
  const auto faces = std::vector<std::array<int, 4>>{
    {1, 2, 4, 3}, {5, 7, 8, 6}, {1, 5, 6, 2}, {3, 4, 8, 7}, {1, 3, 7, 5}, {2, 6, 8, 4}};
  for (const auto& f : faces)
  {
    text += fmt::format("f {0}/1/1 {1}/1/1 {2}/1/1\n", f[0], f[1], f[2]);
    text += fmt::format("f {0}/1/1 {1}/1/1 {2}/1/1\n", f[0], f[2], f[3]);
  }
  return text;
}

/**
 * A game folder of the LP2.2a fixture's bake (the LP1b geometry map's .proc: area 0 east
 * of the doorway's portal plane x = 8, area 1 west of it), the test decls and the span
 * box, served by a GameFileSource, as BakeGame (tst_Geometry.cpp) is
 */
class ModelBakeGame
{
private:
  fs::TestEnvironment m_env;

public:
  ModelBakeGame()
  {
    for (const auto& file : modelSceneDecls())
    {
      writeTextFile(base() / file.path, *file.text);
    }
    writeTextFile(base() / "models" / "test" / "span.obj", spanBoxObj());
    writeTextFile(
      base() / "maps" / "lp1b_geometry.proc",
      readTextFile(
        litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }
  std::string mapPath() const { return (base() / "maps" / "lp1b_geometry.map").string(); }

  std::unique_ptr<MaterialService> service() const
  {
    auto options = MaterialServiceOptions{};
    options.makeSource = [path = m_env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    };
    options.pollInterval = 0ms;
    return MaterialService::create(std::move(options))
           | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  }
};

} // namespace

TEST_CASE("SceneGeometry, a model's references, never clipped (LP7.2, C52, D130, D140)")
{
  static_cast<void>(skinTestTable());
  const auto game = ModelBakeGame{};
  auto service = game.service();
  auto pool = CpuPool::create() | kdl::value();
  {
    auto scene = SceneGeometry{pool.get(), service.get()};
    const auto span = [](const std::string& name, const std::string& origin) {
      return "{\n\"classname\" \"func_static\"\n\"name\" \"" + name
             + "\"\n\"model\" \"models/test/span.obj\"\n\"origin\" \"" + origin
             + "\"\n}\n";
    };
    // the fixture map and three span boxes: through the doorway (its pivot east, its
    // centre west), through the wall beside the doorway, and far outside the hull
    auto snapshot = snapshotOfMapText(
      readTextFile(litFixtureFolder() / "maps" / "lp1b_geometry.map")
      + span("doorway", "20 0 8") + span("wall", "20 56 8")
      + span("outside", "5000 0 8"));
    snapshot.sequence = 1;
    snapshot.mapPath = game.mapPath();
    scene.takeSnapshot(snapshot);
    const auto doorway = entityNamed(snapshot, "doorway");
    const auto wall = entityNamed(snapshot, "wall");
    const auto outside = entityNamed(snapshot, "outside");
    const auto has = [&](const PreviewId id) {
      return std::ranges::any_of(scene.instances(), [&](const auto& entry) {
        return entry.first.entity == id
               && entry.second.geometry->record->state == ModelRecord::State::Ready;
      });
    };
    const auto settled = waitUntil(
      [&] {
        scene.collect();
        scene.finishForTesting();
        return scene.procTree() && scene.areaData() && has(doorway) && has(wall)
               && has(outside) && !scene.waitsForModels();
      },
      10000ms);
    REQUIRE(settled);
    const auto setOf = [](const std::vector<int>& areas) {
      return std::set<int>{areas.begin(), areas.end()};
    };

    // the doorway box: world x -40..10 across the portal plane, both areas; its
    // references from the record's bounds (R_CreateEntityRefs), in the chain order
    const auto& d = scene.entityStates().at(doorway);
    CHECK(
      d.localReferenceBounds
      == EngineBounds{EngineVec3{-60, -8, 24}, EngineVec3{-10, 8, 40}});
    CHECK(
      d.globalReferenceBounds
      == EngineBounds{EngineVec3{-40, -8, 32}, EngineVec3{10, 8, 48}});
    CHECK(setOf(d.areas) == std::set<int>{0, 1});
    auto pushed = pushFrustumIntoTree(
      *scene.procTree(),
      scene.areaData()->commonAreas,
      d.inverseBaseModelProject,
      engineUnitCube());
    std::ranges::reverse(pushed);
    CHECK(d.areas == pushed);
    // the chunk key keeps the pivot's area as a key only (east: 0)
    const auto instance = std::ranges::find_if(scene.instances(), [&](const auto& entry) {
      return entry.first.entity == doorway;
    });
    REQUIRE(instance != scene.instances().end());
    CHECK(instance->first.area == 0);
    CHECK(instance->second.areas == d.areas);

    // through the wall beside the doorway: never clipped, both areas (SC625)
    CHECK(setOf(scene.entityStates().at(wall).areas) == std::set<int>{0, 1});

    // outside the hull: no reference, the outside set (D81, K14 extended; the engine
    // draws nothing of an entity without a reference: the recorded difference, D140)
    const auto& o = scene.entityStates().at(outside);
    CHECK(o.areas.empty());
    CHECK_FALSE(engineBoundsIsCleared(o.globalReferenceBounds));

    // the frontend: the grid area per surface is the area of its centre among the
    // entity's references (tr_frontend_addmodels.cpp:812-853), the second area here;
    // the outside box is drawn when in the frustum
    const auto& table = *skinTestTable();
    auto grid = GridTable{};
    for (const auto area : {0, 1})
    {
      auto entry = GridArea{};
      entry.area = area;
      entry.hasPoints = true;
      grid.areas.push_back(entry);
    }
    auto camera = CameraState{};
    camera.position = vm::vec3f{-150, 0, 32};
    camera.viewportWidth = 160;
    camera.viewportHeight = 120;
    auto inputs = FrontendInputs{};
    inputs.view = litViewOf(camera, 160, 120);
    inputs.chunks = &scene.chunks();
    inputs.instances = &scene.instances();
    inputs.entities = &scene.entityStates();
    inputs.tree = scene.procTree().get();
    inputs.areas = scene.areaData().get();
    inputs.grid = &grid;
    inputs.gridValid = [](int) { return true; };
    inputs.material = [&](const std::string& name) {
      const auto slot = table.find(name);
      return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
    };
    const auto lists = buildFrontendLists(inputs);
    const auto surface = std::ranges::find_if(lists.drawSurfs, [&](const LitSurface& s) {
      return s.instance && s.instance->entity == doorway;
    });
    REQUIRE(surface != lists.drawSurfs.end());
    REQUIRE(surface->gridArea);
    CHECK(*surface->gridArea == 1);
    // the outside box at x 4940..4990 is behind no portal: seen down the view's x axis
    auto far = camera;
    far.position = vm::vec3f{4900, 0, 32};
    inputs.view = litViewOf(far, 160, 120);
    const auto farLists = buildFrontendLists(inputs);
    CHECK(std::ranges::any_of(farLists.drawSurfs, [&](const LitSurface& s) {
      return s.instance && s.instance->entity == outside && !s.gridArea;
    }));
  }
  pool->stop();
}

TEST_CASE("SceneGeometry: an entity id taken by another kind (LP7.2, C52)")
{
  auto model = ModelScene{};
  const auto lamp = model.take(
    WorldBox
    + pointEntity(
      "light",
      "\"name\" \"lamp\"\n\"model\" \"models/test/quad.obj\"\n\"origin\" \"60 0 2\"\n"));
  const auto id = entityNamed(lamp, "lamp");
  REQUIRE(model.instancesOf(id).size() == 1);
  // the same id a func_static with an .md5mesh: its instance goes
  const auto md5 = model.take(
    WorldBox
    + pointEntity(
      "func_static",
      "\"name\" \"md5\"\n\"model\" \"models/test/n11.md5mesh\"\n\"origin\" \"60 0 "
      "2\"\n"));
  REQUIRE(entityNamed(md5, "md5") == id);
  CHECK(model.instancesOf(id).empty());
  CHECK(model.scene->modelCounts().instances == 0);
  CHECK(model.scene->modelCounts().skinned == 1);
  // and back to a light without a model, then removed
  model.take(
    WorldBox + pointEntity("light", "\"name\" \"plain\"\n\"origin\" \"60 0 2\"\n"));
  CHECK(model.scene->modelCounts().entities == 0);
}

} // namespace tb::lit
