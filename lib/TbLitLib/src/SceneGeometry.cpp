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

#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

namespace tb::lit
{
namespace
{

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

// the .proc beside a map file (C10's search of the game folder is LP1c.1's)
std::string procPathOf(const std::string& mapPath)
{
  return std::filesystem::path{mapPath}.replace_extension(".proc").string();
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
  std::shared_ptr<LightTablePublication> lights)
  : m_pool{pool}
  , m_materials{materials}
  , m_lights{pool, materials, std::move(lights)}
{
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

  if (snapshot.mapPath && snapshot.mapPath != m_mapPath)
  {
    m_mapPath = snapshot.mapPath;
    m_proc.reset();
    if (m_materials)
    {
      m_materials->loadProcFile(procPathOf(*m_mapPath));
    }
    markAll();
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
  m_table = std::move(table);
  scanTable();
}

void SceneGeometry::scanTable()
{
  // R3.1: what the cells wait for is looked for in every table the scene adopts, also
  // one that a batch for other cells adopts first
  if (m_mapPath && !m_proc)
  {
    if (const auto slot = m_table->findProcFile(procPathOf(*m_mapPath));
        slot && slot->tree)
    {
      m_proc = slot->tree;
      m_lights.setProcTree(m_proc);
      markAll();
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
  for (auto it = m_provisional.begin(); it != m_provisional.end();)
  {
    const auto slot = m_table->find(it->first);
    if (
      slot && slot->entry
      && slot->entry->editorImageSize.state == EditorImageSize::State::Ready)
    {
      m_dirty.insert(it->second.begin(), it->second.end());
      it = m_provisional.erase(it);
    }
    else
    {
      ++it;
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
  forgetCells(m_provisional, output.cells);
  forgetCells(m_pendingMaterials, output.cells);
  for (const auto& ref : output.cells)
  {
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
    if (chunk.provisional)
    {
      m_provisional[chunk.key.material].insert(ref);
    }
    auto key = chunk.key;
    m_chunks.insert_or_assign(std::move(key), std::move(chunk));
  }
  for (const auto& material : output.pendingMaterials)
  {
    m_pendingMaterials[material].insert(output.cells.begin(), output.cells.end());
  }
  if (!output.cells.empty())
  {
    m_solidPieces[output.cells.front()] += output.solidPieces;
  }
  for (auto& state : output.entities)
  {
    const auto id = state.id;
    m_entityStates.insert_or_assign(id, std::move(state));
  }
  for (auto& warning : output.warnings)
  {
    m_warnings.push_back(std::move(warning));
  }
}

bool SceneGeometry::collect()
{
  const auto lightsApplied = m_lights.collect();
  const auto chunksApplied = collectChunks();
  return chunksApplied || lightsApplied;
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
    m_appliedSequence = m_batchSequence;
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

std::uint64_t SceneGeometry::appliedSequence() const
{
  return m_appliedSequence;
}

std::shared_ptr<const ProcTree> SceneGeometry::procTree() const
{
  return m_proc;
}

std::shared_ptr<const LightTable> SceneGeometry::lightTable() const
{
  return m_lights.table();
}

std::vector<std::string> SceneGeometry::takeWarnings()
{
  auto warnings = std::exchange(m_warnings, {});
  for (auto& warning : m_lights.takeWarnings())
  {
    warnings.push_back(std::move(warning));
  }
  return warnings;
}

void SceneGeometry::finishForTesting()
{
  for (auto& job : m_jobs)
  {
    job.wait();
  }
  collect();
  m_lights.finishForTesting();
}

} // namespace tb::lit
