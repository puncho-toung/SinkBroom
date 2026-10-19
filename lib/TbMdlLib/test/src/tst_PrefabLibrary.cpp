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

#include "fs/TestEnvironment.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/ContentHash.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

using namespace std::chrono_literals;

const auto UserTorch = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
"_tb_prefab_name" "Torch (user)"
"_tb_prefab_class" "light"
"_tb_prefab_tags" "light;torch"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
"name" "torch"
}
)PREFAB"};

const auto ModTorch = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
"_tb_prefab_name" "Torch (mod)"
"_tb_prefab_class" "light"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
}
)PREFAB"};

const auto NotAPrefab = std::string{R"PREFAB(// entity 0
{
"classname" "worldspawn"
}
)PREFAB"};

const auto Broken = std::string{R"PREFAB({
"classname" "worldspawn"
)PREFAB"};

void waitForScan(PrefabLibrary& library)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (library.scanning() && std::chrono::steady_clock::now() < deadline)
  {
    library.processPendingResults();
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(library.scanning());
}

std::string loadBytes(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::in | std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

std::vector<std::string> linkIds(const std::vector<Node*>& nodes)
{
  auto result = std::vector<std::string>{};
  Node::visitAll(
    nodes,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        result.push_back(groupNode.linkId());
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        result.push_back(entityNode.linkId());
        entityNode.visitChildren(thisLambda);
      },
      [&](BrushNode& brushNode) { result.push_back(brushNode.linkId()); },
      [&](PatchNode& patchNode) { result.push_back(patchNode.linkId()); }));
  return result;
}

const EntityNode* firstEntityNode(const WorldNode& worldNode)
{
  return dynamic_cast<const EntityNode*>(worldNode.defaultLayer()->children().front());
}

} // namespace

TEST_CASE("PrefabLibrary")
{
  auto env = fs::TestEnvironment{[](auto& e) {
    e.createDirectory("user/lights");
    e.createDirectory("base/prefabs/lights");
    e.createFile("user/lights/torch.map", UserTorch);
    e.createFile("base/prefabs/lights/torch.map", ModTorch);
    e.createFile("base/prefabs/broken.map", Broken);
    e.createFile("base/prefabs/notaprefab.map", NotAPrefab);
    e.createFile("base/prefabs/readme.txt", "not a map");
  }};

  auto fixture = MapFixture{};
  auto& map = fixture.create(MapFixtureConfig{
    .mapFormat = MapFormat::Doom3_Valve,
    .gameInfo = detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, env.dir()),
  });
  // the Doom 3 BFG fixture loads an FGD; let the map release those definitions first
  map.entityDefinitionsWillChangeNotifier();
  map.entityDefinitionManager().setDefinitions({
    {"light",
     Color{},
     "",
     {PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""}},
     PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
  });

  auto& library = map.prefabLibrary();

  // an assembly in the mod root, written by savePrefab
  auto* lightNode = new EntityNode{
    Entity{{{"classname", "light"}, {"origin", "0 0 0"}, {"name", "lamp"}}}};
  auto* brushNode = createBrushNode(map);
  addNodes(map, {{&parentForNodes(map), {lightNode, brushNode}}});
  REQUIRE(
    savePrefab(
      map,
      {lightNode, brushNode},
      SavePrefabOptions{.name = "crate", .category = "props", .root = PrefabRoot::Mod})
    | kdl::is_success());
  env.createFile("base/prefabs/props/crate.png", "not really a png");

  SECTION("Roots")
  {
    CHECK(
      library.roots()
      == std::vector<std::filesystem::path>{env.dir() / "base" / "prefabs"});
    CHECK(library.writeRootFor(PrefabRoot::User).empty());
    CHECK(library.writeRootFor(PrefabRoot::Mod) == env.dir() / "base" / "prefabs");

    library.setUserRoot(env.dir() / "user");
    CHECK(
      library.roots()
      == std::vector<std::filesystem::path>{
        env.dir() / "user", env.dir() / "base" / "prefabs"});
  }

  SECTION("Scanning")
  {
    library.setUserRoot(env.dir() / "user");

    auto notifications = 0;
    auto connection =
      library.libraryDidChangeNotifier.connect([&]() { ++notifications; });

    library.rescan();
    CHECK(library.scanning());
    waitForScan(library);
    CHECK(notifications == 1);

    const auto& entries = library.entries();
    auto paths = std::vector<std::string>{};
    for (const auto& entry : entries)
    {
      paths.push_back(entry->relativePathString);
    }

    // sorted by root, then path; the user torch shadows the mod torch
    CHECK(
      paths
      == std::vector<std::string>{
        "lights/torch.map", "broken.map", "notaprefab.map", "props/crate.map"});
    CHECK(entries[0]->root == PrefabRoot::User);
    CHECK(entries[1]->root == PrefabRoot::Mod);
    CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == nullptr);

    SECTION("Entries")
    {
      const auto* torch = library.find(PrefabRoot::User, "lights/torch.map");
      REQUIRE(torch == entries[0].get());
      CHECK(torch->absolutePath == env.dir() / "user" / "lights" / "torch.map");
      CHECK(torch->hash == fnv1a64Hex(loadBytes(torch->absolutePath)));
      CHECK(!torch->error);
      CHECK(torch->thumbnailPath == std::nullopt);
      CHECK(torch->metadata.kind == PrefabKind::Entity);
      CHECK(torch->metadata.name == "Torch (user)");
      CHECK(torch->metadata.className == "light");
      CHECK(torch->metadata.tags == std::vector<std::string>{"light", "torch"});
      REQUIRE(torch->world);
      // definitions are assigned by classname
      const auto* torchNode = firstEntityNode(*torch->world);
      REQUIRE(torchNode);
      CHECK(
        torchNode->entity().definition()
        == map.entityDefinitionManager().definition("light"));

      const auto* broken = library.find(PrefabRoot::Mod, "broken.map");
      REQUIRE(broken);
      CHECK(broken->error);
      CHECK(broken->world == nullptr);
      CHECK(broken->metadata.name == "broken");

      const auto* notAPrefab = library.find(PrefabRoot::Mod, "notaprefab.map");
      REQUIRE(notAPrefab);
      CHECK(notAPrefab->error);
      CHECK(notAPrefab->world == nullptr);

      const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
      REQUIRE(crate);
      CHECK(crate->metadata.kind == PrefabKind::Assembly);
      CHECK(crate->metadata.name == "crate");
      CHECK(
        crate->thumbnailPath == env.dir() / "base" / "prefabs" / "props" / "crate.png");
      CHECK(crate->hash == fnv1a64Hex(loadBytes(crate->absolutePath)));
    }

    SECTION("File names compare as the platform compares them")
    {
      env.createDirectory("user/props");
      env.createFile("user/props/Crate.map", UserTorch);
      library.rescan();
      waitForScan(library);

#ifdef _WIN32
      // Crate.map and crate.map are one file name, so the user root wins
      CHECK(library.find(PrefabRoot::Mod, "props/crate.map") == nullptr);
      CHECK(library.find(PrefabRoot::User, "props/crate.map") != nullptr);
      CHECK(library.find(PrefabRoot::User, "PROPS/CRATE.MAP") != nullptr);
      CHECK(library.fromPayload("prefab:user/Lights/Torch.map") != nullptr);
#else
      CHECK(library.find(PrefabRoot::Mod, "props/crate.map") != nullptr);
      CHECK(library.find(PrefabRoot::User, "props/crate.map") == nullptr);
      CHECK(library.find(PrefabRoot::User, "props/Crate.map") != nullptr);
#endif
    }

    SECTION("Payloads")
    {
      for (const auto& entry : entries)
      {
        CHECK(library.fromPayload(library.payloadFor(*entry)) == entry.get());
      }
      CHECK(library.payloadFor(*entries[0]) == "prefab:user/lights/torch.map");
      CHECK(library.payloadFor(*entries[3]) == "prefab:mod/props/crate.map");
      CHECK(library.fromPayload("prefab:mod/lights/torch.map") == nullptr);
      CHECK(library.fromPayload("entity:light") == nullptr);
      CHECK(library.fromPayload("prefab:other/lights/torch.map") == nullptr);
    }

    SECTION("Rescanning")
    {
      library.rescan();
      library.rescan();
      waitForScan(library);
      CHECK(library.entries().size() == 4);
      CHECK(notifications == 2);

      library.rescan();
      waitForScan(library);
      CHECK(library.entries().size() == 4);
    }

    SECTION("cloneBody")
    {
      const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
      REQUIRE(crate);

      auto clones1 = library.cloneBody(*crate, map.worldBounds());
      auto clones2 = library.cloneBody(*crate, map.worldBounds());
      REQUIRE(clones1.size() == 1);
      REQUIRE(clones2.size() == 1);

      const auto* groupNode = dynamic_cast<const GroupNode*>(clones1.front());
      REQUIRE(groupNode);
      CHECK(groupNode->group().name() == "crate");

      // re-keyed: the two clones and the library entry share no link id
      const auto ids1 = linkIds(clones1);
      const auto ids2 = linkIds(clones2);
      const auto entryIds = linkIds(crate->world->defaultLayer()->children());
      for (const auto& id : ids1)
      {
        CHECK(!kdl::ranges::contains(ids2, id));
        CHECK(!kdl::ranges::contains(entryIds, id));
      }

      // definitions survive the clone
      const auto* clonedLightNode = static_cast<const EntityNode*>(nullptr);
      for (const auto* child : groupNode->children())
      {
        if (const auto* entityNode = dynamic_cast<const EntityNode*>(child))
        {
          clonedLightNode = entityNode;
        }
      }
      REQUIRE(clonedLightNode);
      CHECK(
        clonedLightNode->entity().definition()
        == map.entityDefinitionManager().definition("light"));

      kdl::vec_clear_and_delete(clones1);
      kdl::vec_clear_and_delete(clones2);

      CHECK(
        library.cloneBody(*library.find(PrefabRoot::Mod, "broken.map"), map.worldBounds())
          .empty());
    }

    SECTION("Entity definitions are reloaded")
    {
      const auto* torchNode = firstEntityNode(*entries[0]->world);
      REQUIRE(torchNode);

      map.entityDefinitionsWillChangeNotifier();
      CHECK(torchNode->entity().definition() == nullptr);

      map.entityDefinitionsDidChangeNotifier();
      library.processPendingResults();
      CHECK(
        torchNode->entity().definition()
        == map.entityDefinitionManager().definition("light"));
      CHECK(notifications == 2);
    }

    SECTION("A rescan reuses unchanged entries")
    {
      const auto* torch = library.find(PrefabRoot::User, "lights/torch.map");
      const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
      REQUIRE(torch);
      REQUIRE(crate);
      const auto* torchWorld = torch->world.get();

      library.rescan();
      waitForScan(library);
      CHECK(notifications == 2);
      // same address, same world
      CHECK(library.find(PrefabRoot::User, "lights/torch.map") == torch);
      CHECK(torch->world.get() == torchWorld);
      CHECK(library.find(PrefabRoot::Mod, "props/crate.map") == crate);

      SECTION("A changed size is read again")
      {
        env.createFile(
          "user/lights/torch.map",
          kdl::str_replace_every(UserTorch, "Torch (user)", "Torch (changed)"));
        library.rescan();
        waitForScan(library);

        const auto* changedTorch = library.find(PrefabRoot::User, "lights/torch.map");
        REQUIRE(changedTorch);
        CHECK(changedTorch->metadata.name == "Torch (changed)");
        CHECK(changedTorch->hash == fnv1a64Hex(loadBytes(changedTorch->absolutePath)));
        // the other entries are kept
        CHECK(library.find(PrefabRoot::Mod, "props/crate.map") == crate);
      }

      SECTION("A changed modification time is read again")
      {
        // the same size
        env.createFile(
          "user/lights/torch.map",
          kdl::str_replace_every(UserTorch, "Torch (user)", "Torch (iser)"));
        const auto path = env.dir() / "user" / "lights" / "torch.map";
        std::filesystem::last_write_time(path, torch->modificationTime + 10s);
        REQUIRE(std::filesystem::file_size(path) == torch->fileSize);

        library.rescan();
        waitForScan(library);

        const auto* changedTorch = library.find(PrefabRoot::User, "lights/torch.map");
        REQUIRE(changedTorch);
        CHECK(changedTorch->metadata.name == "Torch (iser)");
      }

      SECTION("A new thumbnail is picked up")
      {
        env.createFile("user/lights/torch.png", "not really a png");
        library.rescan();
        waitForScan(library);
        CHECK(library.find(PrefabRoot::User, "lights/torch.map") == torch);
        CHECK(torch->thumbnailPath == env.dir() / "user" / "lights" / "torch.png");
      }
    }

    SECTION("updateEntry")
    {
      const auto* torch = library.find(PrefabRoot::User, "lights/torch.map");
      REQUIRE(torch);

      SECTION("A changed file is read again in place")
      {
        env.createFile(
          "user/lights/torch.map",
          kdl::str_replace_every(UserTorch, "Torch (user)", "Torch (saved)"));
        library.updateEntry(env.dir() / "user" / "lights" / "torch.map");
        CHECK(library.scanning());
        waitForScan(library);

        CHECK(notifications == 2);
        CHECK(library.entries().size() == 4);
        CHECK(library.find(PrefabRoot::User, "lights/torch.map") == torch);
        CHECK(torch->metadata.name == "Torch (saved)");
        REQUIRE(torch->world);
        CHECK(
          firstEntityNode(*torch->world)->entity().definition()
          == map.entityDefinitionManager().definition("light"));
      }

      SECTION("A new file is added in order")
      {
        env.createFile("user/lights/lamp.map", UserTorch);
        library.updateEntry(env.dir() / "user" / "lights" / "lamp.map");
        waitForScan(library);

        CHECK(notifications == 2);
        REQUIRE(library.entries().size() == 5);
        CHECK(library.entries()[0]->relativePathString == "lights/lamp.map");
        CHECK(library.find(PrefabRoot::User, "lights/torch.map") == torch);
      }

      SECTION("A shadowed file of the mod root stays hidden")
      {
        library.updateEntry(env.dir() / "base" / "prefabs" / "lights" / "torch.map");
        waitForScan(library);
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == nullptr);
        CHECK(library.entries().size() == 4);
      }

      SECTION("A removed file rescans")
      {
        REQUIRE(env.remove("user/lights/torch.map"));
        library.updateEntry(env.dir() / "user" / "lights" / "torch.map");
        waitForScan(library);

        CHECK(library.find(PrefabRoot::User, "lights/torch.map") == nullptr);
        // the mod torch is no longer shadowed
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") != nullptr);
      }

      SECTION("A file under no root is ignored")
      {
        library.updateEntry(env.dir() / "elsewhere" / "torch.map");
        CHECK_FALSE(library.scanning());
        library.processPendingResults();
        CHECK(notifications == 1);
      }
    }
  }

  SECTION("An abandoned scan reads no more files")
  {
    library.setUserRoot(env.dir() / "user");

    auto entered = std::atomic<int>{0};
    auto reads = std::atomic<int>{0};
    auto release = std::atomic<bool>{false};
    library.setReadHookForTesting([&](const auto&) {
      ++reads;
      ++entered;
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (!release && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(1ms);
      }
    });

    auto notifications = 0;
    auto connection =
      library.libraryDidChangeNotifier.connect([&]() { ++notifications; });

    // four files to read (the mod torch is shadowed), two of them block the two workers
    library.rescan();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (entered < 2 && std::chrono::steady_clock::now() < deadline)
    {
      library.processPendingResults();
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(entered == 2);

    library.rescan();
    release = true;
    waitForScan(library);

    // the two queued tasks of the first scan returned without reading
    CHECK(reads == 6);
    CHECK(library.entries().size() == 4);
    CHECK(notifications == 1);
  }

  SECTION("An exception on a worker becomes an error")
  {
    library.setReadHookForTesting([](const auto& path) {
      if (path.filename() == "notaprefab.map")
      {
        throw std::runtime_error{"cannot read"};
      }
    });

    library.rescan();
    waitForScan(library);

    const auto* notAPrefab = library.find(PrefabRoot::Mod, "notaprefab.map");
    REQUIRE(notAPrefab);
    CHECK(notAPrefab->error == "cannot read");
    CHECK(notAPrefab->world == nullptr);
    CHECK(notAPrefab->metadata.name == "notaprefab");

    const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(crate);
    CHECK(crate->world);
  }

  SECTION("A large assembly keeps no world")
  {
    auto bigNodes = std::vector<Node*>{};
    for (size_t i = 0; i < PrefabWorldBrushBudget + 1; ++i)
    {
      bigNodes.push_back(createBrushNode(map));
    }
    addNodes(map, {{&parentForNodes(map), bigNodes}});
    REQUIRE(
      savePrefab(map, bigNodes, SavePrefabOptions{.name = "big", .root = PrefabRoot::Mod})
      | kdl::is_success());

    library.rescan();
    waitForScan(library);

    const auto* big = library.find(PrefabRoot::Mod, "big.map");
    REQUIRE(big);
    CHECK(!big->error);
    CHECK(big->world == nullptr);
    CHECK(big->hasBody());
    REQUIRE(big->bodyPreview);
    CHECK(big->bodyPreview->edgeVertices.size() == 2 * PrefabPreviewMaxEdges);
    CHECK(big->bodyPreview->bounds.size() == vm::vec3d{32, 32, 32});

    // an assembly within the budget keeps its world
    const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(crate);
    CHECK(crate->world);
    CHECK(!crate->bodyPreview);

    SECTION("cloneBody reads the file again")
    {
      auto clones = library.cloneBody(*big, map.worldBounds());
      REQUIRE(clones.size() == 1);
      const auto* groupNode = dynamic_cast<const GroupNode*>(clones.front());
      REQUIRE(groupNode);
      CHECK(groupNode->childCount() == PrefabWorldBrushBudget + 1);
      kdl::vec_clear_and_delete(clones);
    }

    SECTION("cloneBody of a changed file is empty")
    {
      env.createFile(
        "base/prefabs/big.map", loadBytes(big->absolutePath) + "// changed\n");
      CHECK(library.cloneBody(*big, map.worldBounds()).empty());
    }
  }

  SECTION("Missing roots are empty")
  {
    auto otherFixture = MapFixture{};
    auto& otherMap = otherFixture.create(MapFixtureConfig{
      .mapFormat = MapFormat::Doom3_Valve,
      .gameInfo =
        detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, env.dir() / "missing"),
    });
    auto& otherLibrary = otherMap.prefabLibrary();
    otherLibrary.setUserRoot(env.dir() / "missing" / "user");

    auto notifications = 0;
    auto connection =
      otherLibrary.libraryDidChangeNotifier.connect([&]() { ++notifications; });

    otherLibrary.rescan();
    waitForScan(otherLibrary);
    CHECK(otherLibrary.entries().empty());
    CHECK(notifications == 1);
  }
}

} // namespace tb::mdl
