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

// LP1c.1: the bake readers (C10, A2, A9, D64, K16, §3.7): the .bproc against the .proc
// the engine wrote it from, the C10 search on a fake game folder, the light grid's
// acceptance rule, the validation, and the probe images' names.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/BprocFile.h"
#include "lit/GameFileSource.h"
#include "lit/LightGridFile.h"
#include "lit/MapBake.h"
#include "lit/MaterialDecls.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

std::filesystem::path goldens()
{
  return litFixtureFolder() / "goldens" / "lp1b_geometry";
}

std::span<const std::byte> bytesOf(const std::string& text)
{
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// a two-area map with one node, the plane x = 0
const auto TinyProc = std::string{
  "mapProcFile004\n\ninterAreaPortals { 2 0 }\n\nnodes { 1\n( 1 0 0 0 ) -1 -2\n}\n"};

void putBig(std::string& out, const std::uint64_t value, const int bytes)
{
  for (auto i = bytes - 1; i >= 0; --i)
  {
    out.push_back(char((value >> (8 * i)) & 0xFF));
  }
}

void putFloat(std::string& out, const float value)
{
  auto bits = std::uint32_t{0};
  std::memcpy(&bits, &value, sizeof(bits));
  putBig(out, bits, 4);
}

// one grid entry per area, two points each, as WriteLightGrid writes them
LightGrid gridOf(const int areas)
{
  auto grid = LightGrid{};
  for (auto a = 0; a < areas; ++a)
  {
    auto area = LightGridArea{};
    area.area = a;
    area.probeSize = 8;
    area.borderSize = 1;
    area.origin = {float(a * 64), -32.5f, 16};
    area.size = {64, 64, 128};
    area.bounds = {2, 1, 1};
    area.points = {
      {1, {float(a * 64), -32.5f, 16}}, {0, {float(a * 64 + 64), -32.5f, 16}}};
    grid.areas.push_back(area);
  }
  return grid;
}

// RenderWorld_lightgrid.cpp's WriteLightGridsToFile and WriteLightGrid
std::string gridText(const LightGrid& grid)
{
  auto out = std::string{"LGRID \"4\"\n\n"};
  for (const auto& area : grid.areas)
  {
    out += fmt::format(
      "lightGridPoints {{ /* area = */ {} /* numLightGridPoints = */ {} /* "
      "imageSingleProbeSize = */ {} /* imageBorderSize = */ {} \n",
      area.area,
      area.points.size(),
      area.probeSize,
      area.borderSize);
    out += fmt::format(
      "/* gridMins */ \t ( {:f} {:f} {:f} )\n",
      area.origin[0],
      area.origin[1],
      area.origin[2]);
    out += fmt::format(
      "/* gridSize */ \t ( {:f} {:f} {:f} )\n", area.size[0], area.size[1], area.size[2]);
    out += fmt::format(
      "/* gridBounds */ {} {} {}\n\n", area.bounds[0], area.bounds[1], area.bounds[2]);
    for (const auto& point : area.points)
    {
      out += fmt::format(
        " {} ( {:f} {:f} {:f} )",
        int(point.valid),
        point.origin[0],
        point.origin[1],
        point.origin[2]);
    }
    out += "}\n\n";
  }
  return out;
}

// LoadLightGridFile's binary as its text branch writes it
std::string gridBinary(
  const LightGrid& grid,
  const std::int64_t storedTime,
  const std::uint32_t magic = LightGridBinaryMagic)
{
  auto out = std::string{};
  putBig(out, magic, 4);
  putBig(out, std::uint64_t(storedTime), 8);
  putBig(out, std::uint32_t(grid.areas.size()), 4);
  for (const auto& area : grid.areas)
  {
    const auto type = std::string{"lightGridPoints"};
    for (auto i = 0; i < 4; ++i)
    {
      out.push_back(char((type.size() >> (8 * i)) & 0xFF));
    }
    out += type;
    for (const auto value :
         {area.area, int(area.points.size()), area.probeSize, area.borderSize})
    {
      putBig(out, std::uint32_t(value), 4);
    }
    for (const auto value : area.origin)
    {
      putFloat(out, value);
    }
    for (const auto value : area.size)
    {
      putFloat(out, value);
    }
    for (const auto value : area.bounds)
    {
      putBig(out, std::uint32_t(value), 4);
    }
    for (const auto& point : area.points)
    {
      out.push_back(char(point.valid));
      for (const auto value : point.origin)
      {
        putFloat(out, value);
      }
    }
  }
  return out;
}

void drain()
{
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace

TEST_CASE("BprocFile, the engine's .bproc reads as the .proc it was written from (D64)")
{
  const auto proc =
    readProcFile(readTextFile(goldens() / "lp1b_geometry.proc")) | kdl::value();
  const auto bytes = readTextFile(goldens() / "lp1b_geometry.bproc");
  const auto bproc = readBprocTree(bytesOf(bytes), true) | kdl::value();
  drain();

  CHECK(bproc.version == 3);
  // the name the game gave InitFromMap, extension and all
  CHECK(bproc.mapName == "maps/lp1b_geometry.map");
  CHECK(bproc.timeStamp > 0);
  CHECK(bproc.tree.version == 0);
  // the idLexer values the engine wrote: equal bit for bit
  CHECK(bproc.tree.numAreas == proc.numAreas);
  REQUIRE(bproc.tree.nodes.size() == proc.nodes.size());
  for (std::size_t i = 0; i < proc.nodes.size(); ++i)
  {
    CHECK(bproc.tree.nodes[i].plane == proc.nodes[i].plane);
    CHECK(bproc.tree.nodes[i].children == proc.nodes[i].children);
  }
  REQUIRE(bproc.tree.portals.size() == proc.portals.size());
  for (std::size_t i = 0; i < proc.portals.size(); ++i)
  {
    CHECK(bproc.tree.portals[i].positiveArea == proc.portals[i].positiveArea);
    CHECK(bproc.tree.portals[i].negativeArea == proc.portals[i].negativeArea);
    CHECK(bproc.tree.portals[i].points == proc.portals[i].points);
  }
  // the models by name and area; their surfaces are the loaded model's (the engine's
  // FinishSurfaces adds the back sides of a two-sided material), not the text's
  REQUIRE(bproc.tree.models.size() == proc.models.size());
  for (std::size_t m = 0; m < proc.models.size(); ++m)
  {
    CHECK(bproc.tree.models[m].name == proc.models[m].name);
    CHECK(bproc.tree.models[m].area == proc.models[m].area);
  }
  // PointInArea agrees on both trees
  for (const auto& point :
       {std::array<float, 3>{128, 0, 64},
        std::array<float, 3>{-128, 0, 64},
        std::array<float, 3>{264, 0, 64}})
  {
    CHECK(pointInArea(bproc.tree, point) == pointInArea(proc, point));
  }

  SECTION("an old version is used only without a .proc (A9)")
  {
    for (const auto version : {1, 2})
    {
      auto old = bytes;
      old[3] = char(version);
      CHECK(readBprocTree(bytesOf(old), true).is_error());
      CHECK(readBprocTree(bytesOf(old), false).is_success());
    }
  }

  SECTION("a model whose time is not the file's fails the file (LoadBinaryModel)")
  {
    // the header: magic, entry count, the map name (a length and its bytes), the time
    auto shifted = bytes;
    const auto timeAt = 8 + 4 + bproc.mapName.size();
    shifted[timeAt + 7] = char(shifted[timeAt + 7] ^ 1);
    CHECK_THAT(
      std::get<Error>((readBprocTree(bytesOf(shifted), true)).error()).msg,
      Catch::Matchers::ContainsSubstring("time differs"));
  }

  SECTION("a cut file fails")
  {
    CHECK(readBprocTree(bytesOf(bytes.substr(0, bytes.size() - 1)), true).is_error());
  }
}

TEST_CASE("LightGridFile, the readers and the acceptance rule (A2, SC103, SC104)")
{
  const auto grid = gridOf(2);

  SECTION("the text the engine writes")
  {
    CHECK((readLightGridText(gridText(grid), 2) | kdl::value()) == grid);
    // an area past the map's count is the engine's idLexer::Error
    CHECK(readLightGridText(gridText(grid), 1).is_error());
    CHECK(readLightGridText("LGRID \"2\"\n", 2).is_error());
    CHECK(readLightGridText("LGRID 3\n", 2).is_success());
    CHECK(readLightGridText("NOTGRID 4\n", 2).is_error());
    drain();
  }

  SECTION("the binary the engine writes")
  {
    const auto bytes = gridBinary(grid, 1700000000);
    const auto header = readLightGridBinaryHeader(bytesOf(bytes)) | kdl::value();
    CHECK(header.magic == LightGridBinaryMagic);
    CHECK(header.storedTime == 1700000000);
    CHECK(header.numEntries == 2);
    CHECK((readLightGridBinary(bytesOf(bytes), 2) | kdl::value()) == grid);
    CHECK(readLightGridBinary(bytesOf(bytes), 1).is_error());
    CHECK(readLightGridBinary(bytesOf(bytes.substr(0, bytes.size() - 2)), 2).is_error());
  }

  SECTION("the binary is taken when the text's time equals its own, or is 0 (A2)")
  {
    auto header = LightGridBinaryHeader{LightGridBinaryMagic, 1700000000, 2};
    CHECK(lightGridBinaryAccepted(header, 1700000000));
    // the text in a container
    CHECK(lightGridBinaryAccepted(header, 0));
    // no text file: FILE_NOT_FOUND_TIMESTAMP
    CHECK_FALSE(lightGridBinaryAccepted(header, -1));
    CHECK_FALSE(lightGridBinaryAccepted(header, 1700000001));
    header.numEntries = 0;
    CHECK_FALSE(lightGridBinaryAccepted(header, 1700000000));
    header = LightGridBinaryHeader{LightGridBinaryMagic - 1, 1700000000, 2};
    CHECK_FALSE(lightGridBinaryAccepted(header, 1700000000));
  }

  SECTION("SH probes, optional (SC105)")
  {
    auto text = std::string{"SHPROBES 1 2\n"};
    for (auto p = 0; p < 2; ++p)
    {
      text += fmt::format("{} ( 1 2 3 ) (", p);
      for (auto i = 0; i < 27; ++i)
      {
        text += fmt::format(" {}", p * 100 + i);
      }
      text += " )\n";
    }
    const auto probes = readShProbes(text) | kdl::value();
    REQUIRE(probes.size() == 2u);
    CHECK(probes[1].area == 1);
    CHECK(probes[1].sh[26] == 126.0f);
    CHECK(readShProbes("SHPROBES 2 0\n").is_error());
    drain();
  }
}

TEST_CASE("MapBake, the C10 search through the game files")
{
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  const auto mapName = std::string{"maps/sub/m"};
  const auto procPath = base / "maps" / "sub" / "m.proc";
  const auto bprocPath = base / "generated" / "maps" / "sub" / "m.bproc";
  const auto gridPath = base / "maps" / "sub" / "m.lightgrid";
  const auto binaryPath = base / "generated" / "maps" / "sub" / "m.blightgrid";
  writeTextFile(bprocPath, readTextFile(goldens() / "lp1b_geometry.bproc"));
  const auto readBake = [&] {
    auto source = GameFileSource{env.dir()};
    auto bake = readMapBake(source, mapName);
    drain();
    return bake;
  };

  SECTION("the .proc first (K16)")
  {
    writeTextFile(procPath, TinyProc);
    const auto bake = readBake();
    CHECK(bake.geometry == MapBake::Geometry::Proc);
    REQUIRE(bake.tree);
    CHECK(bake.tree->nodes.size() == 1u);
    CHECK(bake.bakeBase == "maps/sub/m");
    CHECK(bake.files.front().path == "maps/sub/m.proc");
    CHECK(bake.files.front().probe.kind == GameFileProbe::Kind::Loose);
  }

  SECTION("else the .bproc, whose stored map name names the bake files")
  {
    const auto bake = readBake();
    CHECK(bake.geometry == MapBake::Geometry::Bproc);
    REQUIRE(bake.tree);
    CHECK(bake.tree->nodes.size() == 39u);
    CHECK(bake.bakeBase == "maps/lp1b_geometry");
  }

  SECTION("a .proc that does not parse counts as none; the .bproc stands in")
  {
    writeTextFile(procPath, "mapProcFile004\nnodes { 1 ( 1 0 0 0 ) -1\n");
    const auto bake = readBake();
    CHECK(bake.geometry == MapBake::Geometry::Bproc);
    CHECK_THAT(bake.geometryError, Catch::Matchers::ContainsSubstring(".proc"));
  }

  SECTION("the .proc in a .pk4 is found as the engine finds it (C8)")
  {
    writePk4(base / "maps.pk4", {{"maps/sub/m.proc", TinyProc}});
    const auto bake = readBake();
    CHECK(bake.geometry == MapBake::Geometry::Proc);
    CHECK(bake.files.front().probe.container == "maps.pk4");
  }

  SECTION("no .proc and no .bproc: no bake")
  {
    std::filesystem::remove(bprocPath);
    const auto bake = readBake();
    CHECK(bake.geometry == MapBake::Geometry::None);
    CHECK_FALSE(bake.tree);
    CHECK_THAT(
      bake.geometryError, Catch::Matchers::ContainsSubstring("no maps/sub/m.proc"));
  }

  SECTION("the light grid: the binary when its time is the text's (A2)")
  {
    writeTextFile(procPath, TinyProc);
    auto text = gridOf(2);
    text.areas[0].probeSize = 16;
    writeTextFile(gridPath, gridText(text));
    REQUIRE(setUnixModificationTime(gridPath, 1700000000));
    writeTextFile(binaryPath, gridBinary(gridOf(2), 1700000000));
    auto bake = readBake();
    CHECK(bake.grid == MapBake::Grid::Binary);
    REQUIRE(bake.lightGrid);
    CHECK(bake.lightGrid->areas[0].probeSize == 8);
    CHECK(bake.dropReason.empty());

    // a text saved since: the binary is stale, the text is read
    REQUIRE(setUnixModificationTime(gridPath, 1700000001));
    bake = readBake();
    CHECK(bake.grid == MapBake::Grid::Text);
    REQUIRE(bake.lightGrid);
    CHECK(bake.lightGrid->areas[0].probeSize == 16);
    CHECK_THAT(bake.gridError, Catch::Matchers::ContainsSubstring("was not made from"));

    // no text file: the binary is not taken, and there is no grid
    std::filesystem::remove(gridPath);
    bake = readBake();
    CHECK(bake.grid == MapBake::Grid::None);
    CHECK_FALSE(bake.lightGrid);
    CHECK_THAT(bake.gridError, Catch::Matchers::ContainsSubstring("is not used without"));
  }

  SECTION("the light grid: a text in a container has time 0, which takes any binary")
  {
    writeTextFile(procPath, TinyProc);
    writePk4(base / "grid.pk4", {{"maps/sub/m.lightgrid", gridText(gridOf(2))}});
    writeTextFile(binaryPath, gridBinary(gridOf(2), 12345));
    const auto bake = readBake();
    CHECK(bake.grid == MapBake::Grid::Binary);
  }

  SECTION("a binary that does not parse counts as none; the text stands in")
  {
    writeTextFile(procPath, TinyProc);
    writeTextFile(gridPath, gridText(gridOf(2)));
    REQUIRE(setUnixModificationTime(gridPath, 1700000000));
    const auto binary = gridBinary(gridOf(2), 1700000000);
    writeTextFile(binaryPath, binary.substr(0, binary.size() - 3));
    const auto bake = readBake();
    CHECK(bake.grid == MapBake::Grid::Text);
    CHECK_FALSE(bake.gridError.empty());
  }

  SECTION("area counts that disagree drop the grid and the probes (§3.7)")
  {
    writeTextFile(procPath, TinyProc);
    // one entry for a two-area map
    writeTextFile(gridPath, gridText(gridOf(1)));
    writeTextFile(
      base / "maps" / "sub" / "m.shprobes",
      "SHPROBES 1 1\n0 ( 0 0 0 ) ( 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
      ")\n");
    auto bake = readBake();
    CHECK_THAT(
      bake.dropReason, Catch::Matchers::ContainsSubstring("the map has 2 areas"));
    CHECK_FALSE(bake.lightGrid);
    CHECK_FALSE(bake.shProbes);
    CHECK(bake.tree);

    writeTextFile(gridPath, gridText(gridOf(2)));
    bake = readBake();
    CHECK(bake.dropReason.empty());
    CHECK(bake.lightGrid);
    REQUIRE(bake.shProbes);
    CHECK(bake.shProbes->size() == 1u);

    // an SH probe in an area the map does not have: the probes alone are dropped (S3.12)
    writeTextFile(
      base / "maps" / "sub" / "m.shprobes",
      "SHPROBES 1 1\n5 ( 0 0 0 ) ( 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
      ")\n");
    bake = readBake();
    CHECK(bake.dropReason.empty());
    CHECK(bake.lightGrid);
    CHECK_FALSE(bake.shProbes);
    CHECK_THAT(bake.shProbesError, Catch::Matchers::ContainsSubstring("SH probe"));
  }

  SECTION("a text grid's point count is bounded by its points, not allocated ahead")
  {
    writeTextFile(procPath, TinyProc);
    writeTextFile(
      gridPath,
      "LGRID 3\nlightGridPoints { 0 2000000000 64 2 ( 0 0 0 ) ( 64 64 128 ) 1 1 1 1 ( 0 "
      "0 0 "
      ") }\n");
    const auto bake = readBake();
    CHECK_FALSE(bake.lightGrid);
    CHECK_THAT(bake.gridError, Catch::Matchers::ContainsSubstring("bad grid point"));
  }

  SECTION("a changed bake file is seen by a probe")
  {
    writeTextFile(procPath, TinyProc);
    REQUIRE(setUnixModificationTime(procPath, 1700000000));
    auto source = GameFileSource{env.dir()};
    const auto bake = readMapBake(source, mapName);
    CHECK_FALSE(mapBakeChanged(source, bake));
    REQUIRE(setUnixModificationTime(procPath, 1700000005));
    CHECK(mapBakeChanged(source, bake));
    // a grid that appears changes it too
    REQUIRE(setUnixModificationTime(procPath, 1700000000));
    CHECK_FALSE(mapBakeChanged(source, bake));
    writeTextFile(gridPath, gridText(gridOf(2)));
    // a new file is seen once the source looks at its folder again, as the poll makes it
    // (S2.2)
    source.looseFilesMayHaveChanged();
    CHECK(mapBakeChanged(source, bake));
    drain();
  }

  SECTION("Z.1: a bake in a container rebuilt in place is seen after a reload")
  {
    const auto pk4Path = base / "maps.pk4";
    writePk4(pk4Path, {{"maps/sub/m.proc", TinyProc}});
    auto first = GameFileSource{env.dir()};
    const auto bake = readMapBake(first, mapName);
    // the read batch's containers close, as the parse thread closes them after each item
    first.closeFiles();
    REQUIRE(bake.geometry == MapBake::Geometry::Proc);
    REQUIRE(bake.files.front().probe.container == "maps.pk4");
    CHECK(bake.files.front().probe.containerSize > 0u);

    // a reload over the same container: unchanged
    {
      auto again = GameFileSource{env.dir()};
      CHECK_FALSE(mapBakeChanged(again, bake));
    }

    // the container rewritten with the same entry: its entry's probe (time 0, size 0,
    // the container's name) is alike, its stamp and size are not
    writePk4(pk4Path, {{"maps/sub/m.proc", TinyProc}, {"maps/sub/other.txt", "padding"}});
    bumpModificationTime(pk4Path);
    auto rebuilt = GameFileSource{env.dir()};
    CHECK(rebuilt.probe("maps/sub/m.proc") == bake.files.front().probe);
    CHECK(mapBakeChanged(rebuilt, bake));
    drain();
  }

  SECTION("the engine's map name of a document path")
  {
    auto source = GameFileSource{env.dir()};
    CHECK(
      engineMapName(source, (base / "maps" / "sub" / "m.map").string()) == "maps/sub/m");
    CHECK(engineMapName(source, "maps\\sub\\m.map") == "maps/sub/m");
    CHECK_FALSE(engineMapName(source, (env.dir() / "elsewhere.map").string()));
  }
}

TEST_CASE("MapBake, the probe images' names (SC105)")
{
  const auto env = fs::TestEnvironment{};
  auto source = GameFileSource{env.dir()};
  auto decls = MaterialDecls{source};
  const auto generatedName = [&](const MaterialImageKey& key) {
    auto name = decls.imageCacheName(key);
    REQUIRE(name);
    return name.value->generatedName;
  };

  CHECK(
    generatedName(lightGridImageKey("maps/game/admin", 3))
    == "env/maps/game/admin/area3_lightgrid_amb#__2100");
  const auto probes = envProbeImageKeys("maps/a-b", 2, {-16, 32, -48});
  CHECK(generatedName(probes[0]) == "env/maps/a_b/area2_envprobe__16_32__48_amb#__2105");
  CHECK(generatedName(probes[1]) == "env/maps/a_b/area2_envprobe__16_32__48_spec#__2108");
  CHECK(
    generatedName(probes[2]) == "env/maps/a_b/area2_envprobe__16_32__48_depth#__2208");
  drain();
}

} // namespace tb::lit
