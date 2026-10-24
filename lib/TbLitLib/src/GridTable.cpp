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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_local.h,
// renderer/RenderWorld_lightgrid.cpp and renderer/RenderBackend.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/GridTable.h"

#include "lit/LightGridFile.h"
#include "lit/MapBake.h"
#include "lit/ProcFile.h"

#include <algorithm>
#include <utility>

namespace tb::lit
{

const GridArea* GridTable::find(const int area) const
{
  return area >= 0 && std::size_t(area) < areas.size() ? &areas[std::size_t(area)]
                                                       : nullptr;
}

bool GridTable::hasPoints(const int area) const
{
  const auto* grid = find(area);
  return grid && grid->hasPoints;
}

std::vector<GridArea> gridAreasOf(const MapBake& bake)
{
  // SetupLightGrid runs only with a world (InitFromMap), and the preview's drop (§3.7,
  // K29) leaves no grid
  if (!bake.tree || !bake.lightGrid || !bake.dropReason.empty())
  {
    return {};
  }
  const auto numAreas = bake.tree->numAreas;
  auto areas = std::vector<GridArea>(std::size_t(std::max(numAreas, 0)));
  for (auto i = 0; i < numAreas; ++i)
  {
    auto& grid = areas[std::size_t(i)];
    grid.area = i;
    // LoadLightGridImages (RenderWorld_lightgrid.cpp:430-438): every area's atlas
    grid.atlas = lightGridImageKey(bake.bakeBase, i);
  }
  // ParseLightGridPoints and ReadBinaryLightGridPoints (:694-781, :783-845) write the
  // entry's fields into portalAreas[ area ].lightGrid, the file's order: a later entry of
  // the same area overwrites an earlier one
  for (const auto& entry : bake.lightGrid->areas)
  {
    if (entry.area < 0 || entry.area >= numAreas)
    {
      continue;
    }
    auto& grid = areas[std::size_t(entry.area)];
    grid.origin = entry.origin;
    grid.size = entry.size;
    grid.bounds = entry.bounds;
    grid.probeSize = entry.probeSize;
    grid.borderSize = entry.borderSize;
    grid.hasPoints = !entry.points.empty();
  }
  return areas;
}

GridParms engineGridParms(const GridArea& area, const int width, const int height)
{
  auto parms = GridParms{};
  // RenderBackend.cpp:1225-1235: use rpGlobalLightOrigin for lightGrid center
  parms.globalLightOrigin = {area.origin[0], area.origin[1], area.origin[2], 1.0f};
  parms.jitterTexScale = {area.size[0], area.size[1], area.size[2], 1.0f};
  parms.jitterTexOffset = {
    float(area.bounds[0]), float(area.bounds[1]), float(area.bounds[2]), 1.0f};
  // :1237-1244: individual probe sizes on the atlas image
  parms.screenCorrectionFactor = {
    float(area.probeSize - area.borderSize),
    float(area.probeSize),
    float(area.borderSize),
    area.probeSize != 0 ? float(area.probeSize - area.borderSize) / float(area.probeSize)
                        : 0.0f};
  // :1273-1276: idVec4 textureSize( res.x, res.y, 1.0f / res.x, 1.0f / res.y )
  const auto w = float(width);
  const auto h = float(height);
  parms.cascadeDistances = {w, h, 1.0f / w, 1.0f / h};
  return parms;
}

GridTablePublication::GridTablePublication()
  : m_table{std::make_shared<const GridTable>()}
{
}

std::shared_ptr<const GridTable> GridTablePublication::table() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_table;
}

void GridTablePublication::publish(std::shared_ptr<const GridTable> table)
{
  auto previous = std::shared_ptr<const GridTable>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    previous = std::exchange(m_table, std::move(table));
  }
  // the table replaced is freed outside the lock
}

} // namespace tb::lit
