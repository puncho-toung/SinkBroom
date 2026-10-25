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

#include "lit/SceneGeometry.h"

#include "lit/AreaData.h"
#include "lit/EngineLight.h"
#include "lit/EntityState.h"
#include "lit/LightData.h"
#include "lit/MapBake.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace tb::lit
{
namespace
{

// LP2.4: setSceneMapPathOverrideForTesting's path; constant-initialised and trivially
// destructible (C9), the string the caller's
constinit auto s_mapPathOverride = std::atomic<const char*>{nullptr};

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             const auto lower = [](const char c) {
               return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
             };
             return lower(a) == lower(b);
           });
}

bool isPrimitive(const SceneNode& node)
{
  return node.kind == SnapshotNodeKind::Brush || node.kind == SnapshotNodeKind::Patch;
}

// CONTENTS_ORIGIN (renderer/Material.h), which ChunkBuilder's IsOriginBrush reads
constexpr auto ContentsOrigin = 1 << 22;

// LPF.0b (C43): the material has CONTENTS_ORIGIN in a table
bool originIn(const MaterialTable* table, const std::string& material)
{
  const auto slot = table ? table->find(material) : nullptr;
  return slot && slot->entry && (slot->entry->contentFlags & ContentsOrigin) != 0;
}

// LP5.F (S2.2): the material's editor-image size in a table is final for cells built at
// a Default size or not (Chunk::provisional): its slot is measured, or Failed (no entry,
// no image), or (D52) a Ready size kept for it not published again unmeasured (D111)
bool sizeFinalIn(
  const MaterialTable* table, const std::string& material, const bool defaultSize)
{
  const auto slot = table ? table->find(material) : nullptr;
  return slot
         && (!slot->entry || slot->sizeMeasured || (!defaultSize && !slot->remeasured));
}

// removes refs from every set of waiting, and the sets that are empty then
void forgetCells(
  std::map<std::string, std::set<CellRef>>& waiting, const std::vector<CellRef>& refs)
{
  for (auto it = waiting.begin(); it != waiting.end();)
  {
    for (const auto& ref : refs)
    {
      it->second.erase(ref);
    }
    it = it->second.empty() ? waiting.erase(it) : std::next(it);
  }
}

} // namespace

SceneGeometry::SceneGeometry(
  CpuPool* pool,
  MaterialService* materials,
  std::shared_ptr<LightTablePublication> lights,
  std::shared_ptr<ProbeTablePublication> probes,
  std::shared_ptr<GridTablePublication> grids)
  : m_pool{pool}
  , m_materials{materials}
  , m_grids{grids ? std::move(grids) : std::make_shared<GridTablePublication>()}
  , m_probes{pool, std::move(probes)}
  , m_lights{pool, materials, std::move(lights)}
{
  // C37: the automatic probes need the env_probe entityDef (Game_local.cpp:1295)
  if (m_materials)
  {
    static_cast<void>(m_materials->entityDef(EnvProbeClassname));
  }
}

SceneGeometry::~SceneGeometry()
{
  // R5.22: the futures of a batch in flight are only waited for
  for (auto& job : m_jobs)
  {
    if (job.valid())
    {
      job.wait();
    }
  }
}

bool SceneGeometry::readyForSnapshot() const
{
  return m_jobs.empty() && m_lights.readyForSnapshot();
}

bool SceneGeometry::busy() const
{
  return !m_jobs.empty() || m_lights.busy();
}

bool SceneGeometry::probesBuilding() const
{
  return m_probesDirty || m_probes.busy();
}

bool SceneGeometry::probesInFlight() const
{
  // without a material service an entityDef is never answered: a provisional table is
  // then final
  return probesBuilding() || (m_materials && m_probes.table()->provisional);
}

PreviewId SceneGeometry::chunkEntityOf(const SceneNode& node) const
{
  // a func_group's primitives join worldspawn (A11)
  const auto it = m_entities.find(node.entity);
  if (
    it != m_entities.end() && m_worldspawn != NoPreviewId
    && equalsNoCase(it->second->classname, "func_group"))
  {
    return m_worldspawn;
  }
  return node.entity;
}

void SceneGeometry::markEntity(const PreviewId entity)
{
  // R2.4: the entity's own nodes, not every node of the scene
  if (const auto nodes = m_entityNodes.find(entity); nodes != m_entityNodes.end())
  {
    for (const auto id : nodes->second)
    {
      if (const auto ref = m_nodeCells.find(id); ref != m_nodeCells.end())
      {
        m_dirty.insert(ref->second);
      }
    }
  }
}

void SceneGeometry::markAll()
{
  for (const auto& [ref, nodes] : m_cellNodes)
  {
    m_dirty.insert(ref);
  }
  for (const auto& [key, chunk] : m_chunks)
  {
    m_dirty.insert(CellRef{key.entity, key.cell});
  }
}

void SceneGeometry::addNode(
  const PreviewId id, const CellRef& ref, const PreviewId entity)
{
  m_nodeCells[id] = ref;
  m_cellNodes[ref].insert(id);
  m_entityNodes[entity].insert(id);
  m_dirty.insert(ref);
}

void SceneGeometry::removeNode(const PreviewId id)
{
  if (const auto cell = m_nodeCells.find(id); cell != m_nodeCells.end())
  {
    m_dirty.insert(cell->second);
    if (const auto nodes = m_cellNodes.find(cell->second); nodes != m_cellNodes.end())
    {
      nodes->second.erase(id);
      if (nodes->second.empty())
      {
        m_cellNodes.erase(nodes);
      }
    }
    m_nodeCells.erase(cell);
  }
  if (const auto node = m_nodes.find(id); node != m_nodes.end())
  {
    if (const auto nodes = m_entityNodes.find(node->second->entity);
        nodes != m_entityNodes.end())
    {
      nodes->second.erase(id);
      if (nodes->second.empty())
      {
        m_entityNodes.erase(nodes);
      }
    }
  }
}

void SceneGeometry::rekeyEntity(const PreviewId entity)
{
  const auto nodes = m_entityNodes.find(entity);
  if (nodes == m_entityNodes.end())
  {
    return;
  }
  for (const auto id : nodes->second)
  {
    const auto cell = m_nodeCells.find(id);
    const auto node = m_nodes.find(id);
    if (cell == m_nodeCells.end() || node == m_nodes.end())
    {
      continue;
    }
    const auto ref = CellRef{chunkEntityOf(*node->second), cell->second.cell};
    if (ref == cell->second)
    {
      continue;
    }
    // the old key's chunks go, the new key's are built
    m_dirty.insert(cell->second);
    if (const auto old = m_cellNodes.find(cell->second); old != m_cellNodes.end())
    {
      old->second.erase(id);
      if (old->second.empty())
      {
        m_cellNodes.erase(old);
      }
    }
    cell->second = ref;
    m_cellNodes[ref].insert(id);
    m_dirty.insert(ref);
  }
}

void SceneGeometry::takeSnapshot(const Snapshot& snapshot)
{
  m_batchSequence = snapshot.sequence;
  // C37: the entities, and with them the placed probes, may have changed
  m_probesDirty = true;

  // B0.4: removed holds node and entity ids
  for (const auto id : snapshot.removed)
  {
    removeNode(id);
    m_nodes.erase(id);
    if (m_entities.erase(id) > 0)
    {
      markEntity(id);
      m_entityStates.erase(id);
      if (id == m_worldspawn)
      {
        m_worldspawn = NoPreviewId;
      }
    }
  }

  // entity records, also without their nodes: their state applies to all their primitives
  for (auto& entity : sceneEntitiesOf(snapshot))
  {
    const auto id = entity->id;
    const auto isWorld = equalsNoCase(entity->classname, "worldspawn");
    const auto old = m_entities.find(id);
    const auto classnameChanged =
      old != m_entities.end() && old->second->classname != entity->classname;
    const auto changed =
      old == m_entities.end() || old->second->keys != entity->keys || classnameChanged;
    m_entities[id] = std::move(entity);
    if (isWorld)
    {
      m_worldspawn = id;
      // V.5: the material that replaces every side and patch (MapFile.cpp:1767-1796) is
      // asked for, so that a table has it
      if (m_materials)
      {
        const auto merged = mergeSpawnArgs(m_entities[id]->keys, nullptr);
        if (const auto* material = findKey(merged, "overrideMaterial"))
        {
          static_cast<void>(m_materials->lookup(*material));
        }
      }
    }
    if (classnameChanged)
    {
      // R2.3: into or out of func_group, the nodes' chunk entity changes with it
      rekeyEntity(id);
    }
    if (changed)
    {
      // worldspawn's keys (removeEntities, overrideMaterial) touch every primitive
      if (isWorld)
      {
        markAll();
      }
      else
      {
        markEntity(id);
      }
    }
  }

  // nodes: the cell they left and the cell they are in
  for (auto& node : sceneNodesOf(snapshot))
  {
    const auto id = node->id;
    removeNode(id);
    if (isPrimitive(*node))
    {
      addNode(id, CellRef{chunkEntityOf(*node), cellOfNode(*node)}, node->entity);
      // the materials are asked for now, so that the table has them soon (C21)
      if (m_materials)
      {
        for (const auto& face : node->faces)
        {
          static_cast<void>(m_materials->lookup(engineSideMaterial(face.material)));
        }
        if (node->patch)
        {
          static_cast<void>(
            m_materials->lookup(enginePatchMaterial(node->patch->material)));
        }
      }
    }
    m_nodes[id] = std::move(node);
  }

  // LP2.4: a measurement's map copy stands for the game's map of the override's path
  auto mapPath = snapshot.mapPath;
  if (const auto* overridePath = s_mapPathOverride.load(); overridePath && mapPath)
  {
    mapPath = std::string{overridePath};
  }
  if (mapPath && mapPath != m_mapPath)
  {
    // C10: the bake is asked for this path; a slot read before (a Save As back to it) is
    // never adopted, only the answer to this request
    m_mapPath = std::move(mapPath);
    m_proc.reset();
    m_areas.reset();
    m_bakeSlot.reset();
    // LP4.3 (C36): the old path's grid goes with its bake
    publishGridTable();
    m_bakeTicket = m_materials ? m_materials->loadMapBake(*m_mapPath) : 0;
    markAll();
  }

  // V.8: a tree the table holds is adopted before the lights take the snapshot, so that
  // they are built with its areas once, not without them first; W.2: the lights take it
  // with the snapshot, not on their own before it
  if (m_jobs.empty())
  {
    refreshTable();
    // the table adopted before the path was known may hold it too
    if (m_table && m_mapPath)
    {
      scanTable();
    }
  }
  m_lights.takeSnapshot(snapshot, m_proc);
  startBatch();
}

void SceneGeometry::refreshTable()
{
  if (!m_materials)
  {
    return;
  }
  auto table = m_materials->table();
  if (!table || table == m_table)
  {
    return;
  }
  // V.2: a reload's new registry: every chunk and entity state was built with the old
  // registry's materials and entityDefs, and the new table answers every name at once.
  // W.1: also after a build that failed (registry 0, Failed slots), whose cells waited
  // for nothing
  const auto reloaded = m_table && table->registry != m_table->registry;
  m_table = std::move(table);
  if (reloaded)
  {
    m_pendingMaterials.clear();
    m_sizedCells.clear();
    m_pendingEntityDefs.clear();
    // LPF.0b: every cell is built again with the new registry's entries
    m_materialCells.clear();
    m_cellMaterials.clear();
    m_materialsRevision = m_table->materialsRevision;
    markAll();
  }
  scanTable();
}

bool SceneGeometry::waitsForMaterials() const
{
  return !m_pendingMaterials.empty() || m_waitsForMeasure;
}

bool SceneGeometry::waitsForEntityDefs() const
{
  return !m_pendingEntityDefs.empty();
}

bool SceneGeometry::waitsForBake() const
{
  return m_mapPath && m_materials && m_bakeTicket != 0
         && (!m_bakeSlot || m_bakeSlot->ticket < m_bakeTicket);
}

EditorImageSize SceneGeometry::tableSizeOf(const std::string& material) const
{
  const auto slot = m_table ? m_table->find(material) : nullptr;
  return slot && slot->entry ? slot->entry->editorImageSize : EditorImageSize{};
}

void SceneGeometry::forgetMaterialCells(const std::vector<CellRef>& cells)
{
  for (const auto& ref : cells)
  {
    const auto materials = m_cellMaterials.find(ref);
    if (materials == m_cellMaterials.end())
    {
      continue;
    }
    for (const auto& material : materials->second)
    {
      if (const auto it = m_materialCells.find(material); it != m_materialCells.end())
      {
        it->second.cells.erase(ref);
        if (it->second.cells.empty())
        {
          m_materialCells.erase(it);
        }
      }
    }
    m_cellMaterials.erase(materials);
  }
}

void SceneGeometry::forgetSizedCells(const std::vector<CellRef>& cells)
{
  for (auto it = m_sizedCells.begin(); it != m_sizedCells.end();)
  {
    for (const auto& ref : cells)
    {
      it->second.cells.erase(ref);
    }
    it = it->second.cells.empty() ? m_sizedCells.erase(it) : std::next(it);
  }
}

void SceneGeometry::updateMeasureWait()
{
  m_waitsForMeasure = std::ranges::any_of(
    m_sizedCells, [](const auto& sized) { return !sized.second.measured; });
}

void SceneGeometry::scanTable()
{
  // C37: a new table may answer an entityDef the probes wait for, or bring a bake
  m_probesDirty = true;
  // R3.1: what the cells wait for is looked for in every table the scene adopts, also
  // one that a batch for other cells adopts first. C10: the bake that answers the map
  // path's request, or a later read of it; the lights take its tree with the next
  // snapshot or batch (W.2)
  if (m_mapPath)
  {
    if (const auto slot = m_table->findMapBake(*m_mapPath);
        slot && slot != m_bakeSlot && slot->ticket >= m_bakeTicket)
    {
      m_bakeSlot = slot;
      // C31: the bake's AreaData comes with its tree
      m_areas = slot->tree && slot->bake ? slot->bake->areas : nullptr;
      // LP4.3 (C36): and its light grid
      publishGridTable();
      if (slot->tree != m_proc)
      {
        m_proc = slot->tree;
        markAll();
      }
    }
  }
  for (auto it = m_pendingMaterials.begin(); it != m_pendingMaterials.end();)
  {
    if (m_table->find(it->first))
    {
      m_dirty.insert(it->second.begin(), it->second.end());
      it = m_pendingMaterials.erase(it);
    }
    else
    {
      ++it;
    }
  }
  // LP1c fix (S2.5): a material whose editor-image size is no more the one its cells
  // were built with rebuilds them (Default to Ready, Ready to another size, a deleted
  // image back to Default)
  for (auto it = m_sizedCells.begin(); it != m_sizedCells.end();)
  {
    if (tableSizeOf(it->first) != it->second.size)
    {
      m_dirty.insert(it->second.cells.begin(), it->second.cells.end());
      it = m_sizedCells.erase(it);
    }
    else
    {
      // LP5.F: a measure that found the size the cells were built with ends their wait;
      // a slot published again unmeasured (D111) starts one
      it->second.measured = sizeFinalIn(
        m_table.get(),
        it->first,
        it->second.size.state == EditorImageSize::State::Default);
      ++it;
    }
  }
  updateMeasureWait();
  m_sizesRevision = m_table->sizesRevision;
  // LPF.0b (C43, D106, P5.9): a re-parse moved the table's materialsRevision: the cells
  // of every material whose revision stamp is no more the one they were built with are
  // built again (whether the material made a chunk or not), and an entity whose material
  // gained or lost CONTENTS_ORIGIN is built again whole, since its origin brushes' pivot
  // moves (ChunkBuilder's IsOriginBrush). An image override moves no revision
  if (m_table->materialsRevision != m_materialsRevision)
  {
    m_materialsRevision = m_table->materialsRevision;
    for (auto& [material, built] : m_materialCells)
    {
      const auto stamp = m_table->revisionStamp(material);
      if (stamp == built.stamp)
      {
        continue;
      }
      m_dirty.insert(built.cells.begin(), built.cells.end());
      const auto origin = originIn(m_table.get(), material);
      if (origin != built.origin)
      {
        for (const auto& ref : built.cells)
        {
          // worldspawn's own and a func_group's primitives have no pivot
          if (ref.entity != m_worldspawn)
          {
            markEntity(ref.entity);
          }
        }
      }
      built.stamp = stamp;
      built.origin = origin;
    }
  }
  // R3.2: an entityDef that arrives after the entity's first batch rebuilds its chunks
  for (auto it = m_pendingEntityDefs.begin(); it != m_pendingEntityDefs.end();)
  {
    if (m_table->findEntityDef(it->first))
    {
      for (const auto entity : it->second)
      {
        markEntity(entity);
      }
      it = m_pendingEntityDefs.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void SceneGeometry::startBatch()
{
  if (!m_jobs.empty())
  {
    return;
  }
  refreshTable();
  // W.2: a tree adopted between snapshots reaches the lights here (none changes)
  m_lights.setProcTree(m_proc);
  // C37, D87: the probes' inputs, built again when they changed
  if (m_probesDirty)
  {
    m_probesDirty = false;
    m_probes.update(probeInput());
  }
  if (m_dirty.empty())
  {
    return;
  }

  // one job per world cell; one per brush entity with every cell of the entity (R3.4:
  // the pivot of its origin brushes applies to all its chunks)
  auto entityJobs = std::map<PreviewId, std::set<CellRef>>{};
  auto jobs = std::vector<std::vector<CellRef>>{};
  for (const auto& ref : m_dirty)
  {
    if (ref.entity == m_worldspawn)
    {
      jobs.push_back({ref});
    }
    else
    {
      entityJobs[ref.entity].insert(ref);
    }
  }
  m_dirty.clear();
  m_lastBatchCells.clear();
  for (const auto& job : jobs)
  {
    m_lastBatchCells.insert(job.begin(), job.end());
  }
  for (auto& [entity, refs] : entityJobs)
  {
    if (const auto nodes = m_entityNodes.find(entity); nodes != m_entityNodes.end())
    {
      for (const auto id : nodes->second)
      {
        if (const auto ref = m_nodeCells.find(id);
            ref != m_nodeCells.end() && ref->second.entity == entity)
        {
          refs.insert(ref->second);
        }
      }
    }
    jobs.emplace_back(refs.begin(), refs.end());
    m_lastBatchCells.insert(refs.begin(), refs.end());
  }

  const auto worldspawn = m_entities.find(m_worldspawn);
  for (auto& refs : jobs)
  {
    auto input = std::make_shared<ChunkJobInput>();
    input->cells = refs;
    input->proc = m_proc;
    input->materials = m_table;
    const auto addEntity = [&](const PreviewId id) {
      const auto it = m_entities.find(id);
      if (it == m_entities.end() || input->entities.contains(id))
      {
        return;
      }
      input->entities[id] = it->second;
      const auto& classname = it->second->classname;
      if (input->entityDefs.contains(classname))
      {
        return;
      }
      if (const auto slot = m_table ? m_table->findEntityDef(classname) : nullptr)
      {
        if (slot->entry)
        {
          input->entityDefs[classname] = slot->entry;
        }
      }
      else if (m_materials)
      {
        // C22: asked now; R3.2: the entity is built again once a table has the answer
        static_cast<void>(m_materials->entityDef(classname));
        m_pendingEntityDefs[classname].insert(id);
      }
    };
    if (worldspawn != m_entities.end())
    {
      input->worldspawn = worldspawn->second;
      addEntity(m_worldspawn);
    }
    for (const auto& ref : refs)
    {
      if (const auto nodes = m_cellNodes.find(ref); nodes != m_cellNodes.end())
      {
        for (const auto id : nodes->second)
        {
          const auto& node = m_nodes.at(id);
          input->nodes.push_back(node);
          addEntity(node->entity);
        }
      }
    }

    auto job = [input](const CpuJobContext& context) -> Result<ChunkJobOutput> {
      // R2.1: the job of a cancelled pool returns at its next node
      auto output = buildChunks(*input, &context);
      if (context.stale())
      {
        return Error{"The chunk job was abandoned"};
      }
      return output;
    };
    if (m_pool && !m_pool->stopped())
    {
      m_jobs.push_back(m_pool->run(std::move(job)));
    }
    else
    {
      // no pool: the job runs here, and its messages are drained here
      auto promise = std::promise<CpuJobResult<ChunkJobOutput>>{};
      m_jobs.push_back(promise.get_future());
      auto output = CpuJobResult<ChunkJobOutput>{buildChunks(*input), {}};
      output.messages = drainEngineMessagesForThread(std::this_thread::get_id());
      promise.set_value(std::move(output));
    }
  }
}

void SceneGeometry::apply(ChunkJobOutput output)
{
  // what the rebuilt cells waited for is replaced by what they wait for now
  forgetSizedCells(output.cells);
  forgetCells(m_pendingMaterials, output.cells);
  forgetMaterialCells(output.cells);
  // LPF.0b (C43, D106): every material the job read, drawn or not, with the revision
  // stamp and the CONTENTS_ORIGIN of the table the batch was built with (still the
  // scene's); a material whose other cells were built with an older stamp keeps it, so
  // that the next scan builds them all
  for (const auto& material : output.materials)
  {
    auto& built = m_materialCells[material];
    if (built.cells.empty())
    {
      built.stamp = m_table ? m_table->revisionStamp(material) : 0;
      built.origin = originIn(m_table.get(), material);
    }
    for (const auto& ref : output.cells)
    {
      built.cells.insert(ref);
      m_cellMaterials[ref].push_back(material);
    }
  }
  for (const auto& ref : output.cells)
  {
    m_touchedEntities.insert(ref.entity);
    // the cell's old chunks go
    for (auto it = m_chunks.lower_bound(
           ChunkKey{std::numeric_limits<int>::min(), ref.entity, {}, ref.cell, 0});
         it != m_chunks.end() && it->first.entity == ref.entity
         && it->first.cell == ref.cell;)
    {
      it = m_chunks.erase(it);
    }
    m_solidPieces.erase(ref);
  }
  for (auto& chunk : output.chunks)
  {
    const auto ref = CellRef{chunk.key.entity, chunk.key.cell};
    // the table the batch was built with is still the scene's (a batch adopts no other)
    auto& sized = m_sizedCells[chunk.key.material];
    if (sized.cells.empty())
    {
      sized.size = tableSizeOf(chunk.key.material);
      // LP5.F (D77, A41): a chunk built at a Default size while its slot is unmeasured
      // waits for the measure (waitsForMaterials); a measured Default is final
      sized.measured = sizeFinalIn(m_table.get(), chunk.key.material, chunk.provisional);
      if (!sized.measured && m_materials)
      {
        // a slot the bulk parse published is measured once looked up (S2.2)
        static_cast<void>(m_materials->lookup(chunk.key.material));
      }
    }
    sized.cells.insert(ref);
    auto key = chunk.key;
    m_chunks.insert_or_assign(std::move(key), std::move(chunk));
  }
  for (const auto& material : output.pendingMaterials)
  {
    m_pendingMaterials[material].insert(output.cells.begin(), output.cells.end());
    // V.5: a material only the job knew of (an overrideMaterial) is asked for too
    if (m_materials)
    {
      static_cast<void>(m_materials->lookup(material));
    }
  }
  updateMeasureWait();
  if (!output.cells.empty())
  {
    m_solidPieces[output.cells.front()] += output.solidPieces;
  }
  for (auto& state : output.entities)
  {
    const auto id = state.id;
    m_touchedEntities.insert(id);
    m_entityStates.insert_or_assign(id, std::move(state));
  }
  for (auto& warning : output.warnings)
  {
    m_warnings.push_back(std::move(warning));
  }
}

void SceneGeometry::updateEntityReferences()
{
  if (m_touchedEntities.empty())
  {
    return;
  }
  auto touched = std::exchange(m_touchedEntities, {});
  std::erase_if(
    touched, [&](const PreviewId id) { return !m_entityStates.contains(id); });
  if (touched.empty())
  {
    return;
  }

  // the union of every chunk of each touched entity, per material (model space)
  auto unions = std::unordered_map<PreviewId, std::map<std::string, EngineBounds>>{};
  for (const auto& [key, chunk] : m_chunks)
  {
    if (touched.contains(key.entity))
    {
      auto& perMaterial = unions[key.entity];
      const auto [it, added] =
        perMaterial.try_emplace(key.material, engineClearedBounds());
      it->second = engineAddBounds(it->second, chunk.bounds);
    }
  }

  // FinishSurfaces' growth of a deforming material's surface (Model.cpp:1346-1360), from
  // the table the batch was built with
  const auto deforms = [&](const std::string& material) {
    const auto slot = m_table ? m_table->find(material) : nullptr;
    return slot && slot->entry && slot->entry->deform != 0;
  };
  const auto hasTree = m_proc && !m_proc->nodes.empty();
  const auto commonAreas = !hasTree  ? std::vector<int>{}
                           : m_areas ? m_areas->commonAreas
                                     : commonChildrenAreas(*m_proc);

  for (const auto id : touched)
  {
    auto& state = m_entityStates.at(id);
    state.materialBounds.clear();
    state.localReferenceBounds = engineClearedBounds();
    if (const auto it = unions.find(id); it != unions.end())
    {
      for (auto& [material, bounds] : it->second)
      {
        auto surface = deforms(material) ? engineDeformSurfaceBounds(bounds) : bounds;
        state.localReferenceBounds = engineAddBounds(state.localReferenceBounds, surface);
        state.materialBounds.emplace(material, surface);
      }
    }
    state.areas.clear();
    // R_CreateEntityRefs: cleared bounds add no reference
    if (engineBoundsIsCleared(state.localReferenceBounds))
    {
      state.inverseBaseModelProject = EngineMat4{};
      state.globalReferenceBounds = engineClearedBounds();
      continue;
    }
    // R_DeriveEntityData (RenderWorld_defs.cpp:49-60)
    const auto modelRenderMatrix =
      engineMatrixFromOriginAxis(state.modelOrigin, state.parms.axis);
    state.inverseBaseModelProject =
      engineOffsetScaleForBounds(modelRenderMatrix, state.localReferenceBounds);
    state.globalReferenceBounds =
      engineProjectedBounds(state.inverseBaseModelProject, engineUnitCube(), false);
    if (hasTree)
    {
      // PushFrustumIntoTree( entity, NULL, inverseBaseModelProject, bounds_unitCube ),
      // each new reference prepended to the entity's chain (RenderWorld.cpp:1836-1837)
      auto pushed = pushFrustumIntoTree(
        *m_proc, commonAreas, state.inverseBaseModelProject, engineUnitCube());
      state.areas.assign(pushed.rbegin(), pushed.rend());
    }
  }
}

bool SceneGeometry::collect()
{
  const auto lightsApplied = m_lights.collect();
  // C37: a finished probe build publishes its table; it is no batch of the scene's (a
  // frame that waited for it sees probesInFlight() change, geometryTick's trigger), and
  // its warnings are posted with the next batch
  m_probes.collect();
  const auto chunksApplied = collectChunks();
  return chunksApplied || lightsApplied;
}

ProbeJobInput SceneGeometry::probeInput()
{
  // PopulateEnvironmentProbes' inputs (Game_local.cpp:1275-1336) and
  // R_DeriveEnvprobeData's (RenderWorld_defs.cpp:808-924)
  auto input = ProbeJobInput{};
  input.tree = m_proc;
  input.areas = m_proc ? m_areas : nullptr;
  if (m_proc && m_bakeSlot && m_bakeSlot->bake)
  {
    input.shProbes = m_bakeSlot->bake->shProbes;
    input.bakeBase = m_bakeSlot->bake->bakeBase;
  }

  // C22: an entityDef the table does not have yet is asked for, and what depends on it
  // is provisional until a table answers
  auto asked = std::set<std::string>{};
  const auto entityDef = [&](const std::string& classname) {
    const auto slot = m_table ? m_table->findEntityDef(classname) : nullptr;
    if (!slot)
    {
      input.provisional = true;
      if (m_materials && asked.insert(classname).second)
      {
        static_cast<void>(m_materials->entityDef(classname));
      }
    }
    return slot;
  };
  if (const auto slot = entityDef(std::string{EnvProbeClassname}))
  {
    input.envProbeDef = slot->entry ? EnvProbeDefState::Found : EnvProbeDefState::Missing;
  }

  // the placed probes: SpawnEntityDef's spawnclass of the merged spawn args (the map's
  // keys over the entityDef's, Game_local.cpp:3909), EnvironmentProbe; in preview-id
  // order
  auto ids = std::vector<PreviewId>{};
  ids.reserve(m_entities.size());
  for (const auto& [id, entity] : m_entities)
  {
    if (id != m_worldspawn && !entity->classname.empty())
    {
      ids.push_back(id);
    }
  }
  std::ranges::sort(ids);
  for (const auto id : ids)
  {
    const auto& entity = *m_entities.at(id);
    const auto slot = entityDef(entity.classname);
    if (!slot)
    {
      continue;
    }
    const auto* defKeys = slot->entry ? &slot->entry->keys : nullptr;
    const auto* own = findKey(entity.keys, "spawnclass");
    const auto* inherited = defKeys ? findKey(*defKeys, "spawnclass") : nullptr;
    const auto* spawnclass = own ? own : inherited;
    if (spawnclass && *spawnclass == EnvProbeSpawnclass)
    {
      input.placed.push_back(ProbeEntity{id, mergeSpawnArgs(entity.keys, defKeys)});
    }
  }
  return input;
}

bool SceneGeometry::collectChunks()
{
  if (!m_jobs.empty())
  {
    for (const auto& job : m_jobs)
    {
      if (job.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
      {
        return false;
      }
    }
    // every job of the batch is done: its results apply together, in sequence order
    auto jobs = std::move(m_jobs);
    m_jobs.clear();
    for (auto& job : jobs)
    {
      try
      {
        auto result = job.get();
        for (const auto& message : result.messages)
        {
          if (message.kind != DeclMessageKind::Info)
          {
            m_warnings.push_back(message.text);
          }
        }
        std::move(result.result)
          | kdl::transform([&](ChunkJobOutput output) { apply(std::move(output)); })
          | kdl::transform_error([&](auto e) { m_warnings.push_back(std::move(e.msg)); });
      }
      catch (const std::exception& e)
      {
        m_warnings.push_back(fmt::format("A chunk job failed: {}", e.what()));
      }
    }
    // D80: the references of the entities the batch touched, over all their chunks
    updateEntityReferences();
    m_appliedSequence = m_batchSequence;
    // V.1: a table published while the batch ran woke an idle pass that could not adopt
    // it; it is adopted now, and what the batch left waiting is built at once
    startBatch();
    return true;
  }

  // between snapshots: a newer table may bring what cells wait for (startBatch adopts
  // and scans it)
  startBatch();
  return false;
}

const std::map<ChunkKey, Chunk>& SceneGeometry::chunks() const
{
  return m_chunks;
}

const std::unordered_map<PreviewId, ChunkEntityState>& SceneGeometry::entityStates() const
{
  return m_entityStates;
}

std::size_t SceneGeometry::solidPieces() const
{
  auto total = std::size_t{0};
  for (const auto& [ref, count] : m_solidPieces)
  {
    total += count;
  }
  return total;
}

const std::set<CellRef>& SceneGeometry::lastBatchCellsForTesting() const
{
  return m_lastBatchCells;
}

std::uint64_t SceneGeometry::appliedSequence() const
{
  return m_appliedSequence;
}

std::uint64_t SceneGeometry::scannedMaterialsRevision() const
{
  return m_materialsRevision;
}

std::uint64_t SceneGeometry::scannedSizesRevision() const
{
  return m_sizesRevision;
}

std::shared_ptr<const ProcTree> SceneGeometry::procTree() const
{
  return m_proc;
}

std::shared_ptr<const AreaData> SceneGeometry::areaData() const
{
  return m_areas;
}

std::shared_ptr<const LightTable> SceneGeometry::lightTable() const
{
  return m_lights.table();
}

std::shared_ptr<const ProbeTable> SceneGeometry::probeTable() const
{
  return m_probes.table();
}

std::shared_ptr<const GridTable> SceneGeometry::gridTable() const
{
  return m_grids->table();
}

std::shared_ptr<const MapBake> SceneGeometry::adoptedBake() const
{
  return m_bakeSlot ? m_bakeSlot->bake : nullptr;
}

bool SceneGeometry::bakeAnswered() const
{
  return m_mapPath && m_bakeSlot && !waitsForBake();
}

void SceneGeometry::publishGridTable()
{
  // SetupLightGrid runs with the world it belongs to (InitFromMap): the slot's tree
  const auto* bake = m_bakeSlot && m_bakeSlot->tree ? m_bakeSlot->bake.get() : nullptr;
  auto areas = bake ? gridAreasOf(*bake) : std::vector<GridArea>{};
  const auto current = m_grids->table();
  if (current && current->areas == areas)
  {
    return;
  }
  auto table = std::make_shared<GridTable>();
  // the publication outlives a render thread: a new scene's table follows its last one
  table->version = (current ? current->version : 0) + 1;
  table->areas = std::move(areas);
  m_grids->publish(std::move(table));
}

std::vector<std::string> SceneGeometry::takeWarnings()
{
  auto warnings = std::exchange(m_warnings, {});
  for (auto& warning : m_lights.takeWarnings())
  {
    warnings.push_back(std::move(warning));
  }
  for (auto& warning : m_probes.takeWarnings())
  {
    warnings.push_back(std::move(warning));
  }
  return warnings;
}

void SceneGeometry::finishForTesting()
{
  // a batch that applies may start the next one (V.1), so until none is in flight
  while (busy() || probesBuilding())
  {
    for (auto& job : m_jobs)
    {
      job.wait();
    }
    m_lights.finishForTesting();
    m_probes.finishForTesting();
    collect();
  }
}

void setSceneMapPathOverrideForTesting(const char* gameRelativeMapPath)
{
  s_mapPathOverride = gameRelativeMapPath;
}

} // namespace tb::lit
