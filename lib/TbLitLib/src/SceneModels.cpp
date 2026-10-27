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
// GPL Source Code and RBDOOM-3-BFG (neo/d3xp/Entity.cpp, d3xp/Misc.cpp,
// d3xp/Light.cpp, renderer/tr_frontend_addmodels.cpp and
// renderer/RenderWorld_defs.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.2 (C52, C53, D124, D127, D128, D129, D139, D140): the scene's model entities,
// SceneGeometry's members of that part

#include "lit/AreaData.h"
#include "lit/EngineModels.h"
#include "lit/EntityState.h"
#include "lit/LightData.h"
#include "lit/MaterialService.h"
#include "lit/ModelLoading.h"
#include "lit/ProcFile.h"
#include "lit/SceneGeometry.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace tb::lit
{
namespace
{

/**
 * D139: the spawnclasses of d3xp/*.cpp's CLASS_DECLARATIONs at 285ea4d1 that derive from
 * idAnimatedEntity (Entity.cpp:6283), whose SetModel is idAnimatedEntity's
 * (Entity.cpp:6459-6481: animator.SetModel( modelname ), which a modelDef name answers
 * with its animated mesh; idEntity::SetModel otherwise) or, for idWeapon, its own
 * (Weapon.cpp:1690-1715, M-ENT-DIFF-01): idAnimatedEntity, then the others in the grep's
 * order but idAI before idAI_Vagary; mirror.json M-ENT-TABLE-01 holds each declaration's
 * line
 */
constexpr auto AnimatedSpawnclasses = std::array<std::string_view, 21>{
  "idAnimatedEntity",
  "idAFAttachment",
  "idAFEntity_Base",
  "idAFEntity_Gibbable",
  "idAFEntity_Generic",
  "idAFEntity_WithAttachedHead",
  "idAFEntity_Vehicle",
  "idAFEntity_VehicleSimple",
  "idAFEntity_VehicleFourWheels",
  "idAFEntity_VehicleSixWheels",
  "idAFEntity_VehicleAutomated",
  "idAFEntity_SteamPipe",
  "idAFEntity_ClawFourFingers",
  "idAFEntity_Harvest",
  "idActor",
  "idAnimated",
  "idPlayer",
  "idWeapon",
  "idAI",
  "idAI_Vagary",
  "idTestModel",
};

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

bool startsWithNoCase(const std::string_view text, const std::string_view prefix)
{
  return text.size() >= prefix.size()
         && equalsNoCase(text.substr(0, prefix.size()), prefix);
}

std::string lowerCase(std::string_view text)
{
  auto lower = std::string{text};
  for (auto& c : lower)
  {
    c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  }
  return lower;
}

std::uint64_t fnv(std::uint64_t h, const void* data, const std::size_t size)
{
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (auto i = std::size_t{0}; i < size; ++i)
  {
    h ^= bytes[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

std::uint64_t fnvText(const std::uint64_t h, const std::string& text)
{
  return fnv(fnv(h, text.data(), text.size()), "\0", 1);
}

constexpr auto FnvBasis = 0xCBF29CE484222325ull;

// R_AxisToModelMatrix's rows (RenderWorld_defs.cpp:49-60): a model-space point v is
// origin + v.x axis[0] + v.y axis[1] + v.z axis[2] in world space; the bounds of the
// eight corners
EngineBounds worldBoundsOf(
  const std::array<EngineVec3, 2>& bounds,
  const EngineVec3& origin,
  const EngineMat3& axis)
{
  auto result = engineClearedBounds();
  for (auto corner = 0; corner < 8; ++corner)
  {
    const auto v = EngineVec3{
      bounds[std::size_t(corner & 1)][0],
      bounds[std::size_t((corner >> 1) & 1)][1],
      bounds[std::size_t((corner >> 2) & 1)][2]};
    auto w = origin;
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      w[c] += v[0] * axis[0 * 3 + c] + v[1] * axis[1 * 3 + c] + v[2] * axis[2 * 3 + c];
    }
    result = engineAddBounds(result, EngineBounds{w, w});
  }
  return result;
}

bool isAnimatedSpawnclass(const std::string_view spawnclass)
{
  return std::ranges::any_of(AnimatedSpawnclasses, [&](const std::string_view name) {
    return name == spawnclass;
  });
}

} // namespace

void SceneGeometry::markModelEntity(const PreviewId entity)
{
  m_dirtyModels.insert(entity);
}

void SceneGeometry::markAllModelEntities()
{
  for (const auto& [id, entity] : m_entities)
  {
    m_dirtyModels.insert(id);
  }
}

bool SceneGeometry::waitsForModels() const
{
  return m_materials && std::ranges::any_of(m_modelEntities, [](const auto& entry) {
           return entry.second.kind == ModelEntity::Kind::Pending
                  || !entry.second.pendingMaterials.empty()
                  || (!entry.second.skinName.empty() && !entry.second.skin);
         });
}

SceneGeometry::ModelCounts SceneGeometry::modelCounts() const
{
  return m_modelCounts;
}

void SceneGeometry::modelNotice(const PreviewId entity, const std::string& text)
{
  if (m_modelNotices.emplace(entity, text).second)
  {
    m_warnings.push_back(text);
  }
}

void SceneGeometry::removeModelInstances(const PreviewId entity)
{
  const auto it = m_modelEntities.find(entity);
  if (it == m_modelEntities.end())
  {
    return;
  }
  for (const auto& key : it->second.instances)
  {
    m_instances.erase(key);
    m_modelsChanged = true;
  }
  it->second.instances.clear();
}

void SceneGeometry::scanModelEntities(const bool materialsMoved)
{
  // a record, a skin or a material the last build waited for (or read) moved
  for (const auto& [id, model] : m_modelEntities)
  {
    if (model.kind == ModelEntity::Kind::None)
    {
      continue;
    }
    const auto recordMoved =
      !model.model.empty() && m_table->findModel(model.model) != model.record;
    const auto skinMoved =
      !model.skinName.empty() && m_table->findSkin(model.skinName) != model.skin;
    const auto materialArrived = std::ranges::any_of(
      model.pendingMaterials,
      [&](const std::string& name) { return m_table->find(name) != nullptr; });
    if (materialsMoved || recordMoved || skinMoved || materialArrived)
    {
      m_dirtyModels.insert(id);
    }
  }
}

void SceneGeometry::updateModelEntities()
{
  if (m_dirtyModels.empty())
  {
    return;
  }
  auto dirty = std::exchange(m_dirtyModels, {});
  for (const auto id : dirty)
  {
    buildModelEntity(id);
  }
  // the shared surfaces no instance holds any more
  std::erase_if(
    m_sharedSurfaces, [](const auto& entry) { return entry.second.use_count() == 1; });

  auto counts = ModelCounts{};
  for (const auto& [id, model] : m_modelEntities)
  {
    switch (model.kind)
    {
    case ModelEntity::Kind::Pending:
      ++counts.pending;
      ++counts.entities;
      break;
    case ModelEntity::Kind::Skinned:
      ++counts.skinned;
      ++counts.entities;
      break;
    case ModelEntity::Kind::Ready:
    case ModelEntity::Kind::Cube:
      ++counts.entities;
      break;
    case ModelEntity::Kind::None:
    case ModelEntity::Kind::Nothing:
      break;
    }
  }
  counts.instances = m_instances.size();
  m_modelCounts = counts;
}

void SceneGeometry::buildModelEntity(const PreviewId id)
{
  removeModelInstances(id);
  const auto previous = m_modelEntities.find(id);
  const auto ownedState = previous != m_modelEntities.end() && previous->second.ownsState;
  const auto finish = [&](ModelEntity model) {
    if (ownedState && !model.ownsState)
    {
      m_entityStates.erase(id);
    }
    if (model.kind == ModelEntity::Kind::None)
    {
      m_modelEntities.erase(id);
    }
    else
    {
      m_modelEntities.insert_or_assign(id, std::move(model));
    }
  };

  const auto entityIt = m_entities.find(id);
  if (entityIt == m_entities.end() || id == m_worldspawn)
  {
    finish(ModelEntity{});
    return;
  }
  const auto& entity = *entityIt->second;
  if (
    equalsNoCase(entity.classname, "worldspawn")
    || equalsNoCase(entity.classname, "func_group"))
  {
    finish(ModelEntity{});
    return;
  }

  // C22, A10: the map keys over the entityDef's; R3.2: a pending entityDef is asked for
  // and the entity built again once a table has it
  const auto defSlot = m_table ? m_table->findEntityDef(entity.classname) : nullptr;
  if (!defSlot && m_materials)
  {
    static_cast<void>(m_materials->entityDef(entity.classname));
    m_pendingEntityDefs[entity.classname].insert(id);
  }
  const auto* defEntry = defSlot ? defSlot->entry.get() : nullptr;
  const auto spawnArgs =
    mergeSpawnArgs(entity.keys, defEntry ? &defEntry->keys : nullptr);

  // Entity.cpp:702-729: the model key; RB's TrenchBroom interop: a nameless model key on
  // a "genmodel_" entity takes its def's own
  auto modelName = std::string{spawnString(spawnArgs, "model")};
  const auto name = std::string{spawnString(spawnArgs, "name")};
  if (modelName.empty() && defEntry && startsWithNoCase(name, "genmodel_"))
  {
    modelName = std::string{spawnString(defEntry->keys, "model")};
  }
  if (modelName.empty())
  {
    finish(ModelEntity{});
    return;
  }

  // the entity's own nodes: a brush entity has primitives, a point entity and a light a
  // node of their own
  auto primitives = false;
  auto visible = false;
  auto light = false;
  if (const auto nodes = m_entityNodes.find(id); nodes != m_entityNodes.end())
  {
    for (const auto nodeId : nodes->second)
    {
      if (const auto node = m_nodes.find(nodeId); node != m_nodes.end())
      {
        primitives = true;
        visible = visible || node->second->visible;
      }
    }
  }
  if (const auto node = m_nodes.find(id); node != m_nodes.end() && !primitives)
  {
    visible = node->second->visible;
    light = node->second->kind == SnapshotNodeKind::Light;
  }

  // K17: a brush entity whose model is its own name draws its brushes (its chunks)
  if (primitives && equalsNoCase(name, modelName))
  {
    finish(ModelEntity{});
    return;
  }

  auto model = ModelEntity{};
  model.kind = ModelEntity::Kind::Nothing;
  const auto who = fmt::format(
    "the model of '{}' ({})", name.empty() ? std::to_string(id) : name, entity.classname);

  // D128: hide 1 (Entity.cpp:651-656; a light's model too, Light.cpp Event_Hide), a
  // dmapOnly class (its plain def, C22: never spawned), worldspawn's removeEntities
  // (MapFile.cpp:2270-2282), a hidden node (D30): nothing is drawn
  const auto worldspawn = m_entities.find(m_worldspawn);
  const auto removed =
    worldspawn != m_entities.end()
    && isRemovedClassname(removedClassnames(worldspawn->second->keys), entity.classname);
  auto parms = parseSpawnArgsToRenderEntity(spawnArgs);
  if (parms.hidden || (defEntry && defEntry->dmapOnly) || removed || !visible)
  {
    finish(std::move(model));
    return;
  }

  // C51 (1), K47: a name a .proc model has (an area's, a brush entity's) draws nothing on
  // another entity, where the engine draws that model
  const auto canonical = canonicalModelName(modelName);
  const auto extension = engineFileExtension(canonical);
  if (extension.empty())
  {
    auto procModel = startsWithNoCase(canonical, "_area");
    for (const auto& procEntry : m_proc ? m_proc->models : std::vector<ProcModel>{})
    {
      procModel = procModel || lowerCase(procEntry.name) == canonical;
    }
    for (const auto& [otherId, nodes] : m_entityNodes)
    {
      if (otherId == id || nodes.empty())
      {
        continue;
      }
      if (const auto other = m_entities.find(otherId); other != m_entities.end())
      {
        const auto* otherName = findKey(other->second->keys, "name");
        procModel = procModel || (otherName && lowerCase(*otherName) == canonical);
      }
    }
    if (procModel)
    {
      modelNotice(
        id,
        fmt::format(
          "{} names the .proc model '{}': drawn as nothing here, where the engine draws "
          "that model (K47)",
          who,
          modelName));
      finish(std::move(model));
      return;
    }
  }

  // D139: a modelDef name (no extension, no .proc model) is the spawnclass's to decide:
  // an idAnimatedEntity draws its animated mesh, which the preview does not (K41); an
  // idEntity's SetModel finds the default cube (Entity.cpp:704, ModelManager.cpp:508-523)
  const auto* spawnclass = findKey(spawnArgs, "spawnclass");
  const auto animated = spawnclass && isAnimatedSpawnclass(*spawnclass);
  if (extension.empty() && animated)
  {
    model.kind = ModelEntity::Kind::Skinned;
    modelNotice(
      id,
      fmt::format(
        "{}: the {} spawnclass draws the modelDef '{}' as an animated mesh: nothing is "
        "drawn here (K41)",
        who,
        *spawnclass,
        modelName));
    finish(std::move(model));
    return;
  }

  // C51: the record (Pending until the service answers, D124)
  model.model = canonical;
  model.record = m_materials ? m_materials->requestModel(canonical) : nullptr;
  if (!model.record && m_table)
  {
    model.record = m_table->findModel(canonical);
  }
  if (!model.record || model.record->state == ModelRecord::State::Pending)
  {
    model.kind = ModelEntity::Kind::Pending;
    finish(std::move(model));
    return;
  }
  const auto& record = *model.record;
  if (record.state == ModelRecord::State::Skinned)
  {
    model.kind = ModelEntity::Kind::Skinned;
    modelNotice(
      id,
      fmt::format("{}: '{}' is a skinned model: nothing is drawn (K41)", who, modelName));
    finish(std::move(model));
    return;
  }
  model.kind = record.state == ModelRecord::State::Ready ? ModelEntity::Kind::Ready
                                                         : ModelEntity::Kind::Cube;
  if (model.kind == ModelEntity::Kind::Cube)
  {
    modelNotice(
      id,
      fmt::format(
        "{}: '{}' draws the default cube: {}",
        who,
        modelName,
        record.reason.empty() ? std::string{"the engine's default model"}
                              : record.reason));
  }
  if (!record.note.empty())
  {
    modelNotice(id, fmt::format("{}: '{}': {}", who, modelName, record.note));
  }

  // the entity's state: a brush entity's is its chunk job's (its pivot with the origin
  // brushes' offset, Entity.cpp:701-707); a point entity's and a light's the scene's
  auto state = ChunkEntityState{};
  if (primitives)
  {
    const auto it = m_entityStates.find(id);
    if (it == m_entityStates.end())
    {
      // its batch has not applied yet: built again when it has (collectChunks)
      model.kind = ModelEntity::Kind::Pending;
      finish(std::move(model));
      return;
    }
    state = it->second;
  }
  else
  {
    // Light.cpp SetLightLevel at spawn: a light's model takes the light's colour times
    // its level, 0 when it starts off (Off: currentLevel 0)
    if (light && spawnBool(spawnArgs, "start_off", "0"))
    {
      parms.shaderParms[0] = parms.shaderParms[1] = parms.shaderParms[2] = 0.0f;
    }
    state.id = id;
    state.parms = parms;
    state.modelOrigin = parms.origin;
    state.hash = renderEntityHash(parms);
    model.ownsState = true;
  }
  state.visible = true;

  // C52, D130 (M-REF-01 extended): R_CreateEntityRefs from the model's bounds
  // (RenderWorld_defs.cpp:156-197: hModel->Bounds( &parms ), FinishSurfaces' or the
  // binary's, deforms grown), never clipped into areas (SC625): R_DeriveEntityData, then
  // PushFrustumIntoTree's areas in the engine's chain order; cleared bounds or no area
  // reached: no reference, D81's outside set (K14 extended, D140)
  state.localReferenceBounds = record.bounds;
  state.materialBounds.clear();
  for (const auto& surface : record.surfaces)
  {
    const auto [it, added] =
      state.materialBounds.try_emplace(surface.material, engineClearedBounds());
    it->second = engineAddBounds(it->second, surface.bounds);
  }
  state.areas.clear();
  if (engineBoundsIsCleared(state.localReferenceBounds))
  {
    state.inverseBaseModelProject = EngineMat4{};
    state.globalReferenceBounds = engineClearedBounds();
  }
  else
  {
    state.inverseBaseModelProject = engineOffsetScaleForBounds(
      engineMatrixFromOriginAxis(state.modelOrigin, state.parms.axis),
      state.localReferenceBounds);
    state.globalReferenceBounds =
      engineProjectedBounds(state.inverseBaseModelProject, engineUnitCube(), false);
    if (m_proc && !m_proc->nodes.empty())
    {
      const auto commonAreas =
        m_areas ? m_areas->commonAreas : commonChildrenAreas(*m_proc);
      auto pushed = pushFrustumIntoTree(
        *m_proc, commonAreas, state.inverseBaseModelProject, engineUnitCube());
      state.areas.assign(pushed.rbegin(), pushed.rend());
    }
  }

  // C53: the shader, else the skin (a pending skin: no remap, provisional, asked)
  const auto* skin = static_cast<const SkinRecord*>(nullptr);
  if (parms.shader.empty() && !parms.skin.empty())
  {
    model.skinName = parms.skin;
    model.skin = m_table ? m_table->findSkin(parms.skin) : nullptr;
    if (!model.skin && m_materials)
    {
      static_cast<void>(m_materials->skin(parms.skin));
    }
    skin = model.skin ? model.skin->entry.get() : nullptr;
  }
  const auto skinPending = !model.skinName.empty() && !model.skin;

  // a material's entry in the scene's table, asked for when it lacks it
  const auto entryOf = [&](const std::string& material) -> const MaterialEntry* {
    const auto slot = m_table ? m_table->find(material) : nullptr;
    if (!slot)
    {
      if (m_materials)
      {
        static_cast<void>(m_materials->lookup(material));
      }
      if (
        std::ranges::find(model.pendingMaterials, material)
        == model.pendingMaterials.end())
      {
        model.pendingMaterials.push_back(material);
      }
      return nullptr;
    }
    return slot->entry.get();
  };

  const auto area = m_proc ? pointInArea(*m_proc, state.modelOrigin) : 0;
  auto cell = CellCoord{};
  for (auto c = std::size_t{0}; c < 3; ++c)
  {
    cell[c] = int(std::floor(double(state.modelOrigin[c]) / CellSize));
  }

  for (auto s = std::size_t{0}; s < record.surfaces.size(); ++s)
  {
    const auto& surface = record.surfaces[s];
    if (surface.indexes.empty())
    {
      continue;
    }
    // tr_frontend_addmodels.cpp:579: neither drawn nor casting shadows (collision hulls)
    const auto* own = entryOf(surface.material);
    if (own && !engineMaterialDrawn(*own) && !own->surfaceCastsShadow)
    {
      continue;
    }
    const auto drawName =
      engineDrawMaterial(surface.material, own, parms.shader, skin, entryOf);
    if (!drawName)
    {
      continue;
    }
    const auto* draw = *drawName == surface.material ? own : entryOf(*drawName);

    auto shared = std::shared_ptr<const SharedSurface>{};
    const auto placement = SharedGeometryKey{record.name, s, record.version};
    if (const auto it = m_sharedSurfaces.find(placement); it != m_sharedSurfaces.end())
    {
      shared = it->second;
    }
    else
    {
      shared = makeSharedSurface(model.record, s);
      m_sharedSurfaces.emplace(placement, shared);
    }

    auto instance = ModelInstance{};
    instance.entity = id;
    instance.surface = s;
    instance.geometry = shared;
    instance.modelOrigin = state.modelOrigin;
    instance.axis = state.parms.axis;
    instance.shaderParms = state.parms.shaderParms;
    instance.worldBounds =
      worldBoundsOf(surface.bounds, state.modelOrigin, state.parms.axis);
    instance.areas = state.areas;
    auto& header = instance.header;
    header.key = ChunkKey{area, id, *drawName, cell, int(s)};
    header.bounds = surface.bounds;
    header.sourceMaterial = surface.material;
    header.provisional = !own || !draw || skinPending;
    // S36: the draw material's cast class, the noshadows flag (LP3.0: C57's rule)
    header.castClass = engineCastClass(draw, state.parms.noShadows);
    // the caster version: the geometry (the record's name, version and surface), the cast
    // class and the flags; an entity whose model changes to another record of an equal
    // version moves it too (LightTriangles' stamp reads it)
    auto caster = fnvText(FnvBasis, record.name);
    caster = fnv(caster, &record.version, sizeof(record.version));
    caster = fnv(caster, &s, sizeof(s));
    caster = fnv(caster, &header.castClass, sizeof(header.castClass));
    const auto flags =
      std::array<bool, 2>{state.parms.noShadows, state.parms.noSelfShadows};
    caster = fnv(caster, flags.data(), sizeof(flags));
    if (header.castClass == CastClass::Perforated)
    {
      caster = fnvText(caster, *drawName);
    }
    header.casterVersion = caster;
    auto appearance = fnvText(caster, *drawName);
    appearance = fnvText(appearance, surface.material);
    appearance = fnv(appearance, &state.hash, sizeof(state.hash));
    appearance = fnv(appearance, state.modelOrigin.data(), sizeof(EngineVec3));
    header.appearanceVersion = appearance;

    model.instances.push_back(header.key);
    m_instances.insert_or_assign(header.key, std::move(instance));
    m_modelsChanged = true;
  }

  m_entityStates.insert_or_assign(id, std::move(state));
  finish(std::move(model));
}

} // namespace tb::lit
