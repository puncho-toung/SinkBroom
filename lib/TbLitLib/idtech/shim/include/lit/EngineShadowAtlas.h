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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp and
// renderer/NVRHI/RenderBackend_NVRHI.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// LP3.0 (C59, D145, D160): the facade over the engine's shadow-atlas packing: the copied
// TileMap (idlib/TileMap.{h,cpp}, whole) and idSort_Quick (idlib/containers/Sort.h,
// whole), with the sort-rects comparator and the placement arithmetic of ShadowAtlasPass
// transcribed (RenderBackend.cpp:3587-3601, 3718-3783). Standard types only; C++17 and
// C++20. Threading: an EngineTileMap is its caller's alone (the copied TileMap reads and
// writes only its own nodes, allocating them once in init); the functions are pure. LP3.1
// runs them on the render thread (mirror.json's pure-function entry).

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

class TileMap;

namespace tb::lit
{

// a tile the copied TileMap found: Tile::position, the node's centre in [-1, 1]², and
// Tile::size, the asked size (clamped) over the map's
struct EngineTile
{
  float x = 0.0f;
  float y = 0.0f;
  float size = 0.0f;
};

/**
 * The renderer's tileMap (RenderCommon.h), the copied TileMap behind a pointer: init is
 * TileMap::Init( mapSize, maxAbsTileSize, numLevels ) (TileMap.cpp:45-84; the backend's
 * Init( r_shadowMapAtlasSize 8192, shadowMapResolutions[0] 1024, 8 ), RenderBackend_NVRHI
 * .cpp:207-215: 21,845 nodes, the smallest tile 64), clear TileMap::Clear (:111-117),
 * getTile TileMap::GetTile (:119-137: the size clamped to [64, 1024], the required level
 * 13 - ceil( log2( size ) ), the first fit depth first in child order; none: nullopt).
 */
class EngineTileMap
{
public:
  EngineTileMap();
  ~EngineTileMap();

  EngineTileMap(const EngineTileMap&) = delete;
  EngineTileMap& operator=(const EngineTileMap&) = delete;

  bool init(unsigned int mapSize, unsigned int maxAbsTileSize, unsigned int numLevels);
  void clear();
  std::optional<EngineTile> getTile(float size);

private:
  std::unique_ptr<TileMap> m_map;
};

// a shadow map's side, as ShadowAtlasPass' inputSizes hold it (idVec2i)
struct EngineAtlasSize
{
  int x = 0;
  int y = 0;
};

/**
 * ShadowAtlasPass' sort of the sides (RenderBackend.cpp:3728-3738): the indices 0..n-1
 * sorted by the copied idSort_Quick (unstable: equal keys come out in its own order) with
 * idSortrects' comparator (:3587-3601, transcribed): SizeMetric( v ) = v.x · v.x + v.y ·
 * v.y, Compare( a, b ) = SizeMetric( sizes[b] ) - SizeMetric( sizes[a] ), largest first.
 * Pure.
 */
std::vector<int> engineSortAtlasSizes(const std::vector<EngineAtlasSize>& sizes);

// a side's place in the atlas: its top-left pixel and its side (outputPositions,
// outputSizes); placed false: no tile ((-1, -1), its asked size)
struct EngineAtlasPlacement
{
  int x = -1;
  int y = -1;
  int size = 0;
  bool placed = false;
};

/**
 * ShadowAtlasPass' layout (RenderBackend.cpp:3718-3783) of sizes, the sides in the order
 * loop 1 appended them: engineSortAtlasSizes, then tileMap.clear(), then per sorted side
 * area = Max( x, y ), tileMap.getTile( area ); a side without a tile is (-1, -1) and
 * area; else imageSize = int( tile.size · atlasSize ), uv = position · 0.5 + 0.5, iPos =
 * int( uv · atlasSize ) - imageSize · 0.5 (back to int), the tile's top-left pixel
 * (SC719). The placements by side, as outputPositions and outputSizes index them; order,
 * when not null, takes the sorted order. Pure but for tileMap, which it clears and fills.
 */
std::vector<EngineAtlasPlacement> engineShadowAtlasLayout(
  EngineTileMap& tileMap,
  const std::vector<EngineAtlasSize>& sizes,
  int atlasSize,
  std::vector<int>* order = nullptr);

} // namespace tb::lit
