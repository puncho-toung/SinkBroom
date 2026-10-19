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

#include "mdl/PrefabFile.h"

#include "fs/DiskFileSystem.h"
#include "fs/DiskIO.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/MapHeader.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"
#include "version/Version.h"

#include "kd/overload.h"
#include "kd/path_utils.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"
#include "kd/task_manager.h"

#include "vm/mat_ext.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <ctime>
#include <ranges>
#include <sstream>
#include <unordered_set>

namespace tb::mdl
{
namespace
{

// the engine's bind key is typed in none of the shipped FGDs, so it is named here
const auto BindKey = std::string{"bind"};

// the naming key of games without naming rules, as in the default link properties
const auto DefaultNameKey = std::string{"name"};

/**
 * Brushes and patches of a brush entity stand for that entity; nodes below another
 * selected node are dropped.
 */
std::vector<Node*> normalizeSelection(const std::vector<Node*>& selectedNodes)
{
  auto nodes = std::vector<Node*>{};
  for (auto* node : selectedNodes)
  {
    auto* entityNode = dynamic_cast<EntityNode*>(node->parent());
    auto* nodeToAdd = entityNode ? static_cast<Node*>(entityNode) : node;
    if (!kdl::ranges::contains(nodes, nodeToAdd))
    {
      nodes.push_back(nodeToAdd);
    }
  }

  return nodes | std::views::filter([&](const auto* node) {
           return !node->isDescendantOf(nodes);
         })
         | kdl::ranges::to<std::vector>();
}

std::vector<EntityNode*> collectEntityNodes(const std::vector<Node*>& nodes)
{
  auto result = std::vector<EntityNode*>{};
  Node::visitAll(
    nodes,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
      [&](EntityNode& entityNode) { result.push_back(&entityNode); },
      [](BrushNode&) {},
      [](PatchNode&) {}));
  return result;
}

/**
 * The names, lower case, by which the given entities can be the target of a link: the
 * naming key's value and the values of every key typed as a link target.
 */
std::unordered_set<std::string> entityNames(const std::vector<EntityNode*>& entityNodes)
{
  auto result = std::unordered_set<std::string>{};
  for (const auto* entityNode : entityNodes)
  {
    const auto& entity = entityNode->entity();
    const auto& naming = entityNode->entityPropertyConfig().naming;
    const auto& nameKey = naming ? naming->nameKey : DefaultNameKey;
    if (const auto* name = entity.property(nameKey); name && !name->empty())
    {
      result.insert(kdl::str_to_lower(*name));
    }

    const auto linkTargetDefinitions =
      getLinkTargetPropertyDefinitions(entity.definition());
    for (const auto& property : entity.properties())
    {
      if (
        !property.value().empty()
        && std::ranges::any_of(linkTargetDefinitions, [&](const auto* definition) {
             return isNumberedProperty(definition->key, property.key());
           }))
      {
        result.insert(kdl::str_to_lower(property.value()));
      }
    }
  }
  return result;
}

bool isLinkKey(const Entity& entity, const std::string& key)
{
  return key == BindKey || isNumberedProperty(EntityPropertyKeys::Target, key)
         || std::ranges::any_of(
           getLinkSourcePropertyDefinitions(entity.definition()),
           [&](const auto* definition) {
             return isNumberedProperty(definition->key, key);
           });
}

std::vector<EntityProperty> externalLinkProperties(
  const Entity& entity, const std::unordered_set<std::string>& namesInSelection)
{
  return entity.properties() | std::views::filter([&](const auto& property) {
           return !property.value().empty() && isLinkKey(entity, property.key())
                  && !namesInSelection.contains(kdl::str_to_lower(property.value()));
         })
         | kdl::ranges::to<std::vector>();
}

void clearExternalLinks(
  EntityNode& entityNode, const std::unordered_set<std::string>& namesInSelection)
{
  const auto properties = externalLinkProperties(entityNode.entity(), namesInSelection);
  if (!properties.empty())
  {
    auto entity = entityNode.entity();
    for (const auto& property : properties)
    {
      entity.removeProperty(property.key());
    }
    entityNode.setEntity(std::move(entity));
  }
}

void addDefinitionDefaults(EntityNode& entityNode)
{
  if (const auto* definition = entityNode.entity().definition())
  {
    auto entity = entityNode.entity();
    for (const auto& propertyDefinition : definition->propertyDefinitions)
    {
      if (!entity.hasProperty(propertyDefinition.key))
      {
        if (
          const auto defaultValue = PropertyDefinition::defaultValue(propertyDefinition))
        {
          entity.addOrUpdateProperty(propertyDefinition.key, *defaultValue);
        }
      }
    }
    entityNode.setEntity(std::move(entity));
  }
}

/**
 * Translates the given nodes like the translate command does, except that a brush
 * entity's origin key moves with its brushes, and that alignment is always locked, so
 * the file keeps the selection's faces and its entities' relation to their brushes.
 */
Result<void> translateNodes(
  const std::vector<Node*>& nodes,
  const vm::vec3d& delta,
  const vm::bbox3d& worldBounds,
  const EntityPropertyConfig& entityPropertyConfig)
{
  const auto transformation = vm::translation_matrix(delta);
  auto errors = std::vector<Error>{};

  Node::visitAll(
    nodes,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        auto group = groupNode.group();
        group.transform(transformation);
        groupNode.setGroup(std::move(group));
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        auto entity = entityNode.entity();
        if (!entityNode.hasChildren())
        {
          entity.transform(
            transformation,
            entityPropertyConfig.updateAnglePropertyAfterTransform,
            entityPropertyConfig.rotationPolicy);
        }
        else
        {
          entityNode.visitChildren(thisLambda);
          if (entity.hasProperty(EntityPropertyKeys::Origin))
          {
            entity.setOrigin(entity.origin() + delta);
          }
        }
        entityNode.setEntity(std::move(entity));
      },
      [&](BrushNode& brushNode) {
        auto brush = brushNode.brush();
        brush.transform(worldBounds, transformation, true)
          | kdl::transform([&]() { brushNode.setBrush(std::move(brush)); })
          | kdl::transform_error([&](auto e) { errors.push_back(std::move(e)); });
      },
      [&](PatchNode& patchNode) {
        auto patch = patchNode.patch();
        patch.transform(transformation);
        patchNode.setPatch(std::move(patch));
      }));

  if (!errors.empty())
  {
    return errors.front();
  }
  return kdl::void_success;
}

/**
 * The point that the file puts at 0 0 0 (D10): an entity's origin for the entity kind
 * (the snapped bounds centre for a brush entity without an origin key), the snapped
 * bounds centre of the body for an assembly.
 */
vm::vec3d referencePoint(const Map& map, const PrefabKind kind, const Node& body)
{
  if (kind == PrefabKind::Entity)
  {
    if (
      const auto* entityNode = dynamic_cast<const EntityNode*>(&body);
      entityNode
      && (!entityNode->hasChildren() || entityNode->entity().hasProperty(EntityPropertyKeys::Origin)))
    {
      return entityNode->entity().origin();
    }
  }
  return map.grid().snap(body.logicalBounds().center());
}

/**
 * Replaces every character that a file name cannot hold on any platform.
 */
std::string sanitizeFileName(const std::string& name)
{
  auto result = std::string{};
  for (const auto c : name)
  {
    const auto isInvalid =
      static_cast<unsigned char>(c) < 0x20
      || std::string_view{"<>:\"/\\|?*"}.find(c) != std::string_view::npos;
    result.push_back(isInvalid ? '_' : c);
  }

  result = kdl::str_trim(result);
  while (!result.empty() && (result.back() == '.' || result.back() == ' '))
  {
    result.pop_back();
  }
  return result;
}

bool isValidCategory(const std::filesystem::path& category)
{
  return !category.has_root_path()
         && std::ranges::none_of(
           category, [](const auto& element) { return element == ".."; });
}

std::string localTimestamp()
{
  const auto now = std::time(nullptr);
  auto localTime = std::tm{};
#ifdef _WIN32
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif

  char buffer[32];
  const auto length =
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &localTime);
  return std::string(buffer, length);
}

/**
 * The map's path relative to its mod folder (forward slashes), or its file name if it is
 * not in that folder.
 */
std::string sourceName(const Map& map)
{
  const auto modRoot = map.prefabLibrary().writeRootFor(PrefabRoot::Mod).parent_path();
  if (!modRoot.empty() && map.persistent())
  {
    const auto relativePath = map.path().lexically_relative(modRoot);
    if (!relativePath.empty() && kdl::path_front(relativePath) != "..")
    {
      return relativePath.generic_string();
    }
  }
  return map.filename();
}

std::vector<EntityProperty> metadataProperties(
  const Map& map,
  const PrefabKind kind,
  const std::string& className,
  const SavePrefabOptions& options)
{
  auto tags = options.tags
              | std::views::transform([](const auto& tag) { return kdl::str_trim(tag); })
              | std::views::filter([](const auto& tag) { return !tag.empty(); })
              | kdl::ranges::to<std::vector>();

  auto properties = std::vector<EntityProperty>{
    {EntityPropertyKeys::Classname, EntityPropertyValues::WorldspawnClassname},
    {PrefabPropertyKeys::Kind,
     kind == PrefabKind::Entity ? PrefabPropertyValues::KindEntity
                                : PrefabPropertyValues::KindAssembly},
    {PrefabPropertyKeys::Name, options.name},
  };
  if (kind == PrefabKind::Entity)
  {
    properties.emplace_back(PrefabPropertyKeys::Class, className);
  }
  if (!tags.empty())
  {
    properties.emplace_back(PrefabPropertyKeys::Tags, kdl::str_join(tags, ";"));
  }
  properties.emplace_back(PrefabPropertyKeys::Saved, localTimestamp());
  properties.emplace_back(PrefabPropertyKeys::Source, sourceName(map));
  return properties;
}

std::string serializePrefab(const Map& map, const WorldNode& worldNode)
{
  const auto generator =
    fmt::format("TrenchBroom {} (Build {})", VERSION_STR, BUILD_ID_STR);

  auto stream = std::stringstream{};
  writeMapHeader(
    stream, map.gameInfo().gameConfig.name, worldNode.mapFormat(), generator);

  // the map's task manager is not available through a const map; brush serialisation
  // runs inline
  auto taskManager = kdl::task_manager{0};
  auto writer = NodeWriter{worldNode, stream};
  writer.setExporting(false);
  writer.writeMap(taskManager);

  return stream.str();
}

Result<std::filesystem::path> writePrefabFile(
  const std::filesystem::path& directory,
  const std::string& fileName,
  const std::string& contents)
{
  return fs::Disk::createDirectory(directory) | kdl::and_then([&](auto) {
           auto fs = fs::WritableDiskFileSystem{directory};
           return fs.createFileAtomic(fileName, contents);
         })
         | kdl::transform([&]() { return directory / fileName; });
}

} // namespace

PrefabKind derivePrefabKind(
  const std::vector<Node*>& selectedNodes, const bool forceAssembly)
{
  const auto nodes = normalizeSelection(selectedNodes);
  return !forceAssembly && nodes.size() == 1
             && dynamic_cast<const EntityNode*>(nodes.front())
           ? PrefabKind::Entity
           : PrefabKind::Assembly;
}

std::vector<ExternalLink> findExternalLinks(const std::vector<Node*>& selectedNodes)
{
  const auto entityNodes = collectEntityNodes(normalizeSelection(selectedNodes));
  const auto namesInSelection = entityNames(entityNodes);

  auto result = std::vector<ExternalLink>{};
  for (const auto* entityNode : entityNodes)
  {
    for (const auto& property :
         externalLinkProperties(entityNode->entity(), namesInSelection))
    {
      result.push_back(ExternalLink{entityNode, property.key(), property.value()});
    }
  }
  return result;
}

Result<std::filesystem::path> savePrefab(
  const Map& map,
  const std::vector<Node*>& selectedNodes,
  const SavePrefabOptions& options)
{
  const auto nodes = normalizeSelection(selectedNodes);
  if (nodes.empty())
  {
    return Error{"Cannot save an empty selection as a prefab"};
  }

  const auto fileName = sanitizeFileName(options.name);
  if (fileName.empty())
  {
    return Error{"Cannot save a prefab without a name"};
  }

  if (!isValidCategory(options.category))
  {
    return Error{fmt::format(
      "Prefab category {} must be a folder inside the library", options.category)};
  }

  const auto rootPath = map.prefabLibrary().writeRootFor(options.root);
  if (rootPath.empty())
  {
    return Error{
      options.root == PrefabRoot::User
        ? "Cannot save a prefab to the user library: the Prefab Path is not set"
        : "Cannot save a prefab to the mod library: the game path is not set"};
  }

  const auto kind = derivePrefabKind(nodes, options.forceAssembly);
  const auto namesInSelection = entityNames(collectEntityNodes(nodes));
  const auto& worldBounds = map.worldBounds();
  const auto& entityPropertyConfig = map.worldNode().entityPropertyConfig();

  // the clones keep their entity definitions, which the link and default rules need
  auto body = nodes | std::views::transform([&](const auto* node) {
                return node->cloneRecursively(worldBounds);
              })
              | kdl::ranges::to<std::vector>();
  const auto className =
    kind == PrefabKind::Entity
      ? static_cast<const EntityNode*>(body.front())->entity().classname()
      : std::string{};

  auto worldNode = std::make_unique<WorldNode>(
    entityPropertyConfig,
    Entity{metadataProperties(map, kind, className, options)},
    map.worldNode().mapFormat());

  // the world owns the clones from here on
  auto* topNode = static_cast<Node*>(nullptr);
  if (kind == PrefabKind::Assembly)
  {
    // worldspawn-level brushes of the selection end up in the group
    auto* groupNode = new GroupNode{Group{options.name}};
    groupNode->addChildren(body);
    worldNode->defaultLayer()->addChild(groupNode);
    topNode = groupNode;
  }
  else
  {
    worldNode->defaultLayer()->addChildren(body);
    topNode = body.front();
  }

  for (auto* entityNode : collectEntityNodes(body))
  {
    if (options.clearExternalLinks)
    {
      clearExternalLinks(*entityNode, namesInSelection);
    }
    if (options.includeDefaults)
    {
      addDefinitionDefaults(*entityNode);
    }
    if (kind == PrefabKind::Entity)
    {
      // an entity snapshot carries no _tb_ key
      auto entity = entityNode->entity();
      entity.setProtectedProperties({});
      entityNode->setEntity(std::move(entity));
    }
  }

  const auto delta = -referencePoint(map, kind, *topNode);
  return translateNodes(body, delta, worldBounds, entityPropertyConfig)
         | kdl::and_then([&]() {
             // link sets inside the prefab keep their membership under fresh ids, so a
             // placement joins none of the source map's link sets
             rekeyLinkIds({topNode});

             return writePrefabFile(
               rootPath / options.category,
               fileName + ".map",
               serializePrefab(map, *worldNode));
           });
}

Result<PrefabMetadata> readPrefabMetadata(const WorldNode& world)
{
  const auto& entity = world.entity();
  const auto* kind = entity.property(PrefabPropertyKeys::Kind);
  if (!kind)
  {
    return Error{fmt::format("Not a prefab: {} is missing", PrefabPropertyKeys::Kind)};
  }

  auto metadata = PrefabMetadata{};
  if (*kind == PrefabPropertyValues::KindEntity)
  {
    metadata.kind = PrefabKind::Entity;
  }
  else if (*kind == PrefabPropertyValues::KindAssembly)
  {
    metadata.kind = PrefabKind::Assembly;
  }
  else
  {
    return Error{fmt::format("Not a prefab: unknown prefab kind '{}'", *kind)};
  }

  const auto value = [&](const auto& key) {
    const auto* str = entity.property(key);
    return str ? *str : std::string{};
  };

  metadata.name = value(PrefabPropertyKeys::Name);
  metadata.className =
    metadata.kind == PrefabKind::Entity ? value(PrefabPropertyKeys::Class) : "";
  metadata.tags =
    kdl::str_split(value(PrefabPropertyKeys::Tags), ";")
    | std::views::transform([](const auto& tag) { return kdl::str_trim(tag); })
    | std::views::filter([](const auto& tag) { return !tag.empty(); })
    | kdl::ranges::to<std::vector>();
  metadata.saved = value(PrefabPropertyKeys::Saved);
  metadata.source = value(PrefabPropertyKeys::Source);
  return metadata;
}

} // namespace tb::mdl
