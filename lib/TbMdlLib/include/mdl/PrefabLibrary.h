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

#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "mdl/PrefabFile.h"

#include "vm/bbox.h"

#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
class WorldNode;

struct PrefabEntry
{
  PrefabRoot root = PrefabRoot::User;
  // forward slashes, includes ".map", unique per root
  std::filesystem::path relativePath;
  std::filesystem::path absolutePath;
  // <stem>.png beside it, if present
  std::optional<std::filesystem::path> thumbnailPath;
  PrefabMetadata metadata;
  // fnv1a64Hex of the file bytes
  std::string hash;
  // parsed body; definitions assigned; never mutated
  std::unique_ptr<WorldNode> world;
  // set, with world == nullptr, when the file failed
  std::optional<std::string> error;

  PrefabEntry();
  ~PrefabEntry();
  PrefabEntry(PrefabEntry&&) noexcept;
  PrefabEntry& operator=(PrefabEntry&&) noexcept;
};

/**
 * The prefab files found under the library roots: the per-user root (the game's Prefab
 * Path preference, given by setUserRoot) and <game path>/<mod>/prefabs/. Subfolders are
 * categories. The folders are the registry; nothing else is stored.
 *
 * Files are read and parsed on task manager workers; processPendingResults, called on the
 * GUI thread, takes the results over.
 */
class PrefabLibrary
{
private:
  Map& m_map;
  std::filesystem::path m_userRoot;

  std::vector<PrefabEntry> m_entries;
  std::vector<std::future<PrefabEntry>> m_pendingEntries;
  bool m_scanning = false;
  bool m_definitionsStale = false;

  NotifierConnection m_notifierConnection;

public:
  Notifier<> libraryDidChangeNotifier;

  explicit PrefabLibrary(Map& map);
  ~PrefabLibrary();

  /**
   * The per-user root, the value of the game's Prefab Path preference. The library does
   * not read preferences (the map does not either; its game path is given to it the same
   * way). An empty path means that the user has not set one.
   */
  const std::filesystem::path& userRoot() const;
  void setUserRoot(std::filesystem::path userRoot);

  // C1 order: User (if set), Mod (if the game path is set)
  std::vector<std::filesystem::path> roots() const;
  // where a new file goes; empty if that root is not available
  std::filesystem::path writeRootFor(PrefabRoot root) const;

  /**
   * Lists the *.map files under the roots and starts reading and parsing them on task
   * manager workers. A rescan while one runs abandons the running one.
   */
  void rescan();

  /**
   * Call on the GUI thread. Once every file of the current scan is read, replaces the
   * entries with the scan's results, assigns entity definitions, reads the metadata and
   * fires libraryDidChangeNotifier once. Also reassigns entity definitions after the
   * map's definitions were reloaded.
   */
  void processPendingResults();

  bool scanning() const;

  // sorted by root, then relativePath
  const std::vector<PrefabEntry>& entries() const;
  const PrefabEntry* find(
    PrefabRoot root, const std::filesystem::path& relativePath) const;

  // C4: "prefab:user/<rel>" | "prefab:mod/<rel>"
  std::string payloadFor(const PrefabEntry& entry) const;
  // nullptr if not ours
  const PrefabEntry* fromPayload(const std::string& payload) const;

  /**
   * Returns caller-owned clones of the body (the one entity, or the one top-level group),
   * link ids re-keyed (rekeyLinkIds), definitions kept; empty when entry.world is null.
   *
   * The clones are not prepared for adding to a map; see prepareNodesForAdding.
   */
  std::vector<Node*> cloneBody(
    const PrefabEntry& entry, const vm::bbox3d& worldBounds) const;

private:
  void connectObservers();
  void entityDefinitionsWillChange();
  void entityDefinitionsDidChange();

  void setEntityDefinitions(PrefabEntry& entry) const;
};

} // namespace tb::mdl
