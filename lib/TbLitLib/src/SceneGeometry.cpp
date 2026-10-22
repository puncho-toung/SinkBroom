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

} // namespace

SceneGeometry::SceneGeometry(CpuPool* pool, MaterialService* materials)
  : m_pool{pool}
  , m_materials{materials}
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
  return m_jobs.empty();
}

bool SceneGeometry::busy() const
{
  return !m_jobs.empty();
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
  for (const auto& [id, ref] : m_nodeCells)
  {
    const auto node = m_nodes.find(id);
    if (node != m_nodes.end() && node->second->entity == entity)
    {
      m_dirty.insert(ref);
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

void SceneGeometry::takeSnapshot(const Snapshot& snapshot)
{
  m_batchSequence = snapshot.sequence;

  // B0.4: removed holds node and entity ids
  for (const auto id : snapshot.removed)
  {
    if (const auto cell = m_nodeCells.find(id); cell != m_nodeCells.end())
    {
      m_dirty.insert(cell->second);
      m_cellNodes[cell->second].erase(id);
      m_nodeCells.erase(cell);
    }
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
    const auto changed = old == m_entities.end() || old->second->keys != entity->keys
                         || old->second->classname != entity->classname;
    m_entities[id] = std::move(entity);
    if (isWorld)
    {
      m_worldspawn = id;
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
    if (const auto old = m_nodeCells.find(id); old != m_nodeCells.end())
    {
      m_dirty.insert(old->second);
      m_cellNodes[old->second].erase(id);
      m_nodeCells.erase(old);
    }
    if (isPrimitive(*node))
    {
      const auto ref = CellRef{chunkEntityOf(*node), cellOfNode(*node)};
      m_nodeCells[id] = ref;
      m_cellNodes[ref].insert(id);
      m_dirty.insert(ref);
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

  startBatch();
}

void SceneGeometry::refreshTable()
{
  if (!m_materials)
  {
    return;
  }
  m_table = m_materials->table();
  for (const auto& [id, entity] : m_entities)
  {
    static_cast<void>(m_materials->entityDef(entity->classname));
  }
}

void SceneGeometry::startBatch()
{
  if (m_dirty.empty() || !m_jobs.empty())
  {
    return;
  }
  refreshTable();
  if (m_table)
  {
    m_lastTableVersion = m_table->version;
  }

  // one job per world cell, one per brush entity with all its cells
  auto jobs = std::map<PreviewId, std::vector<CellRef>>{};
  auto worldJobs = std::vector<std::vector<CellRef>>{};
  for (const auto& ref : m_dirty)
  {
    if (ref.entity == m_worldspawn)
    {
      worldJobs.push_back({ref});
    }
    else
    {
      jobs[ref.entity].push_back(ref);
    }
  }
  for (auto& [entity, refs] : jobs)
  {
    worldJobs.push_back(std::move(refs));
  }
  m_dirty.clear();

  const auto worldspawn = m_entities.find(m_worldspawn);
  for (auto& refs : worldJobs)
  {
    auto input = std::make_shared<ChunkJobInput>();
    input->cells = refs;
    input->proc = m_proc;
    input->materials = m_table;
    if (worldspawn != m_entities.end())
    {
      input->worldspawn = worldspawn->second;
      input->entities[m_worldspawn] = worldspawn->second;
    }
    const auto addEntity = [&](const PreviewId id) {
      const auto it = m_entities.find(id);
      if (it == m_entities.end())
      {
        return;
      }
      input->entities[id] = it->second;
      if (m_table)
      {
        if (const auto slot = m_table->findEntityDef(it->second->classname);
            slot && slot->entry)
        {
          input->entityDefs[it->second->classname] = slot->entry;
        }
      }
    };
    for (const auto& ref : refs)
    {
      for (const auto id : m_cellNodes[ref])
      {
        const auto& node = m_nodes.at(id);
        input->nodes.push_back(node);
        addEntity(node->entity);
      }
    }
    // a brush entity's origin brushes may lie in other cells
    if (refs.front().entity != m_worldspawn)
    {
      const auto entity = refs.front().entity;
      for (const auto& [id, node] : m_nodes)
      {
        if (node->entity == entity && !m_cellNodes[refs.front()].contains(id))
        {
          input->nodes.push_back(node);
        }
      }
    }

    auto job = [input](const CpuJobContext& context) -> Result<ChunkJobOutput> {
      if (context.stale())
      {
        return Error{"The chunk job was abandoned"};
      }
      return buildChunks(*input);
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
    for (auto& [material, cells] : m_provisional)
    {
      cells.erase(ref);
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
    for (const auto& ref : output.cells)
    {
      m_provisional[material].insert(ref);
    }
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

  // between snapshots: what a newer table or the .proc made stale
  if (m_materials)
  {
    const auto table = m_materials->table();
    if (table && table->version != m_lastTableVersion)
    {
      m_lastTableVersion = table->version;
      if (m_mapPath && !m_proc)
      {
        if (const auto slot = table->findProcFile(procPathOf(*m_mapPath));
            slot && slot->tree)
        {
          m_proc = slot->tree;
          markAll();
        }
      }
      for (auto it = m_provisional.begin(); it != m_provisional.end();)
      {
        const auto slot = table->find(it->first);
        if (
          slot && slot->entry
          && slot->entry->editorImageSize.state == EditorImageSize::State::Ready)
        {
          m_dirty.insert(it->second.begin(), it->second.end());
          it = m_provisional.erase(it);
        }
        else if (slot && slot->entry && (!m_table || !m_table->find(it->first)))
        {
          // a material that was pending is known now
          m_dirty.insert(it->second.begin(), it->second.end());
          ++it;
        }
        else
        {
          ++it;
        }
      }
      startBatch();
    }
  }
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

std::vector<std::string> SceneGeometry::takeWarnings()
{
  return std::exchange(m_warnings, {});
}

void SceneGeometry::finishForTesting()
{
  for (auto& job : m_jobs)
  {
    job.wait();
  }
  collect();
}

} // namespace tb::lit
