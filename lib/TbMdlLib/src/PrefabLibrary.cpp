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
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/ContentHash.h"
#include "mdl/EntityColorPropertyValue.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_World.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/hash_utils.h"
#include "kd/overload.h"
#include "kd/path_utils.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/task_manager.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tb::mdl
{
namespace
{

// C4; a placement's prefab path is the payload without it (DD-16)
const auto PayloadPrefix = std::string{"prefab:"};
const auto PayloadPrefixUser = std::string{"prefab:user/"};
const auto PayloadPrefixMod = std::string{"prefab:mod/"};

// the mod folder that holds the mod root, when the map enables no mod
const auto DefaultModName = std::string{"base"};

// the scan runs beside the map's own task manager, so it gets few workers
constexpr auto WorkerCount = std::size_t(2);

/**
 * The path as UTF-8 with forward slashes. Unlike generic_string, this never depends on
 * the process's code page, and never throws for a character outside it.
 */
std::string toUtf8String(const std::filesystem::path& path)
{
  const auto str = path.generic_u8string();
  return std::string{str.begin(), str.end()};
}

/**
 * The key by which two relative paths name the same file. Windows file names ignore
 * case, so there Torch.map and torch.map are one file. Only ASCII is lowered.
 */
std::string makePathKey(const std::string& relativePathString)
{
#ifdef _WIN32
  return kdl::str_to_lower(relativePathString);
#else
  return relativePathString;
#endif
}

/**
 * The path key of the relative path in a payload, which is UTF-8 with forward slashes, as
 * makePathKey makes it from the path that the payload names; on Windows, a backslash
 * is a separator there too.
 */
std::string makePayloadPathKey(const std::string_view relativePathString)
{
#ifdef _WIN32
  auto result = kdl::str_to_lower(relativePathString);
  std::ranges::replace(result, '\\', '/');
  return result;
#else
  return std::string{relativePathString};
#endif
}

// the same rule for absolute paths, which never leave the worker that lists the roots
std::filesystem::path makeFileKey(const std::filesystem::path& path)
{
#ifdef _WIN32
  return kdl::path_to_lower(path);
#else
  return path;
#endif
}

/**
 * DD-27: the autosave folder of a map saved in a root holds backups, not prefabs, so
 * neither it nor anything under it is listed; its name is matched in any case.
 */
bool isAutosaveFolder(const std::filesystem::path& folderName)
{
  return kdl::path_to_lower(folderName) == "autosave";
}

bool isInAutosaveFolder(const std::filesystem::path& relativePath)
{
  const auto folders = relativePath.parent_path();
  return std::any_of(folders.begin(), folders.end(), isAutosaveFolder);
}

bool isStale(
  const std::atomic<std::uint64_t>& currentGeneration, const std::uint64_t generation)
{
  return currentGeneration.load() != generation;
}

/**
 * Reads and hashes one prefab file and parses it. Never touches the map or the library;
 * runs on a worker or, for cloneBody, on the GUI thread.
 */
Result<std::unique_ptr<WorldNode>> readPrefabWorld(
  const std::filesystem::path& absolutePath,
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const EntityPropertyConfig& entityPropertyConfig,
  std::string& hash)
{
  return fs::Disk::openFile(absolutePath) | kdl::and_then([&](auto file) {
           auto fileReader = file->reader().buffer();
           const auto bytes = fileReader.stringView();
           // a checkout with other line endings is the same prefab (DD-12)
           hash = fnv1a64HexNormalizedLineEndings(bytes);

           auto logger = NullLogger{};
           auto status = SimpleParserStatus{logger};
           // a nested task manager would wait on the workers that run this task
           auto taskManager = kdl::task_manager{0};
           // nothing queries a library world spatially, so it gets no node tree
           return WorldReader::tryRead(
             bytes,
             {mapFormat},
             worldBounds,
             entityPropertyConfig,
             status,
             taskManager,
             WorldReaderNodeTree::Skip);
         });
}

/**
 * The prefab's body: every node in the world's layers, which for a well-formed file is
 * the one entity or the one group. A world cannot give up its default layer, so a body is
 * cloned from the layers' children rather than dissolved.
 */
std::vector<const Node*> bodyNodes(const WorldNode& worldNode)
{
  auto result = std::vector<const Node*>{};
  for (const auto* layerNode : worldNode.allLayers())
  {
    for (const auto* node : layerNode->children())
    {
      result.push_back(node);
    }
  }
  return result;
}

std::size_t countBrushesAndPatches(const WorldNode& worldNode)
{
  auto count = std::size_t(0);
  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const GroupNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const EntityNode& node) { node.visitChildren(thisLambda); },
    [&](const BrushNode&) { ++count; },
    [&](const PatchNode&) { ++count; }));
  return count;
}

// the key whose colour a light's cell takes, as the browser does for a kept world
const auto LightColorKey = std::string{"_color"};

bool isLightClassname(const std::string& classname)
{
  return kdl::cs::str_is_prefix(classname, "light");
}

const EntityNode* firstEntityNode(const std::vector<const Node*>& nodes)
{
  auto result = static_cast<const EntityNode*>(nullptr);
  const auto visitor = kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const GroupNode& node) {
      if (!result)
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](const EntityNode& node) {
      if (!result)
      {
        result = &node;
      }
    },
    [](const BrushNode&) {},
    [](const PatchNode&) {});

  for (const auto* node : nodes)
  {
    node->accept(visitor);
  }
  return result;
}

/**
 * The bounds of the body and its first PrefabPreviewMaxEdges brush edges and patch grid
 * lines, which is what a browser cell draws.
 */
PrefabBodyPreview makeBodyPreview(const WorldNode& worldNode)
{
  auto preview = PrefabBodyPreview{};
  auto& vertices = preview.edgeVertices;
  const auto full = [&]() { return vertices.size() / 2 >= PrefabPreviewMaxEdges; };
  const auto addEdge = [&](const vm::vec3d& start, const vm::vec3d& end) {
    if (!full())
    {
      vertices.emplace_back(start);
      vertices.emplace_back(end);
    }
  };

  const auto nodes = bodyNodes(worldNode);
  if (!nodes.empty())
  {
    preview.bounds = nodes.front()->logicalBounds();
    for (const auto* node : nodes)
    {
      preview.bounds = vm::merge(preview.bounds, node->logicalBounds());
    }
  }

  const auto visitor = kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const GroupNode& node) {
      if (!full())
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const EntityNode& node) {
      if (!full())
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](const BrushNode& brushNode) {
      for (const auto* edge : brushNode.brush().edges())
      {
        if (full())
        {
          return;
        }
        const auto segment = edge->segment();
        addEdge(segment.start(), segment.end());
      }
    },
    [&](const PatchNode& patchNode) {
      const auto& grid = patchNode.grid();
      for (size_t row = 0; row < grid.pointRowCount && !full(); ++row)
      {
        for (size_t col = 0; col < grid.pointColumnCount; ++col)
        {
          const auto& position = grid.point(row, col).position;
          if (col + 1 < grid.pointColumnCount)
          {
            addEdge(position, grid.point(row, col + 1).position);
          }
          if (row + 1 < grid.pointRowCount)
          {
            addEdge(position, grid.point(row + 1, col).position);
          }
        }
      }
    });

  for (const auto* node : nodes)
  {
    node->accept(visitor);
  }

  // DD-21: what the cell colour is made of, since the world is dropped before the
  // library assigns the entity definitions
  if (const auto* entityNode = firstEntityNode(nodes))
  {
    const auto& entity = entityNode->entity();
    preview.firstEntityClassname = entity.classname();
    if (isLightClassname(preview.firstEntityClassname))
    {
      if (const auto* value = entity.property(LightColorKey))
      {
        preview.firstEntityLightColor = *value;
      }
    }
  }
  return preview;
}

/**
 * The colour of a cell whose world was dropped, from the entity definitions of the map:
 * the light's _color, else the first entity's definition colour.
 */
std::optional<RgbaF> bodyPreviewColor(
  const PrefabBodyPreview& preview, const EntityDefinitionManager* manager)
{
  if (preview.firstEntityClassname.empty())
  {
    return std::nullopt;
  }

  const auto* definition =
    manager ? manager->definition(preview.firstEntityClassname) : nullptr;
  if (preview.firstEntityLightColor)
  {
    if (const auto color = parseEntityColorPropertyValue(
          definition, LightColorKey, *preview.firstEntityLightColor);
        color.is_success())
    {
      return color.value().color.to<RgbaF>();
    }
  }
  return definition ? std::optional{definition->color.to<RgbaF>()} : std::nullopt;
}

/**
 * Caller-owned, re-keyed clones of the body of the given world.
 */
std::vector<Node*> cloneBodyNodes(
  const WorldNode& worldNode, const vm::bbox3d& worldBounds)
{
  // owned until every clone is made and re-keyed, so that a throw leaks none of them
  auto clones = std::vector<std::unique_ptr<Node>>{};
  for (const auto* node : bodyNodes(worldNode))
  {
    clones.emplace_back(node->cloneRecursively(worldBounds));
  }

  auto result = clones
                | std::views::transform([](const auto& clone) { return clone.get(); })
                | kdl::ranges::to<std::vector>();
  rekeyLinkIds(result);

  // the caller owns them now
  for (auto& clone : clones)
  {
    static_cast<void>(clone.release());
  }
  return result;
}

/**
 * The body of the given world, taken out of it: caller-owned and re-keyed, like the
 * clones of cloneBodyNodes, but without a copy. A clone has no persistent group ID and no
 * file position, so the nodes lose theirs.
 */
std::vector<Node*> detachBodyNodes(WorldNode& worldNode)
{
  auto detached = std::vector<std::unique_ptr<Node>>{};
  for (auto* layerNode : worldNode.allLayers())
  {
    // copied, since removing a child changes the list
    for (auto* node : std::vector<Node*>{layerNode->children()})
    {
      layerNode->removeChild(node);
      detached.emplace_back(node);
    }
  }

  auto result = detached
                | std::views::transform([](const auto& node) { return node.get(); })
                | kdl::ranges::to<std::vector>();
  Node::visitAll(
    result,
    kdl::overload(
      [](auto&& thisLambda, WorldNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, LayerNode& node) { node.visitChildren(thisLambda); },
      [](auto&& thisLambda, GroupNode& node) {
        node.resetPersistentId();
        node.setFilePosition(0, 0);
        node.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, EntityNode& node) {
        node.setFilePosition(0, 0);
        node.visitChildren(thisLambda);
      },
      [](BrushNode& node) { node.setFilePosition(0, 0); },
      [](PatchNode& node) { node.setFilePosition(0, 0); }));
  rekeyLinkIds(result);

  // the caller owns them now
  for (auto& node : detached)
  {
    static_cast<void>(node.release());
  }
  return result;
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

template <typename T>
bool isReady(const std::future<T>& future)
{
  return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
}

// one key for a root and a relative path
std::string entryKey(const PrefabRoot root, const std::string& pathKey)
{
  return (root == PrefabRoot::User ? "u:" : "m:") + pathKey;
}

bool compareEntries(
  const std::unique_ptr<PrefabEntry>& lhs, const std::unique_ptr<PrefabEntry>& rhs)
{
  return lhs->root != rhs->root ? lhs->root < rhs->root
                                : lhs->relativePathString < rhs->relativePathString;
}

/**
 * Whether the file of an entry that failed was read again and failed in the same way:
 * the same file, unchanged, the same error. A scan then keeps the old entry.
 */
bool failsAsBefore(const PrefabEntry& previousEntry, const PrefabEntry& entry)
{
  return previousEntry.error && entry.error && *previousEntry.error == *entry.error
         && previousEntry.absolutePath == entry.absolutePath
         && previousEntry.fileSize == entry.fileSize
         && previousEntry.modificationTime == entry.modificationTime
         && previousEntry.hash == entry.hash;
}

} // namespace

PrefabEntry::PrefabEntry() = default;
PrefabEntry::~PrefabEntry() = default;
PrefabEntry::PrefabEntry(PrefabEntry&&) noexcept = default;
PrefabEntry& PrefabEntry::operator=(PrefabEntry&&) noexcept = default;

bool PrefabEntry::hasBody() const
{
  return world || bodyPreview;
}

namespace
{

/**
 * Runs on a worker: reads, hashes and parses one prefab file and reads its metadata.
 * Must not touch the map or the library. Never throws: an exception, which the task
 * manager would not catch, becomes the entry's error.
 */
PrefabEntry readPrefabEntry(
  const PrefabRoot root,
  const std::filesystem::path& absolutePath,
  const std::filesystem::path& relativePath,
  const std::uintmax_t fileSize,
  const std::filesystem::file_time_type modificationTime,
  const std::optional<std::filesystem::path>& thumbnailPath,
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const EntityPropertyConfig& entityPropertyConfig,
  const std::atomic<std::uint64_t>& currentGeneration,
  const std::uint64_t generation,
  const PrefabLibrary::ReadHook& readHook)
{
  auto entry = PrefabEntry{};
  entry.root = root;
  entry.relativePath = relativePath;
  entry.absolutePath = absolutePath;
  entry.thumbnailPath = thumbnailPath;
  entry.fileSize = fileSize;
  entry.modificationTime = modificationTime;

  try
  {
    entry.relativePathString = toUtf8String(relativePath);
    entry.pathKey = makePathKey(entry.relativePathString);
    // until the metadata is read, or when the file fails
    entry.metadata.name = toUtf8String(absolutePath.stem());

    if (isStale(currentGeneration, generation))
    {
      entry.error = "The scan was abandoned";
      return entry;
    }

    if (readHook)
    {
      readHook(absolutePath);
    }

    readPrefabWorld(
      absolutePath, mapFormat, worldBounds, entityPropertyConfig, entry.hash)
      | kdl::transform([&](auto worldNode) { entry.world = std::move(worldNode); })
      | kdl::transform_error([&](auto e) { entry.error = std::move(e.msg); });

    if (entry.world)
    {
      readPrefabMetadata(*entry.world)
        | kdl::transform([&](auto metadata) { entry.metadata = std::move(metadata); })
        | kdl::transform_error([&](auto e) {
            entry.error = std::move(e.msg);
            entry.world.reset();
          });
    }

    if (
      entry.world && entry.metadata.kind == PrefabKind::Assembly
      && countBrushesAndPatches(*entry.world) > PrefabWorldBrushBudget)
    {
      // DD-19: a large assembly keeps what its cell needs; cloneBody reads it again
      entry.bodyPreview = makeBodyPreview(*entry.world);
      entry.world.reset();
    }
  }
  catch (const std::exception& e)
  {
    entry.world.reset();
    entry.bodyPreview.reset();
    entry.error = e.what();
  }

  return entry;
}

} // namespace

PrefabLibrary::PrefabLibrary(Map& map)
  : m_map{map}
  , m_generation{std::make_shared<std::atomic<std::uint64_t>>(0)}
{
  connectObservers();
}

PrefabLibrary::~PrefabLibrary()
{
  // tasks that have not started yet return at once
  ++*m_generation;
}

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
    // DD-32: a set Prefab Path replaces the mod root
    const auto folder = modFolder();
    return m_userRoot.empty() && !folder.empty() ? folder / "maps" / "prefabs"
                                                 : std::filesystem::path{};
  }
    switchDefault();
  }
}

std::optional<PrefabRoot> PrefabLibrary::writeRoot() const
{
  for (const auto root : {PrefabRoot::User, PrefabRoot::Mod})
  {
    if (!writeRootFor(root).empty())
    {
      return root;
    }
  }
  return std::nullopt;
}

std::filesystem::path PrefabLibrary::modFolder() const
{
  const auto& gamePath = m_map.gamePath();
  if (gamePath.empty())
  {
    return {};
  }
  // the last enabled mod is the one that wins in the game file system
  const auto mods = enabledMods(m_map);
  return gamePath / (mods.empty() ? DefaultModName : mods.back());
}

void PrefabLibrary::rescan()
{
  // a scan that is still running is abandoned; its tasks own everything they use and
  // return at once if they have not started
  const auto generation = ++*m_generation;
  m_pendingListing.reset();
  m_pendingEntries.clear();
  m_reusedEntries.clear();
  m_pendingDirectories.clear();
  m_pendingUpdates.clear();
  m_updateSequences.clear();
  m_scanning = true;

  auto rootPaths = std::vector<std::pair<PrefabRoot, std::filesystem::path>>{};
  for (const auto root : {PrefabRoot::User, PrefabRoot::Mod})
  {
    if (auto rootPath = writeRootFor(root); !rootPath.empty())
    {
      rootPaths.emplace_back(root, std::move(rootPath));
    }
  }

  if (rootPaths.empty())
  {
    // finished at the next processPendingResults
    return;
  }

  // phase 1: list the roots on a worker; processPendingResults starts phase 2
  m_pendingListing = taskManager().run_task(
    std::function<Listing()>{[rootPaths, currentGeneration = m_generation, generation]() {
      auto listing = Listing{};
      for (const auto& [root, rootPath] : rootPaths)
      {
        if (isStale(*currentGeneration, generation))
        {
          return Listing{};
        }

        try
        {
          auto error = std::error_code{};
          const auto fixedRootPath = fs::Disk::fixPath(rootPath);
          if (!std::filesystem::is_directory(fixedRootPath, error))
          {
            // a missing folder is an empty root
            continue;
          }
          listing.directories.push_back(fixedRootPath.lexically_normal());

          // the directory entries carry the file type, size and time, so listing a
          // root costs no stat per file
          auto files = std::vector<ListedFile>{};
          auto thumbnails = std::set<std::filesystem::path>{};
          auto it = std::filesystem::recursive_directory_iterator{
            fixedRootPath,
            std::filesystem::directory_options::follow_directory_symlink,
            error};
          const auto end = std::filesystem::recursive_directory_iterator{};
          while (!error && it != end)
          {
            const auto& directoryEntry = *it;
            auto entryError = std::error_code{};
            if (directoryEntry.is_directory(entryError))
            {
              if (isAutosaveFolder(directoryEntry.path().filename()))
              {
                it.disable_recursion_pending();
              }
              else
              {
                // watched by the panel, so that a file copied into an empty folder is
                // seen
                listing.directories.push_back(directoryEntry.path().lexically_normal());
              }
            }
            else if (directoryEntry.is_regular_file(entryError))
            {
              const auto& absolutePath = directoryEntry.path();
              const auto extension = kdl::path_to_lower(absolutePath.extension());
              if (extension == ".png")
              {
                thumbnails.insert(makeFileKey(absolutePath));
              }
              else if (extension == ".map")
              {
                auto file = ListedFile{};
                file.root = root;
                file.absolutePath = absolutePath;
                file.relativePath = absolutePath.lexically_relative(fixedRootPath);
                file.relativePathString = toUtf8String(file.relativePath);
                file.pathKey = makePathKey(file.relativePathString);
                file.fileSize = directoryEntry.file_size(entryError);
                file.modificationTime = directoryEntry.last_write_time(entryError);
                files.push_back(std::move(file));
              }
            }
            it.increment(error);
          }

          if (error)
          {
            listing.warnings.push_back(fmt::format(
              "Could not scan prefab folder {}: {}", rootPath, error.message()));
          }

          for (auto& file : files)
          {
            if (auto thumbnailPath =
                  kdl::path_replace_extension(file.absolutePath, ".png");
                thumbnails.contains(makeFileKey(thumbnailPath)))
            {
              file.thumbnailPath = std::move(thumbnailPath);
            }
            listing.files.push_back(std::move(file));
          }
        }
        catch (const std::exception& e)
        {
          listing.warnings.push_back(
            fmt::format("Could not scan prefab folder {}: {}", rootPath, e.what()));
        }
      }
      return listing;
    }});
}

void PrefabLibrary::updateEntry(const std::filesystem::path& absolutePath)
{
  const auto path = absolutePath.lexically_normal();
  for (const auto root : {PrefabRoot::User, PrefabRoot::Mod})
  {
    const auto rootPath = writeRootFor(root);
    if (rootPath.empty())
    {
      continue;
    }

    const auto relativePath = path.lexically_relative(rootPath.lexically_normal());
    if (
      relativePath.empty() || relativePath == "." || kdl::path_front(relativePath) == ".."
      || isInAutosaveFolder(relativePath))
    {
      continue;
    }

    // R2-pf3-1: only a file under a root restarts a running scan
    if (m_scanning)
    {
      // the scan restarts and lists the file with the others
      rescan();
      return;
    }

    auto error = std::error_code{};
    if (!std::filesystem::is_regular_file(path, error))
    {
      // a removed file may uncover a file of the mod root
      rescan();
      return;
    }

    const auto fileSize = std::filesystem::file_size(path, error);
    const auto modificationTime = std::filesystem::last_write_time(path, error);
    auto thumbnailPath = std::optional{kdl::path_replace_extension(path, ".png")};
    if (!std::filesystem::is_regular_file(*thumbnailPath, error))
    {
      thumbnailPath = std::nullopt;
    }

    // R2-pf-mdl-6: two updates of one file run on two workers, so the first may finish
    // last; only the one that was started last is taken over
    auto key = entryKey(root, makePathKey(toUtf8String(relativePath)));
    const auto sequence = ++m_nextUpdateSequence;
    m_updateSequences[key] = sequence;

    const auto& worldNode = m_map.worldNode();
    m_pendingUpdates.push_back(PendingUpdate{
      std::move(key),
      sequence,
      taskManager().run_task(std::function<PrefabEntry()>{
        [root,
         path,
         relativePath,
         fileSize,
         modificationTime,
         thumbnailPath,
         mapFormat = worldNode.mapFormat(),
         entityPropertyConfig = worldNode.entityPropertyConfig(),
         worldBounds = m_map.worldBounds(),
         currentGeneration = m_generation,
         generation = m_generation->load(),
         readHook = m_readHook]() {
          return readPrefabEntry(
            root,
            path,
            relativePath,
            fileSize,
            modificationTime,
            thumbnailPath,
            mapFormat,
            worldBounds,
            entityPropertyConfig,
            *currentGeneration,
            generation,
            readHook);
        }})});
    return;
  }
}

void PrefabLibrary::processPendingResults()
{
  auto changed = false;

  if (m_pendingListing && isReady(*m_pendingListing))
  {
    auto listing = m_pendingListing->get();
    m_pendingListing.reset();
    for (const auto& warning : listing.warnings)
    {
      m_map.logger().warn() << warning;
    }
    startParsing(std::move(listing));
  }

  if (
    m_scanning && !m_pendingListing
    && std::ranges::all_of(m_pendingEntries, isReady<PrefabEntry>))
  {
    changed = integrateScan() || changed;
  }

  for (auto it = m_pendingUpdates.begin(); it != m_pendingUpdates.end();)
  {
    if (isReady(it->future))
    {
      auto entry = std::make_unique<PrefabEntry>(it->future.get());
      if (const auto sequenceIt = m_updateSequences.find(it->key);
          sequenceIt != m_updateSequences.end() && sequenceIt->second == it->sequence)
      {
        m_updateSequences.erase(sequenceIt);
        changed = integrateUpdate(std::move(entry)) || changed;
      }
      // else a later update of the same file was started, and its result wins
      it = m_pendingUpdates.erase(it);
    }
    else
    {
      ++it;
    }
  }

  if (m_definitionsStale)
  {
    for (auto& entry : m_entries)
    {
      setEntityDefinitions(*entry);
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
  return m_scanning || !m_pendingUpdates.empty();
}

const std::vector<std::unique_ptr<PrefabEntry>>& PrefabLibrary::entries() const
{
  return m_entries;
}

const std::vector<std::filesystem::path>& PrefabLibrary::directories() const
{
  return m_directories;
}

const PrefabEntry* PrefabLibrary::find(
  const PrefabRoot root, const std::filesystem::path& relativePath) const
{
  const auto it =
    m_entriesByKey.find(entryKey(root, makePathKey(toUtf8String(relativePath))));
  return it != m_entriesByKey.end() ? it->second : nullptr;
}

std::string PrefabLibrary::payloadFor(const PrefabEntry& entry) const
{
  return (entry.root == PrefabRoot::User ? PayloadPrefixUser : PayloadPrefixMod)
         + entry.relativePathString;
}

const PrefabEntry* PrefabLibrary::fromPayload(const std::string& payload) const
{
  // payloads are UTF-8, whatever the process's code page is, so the key is made from the
  // text, without a path in between (R2-pf-ui-6)
  const auto lookUp = [&](const PrefabRoot root, const std::string& prefix) {
    const auto it = m_entriesByKey.find(entryKey(
      root, makePayloadPathKey(std::string_view{payload}.substr(prefix.size()))));
    return it != m_entriesByKey.end() ? it->second : nullptr;
  };

  if (kdl::cs::str_is_prefix(payload, PayloadPrefixUser))
  {
    return lookUp(PrefabRoot::User, PayloadPrefixUser);
  }
  if (kdl::cs::str_is_prefix(payload, PayloadPrefixMod))
  {
    return lookUp(PrefabRoot::Mod, PayloadPrefixMod);
  }
  return nullptr;
}

std::vector<Node*> PrefabLibrary::cloneBody(
  const PrefabEntry& entry, const vm::bbox3d& worldBounds) const
{
  if (entry.world)
  {
    return cloneBodyNodes(*entry.world, worldBounds);
  }
  if (!entry.bodyPreview)
  {
    return {};
  }

  // DD-19: the library dropped this world; read the file again
  auto result = std::vector<Node*>{};
  try
  {
    const auto& worldNode = m_map.worldNode();
    auto hash = std::string{};
    readPrefabWorld(
      entry.absolutePath,
      worldNode.mapFormat(),
      worldBounds,
      worldNode.entityPropertyConfig(),
      hash)
      | kdl::transform([&](auto prefabWorldNode) {
          if (hash != entry.hash)
          {
            m_map.logger().warn()
              << "Cannot read prefab '" << entry.relativePathString
              << "': the file has changed since the library was read; rescan it";
            return;
          }
          prefabWorldNode->accept(
            makeSetEntityDefinitionsVisitor(&m_map.entityDefinitionManager()));
          // R2-pf-mdl-2: the world is thrown away, so its nodes are taken, not copied
          result = detachBodyNodes(*prefabWorldNode);
        })
      | kdl::transform_error([&](const auto& e) {
          m_map.logger().warn()
            << "Cannot read prefab '" << entry.relativePathString << "': " << e.msg;
        });
  }
  catch (const std::exception& e)
  {
    m_map.logger().warn() << "Cannot read prefab '" << entry.relativePathString
                          << "': " << e.what();
  }
  return result;
}

std::vector<PrefabPlacement> PrefabLibrary::placements() const
{
  auto result = std::vector<PrefabPlacement>{};

  // R2-pf-mdl-14: the keys view the strings of the groups, which the walk does not change
  using Key = std::pair<std::string_view, std::string_view>;
  const auto hashKey = [](const Key& key) { return kdl::hash(key.first, key.second); };
  // (prefab path, link ID) -> index into result
  auto indices = std::unordered_map<Key, std::size_t, decltype(hashKey)>{0, hashKey};
  // R2-pf-mdl-3: prefab path -> entry, so that each path is looked up once
  auto entriesByPath = std::unordered_map<std::string_view, const PrefabEntry*>{};

  m_map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, GroupNode& node) {
      if (const auto& prefabPath = node.group().prefabPath())
      {
        const auto [it, inserted] =
          indices.try_emplace(Key{*prefabPath, node.linkId()}, result.size());
        if (inserted)
        {
          const auto [entryIt, entryInserted] =
            entriesByPath.try_emplace(*prefabPath, nullptr);
          if (entryInserted)
          {
            entryIt->second = fromPayload(PayloadPrefix + *prefabPath);
          }

          result.push_back(PrefabPlacement{
            .prefabPath = *prefabPath,
            .firstMember = &node,
            .members = {},
            .placedHash = node.group().prefabHash().value_or(""),
            .entry = entryIt->second,
          });
        }
        result[it->second].members.push_back(&node);
      }
      node.visitChildren(thisLambda);
    },
    // the children of an entity are brushes and patches
    [](EntityNode&) {},
    [](BrushNode&) {},
    [](PatchNode&) {}));

  return result;
}

std::map<PrefabPlacementKey, PrefabPlacement> PrefabLibrary::placementsByKey() const
{
  auto result = std::map<PrefabPlacementKey, PrefabPlacement>{};
  for (auto& placement : placements())
  {
    auto key = PrefabPlacementKey{placement.prefabPath, placement.firstMember->linkId()};
    result.emplace(std::move(key), std::move(placement));
  }
  return result;
}

std::vector<PrefabPlacement> PrefabLibrary::changedPlacements() const
{
  auto result = placements();
  std::erase_if(result, [](const auto& placement) {
    return placement.entry && placement.entry->hash == placement.placedHash;
  });
  return result;
}

void PrefabLibrary::setReadHookForTesting(ReadHook readHook)
{
  m_readHook = std::move(readHook);
}

kdl::task_manager& PrefabLibrary::taskManager()
{
  if (!m_taskManager)
  {
    m_taskManager = std::make_unique<kdl::task_manager>(WorkerCount);
  }
  return *m_taskManager;
}

void PrefabLibrary::startParsing(Listing listing)
{
  // the user root wins over the mod root for an equal relative path, so a shadowed file
  // of the mod root is not read at all
  auto userPathKeys = std::unordered_set<std::string>{};
  for (const auto& file : listing.files)
  {
    if (file.root == PrefabRoot::User)
    {
      userPathKeys.insert(file.pathKey);
    }
  }


  m_pendingDirectories = std::move(listing.directories);

  const auto& worldNode = m_map.worldNode();
  const auto mapFormat = worldNode.mapFormat();
  const auto& entityPropertyConfig = worldNode.entityPropertyConfig();
  const auto worldBounds = m_map.worldBounds();
  const auto generation = m_generation->load();

  for (auto& file : listing.files)
  {
    if (file.root == PrefabRoot::Mod && userPathKeys.contains(file.pathKey))
    {
      continue;
    }

    if (const auto it = m_entriesByKey.find(entryKey(file.root, file.pathKey));
        it != m_entriesByKey.end() && !it->second->error
        && it->second->absolutePath == file.absolutePath
        && it->second->fileSize == file.fileSize
        && it->second->modificationTime == file.modificationTime)
    {
      // unchanged: kept, with the thumbnail as it is now; a file that failed is read
      // again, since the failure may have been transient (DD-22)
      m_reusedEntries.push_back(ReusedEntry{it->second, std::move(file.thumbnailPath)});
      continue;
    }

    // phase 2: one parse task per new or changed file
    m_pendingEntries.push_back(taskManager().run_task(
      std::function<PrefabEntry()>{[file = std::move(file),
                                    mapFormat,
                                    entityPropertyConfig,
                                    worldBounds,
                                    currentGeneration = m_generation,
                                    generation,
                                    readHook = m_readHook]() {
        return readPrefabEntry(
          file.root,
          file.absolutePath,
          file.relativePath,
          file.fileSize,
          file.modificationTime,
          file.thumbnailPath,
          mapFormat,
          worldBounds,
          entityPropertyConfig,
          *currentGeneration,
          generation,
          readHook);
      }}));
  }
}

bool PrefabLibrary::integrateScan()
{
  // R2-pf-ui-3: only a scan that changed the entries is reported; the first scan changes
  // the library from not scanned to scanned, even when it finds nothing
  auto changed = !m_scannedOnce;
  m_scannedOnce = true;

  auto previousEntries = std::unordered_map<std::string, std::unique_ptr<PrefabEntry>>{};
  for (auto& entry : m_entries)
  {
    auto key = entryKey(entry->root, entry->pathKey);
    previousEntries.emplace(std::move(key), std::move(entry));
  }

  auto entries = std::vector<std::unique_ptr<PrefabEntry>>{};
  entries.reserve(m_reusedEntries.size() + m_pendingEntries.size());

  // a kept entry keeps its address, its world and its definitions
  const auto keep = [&](auto it, std::optional<std::filesystem::path> thumbnailPath) {
    if (it->second->thumbnailPath != thumbnailPath)
    {
      it->second->thumbnailPath = std::move(thumbnailPath);
      changed = true;
    }
    entries.push_back(std::move(it->second));
    previousEntries.erase(it);
  };

  for (auto& reusedEntry : m_reusedEntries)
  {
    if (const auto it = previousEntries.find(
          entryKey(reusedEntry.entry->root, reusedEntry.entry->pathKey));
        it != previousEntries.end())
    {
      keep(it, std::move(reusedEntry.thumbnailPath));
    }
  }

  for (auto& future : m_pendingEntries)
  {
    auto entry = std::make_unique<PrefabEntry>(future.get());
    if (const auto it = previousEntries.find(entryKey(entry->root, entry->pathKey));
        it != previousEntries.end() && failsAsBefore(*it->second, *entry))
    {
      // read again (DD-22) and failed as before: nothing changed
      keep(it, std::move(entry->thumbnailPath));
      continue;
    }

    setEntityDefinitions(*entry);
    entries.push_back(std::move(entry));
    changed = true;
  }

  // the entries that were neither reused nor kept were removed or read again
  changed = changed || !previousEntries.empty();

  m_reusedEntries.clear();
  m_pendingEntries.clear();
  m_scanning = false;

  // the roots are listed one after the other, and one may lie inside the other
  std::ranges::sort(m_pendingDirectories);
  const auto duplicates = std::ranges::unique(m_pendingDirectories);
  m_pendingDirectories.erase(duplicates.begin(), duplicates.end());
  m_directories = std::exchange(m_pendingDirectories, {});

  std::ranges::sort(entries, compareEntries);
  m_entries = std::move(entries);
  rebuildEntryIndex();
  // the entries that were not kept are deleted with previousEntries
  return changed;
}

bool PrefabLibrary::integrateUpdate(std::unique_ptr<PrefabEntry> entry)
{
  setEntityDefinitions(*entry);

  if (const auto it = m_entriesByKey.find(entryKey(entry->root, entry->pathKey));
      it != m_entriesByKey.end())
  {
    // the entry keeps its address, and the index stays as it is
    *it->second = std::move(*entry);
    return true;
  }

  if (entry->root == PrefabRoot::Mod)
  {
    if (m_entriesByKey.contains(entryKey(PrefabRoot::User, entry->pathKey)))
    {
      // shadowed by the user root
      return false;
    }
  }
  else if (const auto it = m_entriesByKey.find(entryKey(PrefabRoot::Mod, entry->pathKey));
           it != m_entriesByKey.end())
  {
    // the mod root's file is shadowed now
    std::erase_if(
      m_entries, [&](const auto& other) { return other.get() == it->second; });
  }

  const auto position = std::ranges::upper_bound(m_entries, entry, compareEntries);
  m_entries.insert(position, std::move(entry));
  rebuildEntryIndex();
  return true;
}

void PrefabLibrary::rebuildEntryIndex()
{
  m_entriesByKey.clear();
  m_entriesByKey.reserve(m_entries.size());
  for (const auto& entry : m_entries)
  {
    m_entriesByKey.emplace(entryKey(entry->root, entry->pathKey), entry.get());
  }
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
    if (entry->world)
    {
      entry->world->accept(makeSetEntityDefinitionsVisitor(nullptr));
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
  if (entry.bodyPreview)
  {
    entry.bodyPreview->color =
      bodyPreviewColor(*entry.bodyPreview, &m_map.entityDefinitionManager());
  }
}

} // namespace tb::mdl
