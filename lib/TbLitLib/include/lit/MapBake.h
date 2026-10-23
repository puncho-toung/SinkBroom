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

#pragma once

#include "lit/AreaData.h"
#include "lit/LightGridFile.h"
#include "lit/MaterialDecls.h"
#include "lit/ProcFile.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// a game file the bake search consulted, and what its probe found (A5); a later probe
// that differs means the bake is read again
struct MapBakeFile
{
  std::string path;
  GameFileProbe probe;

  bool operator==(const MapBakeFile& other) const = default;
};

/**
 * What the C10 search found for a map (§3.7): its area BSP from the .proc or the .bproc,
 * its light grid, its SH probes, and whether they agree. Plain data, immutable once
 * published; LP4's status line shows the reasons.
 */
struct MapBake
{
  enum class Geometry
  {
    None,
    Proc,
    Bproc,
  };
  enum class Grid
  {
    None,
    Binary,
    Text,
  };

  // the engine's map name, "maps/x" (the document's path below base, its extension cut)
  std::string mapName;
  // the name the bake files are named after: InitFromMap's mapName without its extension,
  // the .bproc's stored name when the .bproc is used
  std::string bakeBase;

  Geometry geometry = Geometry::None;
  std::shared_ptr<const ProcTree> tree;
  // why there is no tree, or why the .proc was passed over for the .bproc
  std::string geometryError;
  // C31 (LP4.0): the areas' bounds and what the portal flow reads of the tree, built with
  // it on the parse thread (buildAreaData); null without a tree
  std::shared_ptr<const AreaData> areas;

  Grid grid = Grid::None;
  std::shared_ptr<const LightGrid> lightGrid;
  // why there is no grid, or why the binary was passed over for the text
  std::string gridError;

  std::shared_ptr<const std::vector<ShProbe>> shProbes;
  std::string shProbesError;

  // §3.7: the area counts of the files that exist disagree; the grid and the probes are
  // dropped (no-bake behaviour) and this says why. The tree is kept: it is the newest
  // dmap's (K16)
  std::string dropReason;

  // every file the search consulted, in its order
  std::vector<MapBakeFile> files;
};

/**
 * The engine's map name of a document path: the path below the game's base folder
 * (DeclSource::gameRelativePath) or a relative path as it is, its extension cut
 * (idStr::StripFileExtension); nullopt for a path outside the game folder, whose bakes
 * the engine cannot have made. Parse thread (the source).
 */
std::optional<std::string> engineMapName(
  const DeclSource& source, std::string_view mapPath);

/**
 * C10 through the full C8 search (D55), read only:
 * - geometry: maps/<map>.proc first; when it is missing or does not parse,
 *   generated/maps/<map>.bproc (K16, A9: an old-version .bproc only without any .proc);
 * - the light grid with A2's rule: generated/<base>.blightgrid when
 *   lightGridBinaryAccepted against <base>.lightgrid's time, else the text; a file that
 *   does not parse counts as no file;
 * - <base>.shprobes, optional;
 * - §3.7: each entry of the grid names a distinct area, and the grid names as many areas
 *   as the tree has; every SH probe's area is -1 or below the tree's count. A mismatch
 *   drops the grid and the probes with the reason.
 * Without a tree there is no bake (InitFromMap fails, SetupLightGrid never runs). With a
 * tree, its AreaData (C31: buildAreaData), each surface's material asked of materials as
 * ParseModel's FindMaterial asks it (empty: no deform anywhere). Parse thread (C20: the
 * copied lexer reads the text files, materials the copied registry; the source belongs
 * to it).
 */
MapBake readMapBake(
  DeclSource& source, std::string_view mapName, const AreaMaterialLookup& materials = {});

// the files of a bake probed again: whether any changed since it was read (a new dmap
// run, a bake, a saved file). Parse thread
bool mapBakeChanged(DeclSource& source, const MapBake& bake);

} // namespace tb::lit
