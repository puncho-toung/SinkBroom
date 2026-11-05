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

// LP6.7 (C75, D183, P8.22): Intel's masked occlusion culling as the engine vendors it,
// through its facade (TbLitMoc): the implementation Create() picks on this machine, an
// occluder of idDrawVerts with 32-bit indexes rasterized as the engine's occluders are
// (BACKFACE_CCW, CLIP_PLANE_ALL, VertexLayout( sizeof( idDrawVert ), 4, 8 ) here), boxes
// and rects behind it and in front of it, the depth buffer read back, and the MXCSR's
// FTZ | DAZ set around the calls and given back.

#include "lit/EngineGeometry.h"
#include "lit/EngineMaskedOcclusion.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

constexpr auto BufferSize = 64u;

/**
 * A column-major model-to-clip matrix (the engine's transposed idRenderMatrix) that
 * looks down -z with a 90° field of view: clip x = x, y = y, w = -z (the library reads
 * x, y and w; 1 / w is its depth).
 */
// columns: x's (1, 0, 0, 0), y's (0, 1, 0, 0), z's (0, 0, 0, -1), the translation 0
constexpr auto ModelToClip =
  std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0};

// a box's eight corners (min, max) in clip space through ModelToClip, in the engine's
// corner order (engineMocZeroOneCubeVerts' layout), as (x, y, z, w) idVec4s
std::array<std::array<float, 4>, 8> clipBox(
  const std::array<float, 3>& lo, const std::array<float, 3>& hi)
{
  auto corners = std::array<std::array<float, 4>, 8>{};
  const auto unit = engineMocZeroOneCubeVerts();
  for (std::size_t i = 0; i < 8; ++i)
  {
    const auto x = unit[i][0] == 0.0f ? lo[0] : hi[0];
    const auto y = unit[i][1] == 0.0f ? lo[1] : hi[1];
    const auto z = unit[i][2] == 0.0f ? lo[2] : hi[2];
    corners[i] = {x, y, 0.0f, -z};
  }
  return corners;
}

} // namespace

TEST_CASE("EngineMaskedOcclusion")
{
  // the control bits (DAZ, the masks, the rounding, FTZ); bits 0-5 are the sticky
  // exception flags any float code may raise
  constexpr auto ControlBits = ~std::uint32_t{0x3f};
  const auto mxcsrBefore = engineMxcsr();

  // RenderSystem_init.cpp:2473-2476: Create() under FTZ | DAZ
  auto moc = std::unique_ptr<EngineMaskedOcclusion>{};
  {
    const auto scope = EngineMocFloatScope{};
    moc = EngineMaskedOcclusion::create();
  }
  REQUIRE(moc);
  CHECK((engineMxcsr() & ControlBits) == (mxcsrBefore & ControlBits));
  const auto implementation = moc->implementation();
  std::cout << "masked occlusion culling: Create() picked "
            << engineMocImplementationName(implementation) << " on this machine\n";
  CHECK(!engineMocImplementationName(implementation).empty());

  // the cubes the engine's tests transform (RenderSystem_init.cpp:2125-2236)
  SECTION("the cubes")
  {
    const auto zeroOne = engineMocZeroOneCubeVerts();
    const auto unit = engineMocUnitCubeVerts();
    CHECK(zeroOne[0] == std::array<float, 4>{0, 0, 0, 1});
    CHECK(zeroOne[6] == std::array<float, 4>{1, 1, 1, 1});
    CHECK(zeroOne[3] == std::array<float, 4>{0, 1, 0, 1});
    CHECK(unit[0] == std::array<float, 4>{-1, -1, -1, 1});
    CHECK(unit[5] == std::array<float, 4>{1, -1, 1, 1});
    const auto indexes = engineMocCubeIndexes();
    CHECK(indexes[0] == 2);
    CHECK(indexes[2] == 0);
    CHECK(indexes[33] == 5);
    CHECK(indexes[35] == 6);
  }

  SECTION("an occluder, the tests behind and in front of it, the depth read back")
  {
    // the library's calls between the scope's FTZ | DAZ and the register given back
    const auto mxcsrAtScope = engineMxcsr();
    {
      const auto scope = EngineMocFloatScope{};
      CHECK((engineMxcsr() & EngineMocFlushDenormals) == EngineMocFlushDenormals);

      moc->setResolution(BufferSize, BufferSize);
      auto width = 0u;
      auto height = 0u;
      moc->resolution(width, height);
      CHECK(width == BufferSize);
      CHECK(height == BufferSize);
      moc->setNearClipPlane(1.0f);
      moc->clearBuffer();

      // a quad of two triangles at z = -10 over the left half of the view (NDC x -2..0,
      // y -2..2), front-facing: clockwise on screen as the engine's front faces are
      auto verts = std::vector<EngineDrawVert>(4);
      verts[0].xyz = {-20.0f, -20.0f, -10.0f};
      verts[1].xyz = {-20.0f, 20.0f, -10.0f};
      verts[2].xyz = {0.0f, 20.0f, -10.0f};
      verts[3].xyz = {0.0f, -20.0f, -10.0f};
      const auto indexes = std::vector<unsigned int>{0, 1, 2, 0, 2, 3};
      const auto layout = EngineMocVertexLayout{int(sizeof(EngineDrawVert)), 4, 8};
      CHECK(
        moc->renderTriangles(
          verts.front().xyz.data(),
          indexes.data(),
          2,
          ModelToClip.data(),
          EngineMocBackface::Ccw,
          EngineMocClipAll,
          layout)
        == EngineMocResult::Visible);

      // the same quad wound the other way is back-facing under BACKFACE_CCW
      const auto reversed = std::vector<unsigned int>{0, 2, 1, 0, 3, 2};
      CHECK(
        moc->renderTriangles(
          verts.front().xyz.data(),
          reversed.data(),
          2,
          ModelToClip.data(),
          EngineMocBackface::Ccw,
          EngineMocClipAll,
          layout)
        == EngineMocResult::ViewCulled);

      // boxes, as the engine's light and surface tests give them: clip-space corners, no
      // matrix, the zero-to-one cube's triangles, BACKFACE_NONE, the default layout
      const auto cube = engineMocCubeIndexes();
      const auto test = [&](const std::array<std::array<float, 4>, 8>& corners) {
        return moc->testTriangles(
          corners.front().data(),
          cube.data(),
          12,
          nullptr,
          EngineMocBackface::None,
          EngineMocClipAll,
          EngineMocVertexLayout{});
      };
      const auto behind = clipBox({-15.0f, -5.0f, -30.0f}, {-5.0f, 5.0f, -20.0f});
      const auto inFront = clipBox({-2.0f, -1.0f, -5.0f}, {-1.0f, 1.0f, -3.0f});
      const auto beside = clipBox({5.0f, -5.0f, -30.0f}, {15.0f, 5.0f, -20.0f});
      CHECK(test(behind) == EngineMocResult::Occluded);
      CHECK(test(inFront) == EngineMocResult::Visible);
      CHECK(test(beside) == EngineMocResult::Visible);

      // the rects of the same boxes in NDC with their nearest w
      CHECK(
        moc->testRect(-0.75f, -0.25f, -0.25f, 0.25f, 20.0f) == EngineMocResult::Occluded);
      CHECK(moc->testRect(-0.6f, -0.3f, -0.2f, 0.3f, 3.0f) == EngineMocResult::Visible);
      CHECK(
        moc->testRect(0.25f, -0.25f, 0.75f, 0.25f, 20.0f) == EngineMocResult::Visible);
      // (TestRect pads its pixel box to whole 32 x 8 tiles before its VIEW_CULLED test,
      // MaskedOcclusionCullingCommon.inl:1789-1795, so it never answers VIEW_CULLED: a
      // rect outside the view tests the clamped edge tiles)

      // the depth (1 / w) of an occluded pixel and of a free one (ClearBuffer's -1,
      // "beyond infinity", :284-285)
      auto depth = std::vector<float>(std::size_t(BufferSize) * BufferSize);
      moc->computePixelDepthBuffer(depth.data(), false);
      const auto occluded = depth[std::size_t(32 * BufferSize + 16)];
      const auto free = depth[std::size_t(32 * BufferSize + 48)];
      std::cout << "masked occlusion culling: depth at (16, 32) " << occluded
                << ", at (48, 32) " << free << "\n";
      CHECK(occluded > 0.09f);
      CHECK(occluded <= 0.1f);
      CHECK(free == -1.0f);
    }
    // the register the scope found is back, flags and all
    CHECK(engineMxcsr() == mxcsrAtScope);
  }

  moc.reset();
  CHECK((engineMxcsr() & ControlBits) == (mxcsrBefore & ControlBits));
}

} // namespace tb::lit
