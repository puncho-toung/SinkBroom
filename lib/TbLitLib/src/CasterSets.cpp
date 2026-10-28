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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_addlights.cpp and
// renderer/tr_frontend_addmodels.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "CasterSets.h"

#include "ShadowCache.h"
#include "lit/AreaData.h"

#include <algorithm>

namespace tb::lit
{
namespace
{

// an entity's state in the build's inputs; null: worldspawn's
const ChunkEntityState* entityState(const FrontendInputs& inputs, const PreviewId entity)
{
  if (inputs.entities)
  {
    if (const auto it = inputs.entities->find(entity); it != inputs.entities->end())
    {
      return &it->second;
    }
  }
  return nullptr;
}

// the pair keys' order (ChunkKey::operator<)
bool keyLess(const ChunkKey& lhs, const ChunkKey& rhs)
{
  return lhs < rhs;
}

} // namespace

CasterSetsKey casterSetsKeyOf(const FrontendInputs& inputs, const bool flowed)
{
  auto key = CasterSetsKey{};
  key.chunks = inputs.chunks;
  key.chunksVersion = inputs.chunksVersion;
  key.instances = inputs.instances;
  key.entities = inputs.entities;
  key.tree = inputs.tree;
  key.areas = inputs.areas;
  key.table = inputs.table;
  key.skipShadows = inputs.skipShadows;
  key.lightTriangleCull = static_cast<bool>(inputs.lightTriangles);
  key.flowed = flowed;
  return key;
}

void CasterSets::beginBuild(const CasterSetsKey& key)
{
  if (m_key != key)
  {
    m_key = key;
    m_candidatesMade = false;
    m_candidates.clear();
    m_references.clear();
    m_sets.clear();
  }
  ++m_build;
  m_built = 0;
  m_models.clear();
}

void CasterSets::makeCandidates(const FrontendInputs& inputs)
{
  // LP3.0 (C57, P3.14): every chunk and instance of a cast class, in chunk order then
  // instance order, with what the pairing reads of it: a brush entity's references, a
  // world chunk's area (the outside set in area -1 or past the area count, D81, or
  // without a flow)
  m_candidatesMade = true;
  const auto numAreas =
    m_key && m_key->flowed && inputs.areas ? inputs.areas->areas.size() : std::size_t{0};
  const auto add =
    [&](const ChunkKey& key, const Chunk& chunk, const ModelInstance* instance) {
      if (chunk.castClass == CastClass::None)
      {
        return;
      }
      auto candidate = Candidate{};
      candidate.key = &key;
      candidate.chunk = &chunk;
      candidate.instance = instance;
      if (const auto* state = entityState(inputs, key.entity))
      {
        // a copy: a removed entity's state may go before its batch applies (SG:367)
        const auto [references, made] =
          m_references.try_emplace(key.entity, state->areas);
        candidate.areaSurface.kind = AreaSurfaceKind::Entity;
        candidate.areaSurface.refs = references->second;
      }
      else if (key.area >= 0 && std::size_t(key.area) < numAreas)
      {
        candidate.areaSurface.kind = AreaSurfaceKind::WorldArea;
        candidate.areaSurface.area = key.area;
      }
      else
      {
        candidate.areaSurface.kind = AreaSurfaceKind::WorldOutside;
      }
      m_candidates.push_back(std::move(candidate));
    };
  if (inputs.chunks)
  {
    for (const auto& [key, chunk] : *inputs.chunks)
    {
      if (!chunk.indexes.empty())
      {
        add(key, chunk, nullptr);
      }
    }
  }
  if (inputs.instances)
  {
    for (const auto& [key, instance] : *inputs.instances)
    {
      if (instance.geometry && instance.geometry->indexCount() > 0)
      {
        add(key, instance.header, &instance);
      }
    }
  }
}

const LitSpace* CasterSets::modelOf(const FrontendInputs& inputs, const PreviewId entity)
{
  // the space of the entity without a view (R_SetEntityDefViewEntity's model matrices
  // and shaderParms, as litSpaceOf makes them; worldspawn's for an entity without a
  // state); none for an invisible one
  auto [it, made] = m_models.try_emplace(entity);
  if (made)
  {
    const auto* state = entityState(inputs, entity);
    if (!state)
    {
      it->second = litModelSpaceOf(
        entity,
        EngineVec3{0, 0, 0},
        EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1},
        engineWorldShaderParms());
    }
    else if (state->visible)
    {
      it->second = litModelSpaceOf(
        entity, state->modelOrigin, state->parms.axis, state->parms.shaderParms);
    }
  }
  return it->second ? &*it->second : nullptr;
}

void CasterSets::resolve(Candidate& candidate, const FrontendInputs& inputs)
{
  if (candidate.resolvedBuild == m_build)
  {
    return;
  }
  candidate.resolvedBuild = m_build;
  candidate.model = modelOf(inputs, candidate.key->entity);
  candidate.material = nullptr;
  candidate.materialPending = false;
  candidate.registers = nullptr;
  candidate.provisional = false;
  candidate.perforatedTerm = 0;
  if (candidate.model && inputs.material)
  {
    const auto answer = inputs.material(candidate.key->material);
    candidate.material = answer.entry;
    candidate.materialPending = answer.pending;
  }
  if (candidate.model && candidate.chunk->castClass == CastClass::Perforated)
  {
    // LP3.4 (D155): the registers its stages draw by, evaluated once per build (the
    // surfaces' rule); pending ones keep the lists pending (D77): the table that answers
    // them builds the lists again
    if (candidate.material && inputs.registers)
    {
      const auto registers = inputs.registers(
        candidate.key->entity, *candidate.material, candidate.model->shaderParms);
      candidate.registers = registers.registers;
      candidate.provisional = !registers.registers || !registers.current;
    }
    else
    {
      candidate.provisional = candidate.materialPending || !inputs.registers;
    }
    candidate.perforatedTerm = shadowPerforatedTerm(
      *candidate.key,
      candidate.instance != nullptr,
      candidate.material.get(),
      candidate.registers.get(),
      candidate.provisional);
  }
}

CasterSet CasterSets::make(
  const FrontendInputs& inputs,
  const std::shared_ptr<const LightRecord>& record,
  const int spectrum)
{
  if (!m_candidatesMade)
  {
    makeCandidates(inputs);
  }
  auto set = CasterSet{};
  set.record = record;
  set.lightVersion = record->version;
  set.lightTrianglesVersion = inputs.lightTrianglesVersion;
  set.tableVersion = inputs.tableVersion;
  const auto& projection = record->derived.projection;
  const auto flowed = m_key && m_key->flowed && inputs.areas;
  // lightReachesCaster, the one rule (C57 rev 2.3, D81, K14, H03): the light's area test
  // tabulated once per light (lightCasterAreas, FAL:488-501), then per candidate the
  // outside set for a light without references alone, never for one with references (a
  // hull face in the void that dmap removes casts nothing into the hull); without a tree
  // (D94) nothing is classified and every candidate casts
  const auto areaLight = AreaLight{record->derived.areaNum, record->derived.areas};
  const auto casterAreas =
    flowed ? lightCasterAreas(areaLight, *inputs.areas) : std::vector<char>{};
  for (auto& candidate : m_candidates)
  {
    if (flowed && !lightReachesCaster(areaLight, candidate.areaSurface, casterAreas))
    {
      continue;
    }
    resolve(candidate, inputs);
    if (candidate.model && candidate.chunk->castClass == CastClass::Perforated)
    {
      set.pending = set.pending || candidate.provisional;
      if (candidate.material)
      {
        set.registerConsumers.emplace_back(candidate.key->entity, candidate.material);
      }
    }
    set.tablePending =
      set.tablePending || candidate.materialPending || candidate.provisional;
    if (!candidate.model)
    {
      continue;
    }
    const auto& model = *candidate.model;
    if (engineCullModelBoundsToLight(
          projection.baseLightProject, candidate.chunk->bounds, model.modelRenderMatrix))
    {
      continue;
    }
    // "invisible ink" lights and shaders (FAM:935-938)
    if (candidate.material && candidate.material->spectrum != spectrum)
    {
      continue;
    }
    auto surface = LitSurface{};
    surface.chunk = candidate.chunk;
    surface.instance = candidate.instance;
    surface.material = candidate.material;
    ++set.pairs;
    const auto triangles = inputs.lightTriangles
                             ? inputs.lightTriangles(*record, surface, model)
                             : std::shared_ptr<const LightTriangles>{};
    if (inputs.lightTriangles)
    {
      set.pairKeys.push_back(
        candidate.instance ? candidate.instance->key() : *candidate.key);
    }
    if (triangles && triangles->state == LightTriangles::State::None)
    {
      // no triangle of the surface reaches the light: no shadow surface (FAM:1089)
      ++set.pairsResident;
      continue;
    }
    if (triangles && triangles->state == LightTriangles::State::Pending)
    {
      ++set.pairsPending;
      set.pairPending = true;
    }
    else
    {
      ++set.pairsResident;
    }
    auto caster = KeptCaster{};
    caster.key = *candidate.key;
    caster.isInstance = candidate.instance != nullptr;
    caster.chunk = candidate.chunk;
    caster.instance = candidate.instance;
    caster.material = candidate.material;
    if (triangles && triangles->state != LightTriangles::State::All)
    {
      caster.lightTriangles = triangles;
    }
    // LP3.3 (C61, D150): the light's caster hash, over the casters it keeps
    caster.casterTerm = shadowCasterTerm(
      *candidate.key,
      candidate.instance != nullptr,
      candidate.chunk->casterVersion,
      model.modelMatrix,
      candidate.chunk->castClass,
      candidate.areaSurface);
    set.casterHash += caster.casterTerm;
    ++set.casterCount;
    // LP3.4 (C61, P3.6): a Perforated caster's registers ride on its entry, its term in
    // the light's perforatedHash
    if (candidate.chunk->castClass == CastClass::Perforated)
    {
      caster.perforated = true;
      caster.registers = candidate.registers;
      caster.provisional = candidate.provisional;
      caster.perforatedTerm = candidate.perforatedTerm;
      set.perforatedHash += candidate.perforatedTerm;
    }
    set.casters.push_back(std::move(caster));
  }
  // ChunkKey has operator< alone (not std::totally_ordered): the comparator is given
  std::ranges::sort(set.pairKeys, keyLess);
  return set;
}

CasterSets::Answer CasterSets::set(
  const FrontendInputs& inputs,
  const std::shared_ptr<const LightRecord>& record,
  const int spectrum)
{
  auto [it, added] = m_sets.try_emplace(record->id);
  auto& set = it->second;
  const auto stale =
    added || set.record != record || set.lightVersion != record->version
    || (set.pairPending && set.lightTrianglesVersion != inputs.lightTrianglesVersion)
    || (set.tablePending && set.tableVersion != inputs.tableVersion);
  if (stale)
  {
    set = make(inputs, record, spectrum);
    ++m_built;
  }
  return Answer{&set, stale};
}

std::size_t CasterSets::built() const
{
  return m_built;
}

void CasterSets::endBuild(const LightTable* lights)
{
  if (lights == m_sweptLights && (!lights || lights->version == m_sweptVersion))
  {
    return;
  }
  m_sweptLights = lights;
  m_sweptVersion = lights ? lights->version : 0;
  std::erase_if(
    m_sets, [&](const auto& entry) { return !lights || !lights->find(entry.first); });
}

bool CasterSets::holds(const PreviewId light, const ChunkKey& chunk) const
{
  const auto it = m_sets.find(light);
  return it != m_sets.end()
         && std::ranges::binary_search(it->second.pairKeys, chunk, keyLess);
}

const CasterSet* CasterSets::find(const PreviewId light) const
{
  const auto it = m_sets.find(light);
  return it != m_sets.end() ? &it->second : nullptr;
}

std::size_t CasterSets::size() const
{
  return m_sets.size();
}

void CasterSets::clear()
{
  m_key.reset();
  m_candidatesMade = false;
  m_candidates.clear();
  m_references.clear();
  m_sets.clear();
  m_models.clear();
  m_built = 0;
  m_sweptLights = nullptr;
  m_sweptVersion = 0;
}

} // namespace tb::lit
