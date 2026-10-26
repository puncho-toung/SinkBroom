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
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 GPL Source
// Code and RBDOOM-3-BFG (neo/tools/compilers/dmap/dress_decal.cpp and
// dmap/output.cpp):
//   Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.3 (C54, D132, D138, K42, K43; P7.18): the scene's dress decals, SceneGeometry's
// members of that part

#include "lit/DecalProjectors.h"
#include "lit/EntityState.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"
#include "lit/ProcIslands.h"
#include "lit/SceneGeometry.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>
#include <tuple>

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

std::string lowerCase(std::string_view text)
{
  auto lower = std::string{text};
  for (auto& c : lower)
  {
    c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  }
  return lower;
}

/**
 * K42 (mirror.json M-ENT-01's dmap anchor, usurface.cpp:779-782): dmap inlines a
 * func_static or misc_model with inline 1 on its raw map keys, or every one with
 * worldspawn's inlineAllStatics
 */
bool isInlined(const SceneEntity& entity, const bool inlineAll)
{
  if (
    !equalsNoCase(entity.classname, "func_static")
    && !equalsNoCase(entity.classname, "misc_model"))
  {
    return false;
  }
  return inlineAll || spawnBool(entity.keys, "inline", "0");
}

// two boxes touch, a grown by growth on each side
bool boundsTouch(const EngineBounds& a, const EngineBounds& b, const float growth)
{
  for (auto c = std::size_t{0}; c < 3; ++c)
  {
    if (a[0][c] - growth > b[1][c] || a[1][c] + growth < b[0][c])
    {
      return false;
    }
  }
  return true;
}

} // namespace

SceneGeometry::DressCounts SceneGeometry::dressCounts() const
{
  return m_dressCounts;
}

std::shared_ptr<const DressTable> SceneGeometry::dressTable() const
{
  return m_dress;
}

void SceneGeometry::dressNotice(const PreviewId entity, const std::string& text)
{
  if (m_dressNotices.emplace(entity, text).second)
  {
    m_warnings.push_back(text);
  }
}

bool SceneGeometry::updateDress()
{
  // a batch in flight may be one built for a new tree, whose pieces have not returned:
  // the pass waits for it (collectChunks runs it once the batch applied)
  if (!m_dressDirty || !m_jobs.empty())
  {
    return false;
  }
  m_dressDirty = false;

  auto counts = DressCounts{};
  auto waits = false;
  auto planar = std::uint64_t{0};
  auto table = std::make_shared<DressTable>();
  table->tree = m_proc;

  // every entity in preview-id order; dmap's FindEntity finds a name's first entity
  auto ids = std::vector<PreviewId>{};
  ids.reserve(m_entities.size());
  for (const auto& [id, entity] : m_entities)
  {
    ids.push_back(id);
  }
  std::ranges::sort(ids);
  auto named = std::map<std::string, PreviewId>{};
  for (const auto id : ids)
  {
    if (const auto* name = findKey(m_entities.at(id)->keys, "name"))
    {
      named.try_emplace(lowerCase(*name), id);
    }
  }

  // the projectors (dress_decal.cpp:312-325: every func_decal of the map in its order)
  struct Projector
  {
    PreviewId id = NoPreviewId;
    std::string name;
    DecalProjector projector;
    bool claims = false;
  };
  auto projectors = std::vector<Projector>{};
  const auto inputs = DecalProjectorInputs{
    [&](const std::string& material) -> std::optional<const MaterialEntry*> {
      const auto slot = m_table ? m_table->find(material) : nullptr;
      if (!slot)
      {
        if (m_materials)
        {
          static_cast<void>(m_materials->lookup(material));
        }
        return std::nullopt;
      }
      return slot->entry.get();
    },
    [&](const std::string_view name) -> const EntityKeys* {
      const auto it = named.find(lowerCase(name));
      return it != named.end() ? &m_entities.at(it->second)->keys : nullptr;
    }};
  for (const auto id : ids)
  {
    const auto& entity = *m_entities.at(id);
    if (id == m_worldspawn || !equalsNoCase(entity.classname, FuncDecalClassname))
    {
      continue;
    }
    // C22, A10: the map keys over the entityDef's (SetDefaults, :152-157); a pending
    // entityDef is asked for and decided once a table has it (R3.2)
    const auto defSlot = m_table ? m_table->findEntityDef(entity.classname) : nullptr;
    if (!defSlot && m_materials)
    {
      static_cast<void>(m_materials->entityDef(entity.classname));
      m_pendingEntityDefs[entity.classname].insert(id);
      continue;
    }
    const auto spawnArgs = mergeSpawnArgs(
      entity.keys, defSlot && defSlot->entry ? &defSlot->entry->keys : nullptr);
    // :93-101 DecalName: the map's name key, else the entity's number (here its id)
    const auto* nameKey = findKey(entity.keys, "name");
    auto name = nameKey && !nameKey->empty()
                  ? *nameKey
                  : fmt::format("entity {}", std::uint64_t(id));
    auto parse = parseDecalProjector(name, spawnArgs, inputs);
    if (parse.outcome == DecalProjectorOutcome::Pending)
    {
      waits = waits || m_materials != nullptr;
      continue;
    }
    for (const auto& warning : parse.warnings)
    {
      dressNotice(id, warning);
    }
    if (parse.outcome != DecalProjectorOutcome::Ready)
    {
      // nothing baked (the engine warned): a func_decal without a fragment surface
      ++counts.unmatched;
      continue;
    }
    projectors.push_back(
      Projector{id, std::move(name), std::move(parse.projector), false});
  }

  // K42: the inlined model entities' instances (dmap draws their copy into the world)
  const auto worldspawn = m_entities.find(m_worldspawn);
  const auto inlineAll = worldspawn != m_entities.end()
                         && spawnBool(worldspawn->second->keys, "inlineAllStatics", "0");
  struct Inlined
  {
    std::string material;
    EngineBounds bounds = {};
  };
  auto inlined = std::vector<Inlined>{};
  for (const auto& [key, instance] : m_instances)
  {
    const auto entity = m_entities.find(instance.entity);
    if (entity != m_entities.end() && isInlined(*entity->second, inlineAll))
    {
      inlined.push_back(
        Inlined{lowerCase(chunkSourceMaterial(instance.header)), instance.worldBounds});
    }
  }

  // C54's "matches no editor face": none of the surface's islands is in worldspawn's
  // adoption table, LP7.4's test finds no candidate for any planar one (matchProcIsland:
  // NoCandidate) and no editor patch of its material touches a curved one (patches are
  // no candidates, SC636: "matching no editor face or patch")
  const auto adoption = adoptionOf(m_worldspawn);
  const auto matchesNoEditorSurface = [&](
                                        const std::size_t m,
                                        const std::size_t s,
                                        const int area) {
    if (adoption)
    {
      for (const auto& island : adoption->adopted)
      {
        if (island.model == m && island.surface == s)
        {
          return false;
        }
      }
    }
    const auto& surface = m_proc->models[m].surfaces[s];
    const auto group = AdoptionGroup{m_worldspawn, area, lowerCase(surface.material)};
    auto pieces = std::vector<const AdoptionPiece*>{};
    if (const auto cells = m_groupCells.find(group); cells != m_groupCells.end())
    {
      for (const auto& ref : cells->second)
      {
        if (const auto it = m_cellPieces.find(ref); it != m_cellPieces.end())
        {
          for (const auto& piece : it->second)
          {
            if (piece.area == area && lowerCase(piece.material) == group.material)
            {
              pieces.push_back(&piece);
            }
          }
        }
      }
    }
    for (const auto& island : m_islands->models[m].surfaces[s])
    {
      if (!island.planar)
      {
        for (const auto& [id, node] : m_nodes)
        {
          if (
            !node->patch || chunkEntityOf(*node) != m_worldspawn
            || !equalsNoCase(
              enginePatchMaterial(node->patch->material), surface.material))
          {
            continue;
          }
          auto points = std::vector<EngineVec3>{};
          points.reserve(node->patch->controlPoints.size());
          for (const auto& point : node->patch->controlPoints)
          {
            points.push_back({float(point[0]), float(point[1]), float(point[2])});
          }
          if (boundsTouch(engineBoundsOfPoints(points), island.bounds, AdoptionEpsilon))
          {
            return false;
          }
        }
        continue;
      }
      if (matchProcIsland(surface, island, pieces).verdict != IslandVerdict::NoCandidate)
      {
        return false;
      }
    }
    return true;
  };

  // D138: a .bproc's tree has no islands, and no surface is identified from it; dmap's
  // surfaces are the world's (_areaN), one per projector per area it reaches
  // (output.cpp:439-450)
  if (
    m_proc && m_islands && m_worldspawn != NoPreviewId
    && (!projectors.empty() || !inlined.empty()))
  {
    for (const auto& [area, m] : m_areaModels)
    {
      const auto& surfaces = m_proc->models[m].surfaces;
      const auto& islands = m_islands->models[m].surfaces;
      for (auto s = std::size_t{0}; s < surfaces.size() && s < islands.size(); ++s)
      {
        const auto& surface = surfaces[s];
        const auto material = lowerCase(surface.material);
        // K42 (LP7.2's finding): dmap's inlined copy, never a fragment
        if (std::ranges::any_of(inlined, [&](const Inlined& instance) {
              return instance.material == material
                     && procSurfaceWithin(surface, instance.bounds, InlineBoundsGrowth);
            }))
        {
          ++counts.inlined;
          continue;
        }
        auto claimants = std::vector<Projector*>{};
        for (auto& projector : projectors)
        {
          if (
            lowerCase(projector.projector.material) == material
            && procSurfaceWithin(
              surface, projector.projector.parms.projectionBounds, DressBoundsGrowth))
          {
            claimants.push_back(&projector);
          }
        }
        if (claimants.empty() || !matchesNoEditorSurface(m, s, area))
        {
          continue;
        }
        // P7.18: two projectors claiming one surface: the first by preview id
        for (auto* projector : claimants)
        {
          projector->claims = true;
        }
        for (auto c = std::size_t{1}; c < claimants.size(); ++c)
        {
          ++counts.ties;
          dressNotice(
            claimants[c]->id,
            fmt::format(
              "func_decal '{}': a fragment surface within its projection is drawn as "
              "func_decal '{}''s, the first by id",
              claimants[c]->name,
              claimants.front()->name));
        }
        auto cell = CellCoord{};
        for (auto c = std::size_t{0}; c < 3; ++c)
        {
          const auto centre =
            (double(surface.bounds[0][c]) + double(surface.bounds[1][c])) * 0.5;
          cell[c] = int(std::floor(centre / CellSize));
        }
        table->surfaces.push_back(DressSurface{
          m,
          s,
          CellRef{m_worldspawn, cell},
          area,
          surface.material,
          claimants.front()->id});
        ++counts.surfaces;
        planar += std::uint64_t(std::ranges::count_if(
          islands[s], [](const ProcIsland& island) { return island.planar; }));
      }
    }
  }
  std::ranges::sort(table->surfaces, [](const DressSurface& a, const DressSurface& b) {
    return std::tie(a.model, a.surface) < std::tie(b.model, b.surface);
  });

  // K43: a projector with no fragment surface is counted, and named once the bake it
  // would be read from is known (the map path's request answered, or none to ask)
  const auto final = !m_materials || !m_mapPath || bakeAnswered();
  for (const auto& projector : projectors)
  {
    if (projector.claims)
    {
      continue;
    }
    ++counts.unmatched;
    if (final)
    {
      dressNotice(
        projector.id,
        fmt::format(
          "func_decal {} has no baked fragments: dmap the map", projector.name));
    }
  }

  // the cells of the surfaces that entered or left the table
  const auto noSurfaces = std::vector<DressSurface>{};
  const auto& before = m_dress ? m_dress->surfaces : noSurfaces;
  const auto changed =
    before != table->surfaces || (m_dress && m_dress->tree != table->tree);
  if (changed)
  {
    for (const auto& surface : before)
    {
      m_dirty.insert(surface.cell);
    }
    for (const auto& surface : table->surfaces)
    {
      m_dirty.insert(surface.cell);
    }
    m_dress = table->surfaces.empty() ? nullptr : std::move(table);
  }
  m_dressCounts = counts;
  m_dressPlanarIslands = planar;
  m_dressWaits = waits;
  updateAdoptionCounts();
  return changed;
}

} // namespace tb::lit
