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

#include "kd/invoke.h"
#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

// writes the bytes as they are, unlike TestEnvironment::createFile, which writes text
void writeBytes(const std::filesystem::path& path, const std::string& bytes)
{
  auto stream = std::ofstream{path, std::ios::out | std::ios::binary};
  stream << bytes;
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
      CHECK(
        torch->hash == fnv1a64HexNormalizedLineEndings(loadBytes(torch->absolutePath)));
      CHECK(!torch->error);
      CHECK(torch->thumbnailPath == std::nullopt);
      CHECK(torch->metadata.kind == PrefabKind::Entity);
      CHECK(torch->metadata.name == "Torch (user)");
      CHECK(torch->metadata.className == "light");
      CHECK(torch->metadata.tags == std::vector<std::string>{"light", "torch"});
      REQUIRE(torch->world);
      // DD-20: nothing queries a library world spatially, so its tree is empty
      CHECK(torch->world->nodeTree().empty());
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
      REQUIRE(crate->world);
      CHECK(crate->world->nodeTree().empty());
      CHECK(
        crate->hash == fnv1a64HexNormalizedLineEndings(loadBytes(crate->absolutePath)));
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
      CHECK(library.fromPayload("prefab:user/lights\\torch.map") != nullptr);
#else
      CHECK(library.find(PrefabRoot::Mod, "props/crate.map") != nullptr);
      CHECK(library.find(PrefabRoot::User, "props/crate.map") == nullptr);
      CHECK(library.find(PrefabRoot::User, "props/Crate.map") != nullptr);
#endif
    }

    SECTION("Line endings do not change the hash")
    {
      // DD-12: a checkout may rewrite CRLF to LF or the other way round
      writeBytes(env.dir() / "user" / "lights" / "lf.map", UserTorch);
      writeBytes(
        env.dir() / "user" / "lights" / "crlf.map",
        kdl::str_replace_every(UserTorch, "\n", "\r\n"));
      library.rescan();
      waitForScan(library);

      const auto* lf = library.find(PrefabRoot::User, "lights/lf.map");
      const auto* crlf = library.find(PrefabRoot::User, "lights/crlf.map");
      REQUIRE(lf);
      REQUIRE(crlf);
      CHECK(lf->fileSize != crlf->fileSize);
      CHECK(lf->hash == crlf->hash);
      CHECK(lf->hash == fnv1a64Hex(UserTorch));
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
      const auto* broken = library.find(PrefabRoot::Mod, "broken.map");
      REQUIRE(broken);
      REQUIRE(broken->error);

      library.rescan();
      library.rescan();
      waitForScan(library);
      CHECK(library.entries().size() == 4);
      // R2-pf-ui-3: nothing changed, so nothing is reported; the files that failed were
      // read again (DD-22) and failed as before, so their entries are kept
      CHECK(notifications == 1);
      CHECK(library.find(PrefabRoot::Mod, "broken.map") == broken);

      library.rescan();
      waitForScan(library);
      CHECK(library.entries().size() == 4);
      CHECK(notifications == 1);

      SECTION("A removed file is reported")
      {
        REQUIRE(env.remove("base/prefabs/notaprefab.map"));
        library.rescan();
        waitForScan(library);
        CHECK(library.entries().size() == 3);
        CHECK(notifications == 2);
      }

      SECTION("A failing file that changed is reported")
      {
        env.createFile("base/prefabs/broken.map", Broken + "\"more\" \"text\"\n");
        library.rescan();
        waitForScan(library);
        CHECK(notifications == 2);
      }
    }

    SECTION("Directories")
    {
      const auto normal = [](const std::filesystem::path& path) {
        return path.lexically_normal();
      };
      CHECK(
        library.directories()
        == std::vector<std::filesystem::path>{
          normal(env.dir() / "base" / "prefabs"),
          normal(env.dir() / "base" / "prefabs" / "lights"),
          normal(env.dir() / "base" / "prefabs" / "props"),
          normal(env.dir() / "user"),
          normal(env.dir() / "user" / "lights"),
        });

      // R2-pf-ui-1: an empty folder is listed, so that it can be watched; a new folder
      // changes no entry, so the scan is silent
      env.createDirectory("user/empty/deeper");
      library.rescan();
      waitForScan(library);
      CHECK(notifications == 1);
      CHECK(kdl::ranges::contains(
        library.directories(), normal(env.dir() / "user" / "empty")));
      CHECK(kdl::ranges::contains(
        library.directories(), normal(env.dir() / "user" / "empty" / "deeper")));
      CHECK(library.directories().size() == 7u);
    }

    SECTION("Autosave folders are skipped (DD-27)")
    {
      // the backups of a map saved in a root, in any case of the folder's name
      env.createDirectory("user/autosave/deeper");
      env.createFile("user/autosave/x.1.map", UserTorch);
      env.createFile("user/autosave/deeper/y.map", UserTorch);
      env.createDirectory("user/lights/AutoSave");
      env.createFile("user/lights/AutoSave/torch.1.map", UserTorch);
      library.rescan();
      waitForScan(library);

      CHECK(library.entries().size() == 4u);
      CHECK(library.find(PrefabRoot::User, "autosave/x.1.map") == nullptr);
      CHECK(library.find(PrefabRoot::User, "autosave/deeper/y.map") == nullptr);
      CHECK(library.find(PrefabRoot::User, "lights/AutoSave/torch.1.map") == nullptr);
      CHECK(library.directories().size() == 5u);
      for (const auto& folder : {
             env.dir() / "user" / "autosave",
             env.dir() / "user" / "autosave" / "deeper",
             env.dir() / "user" / "lights" / "AutoSave",
           })
      {
        CHECK(!kdl::ranges::contains(library.directories(), folder.lexically_normal()));
      }

      // nor is a backup read when it is saved
      library.updateEntry(env.dir() / "user" / "autosave" / "x.1.map");
      CHECK_FALSE(library.scanning());
      library.processPendingResults();
      CHECK(library.entries().size() == 4u);
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
      // nothing changed, so nothing is reported (R2-pf-ui-3)
      CHECK(notifications == 1);
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
        CHECK(notifications == 2);
        CHECK(changedTorch->metadata.name == "Torch (changed)");
        CHECK(
          changedTorch->hash
          == fnv1a64HexNormalizedLineEndings(loadBytes(changedTorch->absolutePath)));
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
        // the cell shows the thumbnail, so this is a change
        CHECK(notifications == 2);
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

      SECTION("Of two updates of one file, the later one wins")
      {
        // R2-pf-mdl-6: the first update blocks its worker until the second one has been
        // taken over, so that it finishes last; the state is shared with the worker,
        // which may outlive this section if a check fails
        struct HookState
        {
          std::atomic<bool> entered = false;
          std::atomic<bool> release = false;
          std::atomic<int> calls = 0;
        };
        const auto state = std::make_shared<HookState>();
        library.setReadHookForTesting([state](const auto&) {
          if (state->calls++ == 0)
          {
            state->entered = true;
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!state->release && std::chrono::steady_clock::now() < deadline)
            {
              std::this_thread::sleep_for(1ms);
            }
          }
        });
        auto& entered = state->entered;
        auto& release = state->release;
        // the first update is never left blocked, whatever fails below
        const auto releaseFirst = kdl::invoke_later{[state]() { state->release = true; }};

        const auto path = env.dir() / "user" / "lights" / "torch.map";
        env.createFile(
          "user/lights/torch.map",
          kdl::str_replace_every(UserTorch, "Torch (user)", "Torch (first)"));
        library.updateEntry(path);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!entered && std::chrono::steady_clock::now() < deadline)
        {
          std::this_thread::sleep_for(1ms);
        }
        REQUIRE(entered);

        env.createFile(
          "user/lights/torch.map",
          kdl::str_replace_every(UserTorch, "Torch (user)", "Torch (second, longer)"));
        const auto secondSize = std::filesystem::file_size(path);
        library.updateEntry(path);
        while (torch->fileSize != secondSize
               && std::chrono::steady_clock::now() < deadline)
        {
          library.processPendingResults();
          std::this_thread::sleep_for(1ms);
        }
        REQUIRE(torch->fileSize == secondSize);
        CHECK(torch->metadata.name == "Torch (second, longer)");
        CHECK(notifications == 2);

        // the first update finishes now, and is dropped
        release = true;
        waitForScan(library);
        CHECK(library.find(PrefabRoot::User, "lights/torch.map") == torch);
        CHECK(torch->fileSize == secondSize);
        CHECK(torch->metadata.name == "Torch (second, longer)");
        CHECK(notifications == 2);
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

  SECTION("A save outside the roots does not restart a running scan")
  {
    // R2-pf3-1: another window's prefab editor saves a file of its own library
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

    library.rescan();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (entered < 2 && std::chrono::steady_clock::now() < deadline)
    {
      library.processPendingResults();
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(entered == 2);

    library.updateEntry(env.dir() / "elsewhere" / "torch.map");
    library.updateEntry(env.dir() / "user" / "autosave" / "x.1.map");
    CHECK(library.scanning());
    release = true;
    waitForScan(library);

    // each of the four files was read once, by the one scan
    CHECK(reads == 4);
    CHECK(library.entries().size() == 4);
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

  SECTION("A file that failed is read again by the next scan")
  {
    // DD-22: the file is unchanged, but a failure may have been transient
    auto fail = std::atomic<bool>{true};
    library.setReadHookForTesting([&](const auto& path) {
      if (fail && path.filename() == "crate.map")
      {
        throw std::runtime_error{"locked by another process"};
      }
    });

    library.rescan();
    waitForScan(library);

    const auto* failed = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(failed);
    CHECK(failed->error == "locked by another process");
    CHECK(!failed->hasBody());

    fail = false;
    library.rescan();
    waitForScan(library);

    const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(crate);
    CHECK(!crate->error);
    CHECK(crate->world);
    CHECK(crate->metadata.name == "crate");
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
    // no entity, no colour
    CHECK(big->bodyPreview->firstEntityClassname.empty());
    CHECK(big->bodyPreview->color == std::nullopt);

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

  SECTION("A large assembly keeps the colour of its first entity")
  {
    // DD-21: the cell of a dropped world is drawn in the colour it would have had
    auto* redLightNode = new EntityNode{
      Entity{{{"classname", "light"}, {"origin", "0 0 64"}, {"_color", "1 0 0"}}}};
    auto bigNodes = std::vector<Node*>{redLightNode};
    for (size_t i = 0; i < PrefabWorldBrushBudget + 1; ++i)
    {
      bigNodes.push_back(createBrushNode(map));
    }
    addNodes(map, {{&parentForNodes(map), bigNodes}});
    REQUIRE(
      savePrefab(
        map, bigNodes, SavePrefabOptions{.name = "bigred", .root = PrefabRoot::Mod})
      | kdl::is_success());

    library.rescan();
    waitForScan(library);

    const auto* big = library.find(PrefabRoot::Mod, "bigred.map");
    REQUIRE(big);
    REQUIRE(big->bodyPreview);
    CHECK(big->world == nullptr);
    CHECK(big->bodyPreview->firstEntityClassname == "light");
    CHECK(big->bodyPreview->firstEntityLightColor == "1 0 0");
    CHECK(big->bodyPreview->color == RgbaF{1.0f, 0.0f, 0.0f, 1.0f});

    SECTION("The colour follows the entity definitions")
    {
      // without a definition, a light's _color still counts
      map.entityDefinitionsWillChangeNotifier();
      map.entityDefinitionManager().clear();
      map.entityDefinitionsDidChangeNotifier();
      library.processPendingResults();
      CHECK(big->bodyPreview->color == RgbaF{1.0f, 0.0f, 0.0f, 1.0f});
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
