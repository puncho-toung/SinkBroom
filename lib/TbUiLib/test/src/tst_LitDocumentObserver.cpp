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

#include <QCoreApplication>

#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "lit/Snapshot.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/LitDocumentObserver.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::ui
{
namespace
{

/**
 * The session as the observer sees it: a one-slot inbox that the test empties, as the
 * render thread's frame does, and the material service's reload.
 */
class FakeSession
{
public:
  bool shown = true;
  bool inboxFull = false;
  // refuses offers, as a session whose render thread stopped does
  bool refuse = false;
  std::vector<std::shared_ptr<const lit::Snapshot>> taken;
  std::size_t reloads = 0;

  LitSnapshotTarget target()
  {
    return {
      [this] { return shown; },
      [this] { return !inboxFull; },
      [this](std::shared_ptr<const lit::Snapshot> snapshot) {
        if (inboxFull || refuse)
        {
          return false;
        }
        inboxFull = true;
        taken.push_back(std::move(snapshot));
        return true;
      },
      [this] { ++reloads; },
    };
  }

  // a frame takes the inbox and the render thread posts ReadyForSnapshot
  void take(LitDocumentObserver& observer)
  {
    inboxFull = false;
    observer.readyForSnapshot();
  }

  const lit::Snapshot& last() const
  {
    REQUIRE(!taken.empty());
    return *taken.back();
  }
};

void processEvents()
{
  QCoreApplication::processEvents();
}

mdl::BrushNode* addBrush(mdl::Map& map)
{
  auto* brushNode = mdl::createBrushNode(map);
  mdl::addNodes(map, {{map.editorContext().currentLayer(), {brushNode}}});
  return brushNode;
}

const lit::SnapshotNode* nodeOf(const lit::Snapshot& snapshot, const lit::PreviewId id)
{
  const auto it = std::ranges::find(snapshot.nodes, id, &lit::SnapshotNode::id);
  return it != snapshot.nodes.end() ? &*it : nullptr;
}

const lit::SnapshotEntity* entityOf(
  const lit::Snapshot& snapshot, const lit::PreviewId id)
{
  const auto it = std::ranges::find(snapshot.entities, id, &lit::SnapshotEntity::id);
  return it != snapshot.entities.end() ? &*it : nullptr;
}

bool removes(const lit::Snapshot& snapshot, const lit::PreviewId id)
{
  return std::ranges::find(snapshot.removed, id) != snapshot.removed.end();
}

std::size_t nodeCount(const mdl::Node& node)
{
  auto count = std::size_t{1};
  for (const auto* child : node.children())
  {
    count += nodeCount(*child);
  }
  return count;
}

// the snapshot's polygon vertices of a brush node
std::vector<vm::vec3d> verticesOf(
  const lit::Snapshot& snapshot, const lit::SnapshotNode& node)
{
  auto result = std::vector<vm::vec3d>{};
  for (auto f = node.elements.first; f < node.elements.first + node.elements.count; ++f)
  {
    const auto& polygon = snapshot.faces[f].polygon;
    for (auto v = polygon.first; v < polygon.first + polygon.count; ++v)
    {
      result.push_back(snapshot.vertices[v]);
    }
  }
  return result;
}

std::vector<vm::vec3d> verticesOf(const mdl::BrushNode& brushNode)
{
  auto result = std::vector<vm::vec3d>{};
  for (const auto& face : brushNode.brush().faces())
  {
    const auto polygon = face.polygon();
    result.insert(result.end(), polygon.vertices().begin(), polygon.vertices().end());
  }
  return result;
}

} // namespace

TEST_CASE("LitDocumentObserver")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(mdl::Doom3BFGFixtureConfig);
  auto& map = document.map();
  auto* layer = map.editorContext().currentLayer();
  REQUIRE(layer != nullptr);

  // a brush the document has before the session starts
  auto* brushA = addBrush(map);

  auto session = FakeSession{};
  auto observer = LitDocumentObserver{document, 7, session.target()};
  // every handler leaves no Node* behind: the map's keys are all live after each one
  observer.setCheckKeysForTesting(true);
  processEvents();
  REQUIRE(session.taken.size() == 1u);

  const auto worldId = observer.idForTesting(map.worldNode()).value_or(lit::NoPreviewId);
  const auto idA = observer.idForTesting(*brushA).value_or(lit::NoPreviewId);
  REQUIRE(worldId != lit::NoPreviewId);
  REQUIRE(idA != lit::NoPreviewId);

  SECTION("the first snapshot holds every node, later ones what changed")
  {
    const auto& first = session.last();
    CHECK(first.generation == 7u);
    CHECK(first.sequence == 1u);
    // a new map is not on disk
    CHECK_FALSE(first.mapPath);

    const auto* world = entityOf(first, worldId);
    REQUIRE(world != nullptr);
    CHECK(first.string(world->classname) == "worldspawn");
    CHECK_FALSE(nodeOf(first, worldId));

    const auto* brush = nodeOf(first, idA);
    REQUIRE(brush != nullptr);
    CHECK(brush->entity == worldId);
    CHECK(brush->kind == lit::SnapshotNodeKind::Brush);
    CHECK(brush->visible);
    CHECK_FALSE(brush->active);
    CHECK(verticesOf(first, *brush) == verticesOf(*brushA));
    CHECK(observer.dirtyCountForTesting() == 0u);
    CHECK(observer.trackedNodeCountForTesting() == nodeCount(map.worldNode()));

    // a change while the inbox is full waits for it
    auto* brushB = addBrush(map);
    processEvents();
    CHECK(session.taken.size() == 1u);
    session.take(observer);
    REQUIRE(session.taken.size() == 2u);

    const auto& second = session.last();
    CHECK(second.generation == 7u);
    CHECK(second.sequence == 2u);
    REQUIRE(second.nodes.size() == 1u);
    CHECK(second.nodes.front().id == observer.idForTesting(*brushB));
    CHECK(second.nodes.front().entity == worldId);
    CHECK(second.entities.empty());
    CHECK(second.removed.empty());
  }

  SECTION(
    "§6: a node at a freed and reused address gets a new id; no old record "
    "survives")
  {
    session.take(observer);
    auto* brushB = addBrush(map);
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    const auto oldId = observer.idForTesting(*brushB).value_or(lit::NoPreviewId);
    REQUIRE(nodeOf(session.last(), oldId) != nullptr);
    const auto* const address = static_cast<const void*>(brushB);

    // the add is undone, and the next command drops it from the redo stack: the node is
    // freed
    map.undoCommand();
    CHECK_FALSE(observer.trackedForTesting(oldId));
    CHECK(observer.changeForTesting(oldId) == LitChange::Removed);
    auto* brushC = addBrush(map);
    brushB = nullptr;

    // allocate nodes until one lands at the freed address; an empty brush costs only the
    // node and its render cache, and is kept apart from the document
    auto candidates = std::vector<std::unique_ptr<mdl::BrushNode>>{};
    auto* reused = static_cast<mdl::BrushNode*>(nullptr);
    for (auto i = 0; i < 100000 && !reused; ++i)
    {
      auto candidate = std::make_unique<mdl::BrushNode>(mdl::Brush{});
      if (static_cast<const void*>(candidate.get()) == address)
      {
        reused = candidate.release();
      }
      else
      {
        candidates.push_back(std::move(candidate));
      }
    }
    UNSCOPED_INFO("allocations until the address came back: " << candidates.size() + 1);
    candidates.clear();
    REQUIRE(reused != nullptr);
    // a real brush node in the same storage, which the document deletes as usual
    const auto brush = brushC->brush();
    reused->~BrushNode();
    reused = new (reused) mdl::BrushNode{brush};
    REQUIRE(static_cast<const void*>(reused) == address);

    mdl::addNodes(map, {{layer, {reused}}});
    const auto newId = observer.idForTesting(*reused).value_or(lit::NoPreviewId);
    CHECK(newId != lit::NoPreviewId);
    CHECK(newId != oldId);
    CHECK(newId > oldId);

    session.take(observer);
    REQUIRE(session.taken.size() == 3u);
    const auto& snapshot = session.last();
    CHECK(removes(snapshot, oldId));
    CHECK_FALSE(nodeOf(snapshot, oldId));
    const auto* added = nodeOf(snapshot, newId);
    REQUIRE(added != nullptr);
    CHECK(verticesOf(snapshot, *added) == verticesOf(*reused));
    CHECK_FALSE(observer.changeForTesting(oldId));
    CHECK_FALSE(observer.trackedForTesting(oldId));
    CHECK(observer.keysAreLiveForTesting());
  }

  SECTION("a removal undone puts the node back at its address with a new id")
  {
    mdl::removeNodes(map, {brushA});
    CHECK_FALSE(observer.idForTesting(*brushA));
    session.take(observer);
    REQUIRE(session.taken.size() == 2u);
    CHECK(removes(session.last(), idA));

    // the same object, at the same address
    map.undoCommand();
    const auto newId = observer.idForTesting(*brushA).value_or(lit::NoPreviewId);
    CHECK(newId != lit::NoPreviewId);
    CHECK(newId != idA);
    session.take(observer);
    REQUIRE(session.taken.size() == 3u);
    CHECK(nodeOf(session.last(), newId) != nullptr);
    CHECK_FALSE(removes(session.last(), newId));
    CHECK_FALSE(nodeOf(session.last(), idA));
  }

  SECTION("changes coalesce behind a hidden panel and are flushed when it is shown")
  {
    session.take(observer);
    session.shown = false;

    const auto boundsBefore = brushA->logicalBounds();
    mdl::selectNodes(map, {brushA});
    REQUIRE(mdl::translateSelection(map, vm::vec3d{16, 0, 0}));
    REQUIRE(mdl::translateSelection(map, vm::vec3d{16, 0, 0}));
    auto* brushB = addBrush(map);
    const auto idB = observer.idForTesting(*brushB).value_or(lit::NoPreviewId);
    // a ready while hidden offers nothing either
    observer.readyForSnapshot();
    processEvents();
    CHECK(session.taken.size() == 1u);

    // one entry each, with the bounds the brush had at the last snapshot
    CHECK(observer.changeForTesting(idA) == LitChange::Changed);
    CHECK(observer.oldBoundsForTesting(idA) == boundsBefore);
    CHECK(observer.changeForTesting(idB) == LitChange::Added);

    // panelShown
    session.shown = true;
    observer.requestFlush();
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    const auto& snapshot = session.last();
    CHECK(snapshot.nodes.size() == 2u);
    CHECK(std::ranges::count(snapshot.nodes, idA, &lit::SnapshotNode::id) == 1);
    const auto* moved = nodeOf(snapshot, idA);
    REQUIRE(moved != nullptr);
    CHECK(verticesOf(snapshot, *moved) == verticesOf(*brushA));
    CHECK(nodeOf(snapshot, idB) != nullptr);
    CHECK(observer.dirtyCountForTesting() == 0u);
  }

  SECTION("the active set follows a transaction and folds back when it ends (D26)")
  {
    const auto commit = GENERATE(true, false);
    CAPTURE(commit);

    session.take(observer);
    mdl::selectNodes(map, {brushA});
    const auto verticesBefore = verticesOf(*brushA);

    // a drag: one long-running transaction, a nested commit on every move
    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      map.startTransaction("move", mdl::TransactionScope::Oneshot);
      REQUIRE(mdl::translateSelection(map, vm::vec3d{8, 0, 0}));
      REQUIRE(map.commitTransaction());
      session.take(observer);
      processEvents();
      CHECK(observer.activeForTesting(idA));
      const auto* node = nodeOf(session.last(), idA);
      REQUIRE(node != nullptr);
      CHECK(node->active);
      CHECK(verticesOf(session.last(), *node) == verticesOf(*brushA));
    }
    CHECK(map.isTransactionActive());

    if (commit)
    {
      REQUIRE(map.commitTransaction());
    }
    else
    {
      map.cancelTransaction();
    }
    CHECK_FALSE(map.isTransactionActive());

    // the next flush polls the transaction, whatever was notified meanwhile
    const auto before = session.taken.size();
    session.take(observer);
    REQUIRE(session.taken.size() == before + 1);
    CHECK(observer.activeCountForTesting() == 0u);
    const auto* node = nodeOf(session.last(), idA);
    REQUIRE(node != nullptr);
    CHECK_FALSE(node->active);
    CHECK(verticesOf(session.last(), *node) == verticesOf(*brushA));
    if (!commit)
    {
      CHECK(verticesOf(*brushA) == verticesBefore);
    }

    // folded back once: nothing more to send
    session.take(observer);
    processEvents();
    CHECK(session.taken.size() == before + 1);
  }

  SECTION("visibility ignores the selection, and a show filter marks it dirty (D65, A8)")
  {
    session.take(observer);
    mdl::selectNodes(map, {brushA});

    auto& prefs = PreferenceManager::instance();
    const auto resetShowBrushes =
      kdl::invoke_later{[&] { prefs.resetToDefault(Preferences::ShowBrushes); }};
    setPref(Preferences::ShowBrushes, false);
    CHECK(observer.visibilityDirtyForTesting());
    processEvents();
    REQUIRE(session.taken.size() == 2u);

    // TrenchBroom shows the selected brush; the preview, as the game, does not
    CHECK(map.editorContext().visible(*brushA));
    const auto* hidden = nodeOf(session.last(), idA);
    REQUIRE(hidden != nullptr);
    CHECK_FALSE(hidden->visible);
    // worldspawn's record did not change
    CHECK(session.last().entities.empty());

    session.take(observer);
    setPref(Preferences::ShowBrushes, true);
    processEvents();
    REQUIRE(session.taken.size() == 3u);
    const auto* shown = nodeOf(session.last(), idA);
    REQUIRE(shown != nullptr);
    CHECK(shown->visible);
  }

  SECTION(
    "a hidden node is sent hidden; a filter change that hides nothing sends nothing")
  {
    session.take(observer);
    mdl::hideNodes(map, {brushA});
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    const auto* hidden = nodeOf(session.last(), idA);
    REQUIRE(hidden != nullptr);
    CHECK_FALSE(hidden->visible);
    CHECK(session.last().nodes.size() == 1u);

    session.take(observer);
    document.editorContextDidChangeNotifier();
    CHECK(observer.visibilityDirtyForTesting());
    processEvents();
    CHECK_FALSE(observer.visibilityDirtyForTesting());

    document.groupWasOpenedNotifier();
    CHECK(observer.visibilityDirtyForTesting());
    processEvents();
    document.groupWasClosedNotifier();
    CHECK(observer.visibilityDirtyForTesting());
    processEvents();
    document.entityDefinitionsDidChangeNotifier();
    CHECK(observer.visibilityDirtyForTesting());
    processEvents();
    CHECK(session.taken.size() == 2u);
  }

  SECTION("a change of the material collections reloads the service exactly once")
  {
    document.materialCollectionsDidChangeNotifier();
    CHECK(session.reloads == 1u);
    document.materialCollectionsDidChangeNotifier();
    CHECK(session.reloads == 2u);
  }

  SECTION("one snapshot in flight; one that was not taken is kept and offered first")
  {
    session.take(observer);
    session.refuse = true;
    auto* brushB = addBrush(map);
    processEvents();
    CHECK(session.taken.size() == 1u);
    CHECK(observer.hasPendingSnapshotForTesting());

    // later changes wait behind it
    auto* brushC = addBrush(map);
    processEvents();
    CHECK(observer.hasPendingSnapshotForTesting());

    session.refuse = false;
    observer.requestFlush();
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    CHECK_FALSE(observer.hasPendingSnapshotForTesting());
    CHECK(session.last().sequence == 2u);
    CHECK(nodeOf(session.last(), *observer.idForTesting(*brushB)) != nullptr);
    CHECK_FALSE(nodeOf(session.last(), *observer.idForTesting(*brushC)));

    // the inbox is full: nothing until it is taken
    observer.requestFlush();
    processEvents();
    CHECK(session.taken.size() == 2u);
    session.take(observer);
    REQUIRE(session.taken.size() == 3u);
    CHECK(session.last().sequence == 3u);
    CHECK(nodeOf(session.last(), *observer.idForTesting(*brushC)) != nullptr);
  }

  SECTION("a ReadyForSnapshot that comes while nothing is dirty is remembered (B0.7)")
  {
    session.take(observer);
    processEvents();
    CHECK(session.taken.size() == 1u);

    // no ready comes after the change: the free slot is enough
    auto* brushB = addBrush(map);
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    CHECK(nodeOf(session.last(), *observer.idForTesting(*brushB)) != nullptr);
  }

  SECTION("a document load drops everything (D41)")
  {
    addBrush(map);
    document.documentWasLoadedNotifier();
    CHECK(observer.trackedNodeCountForTesting() == 0u);
    CHECK(observer.dirtyCountForTesting() == 0u);
    CHECK_FALSE(observer.hasPendingSnapshotForTesting());

    session.take(observer);
    addBrush(map);
    processEvents();
    CHECK(session.taken.size() == 1u);
    CHECK(observer.trackedNodeCountForTesting() == 0u);
  }

  SECTION("an entity's keys alone, a light, and removed entity ids (B0.4)")
  {
    session.take(observer);
    auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "func_static"}}}};
    auto* childNode = mdl::createBrushNode(map);
    entityNode->addChild(childNode);
    auto* lightNode =
      new mdl::EntityNode{mdl::Entity{{{"classname", "light"}, {"origin", "0 0 64"}}}};
    mdl::addNodes(map, {{layer, {entityNode, lightNode}}});
    processEvents();
    REQUIRE(session.taken.size() == 2u);

    const auto entityId = observer.idForTesting(*entityNode).value_or(lit::NoPreviewId);
    const auto childId = observer.idForTesting(*childNode).value_or(lit::NoPreviewId);
    const auto lightId = observer.idForTesting(*lightNode).value_or(lit::NoPreviewId);
    {
      const auto& snapshot = session.last();
      const auto* entity = entityOf(snapshot, entityId);
      REQUIRE(entity != nullptr);
      CHECK(snapshot.string(entity->classname) == "func_static");
      // a brush entity has no node of its own
      CHECK_FALSE(nodeOf(snapshot, entityId));
      const auto* child = nodeOf(snapshot, childId);
      REQUIRE(child != nullptr);
      CHECK(child->entity == entityId);

      const auto* light = nodeOf(snapshot, lightId);
      REQUIRE(light != nullptr);
      CHECK(light->kind == lit::SnapshotNodeKind::Light);
      CHECK(light->entity == lightId);
      REQUIRE(light->elements.count == 1u);
      CHECK(snapshot.entities[light->elements.first].id == lightId);
    }

    // a key of the brush entity alone: its record without its brush
    session.take(observer);
    mdl::selectNodes(map, {childNode});
    REQUIRE(mdl::setEntityProperty(map, "lp1b", "1"));
    processEvents();
    REQUIRE(session.taken.size() == 3u);
    {
      const auto& snapshot = session.last();
      CHECK(snapshot.nodes.empty());
      REQUIRE(snapshot.entities.size() == 1u);
      CHECK(snapshot.entities.front().id == entityId);
      const auto keys = snapshot.entities.front().keys;
      CHECK(std::ranges::any_of(
        snapshot.keyValues.begin() + keys.first,
        snapshot.keyValues.begin() + keys.first + keys.count,
        [&](const auto& keyValue) {
          return snapshot.string(keyValue.key) == "lp1b"
                 && snapshot.string(keyValue.value) == "1";
        }));
    }

    // removed: the entity's id and its brush's
    session.take(observer);
    mdl::deselectAll(map);
    mdl::removeNodes(map, {entityNode});
    processEvents();
    REQUIRE(session.taken.size() == 4u);
    CHECK(removes(session.last(), entityId));
    CHECK(removes(session.last(), childId));
    CHECK_FALSE(removes(session.last(), lightId));
  }

  SECTION("the map path: none for a new map, then the path a save gave it")
  {
    session.take(observer);
    auto env = fs::TestEnvironment{};
    REQUIRE(map.saveAs(env.dir() / "lp1b.map") | kdl::is_success());
    processEvents();
    REQUIRE(session.taken.size() == 2u);
    CHECK(session.last().mapPath == map.path().string());
    CHECK(session.last().nodes.empty());

    // saved again to the same path: nothing to send
    session.take(observer);
    REQUIRE(map.save() | kdl::is_success());
    processEvents();
    CHECK(session.taken.size() == 2u);
  }

  SECTION("groups, removals, undo and redo leave no Node* behind (D39)")
  {
    auto* brushB = addBrush(map);
    auto* brushC = addBrush(map);
    mdl::selectNodes(map, {brushB, brushC});
    auto* group = mdl::groupSelectedNodes(map, "lp1b");
    REQUIRE(group != nullptr);
    mdl::deselectAll(map);
    mdl::selectNodes(map, {group});
    mdl::ungroupSelectedNodes(map);
    map.undoCommand();
    map.undoCommand();
    map.redoCommand();
    mdl::selectAllNodes(map);
    mdl::removeSelectedNodes(map);
    map.undoCommand();
    processEvents();

    CHECK(observer.keysAreLiveForTesting());
    CHECK(observer.trackedNodeCountForTesting() == nodeCount(map.worldNode()));
    session.take(observer);
    REQUIRE(session.taken.size() == 2u);
    for (const auto& node : session.last().nodes)
    {
      CHECK(observer.trackedForTesting(node.id));
    }
  }

  CHECK(observer.keyCheckFailuresForTesting() == 0u);
}

} // namespace tb::ui
