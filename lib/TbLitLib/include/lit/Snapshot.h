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

// C23: the plain data of the document that the GUI thread hands to the render thread
// (§3.3, S31, D39). The GUI thread builds a snapshot from its dirty set (LP1b.1) and
// offers it to the session (LitSession::offerSnapshot); after that it is immutable and
// shared by the render thread and the CPU pool's geometry jobs. It is Qt-free, and
// nothing in it points into the document: every record below is trivially copyable and
// refers to the snapshot's other arrays and to its string pool by index.

#include "vm/plane.h"
#include "vm/vec.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tb::lit
{

/**
 * The number the GUI thread gives a node when the preview first sees it (§3.3, S5,
 * R5.9). A node added again, also at an address a removed node had, gets a new one, and
 * one is never reused; chunks, light records and shadow slots are keyed on it.
 */
using PreviewId = std::uint64_t;
// no node
constexpr auto NoPreviewId = PreviewId{0};

// a run of elements of one of the snapshot's arrays
struct SnapshotRange
{
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

// a string of the snapshot's pool: strings[offset, offset + length), unterminated
struct SnapshotString
{
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

enum class SnapshotNodeKind : std::uint8_t
{
  Brush,
  Patch,
  PointEntity,
  Light,
};

// a changed or added node
struct SnapshotNode
{
  PreviewId id = NoPreviewId;
  // the entity the node belongs to: a brush's or patch's entity (worldspawn for world
  // geometry, also in a group or layer); a point entity's or light's own id
  PreviewId entity = NoPreviewId;
  SnapshotNodeKind kind = SnapshotNodeKind::Brush;
  // EditorContext::visible; the selection does not override it (D65)
  bool visible = true;
  // Brush: its faces in faces; Patch: its one element of patches; PointEntity and Light:
  // their one element of entities
  SnapshotRange elements;
  // D26, S4: the node changed since the outermost transaction began, and that transaction
  // is still open (a drag); the node is sent again with false once it has ended
  bool active = false;
};

/**
 * One face of a brush, in world space. The Valve 220 values are those the map serialiser
 * writes for the face (MapFileSerializer's writeValveMaterialInfo: BrushFace::uAxis,
 * vAxis and uvAttributes' offset and scale), round-tripped to float as the engine reads
 * them (P.19), so that the engine's float arithmetic of GetTextureVectors and
 * TriListForSide can be reproduced.
 */
struct SnapshotFace
{
  // BrushFace::polygon's vertices, in its order, in vertices
  SnapshotRange polygon;
  // the three points the serialiser writes for the face (BrushFace::points), and its
  // boundary plane
  std::array<vm::vec3d, 3> points = {};
  vm::plane3d plane;
  // BrushFace::materialName, without the textures/ prefix the engine adds when it reads
  // the map
  SnapshotString material;
  vm::vec3f uAxis = vm::vec3f{0, 0, 0};
  vm::vec3f vAxis = vm::vec3f{0, 0, 0};
  vm::vec2f offset = vm::vec2f{0, 0};
  vm::vec2f scale = vm::vec2f{1, 1};
};

// one Bezier patch, in world space
struct SnapshotPatch
{
  // BezierPatch::materialName, without the textures/ prefix
  SnapshotString material;
  // pointRowCount and pointColumnCount
  std::uint32_t rows = 0;
  std::uint32_t columns = 0;
  // rows × columns control points (x y z u v) in controlPoints, row by row
  SnapshotRange controlPoints;
  // patchDef3: explicit subdivisions (BezierPatch::explicitSubdivisions); patchDef2
  // otherwise
  bool explicitSubdivisions = false;
  std::uint32_t horizontalSubdivisions = 0;
  std::uint32_t verticalSubdivisions = 0;
};

// one key and value of an entity, as the map has them
struct SnapshotKeyValue
{
  SnapshotString key;
  SnapshotString value;
};

// an entity: worldspawn, a brush entity, a point entity or a light
struct SnapshotEntity
{
  PreviewId id = NoPreviewId;
  SnapshotString classname;
  // its keys in keyValues, in map order, a key given twice included (look keys up with
  // the engine's rule, lit::findKey: case-insensitive, the last one wins)
  SnapshotRange keys;
};

/**
 * One snapshot: the nodes that changed since the last one, the ids of the nodes removed
 * since then, and the entities they belong to. At most one is in flight between the GUI
 * thread and the render thread (S31).
 *
 * B0.4: an entity record may appear without any of its nodes: its keys changed and its
 * primitives did not (worldspawn's overrideMaterial or removeEntities, a func_static's
 * origin, rotation or model); it replaces that entity's state for all its primitives, so
 * the consumer rebuilds every chunk of that entity's nodes (all chunks, for worldspawn's
 * keys). removed holds the preview ids of removed entities as well as of removed nodes.
 * mapPath, when set, is the document's path after a save, where the .proc beside it is
 * looked for; a session's first snapshot is complete.
 */
struct Snapshot
{
  // the session generation it was made for; a session drops another one's (D53)
  std::uint64_t generation = 0;
  // the snapshots of a session count from 1
  std::uint64_t sequence = 0;

  std::vector<SnapshotNode> nodes;
  std::vector<PreviewId> removed;
  std::vector<SnapshotEntity> entities;

  std::vector<SnapshotFace> faces;
  std::vector<vm::vec3d> vertices;
  std::vector<SnapshotPatch> patches;
  std::vector<vm::vec<double, 5>> controlPoints;
  std::vector<SnapshotKeyValue> keyValues;
  std::string strings;

  // the document's path (B0.4), unset when it did not change
  std::optional<std::string> mapPath;

  std::string_view string(const SnapshotString& string) const
  {
    return std::string_view{strings}.substr(string.offset, string.length);
  }
};

// the records hold no pointer and copy as bytes
static_assert(std::is_trivially_copyable_v<SnapshotRange>);
static_assert(std::is_trivially_copyable_v<SnapshotString>);
static_assert(std::is_trivially_copyable_v<SnapshotNode>);
static_assert(std::is_trivially_copyable_v<SnapshotFace>);
static_assert(std::is_trivially_copyable_v<SnapshotPatch>);
static_assert(std::is_trivially_copyable_v<SnapshotKeyValue>);
static_assert(std::is_trivially_copyable_v<SnapshotEntity>);
static_assert(std::is_trivially_copyable_v<vm::vec3d>);
static_assert(std::is_trivially_copyable_v<vm::vec<double, 5>>);
static_assert(std::is_nothrow_move_constructible_v<Snapshot>);
static_assert(std::is_nothrow_move_assignable_v<Snapshot>);

} // namespace tb::lit
