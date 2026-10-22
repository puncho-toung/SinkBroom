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

#pragma once

#include <QObject>

#include "base/Macros.h"
#include "base/NotifierConnection.h"
#include "lit/Snapshot.h"

#include "vm/bbox.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{
class Node;
}

namespace tb::ui
{
class LitSnapshotBuilder;
class MapDocument;

/**
 * Where a document observer's snapshots go: the session host's session, or a test's
 * stand-in. Called on the GUI thread only.
 */
struct LitSnapshotTarget
{
  // the preview can be seen (its window is exposed); snapshots are made only then
  std::function<bool()> shown;
  // LitSession::snapshotSlotFree (B0.7): the session's inbox is empty
  std::function<bool()> slotFree;
  // LitSession::offerSnapshot (GUI thread): whether the session took the snapshot
  std::function<bool(std::shared_ptr<const lit::Snapshot>)> offer;
  // MaterialService::reload, which only queues a request for the parse thread (any
  // thread)
  std::function<void()> reloadMaterials;
};

// what happened to a node since the last snapshot (S31)
enum class LitChange
{
  // new to the preview: always sent
  Added,
  // its contents changed: always sent
  Changed,
  // its visibility may have changed: sent when its visibility, kind or active flag
  // differs from what was sent last
  Visibility,
  // gone from the document: its id goes into removed, if it was ever sent
  Removed,
};

/**
 * The lit preview's view of the document (§3.3, D39, S31, D26, D65), one per session: the
 * session host makes it at Start once phase A has run and destroys it in its teardown
 * once the render thread has stopped, before the material service (LitSessionHost). It
 * subscribes on construction and drops every subscription on destruction.
 *
 * Preview ids (S5, R5.9): every node of the document gets a number the first time the
 * preview sees it, in a Node* → id map kept by the five node handlers. nodesWillBeRemoved
 * purges a node and its descendants from the dirty set, erases their keys and retires
 * their ids; nodesWereAdded always mints a new id, so a node at a reused address, or a
 * removal undone, never inherits an old record. No Node* survives a notification: the
 * map's keys are never dereferenced, and a snapshot finds its nodes by walking the
 * document.
 *
 * The dirty set (S31) coalesces (id, LitChange) with the bounds a node had when it first
 * changed since the last snapshot (nodesWillChange, nodesWillBeRemoved; the render thread
 * keeps the old records, so the snapshot does not carry them). The editor context, the
 * show filters (preferences, A8), an opened or closed group and new entity definitions
 * mark every node's visibility dirty with one flag, resolved when the snapshot is built.
 *
 * Snapshots are pulled (S31, B0.3, B0.7): a change asks for a flush from the event loop,
 * as do ReadyForSnapshot, panelShown and an expose of the window. A flush builds a
 * snapshot of the dirty nodes only when the preview can be seen and the session's inbox
 * is empty, offers it, and keeps it until it is taken; at most one is in flight. The
 * first snapshot of a session holds every node, and the map path if the map is on disk.
 *
 * The active set (D26, S4): a node added or changed while Map::isTransactionActive() is
 * true joins it; each flush polls isTransactionActive() and, once it is false, folds the
 * set back (every member changed once more), whatever notification fired. Members are
 * sent with SnapshotNode::active.
 *
 * Threads: the GUI thread only. The target's offer (LitSession::offerSnapshot) is a GUI
 * thread call; reloadMaterials (MaterialService::reload) may be called from any thread.
 */
class LitDocumentObserver
{
private:
  struct DirtyEntry
  {
    LitChange change = LitChange::Changed;
    std::optional<vm::bbox3d> oldBounds;
  };

  // what the last snapshot that held a node said of it
  struct SentState
  {
    // whether it had a node record (worldspawn and brush entities have only an entity
    // record)
    bool node = false;
    lit::SnapshotNodeKind kind = lit::SnapshotNodeKind::Brush;
    bool visible = true;
    bool active = false;

    bool operator==(const SentState&) const = default;
  };

  MapDocument& m_document;
  std::uint64_t m_generation;
  LitSnapshotTarget m_target;

  lit::PreviewId m_nextId = 1;
  std::uint64_t m_sequence = 0;
  // keys are never dereferenced
  std::unordered_map<const mdl::Node*, lit::PreviewId> m_ids;
  std::unordered_map<lit::PreviewId, DirtyEntry> m_dirty;
  std::unordered_map<lit::PreviewId, SentState> m_sent;
  std::unordered_set<lit::PreviewId> m_active;
  bool m_visibilityDirty = false;
  // the map path the last snapshot carried, and one to send
  std::optional<std::string> m_sentMapPath;
  std::optional<std::string> m_mapPathToSend;

  // a snapshot the session did not take yet
  std::shared_ptr<const lit::Snapshot> m_pending;
  // inside a nodesWillChange/nodesDidChange or nodesWillBeRemoved/nodesWereRemoved pair
  int m_changing = 0;
  bool m_flushRequested = false;
  // a document was loaded: the session ends (D41), and nothing is offered any more
  bool m_dropped = false;

  bool m_checkKeys = false;
  std::size_t m_keyCheckFailures = 0;

  // the context of the queued flushes, which go with it
  QObject m_flushContext;
  // last, so that the subscriptions go first
  NotifierConnection m_notifierConnection;

public:
  LitDocumentObserver(
    MapDocument& document, std::uint64_t generation, LitSnapshotTarget target);
  ~LitDocumentObserver();

  // the render thread's inbox is empty (SessionEvent::ReadyForSnapshot, B0.3)
  void readyForSnapshot();
  // a flush from the event loop, at most one queued: after a change, on panelShown and
  // when the window is exposed
  void requestFlush();
  /**
   * Offers the snapshot the session did not take yet, or builds one of the dirty set and
   * offers it, when the preview can be seen and the session's inbox is empty. Does
   * nothing inside a pair of node notifications; the second one asks for a flush.
   */
  void flush();

  // Test hooks.

  std::optional<lit::PreviewId> idForTesting(const mdl::Node& node) const;
  std::size_t trackedNodeCountForTesting() const;
  bool trackedForTesting(lit::PreviewId id) const;
  std::optional<LitChange> changeForTesting(lit::PreviewId id) const;
  std::optional<vm::bbox3d> oldBoundsForTesting(lit::PreviewId id) const;
  std::size_t dirtyCountForTesting() const;
  bool activeForTesting(lit::PreviewId id) const;
  std::size_t activeCountForTesting() const;
  bool visibilityDirtyForTesting() const;
  bool hasPendingSnapshotForTesting() const;
  // every key of the Node* → id map is a node of the document
  bool keysAreLiveForTesting() const;
  // checks the keys after every notification, counting (and in Debug asserting) failures
  void setCheckKeysForTesting(bool checkKeys);
  std::size_t keyCheckFailuresForTesting() const;

private:
  void connectObservers();
  void afterNotification();

  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWillChange(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodeVisibilityDidChange(const std::vector<mdl::Node*>& nodes);
  void visibilityContextDidChange();
  void preferenceDidChange(const std::filesystem::path& path);
  void transactionDidEnd(bool observable);
  void documentWasSaved();
  void documentWasLoaded();
  void materialCollectionsDidChange();

  lit::PreviewId mint(const mdl::Node& node);
  std::optional<lit::PreviewId> find(const mdl::Node& node) const;
  void mark(lit::PreviewId id, LitChange change);
  void retire(lit::PreviewId id, const std::optional<vm::bbox3d>& bounds);
  void markParentEntity(const mdl::Node& node);
  bool transactionActive() const;
  void foldBackActiveSet();
  std::optional<std::string> mapPath() const;

  bool hasChanges() const;
  std::shared_ptr<const lit::Snapshot> build();
  void walk(const mdl::Node& node, lit::PreviewId entity, LitSnapshotBuilder& builder);
  bool needsRecord(lit::PreviewId id, const SentState& current) const;

  deleteCopyAndMove(LitDocumentObserver);
};

} // namespace tb::ui
