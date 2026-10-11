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
#include "mdl/EntityNaming.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"

#include "kd/vector_utils.h"

#include <algorithm>
#include <string>

namespace tb::mdl
{
namespace
{
const auto Type = freeIssueType();

/**
 * Of the entities that share a name, the first one in the map keeps it, and every other
 * one gets a new name, and its model if it has brushes or patches. References are not
 * changed, since it is not clear which of the entities they mean.
 *
 * The changes are not propagated to linked groups, because every instance keeps its own
 * names, and two changed instances of one linked group would fail the transaction.
 */
IssueQuickFix makeMakeNamesUniqueQuickFix(EntityNamingConfig config)
{
  return {
    "Make Names Unique",
    [config = std::move(config)](Map& map, const std::vector<const Issue*>& issues) {
      // changing an entity destroys the issues of every entity with its name, so the
      // nodes are copied out of the issues first
      auto entityNodes = std::vector<EntityNodeBase*>{};
      for (const auto* issue : issues)
      {
        if (issue->type() == Type)
        {
          if (auto* entityNode = dynamic_cast<EntityNodeBase*>(&issue->node()))
          {
            entityNodes.push_back(entityNode);
          }
        }
      }
      entityNodes =
        sortInTreeOrder(map, kdl::vec_sort_and_remove_duplicates(std::move(entityNodes)));

      auto allocator = EntityNameAllocator{map, config};
      auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
      for (auto* entityNode : entityNodes)
      {
        const auto* name = entityNode->entity().property(config.nameKey);
        if (!name || name->empty())
        {
          continue;
        }

        const auto holders =
          sortInTreeOrder(map, findEntitiesWithName(map, config.nameKey, *name));
        if (holders.size() < 2 || holders.front() == entityNode)
        {
          continue;
        }

        auto entity = entityNode->entity();
        auto namingResult = EntityNamingResult{};
        if (assignUniqueEntityName(
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

} // namespace

DuplicateEntityNameValidator::DuplicateEntityNameValidator(
  const Map& map, EntityNamingConfig config)
  : Validator{Type, "Duplicate entity name"}
  , m_map{map}
  , m_config{std::move(config)}
{
  addQuickFix(makeMakeNamesUniqueQuickFix(m_config));
}

void DuplicateEntityNameValidator::doValidate(
  EntityNodeBase& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const
{
  if (const auto* name = entityNode.entity().property(m_config.nameKey);
      name && !name->empty())
  {
    const auto holders = findEntitiesWithName(m_map, m_config.nameKey, *name);
    const auto otherCount = std::ranges::count_if(
      holders, [&](const auto* holder) { return holder != &entityNode; });
    if (otherCount > 0)
    {
      issues.push_back(std::make_unique<EntityPropertyIssue>(
        Type,
        entityNode,
        m_config.nameKey,
        entityNode.name() + " shares the name '" + *name + "' with "
          + std::to_string(otherCount) + " other "
          + (otherCount == 1 ? "entity" : "entities")));
    }
  }
}

} // namespace tb::mdl
