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
#include <vector>

namespace tb::lit
{
struct ProcTree;

// the area of a piece that reached a solid leaf: kept, drawn and lit as without a .proc,
// and counted (D47, K14)
constexpr auto SolidArea = -1;

// a piece of a face or triangle in one area, its points in the winding's order
struct AreaPiece
{
  int area = 0;
  std::vector<EngineVec3> points;
};

/**
 * dmap's PutWindingIntoAreas_r (dmap/usurface.cpp:572-640, §3.4, A12, R5.1) on the
 * .proc's area tree, with fullCarve (dmap.cpp:335-336): at each node a side whose plane
 * equals the node's (dmap's plane identity, NORMAL_EPSILON 0.00001 and DIST_EPSILON 0.01)
 * goes to child 0, one on the flipped plane to child 1; any other winding is cut with the
 * copied idWinding::Split (ON_EPSILON 0.1), which sends a coplanar winding to the side
 * its own normal agrees with. A leaf of an area keeps the piece there; a piece that
 * reaches solid is kept in SolidArea where dmap drops it (D47). Without a tree the
 * winding is one piece in area 0.
 *
 * winding is in the engine's order (clockwise seen from the front, idWinding::GetPlane's
 * normal along sidePlane's), relative to the world; sidePlane is the side's plane as dmap
 * has it (engineSidePlane). Pure; any thread (C24).
 */
std::vector<AreaPiece> clipSideIntoAreas(
  const ProcTree* tree,
  const std::vector<EngineVec3>& winding,
  const EnginePlane& sidePlane);

/**
 * dmap's ClipTriIntoTree_r (dmap/usurface.cpp:449-490), as fullCarve uses it for patch
 * triangles: every node cuts the triangle with Split, without the side-plane rule. Pure;
 * any thread.
 */
std::vector<AreaPiece> clipTriangleIntoAreas(
  const ProcTree* tree, const std::array<EngineVec3, 3>& triangle);

} // namespace tb::lit
