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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_defs.cpp,
// renderer/GLMatrix.cpp, renderer/RenderBackend.cpp, renderer/Model.cpp and
// d3xp/Game_local.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The light and view facade (lit/EngineLight.h) over the copied idRenderMatrix.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineLight.h"
// clang-format on

namespace tb::lit
{
namespace
{

static_assert(sizeof(idRenderMatrix) == sizeof(EngineMat4));
static_assert(sizeof(frustumCorners_t) == sizeof(EngineFrustumCorners));

idVec3 toId(const EngineVec3& v)
{
  return idVec3{v[0], v[1], v[2]};
}

EngineVec3 fromId(const idVec3& v)
{
  return {v.x, v.y, v.z};
}

idMat3 toId(const EngineMat3& m)
{
  return idMat3{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
}

idRenderMatrix toId(const EngineMat4& m)
{
  auto result = idRenderMatrix{};
  std::memcpy(&result, m.data(), sizeof(result));
  return result;
}

EngineMat4 fromId(const idRenderMatrix& m)
{
  auto result = EngineMat4{};
  std::memcpy(result.data(), &m, sizeof(result));
  return result;
}

idBounds toId(const EngineBounds& b)
{
  return idBounds{toId(b[0]), toId(b[1])};
}

EngineBounds fromId(const idBounds& b)
{
  return {fromId(b[0]), fromId(b[1])};
}

EnginePlane fromId(const idPlane& plane)
{
  return {plane[0], plane[1], plane[2], plane[3]};
}

// the renderLight_t fields R_DeriveLightData reads, on the copied types ("light->parms")
struct LightParms
{
  idVec3 origin;
  idMat3 axis;
  idVec3 target;
  idVec3 right;
  idVec3 up;
  idVec3 start;
  idVec3 end;
  idVec3 lightRadius;
  idVec3 lightCenter;
  bool pointLight = false;
  bool parallel = false;
};

/*
========================
R_ComputePointLightProjectionMatrix (RenderWorld_defs.cpp:214-230)

Computes the light projection matrix for a point light.
========================
*/
float R_ComputePointLightProjectionMatrix(
  const LightParms& parms, idRenderMatrix& localProject)
{
  // A point light uses a box projection.
  // This projects into the 0.0 - 1.0 texture range instead of -1.0 to 1.0 clip space
  // range.
  localProject.Zero();
  localProject[0][0] = 0.5f / parms.lightRadius[0];
  localProject[1][1] = 0.5f / parms.lightRadius[1];
  localProject[2][2] = 0.5f / parms.lightRadius[2];
  localProject[0][3] = 0.5f;
  localProject[1][3] = 0.5f;
  localProject[2][3] = 0.5f;
  localProject[3][3] = 1.0f; // identity perspective

  return 1.0f;
}

const float SPOT_LIGHT_MIN_Z_NEAR = 8.0f;
const float SPOT_LIGHT_MIN_Z_FAR = 16.0f;

/*
========================
R_ComputeSpotLightProjectionMatrix (RenderWorld_defs.cpp:242-296)

Computes the light projection matrix for a spot light.
========================
*/
float R_ComputeSpotLightProjectionMatrix(
  const LightParms& parms, idRenderMatrix& localProject)
{
  const float targetDistSqr = parms.target.LengthSqr();
  const float invTargetDist = idMath::InvSqrt(targetDistSqr);
  const float targetDist = invTargetDist * targetDistSqr;

  const idVec3 normalizedTarget = parms.target * invTargetDist;
  const idVec3 normalizedRight =
    parms.right * (0.5f * targetDist / parms.right.LengthSqr());
  const idVec3 normalizedUp = parms.up * (-0.5f * targetDist / parms.up.LengthSqr());

  localProject[0][0] = normalizedRight[0];
  localProject[0][1] = normalizedRight[1];
  localProject[0][2] = normalizedRight[2];
  localProject[0][3] = 0.0f;

  localProject[1][0] = normalizedUp[0];
  localProject[1][1] = normalizedUp[1];
  localProject[1][2] = normalizedUp[2];
  localProject[1][3] = 0.0f;

  localProject[3][0] = normalizedTarget[0];
  localProject[3][1] = normalizedTarget[1];
  localProject[3][2] = normalizedTarget[2];
  localProject[3][3] = 0.0f;

  // Set the falloff vector.
  // This is similar to the Z calculation for depth buffering, which means that the
  // mapped texture is going to be perspective distorted heavily towards the zero end.
  const float zNear = Max(parms.start * normalizedTarget, SPOT_LIGHT_MIN_Z_NEAR);
  const float zFar = Max(parms.end * normalizedTarget, SPOT_LIGHT_MIN_Z_FAR);
  const float zScale = (zNear + zFar) / zFar;

  localProject[2][0] = normalizedTarget[0] * zScale;
  localProject[2][1] = normalizedTarget[1] * zScale;
  localProject[2][2] = normalizedTarget[2] * zScale;
  localProject[2][3] = -zNear * zScale;

  // now offset to the 0.0 - 1.0 texture range instead of -1.0 to 1.0 clip space range
  idVec4 projectedTarget;
  localProject.TransformPoint(parms.target, projectedTarget);

  const float ofs0 = 0.5f - projectedTarget[0] / projectedTarget[3];
  localProject[0][0] += ofs0 * localProject[3][0];
  localProject[0][1] += ofs0 * localProject[3][1];
  localProject[0][2] += ofs0 * localProject[3][2];
  localProject[0][3] += ofs0 * localProject[3][3];

  const float ofs1 = 0.5f - projectedTarget[1] / projectedTarget[3];
  localProject[1][0] += ofs1 * localProject[3][0];
  localProject[1][1] += ofs1 * localProject[3][1];
  localProject[1][2] += ofs1 * localProject[3][2];
  localProject[1][3] += ofs1 * localProject[3][3];

  return 1.0f / (zNear + zFar);
}

/*
========================
R_ComputeParallelLightProjectionMatrix (RenderWorld_defs.cpp:305-321)

Computes the light projection matrix for a parallel light.
========================
*/
float R_ComputeParallelLightProjectionMatrix(
  const LightParms& parms, idRenderMatrix& localProject)
{
  // A parallel light uses a box projection.
  // This projects into the 0.0 - 1.0 texture range instead of -1.0 to 1.0 clip space
  // range.
  localProject.Zero();
  localProject[0][0] = 0.5f / parms.lightRadius[0];
  localProject[1][1] = 0.5f / parms.lightRadius[1];
  localProject[2][2] = 0.5f / parms.lightRadius[2];
  localProject[0][3] = 0.5f;
  localProject[1][3] = 0.5f;
  localProject[2][3] = 0.5f;
  localProject[3][3] = 1.0f; // identity perspective

  return 1.0f;
}

/*
======================
R_AxisToModelMatrix (GLMatrix.cpp:49-70)
======================
*/
void R_AxisToModelMatrix(const idMat3& axis, const idVec3& origin, float modelMatrix[16])
{
  modelMatrix[0 * 4 + 0] = axis[0][0];
  modelMatrix[1 * 4 + 0] = axis[1][0];
  modelMatrix[2 * 4 + 0] = axis[2][0];
  modelMatrix[3 * 4 + 0] = origin[0];

  modelMatrix[0 * 4 + 1] = axis[0][1];
  modelMatrix[1 * 4 + 1] = axis[1][1];
  modelMatrix[2 * 4 + 1] = axis[2][1];
  modelMatrix[3 * 4 + 1] = origin[1];

  modelMatrix[0 * 4 + 2] = axis[0][2];
  modelMatrix[1 * 4 + 2] = axis[1][2];
  modelMatrix[2 * 4 + 2] = axis[2][2];
  modelMatrix[3 * 4 + 2] = origin[2];

  modelMatrix[0 * 4 + 3] = 0.0f;
  modelMatrix[1 * 4 + 3] = 0.0f;
  modelMatrix[2 * 4 + 3] = 0.0f;
  modelMatrix[3 * 4 + 3] = 1.0f;
}

/*
======================
R_LocalPlaneToGlobal (GLMatrix.cpp:330-336)

NOTE: assumes no skewing or scaling transforms
======================
*/
void R_LocalPlaneToGlobal(const float modelMatrix[16], const idPlane& in, idPlane& out)
{
  out[0] = in[0] * modelMatrix[0 * 4 + 0] + in[1] * modelMatrix[1 * 4 + 0]
           + in[2] * modelMatrix[2 * 4 + 0];
  out[1] = in[0] * modelMatrix[0 * 4 + 1] + in[1] * modelMatrix[1 * 4 + 1]
           + in[2] * modelMatrix[2 * 4 + 1];
  out[2] = in[0] * modelMatrix[0 * 4 + 2] + in[1] * modelMatrix[1 * 4 + 2]
           + in[2] * modelMatrix[2 * 4 + 2];
  out[3] = in[3] - modelMatrix[3 * 4 + 0] * out[0] - modelMatrix[3 * 4 + 1] * out[1]
           - modelMatrix[3 * 4 + 2] * out[2];
}

/*
==========================
R_MatrixMultiply (GLMatrix.cpp:77-150)
==========================
*/
void R_MatrixMultiply(const float a[16], const float b[16], float out[16])
{
#if defined(USE_INTRINSICS_SSE)
  __m128 a0 = _mm_loadu_ps(a + 0 * 4);
  __m128 a1 = _mm_loadu_ps(a + 1 * 4);
  __m128 a2 = _mm_loadu_ps(a + 2 * 4);
  __m128 a3 = _mm_loadu_ps(a + 3 * 4);

  __m128 b0 = _mm_loadu_ps(b + 0 * 4);
  __m128 b1 = _mm_loadu_ps(b + 1 * 4);
  __m128 b2 = _mm_loadu_ps(b + 2 * 4);
  __m128 b3 = _mm_loadu_ps(b + 3 * 4);

  __m128 t0 = _mm_mul_ps(_mm_splat_ps(a0, 0), b0);
  __m128 t1 = _mm_mul_ps(_mm_splat_ps(a1, 0), b0);
  __m128 t2 = _mm_mul_ps(_mm_splat_ps(a2, 0), b0);
  __m128 t3 = _mm_mul_ps(_mm_splat_ps(a3, 0), b0);

  t0 = _mm_add_ps(t0, _mm_mul_ps(_mm_splat_ps(a0, 1), b1));
  t1 = _mm_add_ps(t1, _mm_mul_ps(_mm_splat_ps(a1, 1), b1));
  t2 = _mm_add_ps(t2, _mm_mul_ps(_mm_splat_ps(a2, 1), b1));
  t3 = _mm_add_ps(t3, _mm_mul_ps(_mm_splat_ps(a3, 1), b1));

  t0 = _mm_add_ps(t0, _mm_mul_ps(_mm_splat_ps(a0, 2), b2));
  t1 = _mm_add_ps(t1, _mm_mul_ps(_mm_splat_ps(a1, 2), b2));
  t2 = _mm_add_ps(t2, _mm_mul_ps(_mm_splat_ps(a2, 2), b2));
  t3 = _mm_add_ps(t3, _mm_mul_ps(_mm_splat_ps(a3, 2), b2));

  t0 = _mm_add_ps(t0, _mm_mul_ps(_mm_splat_ps(a0, 3), b3));
  t1 = _mm_add_ps(t1, _mm_mul_ps(_mm_splat_ps(a1, 3), b3));
  t2 = _mm_add_ps(t2, _mm_mul_ps(_mm_splat_ps(a2, 3), b3));
  t3 = _mm_add_ps(t3, _mm_mul_ps(_mm_splat_ps(a3, 3), b3));

  _mm_storeu_ps(out + 0 * 4, t0);
  _mm_storeu_ps(out + 1 * 4, t1);
  _mm_storeu_ps(out + 2 * 4, t2);
  _mm_storeu_ps(out + 3 * 4, t3);
#else
  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 4; j++)
    {
      out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j]
                       + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    }
  }
#endif
}

} // namespace

EngineMat4 engineIdentityMatrix()
{
  return fromId(renderMatrix_identity);
}

EngineBounds engineZeroOneCube()
{
  return fromId(bounds_zeroOneCube);
}

EngineMat4 engineMatrixFromOriginAxis(const EngineVec3& origin, const EngineMat3& axis)
{
  auto out = idRenderMatrix{};
  idRenderMatrix::CreateFromOriginAxis(toId(origin), toId(axis), out);
  return fromId(out);
}

EngineMat4 engineMatrixMultiply(const EngineMat4& a, const EngineMat4& b)
{
  auto out = idRenderMatrix{};
  idRenderMatrix::Multiply(toId(a), toId(b), out);
  return fromId(out);
}

std::optional<EngineMat4> engineMatrixInverse(const EngineMat4& matrix)
{
  auto out = idRenderMatrix{};
  out.Zero();
  if (!idRenderMatrix::Inverse(toId(matrix), out))
  {
    return std::nullopt;
  }
  return fromId(out);
}

EngineBounds engineProjectedBounds(
  const EngineMat4& mvp, const EngineBounds& bounds, const bool windowSpace)
{
  auto projected = idBounds{};
  idRenderMatrix::ProjectedBounds(projected, toId(mvp), toId(bounds), windowSpace);
  return fromId(projected);
}

EngineFrustumCorners engineFrustumCorners(
  const EngineMat4& frustumTransform, const EngineBounds& frustumBounds)
{
  // RenderWorld.cpp PushFrustumIntoTree: ALIGNTYPE16 frustumCorners_t corners
  ALIGNTYPE16 frustumCorners_t corners;
  idRenderMatrix::GetFrustumCorners(corners, toId(frustumTransform), toId(frustumBounds));
  auto result = EngineFrustumCorners{};
  std::memcpy(result.x.data(), corners.x, sizeof(corners.x));
  std::memcpy(result.y.data(), corners.y, sizeof(corners.y));
  std::memcpy(result.z.data(), corners.z, sizeof(corners.z));
  return result;
}

EngineFrustumCull engineCullFrustumCorners(
  const EngineFrustumCorners& corners, const EnginePlane& plane)
{
  ALIGNTYPE16 frustumCorners_t copy;
  std::memcpy(copy.x, corners.x.data(), sizeof(copy.x));
  std::memcpy(copy.y, corners.y.data(), sizeof(copy.y));
  std::memcpy(copy.z, corners.z.data(), sizeof(copy.z));
  const auto p = idPlane{plane.a, plane.b, plane.c, plane.d};
  switch (idRenderMatrix::CullFrustumCornersToPlane(copy, p))
  {
  case FRUSTUM_CULL_FRONT:
    return EngineFrustumCull::Front;
  case FRUSTUM_CULL_BACK:
    return EngineFrustumCull::Back;
  case FRUSTUM_CULL_CROSS:
  default:
    return EngineFrustumCull::Cross;
  }
}

EngineBounds engineClearedBounds()
{
  auto bounds = idBounds{};
  bounds.Clear();
  return fromId(bounds);
}

EngineBounds engineZeroBounds()
{
  auto bounds = idBounds{};
  bounds.Zero();
  return fromId(bounds);
}

bool engineBoundsIsCleared(const EngineBounds& bounds)
{
  return toId(bounds).IsCleared();
}

EngineBounds engineAddBounds(const EngineBounds& bounds, const EngineBounds& other)
{
  auto result = toId(bounds);
  static_cast<void>(result.AddBounds(toId(other)));
  return fromId(result);
}

EngineBounds engineUnitCube()
{
  return fromId(bounds_unitCube);
}

EngineVec3 engineBoundsCenter(const EngineBounds& bounds)
{
  return fromId(toId(bounds).GetCenter());
}

float engineVec3Length(const EngineVec3& v)
{
  return toId(v).Length();
}

float engineMathSqrt(const float x)
{
  return idMath::Sqrt(x);
}

EngineMat4 engineOffsetScaleForBounds(const EngineMat4& src, const EngineBounds& bounds)
{
  auto out = idRenderMatrix{};
  idRenderMatrix::OffsetScaleForBounds(toId(src), toId(bounds), out);
  return fromId(out);
}

EngineVec3 engineTransformPoint(const EngineMat4& matrix, const EngineVec3& point)
{
  auto out = idVec3{};
  toId(matrix).TransformPoint(toId(point), out);
  return fromId(out);
}

EngineBounds engineDeformSurfaceBounds(const EngineBounds& bounds)
{
  // Model.cpp:1346-1360, FinishSurfaces' bounds of a surface whose shader has a deform
  auto tri = toId(bounds);
  idVec3 mid = (tri[1] + tri[0]) * 0.5f;
  float radius = (tri[0] - mid).Length();
  radius += 20.0f;

  tri[0][0] = mid[0] - radius;
  tri[0][1] = mid[1] - radius;
  tri[0][2] = mid[2] - radius;

  tri[1][0] = mid[0] + radius;
  tri[1][1] = mid[1] + radius;
  tri[1][2] = mid[2] + radius;
  return fromId(tri);
}

EngineVec3 engineFixDegenerateNormal(const EngineVec3& v)
{
  auto result = toId(v);
  result.FixDegenerateNormal();
  return fromId(result);
}

float engineAngleNormalize360(const float angle)
{
  return idMath::AngleNormalize360(angle);
}

std::vector<std::string> engineDefaultLightStyles()
{
  static_assert(sizeof(predef_lightstyles) / sizeof(predef_lightstyles[0]) == 12);
  auto styles = std::vector<std::string>{};
  for (const auto* style : predef_lightstyles)
  {
    styles.emplace_back(style);
  }
  return styles;
}

EngineLightProjection engineDeriveLightProjection(const EngineLightGeometry& light)
{
  auto parms = LightParms{};
  parms.origin = toId(light.origin);
  parms.axis = toId(light.axis);
  parms.target = toId(light.target);
  parms.right = toId(light.right);
  parms.up = toId(light.up);
  parms.start = toId(light.start);
  parms.end = toId(light.end);
  parms.lightRadius = toId(light.lightRadius);
  parms.lightCenter = toId(light.lightCenter);
  parms.pointLight = light.pointLight;
  parms.parallel = light.parallel;

  auto result = EngineLightProjection{};

  // RenderWorld_defs.cpp:380-469, from "compute the light projection matrix" on
  idRenderMatrix localProject;
  float zScale = 1.0f;
  if (parms.parallel)
  {
    zScale = R_ComputeParallelLightProjectionMatrix(parms, localProject);
  }
  else if (parms.pointLight)
  {
    zScale = R_ComputePointLightProjectionMatrix(parms, localProject);
  }
  else
  {
    zScale = R_ComputeSpotLightProjectionMatrix(parms, localProject);
  }

  // set the old style light projection where Z and W are flipped and
  // for projected lights lightProject[3] is divided by ( zNear + zFar )
  idPlane lightProject[4];
  lightProject[0][0] = localProject[0][0];
  lightProject[0][1] = localProject[0][1];
  lightProject[0][2] = localProject[0][2];
  lightProject[0][3] = localProject[0][3];

  lightProject[1][0] = localProject[1][0];
  lightProject[1][1] = localProject[1][1];
  lightProject[1][2] = localProject[1][2];
  lightProject[1][3] = localProject[1][3];

  lightProject[2][0] = localProject[3][0];
  lightProject[2][1] = localProject[3][1];
  lightProject[2][2] = localProject[3][2];
  lightProject[2][3] = localProject[3][3];

  lightProject[3][0] = localProject[2][0] * zScale;
  lightProject[3][1] = localProject[2][1] * zScale;
  lightProject[3][2] = localProject[2][2] * zScale;
  lightProject[3][3] = localProject[2][3] * zScale;

  // transform the lightProject
  float lightTransform[16];
  R_AxisToModelMatrix(parms.axis, parms.origin, lightTransform);
  for (int i = 0; i < 4; i++)
  {
    idPlane temp = lightProject[i];
    R_LocalPlaneToGlobal(lightTransform, temp, lightProject[i]);
  }

  // adjust global light origin for off center projections and parallel projections
  // we are just faking parallel by making it a very far off center for now
  idVec3 globalLightOrigin;
  if (parms.parallel)
  {
    idVec3 dir = parms.lightCenter;
    if (dir.Normalize() == 0.0f)
    {
      // make point straight up if not specified
      dir[2] = 1.0f;
    }
    globalLightOrigin = parms.origin + dir * 100000.0f;
  }
  else
  {
    globalLightOrigin = parms.origin + parms.axis * parms.lightCenter;
  }

  // Rotate and translate the light projection by the light matrix.
  // 99% of lights remain axis aligned in world space.
  idRenderMatrix lightMatrix;
  idRenderMatrix::CreateFromOriginAxis(parms.origin, parms.axis, lightMatrix);

  // where an inverse fails the engine warns and goes on with the unwritten matrix; here
  // it is zero, and the flag lets the caller warn (mirror.json M-LIGHT-GUARD-03)
  idRenderMatrix inverseLightMatrix;
  inverseLightMatrix.Zero();
  if (!idRenderMatrix::Inverse(lightMatrix, inverseLightMatrix))
  {
    result.lightMatrixInverted = false;
  }

  // 'baseLightProject' goes from global space -> light local space -> light projective
  // space
  idRenderMatrix baseLightProject;
  idRenderMatrix::Multiply(localProject, inverseLightMatrix, baseLightProject);

  // Invert the light projection so we can deform zero-to-one cubes into
  // the light model and calculate global bounds.
  idRenderMatrix inverseBaseLightProject;
  inverseBaseLightProject.Zero();
  if (!idRenderMatrix::Inverse(baseLightProject, inverseBaseLightProject))
  {
    result.baseLightProjectInverted = false;
  }

  // calculate the global light bounds by inverse projecting the zero to one cube with the
  // 'inverseBaseLightProject'
  idBounds globalLightBounds;
  idRenderMatrix::ProjectedBounds(
    globalLightBounds, inverseBaseLightProject, bounds_zeroOneCube, false);

  result.localProject = fromId(localProject);
  result.zScale = zScale;
  for (auto i = 0; i < 4; ++i)
  {
    result.lightProject[std::size_t(i)] = fromId(lightProject[i]);
  }
  result.globalLightOrigin = fromId(globalLightOrigin);
  result.baseLightProject = fromId(baseLightProject);
  result.inverseBaseLightProject = fromId(inverseBaseLightProject);
  result.globalLightBounds = fromId(globalLightBounds);
  return result;
}

EngineFov engineCalcFov(
  const float base_fov, const int width, const int height, const float pixelAspect)
{
  // d3xp/Game_local.cpp:3171-3194 idGameLocal::CalcFov, renderSystem's GetWidth,
  // GetHeight and GetPixelAspect as arguments
  auto fov = EngineFov{};
  float& fov_x = fov.x;
  float& fov_y = fov.y;
  if (width == height)
  {
    // this is the Rift, so don't mess with our aspect ratio corrections
    fov_x = base_fov;
    fov_y = base_fov;
    return fov;
  }

  // Calculate the fov_y based on an ideal aspect ratio
  const float ideal_ratio_x = 16.0f;
  const float ideal_ratio_y = 9.0f;
  const float tanHalfX = idMath::Tan(DEG2RAD(base_fov * 0.5f));
  fov_y = 2.0f * RAD2DEG(idMath::ATan(ideal_ratio_y * tanHalfX, ideal_ratio_x));

  // Then calculate fov_x based on the true aspect ratio
  const float ratio_x = width * pixelAspect;
  const float ratio_y = height;
  const float tanHalfY = idMath::Tan(DEG2RAD(fov_y * 0.5f));
  fov_x = 2.0f * RAD2DEG(idMath::ATan(ratio_x * tanHalfY, ratio_y));
  return fov;
}

// R_SetupProjectionMatrix calls tan( float ): with the STL's <cmath> among the shim's
// includes the float overload, tanf (mirror.json M-LIGHT-FN-07)
static_assert(std::is_same_v<decltype(tan(1.0f)), float>);

EngineGlMatrix engineProjectionMatrix(
  const float fovX,
  const float fovY,
  const float zNear,
  const int viewWidth,
  const int viewHeight)
{
  // renderer/GLMatrix.cpp:418-544 R_SetupProjectionMatrix: no jitter, the #if 1 branch
  const float jitterx = 0.0f;
  const float jittery = 0.0f;

  // this mimics the logic in the Donut Feature Demo
  const float xoffset = -2.0f * jitterx / (1.0f * viewWidth);
  const float yoffset = -2.0f * jittery / (1.0f * viewHeight);

  auto result = EngineGlMatrix{};
  float* projectionMatrix = result.data();

  float ymax = zNear * tan(fovY * idMath::PI / 360.0f);
  float ymin = -ymax;

  float xmax = zNear * tan(fovX * idMath::PI / 360.0f);
  float xmin = -xmax;

  const float width = xmax - xmin;
  const float height = ymax - ymin;

  projectionMatrix[0 * 4 + 0] = 2.0f * zNear / width;
  projectionMatrix[1 * 4 + 0] = 0.0f;
  projectionMatrix[2 * 4 + 0] = xoffset;
  projectionMatrix[3 * 4 + 0] = 0.0f;

  projectionMatrix[0 * 4 + 1] = 0.0f;
  projectionMatrix[1 * 4 + 1] = 2.0f * zNear / height;
  projectionMatrix[2 * 4 + 1] = yoffset;
  projectionMatrix[3 * 4 + 1] = 0.0f;

  // this is the far-plane-at-infinity formulation, and
  // crunches the Z range slightly so w=0 vertexes do not
  // rasterize right at the wraparound point
  projectionMatrix[0 * 4 + 2] = 0.0f;
  projectionMatrix[1 * 4 + 2] = 0.0f;
  projectionMatrix[2 * 4 + 2] = -0.999f; // adjust value to prevent imprecision issues

  // RB: was -2.0f * zNear
  // the transformation into window space has changed from [-1 .. 1] to [0 .. 1]
  projectionMatrix[3 * 4 + 2] = -1.0f * zNear;

  projectionMatrix[0 * 4 + 3] = 0.0f;
  projectionMatrix[1 * 4 + 3] = 0.0f;
  projectionMatrix[2 * 4 + 3] = -1.0f;
  projectionMatrix[3 * 4 + 3] = 0.0f;

  return result;
}

EngineGlMatrix engineViewMatrix(const EngineVec3& viewOrigin, const EngineMat3& viewAxis)
{
  // renderer/GLMatrix.cpp:353-401 R_SetupViewMatrix, the world's modelViewMatrix
  static const float s_flipMatrix[16] = {
    // convert from our coordinate system (looking down X)
    // to OpenGL's coordinate system (looking down -Z)
    0,
    0,
    -1,
    0,
    -1,
    0,
    0,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    0,
    1};

  // transform by the camera placement
  const idVec3 origin = toId(viewOrigin);
  const idMat3 axis = toId(viewAxis);

  float viewerMatrix[16];
  viewerMatrix[0 * 4 + 0] = axis[0][0];
  viewerMatrix[1 * 4 + 0] = axis[0][1];
  viewerMatrix[2 * 4 + 0] = axis[0][2];
  viewerMatrix[3 * 4 + 0] =
    -origin[0] * axis[0][0] - origin[1] * axis[0][1] - origin[2] * axis[0][2];

  viewerMatrix[0 * 4 + 1] = axis[1][0];
  viewerMatrix[1 * 4 + 1] = axis[1][1];
  viewerMatrix[2 * 4 + 1] = axis[1][2];
  viewerMatrix[3 * 4 + 1] =
    -origin[0] * axis[1][0] - origin[1] * axis[1][1] - origin[2] * axis[1][2];

  viewerMatrix[0 * 4 + 2] = axis[2][0];
  viewerMatrix[1 * 4 + 2] = axis[2][1];
  viewerMatrix[2 * 4 + 2] = axis[2][2];
  viewerMatrix[3 * 4 + 2] =
    -origin[0] * axis[2][0] - origin[1] * axis[2][1] - origin[2] * axis[2][2];

  viewerMatrix[0 * 4 + 3] = 0.0f;
  viewerMatrix[1 * 4 + 3] = 0.0f;
  viewerMatrix[2 * 4 + 3] = 0.0f;
  viewerMatrix[3 * 4 + 3] = 1.0f;

  // convert from our coordinate system (looking down X)
  // to OpenGL's coordinate system (looking down -Z)
  auto modelViewMatrix = EngineGlMatrix{};
  R_MatrixMultiply(viewerMatrix, s_flipMatrix, modelViewMatrix.data());
  return modelViewMatrix;
}

EngineMat3 engineViewAxis(const EngineVec3& forward, const EngineVec3& up)
{
  const auto f = toId(forward);
  const auto u = toId(up);
  const auto left = u.Cross(f);
  return {f.x, f.y, f.z, left.x, left.y, left.z, u.x, u.y, u.z};
}

EngineMat4 engineMatrixTranspose(const EngineMat4& matrix)
{
  auto out = idRenderMatrix{};
  idRenderMatrix::Transpose(toId(matrix), out);
  return fromId(out);
}

bool engineCullBoundsToMvp(
  const EngineMat4& mvp, const EngineBounds& bounds, const bool zeroToOne)
{
  return idRenderMatrix::CullBoundsToMVP(toId(mvp), toId(bounds), zeroToOne);
}

EngineBounds engineProjectedFullyClippedBounds(
  const EngineMat4& mvp, const EngineBounds& bounds)
{
  auto projected = idBounds{};
  idRenderMatrix::ProjectedFullyClippedBounds(projected, toId(mvp), toId(bounds), true);
  return fromId(projected);
}

std::array<float, 2> engineDepthBoundsForBounds(
  const EngineMat4& mvp, const EngineBounds& bounds)
{
  auto min = 0.0f;
  auto max = 1.0f;
  idRenderMatrix::DepthBoundsForBounds(min, max, toId(mvp), toId(bounds), true);
  return {min, max};
}

std::uint16_t engineFtoui16(const float value)
{
  return idMath::Ftoui16(value);
}

std::array<EnginePlane, 6> engineFrustumPlanes(
  const EngineMat4& frustum, const bool zeroToOne, const bool normalize)
{
  idPlane planes[6];
  idRenderMatrix::GetFrustumPlanes(planes, toId(frustum), zeroToOne, normalize);
  auto result = std::array<EnginePlane, 6>{};
  for (std::size_t i = 0; i < 6; ++i)
  {
    result[i] = fromId(planes[i]);
  }
  return result;
}

EngineVec3 engineGlobalToNormalizedDeviceCoordinates(
  const EngineGlMatrix& modelViewMatrix,
  const EngineGlMatrix& projectionMatrix,
  const EngineVec3& point)
{
  // GLMatrix.cpp:215-243 R_GlobalToNormalizedDeviceCoordinates, the view's matrices given
  const idVec3 global = toId(point);
  idPlane view;
  idPlane clip;

  for (int i = 0; i < 4; i++)
  {
    view[i] = modelViewMatrix[std::size_t(i + 0 * 4)] * global[0]
              + modelViewMatrix[std::size_t(i + 1 * 4)] * global[1]
              + modelViewMatrix[std::size_t(i + 2 * 4)] * global[2]
              + modelViewMatrix[std::size_t(i + 3 * 4)];
  }

  for (int i = 0; i < 4; i++)
  {
    clip[i] = projectionMatrix[std::size_t(i + 0 * 4)] * view[0]
              + projectionMatrix[std::size_t(i + 1 * 4)] * view[1]
              + projectionMatrix[std::size_t(i + 2 * 4)] * view[2]
              + projectionMatrix[std::size_t(i + 3 * 4)] * view[3];
  }

  idVec3 ndc;
  const float invW = 1.0f / clip[3];
  ndc[0] = clip[0] * invW;
  ndc[1] = clip[1] * invW;
  ndc[2] = clip[2] * invW; // NOTE: in D3D this is in the range [0,1]
  return fromId(ndc);
}

EngineGlMatrix engineModelMatrix(const EngineVec3& origin, const EngineMat3& axis)
{
  auto modelMatrix = EngineGlMatrix{};
  R_AxisToModelMatrix(toId(axis), toId(origin), modelMatrix.data());
  return modelMatrix;
}

EngineGlMatrix engineGlMatrixMultiply(const EngineGlMatrix& a, const EngineGlMatrix& b)
{
  auto out = EngineGlMatrix{};
  R_MatrixMultiply(a.data(), b.data(), out.data());
  return out;
}

EngineVec3 engineGlobalPointToLocal(
  const EngineGlMatrix& modelMatrix, const EngineVec3& point)
{
  // GLMatrix.cpp:267-278 R_GlobalPointToLocal
  const idVec3 in = toId(point);
  idVec3 out;
  idVec3 temp;

  temp[0] = in[0] - modelMatrix[3 * 4 + 0];
  temp[1] = in[1] - modelMatrix[3 * 4 + 1];
  temp[2] = in[2] - modelMatrix[3 * 4 + 2];

  out[0] = temp[0] * modelMatrix[0 * 4 + 0] + temp[1] * modelMatrix[0 * 4 + 1]
           + temp[2] * modelMatrix[0 * 4 + 2];
  out[1] = temp[0] * modelMatrix[1 * 4 + 0] + temp[1] * modelMatrix[1 * 4 + 1]
           + temp[2] * modelMatrix[1 * 4 + 2];
  out[2] = temp[0] * modelMatrix[2 * 4 + 0] + temp[1] * modelMatrix[2 * 4 + 1]
           + temp[2] * modelMatrix[2 * 4 + 2];
  return fromId(out);
}

EnginePlane engineGlobalPlaneToLocal(
  const EngineGlMatrix& modelMatrix, const EnginePlane& plane)
{
  // GLMatrix.cpp:315-321 R_GlobalPlaneToLocal
  const idPlane in{plane.a, plane.b, plane.c, plane.d};
  idPlane out;
  out[0] = in[0] * modelMatrix[0 * 4 + 0] + in[1] * modelMatrix[0 * 4 + 1]
           + in[2] * modelMatrix[0 * 4 + 2];
  out[1] = in[0] * modelMatrix[1 * 4 + 0] + in[1] * modelMatrix[1 * 4 + 1]
           + in[2] * modelMatrix[1 * 4 + 2];
  out[2] = in[0] * modelMatrix[2 * 4 + 0] + in[1] * modelMatrix[2 * 4 + 1]
           + in[2] * modelMatrix[2 * 4 + 2];
  out[3] = in[0] * modelMatrix[3 * 4 + 0] + in[1] * modelMatrix[3 * 4 + 1]
           + in[2] * modelMatrix[3 * 4 + 2] + in[3];
  return fromId(out);
}

void engineBakeTextureMatrixIntoTexgen(
  std::array<EnginePlane, 4>& planes, const EngineGlMatrix& textureMatrix)
{
  // RenderBackend.cpp:223-259 RB_BakeTextureMatrixIntoTexgen, on the copied idPlane
  idPlane lightProject[3];
  for (std::size_t i = 0; i < 3; ++i)
  {
    lightProject[i] = idPlane{planes[i].a, planes[i].b, planes[i].c, planes[i].d};
  }

  float genMatrix[16];
  float final[16];

  genMatrix[0 * 4 + 0] = lightProject[0][0];
  genMatrix[1 * 4 + 0] = lightProject[0][1];
  genMatrix[2 * 4 + 0] = lightProject[0][2];
  genMatrix[3 * 4 + 0] = lightProject[0][3];

  genMatrix[0 * 4 + 1] = lightProject[1][0];
  genMatrix[1 * 4 + 1] = lightProject[1][1];
  genMatrix[2 * 4 + 1] = lightProject[1][2];
  genMatrix[3 * 4 + 1] = lightProject[1][3];

  genMatrix[0 * 4 + 2] = 0.0f;
  genMatrix[1 * 4 + 2] = 0.0f;
  genMatrix[2 * 4 + 2] = 0.0f;
  genMatrix[3 * 4 + 2] = 0.0f;

  genMatrix[0 * 4 + 3] = lightProject[2][0];
  genMatrix[1 * 4 + 3] = lightProject[2][1];
  genMatrix[2 * 4 + 3] = lightProject[2][2];
  genMatrix[3 * 4 + 3] = lightProject[2][3];

  R_MatrixMultiply(genMatrix, textureMatrix.data(), final);

  lightProject[0][0] = final[0 * 4 + 0];
  lightProject[0][1] = final[1 * 4 + 0];
  lightProject[0][2] = final[2 * 4 + 0];
  lightProject[0][3] = final[3 * 4 + 0];

  lightProject[1][0] = final[0 * 4 + 1];
  lightProject[1][1] = final[1 * 4 + 1];
  lightProject[1][2] = final[2 * 4 + 1];
  lightProject[1][3] = final[3 * 4 + 1];

  planes[0] = fromId(lightProject[0]);
  planes[1] = fromId(lightProject[1]);
}

} // namespace tb::lit
