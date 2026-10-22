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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

/**
 * A game file opened for reading. On Windows it shares read, write and delete access, so
 * the user can rewrite or replace the game's files while the preview reads them
 * (R4-3.26); a torn read is caught by the readers' size and time checks. POSIX never
 * locks. The handle closes with the object; callers keep it only for one read batch.
 */
class GameFile
{
private:
#if defined(_WIN32)
  void* m_handle = nullptr;
#else
  int m_fd = -1;
#endif
  std::uint64_t m_size = 0;

public:
  // fails when the file cannot be opened (also for a directory)
  static Result<GameFile> open(const std::filesystem::path& path);

  GameFile() = default;
  GameFile(GameFile&& other) noexcept;
  GameFile& operator=(GameFile&& other) noexcept;
  ~GameFile();

  GameFile(const GameFile&) = delete;
  GameFile& operator=(const GameFile&) = delete;

  // the size when it was opened
  std::uint64_t size() const;

  // exactly length bytes at offset; never reads past the size it was opened with
  Result<void> readAt(std::uint64_t offset, char* buffer, std::size_t length) const;

private:
  void close();
};

// the whole file, opened as GameFile opens it and closed again
Result<std::string> readGameFile(const std::filesystem::path& path);

/**
 * A file's size and modification time, without opening its body; nullopt for no regular
 * file. time is a change stamp in the file system's finest unit (FILETIME ticks on
 * Windows), which tells two saves within one second apart and is compared only with
 * itself (polling). unixSeconds is the time as the engine keeps it (A4, P.15:
 * Sys_FileTimeStamp): on Windows GetFileAttributesExW's last write time, (ticks −
 * 116444736000000000) / 10⁷ as the engine divides; elsewhere stat's st_mtime. A link is
 * followed to its file, as the engine's open follows it. Any thread.
 */
struct GameFileStat
{
  std::uint64_t size = 0;
  std::int64_t time = 0;
  std::int64_t unixSeconds = 0;

  bool operator==(const GameFileStat& other) const = default;
};

std::optional<GameFileStat> statGameFile(const std::filesystem::path& path) noexcept;

/**
 * The engine's Sys_ListFiles for a file extension (sys/win32/win_main.cpp, ListOSFiles):
 * on Windows the folder and every folder below it, names relative to the folder with
 * forward slashes, matched as PathMatchSpecW matches "*<extension>" (the file's long name
 * ends in the extension, case-insensitively); elsewhere only the folder itself, matched
 * as fnmatch matches it (case-sensitively). Directories are never listed. In no defined
 * order.
 *
 * A3.8: on Windows a directory junction or link is followed, as the engine's
 * FindFirstFileW recursion follows any entry with FILE_ATTRIBUTE_DIRECTORY, and listed
 * under its own name. Differences from the engine (mirror.json): a link to a folder the
 * listing is inside of (a loop, which the engine follows until its path is too long) is
 * not followed, with a warning; a folder that cannot be read is skipped, as the engine
 * skips it, but the rest is listed; a name that is not valid Unicode is skipped with a
 * warning. Once cancel is set, nothing more is listed: the listing stops within one
 * directory entry.
 */
std::vector<std::string> listOsFiles(
  const std::filesystem::path& folder,
  const std::string& extension,
  std::vector<std::string>* warnings = nullptr,
  const std::atomic<bool>* cancel = nullptr);

/**
 * The file OpenOSFile opens for a loose path: on Windows the path itself, whose case the
 * file system ignores; elsewhere the path, or else the file in the same folder whose path
 * equals it case-insensitively, as the engine's fallback finds it. nullopt when there is
 * none or it is a directory.
 */
std::optional<std::filesystem::path> findLooseFile(
  const std::filesystem::path& path) noexcept;

// a UTF-8 path for the engine's names, and back; nullopt for a name that is not valid
// Unicode (a lone UTF-16 surrogate, bytes that are not UTF-8). Never throws.
std::optional<std::string> pathToUtf8(const std::filesystem::path& path) noexcept;
std::optional<std::filesystem::path> pathFromUtf8(const std::string& path) noexcept;

// The engine's ASCII-only case rules (idStr::ToLower and Icmp on a signed char): only
// 'A' to 'Z' change. kdl's case helpers use the locale's tolower, so they are not used.
char toLowerAscii(char c);
std::string toLowerAscii(std::string_view text);
// negative, zero or positive, over at most count characters, as idStr::Icmpn
int compareNoCaseAscii(std::string_view lhs, std::string_view rhs, std::size_t count);
bool equalsNoCaseAscii(std::string_view lhs, std::string_view rhs);
bool endsWithNoCaseAscii(std::string_view text, std::string_view suffix);

} // namespace tb::lit
