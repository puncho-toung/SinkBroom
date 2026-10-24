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

// LP4.3 (C36, D88): the grid table the scene publishes from its bake's light grid, the
// grid parms of an area, and (hidden, [.][retail]) admin's table from the retail bake.

#include "LitTestUtils.h"
#include "lit/GameDeclSource.h"
#include "lit/GridTable.h"
#include "lit/LightGridFile.h"
#include "lit/MapBake.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// two areas, a 2 x 2 x 1 grid each (WriteLightGrid's layout, RenderWorld_lightgrid.cpp:
// 449-524): area 0 of the LP2.2a fixture's east room, area 1 of its west room
const auto TwoAreaGrid = std::string{
  "LGRID \"4\"\n\n"
  "lightGridPoints { /* area = */ 0 /* numLightGridPoints = */ 4 "
  "/* imageSingleProbeSize = */ 16 /* imageBorderSize = */ 2 \n"
  "/* gridMins */ ( 32.000000 -96.000000 0.000000 )\n"
  "/* gridSize */ ( 192.000000 192.000000 256.000000 )\n"
  "/* gridBounds */ 2 2 1\n\n"
  " 1 ( 32 -96 0 ) 1 ( 224 -96 0 ) 1 ( 32 96 0 ) 1 ( 224 96 0 )}\n\n"
  "lightGridPoints { /* area = */ 1 /* numLightGridPoints = */ 4 "
  "/* imageSingleProbeSize = */ 16 /* imageBorderSize = */ 2 \n"
  "/* gridMins */ ( -224.000000 -96.000000 0.000000 )\n"
  "/* gridSize */ ( 192.000000 192.000000 256.000000 )\n"
  "/* gridBounds */ 2 2 1\n\n"
  " 1 ( -224 -96 0 ) 1 ( -32 -96 0 ) 1 ( -224 96 0 ) 1 ( -32 96 0 )}\n\n"};

MapBake twoAreaBake(const std::string& gridText)
{
  auto bake = MapBake{};
  bake.mapName = "maps/lp43";
  bake.bakeBase = "maps/lp43";
  auto tree = ProcTree{};
  tree.numAreas = 2;
  bake.tree = std::make_shared<const ProcTree>(std::move(tree));
  bake.geometry = MapBake::Geometry::Proc;
  bake.grid = MapBake::Grid::Text;
  bake.lightGrid = std::make_shared<const LightGrid>(
    readLightGridText(gridText, 2) | kdl::if_error([](const auto& e) { FAIL(e.msg); })
    | kdl::value());
  return bake;
}

} // namespace

TEST_CASE("GridTable")
{
  SECTION("gridAreasOf: a synthetic two-area grid")
  {
    const auto bake = twoAreaBake(TwoAreaGrid);
    const auto areas = gridAreasOf(bake);
    REQUIRE(areas.size() == 2u);
    for (auto i = 0; i < 2; ++i)
    {
      CAPTURE(i);
      const auto& area = areas[std::size_t(i)];
      CHECK(area.area == i);
      CHECK(area.hasPoints);
      CHECK(area.bounds == std::array<int, 3>{2, 2, 1});
      CHECK(area.size == std::array<float, 3>{192, 192, 256});
      CHECK(area.probeSize == 16);
      CHECK(area.borderSize == 2);
      CHECK(area.atlas == lightGridImageKey("maps/lp43", i));
    }
    CHECK(areas[0].origin == std::array<float, 3>{32, -96, 0});
    CHECK(areas[1].origin == std::array<float, 3>{-224, -96, 0});
    CHECK(areas[0].atlas.name == "env/maps/lp43/area0_lightgrid_amb");
    CHECK(areas[0].atlas.usage == 21);
    CHECK(areas[0].atlas.cube == 0);

    auto table = GridTable{};
    table.areas = areas;
    CHECK(table.find(1) == &table.areas[1]);
    CHECK(table.find(2) == nullptr);
    CHECK(table.find(-1) == nullptr);
    CHECK(table.hasPoints(0));
    CHECK_FALSE(table.hasPoints(2));
  }

  SECTION("gridAreasOf: an area with no points keeps its fields, hasPoints false")
  {
    auto text = TwoAreaGrid;
    const auto second = text.find("lightGridPoints", text.find("lightGridPoints") + 1);
    text = text.substr(0, second)
           + "lightGridPoints { 1 0 16 2 ( 0 0 256 ) ( 128 128 256 ) 3 3 0 }\n";
    const auto areas = gridAreasOf(twoAreaBake(text));
    REQUIRE(areas.size() == 2u);
    CHECK(areas[0].hasPoints);
    CHECK_FALSE(areas[1].hasPoints);
    CHECK(areas[1].bounds == std::array<int, 3>{3, 3, 0});
    CHECK(areas[1].origin == std::array<float, 3>{0, 0, 256});
    // its atlas is asked for all the same (LoadLightGridImages loads every area's)
    CHECK(areas[1].atlas.name == "env/maps/lp43/area1_lightgrid_amb");
  }

  SECTION("gridAreasOf: none without a grid, without a tree, or with a dropped bake")
  {
    auto noGrid = twoAreaBake(TwoAreaGrid);
    noGrid.lightGrid.reset();
    noGrid.grid = MapBake::Grid::None;
    CHECK(gridAreasOf(noGrid).empty());

    auto noTree = twoAreaBake(TwoAreaGrid);
    noTree.tree.reset();
    CHECK(gridAreasOf(noTree).empty());

    // K29: a drop reason means no grid, whatever the bake still holds
    auto dropped = twoAreaBake(TwoAreaGrid);
    dropped.dropReason = "the light grid has 1 entries for 1 distinct areas";
    CHECK(gridAreasOf(dropped).empty());
  }

  SECTION("engineGridParms: an engine bake's 14, 16, 2, 0.875")
  {
    auto area = GridArea{};
    area.origin = {-192, -128, 0};
    area.size = {64, 64, 128};
    area.bounds = {7, 7, 3};
    area.probeSize = 16;
    area.borderSize = 2;
    const auto parms = engineGridParms(area, 336, 112);
    CHECK(parms.globalLightOrigin == std::array<float, 4>{-192, -128, 0, 1});
    CHECK(parms.jitterTexScale == std::array<float, 4>{64, 64, 128, 1});
    CHECK(parms.jitterTexOffset == std::array<float, 4>{7, 7, 3, 1});
    CHECK(parms.screenCorrectionFactor == std::array<float, 4>{14, 16, 2, 0.875f});
    CHECK(
      parms.cascadeDistances
      == std::array<float, 4>{336, 112, 1.0f / 336.0f, 1.0f / 112.0f});
  }

  SECTION("GridTablePublication: an empty table first, the latest after")
  {
    auto publication = GridTablePublication{};
    REQUIRE(publication.table());
    CHECK(publication.table()->areas.empty());
    CHECK(publication.table()->version == 0u);
    auto table = std::make_shared<GridTable>();
    table->version = 3;
    table->areas = gridAreasOf(twoAreaBake(TwoAreaGrid));
    publication.publish(table);
    CHECK(publication.table() == table);
  }
}

// LP4.3 (C36, SC336, SC339): admin's grid table from the retail bake (the GI .pk4's
// .blightgrid against its .lightgrid, TB_LIT_GAME_PATH, read only): one area per area of
// the tree, 69 of 72 with points (areas 46, 63 and 64 have none: the baker made no atlas
// for them), every atlas key the GI .pk4's naming
TEST_CASE("GridTable: admin's grid table", "[.][retail]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the table needs the game folder");
  }
  auto source = GameDeclSource{std::filesystem::path{gamePathValue}};
  const auto bake = readMapBake(source, "maps/game/admin");
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  source.closeFiles();
  REQUIRE(bake.tree);
  REQUIRE(bake.lightGrid);
  CHECK(bake.dropReason.empty());
  const auto areas = gridAreasOf(bake);
  CHECK(areas.size() == 72u);
  CHECK(std::size_t(bake.tree->numAreas) == areas.size());
  auto withPoints = 0;
  auto without = std::vector<int>{};
  for (const auto& area : areas)
  {
    CHECK(area.atlas == lightGridImageKey(bake.bakeBase, area.area));
    if (area.hasPoints)
    {
      ++withPoints;
    }
    else
    {
      without.push_back(area.area);
    }
  }
  CHECK(withPoints == 69);
  CHECK(without == std::vector<int>{46, 63, 64});
  std::cout << fmt::format(
    "LP4.3 admin's grid table: {} areas, {} with points; none for areas {}; area 0 "
    "origin "
    "({} {} {}) size ({} {} {}) bounds ({} {} {}) probe {} border {}\n",
    areas.size(),
    withPoints,
    fmt::join(without, ", "),
    areas[0].origin[0],
    areas[0].origin[1],
    areas[0].origin[2],
    areas[0].size[0],
    areas[0].size[1],
    areas[0].size[2],
    areas[0].bounds[0],
    areas[0].bounds[1],
    areas[0].bounds[2],
    areas[0].probeSize,
    areas[0].borderSize);
}

} // namespace tb::lit
