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

} // namespace tb::lit
