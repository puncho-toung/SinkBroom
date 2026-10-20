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
#include "mdl/EditorContext.h"
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
#include "kd/hash_utils.h"
#include "kd/invoke.h"
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
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
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
  // the link IDs of the groups that are not below a member of the placement; a nested
  // group whose old ID one of them holds keeps its fresh ID, since taking the old one
  // would join that group's link set (R1-pf-mdl-3)
  std::unordered_set<std::string> outsideGroupIds;
  // new link ID -> old link ID
  std::unordered_map<std::string, std::string> oldIds;
  std::unordered_set<std::string> usedOldIds;
  // new entity -> the old entity at its position, whose name it keeps
  std::unordered_map<const EntityNode*, const EntityNode*> oldEntities;
};

/**
 * The link IDs of the groups below the given nodes that a group of the map holds which
 * is neither one of the given members nor below one.
 */
std::unordered_set<std::string> groupIdsOutside(
  const Map& map,
  const std::vector<GroupNode*>& members,
  const std::vector<Node*>& oldChildren)
{
  // R2-pf-mdl-12: only the IDs that a transplant could take are looked for; they live in
  // the old children, which outlive the walk
  auto nestedIds = std::unordered_set<std::string_view>{};
  Node::visitAll(
    oldChildren,
    kdl::overload(
      [](auto&& thisLambda, const WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, const GroupNode& groupNode) {
        nestedIds.insert(groupNode.linkId());
        groupNode.visitChildren(thisLambda);
      },
      [](const EntityNode&) {},
      [](const BrushNode&) {},
      [](const PatchNode&) {}));

  auto result = std::unordered_set<std::string>{};
  if (nestedIds.empty())
  {
    return result;
  }

  const auto memberSet = std::unordered_set<const Node*>{members.begin(), members.end()};
  map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, const WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const GroupNode& groupNode) {
      if (!memberSet.contains(&groupNode))
      {
        if (nestedIds.contains(groupNode.linkId()))
        {
          result.insert(groupNode.linkId());
        }
        groupNode.visitChildren(thisLambda);
      }
    },
    [](const EntityNode&) {},
    [](const BrushNode&) {},
    [](const PatchNode&) {}));
  return result;
}

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
    if (
      dynamic_cast<const GroupNode*>(&oldNode)
      && transplant.outsideGroupIds.contains(oldId))
    {
      // its entities lose their overrides, since their old IDs go unused
      continue;
    }

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
 * A displacement rounded to the grid, in grid steps, so that equal displacements compare
 * and hash exactly.
 */
struct GridSteps
{
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  bool operator==(const GridSteps&) const = default;

  bool isZero() const { return x == 0 && y == 0 && z == 0; }
};

struct HashGridSteps
{
  std::size_t operator()(const GridSteps& steps) const
  {
    return kdl::hash(steps.x, steps.y, steps.z);
  }
};

/**
 * DD-23: the most common displacement, each rounded to the given grid size; on a tie the
 * zero vector if it is among the most common, else the first of them, the entities'
 * coming before the brushes' and patches'. The zero vector if there is none.
 */
vm::vec3d modalDisplacement(const Displacements& displacements, const double gridSize)
{
  const auto toSteps = [&](const vm::vec3d& v) {
    return GridSteps{
      static_cast<std::int64_t>(std::round(v.x() / gridSize)),
      static_cast<std::int64_t>(std::round(v.y() / gridSize)),
      static_cast<std::int64_t>(std::round(v.z() / gridSize))};
  };

  struct StepsCount
  {
    std::size_t count = 0;
    // the index of the first displacement with these steps, for the tie rule
    std::size_t firstIndex = 0;
  };

  // R2-pf-mdl-11: one lookup per displacement
  auto counts = std::unordered_map<GridSteps, StepsCount, HashGridSteps>{};
  auto index = std::size_t(0);
  const auto count = [&](const std::vector<vm::vec3d>& values) {
    for (const auto& value : values)
    {
      auto& stepsCount =
        counts.try_emplace(toSteps(value), StepsCount{0, index}).first->second;
      ++stepsCount.count;
      ++index;
    }
  };
  count(displacements.ofEntities);
  count(displacements.ofBrushesAndPatches);

  const auto* result =
    static_cast<const std::pair<const GridSteps, StepsCount>*>(nullptr);
  for (const auto& stepsCount : counts)
  {
    if (
      !result || stepsCount.second.count > result->second.count
      || (stepsCount.second.count == result->second.count
          && !result->first.isZero()
          && (stepsCount.first.isZero()
              || stepsCount.second.firstIndex < result->second.firstIndex)))
    {
      result = &stepsCount;
    }
  }

  return result ? vm::vec3d{
                    gridSize * static_cast<double>(result->first.x),
                    gridSize * static_cast<double>(result->first.y),
                    gridSize * static_cast<double>(result->first.z)}
                : vm::vec3d{0, 0, 0};
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

  // the old children are replaced, so their names are free
  auto allocator = EntityNameAllocator{map, {&firstMember}};

  // the names in the file, before any is changed; an entity that took an old entity's
  // link ID takes its name, which is reserved before any other entity is named, so that
  // no entity before it in the file can take it (R1-pf-mdl-2)
  auto fileNames = std::vector<std::string>{};
  fileNames.reserve(entityNodes.size());
  auto otherEntityNodes = std::vector<EntityNode*>{};
  for (auto* entityNode : entityNodes)
  {
    fileNames.push_back(nameOf(entityNode->entity(), nameKey));
    const auto it = transplant.oldEntities.find(entityNode);
    const auto oldName = it != transplant.oldEntities.end()
                           ? nameOf(it->second->entity(), nameKey)
                           : std::string{};
    if (!oldName.empty())
    {
      auto entity = entityNode->entity();
      entity.addOrUpdateProperty(nameKey, oldName);
      entityNode->setEntity(std::move(entity));
      allocator.reserve(oldName);
    }
    else
    {
      otherEntityNodes.push_back(entityNode);
    }
  }

  // the others as assignUniqueEntityNames names them: every free name is kept and
  // reserved first, then the others get new names
  const auto& config = allocator.config();
  auto keepsName = std::vector<bool>{};
  keepsName.reserve(otherEntityNodes.size());
  for (const auto* entityNode : otherEntityNodes)
  {
    const auto name = nameOf(entityNode->entity(), nameKey);
    const auto keep = !name.empty() && !allocator.isTaken(name);
    if (keep)
    {
      allocator.reserve(name);
    }
    keepsName.push_back(keep);
  }

  auto unused = EntityNamingResult{};
  for (std::size_t i = 0; i < otherEntityNodes.size(); ++i)
  {
    // R2-pf-mdl-13: the entity is copied only when it changes
    auto& entityNode = *otherEntityNodes[i];
    if (!keepsName[i])
    {
      auto entity = entityNode.entity();
      allocateEntityName(
        entity, entityNode.hasChildren(), allocator, EntityNamingMode::Create, unused);
      entityNode.setEntity(std::move(entity));
    }
    else if (
      entityNode.hasChildren() && config.brushEntityModelKey
      && isBrushEntityModelClass(entityNode.entity().classname()))
    {
      // the brush entity model key follows the name, as allocateEntityName sets it
      const auto name = nameOf(entityNode.entity(), nameKey);
      if (const auto* model = entityNode.entity().property(*config.brushEntityModelKey);
          !model || *model != name)
      {
        auto entity = entityNode.entity();
        entity.addOrUpdateProperty(*config.brushEntityModelKey, name);
        entityNode.setEntity(std::move(entity));
      }
    }
  }

  auto namingResult = EntityNamingResult{};
  for (std::size_t i = 0; i < entityNodes.size(); ++i)
  {
    const auto name = nameOf(entityNodes[i]->entity(), nameKey);
    if (name != fileNames[i])
    {
      namingResult.renamed.emplace_back(fileNames[i], name);
    }
    else if (!name.empty())
    {
      // R2-pf-mdl-10: an entity without a name keeps no name that a link could name
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
  // R2-pf-mdl-5: one walk up from each node
  const auto groupNodeSet =
    std::unordered_set<const Node*>{groupNodes.begin(), groupNodes.end()};
  const auto isInside = [&](const Node* node) {
    for (const auto* ancestor = node->parent(); ancestor; ancestor = ancestor->parent())
    {
      if (groupNodeSet.contains(ancestor))
      {
        return true;
      }
    }
    return false;
  };

  const auto& selection = map.selection();
  return std::ranges::any_of(selection.nodes, isInside)
         || std::ranges::any_of(selection.brushFaces, [&](const auto& handle) {
              return isInside(handle.node());
            });
}

/**
 * Whether the open group lies strictly below one of the given groups.
 */
bool openGroupIsInside(const Map& map, const std::vector<GroupNode*>& groupNodes)
{
  const auto* currentGroup = map.editorContext().currentGroup();
  if (!currentGroup)
  {
    return false;
  }

  const auto groupNodeSet =
    std::unordered_set<const Node*>{groupNodes.begin(), groupNodes.end()};
  for (const auto* ancestor = currentGroup->parent(); ancestor;
       ancestor = ancestor->parent())
  {
    if (groupNodeSet.contains(ancestor))
    {
      return true;
    }
  }
  return false;
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
  contract_pre(placement.firstMember->isDescendantOf(map.worldNode()));

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
             transplant.outsideGroupIds =
               groupIdsOutside(map, placement.members, oldChildren);
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

                        // R1-pf-mdl-1: the pending flag is not undoable, so a refresh
                        // that ends without its commit gives the first member the flag it
                        // had, and the next commit does not propagate a cancelled change;
                        // declared after the transaction, so it runs before an unfinished
                        // transaction is cancelled
                        const auto wasPending = firstMember.hasPendingChanges();
                        const auto restorePending = kdl::invoke_later{[&]() {
                          if (transaction.state() != Transaction::State::Committed)
                          {
                            setHasPendingChanges({&firstMember}, wasPending);
                          }
                        }};

                        // DD-35 (R7-file-1): the children of every member are replaced,
                        // so an open group below one would be left detached, and the next
                        // node added to it would fail; it is closed first, as removeNodes
                        // closes a removed open group
                        while (openGroupIsInside(map, placement.members))
                        {
                          closeGroup(map);
                        }

                        if (selectionIsInside(map, placement.members))
                        {
                          // bulk child replacement does not maintain the selection
                          deselectAll(map);
                        }

                        // DD-6, H-A-1: the update copies children, not the group, so
                        // every member gets the new hash itself; done first, while no
                        // group is pending, and no containing group is marked, so only
                        // the first member's link set is updated at the commit
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

  auto entityNodes = std::vector<EntityNode*>{};
  for (auto* entityNodeBase : map.selection().allEntities())
  {
    auto* entityNode = dynamic_cast<EntityNode*>(entityNodeBase);
    if (entityNode && entityNode->entity().classname() == entry.metadata.className)
    {
      entityNodes.push_back(entityNode);
    }
  }

  // R1-pf-mdl-5: entities in two members of one link set would make both members
  // pending, which cancels the commit (F9), so an entity in a member other than the
  // first, in the order of the world, of those that hold selected entities is changed
  // through its linked copy in that first member, once, and the linked group update
  // carries the change to the others
  auto groupsByLinkId = std::unordered_map<std::string, std::unordered_set<GroupNode*>>{};
  for (auto* entityNode : entityNodes)
  {
    if (auto* groupNode = entityNode->containingGroup())
    {
      groupsByLinkId[groupNode->linkId()].insert(groupNode);
    }
  }
  // R2-pf-mdl-7: one walk of the world finds the first group, in the order of the world,
  // of every link set whose selected entities lie in two or more of its members
  auto firstGroups = std::unordered_map<std::string, GroupNode*>{};
  auto linkIdsToWalk = std::unordered_set<std::string_view>{};
  for (const auto& [linkId, groupNodes] : groupsByLinkId)
  {
    if (groupNodes.size() == 1u)
    {
      firstGroups.emplace(linkId, *groupNodes.begin());
    }
    else
    {
      linkIdsToWalk.insert(linkId);
    }
  }
  if (!linkIdsToWalk.empty())
  {
    map.worldNode().accept(kdl::overload(
      [](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        if (const auto it = linkIdsToWalk.find(groupNode.linkId());
            it != linkIdsToWalk.end()
            && groupsByLinkId.at(groupNode.linkId()).contains(&groupNode))
        {
          firstGroups.emplace(groupNode.linkId(), &groupNode);
          linkIdsToWalk.erase(it);
        }
        if (!linkIdsToWalk.empty())
        {
          groupNode.visitChildren(thisLambda);
        }
      },
      [](EntityNode&) {},
      [](BrushNode&) {},
      [](PatchNode&) {}));
  }

  // R2-pf-mdl-8: link ID -> entity among the children of a first group, made once per
  // first group that a selected entity of another member needs
  auto linkedEntities = std::
    unordered_map<const GroupNode*, std::unordered_map<std::string_view, EntityNode*>>{};
  const auto entityToChange = [&](EntityNode& entityNode) -> EntityNode* {
    auto* groupNode = entityNode.containingGroup();
    if (!groupNode)
    {
      return &entityNode;
    }
    auto* firstGroup = firstGroups.at(groupNode->linkId());
    if (firstGroup == groupNode)
    {
      return &entityNode;
    }

    auto [it, inserted] = linkedEntities.try_emplace(firstGroup);
    if (inserted)
    {
      for (auto* child : firstGroup->children())
      {
        if (auto* linkedNode = dynamic_cast<EntityNode*>(child))
        {
          it->second.emplace(linkedNode->linkId(), linkedNode);
        }
      }
    }
    const auto linkedIt = it->second.find(entityNode.linkId());
    return linkedIt != it->second.end() ? linkedIt->second : nullptr;
  };

  // R2-pf-mdl-9: the link source definitions of each entity definition, made once
  auto linkSourceDefinitionsByDefinition =
    std::unordered_map<const EntityDefinition*, std::vector<const PropertyDefinition*>>{};
  const auto linkSourceDefinitionsOf =
    [&](const EntityDefinition* definition) -> const auto& {
    auto [it, inserted] = linkSourceDefinitionsByDefinition.try_emplace(definition);
    if (inserted)
    {
      it->second = getLinkSourcePropertyDefinitions(definition);
    }
    return it->second;
  };

  auto nodesToSwap = std::vector<std::pair<Node*, NodeContents>>{};
  auto changedNodes = std::unordered_set<const EntityNode*>{};
  for (auto* selectedNode : entityNodes)
  {
    auto* entityNode = entityToChange(*selectedNode);
    if (!entityNode || !changedNodes.insert(entityNode).second)
    {
      continue;
    }

    const auto& entity = entityNode->entity();
    const auto& linkSourceDefinitions = linkSourceDefinitionsOf(entity.definition());
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
    return Error{fmt::format(
      "Cannot apply prefab '{}': the linked groups that hold the selected entities "
      "cannot be updated together",
      entry.metadata.name)};
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
