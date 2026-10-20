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
#include "mdl/Group.h"
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

const auto Torch = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
"_tb_prefab_name" "Torch (one)"
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

const auto UserTorch = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
"_tb_prefab_name" "Torch (user)"
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
    e.createDirectory("base/maps/prefabs/lights");
    e.createFile("user/lights/torch.map", UserTorch);
    e.createFile("base/maps/prefabs/lights/torch.map", Torch);
    e.createFile("base/maps/prefabs/broken.map", Broken);
    e.createFile("base/maps/prefabs/notaprefab.map", NotAPrefab);
    e.createFile("base/maps/prefabs/readme.txt", "not a map");
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
  env.createFile("base/maps/prefabs/props/crate.png", "not really a png");

  SECTION("Roots")
  {
    CHECK(
      library.roots()
      == std::vector<std::filesystem::path>{env.dir() / "base" / "maps" / "prefabs"});
    CHECK(library.writeRootFor(PrefabRoot::User).empty());
    CHECK(
      library.writeRootFor(PrefabRoot::Mod) == env.dir() / "base" / "maps" / "prefabs");

    CHECK(library.writeRoot() == PrefabRoot::Mod);
    CHECK(library.modFolder() == env.dir() / "base");

    // DD-32: a Prefab Path replaces the mod root
    library.setUserRoot(env.dir() / "user");
    CHECK(library.roots() == std::vector<std::filesystem::path>{env.dir() / "user"});
    CHECK(library.writeRootFor(PrefabRoot::User) == env.dir() / "user");
    CHECK(library.writeRootFor(PrefabRoot::Mod).empty());
    CHECK(library.writeRoot() == PrefabRoot::User);
    CHECK(library.modFolder() == env.dir() / "base");
  }

  SECTION("A Prefab Path replaces the mod root")
  {
    // DD-32: only the user root is scanned while it is set
    library.setUserRoot(env.dir() / "user");
    library.rescan();
    waitForScan(library);
    REQUIRE(library.entries().size() == 1u);
    CHECK(library.entries().front()->root == PrefabRoot::User);
    CHECK(library.entries().front()->metadata.name == "Torch (user)");
    CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == nullptr);
    CHECK(library.fromPayload("prefab:mod/props/crate.map") == nullptr);
    CHECK(library.fromPayload("prefab:user/lights/torch.map") != nullptr);

    // a save to the mod root is not read while the user root is set
    library.updateEntry(env.dir() / "base" / "maps" / "prefabs" / "props" / "crate.map");
    CHECK_FALSE(library.scanning());

    // cleared, the mod root's files come back
    library.setUserRoot({});
    library.rescan();
    waitForScan(library);
    CHECK(library.entries().size() == 4u);
    CHECK(library.find(PrefabRoot::Mod, "props/crate.map") != nullptr);
    CHECK(library.find(PrefabRoot::User, "lights/torch.map") == nullptr);
  }

  SECTION("Scanning")
  {
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

    // sorted by root, then path; the files that are not maps are skipped
    CHECK(
      paths
      == std::vector<std::string>{
        "broken.map", "lights/torch.map", "notaprefab.map", "props/crate.map"});
    for (const auto& entry : entries)
    {
      CHECK(entry->root == PrefabRoot::Mod);
    }

    SECTION("Entries")
    {
      const auto* torch = library.find(PrefabRoot::Mod, "lights/torch.map");
      REQUIRE(torch == entries[1].get());
      CHECK(
        torch->absolutePath
        == env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.map");
      CHECK(
        torch->hash == fnv1a64HexNormalizedLineEndings(loadBytes(torch->absolutePath)));
      CHECK(!torch->error);
      CHECK(torch->thumbnailPath == std::nullopt);
      CHECK(torch->metadata.kind == PrefabKind::Entity);
      CHECK(torch->metadata.name == "Torch (one)");
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
        crate->thumbnailPath
        == env.dir() / "base" / "maps" / "prefabs" / "props" / "crate.png");
      REQUIRE(crate->world);
      CHECK(crate->world->nodeTree().empty());
      CHECK(
        crate->hash == fnv1a64HexNormalizedLineEndings(loadBytes(crate->absolutePath)));
    }

    SECTION("File names compare as the platform compares them")
    {
#ifdef _WIN32
      CHECK(library.find(PrefabRoot::Mod, "PROPS/CRATE.MAP") != nullptr);
      CHECK(library.fromPayload("prefab:mod/Lights/Torch.map") != nullptr);
      CHECK(library.fromPayload("prefab:mod/lights\\torch.map") != nullptr);
#else
      CHECK(library.find(PrefabRoot::Mod, "PROPS/CRATE.MAP") == nullptr);
      CHECK(library.fromPayload("prefab:mod/Lights/Torch.map") == nullptr);
#endif
    }

    SECTION("Line endings do not change the hash")
    {
      // DD-12: a checkout may rewrite CRLF to LF or the other way round
      writeBytes(env.dir() / "base" / "maps" / "prefabs" / "lights" / "lf.map", Torch);
      writeBytes(
        env.dir() / "base" / "maps" / "prefabs" / "lights" / "crlf.map",
        kdl::str_replace_every(Torch, "\n", "\r\n"));
      library.rescan();
      waitForScan(library);

      const auto* lf = library.find(PrefabRoot::Mod, "lights/lf.map");
      const auto* crlf = library.find(PrefabRoot::Mod, "lights/crlf.map");
      REQUIRE(lf);
      REQUIRE(crlf);
      CHECK(lf->fileSize != crlf->fileSize);
      CHECK(lf->hash == crlf->hash);
      CHECK(lf->hash == fnv1a64Hex(Torch));
    }

    SECTION("Payloads")
    {
      for (const auto& entry : entries)
      {
        CHECK(library.fromPayload(library.payloadFor(*entry)) == entry.get());
      }
      CHECK(library.payloadFor(*entries[1]) == "prefab:mod/lights/torch.map");
      CHECK(library.payloadFor(*entries[3]) == "prefab:mod/props/crate.map");
      // DD-37: the relative path is looked up in the active root, whatever the
      // qualifier says
      CHECK(library.fromPayload("prefab:user/lights/torch.map") == entries[1].get());
      CHECK(library.fromPayload("prefab:user/lights/none.map") == nullptr);
      CHECK(library.fromPayload("entity:light") == nullptr);
      CHECK(library.fromPayload("prefab:other/lights/torch.map") == nullptr);
      CHECK(library.fromPayload("prefab:lights/torch.map") == nullptr);

      CHECK(library.entryForPrefabPath("mod/props/crate.map") == entries[3].get());
      CHECK(library.entryForPrefabPath("user/props/crate.map") == entries[3].get());
      CHECK(library.entryForPrefabPath("props/crate.map") == nullptr);
#ifdef _WIN32
      CHECK(library.entryForPrefabPath("user/Props/Crate.map") == entries[3].get());
#endif
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
        REQUIRE(env.remove("base/maps/prefabs/notaprefab.map"));
        library.rescan();
        waitForScan(library);
        CHECK(library.entries().size() == 3);
        CHECK(notifications == 2);
      }

      SECTION("A failing file that changed is reported")
      {
        env.createFile("base/maps/prefabs/broken.map", Broken + "\"more\" \"text\"\n");
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
          normal(env.dir() / "base" / "maps" / "prefabs"),
          normal(env.dir() / "base" / "maps" / "prefabs" / "lights"),
          normal(env.dir() / "base" / "maps" / "prefabs" / "props"),
        });

      // R2-pf-ui-1: an empty folder is listed, so that it can be watched; a new folder
      // changes no entry, so the scan is silent
      env.createDirectory("base/maps/prefabs/empty/deeper");
      library.rescan();
      waitForScan(library);
      CHECK(notifications == 1);
      CHECK(kdl::ranges::contains(
        library.directories(),
        normal(env.dir() / "base" / "maps" / "prefabs" / "empty")));
      CHECK(kdl::ranges::contains(
        library.directories(),
        normal(env.dir() / "base" / "maps" / "prefabs" / "empty" / "deeper")));
      CHECK(library.directories().size() == 5u);
    }

    SECTION("Autosave folders are skipped (DD-27)")
    {
      // the backups of a map saved in a root, in any case of the folder's name
      env.createDirectory("base/maps/prefabs/autosave/deeper");
      env.createFile("base/maps/prefabs/autosave/x.1.map", Torch);
      env.createFile("base/maps/prefabs/autosave/deeper/y.map", Torch);
      env.createDirectory("base/maps/prefabs/lights/AutoSave");
      env.createFile("base/maps/prefabs/lights/AutoSave/torch.1.map", Torch);
      library.rescan();
      waitForScan(library);

      CHECK(library.entries().size() == 4u);
      CHECK(library.find(PrefabRoot::Mod, "autosave/x.1.map") == nullptr);
      CHECK(library.find(PrefabRoot::Mod, "autosave/deeper/y.map") == nullptr);
      CHECK(library.find(PrefabRoot::Mod, "lights/AutoSave/torch.1.map") == nullptr);
      CHECK(library.directories().size() == 3u);
      for (const auto& folder : {
             env.dir() / "base" / "maps" / "prefabs" / "autosave",
             env.dir() / "base" / "maps" / "prefabs" / "autosave" / "deeper",
             env.dir() / "base" / "maps" / "prefabs" / "lights" / "AutoSave",
           })
      {
        CHECK(!kdl::ranges::contains(library.directories(), folder.lexically_normal()));
      }

      // nor is a backup read when it is saved
      library.updateEntry(
        env.dir() / "base" / "maps" / "prefabs" / "autosave" / "x.1.map");
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
      const auto* torchNode = firstEntityNode(*entries[1]->world);
      REQUIRE(torchNode);

      map.entityDefinitionsWillChangeNotifier();
      CHECK(torchNode->entity().definition() == nullptr);

      // R8-lib-7: assigned as soon as the map has loaded them, before the next
      // processPendingResults, which reports them
      map.entityDefinitionsDidChangeNotifier();
      REQUIRE(map.entityDefinitionManager().definition("light"));
      CHECK(
        torchNode->entity().definition()
        == map.entityDefinitionManager().definition("light"));
      CHECK(notifications == 1);

      library.processPendingResults();
      CHECK(notifications == 2);
      library.processPendingResults();
      CHECK(notifications == 2);
    }

    SECTION("A rescan reuses unchanged entries")
    {
      const auto* torch = library.find(PrefabRoot::Mod, "lights/torch.map");
      const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
      REQUIRE(torch);
      REQUIRE(crate);
      const auto* torchWorld = torch->world.get();

      library.rescan();
      waitForScan(library);
      // nothing changed, so nothing is reported (R2-pf-ui-3)
      CHECK(notifications == 1);
      // same address, same world
      CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
      CHECK(torch->world.get() == torchWorld);
      CHECK(library.find(PrefabRoot::Mod, "props/crate.map") == crate);

      SECTION("A changed size is read again")
      {
        env.createFile(
          "base/maps/prefabs/lights/torch.map",
          kdl::str_replace_every(Torch, "Torch (one)", "Torch (changed)"));
        library.rescan();
        waitForScan(library);

        const auto* changedTorch = library.find(PrefabRoot::Mod, "lights/torch.map");
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
          "base/maps/prefabs/lights/torch.map",
          kdl::str_replace_every(Torch, "Torch (one)", "Torch (two)"));
        const auto path =
          env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.map";
        std::filesystem::last_write_time(path, torch->modificationTime + 10s);
        REQUIRE(std::filesystem::file_size(path) == torch->fileSize);

        library.rescan();
        waitForScan(library);

        const auto* changedTorch = library.find(PrefabRoot::Mod, "lights/torch.map");
        REQUIRE(changedTorch);
        CHECK(changedTorch->metadata.name == "Torch (two)");
      }

      SECTION("A new thumbnail is picked up")
      {
        env.createFile("base/maps/prefabs/lights/torch.png", "not really a png");
        library.rescan();
        waitForScan(library);
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
        CHECK(
          torch->thumbnailPath
          == env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.png");
        // the cell shows the thumbnail, so this is a change
        CHECK(notifications == 2);
      }
    }

    SECTION("updateEntry")
    {
      const auto* torch = library.find(PrefabRoot::Mod, "lights/torch.map");
      REQUIRE(torch);

      SECTION("A changed file is read again in place")
      {
        env.createFile(
          "base/maps/prefabs/lights/torch.map",
          kdl::str_replace_every(Torch, "Torch (one)", "Torch (saved)"));
        library.updateEntry(
          env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.map");
        CHECK(library.scanning());
        waitForScan(library);

        CHECK(notifications == 2);
        CHECK(library.entries().size() == 4);
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
        CHECK(torch->metadata.name == "Torch (saved)");
        REQUIRE(torch->world);
        CHECK(
          firstEntityNode(*torch->world)->entity().definition()
          == map.entityDefinitionManager().definition("light"));
      }

      SECTION("A new file is added in order")
      {
        env.createFile("base/maps/prefabs/lights/lamp.map", Torch);
        library.updateEntry(
          env.dir() / "base" / "maps" / "prefabs" / "lights" / "lamp.map");
        waitForScan(library);

        CHECK(notifications == 2);
        REQUIRE(library.entries().size() == 5);
        CHECK(library.entries()[1]->relativePathString == "lights/lamp.map");
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
      }

      SECTION("A removed file rescans")
      {
        REQUIRE(env.remove("base/maps/prefabs/lights/torch.map"));
        library.updateEntry(
          env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.map");
        waitForScan(library);

        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == nullptr);
        CHECK(library.entries().size() == 3);
      }

      SECTION("A file under no root is ignored")
      {
        library.updateEntry(env.dir() / "elsewhere" / "torch.map");
        CHECK_FALSE(library.scanning());
        library.processPendingResults();
        CHECK(notifications == 1);
      }

      SECTION("A file that is not a map is ignored")
      {
        // R8-lib-6: Save As crate.bak in a root would add an entry that no scan lists
        env.createFile("base/maps/prefabs/props/crate.bak", Torch);
        library.updateEntry(
          env.dir() / "base" / "maps" / "prefabs" / "props" / "crate.bak");
        CHECK_FALSE(library.scanning());
        library.processPendingResults();
        CHECK(notifications == 1);
        CHECK(library.entries().size() == 4);

        // the extension is compared in any case
        env.createFile("base/maps/prefabs/props/BARREL.MAP", Torch);
        library.updateEntry(
          env.dir() / "base" / "maps" / "prefabs" / "props" / "BARREL.MAP");
        CHECK(library.scanning());
        waitForScan(library);
        CHECK(library.entries().size() == 5);
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

        const auto path =
          env.dir() / "base" / "maps" / "prefabs" / "lights" / "torch.map";
        env.createFile(
          "base/maps/prefabs/lights/torch.map",
          kdl::str_replace_every(Torch, "Torch (one)", "Torch (first)"));
        library.updateEntry(path);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!entered && std::chrono::steady_clock::now() < deadline)
        {
          std::this_thread::sleep_for(1ms);
        }
        REQUIRE(entered);

        env.createFile(
          "base/maps/prefabs/lights/torch.map",
          kdl::str_replace_every(Torch, "Torch (one)", "Torch (second, longer)"));
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
        CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
        CHECK(torch->fileSize == secondSize);
        CHECK(torch->metadata.name == "Torch (second, longer)");
        CHECK(notifications == 2);
      }
    }
  }

  SECTION("An abandoned scan reads no more files")
  {
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

    // four files to read, two of them block the two workers
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
    library.updateEntry(env.dir() / "base" / "maps" / "prefabs" / "autosave" / "x.1.map");
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
        "base/maps/prefabs/big.map", loadBytes(big->absolutePath) + "// changed\n");
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

  SECTION("The mod root follows the game's search path")
  {
    // R8-lib-3: without an enabled mod, the mod folder is the game's own, not "base"
    auto otherFixture = MapFixture{};
    auto& otherMap = otherFixture.create(MapFixtureConfig{
      .mapFormat = MapFormat::Valve,
      .gameInfo = detail::makeGameInfoFixture(QuakeGameInfo.gameConfig, env.dir()),
    });
    const auto& otherLibrary = otherMap.prefabLibrary();
    CHECK(otherLibrary.modFolder() == env.dir() / "id1");
    CHECK(
      otherLibrary.writeRootFor(PrefabRoot::Mod)
      == env.dir() / "id1" / "maps" / "prefabs");
  }

  SECTION("Placements resolve in the active root (DD-37)")
  {
    // two unlinked placements, one recorded under each qualifier
    const auto addPlacement = [&](const std::string& prefabPath) {
      auto group = Group{"placed"};
      group.setPrefabPath(prefabPath);
      auto* groupNode = new GroupNode{std::move(group)};
      groupNode->addChild(createBrushNode(map));
      addNodes(map, {{&parentForNodes(map), {groupNode}}});
      return groupNode;
    };
    auto* modPlacedNode = addPlacement("mod/lights/torch.map");
    auto* userPlacedNode = addPlacement("user/lights/torch.map");
    REQUIRE(modPlacedNode->linkId() != userPlacedNode->linkId());

    const auto checkPlacements = [&](const PrefabEntry* torch) {
      REQUIRE(torch);
      const auto placements = library.placements();
      REQUIRE(placements.size() == 2u);
      CHECK(placements[0].firstMember == modPlacedNode);
      CHECK(placements[0].prefabPath == "mod/lights/torch.map");
      CHECK(placements[0].entry == torch);
      CHECK(placements[1].firstMember == userPlacedNode);
      CHECK(placements[1].prefabPath == "user/lights/torch.map");
      CHECK(placements[1].entry == torch);
    };

    SECTION("With the Prefab Path set")
    {
      library.setUserRoot(env.dir() / "user");
      library.rescan();
      waitForScan(library);
      checkPlacements(library.find(PrefabRoot::User, "lights/torch.map"));
    }

    SECTION("Without the Prefab Path")
    {
      library.rescan();
      waitForScan(library);
      checkPlacements(library.find(PrefabRoot::Mod, "lights/torch.map"));
    }
  }

  SECTION("setUserRoot normalises the path")
  {
    const auto userRoot = (env.dir() / "user").lexically_normal();

    // R8-ui-4: a trailing separator names the same root
    library.setUserRoot(env.dir() / "user" / "");
    CHECK(library.userRoot() == userRoot);
    CHECK(library.userRoot().has_filename());
    library.setUserRoot(env.dir() / "user" / "." / "lights" / "..");
    CHECK(library.userRoot() == userRoot);
    CHECK(library.userRoot().has_filename());

    // R8-lib-4: a relative path would resolve against the working folder, so it is unset
    library.setUserRoot("prefabs");
    CHECK(library.userRoot().empty());
    CHECK(library.writeRoot() == PrefabRoot::Mod);
    library.setUserRoot({});
    CHECK(library.userRoot().empty());

    CHECK(PrefabLibrary::normalizeUserRoot("relative/prefabs").empty());
    CHECK(PrefabLibrary::normalizeUserRoot(env.dir() / "user" / "") == userRoot);
    const auto fileSystemRoot = env.dir().root_path();
    CHECK(PrefabLibrary::normalizeUserRoot(fileSystemRoot) == fileSystemRoot);

    // the scan finds the files of the root
    library.setUserRoot(env.dir() / "user" / "");
    library.rescan();
    waitForScan(library);
    CHECK(library.find(PrefabRoot::User, "lights/torch.map") != nullptr);
    library.updateEntry(env.dir() / "user" / "lights" / "torch.map");
    CHECK(library.scanning());
    waitForScan(library);
  }

  SECTION("A file that is not a prefab is not read again (R8-lib-5)")
  {
    auto notAPrefabReads = std::atomic<int>{0};
    auto brokenReads = std::atomic<int>{0};
    library.setReadHookForTesting([&](const auto& path) {
      if (path.filename() == "notaprefab.map")
      {
        ++notAPrefabReads;
      }
      else if (path.filename() == "broken.map")
      {
        ++brokenReads;
      }
    });

    library.rescan();
    waitForScan(library);
    const auto* notAPrefab = library.find(PrefabRoot::Mod, "notaprefab.map");
    REQUIRE(notAPrefab);
    CHECK(notAPrefab->error);
    CHECK(notAPrefab->notAPrefab);
    const auto* broken = library.find(PrefabRoot::Mod, "broken.map");
    REQUIRE(broken);
    CHECK(broken->error);
    CHECK_FALSE(broken->notAPrefab);
    CHECK(notAPrefabReads == 1);
    CHECK(brokenReads == 1);

    // the parse error may be transient (DD-22), the missing kind is not
    library.rescan();
    waitForScan(library);
    CHECK(notAPrefabReads == 1);
    CHECK(brokenReads == 2);
    CHECK(library.find(PrefabRoot::Mod, "notaprefab.map") == notAPrefab);

    // a changed file is read again
    env.createFile("base/maps/prefabs/notaprefab.map", NotAPrefab + "// changed\n");
    library.rescan();
    waitForScan(library);
    CHECK(notAPrefabReads == 2);
  }

  SECTION("A listing that ends early keeps what it did not reach (R8-lib-2)")
  {
    library.rescan();
    waitForScan(library);
    REQUIRE(library.entries().size() == 4u);
    const auto* torch = library.find(PrefabRoot::Mod, "lights/torch.map");
    const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(torch);
    REQUIRE(crate);
    const auto directories = library.directories();
    REQUIRE(directories.size() == 3u);

    auto notifications = 0;
    auto connection =
      library.libraryDidChangeNotifier.connect([&]() { ++notifications; });

    // a file is removed, and the walk ends at its first entry, as an iterator error
    // would end it; a test seam, since an unreadable folder is platform-specific
    REQUIRE(env.remove("base/maps/prefabs/notaprefab.map"));
    library.setListHookForTesting(
      [](const auto&) { return std::make_error_code(std::errc::io_error); });
    library.rescan();
    waitForScan(library);

    // the entries that the walk did not see are kept, the removed one among them
    CHECK(library.entries().size() == 4u);
    CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
    CHECK(library.find(PrefabRoot::Mod, "props/crate.map") == crate);
    CHECK(library.find(PrefabRoot::Mod, "notaprefab.map") != nullptr);
    CHECK(library.directories() == directories);
    CHECK(notifications == 0);

    // a complete walk removes it
    library.setListHookForTesting({});
    library.rescan();
    waitForScan(library);
    CHECK(library.entries().size() == 3u);
    CHECK(library.find(PrefabRoot::Mod, "notaprefab.map") == nullptr);
    CHECK(library.find(PrefabRoot::Mod, "lights/torch.map") == torch);
    CHECK(notifications == 1);
  }

  SECTION("An abandoned listing stops at once (R7-lib-1)")
  {
    // the state is shared with the workers, which may outlive this section if a check
    // fails
    struct HookState
    {
      std::atomic<int> calls = 0;
      std::atomic<bool> block = false;
      std::atomic<bool> entered = false;
      std::atomic<bool> release = false;
    };
    const auto state = std::make_shared<HookState>();
    library.setListHookForTesting([state](const auto&) {
      if (state->calls++ == 0 && state->block)
      {
        state->entered = true;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!state->release && std::chrono::steady_clock::now() < deadline)
        {
          std::this_thread::sleep_for(1ms);
        }
      }
      return std::error_code{};
    });
    const auto releaseWalk = kdl::invoke_later{[state]() { state->release = true; }};

    // the number of entries that one walk of the root reaches
    library.rescan();
    waitForScan(library);
    const auto walkLength = state->calls.load();
    REQUIRE(walkLength > 1);

    // the first walk blocks at its first entry, and is abandoned there
    state->calls = 0;
    state->block = true;
    library.rescan();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!state->entered && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(state->entered);

    library.rescan();
    state->release = true;
    waitForScan(library);

    // the abandoned walk reached no other entry; the second one reached them all
    CHECK(state->calls == 1 + walkLength);
    CHECK(library.entries().size() == 4u);
  }

  SECTION("A large entity snapshot keeps its entity (R7-lib-2)")
  {
    auto* bigEntityNode = new EntityNode{
      Entity{{{"classname", "light"}, {"name", "slab"}, {"_color", "1 0 0"}}}};
    for (size_t i = 0; i < PrefabWorldBrushBudget + 1; ++i)
    {
      bigEntityNode->addChild(createBrushNode(map));
    }
    addNodes(map, {{&parentForNodes(map), {bigEntityNode}}});
    REQUIRE(
      savePrefab(
        map, {bigEntityNode}, SavePrefabOptions{.name = "slab", .root = PrefabRoot::Mod})
      | kdl::is_success());

    library.rescan();
    waitForScan(library);

    const auto* big = library.find(PrefabRoot::Mod, "slab.map");
    REQUIRE(big);
    CHECK(!big->error);
    CHECK(big->metadata.kind == PrefabKind::Entity);
    CHECK(big->world == nullptr);
    CHECK(big->bodyPreview);
    REQUIRE(big->keptSnapshot);
    REQUIRE(big->snapshot() == &*big->keptSnapshot);
    CHECK(big->snapshot()->classname() == "light");
    CHECK(big->snapshot()->property("_color") != nullptr);
    CHECK(
      big->snapshot()->definition() == map.entityDefinitionManager().definition("light"));

    // a small entity snapshot is read from its world, an assembly has none
    const auto* torch = library.find(PrefabRoot::Mod, "lights/torch.map");
    REQUIRE(torch);
    REQUIRE(torch->world);
    CHECK(!torch->keptSnapshot);
    CHECK(torch->snapshot() == &firstEntityNode(*torch->world)->entity());
    const auto* crate = library.find(PrefabRoot::Mod, "props/crate.map");
    REQUIRE(crate);
    CHECK(crate->snapshot() == nullptr);

    SECTION("Its definition follows the map's")
    {
      map.entityDefinitionsWillChangeNotifier();
      CHECK(big->snapshot()->definition() == nullptr);
      map.entityDefinitionsDidChangeNotifier();
      REQUIRE(map.entityDefinitionManager().definition("light"));
      CHECK(
        big->snapshot()->definition()
        == map.entityDefinitionManager().definition("light"));
    }

    SECTION("cloneBody reads the file again")
    {
      auto clones = library.cloneBody(*big, map.worldBounds());
      REQUIRE(clones.size() == 1);
      const auto* entityNode = dynamic_cast<const EntityNode*>(clones.front());
      REQUIRE(entityNode);
      CHECK(entityNode->childCount() == PrefabWorldBrushBudget + 1);
      kdl::vec_clear_and_delete(clones);
    }
  }

  SECTION("Destroying the map while files are read (R7-lib-4)")
  {
    // the state is shared with the workers, which outlive the map until they are joined
    struct HookState
    {
      std::atomic<int> calls = 0;
      std::atomic<int> blocked = 0;
      std::atomic<bool> release = false;
    };
    const auto state = std::make_shared<HookState>();
    auto releaser = std::thread{};
    {
      auto otherFixture = MapFixture{};
      auto& otherMap = otherFixture.create(MapFixtureConfig{
        .mapFormat = MapFormat::Doom3_Valve,
        .gameInfo = detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, env.dir()),
      });
      auto& otherLibrary = otherMap.prefabLibrary();

      // the first file is read at once, so that its future is ready and not taken over;
      // the next two block both workers
      otherLibrary.setReadHookForTesting([state](const auto&) {
        if (state->calls++ == 0)
        {
          return;
        }
        ++state->blocked;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!state->release && std::chrono::steady_clock::now() < deadline)
        {
          std::this_thread::sleep_for(1ms);
        }
      });

      otherLibrary.rescan();
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (state->blocked < 2 && std::chrono::steady_clock::now() < deadline)
      {
        otherLibrary.processPendingResults();
        std::this_thread::sleep_for(1ms);
      }
      REQUIRE(state->blocked == 2);
      CHECK(otherLibrary.scanning());

      // released while the map is destroyed, which joins the workers
      releaser = std::thread{[state]() {
        std::this_thread::sleep_for(50ms);
        state->release = true;
      }};
    }
    releaser.join();

    // the fourth file, queued when the map went, was abandoned without being read
    CHECK(state->calls == 3);
  }
}

} // namespace tb::mdl
