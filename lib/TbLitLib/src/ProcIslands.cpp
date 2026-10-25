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
// idlib/geometry/Winding.cpp and tools/compilers/dmap/usurface.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ProcIslands.h"

#include "lit/ProcFile.h"

#include <cmath>
#include <unordered_map>

namespace tb::lit
{
namespace
{

using Vec3d = std::array<double, 3>;

Vec3d sub(const Vec3d& a, const Vec3d& b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double dot(const Vec3d& a, const Vec3d& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3d cross(const Vec3d& a, const Vec3d& b)
{
  return {
    a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double length(const Vec3d& a)
{
  return std::sqrt(dot(a, a));
}

// below this cross product length a triangle has no plane of its own
constexpr auto DegenerateCross = 1e-6;

class SurfaceView
{
private:
  const ProcSurface& m_surface;
  std::size_t m_floats;

public:
  explicit SurfaceView(const ProcSurface& surface)
    : m_surface{surface}
    , m_floats{std::size_t(surface.vertexFormat == 1 ? 12 : 8)}
  {
  }

  bool hasGeometry() const
  {
    return m_surface.numVerts >= 0
           && m_surface.verts.size() == std::size_t(m_surface.numVerts) * m_floats;
  }

  EngineVec3 position(const int k) const
  {
    const auto* v = m_surface.verts.data() + std::size_t(k) * m_floats;
    return {v[0], v[1], v[2]};
  }

  Vec3d positiond(const int k) const
  {
    const auto p = position(k);
    return {double(p[0]), double(p[1]), double(p[2])};
  }

  EngineVec2 st(const int k) const
  {
    const auto* v = m_surface.verts.data() + std::size_t(k) * m_floats;
    return {v[3], v[4]};
  }
};

// the affine map of one st axis over a triangle: value( p ) = g · p + c, g in the plane
std::array<float, 4> affineMap(
  const Vec3d& a,
  const Vec3d& b,
  const Vec3d& c,
  const double va,
  const double vb,
  const double vc)
{
  const auto e1 = sub(b, a);
  const auto e2 = sub(c, a);
  const auto d1 = vb - va;
  const auto d2 = vc - va;
  const auto e11 = dot(e1, e1);
  const auto e12 = dot(e1, e2);
  const auto e22 = dot(e2, e2);
  const auto det = e11 * e22 - e12 * e12;
  if (det == 0.0)
  {
    return {0, 0, 0, float(va)};
  }
  const auto alpha = (d1 * e22 - d2 * e12) / det;
  const auto beta = (d2 * e11 - d1 * e12) / det;
  const auto g = Vec3d{
    alpha * e1[0] + beta * e2[0],
    alpha * e1[1] + beta * e2[1],
    alpha * e1[2] + beta * e2[2]};
  return {float(g[0]), float(g[1]), float(g[2]), float(va - dot(g, a))};
}

void finishIsland(const SurfaceView& view, ProcIsland& island)
{
  auto points = std::vector<EngineVec3>{};
  points.reserve(island.verts.size());
  for (const auto k : island.verts)
  {
    points.push_back(view.position(k));
  }
  island.bounds = engineBoundsOfPoints(points);

  // the reference triangle: the largest (idWinding::TriangleArea, dmap's measure)
  auto reference = std::size_t{0};
  auto largest = -1.0f;
  for (auto i = std::size_t{0}; i + 2 < island.indexes.size(); i += 3)
  {
    const auto area = engineTriangleArea(
      view.position(island.indexes[i]),
      view.position(island.indexes[i + 1]),
      view.position(island.indexes[i + 2]));
    island.area += area;
    if (area > largest)
    {
      largest = area;
      reference = i;
    }
  }
  const auto a = view.positiond(island.indexes[reference]);
  const auto b = view.positiond(island.indexes[reference + 1]);
  const auto c = view.positiond(island.indexes[reference + 2]);
  // idWinding::GetPlane's orientation: ( c - a ) x ( b - a )
  const auto normal = cross(sub(c, a), sub(b, a));
  const auto normalLength = length(normal);
  if (normalLength < DegenerateCross)
  {
    return;
  }
  const auto n =
    Vec3d{normal[0] / normalLength, normal[1] / normalLength, normal[2] / normalLength};
  const auto d = -dot(n, a);

  for (auto i = std::size_t{0}; i + 2 < island.indexes.size(); i += 3)
  {
    const auto p0 = view.positiond(island.indexes[i]);
    const auto p1 = view.positiond(island.indexes[i + 1]);
    const auto p2 = view.positiond(island.indexes[i + 2]);
    const auto tn = cross(sub(p2, p0), sub(p1, p0));
    const auto tl = length(tn);
    if (tl >= DegenerateCross && dot(tn, n) / tl < double(PlanarCosine))
    {
      return;
    }
  }
  for (const auto k : island.verts)
  {
    if (std::abs(dot(n, view.positiond(k)) + d) > double(PlanarDistance))
    {
      return;
    }
  }

  island.planar = true;
  island.plane = EnginePlane{float(n[0]), float(n[1]), float(n[2]), float(d)};
  const auto sa = view.st(island.indexes[reference]);
  const auto sb = view.st(island.indexes[reference + 1]);
  const auto sc = view.st(island.indexes[reference + 2]);
  island.stMap[0] = affineMap(a, b, c, sa[0], sb[0], sc[0]);
  island.stMap[1] = affineMap(a, b, c, sa[1], sb[1], sc[1]);
}

} // namespace

std::vector<ProcIsland> procSurfaceIslands(const ProcSurface& surface)
{
  const auto view = SurfaceView{surface};
  const auto numVerts = std::size_t(std::max(surface.numVerts, 0));
  if (!view.hasGeometry() || surface.indexes.size() % 3 != 0)
  {
    return {};
  }
  for (const auto index : surface.indexes)
  {
    if (index < 0 || std::size_t(index) >= numVerts)
    {
      return {};
    }
  }

  // RenderWorld_load.cpp:224-268: the triangles connected through a shared vertex index;
  // a union-find makes the walk's partition
  auto parent = std::vector<int>(numVerts);
  for (auto i = std::size_t{0}; i < numVerts; ++i)
  {
    parent[i] = int(i);
  }
  const auto root = [&](int v) {
    while (parent[std::size_t(v)] != v)
    {
      parent[std::size_t(v)] = parent[std::size_t(parent[std::size_t(v)])];
      v = parent[std::size_t(v)];
    }
    return v;
  };
  for (auto i = std::size_t{0}; i < surface.indexes.size(); i += 3)
  {
    const auto r = root(surface.indexes[i]);
    for (auto k = std::size_t{1}; k < 3; ++k)
    {
      const auto o = root(surface.indexes[i + k]);
      if (o != r)
      {
        parent[std::size_t(o)] = r;
      }
    }
  }

  // the islands in the order of their first triangle, as the walk numbers them
  auto islands = std::vector<ProcIsland>{};
  auto islandOf = std::unordered_map<int, std::size_t>{};
  for (auto i = std::size_t{0}; i < surface.indexes.size(); i += 3)
  {
    const auto r = root(surface.indexes[i]);
    const auto [it, added] = islandOf.try_emplace(r, islands.size());
    if (added)
    {
      islands.emplace_back();
    }
    auto& island = islands[it->second];
    island.indexes.insert(
      island.indexes.end(),
      surface.indexes.begin() + std::ptrdiff_t(i),
      surface.indexes.begin() + std::ptrdiff_t(i + 3));
  }
  auto used = std::vector<bool>(numVerts, false);
  for (const auto index : surface.indexes)
  {
    used[std::size_t(index)] = true;
  }
  for (auto k = std::size_t{0}; k < numVerts; ++k)
  {
    if (used[k])
    {
      islands[islandOf.at(root(int(k)))].verts.push_back(int(k));
    }
  }
  for (auto& island : islands)
  {
    finishIsland(view, island);
  }
  return islands;
}

ProcIslands buildProcIslands(const ProcTree& tree)
{
  auto islands = ProcIslands{};
  islands.models.reserve(tree.models.size());
  for (const auto& model : tree.models)
  {
    auto& modelIslands = islands.models.emplace_back();
    modelIslands.surfaces.resize(model.surfaces.size());
    // D138: a .bproc's finished surfaces are not adopted
    if (model.finished)
    {
      continue;
    }
    for (auto s = std::size_t{0}; s < model.surfaces.size(); ++s)
    {
      modelIslands.surfaces[s] = procSurfaceIslands(model.surfaces[s]);
      for (const auto& island : modelIslands.surfaces[s])
      {
        ++(island.planar ? modelIslands.planarIslands : modelIslands.curvedIslands);
      }
    }
  }
  return islands;
}

} // namespace tb::lit
