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

#include "mdl/EntityRotationKeyValidator.h"

#include "mdl/Entity.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityRotation.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"

#include "vm/mat.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace tb::mdl
{
namespace
{
const auto Type = freeIssueType();

/**
 * Returns the entity nodes of the given issues in tree order, keeping the entities of
 * only one group of each link set. Rotation properties are the same in every group of a
 * link set, so updating the linked groups copies the change into the other groups, and
 * changing two groups of one link set at once would fail.
 */
std::vector<EntityNode*> collectEntityNodesToChange(
  const Map& map, const std::vector<const Issue*>& issues)
{
  auto groupsByLinkId = std::unordered_map<std::string, const GroupNode*>{};
  auto result = std::vector<EntityNode*>{};
  for (auto* entityNodeBase : collectEntityNodes(map, issues, Type))
  {
    // this validator reports entity nodes only
    auto* entityNode = static_cast<EntityNode*>(entityNodeBase);
    if (const auto* groupNode = entityNode->containingGroup())
    {
      const auto [it, inserted] = groupsByLinkId.emplace(groupNode->linkId(), groupNode);
      if (!inserted && it->second != groupNode)
      {
        continue;
      }
    }
    result.push_back(entityNode);
  }
  return result;
}

// not makeRemoveEntityPropertiesQuickFix, which edits one issue at a time through the
// selection and so cannot keep to one group per link set
IssueQuickFix makeDeletePropertyQuickFix()
{
  return {"Delete Property", [](Map& map, const std::vector<const Issue*>& issues) {
            // read every issue before anything changes, because a change destroys the
            // issues of the changed entity
            auto keysByNode = std::unordered_map<const Node*, std::vector<std::string>>{};
            for (const auto* issue : issues)
            {
              if (issue->type() == Type)
              {
                keysByNode[&issue->node()].push_back(
                  static_cast<const EntityPropertyIssue*>(issue)->propertyKey());
              }
            }

            auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
            for (auto* entityNode : collectEntityNodesToChange(map, issues))
            {
              auto entity = entityNode->entity();
              for (const auto& key : keysByNode[entityNode])
              {
                entity.removeProperty(key);
              }
              nodesToSwap.emplace_back(entityNode, NodeContents{std::move(entity)});
            }

            if (!nodesToSwap.empty())
            {
              updateNodeContents(map, "Delete Property", std::move(nodesToSwap));
            }
          }};
}

IssueQuickFix makeConvertToAnglesQuickFix()
{
  return {"Convert to Angles", [](Map& map, const std::vector<const Issue*>& issues) {
            auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
            for (auto* entityNode : collectEntityNodesToChange(map, issues))
            {
              auto entity = entityNode->entity();
              // an entity with brushes or patches never gets "angles"
              if (entity.pointEntity())
              {
                // writing the rotation without changing it converts it to "angles",
                // unless the rotation is degenerate and has no angles
                const auto update = applyEntityRotation(
                  entity, vm::mat4x4d::identity(), EntityRotationPolicy::IdTech4);
                if (update.set)
                {
                  applyEntityRotationUpdate(entity, update);
                  nodesToSwap.emplace_back(entityNode, NodeContents{std::move(entity)});
                }
              }
            }

            if (!nodesToSwap.empty())
            {
              updateNodeContents(map, "Convert to Angles", std::move(nodesToSwap));
            }
          }};
}

} // namespace

EntityRotationKeyValidator::EntityRotationKeyValidator()
  : Validator{Type, "Entity rotation property that TrenchBroom does not write"}
{
  addQuickFix(makeDeletePropertyQuickFix());
  addQuickFix(makeConvertToAnglesQuickFix());
}

void EntityRotationKeyValidator::doValidate(
  EntityNode& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const
{
  const auto& entity = entityNode.entity();
  if (entityNode.hasChildren())
  {
    for (const auto& key : {EntityPropertyKeys::Rotation, EntityPropertyKeys::Angles})
    {
      if (entity.hasProperty(key))
      {
        issues.push_back(std::make_unique<EntityPropertyIssue>(
          Type,
          entityNode,
          key,
          entityNode.name()
            + " has brushes or patches, which the game rotates again by its '" + key
            + "' property"));
      }
    }
  }
  else if (const auto keys = idTech4ConvertedRotationKeys(entity); !keys.empty())
  {
    // a degenerate rotation has no angles, so neither a rotate nor Convert to Angles
    // replaces it
    const auto convertible =
      applyEntityRotation(entity, vm::mat4x4d::identity(), EntityRotationPolicy::IdTech4)
        .set.has_value();
    const auto readKey =
      entityRotationInfo(entity, EntityRotationPolicy::IdTech4).propertyKey;

    for (const auto& key : keys)
    {
      const auto description =
        convertible ? "which TrenchBroom replaces by 'angles' when the entity is rotated"
        : key == readKey
          ? "which is degenerate and cannot be converted to 'angles'; edit it by hand"
          : "which cannot be converted to 'angles' because the rotation in '" + readKey
              + "' is degenerate; edit it by hand";
      issues.push_back(std::make_unique<EntityPropertyIssue>(
        Type,
        entityNode,
        key,
        entityNode.name() + " has its rotation in '" + key + "', " + description));
    }
  }
}

} // namespace tb::mdl
