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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

/**
 * A game file opened for reading. On Windows it shares read and delete access, so the
 * user can rebuild the game's containers or re-run dmap while the preview reads
 * (R4-3.26); POSIX never locks. The handle closes with the object; callers keep it only
 * for one read batch.
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
  // nullopt, with the reason, when the file cannot be opened (also a directory)
  static std::optional<GameFile> open(
    const std::filesystem::path& path, std::string* error = nullptr);

  GameFile() = default;
  GameFile(GameFile&& other) noexcept;
  GameFile& operator=(GameFile&& other) noexcept;
  ~GameFile();

  GameFile(const GameFile&) = delete;
  GameFile& operator=(const GameFile&) = delete;

  // the size when it was opened
  std::uint64_t size() const;

  // exactly length bytes at offset, or false; never reads past the end
  bool readAt(std::uint64_t offset, char* buffer, std::size_t length) const;

private:
  void close();
};

// the whole file, opened as GameFile opens it and closed again
std::optional<std::string> readGameFile(
  const std::filesystem::path& path, std::string* error = nullptr);

// a file's size and modification time, without opening it
struct GameFileStat
{
  std::uint64_t size = 0;
  std::int64_t time = 0;
};

std::optional<GameFileStat> statGameFile(const std::filesystem::path& path);

/**
 * The engine's Sys_ListFiles for a file extension (sys/win32/win_main.cpp, ListOSFiles):
 * on Windows the folder and every folder below it, names relative to the folder with
 * forward slashes, matched as PathMatchSpecW matches "*<extension>" (the file's long name
 * ends in the extension, case-insensitively); elsewhere only the folder itself, matched
 * as fnmatch matches it (case-sensitively). Directories are never listed. In no defined
 * order; an unreadable folder lists nothing.
 */
std::vector<std::string> listOsFiles(
  const std::filesystem::path& folder, const std::string& extension);

/**
 * The file OpenOSFile opens for a loose path: on Windows the path itself, whose case the
 * file system ignores; elsewhere the path, or else the file in the same folder whose path
 * equals it case-insensitively, as the engine's fallback finds it. nullopt when there is
 * none or it is a directory.
 */
std::optional<std::filesystem::path> findLooseFile(const std::filesystem::path& path);

// a UTF-8 path for the engine's names; never throws for an unrepresentable path
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(const std::string& path);

} // namespace tb::lit
