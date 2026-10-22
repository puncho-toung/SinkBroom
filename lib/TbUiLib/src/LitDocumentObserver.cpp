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

#include "ui/LitDocumentObserver.h"

#include <QMetaObject>
#include <QtLogging>

#include "base/PreferenceManager.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/LitSnapshotBuilder.h"
#include "ui/MapDocument.h"

#include "kd/overload.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

namespace tb::ui
{
namespace
{

template <typename F>
void visitSubtree(const mdl::Node& node, const F& f)
{
  f(node);
  for (const auto* child : node.children())
  {
    visitSubtree(*child, f);
  }
}

LitChange merge(const LitChange existing, const LitChange change)
{
  if (existing == LitChange::Removed || change == LitChange::Removed)
  {
    return LitChange::Removed;
  }
  if (existing == LitChange::Added || change == LitChange::Added)
  {
    return LitChange::Added;
  }
  if (existing == LitChange::Changed || change == LitChange::Changed)
  {
    return LitChange::Changed;
  }
  return LitChange::Visibility;
}

const mdl::EntityNode* asEntityNode(const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) -> const mdl::EntityNode* { return nullptr; },
    [](const mdl::LayerNode&) -> const mdl::EntityNode* { return nullptr; },
    [](const mdl::GroupNode&) -> const mdl::EntityNode* { return nullptr; },
    [](const mdl::EntityNode& entityNode) -> const mdl::EntityNode* {
      return &entityNode;
    },
    [](const mdl::BrushNode&) -> const mdl::EntityNode* { return nullptr; },
    [](const mdl::PatchNode&) -> const mdl::EntityNode* { return nullptr; }));
}

} // namespace

LitDocumentObserver::LitDocumentObserver(
  MapDocument& document, const std::uint64_t generation, LitSnapshotTarget target)
  : m_document{document}
  , m_generation{generation}
  , m_target{std::move(target)}
{
  // B0.4: the session's first snapshot is complete
  visitSubtree(m_document.map().worldNode(), [&](const mdl::Node& node) {
    mark(mint(node), LitChange::Added);
  });
  m_mapPathToSend = mapPath();

  connectObservers();
  requestFlush();
}

LitDocumentObserver::~LitDocumentObserver() = default;

void LitDocumentObserver::connectObservers()
{
  using Nodes = std::vector<mdl::Node*>;
  auto& document = m_document;

  // the five node handlers (R5.24); each leaves no Node* behind but the map's keys
  m_notifierConnection +=
    document.nodesWereAddedNotifier.connect([&](const Nodes& nodes) {
      nodesWereAdded(nodes);
      afterNotification();
    });
  m_notifierConnection +=
    document.nodesWillChangeNotifier.connect([&](const Nodes& nodes) {
      nodesWillChange(nodes);
      afterNotification();
    });
  m_notifierConnection +=
    document.nodesDidChangeNotifier.connect([&](const Nodes& nodes) {
      nodesDidChange(nodes);
      afterNotification();
    });
  m_notifierConnection +=
    document.nodesWillBeRemovedNotifier.connect([&](const Nodes& nodes) {
      nodesWillBeRemoved(nodes);
      afterNotification();
    });
  m_notifierConnection +=
    document.nodesWereRemovedNotifier.connect([&](const Nodes& nodes) {
      nodesWereRemoved(nodes);
      afterNotification();
    });
  m_notifierConnection +=
    document.nodeVisibilityDidChangeNotifier.connect([&](const Nodes& nodes) {
      nodeVisibilityDidChange(nodes);
      afterNotification();
    });

  // filters (D30): every node's visibility is looked at again in the next snapshot
  m_notifierConnection += document.editorContextDidChangeNotifier.connect(
    [&]() { visibilityContextDidChange(); });
  m_notifierConnection +=
    document.groupWasOpenedNotifier.connect([&]() { visibilityContextDidChange(); });
  m_notifierConnection +=
    document.groupWasClosedNotifier.connect([&]() { visibilityContextDidChange(); });
  m_notifierConnection += document.entityDefinitionsDidChangeNotifier.connect(
    [&]() { visibilityContextDidChange(); });
  // A8: the show filters fire no editorContextDidChange
  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const std::filesystem::path& path) { preferenceDidChange(path); });

  // S4, D26: an observable transaction notification only makes the next flush poll
  // isTransactionActive(); the notifications themselves never end the active set
  m_notifierConnection += document.transactionDoneNotifier.connect(
    [&](const std::string&, const bool observable, bool) {
      transactionDidEnd(observable);
    });
  m_notifierConnection += document.transactionUndoneNotifier.connect(
    [&](const std::string&, const bool observable, bool) {
      transactionDidEnd(observable);
    });

  m_notifierConnection +=
    document.documentWasSavedNotifier.connect([&]() { documentWasSaved(); });
  m_notifierConnection +=
    document.documentWasLoadedNotifier.connect([&]() { documentWasLoaded(); });
  // R5.24: moved here from the panel
  m_notifierConnection += document.materialCollectionsDidChangeNotifier.connect(
    [&]() { materialCollectionsDidChange(); });
}

void LitDocumentObserver::afterNotification()
{
  if (m_checkKeys)
  {
    const auto live = keysAreLiveForTesting();
    assert(live);
    if (!live)
    {
      ++m_keyCheckFailures;
    }
  }
}

void LitDocumentObserver::readyForSnapshot()
{
  flush();
}

void LitDocumentObserver::requestFlush()
{
  if (m_flushRequested || m_dropped)
  {
    return;
  }

  m_flushRequested = true;
  QMetaObject::invokeMethod(
    &m_flushContext,
    [this] {
      m_flushRequested = false;
      // in Qt's event loop, which an exception must not reach
      try
      {
        flush();
      }
      catch (const std::exception& e)
      {
        qWarning("Lit preview: a snapshot could not be made: %s", e.what());
      }
    },
    Qt::QueuedConnection);
}

void LitDocumentObserver::flush()
{
  // never between the two notifications of a change, whose nodes are half done
  if (m_dropped || m_changing > 0 || !m_target.shown() || !m_target.slotFree())
  {
    return;
  }

  // S31: one in flight; a snapshot that was not taken goes first
  if (m_pending)
  {
    if (m_target.offer(m_pending))
    {
      m_pending.reset();
    }
    return;
  }

  foldBackActiveSet();
  if (!hasChanges())
  {
    return;
  }

  if (auto snapshot = build(); snapshot && !m_target.offer(snapshot))
  {
    m_pending = std::move(snapshot);
  }
}

void LitDocumentObserver::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_dropped)
  {
    return;
  }

  const auto active = transactionActive();
  for (const auto* node : nodes)
  {
    visitSubtree(*node, [&](const mdl::Node& added) {
      // always a new id, also at an address that is still a key (R5.9)
      if (const auto old = find(added))
      {
        retire(*old, std::nullopt);
      }
      const auto id = mint(added);
      mark(id, LitChange::Added);
      if (active)
      {
        m_active.insert(id);
      }
    });
    // a point entity that gets a brush becomes a brush entity
    markParentEntity(*node);
  }
  requestFlush();
}

void LitDocumentObserver::nodesWillChange(const std::vector<mdl::Node*>& nodes)
{
  ++m_changing;
  if (m_dropped)
  {
    return;
  }

  for (const auto* node : nodes)
  {
    if (const auto id = find(*node))
    {
      mark(*id, LitChange::Changed);
      // the bounds it had at the last snapshot
      auto& entry = m_dirty.at(*id);
      if (!entry.oldBounds)
      {
        entry.oldBounds = node->logicalBounds();
      }
    }
  }
}

void LitDocumentObserver::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  m_changing = std::max(m_changing - 1, 0);
  if (m_dropped)
  {
    return;
  }

  const auto active = transactionActive();
  for (const auto* node : nodes)
  {
    const auto id = find(*node).value_or(lit::NoPreviewId);
    if (id == lit::NoPreviewId)
    {
      // not seen before: new to the preview
      mark(mint(*node), LitChange::Added);
      continue;
    }

    mark(id, LitChange::Changed);
    if (active)
    {
      m_active.insert(id);
    }

    // a changed classname may hide or show the entity's brushes (a hidden definition)
    if (const auto* entityNode = asEntityNode(*node))
    {
      for (const auto* child : entityNode->children())
      {
        if (const auto childId = find(*child))
        {
          mark(*childId, LitChange::Visibility);
        }
      }
    }
  }
  requestFlush();
}

void LitDocumentObserver::nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes)
{
  ++m_changing;
  if (m_dropped)
  {
    return;
  }

  for (const auto* node : nodes)
  {
    // a brush entity that loses its last brush becomes a point entity
    markParentEntity(*node);
    visitSubtree(*node, [&](const mdl::Node& removed) {
      if (const auto it = m_ids.find(&removed); it != m_ids.end())
      {
        const auto id = it->second;
        m_ids.erase(it);
        retire(id, removed.logicalBounds());
      }
    });
  }
}

void LitDocumentObserver::nodesWereRemoved(const std::vector<mdl::Node*>&)
{
  m_changing = std::max(m_changing - 1, 0);
  requestFlush();
}

void LitDocumentObserver::nodeVisibilityDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_dropped)
  {
    return;
  }

  // a hidden layer or group hides what it holds
  for (const auto* node : nodes)
  {
    visitSubtree(*node, [&](const mdl::Node& changed) {
      if (const auto id = find(changed))
      {
        mark(*id, LitChange::Visibility);
      }
    });
  }
  requestFlush();
}

void LitDocumentObserver::visibilityContextDidChange()
{
  // a flag, resolved when the next snapshot is built
  m_visibilityDirty = true;
  requestFlush();
}

void LitDocumentObserver::preferenceDidChange(const std::filesystem::path& path)
{
  // MapDocument applies them to the editor context first; it observes the preferences
  // since before the session started
  if (
    path == Preferences::ShowPointEntities.path || path == Preferences::ShowBrushes.path
    || path == Preferences::ShowPatches.path)
  {
    visibilityContextDidChange();
  }
}

void LitDocumentObserver::transactionDidEnd(const bool observable)
{
  if (observable)
  {
    requestFlush();
  }
}

void LitDocumentObserver::documentWasSaved()
{
  // the .proc beside the map is looked for there (LP1b.2)
  if (const auto path = mapPath(); path && path != m_sentMapPath)
  {
    m_mapPathToSend = path;
    requestFlush();
  }
}

void LitDocumentObserver::documentWasLoaded()
{
  // D41: the session ends with the old document; nothing of it is kept or offered
  m_ids.clear();
  m_dirty.clear();
  m_sent.clear();
  m_active.clear();
  m_visibilityDirty = false;
  m_mapPathToSend.reset();
  m_pending.reset();
  m_dropped = true;
}

void LitDocumentObserver::materialCollectionsDidChange()
{
  // the material decls may have changed with the collections: the parse thread builds its
  // registry again and republishes every name it was asked for
  m_target.reloadMaterials();
}

lit::PreviewId LitDocumentObserver::mint(const mdl::Node& node)
{
  const auto id = m_nextId++;
  m_ids.insert_or_assign(&node, id);
  return id;
}

std::optional<lit::PreviewId> LitDocumentObserver::find(const mdl::Node& node) const
{
  const auto it = m_ids.find(&node);
  return it != m_ids.end() ? std::optional{it->second} : std::nullopt;
}

void LitDocumentObserver::mark(const lit::PreviewId id, const LitChange change)
{
  const auto [it, inserted] = m_dirty.try_emplace(id, DirtyEntry{change, std::nullopt});
  if (!inserted)
  {
    it->second.change = merge(it->second.change, change);
  }
}

void LitDocumentObserver::retire(
  const lit::PreviewId id, const std::optional<vm::bbox3d>& bounds)
{
  m_active.erase(id);
  if (!m_sent.contains(id))
  {
    // never sent: the render thread has no record of it
    m_dirty.erase(id);
    return;
  }

  auto& entry = m_dirty[id];
  entry.change = LitChange::Removed;
  if (!entry.oldBounds)
  {
    entry.oldBounds = bounds;
  }
}

void LitDocumentObserver::markParentEntity(const mdl::Node& node)
{
  if (const auto* parent = node.parent(); parent && asEntityNode(*parent))
  {
    if (const auto id = find(*parent))
    {
      mark(*id, LitChange::Changed);
    }
  }
}

bool LitDocumentObserver::transactionActive() const
{
  return m_document.map().isTransactionActive();
}

void LitDocumentObserver::foldBackActiveSet()
{
  // D26: polled, whatever notification ended the transaction (a cancel fires none after
  // it)
  if (m_active.empty() || transactionActive())
  {
    return;
  }
  for (const auto id : m_active)
  {
    mark(id, LitChange::Changed);
  }
  m_active.clear();
}

std::optional<std::string> LitDocumentObserver::mapPath() const
{
  const auto& map = m_document.map();
  if (!map.persistent())
  {
    return std::nullopt;
  }
  // the native narrow string, which the render thread's std::filesystem::path reads back;
  // a path that the code page cannot hold is not sent
  try
  {
    return map.path().string();
  }
  catch (const std::exception&)
  {
    return std::nullopt;
  }
}

bool LitDocumentObserver::hasChanges() const
{
  return !m_dirty.empty() || m_visibilityDirty || m_mapPathToSend.has_value();
}

std::shared_ptr<const lit::Snapshot> LitDocumentObserver::build()
{
  auto builder = LitSnapshotBuilder{m_generation, m_sequence + 1};

  // B0.4: node and entity ids alike
  auto removed = std::vector<lit::PreviewId>{};
  auto walkNeeded = m_visibilityDirty;
  for (const auto& [id, entry] : m_dirty)
  {
    if (entry.change == LitChange::Removed)
    {
      if (m_sent.erase(id) > 0)
      {
        removed.push_back(id);
      }
    }
    else
    {
      walkNeeded = true;
    }
  }
  std::ranges::sort(removed);
  for (const auto id : removed)
  {
    builder.addRemoved(id);
  }

  // the dirty nodes are found by walking the document: no Node* is kept to find them by
  if (walkNeeded)
  {
    walk(m_document.map().worldNode(), lit::NoPreviewId, builder);
  }

  if (m_mapPathToSend)
  {
    builder.setMapPath(*m_mapPathToSend);
    m_sentMapPath = std::exchange(m_mapPathToSend, std::nullopt);
  }

  m_dirty.clear();
  m_visibilityDirty = false;

  if (builder.empty())
  {
    return nullptr;
  }
  ++m_sequence;
  return std::make_shared<const lit::Snapshot>(builder.take());
}

void LitDocumentObserver::walk(
  const mdl::Node& node, const lit::PreviewId entity, LitSnapshotBuilder& builder)
{
  auto id = find(node).value_or(lit::NoPreviewId);
  if (id == lit::NoPreviewId)
  {
    // a node no notification reported
    id = mint(node);
    mark(id, LitChange::Added);
  }

  const auto candidate = m_visibilityDirty || m_dirty.contains(id);
  const auto& context = m_document.map().editorContext();
  const auto active = m_active.contains(id);

  // an entity record, with a node for a point entity; a point entity that became a brush
  // entity loses its node
  const auto addEntity = [&](const mdl::Entity& e, const SentState& current) {
    if (!needsRecord(id, current))
    {
      return;
    }
    if (const auto sent = m_sent.find(id);
        sent != m_sent.end() && sent->second.node && !current.node)
    {
      builder.addRemoved(id);
    }
    if (current.node)
    {
      builder.addPointEntity(id, current.kind, current.visible, current.active, e);
    }
    else
    {
      builder.addEntity(id, e);
    }
    m_sent[id] = current;
  };

  auto childEntity = entity;
  node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      childEntity = id;
      if (candidate)
      {
        addEntity(worldNode.entity(), SentState{});
      }
    },
    [](const mdl::LayerNode&) {},
    [](const mdl::GroupNode&) {},
    [&](const mdl::EntityNode& entityNode) {
      childEntity = id;
      if (candidate)
      {
        const auto& e = entityNode.entity();
        addEntity(
          e,
          e.pointEntity()
            ? SentState{true, pointEntityKind(e), visibleInPreview(context, entityNode), active}
            : SentState{});
      }
    },
    [&](const mdl::BrushNode& brushNode) {
      if (candidate)
      {
        const auto current = SentState{
          true,
          lit::SnapshotNodeKind::Brush,
          visibleInPreview(context, brushNode),
          active};
        if (needsRecord(id, current))
        {
          builder.addBrush(id, entity, current.visible, current.active, brushNode);
          m_sent[id] = current;
        }
      }
    },
    [&](const mdl::PatchNode& patchNode) {
      if (candidate)
      {
        const auto current = SentState{
          true,
          lit::SnapshotNodeKind::Patch,
          visibleInPreview(context, patchNode),
          active};
        if (needsRecord(id, current))
        {
          builder.addPatch(id, entity, current.visible, current.active, patchNode);
          m_sent[id] = current;
        }
      }
    }));

  for (const auto* child : node.children())
  {
    walk(*child, childEntity, builder);
  }
}

bool LitDocumentObserver::needsRecord(
  const lit::PreviewId id, const SentState& current) const
{
  if (
    const auto dirty = m_dirty.find(id);
    dirty != m_dirty.end()
    && (dirty->second.change == LitChange::Added || dirty->second.change == LitChange::Changed))
  {
    return true;
  }
  // a visibility change only: sent when something the render thread has changed
  const auto sent = m_sent.find(id);
  return sent == m_sent.end() || sent->second != current;
}

std::optional<lit::PreviewId> LitDocumentObserver::idForTesting(
  const mdl::Node& node) const
{
  return find(node);
}

std::size_t LitDocumentObserver::trackedNodeCountForTesting() const
{
  return m_ids.size();
}

bool LitDocumentObserver::trackedForTesting(const lit::PreviewId id) const
{
  return std::ranges::any_of(
    m_ids, [&](const auto& entry) { return entry.second == id; });
}

std::optional<LitChange> LitDocumentObserver::changeForTesting(
  const lit::PreviewId id) const
{
  const auto it = m_dirty.find(id);
  return it != m_dirty.end() ? std::optional{it->second.change} : std::nullopt;
}

std::optional<vm::bbox3d> LitDocumentObserver::oldBoundsForTesting(
  const lit::PreviewId id) const
{
  const auto it = m_dirty.find(id);
  return it != m_dirty.end() ? it->second.oldBounds : std::nullopt;
}

std::size_t LitDocumentObserver::dirtyCountForTesting() const
{
  return m_dirty.size();
}

bool LitDocumentObserver::activeForTesting(const lit::PreviewId id) const
{
  return m_active.contains(id);
}

std::size_t LitDocumentObserver::activeCountForTesting() const
{
  return m_active.size();
}

bool LitDocumentObserver::visibilityDirtyForTesting() const
{
  return m_visibilityDirty;
}

bool LitDocumentObserver::hasPendingSnapshotForTesting() const
{
  return m_pending != nullptr;
}

bool LitDocumentObserver::keysAreLiveForTesting() const
{
  // the document's nodes, reached from the world; the keys themselves are not followed
  auto live = std::unordered_set<const mdl::Node*>{};
  visitSubtree(
    m_document.map().worldNode(), [&](const mdl::Node& node) { live.insert(&node); });
  return std::ranges::all_of(
    m_ids, [&](const auto& entry) { return live.contains(entry.first); });
}

void LitDocumentObserver::setCheckKeysForTesting(const bool checkKeys)
{
  m_checkKeys = checkKeys;
}

std::size_t LitDocumentObserver::keyCheckFailuresForTesting() const
{
  return m_keyCheckFailures;
}

} // namespace tb::ui
