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
#include "lit/MaterialDecls.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

// where a game file was found (for the tests and the log)
struct GameFileOrigin
{
  enum class Kind
  {
    Loose,
    Pk4,
    Resources,
  };

  Kind kind = Kind::Loose;
  // the container's name as the engine registers it ("_rbdoom_core.pk4",
  // "maps/admin.resources"); empty for a loose file
  std::string container;
};

/**
 * The game's files as the engine's file system finds them (C8, C1, D23, D55): every game
 * file the preview reads, the decls, the image caches, the bakes and the .proc/.bproc,
 * for one search path: <game path>/base. No mods, fs_game, fs_game_base or fs_savepath
 * are searched (recorded in mirror.json). A mirror of framework/FileSystem.cpp at the
 * pinned commit with fs_resourceLoadPriority 0:
 *
 * - Containers are registered as AddGameDirectory registers them: the .resources files of
 *   base (on Windows also those below it, since Sys_ListFiles recurses there), sorted
 *   with idSort_PathStr, then those of base/maps, sorted, with "maps/" before each name;
 *   then the .pk4 files of base, sorted. A container listed twice is kept once, at its
 *   later place, which finds the same file for every name.
 * - A read looks first for the loose file, then in the .pk4 files, then in the .resources
 *   files; containers of a kind are searched from the last registered to the first, as
 *   the engine walks its lists backwards. Inside one container the later of two entries
 *   with one name wins. A .pk4's entries are indexed by the names the zip stores, byte
 *   for byte (A3.13), lowercased as idZipContainer::Init lowercases them.
 * - A listing (ListFiles' GetFileList) merges every source's names of a folder with an
 *   extension: the containers' flat ones, lowercased as the engine stores them, and the
 *   loose ones as Sys_ListFiles lists them; the registry adds each once and sorts.
 * - A probe (A5) answers what a read would find, and its time as the engine keeps it
 *   (A4: a loose file's whole Unix seconds, 0 in a container, -1 for none), without
 *   opening the file's body: a loose file is statted, a container entry is looked up in
 *   the tables read at construction.
 *
 * .pk4 files are read with TrenchBroom's ZipFileSystem (D55), .resources files with
 * lit/ResourceContainer.h. Game files are opened per read, sharing read, write and delete
 * access; containers opened during a read batch stay open until closeFiles() (R4-3.26),
 * which the parse thread calls after each work item and at stop. A .pk4 is opened by
 * TbFsLib, without delete sharing (mirror.json M-FS-DIFF-02). The container tables are
 * read once, at construction: a reload makes a new source.
 *
 * Cancel (R5.20, G4): once the flag is set, the scan and the listings stop; the bound is
 * one directory entry, one container table entry or one .pk4 central directory (TbFsLib
 * reads it in one call). A cancelled source holds what it scanned so far.
 *
 * A loose file that changes while it is read is read again, up to three times. A decl
 * read (readFile) that keeps changing is returned as read and changedWhileRead() asks for
 * another build (A3.5); any other read (readGameFile) that keeps changing reads as no
 * file, with a warning, and is read again when it is asked for again. A loose file that
 * cannot be opened is skipped without a message, as the engine goes on to the containers.
 *
 * Not thread-safe: it belongs to the thread that owns the registry (the parse thread).
 */
class GameFileSource : public DeclSource
{
private:
  struct Pk4;
  struct Resources;

  static constexpr int MaxLooseReadAttempts = 3;

  std::filesystem::path m_base;
  const std::atomic<bool>* m_cancel = nullptr;
  std::vector<std::unique_ptr<Pk4>> m_pk4s;
  std::vector<std::unique_ptr<Resources>> m_resources;
  std::vector<std::string> m_warnings;
  bool m_changedWhileRead = false;

public:
  explicit GameFileSource(
    const std::filesystem::path& gamePath, const std::atomic<bool>* cancel = nullptr);
  ~GameFileSource() override;

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override;
  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override;
  GameFileProbe probe(const std::string& path) override;
  std::optional<std::string> readGameFile(
    const std::string& path, GameFileProbe* found) override;
  std::optional<std::string> gameRelativePath(const std::string& filePath) const override;
  bool usingResourceFiles() const override;
  std::vector<DeclFileTime> looseFileTimes(
    const std::string& folder, const std::string& extension) const override;
  std::vector<std::string> takeWarnings() override;
  void closeFiles() override;
  bool changedWhileRead() const override;
  std::size_t containerCount() const override;

  // the source that a read of path finds, or nullopt
  std::optional<GameFileOrigin> origin(const std::string& path) const;
  // the containers in the order a read searches them, the first searched first
  std::vector<std::string> searchOrder() const;
  // how many containers are open now
  std::size_t openContainers() const;

private:
  bool cancelled() const;
  std::unique_ptr<Pk4> readPk4(const std::string& name);
  GameFileProbe probeFile(const std::string& path) const;
  // the C8 read: the loose file, then the .pk4 files, then the resources; decl says
  // whether a loose file that keeps changing asks for another build (A3.5)
  std::optional<std::string> read(
    const std::string& path, GameFileProbe* found, bool decl);
  std::optional<std::string> readLoose(
    const std::string& path, GameFileProbe* found, bool decl);
  std::optional<std::string> readFromPk4(
    Pk4& pk4, const std::filesystem::path& entryPath, const std::string& path);

  deleteCopyAndMove(GameFileSource);
};

} // namespace tb::lit
