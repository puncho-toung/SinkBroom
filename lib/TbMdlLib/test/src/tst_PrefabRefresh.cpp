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
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PrefabRefresh.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/contains.h"
#include "kd/result.h"

#include "vm/mat_ext.h"

#include <fmt/format.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

using namespace std::chrono_literals;

const auto LampPath = std::string{"mod/props/lamp.map"};

const auto RedLight = std::string{R"PREFAB(// Game: Doom 3 BFG
// Format: Doom3 (Valve)
// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
"_tb_prefab_name" "Red light"
"_tb_prefab_class" "light"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
"name" "red"
"light_radius" "300"
"_color" "1 0 0"
}
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

void rescan(PrefabLibrary& library)
{
  library.rescan();
  waitForScan(library);
}

BrushNode* createBrushNodeAt(const Map& map, const vm::vec3d& delta)
{
  return createBrushNode(map, "material", [&](Brush& brush) {
    REQUIRE(
      brush.transform(map.worldBounds(), vm::translation_matrix(delta), false)
      | kdl::is_success());
  });
}

/**
 * Writes base/prefabs/props/lamp.map from a brush at the given offset and a light named
 * "lamp" at 0 0 64, in this order, with an info_null between them if withMarker is set,
 * and takes the nodes out of the map again. In the file, the group's brushes come before
 * its entities whatever the order of the selection.
 */
void saveLamp(Map& map, const vm::vec3d& brushOffset, const bool withMarker = false)
{
  auto nodes = std::vector<Node*>{createBrushNodeAt(map, brushOffset)};
  if (withMarker)
  {
    nodes.push_back(new EntityNode{
      Entity{{{"classname", "info_null"}, {"origin", "0 0 0"}, {"name", "marker"}}}});
  }
  nodes.push_back(new EntityNode{
    Entity{{{"classname", "light"}, {"origin", "0 0 64"}, {"name", "lamp"}}}});

  addNodes(map, {{&parentForNodes(map), nodes}});
  REQUIRE(
    savePrefab(
      map,
      nodes,
      SavePrefabOptions{.name = "lamp", .category = "props", .root = PrefabRoot::Mod})
    | kdl::is_success());
  removeNodes(map, nodes);
}

const PrefabEntry& findLamp(const PrefabLibrary& library)
{
  // a scan that reads a changed file replaces its entry
  const auto* entry = library.find(PrefabRoot::Mod, "props/lamp.map");
  REQUIRE(entry);
  REQUIRE(entry->world);
  return *entry;
}

/**
 * The prefab group of an entry whose world the library keeps.
 */
const GroupNode& fileGroupOf(const PrefabEntry& entry)
{
  const auto* groupNode =
    dynamic_cast<const GroupNode*>(entry.world->defaultLayer()->children().front());
  REQUIRE(groupNode);
  return *groupNode;
}

/**
 * Places the prefab as the drop tool places a first placement, at the given offset.
 */
GroupNode* place(Map& map, const PrefabEntry& entry, const vm::vec3d& offset)
{
  auto clones = map.prefabLibrary().cloneBody(entry, map.worldBounds());
  REQUIRE(clones.size() == 1u);
  auto* groupNode = dynamic_cast<GroupNode*>(clones.front());
  REQUIRE(groupNode);

  auto group = groupNode->group();
  group.setPrefabPath(LampPath);
  group.setPrefabHash(entry.hash);
  groupNode->setGroup(std::move(group));

  assignUniqueEntityNamesBeforeAdding(map, clones, true);
  addNodes(map, {{&parentForNodes(map), clones}});

  deselectAll(map);
  selectNodes(map, {groupNode});
  REQUIRE(translateSelection(map, offset));
  deselectAll(map);
  return groupNode;
}

GroupNode* placeLinkedCopy(Map& map, GroupNode& groupNode, const vm::vec3d& offset)
{
  deselectAll(map);
  selectNodes(map, {&groupNode});
  auto* copyNode = createLinkedDuplicate(map);
  REQUIRE(copyNode);
  deselectAll(map);
  selectNodes(map, {copyNode});
  REQUIRE(translateSelection(map, offset));
  deselectAll(map);
  return copyNode;
}

EntityNode* findEntity(const GroupNode& groupNode)
{
  for (auto* child : groupNode.children())
  {
    if (auto* entityNode = dynamic_cast<EntityNode*>(child))
    {
      return entityNode;
    }
  }
  return nullptr;
}

BrushNode* findBrush(const GroupNode& groupNode)
{
  for (auto* child : groupNode.children())
  {
    if (auto* brushNode = dynamic_cast<BrushNode*>(child))
    {
      return brushNode;
    }
  }
  return nullptr;
}

std::optional<std::string> propertyOf(
  const EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  return value ? std::optional{*value} : std::nullopt;
}

} // namespace

TEST_CASE("PrefabRefresh")
{
  auto env = fs::TestEnvironment{[](auto& e) {
    e.createDirectory("base/prefabs/lights");
    e.createDirectory("base/prefabs/props");
  }};

  auto fixture = MapFixture{};
  auto& map = fixture.create(MapFixtureConfig{
    .mapFormat = MapFormat::Doom3_Valve,
    .gameInfo = detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, env.dir()),
  });
  REQUIRE(map.worldNode().entityPropertyConfig().naming);

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

  SECTION("refreshPlacement")
  {
    saveLamp(map, {0, 0, 0});
    rescan(library);
    const auto oldHash = findLamp(library).hash;
    // file coordinates; the placements keep the file's origin where it is
    const auto oldFileBrushBounds =
      findBrush(fileGroupOf(findLamp(library)))->logicalBounds();
    const auto oldFileLightOrigin =
      findEntity(fileGroupOf(findLamp(library)))->entity().origin();

    // two linked placements; the second protects its own light colour
    auto* firstNode = place(map, findLamp(library), {256, 0, 0});
    auto* secondNode = placeLinkedCopy(map, *firstNode, {0, 512, 0});
    REQUIRE(firstNode->linkId() == secondNode->linkId());

    selectNodes(map, {findEntity(*secondNode)});
    REQUIRE(setProtectedEntityProperty(map, "_color", true));
    REQUIRE(setEntityProperty(map, "_color", "0 1 0"));
    deselectAll(map);

    const auto firstLightName = propertyOf(*findEntity(*firstNode), "name");
    const auto secondLightName = propertyOf(*findEntity(*secondNode), "name");
    REQUIRE(firstLightName == "lamp");
    REQUIRE(secondLightName);
    REQUIRE(secondLightName != firstLightName);
    REQUIRE(propertyOf(*findEntity(*firstNode), "_color") == std::nullopt);
    REQUIRE(propertyOf(*findEntity(*secondNode), "_color") == "0 1 0");

    const auto firstBrushBounds = findBrush(*firstNode)->logicalBounds();
    const auto secondBrushBounds = findBrush(*secondNode)->logicalBounds();
    const auto firstLightOrigin = findEntity(*firstNode)->entity().origin();
    const auto secondLightOrigin = findEntity(*secondNode)->entity().origin();

    SECTION("placements")
    {
      const auto placements = library.placements();
      REQUIRE(placements.size() == 1u);
      CHECK(placements.front().prefabPath == LampPath);
      CHECK(placements.front().firstMember == firstNode);
      CHECK(placements.front().members == std::vector<GroupNode*>{firstNode, secondNode});
      CHECK(placements.front().placedHash == oldHash);
      CHECK(placements.front().entry == &findLamp(library));
      CHECK(library.changedPlacements().empty());

      // an unlinked placement of the same file is a placement of its own
      auto* thirdNode = place(map, findLamp(library), {-512, 0, 0});
      REQUIRE(thirdNode->linkId() != firstNode->linkId());
      const auto threePlacements = library.placements();
      REQUIRE(threePlacements.size() == 2u);
      CHECK(threePlacements[1].firstMember == thirdNode);
      CHECK(threePlacements[1].members == std::vector<GroupNode*>{thirdNode});
    }

    SECTION("A moved brush follows in every copy, overrides and names survive")
    {
      saveLamp(map, {64, 0, 0});
      rescan(library);
      const auto& entry = findLamp(library);
      REQUIRE(entry.hash != oldHash);

      // the file moved the brush; its reference point, the bounds centre, moved too
      const auto brushDelta =
        findBrush(fileGroupOf(entry))->logicalBounds().min - oldFileBrushBounds.min;
      const auto lightDelta =
        findEntity(fileGroupOf(entry))->entity().origin() - oldFileLightOrigin;
      REQUIRE(brushDelta != lightDelta);

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      CHECK(changed.front().firstMember == firstNode);
      CHECK(changed.front().entry == &entry);

      const auto report = refreshPlacement(map, changed.front());
      REQUIRE(report.is_success());
      CHECK(report.value().entitiesLostOverrides == 0u);
      CHECK(report.value().lostEntities.empty());

      // both copies follow the file
      CHECK(
        findBrush(*firstNode)->logicalBounds() == firstBrushBounds.translate(brushDelta));
      CHECK(
        findBrush(*secondNode)->logicalBounds()
        == secondBrushBounds.translate(brushDelta));
      CHECK(findEntity(*firstNode)->entity().origin() == firstLightOrigin + lightDelta);
      CHECK(findEntity(*secondNode)->entity().origin() == secondLightOrigin + lightDelta);

      // the second copy keeps its protected colour, every copy its light's name
      CHECK(propertyOf(*findEntity(*firstNode), "_color") == std::nullopt);
      CHECK(propertyOf(*findEntity(*secondNode), "_color") == "0 1 0");
      CHECK(
        findEntity(*secondNode)->entity().protectedProperties()
        == std::vector<std::string>{"_color"});
      CHECK(propertyOf(*findEntity(*firstNode), "name") == firstLightName);
      CHECK(propertyOf(*findEntity(*secondNode), "name") == secondLightName);

      // the hash is updated on both, so nothing is changed any more
      CHECK(firstNode->group().prefabHash() == entry.hash);
      CHECK(secondNode->group().prefabHash() == entry.hash);
      CHECK(library.changedPlacements().empty());
      CHECK(firstNode->linkId() == secondNode->linkId());

      SECTION("One undo step reverts everything")
      {
        map.undoCommand();
        CHECK(findBrush(*firstNode)->logicalBounds() == firstBrushBounds);
        CHECK(findBrush(*secondNode)->logicalBounds() == secondBrushBounds);
        CHECK(findEntity(*firstNode)->entity().origin() == firstLightOrigin);
        CHECK(propertyOf(*findEntity(*secondNode), "_color") == "0 1 0");
        CHECK(firstNode->group().prefabHash() == oldHash);
        CHECK(secondNode->group().prefabHash() == oldHash);
        CHECK(library.changedPlacements().size() == 1u);
      }
    }

    SECTION("An entity without a counterpart loses its overrides")
    {
      // another entity takes the light's position in the group; the light comes after it
      saveLamp(map, {0, 0, 0}, true);
      rescan(library);
      const auto& entry = findLamp(library);

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      const auto report = refreshPlacement(map, changed.front());
      REQUIRE(report.is_success());
      CHECK(report.value().entitiesLostOverrides == 1u);
      CHECK(report.value().lostEntities == std::vector<std::string>{"lamp"});

      // the light carries no override any more, and every name is unique
      const auto findLight = [](const GroupNode& groupNode) {
        return dynamic_cast<const EntityNode*>(groupNode.children().back());
      };
      const auto* firstLightNode = findLight(*firstNode);
      const auto* secondLightNode = findLight(*secondNode);
      REQUIRE(firstLightNode);
      REQUIRE(secondLightNode);
      CHECK(firstLightNode->entity().classname() == "light");
      CHECK(propertyOf(*secondLightNode, "_color") == std::nullopt);
      CHECK(propertyOf(*firstLightNode, "name") != propertyOf(*secondLightNode, "name"));
      CHECK(firstNode->group().prefabHash() == entry.hash);
      CHECK(secondNode->group().prefabHash() == entry.hash);
    }

    SECTION("A missing file is an error and the map is untouched")
    {
      REQUIRE(env.remove("base/prefabs/props/lamp.map"));
      rescan(library);

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      CHECK(changed.front().entry == nullptr);

      const auto childrenBefore = firstNode->children();
      CHECK(refreshPlacement(map, changed.front()).is_error());
      CHECK(firstNode->children() == childrenBefore);
      CHECK(firstNode->group().prefabHash() == oldHash);
    }

    SECTION("A transaction that fails leaves the map untouched")
    {
      // the second copy is moved so close to the world bounds that the moved brush
      // leaves them there, so the linked group update fails at the commit
      deselectAll(map);
      selectNodes(map, {secondNode});
      REQUIRE(translateSelection(map, {7680, 0, 0}));
      deselectAll(map);
      const auto movedSecondBrushBounds = findBrush(*secondNode)->logicalBounds();
      REQUIRE(map.worldBounds().contains(movedSecondBrushBounds));

      saveLamp(map, {512, 0, 0});
      rescan(library);
      const auto brushDelta =
        findBrush(fileGroupOf(findLamp(library)))->logicalBounds().min
        - oldFileBrushBounds.min;
      REQUIRE(map.worldBounds().contains(firstBrushBounds.translate(brushDelta)));
      REQUIRE_FALSE(
        map.worldBounds().contains(movedSecondBrushBounds.translate(brushDelta)));

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);

      const auto firstChildren = firstNode->children();
      const auto secondChildren = secondNode->children();
      CHECK(refreshPlacement(map, changed.front()).is_error());
      CHECK(firstNode->children() == firstChildren);
      CHECK(secondNode->children() == secondChildren);
      CHECK(findBrush(*firstNode)->logicalBounds() == firstBrushBounds);
      CHECK(firstNode->group().prefabHash() == oldHash);
      CHECK(secondNode->group().prefabHash() == oldHash);
    }
  }

  SECTION("applyPrefabToSelection")
  {
    env.createFile("base/prefabs/lights/red.map", RedLight);
    rescan(library);
    const auto* entry = library.find(PrefabRoot::Mod, "lights/red.map");
    REQUIRE(entry);
    REQUIRE(entry->metadata.kind == PrefabKind::Entity);

    auto* targetingNode = new EntityNode{Entity{
      {{"classname", "light"},
       {"origin", "64 0 0"},
       {"name", "l1"},
       {"target", "l2"},
       {"target2", "l3"},
       {"light_radius", "64"}}}};
    auto* extraKeyNode = new EntityNode{Entity{
      {{"classname", "light"}, {"origin", "128 0 0"}, {"name", "l2"}, {"extra", "1"}}}};
    auto* protectedNode = new EntityNode{
      Entity{{{"classname", "light"}, {"origin", "192 0 0"}, {"name", "l3"}}}};
    auto* otherNode = new EntityNode{Entity{
      {{"classname", "info_player_start"}, {"origin", "0 0 0"}, {"name", "start"}}}};
    addNodes(
      map,
      {{&parentForNodes(map), {targetingNode, extraKeyNode, protectedNode, otherNode}}});

    selectNodes(map, {protectedNode});
    REQUIRE(setProtectedEntityProperty(map, "_color", true));
    REQUIRE(setEntityProperty(map, "_color", "0 1 0"));
    deselectAll(map);

    selectNodes(map, {targetingNode, extraKeyNode, protectedNode, otherNode});
    const auto before = std::vector{
      targetingNode->entity(),
      extraKeyNode->entity(),
      protectedNode->entity(),
      otherNode->entity()};

    const auto count = applyPrefabToSelection(map, *entry);
    REQUIRE(count.is_success());
    CHECK(count.value() == 3u);

    // the snapshot's keys, the placement keys kept
    CHECK(propertyOf(*targetingNode, "light_radius") == "300");
    CHECK(propertyOf(*targetingNode, "_color") == "1 0 0");
    CHECK(propertyOf(*targetingNode, "origin") == "64 0 0");
    CHECK(propertyOf(*targetingNode, "name") == "l1");
    CHECK(propertyOf(*targetingNode, "target") == "l2");
    CHECK(propertyOf(*targetingNode, "target2") == "l3");

    // a key that the snapshot lacks is removed
    CHECK(propertyOf(*extraKeyNode, "extra") == std::nullopt);
    CHECK(propertyOf(*extraKeyNode, "light_radius") == "300");
    CHECK(propertyOf(*extraKeyNode, "name") == "l2");
    CHECK(propertyOf(*extraKeyNode, "origin") == "128 0 0");

    // a protected property is untouched
    CHECK(propertyOf(*protectedNode, "_color") == "0 1 0");
    CHECK(
      protectedNode->entity().protectedProperties()
      == std::vector<std::string>{"_color"});
    CHECK(propertyOf(*protectedNode, "light_radius") == "300");

    // another class is untouched
    CHECK(otherNode->entity() == before[3]);

    SECTION("One undo step restores every entity")
    {
      map.undoCommand();
      CHECK(targetingNode->entity() == before[0]);
      CHECK(extraKeyNode->entity() == before[1]);
      CHECK(protectedNode->entity() == before[2]);
    }

    SECTION("Nothing of the class is selected")
    {
      deselectAll(map);
      selectNodes(map, {otherNode});
      const auto none = applyPrefabToSelection(map, *entry);
      REQUIRE(none.is_success());
      CHECK(none.value() == 0u);
    }

    SECTION("An assembly cannot be applied")
    {
      saveLamp(map, {0, 0, 0});
      rescan(library);
      const auto* lampEntry = library.find(PrefabRoot::Mod, "props/lamp.map");
      REQUIRE(lampEntry);
      CHECK(applyPrefabToSelection(map, *lampEntry).is_error());
    }
  }
}

} // namespace tb::mdl
