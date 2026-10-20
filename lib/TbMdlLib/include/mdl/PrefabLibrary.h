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

#include "base/Color.h"
#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "mdl/Entity.h"
#include "mdl/PrefabFile.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb::mdl
{
class GroupNode;
class Map;
class Node;
class WorldNode;

// A body with more brushes and patches than this keeps no world in the library; see
// PrefabEntry::bodyPreview.
constexpr auto PrefabWorldBrushBudget = std::size_t(256);
// The most edges that a PrefabBodyPreview keeps, as many as a browser cell draws.
constexpr auto PrefabPreviewMaxEdges = std::size_t(1200);
// The deepest folder below a root that a scan enters, against symbolic link and junction
// loops.
constexpr auto PrefabMaxFolderDepth = 64;

/**
 * What a browser cell needs of a body whose world the library does not keep.
 */
struct PrefabBodyPreview
{
  vm::bbox3d bounds;
  // pairs of points: the brush edges and patch grid lines, at most PrefabPreviewMaxEdges
  std::vector<vm::vec3f> edgeVertices;

  // the classname of the body's first entity, and its _color if it is a light, read
  // before the world was dropped; empty if the body has no entity
  std::string firstEntityClassname;
  std::optional<std::string> firstEntityLightColor;
  // the colour of the cell: the light's _color, else the first entity's definition
  // colour; set from the two above whenever the library assigns entity definitions,
  // nullopt if there is neither
  std::optional<RgbaF> color;
};

struct PrefabEntry
{
  PrefabRoot root = PrefabRoot::User;
  // unique per root, includes ".map"
  std::filesystem::path relativePath;
  std::filesystem::path absolutePath;
  // relativePath as UTF-8 with forward slashes, what payloads hold and entries sort by
  std::string relativePathString;
  // the key by which two relative paths name the same file: relativePathString, lower
  // case (ASCII only) on Windows, where file names ignore case
  std::string pathKey;
  // <stem>.png beside it, if present
  std::optional<std::filesystem::path> thumbnailPath;
  PrefabMetadata metadata;
  // fnv1a64HexNormalizedLineEndings of the file, what a placement records (C3)
  std::string hash;
  // the file's size and modification time when it was read; a rescan reuses an entry
  // whose file still has both, unless the file failed (error is set) for a reason other
  // than notAPrefab
  std::uintmax_t fileSize = 0;
  std::filesystem::file_time_type modificationTime;
  // the parsed body, with an empty node tree that is not updated; only the library
  // changes it: it assigns and unsets the entity definitions, and an updateEntry that it
  // takes over replaces the whole entry, this world included, in place at the same
  // address (R7-lib-3)
  std::unique_ptr<WorldNode> world;
  // set, with world == nullptr, for a body of more than PrefabWorldBrushBudget brushes
  // and patches: its world is dropped once this is built, and cloneBody reads the file
  // again (DD-19, R7-lib-2)
  std::optional<PrefabBodyPreview> bodyPreview;
  // set with bodyPreview for an entity snapshot whose world is dropped: a copy of the
  // snapshot's entity, which applyPrefabToSelection reads; its definition is assigned
  // and unset like the world's (R7-lib-2); see snapshot()
  std::optional<Entity> keptSnapshot;
  // set, with world == nullptr, when the file failed
  std::optional<std::string> error;
  // set with error when the file is a map but not a prefab (it has no or an unknown
  // _tb_prefab_kind): a failure of its contents, not a transient one, so that a rescan
  // reuses the entry while the file's size and modification time are unchanged
  // (R8-lib-5)
  bool notAPrefab = false;

  PrefabEntry();
  ~PrefabEntry();
  PrefabEntry(PrefabEntry&&) noexcept;
  PrefabEntry& operator=(PrefabEntry&&) noexcept;

  // cloneBody can clone the body: the world is kept, or the file is read again
  bool hasBody() const;

  /**
   * The entity of an entity snapshot: the one entity of the kept world, else
   * keptSnapshot; nullptr for an assembly, for a file that failed, or for a body that is
   * not an entity.
   */
  const Entity* snapshot() const;
};

/**
 * The placed assemblies of one prefab file that form one link set: the groups whose
 * Group::prefabPath is the same and that share a link ID. Two unlinked placements of
 * one file are two placements.
 *
 * The group pointers are valid until the map changes. The entry pointer is valid until
 * the next processPendingResults or rescan, which may remove the entry; an update that
 * processPendingResults takes over keeps the entry's address but replaces its contents
 * (R7-lib-3).
 */
struct PrefabPlacement
{
  // C3 as amended by DD-16: user/<rel> or mod/<rel>, as the members hold it
  std::string prefabPath;
  // the first member in the order of the world, the one that a refresh edits
  GroupNode* firstMember = nullptr;
  // every member of the link set that carries the prefab path, in the order of the world
  std::vector<GroupNode*> members;
  // the first member's prefabHash, empty if it has none
  std::string placedHash;
  // nullptr when the library has no such file; see PrefabLibrary::entryForPrefabPath
  const PrefabEntry* entry = nullptr;
};

/**
 * What finds a placement again after the map has changed: its prefab path and the link ID
 * of its members.
 */
using PrefabPlacementKey = std::pair<std::string, std::string>;

/**
 * The prefab files found under the library root: <game path>/<mod>/maps/prefabs/, or the
 * per-user root (the game's Prefab Path preference, given by setUserRoot) instead of it
 * when that is set (DD-32). Subfolders are categories. The folders are the registry;
 * nothing else is stored.
 *
 * A scan runs in two phases on the library's own task manager, so that it never queues
 * work in front of the map's: one task lists the roots (path, size, modification time),
 * then processPendingResults, called on the GUI thread, starts one parse task per file
 * that is new or changed, and takes the results over once all of them are done.
 *
 * Entries live in stable storage: an entry that a rescan reuses, or that updateEntry
 * reads again, keeps its address. A pointer to an entry is valid until the next
 * processPendingResults or rescan.
 */
class PrefabLibrary
{
public:
  // called on a worker with the absolute path of each file before it is read; for tests
  using ReadHook = std::function<void(const std::filesystem::path&)>;
  // called on the worker that lists the roots with the path of each directory entry that
  // the walk reaches; an error that it returns ends the walk of that root as an iterator
  // error would; for tests
  using ListHook = std::function<std::error_code(const std::filesystem::path&)>;

private:
  struct ListedFile
  {
    PrefabRoot root = PrefabRoot::User;
    std::filesystem::path absolutePath;
    std::filesystem::path relativePath;
    std::string relativePathString;
    std::string pathKey;
    std::uintmax_t fileSize = 0;
    std::filesystem::file_time_type modificationTime;
    std::optional<std::filesystem::path> thumbnailPath;
  };

  struct IncompleteRoot
  {
    PrefabRoot root = PrefabRoot::User;
    // as listed, lexically normal
    std::filesystem::path path;
  };

  struct Listing
  {
    std::vector<ListedFile> files;
    // every folder under the roots that exist, the roots included
    std::vector<std::filesystem::path> directories;
    // the roots whose walk an error ended early (R8-lib-2)
    std::vector<IncompleteRoot> incompleteRoots;
    std::vector<std::string> warnings;
  };

  struct ReusedEntry
  {
    PrefabEntry* entry = nullptr;
    std::optional<std::filesystem::path> thumbnailPath;
  };

  struct PendingUpdate
  {
    // the entry key of the file, see m_entriesByKey
    std::string key;
    // the file's number in m_updateSequences when the update was started
    std::uint64_t sequence = 0;
    std::future<PrefabEntry> future;
  };

  Map& m_map;
  std::filesystem::path m_userRoot;

  std::vector<std::unique_ptr<PrefabEntry>> m_entries;
  // (root, pathKey) as one string -> entry, for find and fromPayload
  std::unordered_map<std::string, PrefabEntry*> m_entriesByKey;
  // the directories of the last finished scan, sorted
  std::vector<std::filesystem::path> m_directories;

  // bumped by every rescan; a task of an older generation returns at once
  std::shared_ptr<std::atomic<std::uint64_t>> m_generation;
  std::optional<std::future<Listing>> m_pendingListing;
  std::vector<std::future<PrefabEntry>> m_pendingEntries;
  std::vector<ReusedEntry> m_reusedEntries;
  std::vector<std::filesystem::path> m_pendingDirectories;
  std::vector<IncompleteRoot> m_pendingIncompleteRoots;
  std::vector<PendingUpdate> m_pendingUpdates;
  // entry key -> the number of the latest update of that file; an update that finishes
  // after a later one of the same file is dropped
  std::unordered_map<std::string, std::uint64_t> m_updateSequences;
  std::uint64_t m_nextUpdateSequence = 0;
  bool m_scanning = false;
  bool m_scannedOnce = false;
  // the definitions were reloaded since the last processPendingResults, which reports it
  bool m_definitionsChanged = false;
  ReadHook m_readHook;
  ListHook m_listHook;

  NotifierConnection m_notifierConnection;

  // Declared after the futures, so it is destroyed first: its workers are joined before
  // the futures go away. Created on the first scan, so that a map that never scans
  // starts no threads.
  std::unique_ptr<kdl::task_manager> m_taskManager;

public:
  Notifier<> libraryDidChangeNotifier;

  explicit PrefabLibrary(Map& map);
  ~PrefabLibrary();

  /**
   * The per-user root, the value of the game's Prefab Path preference. The library does
   * not read preferences (the map does not either; its game path is given to it the same
   * way). An empty path means that the user has not set one.
   *
   * setUserRoot stores normalizeUserRoot(userRoot), so that a path that is not absolute
   * counts as unset (R8-lib-4).
   */
  const std::filesystem::path& userRoot() const;
  void setUserRoot(const std::filesystem::path& userRoot);

  /**
   * A Prefab Path as the library uses it: lexically normal, without trailing separators
   * (R8-ui-4); empty if it is not absolute, since it would resolve against the working
   * folder (R8-lib-4). A preference pane can flag a value that this makes empty.
   */
  static std::filesystem::path normalizeUserRoot(const std::filesystem::path& userRoot);

  /**
   * Called once by the map, after it has connected its own observers, so that the
   * observers connected here run after the map's: the library assigns reloaded entity
   * definitions to its entries as soon as the map has loaded them (R8-lib-7).
   */
  void connectObserversAfterMap();

  // DD-32: the user root if set, else the mod root if the game path is set; at most one
  std::vector<std::filesystem::path> roots() const;
  // the root of the given kind; empty if it is not the library's root (DD-32)
  std::filesystem::path writeRootFor(PrefabRoot root) const;
  // the root new files go to, which is the one the library scans; none without a root
  std::optional<PrefabRoot> writeRoot() const;
  // <game path>/<mod>, the mod's folder whatever the root; empty without a game path
  std::filesystem::path modFolder() const;

  /**
   * Starts a scan of the roots. A rescan while one runs abandons the running one. A file
   * whose size and modification time are unchanged is not read again; its entry is kept.
   * A file that failed is read again by every scan, so that a transient failure is
   * retried, unless it is not a prefab (PrefabEntry::notAPrefab).
   *
   * The walk of a root skips the folders that it may not enter and the entries whose
   * status cannot be read, enters folders at most PrefabMaxFolderDepth deep, against
   * link loops, and stops as soon as a later rescan abandons it. An error that ends the
   * walk early marks the root's listing incomplete: the scan then keeps the entries and
   * folders under that root that it did not see, and logs one warning (R8-lib-2).
   */
  void rescan();

  /**
   * Reads the given file again, or for the first time, on a worker, for example after
   * it was saved. A file under no root, in an autosave folder, or whose extension is not
   * .map (in any case) is ignored, even while a scan runs (R8-lib-6); a file that no
   * longer exists, or a call while a scan runs, rescans instead. The result lands in
   * processPendingResults.
   */
  void updateEntry(const std::filesystem::path& absolutePath);

  /**
   * Call on the GUI thread. Starts the parse tasks once the roots are listed; once every
   * file of the current scan is read, replaces the entries with the scan's results,
   * assigns entity definitions and fires libraryDidChangeNotifier once. Takes over the
   * results of updateEntry the same way; of two updates of one file, only the one that
   * was started last is taken over. Also reports that the map's definitions were
   * reloaded, which the library assigned to its entries when that happened (R8-lib-7).
   *
   * libraryDidChangeNotifier fires only when the entries changed: the first scan, an
   * entry that was added, removed or read again, a thumbnail that appeared or went, new
   * definitions. A scan that reused every entry, and read again only files that fail as
   * they failed before (whose entries are then kept), is silent; so the end of a scan
   * shows in scanning() turning false, not always in a notification.
   */
  void processPendingResults();

  // a scan or an update is running
  bool scanning() const;

  // sorted by root, then relativePathString
  const std::vector<std::unique_ptr<PrefabEntry>>& entries() const;

  /**
   * Every folder under the roots as the last finished scan listed them, the roots
   * included, empty folders too, lexically normal and sorted; for watching the folders
   * without walking them again. A root that does not exist has none.
   */
  const std::vector<std::filesystem::path>& directories() const;

  // relative paths compare as file names do on the platform, ignoring case on Windows;
  // the same goes for the rule that the user root wins over the mod root
  const PrefabEntry* find(
    PrefabRoot root, const std::filesystem::path& relativePath) const;

  // C4: "prefab:user/<rel>" | "prefab:mod/<rel>", by the entry's root
  std::string payloadFor(const PrefabEntry& entry) const;
  // entryForPrefabPath of the payload without "prefab:"; nullptr if not ours
  const PrefabEntry* fromPayload(const std::string& payload) const;

  /**
   * The entry that a placement's prefab path ("user/<rel>" or "mod/<rel>", see
   * Group::prefabPath) names. DD-37: the library lists one root at a time (DD-32), so the
   * relative path is looked up whatever its qualifier says: among the entries of the
   * active root (writeRoot) first, then among those of the other root, which the library
   * holds only until a scan after the root changed. Relative paths compare as find
   * compares them. nullptr if there is no such entry, or if the path has neither
   * qualifier.
   *
   * This is the lookup by which "already placed" compares a placement with an entry.
   */
  const PrefabEntry* entryForPrefabPath(std::string_view prefabPath) const;

  /**
   * Returns caller-owned clones of the body (the one entity, or the one top-level group),
   * link ids re-keyed (rekeyLinkIds), definitions kept; empty when the entry has no
   * body. An entry without a world reads its file again and returns the nodes it read,
   * without copying them; if the file has changed since it was scanned, the result is
   * empty and a warning is logged.
   *
   * The clones are not prepared for adding to a map; see prepareNodesForAdding.
   */
  std::vector<Node*> cloneBody(
    const PrefabEntry& entry, const vm::bbox3d& worldBounds) const;

  /**
   * The placed assemblies in the map, from one walk of its groups (the children of
   * entities are not visited), in the order of the world: every group with a prefab path,
   * grouped by that path and its link ID. The entries are looked up in the library as it
   * is, by entryForPrefabPath, so a library that was never scanned reports every file
   * missing. Each distinct prefab path is looked up once.
   */
  std::vector<PrefabPlacement> placements() const;

  /**
   * The placements, from the same walk, by their key: the prefab path and the link ID of
   * the members. Since refreshing one placement replaces the children of the other
   * members of its link set, and with them the groups of any placement nested in them,
   * a caller that refreshes several placements walks again after a refresh replaced a
   * group that another key's placement holds.
   */
  std::map<PrefabPlacementKey, PrefabPlacement> placementsByKey() const;

  /**
   * The placements whose file has changed since they were placed (its hash differs from
   * the placed hash), and those whose file is missing (entry is nullptr; reported, never
   * refreshed).
   */
  std::vector<PrefabPlacement> changedPlacements() const;

  void setReadHookForTesting(ReadHook readHook);
  void setListHookForTesting(ListHook listHook);

private:
  kdl::task_manager& taskManager();
  void startParsing(Listing listing);
  bool integrateScan();
  bool integrateUpdate(std::unique_ptr<PrefabEntry> entry);
  void rebuildEntryIndex();

  void connectObservers();
  void entityDefinitionsWillChange();
  void entityDefinitionsDidChangeAfterMap();

  void setEntityDefinitions(PrefabEntry& entry) const;
};

} // namespace tb::mdl
