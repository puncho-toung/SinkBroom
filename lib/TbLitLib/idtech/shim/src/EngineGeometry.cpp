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
// renderer/tr_trisurf.cpp, idlib/MapFile.cpp, tools/compilers/dmap/map.cpp,
// dmap/output.cpp, dmap/tritools.cpp and dmap/usurface.cpp):
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
 * material uses the mikktspace keyword, so MikkTSpace itself is not copied). The facade
 * never asks for MikkTSpace (mirror.json M-ID-DIFF-04): a material with the keyword takes
 * the unsmoothed path with a warning (lit/ChunkBuilder.h). Were it called, it would fail,
 * and the copied R_DeriveTangentsWithoutNormals would warn and fall back to its own
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

std::string engineProcFloatText(const float value)
{
  // dmap/output.cpp:95-105 WriteFloat
  if (idMath::Fabs(value - idMath::Rint(value)) < 0.001)
  {
    char buffer[32];
    idStr::snPrintf(buffer, sizeof(buffer), "%i", int(idMath::Rint(value)));
    return buffer;
  }
  // framework/File.cpp:73-80, a "%f" of two characters
  idStr text;
  sprintf(text, "%1.10f", double(value));
  text.StripTrailing('0');
  text.StripTrailing('.');
  return text.c_str();
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

  // dmap/map.cpp:443-469 ParsePatch: idMapPatch( info[0], info[1] ), SetSize, the
  // control points (MapFile.cpp:330-352), then the copy of the patch subdivided
  auto cp = idSurface_Patch{patch.width, patch.height};
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
    R_DeriveTangentsWithoutNormals(tri.get(), false);
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
