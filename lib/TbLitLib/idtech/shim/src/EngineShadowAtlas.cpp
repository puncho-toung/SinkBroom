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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp), and it calls the
// copied neo/idlib/TileMap.cpp and neo/idlib/containers/Sort.h:
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP3.0 (C59, D145, D160): the facade of lit/EngineShadowAtlas.h.

#include "lit/EngineShadowAtlas.h"

#include "idlib/TileMap.h"
#include "precompiled.h"

namespace tb::lit
{
namespace
{

// RenderBackend.cpp:3587-3601 idSortrects, over the sides' sizes (idVec2i there)
class idSortrects : public idSort_Quick<int, idSortrects>
{
public:
  int SizeMetric(const EngineAtlasSize v) const
  {
    // skinny rects will sort earlier than square ones, because
    // they are more likely to grow the entire region
    return v.x * v.x + v.y * v.y;
  }
  int Compare(const int& a, const int& b) const
  {
    return SizeMetric((*inputSizes)[std::size_t(b)])
           - SizeMetric((*inputSizes)[std::size_t(a)]);
  }
  const std::vector<EngineAtlasSize>* inputSizes = nullptr;
};

} // namespace

EngineTileMap::EngineTileMap()
  : m_map{std::make_unique<TileMap>()}
{
}

EngineTileMap::~EngineTileMap() = default;

bool EngineTileMap::init(
  const unsigned int mapSize,
  const unsigned int maxAbsTileSize,
  const unsigned int numLevels)
{
  return m_map->Init(mapSize, maxAbsTileSize, numLevels);
}

void EngineTileMap::clear()
{
  m_map->Clear();
}

std::optional<EngineTile> EngineTileMap::getTile(const float size)
{
  Tile tile;
  if (!m_map->GetTile(size, tile))
  {
    return std::nullopt;
  }
  return EngineTile{tile.position.x, tile.position.y, tile.size};
}

std::vector<int> engineSortAtlasSizes(const std::vector<EngineAtlasSize>& sizes)
{
  // :3728-3738
  idList<int> sizeRemap;
  sizeRemap.SetNum(int(sizes.size()));
  for (int i = 0; i < int(sizes.size()); i++)
  {
    sizeRemap[i] = i;
  }

  // Sort the rects from largest to smallest (it makes allocating them in the image
  // better)
  idSortrects sortrectsBySize;
  sortrectsBySize.inputSizes = &sizes;
  sizeRemap.SortWithTemplate(sortrectsBySize);

  auto order = std::vector<int>{};
  order.reserve(sizes.size());
  for (int i = 0; i < sizeRemap.Num(); i++)
  {
    order.push_back(sizeRemap[i]);
  }
  return order;
}

std::vector<EngineAtlasPlacement> engineShadowAtlasLayout(
  EngineTileMap& tileMap,
  const std::vector<EngineAtlasSize>& sizes,
  const int atlasSize,
  std::vector<int>* order)
{
  // :3718-3726: outputPositions and outputSizes, one per side
  auto output = std::vector<EngineAtlasPlacement>(sizes.size());

  const auto sizeRemap = engineSortAtlasSizes(sizes);
  if (order)
  {
    *order = sizeRemap;
  }

  // :3740
  tileMap.clear();

  // :3742-3783
  for (std::size_t i = 0; i < sizes.size(); i++)
  {
    const auto shadowIndex = std::size_t(sizeRemap[i]);

    const auto size = sizes[shadowIndex];

    int area = Max(size.x, size.y);

    const auto tile = tileMap.getTile(float(area));

    if (!tile)
    {
      output[shadowIndex] = EngineAtlasPlacement{-1, -1, area, false};
    }
    else
    {
      int imageSize = int(tile->size * float(atlasSize));

      // convert from [-1..-1] -> [0..1] and flip y
      idVec2 uvPos;
      uvPos.x = tile->x * 0.5f + 0.5f;
      uvPos.y = tile->y * 0.5f + 0.5f;

      int iPosX = int(uvPos.x * float(atlasSize));
      int iPosY = int(uvPos.y * float(atlasSize));

      // RB: this is really odd but necessary
      iPosX = int(float(iPosX) - float(imageSize) * 0.5f);
      iPosY = int(float(iPosY) - float(imageSize) * 0.5f);

      output[shadowIndex] = EngineAtlasPlacement{iPosX, iPosY, imageSize, true};
    }
  }
  return output;
}

} // namespace tb::lit
