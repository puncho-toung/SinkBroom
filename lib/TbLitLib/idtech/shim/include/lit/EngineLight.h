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

// ----- what ParseSpawnArgsToRenderLight and idLight::Spawn call (the copy, pure) -----

// idVec3::FixDegenerateNormal (Vector.h): an axial vector becomes the unit axis its
// non-zero component points along; zero becomes (0 0 -1)
EngineVec3 engineFixDegenerateNormal(const EngineVec3& v);

// idMath::AngleNormalize360 (Math.h)
float engineAngleNormalize360(float angle);

// renderer/Material.h's predef_lightstyles, the Quake light styles idLight::Spawn falls
// back to (ML_NUM_STYLES of them)
std::vector<std::string> engineDefaultLightStyles();

} // namespace tb::lit
