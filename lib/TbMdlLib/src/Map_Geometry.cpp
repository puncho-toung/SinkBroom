/*
 Copyright (C) 2025 Kristian Duske

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

#include "mdl/Map_Geometry.h"

#include "base/Logger.h"
#include "gl/Material.h"
#include "mdl/AddRemoveNodesCommand.h"
#include "mdl/ApplyAndSwap.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/BrushSnap.h"
#include "mdl/BrushVertexCommands.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeHandles.h"
#include "mdl/PatchNode.h"
#include "mdl/Polyhedron3.h"
#include "mdl/SetLinkIdsCommand.h"
#include "mdl/SwapNodeContentsCommand.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"

#include "kd/flat_map.h"
#include "kd/overload.h"
#include "kd/ranges/as_rvalue_view.h"
#include "kd/ranges/to.h"
#include "kd/reflection_impl.h"
#include "kd/result_fold.h"
#include "kd/string_format.h"
#include "kd/task_manager.h"
#include "kd/vector_utils.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::mdl
{

kdl_reflect_impl(TransformVerticesResult);

namespace
{

/**
 * The link IDs of the brushes among the given nodes that are in a closed group and that
 * more than one object in the world carries, found in one pass over the world rather
 * than in one pass per brush.
 */
std::unordered_set<std::string> collectLinkedBrushIdsInClosedGroups(
  WorldNode& worldNode, const std::vector<Node*>& nodes)
{
  auto counts = std::unordered_map<std::string, size_t>{};
  for (const auto* node : nodes)
  {
    if (const auto* brushNode = dynamic_cast<const BrushNode*>(node))
    {
      if (const auto* containingGroup = brushNode->containingGroup();
          containingGroup && containingGroup->closed())
      {
        counts.try_emplace(brushNode->linkId(), 0);
      }
    }
  }

  if (counts.empty())
  {
    return {};
  }

  const auto count = [&](const auto& object) {
    if (const auto it = counts.find(object.linkId()); it != counts.end())
    {
      ++it->second;
    }
  };

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, LayerNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, GroupNode& node) {
      count(node);
      node.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, EntityNode& node) {
      count(node);
      node.visitChildren(thisLambda);
    },
    [&](BrushNode& node) { count(node); },
    [&](PatchNode& node) { count(node); }));

  auto result = std::unordered_set<std::string>{};
  for (const auto& [linkId, linkIdCount] : counts)
  {
    if (linkIdCount > 1)
    {
      result.insert(linkId);
    }
  }
  return result;
}

} // namespace

bool transformSelection(
  Map& map, const std::string& commandName, const vm::mat4x4d& transformation)
{
  if (map.nodeHandles().anyHandleSelected<VertexHandle>())
  {
    const auto selectedVertexPositions =
      VertexHandle::getPositions(map.nodeHandles().selectedHandles<VertexHandle>());
    return transformVertices(map, selectedVertexPositions, transformation).success;
  }

  auto nodesToTransform = std::vector<Node*>{};
  auto entitiesToTransform = std::unordered_map<EntityNodeBase*, size_t>{};

  for (auto* node : map.selection().nodes)
  {
    node->accept(kdl::overload(
      [&](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [&](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        nodesToTransform.push_back(&groupNode);
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        if (!entityNode.hasChildren())
        {
          nodesToTransform.push_back(&entityNode);
        }
        else
        {
          entityNode.visitChildren(thisLambda);
        }
      },
      [&](BrushNode& brushNode) {
        nodesToTransform.push_back(&brushNode);
        entitiesToTransform[brushNode.entity()]++;
      },
      [&](PatchNode& patchNode) {
        nodesToTransform.push_back(&patchNode);
        entitiesToTransform[patchNode.entity()]++;
      }));
  }

  // add entities if all of their children are transformed
  for (const auto& [entityNode, transformedChildCount] : entitiesToTransform)
  {
    if (
      transformedChildCount == entityNode->childCount()
      && !isWorldspawn(entityNode->entity().classname()))
    {
      nodesToTransform.push_back(entityNode);
    }
  }

  using TransformResult = Result<std::pair<Node*, NodeContents>>;

  const auto alignmentLock = map.editorContext().alignmentLock();
  const auto& entityPropertyConfig = map.worldNode().entityPropertyConfig();
  const auto updateAngleProperty = entityPropertyConfig.updateAnglePropertyAfterTransform;
  const auto rotationPolicy = entityPropertyConfig.rotationPolicy;

  // a brush in a closed group that is linked to another object keeps its alignment
  const auto linkedBrushIds = alignmentLock ? std::unordered_set<std::string>{}
                                            : collectLinkedBrushIdsInClosedGroups(
                                                map.worldNode(), nodesToTransform);

  auto tasks =
    nodesToTransform | std::views::transform([&](auto& node) {
      return std::function{[&]() {
        return node->accept(kdl::overload(
          [&](WorldNode&) -> TransformResult { contract_assert(false); },
          [&](LayerNode&) -> TransformResult { contract_assert(false); },
          [&](GroupNode& groupNode) -> TransformResult {
            auto group = groupNode.group();
            group.transform(transformation);
            return std::make_pair(&groupNode, NodeContents{std::move(group)});
          },
          [&](EntityNode& entityNode) -> TransformResult {
            auto entity = entityNode.entity();
            entity.transform(transformation, updateAngleProperty, rotationPolicy);
            return std::make_pair(&entityNode, NodeContents{std::move(entity)});
          },
          [&](BrushNode& brushNode) -> TransformResult {
            const auto* containingGroup = brushNode.containingGroup();
            const bool lockAlignment =
              alignmentLock
              || (containingGroup && containingGroup->closed()
                  && linkedBrushIds.contains(brushNode.linkId()));

            auto brush = brushNode.brush();
            return brush.transform(map.worldBounds(), transformation, lockAlignment)
                   | kdl::and_then([&]() -> TransformResult {
                       return std::make_pair(&brushNode, NodeContents{std::move(brush)});
                     });
          },
          [&](PatchNode& patchNode) -> TransformResult {
            auto patch = patchNode.patch();
            patch.transform(transformation);
            return std::make_pair(&patchNode, NodeContents{std::move(patch)});
          }));
      }};
    });

  const auto success = map.taskManager().run_tasks_and_wait(tasks) | kdl::fold
                       | kdl::transform([&](auto nodesToUpdate) {
                           return updateNodeContents(
                             map,
                             commandName,
                             std::move(nodesToUpdate),
                             collectContainingGroups(map.selection().nodes));
                         })
                       | kdl::value_or(false);

  if (success)
  {
    map.pushRepeatableCommand([&, commandName, transformation]() {
      transformSelection(map, commandName, transformation);
    });
  }

  return success;
}

bool translateSelection(mdl::Map& map, const vm::vec3d& delta)
{
  return transformSelection(map, "Translate Objects", vm::translation_matrix(delta));
}

bool rotateSelection(
  mdl::Map& map, const vm::vec3d& center, const vm::vec3d& axis, double angle)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::rotation_matrix(axis, angle)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Rotate Objects", transformation);
}

bool scaleSelection(mdl::Map& map, const vm::bbox3d& oldBBox, const vm::bbox3d& newBBox)
{
  const auto transformation = vm::scale_bbox_matrix(oldBBox, newBBox);
  return transformSelection(map, "Scale Objects", transformation);
}

bool scaleSelection(mdl::Map& map, const vm::vec3d& center, const vm::vec3d& scaleFactors)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::scaling_matrix(scaleFactors)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Scale Objects", transformation);
}

bool shearSelection(
  mdl::Map& map,
  const vm::bbox3d& box,
  const vm::vec3d& sideToShear,
  const vm::vec3d& delta)
{
  const auto transformation = vm::shear_bbox_matrix(box, sideToShear, delta);
  return transformSelection(map, "Scale Objects", transformation);
}

bool flipSelection(mdl::Map& map, const vm::vec3d& center, const vm::axis::type axis)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::mirror_matrix<double>(axis)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Flip Objects", transformation);
}


TransformVerticesResult transformVertices(
  Map& map, const std::vector<vm::vec3d>& vertexPositions, const vm::mat4x4d& transform)
{
  auto newVertexPositions = std::vector<vm::vec3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto verticesToMove = vertexPositions
                                    | std::views::filter([&](const auto& vertex) {
                                        return brush.hasVertex(vertex);
                                      })
                                    | kdl::ranges::to<std::vector>();
        if (verticesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformVertices(map.worldBounds(), verticesToMove, transform))
        {
          return false;
        }

        return brush.transformVertices(
                 map.worldBounds(),
                 verticesToMove,
                 transform,
                 map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions =
                     brush.findClosestVertexPositions(transform * verticesToMove);
                   kdl::vec_append(newVertexPositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush vertices: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (!newNodes)
  {
    return TransformVerticesResult{false, false};
  }

  kdl::vec_sort_and_remove_duplicates(newVertexPositions);
  const auto hasRemainingVertices = !newVertexPositions.empty();

  auto commandName =
    kdl::str_plural(vertexPositions.size(), "Move Brush Vertex", "Move Brush Vertices");
  auto transaction = Transaction{map, commandName};

  const auto changedLinkedGroups = collectContainingGroups(
    *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

  auto command = std::make_unique<BrushVertexCommand>(
    std::move(commandName), std::move(*newNodes), vertexPositions, newVertexPositions);

  if (!map.executeAndStore(std::move(command)))
  {
    transaction.cancel();
    return TransformVerticesResult{false, false};
  }

  setHasPendingChanges(changedLinkedGroups, true);

  if (!transaction.commit())
  {
    return TransformVerticesResult{false, false};
  }

  return {true, hasRemainingVertices};
}

bool transformEdges(
  Map& map, const std::vector<vm::segment3d>& edgePositions, const vm::mat4x4d& transform)
{
  auto newEdgePositions = std::vector<vm::segment3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto edgesToMove =
          edgePositions
          | std::views::filter([&](const auto& edge) { return brush.hasEdge(edge); })
          | kdl::ranges::to<std::vector>();
        if (edgesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformEdges(map.worldBounds(), edgesToMove, transform))
        {
          return false;
        }

        return brush.transformEdges(
                 map.worldBounds(), edgesToMove, transform, map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions = brush.findClosestEdgePositions(
                     edgesToMove | std::views::transform([&](const auto& edge) {
                       return edge.transform(transform);
                     })
                     | kdl::ranges::to<std::vector>());
                   kdl::vec_append(newEdgePositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush edges: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    kdl::vec_sort_and_remove_duplicates(newEdgePositions);

    const auto commandName =
      kdl::str_plural(edgePositions.size(), "Move Brush Edge", "Move Brush Edges");
    auto transaction = Transaction{map, commandName};

    const auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushEdgeCommand>(
      commandName, std::move(*newNodes), edgePositions, newEdgePositions));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool transformFaces(
  Map& map, const std::vector<vm::polygon3d>& facePositions, const vm::mat4x4d& transform)
{
  auto newFacePositions = std::vector<vm::polygon3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto facesToMove =
          facePositions
          | std::views::filter([&](const auto& face) { return brush.hasFace(face); })
          | kdl::ranges::to<std::vector>();
        if (facesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformFaces(map.worldBounds(), facesToMove, transform))
        {
          return false;
        }

        return brush.transformFaces(
                 map.worldBounds(), facesToMove, transform, map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions = brush.findClosestFacePositions(
                     facesToMove | std::views::transform([&](const auto& face) {
                       return face.transform(transform);
                     })
                     | kdl::ranges::to<std::vector>());
                   kdl::vec_append(newFacePositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush faces: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    kdl::vec_sort_and_remove_duplicates(newFacePositions);

    const auto commandName =
      kdl::str_plural(facePositions.size(), "Move Brush Face", "Move Brush Faces");
    auto transaction = Transaction{map, commandName};

    auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushFaceCommand>(
      commandName, std::move(*newNodes), facePositions, newFacePositions));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool addVertex(Map& map, const vm::vec3d& vertexPosition)
{
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        if (!brush.canAddVertex(map.worldBounds(), vertexPosition))
        {
          return false;
        }

        return brush.addVertex(map.worldBounds(), vertexPosition)
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not add brush vertex: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    const auto commandName = "Add Brush Vertex";
    auto transaction = Transaction{map, commandName};

    const auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushVertexCommand>(
      commandName,
      std::move(*newNodes),
      std::vector<vm::vec3d>{},
      std::vector<vm::vec3d>{vertexPosition}));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool removeVertices(
  Map& map, const std::string& commandName, std::vector<vm::vec3d> vertexPositions)
{
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto verticesToRemove = vertexPositions
                                      | std::views::filter([&](const auto& vertex) {
                                          return brush.hasVertex(vertex);
                                        })
                                      | kdl::ranges::to<std::vector>();
        if (verticesToRemove.empty())
        {
          return true;
        }

        if (!brush.canRemoveVertices(map.worldBounds(), verticesToRemove))
        {
          return false;
        }

        return brush.removeVertices(map.worldBounds(), verticesToRemove)
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not remove brush vertices: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    auto transaction = Transaction{map, commandName};

    auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushVertexCommand>(
      commandName, std::move(*newNodes), vertexPositions, std::vector<vm::vec3d>{}));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool snapVertices(Map& map, const double snapTo)
{
  size_t succeededBrushCount = 0;
  size_t failedBrushCount = 0;

  const auto allSelectedBrushes = map.selection().allBrushes();
  const bool applyAndSwapSuccess = applyAndSwap(
    map,
    "Snap Brush Vertices",
    allSelectedBrushes,
    collectContainingGroups(kdl::vec_static_cast<Node*>(allSelectedBrushes)),
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& originalBrush) {
        if (originalBrush.canSnapVertices(map.worldBounds(), snapTo))
        {
          originalBrush.snapVertices(
            map.worldBounds(), snapTo, map.editorContext().uvLock())
            | kdl::transform([&]() { succeededBrushCount += 1; })
            | kdl::transform_error([&](auto e) {
                map.logger().error() << "Could not snap vertices: " << e.msg;
                failedBrushCount += 1;
              });
        }
        else
        {
          failedBrushCount += 1;
        }
        return true;
      },
      [](BezierPatch&) { return true; }));

  if (!applyAndSwapSuccess)
  {
    return false;
  }
  if (succeededBrushCount > 0)
  {
    map.logger().info() << fmt::format(
      "Snapped vertices of {} {}",
      succeededBrushCount,
      kdl::str_plural(succeededBrushCount, "brush", "brushes"));
  }
  if (failedBrushCount > 0)
  {
    map.logger().info() << fmt::format(
      "Failed to snap vertices of {} {}",
      failedBrushCount,
      kdl::str_plural(failedBrushCount, "brush", "brushes"));
  }

  return true;
}

namespace
{

// A brush as the safe snap check sees it: its vertex positions, one loop of vertex indices
// a face in the face's own boundary order, and the face's three plane points.
brushsnap::Poly toSnapPoly(const Brush& brush)
{
  auto poly = brushsnap::Poly{};
  auto indices = std::map<vm::vec3d, uint32_t>{};
  const auto indexOf = [&](const vm::vec3d& position) {
    const auto [it, added] =
      indices.try_emplace(position, static_cast<uint32_t>(poly.v.size()));
    if (added)
    {
      poly.v.push_back({position.x(), position.y(), position.z()});
    }
    return it->second;
  };

  poly.f.reserve(brush.faceCount());
  poly.facePoints.reserve(brush.faceCount());
  for (const auto& face : brush.faces())
  {
    auto loop = std::vector<uint32_t>{};
    for (const auto* vertex : face.vertices())
    {
      loop.push_back(indexOf(vertex->position()));
    }
    poly.f.push_back(std::move(loop));

    const auto& points = face.points();
    poly.facePoints.push_back({{
      {points[0].x(), points[0].y(), points[0].z()},
      {points[1].x(), points[1].y(), points[1].z()},
      {points[2].x(), points[2].y(), points[2].z()},
    }});
  }
  return poly;
}

struct SafeSnapInput
{
  std::vector<brushsnap::Poly> selected;
  std::vector<brushsnap::Poly> unselected;
  std::vector<const BrushNode*> unselectedNodes;
  std::vector<brushsnap::ContactPair> pairs;
};

// The selected brushes, the other brushes whose bounds touch one of them, and the pairs
// whose bounds touch: only those can share a face or overlap. The octree is asked once, for
// the selection's bounds: it keeps every brush that crosses an axis plane through the origin
// in one node as large as the map, which a query per selected brush would return each time.
// The pairs come from a grid of (x, y) cells. Bounds are widened a little, so brushes that
// only touch are found however a test treats touching bounds.
SafeSnapInput gatherSafeSnapInput(
  const Map& map, const std::vector<BrushNode*>& brushNodes)
{
  constexpr auto QueryMargin = 1.0 / 64.0;
  constexpr auto CellSize = 256.0;
  // a brush over more cells than this is paired by a scan of all brushes instead
  constexpr auto MaxCells = int64_t(64);
  constexpr auto CellOffset = int64_t(1) << 31;

  auto input = SafeSnapInput{};
  if (brushNodes.empty())
  {
    return input;
  }

  // every brush taking part, the selected ones first, and its widened bounds
  auto nodes = std::vector<const BrushNode*>{brushNodes.begin(), brushNodes.end()};
  auto bounds = std::vector<vm::bbox3d>{};
  bounds.reserve(brushNodes.size());
  input.selected.reserve(brushNodes.size());
  auto selected = std::unordered_set<const Node*>{};
  auto selectionBounds = brushNodes.front()->brush().bounds();
  for (const auto* brushNode : brushNodes)
  {
    selected.insert(brushNode);
    input.selected.push_back(toSnapPoly(brushNode->brush()));
    bounds.push_back(brushNode->brush().bounds().expand(QueryMargin));
    selectionBounds = vm::merge(selectionBounds, bounds.back());
  }
  for (const auto* node : map.worldNode().nodeTree().find_intersectors(selectionBounds))
  {
    const auto* brushNode = dynamic_cast<const BrushNode*>(node);
    if (
      brushNode && !selected.contains(brushNode)
      && brushNode->brush().bounds().intersects(selectionBounds))
    {
      nodes.push_back(brushNode);
      bounds.push_back(brushNode->brush().bounds().expand(QueryMargin));
    }
  }

  const auto selectedCount = static_cast<uint32_t>(brushNodes.size());
  const auto cellOf = [&](const double coordinate) {
    return static_cast<int64_t>(std::floor(coordinate / CellSize));
  };
  // cells in x and y only: terrain prisms are tall, and z cells would make them "large"
  const auto cellKey = [&](const int64_t x, const int64_t y) {
    return (static_cast<uint64_t>(x + CellOffset) << 32)
           | static_cast<uint64_t>(y + CellOffset);
  };
  auto cells = std::unordered_map<uint64_t, std::vector<uint32_t>>{};
  auto large = std::vector<uint32_t>{};
  for (uint32_t i = 0; i < nodes.size(); ++i)
  {
    const auto x0 = cellOf(bounds[i].min.x()), x1 = cellOf(bounds[i].max.x());
    const auto y0 = cellOf(bounds[i].min.y()), y1 = cellOf(bounds[i].max.y());
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > MaxCells)
    {
      large.push_back(i);
      continue;
    }
    for (auto x = x0; x <= x1; ++x)
    {
      for (auto y = y0; y <= y1; ++y)
      {
        cells[cellKey(x, y)].push_back(i);
      }
    }
  }

  // a pair as (selected, other), two selected brushes as (lower, higher), once each
  auto keys = std::vector<uint64_t>{};
  const auto consider = [&](uint32_t a, uint32_t b) {
    if (a == b || (a >= selectedCount && b >= selectedCount))
    {
      return;
    }
    if (a >= selectedCount || (b < selectedCount && b < a))
    {
      std::swap(a, b);
    }
    if (bounds[a].intersects(bounds[b]))
    {
      keys.push_back((static_cast<uint64_t>(a) << 32) | b);
    }
  };
  for (const auto& [key, cell] : cells)
  {
    for (size_t i = 0; i < cell.size(); ++i)
    {
      for (size_t j = i + 1; j < cell.size(); ++j)
      {
        consider(cell[i], cell[j]);
      }
    }
  }
  for (const auto i : large)
  {
    for (uint32_t j = 0; j < nodes.size(); ++j)
    {
      consider(i, j);
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

  auto unselectedIndices = std::unordered_map<uint32_t, uint32_t>{};
  input.pairs.reserve(keys.size());
  for (const auto key : keys)
  {
    const auto a = static_cast<uint32_t>(key >> 32);
    const auto b = static_cast<uint32_t>(key);
    if (b < selectedCount)
    {
      input.pairs.emplace_back(a, b);
      continue;
    }
    const auto [it, added] =
      unselectedIndices.try_emplace(b, static_cast<uint32_t>(input.unselected.size()));
    if (added)
    {
      input.unselected.push_back(toSnapPoly(nodes[b]->brush()));
      input.unselectedNodes.push_back(nodes[b]);
    }
    input.pairs.emplace_back(a, selectedCount + it->second);
  }
  return input;
}

using Facets = std::vector<std::vector<vm::vec3d>>;

// The brush's faces as sorted vertex lists, the list sorted: two brushes with the same
// facets have the same faces with the same vertices.
Facets facetsOf(const Brush& brush, const std::optional<double> snapTo = std::nullopt)
{
  auto result = Facets{};
  result.reserve(brush.faceCount());
  for (const auto& face : brush.faces())
  {
    auto facet = face.vertexPositions();
    if (snapTo)
    {
      for (auto& position : facet)
      {
        position = *snapTo * vm::round(position / *snapTo);
      }
    }
    std::sort(facet.begin(), facet.end());
    result.push_back(std::move(facet));
  }
  std::sort(result.begin(), result.end());
  return result;
}

// How deep two convex brushes interpenetrate: the least overlap of their projections over
// the separating axes (the face normals of both and the cross products of their edges), so
// zero or less if they only touch or lie apart. It works on the vertices alone: the brush
// geometry's clean-up, which drops edges under 0.01 units, would lose a thin overlap.
double penetrationDepth(const Brush& a, const Brush& b)
{
  for (size_t i = 0; i < 3; ++i)
  {
    const auto depth = std::min(a.bounds().max[i], b.bounds().max[i])
                       - std::max(a.bounds().min[i], b.bounds().min[i]);
    if (depth <= 0.0)
    {
      return depth;
    }
  }

  const auto edgeDirections = [](const Brush& brush) {
    auto result = std::vector<vm::vec3d>{};
    result.reserve(brush.edgeCount());
    for (const auto* edge : brush.edges())
    {
      result.push_back(vm::normalize(
        edge->secondVertex()->position() - edge->firstVertex()->position()));
    }
    return result;
  };
  auto axes = std::vector<vm::vec3d>{};
  for (const auto* brush : {&a, &b})
  {
    for (const auto& face : brush->faces())
    {
      axes.push_back(face.boundary().normal);
    }
  }
  const auto edgesA = edgeDirections(a);
  const auto edgesB = edgeDirections(b);
  for (const auto& edgeA : edgesA)
  {
    for (const auto& edgeB : edgesB)
    {
      const auto axis = vm::cross(edgeA, edgeB);
      const auto length = vm::length(axis);
      if (length > 1.0e-9)
      {
        axes.push_back(axis / length);
      }
    }
  }

  const auto verticesA = a.vertexPositions();
  const auto verticesB = b.vertexPositions();
  const auto span = [](const std::vector<vm::vec3d>& vertices, const vm::vec3d& axis) {
    auto lo = std::numeric_limits<double>::max();
    auto hi = std::numeric_limits<double>::lowest();
    for (const auto& vertex : vertices)
    {
      const auto d = vm::dot(axis, vertex);
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
    return std::pair{lo, hi};
  };
  auto depth = std::numeric_limits<double>::max();
  for (const auto& axis : axes)
  {
    const auto [loA, hiA] = span(verticesA, axis);
    const auto [loB, hiB] = span(verticesB, axis);
    depth = std::min(depth, std::min(hiA, hiB) - std::max(loA, loB));
    if (depth <= 0.0)
    {
      break;
    }
  }
  return depth;
}

// True if two brushes touch with a positive area: coplanar faces with opposite normals.
bool touchWithArea(const Brush& a, const Brush& b)
{
  auto contacts = std::vector<brushsnap::Contact>{};
  brushsnap::FindContacts({toSnapPoly(a)}, {toSnapPoly(b)}, &contacts);
  return !contacts.empty();
}

} // namespace

bool SafeSnapReport::refused() const
{
  return !error.empty() || !refusedBrushes.empty() || openedContactCount > 0;
}

SafeSnapReport safeSnapVertices(Map& map, const double snapTo)
{
  // Two brushes seal by an overlap if they interpenetrate deeper than this; after the snap
  // any depth above float noise keeps the seal.
  constexpr auto MinSealDepth = 1.0e-4;
  constexpr auto MinSharedDepth = 1.0e-6;

  auto report = SafeSnapReport{};

  const auto brushNodes = map.selection().allBrushes();
  report.brushCount = brushNodes.size();
  if (brushNodes.empty())
  {
    report.error = "No brushes are selected.";
    return report;
  }
  if (
    !(snapTo >= 1.0) || snapTo != std::floor(snapTo)
    || snapTo > double(std::numeric_limits<int32_t>::max()))
  {
    report.error = fmt::format(
      "Safe snap needs a grid of a whole number of units, 1 or more; the grid is {}.",
      snapTo);
    return report;
  }

  auto input = SafeSnapInput{};
  auto plan = brushsnap::SnapPlan{};
  try
  {
    input = gatherSafeSnapInput(map, brushNodes);
    brushsnap::PlanSafeSnap(
      input.selected, input.unselected, input.pairs, static_cast<int32_t>(snapTo), &plan);
    // the seal check needs only the pairs and the unselected nodes
    input.selected = {};
    input.unselected = {};
  }
  catch (const std::exception& e)
  {
    report.error = fmt::format("Safe snap could not check the brushes: {}", e.what());
    return report;
  }

  // A brush inside a group that has linked copies would move those copies as well, and the
  // check never sees them: such a brush is refused. The map's groups are counted by link id
  // in one walk, made only when a selected brush lies in a group.
  auto linkCounts = std::optional<std::unordered_map<std::string, size_t>>{};
  const auto inLinkedGroup = [&](BrushNode& brushNode) {
    if (!brushNode.containingGroup())
    {
      return false;
    }
    if (!linkCounts)
    {
      linkCounts.emplace();
      for (const auto* group : collectGroups({&map.worldNode()}))
      {
        ++(*linkCounts)[group->linkId()];
      }
    }
    for (auto* group = brushNode.containingGroup(); group; group = group->containingGroup())
    {
      if (const auto it = linkCounts->find(group->linkId());
          it != linkCounts->end() && it->second > 1)
      {
        return true;
      }
    }
    return false;
  };

  auto refused = std::vector<bool>(brushNodes.size(), false);
  for (size_t i = 0; i < brushNodes.size(); ++i)
  {
    switch (plan.verdicts[i].verdict)
    {
    case brushsnap::SAME:
      ++report.unchangedCount;
      break;
    case brushsnap::OK:
      if (inLinkedGroup(*brushNodes[i]))
      {
        ++report.linkedCount;
        refused[i] = true;
      }
      break;
    case brushsnap::MERGE:
      ++report.mergeCount;
      refused[i] = true;
      break;
    case brushsnap::FACESET:
      ++report.faceSetCount;
      refused[i] = true;
      break;
    case brushsnap::FLAT:
      ++report.flatCount;
      refused[i] = true;
      break;
    default:
      ++report.rangeCount;
      refused[i] = true;
      break;
    }
  }

  auto opened = std::vector<bool>(brushNodes.size(), false);
  report.openedContactCount = plan.opened.size();  // recounted by pair below when it runs
  for (const auto& contact : plan.opened)
  {
    opened[contact.a] = true;
    if (contact.b < brushNodes.size())
    {
      opened[contact.b] = true;
    }
  }

  // Stage stock TrenchBroom's snap of each accepted brush and keep it only if it has exactly
  // the faces the check expects: stock builds a hull whose epsilon grows with the brush, so
  // on a large brush it can merge faces the exact check keeps apart.
  auto snapped = std::vector<std::optional<Brush>>(brushNodes.size());
  if (plan.commit)
  {
    for (size_t i = 0; i < brushNodes.size(); ++i)
    {
      if (plan.verdicts[i].verdict != brushsnap::OK || refused[i])
      {
        continue;
      }

      auto brush = brushNodes[i]->brush();
      const auto expected = facetsOf(brush, snapTo);
      if (
        !brush.snapVertices(map.worldBounds(), snapTo, map.editorContext().uvLock())
        || facetsOf(brush) != expected)
      {
        ++report.stockMismatchCount;
        refused[i] = true;
        continue;
      }
      snapped[i] = std::move(brush);
    }
  }

  // Every pair where a brush moves and that touches with a face or overlaps deeper than
  // MinSealDepth must stay sealed. A vertex of either brush that lies inside or on the other
  // must still lie inside or on it after the snap: a contact or an overlap that is partly
  // lost opens a gap, or a step where snapped and unsnapped brushes meet. And two brushes
  // that overlapped without a face contact must still overlap or touch with a positive area.
  // A pair counts once, also if the check's own contact test opened it already.
  if (plan.commit && std::ranges::none_of(refused, [](const bool r) { return r; }))
  {
    try
    {
      const auto selectedCount = static_cast<uint32_t>(brushNodes.size());
      const auto before = [&](const uint32_t i) -> const Brush& {
        return i < selectedCount ? brushNodes[i]->brush()
                                 : input.unselectedNodes[i - selectedCount]->brush();
      };
      const auto after = [&](const uint32_t i) -> const Brush& {
        return i < selectedCount && snapped[i] ? *snapped[i] : before(i);
      };
      const auto moved = [&](const uint32_t i, const vm::vec3d& position) {
        return i < selectedCount && snapped[i] ? snapTo * vm::round(position / snapTo)
                                               : position;
      };
      const auto insideOrOn = [](const Brush& brush, const vm::vec3d& position) {
        return std::ranges::all_of(brush.faces(), [&](const auto& face) {
          return face.boundary().point_distance(position) <= brushsnap::PlaneEpsilon;
        });
      };
      // every vertex of y inside or on x before is inside or on x after
      const auto staysContained = [&](const uint32_t x, const uint32_t y) {
        return std::ranges::all_of(before(y).vertexPositions(), [&](const auto& position) {
          return !insideOrOn(before(x), position)
                 || insideOrOn(after(x), moved(y, position));
        });
      };

      auto contactPairs = std::set<std::pair<uint32_t, uint32_t>>{};
      for (const auto& contact : plan.contacts)
      {
        contactPairs.emplace(contact.a, contact.b);
      }
      auto openedPairs = std::set<std::pair<uint32_t, uint32_t>>{};
      for (const auto& contact : plan.opened)
      {
        openedPairs.emplace(contact.a, contact.b);
      }
      report.openedContactCount = openedPairs.size();

      for (const auto& [a, b] : input.pairs)
      {
        const auto moves = snapped[a] || (b < selectedCount && snapped[b]);
        if (!moves || openedPairs.contains({a, b}))
        {
          continue;
        }
        const auto contact = contactPairs.contains({a, b});
        const auto seal =
          !contact && penetrationDepth(before(a), before(b)) >= MinSealDepth;
        if (!contact && !seal)
        {
          continue;
        }
        const auto keeps =
          staysContained(a, b) && staysContained(b, a)
          && (!seal || penetrationDepth(after(a), after(b)) > MinSharedDepth
              || touchWithArea(after(a), after(b)));
        if (keeps)
        {
          continue;
        }
        ++report.openedContactCount;
        opened[a] = true;
        if (b < selectedCount)
        {
          opened[b] = true;
        }
      }
    }
    catch (const std::exception& e)
    {
      report.error = fmt::format("Safe snap could not check the contacts: {}", e.what());
      return report;
    }
  }

  for (size_t i = 0; i < brushNodes.size(); ++i)
  {
    if (refused[i])
    {
      report.refusedBrushes.push_back(brushNodes[i]);
    }
    if (opened[i])
    {
      report.openedContactBrushes.push_back(brushNodes[i]);
    }
  }

  if (report.refused())
  {
    map.logger().info() << fmt::format(
      "Safe snap changed nothing: {} of {} {} would change shape (merge {}, faces {}, "
      "flat {}, range {}, linked {}, stock {}), {} {} would open",
      report.refusedBrushes.size(),
      report.brushCount,
      kdl::str_plural(report.brushCount, "brush", "brushes"),
      report.mergeCount,
      report.faceSetCount,
      report.flatCount,
      report.rangeCount,
      report.linkedCount,
      report.stockMismatchCount,
      report.openedContactCount,
      kdl::str_plural(report.openedContactCount, "contact", "contacts"));
    return report;
  }

  // reserved: NodeContents has no non-throwing move, so a growing vector would copy every
  // staged brush with its geometry
  auto staged = std::vector<std::pair<Node*, NodeContents>>{};
  staged.reserve(static_cast<size_t>(
    std::ranges::count_if(snapped, [](const auto& brush) { return brush.has_value(); })));
  for (size_t i = 0; i < brushNodes.size(); ++i)
  {
    if (snapped[i])
    {
      staged.emplace_back(brushNodes[i], NodeContents{std::move(*snapped[i])});
    }
  }

  if (staged.empty())
  {
    map.logger().info() << fmt::format(
      "Safe snap: all {} {} already on the grid",
      report.brushCount,
      kdl::str_plural(report.brushCount, "brush is", "brushes are"));
    return report;
  }

  // The groups of the snapped brushes: none of them has a linked copy (refused above), so an
  // unchanged linked group elsewhere in the selection does not refuse the snap.
  const auto snappedCount = staged.size();
  auto changedLinkedGroups =
    collectContainingGroups(staged | std::views::keys | kdl::ranges::to<std::vector>());
  if (!updateNodeContents(
        map, "Safe Snap Vertices", std::move(staged), std::move(changedLinkedGroups)))
  {
    report.error =
      "The snapped brushes could not be stored. The usual cause is a linked group whose "
      "copies would change in different ways.";
    return report;
  }

  report.snappedCount = snappedCount;
  report.committed = true;
  map.logger().info() << fmt::format(
    "Safe snap: snapped {} of {} {}",
    report.snappedCount,
    report.brushCount,
    kdl::str_plural(report.brushCount, "brush", "brushes"));
  return report;
}

namespace
{

// The volume of a convex brush, by fans over its faces from a point near the brush, so the
// products stay small far from the origin.
double brushVolume(const Brush& brush)
{
  const auto origin = brush.bounds().center();
  auto sixTimes = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto polygon = face.vertexPositions();
    for (size_t i = 1; i + 1 < polygon.size(); ++i)
    {
      sixTimes += vm::dot(
        polygon[0] - origin, vm::cross(polygon[i] - origin, polygon[i + 1] - origin));
    }
  }
  return std::abs(sixTimes) / 6.0;
}

// The face that closes one side of a cut through the plane `axis` = w: for the piece below
// the plane its outward normal points up the axis, for the piece above, down it. Its points
// lie exactly on the plane, so the cut plane is exact. It takes the material and attributes
// of the brush face whose normal is closest to its own, so the piece keeps only materials
// the brush already has (in Doom 3 a brush's contents come from its materials).
Result<BrushFace> makeCutFace(
  const Brush& brush,
  const vm::axis::type axis,
  const double w,
  const bool below,
  const MapFormat mapFormat)
{
  auto normal = vm::vec3d{0, 0, 0};
  normal[axis] = below ? 1.0 : -1.0;

  const auto u = (axis + 1) % 3;
  const auto v = (axis + 2) % 3;
  auto p0 = vm::vec3d{0, 0, 0};
  p0[axis] = w;
  auto pu = p0;
  pu[u] = 1.0;
  auto pv = p0;
  pv[v] = 1.0;

  // A face's normal is cross(p2 - p0, p1 - p0), and cross(u, v) points up the axis.
  const auto& p1 = below ? pv : pu;
  const auto& p2 = below ? pu : pv;

  const auto& source = *std::ranges::max_element(
    brush.faces(), {}, [&](const auto& face) { return vm::dot(face.normal(), normal); });
  return BrushFace::create(
    p0,
    p1,
    p2,
    source.materialName(),
    source.uvAttributes(),
    source.surfaceAttributes(),
    mapFormat);
}

// True if every face has a loaded material that is neither translucent nor perforated (as
// the editor's material parser marks the engine's coverage): between two pieces of any other
// brush the compiler would draw the cut faces, as it does for a brush whose sides are not all
// opaque. A missing material is not opaque: the engine draws it with its translucent default.
// An image without a material script is translucent too (the engine's implicit material
// blends it, and the editor marks it so); only materials of other games carry no mark.
bool opaqueForChunk(const Brush& brush)
{
  return std::ranges::all_of(brush.faces(), [](const auto& face) {
    const auto* material = face.material();
    return material && !material->surfaceParms().contains("coverage_translucent")
           && !material->surfaceParms().contains("coverage_perforated");
  });
}

// True if a plane w = k * gridSize on a chunked axis lies strictly inside the bounds.
bool crossesGrid(const vm::bbox3d& bounds, const int gridSize, const bool chunkZ)
{
  const auto grid = static_cast<double>(gridSize);
  for (size_t axis = 0; axis < (chunkZ ? size_t(3) : size_t(2)); ++axis)
  {
    if (std::floor(bounds.min[axis] / grid) + 1 < std::ceil(bounds.max[axis] / grid))
    {
      return true;
    }
  }
  return false;
}

// Cuts one brush at the grid planes it strictly crosses, in (axis, plane) order, each piece
// with Brush::clip on two copies. Sweeps repeat until one makes no cut, because a later cut
// can remove the vertex that kept an earlier plane from cutting; the skip counts kept are
// those of the last sweep, the cuts that stay undone.
std::vector<Brush> chunkBrush(
  const Brush& brush,
  const vm::bbox3d& worldBounds,
  const MapFormat mapFormat,
  const int gridSize,
  const bool chunkZ,
  ChunkToGridReport& report)
{
  constexpr auto NearDistance = 0.5;
  constexpr auto MinVolume = 1.0;

  const auto grid = static_cast<double>(gridSize);
  const auto bounds = brush.bounds();
  const auto axisCount = chunkZ ? size_t(3) : size_t(2);

  auto pieces = std::vector<Brush>{brush};
  auto cuts = size_t(0);
  auto previousCuts = size_t(0);
  auto skippedNear = size_t(0);
  auto skippedVolume = size_t(0);
  auto skippedFailed = size_t(0);
  do
  {
    previousCuts = cuts;
    skippedNear = skippedVolume = skippedFailed = 0;
    for (size_t axis = 0; axis < axisCount; ++axis)
    {
      const auto first = static_cast<int64_t>(std::floor(bounds.min[axis] / grid)) + 1;
      const auto last = static_cast<int64_t>(std::ceil(bounds.max[axis] / grid)) - 1;
      for (auto k = first; k <= last; ++k)
      {
        const auto w = static_cast<double>(k) * grid;
        const auto count = pieces.size();
        for (size_t i = 0; i < count; ++i)
        {
          const auto& pieceBounds = pieces[i].bounds();
          if (!(pieceBounds.min[axis] < w && pieceBounds.max[axis] > w))
          {
            continue;
          }

          auto hasBelow = false;
          auto hasAbove = false;
          auto hasNear = false;
          for (const auto* vertex : pieces[i].vertices())
          {
            const auto d = vertex->position()[axis] - w;
            hasBelow = hasBelow || d < 0.0;
            hasAbove = hasAbove || d > 0.0;
            hasNear = hasNear || (d != 0.0 && std::abs(d) < NearDistance);
          }
          if (!hasBelow || !hasAbove)
          {
            continue;
          }
          if (hasNear)
          {
            ++skippedNear;
            continue;
          }

          auto lower = pieces[i];
          auto upper = pieces[i];
          auto lowerFace = makeCutFace(pieces[i], axis, w, true, mapFormat);
          auto upperFace = makeCutFace(pieces[i], axis, w, false, mapFormat);
          if (
            !lowerFace || !upperFace
            || !lower.clip(worldBounds, std::move(lowerFace).value())
            || !upper.clip(worldBounds, std::move(upperFace).value()))
          {
            ++skippedFailed;
            continue;
          }
          if (brushVolume(lower) < MinVolume || brushVolume(upper) < MinVolume)
          {
            ++skippedVolume;
            continue;
          }

          pieces[i] = std::move(lower);
          pieces.push_back(std::move(upper));
          ++cuts;
        }
      }
    }
  } while (cuts != previousCuts);

  report.cutCount += cuts;
  report.skippedNearCount += skippedNear;
  report.skippedVolumeCount += skippedVolume;
  report.skippedFailedCount += skippedFailed;
  return pieces;
}

} // namespace

ChunkToGridReport chunkToGrid(Map& map, const int gridSize, const bool chunkZ)
{
  auto report = ChunkToGridReport{};

  const auto brushNodes = map.selection().brushes;
  report.brushCount = brushNodes.size();
  if (brushNodes.empty() || gridSize < 1)
  {
    return report;
  }

  // Each chunked brush's node keeps its first piece, and the other pieces are added beside
  // it: no node is removed or deselected (both cost the editor time by the selection's size),
  // and the brushes left whole stay selected as they were.
  const auto mapFormat = map.worldNode().mapFormat();
  auto firstPieces = std::vector<std::pair<Node*, NodeContents>>{};
  firstPieces.reserve(brushNodes.size());
  auto otherPieces = std::vector<std::pair<Node*, std::unique_ptr<BrushNode>>>{};
  for (auto* brushNode : brushNodes)
  {
    if (!crossesGrid(brushNode->brush().bounds(), gridSize, chunkZ))
    {
      continue;
    }
    if (!opaqueForChunk(brushNode->brush()))
    {
      ++report.notOpaqueCount;
      continue;
    }

    auto pieces = chunkBrush(
      brushNode->brush(), map.worldBounds(), mapFormat, gridSize, chunkZ, report);
    if (pieces.size() < 2)
    {
      continue;
    }

    report.pieceCount += pieces.size();
    ++report.chunkedCount;
    for (size_t i = 1; i < pieces.size(); ++i)
    {
      otherPieces.emplace_back(
        brushNode->parent(), std::make_unique<BrushNode>(std::move(pieces[i])));
    }
    firstPieces.emplace_back(brushNode, NodeContents{std::move(pieces.front())});
  }

  if (firstPieces.empty())
  {
    map.logger().info() << fmt::format(
      "Chunk to grid: no selected brush was cut at the {} grid", gridSize);
    return report;
  }

  auto transaction = Transaction{map, "Chunk to Grid"};
  auto changedLinkedGroups =
    collectContainingGroups(firstPieces | std::views::keys | kdl::ranges::to<std::vector>());
  if (!updateNodeContents(
        map, "Chunk to Grid", std::move(firstPieces), std::move(changedLinkedGroups)))
  {
    transaction.cancel();
    return report;
  }

  // From here the add command owns the other pieces, and deletes them if it fails.
  auto toAdd = kdl::flat_map<Node*, std::vector<Node*>>{};
  for (auto& [parent, pieceNode] : otherPieces)
  {
    toAdd[parent].push_back(pieceNode.release());
  }
  const auto added = addNodes(map, toAdd);
  if (added.empty())
  {
    transaction.cancel();
    return report;
  }
  selectNodes(map, added);
  report.committed = transaction.commit();

  if (report.committed)
  {
    map.logger().info() << fmt::format(
      "Chunk to grid: cut {} of {} {} into {} pieces ({} cuts; left out: {} near a "
      "vertex, {} too small, {} failed; {} not opaque)",
      report.chunkedCount,
      report.brushCount,
      kdl::str_plural(report.brushCount, "brush", "brushes"),
      report.pieceCount,
      report.cutCount,
      report.skippedNearCount,
      report.skippedVolumeCount,
      report.skippedFailedCount,
      report.notOpaqueCount);
  }
  return report;
}

bool csgConvexMerge(Map& map)
{
  if (!map.selection().hasBrushFaces() && !map.selection().hasOnlyBrushes())
  {
    return false;
  }

  auto points = std::vector<vm::vec3d>{};

  if (map.selection().hasBrushFaces())
  {
    for (const auto& handle : map.selection().brushFaces)
    {
      for (const auto* vertex : handle.face().vertices())
      {
        points.push_back(vertex->position());
      }
    }
  }
  else if (map.selection().hasOnlyBrushes())
  {
    for (const auto* brushNode : map.selection().brushes)
    {
      for (const auto* vertex : brushNode->brush().vertices())
      {
        points.push_back(vertex->position());
      }
    }
  }

  auto polyhedron = Polyhedron3{std::move(points)};
  if (!polyhedron.polyhedron() || !polyhedron.closed())
  {
    return false;
  }

  const auto builder = BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
    map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};
  return builder.createBrush(polyhedron, map.currentMaterialName())
         | kdl::transform([&](auto b) {
             b.cloneFaceAttributesFrom(
               map.selection().brushes | std::views::transform([](const auto* brushNode) {
                 return &brushNode->brush();
               })
               | kdl::ranges::to<std::vector>());

             // The nodelist is either empty or contains only brushes.
             const auto toRemove = map.selection().nodes;

             // We could be merging brushes that have different parents; use the parent
             // of the first brush.
             auto& parentNode = !map.selection().brushes.empty()
                                  ? *map.selection().brushes.front()->parent()
                                : !map.selection().brushFaces.empty()
                                  ? *map.selection().brushFaces.front().node()->parent()
                                  : parentForNodes(map);

             auto* brushNode = new BrushNode{std::move(b)};

             auto transaction = Transaction{map, "CSG Convex Merge"};
             deselectAll(map);
             if (addNodes(map, {{&parentNode, {brushNode}}}).empty())
             {
               transaction.cancel();
               return;
             }
             removeNodes(map, toRemove);
             selectNodes(map, {brushNode});
             transaction.commit();
           })
         | kdl::if_error(
           [&](auto e) { map.logger().error() << "Could not create brush: " << e.msg; })
         | kdl::is_success();
}

bool csgSubtract(Map& map)
{
  const auto subtrahendNodes = std::vector<BrushNode*>{map.selection().brushes};
  if (subtrahendNodes.empty())
  {
    return false;
  }

  auto transaction = Transaction{map, "CSG Subtract"};
  // Select touching, but don't delete the subtrahends yet
  selectTouchingNodes(map, false);

  const auto minuendNodes = std::vector<BrushNode*>{map.selection().brushes};
  const auto subtrahends = subtrahendNodes
                           | std::views::transform([](const auto* subtrahendNode) {
                               return &subtrahendNode->brush();
                             })
                           | kdl::ranges::to<std::vector>();

  auto toAdd = kdl::flat_map<Node*, std::vector<Node*>>{};
  auto toRemove =
    std::vector<Node*>{std::begin(subtrahendNodes), std::end(subtrahendNodes)};

  return minuendNodes | std::views::transform([&](auto* minuendNode) {
           const auto& minuend = minuendNode->brush();
           auto currentSubtractionResults = minuend.subtract(
             map.worldNode().mapFormat(),
             map.worldBounds(),
             map.currentMaterialName(),
             subtrahends);

           return currentSubtractionResults
                  | std::views::filter([](const auto r) { return r | kdl::is_success(); })
                  | kdl::views::as_rvalue | kdl::fold
                  | kdl::transform([&](auto currentBrushes) {
                      if (!currentBrushes.empty())
                      {
                        auto resultNodes = currentBrushes | kdl::views::as_rvalue
                                           | std::views::transform([&](auto b) {
                                               return new BrushNode{std::move(b)};
                                             })
                                           | kdl::ranges::to<std::vector>();
                        auto& toAddForParent = toAdd[minuendNode->parent()];
                        kdl::vec_append(toAddForParent, std::move(resultNodes));
                      }

                      toRemove.push_back(minuendNode);
                    });
         })
         | kdl::fold | kdl::transform([&]() {
             deselectAll(map);
             const auto added = addNodes(map, toAdd);
             removeNodes(map, toRemove);
             selectNodes(map, added);

             return transaction.commit();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not subtract brushes: " << e;
             transaction.cancel();
             return false;
           })
         | kdl::value();
}

bool csgIntersect(Map& map)
{
  const auto brushes = map.selection().brushes;
  if (brushes.size() < 2u)
  {
    return false;
  }

  auto intersection = brushes.front()->brush();

  bool valid = true;
  for (auto it = std::next(std::begin(brushes)), end = std::end(brushes);
       it != end && valid;
       ++it)
  {
    auto* brushNode = *it;
    const auto& brush = brushNode->brush();
    valid = intersection.intersect(map.worldBounds(), brush) | kdl::if_error([&](auto e) {
              map.logger().error() << "Could not intersect brushes: " << e.msg;
            })
            | kdl::is_success();
  }

  const auto toRemove = std::vector<Node*>{std::begin(brushes), std::end(brushes)};

  auto transaction = Transaction{map, "CSG Intersect"};
  deselectNodes(map, toRemove);

  if (valid)
  {
    auto* intersectionNode = new BrushNode{std::move(intersection)};
    if (addNodes(map, {{&parentForNodes(map, toRemove), {intersectionNode}}}).empty())
    {
      transaction.cancel();
      return false;
    }
    removeNodes(map, toRemove);
    selectNodes(map, {intersectionNode});
  }
  else
  {
    removeNodes(map, toRemove);
  }

  return transaction.commit();
}

bool csgHollow(Map& map)
{
  const auto brushNodes = map.selection().brushes;
  if (brushNodes.empty())
  {
    return false;
  }

  bool didHollowAnything = false;
  auto toAdd = kdl::flat_map<Node*, std::vector<Node*>>{};
  auto toRemove = std::vector<Node*>{};

  for (auto* brushNode : brushNodes)
  {
    const auto& originalBrush = brushNode->brush();

    auto shrunkenBrush = originalBrush;
    shrunkenBrush.expand(map.worldBounds(), -double(map.grid().actualSize()), true)
      | kdl::and_then([&]() {
          didHollowAnything = true;

          return originalBrush.subtract(
                   map.worldNode().mapFormat(),
                   map.worldBounds(),
                   map.currentMaterialName(),
                   shrunkenBrush)
                 | kdl::fold | kdl::transform([&](auto fragments) {
                     auto fragmentNodes =
                       fragments | kdl::views::as_rvalue
                       | std::views::transform([](auto&& b) {
                           return new BrushNode{std::forward<decltype(b)>(b)};
                         })
                       | kdl::ranges::to<std::vector>();

                     auto& toAddForParent = toAdd[brushNode->parent()];
                     kdl::vec_append(toAddForParent, fragmentNodes);
                     toRemove.push_back(brushNode);
                   });
        })
      | kdl::transform_error(
        [&](const auto& e) { map.logger().error() << "Could not hollow brush: " << e; });
  }

  if (!didHollowAnything)
  {
    return false;
  }

  auto transaction = Transaction{map, "CSG Hollow"};
  deselectAll(map);
  const auto added = addNodes(map, toAdd);
  if (added.empty())
  {
    transaction.cancel();
    return false;
  }
  removeNodes(map, toRemove);
  selectNodes(map, added);

  return transaction.commit();
}

bool extrudeBrushes(
  Map& map, const std::vector<vm::polygon3d>& faces, const vm::vec3d& delta)
{
  const auto nodes = map.selection().nodes;
  return applyAndSwap(
    map,
    "Resize Brushes",
    nodes,
    collectContainingGroups(nodes),
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto faceIndex = brush.findFace(faces);
        if (!faceIndex)
        {
          // we allow resizing only some of the brushes
          return true;
        }

        return brush.moveBoundary(
                 map.worldBounds(),
                 *faceIndex,
                 delta,
                 map.editorContext().alignmentLock())
               | kdl::transform(
                 [&]() { return map.worldBounds().contains(brush.bounds()); })
               | kdl::transform_error([&](auto e) {
                   map.logger().error() << "Could not resize brush: " << e.msg;
                   return false;
                 })
               | kdl::value();
      },
      [](BezierPatch&) { return true; }));
}

} // namespace tb::mdl
