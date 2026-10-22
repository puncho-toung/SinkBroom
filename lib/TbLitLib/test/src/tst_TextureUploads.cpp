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

// LP2.1 step 5 (SC265-SC268): an image's texture desc and its levels' upload bytes,
// without a device.

#include "TextureUploads.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"

#include "kd/result.h"

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// one level of width × height in format, its bytes as the cache holds them
CachedImage oneLevel(
  const int format, const int width, const int height, const std::size_t bytes)
{
  auto image = CachedImage{};
  image.textureType = Type2D;
  image.format = format;
  image.storedFormat = format;
  image.width = width;
  image.height = height;
  image.numLevels = 1;
  image.levels.push_back(CachedImageLevel{0, 0, width, height, 0, bytes});
  image.data.resize(bytes);
  return image;
}

} // namespace

TEST_CASE("TextureUploads")
{
  SECTION("levelUploadBytes: GetRowPitch's row times the rows writeTexture copies")
  {
    struct Case
    {
      int format = 0;
      int width = 0;
      int height = 0;
      std::uint64_t bytes = 0;
    };
    for (const auto& c : std::vector<Case>{
           // BC: rows of 4 × 4 blocks, a level below 4 one block (a cube's padded level)
           {FormatDxt1, 16, 8, 4 * 8 * 2},
           {FormatDxt1, 2, 2, 8},
           {FormatDxt5, 16, 16, 4 * 16 * 4},
           {FormatDxt5, 4, 4, 16},
           {FormatBc6h, 8, 4, 2 * 16},
           // uncompressed: width × the format's bytes, every row
           {FormatRgba8, 5, 3, 5 * 4 * 3},
           {FormatInt8, 64, 16, 64 * 16},
           {FormatR11g11b10f, 4, 2, 4 * 4 * 2},
           {FormatL8a8, 3, 3, 3 * 2 * 3},
         })
    {
      CAPTURE(c.format, c.width, c.height);
      const auto image = oneLevel(c.format, c.width, c.height, std::size_t(c.bytes));
      // the row is the copied GetRowPitch's
      const auto pitch = engineRowPitch(c.format, c.width);
      REQUIRE(pitch);
      const auto rows =
        (c.format == FormatDxt1 || c.format == FormatDxt5 || c.format == FormatBc6h)
          ? std::uint64_t(std::max(1, (c.height + 3) / 4))
          : std::uint64_t(c.height);
      CHECK(std::uint64_t(*pitch) * rows == c.bytes);
      CHECK(levelUploadBytes(image, image.levels[0]) == Result<std::uint64_t>{c.bytes});

      // a level that holds less than its rows is refused, never read past
      const auto cut = oneLevel(c.format, c.width, c.height, std::size_t(c.bytes - 1));
      CHECK(levelUploadBytes(cut, cut.levels[0]).is_error());
    }

    // T0.14: a format without a pitch
    const auto unknown = oneLevel(99, 4, 4, 64);
    CHECK(levelUploadBytes(unknown, unknown.levels[0]).is_error());
  }

  SECTION("engineTextureDesc: AllocImage's texture")
  {
    // a BC image's size in whole blocks; RGB565 arrives as RGBA8
    auto bc = oneLevel(FormatDxt5, 6, 10, 16 * 2 * 3);
    bc.colorFormat = ColorYCoCgDxt5;
    const auto bcDesc = engineTextureDesc(bc, true, "bc") | kdl::value();
    CHECK(bcDesc.width == 8u);
    CHECK(bcDesc.height == 12u);
    CHECK(bcDesc.format == nvrhi::Format::BC3_UNORM);
    CHECK(bcDesc.dimension == nvrhi::TextureDimension::Texture2D);
    CHECK(bcDesc.initialState == nvrhi::ResourceStates::ShaderResource);
    CHECK(bcDesc.keepInitialState);
    CHECK(bcDesc.mipLevels == 1u);

    // a cube: six slices; INT8's swizzle
    auto cube = oneLevel(FormatInt8, 4, 4, 16);
    cube.textureType = TypeCubic;
    const auto cubeDesc = engineTextureDesc(cube, true, "cube") | kdl::value();
    CHECK(cubeDesc.dimension == nvrhi::TextureDimension::TextureCube);
    CHECK(cubeDesc.arraySize == 6u);
    CHECK(cubeDesc.format == nvrhi::Format::R8_UNORM);
    CHECK(cubeDesc.componentMapping.a == nvrhi::ComponentSwizzle::Red);

    // the engine stops on BC7, and on an image without levels
    CHECK(engineTextureDesc(oneLevel(FormatBc7, 4, 4, 16), true, "bc7").is_error());
    auto empty = oneLevel(FormatRgba8, 4, 4, 64);
    empty.numLevels = 0;
    CHECK(engineTextureDesc(empty, true, "empty").is_error());
  }
}

} // namespace tb::lit
