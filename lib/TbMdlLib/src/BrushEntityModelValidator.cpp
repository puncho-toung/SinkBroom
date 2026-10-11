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

#include "mdl/BrushEntityModelValidator.h"

#include "mdl/Entity.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"

#include "kd/contracts.h"
#include "kd/string_compare.h"
#include "kd/vector_utils.h"

#include <string>

namespace tb::mdl
{
namespace
{
const auto Type = freeIssueType();

/**
 * Sets the model of every entity to its name, and gives entities without a name a unique
 * one first. Like the names, the models are kept per instance of a linked group, so the
 * change is not propagated.
 */
IssueQuickFix makeSetModelToNameQuickFix(EntityNamingConfig config)
{
  return {
    "Set Model to Name",
    [config = std::move(config)](Map& map, const std::vector<const Issue*>& issues) {
      const auto entityNodes = collectEntityNodes(map, issues, Type);

      auto allocator = EntityNameAllocator{map};
      auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
      for (auto* entityNode : entityNodes)
      {
        auto entity = entityNode->entity();
        const auto* currentName = entity.property(config.nameKey);
        auto name = currentName && !currentName->empty()
                      ? *currentName
                      : allocator.allocate(entity.classname());

        entity.addOrUpdateProperty(config.nameKey, name);
        entity.addOrUpdateProperty(*config.brushEntityModelKey, std::move(name));
        nodesToSwap.emplace_back(entityNode, NodeContents{std::move(entity)});
      }

      if (!nodesToSwap.empty())
      {
        updateNodeContents(map, "Set Model to Name", std::move(nodesToSwap), {});
      }
    }};
}

} // namespace

BrushEntityModelValidator::BrushEntityModelValidator(EntityNamingConfig config)
  : Validator{Type, "Brush entity model differs from its name"}
  , m_config{std::move(config)}
{
  contract_pre(m_config.brushEntityModelKey != std::nullopt);

  addQuickFix(makeSetModelToNameQuickFix(m_config));
}

void BrushEntityModelValidator::doValidate(
  EntityNode& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const
{
  const auto& entity = entityNode.entity();
  if (!entityNode.hasChildren() || !isBrushEntityModelClass(entity.classname()))
  {
    return;
  }

  const auto& modelKey = *m_config.brushEntityModelKey;
  const auto* name = entity.property(m_config.nameKey);
  const auto* model = entity.property(modelKey);

  if (!name || name->empty())
  {
    issues.push_back(std::make_unique<EntityPropertyIssue>(
      Type,
      entityNode,
      m_config.nameKey,
      entityNode.name() + " has brushes or patches but no '" + m_config.nameKey
        + "' property"));
  }
  else if (!model)
  {
    issues.push_back(std::make_unique<EntityPropertyIssue>(
      Type,
      entityNode,
      modelKey,
      entityNode.name() + " '" + *name + "' has brushes or patches but no '" + modelKey
        + "' property"));
  }
  // the engine compares models and names without regard to case
  else if (!kdl::ci::str_is_equal(*model, *name))
  {
    issues.push_back(std::make_unique<EntityPropertyIssue>(
      Type,
      entityNode,
      modelKey,
      entityNode.name() + " '" + *name + "' has the " + modelKey + " '" + *model
        + "', which differs from its name"));
  }
}

} // namespace tb::mdl
