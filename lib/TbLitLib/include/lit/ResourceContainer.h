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

#include "base/Result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class GameFile;

// framework/File_Resource.h: RESOURCE_FILE_MAGIC
constexpr std::uint32_t ResourceFileMagic = 0xD000000Du;

// one entry of a container's table (idResourceCacheEntry)
struct ResourceEntry
{
  // backslashes made slashes and lowercased, as Init stores it
  std::string name;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

/**
 * A BFG .resources container: a mirror of idResourceContainer::Init and the read path
 * (framework/File_Resource.cpp, idFile_InnerResource, GetResourceCacheEntry). The header
 * is three big-endian int32s (magic 0xD000000D, table offset, table length); the table is
 * a big-endian entry count, then per entry a little-endian int32 name length, the name, a
 * big-endian offset and a big-endian length.
 *
 * Outside the engine's code every offset and length is checked against the file's size
 * before anything is read: a container whose header or table does not fit is refused, and
 * an entry whose data does not fit is dropped, each with a warning; where the engine
 * would quit (a wrong magic) the container is refused with a warning. Nothing is ever
 * read out of bounds. Lookups are case-insensitive, and of two entries with one name the
 * later wins, as the engine's hash index finds it (idHashIndex::Add puts the newer
 * first).
 */
class ResourceContainer
{
private:
  std::string m_name;
  std::filesystem::path m_path;
  std::uint64_t m_fileSize = 0;
  std::int64_t m_fileTime = 0;
  std::vector<ResourceEntry> m_entries;
  std::unordered_map<std::string, std::size_t> m_index;

public:
  /**
   * Init: reads the header and the table of the file at path. name is the container's
   * name as the engine registers it ("_common.resources", "maps/admin.resources").
   * Fails, saying why, when the file cannot be opened or is not a valid container, or
   * when cancel is set: the table is read entry by entry, and a stop waits for one entry.
   * warnings receives the entries that are dropped.
   */
  static Result<ResourceContainer> open(
    const std::filesystem::path& path,
    std::string name,
    std::vector<std::string>& warnings,
    const std::atomic<bool>* cancel = nullptr);

  const std::string& name() const;
  const std::filesystem::path& path() const;
  // the table, in the file's order
  const std::vector<ResourceEntry>& entries() const;

  // the entry a lookup of this game path finds, or null (GetResourceCacheEntry)
  const ResourceEntry* find(std::string_view gamePath) const;

  /**
   * idFile_InnerResource's read: the entry's bytes from file, which must be this
   * container's file opened now (GameFile). Fails when the file has changed since the
   * table was read or the entry no longer fits in it.
   */
  Result<std::string> read(const ResourceEntry& entry, const GameFile& file) const;
  // LP1c.2: the entry's first maxBytes bytes (all of a shorter one), checked as read is
  Result<std::string> read(
    const ResourceEntry& entry, const GameFile& file, std::size_t maxBytes) const;

  // LP1c fix (S2.2): fails when the container's file has changed since the table was
  // read (its size or time), which read checks before every read; a reader that keeps
  // the file open checks it once, when it opens it, then reads with readOpened
  Result<void> unchanged() const;
  // read's bytes without the file system's check: file must be this container's file,
  // opened after unchanged() succeeded; its size is still checked
  Result<std::string> readOpened(
    const ResourceEntry& entry, const GameFile& file, std::size_t maxBytes) const;

  // the file's size and time when the table was read
  std::uint64_t fileSize() const;
  std::int64_t fileTime() const;
};

// GetResourceCacheEntry's canonical name: backslashes made slashes, ASCII lowercased
std::string canonicalResourceName(std::string_view path);

} // namespace tb::lit
