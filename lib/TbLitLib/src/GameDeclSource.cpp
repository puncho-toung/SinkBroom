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

#include "lit/GameDeclSource.h"

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

struct GameDeclSource::Pk4
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

struct GameDeclSource::Resources
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

GameDeclSource::GameDeclSource(
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

GameDeclSource::~GameDeclSource() = default;

bool GameDeclSource::cancelled() const
{
  return m_cancel && m_cancel->load();
}

std::unique_ptr<GameDeclSource::Pk4> GameDeclSource::readPk4(const std::string& name)
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
    auto zip = openZip(pk4->path);
    if (zip.is_error())
    {
      m_warnings.push_back(fmt::format(
        "Unable to open zip file {}: {}", name, std::get<Error>(zip.error()).msg));
      return nullptr;
    }
    auto zipFileSystem = std::move(zip).value();

    auto paths = zipFileSystem->find("", fs::TraversalMode::Recursive);
    if (paths.is_error())
    {
      m_warnings.push_back(fmt::format(
        "Unable to open zip file {}: {}", name, std::get<Error>(paths.error()).msg));
      return nullptr;
    }

    auto binary = false;
    for (const auto& entryPath : paths.value())
    {
      if (
        static_cast<const fs::FileSystem&>(*zipFileSystem).pathInfo(entryPath)
        != fs::PathInfo::File)
      {
        continue;
      }
      // G3: a name that is not valid Unicode is skipped, never thrown for
      const auto utf8 = pathToUtf8(entryPath);
      if (!utf8)
      {
        m_warnings.push_back(fmt::format(
          "Zip file {}: an entry name is not valid Unicode; it is skipped", name));
        continue;
      }
      const auto entryName = canonicalResourceName(*utf8);
      binary = binary || entryName == BinaryConfig;
      if (pk4->index.emplace(entryName, entryPath).second)
      {
        pk4->names.push_back(entryName);
      }
    }
    // "ignore all binary paks", without a message
    return binary ? nullptr : std::move(pk4);
  }
  catch (const std::exception& e)
  {
    m_warnings.push_back(fmt::format("Unable to open zip file {}: {}", name, e.what()));
    return nullptr;
  }
}

std::vector<std::string> GameDeclSource::listFiles(
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

std::optional<std::string> GameDeclSource::readLoose(
  const std::string& path, std::int64_t* timestamp)
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
  // read is tried again, and a file that keeps changing asks for another build
  for (auto attempt = 0; attempt < MaxLooseReadAttempts; ++attempt)
  {
    const auto before = statGameFile(*file);
    auto text = readGameFile(*file);
    if (text.is_error())
    {
      // OpenFileReadFlags goes on to the containers without a message, and so does this
      return std::nullopt;
    }
    const auto after = statGameFile(*file);
    auto bytes = std::move(text).value();
    const auto stable =
      before && after && *before == *after && after->size == bytes.size();
    if (stable || attempt + 1 == MaxLooseReadAttempts)
    {
      if (!stable)
      {
        m_changedWhileRead = true;
        m_warnings.push_back(
          fmt::format("{} changed while it was read; it is read again later", path));
      }
      if (timestamp)
      {
        *timestamp = after ? after->time : 0;
      }
      return bytes;
    }
  }
  return std::nullopt;
}

std::optional<std::string> GameDeclSource::readFile(
  const std::string& path, std::int64_t* timestamp)
{
  if (timestamp)
  {
    *timestamp = 0;
  }
  const auto relative = checkedRelativePath(path);
  if (!relative)
  {
    return std::nullopt;
  }

  // fs_resourceLoadPriority 0: the search paths, then the .pk4 files, then the resources
  if (auto text = readLoose(*relative, timestamp))
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
    return readFromPk4(pk4, entry->second, *relative);
  }

  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    auto& resources = **it;
    const auto* entry = resources.container.find(canonical);
    if (!entry)
    {
      continue;
    }

    if (!resources.open)
    {
      auto opened = GameFile::open(resources.container.path());
      if (opened.is_error())
      {
        m_warnings.push_back(std::get<Error>(opened.error()).msg);
        return std::nullopt;
      }
      resources.open = std::move(opened).value();
    }
    auto text = resources.container.read(*entry, *resources.open);
    if (text.is_error())
    {
      m_warnings.push_back(std::get<Error>(text.error()).msg);
      return std::nullopt;
    }
    return std::move(text).value();
  }

  return std::nullopt;
}

std::optional<std::string> GameDeclSource::readFromPk4(
  Pk4& pk4, const std::filesystem::path& entryPath, const std::string& path)
{
  try
  {
    if (!pk4.open)
    {
      auto zip = openZip(pk4.path);
      if (zip.is_error())
      {
        m_warnings.push_back(fmt::format(
          "{}: {} cannot be opened again: {}",
          path,
          pk4.name,
          std::get<Error>(zip.error()).msg));
        return std::nullopt;
      }
      pk4.open = std::move(zip).value();
    }

    auto file = pk4.open->openFile(entryPath);
    if (file.is_error())
    {
      m_warnings.push_back(fmt::format(
        "{} cannot be read from {}: {}",
        path,
        pk4.name,
        std::get<Error>(file.error()).msg));
      return std::nullopt;
    }
    auto reader = std::move(file).value()->reader();
    return reader.readString(reader.size());
  }
  catch (const std::exception& e)
  {
    m_warnings.push_back(
      fmt::format("{} cannot be read from {}: {}", path, pk4.name, e.what()));
    return std::nullopt;
  }
}

std::vector<DeclFileTime> GameDeclSource::looseFileTimes(
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

std::vector<std::string> GameDeclSource::takeWarnings()
{
  return std::exchange(m_warnings, {});
}

void GameDeclSource::closeFiles()
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

bool GameDeclSource::changedWhileRead() const
{
  return m_changedWhileRead;
}

std::optional<GameFileOrigin> GameDeclSource::origin(const std::string& path) const
{
  const auto relative = checkedRelativePath(path);
  if (!relative)
  {
    return std::nullopt;
  }
  const auto relativePath = pathFromUtf8(*relative);
  if (relativePath && findLooseFile(m_base / *relativePath))
  {
    return GameFileOrigin{GameFileOrigin::Kind::Loose, {}};
  }
  if (relative->find(':') != std::string::npos)
  {
    return std::nullopt;
  }

  const auto canonical = canonicalResourceName(*relative);
  for (auto it = m_pk4s.rbegin(); it != m_pk4s.rend(); ++it)
  {
    if ((*it)->index.count(canonical))
    {
      return GameFileOrigin{GameFileOrigin::Kind::Pk4, (*it)->name};
    }
  }
  for (auto it = m_resources.rbegin(); it != m_resources.rend(); ++it)
  {
    if ((*it)->container.find(canonical))
    {
      return GameFileOrigin{GameFileOrigin::Kind::Resources, (*it)->container.name()};
    }
  }
  return std::nullopt;
}

std::vector<std::string> GameDeclSource::searchOrder() const
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

std::size_t GameDeclSource::openContainers() const
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
