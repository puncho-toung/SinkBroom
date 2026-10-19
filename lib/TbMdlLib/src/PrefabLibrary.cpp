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

#include "mdl/PrefabLibrary.h"

#include "base/Logger.h"
#include "base/Macros.h"
#include "base/SimpleParserStatus.h"
#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "fs/PathMatcher.h"
#include "fs/TraversalMode.h"
#include "mdl/ContentHash.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_World.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/overload.h"
#include "kd/path_utils.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/task_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace tb::mdl
{
namespace
{

const auto PayloadPrefixUser = std::string{"prefab:user/"};
const auto PayloadPrefixMod = std::string{"prefab:mod/"};

// the mod folder that holds the mod root, when the map enables no mod
const auto DefaultModName = std::string{"base"};

/**
 * Runs on a task manager worker: reads, hashes and parses one prefab file. Must not touch
 * the map or the library, and must not use the map's task manager, which runs this task.
 */
PrefabEntry readPrefabEntry(
  const PrefabRoot root,
  const std::filesystem::path& absolutePath,
  const std::filesystem::path& relativePath,
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const EntityPropertyConfig& entityPropertyConfig)
{
  auto entry = PrefabEntry{};
  entry.root = root;
  entry.relativePath = relativePath.generic_string();
  entry.absolutePath = absolutePath;
  // until the metadata is read, or when the file fails
  entry.metadata.name = absolutePath.stem().string();

  if (const auto thumbnailPath = kdl::path_replace_extension(absolutePath, ".png");
      fs::Disk::pathInfo(thumbnailPath) == fs::PathInfo::File)
  {
    entry.thumbnailPath = thumbnailPath;
  }

  fs::Disk::openFile(absolutePath) | kdl::and_then([&](auto file) {
    auto fileReader = file->reader().buffer();
    const auto bytes = fileReader.stringView();
    entry.hash = fnv1a64Hex(bytes);

    auto logger = NullLogger{};
    auto status = SimpleParserStatus{logger};
    auto taskManager = kdl::task_manager{0};
    return WorldReader::tryRead(
      bytes, {mapFormat}, worldBounds, entityPropertyConfig, status, taskManager);
  }) | kdl::transform([&](auto worldNode) { entry.world = std::move(worldNode); })
    | kdl::transform_error([&](auto e) { entry.error = std::move(e.msg); });

  return entry;
}

auto makeSetEntityDefinitionsVisitor(const EntityDefinitionManager* manager)
{
  // nullptr unsets the definitions
  const auto setEntityDefinition = [=](auto& node) {
    node.setDefinition(manager ? manager->definition(node) : nullptr);
  };

  return kdl::overload(
    [=](auto&& thisLambda, WorldNode& worldNode) {
      setEntityDefinition(worldNode);
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
    [](auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
    [=](EntityNode& entityNode) { setEntityDefinition(entityNode); },
    [](BrushNode&) {},
    [](PatchNode&) {});
}

bool isReady(const std::future<PrefabEntry>& future)
{
  return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
}

/**
 * The key by which two relative paths name the same file. Windows file names ignore case,
 * so there Torch.map and torch.map are one file.
 */
std::string pathKey(const std::filesystem::path& relativePath)
{
#ifdef _WIN32
  return kdl::str_to_lower(relativePath.generic_string());
#else
  return relativePath.generic_string();
#endif
}

bool compareEntries(const PrefabEntry& lhs, const PrefabEntry& rhs)
{
  return lhs.root != rhs.root
           ? lhs.root < rhs.root
           : lhs.relativePath.generic_string() < rhs.relativePath.generic_string();
}

} // namespace

PrefabEntry::PrefabEntry() = default;
PrefabEntry::~PrefabEntry() = default;
PrefabEntry::PrefabEntry(PrefabEntry&&) noexcept = default;
PrefabEntry& PrefabEntry::operator=(PrefabEntry&&) noexcept = default;

PrefabLibrary::PrefabLibrary(Map& map)
  : m_map{map}
{
  connectObservers();
}

PrefabLibrary::~PrefabLibrary() = default;

const std::filesystem::path& PrefabLibrary::userRoot() const
{
  return m_userRoot;
}

void PrefabLibrary::setUserRoot(std::filesystem::path userRoot)
{
  m_userRoot = std::move(userRoot);
}

std::vector<std::filesystem::path> PrefabLibrary::roots() const
{
  auto result = std::vector<std::filesystem::path>{};
  for (const auto root : {PrefabRoot::User, PrefabRoot::Mod})
  {
    if (auto rootPath = writeRootFor(root); !rootPath.empty())
    {
      result.push_back(std::move(rootPath));
    }
  }
  return result;
}

std::filesystem::path PrefabLibrary::writeRootFor(const PrefabRoot root) const
{
  switch (root)
  {
  case PrefabRoot::User:
    return m_userRoot;
  case PrefabRoot::Mod: {
    const auto& gamePath = m_map.gamePath();
    if (gamePath.empty())
    {
      return {};
    }
    // the last enabled mod is the one that wins in the game file system
    const auto mods = enabledMods(m_map);
    return gamePath / (mods.empty() ? DefaultModName : mods.back()) / "prefabs";
  }
    switchDefault();
  }
}

void PrefabLibrary::rescan()
{
  // a scan that is still running is abandoned; its tasks own everything they use
  m_pendingEntries.clear();
  m_scanning = true;

  const auto& worldNode = m_map.worldNode();
  const auto mapFormat = worldNode.mapFormat();
  const auto entityPropertyConfig = worldNode.entityPropertyConfig();
  const auto worldBounds = m_map.worldBounds();

  for (const auto root : {PrefabRoot::User, PrefabRoot::Mod})
  {
    const auto rootPath = writeRootFor(root);
    if (rootPath.empty() || fs::Disk::pathInfo(rootPath) != fs::PathInfo::Directory)
    {
      // a missing folder is an empty root
      continue;
    }

    const auto fixedRootPath = fs::Disk::fixPath(rootPath);
    fs::Disk::find(
      fixedRootPath, fs::TraversalMode::Recursive, fs::makeExtensionPathMatcher({".map"}))
      | kdl::transform([&](const auto& paths) {
          for (const auto& absolutePath : paths)
          {
            if (fs::Disk::pathInfo(absolutePath) != fs::PathInfo::File)
            {
              continue;
            }

            const auto relativePath = absolutePath.lexically_relative(fixedRootPath);
            m_pendingEntries.push_back(
              m_map.taskManager().run_task(std::function<PrefabEntry()>{[=]() {
                return readPrefabEntry(
                  root,
                  absolutePath,
                  relativePath,
                  mapFormat,
                  worldBounds,
                  entityPropertyConfig);
              }}));
          }
        })
      | kdl::transform_error([&](const auto& e) {
          m_map.logger().warn()
            << "Could not scan prefab folder " << rootPath << ": " << e.msg;
        });
  }
}

void PrefabLibrary::processPendingResults()
{
  auto changed = false;

  if (m_scanning && std::ranges::all_of(m_pendingEntries, isReady))
  {
    auto entries = std::vector<PrefabEntry>{};
    entries.reserve(m_pendingEntries.size());
    for (auto& future : m_pendingEntries)
    {
      entries.push_back(future.get());
    }
    m_pendingEntries.clear();
    m_scanning = false;

    for (auto& entry : entries)
    {
      if (entry.world)
      {
        readPrefabMetadata(*entry.world)
          | kdl::transform([&](auto metadata) { entry.metadata = std::move(metadata); })
          | kdl::transform_error([&](auto e) {
              entry.error = std::move(e.msg);
              entry.world.reset();
            });
      }
    }

    std::ranges::sort(entries, compareEntries);

    // the user root wins over the mod root for an equal relative path
    auto userPaths = std::unordered_set<std::string>{};
    for (const auto& entry : entries)
    {
      if (entry.root == PrefabRoot::User)
      {
        userPaths.insert(pathKey(entry.relativePath));
      }
    }
    std::erase_if(entries, [&](const auto& entry) {
      return entry.root == PrefabRoot::Mod
             && userPaths.contains(pathKey(entry.relativePath));
    });

    m_entries = std::move(entries);
    m_definitionsStale = true;
    changed = true;
  }

  if (m_definitionsStale)
  {
    for (auto& entry : m_entries)
    {
      setEntityDefinitions(entry);
    }
    m_definitionsStale = false;
    changed = true;
  }

  if (changed)
  {
    libraryDidChangeNotifier();
  }
}

bool PrefabLibrary::scanning() const
{
  return m_scanning;
}

const std::vector<PrefabEntry>& PrefabLibrary::entries() const
{
  return m_entries;
}

const PrefabEntry* PrefabLibrary::find(
  const PrefabRoot root, const std::filesystem::path& relativePath) const
{
  const auto key = pathKey(relativePath);
  const auto it = std::ranges::find_if(m_entries, [&](const auto& entry) {
    return entry.root == root && pathKey(entry.relativePath) == key;
  });
  return it != m_entries.end() ? &*it : nullptr;
}

std::string PrefabLibrary::payloadFor(const PrefabEntry& entry) const
{
  return (entry.root == PrefabRoot::User ? PayloadPrefixUser : PayloadPrefixMod)
         + entry.relativePath.generic_string();
}

const PrefabEntry* PrefabLibrary::fromPayload(const std::string& payload) const
{
  if (kdl::cs::str_is_prefix(payload, PayloadPrefixUser))
  {
    return find(PrefabRoot::User, payload.substr(PayloadPrefixUser.size()));
  }
  if (kdl::cs::str_is_prefix(payload, PayloadPrefixMod))
  {
    return find(PrefabRoot::Mod, payload.substr(PayloadPrefixMod.size()));
  }
  return nullptr;
}

std::vector<Node*> PrefabLibrary::cloneBody(
  const PrefabEntry& entry, const vm::bbox3d& worldBounds) const
{
  if (!entry.world)
  {
    return {};
  }

  // A world cannot give up its default layer, so the body is cloned from the layers'
  // children rather than dissolved; the worldspawn's own brushes are in the default
  // layer too, and a well-formed file has nothing but the one entity or group.
  const auto& worldNode = std::as_const(*entry.world);
  auto result = std::vector<Node*>{};
  for (const auto* layerNode : worldNode.allLayers())
  {
    for (const auto* node : layerNode->children())
    {
      result.push_back(node->cloneRecursively(worldBounds));
    }
  }

  rekeyLinkIds(result);
  return result;
}

void PrefabLibrary::connectObservers()
{
  // Connected before the map connects its own observers (see Map.h), so these run first.
  m_notifierConnection += m_map.entityDefinitionsWillChangeNotifier.connect(
    this, &PrefabLibrary::entityDefinitionsWillChange);
  m_notifierConnection += m_map.entityDefinitionsDidChangeNotifier.connect(
    this, &PrefabLibrary::entityDefinitionsDidChange);
}

void PrefabLibrary::entityDefinitionsWillChange()
{
  // the map deletes the definitions right after this
  for (auto& entry : m_entries)
  {
    if (entry.world)
    {
      entry.world->accept(makeSetEntityDefinitionsVisitor(nullptr));
    }
  }
}

void PrefabLibrary::entityDefinitionsDidChange()
{
  // this runs before the map has loaded the new definitions
  m_definitionsStale = true;
}

void PrefabLibrary::setEntityDefinitions(PrefabEntry& entry) const
{
  if (entry.world)
  {
    entry.world->accept(
      makeSetEntityDefinitionsVisitor(&m_map.entityDefinitionManager()));
  }
}

} // namespace tb::mdl
