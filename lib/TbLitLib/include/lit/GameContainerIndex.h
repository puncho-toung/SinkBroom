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

#include "base/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class GameFileSource;
class ResourceContainer;

/**
 * LP6.3 (C67 (a), A52): one .pk4's central directory as a scan read it, without the
 * handle a read batch opens: immutable once made, shared by every source built from it.
 */
struct Pk4Table
{
  // as AddGameDirectory registers it: the name Sys_ListFiles lists below base
  std::string name;
  // the file, below the key's base (GameContainerKey::base)
  std::filesystem::path path;
  // the entries as idZipContainer::Init stores them (slashes, lowercased), in the order
  // the zip file system lists them, and the path to open each with
  std::vector<std::string> names;
  std::unordered_map<std::string, std::filesystem::path> index;
  // Y.2: the file's change stamp and size when its directory was read
  std::int64_t stamp = 0;
  std::uint64_t size = 0;
};

// one container of the key: its name as registered and its file's size and change stamp
// (statGameFile)
struct ContainerStamp
{
  std::string name;
  std::uint64_t size = 0;
  std::int64_t stamp = 0;

  bool operator==(const ContainerStamp&) const = default;
};

/**
 * What the scan of a game's containers reads them by (C67 (a)): the base folder, made
 * absolute once (std::filesystem::absolute: a relative game path against the process's
 * current folder) and spelled as the file system spells it (std::filesystem::canonical,
 * when the folder exists: the folders' own case, links resolved); the .resources of base
 * and of base/maps after the duplicate rule, in the registration order, and the .pk4
 * files of base, sorted, each with its file's (size, stamp). Equal keys read the same
 * tables; any other change rebuilds the whole key's (all-or-nothing).
 */
struct GameContainerKey
{
  std::filesystem::path base;
  std::vector<ContainerStamp> resources;
  std::vector<ContainerStamp> pk4s;

  bool operator==(const GameContainerKey&) const = default;
};

/**
 * The containers' listing as GameFileSource's scan takes it (AddGameDirectory,
 * FileSystem.cpp:3174-3252): the names, the listings' warnings and the key. key is
 * nullopt when the listing was cancelled or a listed container could not be statted (no
 * key: no index hit and nothing stored). Any thread; the parse thread's.
 */
struct GameContainerListing
{
  // the key's base, which the tables' paths are below
  std::filesystem::path base;
  // after the duplicate rule, in the registration order
  std::vector<std::string> resources;
  // sorted
  std::vector<std::string> pk4s;
  // the .resources listings' warnings, then the .pk4 listing's
  std::vector<std::string> resourceWarnings;
  std::vector<std::string> pk4Warnings;
  std::optional<GameContainerKey> key;
};

// the listing of <game path>/base's containers (folder is <game path>/base as the source
// spells it); a stop flag set cuts it short and leaves no key
GameContainerListing listGameContainers(
  const std::filesystem::path& folder, const std::atomic<bool>* cancel);

// what a complete scan read: the key, every table and the scan's warnings, in order
struct GameContainerTables
{
  GameContainerKey key;
  // the .resources whose tables were read, in the registration order
  std::vector<std::shared_ptr<const ResourceContainer>> resources;
  // the .pk4 files whose directories were read (no binary pak), sorted
  std::vector<std::shared_ptr<const Pk4Table>> pk4s;
  std::vector<std::string> warnings;
};

// LP6.3: the index file's version; a file of another version is read as none
inline constexpr auto ContainerIndexFileVersion = std::uint32_t{1};

/**
 * The tables in the index file's format: a header (the magic "SBLITIDX", the version,
 * the path unit size, the payload size), the payload (the key in full, the warnings,
 * every .resources table, every .pk4 directory: lengths and counts as little-endian
 * integers, strings as bytes, paths as their native units) and its 64-bit FNV-1a hash.
 * Pure.
 */
std::string writeContainerIndexFile(const GameContainerTables& tables);
/**
 * The tables an index file holds, or nullopt with why (error) when it is short, of
 * another version, damaged (its hash, a count or a length that the file cannot hold, an
 * entry that does not fit in its container, a table its key does not list) or holds
 * another key than key; never reads out of bounds, never returns a partial table. version
 * is the file's (0 when it has no header), damaged whether it is damaged rather than
 * another version or key. Pure.
 */
std::optional<GameContainerTables> readContainerIndexFile(
  const std::string& bytes,
  const GameContainerKey& key,
  std::string* error = nullptr,
  bool* damaged = nullptr);

/**
 * LP6.3 (C67 (a), A52, Q42, P8.17, P8.18): the game's container tables, kept across
 * Stop and persisted, so that a Start whose key matches skips the scan of the .resources
 * tables and the .pk4 central directories (GameFileSource.cpp's constructor, 420-520 ms
 * on admin) and replays its warnings.
 *
 * makeSource is the material service's makeSource: it lists the containers and stats
 * each (the key check: milliseconds, never a table read), then finds the key's tables in
 * memory, else in the index file (folder / containers-<hash of the base>.idx, the key in
 * full checked on read, never trusted from the hash), else scans; a key check while the
 * stop flag is set finds nothing. A scan's tables are stored, in memory and in the file,
 * only when its source is complete (GameFileSource::complete: a cancelled scan stores
 * nothing) and every table's stat equals its key's; the file is written atomically (a
 * temporary file in the folder, then a rename) right after the scan, before the decl
 * read, and a write that fails is a warning of the source. One entry per base: a new key
 * replaces the base's tables whole. A missing file, one of another version or another
 * key rebuilds silently; a damaged one with one warning.
 *
 * Threads: the parse thread is the only reader and writer of the tables; the owner (the
 * lit panel, on the GUI thread) only holds the index and may drop the memory copy. One
 * mutex guards the map of base -> tables, held for a lookup or a store, never during a
 * scan or a file read or write. The tables are immutable: a source built from them
 * shares them and opens its own handles per read batch. Two indexes (two map windows) may
 * read and write one file: the rename makes either copy whole.
 */
class GameContainerIndex
{
private:
  // empty: memory only (tests)
  std::filesystem::path m_folder;
  mutable std::mutex m_mutex;
  std::map<std::filesystem::path::string_type, std::shared_ptr<const GameContainerTables>>
    m_tables;
  std::function<void(const std::string&)> m_afterContainerForTesting;

public:
  explicit GameContainerIndex(std::filesystem::path folder);
  ~GameContainerIndex();

  // the parse thread: a source of gamePath's base folder, its tables from the index when
  // the key matches, else scanned (and stored when complete)
  std::unique_ptr<GameFileSource> makeSource(
    const std::filesystem::path& gamePath, const std::atomic<bool>* cancel);

  // the parse thread: the key's tables in memory, or from the file (then kept in
  // memory); null when neither holds the key. warnings receives a damaged file's line
  std::shared_ptr<const GameContainerTables> find(
    const GameContainerKey& key,
    bool& fromFile,
    std::uint64_t& fileBytes,
    std::vector<std::string>& warnings);

  const std::filesystem::path& folder() const;
  // the index file of a base folder (the key's)
  std::filesystem::path filePath(const std::filesystem::path& base) const;

  // Test hooks.

  // any thread: the memory copy dropped, as at a new launch; the file stays
  void dropMemoryForTesting();
  // any thread: the bases whose tables the memory copy holds
  std::size_t memoryEntriesForTesting() const;
  // runs on the scanning thread after each container's table is read (its name), before
  // the next; a test sets the stop flag there (deterministic, never a timing race)
  void setAfterContainerForTesting(std::function<void(const std::string&)> hook);
  const std::function<void(const std::string&)>& afterContainerForTesting() const;

private:
  // stores a complete scan's tables; the write's seconds and bytes, or a warning
  void store(
    std::shared_ptr<const GameContainerTables> tables,
    double& writeSeconds,
    std::uint64_t& fileBytes,
    std::vector<std::string>& warnings);

  deleteCopyAndMove(GameContainerIndex);
};

} // namespace tb::lit
