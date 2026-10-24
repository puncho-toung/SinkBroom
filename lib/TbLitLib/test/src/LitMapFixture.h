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

#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"
#include "lit/Snapshot.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

// the fixture folder fixture/lit (a game folder: maps, materials, textures, goldens)
std::filesystem::path litFixtureFolder();

/**
 * A snapshot of a map file in TrenchBroom's Doom3 Valve format, as LP1b.1's builder would
 * make it from the document (C23): lit::snapshotOfMapText (lit/FeedScene.h, LPF.2) with
 * generation and sequence 1 and no map path; throws its error.
 */
Snapshot snapshotOfMapText(const std::string& text);

// an axial box brush in the map's Valve 220 text, its faces with this material
// (TrenchBroom's name) and plain axes: (p3 - p1) x (p2 - p1) is each face's outward
// normal
std::string boxBrush(
  int x0, int y0, int z0, int x1, int y1, int z1, const std::string& material);

// the lines of a captured listing (goldens/lp1b_geometry/listImages.txt) after "@@ map":
// image name -> (width, height); CR before LF is ignored (R1.4)
std::map<std::string, std::pair<int, int>> listImagesSizes(const std::string& text);

/**
 * A material table of the fixture materials (materials/lp1bc.mtr) and stand-ins for the
 * base materials the fixture map names (textures/editor/visportal: areaportal, no
 * stages; textures/common/origin: origin, no stages), parsed by the copied parser; the
 * editor-image sizes from listImages, Ready (C21).
 */
std::shared_ptr<const MaterialTable> fixtureMaterialTable(
  const std::map<std::string, std::pair<int, int>>& sizes);

/**
 * A material table of stand-ins for the light materials the light tests name (the tests
 * never read the game): lights/squarelight1 and lights/defaultPointLight with a
 * lightFalloffImage, lights/defaultProjectedLight with _noFalloff, and
 * lights/lp1b_nofalloff without one, parsed by the copied parser; each of implicitNames
 * is looked up too and comes out implicit (no decl).
 */
std::shared_ptr<const MaterialTable> lightMaterialTable(
  const std::vector<std::string>& implicitNames = {});

} // namespace tb::lit
