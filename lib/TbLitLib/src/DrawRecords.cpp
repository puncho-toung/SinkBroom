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

#include "DrawRecords.h"

#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>

namespace tb::lit
{
namespace
{

void combine(std::size_t& seed, const std::size_t value)
{
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

// a surface's prepass record (an opaque one's), and its ambient records once made;
// LP6.5a: their gbuffer records once pass 3 drew them
std::size_t recordCount(const SurfaceRecords& group)
{
  return (group.depth == DepthClass::Opaque ? 1 : 0)
         + (group.ambientFilled ? group.ambient.size() : 0)
         + (group.ambientFilled && group.gbufferFilled ? group.ambient.size() : 0);
}

namespace em = engine_material;

// LP6.1 (D167): FillDepthBufferFast's and AmbientPass' per-surface decisions
void decide(SurfaceRecords& group)
{
  const auto* material = group.material.get();
  group.subview = material && material->sort == em::SortSubview;
  group.depth = !material                                       ? DepthClass::Opaque
                : material->coverage == em::CoverageTranslucent ? DepthClass::Translucent
                : material->coverage == em::CoveragePerforated  ? DepthClass::Perforated
                                                                : DepthClass::Opaque;
  group.ambientDrawn =
    material && group.registers && group.depth != DepthClass::Translucent
    && std::ranges::any_of(material->stages, [&](const MaterialStage& stage) {
         return engineRegister(group.registers->registers, stage.conditionRegister)
                != 0.0f;
       });
}

std::size_t recordCount(const InteractionRecords& group)
{
  return group.filled ? group.draws.size() : 0;
}

} // namespace

RecordedInteraction RecordedInteraction::of(const InteractionDraw& draw)
{
  auto recorded = RecordedInteraction{};
  recorded.images = {draw.bumpImage, draw.specularImage, draw.diffuseImage};
  recorded.bumpMatrix = draw.bumpMatrix;
  recorded.diffuseMatrix = draw.diffuseMatrix;
  recorded.specularMatrix = draw.specularMatrix;
  recorded.diffuseColor = draw.diffuseColor;
  recorded.specularColor = draw.specularColor;
  recorded.vertexColor = draw.vertexColor;
  recorded.inherited = draw.inherited;
  return recorded;
}

SurfaceRecordKey SurfaceRecordKey::of(const LitSurface& surface)
{
  return SurfaceRecordKey{
    surface.chunk,
    surface.instance,
    surface.material.get(),
    surface.registers.get(),
    surface.provisional,
    surface.gridArea};
}

std::size_t SurfaceRecordKeyHash::operator()(const SurfaceRecordKey& key) const
{
  auto seed = std::hash<const void*>{}(key.chunk);
  combine(seed, std::hash<const void*>{}(key.instance));
  combine(seed, std::hash<const void*>{}(key.material));
  combine(seed, std::hash<const void*>{}(key.registers));
  combine(seed, std::size_t(key.provisional));
  combine(seed, key.gridArea ? std::hash<int>{}(*key.gridArea) : ~std::size_t{0});
  return seed;
}

std::size_t InteractionRecordKeyHash::operator()(const InteractionRecordKey& key) const
{
  auto seed = std::hash<std::uint64_t>{}(key.light);
  combine(seed, key.stage);
  combine(seed, std::size_t(key.list));
  combine(seed, std::hash<const void*>{}(key.chunk));
  combine(seed, std::hash<const void*>{}(key.instance));
  combine(seed, std::hash<const void*>{}(key.material));
  combine(seed, std::hash<const void*>{}(key.registers));
  combine(seed, std::size_t(key.provisional));
  return seed;
}

DrawRecordTable::Cleared DrawRecordTable::beginBuild(const Lifetime& lifetime)
{
  ++m_build;
  auto cleared = Cleared{};
  if (m_lifetime)
  {
    const auto& last = *m_lifetime;
    const auto scene = std::tie(
                         last.chunks,
                         last.chunksVersion,
                         last.instances,
                         last.entities,
                         last.tree,
                         last.areas,
                         last.portalFog,
                         last.table)
                       != std::tie(
                         lifetime.chunks,
                         lifetime.chunksVersion,
                         lifetime.instances,
                         lifetime.entities,
                         lifetime.tree,
                         lifetime.areas,
                         lifetime.portalFog,
                         lifetime.table);
    const auto lights =
      last.lights != lifetime.lights || last.lightsVersion != lifetime.lightsVersion;
    const auto grid = last.grid != lifetime.grid
                      || last.gridVersion != lifetime.gridVersion
                      || last.gridValidity != lifetime.gridValidity;
    if (scene || (lights && grid))
    {
      clear();
      cleared = Cleared{true, true};
    }
    else if (lights)
    {
      clearInteractions();
      cleared.interactions = true;
    }
    else if (grid)
    {
      clearSurfaces();
      cleared.surfaces = true;
    }
  }
  m_lifetime = lifetime;
  return cleared;
}

void DrawRecordTable::endBuild()
{
  sweep();
}

bool DrawRecordTable::generations(
  const std::uint64_t renderTargets, const std::uint64_t arenas)
{
  if (!m_generationsSeen)
  {
    m_generationsSeen = true;
    m_renderTargetGeneration = renderTargets;
    m_arenaGeneration = arenas;
    return false;
  }
  if (renderTargets == m_renderTargetGeneration && arenas == m_arenaGeneration)
  {
    return false;
  }
  m_renderTargetGeneration = renderTargets;
  m_arenaGeneration = arenas;
  clear();
  return true;
}

SurfaceRecords& DrawRecordTable::surface(const LitSurface& surface, bool& made)
{
  const auto [it, inserted] = m_surfaces.try_emplace(SurfaceRecordKey::of(surface));
  if (inserted)
  {
    it->second = std::make_unique<SurfaceRecords>();
    it->second->material = surface.material;
    it->second->registers = surface.registers;
    decide(*it->second);
    m_records += recordCount(*it->second);
  }
  made = inserted;
  it->second->lastLinked = m_build;
  return *it->second;
}

InteractionRecords& DrawRecordTable::interaction(
  const InteractionRecordKey& key, const LitSurface& surface, bool& made)
{
  const auto [it, inserted] = m_interactions.try_emplace(key);
  if (inserted)
  {
    it->second = std::make_unique<InteractionRecords>();
    it->second->material = surface.material;
    it->second->registers = surface.registers;
  }
  made = inserted;
  it->second->lastLinked = m_build;
  return *it->second;
}

std::uint64_t DrawRecordTable::lightSerial(const ViewLight& light)
{
  auto& entry = m_lights[light.record.get()];
  if (!entry.record || entry.shader != light.shader || entry.registers != light.registers)
  {
    entry.record = light.record;
    entry.shader = light.shader;
    entry.registers = light.registers;
    entry.serial = ++m_lightSerial;
  }
  entry.lastLinked = m_build;
  return entry.serial;
}

void DrawRecordTable::ambientFilled(SurfaceRecords& group)
{
  group.ambientFilled = true;
  m_records += group.ambient.size();
}

void DrawRecordTable::filled(InteractionRecords& group)
{
  group.filled = true;
  m_records += group.draws.size();
}

void DrawRecordTable::gbufferFilled(SurfaceRecords& group)
{
  if (group.ambientFilled && !group.gbufferFilled)
  {
    group.gbufferFilled = true;
    m_records += group.ambient.size();
  }
}

void DrawRecordTable::resolved(DrawRecord& record)
{
  for (const auto& version : record.slots)
  {
    m_bySlot[version.slot].insert(&record);
  }
}

void DrawRecordTable::release(DrawRecord& record)
{
  for (auto& entry : record.entries)
  {
    if (entry)
    {
      BindingSetCache::release(*entry);
      entry = nullptr;
    }
  }
  record.sets = {};
  for (const auto& version : record.slots)
  {
    if (const auto it = m_bySlot.find(version.slot); it != m_bySlot.end())
    {
      it->second.erase(&record);
      if (it->second.empty())
      {
        m_bySlot.erase(it);
      }
    }
  }
  record.slots.clear();
  record.state = DrawRecord::State::Unresolved;
}

std::size_t DrawRecordTable::slotChanged(const ImageSlotId slot)
{
  const auto it = m_bySlot.find(slot);
  if (it == m_bySlot.end())
  {
    return 0;
  }
  // copied: release edits the index
  const auto records = std::vector<DrawRecord*>{it->second.begin(), it->second.end()};
  for (auto* record : records)
  {
    release(*record);
  }
  return records.size();
}

void DrawRecordTable::stamp(DrawRecord& record, const std::uint64_t frame)
{
  if (record.stamped == frame)
  {
    return;
  }
  record.stamped = frame;
  for (std::size_t i = 0; i < record.setCount && i < record.entries.size(); ++i)
  {
    if (auto* entry = record.entries[i]; entry && entry->lastUsed != frame)
    {
      entry->lastUsed = frame;
    }
  }
}

void DrawRecordTable::clear()
{
  clearSurfaces();
  clearInteractions();
}

void DrawRecordTable::clearSurfaces()
{
  for (auto& [key, group] : m_surfaces)
  {
    releaseGroup(*group);
  }
  m_surfaces.clear();
}

void DrawRecordTable::clearInteractions()
{
  for (auto& [key, group] : m_interactions)
  {
    releaseGroup(*group);
  }
  m_interactions.clear();
  m_lights.clear();
}

std::size_t DrawRecordTable::records() const
{
  return m_records;
}

std::size_t DrawRecordTable::groups() const
{
  return m_surfaces.size() + m_interactions.size();
}

std::size_t DrawRecordTable::recordsOf(const ImageSlotId slot) const
{
  const auto it = m_bySlot.find(slot);
  return it != m_bySlot.end() ? it->second.size() : 0;
}

std::uint64_t DrawRecordTable::build() const
{
  return m_build;
}

std::uint64_t DrawRecordTable::renderTargetGeneration() const
{
  return m_renderTargetGeneration;
}

std::uint64_t DrawRecordTable::arenaGeneration() const
{
  return m_arenaGeneration;
}

void DrawRecordTable::setCapacityForTesting(const std::size_t capacity)
{
  m_capacity = capacity;
}

void DrawRecordTable::releaseGroup(SurfaceRecords& group)
{
  release(group.prepass);
  for (auto& ambient : group.ambient)
  {
    release(ambient.record);
    release(ambient.gbuffer);
  }
  m_records -= std::min(m_records, recordCount(group));
}

void DrawRecordTable::releaseGroup(InteractionRecords& group)
{
  for (auto& draw : group.draws)
  {
    for (auto& variant : draw.variants)
    {
      release(variant);
    }
  }
  m_records -= std::min(m_records, recordCount(group));
}

void DrawRecordTable::sweep()
{
  // a group no build linked in the last SweepAge builds goes, checked every SweepEvery
  // builds
  if (m_build % SweepEvery == 0 && m_build > SweepAge)
  {
    const auto oldest = m_build - SweepAge;
    std::erase_if(m_surfaces, [&](auto& entry) {
      if (entry.second->lastLinked > oldest)
      {
        return false;
      }
      releaseGroup(*entry.second);
      return true;
    });
    std::erase_if(m_interactions, [&](auto& entry) {
      if (entry.second->lastLinked > oldest)
      {
        return false;
      }
      releaseGroup(*entry.second);
      return true;
    });
    std::erase_if(
      m_lights, [&](const auto& entry) { return entry.second.lastLinked <= oldest; });
  }
  if (m_records <= m_capacity)
  {
    return;
  }
  // past the capacity: the least recently linked groups first, never this build's
  struct Candidate
  {
    std::uint64_t lastLinked = 0;
    const SurfaceRecordKey* surface = nullptr;
    const InteractionRecordKey* interaction = nullptr;
  };
  auto candidates = std::vector<Candidate>{};
  for (const auto& [key, group] : m_surfaces)
  {
    if (group->lastLinked < m_build)
    {
      candidates.push_back(Candidate{group->lastLinked, &key, nullptr});
    }
  }
  for (const auto& [key, group] : m_interactions)
  {
    if (group->lastLinked < m_build)
    {
      candidates.push_back(Candidate{group->lastLinked, nullptr, &key});
    }
  }
  std::ranges::sort(candidates, {}, &Candidate::lastLinked);
  for (const auto& candidate : candidates)
  {
    if (m_records <= m_capacity)
    {
      break;
    }
    if (candidate.surface)
    {
      const auto it = m_surfaces.find(*candidate.surface);
      releaseGroup(*it->second);
      m_surfaces.erase(it);
    }
    else
    {
      const auto it = m_interactions.find(*candidate.interaction);
      releaseGroup(*it->second);
      m_interactions.erase(it);
    }
  }
}

} // namespace tb::lit
