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
#include "gl/OrthographicCamera.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EditorContext.h"
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
#include "mdl/Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/CreateEntityTool.h"
#include "ui/CreateEntityToolController.h"
#include "ui/CreatePrefabTool.h"
#include "ui/CreatePrefabToolController.h"
#include "ui/DropTracker.h"
#include "ui/InputState.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/PickRequest.h"
#include "ui/ToolChain.h"
#include "ui/ToolController.h"

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

// an assembly whose body is an entity, not a group
const auto Malformed = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "assembly"
"_tb_prefab_name" "Malformed"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
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

// the first entity in the group, nested groups included
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
    e.createDirectory("base/maps/prefabs/lights");
    e.createFile("base/maps/prefabs/lights/torch.map", Torch);
    e.createFile("base/maps/prefabs/broken.map", "{ this is not a map");
    e.createFile("base/maps/prefabs/malformed.map", Malformed);
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

    // a file that does not parse, and a body that is not what the kind says
    const auto* brokenEntry = library.find(mdl::PrefabRoot::Mod, "broken.map");
    REQUIRE(brokenEntry != nullptr);
    REQUIRE(brokenEntry->error);
    CHECK_FALSE(tool.createPrefab(library.payloadFor(*brokenEntry)));
    CHECK_FALSE(tool.placing());

    const auto* malformedEntry = library.find(mdl::PrefabRoot::Mod, "malformed.map");
    REQUIRE(malformedEntry != nullptr);
    REQUIRE(malformedEntry->world);
    REQUIRE(malformedEntry->metadata.kind == mdl::PrefabKind::Assembly);
    CHECK_FALSE(tool.createPrefab(library.payloadFor(*malformedEntry)));
    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);

    // no transaction is open, so a new one can be committed on its own
    REQUIRE(tool.createPrefab(torchPayload));
    tool.commit();
    map.undoCommand();
    CHECK(defaultLayerChildren == childrenBefore);
  }

  SECTION("A tool with placement off places nothing")
  {
    // DD-29: a prefab editor's tool
    REQUIRE(tool.placementEnabled());
    const auto* undoNameBefore = map.undoCommandName();
    tool.setPlacementEnabled(false);
    CHECK_FALSE(tool.placementEnabled());

    // the controllers refuse the drag, so the view shows the no-drop cursor
    const auto inputState = InputState{0.0f, 0.0f};
    auto controller2D = CreatePrefabToolController2D{tool};
    auto controller3D = CreatePrefabToolController3D{tool};
    const auto acceptsDrop = [&](const ToolController& controller, const auto& payload) {
      return controller.shouldAcceptDrop(inputState, payload);
    };
    CHECK_FALSE(acceptsDrop(controller2D, torchPayload));
    CHECK_FALSE(acceptsDrop(controller3D, cratePayload));

    CHECK_FALSE(tool.createPrefab(torchPayload));
    CHECK_FALSE(tool.createPrefab(cratePayload));
    CHECK_FALSE(tool.placing());
    CHECK_FALSE(map.isTransactionActive());
    CHECK(map.undoCommandName() == undoNameBefore);
    CHECK(defaultLayerChildren == childrenBefore);

    tool.setPlacementEnabled(true);
    CHECK(acceptsDrop(controller2D, torchPayload));
    REQUIRE(tool.createPrefab(torchPayload));
    tool.cancel();
    CHECK(defaultLayerChildren == childrenBefore);
  }

  SECTION("A view's drop chain passes a prefab payload by the entity controller")
  {
    // RL-29 (checklist CL-7): the views chain the entity controller before the prefab
    // controller, and the chain offers a drag to each active controller in turn
    auto entityTool = CreateEntityTool{document};
    auto chain = ToolChain{};
    chain.append(std::make_unique<CreateEntityToolController2D>(entityTool));
    chain.append(std::make_unique<CreatePrefabToolController2D>(tool));

    const auto pickRay = vm::ray3d{{0, 0, 512}, {0, 0, -1}};
    const auto camera = gl::OrthographicCamera{
      1.0f,
      8000.0f,
      gl::Camera::Viewport{0, 0, 1920, 1080},
      vm::vec3f{pickRay.origin},
      vm::vec3f{pickRay.direction},
      vm::vec3f{0, 1, 0}};
    auto inputState = InputState{0.0f, 0.0f};
    inputState.setPickRequest(PickRequest{pickRay, camera});

    REQUIRE(chain.shouldAcceptDrop(inputState, torchPayload));
    auto dropTracker = chain.dragEnter(inputState, torchPayload);
    REQUIRE(dropTracker != nullptr);
    CHECK(tool.placing());
    dropTracker->leave(inputState);
    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);

    // each controller hands the other's payload back
    auto entityController2D = CreateEntityToolController2D{entityTool};
    auto prefabController2D = CreatePrefabToolController2D{tool};
    ToolController& entityController = entityController2D;
    ToolController& prefabController = prefabController2D;
    CHECK(entityController.acceptDrop(inputState, torchPayload) == nullptr);
    CHECK(prefabController.acceptDrop(inputState, "entity:light") == nullptr);
    CHECK_FALSE(map.isTransactionActive());
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
    // C3: the path names the root too
    CHECK(groupNode->group().prefabPath() == "mod/props/crate.map");
    CHECK(groupNode->group().prefabHash() == crateEntry->hash);

    // dropped into the map it was saved from, it joins none of the map's link sets
    for (const auto& linkId : collectLinkIds({groupNode}))
    {
      CHECK_FALSE(kdl::ranges::contains(linkIdsBefore, linkId));
    }

    // the saved group is the placed group, not nested in another one
    CHECK(groupNode->group().name() == "crate");
    CHECK(groupNode->childCount() == 2u);

    // its light got a name that is free in the map
    const auto* lightNode = findLight(*groupNode);
    REQUIRE(lightNode != nullptr);
    CHECK(lightNode->parent() == groupNode);
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
      CHECK(secondGroupNode->group().prefabPath() == "mod/props/crate.map");
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

    SECTION("A placement inside the open first placement is not linked to it")
    {
      // a linked duplicate cannot be added to its own link set (H-D-4)
      mdl::openGroup(map, *groupNode);
      REQUIRE(tool.createPrefab(cratePayload));

      REQUIRE(map.selection().nodes.size() == 1u);
      auto* innerGroupNode = dynamic_cast<mdl::GroupNode*>(map.selection().nodes.front());
      REQUIRE(innerGroupNode != nullptr);
      CHECK(innerGroupNode->parent() == groupNode);
      CHECK(innerGroupNode->linkId() != groupNode->linkId());
      CHECK(innerGroupNode->group().prefabPath() == "mod/props/crate.map");
      CHECK_FALSE(kdl::ranges::contains(linkIdsBefore, innerGroupNode->linkId()));

      tool.commit();
      mdl::closeGroup(map);
      CHECK(defaultLayerChildren.size() == childrenBefore.size() + 1u);
    }

    SECTION("A placement made under the other root is found by its relative path")
    {
      // DD-37: the user root replaces the mod root in the library (DD-32); the placement
      // recorded as mod/props/crate.map names the user root's props/crate.map now, so a
      // second drop is a linked duplicate of it and not a second link set
      library.setUserRoot(env.dir() / "user");
      REQUIRE(
        mdl::savePrefab(
          map,
          {crateNode},
          mdl::SavePrefabOptions{
            .name = "crate", .category = "props", .root = mdl::PrefabRoot::User})
        | kdl::is_success());
      library.rescan();
      waitForScan(library);

      REQUIRE(library.find(mdl::PrefabRoot::Mod, "props/crate.map") == nullptr);
      const auto* userCrateEntry = library.find(mdl::PrefabRoot::User, "props/crate.map");
      REQUIRE(userCrateEntry != nullptr);
      REQUIRE(library.entryForPrefabPath("mod/props/crate.map") == userCrateEntry);

      REQUIRE(tool.createPrefab(library.payloadFor(*userCrateEntry)));
      auto* userGroupNode = dynamic_cast<mdl::GroupNode*>(map.selection().nodes.front());
      REQUIRE(userGroupNode != nullptr);
      CHECK(userGroupNode != groupNode);
      CHECK(userGroupNode->linkId() == groupNode->linkId());
      // a linked duplicate keeps the path of the group that it copies
      CHECK(userGroupNode->group().prefabPath() == "mod/props/crate.map");
      tool.commit();
    }
  }

  SECTION("A placement that the view filter hides is not made")
  {
    map.editorContext().setShowPointEntities(false);
    CHECK_FALSE(tool.createPrefab(torchPayload));
    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);
    map.editorContext().setShowPointEntities(true);
  }

  SECTION("Cancelling a placement leaves the map unchanged")
  {
    // the selection is restored too
    mdl::selectNodes(map, {torchNode});

    SECTION("Entity snapshot")
    {
      REQUIRE(tool.createPrefab(torchPayload));
      tool.updateCreatedPrefab({32, 32, 0});
      tool.cancel();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{torchNode});
    }

    SECTION("Assembly")
    {
      REQUIRE(tool.createPrefab(cratePayload));
      tool.cancel();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{torchNode});
    }

    SECTION("Linked duplicate of an assembly")
    {
      REQUIRE(tool.createPrefab(cratePayload));
      tool.commit();
      const auto childrenAfterFirst = defaultLayerChildren;
      const auto selectionAfterFirst = map.selection().nodes;

      REQUIRE(tool.createPrefab(cratePayload));
      tool.cancel();
      CHECK(defaultLayerChildren == childrenAfterFirst);
      CHECK(map.selection().nodes == selectionAfterFirst);

      map.undoCommand();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{torchNode});
    }

    CHECK_FALSE(tool.placing());
    CHECK(defaultLayerChildren == childrenBefore);
    CHECK(collectLinkIds({&map.worldNode()}) == linkIdsBefore);
  }
}

} // namespace tb::ui
