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

#include "mdl/PrefabRefresh.h"

#include "mdl/AddRemoveNodesCommand.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tb::mdl
{
namespace
{

// the naming key of games without naming rules, as in the default link properties
const auto DefaultNameKey = std::string{"name"};

const Object* asObject(const Node& node)
{
  return node.accept(kdl::overload(
    [](const WorldNode&) -> const Object* { return nullptr; },
    [](const LayerNode&) -> const Object* { return nullptr; },
    [](const GroupNode& groupNode) -> const Object* { return &groupNode; },
    [](const EntityNode& entityNode) -> const Object* { return &entityNode; },
    [](const BrushNode& brushNode) -> const Object* { return &brushNode; },
    [](const PatchNode& patchNode) -> const Object* { return &patchNode; }));
}

Object* asObject(Node& node)
{
  return node.accept(kdl::overload(
    [](WorldNode&) -> Object* { return nullptr; },
    [](LayerNode&) -> Object* { return nullptr; },
    [](GroupNode& groupNode) -> Object* { return &groupNode; },
    [](EntityNode& entityNode) -> Object* { return &entityNode; },
    [](BrushNode& brushNode) -> Object* { return &brushNode; },
    [](PatchNode& patchNode) -> Object* { return &patchNode; }));
}

/**
 * Whether the new node can take the old node's link ID: both are groups, brushes or
 * patches, or both are entities of the same class.
 */
bool isCounterpart(const Node& oldNode, const Node& newNode)
{
  return oldNode.accept(kdl::overload(
    [](const WorldNode&) { return false; },
    [](const LayerNode&) { return false; },
    [&](const GroupNode&) { return dynamic_cast<const GroupNode*>(&newNode) != nullptr; },
    [&](const EntityNode& oldEntityNode) {
      const auto* newEntityNode = dynamic_cast<const EntityNode*>(&newNode);
      return newEntityNode
             && newEntityNode->entity().classname() == oldEntityNode.entity().classname();
    },
    [&](const BrushNode&) { return dynamic_cast<const BrushNode*>(&newNode) != nullptr; },
    [&](const PatchNode&) {
      return dynamic_cast<const PatchNode*>(&newNode) != nullptr;
    }));
}

/**
 * The link IDs that the new children take from the old children (F29, §3.6).
 *
 * The mapping is one to one, so a link set inside the prefab (D11) keeps its membership:
 * the members of a set share their new ID and so take one old ID together, and an old ID
 * is never given to two new IDs.
 */
struct LinkIdTransplant
{
  // new link ID -> old link ID
  std::unordered_map<std::string, std::string> oldIds;
  std::unordered_set<std::string> usedOldIds;
  // new entity -> the old entity at its position, whose name it keeps
  std::unordered_map<const EntityNode*, const EntityNode*> oldEntities;
};

void matchChildren(
  const std::vector<Node*>& oldChildren,
  const std::vector<Node*>& newChildren,
  LinkIdTransplant& transplant)
{
  const auto count = std::min(oldChildren.size(), newChildren.size());
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto& oldNode = *oldChildren[i];
    const auto& newNode = *newChildren[i];
    if (!isCounterpart(oldNode, newNode))
    {
      continue;
    }

    const auto& oldId = asObject(oldNode)->linkId();
    const auto& newId = asObject(newNode)->linkId();
    if (const auto it = transplant.oldIds.find(newId); it != transplant.oldIds.end())
    {
      if (it->second != oldId)
      {
        // another member of a link set inside the prefab, which took another old ID
        continue;
      }
    }
    else if (transplant.usedOldIds.contains(oldId))
    {
      // the old ID went to another new node already
      continue;
    }
    else
    {
      transplant.oldIds.emplace(newId, oldId);
      transplant.usedOldIds.insert(oldId);
    }

    if (const auto* newEntityNode = dynamic_cast<const EntityNode*>(&newNode))
    {
      transplant.oldEntities.emplace(
        newEntityNode, static_cast<const EntityNode*>(&oldNode));
    }

    matchChildren(oldNode.children(), newNode.children(), transplant);
  }
}

void applyTransplant(
  const std::vector<Node*>& newNodes, const LinkIdTransplant& transplant)
{
  const auto setLinkId = [&](Node& node) {
    auto* object = asObject(node);
    if (const auto it = transplant.oldIds.find(object->linkId());
        it != transplant.oldIds.end())
    {
      object->setLinkId(it->second);
    }
  };

  Node::visitAll(
    newNodes,
    kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        setLinkId(groupNode);
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        setLinkId(entityNode);
        entityNode.visitChildren(thisLambda);
      },
      [&](BrushNode& brushNode) { setLinkId(brushNode); },
      [&](PatchNode& patchNode) { setLinkId(patchNode); }));
}

/**
 * The point of a node that DD-23 compares between an old child and the new child that
 * took its link ID: a point entity's origin, a brush's or patch's bounds centre; nullopt
 * for a group or a brush entity, whose children are compared instead.
 */
std::optional<vm::vec3d> alignmentPoint(const Node& node)
{
  return node.accept(kdl::overload(
    [](const WorldNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
    [](const LayerNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
    [](const GroupNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
    [](const EntityNode& entityNode) -> std::optional<vm::vec3d> {
      return entityNode.hasChildren() ? std::nullopt
                                      : std::optional{entityNode.entity().origin()};
    },
    [](const BrushNode& brushNode) -> std::optional<vm::vec3d> {
      return brushNode.logicalBounds().center();
    },
    [](const PatchNode& patchNode) -> std::optional<vm::vec3d> {
      return patchNode.logicalBounds().center();
    }));
}

struct Displacements
{
  // of the point entities, which move only when they are moved
  std::vector<vm::vec3d> ofEntities;
  // of the brushes and patches, whose centres also move when they are reshaped
  std::vector<vm::vec3d> ofBrushesAndPatches;
};

/**
 * The displacements from the old children to the new children that took their link IDs,
 * found as matchChildren matched them: at the same position, of the same kind, with the
 * same link ID after the transplant, recursively.
 */
void collectDisplacements(
  const std::vector<Node*>& oldChildren,
  const std::vector<Node*>& newChildren,
  Displacements& displacements)
{
  const auto count = std::min(oldChildren.size(), newChildren.size());
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto& oldNode = *oldChildren[i];
    const auto& newNode = *newChildren[i];
    if (
      !isCounterpart(oldNode, newNode)
      || asObject(oldNode)->linkId() != asObject(newNode)->linkId())
    {
      continue;
    }

    const auto oldPoint = alignmentPoint(oldNode);
    const auto newPoint = alignmentPoint(newNode);
    if (oldPoint && newPoint)
    {
      auto& target = dynamic_cast<const EntityNode*>(&oldNode)
                       ? displacements.ofEntities
                       : displacements.ofBrushesAndPatches;
      target.push_back(*newPoint - *oldPoint);
    }
    else
    {
      collectDisplacements(oldNode.children(), newNode.children(), displacements);
    }
  }
}

/**
 * DD-23: the most common displacement, each rounded to the given grid size; on a tie the
 * zero vector if it is among the most common, else the first of them, the entities'
 * coming before the brushes' and patches'. The zero vector if there is none.
 */
vm::vec3d modalDisplacement(const Displacements& displacements, const double gridSize)
{
  const auto round = [&](const vm::vec3d& v) {
    return vm::vec3d{
      gridSize * std::round(v.x() / gridSize),
      gridSize * std::round(v.y() / gridSize),
      gridSize * std::round(v.z() / gridSize)};
  };

  // in the order in which they first appear
  auto counts = std::vector<std::pair<vm::vec3d, std::size_t>>{};
  const auto count = [&](const std::vector<vm::vec3d>& values) {
    for (const auto& value : values)
    {
      const auto rounded = round(value);
      if (const auto it = std::ranges::find_if(
            counts, [&](const auto& valueCount) { return valueCount.first == rounded; });
          it != counts.end())
      {
        ++it->second;
      }
      else
      {
        counts.emplace_back(rounded, 1u);
      }
    }
  };
  count(displacements.ofEntities);
  count(displacements.ofBrushesAndPatches);

  auto result = vm::vec3d{0, 0, 0};
  auto maxCount = std::size_t(0);
  for (const auto& [value, valueCount] : counts)
  {
    if (valueCount > maxCount || (valueCount == maxCount && value == vm::vec3d{0, 0, 0}))
    {
      result = value;
      maxCount = valueCount;
    }
  }
  return result;
}

/**
 * DD-23 (H-E-5): the translation that keeps the new children that did not move in the
 * file where the old children were. A file saved from loose nodes has a group without a
 * transformation, so that a change of its body's bounds moves its reference point (D10)
 * and with it every child; a file saved from one group carries the reference point in
 * the group's transformation, which the linked group update undoes, so that its new
 * children are where the file puts them and need no correction.
 */
vm::vec3d alignmentCorrection(
  const Map& map,
  const GroupNode& fileGroup,
  const std::vector<Node*>& oldChildren,
  const std::vector<Node*>& newChildren)
{
  if (fileGroup.group().transformation() != vm::mat4x4d::identity())
  {
    return vm::vec3d{0, 0, 0};
  }

  auto displacements = Displacements{};
  collectDisplacements(oldChildren, newChildren, displacements);
  return -modalDisplacement(displacements, map.grid().actualSize());
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

const std::string& nameKeyOf(const Map& map)
{
  const auto& naming = map.worldNode().entityPropertyConfig().naming;
  return naming ? naming->nameKey : DefaultNameKey;
}

std::string nameOf(const Entity& entity, const std::string& nameKey)
{
  const auto* name = entity.property(nameKey);
  return name ? *name : std::string{};
}

/**
 * Names the new children as a linked group update names the children of a target group,
 * with the old children's names free: an entity that took an old entity's link ID keeps
 * the old entity's name, every other entity keeps its name from the file if no other
 * entity holds it, else it gets a new one. The link sources among the new children are
 * changed to follow the names, as a paste changes them.
 */
void nameNewChildren(
  Map& map,
  const GroupNode& firstMember,
  const std::vector<Node*>& newChildren,
  const LinkIdTransplant& transplant)
{
  const auto& nameKey = nameKeyOf(map);
  const auto entityNodes = collectEntityNodes(newChildren);

  // the names in the file, before any is changed
  auto fileNames = std::vector<std::string>{};
  fileNames.reserve(entityNodes.size());
  for (auto* entityNode : entityNodes)
  {
    fileNames.push_back(nameOf(entityNode->entity(), nameKey));
    if (const auto it = transplant.oldEntities.find(entityNode);
        it != transplant.oldEntities.end())
    {
      if (const auto oldName = nameOf(it->second->entity(), nameKey); !oldName.empty())
      {
        auto entity = entityNode->entity();
        entity.addOrUpdateProperty(nameKey, oldName);
        entityNode->setEntity(std::move(entity));
      }
    }
  }

  // the old children are replaced, so their names are free
  auto allocator = EntityNameAllocator{map, {&firstMember}};
  assignUniqueEntityNames(newChildren, allocator, EntityNamingMode::KeepIfFree);

  auto namingResult = EntityNamingResult{};
  for (std::size_t i = 0; i < entityNodes.size(); ++i)
  {
    const auto name = nameOf(entityNodes[i]->entity(), nameKey);
    if (name != fileNames[i])
    {
      namingResult.renamed.emplace_back(fileNames[i], name);
    }
    else
    {
      namingResult.kept.push_back(name);
    }
  }
  remapLinkSourceValues(map.entityDefinitionManager(), newChildren, namingResult);
}

/**
 * Whether the selection holds a node or a face below one of the given groups, which a
 * refresh or the linked group update replaces.
 */
bool selectionIsInside(const Map& map, const std::vector<GroupNode*>& groupNodes)
{
  const auto isInside = [&](const Node* node) {
    return std::ranges::any_of(groupNodes, [&](const auto* groupNode) {
      return node->isDescendantOf(*groupNode);
    });
  };

  const auto& selection = map.selection();
  return std::ranges::any_of(selection.nodes, isInside)
         || std::ranges::any_of(selection.brushFaces, [&](const auto& handle) {
              return isInside(handle.node());
            });
}

std::string describeEntity(const Entity& entity, const std::string& nameKey)
{
  const auto name = nameOf(entity, nameKey);
  return name.empty() ? entity.classname() : name;
}

std::vector<std::string> lostEntities(
  const std::vector<Node*>& oldChildren,
  const LinkIdTransplant& transplant,
  const std::string& nameKey)
{
  auto result = std::vector<std::string>{};
  for (const auto* entityNode : collectEntityNodes(oldChildren))
  {
    if (!transplant.usedOldIds.contains(entityNode->linkId()))
    {
      result.push_back(describeEntity(entityNode->entity(), nameKey));
    }
  }
  return result;
}

/**
 * The body of the prefab file as a detached group that the caller owns, with link IDs
 * that are not in the map.
 */
Result<std::unique_ptr<GroupNode>> cloneFileGroup(
  const Map& map, const PrefabEntry& entry, const std::string& prefabPath)
{
  auto clones = map.prefabLibrary().cloneBody(entry, map.worldBounds());
  auto owned =
    clones | std::views::transform([](auto* node) { return std::unique_ptr<Node>{node}; })
    | kdl::ranges::to<std::vector>();

  if (owned.empty())
  {
    return Error{fmt::format(
      "Cannot refresh prefab '{}': {}",
      prefabPath,
      entry.error.value_or("the file cannot be read"))};
  }

  auto* groupNode = dynamic_cast<GroupNode*>(owned.front().get());
  if (owned.size() != 1u || !groupNode)
  {
    return Error{
      fmt::format("Cannot refresh prefab '{}': its body is not one group", prefabPath)};
  }
  if (!groupNode->hasChildren())
  {
    return Error{
      fmt::format("Cannot refresh prefab '{}': its group is empty", prefabPath)};
  }

  static_cast<void>(owned.front().release());
  return std::unique_ptr<GroupNode>{groupNode};
}

} // namespace

Result<PrefabRefreshReport> refreshPlacement(Map& map, const PrefabPlacement& placement)
{
  contract_pre(placement.firstMember != nullptr);

  if (!placement.entry)
  {
    return Error{fmt::format(
      "Cannot refresh prefab '{}': the file is missing", placement.prefabPath)};
  }

  const auto& entry = *placement.entry;
  auto& firstMember = *placement.firstMember;

  return cloneFileGroup(map, entry, placement.prefabPath)
         | kdl::and_then([&](auto fileGroup) {
             const auto oldChildren = firstMember.children();

             // F29: the new children take the old children's link IDs, so that the
             // protected properties of every member survive the linked group update
             auto transplant = LinkIdTransplant{};
             matchChildren(oldChildren, fileGroup->children(), transplant);
             applyTransplant(fileGroup->children(), transplant);

             if (map.worldNode().entityPropertyConfig().naming)
             {
               nameNewChildren(map, firstMember, fileGroup->children(), transplant);
             }

             // the children of the file group, transformed into the first member, with
             // the first member's protected properties and nested group names, as a
             // linked group update from the file group to the first member would make
             // them
             return updateLinkedGroups(
                      *fileGroup, {&firstMember}, map.worldBounds(), map.taskManager())
                    | kdl::and_then([&](auto updates) -> Result<PrefabRefreshReport> {
                        auto newChildren = std::move(updates.front().second);
                        const auto newChildNodes =
                          newChildren
                          | std::views::transform([](auto& child) { return child.get(); })
                          | kdl::ranges::to<std::vector>();

                        if (map.worldNode().entityPropertyConfig().naming)
                        {
                          // only an entity of a link set inside the prefab can have taken
                          // a name twice, since the update matches entities by link ID
                          auto allocator = EntityNameAllocator{map, {&firstMember}};
                          assignUniqueEntityNames(
                            newChildNodes, allocator, EntityNamingMode::KeepIfFree);
                        }

                        // DD-23: what did not move in the file stays where it was
                        const auto referenceShift = alignmentCorrection(
                          map, *fileGroup, oldChildren, newChildNodes);
                        if (referenceShift != vm::vec3d{0, 0, 0})
                        {
                          auto shiftError = std::optional<std::string>{};
                          translateDetachedNodes(
                            newChildNodes,
                            referenceShift,
                            map.worldBounds(),
                            map.worldNode().entityPropertyConfig())
                            | kdl::transform_error(
                              [&](auto e) { shiftError = std::move(e.msg); });
                          if (shiftError)
                          {
                            return Error{fmt::format(
                              "Cannot refresh prefab '{}': {}",
                              placement.prefabPath,
                              *shiftError)};
                          }
                        }

                        const auto name = entry.metadata.name.empty()
                                            ? placement.prefabPath
                                            : entry.metadata.name;
                        auto transaction = Transaction{map, "Refresh Prefab " + name};

                        if (selectionIsInside(map, placement.members))
                        {
                          // bulk child replacement does not maintain the selection
                          deselectAll(map);
                        }

                        // added before the old children are removed, since removing every
                        // child of a group removes the group
                        auto command =
                          AddRemoveNodesCommand::add({{&firstMember, newChildNodes}});
                        // the command owns them now
                        for (auto& child : newChildren)
                        {
                          static_cast<void>(child.release());
                        }
                        if (!map.executeAndStore(std::move(command)))
                        {
                          transaction.cancel();
                          return Error{fmt::format(
                            "Cannot refresh prefab '{}'", placement.prefabPath)};
                        }
                        setHasPendingChanges({&firstMember}, true);

                        removeNodes(map, oldChildren);

                        // DD-6, H-A-1: the update copies children, not the group, so
                        // every member gets the new hash itself; no containing group is
                        // marked, so only the first member's link set is updated at the
                        // commit
                        auto groupsToSwap =
                          placement.members | std::views::transform([&](auto* member) {
                            auto group = member->group();
                            group.setPrefabHash(entry.hash);
                            return std::pair{
                              static_cast<Node*>(member), NodeContents{std::move(group)}};
                          })
                          | kdl::ranges::to<std::vector>();
                        if (!updateNodeContents(
                              map, "Set Prefab Hash", std::move(groupsToSwap), {}))
                        {
                          transaction.cancel();
                          return Error{fmt::format(
                            "Cannot refresh prefab '{}'", placement.prefabPath)};
                        }

                        if (!transaction.commit())
                        {
                          // the linked group update failed and cancelled the transaction
                          return Error{fmt::format(
                            "Cannot refresh prefab '{}': its linked copies cannot be "
                            "updated",
                            placement.prefabPath)};
                        }

                        auto report = PrefabRefreshReport{};
                        report.lostEntities =
                          lostEntities(oldChildren, transplant, nameKeyOf(map));
                        report.entitiesLostOverrides = report.lostEntities.size();
                        report.referenceShift = referenceShift;
                        return report;
                      });
           });
}

Result<std::size_t> applyPrefabToSelection(Map& map, const PrefabEntry& entry)
{
  if (entry.metadata.kind != PrefabKind::Entity || !entry.world)
  {
    return Error{fmt::format(
      "Cannot apply prefab '{}': it is not an entity snapshot", entry.metadata.name)};
  }

  const auto* snapshotNode = static_cast<const EntityNode*>(nullptr);
  for (const auto* layerNode : entry.world->allLayers())
  {
    for (const auto* node : layerNode->children())
    {
      if (!snapshotNode)
      {
        snapshotNode = dynamic_cast<const EntityNode*>(node);
      }
    }
  }
  if (!snapshotNode)
  {
    return Error{
      fmt::format("Cannot apply prefab '{}': it holds no entity", entry.metadata.name)};
  }
  const auto& snapshot = snapshotNode->entity();

  const auto& naming = map.worldNode().entityPropertyConfig().naming;
  const auto& nameKey = nameKeyOf(map);

  auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
  for (auto* entityNodeBase : map.selection().allEntities())
  {
    auto* entityNode = dynamic_cast<EntityNode*>(entityNodeBase);
    if (!entityNode || entityNode->entity().classname() != entry.metadata.className)
    {
      continue;
    }

    const auto& entity = entityNode->entity();
    const auto linkSourceDefinitions =
      getLinkSourcePropertyDefinitions(entity.definition());
    const auto keepsOwnValue = [&](const std::string& key) {
      return key == nameKey || key == EntityPropertyKeys::Origin
             || isPrefabLinkKey(linkSourceDefinitions, key)
             || (naming && naming->brushEntityModelKey
                 && key == *naming->brushEntityModelKey && entityNode->hasChildren())
             || kdl::ranges::contains(entity.protectedProperties(), key);
    };

    // in the snapshot's order; a kept key takes the entity's value, or is left out if
    // the entity has none, and a kept key that the snapshot lacks is added at the end
    auto properties = std::vector<EntityProperty>{};
    for (const auto& property : snapshot.properties())
    {
      if (!keepsOwnValue(property.key()))
      {
        properties.push_back(property);
      }
      else if (const auto* value = entity.property(property.key()))
      {
        properties.emplace_back(property.key(), *value);
      }
    }
    for (const auto& property : entity.properties())
    {
      if (keepsOwnValue(property.key()) && !snapshot.hasProperty(property.key()))
      {
        properties.push_back(property);
      }
    }

    auto newEntity = entity;
    newEntity.setProperties(std::move(properties));
    nodesToSwap.emplace_back(entityNode, NodeContents{std::move(newEntity)});
  }

  const auto count = nodesToSwap.size();
  if (count == 0)
  {
    return std::size_t(0);
  }

  if (!updateNodeContents(map, "Apply Prefab", std::move(nodesToSwap)))
  {
    return Error{fmt::format("Cannot apply prefab '{}'", entry.metadata.name)};
  }
  return count;
}

bool canApplyPrefabToSelection(const Map& map, const PrefabEntry& entry)
{
  return entry.metadata.kind == PrefabKind::Entity && entry.world
         && std::ranges::any_of(
           map.selection().allEntities(), [&](const auto* entityNodeBase) {
             return dynamic_cast<const EntityNode*>(entityNodeBase)
                    && entityNodeBase->entity().classname() == entry.metadata.className;
           });
}

} // namespace tb::mdl
