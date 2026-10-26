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
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/EntityState.h"
#include "lit/MaterialService.h"
#include "lit/ModelInstance.h"
#include "lit/SceneGeometry.h"

#include "kd/result.h"

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

} // namespace tb::lit
