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

// The facade over the engine's light and view math (LP1b.3, C5, C24, D58): the copied
// idRenderMatrix (idlib/geometry/RenderMatrix.cpp, verbatim) and the light and projection
// code the shim transcribes on the copied idLib types, so that its float arithmetic is
// the engine's own. It uses standard types only and compiles as C++17 and C++20.
//
// Threading (C24): every function here may run on any thread, the session's CPU pool
// included. The copied idRenderMatrix reads only its arguments and constant statics
// (mirror.json M-LIGHT-STATIC-*), calls no common function and allocates nothing; its
// asserts report to the permanent sink of D63 in Debug builds. Nothing here touches the
// registry.

#include "lit/EngineGeometry.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

// an idRenderMatrix: row-major, m[row * 4 + column], transforms applied by
// left-multiplication (RenderMatrix.h:48-56)
using EngineMat4 = std::array<float, 16>;

// an idBounds: the minimum and the maximum
using EngineBounds = std::array<EngineVec3, 2>;

// frustumCorners_t (RenderMatrix.h:34-39); 16-byte aligned, as the copy loads it
struct alignas(16) EngineFrustumCorners
{
  std::array<float, 8> x = {};
  std::array<float, 8> y = {};
  std::array<float, 8> z = {};
};

// frustumCull_t (RenderMatrix.h:41-46)
enum class EngineFrustumCull
{
  Front = 1,
  Back = 2,
  Cross = 3,
};

// ----- idRenderMatrix (the copy, pure) -----

EngineMat4 engineIdentityMatrix();

// bounds_zeroOneCube (Bounds.cpp): (0 0 0) to (1 1 1)
EngineBounds engineZeroOneCube();

// idRenderMatrix::CreateFromOriginAxis: local space to the space of origin and axis
EngineMat4 engineMatrixFromOriginAxis(const EngineVec3& origin, const EngineMat3& axis);

// idRenderMatrix::Multiply( a, b, out ): out = a · b (the SSE path, USE_INTRINSICS_SSE)
EngineMat4 engineMatrixMultiply(const EngineMat4& a, const EngineMat4& b);

/**
 * idRenderMatrix::Inverse (the SSE path: the cofactors and _mm_rcp32_ps of the
 * determinant, so the result depends on the CPU's reciprocal estimate in its last bits).
 * Null where the copy returns false (|det| below 1e-16), leaving its output unwritten.
 */
std::optional<EngineMat4> engineMatrixInverse(const EngineMat4& matrix);

// idRenderMatrix::ProjectedBounds( projected, mvp, bounds, windowSpace )
EngineBounds engineProjectedBounds(
  const EngineMat4& mvp, const EngineBounds& bounds, bool windowSpace);

// idRenderMatrix::GetFrustumCorners( corners, frustumTransform, frustumBounds )
EngineFrustumCorners engineFrustumCorners(
  const EngineMat4& frustumTransform, const EngineBounds& frustumBounds);

/**
 * idRenderMatrix::CullFrustumCornersToPlane (the SSE path): a corner whose distance has
 * its sign bit clear (d >= +0) is in front, one with it set behind; Front when no corner
 * is behind, Back when every corner is, Cross otherwise.
 */
EngineFrustumCull engineCullFrustumCorners(
  const EngineFrustumCorners& corners, const EnginePlane& plane);

// ----- idBounds and the area bounds (LP4.0, C31; the copy and a transcription, pure)
// -----

// idBounds::Clear (Bounds.h:226-230): inside out, ( +INFINITUM, -INFINITUM )
EngineBounds engineClearedBounds();

// idBounds::Zero (Bounds.h:232-236): a single point at the origin, not cleared
EngineBounds engineZeroBounds();

// idBounds::IsCleared (Bounds.h:252-255): the minimum's x above the maximum's
bool engineBoundsIsCleared(const EngineBounds& bounds);

// idBounds::AddBounds (Bounds.h:293-327): bounds grown to hold other, per component
EngineBounds engineAddBounds(const EngineBounds& bounds, const EngineBounds& other);

// bounds_unitCube (Bounds.cpp:34): (-1 -1 -1) to (1 1 1)
EngineBounds engineUnitCube();

// idBounds::GetCenter (Bounds.h:238-241): ( b[1] + b[0] ) · 0.5 per component
EngineVec3 engineBoundsCenter(const EngineBounds& bounds);

// LP4.2a (C37): idVec3::Length (Vector.h:777-780), idMath::Sqrt( x·x + y·y + z·z ), which
// is x · InvSqrt( x ) = x · sqrtf( 1 / x ) above FLT_SMALLEST_NON_DENORMAL
// (Math.h:527-552), not sqrtf( x ): its last bit may differ from a plain square root
float engineVec3Length(const EngineVec3& v);

// LP5.0 (C49): idMath::Sqrt( x ) itself (Math.h:549-552), x · InvSqrt( x ) through the
// copy, for R_FogImage's distances (src/IntrinsicImages.cpp, Image_intrinsic.cpp:465-466)
float engineMathSqrt(float x);

/**
 * idRenderMatrix::OffsetScaleForBounds( src, bounds, out ) (RenderMatrix.cpp:793-869, its
 * SSE path): the matrix that takes the unit cube onto bounds in src's space, as
 * R_DeriveEntityData makes an entity's inverseBaseModelProject (RenderWorld_defs.cpp:56).
 */
EngineMat4 engineOffsetScaleForBounds(const EngineMat4& src, const EngineBounds& bounds);

// idRenderMatrix::TransformPoint( in, out ) (RenderMatrix.h:328-336): the rows' first
// three columns times the point plus the fourth
EngineVec3 engineTransformPoint(const EngineMat4& matrix, const EngineVec3& point);

/**
 * idRenderModelStatic::FinishSurfaces' growth of the bounds of a surface whose material
 * has a deform (Model.cpp:1346-1360, transcribed on the copied idVec3): the cube
 * mid ± ( ( bounds[0] - mid ).Length() + 20 ), mid = ( bounds[1] + bounds[0] ) · 0.5,
 * Length through idMath::Sqrt (x · InvSqrt( x )).
 */
EngineBounds engineDeformSurfaceBounds(const EngineBounds& bounds);

// ----- what ParseSpawnArgsToRenderLight and idLight::Spawn call (the copy, pure) -----

// idVec3::FixDegenerateNormal (Vector.h): an axial vector becomes the unit axis its
// non-zero component points along; zero becomes (0 0 -1)
EngineVec3 engineFixDegenerateNormal(const EngineVec3& v);

// idMath::AngleNormalize360 (Math.h)
float engineAngleNormalize360(float angle);

// renderer/Material.h's predef_lightstyles, the Quake light styles idLight::Spawn falls
// back to (ML_NUM_STYLES of them)
std::vector<std::string> engineDefaultLightStyles();

// ----- R_DeriveLightData's arithmetic (transcribed, pure) -----

// renderLight_t's geometry, as ParseSpawnArgsToRenderLight leaves it
struct EngineLightGeometry
{
  EngineVec3 origin = {0, 0, 0};
  EngineMat3 axis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  EngineVec3 target = {0, 0, 0};
  EngineVec3 right = {0, 0, 0};
  EngineVec3 up = {0, 0, 0};
  EngineVec3 start = {0, 0, 0};
  EngineVec3 end = {0, 0, 0};
  EngineVec3 lightRadius = {0, 0, 0};
  EngineVec3 lightCenter = {0, 0, 0};
  bool pointLight = false;
  bool parallel = false;
};

// what R_DeriveLightData derives from the geometry alone (its material half is
// LightData's)
struct EngineLightProjection
{
  // the projection before the swap (RenderMatrix row-major) and its zScale (1 for point
  // and parallel lights, 1 / ( zNear + zFar ) for spot lights)
  EngineMat4 localProject = {};
  float zScale = 1.0f;
  // lightProject[4]: localProject's rows with Z and W swapped (C5: [2] = localProject[3],
  // [3] = localProject[2] · zScale), each taken to global space by R_LocalPlaneToGlobal
  std::array<EnginePlane, 4> lightProject = {};
  // parallel: origin + normalize( lightCenter, or +Z ) · 100000; else origin + axis ·
  // lightCenter
  EngineVec3 globalLightOrigin = {0, 0, 0};
  // global space -> light projective space, and back
  EngineMat4 baseLightProject = {};
  EngineMat4 inverseBaseLightProject = {};
  // ProjectedBounds( inverseBaseLightProject, bounds_zeroOneCube, windowSpace false )
  EngineBounds globalLightBounds = {};
  // false where the engine warns "lightMatrix invert failed" or "baseLightProject invert
  // failed" and goes on with an unwritten matrix; here that matrix is zero
  bool lightMatrixInverted = true;
  bool baseLightProjectInverted = true;
};

/**
 * RenderWorld_defs.cpp:330-470 R_DeriveLightData from "compute the light projection
 * matrix" on (SC63, SC64, C5), with R_ComputePointLightProjectionMatrix (:214-230),
 * R_ComputeSpotLightProjectionMatrix (:242-296), R_ComputeParallelLightProjectionMatrix
 * (:305-321) and GLMatrix.cpp's R_AxisToModelMatrix and R_LocalPlaneToGlobal, transcribed
 * on the copied idLib types, and the copied idRenderMatrix's CreateFromOriginAxis,
 * Inverse, Multiply and ProjectedBounds. Pure; any thread (C24).
 */
EngineLightProjection engineDeriveLightProjection(const EngineLightGeometry& light);

// ----- the view (transcribed, pure) -----

// a matrix as the engine's float[16] view matrices hold it: OpenGL's column-major layout,
// element (row r, column c) at [c * 4 + r]
using EngineGlMatrix = std::array<float, 16>;

struct EngineFov
{
  float x = 0.0f;
  float y = 0.0f;
};

/**
 * idGameLocal::CalcFov (d3xp/Game_local.cpp:3171-3194, SC68): fov_y from baseFov
 * through a 16:9 ideal ratio, then fov_x from the window's true aspect (width ·
 * pixelAspect : height); a square window takes baseFov for both. width and height are
 * renderSystem->GetWidth() and GetHeight().
 */
EngineFov engineCalcFov(float baseFov, int width, int height, float pixelAspect);

/**
 * R_SetupProjectionMatrix (renderer/GLMatrix.cpp:418-544, SC67, A16) without TAA jitter
 * (jitter 0, so xoffset and yoffset are -2 · 0 / size: negative zero), flipProjection or
 * an oblique projection: ymax = zNear · tan( fov_y · PI / 360 ), xmax likewise, the far
 * plane at infinity with [2][2] = -0.999, [3][2] = -zNear (clip z 0..1) and [2][3] = -1.
 * viewWidth and viewHeight are the viewport's (x2 - x1 + 1, y2 - y1 + 1), at least 1.
 */
EngineGlMatrix engineProjectionMatrix(
  float fovX, float fovY, float zNear, int viewWidth, int viewHeight);

/**
 * R_SetupViewMatrix (renderer/GLMatrix.cpp:353-401): the world's modelViewMatrix, the
 * viewer matrix of vieworg and viewaxis (rows forward, left, up, the engine's Z-up frame)
 * times s_flipMatrix through R_MatrixMultiply (its SSE path), from looking down X to
 * OpenGL's looking down -Z.
 */
EngineGlMatrix engineViewMatrix(const EngineVec3& origin, const EngineMat3& axis);

// a view axis from a forward and an up direction as idMat3 rows: forward, up.Cross(
// forward ) (left), up
EngineMat3 engineViewAxis(const EngineVec3& forward, const EngineVec3& up);

// ----- the frame's matrices (LP2.2a; the copy, and GLMatrix.cpp transcribed, pure) -----

// idRenderMatrix::Transpose( src, out ): the engine's float[16] view matrices as render
// matrices (tr_frontend_main.cpp:904-907, tr_frontend_addmodels.cpp:506-507)
EngineMat4 engineMatrixTranspose(const EngineMat4& matrix);

// idRenderMatrix::CullBoundsToMVP( mvp, bounds, zeroToOne ): true when the bounds are
// outside one clip plane (the copy, its SSE path)
bool engineCullBoundsToMvp(
  const EngineMat4& mvp, const EngineBounds& bounds, bool zeroToOne);

// idRenderMatrix::ProjectedFullyClippedBounds( projected, mvp, bounds, true ): the bounds
// clipped to the view frustum and projected to window space, x and y 0..1 with y up
EngineBounds engineProjectedFullyClippedBounds(
  const EngineMat4& mvp, const EngineBounds& bounds);

/**
 * LP5.2a (C46, D115): idRenderMatrix::DepthBoundsForBounds( min, max, mvp, bounds,
 * windowSpace true ) (RenderMatrix.cpp:3712-3830, the copy, its SSE path): the clip depth
 * range of the bounds' eight corners, a corner behind the eye (w below the smallest
 * normal float) at minus infinity, then moved to window space and clamped to 0..1;
 * (min, max). Pure; any thread (M-STAGE-PURE-01).
 */
std::array<float, 2> engineDepthBoundsForBounds(
  const EngineMat4& mvp, const EngineBounds& bounds);

/**
 * LP5.2a (C46, D115): idMath::Ftoui16 (Math.h:1407-1427, the copy): C_FLOAT_TO_INT's
 * truncation, then 0 below 0 and 65535 above it. Pure; any thread.
 */
std::uint16_t engineFtoui16(float value);

/**
 * idRenderMatrix::GetFrustumPlanes( planes, frustum, zeroToOne, normalize )
 * (RenderMatrix.cpp:4436-4515, the copy): left, right, bottom, top, near, far, each
 * pointing inside; without zeroToOne the near plane is row 3 + row 2 (isZeroOneZ is
 * false), so for a clip z of 0..1 it lies at zNear / 1.999 rather than at zNear; with
 * normalize every plane times idMath::InvSqrt of its normal's squared length.
 */
std::array<EnginePlane, 6> engineFrustumPlanes(
  const EngineMat4& frustum, bool zeroToOne, bool normalize);

/**
 * R_GlobalToNormalizedDeviceCoordinates (GLMatrix.cpp:215-243, transcribed): the global
 * point through the world's modelViewMatrix, then the projectionMatrix, both float[16]s
 * as the engine's view holds them, divided by the clip w (1 / w, then three products);
 * clip z 0..1.
 */
EngineVec3 engineGlobalToNormalizedDeviceCoordinates(
  const EngineGlMatrix& modelViewMatrix,
  const EngineGlMatrix& projectionMatrix,
  const EngineVec3& global);

// R_AxisToModelMatrix (GLMatrix.cpp:49-70): an entity's float[16] model matrix
EngineGlMatrix engineModelMatrix(const EngineVec3& origin, const EngineMat3& axis);

// R_MatrixMultiply (GLMatrix.cpp:77-150, its SSE path): out = a · b on float[16]s
EngineGlMatrix engineGlMatrixMultiply(const EngineGlMatrix& a, const EngineGlMatrix& b);

// R_GlobalPointToLocal (GLMatrix.cpp:267-278), no skew or scale assumed, as there
EngineVec3 engineGlobalPointToLocal(
  const EngineGlMatrix& modelMatrix, const EngineVec3& point);

// R_GlobalPlaneToLocal (GLMatrix.cpp:315-321), no skew or scale assumed, as there
EnginePlane engineGlobalPlaneToLocal(
  const EngineGlMatrix& modelMatrix, const EnginePlane& plane);

/**
 * RB_BakeTextureMatrixIntoTexgen (RenderBackend.cpp:223-259): the light projection's S
 * and T planes multiplied by a light stage's texture matrix (RB_GetShaderTextureMatrix's
 * layout) through R_MatrixMultiply; Q (plane 2) is read, planes 2 and 3 are left as
 * they are.
 */
void engineBakeTextureMatrixIntoTexgen(
  std::array<EnginePlane, 4>& lightProject, const EngineGlMatrix& textureMatrix);

// ----- the static interactions' light triangles (DX.2; the copy, pure) -----

/**
 * R_CreateInteractionLightTris with R_CalcInteractionCullBits (Interaction.cpp, the
 * copied ranges, r_lightAllBackFaces 1 so no facing cull): of a surface of entity-local
 * positions with its local bounds and its 16-bit triangle indexes, the indexes of the
 * triangles that are not culled to the light volume of baseLightProject in the space of
 * modelMatrix, in their order. A triangle is culled when its three vertices are within
 * LIGHT_CLIP_EPSILON (0.1) of one of the volume's six planes, or behind it:
 * GetFrustumPlanes( baseLightProject, true, true ) taken into the surface's space by
 * R_GlobalPlaneToLocal (not normalised again for a scaled entity). Empty: the engine
 * makes no interaction surface. Pure; any thread, the CPU pool's included (mirror.json
 * M-LIGHT-POOL-02).
 */
std::vector<std::uint16_t> engineInteractionLightTris(
  const EngineMat4& baseLightProject,
  const EngineGlMatrix& modelMatrix,
  const EngineBounds& localBounds,
  const std::vector<EngineVec3>& positions,
  const std::vector<std::uint16_t>& indexes);

} // namespace tb::lit
