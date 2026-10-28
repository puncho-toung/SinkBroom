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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class GameContainerIndex;
struct GameContainerListing;
struct GameContainerTables;
struct Pk4Table;

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
 * LP6.3 (C67 (a), A52): the tables are immutable objects shared by the sources made from
 * them (lit/GameContainerIndex.h); a source keeps only its own open handles, loose
 * folders and warnings. A source made with a container index lists the containers and
 * stats each (the key check), takes the key's tables from the index when it holds them
 * (replaying the scan's warnings), and scans otherwise; a reload checks the key again and
 * scans only when it moved. Whether the tables were scanned or found, every read,
 * listing, probe and the search order are the same. The tables' paths are below the
 * key's base (the base folder made absolute and spelled as the file system spells it);
 * the loose files are looked up below the base folder as the game path gives it.
 *
 * Cancel (R5.20, G4): once the flag is set, the scan and the listings stop; the bound is
 * one directory entry, one container table entry or one .pk4 central directory (TbFsLib
 * reads it in one call). A cancelled source holds what it scanned so far, and is not
 * complete: the index stores nothing of it.
 *
 * LP1c fix (S2.2): the loose folders a probe or a read looks in are listed once each and
 * kept with their names (case-folded as the file system compares them: without case on
 * Windows, ASCII case elsewhere), so that a loose file that is not there is a lookup, not
 * a file-system call. looseFilesMayHaveChanged (the parse thread's poll) starts a new
 * round: the first look into a folder after it stats the folder and lists it again when
 * its time changed (a file added, removed or renamed in it), so a new file is seen at the
 * next poll; a file's own changes are seen by its stat, as before.
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
  struct LooseFolder;

  static constexpr int MaxLooseReadAttempts = 3;

  std::filesystem::path m_base;
  const std::atomic<bool>* m_cancel = nullptr;
  std::vector<std::unique_ptr<Pk4>> m_pk4s;
  std::vector<std::unique_ptr<Resources>> m_resources;
  std::vector<std::string> m_warnings;
  bool m_changedWhileRead = false;
  // LP6.3: set at the constructor's last line, so never for a cancelled scan; the tables
  // a complete scan read, for the index to store (null for a hit or without a key); where
  // the tables came from
  bool m_complete = false;
  std::shared_ptr<const GameContainerTables> m_scanned;
  ContainerTablesOrigin m_origin;
  // LP1c fix (S2.2): the loose folders looked in, by their case-folded path, and the
  // round of polls whose changes they show
  std::unordered_map<std::filesystem::path::string_type, LooseFolder> m_looseFolders;
  std::uint64_t m_looseRound = 0;
  std::size_t m_looseListings = 0;

public:
  explicit GameFileSource(
    const std::filesystem::path& gamePath, const std::atomic<bool>* cancel = nullptr);
  /**
   * LP6.3 (C67 (a)): with index, the key's tables when the index holds them (a key check
   * while cancel is set finds none), else a scan, which the index stores once the source
   * is complete (GameContainerIndex::makeSource). The parse thread
   */
  GameFileSource(
    const std::filesystem::path& gamePath,
    const std::atomic<bool>* cancel,
    GameContainerIndex* index);
  ~GameFileSource() override;

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override;
  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override;
  GameFileProbe probe(const std::string& path) override;
  std::optional<std::string> readGameFile(
    const std::string& path, GameFileProbe* found) override;
  std::optional<std::string> readGameFileHead(
    const std::string& path, std::size_t maxBytes, GameFileProbe* found) override;
  std::optional<std::string> gameRelativePath(const std::string& filePath) const override;
  bool usingResourceFiles() const override;
  std::vector<DeclFileTime> looseFileTimes(
    const std::string& folder, const std::string& extension) const override;
  std::vector<std::string> takeWarnings() override;
  void closeFiles() override;
  void looseFilesMayHaveChanged() override;
  bool changedWhileRead() const override;
  std::size_t containerCount() const override;
  ContainerTablesOrigin containerTablesOrigin() const override;

  // LP6.3: the constructor ran to its end (no cancel cut the scan short)
  bool complete() const;
  // LP6.3: the tables a complete scan with a key read (null for a hit, a cancelled scan
  // or a listing without a key), which the index stores
  std::shared_ptr<const GameContainerTables> scannedTables() const;
  // LP6.3: the index's write of the scanned tables: its seconds and bytes, and its
  // warnings (a write that failed), taken with the source's others
  void recordIndexWrite(
    double seconds, std::uint64_t bytes, std::vector<std::string> warnings);

  // the containers in the order a read searches them, the first searched first
  std::vector<std::string> searchOrder() const;
  // how many containers are open now
  std::size_t openContainers() const;
  // how many times a loose folder was listed (Y.3)
  std::size_t looseListingsForTesting() const;
  // LP7.1a: every container entry's name (the .resources files', then the .pk4 files',
  // as their tables hold them), for the retail model sweep, which lists a folder's
  // entries below its subfolders too; a listing (listFiles) of a container is flat
  std::vector<std::string> containerEntryNamesForTesting() const;

private:
  bool cancelled() const;
  // LP6.3: the containers of a listing read, in the registration order; false once a
  // cancel cut it short. afterContainer runs after each container's table (tests)
  bool scan(
    GameContainerListing listing,
    const std::function<void(const std::string&)>& afterContainer);
  // LP6.3: a key's tables found in the index, and the scan's warnings replayed
  void adopt(const GameContainerTables& tables);
  std::shared_ptr<const Pk4Table> readPk4(
    const std::filesystem::path& base, const std::string& name);
  // the loose file a read of this relative path opens, or nullopt; nullopt at once when
  // the folder's listing has no such name (S2.2)
  std::optional<std::filesystem::path> looseFile(const std::filesystem::path& relative);
  LooseFolder& looseFolder(const std::filesystem::path& folder);
  GameFileProbe probeFile(const std::string& path);
  // the C8 read: the loose file, then the .pk4 files, then the resources; decl says
  // whether a loose file that keeps changing asks for another build (A3.5)
  std::optional<std::string> read(
    const std::string& path,
    GameFileProbe* found,
    bool decl,
    std::size_t maxBytes = SIZE_MAX);
  std::optional<std::string> readLoose(
    const std::string& path, GameFileProbe* found, bool decl, std::size_t maxBytes);
  std::optional<std::string> readFromPk4(
    Pk4& pk4, const std::filesystem::path& entryPath, const std::string& path);

  deleteCopyAndMove(GameFileSource);
};

} // namespace tb::lit
