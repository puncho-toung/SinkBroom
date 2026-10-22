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
#include "lit/GameFile.h"
#include "lit/ResourceContainer.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
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

void sortEnginePaths(std::vector<std::string>& names)
{
  // idSort_PathStr
  std::sort(names.begin(), names.end(), [](const auto& lhs, const auto& rhs) {
    return compareEnginePaths(lhs, rhs) < 0;
  });
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
  // as AddGameDirectory registers it: the name Sys_ListFiles lists below base
  std::string name;
  std::filesystem::path path;
  // the entries as idZipContainer::Init stores them (slashes, lowercased), in the order
  // the zip file system lists them, and the path to open each with
  std::vector<std::string> names;
  std::unordered_map<std::string, std::filesystem::path> index;
  // open during a read batch only
  std::unique_ptr<fs::ZipFileSystem> open;
};

struct GameFileSource::Resources
{
  ResourceContainer container;
  // open during a read batch only
  std::optional<GameFile> open;
};

namespace
{

Result<std::unique_ptr<fs::ZipFileSystem>> openZip(const std::filesystem::path& path)
{
  return fs::createCFile(path) | kdl::and_then([](auto file) {
           return fs::createImageFileSystem<fs::ZipFileSystem>(std::move(file));
         });
}

} // namespace

GameFileSource::GameFileSource(
  const std::filesystem::path& gamePath, const std::atomic<bool>* cancel)
  : m_base{gamePath / "base"}
  , m_cancel{cancel}
{
  // AddGameDirectory( <game path>, "base" ), FileSystem.cpp:3174-3252
  auto resourceNames = listOsFiles(m_base, ".resources", &m_warnings, m_cancel);
  sortEnginePaths(resourceNames);
  auto mapResourceNames =
    listOsFiles(m_base / "maps", ".resources", &m_warnings, m_cancel);
  sortEnginePaths(mapResourceNames);
  for (auto& name : mapResourceNames)
  {
    resourceNames.push_back("maps/" + name);
  }

  // a container listed twice (Windows lists base/maps in both passes) is kept at its
  // later place: the earlier one finds nothing the later one does not find first
  auto keep = std::vector<bool>(resourceNames.size(), true);
  for (std::size_t i = 0; i < resourceNames.size(); ++i)
  {
    for (auto k = i + 1; k < resourceNames.size(); ++k)
    {
      if (equalsNoCaseAscii(resourceNames[i], resourceNames[k]))
      {
        keep[i] = false;
        break;
      }
    }
  }

  for (std::size_t i = 0; i < resourceNames.size(); ++i)
  {
    if (cancelled())
    {
      return;
    }
    const auto path = pathFromUtf8(resourceNames[i]);
    if (!keep[i] || !path)
    {
      continue;
    }
    ResourceContainer::open(m_base / *path, resourceNames[i], m_warnings, m_cancel)
      | kdl::transform([&](ResourceContainer container) {
          m_resources.push_back(
            std::make_unique<Resources>(Resources{std::move(container), std::nullopt}));
        })
      | kdl::transform_error([&](const auto& e) {
          if (!cancelled())
          {
            m_warnings.push_back(e.msg);
          }
        });
  }

  // RB: Zip support, the base pass only
  auto pk4Names = listOsFiles(m_base, ".pk4", &m_warnings, m_cancel);
  sortEnginePaths(pk4Names);
  for (const auto& name : pk4Names)
  {
    // a .pk4's central directory is read in one call (TbFsLib), so a stop waits for one
    if (cancelled())
    {
      return;
    }
    if (auto pk4 = readPk4(name))
    {
      m_pk4s.push_back(std::move(pk4));
    }
  }
}

GameFileSource::~GameFileSource() = default;

bool GameFileSource::cancelled() const
{
  return m_cancel && m_cancel->load();
}

std::unique_ptr<GameFileSource::Pk4> GameFileSource::readPk4(const std::string& name)
{
  const auto path = pathFromUtf8(name);
  if (!path)
  {
    return nullptr;
  }

  auto pk4 = std::make_unique<Pk4>();
  pk4->name = name;
  pk4->path = m_base / *path;

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
               return binary ? nullptr : std::move(pk4);
             })
           | kdl::transform_error([&](const auto& e) {
               m_warnings.push_back(
                 fmt::format("Unable to open zip file {}: {}", name, e.msg));
               return std::unique_ptr<Pk4>{};
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
    for (const auto& entry : (*it)->container.entries())
    {
      if (auto name = flatName(entry.name, folder, extension))
      {
        add(std::move(*name));
      }
    }
  }
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    for (const auto& entryName : (*it)->names)
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
  const std::string& path, GameFileProbe* found, const bool decl)
{
  const auto relative = pathFromUtf8(path);
  if (!relative)
  {
    return std::nullopt;
  }
  const auto file = findLooseFile(m_base / *relative);
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
    auto text = lit::readGameFile(*file) | kdl::transform([](std::string bytes) {
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
    const auto stable =
      before && after && *before == *after && after->size == bytes.size();
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
        found->size = bytes.size();
        found->changeStamp = after ? after->time : 0;
      }
      return bytes;
    }
  }
  return std::nullopt;
}

std::optional<std::string> GameFileSource::read(
  const std::string& path, GameFileProbe* found, const bool decl)
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
  if (auto text = readLoose(*relative, found, decl))
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
    const auto entry = pk4.index.find(canonical);
    if (entry == pk4.index.end())
    {
      continue;
    }
    auto text = readFromPk4(pk4, entry->second, *relative);
    if (text && found)
    {
      // a zip's file has time 0 (idFile_InZip::Timestamp)
      *found = GameFileProbe{GameFileProbe::Kind::Pk4, pk4.name, 0, text->size(), 0};
    }
    return text;
  }

  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    auto& resources = **it;
    const auto* entry = resources.container.find(canonical);
    if (!entry)
    {
      continue;
    }

    // the container stays open for the read batch (closeFiles)
    const auto openContainer = [&]() -> Result<void> {
      if (resources.open)
      {
        return kdl::void_success;
      }
      return GameFile::open(resources.container.path())
             | kdl::transform([&](GameFile file) { resources.open = std::move(file); });
    };
    auto text =
      openContainer()
      | kdl::and_then([&]() { return resources.container.read(*entry, *resources.open); })
      | kdl::transform(
        [](std::string bytes) { return std::optional<std::string>{std::move(bytes)}; })
      | kdl::transform_error([&](const auto& e) {
          m_warnings.push_back(e.msg);
          return std::optional<std::string>{};
        })
      | kdl::value();
    if (text && found)
    {
      // an inner resource has time 0 (idFile_InnerResource)
      *found = GameFileProbe{
        GameFileProbe::Kind::Resources, resources.container.name(), 0, text->size(), 0};
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

GameFileProbe GameFileSource::probeFile(const std::string& path) const
{
  const auto relative = checkedRelativePath(path);
  if (!relative)
  {
    return {};
  }

  // the loose file, statted: its body is not opened
  if (const auto relativePath = pathFromUtf8(*relative))
  {
    if (const auto file = findLooseFile(m_base / *relativePath))
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
    if ((*it)->index.count(canonical))
    {
      return GameFileProbe{GameFileProbe::Kind::Pk4, (*it)->name, 0, 0, 0};
    }
  }
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    if (const auto* entry = (*it)->container.find(canonical))
    {
      return GameFileProbe{
        GameFileProbe::Kind::Resources, (*it)->container.name(), 0, entry->length, 0};
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
      return openZip(pk4.path)
             | kdl::transform(
               [&](std::unique_ptr<fs::ZipFileSystem> zip) { pk4.open = std::move(zip); })
             | kdl::or_else([&](const auto& e) -> Result<void> {
                 return Error{fmt::format(
                   "{}: {} cannot be opened again: {}", path, pk4.name, e.msg)};
               });
    };
    return openPk4() | kdl::and_then([&]() {
             return pk4.open->openFile(entryPath)
                    | kdl::or_else(
                      [&](const auto& e) -> Result<std::shared_ptr<fs::File>> {
                        return Error{fmt::format(
                          "{} cannot be read from {}: {}", path, pk4.name, e.msg)};
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
      fmt::format("{} cannot be read from {}: {}", path, pk4.name, e.what()));
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

std::optional<GameFileOrigin> GameFileSource::origin(const std::string& path) const
{
  const auto found = probeFile(path);
  switch (found.kind)
  {
  case GameFileProbe::Kind::Missing:
    return std::nullopt;
  case GameFileProbe::Kind::Loose:
    return GameFileOrigin{GameFileOrigin::Kind::Loose, {}};
  case GameFileProbe::Kind::Pk4:
    return GameFileOrigin{GameFileOrigin::Kind::Pk4, found.container};
  case GameFileProbe::Kind::Resources:
    return GameFileOrigin{GameFileOrigin::Kind::Resources, found.container};
  }
  return std::nullopt;
}

std::vector<std::string> GameFileSource::searchOrder() const
{
  auto order = std::vector<std::string>{};
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    order.push_back((*it)->name);
  }
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    order.push_back((*it)->container.name());
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
