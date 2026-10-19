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
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/CreatePrefabTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include "kd/overload.h"
#include "kd/ranges/contains.h"

#include "vm/bbox.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
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
"_tb_prefab_name" "Torch"
"_tb_prefab_class" "light"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
"name" "torch"
"light_radius" "128"
}
)PREFAB"};

void waitForScan(mdl::PrefabLibrary& library)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (library.scanning() && std::chrono::steady_clock::now() < deadline)
  {
    library.processPendingResults();
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(library.scanning());
}

std::vector<std::string> collectLinkIds(const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<std::string>{};
  mdl::Node::visitAll(
    nodes,
    kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        result.push_back(groupNode.linkId());
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        result.push_back(entityNode.linkId());
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) { result.push_back(brushNode.linkId()); },
      [&](mdl::PatchNode& patchNode) { result.push_back(patchNode.linkId()); }));
  return result;
}

std::string nameOf(const mdl::EntityNode& entityNode)
{
  const auto* name = entityNode.entity().property("name");
  return name ? *name : std::string{};
}

// the first entity in the group, nested groups included (savePrefab puts a saved group
// inside the prefab's own group)
const mdl::EntityNode* findLight(const mdl::GroupNode& groupNode)
{
  for (const auto* child : groupNode.children())
  {
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(child))
    {
      return entityNode;
    }
    if (const auto* nestedGroupNode = dynamic_cast<const mdl::GroupNode*>(child))
    {
      if (const auto* entityNode = findLight(*nestedGroupNode))
      {
        return entityNode;
      }
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("CreatePrefabTool")
{
  auto env = fs::TestEnvironment{[](auto& e) {
    e.createDirectory("base/prefabs/lights");
    e.createFile("base/prefabs/lights/torch.map", Torch);
  }};

  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(mdl::MapFixtureConfig{
    .mapFormat = mdl::MapFormat::Doom3_Valve,
    .gameInfo =
      mdl::detail::makeGameInfoFixture(mdl::Doom3BFGGameInfo.gameConfig, env.dir()),
  });
  auto& map = document.map();
  REQUIRE(map.worldNode().entityPropertyConfig().naming);

  // the Doom 3 BFG fixture loads an FGD; let the map release those definitions first
  map.entityDefinitionsWillChangeNotifier();
  map.entityDefinitionManager().setDefinitions({
    {"light",
     Color{},
     "",
     {mdl::PropertyDefinition{"name", mdl::PropertyValueTypes::LinkTarget{}, "", ""}},
     mdl::PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
  });

  // the source map: a light named "torch", and a linked pair of crates, one of them saved
  // as an assembly
  auto* torchNode = new mdl::EntityNode{
    mdl::Entity{{{"classname", "light"}, {"origin", "256 256 0"}, {"name", "torch"}}}};
  auto* lampNode = new mdl::EntityNode{
    mdl::Entity{{{"classname", "light"}, {"origin", "0 0 0"}, {"name", "lamp"}}}};
  auto* brushNode = mdl::createBrushNode(map);
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {torchNode, lampNode, brushNode}}});

  mdl::selectNodes(map, {lampNode, brushNode});
  auto* crateNode = mdl::groupSelectedNodes(map, "crate");
  REQUIRE(crateNode != nullptr);
  auto* crateDuplicateNode = mdl::createLinkedDuplicate(map);
  REQUIRE(crateDuplicateNode != nullptr);
  mdl::deselectAll(map);

  REQUIRE(
    mdl::savePrefab(
      map,
      {crateNode},
      mdl::SavePrefabOptions{
        .name = "crate", .category = "props", .root = mdl::PrefabRoot::Mod})
    | kdl::is_success());

  auto& library = map.prefabLibrary();
  library.rescan();
  waitForScan(library);

  const auto* crateEntry = library.find(mdl::PrefabRoot::Mod, "props/crate.map");
  REQUIRE(crateEntry != nullptr);
  REQUIRE(crateEntry->metadata.kind == mdl::PrefabKind::Assembly);
  const auto* torchEntry = library.find(mdl::PrefabRoot::Mod, "lights/torch.map");
  REQUIRE(torchEntry != nullptr);
  REQUIRE(torchEntry->metadata.kind == mdl::PrefabKind::Entity);

  const auto cratePayload = library.payloadFor(*crateEntry);
  const auto torchPayload = library.payloadFor(*torchEntry);

  auto& defaultLayerChildren = map.worldNode().defaultLayer()->children();
  const auto childrenBefore = defaultLayerChildren;
  const auto linkIdsBefore = collectLinkIds({&map.worldNode()});

  auto tool = CreatePrefabTool{document};

  SECTION("A payload that names no prefab opens no transaction")
  {
    CHECK_FALSE(tool.createPrefab("prefab:mod/missing.map"));
    CHECK_FALSE(tool.createPrefab("entity:light"));
    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);

    // no transaction is open, so a new one can be committed on its own
    REQUIRE(tool.createPrefab(torchPayload));
    tool.commit();
    map.undoCommand();
    CHECK(defaultLayerChildren == childrenBefore);
  }

  SECTION("An entity snapshot is placed as one plain entity with a unique name")
  {
    REQUIRE(tool.createPrefab(torchPayload));
    CHECK(tool.placing());

    REQUIRE(map.selection().nodes.size() == 1u);
    const auto* entityNode =
      dynamic_cast<const mdl::EntityNode*>(map.selection().nodes.front());
    REQUIRE(entityNode != nullptr);
    CHECK(entityNode->parent() == map.worldNode().defaultLayer());
    CHECK(entityNode->entity().classname() == "light");
    CHECK(*entityNode->entity().property("light_radius") == "128");
    CHECK(nameOf(*entityNode) != "torch");
    CHECK(entityNode->entity().definition() != nullptr);

    tool.updateCreatedPrefab({64, 0, 0});
    CHECK(entityNode->entity().origin() == vm::vec3d{64, 0, 0});

    tool.commit();
    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren.size() == childrenBefore.size() + 1u);

    SECTION("Two placements are unrelated entities")
    {
      REQUIRE(tool.createPrefab(torchPayload));
      const auto* secondEntityNode =
        dynamic_cast<const mdl::EntityNode*>(map.selection().nodes.front());
      REQUIRE(secondEntityNode != nullptr);
      CHECK(secondEntityNode != entityNode);
      CHECK(nameOf(*secondEntityNode) != nameOf(*entityNode));
      CHECK(nameOf(*secondEntityNode) != "torch");
      tool.commit();
    }

    SECTION("Undo removes the placement in one step")
    {
      map.undoCommand();
      CHECK(defaultLayerChildren == childrenBefore);
    }
  }

  SECTION("An assembly is placed as a group that records its file")
  {
    REQUIRE(tool.createPrefab(cratePayload));

    REQUIRE(map.selection().nodes.size() == 1u);
    auto* groupNode = dynamic_cast<mdl::GroupNode*>(map.selection().nodes.front());
    REQUIRE(groupNode != nullptr);
    CHECK(groupNode->group().prefabPath() == "props/crate.map");
    CHECK(groupNode->group().prefabHash() == crateEntry->hash);

    // dropped into the map it was saved from, it joins none of the map's link sets
    for (const auto& linkId : collectLinkIds({groupNode}))
    {
      CHECK_FALSE(kdl::ranges::contains(linkIdsBefore, linkId));
    }

    // its light got a name that is free in the map
    const auto* lightNode = findLight(*groupNode);
    REQUIRE(lightNode != nullptr);
    CHECK(nameOf(*lightNode) != "lamp");
    CHECK(nameOf(*lightNode) != nameOf(*findLight(*crateDuplicateNode)));

    tool.commit();

    SECTION("A second placement is a linked duplicate of the first")
    {
      REQUIRE(tool.createPrefab(cratePayload));

      REQUIRE(map.selection().nodes.size() == 1u);
      auto* secondGroupNode =
        dynamic_cast<mdl::GroupNode*>(map.selection().nodes.front());
      REQUIRE(secondGroupNode != nullptr);
      CHECK(secondGroupNode != groupNode);
      CHECK(secondGroupNode->linkId() == groupNode->linkId());
      CHECK(secondGroupNode->group().prefabPath() == "props/crate.map");
      CHECK(secondGroupNode->group().prefabHash() == crateEntry->hash);
      CHECK_FALSE(kdl::ranges::contains(linkIdsBefore, secondGroupNode->linkId()));

      const auto* secondLightNode = findLight(*secondGroupNode);
      REQUIRE(secondLightNode != nullptr);
      CHECK(nameOf(*secondLightNode) != nameOf(*lightNode));

      tool.updateCreatedPrefab({0, 128, 0});
      tool.commit();

      CHECK(
        secondGroupNode->logicalBounds()
        == groupNode->logicalBounds().translate(vm::vec3d{0, 128, 0}));
      CHECK(defaultLayerChildren.size() == childrenBefore.size() + 2u);
    }
  }

  SECTION("Cancelling a placement leaves the map unchanged")
  {
    SECTION("Entity snapshot")
    {
      REQUIRE(tool.createPrefab(torchPayload));
      tool.updateCreatedPrefab({32, 32, 0});
      tool.cancel();
    }

    SECTION("Assembly")
    {
      REQUIRE(tool.createPrefab(cratePayload));
      tool.cancel();
    }

    SECTION("Linked duplicate of an assembly")
    {
      REQUIRE(tool.createPrefab(cratePayload));
      tool.commit();
      const auto childrenAfterFirst = defaultLayerChildren;

      REQUIRE(tool.createPrefab(cratePayload));
      tool.cancel();
      CHECK(defaultLayerChildren == childrenAfterFirst);

      map.undoCommand();
    }

    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);
    CHECK(collectLinkIds({&map.worldNode()}) == linkIdsBefore);
  }
}

} // namespace tb::ui
