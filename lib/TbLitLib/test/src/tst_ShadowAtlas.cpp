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

#include "ShadowCache.h"
#include "lit/EngineLight.h"
#include "lit/EngineShadowAtlas.h"

#include <algorithm>
#include <iostream>
#include <map>
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

namespace
{

// LP3.3: the allocator's first fit of sizes in the engine's sort order over an empty map
std::vector<EngineAtlasPlacement> firstFit(
  ShadowTileAllocator& allocator, const std::vector<EngineAtlasSize>& sizes)
{
  auto placements = std::vector<EngineAtlasPlacement>(sizes.size());
  for (const auto index : engineSortAtlasSizes(sizes))
  {
    const auto size = std::max(sizes[std::size_t(index)].x, sizes[std::size_t(index)].y);
    auto& placement = placements[std::size_t(index)];
    placement.size = size;
    if (const auto rect = allocator.allocate(size))
    {
      placement.x = rect->x;
      placement.y = rect->y;
      placement.placed = true;
    }
  }
  return placements;
}

/**
 * LP3.3 (P3.4b): a relocation plan run on a model atlas and scratch of 64-texel cells,
 * each cell's content the id of the tile it held: whether every move's destination ends
 * with its source's content as it was, and every tile no move touched keeps its own.
 */
bool relocates(
  const std::vector<ShadowMove>& moves,
  const std::vector<AtlasRect>& still,
  const ShadowRelocationPlan& plan)
{
  constexpr auto Cell = 64;
  auto atlas = std::map<std::pair<int, int>, int>{};
  auto scratch = std::map<std::pair<int, int>, int>{};
  const auto fill = [&](const AtlasRect& rect, const int id) {
    for (auto y = rect.y; y < rect.y + rect.size; y += Cell)
    {
      for (auto x = rect.x; x < rect.x + rect.size; x += Cell)
      {
        atlas[{x, y}] = id * 1000000 + (y - rect.y) * 1000 + (x - rect.x);
      }
    }
  };
  for (std::size_t m = 0; m < moves.size(); ++m)
  {
    fill(moves[m].from, int(m) + 1);
  }
  for (std::size_t s = 0; s < still.size(); ++s)
  {
    fill(still[s], int(moves.size() + s) + 1);
  }
  const auto expected = atlas;
  for (const auto& copy : plan.copies)
  {
    for (auto dy = 0; dy < copy.atlas.size; dy += Cell)
    {
      for (auto dx = 0; dx < copy.atlas.size; dx += Cell)
      {
        const auto a = std::pair{copy.atlas.x + dx, copy.atlas.y + dy};
        const auto s = std::pair{copy.scratch.x + dx, copy.scratch.y + dy};
        if (copy.save)
        {
          scratch[s] = atlas[a];
        }
        else
        {
          atlas[a] = scratch[s];
        }
      }
    }
  }
  for (std::size_t m = 0; m < moves.size(); ++m)
  {
    if (std::ranges::find(plan.redraw, m) != plan.redraw.end())
    {
      continue;
    }
    const auto& move = moves[m];
    for (auto dy = 0; dy < move.from.size; dy += Cell)
    {
      for (auto dx = 0; dx < move.from.size; dx += Cell)
      {
        if (
          atlas[{move.to.x + dx, move.to.y + dy}]
          != expected.at({move.from.x + dx, move.from.y + dy}))
        {
          return false;
        }
      }
    }
  }
  for (const auto& rect : still)
  {
    for (auto y = rect.y; y < rect.y + rect.size; y += Cell)
    {
      for (auto x = rect.x; x < rect.x + rect.size; x += Cell)
      {
        if (atlas[{x, y}] != expected.at({x, y}))
        {
          return false;
        }
      }
    }
  }
  return true;
}

// the number of save / restore phases of a plan (a phase: copies of one kind in a row)
std::size_t phasesOf(const ShadowRelocationPlan& plan)
{
  auto phases = std::size_t{0};
  for (std::size_t i = 0; i < plan.copies.size(); ++i)
  {
    phases += i == 0 || plan.copies[i].save != plan.copies[i - 1].save ? 1 : 0;
  }
  return phases;
}

} // namespace

TEST_CASE("ShadowAtlas: the held layout's free-rect structure (LP3.3, C62, D152)")
{
  auto map = EngineTileMap{};
  REQUIRE(map.init(AtlasSize, unsigned(EngineShadowMapResolutions[0]), 8));
  auto allocator = ShadowTileAllocator{AtlasSize, 64};

  SECTION("first fit over an empty map, in the engine's order, is the engine's packing")
  {
    auto mixed = sides(6, 512);
    const auto cascades = sides(4, 1024);
    mixed.insert(mixed.end(), cascades.begin(), cascades.end());
    const auto more = sides(12, 256);
    mixed.insert(mixed.end(), more.begin(), more.end());
    for (const auto& list :
         {sides(6, 512), sides(4, 1024), sides(258, 512), sides(252, 512), mixed})
    {
      CAPTURE(list.size());
      allocator.clear();
      const auto engine = engineShadowAtlasLayout(map, list, AtlasSize);
      const auto fitted = firstFit(allocator, list);
      REQUIRE(engine.size() == fitted.size());
      auto mismatches = 0;
      for (std::size_t i = 0; i < engine.size(); ++i)
      {
        mismatches +=
          engine[i].placed != fitted[i].placed
          || (engine[i].placed && (engine[i].x != fitted[i].x || engine[i].y != fitted[i].y));
      }
      CHECK(mismatches == 0);
    }
    // D160's orders: side 4 at (0, 7680); cascade 0 the fourth 1024, at (1024, 7168); 43
    // lights: shadowIndex 0 and 128 without a tile
    allocator.clear();
    CHECK(firstFit(allocator, sides(6, 512))[4].y == 7680);
    allocator.clear();
    const auto parallel = firstFit(allocator, sides(4, 1024));
    CHECK(std::pair{parallel[0].x, parallel[0].y} == std::pair{1024, 7168});
    allocator.clear();
    const auto overflow = firstFit(allocator, sides(258, 512));
    CHECK_FALSE(overflow[0].placed);
    CHECK_FALSE(overflow[128].placed);
    CHECK(allocator.taken() == 256u);
  }

  SECTION(
    "tiles freed and taken in any order; a node over or under a taken one is not free")
  {
    const auto a = allocator.allocate(512);
    REQUIRE(a);
    CHECK(*a == AtlasRect{0, 7680, 512});
    // the 1024 node holding it is not free; the next 1024 is
    const auto b = allocator.allocate(1024);
    REQUIRE(b);
    CHECK(*b == AtlasRect{0, 6144, 1024});
    CHECK(allocator.taken() == 2u);
    // a 256 goes into the first free 256 node: beside the 512, inside its 1024
    const auto c = allocator.allocate(256);
    REQUIRE(c);
    CHECK(*c == AtlasRect{0, 7424, 256});
    allocator.release(*a);
    CHECK(allocator.taken() == 2u);
    // the freed 512 is free again, the 1024 over it is not (the 256 is under it)
    CHECK(allocator.allocate(512) == std::optional{AtlasRect{0, 7680, 512}});
    CHECK_FALSE(allocator.reserve(AtlasRect{0, 7168, 1024}));
    CHECK_FALSE(allocator.reserve(AtlasRect{0, 7680, 512}));
    // reserve takes a free node of the engine's placement, never a rect that is none
    CHECK(allocator.reserve(AtlasRect{4096, 4096, 1024}));
    CHECK_FALSE(allocator.reserve(AtlasRect{4100, 4096, 1024}));
    CHECK(allocator.taken() == 4u);
    allocator.clear();
    CHECK(allocator.taken() == 0u);
    // a size above the largest node, none
    CHECK_FALSE(allocator.allocate(8192));
  }

  SECTION("the vacated parts: what no tile of the frame covers (D147, P3.3)")
  {
    const auto big = AtlasRect{0, 6144, 1024};
    CHECK(uncoveredRects(big, {}, 64) == std::vector<AtlasRect>{big});
    const auto inside = std::vector<AtlasRect>{AtlasRect{0, 6144, 512}};
    const auto parts = uncoveredRects(big, inside, 64);
    CHECK(
      parts
      == std::vector<AtlasRect>{
        AtlasRect{512, 6144, 512}, AtlasRect{0, 6656, 512}, AtlasRect{512, 6656, 512}});
    const auto over = std::vector<AtlasRect>{AtlasRect{0, 6144, 2048}};
    CHECK(uncoveredRects(big, over, 64).empty());
    const auto elsewhere = std::vector<AtlasRect>{AtlasRect{4096, 0, 1024}};
    CHECK(uncoveredRects(big, elsewhere, 64) == std::vector<AtlasRect>{big});
  }
}

TEST_CASE("ShadowAtlas: the relocation's permutation order (LP3.3, D146, P3.4)")
{
  SECTION("moves that fit the scratch: every source saved before any destination")
  {
    // a swap (a cycle of two) and a chain into a free rect
    const auto moves = std::vector<ShadowMove>{
      {AtlasRect{0, 7680, 512}, AtlasRect{512, 7680, 512}},
      {AtlasRect{512, 7680, 512}, AtlasRect{0, 7680, 512}},
      {AtlasRect{0, 7168, 512}, AtlasRect{1024, 7168, 512}},
    };
    const auto plan = planShadowRelocation(moves, 2048, 64);
    CHECK(plan.redraw.empty());
    REQUIRE(plan.copies.size() == 6u);
    CHECK(phasesOf(plan) == 2u);
    for (std::size_t i = 0; i < 3; ++i)
    {
      CHECK(plan.copies[i].save);
      CHECK_FALSE(plan.copies[i + 3].save);
    }
    CHECK(relocates(moves, {AtlasRect{0, 6144, 1024}}, plan));
  }

  SECTION("more than the scratch holds: batches, chains from the free end, cycles")
  {
    // a chain of six 1024 tiles (each into the next's rect, the last into a free one)
    // and a cycle of six (each into the next's, the last into the first's): 12 tiles,
    // the scratch holds four
    auto moves = std::vector<ShadowMove>{};
    for (auto i = 0; i < 6; ++i)
    {
      moves.push_back({AtlasRect{1024 * i, 0, 1024}, AtlasRect{1024 * (i + 1), 0, 1024}});
    }
    for (auto i = 0; i < 6; ++i)
    {
      moves.push_back(
        {AtlasRect{1024 * i, 2048, 1024}, AtlasRect{1024 * ((i + 1) % 6), 2048, 1024}});
    }
    const auto plan = planShadowRelocation(moves, 2048, 64);
    CHECK(plan.redraw.empty());
    CHECK(relocates(moves, {AtlasRect{0, 4096, 1024}}, plan));
    CHECK(phasesOf(plan) > 2u);
    // every move restored once
    auto restored = std::set<std::size_t>{};
    for (const auto& copy : plan.copies)
    {
      if (!copy.save)
      {
        CHECK(restored.insert(copy.move).second);
      }
    }
    CHECK(restored.size() == moves.size());
    std::cout << "LP3.3 relocation: 12 moves of 1024 through a 2048 scratch, "
              << plan.copies.size() << " copies in " << phasesOf(plan) << " phases\n";
  }

  SECTION("sizes mixed: a 1024 destination over four 512 sources that move away")
  {
    const auto moves = std::vector<ShadowMove>{
      {AtlasRect{0, 0, 512}, AtlasRect{2048, 0, 512}},
      {AtlasRect{512, 0, 512}, AtlasRect{2560, 0, 512}},
      {AtlasRect{0, 512, 512}, AtlasRect{2048, 512, 512}},
      {AtlasRect{512, 512, 512}, AtlasRect{2560, 512, 512}},
      {AtlasRect{4096, 0, 1024}, AtlasRect{0, 0, 1024}},
    };
    const auto plan = planShadowRelocation(moves, 2048, 64);
    CHECK(plan.redraw.empty());
    CHECK(relocates(moves, {}, plan));
    CHECK(phasesOf(plan) == 2u);
  }

  SECTION("a cycle the scratch cannot hold is drawn again")
  {
    // two 1024 tiles swapping through a 512 scratch: no slot for either
    const auto moves = std::vector<ShadowMove>{
      {AtlasRect{0, 0, 1024}, AtlasRect{1024, 0, 1024}},
      {AtlasRect{1024, 0, 1024}, AtlasRect{0, 0, 1024}},
    };
    const auto plan = planShadowRelocation(moves, 1024, 64);
    CHECK(plan.redraw.size() == 2u);
    CHECK(plan.copies.empty());
    CHECK(planShadowRelocation({}, 2048, 64).copies.empty());
  }
}

} // namespace tb::lit
