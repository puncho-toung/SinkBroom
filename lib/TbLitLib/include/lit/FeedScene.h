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

#pragma once

// LPF.2 (D107, P5.11): the snapshot records a host outside SinkBroom feeds a session
// with (lib/TbLitLib/FEED.md), made as the editor's LitSnapshotBuilder (TbUiLib) makes
// them from a document (C23): record builders for a quad, a sphere, a point light and
// worldspawn, and a map text's snapshot. Pure functions of their arguments, Qt-free
// (D28); any thread, since a snapshot is plain data until it is offered.

#include "base/Result.h"
#include "lit/LitSession.h"
#include "lit/Snapshot.h"

#include "vm/vec.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

/**
 * LPF.2 (D107, P5.11, D53): the snapshot of a map file in TrenchBroom's Doom 3 Valve 220
 * text, as the editor's builder makes it from the document after loading that text:
 * every entity with its keys in map order (preview ids from 1 in file order, entities
 * first, then each entity's brushes and patches in file order); per brush its faces with
 * TrenchBroom's polygon (counter-clockwise seen from outside, the least vertex first, as
 * vm::polygon keeps it), the three points, the plane ((p3 - p1) x (p2 - p1) outward) and
 * the Valve values; per patch (patchDef2, patchDef3) its control points; a point entity
 * or a light (classname "light") as a node whose entity is itself. Every number is read
 * as the engine's idLexer reads it back (lexerFloat, R3.10), as the editor's builder
 * reads the serialiser's text; the first entity is worldspawn.
 *
 * The brush polygons are the one thing the editor computes otherwise (its brush geometry,
 * from the same planes): here each vertex is the intersection of three of the brush's
 * planes, solved in double precision by Cramer's rule, kept when no plane of the brush
 * has it more than 1e-6 units outside and merged with another within 1e-6 units. That is
 * exact for an axial brush whose coordinates are integers (or any value a float holds
 * exactly); for a non-axial brush each vertex carries the rounding error of that solve,
 * a relative error near the double epsilon (about 1e-12 units at 4096 units from the
 * origin), which is below what the editor's own vertices keep and far below the float
 * the engine reads them into. A face that no vertex reaches gets an empty polygon (the
 * editor would have dropped the face). Generation and sequence are the caller's (D53: the
 * session drops another generation's snapshot; sequences rise from 1); mapPath is set
 * only when given (without it the session reads no bake: K35).
 *
 * Every scan is bounded: an unterminated quoted string, an unclosed entity, brush or
 * patch block, a number lexerFloat refuses or a missing token is an error naming the
 * line, never a loop (P5.11). Pure; any thread.
 */
Result<Snapshot> snapshotOfMapText(
  const std::string& text,
  std::uint64_t generation,
  std::uint64_t sequence,
  std::optional<std::string> mapPath = std::nullopt);

namespace feed
{

/**
 * The entity record of worldspawn (classname first, then keys in order), which every
 * snapshot with world geometry needs: brushes and patches whose entity is entityId are
 * world geometry (the chunk key's entity). Appends to snapshot.
 */
void worldspawn(
  Snapshot& snapshot,
  PreviewId entityId,
  const std::vector<std::pair<std::string, std::string>>& keys = {});

/**
 * A six-sided axial brush (node nodeId of entity entityId) whose face toward normal (an
 * axis, either sign) is the square of side 2 · halfSize centred on centre and carries
 * material; its other five faces, thickness units behind, carry sideMaterial. Materials
 * are named as the map writes them (without textures/). Every face has TrenchBroom's
 * Valve axes for its normal as the editor writes them for a new face (x: [0 1 0] [0 0
 * -1], y: [1 0 0] [0 0 -1], z: [1 0 0] [0 -1 0]), offset and textureScale (texels per
 * unit: the Valve scale); the polygon, points and plane as snapshotOfMapText makes them,
 * every value through lexerFloat. An error, and nothing appended, for a normal that is
 * not an axis or a size that is not positive.
 */
Result<void> quadBrush(
  Snapshot& snapshot,
  PreviewId entityId,
  PreviewId nodeId,
  const vm::vec3d& centre,
  const vm::vec3d& normal,
  double halfSize,
  const std::string& material,
  const std::string& sideMaterial,
  double textureScale = 1.0,
  const vm::vec2f& offset = vm::vec2f{0, 0},
  double thickness = 16.0);

/**
 * A sphere as one patchDef3 (node nodeId of entity entityId): 9 rows by 17 columns of
 * control points, quadratic Bezier segments of 45 degrees (4 from pole to pole, 8 around:
 * each segment's middle point on the tangents' intersection, r / cos 22.5 degrees from
 * the axis), with explicit subdivisions (8 × 8 by default: 33 × 65 = 2,145 vertices once
 * subdivided, K19's bounds 99 control points a side, 1 to 64 subdivisions, 65,535
 * vertices). A non-rational quadratic Bezier only approximates a circle (at most about
 * 0.6 per cent outside the radius, between the control points). The pole rows pinch to
 * one point, so their triangles are degenerate and the chunk builder drops them as dmap
 * does (usurface.cpp:657-661). st: s = column / 16, t = row / 8. Triangles face outward.
 * An error, and nothing appended, for a radius that is not positive or subdivisions
 * outside K19's bounds.
 */
Result<void> patchDef3Sphere(
  Snapshot& snapshot,
  PreviewId entityId,
  PreviewId nodeId,
  const vm::vec3d& centre,
  double radius,
  const std::string& material,
  std::uint32_t horizontalSubdivisions = 8,
  std::uint32_t verticalSubdivisions = 8);

/**
 * A point light as the editor's builder writes one (addPointEntity): the entity record
 * (classname "light", then origin, light_radius "r r r", _color, and texture when not
 * empty, the numbers as fmt's shortest text) and a Light node whose id and entity are
 * id, as a point entity's are. Without texture the light gets the light entityDef's
 * shader, else lights/defaultPointLight (the game's, C22). Appends to snapshot.
 */
void pointLight(
  Snapshot& snapshot,
  PreviewId id,
  const vm::vec3d& origin,
  double radius,
  const vm::vec3f& colour,
  const std::string& texture = {});

/**
 * A camera at eye looking at target, z up (or x up when it looks straight up or down),
 * with TrenchBroom's horizontal field of view of a 4:3 frame (CameraState); the viewport
 * is the session's target extent.
 */
CameraState cameraLookingAt(
  const vm::vec3f& eye, const vm::vec3f& target, float fovDegrees = 90.0f);

} // namespace feed
} // namespace tb::lit
