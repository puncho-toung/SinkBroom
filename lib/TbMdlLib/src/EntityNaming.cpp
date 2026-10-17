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

#include "mdl/EntityNaming.h"

#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/PatchNode.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/vector_utils.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <unordered_map>

namespace tb::mdl
{
namespace
{

// The engine refuses to spawn an entity with one of these names.
constexpr auto ReservedEngineNames =
  std::array<std::string_view, 2>{"NULL", "null_entity"};

// The map compiler merges the brushes of func_group entities into the world.
constexpr auto FuncGroupClassname = std::string_view{"func_group"};

/**
 * Returns the position of the world, every layer, group and entity of the map in a depth
 * first traversal.
 */
std::unordered_map<const Node*, size_t> getTreePositions(const Map& map)
{
  auto treePositions = std::unordered_map<const Node*, size_t>{};
  const auto add = [&](const Node& node) {
    treePositions.emplace(&node, treePositions.size());
  };

  map.worldNode().accept(kdl::overload(
    [&](auto&& thisLambda, const WorldNode& worldNode) {
      add(worldNode);
      worldNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const LayerNode& layerNode) {
      add(layerNode);
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const GroupNode& groupNode) {
      add(groupNode);
      groupNode.visitChildren(thisLambda);
    },
    [&](const EntityNode& entityNode) { add(entityNode); },
    [](const BrushNode&) {},
    [](const PatchNode&) {}));

  return treePositions;
}

template <typename N>
std::vector<N*> sortByTreePosition(const Map& map, std::vector<N*> nodes)
{
  const auto treePositions = getTreePositions(map);
  const auto treePosition = [&](const Node* node) {
    const auto it = treePositions.find(node);
    return it != treePositions.end() ? it->second : treePositions.size();
  };

  std::ranges::stable_sort(nodes, [&](const auto* lhs, const auto* rhs) {
    return treePosition(lhs) < treePosition(rhs);
  });
  return nodes;
}

} // namespace

EntityNameIndex::EntityNameIndex(std::string nameKey)
  : m_nameKey{std::move(nameKey)}
{
}

void EntityNameIndex::addEntityNode(EntityNodeBase& entityNode)
{
  if (const auto* name = entityNode.entity().property(m_nameKey); name && !name->empty())
  {
    m_entityNodes[kdl::str_to_lower(*name)].push_back(&entityNode);
  }
}

void EntityNameIndex::removeEntityNode(EntityNodeBase& entityNode)
{
  if (const auto* name = entityNode.entity().property(m_nameKey); name && !name->empty())
  {
    if (const auto it = m_entityNodes.find(kdl::str_to_lower(*name));
        it != m_entityNodes.end())
    {
      std::erase(it->second, &entityNode);
      if (it->second.empty())
      {
        m_entityNodes.erase(it);
      }
    }
  }
}

std::vector<EntityNodeBase*> EntityNameIndex::findEntityNodes(
  const std::string_view name) const
{
  const auto it = m_entityNodes.find(kdl::str_to_lower(name));
  return it != m_entityNodes.end() ? it->second : std::vector<EntityNodeBase*>{};
}

EntityNameAllocator::EntityNameAllocator(
  const Map& map, std::vector<const Node*> excluded)
  : m_map{map}
  , m_config{*map.worldNode().entityPropertyConfig().naming}
  , m_excluded{excluded.begin(), excluded.end()}
{
  contract_pre(map.entityNameIndex() != nullptr);
}

const EntityNamingConfig& EntityNameAllocator::config() const
{
  return m_config;
}

bool EntityNameAllocator::isTaken(const std::string_view name) const
{
  if (isReservedEntityName(name) || m_reserved.contains(kdl::str_to_lower(name)))
  {
    return true;
  }

  return std::ranges::any_of(
    findEntitiesWithName(m_map, name),
    [&](const auto* entityNode) { return !isExcluded(*entityNode); });
}

std::string EntityNameAllocator::allocate(const std::string_view classname)
{
  // the names below the next index were taken when they were tried, and no name becomes
  // free during an operation
  auto& nextIndex = m_nextIndices[kdl::str_to_lower(classname)];
  while (true)
  {
    auto name = std::string{classname} + "_" + std::to_string(nextIndex++);
    if (!isTaken(name))
    {
      contract_assert(!isReservedEntityName(name));

      reserve(name);
      return name;
    }
  }
}

void EntityNameAllocator::reserve(const std::string_view name)
{
  m_reserved.insert(kdl::str_to_lower(name));
}

bool EntityNameAllocator::isExcluded(const Node& node) const
{
  if (m_excluded.empty())
  {
    return false;
  }

  for (const auto* ancestor = &node; ancestor; ancestor = ancestor->parent())
  {
    if (m_excluded.contains(ancestor))
    {
      return true;
    }
  }
  return false;
}

bool isReservedEntityName(const std::string_view name)
{
  // idEntity::SetName compares with idStr::operator==, which respects case
  return kdl::ranges::contains(ReservedEngineNames, name);
}

bool isBrushEntityModelClass(const std::string_view classname)
{
  return !isWorldspawn(std::string{classname}) && classname != FuncGroupClassname;
}

namespace
{

/**
 * Reserves the name of the given entity if it has one that is not taken, and returns
 * whether it did. The worldspawn entity never keeps a name.
 */
bool reserveNameIfFree(const Entity& entity, EntityNameAllocator& allocator)
{
  if (isWorldspawn(entity.classname()))
  {
    return false;
  }

  const auto* name = entity.property(allocator.config().nameKey);
  if (name && !name->empty() && !allocator.isTaken(*name))
  {
    allocator.reserve(*name);
    return true;
  }
  return false;
}

/**
 * Like allocateEntityName, but the entity keeps its name if keepName is true, in which
 * case the name must be reserved already.
 */
bool assignEntityName(
  Entity& entity,
  const bool hasBrushesOrPatches,
  EntityNameAllocator& allocator,
  const bool keepName,
  EntityNamingResult& result)
{
  // copied, because changing a property clears the entity's cached classname
  const auto classname = entity.classname();
  if (isWorldspawn(classname))
  {
    return false;
  }

  const auto& config = allocator.config();
  const auto* currentName = entity.property(config.nameKey);
  const auto oldName = currentName ? *currentName : std::string{};

  auto changed = false;
  auto name = oldName;
  if (keepName)
  {
    result.kept.push_back(oldName);
  }
  else
  {
    name = allocator.allocate(classname);
    entity.addOrUpdateProperty(config.nameKey, name);
    result.renamed.emplace_back(oldName, name);
    changed = true;
  }

  if (
    hasBrushesOrPatches && config.brushEntityModelKey
    && isBrushEntityModelClass(classname))
  {
    if (const auto* model = entity.property(*config.brushEntityModelKey);
        !model || *model != name)
    {
      entity.addOrUpdateProperty(*config.brushEntityModelKey, name);
      changed = true;
    }
  }

  return changed;
}

} // namespace

bool allocateEntityName(
  Entity& entity,
  const bool hasBrushesOrPatches,
  EntityNameAllocator& allocator,
  const EntityNamingMode mode,
  EntityNamingResult& result)
{
  const auto keepName =
    mode == EntityNamingMode::KeepIfFree && reserveNameIfFree(entity, allocator);
  return assignEntityName(entity, hasBrushesOrPatches, allocator, keepName, result);
}

EntityNamingResult assignUniqueEntityNames(
  const std::vector<Node*>& rootsInSourceOrder,
  EntityNameAllocator& allocator,
  const EntityNamingMode mode)
{
  // the kept names are reserved before any new name is handed out, so that no entity
  // gets the name that an entity further on keeps
  auto entityNodes = std::vector<std::pair<EntityNode*, bool>>{};
  Node::visitAll(
    rootsInSourceOrder,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
      [&](EntityNode& entityNode) {
        const auto keepName = mode == EntityNamingMode::KeepIfFree
                              && reserveNameIfFree(entityNode.entity(), allocator);
        entityNodes.emplace_back(&entityNode, keepName);
      },
      [](BrushNode&) {},
      [](PatchNode&) {}));

  auto result = EntityNamingResult{};
  for (const auto& [entityNode, keepName] : entityNodes)
  {
    auto entity = entityNode->entity();
    if (assignEntityName(entity, entityNode->hasChildren(), allocator, keepName, result))
    {
      entityNode->setEntity(std::move(entity));
    }
  }

  return result;
}

void remapLinkSourceValues(
  const EntityDefinitionManager& entityDefinitionManager,
  const std::vector<Node*>& rootsInSourceOrder,
  const EntityNamingResult& namingResult,
  const std::vector<const GroupNode*>& skippedGroups)
{
  // lowercase old name -> new name
  auto newNames = std::unordered_map<std::string, std::string>{};
  for (const auto& [oldName, newName] : namingResult.renamed)
  {
    if (
      !oldName.empty()
      && std::ranges::none_of(namingResult.kept, [&](const auto& keptName) {
           return kdl::ci::str_is_equal(keptName, oldName);
         }))
    {
      // the first entity that was renamed from a name wins
      newNames.emplace(kdl::str_to_lower(oldName), newName);
    }
  }

  if (newNames.empty())
  {
    return;
  }

  const auto remap = [&](EntityNodeBase& entityNode) {
    const auto* definition =
      entityDefinitionManager.definition(entityNode.entity().classname());
    const auto linkSourceDefinitions = getLinkSourcePropertyDefinitions(definition);
    if (linkSourceDefinitions.empty())
    {
      return;
    }

    auto properties = entityNode.entity().properties();
    auto changed = false;
    for (auto& property : properties)
    {
      if (std::ranges::any_of(linkSourceDefinitions, [&](const auto* propertyDefinition) {
            return isNumberedProperty(propertyDefinition->key, property.key());
          }))
      {
        if (const auto it = newNames.find(kdl::str_to_lower(property.value()));
            it != newNames.end())
        {
          property.setValue(it->second);
          changed = true;
        }
      }
    }

    if (changed)
    {
      auto entity = entityNode.entity();
      entity.setProperties(std::move(properties));
      entityNode.setEntity(std::move(entity));
    }
  };

  Node::visitAll(
    rootsInSourceOrder,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        if (!kdl::ranges::contains(skippedGroups, &groupNode))
        {
          groupNode.visitChildren(thisLambda);
        }
      },
      [&](EntityNode& entityNode) { remap(entityNode); },
      [](BrushNode&) {},
      [](PatchNode&) {}));
}

void assignUniqueEntityNamesBeforeAdding(
  const Map& map,
  const std::vector<Node*>& rootsInSourceOrder,
  const bool remapLinkSources)
{
  // the nodes are not part of the map yet, so they see the default config
  contract_pre(map.worldNode().entityPropertyConfig().naming != std::nullopt);

  auto allocator = EntityNameAllocator{map};
  const auto namingResult =
    assignUniqueEntityNames(rootsInSourceOrder, allocator, EntityNamingMode::KeepIfFree);

  if (remapLinkSources)
  {
    // a group that joined a link set in the map is overwritten by the next update of
    // that set, link source values included, so its references are left alone
    const auto linkIdsInMap = kdl::vec_sort(collectLinkedGroupIds(map.worldNode()));
    const auto linkedGroups =
      collectGroups(rootsInSourceOrder) | std::views::filter([&](const auto* groupNode) {
        return std::ranges::binary_search(linkIdsInMap, groupNode->linkId());
      })
      | std::views::transform(
        [](const auto* groupNode) { return static_cast<const GroupNode*>(groupNode); })
      | kdl::ranges::to<std::vector>();

    remapLinkSourceValues(
      map.entityDefinitionManager(), rootsInSourceOrder, namingResult, linkedGroups);
  }
}

std::vector<EntityNodeBase*> sortInTreeOrder(
  const Map& map, std::vector<EntityNodeBase*> entityNodes)
{
  return sortByTreePosition(map, std::move(entityNodes));
}

std::vector<Node*> sortInTreeOrder(const Map& map, std::vector<Node*> nodes)
{
  return sortByTreePosition(map, std::move(nodes));
}

std::vector<EntityNodeBase*> collectEntityNodes(
  const Map& map, const std::vector<const Issue*>& issues, const IssueType type)
{
  auto entityNodes = std::vector<EntityNodeBase*>{};
  for (const auto* issue : issues)
  {
    if (issue->type() == type)
    {
      if (auto* entityNode = dynamic_cast<EntityNodeBase*>(&issue->node()))
      {
        entityNodes.push_back(entityNode);
      }
    }
  }
  return sortInTreeOrder(
    map, kdl::vec_sort_and_remove_duplicates(std::move(entityNodes)));
}

std::vector<EntityNodeBase*> findEntitiesWithName(
  const Map& map, const std::string_view name)
{
  const auto* entityNameIndex = map.entityNameIndex();
  return entityNameIndex && !name.empty() ? entityNameIndex->findEntityNodes(name)
                                          : std::vector<EntityNodeBase*>{};
}

} // namespace tb::mdl
