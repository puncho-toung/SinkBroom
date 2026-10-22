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
struct MaterialTable;
struct ProcTree;

/**
 * The render thread's scene and chunks (§3.3, §3.4, S31, B0.3): it merges each snapshot
 * into the scene it keeps (entity records, nodes, the cells their primitives are chunked
 * in), works out the cells that changed, and builds them as jobs on the session's CPU
 * pool (D59), inputs by shared_ptr. One batch of jobs is in flight at a time: the render
 * thread takes the next snapshot only when the last one's chunks are applied
 * (readyForSnapshot), so results apply in the snapshots' sequence order. A batch is also
 * started, between snapshots, when the material table makes a provisional chunk's size
 * Ready or brings a pending material (C21), or when the .proc of the map path arrives.
 *
 * Without a pool (or once it is stopped) the batches run on the calling thread. Without a
 * material service every material takes the defaults, provisionally, and there is no
 * .proc: every piece is in area 0.
 *
 * The scene's lights are the LightRegistry's (LP1b.3, C15): it takes the same snapshots
 * and .proc tree, builds the light records as its own pool job, and publishes them in a
 * LightTable (lightTable()). A snapshot is taken only when neither the chunks nor the
 * lights have a job in flight.
 *
 * Threads: the render thread only, lightTable() aside (any thread); the jobs on the pool
 * see only their inputs.
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
  std::optional<std::string> m_mapPath;
  std::shared_ptr<const ProcTree> m_proc;
  std::shared_ptr<const MaterialTable> m_table;

  // the chunks, and the cells built with materials that were pending or provisional
  std::map<ChunkKey, Chunk> m_chunks;
  std::unordered_map<PreviewId, ChunkEntityState> m_entityStates;
  std::map<std::string, std::set<CellRef>> m_provisional;
  std::map<CellRef, std::size_t> m_solidPieces;

  // the batch in flight
  std::set<CellRef> m_dirty;
  std::vector<std::future<CpuJobResult<ChunkJobOutput>>> m_jobs;
  std::uint64_t m_batchSequence = 0;
  std::uint64_t m_appliedSequence = 0;
  std::uint64_t m_lastTableVersion = 0;
  std::vector<std::string> m_warnings;

  // the light records (declared last: its job is waited for first)
  LightRegistry m_lights;

public:
  SceneGeometry(CpuPool* pool, MaterialService* materials);
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
  // C15: the latest published light records (never null); any thread
  std::shared_ptr<const LightTable> lightTable() const;
  // what the jobs and the copied code reported, oldest first
  std::vector<std::string> takeWarnings();

  // waits for the batch in flight and applies it (tests)
  void finishForTesting();

  deleteCopyAndMove(SceneGeometry);

private:
  void markEntity(PreviewId entity);
  void markAll();
  void startBatch();
  void refreshTable();
  void apply(ChunkJobOutput output);
  bool collectChunks();
  PreviewId chunkEntityOf(const SceneNode& node) const;
};

} // namespace tb::lit
