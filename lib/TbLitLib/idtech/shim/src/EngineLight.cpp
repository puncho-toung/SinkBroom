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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_defs.cpp and
// renderer/GLMatrix.cpp):
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

} // namespace tb::lit
