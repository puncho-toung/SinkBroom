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

#pragma once

#include "base/Macros.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// LP4.3 (C36, D88, SC334-SC341): the map's light grid as the render thread reads it: per
// portal area its LightGrid fields (RenderWorld_local.h:71-128) from the bake's
// .lightgrid or .blightgrid, whether it has points, and the image key of its irradiance
// atlas, which LoadLightGridImages loads for every area (RenderWorld_lightgrid.cpp:
// 415-447). The grid points themselves stay in the bake: the preview reads only whether
// an area has any (lightGridPoints.Num(), tr_frontend_addmodels.cpp:831, 845).

namespace tb::lit
{
struct MapBake;

/**
 * One portal area's LightGrid (RenderWorld_local.h:71-128) as LoadLightGridFile leaves it
 * for the frontend and the backend: lightGridOrigin, lightGridSize, lightGridBounds,
 * imageSingleProbeSize and imageBorderSize from the file's entry of the area, whether
 * lightGridPoints is non-empty, and the key of its irradianceImage
 * (lightGridImageKey( bakeBase, area ): "env/<base>/area<N>_lightgrid_amb", #__2100). An
 * area the file has no entry for keeps LightGrid's constructor values (:58-66: size
 * defaultLightGridSize, the sizes 16 and 2) and no points; readMapBake's validation
 * (§3.7, K29) drops a grid that does not name every area once, so a published table
 * never holds one.
 */
struct GridArea
{
  int area = -1;
  std::array<float, 3> origin = {0, 0, 0};
  std::array<float, 3> size = {64, 64, 128};
  std::array<int, 3> bounds = {0, 0, 0};
  int probeSize = 16;
  int borderSize = 2;
  bool hasPoints = false;
  MaterialImageKey atlas;

  bool operator==(const GridArea&) const = default;
};

/**
 * The published light grid of the scene's bake (C36, D88, like ProbeTable): immutable
 * once published, one GridArea per portal area of the bake's tree, indexed by the area
 * number; empty when the bake has no tree, no grid, or was dropped (dropReason). version
 * grows whenever the areas change (the lists' key carries it, D79).
 */
struct GridTable
{
  std::uint64_t version = 0;
  std::vector<GridArea> areas;

  // the area's grid, or null for an area outside the table
  const GridArea* find(int area) const;
  // lightGridPoints.Num() != 0 of an area in the table
  bool hasPoints(int area) const;
};

/**
 * The areas of a bake's light grid (C36): with a tree, a light grid and no drop reason,
 * per area 0 .. numAreas - 1 of the tree the file's entry of that area (the last one,
 * where a mismatched grid would name it twice; a validated one names it once) and
 * lightGridImageKey( bakeBase, area ); otherwise none. The version is the publisher's.
 * Pure; any thread (the bake is immutable).
 */
std::vector<GridArea> gridAreasOf(const MapBake& bake);

/**
 * RenderBackend.cpp:1225-1244 and :1273-1276: the light-grid path's per-draw parms of an
 * area whose atlas texture is width × height texels: GLOBALLIGHTORIGIN = ( origin, 1 ),
 * JITTERTEXSCALE = ( size, 1 ), JITTERTEXOFFSET = ( bounds as floats, 1 ),
 * SCREENCORRECTIONFACTOR = ( single - border, single, border, float( single - border ) /
 * single ) (14, 16, 2, 0.875 for an engine bake), CASCADEDISTANCES = ( w, h, 1 / w, 1 / h
 * ) of GetUploadResolution, in that order. Pure.
 */
struct GridParms
{
  std::array<float, 4> globalLightOrigin = {};
  std::array<float, 4> jitterTexScale = {};
  std::array<float, 4> jitterTexOffset = {};
  std::array<float, 4> screenCorrectionFactor = {};
  std::array<float, 4> cascadeDistances = {};

  bool operator==(const GridParms&) const = default;
};
GridParms engineGridParms(const GridArea& area, int width, int height);

/**
 * Where the scene publishes its grid tables (R2.9, as ProbeTablePublication): the session
 * owns it, the render thread's scene publishes into it, any thread reads the latest
 * table, also after the render thread has ended. Never null: an empty table at first.
 */
class GridTablePublication
{
private:
  mutable std::mutex m_mutex;
  std::shared_ptr<const GridTable> m_table;

public:
  GridTablePublication();

  // the latest table; any thread
  std::shared_ptr<const GridTable> table() const;
  void publish(std::shared_ptr<const GridTable> table);

  deleteCopyAndMove(GridTablePublication);
};

} // namespace tb::lit
