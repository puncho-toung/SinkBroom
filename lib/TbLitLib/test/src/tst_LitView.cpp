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

} // namespace tb::lit
