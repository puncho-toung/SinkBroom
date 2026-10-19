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

#include "ui/CreatePrefabTool.h"

#include "base/Logger.h"
#include "base/Macros.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNaming.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/k.h"
#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/vector_utils.h"

#include <ranges>
#include <string>
#include <vector>

namespace tb::ui
{
namespace
{

const auto TransactionName = std::string{"Place Prefab"};

/**
 * Returns the first group, in the order of the world, that was placed from the prefab
 * file with the given path and of which a linked duplicate can be added under the given
 * parent, that is, neither the parent nor one of its ancestors is in its link set.
 */
mdl::GroupNode* findPlacedAssembly(
  mdl::WorldNode& worldNode, const std::string& prefabPath, const mdl::Node& parent)
{
  auto parentLinkIds = std::vector<std::string>{};
  for (const auto* node = &parent; node != nullptr; node = node->parent())
  {
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(node))
    {
      parentLinkIds.push_back(groupNode->linkId());
    }
  }

  auto* result = static_cast<mdl::GroupNode*>(nullptr);
  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, mdl::LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, mdl::GroupNode& node) {
      if (result)
      {
        return;
      }
      if (
        node.group().prefabPath() == prefabPath
        && !kdl::ranges::contains(parentLinkIds, node.linkId()))
      {
        result = &node;
        return;
      }
      node.visitChildren(thisLambda);
    },
    [](mdl::EntityNode&) {},
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));
  return result;
}

/**
 * A prefab body (C2) is one entity for an entity snapshot and one group for an assembly.
 */
bool isWellFormedBody(const std::vector<mdl::Node*>& nodes, const mdl::PrefabKind kind)
{
  if (nodes.size() != 1u)
  {
    return false;
  }

  switch (kind)
  {
  case mdl::PrefabKind::Entity:
    return dynamic_cast<const mdl::EntityNode*>(nodes.front()) != nullptr;
  case mdl::PrefabKind::Assembly:
    return dynamic_cast<const mdl::GroupNode*>(nodes.front()) != nullptr;
    switchDefault();
  }
}

} // namespace

CreatePrefabTool::CreatePrefabTool(MapDocument& document)
  : Tool{K(initiallyActive)}
  , m_document{document}
{
}

bool CreatePrefabTool::createPrefab(const std::string& payload)
{
  contract_pre(!placing());

  auto& map = m_document.map();
  const auto& library = map.prefabLibrary();

  const auto* entry = library.fromPayload(payload);
  if (!entry)
  {
    return false;
  }

  const auto prefabPath = entry->relativePath.generic_string();
  if (!entry->world)
  {
    map.logger().warn() << "Cannot place prefab '" << prefabPath
                        << "': " << entry->error.value_or("the file was not read");
    return false;
  }

  m_referenceBounds = map.referenceBounds();
  m_placementPlane = std::nullopt;

  auto& parent = mdl::parentForNodes(map);
  const auto kind = entry->metadata.kind;

  auto addedNodes = std::vector<mdl::Node*>{};
  if (
    auto* placedGroupNode = kind == mdl::PrefabKind::Assembly
                              ? findPlacedAssembly(map.worldNode(), prefabPath, parent)
                              : nullptr)
  {
    // a second placement of an assembly is a linked duplicate of the first
    map.startTransaction(TransactionName, mdl::TransactionScope::LongRunning);
    if (auto* groupNode = mdl::placeLinkedDuplicate(map, *placedGroupNode, parent))
    {
      addedNodes.push_back(groupNode);
    }
  }
  else
  {
    auto clones = library.cloneBody(*entry, map.worldBounds());
    if (!isWellFormedBody(clones, kind))
    {
      map.logger().warn() << "Cannot place prefab '" << prefabPath
                          << "': its body is not one "
                          << (kind == mdl::PrefabKind::Entity ? "entity" : "group");
      kdl::vec_clear_and_delete(clones);
      return false;
    }

    if (kind == mdl::PrefabKind::Assembly)
    {
      // the first placement records the file it came from (C3); cloneBody gave it, and
      // every link set inside it, link IDs that are not in the map
      auto* groupNode = static_cast<mdl::GroupNode*>(clones.front());
      auto group = groupNode->group();
      group.setPrefabPath(prefabPath);
      group.setPrefabHash(entry->hash);
      groupNode->setGroup(std::move(group));
    }

    map.startTransaction(TransactionName, mdl::TransactionScope::LongRunning);

    // clones must not be used after this
    const auto nodesToAdd = mdl::prepareNodesForAdding(map, clones);
    if (map.worldNode().entityPropertyConfig().naming)
    {
      mdl::assignUniqueEntityNamesBeforeAdding(
        map,
        nodesToAdd | std::views::values | std::views::join
          | kdl::ranges::to<std::vector>(),
        true);
    }

    addedNodes = mdl::addNodes(map, nodesToAdd);
  }

  if (addedNodes.empty())
  {
    map.cancelTransaction();
    return false;
  }

  mdl::deselectAll(map);
  mdl::selectNodes(map, mdl::collectSelectableNodes(addedNodes, map.editorContext()));
  m_prefabNode = addedNodes.front();

  return true;
}

bool CreatePrefabTool::placing() const
{
  return m_prefabNode != nullptr;
}

void CreatePrefabTool::updateCreatedPrefab(const vm::vec3d& delta)
{
  contract_pre(placing());

  if (!vm::is_zero(delta, vm::Cd::almost_zero()))
  {
    translateSelection(m_document.map(), delta);
  }
}

void CreatePrefabTool::updatePrefabPosition2D(const vm::ray3d& pickRay)
{
  contract_pre(placing());

  const auto toMin = m_referenceBounds.min - pickRay.origin;
  const auto toMax = m_referenceBounds.max - pickRay.origin;
  const auto anchor = dot(toMin, pickRay.direction) > dot(toMax, pickRay.direction)
                        ? m_referenceBounds.min
                        : m_referenceBounds.max;
  const auto dragPlane = vm::plane3d(anchor, -pickRay.direction);

  auto& map = m_document.map();
  const auto& grid = map.grid();
  updateCreatedPrefab(grid.moveDeltaForBounds(
    dragPlane, m_prefabNode->logicalBounds(), map.worldBounds(), pickRay));
}

void CreatePrefabTool::updatePrefabPosition3D(
  const vm::ray3d& pickRay, const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  contract_pre(placing());

  auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto bounds = m_prefabNode->logicalBounds();

  if (!m_placementPlane)
  {
    // the prefab's own faces are in the pick result too; skip them and lock the plane to
    // the first face behind them
    for (const auto& hit : pickResult.all(type(mdl::BrushNode::BrushHitType)))
    {
      if (const auto faceHandle = mdl::hitToFaceHandle(hit))
      {
        if (isPrefabNode(*faceHandle->node()))
        {
          continue;
        }

        m_placementPlane = faceHandle->face().boundary();
        break;
      }
    }
  }

  if (m_placementPlane)
  {
    updateCreatedPrefab(
      grid.moveDeltaForBounds(*m_placementPlane, bounds, map.worldBounds(), pickRay));
  }
  else
  {
    const auto newPosition = vm::point_at_distance(
      pickRay, static_cast<double>(gl::Camera::DefaultPointDistance));
    const auto boundsCenter = bounds.center();
    updateCreatedPrefab(grid.moveDeltaForPoint(boundsCenter, newPosition - boundsCenter));
  }
}

void CreatePrefabTool::commit()
{
  contract_pre(placing());

  m_document.map().commitTransaction();
  m_prefabNode = nullptr;
  m_placementPlane = std::nullopt;
}

void CreatePrefabTool::cancel()
{
  contract_pre(placing());

  m_document.map().cancelTransaction();
  m_prefabNode = nullptr;
  m_placementPlane = std::nullopt;
}

bool CreatePrefabTool::isPrefabNode(const mdl::Node& node) const
{
  return &node == m_prefabNode || node.isDescendantOf(*m_prefabNode);
}

} // namespace tb::ui
