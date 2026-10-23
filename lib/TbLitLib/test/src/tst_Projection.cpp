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

#include "lit/Projection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

bool near(const float a, const float b, const float tolerance = 1.0e-4f)
{
  return std::abs(a - b) <= tolerance;
}

// out = m · ( x y z 1 ) for a matrix in the engine's layout ([column * 4 + row])
std::array<float, 4> transform(const EngineGlMatrix& m, const EngineVec3& p)
{
  auto out = std::array<float, 4>{};
  for (auto row = 0; row < 4; ++row)
  {
    out[std::size_t(row)] =
      m[std::size_t(0 * 4 + row)] * p[0] + m[std::size_t(1 * 4 + row)] * p[1]
      + m[std::size_t(2 * 4 + row)] * p[2] + m[std::size_t(3 * 4 + row)];
  }
  return out;
}

CameraState cameraOf(
  const float fov, const std::uint32_t width, const std::uint32_t height)
{
  auto camera = CameraState{};
  camera.fovDegrees = fov;
  camera.viewportWidth = width;
  camera.viewportHeight = height;
  return camera;
}

} // namespace

// The values are compared with an independent script in tst_LightGoldens.cpp; these
// check the conventions.

TEST_CASE("Projection: TrenchBroom's fov to g_fov (S39)")
{
  // a 4:3 frame with a 90 degree horizontal fov: tan( fov_y / 2 ) = 0.75
  const auto fovY = verticalFovOf(90.0f);
  CHECK(near(fovY, 73.739795f));
  // g_fov's 16:9 frame: tan( g_fov / 2 ) = 16 / 9 · 0.75 = 4 / 3
  CHECK(near(gFovOf(fovY), 106.260205f));
}

TEST_CASE("Projection: CalcFov and the projection matrix (A16)")
{
  SECTION("16:9: the vertical fov is TrenchBroom's, the horizontal g_fov")
  {
    const auto view = viewProjectionOf(cameraOf(90.0f, 1920, 1080));
    CHECK(near(view.fov.y, verticalFovOf(90.0f)));
    CHECK(near(view.fov.x, view.gFov));
  }

  SECTION("4:3: the horizontal fov is TrenchBroom's")
  {
    const auto view = viewProjectionOf(cameraOf(90.0f, 800, 600));
    CHECK(near(view.fov.x, 90.0f));
  }

  SECTION("a square window takes g_fov for both axes")
  {
    const auto view = viewProjectionOf(cameraOf(90.0f, 512, 512));
    CHECK(view.fov.x == view.gFov);
    CHECK(view.fov.y == view.gFov);
  }

  const auto view = viewProjectionOf(cameraOf(90.0f, 1920, 1080));
  const auto& p = view.projectionMatrix;
  // infinite far, clip z 0..1: [2][2] = -0.999, [3][2] = -zNear, [2][3] = -1
  CHECK(p[2 * 4 + 2] == -0.999f);
  CHECK(p[3 * 4 + 2] == -3.0f);
  CHECK(p[2 * 4 + 3] == -1.0f);
  CHECK(p[3 * 4 + 3] == 0.0f);
  // no jitter: the offsets are -2 · 0 / size, negative zeros
  CHECK(p[2 * 4 + 0] == 0.0f);
  CHECK(std::signbit(p[2 * 4 + 0]));
  CHECK(std::signbit(p[2 * 4 + 1]));
  // 2 · zNear / ( 2 · xmax ) = 1 / tan( fov_x / 2 )
  CHECK(near(p[0], 1.0f / std::tan(view.fov.x * 3.14159265f / 360.0f)));

  // depth = 0.999 - zNear / distance: -0.001 at zNear (the -0.999 crunch), towards 0.999
  // at infinity
  const auto nearPoint = transform(p, {0, 0, -3});
  CHECK(near(nearPoint[2] / nearPoint[3], -0.001f, 1.0e-6f));
  const auto farPoint = transform(p, {0, 0, -30000});
  CHECK(near(farPoint[2] / farPoint[3], 0.999f - 0.0001f, 1.0e-6f));
}

TEST_CASE("Projection: the view matrix looks down -Z (R_SetupViewMatrix)")
{
  auto camera = cameraOf(90.0f, 1920, 1080);
  camera.position = vm::vec3f{10, 20, 30};
  camera.direction = vm::vec3f{0, 1, 0};
  camera.up = vm::vec3f{0, 0, 1};
  const auto view = viewProjectionOf(camera);
  // forward, left = up × forward, up
  CHECK(view.viewAxis == EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1});

  // a point ahead of the camera is on view -Z, one to its left on view -X, one above
  // on view +Y
  const auto ahead = transform(view.modelViewMatrix, {10, 120, 30});
  CHECK(near(ahead[0], 0.0f));
  CHECK(near(ahead[1], 0.0f));
  CHECK(near(ahead[2], -100.0f));
  const auto left = transform(view.modelViewMatrix, {0, 20, 30});
  CHECK(near(left[0], -10.0f));
  const auto above = transform(view.modelViewMatrix, {10, 20, 40});
  CHECK(near(above[1], 10.0f));

  CHECK(
    parityCameraLine(camera, view).rfind("camera 10 20 30 yaw 90.0000 pitch -0.0000", 0)
    == 0);
}

TEST_CASE("Projection: the integer g_fov's inverse (C29, P2.6)")
{
  // the parity rows' g_fov: the fov found gives the engine's base_fov exactly, so the
  // projection is R_SetupProjectionMatrix's for g_fov 90 at 1280 x 720
  const auto fov = trenchBroomFovOfGFov(90);
  REQUIRE(fov);
  CHECK(gFovOf(verticalFovOf(*fov)) == 90.0f);
  CHECK(near(*fov, 73.739795f));

  const auto view = viewProjectionOf(cameraOf(*fov, 1280, 720));
  CHECK(view.gFov == 90.0f);
  const auto engineFov = engineCalcFov(90.0f, 1280, 720, 1.0f);
  CHECK(view.fov.x == engineFov.x);
  CHECK(view.fov.y == engineFov.y);
  CHECK(
    view.projectionMatrix
    == engineProjectionMatrix(engineFov.x, engineFov.y, 3.0f, 1280, 720));

  // how many of the cvar's values have an exact inverse in float
  auto exact = 0;
  for (auto g = 1; g <= 179; ++g)
  {
    if (const auto f = trenchBroomFovOfGFov(g))
    {
      CHECK(gFovOf(verticalFovOf(*f)) == float(g));
      ++exact;
    }
  }
  INFO(exact << " of the 179 integer g_fov values have an exact inverse");
  // U3.17: pinned, so that a change to gFovOf or verticalFovOf is seen
  CHECK(exact == 122);
  CHECK_FALSE(trenchBroomFovOfGFov(0));
  CHECK_FALSE(trenchBroomFovOfGFov(180));
}

TEST_CASE("Projection: the parity rows' camera lines round trip (C29)")
{
  // parity.py's camera lines of the twelve rows (the render-view origin at 96 above the
  // floor of each room, 1024 apart along x)
  const auto lines = std::array<std::string_view, 12>{
    "camera 40 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 1144 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 2240 40 96 yaw 90.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 3416 192 96 yaw 180.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 4136 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 5312 344 96 yaw 270.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 6184 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 7208 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 8232 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 9256 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 10280 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720",
    "camera 11304 192 96 yaw 0.0000 pitch 0.0000 g_fov 90 viewport 1280 720"};
  const auto engineFov = engineCalcFov(90.0f, 1280, 720, 1.0f);
  const auto engineProjection =
    engineProjectionMatrix(engineFov.x, engineFov.y, 3.0f, 1280, 720);
  constexpr auto deg2rad = 3.14159265358979323846f / 180.0f;
  for (const auto line : lines)
  {
    CAPTURE(line);
    const auto parsed = parityCameraOfLine(line);
    REQUIRE(parsed);
    const auto& camera = parsed->camera;
    CHECK(parsed->gFov == 90);
    CHECK(camera.viewportWidth == 1280u);
    CHECK(camera.viewportHeight == 720u);
    // idAngles( 0, yaw, 0 ).ToMat3(): forward ( cos, sin, -0 ), up ( 0, 0, 1 )
    CHECK(camera.direction.x() == std::cos(parsed->yaw * deg2rad));
    CHECK(camera.direction.y() == std::sin(parsed->yaw * deg2rad));
    CHECK(camera.direction.z() == 0.0f);
    CHECK(camera.up == vm::vec3f{0, 0, 1});

    // the fov is the integer g_fov's, the projection the engine's
    const auto view = viewProjectionOf(camera);
    CHECK(view.gFov == 90.0f);
    CHECK(view.projectionMatrix == engineProjection);
    // the left axis is the engine's mat[ 1 ] ( -sy, cy, 0 )
    CHECK(view.viewAxis[3] == -camera.direction.y());
    CHECK(view.viewAxis[4] == camera.direction.x());

    // and back: the preview's own line names the same camera
    const auto again = parityCameraOfLine(parityCameraLine(camera, view));
    REQUIRE(again);
    CHECK(again->camera.position == camera.position);
    CHECK(again->gFov == 90);
    CHECK(again->camera.fovDegrees == camera.fovDegrees);
    const auto turn = std::fmod(std::abs(again->yaw - parsed->yaw), 360.0f);
    CHECK(std::min(turn, 360.0f - turn) < 1.0e-3f);
    CHECK(std::abs(again->pitch) < 1.0e-3f);
  }

  CHECK_FALSE(parityCameraOfLine("camera 1 2 3 yaw 0 pitch 0 g_fov 90 viewport 64"));
  CHECK_FALSE(parityCameraOfLine("camera 1 2 x yaw 0 pitch 0 g_fov 90 viewport 64 48"));
  CHECK_FALSE(parityCameraOfLine("camera 1 2 3 yaw 0 pitch 0 g_fov 0 viewport 64 48"));
}

} // namespace tb::lit
