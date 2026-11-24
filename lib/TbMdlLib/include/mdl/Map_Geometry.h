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

#pragma once

#include "kd/reflection_decl.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/polygon.h"
#include "vm/segment.h"
#include "vm/util.h"
#include "vm/vec.h"

#include <string>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Map;

bool transformSelection(
  Map& map, const std::string& commandName, const vm::mat4x4d& transformation);

bool translateSelection(Map& map, const vm::vec3d& delta);
bool rotateSelection(
  Map& map, const vm::vec3d& center, const vm::vec3d& axis, double angle);
bool scaleSelection(Map& map, const vm::bbox3d& oldBBox, const vm::bbox3d& newBBox);
bool scaleSelection(Map& map, const vm::vec3d& center, const vm::vec3d& scaleFactors);
bool shearSelection(
  Map& map, const vm::bbox3d& box, const vm::vec3d& sideToShear, const vm::vec3d& delta);
bool flipSelection(Map& map, const vm::vec3d& center, vm::axis::type axis);

struct TransformVerticesResult
{
  bool success;
  bool hasRemainingVertices;

  kdl_reflect_decl(TransformVerticesResult, success, hasRemainingVertices);
};

TransformVerticesResult transformVertices(
  Map& map, const std::vector<vm::vec3d>& vertexPositions, const vm::mat4x4d& transform);
bool transformEdges(
  Map& map,
  const std::vector<vm::segment3d>& edgePositions,
  const vm::mat4x4d& transform);
bool transformFaces(
  Map& map,
  const std::vector<vm::polygon3d>& facePositions,
  const vm::mat4x4d& transform);

bool addVertex(Map& map, const vm::vec3d& vertexPosition);
bool removeVertices(
  Map& map, const std::string& commandName, std::vector<vm::vec3d> vertexPositions);

bool snapVertices(Map& map, double snapTo);

struct SafeSnapReport
{
  // The selected brushes looked at, those already on the grid, and those snapped.
  size_t brushCount = 0;
  size_t unchangedCount = 0;
  size_t snappedCount = 0;

  // Why brushes were refused: a snap that merges vertices, changes the faces, flattens the
  // brush or leaves the range the check handles; a brush inside a group with linked copies
  // (the snap would move the copies unchecked); or a stock snap that did not give the faces
  // the check expects.
  size_t mergeCount = 0;
  size_t faceSetCount = 0;
  size_t flatCount = 0;
  size_t rangeCount = 0;
  size_t linkedCount = 0;
  size_t stockMismatchCount = 0;
  std::vector<BrushNode*> refusedBrushes;

  // Contacts with other brushes that the snap would open, and the selected brushes that
  // hold them: faces that touch with a positive area, and brushes that overlap (a seal).
  size_t openedContactCount = 0;
  std::vector<BrushNode*> openedContactBrushes;

  // Set when the command could not run at all (no brushes, a grid below one unit).
  std::string error;
  // True when the snapped brushes were stored as one undoable command.
  bool committed = false;

  bool refused() const;
};

/**
 * Rounds the vertices of all selected brushes to multiples of `snapTo`, a whole number of
 * units of 1 or more, but only if every brush keeps its faces, every face contact with
 * another brush keeps a positive area, and every two brushes that overlap still overlap or
 * touch with a positive area. Otherwise nothing changes, and the report names the offending
 * brushes. Unlike snapVertices, a brush is never rebuilt with other faces, and a brush in a
 * group with linked copies is refused.
 */
SafeSnapReport safeSnapVertices(Map& map, double snapTo);

struct ChunkToGridReport
{
  // The selected brushes looked at, those replaced by pieces, and the pieces made.
  size_t brushCount = 0;
  size_t chunkedCount = 0;
  size_t pieceCount = 0;
  size_t cutCount = 0;

  // Cuts left out: a vertex lies within half a unit of the plane, a piece would be under
  // one cubic unit, or the clip gave no valid brush.
  size_t skippedNearCount = 0;
  size_t skippedVolumeCount = 0;
  size_t skippedFailedCount = 0;

  // Brushes left whole because a face's material is translucent, perforated or missing: the
  // compiler would draw the faces between their pieces.
  size_t notOpaqueCount = 0;

  // True when the pieces replaced their brushes as one undoable command.
  bool committed = false;
};

/**
 * Cuts each selected brush at the planes x = k * gridSize and y = k * gridSize (and
 * z = k * gridSize if chunkZ is set) that it strictly crosses, and replaces it by the pieces
 * in its own parent. Every original face plane is kept; each cut adds one axial plane whose
 * face takes the material of the brush face closest to it in direction. A brush with a face
 * whose material is translucent, perforated or missing is left whole.
 */
ChunkToGridReport chunkToGrid(Map& map, int gridSize = 512, bool chunkZ = false);

bool csgConvexMerge(Map& map);
bool csgSubtract(Map& map);
bool csgIntersect(Map& map);
bool csgHollow(Map& map);

bool extrudeBrushes(
  Map& map, const std::vector<vm::polygon3d>& faces, const vm::vec3d& delta);

} // namespace tb::mdl
