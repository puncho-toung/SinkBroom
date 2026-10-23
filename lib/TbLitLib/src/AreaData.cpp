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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.cpp,
// renderer/RenderWorld_load.cpp and renderer/RenderWorld_defs.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/AreaData.h"

#include "lit/LightData.h"
#include "lit/ProcFile.h"

#include <algorithm>
#include <unordered_map>

namespace tb::lit
{
namespace
{

// deform_t's DFRM_NONE (Material.h:74)
constexpr auto DeformNone = 0;

// FindMaterial( name )->Deform() once per name; no entry: no deform
class DeformCache
{
private:
  const AreaMaterialLookup& m_materials;
  std::unordered_map<std::string, bool> m_deforms;

public:
  explicit DeformCache(const AreaMaterialLookup& materials)
    : m_materials{materials}
  {
  }

  bool deforms(const std::string& material)
  {
    if (!m_materials)
    {
      return false;
    }
    if (const auto it = m_deforms.find(material); it != m_deforms.end())
    {
      return it->second;
    }
    const auto entry = m_materials(material);
    const auto deforms = entry && entry->deform != DeformNone;
    m_deforms.emplace(material, deforms);
    return deforms;
  }
};

EngineBounds modelBoundsOf(const ProcModel& model, DeformCache& deforms)
{
  // a .bproc's model: LoadBinaryModel's stored bounds, written already grown
  if (model.boundsKnown)
  {
    return model.bounds;
  }
  // Model.cpp:1202-1209, :1328-1332: without surfaces a point at the origin
  if (model.surfaces.empty())
  {
    return engineZeroBounds();
  }
  // :1333-1366
  auto bounds = engineClearedBounds();
  for (const auto& surface : model.surfaces)
  {
    auto surfaceBounds = EngineBounds{surface.bounds[0], surface.bounds[1]};
    // if the surface has a deformation, increase the bounds
    if (deforms.deforms(surface.material))
    {
      surfaceBounds = engineDeformSurfaceBounds(surfaceBounds);
    }
    // add to the model bounds
    bounds = engineAddBounds(bounds, surfaceBounds);
  }
  return bounds;
}

} // namespace

EngineBounds procModelBounds(const ProcModel& model, const AreaMaterialLookup& materials)
{
  auto deforms = DeformCache{materials};
  return modelBoundsOf(model, deforms);
}

AreaData buildAreaData(const ProcTree& tree, const AreaMaterialLookup& materials)
{
  auto data = AreaData{};
  const auto numAreas = std::size_t(std::max(tree.numAreas, 0));

  // FindModel( "_areaN" ): the last model of the name
  auto models = std::vector<const ProcModel*>(numAreas, nullptr);
  for (const auto& model : tree.models)
  {
    if (model.area >= 0 && std::size_t(model.area) < numAreas)
    {
      models[std::size_t(model.area)] = &model;
    }
  }

  // RenderWorld_load.cpp:1029-1095 AddWorldModelEntities, per area
  auto deforms = DeformCache{materials};
  // :1074-1086: the world entity's axis is the identity, its origin zero;
  // R_DeriveEntityData (RenderWorld_defs.cpp:49-60) makes its modelRenderMatrix with
  // CreateFromOriginAxis
  const auto modelRenderMatrix =
    engineMatrixFromOriginAxis({0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1});
  data.areas.reserve(numAreas);
  for (auto i = std::size_t{0}; i < numAreas; ++i)
  {
    auto area = AreaInfo{};
    // the local and global reference bounds are the same for area models
    area.modelBounds =
      models[i] ? modelBoundsOf(*models[i], deforms) : engineZeroBounds();
    area.inverseBaseModelProject =
      engineOffsetScaleForBounds(modelRenderMatrix, area.modelBounds);
    area.globalBounds =
      engineProjectedBounds(area.inverseBaseModelProject, engineUnitCube(), false);
    data.areas.push_back(area);
  }

  // the portal sides and, with every portal open, the connected areas (D82)
  auto sides = areaPortalSides(tree);
  for (auto i = std::size_t{0}; i < numAreas && i < sides.size(); ++i)
  {
    data.areas[i].portals = std::move(sides[i]);
  }
  data.components = connectedComponents(tree);

  // RenderWorld_load.cpp:990 CommonChildrenArea_r( &areaNodes[0] )
  data.commonAreas = commonChildrenAreas(tree);
  return data;
}

std::vector<std::vector<AreaPortal>> areaPortalSides(const ProcTree& tree)
{
  const auto numAreas = std::size_t(std::max(tree.numAreas, 0));
  auto lists = std::vector<std::vector<AreaPortal>>(numAreas);
  const auto inRange = [&](const int area) {
    return area >= 0 && std::size_t(area) < numAreas;
  };
  for (auto i = std::size_t{0}; i < tree.portals.size(); ++i)
  {
    const auto& portal = tree.portals[i];
    const auto a1 = portal.positiveArea;
    const auto a2 = portal.negativeArea;
    if (!inRange(a1) || !inRange(a2))
    {
      continue;
    }
    auto winding = std::vector<EngineVec3>{};
    winding.reserve(portal.points.size());
    for (const auto& point : portal.points)
    {
      winding.push_back({point[0], point[1], point[2]});
    }

    // add the portal to a1
    auto front = AreaPortal{};
    front.portal = i;
    front.side = 0;
    front.intoArea = a2;
    front.points = winding;
    front.plane = engineWindingPlane(front.points).value_or(EnginePlane{});
    lists[std::size_t(a1)].push_back(std::move(front));

    // reverse it for a2
    auto back = AreaPortal{};
    back.portal = i;
    back.side = 1;
    back.intoArea = a1;
    back.points = engineReverseWinding(winding).value_or(std::vector<EngineVec3>{});
    back.plane = engineWindingPlane(back.points).value_or(EnginePlane{});
    lists[std::size_t(a2)].push_back(std::move(back));
  }
  // p->next = portalAreas[a].portals: each side was prepended
  for (auto& list : lists)
  {
    std::ranges::reverse(list);
  }
  return lists;
}

std::vector<int> connectedComponents(const ProcTree& tree)
{
  const auto numAreas = std::size_t(std::max(tree.numAreas, 0));
  const auto sides = areaPortalSides(tree);
  auto components = std::vector<int>(numAreas, -1);
  auto next = 0;
  auto stack = std::vector<std::size_t>{};
  for (auto start = std::size_t{0}; start < numAreas; ++start)
  {
    if (components[start] >= 0)
    {
      continue;
    }
    // FloodConnectedAreas from start, through every portal (none blocks)
    components[start] = next;
    stack.push_back(start);
    while (!stack.empty())
    {
      const auto area = stack.back();
      stack.pop_back();
      for (const auto& side : sides[area])
      {
        const auto into = std::size_t(side.intoArea);
        if (components[into] < 0)
        {
          components[into] = next;
          stack.push_back(into);
        }
      }
    }
    ++next;
  }
  return components;
}

bool areasConnected(const std::vector<int>& components, const int a, const int b)
{
  if (a == -1 || b == -1)
  {
    return false;
  }
  if (
    a < 0 || b < 0 || std::size_t(a) >= components.size()
    || std::size_t(b) >= components.size())
  {
    return false;
  }
  return components[std::size_t(a)] == components[std::size_t(b)];
}

} // namespace tb::lit
