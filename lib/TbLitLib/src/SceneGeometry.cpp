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

#include "ViewFlow.h"
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

// LP7.4: a name in lower case (the adoption groups' materials, the models' names)
std::string lowerCase(std::string_view text)
{
  auto lower = std::string{text};
  for (auto& c : lower)
  {
    c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  }
  return lower;
}

// LP7.4 (K44): a face that is the same for the chunk builder (its polygon, plane points,
// material and Valve values)
bool sameFace(const SceneFace& lhs, const SceneFace& rhs)
{
  const auto& a = lhs.texture;
  const auto& b = rhs.texture;
  return lhs.polygon == rhs.polygon && lhs.points == rhs.points
         && lhs.material == rhs.material && a.uAxis == b.uAxis && a.uOffset == b.uOffset
         && a.vAxis == b.vAxis && a.vOffset == b.vOffset && a.uScale == b.uScale
         && a.vScale == b.vScale;
}

// LP7.4: FNV-1a over a piece's fields, for the groups a rebuilt cell changed
std::uint64_t pieceHash(std::uint64_t h, const AdoptionPiece& piece)
{
  const auto add = [&](const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (auto i = std::size_t{0}; i < size; ++i)
    {
      h ^= bytes[i];
      h *= 0x100000001B3ull;
    }
  };
  add(&piece.key.node, sizeof(piece.key.node));
  add(&piece.key.face, sizeof(piece.key.face));
  add(&piece.key.piece, sizeof(piece.key.piece));
  add(&piece.area, sizeof(piece.area));
  add(piece.material.data(), piece.material.size());
  add(&piece.plane, sizeof(piece.plane));
  add(piece.texVec.data(), sizeof(piece.texVec));
  add(&piece.textureWidth, sizeof(piece.textureWidth));
  add(&piece.textureHeight, sizeof(piece.textureHeight));
  add(piece.stOrigin.data(), sizeof(piece.stOrigin));
  add(piece.points.data(), piece.points.size() * sizeof(EngineVec3));
  return h;
}

bool sameAdoptedIsland(const AdoptedIsland& lhs, const AdoptedIsland& rhs)
{
  return lhs.model == rhs.model && lhs.surface == rhs.surface && lhs.island == rhs.island
         && lhs.cell == rhs.cell && lhs.area == rhs.area && lhs.material == rhs.material
         && lhs.candidates == rhs.candidates;
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

  // LP7.4 (K44): the nodes and faces this snapshot changes, whose adopted islands leave
  // the tables before the batch, so that no batch draws an island over an edited face
  auto changedNodes = std::set<PreviewId>{};
  auto changedFaces = std::map<PreviewId, std::set<std::uint32_t>>{};
  const auto nodeChanged = [&](const PreviewId id) { changedNodes.insert(id); };
  const auto entityChanged = [&](const PreviewId entity) {
    if (const auto nodes = m_entityNodes.find(entity); nodes != m_entityNodes.end())
    {
      for (const auto id : nodes->second)
      {
        nodeChanged(id);
      }
    }
  };
  auto changedEntities = std::set<PreviewId>{};

  // B0.4: removed holds node and entity ids
  for (const auto id : snapshot.removed)
  {
    nodeChanged(id);
    entityChanged(id);
    changedEntities.insert(id);
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
      // LP7.4 (K44): its state, its pivot or its primitives' entity may have changed
      changedEntities.insert(id);
      entityChanged(id);
    }
  }

  // nodes: the cell they left and the cell they are in
  for (auto& node : sceneNodesOf(snapshot))
  {
    const auto id = node->id;
    // LP7.4 (K44): the faces that differ from the node's last record
    if (const auto old = m_nodes.find(id); old != m_nodes.end())
    {
      const auto& before = *old->second;
      if (
        before.kind != node->kind || before.entity != node->entity
        || before.visible != node->visible || before.faces.size() != node->faces.size())
      {
        nodeChanged(id);
      }
      else
      {
        for (auto f = std::size_t{0}; f < node->faces.size(); ++f)
        {
          if (!sameFace(before.faces[f], node->faces[f]))
          {
            changedFaces[id].insert(std::uint32_t(f));
          }
        }
      }
    }
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
    // LP7.4: and the adoption with its tree
    adoptTree();
  }

  // LP7.4 (K44): an island one of whose candidates changed, or whose entity did, leaves
  // its table, its cells marked; the pass after the batch tries it again
  if (!changedNodes.empty() || !changedFaces.empty() || !changedEntities.empty())
  {
    dropAdoption([&](const PreviewId entity, const AdoptedIsland& island) {
      return changedEntities.contains(entity)
             || std::ranges::any_of(island.candidates, [&](const AdoptionPieceKey& key) {
                  if (changedNodes.contains(key.node))
                  {
                    return true;
                  }
                  const auto faces = changedFaces.find(key.node);
                  return faces != changedFaces.end() && faces->second.contains(key.face);
                });
    });
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
    m_pendingSkins.clear();
    // LPF.0b: every cell is built again with the new registry's entries
    m_materialCells.clear();
    m_cellMaterials.clear();
    m_materialsRevision = m_table->materialsRevision;
    markAll();
    // LP7.4 (K44): every island is tried again with the new registry's materials
    dropAdoption([](PreviewId, const AdoptedIsland&) { return true; });
    m_adoptionAll = true;
  }
  scanTable();
}

bool SceneGeometry::waitsForMaterials() const
{
  return !m_pendingMaterials.empty() || m_waitsForMeasure || !m_pendingSkins.empty();
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
        // LP7.4 (C55): its islands; every group is evaluated once the batch built with
        // the tree applies, and the cells of the islands adopted are built again then
        adoptTree();
      }
    }
  }
  // LP7.4 (K44): the materials whose cells are built again for their size or revision,
  // and the entities whose pivot moves: their islands leave the tables for the batch
  auto rescanned = std::set<std::string>{};
  auto repivoted = std::set<PreviewId>{};
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
      rescanned.insert(lowerCase(it->first));
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
      rescanned.insert(lowerCase(material));
      const auto origin = originIn(m_table.get(), material);
      if (origin != built.origin)
      {
        for (const auto& ref : built.cells)
        {
          // worldspawn's own and a func_group's primitives have no pivot
          if (ref.entity != m_worldspawn)
          {
            markEntity(ref.entity);
            repivoted.insert(ref.entity);
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
        repivoted.insert(entity);
      }
      it = m_pendingEntityDefs.erase(it);
    }
    else
    {
      ++it;
    }
  }
  // LP7.2 (C53, A41): a skin that arrives rebuilds the entities built without it
  for (auto it = m_pendingSkins.begin(); it != m_pendingSkins.end();)
  {
    if (m_table->findSkin(it->first))
    {
      for (const auto entity : it->second)
      {
        markEntity(entity);
      }
      it = m_pendingSkins.erase(it);
    }
    else
    {
      ++it;
    }
  }
  if (!rescanned.empty() || !repivoted.empty())
  {
    dropAdoption([&](const PreviewId entity, const AdoptedIsland& island) {
      return repivoted.contains(entity) || rescanned.contains(lowerCase(island.material));
    });
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
    // LP7.4 (C55): and the cells of its adopted islands
    if (const auto adoption = m_adoptions.find(entity); adoption != m_adoptions.end())
    {
      for (const auto& island : adoption->second->adopted)
      {
        refs.insert(island.cell);
      }
    }
    jobs.emplace_back(refs.begin(), refs.end());
    m_lastBatchCells.insert(refs.begin(), refs.end());
  }

  const auto worldspawn = m_entities.find(m_worldspawn);
  // LP7.4 (C55): with a .proc's islands the jobs return their pieces
  const auto collectPieces = m_proc && m_islands;
  for (auto& refs : jobs)
  {
    auto input = std::make_shared<ChunkJobInput>();
    input->cells = refs;
    input->proc = m_proc;
    input->materials = m_table;
    input->collectPieces = collectPieces;
    // a job's cells are of one chunk entity (a world cell, or every cell of an entity)
    if (const auto adoption = m_adoptions.find(refs.front().entity);
        adoption != m_adoptions.end())
    {
      input->adoptions.emplace(adoption->first, adoption->second);
    }
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
      const auto defSlot = m_table ? m_table->findEntityDef(classname) : nullptr;
      if (defSlot)
      {
        if (defSlot->entry)
        {
          input->entityDefs[classname] = defSlot->entry;
        }
      }
      else if (m_materials)
      {
        // C22: asked now; R3.2: the entity is built again once a table has the answer
        static_cast<void>(m_materials->entityDef(classname));
        m_pendingEntityDefs[classname].insert(id);
      }
      // LP7.2 (C53, D131): a brush entity's skin from its merged keys (a shader key
      // takes the surfaces first, so its skin is not needed); asked now when the table
      // lacks it, the entity built again once a table has it (A41)
      if (equalsNoCase(classname, "worldspawn") || equalsNoCase(classname, "func_group"))
      {
        return;
      }
      const auto merged = mergeSpawnArgs(
        it->second->keys, defSlot && defSlot->entry ? &defSlot->entry->keys : nullptr);
      const auto* shader = findKey(merged, "shader");
      const auto* skin = findKey(merged, "skin");
      if ((shader && !shader->empty()) || !skin || skin->empty())
      {
        return;
      }
      if (const auto slot = m_table ? m_table->findSkin(*skin) : nullptr)
      {
        input->skins[*skin] = slot;
      }
      else if (m_materials)
      {
        static_cast<void>(m_materials->skin(*skin));
        m_pendingSkins[*skin].insert(id);
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
  // LP7.4 (C55): the cells' face pieces, for the pass once the batch has applied
  storePieces(output);
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
    // the table the batch was built with is still the scene's (a batch adopts no other);
    // LP7.2 (C21, C53): the size is the faces' own material's, whatever draws them
    const auto& own = chunkSourceMaterial(chunk);
    auto& sized = m_sizedCells[own];
    if (sized.cells.empty())
    {
      sized.size = tableSizeOf(own);
      // LP5.F (D77, A41): a chunk built at a Default size while its slot is unmeasured
      // waits for the measure (waitsForMaterials); a measured Default is final
      sized.measured = sizeFinalIn(m_table.get(), own, chunk.provisional);
      if (!sized.measured && m_materials)
      {
        // a slot the bulk parse published is measured once looked up (S2.2)
        static_cast<void>(m_materials->lookup(own));
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

  // the union of every chunk of each touched entity, per material (model space); LP7.2
  // (C53, D131): per the faces' own material, the engine's model surface (dmap makes one
  // per material, the remap is the frontend's), whose deform grows its bounds
  auto unions = std::unordered_map<PreviewId, std::map<std::string, EngineBounds>>{};
  for (const auto& [key, chunk] : m_chunks)
  {
    if (touched.contains(key.entity))
    {
      auto& perMaterial = unions[key.entity];
      const auto [it, added] =
        perMaterial.try_emplace(chunkSourceMaterial(chunk), engineClearedBounds());
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

void SceneGeometry::adoptTree()
{
  // the old tree's tables, pieces and groups go (markAll marks the cells of its islands,
  // which hold chunks)
  m_adoptions.clear();
  m_cellPieces.clear();
  m_groupCells.clear();
  m_dirtyGroups.clear();
  m_areaModels.clear();
  m_namedModels.clear();
  // D138: a .bproc's bake has no islands
  m_islands = m_proc && m_bakeSlot && m_bakeSlot->bake && m_bakeSlot->tree == m_proc
                ? m_bakeSlot->bake->islands
                : nullptr;
  if (m_islands && m_islands->models.size() != m_proc->models.size())
  {
    m_islands.reset();
  }
  if (m_islands)
  {
    for (auto m = std::size_t{0}; m < m_proc->models.size(); ++m)
    {
      const auto& model = m_proc->models[m];
      if (model.area >= 0)
      {
        m_areaModels.try_emplace(model.area, m);
      }
      else
      {
        m_namedModels.try_emplace(lowerCase(model.name), m);
      }
    }
  }
  m_adoptionAll = m_islands != nullptr;
  updateAdoptionCounts();
}

void SceneGeometry::storePieces(const ChunkJobOutput& output)
{
  // per (cell, group) the pieces' hash before the job and after it: a group whose pieces
  // in a rebuilt cell differ is evaluated again
  using Key = std::pair<CellRef, AdoptionGroup>;
  auto hashes = std::map<Key, std::array<std::uint64_t, 2>>{};
  constexpr auto Basis = 0xCBF29CE484222325ull;
  for (const auto& ref : output.cells)
  {
    const auto it = m_cellPieces.find(ref);
    if (it == m_cellPieces.end())
    {
      continue;
    }
    for (const auto& piece : it->second)
    {
      auto group = AdoptionGroup{ref.entity, piece.area, lowerCase(piece.material)};
      auto& hash =
        hashes.try_emplace(Key{ref, group}, std::array{Basis, Basis}).first->second[0];
      hash = pieceHash(hash, piece);
      if (const auto cells = m_groupCells.find(group); cells != m_groupCells.end())
      {
        cells->second.erase(ref);
        if (cells->second.empty())
        {
          m_groupCells.erase(cells);
        }
      }
    }
    m_cellPieces.erase(it);
  }
  if (!m_islands)
  {
    return;
  }
  for (const auto& record : output.pieces)
  {
    const auto& ref = record.cell;
    auto group =
      AdoptionGroup{ref.entity, record.piece.area, lowerCase(record.piece.material)};
    auto& hash =
      hashes.try_emplace(Key{ref, group}, std::array{Basis, Basis}).first->second[1];
    hash = pieceHash(hash, record.piece);
    m_groupCells[std::move(group)].insert(ref);
    m_cellPieces[ref].push_back(record.piece);
  }
  for (const auto& [key, hash] : hashes)
  {
    if (hash[0] != hash[1])
    {
      m_dirtyGroups.insert(key.second);
    }
  }
}

void SceneGeometry::markAdoptedIsland(const AdoptedIsland& island)
{
  m_dirty.insert(island.cell);
  for (const auto& key : island.candidates)
  {
    if (const auto cell = m_nodeCells.find(key.node); cell != m_nodeCells.end())
    {
      m_dirty.insert(cell->second);
    }
  }
}

namespace
{

// LP7.4: an adoption table as the jobs read it
std::shared_ptr<const EntityAdoption> publishAdoption(
  std::shared_ptr<const ProcTree> tree,
  std::shared_ptr<const ProcIslands> islands,
  std::vector<AdoptedIsland> adopted)
{
  auto adoption = std::make_shared<EntityAdoption>();
  adoption->tree = std::move(tree);
  adoption->islands = std::move(islands);
  for (const auto& island : adopted)
  {
    adoption->replaced.insert(island.candidates.begin(), island.candidates.end());
  }
  adoption->adopted = std::move(adopted);
  return adoption;
}

} // namespace

template <typename Drop>
void SceneGeometry::dropAdoption(const Drop& drop)
{
  for (auto it = m_adoptions.begin(); it != m_adoptions.end();)
  {
    const auto entity = it->first;
    auto kept = std::vector<AdoptedIsland>{};
    auto dropped = false;
    for (const auto& island : it->second->adopted)
    {
      if (drop(entity, island))
      {
        // its cells are built without it; the pass after that batch tries it again
        markAdoptedIsland(island);
        m_dirtyGroups.insert(
          AdoptionGroup{entity, island.area, lowerCase(island.material)});
        dropped = true;
      }
      else
      {
        kept.push_back(island);
      }
    }
    if (!dropped)
    {
      ++it;
    }
    else if (kept.empty())
    {
      it = m_adoptions.erase(it);
    }
    else
    {
      it->second = publishAdoption(m_proc, m_islands, std::move(kept));
      ++it;
    }
  }
  updateAdoptionCounts();
}

void SceneGeometry::updateAdoption()
{
  if (!m_islands || !m_proc)
  {
    m_dirtyGroups.clear();
    m_adoptionAll = false;
    return;
  }
  auto groups = std::exchange(m_dirtyGroups, {});
  if (std::exchange(m_adoptionAll, false))
  {
    for (const auto& [group, cells] : m_groupCells)
    {
      groups.insert(group);
    }
    for (const auto& [entity, adoption] : m_adoptions)
    {
      for (const auto& island : adoption->adopted)
      {
        groups.insert(AdoptionGroup{entity, island.area, lowerCase(island.material)});
      }
    }
  }
  if (groups.empty())
  {
    return;
  }

  // the model of a group: a world group's area model, a brush entity's own (K17: dmap
  // names it by the entity's name)
  const auto modelOf = [&](const AdoptionGroup& group) -> std::optional<std::size_t> {
    if (group.area >= 0)
    {
      const auto it =
        group.entity == m_worldspawn ? m_areaModels.find(group.area) : m_areaModels.end();
      return it != m_areaModels.end() ? std::optional{it->second} : std::nullopt;
    }
    const auto entity = m_entities.find(group.entity);
    const auto* name =
      entity != m_entities.end() ? findKey(entity->second->keys, "name") : nullptr;
    const auto it = name ? m_namedModels.find(lowerCase(*name)) : m_namedModels.end();
    return it != m_namedModels.end() ? std::optional{it->second} : std::nullopt;
  };
  // an island's cell: its bounds' centre in the entity's editor space (a brush entity's
  // .proc positions are relative to its origin and origin offset, unrotated)
  const auto cellOf = [&](const PreviewId entity, const ProcIsland& island) {
    auto offset = EngineVec3{0, 0, 0};
    if (entity != m_worldspawn)
    {
      if (const auto state = m_entityStates.find(entity); state != m_entityStates.end())
      {
        offset = state->second.modelOrigin;
      }
    }
    auto cell = CellCoord{};
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      const auto centre =
        (double(island.bounds[0][c]) + double(island.bounds[1][c])) * 0.5 + offset[c];
      cell[c] = int(std::floor(centre / CellSize));
    }
    return CellRef{entity, cell};
  };
  // the islands a group adopts now
  const auto evaluate = [&](const AdoptionGroup& group) {
    auto adopted = std::vector<AdoptedIsland>{};
    const auto model = modelOf(group);
    const auto cells = m_groupCells.find(group);
    if (!model || cells == m_groupCells.end())
    {
      return adopted;
    }
    auto pieces = std::vector<const AdoptionPiece*>{};
    for (const auto& ref : cells->second)
    {
      if (const auto it = m_cellPieces.find(ref); it != m_cellPieces.end())
      {
        for (const auto& piece : it->second)
        {
          if (piece.area == group.area && lowerCase(piece.material) == group.material)
          {
            pieces.push_back(&piece);
          }
        }
      }
    }
    const auto& surfaces = m_proc->models[*model].surfaces;
    const auto& islands = m_islands->models[*model].surfaces;
    for (auto s = std::size_t{0}; s < surfaces.size() && s < islands.size(); ++s)
    {
      if (lowerCase(surfaces[s].material) != group.material)
      {
        continue;
      }
      for (auto i = std::size_t{0}; i < islands[s].size(); ++i)
      {
        const auto& island = islands[s][i];
        // a chunk holds an island whole (C55: islands merged under MaxChunkWeldedVerts)
        if (!island.planar || island.verts.size() > MaxChunkWeldedVerts)
        {
          continue;
        }
        const auto match = matchProcIsland(surfaces[s], island, pieces);
        if (match.verdict != IslandVerdict::Adopted)
        {
          continue;
        }
        auto entry = AdoptedIsland{};
        entry.model = *model;
        entry.surface = s;
        entry.island = i;
        entry.cell = cellOf(group.entity, island);
        entry.area = group.area;
        entry.material = pieces[match.candidates.front()]->material;
        entry.triangles = island.indexes.size() / 3;
        for (const auto c : match.candidates)
        {
          entry.candidates.push_back(pieces[c]->key);
        }
        std::ranges::sort(entry.candidates);
        adopted.push_back(std::move(entry));
      }
    }
    return adopted;
  };

  auto entries = std::map<PreviewId, std::vector<AdoptedIsland>>{};
  for (const auto& group : groups)
  {
    if (!entries.contains(group.entity))
    {
      const auto it = m_adoptions.find(group.entity);
      entries[group.entity] =
        it != m_adoptions.end() ? it->second->adopted : std::vector<AdoptedIsland>{};
    }
  }
  auto changed = std::set<PreviewId>{};
  for (const auto& group : groups)
  {
    auto& list = entries.at(group.entity);
    auto before = std::vector<AdoptedIsland>{};
    auto others = std::vector<AdoptedIsland>{};
    for (auto& island : list)
    {
      const auto inGroup =
        island.area == group.area && lowerCase(island.material) == group.material;
      (inGroup ? before : others).push_back(std::move(island));
    }
    auto after = evaluate(group);
    const auto same = std::ranges::equal(before, after, sameAdoptedIsland);
    if (!same)
    {
      for (const auto& island : before)
      {
        markAdoptedIsland(island);
      }
      for (const auto& island : after)
      {
        markAdoptedIsland(island);
      }
      changed.insert(group.entity);
    }
    list = std::move(others);
    list.insert(
      list.end(),
      std::make_move_iterator(after.begin()),
      std::make_move_iterator(after.end()));
  }
  for (const auto entity : changed)
  {
    auto& list = entries.at(entity);
    if (list.empty())
    {
      m_adoptions.erase(entity);
    }
    else
    {
      m_adoptions[entity] = publishAdoption(m_proc, m_islands, std::move(list));
    }
  }
  updateAdoptionCounts();
}

void SceneGeometry::updateAdoptionCounts()
{
  auto counts = AdoptionCounts{};
  if (m_islands && m_proc)
  {
    for (const auto& [entity, adoption] : m_adoptions)
    {
      counts.adoptedIslands += adoption->adopted.size();
      for (const auto& island : adoption->adopted)
      {
        counts.adoptedTris += island.triangles;
      }
    }
    // the models the scene draws from: every area model with a worldspawn, each brush
    // entity's own while it has pieces
    auto models = std::set<std::size_t>{};
    if (m_worldspawn != NoPreviewId)
    {
      for (const auto& [area, m] : m_areaModels)
      {
        models.insert(m);
      }
    }
    for (const auto& [group, cells] : m_groupCells)
    {
      if (group.area >= 0)
      {
        continue;
      }
      const auto entity = m_entities.find(group.entity);
      const auto* name =
        entity != m_entities.end() ? findKey(entity->second->keys, "name") : nullptr;
      if (const auto it =
            name ? m_namedModels.find(lowerCase(*name)) : m_namedModels.end();
          it != m_namedModels.end())
      {
        models.insert(it->second);
      }
    }
    auto planar = std::uint64_t{0};
    for (const auto m : models)
    {
      planar += m_islands->models[m].planarIslands;
      counts.curvedIslands += m_islands->models[m].curvedIslands;
    }
    counts.unmatchedIslands =
      planar > counts.adoptedIslands ? planar - counts.adoptedIslands : 0;
  }
  m_adoptionCounts = counts;
}

SceneGeometry::AdoptionCounts SceneGeometry::adoptionCounts() const
{
  return m_adoptionCounts;
}

std::shared_ptr<const EntityAdoption> SceneGeometry::adoptionOf(
  const PreviewId entity) const
{
  const auto it = m_adoptions.find(entity);
  return it != m_adoptions.end() ? it->second : nullptr;
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
    // LP7.4 (C55, P7.11): beside them, the adoption of the groups the batch changed; the
    // cells of an island whose adoption changed are built by the batch started below
    updateAdoption();
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

std::shared_ptr<const PortalFogTable> SceneGeometry::portalFogTable() const
{
  if (!m_areas)
  {
    m_portalFog.reset();
    m_portalFogAreas.reset();
    return m_portalFog;
  }
  const auto lights = m_lights.table();
  if (
    !m_portalFog || m_portalFogAreas != m_areas
    || m_portalFog->lightsVersion != lights->version)
  {
    m_portalFogAreas = m_areas;
    m_portalFog = std::make_shared<const PortalFogTable>(
      buildPortalFogTable(*lights, *m_areas, lights->version));
  }
  return m_portalFog;
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
