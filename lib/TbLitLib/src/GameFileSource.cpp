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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/framework/FileSystem.cpp and
// framework/File_Zip.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/GameFileSource.h"

#include "fs/File.h"
#include "fs/PathInfo.h"
#include "fs/Reader.h"
#include "fs/TraversalMode.h"
#include "fs/ZipFileSystem.h"
#include "lit/GameContainerIndex.h"
#include "lit/GameFile.h"
#include "lit/ResourceContainer.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace tb::lit
{
namespace
{

// framework/File_Zip.cpp: a .pk4 holding this file is a binary pak, which Init refuses
constexpr auto BinaryConfig = "binary.conf";

using Clock = std::chrono::steady_clock;

double secondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double>(Clock::now() - start).count();
}

/**
 * GetFileList's test of a container entry (FileSystem.cpp:2322-2391, also 2407-2476): the
 * entry's name below relativePath when it lies directly in it (no subfolder, as
 * allowSubdirsForResourcePaks is false for ListFiles) and ends in extension; with its
 * trailing slashes stripped.
 */
std::optional<std::string> flatName(
  const std::string& filename,
  const std::string& relativePath,
  const std::string& extension)
{
  auto pathLength = relativePath.size();
  if (pathLength)
  {
    // for the trailing '/'
    ++pathLength;
  }

  // if the name is not long enough to at least contain the path
  if (filename.size() <= pathLength)
  {
    return std::nullopt;
  }
  // check for a path match without the trailing '/'
  if (pathLength && compareNoCaseAscii(filename, relativePath, pathLength - 1) != 0)
  {
    return std::nullopt;
  }
  // ensure we have a path, and not just a filename containing the path
  if (filename[pathLength] == '\0' || (pathLength && filename[pathLength - 1] != '/'))
  {
    return std::nullopt;
  }
  // make sure the file is not in a subdirectory
  auto j = pathLength;
  for (; j + 1 < filename.size(); ++j)
  {
    if (filename[j] == '/')
    {
      break;
    }
  }
  if (j + 1 < filename.size())
  {
    return std::nullopt;
  }
  // check for extension match
  if (
    filename.size() < extension.size()
    || !equalsNoCaseAscii(filename.substr(filename.size() - extension.size()), extension))
  {
    return std::nullopt;
  }

  auto work = filename.substr(pathLength);
  while (!work.empty() && work.back() == '/')
  {
    work.pop_back();
  }
  return work;
}

/**
 * A3.13: a .pk4 entry's name as the zip stores it. TbFsLib's ZipFileSystem builds each
 * entry's path from miniz's raw name (std::filesystem::path{std::string}, which decodes
 * the bytes in the narrow code page), so the path's native narrow form, encoded in the
 * same code page, gives the bytes back, as idZipContainer::Init stores them; nullopt when
 * the code page cannot hold the name.
 */
std::optional<std::string> rawZipName(const std::filesystem::path& path) noexcept
{
  try
  {
    return path.string();
  }
  catch (...)
  {
    return std::nullopt;
  }
}

// OpenFileReadFlags' checks of a relative path: the leading slash is dropped, and a path
// that could back up the tree, or is empty, finds nothing
std::optional<std::string> checkedRelativePath(std::string path)
{
  if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
  {
    path.erase(0, 1);
  }
  if (
    path.find("..") != std::string::npos || path.find("::") != std::string::npos
    || path.empty())
  {
    return std::nullopt;
  }
  return path;
}

} // namespace

struct GameFileSource::Pk4
{
  // LP6.3 (C67 (a)): the central directory as the scan read it, shared and immutable
  std::shared_ptr<const Pk4Table> table;
  // open during a read batch only
  std::unique_ptr<fs::ZipFileSystem> open;
};

struct GameFileSource::Resources
{
  // LP6.3 (C67 (a)): the table as the scan read it, shared and immutable
  std::shared_ptr<const ResourceContainer> container;
  // open during a read batch only (S2.2: statted once, when it is opened)
  std::optional<GameFile> open;
};

struct GameFileSource::LooseFolder
{
  // the folder's time when it was listed, and whether it was there
  std::filesystem::file_time_type time{};
  bool exists = false;
  // X.10: when it was listed, by the file clock
  std::filesystem::file_time_type listed{};
  // its entries' names, case-folded
  std::unordered_set<std::filesystem::path::string_type> names;
  // the round of polls it was last looked at in
  std::uint64_t round = 0;
};

namespace
{

/**
 * LP1c fix (S2.2): a name as the loose file system compares it: on Windows without case
 * (each UTF-16 unit through towlower, as the file system's upcase table folds the usual
 * scripts), elsewhere ASCII case, which the engine's OpenOSFile fallback ignores within a
 * folder.
 */
std::filesystem::path::string_type foldedName(std::filesystem::path::string_type name)
{
#if defined(_WIN32)
  for (auto& c : name)
  {
    c = wchar_t(std::towlower(std::wint_t(c)));
  }
#else
  for (auto& c : name)
  {
    c = toLowerAscii(c);
  }
#endif
  return name;
}

// a folder path as the index keys it: on Windows case-folded, elsewhere as it is (the
// fallback looks only in the folder the path names)
std::filesystem::path::string_type folderKey(const std::filesystem::path& folder)
{
#if defined(_WIN32)
  return foldedName(folder.lexically_normal().native());
#else
  return folder.lexically_normal().native();
#endif
}

Result<std::unique_ptr<fs::ZipFileSystem>> openZip(const std::filesystem::path& path)
{
  return fs::createCFile(path) | kdl::and_then([](auto file) {
           return fs::createImageFileSystem<fs::ZipFileSystem>(std::move(file));
         });
}

} // namespace

GameFileSource::GameFileSource(
  const std::filesystem::path& gamePath, const std::atomic<bool>* cancel)
  : GameFileSource{gamePath, cancel, nullptr}
{
}

GameFileSource::GameFileSource(
  const std::filesystem::path& gamePath,
  const std::atomic<bool>* cancel,
  GameContainerIndex* index)
  : m_base{gamePath / "base"}
  , m_cancel{cancel}
{
  // AddGameDirectory( <game path>, "base" ), FileSystem.cpp:3174-3252: the containers'
  // listing; LP6.3 (C67 (a)): with one stat per container it is the index's key
  const auto keyStart = Clock::now();
  auto listing = listGameContainers(m_base, m_cancel);
  m_origin.keySeconds = secondsSince(keyStart);

  auto found = std::shared_ptr<const GameContainerTables>{};
  if (index)
  {
    m_origin.kind = ContainerTablesOrigin::Kind::Scan;
    // a key check while the stop flag is set finds nothing (G4)
    if (listing.key && !cancelled())
    {
      auto fromFile = false;
      auto fileBytes = std::uint64_t{0};
      found = index->find(*listing.key, fromFile, fileBytes, m_warnings);
      if (found)
      {
        m_origin.kind = fromFile ? ContainerTablesOrigin::Kind::File
                                 : ContainerTablesOrigin::Kind::Memory;
        m_origin.fileBytes = fileBytes;
      }
    }
  }
  if (found)
  {
    adopt(*found);
  }
  else if (!scan(
             std::move(listing),
             index ? index->afterContainerForTesting()
                   : std::function<void(const std::string&)>{}))
  {
    // cancelled (R5.20, G4): what was scanned so far stays, and the source is not
    // complete
    return;
  }
  m_complete = true;
}

bool GameFileSource::scan(
  GameContainerListing listing,
  const std::function<void(const std::string&)>& afterContainer)
{
  // the scan's own warnings, from here: a damaged index file's line before them is not
  // stored with the tables
  const auto firstWarning = m_warnings.size();
  m_warnings.insert(
    m_warnings.end(),
    std::make_move_iterator(listing.resourceWarnings.begin()),
    std::make_move_iterator(listing.resourceWarnings.end()));

  for (const auto& name : listing.resources)
  {
    if (cancelled())
    {
      return false;
    }
    if (const auto path = pathFromUtf8(name))
    {
      ResourceContainer::open(listing.base / *path, name, m_warnings, m_cancel)
        | kdl::transform([&](ResourceContainer container) {
            m_resources.push_back(std::make_unique<Resources>(Resources{
              std::make_shared<const ResourceContainer>(std::move(container)),
              std::nullopt}));
          })
        | kdl::transform_error([&](const auto& e) {
            if (!cancelled())
            {
              m_warnings.push_back(e.msg);
            }
          });
    }
    if (afterContainer)
    {
      afterContainer(name);
    }
  }

  // RB: Zip support, the base pass only
  m_warnings.insert(
    m_warnings.end(),
    std::make_move_iterator(listing.pk4Warnings.begin()),
    std::make_move_iterator(listing.pk4Warnings.end()));
  for (const auto& name : listing.pk4s)
  {
    // a .pk4's central directory is read in one call (TbFsLib), so a stop waits for one
    if (cancelled())
    {
      return false;
    }
    if (auto pk4 = readPk4(listing.base, name))
    {
      m_pk4s.push_back(std::make_unique<Pk4>(Pk4{std::move(pk4), nullptr}));
    }
    if (afterContainer)
    {
      afterContainer(name);
    }
  }
  if (cancelled())
  {
    return false;
  }

  // LP6.3: what the index stores, when the listing had a key
  if (listing.key)
  {
    auto tables = std::make_shared<GameContainerTables>();
    tables->key = std::move(*listing.key);
    for (const auto& resources : m_resources)
    {
      tables->resources.push_back(resources->container);
    }
    for (const auto& pk4 : m_pk4s)
    {
      tables->pk4s.push_back(pk4->table);
    }
    tables->warnings.assign(
      m_warnings.begin() + std::ptrdiff_t(firstWarning), m_warnings.end());
    m_scanned = std::move(tables);
  }
  return true;
}

void GameFileSource::adopt(const GameContainerTables& tables)
{
  for (const auto& container : tables.resources)
  {
    m_resources.push_back(
      std::make_unique<Resources>(Resources{container, std::nullopt}));
  }
  for (const auto& table : tables.pk4s)
  {
    m_pk4s.push_back(std::make_unique<Pk4>(Pk4{table, nullptr}));
  }
  // the scan's warnings, as the scan gave them
  m_warnings.insert(m_warnings.end(), tables.warnings.begin(), tables.warnings.end());
}

GameFileSource::~GameFileSource() = default;

bool GameFileSource::cancelled() const
{
  return m_cancel && m_cancel->load();
}

std::shared_ptr<const Pk4Table> GameFileSource::readPk4(
  const std::filesystem::path& base, const std::string& name)
{
  const auto path = pathFromUtf8(name);
  if (!path)
  {
    return nullptr;
  }

  auto pk4 = std::make_shared<Pk4Table>();
  pk4->name = name;
  pk4->path = base / *path;
  if (const auto stat = statGameFile(pk4->path))
  {
    pk4->stamp = stat->time;
    pk4->size = stat->size;
  }

  try
  {
    return openZip(pk4->path)
           | kdl::and_then([&](std::unique_ptr<fs::ZipFileSystem> zipFileSystem) {
               // the entries, and whether the .pk4 is a binary one
               return zipFileSystem->find("", fs::TraversalMode::Recursive)
                      | kdl::transform(
                        [&](const std::vector<std::filesystem::path>& paths) {
                          auto binary = false;
                          for (const auto& entryPath : paths)
                          {
                            if (
                              static_cast<const fs::FileSystem&>(*zipFileSystem)
                                .pathInfo(entryPath)
                              != fs::PathInfo::File)
                            {
                              continue;
                            }
                            // A3.13: the name as the zip stores it, byte for byte; G3: a
                            // name that cannot be given back is skipped, never thrown for
                            const auto raw = rawZipName(entryPath);
                            if (!raw)
                            {
                              m_warnings.push_back(fmt::format(
                                "Zip file {}: an entry name is not valid Unicode; it is "
                                "skipped",
                                name));
                              continue;
                            }
                            const auto entryName = canonicalResourceName(*raw);
                            binary = binary || entryName == BinaryConfig;
                            if (pk4->index.emplace(entryName, entryPath).second)
                            {
                              pk4->names.push_back(entryName);
                            }
                          }
                          return binary;
                        });
             })
           | kdl::transform([&](const bool binary) {
               // "ignore all binary paks", without a message
               return binary ? nullptr : std::shared_ptr<const Pk4Table>{std::move(pk4)};
             })
           | kdl::transform_error([&](const auto& e) {
               m_warnings.push_back(
                 fmt::format("Unable to open zip file {}: {}", name, e.msg));
               return std::shared_ptr<const Pk4Table>{};
             })
           | kdl::value();
  }
  catch (const std::exception& e)
  {
    m_warnings.push_back(fmt::format("Unable to open zip file {}: {}", name, e.what()));
    return nullptr;
  }
}

std::vector<std::string> GameFileSource::listFiles(
  const std::string& folder, const std::string& extension)
{
  auto names = std::vector<std::string>{};
  auto seen = std::unordered_set<std::string>{};
  const auto add = [&](std::string name) {
    if (seen.insert(canonicalResourceName(name)).second)
    {
      names.push_back(std::move(name));
    }
  };

  // GetFileList: the resource files, the last registered first, each in its table's
  // order; then the .pk4 files the same way; then the loose files
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    for (const auto& entry : (*it)->container->entries())
    {
      if (auto name = flatName(entry.name, folder, extension))
      {
        add(std::move(*name));
      }
    }
  }
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    for (const auto& entryName : (*it)->table->names)
    {
      if (auto name = flatName(entryName, folder, extension))
      {
        add(std::move(*name));
      }
    }
  }
  if (const auto folderPath = pathFromUtf8(folder))
  {
    for (auto& name : listOsFiles(m_base / *folderPath, extension, &m_warnings, m_cancel))
    {
      add(std::move(name));
    }
  }

  return names;
}

std::optional<std::string> GameFileSource::readLoose(
  const std::string& path,
  GameFileProbe* found,
  const bool decl,
  const std::size_t maxBytes)
{
  const auto relative = pathFromUtf8(path);
  if (!relative)
  {
    return std::nullopt;
  }
  const auto file = looseFile(*relative);
  if (!file)
  {
    return std::nullopt;
  }

  // A3.5: the file is statted before and after the read; while a save changes it, the
  // read is tried again; a decl file that keeps changing asks for another build, any
  // other file reads as none until it is asked for again
  for (auto attempt = 0; attempt < MaxLooseReadAttempts; ++attempt)
  {
    const auto before = statGameFile(*file);
    auto text = readGameFilePrefix(*file, maxBytes)
                | kdl::transform([](std::string bytes) {
                    return std::optional<std::string>{std::move(bytes)};
                  })
                | kdl::transform_error([](const auto&) {
                    // OpenFileReadFlags goes on to the containers without a message, and
                    // so does this
                    return std::optional<std::string>{};
                  })
                | kdl::value();
    if (!text)
    {
      return std::nullopt;
    }
    const auto after = statGameFile(*file);
    auto bytes = std::move(*text);
    const auto stable = before && after && *before == *after
                        && std::min(after->size, std::uint64_t(maxBytes)) == bytes.size();
    if (stable || attempt + 1 == MaxLooseReadAttempts)
    {
      if (!stable)
      {
        if (!decl)
        {
          m_warnings.push_back(fmt::format(
            "{} changed while it was read; it is read again when it is asked for", path));
          return std::nullopt;
        }
        m_changedWhileRead = true;
        m_warnings.push_back(
          fmt::format("{} changed while it was read; it is read again later", path));
      }
      if (found)
      {
        found->kind = GameFileProbe::Kind::Loose;
        found->container.clear();
        found->time = after ? after->unixSeconds : 0;
        found->size = after ? after->size : bytes.size();
        found->changeStamp = after ? after->time : 0;
      }
      return bytes;
    }
  }
  return std::nullopt;
}

std::optional<std::string> GameFileSource::read(
  const std::string& path,
  GameFileProbe* found,
  const bool decl,
  const std::size_t maxBytes)
{
  if (found)
  {
    *found = GameFileProbe{};
  }
  const auto relative = checkedRelativePath(path);
  if (!relative)
  {
    return std::nullopt;
  }

  // fs_resourceLoadPriority 0: the search paths, then the .pk4 files, then the resources
  if (auto text = readLoose(*relative, found, decl, maxBytes))
  {
    return text;
  }

  // GetZipCacheEntry and GetResourceCacheEntry refuse an OS path
  if (relative->find(':') != std::string::npos)
  {
    return std::nullopt;
  }
  const auto canonical = canonicalResourceName(*relative);

  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    auto& pk4 = **it;
    const auto& table = *pk4.table;
    const auto entry = table.index.find(canonical);
    if (entry == table.index.end())
    {
      continue;
    }
    auto text = readFromPk4(pk4, entry->second, *relative);
    if (text && found)
    {
      // a zip's file has time 0 (idFile_InZip::Timestamp)
      *found = GameFileProbe{GameFileProbe::Kind::Pk4, table.name, 0, text->size(), 0};
      found->containerStamp = table.stamp;
      found->containerSize = table.size;
    }
    if (text && text->size() > maxBytes)
    {
      // a zip entry inflates from its start: read whole, then cut
      text->resize(maxBytes);
    }
    return text;
  }

  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    auto& resources = **it;
    const auto& container = *resources.container;
    const auto* entry = container.find(canonical);
    if (!entry)
    {
      continue;
    }

    // the container stays open for the read batch (closeFiles); S2.2: whether it changed
    // since its table was read is looked at once, when it is opened
    const auto openContainer = [&]() -> Result<void> {
      if (resources.open)
      {
        return kdl::void_success;
      }
      return container.unchanged() | kdl::or_else([&](const auto& e) -> Result<void> {
               return Error{
                 fmt::format("{}; '{}' is not read from it", e.msg, entry->name)};
             })
             | kdl::and_then([&]() {
                 return GameFile::open(container.path())
                        | kdl::transform(
                          [&](GameFile file) { resources.open = std::move(file); });
               });
    };
    auto text = openContainer() | kdl::and_then([&]() {
                  return container.readOpened(*entry, *resources.open, maxBytes);
                })
                | kdl::transform([](std::string bytes) {
                    return std::optional<std::string>{std::move(bytes)};
                  })
                | kdl::transform_error([&](const auto& e) {
                    m_warnings.push_back(e.msg);
                    return std::optional<std::string>{};
                  })
                | kdl::value();
    if (text && found)
    {
      // an inner resource has time 0 (idFile_InnerResource)
      *found = GameFileProbe{
        GameFileProbe::Kind::Resources, container.name(), 0, entry->length, 0};
      found->containerStamp = container.fileTime();
      found->containerSize = container.fileSize();
    }
    return text;
  }

  return std::nullopt;
}

std::optional<std::string> GameFileSource::readFile(
  const std::string& path, std::int64_t* timestamp)
{
  auto found = GameFileProbe{};
  auto text = read(path, &found, true);
  if (timestamp)
  {
    // A4: the engine's time; a file that is not read has none
    *timestamp = text ? found.time : 0;
  }
  return text;
}

std::optional<std::string> GameFileSource::readGameFile(
  const std::string& path, GameFileProbe* found)
{
  return read(path, found, false);
}

std::optional<std::string> GameFileSource::readGameFileHead(
  const std::string& path, const std::size_t maxBytes, GameFileProbe* found)
{
  return read(path, found, false, maxBytes);
}

GameFileSource::LooseFolder& GameFileSource::looseFolder(
  const std::filesystem::path& folder)
{
  auto& entry = m_looseFolders[folderKey(folder)];
  const auto listed = entry.round != 0;
  if (listed && entry.round == m_looseRound + 1)
  {
    return entry;
  }

  // a new round (or the first look): the folder's time, and its names again when it
  // changed
  const auto path = m_base / folder;
  auto ec = std::error_code{};
  const auto time = std::filesystem::last_write_time(path, ec);
  const auto exists = !ec && std::filesystem::is_directory(path, ec) && !ec;
  // X.10: a folder listed within 2 s of its time may have had a file added in the same
  // tick of a coarse file-system clock (FAT, HFS+, network shares): listed again
  // Y.3: a time ahead of the clock (a share's skew) counts as listed once, not as recent
  const auto recent =
    exists && time <= entry.listed && entry.listed - time < std::chrono::seconds{2};
  if (!listed || exists != entry.exists || (exists && time != entry.time) || recent)
  {
    ++m_looseListings;
    entry.listed = std::filesystem::file_time_type::clock::now();
    entry.names.clear();
    entry.exists = exists;
    entry.time = exists ? time : std::filesystem::file_time_type{};
    if (exists)
    {
      auto listError = std::error_code{};
      for (auto it = std::filesystem::directory_iterator{path, listError};
           !listError && it != std::filesystem::directory_iterator{};
           it.increment(listError))
      {
        entry.names.insert(foldedName(it->path().filename().native()));
      }
    }
  }
  entry.round = m_looseRound + 1;
  return entry;
}

std::optional<std::filesystem::path> GameFileSource::looseFile(
  const std::filesystem::path& relative)
{
  try
  {
    const auto& folder = looseFolder(relative.parent_path());
    if (!folder.names.contains(foldedName(relative.filename().native())))
    {
      return std::nullopt;
    }
  }
  catch (...)
  {
    // a folder that cannot be listed is looked at as before
  }
  return findLooseFile(m_base / relative);
}

void GameFileSource::looseFilesMayHaveChanged()
{
  ++m_looseRound;
}

std::size_t GameFileSource::looseListingsForTesting() const
{
  return m_looseListings;
}

std::vector<std::string> GameFileSource::containerEntryNamesForTesting() const
{
  auto names = std::vector<std::string>{};
  for (const auto& resources : m_resources)
  {
    for (const auto& entry : resources->container->entries())
    {
      names.push_back(entry.name);
    }
  }
  for (const auto& pk4 : m_pk4s)
  {
    names.insert(names.end(), pk4->table->names.begin(), pk4->table->names.end());
  }
  return names;
}

GameFileProbe GameFileSource::probeFile(const std::string& path)
{
  const auto relative = checkedRelativePath(path);
  if (!relative)
  {
    return {};
  }

  // the loose file, statted: its body is not opened
  if (const auto relativePath = pathFromUtf8(*relative))
  {
    if (const auto file = looseFile(*relativePath))
    {
      if (const auto stat = statGameFile(*file))
      {
        return GameFileProbe{
          GameFileProbe::Kind::Loose, {}, stat->unixSeconds, stat->size, stat->time};
      }
    }
  }
  if (relative->find(':') != std::string::npos)
  {
    return {};
  }

  // the containers' tables, read at construction
  const auto canonical = canonicalResourceName(*relative);
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    const auto& table = *(*it)->table;
    if (table.index.count(canonical))
    {
      auto found = GameFileProbe{GameFileProbe::Kind::Pk4, table.name, 0, 0, 0};
      found.containerStamp = table.stamp;
      found.containerSize = table.size;
      return found;
    }
  }
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    const auto& container = *(*it)->container;
    if (const auto* entry = container.find(canonical))
    {
      auto found = GameFileProbe{
        GameFileProbe::Kind::Resources, container.name(), 0, entry->length, 0};
      found.containerStamp = container.fileTime();
      found.containerSize = container.fileSize();
      return found;
    }
  }
  return {};
}

GameFileProbe GameFileSource::probe(const std::string& path)
{
  return probeFile(path);
}

std::optional<std::string> GameFileSource::gameRelativePath(
  const std::string& filePath) const
{
  // the file path as the document gives it (the native narrow string), compared with the
  // base folder component by component, ignoring ASCII case on Windows, whose file
  // system ignores it; the rest joined with slashes
  try
  {
    const auto file = std::filesystem::path{filePath}.lexically_normal();
    const auto base = m_base.lexically_normal();
    auto fileIt = file.begin();
    for (const auto& part : base)
    {
      if (part.empty())
      {
        continue;
      }
      while (fileIt != file.end() && fileIt->empty())
      {
        ++fileIt;
      }
      if (fileIt == file.end())
      {
        return std::nullopt;
      }
      const auto a = pathToUtf8(part);
      const auto b = pathToUtf8(*fileIt);
#if defined(_WIN32)
      const auto same = a && b && equalsNoCaseAscii(*a, *b);
#else
      const auto same = a && b && *a == *b;
#endif
      if (!same)
      {
        return std::nullopt;
      }
      ++fileIt;
    }
    auto relative = std::string{};
    for (; fileIt != file.end(); ++fileIt)
    {
      const auto part = pathToUtf8(*fileIt);
      if (!part)
      {
        return std::nullopt;
      }
      if (part->empty())
      {
        continue;
      }
      relative += (relative.empty() ? "" : "/") + *part;
    }
    return relative.empty() ? std::nullopt : std::optional{relative};
  }
  catch (...)
  {
    return std::nullopt;
  }
}

bool GameFileSource::usingResourceFiles() const
{
  // resourceFilesFound: a container whose Init succeeded
  return !m_resources.empty();
}

std::optional<std::string> GameFileSource::readFromPk4(
  Pk4& pk4, const std::filesystem::path& entryPath, const std::string& path)
{
  try
  {
    // the .pk4 stays open for the read batch (closeFiles)
    const auto openPk4 = [&]() -> Result<void> {
      if (pk4.open)
      {
        return kdl::void_success;
      }
      return openZip(pk4.table->path)
             | kdl::transform(
               [&](std::unique_ptr<fs::ZipFileSystem> zip) { pk4.open = std::move(zip); })
             | kdl::or_else([&](const auto& e) -> Result<void> {
                 return Error{fmt::format(
                   "{}: {} cannot be opened again: {}", path, pk4.table->name, e.msg)};
               });
    };
    return openPk4() | kdl::and_then([&]() {
             return pk4.open->openFile(entryPath)
                    | kdl::or_else(
                      [&](const auto& e) -> Result<std::shared_ptr<fs::File>> {
                        return Error{fmt::format(
                          "{} cannot be read from {}: {}", path, pk4.table->name, e.msg)};
                      });
           })
           | kdl::transform([](std::shared_ptr<fs::File> file) {
               // every byte, NULs included (Reader::readString stops at the first)
               auto reader = file->reader();
               auto bytes = std::string(reader.size(), '\0');
               reader.read(bytes.data(), bytes.size());
               return std::optional<std::string>{std::move(bytes)};
             })
           | kdl::transform_error([&](const auto& e) {
               m_warnings.push_back(e.msg);
               return std::optional<std::string>{};
             })
           | kdl::value();
  }
  catch (const std::exception& e)
  {
    m_warnings.push_back(
      fmt::format("{} cannot be read from {}: {}", path, pk4.table->name, e.what()));
    return std::nullopt;
  }
}

std::vector<DeclFileTime> GameFileSource::looseFileTimes(
  const std::string& folder, const std::string& extension) const
{
  auto times = std::vector<DeclFileTime>{};
  const auto folderPath = pathFromUtf8(folder);
  if (!folderPath)
  {
    return times;
  }
  const auto base = m_base / *folderPath;
  for (const auto& name : listOsFiles(base, extension))
  {
    const auto namePath = pathFromUtf8(name);
    if (!namePath)
    {
      continue;
    }
    if (const auto stat = statGameFile(base / *namePath))
    {
      times.push_back(DeclFileTime{folder + "/" + name, stat->time, stat->size});
    }
  }
  std::sort(times.begin(), times.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.path < rhs.path;
  });
  return times;
}

std::vector<std::string> GameFileSource::takeWarnings()
{
  return std::exchange(m_warnings, {});
}

void GameFileSource::closeFiles()
{
  for (auto& pk4 : m_pk4s)
  {
    pk4->open.reset();
  }
  for (auto& resources : m_resources)
  {
    resources->open.reset();
  }
}

bool GameFileSource::changedWhileRead() const
{
  return m_changedWhileRead;
}

std::size_t GameFileSource::containerCount() const
{
  return m_pk4s.size() + m_resources.size();
}

ContainerTablesOrigin GameFileSource::containerTablesOrigin() const
{
  return m_origin;
}

bool GameFileSource::complete() const
{
  return m_complete;
}

std::shared_ptr<const GameContainerTables> GameFileSource::scannedTables() const
{
  return m_scanned;
}

void GameFileSource::recordIndexWrite(
  const double seconds, const std::uint64_t bytes, std::vector<std::string> warnings)
{
  m_origin.writeSeconds = seconds;
  m_origin.fileBytes = bytes;
  m_warnings.insert(
    m_warnings.end(),
    std::make_move_iterator(warnings.begin()),
    std::make_move_iterator(warnings.end()));
}

std::vector<std::string> GameFileSource::searchOrder() const
{
  auto order = std::vector<std::string>{};
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    order.push_back((*it)->table->name);
  }
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    order.push_back((*it)->container->name());
  }
  return order;
}

std::size_t GameFileSource::openContainers() const
{
  auto count = std::size_t{0};
  for (const auto& pk4 : m_pk4s)
  {
    count += pk4->open ? 1u : 0u;
  }
  for (const auto& resources : m_resources)
  {
    count += resources->open ? 1u : 0u;
  }
  return count;
}

} // namespace tb::lit
