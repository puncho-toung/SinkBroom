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
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/NodeIndex.h"
#include "mdl/PatchNode.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"

#include <algorithm>
#include <array>
#include <optional>
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

// Numbers above this are not handed out, so names with more digits cannot block one.
constexpr auto MaxIndexDigits = size_t{9};

bool isReservedEngineName(const std::string_view name)
{
  return std::ranges::any_of(ReservedEngineNames, [&](const auto reservedName) {
    return kdl::ci::str_is_equal(name, reservedName);
  });
}

bool isAsciiLetter(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/**
 * Returns a node index pattern that matches the given string whatever the case of its
 * letters. The index is case sensitive, so every letter matches any character and the
 * callers compare the hits again.
 */
std::string makeCaseInsensitivePattern(const std::string_view str)
{
  auto result = std::string{};
  result.reserve(str.size());

  for (const auto c : str)
  {
    if (isAsciiLetter(c))
    {
      result += '?';
    }
    else
    {
      result += NodeIndex::escapePattern(std::string_view{&c, 1});
    }
  }

  return result;
}

/**
 * If the given name is <classname>_<N> without regard to case, where N is written
 * without leading zeros, returns N.
 */
std::optional<size_t> parseIndex(
  const std::string_view name, const std::string_view classname)
{
  if (
    name.size() <= classname.size() + 1
    || !kdl::ci::str_is_equal(name.substr(0, classname.size()), classname)
    || name[classname.size()] != '_')
  {
    return std::nullopt;
  }

  const auto digits = name.substr(classname.size() + 1);
  if (
    digits.size() > MaxIndexDigits || (digits.size() > 1 && digits.front() == '0')
    || !std::ranges::all_of(digits, [](const auto c) { return c >= '0' && c <= '9'; }))
  {
    return std::nullopt;
  }

  auto index = size_t{0};
  for (const auto c : digits)
  {
    index = index * 10 + size_t(c - '0');
  }
  return index;
}

} // namespace

EntityNameAllocator::EntityNameAllocator(
  const Map& map, EntityNamingConfig config, std::vector<const Node*> excluded)
  : m_map{map}
  , m_config{std::move(config)}
  , m_excluded{std::move(excluded)}
{
}

const EntityNamingConfig& EntityNameAllocator::config() const
{
  return m_config;
}

bool EntityNameAllocator::isTaken(const std::string_view name) const
{
  if (isReservedEngineName(name) || m_reserved.contains(kdl::str_to_lower(name)))
  {
    return true;
  }

  return std::ranges::any_of(
    m_map.findNodes<EntityNodeBase>(makeCaseInsensitivePattern(name)),
    [&](const auto* entityNode) {
      const auto* value = entityNode->entity().property(m_config.nameKey);
      return value && kdl::ci::str_is_equal(*value, name) && !isExcluded(*entityNode);
    });
}

std::string EntityNameAllocator::allocate(const std::string_view classname)
{
  const auto& indices = usedIndices(classname);
  for (auto index = size_t{0};; ++index)
  {
    if (!indices.contains(index))
    {
      auto name = std::string{classname} + "_" + std::to_string(index);
      if (!m_reserved.contains(kdl::str_to_lower(name)))
      {
        contract_assert(!isReservedEngineName(name));

        reserve(name);
        return name;
      }
    }
  }
}

void EntityNameAllocator::reserve(const std::string_view name)
{
  m_reserved.insert(kdl::str_to_lower(name));
}

bool EntityNameAllocator::isExcluded(const Node& node) const
{
  return std::ranges::any_of(m_excluded, [&](const auto* excludedNode) {
    return &node == excludedNode || node.isDescendantOf(*excludedNode);
  });
}

const std::set<size_t>& EntityNameAllocator::usedIndices(const std::string_view classname)
{
  auto key = kdl::str_to_lower(classname);
  if (const auto it = m_usedIndices.find(key); it != m_usedIndices.end())
  {
    return it->second;
  }

  // one index query per classname and operation
  auto indices = std::set<size_t>{};
  for (const auto* entityNode :
       m_map.findNodes<EntityNodeBase>(makeCaseInsensitivePattern(classname) + "_%*"))
  {
    if (const auto* value = entityNode->entity().property(m_config.nameKey))
    {
      if (const auto index = parseIndex(*value, classname);
          index && !isExcluded(*entityNode))
      {
        indices.insert(*index);
      }
    }
  }

  return m_usedIndices.emplace(std::move(key), std::move(indices)).first->second;
}

bool isBrushEntityModelClass(const std::string_view classname)
{
  return !isWorldspawn(std::string{classname}) && classname != FuncGroupClassname;
}

bool assignUniqueEntityName(
  Entity& entity,
  const bool hasBrushesOrPatches,
  EntityNameAllocator& allocator,
  const EntityNamingMode mode,
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
  if (
    mode == EntityNamingMode::KeepIfFree && !oldName.empty()
    && !allocator.isTaken(oldName))
  {
    allocator.reserve(oldName);
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

EntityNamingResult assignUniqueEntityNames(
  const std::vector<Node*>& rootsInSourceOrder,
  EntityNameAllocator& allocator,
  const EntityNamingMode mode)
{
  auto result = EntityNamingResult{};

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
        auto entity = entityNode.entity();
        if (assignUniqueEntityName(
              entity, entityNode.hasChildren(), allocator, mode, result))
        {
          entityNode.setEntity(std::move(entity));
        }
      },
      [](BrushNode&) {},
      [](PatchNode&) {}));

  return result;
}

void remapLinkSourceValues(
  const EntityDefinitionManager& entityDefinitionManager,
  const std::vector<Node*>& rootsInSourceOrder,
  const EntityNamingResult& namingResult)
{
  auto newNames = std::unordered_map<std::string, std::string>{};
  for (const auto& [oldName, newName] : namingResult.renamed)
  {
    if (!oldName.empty() && !kdl::ranges::contains(namingResult.kept, oldName))
    {
      // the first entity that was renamed from a name wins
      newNames.emplace(oldName, newName);
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
        if (const auto it = newNames.find(property.value()); it != newNames.end())
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
      [](
        auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
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
  if (const auto& naming = map.worldNode().entityPropertyConfig().naming)
  {
    auto allocator = EntityNameAllocator{map, *naming};
    const auto namingResult = assignUniqueEntityNames(
      rootsInSourceOrder, allocator, EntityNamingMode::KeepIfFree);

    if (remapLinkSources)
    {
      remapLinkSourceValues(
        map.entityDefinitionManager(), rootsInSourceOrder, namingResult);
    }
  }
}

std::vector<EntityNodeBase*> sortInTreeOrder(
  const Map& map, std::vector<EntityNodeBase*> entityNodes)
{
  auto treePositions = std::unordered_map<const Node*, size_t>{};
  map.worldNode().accept(kdl::overload(
    [&](auto&& thisLambda, const WorldNode& worldNode) {
      treePositions.emplace(&worldNode, treePositions.size());
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const GroupNode& groupNode) {
      groupNode.visitChildren(thisLambda);
    },
    [&](const EntityNode& entityNode) {
      treePositions.emplace(&entityNode, treePositions.size());
    },
    [](const BrushNode&) {},
    [](const PatchNode&) {}));

  const auto treePosition = [&](const Node* node) {
    const auto it = treePositions.find(node);
    return it != treePositions.end() ? it->second : treePositions.size();
  };

  std::ranges::stable_sort(entityNodes, [&](const auto* lhs, const auto* rhs) {
    return treePosition(lhs) < treePosition(rhs);
  });
  return entityNodes;
}

std::vector<EntityNodeBase*> findEntitiesWithName(
  const Map& map, const std::string& nameKey, const std::string& name)
{
  if (name.empty())
  {
    return {};
  }

  auto result = map.findNodes<EntityNodeBase>(NodeIndex::escapePattern(name));
  std::erase_if(result, [&](const auto* entityNode) {
    const auto* value = entityNode->entity().property(nameKey);
    return !value || *value != name;
  });
  return result;
}

} // namespace tb::mdl
