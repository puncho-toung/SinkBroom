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
#include "lit/LightRecord.h"
#include "lit/Snapshot.h"

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
struct MapBakeSlot;
struct MaterialTable;
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
 * entityDef that was pending when an entity was built (C22), and the bake of the map
 * path. The cells concerned are built again in that batch. Between snapshots, collect
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
 * The scene's lights are the LightRegistry's (LP1b.3, C15): it takes the same snapshots
 * and .proc tree, builds the light records as its own pool job, and publishes them in a
 * LightTable, into the session's LightTablePublication (R2.9: LitSession::lightTable is
 * what other threads read). A snapshot is taken only when neither the chunks nor the
 * lights have a job in flight.
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
  // the table the last batch was built with; every table adopted is scanned once
  std::shared_ptr<const MaterialTable> m_table;

  // the chunks, and what their cells wait for (R3.1, R3.2): the cells built while a
  // material was not in the table, the cells built with a Default editor-image size, and
  // the entities built while their classname's entityDef was not in the table
  std::map<ChunkKey, Chunk> m_chunks;
  std::unordered_map<PreviewId, ChunkEntityState> m_entityStates;
  std::map<std::string, std::set<CellRef>> m_pendingMaterials;
  std::map<std::string, std::set<CellRef>> m_provisional;
  std::map<std::string, std::set<PreviewId>> m_pendingEntityDefs;
  std::map<CellRef, std::size_t> m_solidPieces;

  // the batch in flight
  std::set<CellRef> m_dirty;
  std::vector<std::future<CpuJobResult<ChunkJobOutput>>> m_jobs;
  std::uint64_t m_batchSequence = 0;
  std::uint64_t m_appliedSequence = 0;
  std::vector<std::string> m_warnings;

  // the light records (declared last: its job is waited for first)
  LightRegistry m_lights;

public:
  // lights may be null: the light registry then publishes into a publication of its own
  SceneGeometry(
    CpuPool* pool,
    MaterialService* materials,
    std::shared_ptr<LightTablePublication> lights = nullptr);
  ~SceneGeometry();

  // B0.3: no batch is in flight
  bool readyForSnapshot() const;
  bool busy() const;

  // merges the snapshot (B0.4's rules) and starts its batch; only when readyForSnapshot
  void takeSnapshot(const Snapshot& snapshot);

  /**
   * Collects the finished jobs; once every job of the batch is done its results replace
   * the chunks of its cells, and true is returned; so too when the light job is done and
   * its records are published. When nothing is in flight it starts a batch for what a
   * new material table or .proc made stale.
   */
  bool collect();

  const std::map<ChunkKey, Chunk>& chunks() const;
  const std::unordered_map<PreviewId, ChunkEntityState>& entityStates() const;
  // K14: the pieces of world faces in solid
  std::size_t solidPieces() const;
  std::uint64_t appliedSequence() const;
  std::shared_ptr<const ProcTree> procTree() const;
  // C15: the latest published light records (never null); the render thread, which owns
  // the scene (other threads read LitSession::lightTable, R2.9)
  std::shared_ptr<const LightTable> lightTable() const;
  // what the jobs and the copied code reported, oldest first
  std::vector<std::string> takeWarnings();

  // waits for the batches in flight and applies them, until none is in flight (tests)
  void finishForTesting();

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
  void apply(ChunkJobOutput output);
  bool collectChunks();
  PreviewId chunkEntityOf(const SceneNode& node) const;
};

} // namespace tb::lit
