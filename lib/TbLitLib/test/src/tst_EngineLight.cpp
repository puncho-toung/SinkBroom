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

#include <cmath>
#include <thread>

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

} // namespace tb::lit
