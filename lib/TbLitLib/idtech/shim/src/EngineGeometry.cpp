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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/File.cpp, renderer/Model.cpp,
// renderer/tr_trisurf.cpp, idlib/Lib.cpp, idlib/MapFile.cpp,
// idlib/geometry/Surface_Patch.cpp,
// tools/compilers/dmap/map.cpp, dmap/output.cpp, dmap/tritools.cpp and
// dmap/usurface.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
//   Copyright (C) 2020 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The geometry facade (lit/EngineGeometry.h). The copied ranges of
// renderer/tr_trisurf.cpp are compiled here, because R_DuplicateMirroredVertexes,
// R_CreateSilRemap and R_FaceNegativePolarity are static in that file; before them the
// shim defines the one static the copy calls but the copy leaves out,
// R_DeriveMikktspaceTangents (Q18).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineGeometry.h"
// clang-format on

/**
 * tr_trisurf.cpp:876-883's R_DeriveMikktspaceTangents, not copied (Q18: no retail or user
 * material uses the mikktspace keyword, so MikkTSpace itself is not copied). A material
 * with the keyword asks for it (EngineTangentPath::Mikktspace, mirror.json M-ID-DIFF-04):
 * it fails, and the copied R_DeriveTangentsWithoutNormals warns and falls back to its own
 * derivation, as the engine does when MikkTSpace fails.
 */
static bool R_DeriveMikktspaceTangents(srfTriangles_t*)
{
  return false;
}

// the copied ranges (renderer/tr_trisurf.cpp is HEADER_FILE_ONLY in the build)
#include "renderer/tr_trisurf.cpp"

// RenderCommon.h's tr, reduced to its tangent counter (constant-initialised)
idRenderSystemLocal tr;

namespace tb::lit
{
namespace
{

static_assert(sizeof(idDrawVert) == sizeof(EngineDrawVert));
static_assert(sizeof(idPlane) == sizeof(EnginePlane));

idDrawVert toId(const EngineDrawVert& vert)
{
  auto result = idDrawVert{};
  std::memcpy(&result, &vert, sizeof(result));
  return result;
}

EngineDrawVert fromId(const idDrawVert& vert)
{
  auto result = EngineDrawVert{};
  std::memcpy(&result, &vert, sizeof(result));
  return result;
}

idVec3 toId(const EngineVec3& v)
{
  return idVec3{v[0], v[1], v[2]};
}

EngineVec3 fromId(const idVec3& v)
{
  return {v.x, v.y, v.z};
}

idPlane toId(const EnginePlane& plane)
{
  return idPlane{plane.a, plane.b, plane.c, plane.d};
}

EnginePlane fromId(const idPlane& plane)
{
  return {plane[0], plane[1], plane[2], plane[3]};
}

std::vector<EngineVec3> pointsOf(const idWinding* winding)
{
  auto points = std::vector<EngineVec3>{};
  if (winding)
  {
    points.reserve(std::size_t(winding->GetNumPoints()));
    for (auto i = 0; i < winding->GetNumPoints(); ++i)
    {
      points.push_back(fromId((*winding)[i].ToVec3()));
    }
  }
  return points;
}

// the stand-in's arrays are Mem_Alloc16 blocks (R_AllocStaticTriSurf*)
void freeTriSurf(srfTriangles_t* tri)
{
  if (tri)
  {
    Mem_Free(tri->verts);
    Mem_Free(tri->indexes);
    Mem_Free(tri->silIndexes);
    Mem_Free(tri->mirroredVerts);
    Mem_Free(tri->dupVerts);
    Mem_Free(tri->dominantTris);
    Mem_Free(tri);
  }
}

struct TriSurfDeleter
{
  void operator()(srfTriangles_t* tri) const { freeTriSurf(tri); }
};

using TriSurf = std::unique_ptr<srfTriangles_t, TriSurfDeleter>;

TriSurf makeTriSurf(
  const std::vector<EngineDrawVert>& verts, const std::vector<std::uint16_t>& indexes)
{
  auto tri = TriSurf{R_AllocStaticTriSurf()};
  tri->numVerts = int(verts.size());
  tri->numIndexes = int(indexes.size());
  R_AllocStaticTriSurfVerts(tri.get(), tri->numVerts);
  R_AllocStaticTriSurfIndexes(tri.get(), tri->numIndexes);
  if (!verts.empty())
  {
    std::memcpy(tri->verts, verts.data(), verts.size() * sizeof(idDrawVert));
  }
  if (!indexes.empty())
  {
    std::memcpy(tri->indexes, indexes.data(), indexes.size() * sizeof(triIndex_t));
  }
  return tri;
}

EngineTriangles trianglesOf(const srfTriangles_t& tri)
{
  auto result = EngineTriangles{};
  result.verts.resize(std::size_t(tri.numVerts));
  if (tri.numVerts > 0)
  {
    std::memcpy(result.verts.data(), tri.verts, result.verts.size() * sizeof(idDrawVert));
  }
  result.indexes.assign(tri.indexes, tri.indexes + tri.numIndexes);
  result.numMirroredVerts = tri.numMirroredVerts;
  if (tri.numMirroredVerts > 0)
  {
    result.mirroredVerts.assign(
      tri.mirroredVerts, tri.mirroredVerts + tri.numMirroredVerts);
  }
  return result;
}

} // namespace

bool EngineDrawVert::operator==(const EngineDrawVert& other) const
{
  return std::memcmp(this, &other, sizeof(EngineDrawVert)) == 0;
}

bool EngineDrawVert::operator!=(const EngineDrawVert& other) const
{
  return !(*this == other);
}

std::uint16_t engineF32toF16(const float value)
{
  return F32toF16(value);
}

float engineF16toF32(const std::uint16_t value)
{
  return F16toF32(value);
}

void engineSetTexCoord(EngineDrawVert& vert, const float s, const float t)
{
  auto v = toId(vert);
  v.SetTexCoord(s, t);
  vert = fromId(v);
}

EngineVec2 engineTexCoord(const EngineDrawVert& vert)
{
  const auto st = toId(vert).GetTexCoord();
  return {st.x, st.y};
}

void engineSetNormal(EngineDrawVert& vert, const EngineVec3& normal)
{
  auto v = toId(vert);
  v.SetNormal(toId(normal));
  vert = fromId(v);
}

EngineVec3 engineNormal(const EngineDrawVert& vert)
{
  return fromId(toId(vert).GetNormal());
}

EngineVec3 engineTangent(const EngineDrawVert& vert)
{
  return fromId(toId(vert).GetTangent());
}

float engineBiTangentSign(const EngineDrawVert& vert)
{
  return toId(vert).GetBiTangentSign();
}

EngineDrawVert engineClearedVert()
{
  auto v = idDrawVert{};
  v.Clear();
  // RenderWorld_load.cpp ParseModel's opaque white (C4); color2 stays zero
  v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
  return fromId(v);
}

std::uint32_t enginePackColorRound(const std::array<float, 4>& rgba)
{
  // Lib.cpp:175-182 PackColorRound: "Rounds to the nearest byte; Ftob clamps, so values
  // outside 0..1 pack as 0 or 255"
  const auto dx = idMath::Ftob(rgba[0] * 255.0f + 0.5f);
  const auto dy = idMath::Ftob(rgba[1] * 255.0f + 0.5f);
  const auto dz = idMath::Ftob(rgba[2] * 255.0f + 0.5f);
  const auto dw = idMath::Ftob(rgba[3] * 255.0f + 0.5f);
  return (std::uint32_t(dx) << 0) | (std::uint32_t(dy) << 8) | (std::uint32_t(dz) << 16)
         | (std::uint32_t(dw) << 24);
}

void engineSetColor(EngineDrawVert& vert, const std::uint32_t color)
{
  auto v = toId(vert);
  v.SetColor(dword(color));
  vert = fromId(v);
}

std::optional<EnginePlane> engineSidePlane(
  const EngineVec3& p0, const EngineVec3& p1, const EngineVec3& p2)
{
  auto plane = idPlane{};
  if (!plane.FromPoints(toId(p0), toId(p1), toId(p2)))
  {
    return std::nullopt;
  }
  // dmap/map.cpp:76-85 FindFloatPlane, DIST_EPSILON 0.01f
  plane.FixDegeneracies(0.01f);
  return fromId(plane);
}

bool enginePlanesEqual(
  const EnginePlane& lhs,
  const EnginePlane& rhs,
  const float normalEpsilon,
  const float distEpsilon)
{
  return toId(lhs).Compare(toId(rhs), normalEpsilon, distEpsilon);
}

EnginePlane engineFlippedPlane(const EnginePlane& plane)
{
  return fromId(-toId(plane));
}

std::optional<EnginePlane> engineWindingPlane(const std::vector<EngineVec3>& points)
{
  if (points.size() < 3 || points.size() > std::size_t(engineBounds::MaxWindingPoints))
  {
    return std::nullopt;
  }
  auto ids = std::vector<idVec3>{};
  ids.reserve(points.size());
  for (const auto& point : points)
  {
    ids.push_back(toId(point));
  }
  const auto winding = idWinding{ids.data(), int(ids.size())};
  auto plane = idPlane{};
  winding.GetPlane(plane);
  return fromId(plane);
}

float enginePlaneDistance(const EnginePlane& plane, const EngineVec3& point)
{
  return toId(plane).Distance(toId(point));
}

std::optional<EnginePlane> enginePlaneFromCrossThroughPoint(
  const EngineVec3& v2, const EngineVec3& v1, const EngineVec3& point)
{
  // RenderWorld_portals.cpp:629-636
  auto plane = idPlane{};
  plane.Normal().Cross(toId(v2), toId(v1));

  // if it is degenerate, skip the plane
  if (plane.Normalize() < 0.01f)
  {
    return std::nullopt;
  }
  plane.FitThroughPoint(toId(point));
  return fromId(plane);
}

std::optional<std::vector<EngineVec3>> engineClipWindingInPlace(
  const std::vector<EngineVec3>& points, const EnginePlane& plane, const float epsilon)
{
  if (points.size() > std::size_t(engineBounds::MaxClipWindingPoints))
  {
    return points;
  }
  auto ids = std::vector<idVec3>{};
  ids.reserve(points.size());
  for (const auto& point : points)
  {
    ids.push_back(toId(point));
  }
  auto winding = idFixedWinding{ids.data(), int(ids.size())};
  if (!winding.ClipInPlace(toId(plane), epsilon, false))
  {
    return std::nullopt;
  }
  return pointsOf(&winding);
}

std::optional<std::vector<EngineVec3>> engineReverseWinding(
  const std::vector<EngineVec3>& points)
{
  if (points.size() > std::size_t(engineBounds::MaxWindingPoints))
  {
    return std::nullopt;
  }
  auto ids = std::vector<idVec3>{};
  ids.reserve(points.size());
  for (const auto& point : points)
  {
    ids.push_back(toId(point));
  }
  const auto winding = idWinding{ids.data(), int(ids.size())};
  const auto* reversed = winding.Reverse();
  auto result = pointsOf(reversed);
  delete reversed;
  return result;
}

std::optional<EngineSplit> engineSplitWinding(
  const std::vector<EngineVec3>& points, const EnginePlane& plane, const float epsilon)
{
  // C24 (2): Split's FatalError ("points exceeded estimate") cannot be reached by a
  // convex winding, and its _alloca blocks grow with the point count
  if (points.size() < 3 || points.size() > std::size_t(engineBounds::MaxWindingPoints))
  {
    return std::nullopt;
  }
  auto ids = std::vector<idVec3>{};
  ids.reserve(points.size());
  for (const auto& point : points)
  {
    ids.push_back(toId(point));
  }
  const auto winding = idWinding{ids.data(), int(ids.size())};
  idWinding* front = nullptr;
  idWinding* back = nullptr;
  const auto side = winding.Split(toId(plane), epsilon, &front, &back);

  auto result = EngineSplit{};
  result.side = EngineSplitSide(side);
  // a winding on the plane is copied whole to the side its normal agrees with
  result.front = pointsOf(front);
  result.back = pointsOf(back);
  delete front;
  delete back;
  return result;
}

float engineTriangleArea(const EngineVec3& a, const EngineVec3& b, const EngineVec3& c)
{
  return idWinding::TriangleArea(toId(a), toId(b), toId(c));
}

int engineFtoi(const float value)
{
  return idMath::Ftoi(value);
}

EngineMat3 engineAnglesToMat3(const float pitch, const float yaw, const float roll)
{
  const auto m = idAngles{pitch, yaw, roll}.ToMat3();
  return {
    m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
}

EngineMat3 engineMat3Multiply(const EngineMat3& lhs, const EngineMat3& rhs)
{
  const auto a =
    idMat3{lhs[0], lhs[1], lhs[2], lhs[3], lhs[4], lhs[5], lhs[6], lhs[7], lhs[8]};
  const auto b =
    idMat3{rhs[0], rhs[1], rhs[2], rhs[3], rhs[4], rhs[5], rhs[6], rhs[7], rhs[8]};
  const auto m = a * b;
  return {
    m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
}

std::array<EngineVec3, 2> engineBoundsOfPoints(const std::vector<EngineVec3>& points)
{
  auto ids = std::vector<idVec3>{};
  ids.reserve(points.size());
  for (const auto& point : points)
  {
    ids.push_back(toId(point));
  }
  auto bounds = idBounds{};
  bounds.FromPoints(ids.data(), int(ids.size()));
  return {fromId(bounds[0]), fromId(bounds[1])};
}

EngineVec2 engineValveTexCoord(
  const EngineVec3& xyz,
  const EngineValveTexture& texture,
  const float textureWidth,
  const float textureHeight)
{
  // MapFile.cpp:104-117 GetTextureVectors (PROJECTION_VALVE220)
  idVec4 v[2];
  v[0][0] = texture.uAxis[0] * (1.0f / texture.uScale);
  v[0][1] = texture.uAxis[1] * (1.0f / texture.uScale);
  v[0][2] = texture.uAxis[2] * (1.0f / texture.uScale);
  v[0][3] = texture.uOffset;
  v[1][0] = texture.vAxis[0] * (1.0f / texture.vScale);
  v[1][1] = texture.vAxis[1] * (1.0f / texture.vScale);
  v[1][2] = texture.vAxis[2] * (1.0f / texture.vScale);
  v[1][3] = texture.vOffset;

  // dmap/usurface.cpp:233-242 TriListForSide
  const auto position = toId(xyz);
  idVec2 st;
  st.x = (position * v[0].ToVec3()) + v[0][3];
  st.y = (position * v[1].ToVec3()) + v[1][3];
  st.x /= textureWidth;
  st.y /= textureHeight;
  return {st.x, st.y};
}

namespace
{

/**
 * V.6: "%1.10f" of a float as the "C" locale prints it, without the C library, whose
 * decimal point follows LC_NUMERIC (Qt sets it from the environment on Unix): the exact
 * value rounded to ten decimals, a tie to even, as the C runtimes round. For the values
 * WriteFloat formats so (not within 0.001 of an integer, so below 2^23 in magnitude).
 */
std::string fixedTenDecimals(const float value)
{
  if (!std::isfinite(value))
  {
    return std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
  }
  auto exponent = 0;
  const auto fraction = std::frexp(double(value), &exponent);
  // |value| = m / 2^shift, m below 2^24
  const auto m = std::uint64_t(std::fabs(std::ldexp(fraction, 24)));
  const auto shift = 24 - exponent;
  constexpr auto Scale = std::uint64_t{10000000000};
  auto q = std::uint64_t{0};
  if (shift < 1)
  {
    // an integer of 2^23 or more (WriteFloat prints those with %i)
    q = std::uint64_t(std::fabs(double(value))) * Scale;
  }
  else if (shift < 64)
  {
    // round( m * 10^10 / 2^shift ): m * 10^10 is below 2^58
    const auto product = m * Scale;
    q = product >> shift;
    const auto rest = product & ((std::uint64_t{1} << shift) - 1);
    const auto half = std::uint64_t{1} << (shift - 1);
    if (rest > half || (rest == half && (q & 1) != 0))
    {
      ++q;
    }
  }
  const auto digits = std::to_string(q % Scale);
  return (std::signbit(value) ? "-" : "") + std::to_string(q / Scale) + "."
         + std::string(10 - digits.size(), '0') + digits;
}

} // namespace

std::string engineProcFloatText(const float value)
{
  // dmap/output.cpp:95-105 WriteFloat
  if (idMath::Fabs(value - idMath::Rint(value)) < 0.001)
  {
    char buffer[32];
    idStr::snPrintf(buffer, sizeof(buffer), "%i", int(idMath::Rint(value)));
    return buffer;
  }
  // framework/File.cpp:73-80, a "%f" of two characters: "%1.10f" (V.6: in the "C"
  // locale), then idStr::StripTrailing of every trailing '0', then of the '.'
  auto text = fixedTenDecimals(value);
  while (!text.empty() && text.back() == '0')
  {
    text.pop_back();
  }
  while (!text.empty() && text.back() == '.')
  {
    text.pop_back();
  }
  return text;
}

namespace
{

using PatchPoint = std::array<float, 3>;
using PatchLine = std::vector<PatchPoint>;

// Surface_Patch.cpp:262-266 LerpVert's position
PatchPoint lerpPoint(const PatchPoint& a, const PatchPoint& b)
{
  return {0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2])};
}

PatchLine lerpLine(const PatchLine& a, const PatchLine& b)
{
  auto line = PatchLine(a.size());
  for (auto i = std::size_t{0}; i < a.size(); ++i)
  {
    line[i] = lerpPoint(a[i], b[i]);
  }
  return line;
}

// Surface_Patch.cpp:690-693, :705-706 (and :754-756, :767-768): the midpoint of a span
// of three points off far enough to subdivide, Square( 4.0f ) for both directions
bool spanTooCurved(const PatchLine& a, const PatchLine& b, const PatchLine& c)
{
  for (auto i = std::size_t{0}; i < a.size(); ++i)
  {
    auto delta = PatchPoint{};
    for (auto l = std::size_t{0}; l < 3; ++l)
    {
      const auto mid = (a[i][l] + b[i][l] * 2.0f + c[i][l]) * 0.25f;
      delta[l] = b[i][l] - mid;
    }
    if (delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] > 16.0f)
    {
      return true;
    }
  }
  return false;
}

// V.3: more halvings than a real patch ever needs (each divides a span's deviation by 4)
constexpr auto MaxSpanHalvings = 30;

/**
 * One span of Subdivide's pass (Surface_Patch.cpp:683-742 or :745-804): a span whose
 * midpoint is off too far gets two new lines, prev, mid and next replacing the peak, and
 * its left half is checked again before its right one. Appends the lines after a to out
 * (when out is given) and counts them; stops once count exceeds limit. V.3: a span still
 * too curved after MaxSpanHalvings is one that float rounding keeps curved for ever
 * (coordinates beyond about 2^26), where Subdivide would never end: the count is set past
 * the limit, so that the patch is refused.
 */
void subdivideSpan(
  const PatchLine& a,
  const PatchLine& b,
  const PatchLine& c,
  std::vector<PatchLine>* out,
  std::int64_t& count,
  const std::int64_t limit,
  const int depth)
{
  if (count > limit || !spanTooCurved(a, b, c))
  {
    if (out)
    {
      out->push_back(b);
      out->push_back(c);
    }
    count += 2;
    return;
  }
  if (depth >= MaxSpanHalvings)
  {
    count = limit + 1;
    return;
  }
  const auto prev = lerpLine(a, b);
  const auto next = lerpLine(b, c);
  const auto mid = lerpLine(prev, next);
  subdivideSpan(a, prev, mid, out, count, limit, depth + 1);
  subdivideSpan(mid, next, c, out, count, limit, depth + 1);
}

} // namespace

namespace
{

// M-GEO-FN-17: idSurface_Patch's private members the transcription below calls, read
// without changing the copied class: an explicit instantiation may name a private member
// ([temp.explicit]), and the friend function it defines hands the member pointer out
struct GenerateNormalsTag
{
  using type = void (idSurface_Patch::*)();
  friend type get(GenerateNormalsTag);
};

struct GenerateIndexesTag
{
  using type = void (idSurface_Patch::*)();
  friend type get(GenerateIndexesTag);
};

struct ExpandTag
{
  using type = void (idSurface_Patch::*)();
  friend type get(ExpandTag);
};

struct RemoveLinearColumnsRowsTag
{
  using type = void (idSurface_Patch::*)();
  friend type get(RemoveLinearColumnsRowsTag);
};

struct CollapseTag
{
  using type = void (idSurface_Patch::*)();
  friend type get(CollapseTag);
};

template <typename Tag, typename Tag::type Member>
struct PrivateMember
{
  friend typename Tag::type get(Tag) { return Member; }
};

template struct PrivateMember<GenerateNormalsTag, &idSurface_Patch::GenerateNormals>;
template struct PrivateMember<GenerateIndexesTag, &idSurface_Patch::GenerateIndexes>;
template struct PrivateMember<ExpandTag, &idSurface_Patch::Expand>;
template struct PrivateMember<
  RemoveLinearColumnsRowsTag,
  &idSurface_Patch::RemoveLinearColumnsRows>;
template struct PrivateMember<CollapseTag, &idSurface_Patch::Collapse>;

/**
 * M-GEO-DEFECT-01, M-GEO-FN-17 (39-PATCH-NORMAL-FLAKE.md F1): idSurface_Patch with the
 * engine's SubdivideExplicit, SampleSinglePatch and SampleSinglePatchPoint
 * (idlib/geometry/Surface_Patch.cpp:487-655) transcribed, the copy left as it is. The one
 * difference: SampleSinglePatchPoint gathers the normal's three components in a local
 * idVec3 and calls SetNormal once, as the GPL Doom 3 line commented out at :552 wrote the
 * float components. The engine's per-component GetNormal/SetNormal (:549-551)
 * renormalises the bytes the vertex already holds, and SubdivideExplicit's new
 * idDrawVert[] (:597) never initialised them: the copied code's normals are a function of
 * the heap block's earlier contents. Everything else is the copied functions' logic: the
 * same arithmetic, the same writes, the same allocation. Pool-safe as the copy is (C24,
 * M-GEO-POOL-02).
 */
class GatheredPatch : public idSurface_Patch
{
public:
  GatheredPatch(const int maxPatchWidth, const int maxPatchHeight)
    : idSurface_Patch{maxPatchWidth, maxPatchHeight}
  {
  }

  // Surface_Patch.cpp:586-655
  void SubdivideExplicit(
    const int horzSubdivisions,
    const int vertSubdivisions,
    const bool genNormals,
    const bool removeLinear = false)
  {
    idDrawVert sample[3][3];
    const int outWidth = ((width - 1) / 2 * horzSubdivisions) + 1;
    const int outHeight = ((height - 1) / 2 * vertSubdivisions) + 1;
    auto* dv = new (TAG_IDLIB_SURFACE) idDrawVert[outWidth * outHeight];

    // generate normals for the control mesh
    if (genNormals)
    {
      (this->*get(GenerateNormalsTag{}))();
    }

    int baseCol = 0;
    for (int i = 0; i + 2 < width; i += 2)
    {
      int baseRow = 0;
      for (int j = 0; j + 2 < height; j += 2)
      {
        for (int k = 0; k < 3; k++)
        {
          for (int l = 0; l < 3; l++)
          {
            sample[k][l] = verts[((j + l) * width) + i + k];
          }
        }
        SampleSinglePatch(
          sample, baseCol, baseRow, outWidth, horzSubdivisions, vertSubdivisions, dv);
        baseRow += vertSubdivisions;
      }
      baseCol += horzSubdivisions;
    }
    verts.SetNum(outWidth * outHeight);
    for (int i = 0; i < outWidth * outHeight; i++)
    {
      verts[i] = dv[i];
    }

    delete[] dv;

    width = maxWidth = outWidth;
    height = maxHeight = outHeight;
    expanded = false;

    if (removeLinear)
    {
      (this->*get(ExpandTag{}))();
      (this->*get(RemoveLinearColumnsRowsTag{}))();
      (this->*get(CollapseTag{}))();
    }

    // normalize all the lerped normals
    if (genNormals)
    {
      idVec3 tempNormal;
      for (int i = 0; i < width * height; i++)
      {
        tempNormal = verts[i].GetNormal();
        tempNormal.Normalize();
        verts[i].SetNormal(tempNormal);
      }
    }

    (this->*get(GenerateIndexesTag{}))();
  }

private:
  // Surface_Patch.cpp:487-561, the normal gathered (the one difference)
  void SampleSinglePatchPoint(
    const idDrawVert ctrl[3][3], const float u, const float v, idDrawVert* out) const
  {
    float vCtrl[3][8];

    // find the control points for the v coordinate
    for (int vPoint = 0; vPoint < 3; vPoint++)
    {
      for (int axis = 0; axis < 8; axis++)
      {
        float a, b, c;
        if (axis < 3)
        {
          a = ctrl[0][vPoint].xyz[axis];
          b = ctrl[1][vPoint].xyz[axis];
          c = ctrl[2][vPoint].xyz[axis];
        }
        else if (axis < 6)
        {
          a = ctrl[0][vPoint].GetNormal()[axis - 3];
          b = ctrl[1][vPoint].GetNormal()[axis - 3];
          c = ctrl[2][vPoint].GetNormal()[axis - 3];
        }
        else
        {
          a = ctrl[0][vPoint].GetTexCoord()[axis - 6];
          b = ctrl[1][vPoint].GetTexCoord()[axis - 6];
          c = ctrl[2][vPoint].GetTexCoord()[axis - 6];
        }
        const float qA = a - 2.0f * b + c;
        const float qB = 2.0f * b - 2.0f * a;
        const float qC = a;
        vCtrl[vPoint][axis] = qA * u * u + qB * u + qC;
      }
    }

    // interpolate the v value
    idVec3 normal;
    for (int axis = 0; axis < 8; axis++)
    {
      const float a = vCtrl[0][axis];
      const float b = vCtrl[1][axis];
      const float c = vCtrl[2][axis];
      const float qA = a - 2.0f * b + c;
      const float qB = 2.0f * b - 2.0f * a;
      const float qC = a;

      if (axis < 3)
      {
        out->xyz[axis] = qA * v * v + qB * v + qC;
      }
      else if (axis < 6)
      {
        // the GPL original: out->normal[axis-3] = qA * v * v + qB * v + qC;
        normal[axis - 3] = qA * v * v + qB * v + qC;
      }
      else
      {
        idVec2 tempST = out->GetTexCoord();
        tempST[axis - 6] = qA * v * v + qB * v + qC;
        out->SetTexCoord(tempST);
      }
    }
    out->SetNormal(normal);
  }

  // Surface_Patch.cpp:563-584
  void SampleSinglePatch(
    const idDrawVert ctrl[3][3],
    const int baseCol,
    const int baseRow,
    const int outWidth,
    int horzSub,
    int vertSub,
    idDrawVert* outVerts) const
  {
    horzSub++;
    vertSub++;
    for (int i = 0; i < horzSub; i++)
    {
      for (int j = 0; j < vertSub; j++)
      {
        const float u = float(i) / float(horzSub - 1);
        const float v = float(j) / float(vertSub - 1);
        SampleSinglePatchPoint(
          ctrl, u, v, &outVerts[((baseRow + j) * outWidth) + i + baseCol]);
      }
    }
  }
};

} // namespace

std::array<std::int64_t, 2> patchSubdivisionBound(
  const EnginePatch& patch, const std::int64_t maxVertices)
{
  const auto width = patch.width;
  const auto height = patch.height;
  if (
    width < 1 || height < 1
    || patch.controlPoints.size() != std::size_t(width) * std::size_t(height))
  {
    return {0, 0};
  }

  // the horizontal pass on the control columns (verts[i * width + j]: i the row, j the
  // column); each column is a line of height points
  auto columns =
    std::vector<PatchLine>(std::size_t(width), PatchLine(std::size_t(height)));
  for (auto i = 0; i < height; ++i)
  {
    for (auto j = 0; j < width; ++j)
    {
      columns[std::size_t(j)][std::size_t(i)] =
        patch.controlPoints[std::size_t(i * width + j)].xyz;
    }
  }
  const auto columnLimit = maxVertices / height;
  auto expanded = std::vector<PatchLine>{columns.front()};
  auto outWidth = std::int64_t{1};
  auto j = 0;
  for (; j + 2 < width; j += 2)
  {
    subdivideSpan(
      columns[std::size_t(j)],
      columns[std::size_t(j + 1)],
      columns[std::size_t(j + 2)],
      &expanded,
      outWidth,
      columnLimit,
      0);
    if (outWidth > columnLimit)
    {
      return {outWidth, height};
    }
  }
  for (++j; j < width; ++j)
  {
    expanded.push_back(columns[std::size_t(j)]);
    ++outWidth;
  }

  // the vertical pass on the rows of the expanded patch, counted only
  auto rows = std::vector<PatchLine>(std::size_t(height), PatchLine(expanded.size()));
  for (auto c = std::size_t{0}; c < expanded.size(); ++c)
  {
    for (auto r = std::size_t{0}; r < std::size_t(height); ++r)
    {
      rows[r][c] = expanded[c][r];
    }
  }
  expanded.clear();
  const auto rowLimit = maxVertices / outWidth;
  auto outHeight = std::int64_t{1};
  auto r = 0;
  for (; r + 2 < height; r += 2)
  {
    subdivideSpan(
      rows[std::size_t(r)],
      rows[std::size_t(r + 1)],
      rows[std::size_t(r + 2)],
      nullptr,
      outHeight,
      rowLimit,
      0);
    if (outHeight > rowLimit)
    {
      return {outWidth, outHeight};
    }
  }
  outHeight += height - 1 - r;
  return {outWidth, outHeight};
}

std::optional<EngineSurface> engineSubdividePatch(
  const EnginePatch& patch, std::string* error)
{
  const auto refuse = [&](const char* why) -> std::optional<EngineSurface> {
    if (error)
    {
      *error = why;
    }
    return std::nullopt;
  };

  // K19 and C24 (2): SetSize's FatalErrors need 1 <= width, height <= the allocation,
  // which the constructor below makes; the rest bounds the heap and the 16-bit indexes
  if (
    patch.width < 1 || patch.height < 1 || patch.width > engineBounds::MaxPatchSide
    || patch.height > engineBounds::MaxPatchSide)
  {
    return refuse("the patch has more than 99 control points per side, or none");
  }
  if (patch.controlPoints.size() != std::size_t(patch.width * patch.height))
  {
    return refuse("the patch's control points do not match its size");
  }
  if (
    patch.explicitSubdivisions
    && (patch.horizontalSubdivisions < 1 || patch.verticalSubdivisions < 1
        || patch.horizontalSubdivisions > engineBounds::MaxPatchSubdivisions
        || patch.verticalSubdivisions > engineBounds::MaxPatchSubdivisions))
  {
    return refuse("the patch's subdivisions are outside 1 to 64");
  }
  if (patch.explicitSubdivisions)
  {
    const auto columns =
      std::int64_t((patch.width - 1) / 2) * patch.horizontalSubdivisions + 1;
    const auto rows =
      std::int64_t((patch.height - 1) / 2) * patch.verticalSubdivisions + 1;
    if (columns * rows > engineBounds::MaxPatchVertices)
    {
      return refuse("the subdivided patch would have more than 65535 vertices");
    }
  }
  else
  {
    // R1.3: Subdivide ends in GenerateEdgeIndexes, whose two blocks are _alloca16 of 4
    // bytes per vertex and per index (Surface.cpp:964-972), so the size is bounded
    // before it runs, not after
    const auto [columns, rows] =
      patchSubdivisionBound(patch, engineBounds::MaxPatchStackBytes / 4);
    const auto stackBytes =
      4 * columns * rows + 4 * 6 * (columns - 1) * (rows - 1) + 2 * 16;
    if (stackBytes > engineBounds::MaxPatchStackBytes)
    {
      return refuse(
        "the patch's subdivision could never end or take more than 2 MB of the thread's "
        "stack (about 74,000 vertices)");
    }
  }

  // dmap/map.cpp:443-469 ParsePatch: idMapPatch( info[0], info[1] ), SetSize, the
  // control points (MapFile.cpp:330-352), then the copy of the patch subdivided
  // M-GEO-DEFECT-01: the patchDef3 path through the transcription that gathers the normal
  auto cp = GatheredPatch{patch.width, patch.height};
  cp.SetSize(patch.width, patch.height);
  for (auto i = 0; i < patch.width * patch.height; ++i)
  {
    cp[i] = toId(patch.controlPoints[std::size_t(i)]);
  }
  if (patch.explicitSubdivisions)
  {
    cp.SubdivideExplicit(patch.horizontalSubdivisions, patch.verticalSubdivisions, true);
  }
  else
  {
    // MapFile.h:54-57 DEFAULT_CURVE_MAX_ERROR, DEFAULT_CURVE_MAX_LENGTH
    cp.Subdivide(4.0f, 4.0f, -1.0f, true);
  }
  if (cp.GetNumVertices() > engineBounds::MaxPatchVertices)
  {
    return refuse("the subdivided patch has more than 65535 vertices");
  }

  auto surface = EngineSurface{};
  surface.verts.reserve(std::size_t(cp.GetNumVertices()));
  for (auto i = 0; i < cp.GetNumVertices(); ++i)
  {
    surface.verts.push_back(fromId(cp[i]));
  }
  // dmap/map.cpp:416-418 ParseSurface: v[0], v[1], v[2] = indexes i + 1, i + 2, i
  const auto* indexes = cp.GetIndexes();
  surface.indexes.reserve(std::size_t(cp.GetNumIndexes()));
  for (auto i = 0; i + 2 < cp.GetNumIndexes(); i += 3)
  {
    surface.indexes.push_back(indexes[i + 1]);
    surface.indexes.push_back(indexes[i + 2]);
    surface.indexes.push_back(indexes[i + 0]);
  }
  return surface;
}

std::optional<EngineTriangles> engineCleanupTriangles(
  std::vector<EngineDrawVert> verts,
  std::vector<std::uint16_t> indexes,
  const EngineTangentPath path)
{
  // C24 (2): R_RangeCheckIndexes' Errors as pre-validation; R_BuildDominantTris' Error
  // is reached only when an allocation fails
  if (
    verts.size() > std::size_t(engineBounds::MaxSurfaceVertices)
    || indexes.size() % 3 != 0
    || indexes.size() > std::size_t(std::numeric_limits<int>::max() / 4))
  {
    return std::nullopt;
  }
  for (const auto index : indexes)
  {
    if (index >= verts.size())
    {
      return std::nullopt;
    }
  }
  if (indexes.empty())
  {
    auto result = EngineTriangles{};
    result.verts = std::move(verts);
    return result;
  }

  // tr_trisurf.cpp:1810-1847 R_CleanupTriangles( tri, false, true, unsmoothed, false )
  auto tri = makeTriSurf(verts, indexes);
  R_CreateSilIndexes(tri.get());
  R_RemoveDegenerateTriangles(tri.get());
  R_DuplicateMirroredVertexes(tri.get());
  if (path == EngineTangentPath::Unsmoothed)
  {
    R_BuildDominantTris(tri.get());
    R_DeriveTangents(tri.get());
  }
  else
  {
    R_DeriveTangentsWithoutNormals(tri.get(), path == EngineTangentPath::Mikktspace);
  }
  return trianglesOf(*tri);
}

EngineTriangles engineReverseTriangles(
  const std::vector<EngineDrawVert>& verts, const std::vector<std::uint16_t>& indexes)
{
  const auto source = makeTriSurf(verts, indexes);
  // Model.cpp:1265-1266
  auto copy = TriSurf{R_CopyStaticTriSurf(source.get())};
  R_ReverseTriangles(copy.get());
  return trianglesOf(*copy);
}

EngineDrawVert engineVertFromOriginal(
  const EngineVec3& xyz, const std::array<EngineDrawVert, 3>& original)
{
  // dmap/tritools.cpp:262-300 TriVertsFromOriginal, for one corner
  auto result = toId(original[0]);
  result.xyz = toId(xyz);
  const auto o0 = toId(original[0]);
  const auto o1 = toId(original[1]);
  const auto o2 = toId(original[2]);
  const auto denom = idWinding::TriangleArea(o0.xyz, o1.xyz, o2.xyz);
  if (denom == 0)
  {
    // original was degenerate, so it doesn't matter
    return fromId(result);
  }
  const auto a = idWinding::TriangleArea(result.xyz, o1.xyz, o2.xyz) / denom;
  const auto b = idWinding::TriangleArea(result.xyz, o2.xyz, o0.xyz) / denom;
  const auto c = idWinding::TriangleArea(result.xyz, o0.xyz, o1.xyz) / denom;

  const idVec2 aST = o0.GetTexCoord();
  const idVec2 bST = o1.GetTexCoord();
  const idVec2 cST = o2.GetTexCoord();
  result.SetTexCoord(
    a * aST.x + b * bST.x + c * cST.x, a * aST.y + b * bST.y + c * cST.y);

  idVec3 tempNormal;
  for (auto j = 0; j < 3; j++)
  {
    tempNormal[j] = a * o0.GetNormal()[j] + b * o1.GetNormal()[j] + c * o2.GetNormal()[j];
  }
  tempNormal.Normalize();
  result.SetNormal(tempNormal);
  return fromId(result);
}

} // namespace tb::lit
