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

#include "mdl/DuplicateEntityNameValidator.h"

#include "mdl/Entity.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/PropertyDefinition.h"

#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/vector_utils.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

namespace tb::mdl
{
namespace
{
const auto Type = freeIssueType();

bool isNameIssue(const Issue& issue, const EntityNamingConfig& config)
{
  return static_cast<const EntityPropertyIssue&>(issue).propertyKey() == config.nameKey;
}

/**
 * Of the entities that share a name, the first one in the map keeps it, and every other
 * one gets a new name, and its model if it has brushes or patches. An entity with a name
 * that the engine reserves gets a new name, too. References are not changed, since it is
 * not clear which of the entities they mean.
 *
 * The changes are not propagated to linked groups, because every instance keeps its own
 * names, and two changed instances of one linked group would fail the transaction.
 */
IssueQuickFix makeMakeNamesUniqueQuickFix(EntityNamingConfig config)
{
  return {
    "Make Names Unique",
    [config = std::move(config)](Map& map, const std::vector<const Issue*>& issues) {
      const auto entityNodes = collectEntityNodes(
        map,
        issues | std::views::filter([&](const auto* issue) {
          return issue->type() == Type && isNameIssue(*issue, config);
        }) | kdl::ranges::to<std::vector>(),
        Type);

      auto allocator = EntityNameAllocator{map};
      auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
      for (auto* entityNode : entityNodes)
      {
        const auto* name = entityNode->entity().property(config.nameKey);
        if (!name || name->empty())
        {
          continue;
        }

        if (!isReservedEntityName(*name))
        {
          const auto holders = sortInTreeOrder(map, findEntitiesWithName(map, *name));
          if (holders.size() < 2 || holders.front() == entityNode)
          {
            continue;
          }
        }

        auto entity = entityNode->entity();
        auto namingResult = EntityNamingResult{};
        if (allocateEntityName(
              entity,
              entityNode->hasChildren(),
              allocator,
              EntityNamingMode::Create,
              namingResult))
        {
          nodesToSwap.emplace_back(entityNode, NodeContents{std::move(entity)});
        }
      }

      if (!nodesToSwap.empty())
      {
        updateNodeContents(map, "Make Names Unique", std::move(nodesToSwap), {});
      }
    }};
}

/**
 * Removes the link source properties that refer to the entity itself. Like any other
 * change of a link source property, this is propagated to linked groups.
 */
IssueQuickFix makeRemoveSelfReferencesQuickFix(EntityNamingConfig config)
{
  return {
    "Remove References to Itself",
    [config = std::move(config)](Map& map, const std::vector<const Issue*>& issues) {
      // copied out of the issues first, which a change destroys
      auto keysByEntityNode = std::vector<std::pair<EntityNodeBase*, std::string>>{};
      for (const auto* issue : issues)
      {
        if (issue->type() == Type && !isNameIssue(*issue, config))
        {
          if (auto* entityNode = dynamic_cast<EntityNodeBase*>(&issue->node()))
          {
            keysByEntityNode.emplace_back(
              entityNode, static_cast<const EntityPropertyIssue*>(issue)->propertyKey());
          }
        }
      }

      auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
      for (auto* entityNode : sortInTreeOrder(
             map,
             kdl::vec_sort_and_remove_duplicates(
               keysByEntityNode | std::views::keys | kdl::ranges::to<std::vector>())))
      {
        auto entity = entityNode->entity();
        const auto* name = entity.property(config.nameKey);
        if (!name)
        {
          continue;
        }

        const auto ownName = *name;
        for (const auto& [keyEntityNode, key] : keysByEntityNode)
        {
          if (const auto* value = entity.property(key);
              keyEntityNode == entityNode && value
              && kdl::ci::str_is_equal(*value, ownName))
          {
            entity.removeProperty(key);
          }
        }
        nodesToSwap.emplace_back(entityNode, NodeContents{std::move(entity)});
      }

      if (!nodesToSwap.empty())
      {
        updateNodeContents(map, "Remove References to Itself", std::move(nodesToSwap));
      }
    }};
}

} // namespace

DuplicateEntityNameValidator::DuplicateEntityNameValidator(
  const Map& map, EntityNamingConfig config)
  : Validator{Type, "Duplicate or invalid entity name"}
  , m_map{map}
  , m_config{std::move(config)}
{
  addQuickFix(makeMakeNamesUniqueQuickFix(m_config));
  addQuickFix(makeRemoveSelfReferencesQuickFix(m_config));
}

void DuplicateEntityNameValidator::doValidate(
  EntityNodeBase& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const
{
  const auto& entity = entityNode.entity();
  const auto* name = entity.property(m_config.nameKey);
  if (!name || name->empty())
  {
    return;
  }

  if (isReservedEntityName(*name))
  {
    issues.push_back(std::make_unique<EntityPropertyIssue>(
      Type,
      entityNode,
      m_config.nameKey,
      entityNode.name() + " has the name '" + *name + "', which the engine reserves"));
  }
  else
  {
    // the engine finds entities by name without regard to case
    auto otherCount = size_t{0};
    auto differentCase = false;
    for (const auto* holder : findEntitiesWithName(m_map, *name))
    {
      if (holder != &entityNode)
      {
        ++otherCount;
        if (const auto* holderName = holder->entity().property(m_config.nameKey);
            holderName && *holderName != *name)
        {
          differentCase = true;
        }
      }
    }

    if (otherCount > 0)
    {
      issues.push_back(std::make_unique<EntityPropertyIssue>(
        Type,
        entityNode,
        m_config.nameKey,
        entityNode.name() + " shares the name '" + *name + "' with "
          + std::to_string(otherCount) + " other "
          + (otherCount == 1 ? "entity" : "entities")
          + (differentCase ? " if case is ignored, as the engine ignores it" : "")));
    }
  }

  // the engine refuses to spawn an entity that refers to itself
  for (const auto* propertyDefinition :
       getLinkSourcePropertyDefinitions(entity.definition()))
  {
    for (const auto& property : entity.numberedProperties(propertyDefinition->key))
    {
      if (kdl::ci::str_is_equal(property.value(), *name))
      {
        issues.push_back(std::make_unique<EntityPropertyIssue>(
          Type,
          entityNode,
          property.key(),
          entityNode.name() + " '" + *name + "' refers to itself with '" + property.key()
            + "'"));
      }
    }
  }
}

} // namespace tb::mdl
