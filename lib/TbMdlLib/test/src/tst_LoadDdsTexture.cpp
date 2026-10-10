/*
 Copyright (C) 2023 iOrange
 Copyright (C) 2023 Kristian Duske

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

#include "TestEnvironment.h"
#include "fs/DiskFileSystem.h"
#include "gl/Texture.h"
#include "mdl/CatchConfig.h"
#include "mdl/LoadDdsTexture.h"
#include "mdl/Palette.h"
#include "mdl/TestUtils.h"

#include "kd/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

auto loadTexture(const std::string& name)
{
  const auto ddsPath = getFixtureRoot() / "test/mdl/LoadDdsTexture/";
  auto diskFS = fs::DiskFileSystem{ddsPath};

  const auto file = diskFS.openFile(name) | kdl::value();
  auto reader = file->reader().buffer();
  return loadDdsTexture(reader) | kdl::value();
}

void assertTexture(
  const std::string& name,
  const size_t width,
  const size_t height,
  const GLenum format,
  const img::ImageAlphaDomain alphaDomain)
{
  const auto texture = loadTexture(name);

  CHECK(texture.width() == width);
  CHECK(texture.height() == height);
  CHECK(texture.format() == format);
  CHECK(texture.alphaDomain() == alphaDomain);
}

} // namespace

TEST_CASE("loadDdsTexture")
{
  SECTION("isDdsTexture")
  {
    CHECK(isDdsTexture("texture.dds"));
    CHECK(isDdsTexture("texture.DDS"));
    CHECK(isDdsTexture("path/to/texture.dds"));
    CHECK(!isDdsTexture("texture.d"));
    CHECK(!isDdsTexture("texture"));
  }

  assertTexture("dds_rgb.dds", 128, 128, GL_BGR, img::ImageAlphaDomain::Opaque);
  // dds_rgba.dds has genuine per-pixel alpha (verified by inspecting its pixel data).
  assertTexture("dds_rgba.dds", 128, 128, GL_BGRA, img::ImageAlphaDomain::Graduated);
  // DXT1 is treated as Opaque and DXT3/DXT5 as Graduated without decoding compressed
  // blocks -- see the comment on classifyDdsAlphaDomain in LoadDdsTexture.cpp.
  assertTexture(
    "dds_bc1.dds",
    128,
    128,
    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
    img::ImageAlphaDomain::Opaque);
  assertTexture(
    "dds_bc2.dds",
    128,
    128,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
    img::ImageAlphaDomain::Graduated);
  assertTexture(
    "dds_bc3.dds",
    128,
    128,
    GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
    img::ImageAlphaDomain::Graduated);

  SECTION("DXT1 with sizes that aren't multiples of 4")
  {
    // 20x12 with the full mip chain 20x12, 10x6, 5x3, 2x1, 1x1; every block of a level
    // has that level's colour, so a level read with too few blocks shifts the levels
    // after it
    const auto texture = loadTexture("dds_bc1_npot.dds");
    CHECK(texture.width() == 20);
    CHECK(texture.height() == 12);
    CHECK(texture.format() == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);

    const auto& buffers = texture.buffersIfLoaded();
    REQUIRE(buffers.size() == 5);

    // 8 bytes per 4x4 block, partial blocks included
    CHECK(buffers[0].size() == 8 * 5 * 3);
    CHECK(buffers[1].size() == 8 * 3 * 2);
    CHECK(buffers[2].size() == 8 * 2 * 1);
    CHECK(buffers[3].size() == 8 * 1 * 1);
    CHECK(buffers[4].size() == 8 * 1 * 1);

    const auto firstColor = [&](const size_t level) {
      const auto* data = buffers[level].data();
      return uint16_t(data[0] | (data[1] << 8));
    };
    CHECK(firstColor(0) == 0xF800);
    CHECK(firstColor(1) == 0x07E0);
    CHECK(firstColor(2) == 0x001F);
    CHECK(firstColor(3) == 0xFFE0);
    CHECK(firstColor(4) == 0x8410);
  }
}

} // namespace tb::mdl
