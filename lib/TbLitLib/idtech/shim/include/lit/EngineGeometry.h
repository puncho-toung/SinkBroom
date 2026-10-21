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

// The facade over the engine's geometry code copied into TbLitIdLib (D57, C24):
// idDrawVert's encoders, idPlane, idWinding::Split, idSurface_Patch and the copied
// tangent ranges of tr_trisurf.cpp. It uses standard types only and compiles as C++17 and
// C++20.
//
// Threading (C24): every function here may run on any thread, the session's CPU pool
// included. The copied code they call reports through the permanent sink of D63 (the
// caller drains its thread's messages, lit/MaterialDecls.h), allocates with the tagged
// new and Mem_Alloc16, which the shim forwards to the global allocator, calls the scalar
// SIMDProcessor stand-in, and reads only constant statics (mirror.json M-GEO-*). Every
// FatalError site the copies hold is unreachable for the inputs these functions accept:
// each validates its input first (the bounds below, K19) and refuses what the copy could
// not take. Nothing here touches the registry, so no function needs the parse thread.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

using EngineVec2 = std::array<float, 2>;
using EngineVec3 = std::array<float, 3>;

/**
 * idDrawVert, byte for byte (DrawVert.h:115-136, F25): xyz, st as two half floats,
 * normal and tangent as bytes (tangent[3] is the bitangent sign), color and color2.
 */
struct EngineDrawVert
{
  EngineVec3 xyz = {0, 0, 0};
  std::array<std::uint16_t, 2> st = {0, 0};
  std::array<std::uint8_t, 4> normal = {0, 0, 0, 0};
  std::array<std::uint8_t, 4> tangent = {0, 0, 0, 0};
  std::array<std::uint8_t, 4> color = {0, 0, 0, 0};
  std::array<std::uint8_t, 4> color2 = {0, 0, 0, 0};

  bool operator==(const EngineDrawVert& other) const;
  bool operator!=(const EngineDrawVert& other) const;
};

static_assert(sizeof(EngineDrawVert) == 32);

// idPlane's four floats: Distance(p) = a·x + b·y + c·z + d
struct EnginePlane
{
  float a = 0;
  float b = 0;
  float c = 0;
  float d = 0;
};

// the bounds the functions below validate their input against (K19, C24 (2))
namespace engineBounds
{
// the points of a winding idWinding::Split takes: its _alloca blocks stay below 32 KB
inline constexpr int MaxWindingPoints = 1024;
// a patch's control points per side; the dmap and engine patch code allocates on the heap
inline constexpr int MaxPatchSide = 99;
// explicit patchDef3 subdivisions per side
inline constexpr int MaxPatchSubdivisions = 64;
// what a subdivided patch may have: its triangles are indexed with 16 bits
inline constexpr int MaxPatchVertices = 65535;
// the vertices of a triangle surface the tangent code takes (triIndex_t)
inline constexpr int MaxSurfaceVertices = 65535;
} // namespace engineBounds

// ----- idDrawVert's encoders (pure) -----

// F32toF16 and F16toF32 (DrawVert.h:59-105): truncating, flushing below 2^-14 to zero
std::uint16_t engineF32toF16(float value);
float engineF16toF32(std::uint16_t value);

// idDrawVert::SetTexCoord, GetTexCoord
void engineSetTexCoord(EngineDrawVert& vert, float s, float t);
EngineVec2 engineTexCoord(const EngineDrawVert& vert);

// idDrawVert::SetNormal, GetNormal, GetTangent, GetBiTangentSign
void engineSetNormal(EngineDrawVert& vert, const EngineVec3& normal);
EngineVec3 engineNormal(const EngineDrawVert& vert);
EngineVec3 engineTangent(const EngineDrawVert& vert);
float engineBiTangentSign(const EngineDrawVert& vert);

// idDrawVert::Clear with C4's colours: white, color2 zero
EngineDrawVert engineClearedVert();

// ----- planes and windings (pure) -----

/**
 * A map brush side's plane as dmap has it: idPlane::FromPoints of the side's three points
 * (MapFile.cpp ParseValve220, already relative to the entity's origin), then
 * FixDegeneracies(DIST_EPSILON 0.01), as FindFloatPlane does (dmap/map.cpp:76-85). Null
 * for three collinear points.
 */
std::optional<EnginePlane> engineSidePlane(
  const EngineVec3& p0, const EngineVec3& p1, const EngineVec3& p2);

// idPlane::Compare(p, normalEps, distEps): dmap's plane identity uses NORMAL_EPSILON
// 0.00001 and DIST_EPSILON 0.01 (dmap/map.cpp:68-69)
bool enginePlanesEqual(
  const EnginePlane& lhs, const EnginePlane& rhs, float normalEpsilon, float distEpsilon);
EnginePlane engineFlippedPlane(const EnginePlane& plane);

// idWinding::GetPlane of the points, or null for fewer than three
std::optional<EnginePlane> engineWindingPlane(const std::vector<EngineVec3>& points);

// what idWinding::Split returns: SIDE_FRONT, SIDE_BACK, SIDE_ON or SIDE_CROSS
// (Plane.h:47-50)
enum class EngineSplitSide
{
  Front = 0,
  Back = 1,
  On = 2,
  Cross = 3,
};

struct EngineSplit
{
  EngineSplitSide side = EngineSplitSide::On;
  // the pieces Split made; a side with nothing is empty
  std::vector<EngineVec3> front;
  std::vector<EngineVec3> back;
};

/**
 * idWinding::Split(plane, epsilon) of a convex winding (ON_EPSILON is 0.1): a winding on
 * the plane goes whole to the front, with Front, when its own normal agrees with the
 * plane's, and to the back, with Back, otherwise (Winding.cpp:137-150). Null for more
 * than MaxWindingPoints points or fewer than three.
 */
std::optional<EngineSplit> engineSplitWinding(
  const std::vector<EngineVec3>& points, const EnginePlane& plane, float epsilon);

// idWinding::TriangleArea (Winding.cpp), dmap's measure of a triangle
float engineTriangleArea(const EngineVec3& a, const EngineVec3& b, const EngineVec3& c);

// ----- texture coordinates (pure) -----

// a brush side's Valve 220 texture values as the engine reads them from the map
struct EngineValveTexture
{
  EngineVec3 uAxis = {0, 0, 0};
  float uOffset = 0;
  EngineVec3 vAxis = {0, 0, 0};
  float vOffset = 0;
  float uScale = 1;
  float vScale = 1;
};

/**
 * dmap's texture coordinate of a side's vertex: idMapBrushSide::GetTextureVectors
 * (MapFile.cpp:104-117, axis·(1/scale)) and TriListForSide (dmap/usurface.cpp:233-242,
 * xyz·vector + offset, then divided by the editor image's size), in the engine's float
 * arithmetic. xyz is relative to the entity's origin key. Not yet through SetTexCoord.
 */
EngineVec2 engineValveTexCoord(
  const EngineVec3& xyz,
  const EngineValveTexture& texture,
  float textureWidth,
  float textureHeight);

/**
 * dmap's WriteFloat (dmap/output.cpp:95-105) through FS_WriteFloatString
 * (framework/File.cpp:41-86): within 0.001 of an integer, that integer ("%i"); otherwise
 * "%1.10f" without trailing zeros. Without the trailing space.
 */
std::string engineProcFloatText(float value);

// ----- patches (pure) -----

struct EnginePatch
{
  // the engine's width (TrenchBroom's row count) and height (its column count) (SC48)
  int width = 0;
  int height = 0;
  // width × height control points stored [i·width + j], i the column, j the row, with
  // xyz relative to the entity's origin and st as the map parse rounds them
  // (MapFile.cpp:330-352: SetTexCoord)
  std::vector<EngineDrawVert> controlPoints;
  // patchDef3
  bool explicitSubdivisions = false;
  int horizontalSubdivisions = 0;
  int verticalSubdivisions = 0;
};

struct EngineSurface
{
  std::vector<EngineDrawVert> verts;
  // three per triangle
  std::vector<int> indexes;
};

/**
 * dmap's ParsePatch (dmap/map.cpp:443-469): idSurface_Patch::Subdivide(4, 4, -1, true),
 * or SubdivideExplicit(h, v, true) for patchDef3, then ParseSurface's triangle order
 * (i + 1, i + 2, i) (map.cpp:416-418). Null, with why in error, when the patch is outside
 * the bounds (K19): sides of 1 to MaxPatchSide, subdivisions of 1 to
 * MaxPatchSubdivisions, at most MaxPatchVertices vertices once subdivided.
 */
std::optional<EngineSurface> engineSubdividePatch(
  const EnginePatch& patch, std::string* error = nullptr);

// ----- triangle surfaces (pure) -----

// the tangent derivation of R_CleanupTriangles (tr_trisurf.cpp:1810-1847, C25)
enum class EngineTangentPath
{
  // createNormals false, no MikkTSpace: R_DeriveTangentsWithoutNormals
  WithoutNormals,
  // unsmoothedTangents: R_BuildDominantTris, then R_DeriveTangents, which derives the
  // normals too
  Unsmoothed,
};

struct EngineTriangles
{
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
  // the duplicated mirrored vertices at the end of verts (R_DuplicateMirroredVertexes)
  int numMirroredVerts = 0;
  std::vector<int> mirroredVerts;
};

/**
 * R_CleanupTriangles' steps on a surface without generated normals, in its order: the
 * index range check (as pre-validation), R_CreateSilIndexes, R_RemoveDegenerateTriangles,
 * R_DuplicateMirroredVertexes, then the tangents of path (R_CreateDupVerts changes
 * nothing drawn and is left out, D57). Null when an index is out of range, the index
 * count is no multiple of three, or there are more than MaxSurfaceVertices vertices.
 */
std::optional<EngineTriangles> engineCleanupTriangles(
  std::vector<EngineDrawVert> verts,
  std::vector<std::uint16_t> indexes,
  EngineTangentPath path);

/**
 * FinishSurfaces' back side (Model.cpp:1252-1276): R_CopyStaticTriSurf, then
 * R_ReverseTriangles (normals negated, the first two indexes of each triangle swapped).
 */
EngineTriangles engineReverseTriangles(
  const std::vector<EngineDrawVert>& verts, const std::vector<std::uint16_t>& indexes);

/**
 * dmap's TriVertsFromOriginal (dmap/tritools.cpp:262-300): the st and normal of a
 * fragment's corner from the original triangle's, by barycentric area (TriangleArea).
 */
EngineDrawVert engineVertFromOriginal(
  const EngineVec3& xyz, const std::array<EngineDrawVert, 3>& original);

} // namespace tb::lit
