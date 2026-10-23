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
  // Model.cpp:1203-1208, :1330-1333: without surfaces a point at the origin
  if (model.surfaces.empty())
  {
    return engineZeroBounds();
  }
  // :1334-1363
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

  // RenderWorld_load.cpp:990 CommonChildrenArea_r( &areaNodes[0] )
  data.commonAreas = commonChildrenAreas(tree);
  return data;
}

} // namespace tb::lit
