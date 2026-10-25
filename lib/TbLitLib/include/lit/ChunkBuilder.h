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

#include "base/Result.h"
#include "lit/EngineGeometry.h"
#include "lit/EngineLight.h"
#include "lit/EntityState.h"
#include "lit/MaterialDecls.h"
#include "lit/ProcIslands.h"
#include "lit/Snapshot.h"

#include "vm/vec.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class CpuJobContext;
struct MaterialTable;
struct ProcTree;

// a chunk's 1024-unit cell (D59): floor(world / 1024) per axis
using CellCoord = std::array<int, 3>;
constexpr auto CellSize = 1024.0;
// the triangles of a chunk are ordered by 256-unit sub-cell, in at most 32 ranges (R-C4)
constexpr auto SubCellSize = 256.0f;
constexpr auto MaxChunkRanges = std::size_t{32};
// a chunk's vertices before tangents and back sides; with both it stays below 65535
constexpr auto MaxChunkWeldedVerts = std::size_t{16000};

// ----- the scene the render thread keeps from the snapshots (C23), immutable records
// -----

// a brush face, out of the snapshot (world space, TrenchBroom's order)
struct SceneFace
{
  std::vector<vm::vec3d> polygon;
  std::array<vm::vec3d, 3> points = {};
  // TrenchBroom's name, without textures/
  std::string material;
  EngineValveTexture texture;
};

struct ScenePatch
{
  // TrenchBroom's name, without the textures/ prefix the engine adds when missing
  std::string material;
  std::uint32_t rows = 0;
  std::uint32_t columns = 0;
  // rows × columns (x y z u v), row by row
  std::vector<vm::vec<double, 5>> controlPoints;
  bool explicitSubdivisions = false;
  std::uint32_t horizontalSubdivisions = 0;
  std::uint32_t verticalSubdivisions = 0;
};

struct SceneNode
{
  PreviewId id = NoPreviewId;
  PreviewId entity = NoPreviewId;
  SnapshotNodeKind kind = SnapshotNodeKind::Brush;
  bool visible = true;
  std::vector<SceneFace> faces;
  std::optional<ScenePatch> patch;
};

struct SceneEntity
{
  PreviewId id = NoPreviewId;
  std::string classname;
  // the map keys in map order, duplicates kept
  EntityKeys keys;
};

// the scene records a snapshot holds (C23, B0.4), out of its arrays
std::vector<std::shared_ptr<const SceneNode>> sceneNodesOf(const Snapshot& snapshot);
std::vector<std::shared_ptr<const SceneEntity>> sceneEntitiesOf(const Snapshot& snapshot);

// the cell of a node's primitives (the centroid of its faces' or control points' world
// positions); a node is chunked in one cell
CellCoord cellOfNode(const SceneNode& node);

// ----- chunks (D59, C4, S36, R5.28) -----

enum class CastClass : std::uint8_t
{
  None,
  Opaque,
  Perforated,
};

// (area, entity, material, cell) and a part number for a key whose triangles do not fit
// in one chunk; the entity's state is the chunk's, so a changed state rebuilds the
// entity's chunks
struct ChunkKey
{
  // a world piece's area (clipSideIntoAreas: -1 for a piece in solid, D47, K14); for a
  // brush entity's chunk the area of its pivot (pointInArea( modelOrigin ),
  // ChunkBuilder.cpp's makeContext) as a key only (P4.9): visibility, pairing and the
  // grid area read the entity's references (ChunkEntityState::areas, C31), never this
  int area = 0;
  PreviewId entity = NoPreviewId;
  // the engine's material name ("textures/...")
  std::string material;
  CellCoord cell = {0, 0, 0};
  int part = 0;

  bool operator==(const ChunkKey& other) const;
  bool operator<(const ChunkKey& other) const;
};

// a run of a chunk's indexes in one sub-cell, and its bounds
struct ChunkRange
{
  std::uint32_t firstIndex = 0;
  std::uint32_t indexCount = 0;
  std::array<EngineVec3, 2> bounds = {};
};

/**
 * One chunk: entity-local idDrawVerts (world − (origin + originOffset), as dmap writes
 * them), 16-bit indexes, at most 65535 vertices, the sub-cell ranges, the bounds, and its
 * cast class and two versions (S36, R5.28): the caster version covers the positions, the
 * cast class, the noshadows flags and, for a perforated caster, the material and the st;
 * the appearance version everything else. Provisional: built with a C21 Default
 * editor-image size, and rebuilt when the size is Ready.
 */
struct Chunk
{
  ChunkKey key;
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
  std::vector<ChunkRange> ranges;
  std::array<EngineVec3, 2> bounds = {};
  CastClass castClass = CastClass::Opaque;
  bool provisional = false;
  std::uint64_t casterVersion = 0;
  std::uint64_t appearanceVersion = 0;
};

/**
 * What the renderer needs of an entity whose chunks exist (S34, A7): the engine's model
 * matrix (origin + __brushOrigin, axis = scale·rotation) and the render entity's parms.
 */
struct ChunkEntityState
{
  PreviewId id = NoPreviewId;
  RenderEntityParms parms;
  // the pivot the vertices are relative to: origin + originOffset (__brushOrigin)
  EngineVec3 modelOrigin = {0, 0, 0};
  // the mean centre of the CONTENTS_ORIGIN brushes, relative to the origin
  EngineVec3 originOffset = {0, 0, 0};
  // K17: a brush entity draws only through its model key, which must name its own model
  // (dmap names it by name); dmapOnly entities and removeEntities drop the primitives
  bool visible = true;
  std::uint64_t hash = 0;

  // ----- LP4.0 (C31, D80): the entity's reference set, which SceneGeometry::apply builds
  // on the render thread from every chunk of the entity (across cells and parts) once a
  // batch touched it; a chunk job leaves these empty -----

  // per engine material name, the union of the entity's chunks of that material in model
  // space, grown to FinishSurfaces' cube where the material has a deform (Model.cpp
  // :1346-1360): the engine's per-model-surface tri->bounds, which the grid-area rule
  // reads (tr_frontend_addmodels.cpp:817-825, C36, P4.11)
  std::map<std::string, EngineBounds> materialBounds;
  // the model's bounds (FinishSurfaces: the union of materialBounds); cleared without a
  // chunk, where the engine's Zero() bounds of an empty model would reference the pivot's
  // area: nothing is drawn either way
  EngineBounds localReferenceBounds = {};
  // R_DeriveEntityData (RenderWorld_defs.cpp:49-60): OffsetScaleForBounds(
  // modelRenderMatrix, localReferenceBounds ) with modelRenderMatrix =
  // CreateFromOriginAxis( modelOrigin, parms.axis ), litSpaceOf's; zero for cleared
  // bounds (R_CreateEntityRefs returns before deriving them)
  EngineMat4 inverseBaseModelProject = {};
  // ProjectedBounds( inverseBaseModelProject, bounds_unitCube, false ); cleared for
  // cleared bounds
  EngineBounds globalReferenceBounds = {};
  /**
   * R_CreateEntityRefs (RenderWorld_defs.cpp:156-197): the areas pushFrustumIntoTree(
   * tree, commonAreas, inverseBaseModelProject, bounds_unitCube ) reaches, in the
   * engine's chain order: AddEntityRefToArea prepends each new reference to the entity's
   * chain (RenderWorld.cpp:1836-1837), so the reverse of the push order (a light's
   * derived.areas keeps the push order). Empty without a tree, and for cleared bounds
   * (no references: the outside set, D81)
   */
  std::vector<int> areas;
};

// the brush primitives' cells of one entity to rebuild
struct CellRef
{
  PreviewId entity = NoPreviewId;
  CellCoord cell = {0, 0, 0};

  bool operator==(const CellRef& other) const;
  bool operator<(const CellRef& other) const;
};

/**
 * One job's input (D59, held by shared_ptr, immutable): the cells to rebuild, every node
 * of those cells, every node of each brush entity among them (for its origin brushes),
 * the entities, worldspawn (whose overrideMaterial and removeEntities apply to all, A11),
 * the area tree (null without a .proc) and the material table (null without a service).
 */
struct ChunkJobInput
{
  std::vector<CellRef> cells;
  std::vector<std::shared_ptr<const SceneNode>> nodes;
  std::unordered_map<PreviewId, std::shared_ptr<const SceneEntity>> entities;
  std::shared_ptr<const SceneEntity> worldspawn;
  // the entityDefs of the entities' classnames, by classname as the entities spell it
  std::unordered_map<std::string, std::shared_ptr<const EntityDefEntry>> entityDefs;
  std::shared_ptr<const ProcTree> proc;
  std::shared_ptr<const MaterialTable> materials;
  // LP7.4 (C55): the job returns its face pieces (ChunkJobOutput::pieces) for the
  // adoption pass
  bool collectPieces = false;
};

// LP7.4 (C55, P7.11): a face piece a job built, with the cell (and the chunk entity) it
// is chunked in, for the whole-entity adoption pass after apply (SceneGeometry)
struct ChunkPiece
{
  CellRef cell;
  AdoptionPiece piece;
};

struct ChunkJobOutput
{
  // the cells the job rebuilt: their old chunks are replaced by chunks
  std::vector<CellRef> cells;
  std::vector<Chunk> chunks;
  std::vector<ChunkEntityState> entities;
  // pieces of world faces in solid (K14)
  std::size_t solidPieces = 0;
  // materials the table did not have yet; the chunks using them are provisional
  std::vector<std::string> pendingMaterials;
  // LPF.0b (C43, P5.9): every material the job read from the table, drawn or not: its
  // faces' and patches' (worldspawn's overrideMaterial in their place) and those its
  // brush entities' origin test read, so that a re-parse builds their cells again
  std::vector<std::string> materials;
  std::vector<std::string> warnings;
  // LP7.4 (C55): with ChunkJobInput::collectPieces, every brush face piece that makes
  // triangles (world and brush entity faces; patches never), in the nodes' order
  std::vector<ChunkPiece> pieces;
};

/**
 * Builds the chunks of the input's cells (D59, §3.4): per face, the engine's material
 * name (or worldspawn's overrideMaterial), dmap's visibility rule (neither drawn nor
 * casting shadows: skipped), the polygon in the engine's winding order, split into areas
 * (world primitives, AreaTree; a brush entity's chunks take the area of its pivot), st by
 * dmap's arithmetic from the origin-relative position and the editor image's size through
 * SetTexCoord, the side plane's normal through SetNormal, TriListForSide's fan; patches
 * through the copied subdivision; then per chunk: welded vertices, st recentred per
 * island and truncated again, R_CleanupTriangles' steps with the material's tangent path
 * (C25: unsmoothedTangents; mikktspace the engine's failure path, whose copy warns,
 * M-ID-DIFF-04), a cleaned reversed copy for createBackSides, the triangles ordered by
 * sub-cell, the bounds, the cast class and the versions.
 *
 * Pool-safe (C24): it calls only the geometry facade and plain code; any thread. The
 * copied code's messages go to the calling thread's sink, which the pool drains. With a
 * job context it stops at the first node after the pool was cancelled (R2.1), and what it
 * returns then is incomplete.
 */
ChunkJobOutput buildChunks(
  const ChunkJobInput& input, const CpuJobContext* context = nullptr);

// the tangent path of a material (C25, M-ID-DIFF-04)
EngineTangentPath tangentPath(const MaterialEntry* material);

// the engine's name of a brush side's and a patch's material (MapFile.cpp:788-796,
// :296-304)
std::string engineSideMaterial(const std::string& tbName);
std::string enginePatchMaterial(const std::string& tbName);

} // namespace tb::lit
