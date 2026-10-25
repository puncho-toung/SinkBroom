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

#include "lit/EngineGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tb::lit
{
struct ProcSurface;
struct ProcTree;

// LP7.4 (C55, D133): an island is planar when every triangle's normal is within this
// cosine of the reference triangle's and every vertex within PlanarDistance of its plane
inline constexpr auto PlanarCosine = 0.999f;
inline constexpr auto PlanarDistance = 1.0f / 32.0f;

/**
 * LP7.4 (C55, SC638): one island of a .proc surface, the triangles connected through
 * shared vertex indexes (RenderWorld_load.cpp:224-268, the walk ParseModel recentres the
 * st by; a vertex belongs to one island, so the island's recentring and tangents are the
 * whole surface's).
 *
 * Planar when every triangle's plane is one plane: each non-degenerate triangle's normal
 * within PlanarCosine of the reference triangle's, every vertex within PlanarDistance of
 * its plane; the reference is the island's largest triangle (its best-conditioned plane).
 * A patch is one curved island, as are faces within 2.6 degrees that dmap's MatchVert
 * welds (K28 stays there); a curved island is never adopted.
 */
struct ProcIsland
{
  // the surface's vertex indexes of the island, ascending
  std::vector<int> verts;
  // its triangles, three surface vertex indexes each, in the surface's order
  std::vector<int> indexes;
  // idBounds of its positions as the .proc carries them
  std::array<EngineVec3, 2> bounds = {};
  // the sum of its triangles' idWinding::TriangleArea
  float area = 0;
  bool planar = false;
  // planar: the reference triangle's plane, idWinding::GetPlane's normal (( c - a ) x ( b
  // - a ), the engine's winding: the front the face's), Distance( p ) = n · p + d
  EnginePlane plane;
  /**
   * planar: the affine map of the reference triangle's positions to its st, as the .proc
   * carries them (before ParseModel's recentring): s = stMap[0] · ( x y z 1 ), t =
   * stMap[1] · ( x y z 1 ), the gradient in the plane; dmap's texVec[2][4] shape
   * (textureVectors_t, dmap.h) in st units, a Valve 220 side's being texel units divided
   * by its image's size (dmap/usurface.cpp:233-242)
   */
  std::array<std::array<float, 4>, 2> stMap = {};
};

// the islands of a model's surfaces, one entry per surface (none for a finished model)
struct ProcModelIslands
{
  std::vector<std::vector<ProcIsland>> surfaces;
  std::size_t planarIslands = 0;
  std::size_t curvedIslands = 0;
};

// the islands of every model of a tree, one entry per model
struct ProcIslands
{
  std::vector<ProcModelIslands> models;
};

/**
 * LP7.4 (C55): the islands of one .proc surface with its geometry, in the order of their
 * first triangle; none for a surface without its geometry (read without it, or a .bproc's
 * finished surface: D138). Pure; any thread (C24: plain code).
 */
std::vector<ProcIsland> procSurfaceIslands(const ProcSurface& surface);

/**
 * LP7.4 (C55, D138): procSurfaceIslands of every surface of every model; a finished model
 * (a .bproc's, ProcModel::finished) yields none. Pure; any thread.
 */
ProcIslands buildProcIslands(const ProcTree& tree);

// an island's draw vertices and its triangles as indexes into them
struct ProcIslandGeometry
{
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
};

/**
 * LP7.4 (C55, SC642): the island's vertices as loadProcSurface makes them (the
 * ParseModel mirror, RenderWorld_load.cpp:269-321: st recentred by the island's
 * Ftoi((min + max) / 2), SetTexCoord, SetNormal, the colour), byte for byte, in
 * island.verts order, and its triangles as indexes into them (src/ProcFile.cpp, beside
 * loadProcSurface). Empty without the surface's geometry, and for an island of more than
 * engineBounds::MaxSurfaceVertices vertices. Pure; any thread.
 */
ProcIslandGeometry loadProcIsland(const ProcSurface& surface, const ProcIsland& island);

} // namespace tb::lit
