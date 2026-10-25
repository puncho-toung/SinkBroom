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

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
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

// ----- the test (LP7.4, C55, P7.1) -----

using Vec2d = std::array<double, 2>;

double cross2(const Vec2d& a, const Vec2d& b)
{
  return a[0] * b[1] - a[1] * b[0];
}

Vec2d sub2(const Vec2d& a, const Vec2d& b)
{
  return {a[0] - b[0], a[1] - b[1]};
}

// an orthonormal frame of a plane: positions as ( p · u, p · v ), u x v the normal
struct PlaneFrame
{
  Vec3d u = {1, 0, 0};
  Vec3d v = {0, 1, 0};

  explicit PlaneFrame(const Vec3d& n)
  {
    const auto axis = std::abs(n[0]) < 0.9 ? Vec3d{1, 0, 0} : Vec3d{0, 1, 0};
    const auto c = cross(axis, n);
    const auto l = length(c);
    u = {c[0] / l, c[1] / l, c[2] / l};
    v = cross(n, u);
  }

  Vec2d project(const EngineVec3& p) const
  {
    const auto q = Vec3d{double(p[0]), double(p[1]), double(p[2])};
    return {dot(q, u), dot(q, v)};
  }
};

double signedArea(const std::vector<Vec2d>& polygon)
{
  auto area = 0.0;
  for (auto i = std::size_t{0}; i < polygon.size(); ++i)
  {
    area += cross2(polygon[i], polygon[(i + 1) % polygon.size()]);
  }
  return area * 0.5;
}

// counter-clockwise in the frame
void makeCounterClockwise(std::vector<Vec2d>& polygon)
{
  if (signedArea(polygon) < 0)
  {
    std::ranges::reverse(polygon);
  }
}

double perimeter(const std::vector<Vec2d>& polygon)
{
  auto sum = 0.0;
  for (auto i = std::size_t{0}; i < polygon.size(); ++i)
  {
    const auto d = sub2(polygon[(i + 1) % polygon.size()], polygon[i]);
    sum += std::sqrt(d[0] * d[0] + d[1] * d[1]);
  }
  return sum;
}

// a convex counter-clockwise polygon less what lies right of the line a -> b
std::vector<Vec2d> clipLeft(
  const std::vector<Vec2d>& polygon, const Vec2d& a, const Vec2d& b)
{
  auto out = std::vector<Vec2d>{};
  const auto edge = sub2(b, a);
  for (auto i = std::size_t{0}; i < polygon.size(); ++i)
  {
    const auto& p = polygon[i];
    const auto& q = polygon[(i + 1) % polygon.size()];
    const auto dp = cross2(edge, sub2(p, a));
    const auto dq = cross2(edge, sub2(q, a));
    if (dp >= 0)
    {
      out.push_back(p);
    }
    if ((dp >= 0) != (dq >= 0))
    {
      const auto t = dp / (dp - dq);
      out.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t});
    }
  }
  return out;
}

// the area two convex counter-clockwise polygons share
double sharedArea(const std::vector<Vec2d>& polygon, const std::vector<Vec2d>& clip)
{
  auto inside = polygon;
  for (auto i = std::size_t{0}; i < clip.size() && inside.size() >= 3; ++i)
  {
    inside = clipLeft(inside, clip[i], clip[(i + 1) % clip.size()]);
  }
  return inside.size() >= 3 ? std::max(signedArea(inside), 0.0) : 0.0;
}

double segmentDistance(const Vec2d& p, const Vec2d& a, const Vec2d& b)
{
  const auto ab = sub2(b, a);
  const auto ap = sub2(p, a);
  const auto ll = ab[0] * ab[0] + ab[1] * ab[1];
  const auto t =
    ll > 0 ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1]) / ll, 0.0, 1.0) : 0.0;
  const auto d = Vec2d{ap[0] - ab[0] * t, ap[1] - ab[1] * t};
  return std::sqrt(d[0] * d[0] + d[1] * d[1]);
}

// p inside the convex counter-clockwise polygon, or within tolerance of its boundary
bool nearConvex(const Vec2d& p, const std::vector<Vec2d>& polygon, const double tolerance)
{
  auto inside = true;
  auto nearest = std::numeric_limits<double>::infinity();
  for (auto i = std::size_t{0}; i < polygon.size(); ++i)
  {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1) % polygon.size()];
    inside = inside && cross2(sub2(b, a), sub2(p, a)) >= 0;
    nearest = std::min(nearest, segmentDistance(p, a, b));
  }
  return inside || nearest <= tolerance;
}

bool nearAny(
  const Vec2d& p, const std::vector<std::vector<Vec2d>>& polygons, const double tolerance)
{
  return std::ranges::any_of(
    polygons, [&](const auto& polygon) { return nearConvex(p, polygon, tolerance); });
}

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             const auto lower = [](const char c) {
               return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
             };
             return lower(a) == lower(b);
           });
}

// the unit of the last place of a half float of magnitude x (F32toF16 truncates, and
// flushes below 2^-14 to zero)
double halfUlp(const double x)
{
  constexpr auto Smallest = 1.0 / 16384.0;
  if (x < Smallest)
  {
    return Smallest;
  }
  auto exponent = 0;
  static_cast<void>(std::frexp(x, &exponent));
  return std::ldexp(1.0, exponent - 11);
}

// the piece's st through dmap's rule at every island vertex, against the vertex's
bool texVecMatches(
  const SurfaceView& view, const ProcIsland& island, const AdoptionPiece& piece)
{
  const auto sizes = std::array<float, 2>{piece.textureWidth, piece.textureHeight};
  for (const auto k : island.verts)
  {
    const auto p = view.position(k);
    const auto st = view.st(k);
    const auto q = EngineVec3{
      p[0] + piece.stOrigin[0], p[1] + piece.stOrigin[1], p[2] + piece.stOrigin[2]};
    for (auto i = std::size_t{0}; i < 2; ++i)
    {
      const auto& v = piece.texVec[i];
      // dmap/usurface.cpp:233-242 TriListForSide
      const auto value = (q[0] * v[0] + q[1] * v[1] + q[2] * v[2] + v[3]) / sizes[i];
      const auto snap =
        (std::abs(double(v[0])) + std::abs(double(v[1])) + std::abs(double(v[2])))
        * double(AdoptionEpsilon) / double(sizes[i]);
      const auto tolerance =
        2.0 * halfUlp(std::max(std::abs(double(value)), std::abs(double(st[i])))) + 0.001
        + snap;
      if (std::abs(double(value) - double(st[i])) > tolerance)
      {
        return false;
      }
    }
  }
  return true;
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

IslandMatch matchProcIsland(
  const ProcSurface& surface,
  const ProcIsland& island,
  const std::vector<const AdoptionPiece*>& pieces)
{
  auto match = IslandMatch{};
  if (!island.planar)
  {
    match.verdict = IslandVerdict::Curved;
    return match;
  }
  const auto view = SurfaceView{surface};
  if (!view.hasGeometry())
  {
    return match;
  }
  const auto n = Vec3d{island.plane.a, island.plane.b, island.plane.c};
  const auto frame = PlaneFrame{n};
  const auto epsilon = double(AdoptionEpsilon);
  const auto distance = double(AdoptionDistance);

  // the island in its plane: its vertices and its triangles
  auto centroid = Vec3d{0, 0, 0};
  auto islandPoints = std::vector<Vec2d>{};
  islandPoints.reserve(island.verts.size());
  for (const auto k : island.verts)
  {
    const auto p = view.position(k);
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      centroid[c] += double(p[c]);
    }
    islandPoints.push_back(frame.project(p));
  }
  for (auto& c : centroid)
  {
    c /= double(island.verts.size());
  }
  auto triangles = std::vector<std::vector<Vec2d>>{};
  triangles.reserve(island.indexes.size() / 3);
  for (auto i = std::size_t{0}; i + 2 < island.indexes.size(); i += 3)
  {
    auto triangle = std::vector<Vec2d>{
      frame.project(view.position(island.indexes[i])),
      frame.project(view.position(island.indexes[i + 1])),
      frame.project(view.position(island.indexes[i + 2]))};
    makeCounterClockwise(triangle);
    triangles.push_back(std::move(triangle));
  }

  // the candidates: material, plane, texture vectors, overlap
  auto polygons = std::vector<std::vector<Vec2d>>{};
  auto candidateArea = 0.0;
  auto candidatePerimeter = 0.0;
  for (auto i = std::size_t{0}; i < pieces.size(); ++i)
  {
    const auto& piece = *pieces[i];
    if (piece.points.size() < 3 || !equalsNoCase(piece.material, surface.material))
    {
      continue;
    }
    const auto pn = Vec3d{piece.plane.a, piece.plane.b, piece.plane.c};
    if (
      dot(pn, n) < double(PlanarCosine)
      || std::abs(dot(pn, centroid) + double(piece.plane.d)) > epsilon)
    {
      continue;
    }
    auto points = std::vector<EngineVec3>{piece.points};
    const auto bounds = engineBoundsOfPoints(points);
    auto apart = false;
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      apart = apart || bounds[0][c] > island.bounds[1][c] + AdoptionEpsilon
              || bounds[1][c] < island.bounds[0][c] - AdoptionEpsilon;
    }
    if (apart || !texVecMatches(view, island, piece))
    {
      continue;
    }
    auto polygon = std::vector<Vec2d>{};
    polygon.reserve(piece.points.size());
    for (const auto& p : piece.points)
    {
      polygon.push_back(frame.project(p));
    }
    makeCounterClockwise(polygon);
    auto shared = 0.0;
    for (const auto& triangle : triangles)
    {
      shared += sharedArea(polygon, triangle);
    }
    const auto edge = perimeter(polygon);
    if (shared <= epsilon * edge)
    {
      continue;
    }
    match.candidates.push_back(i);
    candidateArea += signedArea(polygon);
    candidatePerimeter += edge;
    polygons.push_back(std::move(polygon));
  }
  if (match.candidates.empty())
  {
    return match;
  }

  // two-way containment in the plane, then the areas
  for (const auto& p : islandPoints)
  {
    if (!nearAny(p, polygons, distance))
    {
      match.verdict = IslandVerdict::IslandOutside;
      return match;
    }
  }
  for (const auto& polygon : polygons)
  {
    for (const auto& p : polygon)
    {
      if (!nearAny(p, triangles, distance))
      {
        match.verdict = IslandVerdict::PieceOutside;
        return match;
      }
    }
  }
  // no overlap beyond a sliver, so that the summed area is the union's
  auto boxes = std::vector<std::array<Vec2d, 2>>{};
  boxes.reserve(polygons.size());
  for (const auto& polygon : polygons)
  {
    auto box = std::array<Vec2d, 2>{polygon.front(), polygon.front()};
    for (const auto& p : polygon)
    {
      for (auto c = std::size_t{0}; c < 2; ++c)
      {
        box[0][c] = std::min(box[0][c], p[c]);
        box[1][c] = std::max(box[1][c], p[c]);
      }
    }
    boxes.push_back(box);
  }
  for (auto i = std::size_t{0}; i < polygons.size(); ++i)
  {
    for (auto j = i + 1; j < polygons.size(); ++j)
    {
      if (
        boxes[i][0][0] >= boxes[j][1][0] || boxes[j][0][0] >= boxes[i][1][0]
        || boxes[i][0][1] >= boxes[j][1][1] || boxes[j][0][1] >= boxes[i][1][1])
      {
        continue;
      }
      const auto sliver =
        epsilon * std::min(perimeter(polygons[i]), perimeter(polygons[j]));
      if (sharedArea(polygons[i], polygons[j]) > sliver)
      {
        match.verdict = IslandVerdict::Overlap;
        return match;
      }
    }
  }
  if (std::abs(candidateArea - double(island.area)) > candidatePerimeter * epsilon)
  {
    match.verdict = IslandVerdict::Area;
    return match;
  }
  match.verdict = IslandVerdict::Adopted;
  return match;
}

} // namespace tb::lit
