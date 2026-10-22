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
// GPL Source Code (neo/tools/compilers/dmap/usurface.cpp and dmap/map.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/AreaTree.h"

#include "lit/ProcFile.h"

namespace tb::lit
{
namespace
{

// dmap/map.cpp:68-69, dmap's plane identity
constexpr auto NormalEpsilon = 0.00001f;
constexpr auto DistEpsilon = 0.01f;
// idlib/math/Plane.h:44
constexpr auto OnEpsilon = 0.1f;

EnginePlane nodePlane(const ProcNode& node)
{
  return {node.plane[0], node.plane[1], node.plane[2], node.plane[3]};
}

struct Clipper
{
  const ProcTree& tree;
  const EnginePlane* sidePlane;
  std::vector<AreaPiece> pieces;

  void leaf(const int child, std::vector<EngineVec3> points)
  {
    // a child of 0 is solid, a negative one the area -1 - child
    pieces.push_back(AreaPiece{child == 0 ? SolidArea : -1 - child, std::move(points)});
  }

  void clip(std::vector<EngineVec3> points, const int nodeNum, const std::size_t depth)
  {
    if (points.size() < 3)
    {
      return;
    }
    // a malformed tree that loops ends in solid
    if (depth > tree.nodes.size())
    {
      leaf(0, std::move(points));
      return;
    }
    const auto& node = tree.nodes[std::size_t(nodeNum)];
    const auto plane = nodePlane(node);
    const auto descend = [&](const int child, std::vector<EngineVec3> piece) {
      if (child > 0)
      {
        clip(std::move(piece), child, depth + 1);
      }
      else
      {
        leaf(child, std::move(piece));
      }
    };

    if (sidePlane)
    {
      // usurface.cpp:584-594: side->planenum == node->planenum, or its flip
      if (enginePlanesEqual(*sidePlane, plane, NormalEpsilon, DistEpsilon))
      {
        descend(node.children[0], std::move(points));
        return;
      }
      if (enginePlanesEqual(
            *sidePlane, engineFlippedPlane(plane), NormalEpsilon, DistEpsilon))
      {
        descend(node.children[1], std::move(points));
        return;
      }
    }

    auto split = engineSplitWinding(points, plane, OnEpsilon);
    if (!split)
    {
      // more points than Split takes (C24 (2)): kept whole, in solid
      leaf(0, std::move(points));
      return;
    }
    descend(node.children[0], std::move(split->front));
    descend(node.children[1], std::move(split->back));
  }
};

std::vector<AreaPiece> clipInto(
  const ProcTree* tree, std::vector<EngineVec3> points, const EnginePlane* sidePlane)
{
  if (!tree || tree->nodes.empty())
  {
    return {AreaPiece{0, std::move(points)}};
  }
  auto clipper = Clipper{*tree, sidePlane, {}};
  clipper.clip(std::move(points), 0, 0);
  return std::move(clipper.pieces);
}

} // namespace

std::vector<AreaPiece> clipSideIntoAreas(
  const ProcTree* tree,
  const std::vector<EngineVec3>& winding,
  const EnginePlane& sidePlane)
{
  return clipInto(tree, winding, &sidePlane);
}

std::vector<AreaPiece> clipTriangleIntoAreas(
  const ProcTree* tree, const std::array<EngineVec3, 3>& triangle)
{
  return clipInto(tree, {triangle.begin(), triangle.end()}, nullptr);
}

} // namespace tb::lit
