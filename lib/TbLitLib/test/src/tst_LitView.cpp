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

// LP4.0 (C32.1, C32.4, P4.16): the view's idScreenRect, its frustum planes and its
// normalized device coordinates, as the portal flow reads them.

#include "LitView.h"

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("LitView: idScreenRect's AddPoint and the rest (C32.4, P4.16)")
{
  SECTION("AddPoint truncates toward zero (idMath::Ftoi)")
  {
    auto rect = ScreenRect{};
    rect.clear();
    CHECK(rect.isEmpty());
    rect.addPoint(10.7f, 20.2f);
    CHECK(rect.x1 == 10);
    CHECK(rect.x2 == 10);
    CHECK(rect.y1 == 20);
    CHECK(rect.y2 == 20);
    CHECK_FALSE(rect.isEmpty());
    CHECK(rect.area() == 1);
    rect.addPoint(3.9f, 25.99f);
    CHECK(rect.x1 == 3);
    CHECK(rect.x2 == 10);
    CHECK(rect.y1 == 20);
    CHECK(rect.y2 == 25);
    CHECK(rect.area() == 8 * 6);

    // negatives: -0.5 is 0, -1.5 is -1 (not the floor)
    auto negative = ScreenRect{};
    negative.clear();
    negative.addPoint(-0.5f, -1.5f);
    CHECK(negative.x1 == 0);
    CHECK(negative.x2 == 0);
    CHECK(negative.y1 == -1);
    CHECK(negative.y2 == -1);
  }

  SECTION("Expand after AddPoint: a pixel each way")
  {
    auto rect = ScreenRect{};
    rect.clear();
    rect.addPoint(5.5f, 6.5f);
    rect.addPoint(9.0f, 7.25f);
    rect.expand();
    CHECK(rect.x1 == 4);
    CHECK(rect.y1 == 5);
    CHECK(rect.x2 == 10);
    CHECK(rect.y2 == 8);
    // Clear's z: zmin 0, zmax 1, untouched by the rest
    CHECK(rect.zmin == 0.0f);
    CHECK(rect.zmax == 1.0f);
  }

  SECTION("the int to short store keeps the low 16 bits, as the engine's does")
  {
    auto rect = ScreenRect{};
    rect.clear();
    // 40000 > x2 (-32000) is stored as 40000 - 65536; 40000 < x1 (32000) is not
    rect.addPoint(40000.0f, 0.0f);
    CHECK(rect.x2 == -25536);
    CHECK(rect.x1 == 32000);
  }

  SECTION("Intersect, Union and IsEmpty")
  {
    auto a = ScreenRect{};
    a.clear();
    a.addPoint(0.0f, 0.0f);
    a.addPoint(10.0f, 10.0f);
    auto b = ScreenRect{};
    b.clear();
    b.addPoint(5.0f, -5.0f);
    b.addPoint(20.0f, 5.0f);
    auto both = a;
    both.intersect(b);
    CHECK(both.x1 == 5);
    CHECK(both.y1 == 0);
    CHECK(both.x2 == 10);
    CHECK(both.y2 == 5);
    auto either = a;
    either.unite(b);
    CHECK(either.x1 == 0);
    CHECK(either.y1 == -5);
    CHECK(either.x2 == 20);
    CHECK(either.y2 == 10);
    // a cleared rect unites to the other and intersects to empty
    auto cleared = ScreenRect{};
    cleared.clear();
    auto united = cleared;
    united.unite(a);
    CHECK(united.equals(a));
    auto cut = a;
    cut.intersect(cleared);
    CHECK(cut.isEmpty());
    // IsEmpty: x1 > x2 or y1 > y2, one row or column is not empty
    auto line = ScreenRect{};
    line.clear();
    line.addPoint(3.0f, 4.0f);
    line.addPoint(3.0f, 9.0f);
    CHECK_FALSE(line.isEmpty());
  }
}

namespace
{

// LP2.2a's fixture camera (tst_LitPasses.cpp): at the west room's end looking east,
// 128 × 96, TrenchBroom's fov 90: fov_x 90, fov_y 2 · atan( 0.75 ) = 73.74
LitView fixtureView()
{
  auto camera = CameraState{};
  camera.position = vm::vec3f{-240, 0, 64};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  return litViewOf(camera, 128, 96);
}

bool closeTo(const float a, const float b, const float tolerance)
{
  return std::abs(a - b) <= tolerance;
}

bool planeNear(const EnginePlane& p, const EnginePlane& q, const float dTolerance)
{
  return closeTo(p.a, q.a, 1e-5f) && closeTo(p.b, q.b, 1e-5f) && closeTo(p.c, q.c, 1e-5f)
         && closeTo(p.d, q.d, dTolerance);
}

} // namespace

TEST_CASE("LitView: the portal flow's frustum planes and NDC (C32.1, C32.4)")
{
  const auto view = fixtureView();
  const auto planes = engineViewFrustumPlanes(view);
  const auto h = 0.70710677f;
  // left (screen left is +y) and right: through the eye (-240 0 64) at 45 degrees,
  // pointing out; d = -( n · eye ) = -240 · h
  CHECK(planeNear(planes[0], EnginePlane{-h, h, 0, -240.0f * h}, 1e-3f));
  CHECK(planeNear(planes[1], EnginePlane{-h, -h, 0, -240.0f * h}, 1e-3f));
  // bottom and top: tan( 73.74 / 2 ) = 0.75, so normals ( -0.6 0 -+0.8 ); d = -( n ·
  // eye ): -( 144 - 51.2 ) and -( 144 + 51.2 )
  CHECK(planeNear(planes[2], EnginePlane{-0.6f, 0, -0.8f, -92.8f}, 1e-3f));
  CHECK(planeNear(planes[3], EnginePlane{-0.6f, 0, 0.8f, -195.2f}, 1e-3f));
  // near: row 3 + row 2 of the mvp with [2][2] = -0.999 and [3][2] = -zNear is the
  // plane zNear / 1.999 ahead of the eye (clip z > -w of a 0..1 range), x = -238.49925;
  // negated, then d - 3: the eye plane's d (-240) + 1.50075 - 3, 1.49925 behind the eye
  CHECK(
    planeNear(planes[4], EnginePlane{-1, 0, 0, -240.0f + 3.0f / 1.999f - 3.0f}, 1e-3f));
  // far: row 3 - row 2 = ( 0 0 -0.001 3 ) of the infinite projection: the plane z_eye =
  // 3000, 3000 behind the eye (the flow takes the first five planes only)
  CHECK(closeTo(planes[5].a, -1.0f, 1e-6f));
  CHECK(closeTo(planes[5].d, -240.0f - 3000.0f, 0.1f));

  // the sign convention: a point in view is on the negative side of every plane
  const auto ahead = EngineVec3{-140, 0, 64};
  for (const auto& plane : planes)
  {
    CHECK(enginePlaneDistance(plane, ahead) < 0.0f);
  }
  // one behind the eye is outside the near plane, one left of the frustum outside the
  // left plane
  CHECK(enginePlaneDistance(planes[4], EngineVec3{-245, 0, 64}) > 0.0f);
  CHECK(enginePlaneDistance(planes[0], EngineVec3{-140, 150, 64}) > 0.0f);

  // NDC: the point 100 ahead is the centre, z = ( 0.999 · 100 - 3 ) / 100
  const auto centre = engineGlobalToNdc(view, ahead);
  CHECK(closeTo(centre[0], 0.0f, 1e-6f));
  CHECK(closeTo(centre[1], 0.0f, 1e-6f));
  CHECK(closeTo(centre[2], 0.969f, 1e-6f));
  // 25 to the left (+y) and 10 up, 100 ahead: x = -25 / 100 (P00 = 1 for fov_x 90),
  // y = 10 / ( 100 · 0.75 )
  const auto off = engineGlobalToNdc(view, EngineVec3{-140, 25, 74});
  CHECK(closeTo(off[0], -0.25f, 1e-6f));
  CHECK(closeTo(off[1], 10.0f / 75.0f, 1e-6f));
  CHECK(closeTo(off[2], 0.969f, 1e-6f));
}

} // namespace tb::lit
