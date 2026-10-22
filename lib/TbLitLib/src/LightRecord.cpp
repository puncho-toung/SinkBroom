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

#include "lit/LightRecord.h"

#include "lit/CpuPool.h"
#include "lit/EntityState.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

namespace tb::lit
{
namespace
{

EntityKeys keysOf(const Snapshot& snapshot, const SnapshotEntity& entity)
{
  auto keys = EntityKeys{};
  keys.reserve(entity.keys.count);
  for (auto i = entity.keys.first; i < entity.keys.first + entity.keys.count; ++i)
  {
    const auto& keyValue = snapshot.keyValues[i];
    keys.emplace_back(snapshot.string(keyValue.key), snapshot.string(keyValue.value));
  }
  return keys;
}

bool isWorldspawn(const std::string_view classname)
{
  constexpr auto worldspawn = std::string_view{"worldspawn"};
  return classname.size() == worldspawn.size()
         && std::equal(
           classname.begin(),
           classname.end(),
           worldspawn.begin(),
           [](const char a, const char b) {
             return (a >= 'A' && a <= 'Z' ? char(a - 'A' + 'a') : a) == b;
           });
}

void eraseFromAll(std::map<std::string, std::set<PreviewId>>& waiting, const PreviewId id)
{
  for (auto it = waiting.begin(); it != waiting.end();)
  {
    it->second.erase(id);
    it = it->second.empty() ? waiting.erase(it) : std::next(it);
  }
}

} // namespace

std::shared_ptr<const LightRecord> LightTable::find(const PreviewId id) const
{
  const auto* record = records.find(id);
  return record ? *record : nullptr;
}

LightJobOutput buildLightRecords(const LightJobInput& input, const CpuJobContext* context)
{
  auto output = LightJobOutput{};
  const auto* table = input.materials.get();
  const auto* tree = input.proc.get();
  const auto common = tree ? commonChildrenAreas(*tree) : std::vector<int>{};
  const auto removedClasses = removedClassnames(input.worldspawn);

  for (const auto& in : input.lights)
  {
    // R2.1: a cancelled pool's job stops here; its output is dropped
    if (context && context->stale())
    {
      break;
    }
    // R3.12: MapFile.cpp:2270-2282 deletes the entities of a removed class
    if (isRemovedClassname(removedClasses, in.classname))
    {
      output.removed.emplace_back(in.id, in.version);
      continue;
    }
    auto record = LightRecord{};
    record.id = in.id;
    record.version = in.version;
    record.classname = in.classname;
    record.visible = in.visible;

    // C22, A10: the entityDef's merged keys fill what the map's leave out
    const auto slot = table ? table->findEntityDef(in.classname) : nullptr;
    record.pendingEntityDef = !slot;
    const auto* defKeys = slot && slot->entry ? &slot->entry->keys : nullptr;
    record.spawnArgs = mergeSpawnArgs(in.mapKeys, defKeys);

    record.light = parseLight(record.spawnArgs);
    record.derived = deriveLight(record.light, table, tree, tree ? &common : nullptr);
    if (record.derived.shader.material)
    {
      record.material = MaterialId::of(*record.derived.shader.material);
    }
    if (record.derived.shader.pending)
    {
      for (auto& name : lightMaterialNames(record.light))
      {
        if (!table || !table->find(name))
        {
          record.pendingMaterials.push_back(std::move(name));
        }
      }
    }
    record.provisional = record.pendingEntityDef || record.derived.shader.pending;

    // the parse's warnings depend on the merged keys: said once the entityDef is known
    if (!record.pendingEntityDef)
    {
      const auto* name = findKey(record.spawnArgs, "name");
      const auto who =
        name ? fmt::format("light '{}'", *name) : fmt::format("light {}", record.id);
      for (const auto* warnings : {&record.light.warnings, &record.derived.warnings})
      {
        for (const auto& warning : *warnings)
        {
          output.warnings.push_back(fmt::format("{}: {}", who, warning));
        }
      }
    }
    output.records.push_back(std::move(record));
  }
  return output;
}

LightTablePublication::LightTablePublication()
  : m_table{std::make_shared<const LightTable>()}
{
}

std::shared_ptr<const LightTable> LightTablePublication::table() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_table;
}

void LightTablePublication::publish(std::shared_ptr<const LightTable> table)
{
  auto previous = std::shared_ptr<const LightTable>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    previous = std::exchange(m_table, std::move(table));
  }
  // the table replaced is freed outside the lock
}

LightRegistry::LightRegistry(
  CpuPool* pool,
  MaterialService* materials,
  std::shared_ptr<LightTablePublication> publication)
  : m_pool{pool}
  , m_materials{materials}
  , m_publication{
      publication ? std::move(publication) : std::make_shared<LightTablePublication>()}
{
  // the default lights every light may fall back to (RenderSystem_init.cpp:1784-1785)
  if (m_materials)
  {
    static_cast<void>(m_materials->lookup(DefaultPointLightMaterial));
    static_cast<void>(m_materials->lookup(DefaultProjectedLightMaterial));
  }
}

LightRegistry::~LightRegistry()
{
  // R5.22: the future of a job in flight is only waited for
  if (m_job && m_job->valid())
  {
    m_job->wait();
  }
}

bool LightRegistry::readyForSnapshot() const
{
  return !m_job;
}

bool LightRegistry::busy() const
{
  return m_job.has_value();
}

void LightRegistry::takeSnapshot(
  const Snapshot& snapshot, std::shared_ptr<const ProcTree> proc)
{
  if (proc != m_proc)
  {
    m_proc = std::move(proc);
    for (const auto& [id, light] : m_lights)
    {
      m_dirty.insert(id);
    }
  }

  // B0.4: removed holds node and entity ids
  for (const auto id : snapshot.removed)
  {
    if (m_lights.erase(id) > 0)
    {
      m_removed.insert(id);
      m_dirty.erase(id);
    }
    if (id == m_worldspawn)
    {
      m_worldspawn = NoPreviewId;
      m_worldspawnKeys.clear();
      for (const auto& [light, state] : m_lights)
      {
        m_dirty.insert(light);
      }
    }
  }

  auto entities = std::unordered_map<PreviewId, const SnapshotEntity*>{};
  for (const auto& entity : snapshot.entities)
  {
    entities[entity.id] = &entity;
    // R3.12: worldspawn's removeEntities* keys decide which lights exist
    if (isWorldspawn(snapshot.string(entity.classname)))
    {
      auto keys = keysOf(snapshot, entity);
      if (entity.id != m_worldspawn || keys != m_worldspawnKeys)
      {
        m_worldspawn = entity.id;
        m_worldspawnKeys = std::move(keys);
        for (const auto& [id, light] : m_lights)
        {
          m_dirty.insert(id);
        }
      }
    }
  }

  auto changed = std::set<PreviewId>{};
  for (const auto& node : snapshot.nodes)
  {
    // a light's node is its entity's own
    const auto id = node.entity;
    if (node.kind != SnapshotNodeKind::Light)
    {
      // a light whose classname is no longer a light's
      if (node.kind == SnapshotNodeKind::PointEntity && m_lights.erase(id) > 0)
      {
        m_removed.insert(id);
        m_dirty.erase(id);
      }
      continue;
    }
    auto& light = m_lights[id];
    light.visible = node.visible;
    if (const auto entity = entities.find(id); entity != entities.end())
    {
      light.classname = std::string{snapshot.string(entity->second->classname)};
      light.mapKeys = keysOf(snapshot, *entity->second);
    }
    m_removed.erase(id);
    changed.insert(id);
  }

  // B0.4: an entity record without its node: a light's keys alone changed
  for (const auto& entity : snapshot.entities)
  {
    if (const auto light = m_lights.find(entity.id);
        light != m_lights.end() && !changed.contains(entity.id))
    {
      light->second.classname = std::string{snapshot.string(entity.classname)};
      light->second.mapKeys = keysOf(snapshot, entity);
      changed.insert(entity.id);
    }
  }

  for (const auto id : changed)
  {
    m_dirty.insert(id);
    // C22: asked now, so that a later table has it
    if (m_materials)
    {
      static_cast<void>(m_materials->entityDef(m_lights.at(id).classname));
    }
  }
  startBatch();
}

void LightRegistry::setProcTree(std::shared_ptr<const ProcTree> proc)
{
  if (proc == m_proc)
  {
    return;
  }
  m_proc = std::move(proc);
  for (const auto& [id, light] : m_lights)
  {
    m_dirty.insert(id);
  }
  startBatch();
}

void LightRegistry::publish()
{
  auto table = std::make_shared<LightTable>();
  table->version = ++m_tableVersion;
  table->records = m_records;
  m_publication->publish(std::move(table));
}

void LightRegistry::startBatch()
{
  if (m_job)
  {
    return;
  }
  if (!m_removed.empty())
  {
    for (const auto id : m_removed)
    {
      m_records = m_records.erase(id);
      eraseFromAll(m_waitingMaterials, id);
      eraseFromAll(m_waitingEntityDefs, id);
    }
    m_removed.clear();
    publish();
  }
  refreshTable();
  if (m_dirty.empty())
  {
    return;
  }

  auto input = std::make_shared<LightJobInput>();
  input->materials = m_table;
  input->proc = m_proc;
  input->worldspawn = m_worldspawnKeys;
  for (const auto id : m_dirty)
  {
    auto& light = m_lights.at(id);
    ++light.version;
    input->lights.push_back(
      {id, light.version, light.classname, light.mapKeys, light.visible});
    eraseFromAll(m_waitingMaterials, id);
    eraseFromAll(m_waitingEntityDefs, id);
  }
  m_dirty.clear();

  auto job = [input](const CpuJobContext& context) -> Result<LightJobOutput> {
    // R2.1: the job of a cancelled pool returns at its next light
    auto output = buildLightRecords(*input, &context);
    if (context.stale())
    {
      return Error{"The light job was abandoned"};
    }
    return output;
  };
  if (m_pool && !m_pool->stopped())
  {
    m_job = m_pool->run(std::move(job));
  }
  else
  {
    // no pool: the job runs here, and its messages are drained here
    auto promise = std::promise<CpuJobResult<LightJobOutput>>{};
    m_job = promise.get_future();
    auto output = CpuJobResult<LightJobOutput>{buildLightRecords(*input), {}};
    output.messages = drainEngineMessagesForThread(std::this_thread::get_id());
    promise.set_value(std::move(output));
  }
}

void LightRegistry::refreshTable()
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
  // V.2: a reload's new registry: every record holds the old registry's MaterialId and
  // entityDef keys, so every light is built again
  const auto reloaded =
    m_table && m_table->registry != 0 && table->registry != m_table->registry;
  m_table = std::move(table);
  if (reloaded)
  {
    m_waitingMaterials.clear();
    m_waitingEntityDefs.clear();
    for (const auto& [id, light] : m_lights)
    {
      m_dirty.insert(id);
    }
  }
  // R3.1: what the lights wait for is looked for in every table the registry adopts
  for (auto it = m_waitingMaterials.begin(); it != m_waitingMaterials.end();)
  {
    if (m_table->find(it->first))
    {
      m_dirty.insert(it->second.begin(), it->second.end());
      it = m_waitingMaterials.erase(it);
    }
    else
    {
      ++it;
    }
  }
  for (auto it = m_waitingEntityDefs.begin(); it != m_waitingEntityDefs.end();)
  {
    if (m_table->findEntityDef(it->first))
    {
      m_dirty.insert(it->second.begin(), it->second.end());
      it = m_waitingEntityDefs.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void LightRegistry::apply(LightJobOutput output)
{
  for (const auto& [id, version] : output.removed)
  {
    // R3.12: a removed class has no record; a newer build of the light is queued when
    // it changed again
    const auto light = m_lights.find(id);
    if (light != m_lights.end() && light->second.version == version)
    {
      m_records = m_records.erase(id);
    }
  }
  for (auto& record : output.records)
  {
    // a light removed, or changed again, while the job ran: its record is dropped (a
    // newer build is queued)
    const auto light = m_lights.find(record.id);
    if (light == m_lights.end() || light->second.version != record.version)
    {
      continue;
    }
    if (record.pendingEntityDef)
    {
      m_waitingEntityDefs[record.classname].insert(record.id);
    }
    for (const auto& name : record.pendingMaterials)
    {
      m_waitingMaterials[name].insert(record.id);
      if (m_materials)
      {
        static_cast<void>(m_materials->lookup(name));
      }
    }
    const auto id = record.id;
    m_records = m_records.set(id, std::make_shared<const LightRecord>(std::move(record)));
  }
  for (auto& warning : output.warnings)
  {
    m_warnings.push_back(std::move(warning));
  }
}

bool LightRegistry::collect()
{
  if (m_job)
  {
    if (m_job->wait_for(std::chrono::seconds{0}) != std::future_status::ready)
    {
      return false;
    }
    auto job = std::move(*m_job);
    m_job.reset();
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
        | kdl::transform([&](LightJobOutput output) { apply(std::move(output)); })
        | kdl::transform_error([&](auto e) { m_warnings.push_back(std::move(e.msg)); });
    }
    catch (const std::exception& e)
    {
      m_warnings.push_back(fmt::format("A light job failed: {}", e.what()));
    }
    publish();
    startBatch();
    return true;
  }

  // between jobs: a newer material table may bring what lights wait for (startBatch
  // adopts and scans it)
  startBatch();
  return false;
}

std::shared_ptr<const LightTable> LightRegistry::table() const
{
  return m_publication->table();
}

std::vector<std::string> LightRegistry::takeWarnings()
{
  return std::exchange(m_warnings, {});
}

void LightRegistry::finishForTesting()
{
  while (m_job)
  {
    m_job->wait();
    collect();
  }
}

} // namespace tb::lit
