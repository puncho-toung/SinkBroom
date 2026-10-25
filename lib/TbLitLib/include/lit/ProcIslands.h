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
#include "lit/Snapshot.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
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

// ----- LP7.4 (C55, D133, P7.1): the candidates of a planar island and the test -----

/**
 * The per-axis tolerance of the test: the T-junction fix snaps a vertex to 1/32 and
 * merges it into a hash vertex within one snap unit (tritjunction.cpp:122-151, up to 3/64
 * per axis), and MatchVert welds within 0.01 (output.cpp:127-180). In the plane a point
 * may so move by up to AdoptionEpsilon times the square root of 3 (AdoptionDistance).
 */
inline constexpr auto AdoptionEpsilon = 3.0f / 64.0f + 0.01f;
inline constexpr auto AdoptionDistance = AdoptionEpsilon * 1.7320508f;

// a face piece: the node, the face's index in it, the piece's index in the face's clip
// into the areas
struct AdoptionPieceKey
{
  PreviewId node = NoPreviewId;
  std::uint32_t face = 0;
  std::uint32_t piece = 0;

  auto operator<=>(const AdoptionPieceKey&) const = default;
};

/**
 * What the test reads of one face piece the chunk builder makes (ChunkJobOutput::pieces):
 * its model (a world piece's area, the .proc's _areaN; -1 for a brush entity's piece,
 * whose model is the entity's own, K17), the material it is built with (worldspawn's
 * overrideMaterial in its place, before any remap), dmap's side plane, the side's texture
 * vectors (engineValveTextureVectors) and editor-image size, and the winding in the
 * frame the .proc's positions are in (the parse frame less the entity's origin offset,
 * read back as ParseModel reads what dmap wrote, procReadBack).
 */
struct AdoptionPiece
{
  AdoptionPieceKey key;
  int area = 0;
  std::string material;
  // the side plane in the piece's frame
  EnginePlane plane;
  EngineTextureVectors texVec = {};
  float textureWidth = 8;
  float textureHeight = 8;
  // the side's st at a point p of the frame are its texVec's at p + stOrigin (the origin
  // offset dmap subtracts after TriListForSide, output.cpp:589)
  EngineVec3 stOrigin = {0, 0, 0};
  std::vector<EngineVec3> points;
};

enum class IslandVerdict
{
  Adopted,
  // not planar: never adopted
  Curved,
  // no face piece of its material, plane and texture vectors overlaps it
  NoCandidate,
  // an island vertex lies outside the candidates' union
  IslandOutside,
  // a candidate's vertex lies outside the island's triangles
  PieceOutside,
  // two candidates overlap by more than a sliver
  Overlap,
  // the summed areas differ by more than the candidates' perimeter times AdoptionEpsilon
  Area,
};

struct IslandMatch
{
  IslandVerdict verdict = IslandVerdict::NoCandidate;
  // the candidates, as indexes into the pieces given
  std::vector<std::size_t> candidates;
};

/**
 * LP7.4 (C55, P7.1): the test of one planar island of surface against the face pieces of
 * its model. The candidates are the pieces whose material equals the surface's (case
 * aside), whose plane equals the island's (normals within PlanarCosine, the island's
 * centroid within AdoptionEpsilon of the piece's plane), whose texture vectors equal the
 * island's st map, and whose polygon overlaps the island's triangles by more than a
 * sliver (an intersection area above the piece's perimeter times AdoptionEpsilon), so
 * that disjoint coplanar islands of one material never fail each other.
 *
 * The texture vectors are compared where dmap's st are: at every island vertex, the
 * piece's st through dmap's rule ((texVec · ( p + stOrigin ) + offset) / size,
 * TriListForSide) against the vertex's st within what dmap's own writing leaves of the
 * equality AddTriListToArea's epsilons stand for (TEXTURE_VECTOR_EQUAL_EPSILON 0.001,
 * TEXTURE_OFFSET_EQUAL_EPSILON 0.005, usurface.cpp:36-37, in texels): dmap's idDrawVert
 * stores st as half floats (two half-float units of the larger value), WriteFloat writes
 * a value within 0.001 of an integer as the integer (output.cpp:95-105), and the
 * T-junction snap moves the position under its st (the gradient times AdoptionEpsilon
 * per axis).
 *
 * Adopted when there is a candidate and, in the island's plane, every island vertex lies
 * within AdoptionDistance of the candidates' union, every candidate vertex within
 * AdoptionDistance of the island's triangles (a colinear or interior vertex the optimizer
 * dropped lies on an edge or inside, optimize.cpp:1967-2000), no two candidates overlap
 * by more than a sliver (the smaller perimeter times AdoptionEpsilon: so that the summed
 * area is the union's, and a hole the candidates leave is not paid for by an overlap
 * elsewhere, as a face moved within its plane would leave both), and the candidates'
 * summed area is the island's within their summed perimeter times AdoptionEpsilon. Pure;
 * any thread.
 */
IslandMatch matchProcIsland(
  const ProcSurface& surface,
  const ProcIsland& island,
  const std::vector<const AdoptionPiece*>& pieces);

} // namespace tb::lit
