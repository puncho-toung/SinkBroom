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

#include "base/Macros.h"
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/GridTable.h"
#include "lit/LightRecord.h"
#include "lit/ProbeRecord.h"
#include "lit/Snapshot.h"

#include <compare>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class MaterialService;
struct AreaData;
struct MapBake;
struct MapBakeSlot;
struct MaterialTable;
struct PortalFogTable;
struct ProcTree;

/**
 * The render thread's scene and chunks (§3.3, §3.4, S31, B0.3): it merges each snapshot
 * into the scene it keeps (entity records, nodes, the cells their primitives are chunked
 * in), works out the cells that changed, and builds them as jobs on the session's CPU
 * pool (D59), inputs by shared_ptr. One batch of jobs is in flight at a time: the render
 * thread takes the next snapshot only when the last one's chunks are applied
 * (readyForSnapshot), so results apply in the snapshots' sequence order. A brush entity's
 * job rebuilds every cell of the entity, because its state (the origin brushes' pivot
 * among it) applies to all its chunks.
 *
 * Every batch first adopts the latest material table, and each table it adopts is
 * scanned for what the scene waits for (R3.1, R3.2): a material that was pending when a
 * cell was built, an editor-image size that was Default (C21) and is Ready now, an
 * entityDef that was pending when an entity was built (C22), the bake of the map path,
 * and (LPF.0b, C43) a re-parsed material's revision: its cells, drawn or not, and the
 * whole entity when its CONTENTS_ORIGIN changed. The cells concerned are built again in
 * that batch. Between snapshots, collect
 * starts such a batch when a newer table has arrived; the owner wakes the render thread
 * for it (LitSession::tableChanged, R2.2).
 *
 * The bake (C10, LP1c.1): a new map path asks the material service for its bake
 * (loadMapBake) and adopts only a slot that answers that request or a later one, never
 * one read before it (a Save As back to a path read earlier, with dmap run in between);
 * a later slot of the path with another tree (the service read a changed bake again)
 * replaces it and every cell is built again. The lights take the tree with the snapshot
 * when it arrives in a snapshot's pass, so a changed light is built once (W.2).
 *
 * Without a pool (or once it is stopped) the batches run on the calling thread. Without a
 * material service every material takes the defaults, provisionally, and there is no
 * .proc: every piece is in area 0.
 *
 * The entities' references (LP4.0, C31, D80): once every job of a batch has applied,
 * each entity whose chunks or state the batch touched gets, from all its chunks across
 * cells and parts, its per-material and model bounds, R_DeriveEntityData's
 * inverseBaseModelProject and globalReferenceBounds and R_CreateEntityRefs' areas over
 * the adopted tree (ChunkEntityState), on the render thread through the copied pure
 * functions (mirror.json M-REF-01, M-FLOW-PURE-01); a new tree rebuilds every cell, so
 * every entity's references with it. Immutable until the next batch applies.
 *
 * The .proc's triangles (LP7.4, C55, D133, P7.11, K44): with a .proc's tree, every job
 * returns its face pieces; once a batch has applied, a whole-entity pass beside the
 * references evaluates, per (entity, model, material) group whose pieces changed (every
 * group when a tree arrives), the islands of the model's surfaces of the material against
 * the entity's pieces (matchProcIsland), synchronously on the render thread (the pieces
 * are here, and the test is a few comparisons per island), and keeps an adoption table
 * per entity. Where it changed, the island's cell (its bounds' centre's) and its
 * candidates' cells are marked, and the batch the pass starts builds them with the table
 * as an input: so the tree's arrival, which marks every cell, makes two batches, the
 * second drawing the adopted islands. Before a snapshot's batch, an island one of whose
 * candidate faces changed (or whose entity's keys did, or whose material was parsed or
 * measured again) leaves the table with its cells marked (K44), so no batch draws it over
 * an edited face; the pass after that batch tries it again.
 *
 * The scene's lights are the LightRegistry's (LP1b.3, C15): it takes the same snapshots
 * and .proc tree, builds the light records as its own pool job, and publishes them in a
 * LightTable, into the session's LightTablePublication (R2.9: LitSession::lightTable is
 * what other threads read). A snapshot is taken only when neither the chunks nor the
 * lights have a job in flight.
 *
 * The scene's probes are the ProbeRegistry's (LP4.2a, C37, D87): whenever a snapshot is
 * taken, a table adopted or a bake adopted, the scene hands it the bake's tree, AreaData,
 * .shprobes and base name, the env_probe entityDef's state and the placed probes (every
 * entity but worldspawn whose merged spawn args name spawnclass EnvironmentProbe, its
 * classname's entityDef asked for when the table has none: the probes are provisional
 * meanwhile), and the registry builds every probe again as a pool job when they differ
 * from the last build's, publishing into the session's ProbeTablePublication
 * (LitSession::probeTable).
 *
 * Threads: the render thread only; the jobs on the pool
 * see only their inputs, and return early once the pool is cancelled (R2.1).
 */
class SceneGeometry
{
private:
  CpuPool* m_pool;
  MaterialService* m_materials;

  // the scene, from the snapshots (C23)
  std::unordered_map<PreviewId, std::shared_ptr<const SceneEntity>> m_entities;
  std::unordered_map<PreviewId, std::shared_ptr<const SceneNode>> m_nodes;
  PreviewId m_worldspawn = NoPreviewId;
  // the cell each primitive node is chunked in, and the nodes of each cell
  std::unordered_map<PreviewId, CellRef> m_nodeCells;
  std::map<CellRef, std::set<PreviewId>> m_cellNodes;
  // R2.4: the primitive nodes of each entity (the node's own entity, a func_group's too)
  std::unordered_map<PreviewId, std::set<PreviewId>> m_entityNodes;
  std::optional<std::string> m_mapPath;
  // the bake slot adopted, and the ticket of the map path's request (C10)
  std::shared_ptr<const MapBakeSlot> m_bakeSlot;
  std::uint64_t m_bakeTicket = 0;
  std::shared_ptr<const ProcTree> m_proc;
  // C31 (LP4.0): the bake's AreaData, adopted with the tree (null without one)
  std::shared_ptr<const AreaData> m_areas;
  // the table the last batch was built with; every table adopted is scanned once
  std::shared_ptr<const MaterialTable> m_table;
  // LP4.3 (C36, D88): where the grid table of the adopted bake is published
  std::shared_ptr<GridTablePublication> m_grids;

  // the cells of each material and the editor-image size they were built with (LP1c fix
  // S2.5: a cell is built again when the table's size of its material changes, Default to
  // Ready, Ready to another Ready, or back to Default); LP5.F (D77, A41): measured is
  // false while that size is a Default (Chunk::provisional) whose slot is not measured
  // yet, or (D111) one whose slot is published again unmeasured (remeasured), until a
  // scanned table's slot is measured at the same size
  struct SizedCells
  {
    EditorImageSize size;
    bool measured = true;
    std::set<CellRef> cells;
  };

  // the chunks, and what their cells wait for (R3.1, R3.2): the cells built while a
  // material was not in the table, the cells of each material with the editor-image size
  // they were built with, and the entities built while their classname's entityDef was
  // not in the table
  std::map<ChunkKey, Chunk> m_chunks;
  std::unordered_map<PreviewId, ChunkEntityState> m_entityStates;
  std::map<std::string, std::set<CellRef>> m_pendingMaterials;
  std::map<std::string, SizedCells> m_sizedCells;
  // LP5.F: a sized cell's measured is false (updated with m_sizedCells)
  bool m_waitsForMeasure = false;
  std::map<std::string, std::set<PreviewId>> m_pendingEntityDefs;
  // LP7.2 (C53, A41): the brush entities built while their skin was not in the table
  // (their chunks provisional), by the skin's name
  std::map<std::string, std::set<PreviewId>> m_pendingSkins;
  std::map<CellRef, std::size_t> m_solidPieces;
  /**
   * LPF.0b (C43, D106, P5.9): the cells of each material their chunk jobs read (drawn or
   * not, so that a stage-less material given its first stage builds its cells), with the
   * material's revision stamp (MaterialTable::revisionStamp) and its CONTENTS_ORIGIN when
   * they were built, and each cell's materials; scanned when a table's
   * materialsRevision moves (the last scanned one)
   */
  struct MaterialCells
  {
    std::uint64_t stamp = 0;
    bool origin = false;
    std::set<CellRef> cells;
  };
  std::map<std::string, MaterialCells> m_materialCells;
  std::map<CellRef, std::vector<std::string>> m_cellMaterials;
  std::uint64_t m_materialsRevision = 0;
  // LP5.F (D111): the table's sizesRevision the scene last scanned
  std::uint64_t m_sizesRevision = 0;
  // the cells of the last batch started (tests)
  std::set<CellRef> m_lastBatchCells;
  // D80: the entities whose chunks the applied jobs touched, whose references are built
  // once the batch has applied
  std::set<PreviewId> m_touchedEntities;

  // the batch in flight
  std::set<CellRef> m_dirty;
  std::vector<std::future<CpuJobResult<ChunkJobOutput>>> m_jobs;
  std::uint64_t m_batchSequence = 0;
  std::uint64_t m_appliedSequence = 0;
  std::vector<std::string> m_warnings;

  // LP4.2a (D87): the probe inputs may have changed since they were last handed over
  bool m_probesDirty = true;
  // the probe records
  ProbeRegistry m_probes;
  // the light records (declared last: its job is waited for first)
  LightRegistry m_lights;
  // LP5.3 (C49, D123): the portal -> fog light table of the last light table and
  // AreaData asked for (portalFogTable)
  mutable std::shared_ptr<const PortalFogTable> m_portalFog;
  mutable std::shared_ptr<const AreaData> m_portalFogAreas;

public:
  // LP7.4 (C55): the scene's current adoption totals (the frame log's adopted_islands,
  // adopted_tris, unmatched_islands, curved_islands)
  struct AdoptionCounts
  {
    std::uint64_t adoptedIslands = 0;
    std::uint64_t adoptedTris = 0;
    std::uint64_t unmatchedIslands = 0;
    std::uint64_t curvedIslands = 0;

    bool operator==(const AdoptionCounts&) const = default;
  };

private:
  // ----- LP7.4 (C55, D133, P7.11, K44): the adoption of the .proc's triangles -----

  // a model, a material and an entity: the islands of the model's surfaces of the
  // material against the entity's face pieces of the model (area: a world piece's;
  // -1: a brush entity's own model), the material in lower case
  struct AdoptionGroup
  {
    PreviewId entity = NoPreviewId;
    int area = 0;
    std::string material;

    auto operator<=>(const AdoptionGroup&) const = default;
  };
  // the adopted tree's islands (MapBake::islands; null without a tree, none for a
  // .bproc: D138) and its models by area and by lower-case name
  std::shared_ptr<const ProcIslands> m_islands;
  std::map<int, std::size_t> m_areaModels;
  std::map<std::string, std::size_t> m_namedModels;
  // the face pieces of each cell's last build, and the cells that hold each group's
  std::map<CellRef, std::vector<AdoptionPiece>> m_cellPieces;
  std::map<AdoptionGroup, std::set<CellRef>> m_groupCells;
  // the groups whose pieces the applied jobs changed; all of them once a tree arrives
  std::set<AdoptionGroup> m_dirtyGroups;
  bool m_adoptionAll = false;
  // the entities' adoption tables (the jobs' input), immutable once published
  std::unordered_map<PreviewId, std::shared_ptr<const EntityAdoption>> m_adoptions;
  AdoptionCounts m_adoptionCounts;

public:
  // lights, probes and grids may be null: the registries (and the scene's grid tables)
  // then publish into publications of their own
  SceneGeometry(
    CpuPool* pool,
    MaterialService* materials,
    std::shared_ptr<LightTablePublication> lights = nullptr,
    std::shared_ptr<ProbeTablePublication> probes = nullptr,
    std::shared_ptr<GridTablePublication> grids = nullptr);
  ~SceneGeometry();

  // B0.3: no batch is in flight
  bool readyForSnapshot() const;
  // a batch or the light job is in flight (DX.4: FrameConvergence::geometryInFlight)
  bool busy() const;
  // LP4.2b step 0: the probe build runs, or its inputs wait to be handed over or built
  bool probesBuilding() const;
  /**
   * LP4.2b step 0 (FrameConvergence::probesInFlight): probesBuilding(), or the published
   * probe table is provisional (an entityDef the probes depend on was pending) while a
   * material service can still answer it; a frame drawn meanwhile may show the probes'
   * defaults. The build's collection is the trigger (geometryTick), as the light job's.
   */
  bool probesInFlight() const;

  // merges the snapshot (B0.4's rules) and starts its batch; only when readyForSnapshot
  void takeSnapshot(const Snapshot& snapshot);

  /**
   * Collects the finished jobs; once every job of the batch is done its results replace
   * the chunks of its cells, and true is returned; so too when the light job is done and
   * its records are published. LP4.2a: a finished probe build is published too, without
   * counting as a batch (probesInFlight() says it ran). When nothing is in flight it
   * starts a batch for what a new material table or .proc made stale.
   */
  bool collect();

  const std::map<ChunkKey, Chunk>& chunks() const;
  const std::unordered_map<PreviewId, ChunkEntityState>& entityStates() const;
  // K14: the pieces of world faces in solid
  std::size_t solidPieces() const;
  std::uint64_t appliedSequence() const;
  // LPF.1 (C44): the material table's materialsRevision the scene last scanned (its
  // cells of a re-parsed material are rebuilt by the batch it started then, C43)
  std::uint64_t scannedMaterialsRevision() const;
  // LP5.F (D111): the material table's sizesRevision the scene last scanned
  std::uint64_t scannedSizesRevision() const;
  std::shared_ptr<const ProcTree> procTree() const;
  // C31 (LP4.0): the AreaData of procTree()'s bake (MapBake::areas), null without a tree;
  // immutable, so any thread may read what it points to
  std::shared_ptr<const AreaData> areaData() const;
  // C15: the latest published light records (never null); the render thread, which owns
  // the scene (other threads read LitSession::lightTable, R2.9)
  std::shared_ptr<const LightTable> lightTable() const;
  /**
   * LP5.3 (C49, D123, P6.8): the portal -> fog light table (buildPortalFogTable,
   * src/ViewFlow.h) of lightTable() over areaData(), built again when either changed
   * since the last call (a fog light, its shader's fogLight or MF_NOPORTALFOG through a
   * re-parse that builds its record again, a new tree); null without a tree. Immutable
   * once made; the render thread
   */
  std::shared_ptr<const PortalFogTable> portalFogTable() const;
  // LP4.2a (C37, D87): the latest published probe records (never null); the render
  // thread (other threads read LitSession::probeTable)
  std::shared_ptr<const ProbeTable> probeTable() const;
  /**
   * LP4.3 (C36, D88): the light grid of the adopted bake (gridAreasOf: empty without a
   * tree, a grid, or with a dropped bake), published when the scene adopts a bake slot or
   * forgets one (a new map path); its version moves only when its areas change. Never
   * null; the render thread (other threads read LitSession::gridTable)
   */
  std::shared_ptr<const GridTable> gridTable() const;
  /**
   * LP4.3 (C38, D90): the bake the scene adopted for its map path (MapBakeSlot::bake):
   * null before the path's request is answered, for a path outside the game folder, and
   * without a map path; immutable. answered tells the first case from the others: the
   * scene has a map path and its bake request was answered. The render thread
   */
  std::shared_ptr<const MapBake> adoptedBake() const;
  bool bakeAnswered() const;
  // what the jobs and the copied code reported, oldest first
  std::vector<std::string> takeWarnings();
  /**
   * LP2.2b (D77): chunks were built with the defaults of a material the table does not
   * have yet; they are built again when it has it. LP5.F (A41): or a chunk of a Default
   * editor-image size (Chunk::provisional) was built while its material's size was not
   * measured yet (MaterialSlot::sizeMeasured); the wait ends when a table the scene scans
   * has the slot measured, and the cells are built again when its size differs (S2.5).
   * One built from a measured Default slot is the engine's own answer for an image it
   * cannot load (C21's 8 × 8), rebuilt only when a file changes (S2.5). LP5.F (D111): a
   * slot published again unmeasured (remeasured) makes its cells wait so too, whatever
   * their size; a kept Ready size (D52) does not. LP7.2 (C53, A41): or a brush entity was
   * built while its skin was not in the table (its chunks provisional, built again when
   * a table has it).
   */
  bool waitsForMaterials() const;
  // DX.4: entities were built with default parms while their classname's entityDef was
  // not in the table; they are built again when it is
  bool waitsForEntityDefs() const;
  // DX.4 (C10): the bake of the map's path was asked for and the table has not answered
  // that request yet
  bool waitsForBake() const;

  /**
   * LP7.4 (C55): the scene's adoption totals: the adopted islands and their triangles,
   * and over the models the scene draws from (every _areaN model with a worldspawn,
   * each brush entity's own model while the entity has face pieces) the planar islands
   * no editor face matched and the curved ones. The render thread
   */
  AdoptionCounts adoptionCounts() const;
  /**
   * LP7.4 (C55): an entity's adoption table, or null (none adopted): the islands that
   * match its editor faces, which LP7.3's dress identification asks of a surface (C54's
   * "matches no editor face"); immutable. The render thread
   */
  std::shared_ptr<const EntityAdoption> adoptionOf(PreviewId entity) const;

  // waits for the batches in flight and applies them, until none is in flight (tests)
  void finishForTesting();
  // LPF.0b: the cells the last batch started builds (tests)
  const std::set<CellRef>& lastBatchCellsForTesting() const;

  deleteCopyAndMove(SceneGeometry);

private:
  void markEntity(PreviewId entity);
  void markAll();
  // R2.3: the cells of an entity's nodes once its chunk entity may have changed
  void rekeyEntity(PreviewId entity);
  void addNode(PreviewId id, const CellRef& ref, PreviewId entity);
  void removeNode(PreviewId id);
  void startBatch();
  // adopts the latest material table and, when it is new, scans it (R3.1)
  void refreshTable();
  void scanTable();
  // S2.5: the editor-image size the scene's table has for a material (Default when it
  // has none)
  EditorImageSize tableSizeOf(const std::string& material) const;
  void forgetSizedCells(const std::vector<CellRef>& cells);
  // LP5.F: m_waitsForMeasure from m_sizedCells
  void updateMeasureWait();
  void forgetMaterialCells(const std::vector<CellRef>& cells);
  void apply(ChunkJobOutput output);
  // C31, D80: the touched entities' bounds and area references from their chunks
  void updateEntityReferences();
  // LP7.4 (C55): a new tree's islands and models, the old adoption forgotten
  void adoptTree();
  // LP7.4: the face pieces of the cells an applied job rebuilt, and the groups they
  // changed
  void storePieces(const ChunkJobOutput& output);
  // LP7.4 (C55, P7.11): the whole-entity pass after the batch applied: the changed
  // groups evaluated, the cells of every island whose adoption changed marked
  void updateAdoption();
  // LP7.4 (K44): the adopted islands drop( entity, island ) selects leave the tables,
  // their cells marked
  template <typename Drop>
  void dropAdoption(const Drop& drop);
  void markAdoptedIsland(const AdoptedIsland& island);
  void updateAdoptionCounts();
  bool collectChunks();
  PreviewId chunkEntityOf(const SceneNode& node) const;
  // LP4.2a (C37): the probe registry's inputs now, its entityDefs asked for
  ProbeJobInput probeInput();
  // LP4.3 (C36): the grid table of the bake adopted now, published when its areas changed
  void publishGridTable();
};

/**
 * LP2.4 (LitSession::setMapPathOverrideForTesting), any thread: the game-relative map
 * path takeSnapshot uses instead of a snapshot's document path, until null; the caller
 * keeps the string alive meanwhile. Tests and measurements only.
 */
void setSceneMapPathOverrideForTesting(const char* gameRelativeMapPath);

} // namespace tb::lit
