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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_load.cpp,
// renderer/RenderWorld_lightgrid.cpp and renderer/RenderWorld_defs.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/MapBake.h"

#include "lit/BprocFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <span>

namespace tb::lit
{
namespace
{

// idStr::StripFileExtension: cut at the last '.', wherever it is
std::string stripExtension(std::string name)
{
  if (const auto dot = name.rfind('.'); dot != std::string::npos)
  {
    name.erase(dot);
  }
  return name;
}

// idStr::SetFileExtension
std::string withExtension(const std::string& name, const std::string_view extension)
{
  return stripExtension(name) + "." + std::string{extension};
}

std::span<const std::byte> bytesOf(const std::string& text)
{
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// a game file the bake consults, probed (A5) and kept once, so that mapBakeChanged
// compares probes with probes
GameFileProbe probeFile(DeclSource& source, MapBake& bake, const std::string& path)
{
  const auto known =
    std::find_if(bake.files.begin(), bake.files.end(), [&](const MapBakeFile& file) {
      return file.path == path;
    });
  if (known != bake.files.end())
  {
    return known->probe;
  }
  auto found = source.probe(path);
  bake.files.push_back(MapBakeFile{path, found});
  return found;
}

// a game file read for the bake, its probe kept
std::optional<std::string> readFile(
  DeclSource& source, MapBake& bake, const std::string& path)
{
  static_cast<void>(probeFile(source, bake, path));
  return source.readGameFile(path, nullptr);
}

// InitFromMap (RenderWorld_load.cpp:764-998) in C10's order (K16)
void readGeometry(DeclSource& source, MapBake& bake)
{
  const auto procPath = withExtension(bake.mapName, "proc");
  const auto procProbe = probeFile(source, bake, procPath);
  if (procProbe.exists())
  {
    if (const auto text = readFile(source, bake, procPath))
    {
      readProcFile(*text) | kdl::transform([&](ProcTree tree) {
        bake.geometry = MapBake::Geometry::Proc;
        bake.tree = std::make_shared<const ProcTree>(std::move(tree));
      }) | kdl::transform_error([&](auto e) { bake.geometryError = std::move(e.msg); });
    }
    else
    {
      bake.geometryError = procPath + " cannot be read";
    }
    if (bake.tree)
    {
      bake.bakeBase = stripExtension(bake.mapName);
      return;
    }
  }

  // the .bproc: an old version only when no .proc exists at all
  const auto bprocPath = "generated/" + withExtension(bake.mapName, "bproc");
  const auto bytes = readFile(source, bake, bprocPath);
  if (!bytes)
  {
    if (bake.geometryError.empty())
    {
      bake.geometryError = fmt::format("no {} and no {}", procPath, bprocPath);
    }
    return;
  }
  readBprocTree(bytesOf(*bytes), procProbe.exists())
    | kdl::transform([&](BprocTree bproc) {
        bake.geometry = MapBake::Geometry::Bproc;
        bake.tree = std::make_shared<const ProcTree>(std::move(bproc.tree));
        // the world's mapName is the stored one, which names the bake files
        bake.bakeBase = stripExtension(bproc.mapName);
      })
    | kdl::transform_error([&](auto e) {
        bake.geometryError =
          bake.geometryError.empty() ? e.msg : bake.geometryError + "; " + e.msg;
      });
}

// SetupLightGrid and LoadLightGridFile (RenderWorld_lightgrid.cpp:383-413, 526-688)
void readLightGrid(DeclSource& source, MapBake& bake)
{
  const auto numAreas = bake.tree->numAreas;
  const auto textPath = bake.bakeBase + ".lightgrid";
  const auto binaryPath = "generated/" + withExtension(textPath, "blightgrid");

  // GetTimestamp: the probe's time, -1 for no file
  const auto textTime = probeFile(source, bake, textPath).time;
  if (const auto bytes = readFile(source, bake, binaryPath))
  {
    const auto data = bytesOf(*bytes);
    readLightGridBinaryHeader(data)
      | kdl::and_then([&](const LightGridBinaryHeader& header) -> Result<void> {
          if (!lightGridBinaryAccepted(header, textTime))
          {
            // why, for the status line (A2)
            bake.gridError = textTime == -1
                               ? binaryPath + " is not used without " + textPath
                               : binaryPath + " was not made from this " + textPath;
            return kdl::void_success;
          }
          return readLightGridBinary(data, numAreas)
                 | kdl::transform([&](LightGrid grid) {
                     bake.grid = MapBake::Grid::Binary;
                     bake.lightGrid = std::make_shared<const LightGrid>(std::move(grid));
                   });
        })
      | kdl::transform_error([&](auto e) { bake.gridError = std::move(e.msg); });
    if (bake.lightGrid)
    {
      return;
    }
  }

  // the text file; a binary that did not parse counts as none (§3.7)
  if (const auto text = readFile(source, bake, textPath))
  {
    readLightGridText(*text, numAreas) | kdl::transform([&](LightGrid grid) {
      bake.grid = MapBake::Grid::Text;
      bake.lightGrid = std::make_shared<const LightGrid>(std::move(grid));
    }) | kdl::transform_error([&](auto e) {
      bake.gridError = bake.gridError.empty() ? e.msg : bake.gridError + "; " + e.msg;
    });
  }
}

// LoadSHProbesFile (RenderWorld_defs.cpp:743-797): optional
void readShProbesFile(DeclSource& source, MapBake& bake)
{
  const auto path = withExtension(bake.bakeBase, "shprobes");
  if (const auto text = readFile(source, bake, path))
  {
    readShProbes(*text) | kdl::transform([&](std::vector<ShProbe> probes) {
      bake.shProbes = std::make_shared<const std::vector<ShProbe>>(std::move(probes));
    }) | kdl::transform_error([&](auto e) { bake.shProbesError = std::move(e.msg); });
  }
}

// §3.7: the area counts of the files that exist agree, or the bake is dropped
void validate(MapBake& bake)
{
  const auto numAreas = bake.tree->numAreas;
  if (bake.lightGrid)
  {
    auto areas = std::set<int>{};
    for (const auto& area : bake.lightGrid->areas)
    {
      areas.insert(area.area);
    }
    if (
      areas.size() != bake.lightGrid->areas.size()
      || int(bake.lightGrid->areas.size()) != numAreas)
    {
      bake.dropReason = fmt::format(
        "the light grid has {} entries for {} distinct areas, the map has {} areas",
        bake.lightGrid->areas.size(),
        areas.size(),
        numAreas);
    }
  }
  // LP1c fix (S3.12): an SH probe outside the areas drops the probes alone; the grid,
  // which the probes do not touch, stays
  if (bake.shProbes)
  {
    for (const auto& probe : *bake.shProbes)
    {
      if (probe.area < -1 || probe.area >= numAreas)
      {
        bake.shProbesError = fmt::format(
          "an SH probe is in area {}, the map has {} areas; the probes are dropped",
          probe.area,
          numAreas);
        bake.shProbes.reset();
        break;
      }
    }
  }
  if (!bake.dropReason.empty())
  {
    bake.lightGrid.reset();
    bake.grid = MapBake::Grid::None;
    bake.shProbes.reset();
  }
}

} // namespace

std::optional<std::string> engineMapName(
  const DeclSource& source, const std::string_view mapPath)
{
  try
  {
    const auto text = std::string{mapPath};
    if (const auto relative = source.gameRelativePath(text))
    {
      return stripExtension(*relative);
    }
    // a game-relative name, as the engine's map command takes it
    if (!text.empty() && std::filesystem::path{text}.is_relative())
    {
      auto name = text;
      std::replace(name.begin(), name.end(), '\\', '/');
      return stripExtension(name);
    }
  }
  catch (...)
  {
  }
  return std::nullopt;
}

MapBake readMapBake(
  DeclSource& source, const std::string_view mapName, const AreaMaterialLookup& materials)
{
  auto bake = MapBake{};
  bake.mapName = std::string{mapName};
  readGeometry(source, bake);
  if (!bake.tree)
  {
    return bake;
  }
  // C31: the areas' bounds with the materials ParseModel's FindMaterial would find, once
  // the tree is chosen (RenderWorld_load.cpp:245, Model.cpp:1330-1363, :1029-1095)
  bake.areas = std::make_shared<const AreaData>(buildAreaData(*bake.tree, materials));
  readLightGrid(source, bake);
  readShProbesFile(source, bake);
  validate(bake);
  return bake;
}

namespace
{

/**
 * Z.1: a bake file found as it was then, against what a probe finds now: GameFileProbe's
 * == (kind, container, time, size, change stamp) and, for a container entry, the
 * container file's own stamp and size, so that a .pk4 or .resources rebuilt in place
 * reads as changed after a reload (Y.2), as MaterialService's sameFile compares an
 * image's files. Both sides are probes, so a .pk4 entry's size is 0 on both.
 */
bool sameBakeFile(const GameFileProbe& then, const GameFileProbe& now)
{
  return then == now && then.containerStamp == now.containerStamp
         && then.containerSize == now.containerSize;
}

} // namespace

bool mapBakeChanged(DeclSource& source, const MapBake& bake)
{
  for (const auto& file : bake.files)
  {
    if (!sameBakeFile(file.probe, source.probe(file.path)))
    {
      return true;
    }
  }
  return false;
}

} // namespace tb::lit
