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
#include "mdl/EditorContext.h"
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
"target" "snap_target"
"killtarget" "snap_kill"
"model" "snap_model"
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
 * Writes base/maps/prefabs/props/lamp.map from the given nodes, which are added to the
 * map for the save and taken out of it again.
 */
void saveLampFrom(Map& map, const std::vector<Node*>& nodes)
{
  addNodes(map, {{&parentForNodes(map), nodes}});
  REQUIRE(
    savePrefab(
      map,
      nodes,
      SavePrefabOptions{.name = "lamp", .category = "props", .root = PrefabRoot::Mod})
    | kdl::is_success());
  removeNodes(map, nodes);
}

/**
 * Writes base/maps/prefabs/props/lamp.map from a brush at the given offset and a light
 * named "lamp" at 0 0 64, in this order, with an info_null between them if withMarker is
 * set. In the file, the group's brushes come before its entities whatever the order of
 * the selection.
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
  saveLampFrom(map, nodes);
}

/**
 * A group named "inner" that holds one brush at the given offset, in the map.
 */
GroupNode* addInnerGroup(Map& map, const vm::vec3d& brushOffset)
{
  auto* brushNode = createBrushNodeAt(map, brushOffset);
  addNodes(map, {{&parentForNodes(map), {brushNode}}});
  deselectAll(map);
  selectNodes(map, {brushNode});
  auto* groupNode = groupSelectedNodes(map, "inner");
  REQUIRE(groupNode);
  deselectAll(map);
  return groupNode;
}

/**
 * Writes base/maps/prefabs/props/lamp.map from two inner groups, the second 128 units
 * along X from the first, which are linked if linked is set.
 */
void saveInnerPair(Map& map, const bool linked)
{
  auto* firstInner = addInnerGroup(map, {0, 0, 0});
  auto* secondInner = static_cast<GroupNode*>(nullptr);
  if (linked)
  {
    selectNodes(map, {firstInner});
    secondInner = createLinkedDuplicate(map);
    REQUIRE(secondInner);
    deselectAll(map);
    selectNodes(map, {secondInner});
    REQUIRE(translateSelection(map, {128, 0, 0}));
    deselectAll(map);
    REQUIRE(secondInner->linkId() == firstInner->linkId());
  }
  else
  {
    secondInner = addInnerGroup(map, {128, 0, 0});
    REQUIRE(secondInner->linkId() != firstInner->linkId());
  }

  REQUIRE(
    savePrefab(
      map,
      {firstInner, secondInner},
      SavePrefabOptions{.name = "lamp", .category = "props", .root = PrefabRoot::Mod})
    | kdl::is_success());
  removeNodes(map, {firstInner, secondInner});
}

std::vector<GroupNode*> innerGroupsOf(const GroupNode& groupNode)
{
  auto result = std::vector<GroupNode*>{};
  for (auto* child : groupNode.children())
  {
    if (auto* innerNode = dynamic_cast<GroupNode*>(child))
    {
      result.push_back(innerNode);
    }
  }
  return result;
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
    e.createDirectory("base/maps/prefabs/lights");
    e.createDirectory("base/maps/prefabs/props");
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
     {PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
      PropertyDefinition{"killtarget", PropertyValueTypes::LinkSource{}, "", ""}},
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

      // the same placements by their keys
      const auto byKey = library.placementsByKey();
      REQUIRE(byKey.size() == 2u);
      const auto firstIt = byKey.find(PrefabPlacementKey{LampPath, firstNode->linkId()});
      REQUIRE(firstIt != byKey.end());
      CHECK(firstIt->second.members == std::vector<GroupNode*>{firstNode, secondNode});
      const auto thirdIt = byKey.find(PrefabPlacementKey{LampPath, thirdNode->linkId()});
      REQUIRE(thirdIt != byKey.end());
      CHECK(thirdIt->second.firstMember == thirdNode);
      CHECK(thirdIt->second.entry == &findLamp(library));
    }

    SECTION("A moved brush follows in every copy, overrides and names survive")
    {
      saveLamp(map, {64, 0, 0});
      rescan(library);
      const auto& entry = findLamp(library);
      REQUIRE(entry.hash != oldHash);

      // the file moved the brush; its reference point, the bounds centre, moved too, so
      // that the light, which did not move, moved in the file (H-E-5)
      const auto brushDelta =
        findBrush(fileGroupOf(entry))->logicalBounds().min - oldFileBrushBounds.min;
      const auto lightDelta =
        findEntity(fileGroupOf(entry))->entity().origin() - oldFileLightOrigin;
      REQUIRE(brushDelta != lightDelta);
      REQUIRE(lightDelta != vm::vec3d{0, 0, 0});
      REQUIRE(brushDelta - lightDelta == vm::vec3d{64, 0, 0});

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      CHECK(changed.front().firstMember == firstNode);
      CHECK(changed.front().entry == &entry);

      const auto report = refreshPlacement(map, changed.front());
      REQUIRE(report.is_success());
      CHECK(report.value().entitiesLostOverrides == 0u);
      CHECK(report.value().lostEntities.empty());

      // DD-23: one brush against one light is a tie, which the light's origin wins, so
      // the light stays and the brush moves by its own delta in both copies
      CHECK(report.value().referenceShift == -lightDelta);
      CHECK(
        findBrush(*firstNode)->logicalBounds()
        == firstBrushBounds.translate(vm::vec3d{64, 0, 0}));
      CHECK(
        findBrush(*secondNode)->logicalBounds()
        == secondBrushBounds.translate(vm::vec3d{64, 0, 0}));
      CHECK(findEntity(*firstNode)->entity().origin() == firstLightOrigin);
      CHECK(findEntity(*secondNode)->entity().origin() == secondLightOrigin);

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

    SECTION("A file in which everything moved moves everything")
    {
      // an unlinked placement of the same file
      auto* thirdNode = place(map, findLamp(library), {-512, 0, 0});
      const auto thirdBrushBounds = findBrush(*thirdNode)->logicalBounds();
      const auto thirdLightOrigin = findEntity(*thirdNode)->entity().origin();

      // every child of the first copy moves, and the prefab is updated from the copy
      // (D13), so that its group carries the reference point
      const auto delta = vm::vec3d{0, 0, 32};
      openGroup(map, *firstNode);
      selectNodes(map, firstNode->children());
      REQUIRE(translateSelection(map, delta));
      deselectAll(map);
      closeGroup(map);
      REQUIRE(
        savePrefab(
          map,
          {firstNode},
          SavePrefabOptions{.name = "lamp", .category = "props", .root = PrefabRoot::Mod})
        | kdl::is_success());
      rescan(library);
      const auto& entry = findLamp(library);
      REQUIRE(entry.hash != oldHash);

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 2u);
      REQUIRE(changed[0].firstMember == firstNode);
      REQUIRE(changed[1].firstMember == thirdNode);

      // the copies that were moved already stay where they are
      const auto firstReport = refreshPlacement(map, changed[0]);
      REQUIRE(firstReport.is_success());
      CHECK(firstReport.value().referenceShift == vm::vec3d{0, 0, 0});
      CHECK(findBrush(*firstNode)->logicalBounds() == firstBrushBounds.translate(delta));
      CHECK(findEntity(*firstNode)->entity().origin() == firstLightOrigin + delta);
      CHECK(
        findBrush(*secondNode)->logicalBounds() == secondBrushBounds.translate(delta));

      // the other placement moves by the whole delta; nothing is corrected away
      const auto thirdChanged = library.changedPlacements();
      REQUIRE(thirdChanged.size() == 1u);
      REQUIRE(thirdChanged.front().firstMember == thirdNode);
      const auto thirdReport = refreshPlacement(map, thirdChanged.front());
      REQUIRE(thirdReport.is_success());
      CHECK(thirdReport.value().referenceShift == vm::vec3d{0, 0, 0});
      CHECK(findBrush(*thirdNode)->logicalBounds() == thirdBrushBounds.translate(delta));
      CHECK(findEntity(*thirdNode)->entity().origin() == thirdLightOrigin + delta);
      CHECK(library.changedPlacements().empty());
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
      REQUIRE(env.remove("base/maps/prefabs/props/lamp.map"));
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

      // the light stays (DD-23), so the brush moves by its own delta
      const auto brushDelta = vm::vec3d{512, 0, 0};
      saveLamp(map, brushDelta);
      rescan(library);
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

      // R1-pf-mdl-1: no member is left pending for the next commit
      CHECK_FALSE(firstNode->hasPendingChanges());
      CHECK_FALSE(secondNode->hasPendingChanges());
    }

    SECTION("An error before the commit leaves the map untouched and nothing pending")
    {
      // the first copy is moved so close to the world bounds that its new brush leaves
      // them, so the refresh fails before it changes the map
      deselectAll(map);
      selectNodes(map, {firstNode});
      REQUIRE(translateSelection(map, {7680, 0, 0}));
      deselectAll(map);
      const auto movedFirstBrushBounds = findBrush(*firstNode)->logicalBounds();

      saveLamp(map, {512, 0, 0});
      rescan(library);
      REQUIRE_FALSE(map.worldBounds().contains(
        movedFirstBrushBounds.translate(vm::vec3d{512, 0, 0})));

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(changed.front().firstMember == firstNode);

      const auto firstChildren = firstNode->children();
      const auto secondChildren = secondNode->children();
      CHECK(refreshPlacement(map, changed.front()).is_error());
      CHECK(firstNode->children() == firstChildren);
      CHECK(secondNode->children() == secondChildren);
      CHECK(firstNode->group().prefabHash() == oldHash);
      CHECK_FALSE(firstNode->hasPendingChanges());
      CHECK_FALSE(secondNode->hasPendingChanges());
    }

    SECTION("A selection inside a copy is cleared")
    {
      saveLamp(map, {64, 0, 0});
      rescan(library);

      selectNodes(map, {findBrush(*secondNode)});
      REQUIRE(map.selection().hasNodes());

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(refreshPlacement(map, changed.front()).is_success());
      CHECK_FALSE(map.selection().hasNodes());
      CHECK(
        findBrush(*secondNode)->logicalBounds()
        == secondBrushBounds.translate(vm::vec3d{64, 0, 0}));

      // the undo restores the old children, not the selection
      map.undoCommand();
      CHECK(findBrush(*secondNode)->logicalBounds() == secondBrushBounds);
    }

    SECTION("A placement inside linked groups")
    {
      // an unlinked placement of the same file, grouped, and the group duplicated
      // linked, so that the placement and its copy are members of one link set inside
      // the members of another
      auto* innerNode = place(map, findLamp(library), {-512, 0, 0});
      selectNodes(map, {innerNode});
      auto* outerNode = groupSelectedNodes(map, "outer");
      REQUIRE(outerNode);
      deselectAll(map);
      selectNodes(map, {outerNode});
      auto* outerCopyNode = createLinkedDuplicate(map);
      REQUIRE(outerCopyNode);
      deselectAll(map);
      selectNodes(map, {outerCopyNode});
      REQUIRE(translateSelection(map, {0, -1024, 0}));
      deselectAll(map);

      const auto innerGroups = innerGroupsOf(*outerCopyNode);
      REQUIRE(innerGroups.size() == 1u);
      auto* innerCopyNode = innerGroups.front();
      REQUIRE(innerCopyNode->linkId() == innerNode->linkId());
      const auto innerBrushBounds = findBrush(*innerNode)->logicalBounds();
      const auto innerCopyBrushBounds = findBrush(*innerCopyNode)->logicalBounds();

      saveLamp(map, {64, 0, 0});
      rescan(library);

      const auto changed = library.changedPlacements();
      const auto it = std::ranges::find_if(changed, [&](const auto& placement) {
        return placement.firstMember == innerNode;
      });
      REQUIRE(it != changed.end());
      CHECK(it->members == std::vector<GroupNode*>{innerNode, innerCopyNode});

      REQUIRE(refreshPlacement(map, *it).is_success());
      CHECK(
        findBrush(*innerNode)->logicalBounds()
        == innerBrushBounds.translate(vm::vec3d{64, 0, 0}));
      CHECK(
        findBrush(*innerCopyNode)->logicalBounds()
        == innerCopyBrushBounds.translate(vm::vec3d{64, 0, 0}));
      CHECK(innerNode->group().prefabHash() == findLamp(library).hash);
      CHECK(innerCopyNode->group().prefabHash() == findLamp(library).hash);
      CHECK(outerCopyNode->linkId() == outerNode->linkId());

      // the other placement is untouched
      CHECK(findBrush(*firstNode)->logicalBounds() == firstBrushBounds);
    }

    SECTION("A transplanted entity keeps its name against an earlier one of the file")
    {
      // no brush, so that the first entity of the file stands where the brush stood, and
      // takes the light's name; the light stands where the light stood
      saveLampFrom(
        map,
        {new EntityNode{
           Entity{{{"classname", "info_null"}, {"origin", "0 0 0"}, {"name", "lamp"}}}},
         new EntityNode{
           Entity{{{"classname", "light"}, {"origin", "0 0 64"}, {"name", "other"}}}}});
      rescan(library);
      const auto& fileGroup = fileGroupOf(findLamp(library));
      REQUIRE(fileGroup.childCount() == 2u);
      REQUIRE(
        propertyOf(*static_cast<const EntityNode*>(fileGroup.children().front()), "name")
        == firstLightName);

      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      const auto report = refreshPlacement(map, changed.front());
      REQUIRE(report.is_success());
      CHECK(report.value().lostEntities.empty());

      // the light keeps its name in both copies, the info_null gets another
      const auto findLight = [](const GroupNode& groupNode) {
        return dynamic_cast<const EntityNode*>(groupNode.children().back());
      };
      const auto findMarker = [](const GroupNode& groupNode) {
        return dynamic_cast<const EntityNode*>(groupNode.children().front());
      };
      REQUIRE(findLight(*firstNode));
      REQUIRE(findMarker(*firstNode));
      CHECK(findLight(*firstNode)->entity().classname() == "light");
      CHECK(propertyOf(*findLight(*firstNode), "name") == firstLightName);
      CHECK(propertyOf(*findLight(*secondNode), "name") == secondLightName);
      CHECK(propertyOf(*findMarker(*firstNode), "name") != firstLightName);
      CHECK(
        propertyOf(*findMarker(*firstNode), "name")
        != propertyOf(*findMarker(*secondNode), "name"));
      CHECK(propertyOf(*findLight(*secondNode), "_color") == "0 1 0");
    }
  }

  SECTION("refreshPlacement with inner groups")
  {
    const auto innerIdsLinked = [](const GroupNode& groupNode) {
      const auto innerGroups = innerGroupsOf(groupNode);
      REQUIRE(innerGroups.size() == 2u);
      return innerGroups[0]->linkId() == innerGroups[1]->linkId();
    };

    SECTION("A file that unlinks the inner groups unlinks them")
    {
      saveInnerPair(map, true);
      rescan(library);
      auto* placedNode = place(map, findLamp(library), {256, 0, 0});
      REQUIRE(innerIdsLinked(*placedNode));
      const auto innerBounds = std::vector{
        innerGroupsOf(*placedNode)[0]->logicalBounds(),
        innerGroupsOf(*placedNode)[1]->logicalBounds()};

      saveInnerPair(map, false);
      rescan(library);
      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      const auto report = refreshPlacement(map, changed.front());
      REQUIRE(report.is_success());
      CHECK(report.value().referenceShift == vm::vec3d{0, 0, 0});

      CHECK_FALSE(innerIdsLinked(*placedNode));
      CHECK(innerGroupsOf(*placedNode)[0]->logicalBounds() == innerBounds[0]);
      CHECK(innerGroupsOf(*placedNode)[1]->logicalBounds() == innerBounds[1]);
    }

    SECTION("A file that links the inner groups links them")
    {
      saveInnerPair(map, false);
      rescan(library);
      auto* placedNode = place(map, findLamp(library), {256, 0, 0});
      REQUIRE_FALSE(innerIdsLinked(*placedNode));
      const auto firstInnerId = innerGroupsOf(*placedNode)[0]->linkId();

      saveInnerPair(map, true);
      rescan(library);
      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(refreshPlacement(map, changed.front()).is_success());

      // the inner link set took the first inner group's old ID
      CHECK(innerIdsLinked(*placedNode));
      CHECK(innerGroupsOf(*placedNode)[0]->linkId() == firstInnerId);
    }

    SECTION("An inner group linked to a group outside the placement keeps its own ID")
    {
      saveInnerPair(map, false);
      rescan(library);
      auto* placedNode = place(map, findLamp(library), {256, 0, 0});

      // a linked copy of the first inner group outside the placement, as a user may make
      // one
      auto* outsideNode = static_cast<GroupNode*>(
        innerGroupsOf(*placedNode)[0]->cloneRecursively(map.worldBounds()));
      addNodes(map, {{&parentForNodes(map), {outsideNode}}});
      const auto outsideId = outsideNode->linkId();
      REQUIRE(outsideId == innerGroupsOf(*placedNode)[0]->linkId());
      const auto outsideChildren = outsideNode->children();

      // the file changes, so that the placement is refreshed
      saveInnerPair(map, true);
      rescan(library);
      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(refreshPlacement(map, changed.front()).is_success());

      // the placement's inner groups do not join the outside group's link set
      for (const auto* innerNode : innerGroupsOf(*placedNode))
      {
        CHECK(innerNode->linkId() != outsideId);
      }
      CHECK(innerIdsLinked(*placedNode));
      CHECK(outsideNode->children() == outsideChildren);
    }
  }

  SECTION("refreshPlacement closes an open group inside a member")
  {
    // DD-35: a group open below a member would be detached by the refresh, and the next
    // node added to it would fail a contract
    const auto isInWorld = [&](const Node& node) {
      return &node == &map.worldNode() || node.isDescendantOf(map.worldNode());
    };
    const auto canAddNode = [&]() {
      auto* brushNode = createBrushNodeAt(map, {0, 0, -256});
      auto& parentNode = parentForNodes(map);
      REQUIRE(isInWorld(parentNode));
      CHECK(addNodes(map, {{&parentNode, {brushNode}}}).size() == 1u);
    };

    SECTION("A nested group open inside a member other than the first")
    {
      saveInnerPair(map, false);
      rescan(library);
      auto* firstNode = place(map, findLamp(library), {256, 0, 0});
      auto* secondNode = placeLinkedCopy(map, *firstNode, {0, 512, 0});
      const auto firstChildren = firstNode->children();
      const auto secondChildren = secondNode->children();

      auto* openNode = innerGroupsOf(*secondNode).front();
      openGroup(map, *secondNode);
      openGroup(map, *openNode);
      REQUIRE(map.editorContext().currentGroup() == openNode);

      saveInnerPair(map, true);
      rescan(library);
      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(changed.front().firstMember == firstNode);
      REQUIRE(refreshPlacement(map, changed.front()).is_success());

      // the old inner group is gone from the second member; the member stays open
      REQUIRE_FALSE(kdl::ranges::contains(secondNode->children(), openNode));
      CHECK(map.editorContext().currentGroup() == secondNode);
      canAddNode();

      SECTION("Undo restores the content and the open group")
      {
        // the brush that canAddNode added, then the refresh
        map.undoCommand();
        map.undoCommand();
        CHECK(firstNode->children() == firstChildren);
        CHECK(secondNode->children() == secondChildren);
        CHECK(map.editorContext().currentGroup() == openNode);
      }
    }

    SECTION("A group two levels deep inside the first member")
    {
      // the file's group holds a brush and a group "mid" that holds the group "inner"
      const auto saveNested = [&](const vm::vec3d& brushOffset) {
        auto* innerNode = addInnerGroup(map, {0, 0, 0});
        selectNodes(map, {innerNode});
        auto* midNode = groupSelectedNodes(map, "mid");
        REQUIRE(midNode);
        deselectAll(map);
        auto* brushNode = createBrushNodeAt(map, brushOffset);
        addNodes(map, {{&parentForNodes(map), {brushNode}}});
        REQUIRE(
          savePrefab(
            map,
            {midNode, brushNode},
            SavePrefabOptions{
              .name = "lamp", .category = "props", .root = PrefabRoot::Mod})
          | kdl::is_success());
        removeNodes(map, {midNode, brushNode});
      };

      saveNested({128, 0, 0});
      rescan(library);
      auto* firstNode = place(map, findLamp(library), {256, 0, 0});
      auto* secondNode = placeLinkedCopy(map, *firstNode, {0, 512, 0});
      const auto firstChildren = firstNode->children();
      const auto secondChildren = secondNode->children();

      const auto midGroups = innerGroupsOf(*firstNode);
      REQUIRE(midGroups.size() == 1u);
      auto* midNode = midGroups.front();
      const auto innerGroups = innerGroupsOf(*midNode);
      REQUIRE(innerGroups.size() == 1u);
      auto* openNode = innerGroups.front();
      openGroup(map, *firstNode);
      openGroup(map, *midNode);
      openGroup(map, *openNode);
      REQUIRE(map.editorContext().currentGroup() == openNode);

      saveNested({192, 0, 0});
      rescan(library);
      const auto changed = library.changedPlacements();
      REQUIRE(changed.size() == 1u);
      REQUIRE(refreshPlacement(map, changed.front()).is_success());

      REQUIRE_FALSE(kdl::ranges::contains(firstNode->children(), midNode));
      CHECK(map.editorContext().currentGroup() == firstNode);
      canAddNode();

      SECTION("Undo restores the content and the open group")
      {
        map.undoCommand();
        map.undoCommand();
        CHECK(firstNode->children() == firstChildren);
        CHECK(secondNode->children() == secondChildren);
        CHECK(map.editorContext().currentGroup() == openNode);
      }
    }
  }

  SECTION("refreshPlacement aligns by the first of tied displacements")
  {
    // two brushes 256 units apart, saved from loose nodes
    saveLampFrom(
      map, {createBrushNodeAt(map, {0, 0, 0}), createBrushNodeAt(map, {256, 0, 0})});
    rescan(library);
    auto* placedNode = place(map, findLamp(library), {0, 0, 512});
    REQUIRE(placedNode->childCount() == 2u);
    const auto oldFirstBounds = placedNode->children()[0]->logicalBounds();

    // the second brush moves in the file, which moves the reference point by half of it,
    // so that the two brushes are displaced in opposite directions: a tie of two
    // displacements, neither of them zero
    saveLampFrom(
      map, {createBrushNodeAt(map, {0, 0, 0}), createBrushNodeAt(map, {320, 0, 0})});
    rescan(library);
    const auto& fileGroup = fileGroupOf(findLamp(library));
    REQUIRE(fileGroup.childCount() == 2u);
    const auto fileOffset = fileGroup.children()[1]->logicalBounds().min
                            - fileGroup.children()[0]->logicalBounds().min;

    const auto changed = library.changedPlacements();
    REQUIRE(changed.size() == 1u);
    const auto report = refreshPlacement(map, changed.front());
    REQUIRE(report.is_success());

    // R2-pf-mdl-11: the first displacement wins, so the first child stays where it was
    CHECK(report.value().referenceShift != vm::vec3d{0, 0, 0});
    REQUIRE(placedNode->childCount() == 2u);
    CHECK(placedNode->children()[0]->logicalBounds() == oldFirstBounds);
    CHECK(
      placedNode->children()[1]->logicalBounds().min
        - placedNode->children()[0]->logicalBounds().min
      == fileOffset);
  }

  SECTION("refreshPlacement of a large assembly")
  {
    // R2-pf-mdl-2: the library keeps no world for it, so the refresh reads the file and
    // takes its nodes instead of copying them
    const auto saveBig = [&](const vm::vec3d& lightOrigin) {
      auto nodes = std::vector<Node*>{};
      for (size_t i = 0; i < PrefabWorldBrushBudget + 1; ++i)
      {
        nodes.push_back(createBrushNode(map));
      }
      nodes.push_back(new EntityNode{Entity{
        {{"classname", "light"},
         {"origin",
          fmt::format("{} {} {}", lightOrigin.x(), lightOrigin.y(), lightOrigin.z())},
         {"name", "lamp"}}}});
      saveLampFrom(map, nodes);
      rescan(library);
    };
    const auto findBig = [&]() -> const PrefabEntry& {
      const auto* entry = library.find(PrefabRoot::Mod, "props/lamp.map");
      REQUIRE(entry);
      REQUIRE(entry->world == nullptr);
      REQUIRE(entry->bodyPreview);
      return *entry;
    };

    saveBig({0, 0, 0});
    auto* firstNode = place(map, findBig(), {256, 0, 0});
    auto* secondNode = placeLinkedCopy(map, *firstNode, {0, 512, 0});
    REQUIRE(firstNode->childCount() == PrefabWorldBrushBudget + 2);
    const auto firstBrushBounds = findBrush(*firstNode)->logicalBounds();
    const auto firstLightOrigin = findEntity(*firstNode)->entity().origin();
    const auto secondLightOrigin = findEntity(*secondNode)->entity().origin();
    const auto oldHash = findBig().hash;

    // the light moves up in the file; the brushes, the majority, stay
    saveBig({0, 0, 64});
    const auto& entry = findBig();
    REQUIRE(entry.hash != oldHash);

    const auto changed = library.changedPlacements();
    REQUIRE(changed.size() == 1u);
    const auto report = refreshPlacement(map, changed.front());
    REQUIRE(report.is_success());
    CHECK(report.value().entitiesLostOverrides == 0u);

    CHECK(firstNode->childCount() == PrefabWorldBrushBudget + 2);
    CHECK(secondNode->childCount() == PrefabWorldBrushBudget + 2);
    CHECK(findBrush(*firstNode)->logicalBounds() == firstBrushBounds);
    CHECK(
      findEntity(*firstNode)->entity().origin()
      == firstLightOrigin + vm::vec3d{0, 0, 64});
    CHECK(
      findEntity(*secondNode)->entity().origin()
      == secondLightOrigin + vm::vec3d{0, 0, 64});
    CHECK(propertyOf(*findEntity(*firstNode), "name") == "lamp");
    CHECK(firstNode->group().prefabHash() == entry.hash);
    CHECK(secondNode->group().prefabHash() == entry.hash);
    CHECK(library.changedPlacements().empty());
    // the library still keeps no world for it
    CHECK(findBig().world == nullptr);

    map.undoCommand();
    CHECK(findEntity(*firstNode)->entity().origin() == firstLightOrigin);
    CHECK(findEntity(*secondNode)->entity().origin() == secondLightOrigin);
    CHECK(firstNode->group().prefabHash() == oldHash);
  }

  SECTION("applyPrefabToSelection")
  {
    env.createFile("base/maps/prefabs/lights/red.map", RedLight);
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
       {"killtarget", "l2"},
       {"light_radius", "64"}}}};
    auto* extraKeyNode = new EntityNode{Entity{
      {{"classname", "light"}, {"origin", "128 0 0"}, {"name", "l2"}, {"extra", "1"}}}};
    auto* protectedNode = new EntityNode{
      Entity{{{"classname", "light"}, {"origin", "192 0 0"}, {"name", "l3"}}}};
    auto* otherNode = new EntityNode{Entity{
      {{"classname", "info_player_start"}, {"origin", "0 0 0"}, {"name", "start"}}}};
    // a brush entity of the class, whose model is its own (H-E-10)
    auto* brushLightNode = new EntityNode{Entity{
      {{"classname", "light"}, {"name", "l4"}, {"model", "l4"}, {"light_radius", "8"}}}};
    auto* brushNode = createBrushNodeAt(map, {512, 0, 0});
    brushLightNode->addChild(brushNode);
    addNodes(
      map,
      {{&parentForNodes(map),
        {targetingNode, extraKeyNode, protectedNode, otherNode, brushLightNode}}});

    selectNodes(map, {protectedNode});
    REQUIRE(setProtectedEntityProperty(map, "_color", true));
    REQUIRE(setEntityProperty(map, "_color", "0 1 0"));
    deselectAll(map);

    selectNodes(map, {targetingNode, extraKeyNode, protectedNode, otherNode, brushNode});
    const auto before = std::vector{
      targetingNode->entity(),
      extraKeyNode->entity(),
      protectedNode->entity(),
      otherNode->entity(),
      brushLightNode->entity()};
    REQUIRE(propertyOf(*brushLightNode, "model"));

    CHECK(canApplyPrefabToSelection(map, *entry));
    const auto count = applyPrefabToSelection(map, *entry);
    REQUIRE(count.is_success());
    CHECK(count.value() == 4u);

    // the snapshot's keys, the placement keys kept
    CHECK(propertyOf(*targetingNode, "light_radius") == "300");
    CHECK(propertyOf(*targetingNode, "_color") == "1 0 0");
    CHECK(propertyOf(*targetingNode, "origin") == "64 0 0");
    CHECK(propertyOf(*targetingNode, "name") == "l1");
    CHECK(propertyOf(*targetingNode, "target") == "l2");
    CHECK(propertyOf(*targetingNode, "target2") == "l3");
    // C6: a key that the definition types as a link source keeps its value
    CHECK(propertyOf(*targetingNode, "killtarget") == "l2");
    // a point entity's model is its asset, which the snapshot gives
    CHECK(propertyOf(*targetingNode, "model") == "snap_model");

    // a key that the snapshot lacks is removed
    CHECK(propertyOf(*extraKeyNode, "extra") == std::nullopt);
    CHECK(propertyOf(*extraKeyNode, "light_radius") == "300");
    CHECK(propertyOf(*extraKeyNode, "name") == "l2");
    CHECK(propertyOf(*extraKeyNode, "origin") == "128 0 0");

    // a kept key that the entity lacks stays absent
    CHECK(propertyOf(*extraKeyNode, "target") == std::nullopt);
    CHECK(propertyOf(*extraKeyNode, "killtarget") == std::nullopt);

    // a brush entity keeps its model key (H-E-10) and has no origin key still
    CHECK(propertyOf(*brushLightNode, "model") == *before[4].property("model"));
    CHECK(propertyOf(*brushLightNode, "origin") == std::nullopt);
    CHECK(propertyOf(*brushLightNode, "light_radius") == "300");
    CHECK(propertyOf(*brushLightNode, "name") == "l4");

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
      CHECK(brushLightNode->entity() == before[4]);
    }

    SECTION("Entities in linked copies change once, through the first copy")
    {
      auto* lightNode = new EntityNode{Entity{
        {{"classname", "light"},
         {"origin", "0 0 512"},
         {"name", "l5"},
         {"light_radius", "1"}}}};
      addNodes(map, {{&parentForNodes(map), {lightNode}}});
      deselectAll(map);
      selectNodes(map, {lightNode});
      auto* groupNode = groupSelectedNodes(map, "lights");
      REQUIRE(groupNode);
      deselectAll(map);
      selectNodes(map, {groupNode});
      auto* copyNode = createLinkedDuplicate(map);
      REQUIRE(copyNode);
      deselectAll(map);
      selectNodes(map, {copyNode});
      REQUIRE(translateSelection(map, {0, 0, 128}));
      deselectAll(map);

      auto* copyLightNode = findEntity(*copyNode);
      REQUIRE(copyLightNode);
      const auto copyLightName = propertyOf(*copyLightNode, "name");
      REQUIRE(copyLightName != "l5");

      SECTION("Both copies selected")
      {
        selectNodes(map, {copyLightNode, lightNode});
      }
      SECTION("Only the second copy selected")
      {
        selectNodes(map, {copyLightNode});
      }

      const auto linkedCount = applyPrefabToSelection(map, *entry);
      REQUIRE(linkedCount.is_success());
      CHECK(linkedCount.value() == 1u);

      // the linked group update carries the change to the copy, which keeps its name
      CHECK(propertyOf(*findEntity(*groupNode), "light_radius") == "300");
      CHECK(propertyOf(*findEntity(*copyNode), "light_radius") == "300");
      CHECK(propertyOf(*findEntity(*groupNode), "name") == "l5");
      CHECK(propertyOf(*findEntity(*copyNode), "name") == copyLightName);
    }

    SECTION("Nothing of the class is selected")
    {
      deselectAll(map);
      selectNodes(map, {otherNode});
      CHECK_FALSE(canApplyPrefabToSelection(map, *entry));
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
      CHECK_FALSE(canApplyPrefabToSelection(map, *lampEntry));
      CHECK(applyPrefabToSelection(map, *lampEntry).is_error());
    }
  }
}

} // namespace tb::mdl
