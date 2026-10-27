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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp,
// renderer/tr_frontend_main.cpp, renderer/GLMatrix.cpp, idlib/math/Vector.{h,cpp},
// idlib/math/Math.h, idlib/bv/Bounds.h and idlib/geometry/RenderMatrix.{h,cpp}):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP3.1 (C58, D143, D160, SC710-SC712): the shadow maps' matrices of the copied ranges
// (lit/EngineShadowMatrices.h) on fixed inputs, bit for bit against a straight float32
// transcription of the engine's arithmetic written here: R_SetupSplitFrustums with
// R_SetupProjectionMatrix2, and SetupShadowMapMatrices for a point light's six sides, a
// spot light and a parallel light's four cascades. The idRenderMatrix operations the
// range calls (Multiply, Transpose, Inverse, GetFrustumCorners) are the copy's, through
// the light facade, as R_MatrixMultiply is (engineGlMatrixMultiply): the test transcribes
// what the ranges themselves compute.

#include "LitView.h"
#include "lit/EngineLight.h"
#include "lit/EngineShadowMatrices.h"
#include "lit/EngineTables.h"
#include "lit/LitSession.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// idMath::PI and idMath::INFINITUM (Math.h), FLT_SMALLEST_NON_DENORMAL (Math.h)
constexpr auto Pi = 3.14159265358979323846f;
constexpr auto Infinitum = 1e30f;
constexpr auto SmallestNonDenormal = 1.1754943508222875e-038f;

bool bitEqual(const EngineMat4& a, const EngineMat4& b)
{
  return std::memcmp(a.data(), b.data(), sizeof(float) * a.size()) == 0;
}

// idMath::InvSqrt (Math.h:527-552)
float invSqrt(const float x)
{
  return (x > SmallestNonDenormal) ? std::sqrt(1.0f / x) : Infinitum;
}

// R_SetupProjectionMatrix2 (GLMatrix.cpp:548-603) of a viewport w × h, the jitter 0,
// r_centerX and r_centerY 0, no stereo separation, no flip
std::array<float, 16> projectionMatrix2(
  const float fovX,
  const float fovY,
  const int w,
  const int h,
  const float zNear,
  const float zFar)
{
  auto ymax = zNear * std::tan(fovY * Pi / 360.0f);
  auto ymin = -ymax;
  auto xmax = zNear * std::tan(fovX * Pi / 360.0f);
  auto xmin = -xmax;
  const auto width = xmax - xmin;
  const auto height = ymax - ymin;
  const auto viewWidth = w;
  const auto viewHeight = h;
  auto jitterx = 0.0f;
  auto jittery = 0.0f;
  jitterx = jitterx * width / float(viewWidth);
  jitterx += 0.0f;
  jitterx += 0.0f;
  xmin += jitterx * width;
  xmax += jitterx * width;
  jittery = jittery * height / float(viewHeight);
  jittery += 0.0f;
  ymin += jittery * height;
  ymax += jittery * height;
  const auto depth = zFar - zNear;
  auto m = std::array<float, 16>{};
  m[0 * 4 + 0] = 2.0f * zNear / width;
  m[1 * 4 + 0] = 0.0f;
  m[2 * 4 + 0] = (xmax + xmin) / width;
  m[3 * 4 + 0] = 0.0f;
  m[0 * 4 + 1] = 0.0f;
  m[1 * 4 + 1] = 2.0f * zNear / height;
  m[2 * 4 + 1] = (ymax + ymin) / height;
  m[3 * 4 + 1] = 0.0f;
  m[0 * 4 + 2] = 0.0f;
  m[1 * 4 + 2] = 0.0f;
  m[2 * 4 + 2] = -(zFar + zNear) / depth;
  m[3 * 4 + 2] = -2 * zFar * zNear / depth;
  m[0 * 4 + 3] = 0.0f;
  m[1 * 4 + 3] = 0.0f;
  m[2 * 4 + 3] = -1.0f;
  m[3 * 4 + 3] = 0.0f;
  return m;
}

// R_SetupSplitFrustums (tr_frontend_main.cpp:397-456): r_znear 3 (no cramZNear),
// r_shadowMapSplitWeight 0.9, r_shadowMapSplits 3
EngineSplitFrustums splitFrustums(const LitView& view)
{
  const auto zNearStart = 3.0f;
  const auto zFarEnd = 10000.0f;
  auto zNear = zNearStart;
  auto zFar = zFarEnd;
  const auto lambda = 0.9f;
  const auto ratio = zFarEnd / zNearStart;
  const auto splits = 3;
  auto out = EngineSplitFrustums{};
  for (auto i = 0; i < 6; ++i)
  {
    out.splitDistances[std::size_t(i)] = Infinitum;
  }
  for (auto i = 1; i <= splits + 1 && i < 6; ++i)
  {
    const auto si = float(i) / float(splits + 1);
    if (i > 1)
    {
      zNear = zFar - (zFar * 0.005f);
    }
    zFar = 1.005f * lambda * (zNearStart * std::pow(ratio, si))
           + (1 - lambda) * (zNearStart + (zFarEnd - zNearStart) * si);
    if (i <= splits)
    {
      out.splitDistances[std::size_t(i - 1)] = zFar;
    }
    const auto projection =
      projectionMatrix2(view.fovX, view.fovY, view.width, view.height, zNear, zFar);
    out.frustumMVPs[std::size_t(i)] = engineMatrixMultiply(
      engineMatrixTranspose(projection),
      engineMatrixTranspose(view.worldModelViewMatrix));
  }
  return out;
}

// the NVRHI matClipToUvzw of a point or parallel side (RenderBackend.cpp:2999-3006,
// 3131-3138) and of a spot light (:3152-3157)
constexpr auto ClipToUvzw = EngineMat4{
  1.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  -1.0f,
  0.0f,
  0.0f, //
  0.0f,
  0.0f,
  2.0f,
  -1.0f,
  0.0f,
  0.0f,
  0.0f,
  1.0f};
constexpr auto SpotClipToUvzw = EngineMat4{
  0.5f,
  0.0f,
  0.0f,
  0.5f,
  0.0f,
  -0.5f,
  0.0f,
  0.5f, //
  0.0f,
  0.0f,
  1.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  1.0f};

// a point light's side (RenderBackend.cpp:3009-3140)
EngineShadowSideMatrices pointSide(const EngineVec3& origin, const int side)
{
  auto viewMatrix = std::array<float, 16>{};
  switch (side)
  {
  case 0:
    viewMatrix[0] = 1;
    viewMatrix[9] = 1;
    viewMatrix[6] = -1;
    break;
  case 1:
    viewMatrix[0] = -1;
    viewMatrix[9] = -1;
    viewMatrix[6] = -1;
    break;
  case 2:
    viewMatrix[4] = 1;
    viewMatrix[1] = -1;
    viewMatrix[10] = 1;
    break;
  case 3:
    viewMatrix[4] = -1;
    viewMatrix[1] = -1;
    viewMatrix[10] = -1;
    break;
  case 4:
    viewMatrix[8] = 1;
    viewMatrix[1] = -1;
    viewMatrix[6] = -1;
    break;
  case 5:
    viewMatrix[8] = -1;
    viewMatrix[1] = 1;
    viewMatrix[6] = -1;
    break;
  default:
    break;
  }
  viewMatrix[12] =
    -origin[0] * viewMatrix[0] + -origin[1] * viewMatrix[4] + -origin[2] * viewMatrix[8];
  viewMatrix[13] =
    -origin[0] * viewMatrix[1] + -origin[1] * viewMatrix[5] + -origin[2] * viewMatrix[9];
  viewMatrix[14] =
    -origin[0] * viewMatrix[2] + -origin[1] * viewMatrix[6] + -origin[2] * viewMatrix[10];
  viewMatrix[3] = 0;
  viewMatrix[7] = 0;
  viewMatrix[11] = 0;
  viewMatrix[15] = 1;
  constexpr auto flip =
    std::array<float, 16>{0, 0, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
  const auto lightViewMatrix = engineGlMatrixMultiply(viewMatrix, flip);

  auto out = EngineShadowSideMatrices{};
  out.lightView = engineMatrixTranspose(lightViewMatrix);

  const auto zNear = 4.0f;
  const auto fov = 92.0f;
  const auto ymax = zNear * std::tan(fov * Pi / 360.0f);
  const auto ymin = -ymax;
  const auto xmax = zNear * std::tan(fov * Pi / 360.0f);
  const auto xmin = -xmax;
  const auto width = xmax - xmin;
  const auto height = ymax - ymin;
  auto p = std::array<float, 16>{};
  p[0 * 4 + 0] = -2.0f * zNear / width;
  p[1 * 4 + 0] = 0.0f;
  p[2 * 4 + 0] = (xmax + xmin) / width;
  p[3 * 4 + 0] = 0.0f;
  p[0 * 4 + 1] = 0.0f;
  p[1 * 4 + 1] = 2.0f * zNear / height;
  p[2 * 4 + 1] = (ymax + ymin) / height;
  p[3 * 4 + 1] = 0.0f;
  p[0 * 4 + 2] = 0.0f;
  p[1 * 4 + 2] = 0.0f;
  p[2 * 4 + 2] = -0.999f;
  p[3 * 4 + 2] = -2.0f * zNear;
  p[0 * 4 + 3] = 0.0f;
  p[1 * 4 + 3] = 0.0f;
  p[2 * 4 + 3] = -1.0f;
  p[3 * 4 + 3] = 0.0f;
  out.lightProjection = engineMatrixTranspose(p);
  out.shadowV = out.lightView;
  out.shadowP = engineMatrixMultiply(ClipToUvzw, out.lightProjection);
  return out;
}

// MatrixOrthogonalProjectionRH (RenderBackend.cpp:2748-2766)
std::array<float, 16> orthogonal(
  const float left,
  const float right,
  const float bottom,
  const float top,
  const float zNear,
  const float zFar)
{
  auto m = std::array<float, 16>{};
  m[0] = 2 / (right - left);
  m[4] = 0;
  m[8] = 0;
  m[12] = (left + right) / (left - right);
  m[1] = 0;
  m[5] = 2 / (top - bottom);
  m[9] = 0;
  m[13] = (top + bottom) / (bottom - top);
  m[2] = 0;
  m[6] = 0;
  m[10] = 1 / (zNear - zFar);
  m[14] = zNear / (zNear - zFar);
  m[3] = 0;
  m[7] = 0;
  m[11] = 0;
  m[15] = 1;
  return m;
}

// idBounds: Clear and AddPoint (Bounds.h)
struct Bounds
{
  EngineVec3 b0 = {Infinitum, Infinitum, Infinitum};
  EngineVec3 b1 = {-Infinitum, -Infinitum, -Infinitum};

  void add(const EngineVec3& v)
  {
    for (std::size_t i = 0; i < 3; ++i)
    {
      if (v[i] < b0[i])
      {
        b0[i] = v[i];
      }
      if (v[i] > b1[i])
      {
        b1[i] = v[i];
      }
    }
  }
};

// TransformPoint( idVec4, idVec4 ) (RenderMatrix.h:358-366), then xyz / w
EngineVec3 transformDivided(
  const EngineMat4& m, const float x, const float y, const float z)
{
  const auto in = std::array<float, 4>{x, y, z, 1.0f};
  auto out = std::array<float, 4>{};
  for (std::size_t r = 0; r < 4; ++r)
  {
    out[r] = in[0] * m[r * 4 + 0] + in[1] * m[r * 4 + 1] + in[2] * m[r * 4 + 2]
             + in[3] * m[r * 4 + 3];
  }
  out[0] /= out[3];
  out[1] /= out[3];
  out[2] /= out[3];
  return {out[0], out[1], out[2]};
}

// a parallel light's cascade (RenderBackend.cpp:2841-3007)
EngineShadowSideMatrices parallelSide(
  const EngineShadowLight& light,
  const LitView& view,
  const EngineSplitFrustums& splits,
  const int side)
{
  // idVec3 lightDir = -lightCenter; Normalize (Vector.h); 0 -> ( 0 0 -1 )
  auto lightDir =
    EngineVec3{-light.lightCenter[0], -light.lightCenter[1], -light.lightCenter[2]};
  {
    const auto sqrLength =
      lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1] + lightDir[2] * lightDir[2];
    const auto invLength = invSqrt(sqrLength);
    lightDir[0] *= invLength;
    lightDir[1] *= invLength;
    lightDir[2] *= invLength;
    if (invLength * sqrLength == 0.0f)
    {
      lightDir[2] = -1.0f;
    }
  }
  // idVec3::ToMat3 (Vector.cpp:238-261)
  auto rotation = EngineMat3{};
  rotation[0] = lightDir[0];
  rotation[1] = lightDir[1];
  rotation[2] = lightDir[2];
  auto d = lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1];
  if (d == 0.0f)
  {
    rotation[3] = 1.0f;
    rotation[4] = 0.0f;
    rotation[5] = 0.0f;
  }
  else
  {
    d = invSqrt(d);
    rotation[3] = -lightDir[1] * d;
    rotation[4] = lightDir[0] * d;
    rotation[5] = 0.0f;
  }
  // mat[2] = mat[0].Cross( mat[1] )
  rotation[6] = rotation[1] * rotation[5] - rotation[2] * rotation[4];
  rotation[7] = rotation[2] * rotation[3] - rotation[0] * rotation[5];
  rotation[8] = rotation[0] * rotation[4] - rotation[1] * rotation[3];

  // idRenderMatrix::CreateViewMatrix( vieworg, rotation ) (RenderMatrix.cpp:669-690)
  const auto& o = view.viewOrigin;
  const auto axis = [&](const std::size_t r, const std::size_t c) {
    return rotation[r * 3 + c];
  };
  auto lightView = EngineMat4{};
  lightView[0] = -axis(1, 0);
  lightView[1] = -axis(1, 1);
  lightView[2] = -axis(1, 2);
  lightView[3] = o[0] * axis(1, 0) + o[1] * axis(1, 1) + o[2] * axis(1, 2);
  lightView[4] = axis(2, 0);
  lightView[5] = axis(2, 1);
  lightView[6] = axis(2, 2);
  lightView[7] = -(o[0] * axis(2, 0) + o[1] * axis(2, 1) + o[2] * axis(2, 2));
  lightView[8] = -axis(0, 0);
  lightView[9] = -axis(0, 1);
  lightView[10] = -axis(0, 2);
  lightView[11] = o[0] * axis(0, 0) + o[1] * axis(0, 1) + o[2] * axis(0, 2);
  lightView[12] = 0.0f;
  lightView[13] = 0.0f;
  lightView[14] = 0.0f;
  lightView[15] = 1.0f;

  auto lightBounds = Bounds{};
  const auto corners =
    engineFrustumCorners(light.inverseBaseLightProject, engineZeroOneCube());
  for (std::size_t j = 0; j < 8; ++j)
  {
    lightBounds.add(
      transformDivided(lightView, corners.x[j], corners.y[j], corners.z[j]));
  }

  const auto inverse = engineMatrixInverse(splits.frustumMVPs[1 + std::size_t(side)]);
  REQUIRE(inverse);
  const auto splitCorners = engineFrustumCorners(*inverse, engineUnitCube());
  auto cropBounds = Bounds{};
  for (std::size_t j = 0; j < 8; ++j)
  {
    cropBounds.add(transformDivided(
      lightView, splitCorners.x[j], splitCorners.y[j], splitCorners.z[j]));
  }
  if (cropBounds.b0[0] < lightBounds.b0[0])
  {
    cropBounds.b0[0] = lightBounds.b0[0];
  }
  if (cropBounds.b0[1] < lightBounds.b0[1])
  {
    cropBounds.b0[1] = lightBounds.b0[1];
  }
  if (cropBounds.b1[0] > lightBounds.b1[0])
  {
    cropBounds.b1[0] = lightBounds.b1[0];
  }
  if (cropBounds.b1[1] > lightBounds.b1[1])
  {
    cropBounds.b1[1] = lightBounds.b1[1];
  }
  cropBounds.b0[2] = lightBounds.b0[2];
  cropBounds.b1[2] = lightBounds.b1[2];

  const auto projection = orthogonal(
    cropBounds.b0[0],
    cropBounds.b1[0],
    cropBounds.b0[1],
    cropBounds.b1[1],
    -cropBounds.b1[2],
    -cropBounds.b0[2]);
  auto out = EngineShadowSideMatrices{};
  out.lightView = lightView;
  out.lightProjection = engineMatrixTranspose(projection);
  out.shadowV = lightView;
  out.shadowP = engineMatrixMultiply(ClipToUvzw, out.lightProjection);
  return out;
}

void checkBitEqual(const EngineShadowSideMatrices& a, const EngineShadowSideMatrices& b)
{
  CHECK(bitEqual(a.lightView, b.lightView));
  CHECK(bitEqual(a.lightProjection, b.lightProjection));
  CHECK(bitEqual(a.shadowV, b.shadowV));
  CHECK(bitEqual(a.shadowP, b.shadowP));
}

CameraState camera()
{
  // a view 160 × 120 at fov 90, looking along ( 0.8, 0.6, 0 )
  auto state = CameraState{};
  state.position = {12.5f, -40.25f, 64.0f};
  state.direction = {0.8f, 0.6f, 0.0f};
  state.up = {0.0f, 0.0f, 1.0f};
  state.fovDegrees = 90.0f;
  state.viewportWidth = 160;
  state.viewportHeight = 120;
  return state;
}

EngineShadowView shadowViewOf(const LitView& view)
{
  auto out = EngineShadowView{};
  out.vieworg = view.viewOrigin;
  out.viewaxis = view.viewAxis;
  out.fovX = view.fovX;
  out.fovY = view.fovY;
  out.width = view.width;
  out.height = view.height;
  out.worldModelViewMatrix = view.worldModelViewMatrix;
  out.projectionMatrix = view.projectionMatrix;
  return out;
}

} // namespace

TEST_CASE("EngineShadowMatrices (LP3.1, C58, D160)")
{
  const auto view = litViewOf(camera(), 160, 120);
  const auto shadowView = shadowViewOf(view);

  SECTION("the stand-in cvars hold EngineFrame's values")
  {
    const auto cvars = engineShadowMatrixCvars();
    CHECK(cvars.znear == EngineFrame.znear);
    CHECK(cvars.shadowMapFrustumFov == EngineFrame.shadowMapFrustumFov);
    CHECK(cvars.shadowMapSplits == EngineFrame.shadowMapSplits);
    CHECK(cvars.shadowMapSplitWeight == EngineFrame.shadowMapSplitWeight);
    CHECK(cvars.centerX == 0.0f);
    CHECK(cvars.centerY == 0.0f);
  }

  SECTION("R_SetupSplitFrustums: the split distances and the cascades' MVPs")
  {
    const auto copied = engineSetupSplitFrustums(shadowView);
    const auto expected = splitFrustums(view);
    for (std::size_t i = 0; i < EngineMaxFrustums; ++i)
    {
      CHECK(std::memcmp(&copied.splitDistances[i], &expected.splitDistances[i], 4) == 0);
    }
    for (std::size_t i = 1; i <= 4; ++i)
    {
      CHECK(bitEqual(copied.frustumMVPs[i], expected.frustumMVPs[i]));
    }
    // SC712: ≈ 270.8, 656.8, 1940.5, then idMath::INFINITUM
    std::cout << "LP3.1 split distances: " << copied.splitDistances[0] << " "
              << copied.splitDistances[1] << " " << copied.splitDistances[2] << " "
              << copied.splitDistances[3] << "\n";
    CHECK(std::abs(copied.splitDistances[0] - 270.8f) < 0.1f);
    CHECK(std::abs(copied.splitDistances[1] - 656.8f) < 0.1f);
    CHECK(std::abs(copied.splitDistances[2] - 1940.5f) < 0.1f);
    CHECK(copied.splitDistances[3] == Infinitum);
    CHECK(copied.splitDistances[5] == Infinitum);
  }

  SECTION("a point light's six sides")
  {
    auto geometry = EngineLightGeometry{};
    geometry.origin = {37.5f, -12.25f, 80.0f};
    geometry.lightRadius = {200.0f, 180.0f, 160.0f};
    geometry.pointLight = true;
    const auto projection = engineDeriveLightProjection(geometry);
    auto light = EngineShadowLight{};
    light.globalLightOrigin = projection.globalLightOrigin;
    light.baseLightProject = projection.baseLightProject;
    light.inverseBaseLightProject = projection.inverseBaseLightProject;
    light.pointLight = true;
    const auto splits = engineSetupSplitFrustums(shadowView);
    for (auto side = 0; side < 6; ++side)
    {
      CAPTURE(side);
      checkBitEqual(
        engineShadowMapMatrices(light, side, shadowView, splits),
        pointSide(light.globalLightOrigin, side));
    }
    // [2][2] -0.999 and [3][2] -2 · zNear as the render matrix holds them (transposed)
    const auto side0 = engineShadowMapMatrices(light, 0, shadowView, splits);
    CHECK(side0.lightProjection[2 * 4 + 2] == -0.999f);
    CHECK(side0.lightProjection[2 * 4 + 3] == -8.0f);
    CHECK(side0.lightProjection[3 * 4 + 2] == -1.0f);
  }

  SECTION("a spot light")
  {
    auto geometry = EngineLightGeometry{};
    geometry.origin = {-20.0f, 64.0f, 96.0f};
    geometry.target = {180.0f, 10.0f, -60.0f};
    geometry.right = {10.0f, -60.0f, 0.0f};
    geometry.up = {30.0f, 5.0f, 55.0f};
    const auto projection = engineDeriveLightProjection(geometry);
    auto light = EngineShadowLight{};
    light.globalLightOrigin = projection.globalLightOrigin;
    light.baseLightProject = projection.baseLightProject;
    light.inverseBaseLightProject = projection.inverseBaseLightProject;
    const auto copied = engineShadowMapMatrices(
      light, -1, shadowView, engineSetupSplitFrustums(shadowView));
    auto expected = EngineShadowSideMatrices{};
    expected.lightView = engineIdentityMatrix();
    expected.lightProjection = light.baseLightProject;
    expected.shadowV = expected.lightView;
    expected.shadowP = engineMatrixMultiply(
      SpotClipToUvzw,
      engineMatrixMultiply(engineWindowSpaceToClipSpace(), light.baseLightProject));
    checkBitEqual(copied, expected);
    // windowToClip (RenderMatrix.cpp:86-91)
    CHECK(
      engineWindowSpaceToClipSpace()
      == EngineMat4{2, 0, 0, -1, 0, 2, 0, -1, 0, 0, 2, -1, 0, 0, 0, 1});
    CHECK(
      engineClipSpaceToWindowSpace()
      == EngineMat4{0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 0, 1});
  }

  SECTION("a parallel light's four cascades")
  {
    auto geometry = EngineLightGeometry{};
    geometry.origin = {40.0f, 20.0f, 100.0f};
    geometry.lightRadius = {300.0f, 250.0f, 120.0f};
    geometry.lightCenter = {30.0f, -20.0f, 100.0f};
    geometry.pointLight = true;
    geometry.parallel = true;
    const auto projection = engineDeriveLightProjection(geometry);
    auto light = EngineShadowLight{};
    light.globalLightOrigin = projection.globalLightOrigin;
    light.baseLightProject = projection.baseLightProject;
    light.inverseBaseLightProject = projection.inverseBaseLightProject;
    light.pointLight = true;
    light.parallel = true;
    light.lightCenter = geometry.lightCenter;
    const auto splits = engineSetupSplitFrustums(shadowView);
    for (auto side = 0; side < 4; ++side)
    {
      CAPTURE(side);
      checkBitEqual(
        engineShadowMapMatrices(light, side, shadowView, splits),
        parallelSide(light, view, splits, side));
    }
  }
}

} // namespace tb::lit
