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

#include "lit/EngineGeometry.h"
#include "lit/MaterialDecls.h"

#include <cmath>
#include <future>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

EngineDrawVert vertAt(
  const float x, const float y, const float z, const float s, const float t)
{
  auto v = engineClearedVert();
  v.xyz = {x, y, z};
  engineSetTexCoord(v, s, t);
  engineSetNormal(v, {0, 0, 1});
  return v;
}

} // namespace

TEST_CASE("EngineGeometry: the idDrawVert encoders")
{
  // F32toF16 truncates: 1/3 has no exact half, the next half up is not taken
  const auto third = engineF16toF32(engineF32toF16(1.0f / 3.0f));
  CHECK(third < 1.0f / 3.0f);
  CHECK(engineF16toF32(engineF32toF16(0.5f)) == 0.5f);
  // below 2^-14 flushes to zero, beyond the range clamps to 0x7BFF
  CHECK(engineF32toF16(0.00001f) == 0);
  CHECK(engineF32toF16(1.0e6f) == 0x7BFF);

  auto v = engineClearedVert();
  CHECK(v.color == std::array<std::uint8_t, 4>{255, 255, 255, 255});
  CHECK(v.color2 == std::array<std::uint8_t, 4>{0, 0, 0, 0});
  engineSetNormal(v, {0, 0, 1});
  CHECK(v.normal[0] == 128);
  CHECK(v.normal[2] == 255);
  const auto n = engineNormal(v);
  CHECK(std::abs(n[2] - 1.0f) < 0.001f);
}

TEST_CASE("EngineGeometry: Valve texture coordinates and WriteFloat")
{
  // GetTextureVectors multiplies by 1 / scale; TriListForSide divides by the image size
  const auto texture = EngineValveTexture{{1, 0, 0}, 16, {0, -1, 0}, 8, 0.5f, 0.5f};
  const auto st = engineValveTexCoord({8, -128, 0}, texture, 64, 32);
  CHECK(st[0] == (8.0f * (1.0f / 0.5f) + 16.0f) / 64.0f);
  CHECK(st[1] == (128.0f * (1.0f / 0.5f) + 8.0f) / 32.0f);

  CHECK(engineProcFloatText(8.0f) == "8");
  CHECK(engineProcFloatText(-16.0004f) == "-16");
  CHECK(engineProcFloatText(0.5f) == "0.5");
  CHECK(engineProcFloatText(8.25f) == "8.25");
  CHECK(engineProcFloatText(1.0f / 255.0f) == "0.0039215689");
}

TEST_CASE("EngineGeometry: idWinding::Split")
{
  // a floor quad from x = -16 to 16, wound clockwise seen from above (the engine's order)
  const auto quad =
    std::vector<EngineVec3>{{-16, -16, 0}, {-16, 16, 0}, {16, 16, 0}, {16, -16, 0}};
  const auto plane = engineWindingPlane(quad);
  REQUIRE(plane);
  CHECK(plane->c == 1.0f);

  SECTION("a crossing plane cuts it in two")
  {
    const auto split = engineSplitWinding(quad, EnginePlane{1, 0, 0, 0}, 0.1f);
    REQUIRE(split);
    CHECK(split->side == EngineSplitSide::Cross);
    CHECK(split->front.size() == 4u);
    CHECK(split->back.size() == 4u);
  }

  SECTION("on the plane, it goes to the side its own normal agrees with")
  {
    const auto same = engineSplitWinding(quad, *plane, 0.1f);
    REQUIRE(same);
    CHECK(same->side == EngineSplitSide::Front);
    CHECK(same->front.size() == 4u);
    CHECK(same->back.empty());

    const auto flipped = engineSplitWinding(quad, engineFlippedPlane(*plane), 0.1f);
    REQUIRE(flipped);
    CHECK(flipped->side == EngineSplitSide::Back);
    CHECK(flipped->front.empty());
    CHECK(flipped->back.size() == 4u);
  }

  SECTION("the bound refuses an oversized winding")
  {
    auto many = std::vector<EngineVec3>(
      std::size_t(engineBounds::MaxWindingPoints + 1), EngineVec3{0, 0, 0});
    CHECK_FALSE(engineSplitWinding(many, EnginePlane{1, 0, 0, 0}, 0.1f));
  }
}

TEST_CASE("EngineGeometry: dmap's side plane and plane identity")
{
  // TB's three points of a +z face at z = 0
  const auto plane = engineSidePlane({272, 144, 0}, {272, 145, 0}, {273, 144, 0});
  REQUIRE(plane);
  CHECK(plane->c == 1.0f);
  CHECK(plane->d == 0.0f);
  CHECK(enginePlanesEqual(*plane, EnginePlane{0, 0, 1, 0.005f}, 0.00001f, 0.01f));
  CHECK_FALSE(enginePlanesEqual(*plane, EnginePlane{0, 0, 1, 0.02f}, 0.00001f, 0.01f));
  CHECK(enginePlanesEqual(
    engineFlippedPlane(*plane), EnginePlane{0, 0, -1, 0}, 0.00001f, 0.01f));
  CHECK_FALSE(engineSidePlane({0, 0, 0}, {1, 0, 0}, {2, 0, 0}));
}

TEST_CASE("EngineGeometry: patches")
{
  auto patch = EnginePatch{};
  patch.width = 3;
  patch.height = 3;
  for (auto i = 0; i < 3; ++i)
  {
    for (auto j = 0; j < 3; ++j)
    {
      // [i·width + j]: i the column, j the row
      patch.controlPoints.push_back(vertAt(
        float(j) * 32,
        j == 1 ? 16.0f : 0.0f,
        float(i) * 32,
        float(j) * 0.5f,
        float(i) * 0.5f));
    }
  }

  SECTION("patchDef2 subdivides by error, triangles rotated")
  {
    const auto surface = engineSubdividePatch(patch);
    REQUIRE(surface);
    CHECK(surface->verts.size() > 9u);
    CHECK(surface->indexes.size() % 3 == 0u);
  }

  SECTION("patchDef3 subdivides explicitly")
  {
    patch.explicitSubdivisions = true;
    patch.horizontalSubdivisions = 3;
    patch.verticalSubdivisions = 3;
    const auto surface = engineSubdividePatch(patch);
    REQUIRE(surface);
    CHECK(surface->verts.size() == 16u);
    CHECK(surface->indexes.size() == 54u);
  }

  SECTION("K19: patches beyond the bounds are refused")
  {
    auto error = std::string{};
    patch.explicitSubdivisions = true;
    patch.horizontalSubdivisions = 65;
    patch.verticalSubdivisions = 1;
    CHECK_FALSE(engineSubdividePatch(patch, &error));
    CHECK_FALSE(error.empty());

    auto huge = EnginePatch{};
    huge.width = 101;
    huge.height = 3;
    huge.controlPoints.resize(303);
    CHECK_FALSE(engineSubdividePatch(huge, &error));
  }
}

TEST_CASE("EngineGeometry: triangle cleanup and tangents")
{
  // two triangles of a quad, and a degenerate one
  auto verts = std::vector<EngineDrawVert>{
    vertAt(0, 0, 0, 0, 0),
    vertAt(0, 32, 0, 0, 1),
    vertAt(32, 32, 0, 1, 1),
    vertAt(32, 0, 0, 1, 0),
  };
  const auto indexes = std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3, 0, 0, 1};

  SECTION("without normals: degenerates removed, tangents set")
  {
    const auto result =
      engineCleanupTriangles(verts, indexes, EngineTangentPath::WithoutNormals);
    REQUIRE(result);
    CHECK(result->indexes.size() == 6u);
    // s grows with x: the tangent is +x
    const auto tangent = engineTangent(result->verts[0]);
    CHECK(tangent[0] > 0.99f);
    CHECK(int(result->verts[0].tangent[0]) == 255);
    // the pool's thread drains what the copy reported
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  }

  SECTION("unsmoothed: dominant triangles derive the normals too")
  {
    const auto result =
      engineCleanupTriangles(verts, indexes, EngineTangentPath::Unsmoothed);
    REQUIRE(result);
    const auto normal = engineNormal(result->verts[0]);
    CHECK(std::abs(std::abs(normal[2]) - 1.0f) < 0.01f);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  }

  SECTION("an index out of range is refused")
  {
    CHECK_FALSE(engineCleanupTriangles(
      verts, std::vector<std::uint16_t>{0, 1, 9}, EngineTangentPath::WithoutNormals));
  }

  SECTION("reversed copies flip the normals and the winding")
  {
    const auto reversed =
      engineReverseTriangles(verts, std::vector<std::uint16_t>{0, 1, 2});
    CHECK(reversed.indexes == std::vector<std::uint16_t>{1, 0, 2});
    CHECK(engineNormal(reversed.verts[0])[2] < -0.99f);
  }

  SECTION("C24: two threads run the copies at once")
  {
    const auto run = [&] {
      auto count = 0;
      for (auto i = 0; i < 200; ++i)
      {
        if (engineCleanupTriangles(verts, indexes, EngineTangentPath::WithoutNormals))
        {
          ++count;
        }
      }
      static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
      return count;
    };
    auto a = std::async(std::launch::async, run);
    auto b = std::async(std::launch::async, run);
    CHECK(a.get() == 200);
    CHECK(b.get() == 200);
  }
}

} // namespace tb::lit
