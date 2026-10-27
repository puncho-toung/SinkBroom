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

#include "lit/CpuPool.h"
#include "lit/EngineLight.h"
#include "lit/MaterialDecls.h"

#include "kd/result.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// within rel of the larger magnitude, or abs near zero: the copy's Inverse and corner
// division go through _mm_rcp32_ps, two Newton steps on the CPU's 12-bit estimate
bool near(
  const float a, const float b, const float rel = 1.0e-6f, const float abs = 1.0e-6f)
{
  return std::abs(a - b) <= std::max(abs, rel * std::max(std::abs(a), std::abs(b)));
}

bool near(const EngineMat4& a, const EngineMat4& b)
{
  for (auto i = 0; i < 16; ++i)
  {
    if (!near(a[std::size_t(i)], b[std::size_t(i)]))
    {
      return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("EngineLight: the copied idRenderMatrix")
{
  const auto identity = engineIdentityMatrix();
  CHECK(identity == EngineMat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
  CHECK(engineZeroOneCube() == EngineBounds{EngineVec3{0, 0, 0}, EngineVec3{1, 1, 1}});

  // CreateFromOriginAxis: the axis' rows become the columns, the origin the last column
  const auto axis = EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1};
  const auto m = engineMatrixFromOriginAxis({10, 20, 30}, axis);
  CHECK(m == EngineMat4{0, -1, 0, 10, 1, 0, 0, 20, 0, 0, 1, 30, 0, 0, 0, 1});

  // Multiply on small integers is exact
  const auto a = EngineMat4{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const auto product = engineMatrixMultiply(a, identity);
  CHECK(product == a);
  const auto square = engineMatrixMultiply(a, a);
  CHECK(square[0] == 90.0f);
  CHECK(square[15] == 600.0f);

  // Inverse: of the rigid matrix, and none of a singular one
  const auto inverse = engineMatrixInverse(m);
  REQUIRE(inverse);
  CHECK(near(engineMatrixMultiply(m, *inverse), identity));
  CHECK_FALSE(engineMatrixInverse(a));

  // ProjectedBounds of the unit cube through a translation, in clip space
  const auto bounds = engineProjectedBounds(
    engineMatrixFromOriginAxis({1, 2, 3}, {1, 0, 0, 0, 1, 0, 0, 0, 1}),
    engineZeroOneCube(),
    false);
  CHECK(near(bounds[0][0], 1.0f));
  CHECK(near(bounds[1][2], 4.0f));
}

TEST_CASE("EngineLight: frustum corners and their plane test")
{
  const auto corners = engineFrustumCorners(engineIdentityMatrix(), engineZeroOneCube());
  // corner index (z << 2) | (y << 1) | x
  for (auto i = 0; i < 8; ++i)
  {
    const auto index = std::size_t(i);
    CHECK(near(corners.x[index], float(i & 1)));
    CHECK(near(corners.y[index], float((i >> 1) & 1)));
    CHECK(near(corners.z[index], float((i >> 2) & 1)));
  }

  // x = 2 is beyond the cube, x = -1 before it, x = 0.5 through it
  CHECK(engineCullFrustumCorners(corners, {1, 0, 0, -2}) == EngineFrustumCull::Back);
  CHECK(engineCullFrustumCorners(corners, {1, 0, 0, 1}) == EngineFrustumCull::Front);
  CHECK(engineCullFrustumCorners(corners, {1, 0, 0, -0.5f}) == EngineFrustumCull::Cross);
  // D58: a corner on the plane (d = +0) counts as in front
  CHECK(engineCullFrustumCorners(corners, {1, 0, 0, 0}) == EngineFrustumCull::Front);
  CHECK(engineCullFrustumCorners(corners, {-1, 0, 0, 0}) == EngineFrustumCull::Cross);
}

TEST_CASE("EngineLight: the copy runs on the CPU pool (C24)")
{
  auto pool =
    CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  auto future = pool->run([](const CpuJobContext&) -> Result<EngineMat4> {
    const auto m = engineMatrixFromOriginAxis({1, 2, 3}, {0, 0, 1, 0, 1, 0, -1, 0, 0});
    return engineMatrixInverse(m).value_or(EngineMat4{});
  });
  auto result = future.get();
  // the copy reports nothing (no common call, no assert reached)
  CHECK(result.messages.empty());
  const auto expected = engineMatrixInverse(
    engineMatrixFromOriginAxis({1, 2, 3}, {0, 0, 1, 0, 1, 0, -1, 0, 0}));
  REQUIRE(expected);
  CHECK(result.result == Result<EngineMat4>{*expected});
  pool->stop();
}

TEST_CASE("EngineLight: the shadow LOD of SC703's table (LP3.0, C58, D144)")
{
  // a window-space box of projected radius r: (0, 0.5, 0.5) to (2r, 0.5, 0.5), so the
  // copied GetRadius( GetCenter() ) is Sqrt( r · r ) = t · sqrtf( 1 / t ); the radius
  // bits, flod and the fade were computed by a script with every operation rounded to
  // float32 (the engine's arithmetic)
  const auto boxOf = [](const float r) {
    return EngineBounds{EngineVec3{0.0f, 0.5f, 0.5f}, EngineVec3{2.0f * r, 0.5f, 0.5f}};
  };
  const auto bitsOf = [](const float value) {
    auto bits = std::uint32_t{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  };
  struct Row
  {
    float r = 0.0f;
    int pointLod = 0;
    int parallelLod = 0;
    std::uint32_t fadeBits = 0;
  };
  // SC703 at its 4-place boundaries; its table is rounded: the copied code's exact
  // boundaries are (1 - k/6) / 1.4 (0.119048, 0.238095, 0.357143, 0.476190, 0.595238), so
  // 0.4762 is LOD 1 (the table's row "0.3571 < r <= 0.4762: 2" ends at 0.476190)
  const auto rows = std::vector<Row>{
    // r = 0: "object intersects near view plane", flod 0, LOD 0 (512 for point and spot)
    {0.0f, 1, 0, 0},
    // flod 5.0004: LOD 5 is past the last, no shadow
    {0.119f, -1, -1, 0},
    // flod 4.99956: LOD 4 (128) with the fade frac( flod ) = 0.99956
    {0.1191f, 4, 4, 0x3f7fe328u},
    {0.2f, 4, 4, 0x3ea3d700u},
    // flod 3.99996 and 3.00036: LOD 3 (256)
    {0.2381f, 3, 3, 0},
    {0.3571f, 3, 3, 0},
    // flod 1.99992 and 1.00032: LOD 1 (512)
    {0.4762f, 1, 1, 0},
    {0.5952f, 1, 1, 0},
    // a camera inside the volume: r >= sqrt( 0.5 ), LOD 0, 1024 for a parallel light
    // alone
    {0.7071f, 1, 0, 0},
    {1.0f, 1, 0, 0},
  };
  for (const auto& row : rows)
  {
    CAPTURE(row.r);
    const auto point = engineShadowLod(boxOf(row.r), false);
    const auto parallel = engineShadowLod(boxOf(row.r), true);
    CHECK(point.lod == row.pointLod);
    CHECK(parallel.lod == row.parallelLod);
    CHECK(bitsOf(point.fadeOut) == row.fadeBits);
    CHECK(bitsOf(parallel.fadeOut) == row.fadeBits);
    // the defaults are r_shadowMapLodScale 1.4 and r_shadowMapLodBias 0
    CHECK(engineShadowLod(boxOf(row.r), false, 1.4f, 0) == point);
  }
  // the radius through the copy: 0.1191's is 0x3df3eab3 (a box's radius above 1 is 1)
  CHECK(engineMathSqrt(0.1191f * 0.1191f) == 0.11909999698400497f);
  CHECK(engineShadowLod(boxOf(5.0f), true).lod == 0);
  // the scale is capped at 20; a bias moves the LOD after the truncation and before the
  // "no shadow" test, which the fade follows (Ftoi, then lodBias, then >= 5)
  CHECK(engineShadowLod(boxOf(0.2381f), false, 1.4f, 1).lod == 4);
  CHECK(engineShadowLod(boxOf(0.2381f), false, 1.4f, 2).lod == -1);
  CHECK(engineShadowLod(boxOf(0.1191f), false, 1.4f, -10).lod == 1);
  CHECK(
    engineShadowLod(boxOf(0.1f), false, 100.0f, 0)
    == engineShadowLod(boxOf(0.1f), false, 20.0f, 0));
  // the resolutions per LOD (Framebuffer.h:48)
  CHECK(EngineShadowMapResolutions == std::array<int, 5>{1024, 512, 256, 256, 128});
}

} // namespace tb::lit
