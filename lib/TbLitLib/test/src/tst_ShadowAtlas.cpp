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

// LP3.0 (C59, D145, D160, P3.1): the engine's shadow-atlas packing, the copied TileMap
// and idSort_Quick with ShadowAtlasPass' comparator and placement, on the layouts the
// plan names. The orders and positions below are the copied code's own (the copied code
// is the truth, D160); where they differ from the plan review's the report says so.

#include "lit/EngineLight.h"
#include "lit/EngineShadowAtlas.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// RenderBackend_NVRHI.cpp:207-215: Init( r_shadowMapAtlasSize, shadowMapResolutions[0],
// 8 )
constexpr auto AtlasSize = 8192;

std::vector<EngineAtlasSize> sides(const std::size_t count, const int size)
{
  return std::vector<EngineAtlasSize>(count, EngineAtlasSize{size, size});
}

void print(
  const char* what,
  const std::vector<int>& order,
  const std::vector<EngineAtlasPlacement>& placements)
{
  std::cout << what << ": order";
  for (const auto side : order)
  {
    std::cout << " " << side;
  }
  std::cout << "\n";
  for (std::size_t i = 0; i < placements.size() && i < 8; ++i)
  {
    const auto& p = placements[i];
    std::cout << "  side " << i << ": (" << p.x << ", " << p.y << ") " << p.size
              << (p.placed ? "" : " not placed") << "\n";
  }
}

// no two placed tiles overlap, and each lies inside the atlas
bool disjoint(const std::vector<EngineAtlasPlacement>& placements)
{
  for (std::size_t i = 0; i < placements.size(); ++i)
  {
    const auto& a = placements[i];
    if (!a.placed)
    {
      continue;
    }
    if (a.x < 0 || a.y < 0 || a.x + a.size > AtlasSize || a.y + a.size > AtlasSize)
    {
      return false;
    }
    for (std::size_t j = i + 1; j < placements.size(); ++j)
    {
      const auto& b = placements[j];
      if (
        b.placed && a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size
        && b.y < a.y + a.size)
      {
        return false;
      }
    }
  }
  return true;
}

} // namespace

TEST_CASE("ShadowAtlas: the engine's packing on the plan's layouts (LP3.0, D160, P3.1)")
{
  auto map = EngineTileMap{};
  REQUIRE(map.init(AtlasSize, unsigned(EngineShadowMapResolutions[0]), 8));

  SECTION("(a) one point light's six 512 sides")
  {
    auto order = std::vector<int>{};
    const auto placements =
      engineShadowAtlasLayout(map, sides(6, 512), AtlasSize, &order);
    print("one point light", order, placements);
    CHECK(order == std::vector<int>{4, 3, 5, 1, 0, 2});
    // the tiles in order (SC719): (0,7680), (0,7168), (512,7168), (512,7680), (0,6656),
    // (0,6144)
    const auto at = [&](const std::size_t side) {
      return std::pair{placements[side].x, placements[side].y};
    };
    CHECK(at(4) == std::pair{0, 7680});
    CHECK(at(3) == std::pair{0, 7168});
    CHECK(at(5) == std::pair{512, 7168});
    CHECK(at(1) == std::pair{512, 7680});
    CHECK(at(0) == std::pair{0, 6656});
    CHECK(at(2) == std::pair{0, 6144});
    for (const auto& p : placements)
    {
      CHECK(p.placed);
      CHECK(p.size == 512);
    }
    CHECK(disjoint(placements));
  }

  SECTION("(b) one parallel light's four 1024 cascades")
  {
    auto order = std::vector<int>{};
    const auto placements =
      engineShadowAtlasLayout(map, sides(4, 1024), AtlasSize, &order);
    print("one parallel light", order, placements);
    CHECK(order == std::vector<int>{1, 2, 3, 0});
    // the first 1024 tile at (0, 7168), cascade 0 the fourth
    CHECK(placements[1].x == 0);
    CHECK(placements[1].y == 7168);
    CHECK(placements[0].x == 1024);
    CHECK(placements[0].y == 7168);
    for (const auto& p : placements)
    {
      CHECK(p.placed);
      CHECK(p.size == 1024);
    }
    CHECK(disjoint(placements));
  }

  SECTION("(c) 43 point lights at 512: 258 equal keys")
  {
    auto order = std::vector<int>{};
    const auto placements =
      engineShadowAtlasLayout(map, sides(43 * 6, 512), AtlasSize, &order);
    auto unplaced = std::vector<int>{};
    for (std::size_t i = 0; i < placements.size(); ++i)
    {
      if (!placements[i].placed)
      {
        unplaced.push_back(int(i));
      }
    }
    std::cout << "43 point lights: unplaced sides";
    for (const auto side : unplaced)
    {
      std::cout << " " << side << " (light " << side / 6 << ")";
    }
    std::cout << "; order head";
    for (std::size_t i = 0; i < 8; ++i)
    {
      std::cout << " " << order[i];
    }
    std::cout << " ... tail";
    for (std::size_t i = order.size() - 4; i < order.size(); ++i)
    {
      std::cout << " " << order[i];
    }
    std::cout << "\n";
    // the review's run of the copied loop: [256, 255 ... 129, 257, 127 ... 0, 128]
    CHECK(order.front() == 256);
    CHECK(order.back() == 128);
    auto expected = std::vector<int>{};
    for (auto i = 256; i >= 129; --i)
    {
      expected.push_back(i);
    }
    expected.push_back(257);
    for (auto i = 127; i >= 0; --i)
    {
      expected.push_back(i);
    }
    expected.push_back(128);
    CHECK(order == expected);
    CHECK(unplaced == std::vector<int>{0, 128});
    // so lights 0 and 21 of the input order are unshadowed (a failed side unshadows its
    // light, its placed sides still drawn: SC720)
    auto lights = std::set<int>{};
    for (const auto side : unplaced)
    {
      lights.insert(side / 6);
    }
    CHECK(lights == std::set<int>{0, 21});
    // an unplaced side keeps its asked size at (-1, -1)
    CHECK(placements[0].x == -1);
    CHECK(placements[0].y == -1);
    CHECK(placements[0].size == 512);
    CHECK(disjoint(placements));
  }

  SECTION("(d) 42 point lights at 512: all 252 placed")
  {
    const auto placements = engineShadowAtlasLayout(map, sides(42 * 6, 512), AtlasSize);
    CHECK(std::ranges::all_of(placements, [](const auto& p) { return p.placed; }));
    CHECK(disjoint(placements));
  }

  SECTION("(e) the capacity of each size (SC719)")
  {
    for (const auto& [size, capacity] : std::vector<std::pair<int, std::size_t>>{
           {1024, 64}, {512, 256}, {256, 1024}, {128, 4096}, {64, 16384}})
    {
      CAPTURE(size);
      map.clear();
      auto count = std::size_t{0};
      while (map.getTile(float(size)))
      {
        ++count;
      }
      CHECK(count == capacity);
      const auto placements =
        engineShadowAtlasLayout(map, sides(capacity + 1, size), AtlasSize);
      CHECK(
        std::size_t(
          std::ranges::count_if(placements, [](const auto& p) { return p.placed; }))
        == capacity);
    }
    // a size is clamped to [64, 1024]: 2048 asks for a 1024 tile, 32 for a 64 one
    map.clear();
    const auto big = map.getTile(2048.0f);
    REQUIRE(big);
    CHECK(big->size == 1024.0f / 8192.0f);
    const auto small = map.getTile(32.0f);
    REQUIRE(small);
    CHECK(small->size == 64.0f / 8192.0f);
  }

  SECTION("a mixed list: largest first, a 1024 cascade before every 512 side")
  {
    auto list = sides(6, 512);
    const auto cascades = sides(4, 1024);
    list.insert(list.end(), cascades.begin(), cascades.end());
    auto order = std::vector<int>{};
    const auto placements = engineShadowAtlasLayout(map, list, AtlasSize, &order);
    print("a point and a parallel light", order, placements);
    for (std::size_t i = 0; i < 4; ++i)
    {
      CHECK(order[i] >= 6);
    }
    CHECK(disjoint(placements));
  }
}

TEST_CASE("ShadowAtlas: an uninitialised or invalid tile map (LP3.0)")
{
  // TileMap::Init's checks (TileMap.cpp:45-65): a power of two, at least one level, the
  // largest tile at most the map and at least 16, the smallest at least 16
  auto map = EngineTileMap{};
  CHECK_FALSE(map.init(8000, 1024, 8));
  CHECK_FALSE(map.init(8192, 16384, 8));
  CHECK_FALSE(map.init(8192, 1024, 0));
  CHECK_FALSE(map.init(1024, 1024, 8));
  CHECK(engineSortAtlasSizes({}).empty());
  // sizes of three kinds: largest first by x² + y²
  CHECK(
    engineSortAtlasSizes({{256, 256}, {1024, 1024}, {512, 512}})
    == std::vector<int>{1, 2, 0});
}

} // namespace tb::lit
