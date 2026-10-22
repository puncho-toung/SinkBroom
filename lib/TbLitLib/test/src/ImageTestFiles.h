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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// LP1c.2: image files written byte by byte for the loader and fallback tests, in the
// formats the copied loaders read. Every writer is SinkBroom's own.

namespace tb::lit::test
{

// RGBA8 pixels, top row first
struct Rgba
{
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;

  std::uint8_t at(int x, int y, int channel) const;
};

// a picture whose channels differ: r = 16x, g = 255 - 16y, b = 8(x + y), a = 255 - 8x
Rgba gradient(int width, int height);
// one colour
Rgba flat(
  int width,
  int height,
  std::uint8_t r,
  std::uint8_t g,
  std::uint8_t b,
  std::uint8_t a = 255);

// an uncompressed TGA (type 2), 24 or 32 bits, stored bottom row first as the engine's
// LoadTGA expects without the flip bit
std::string tgaFile(const Rgba& image, int pixelSize = 32);
// a grey TGA (type 3, 8 bits) of the red channel
std::string greyTgaFile(const Rgba& image);
// a run-length TGA (type 10), runs of equal pixels and raw packets otherwise
std::string rleTgaFile(const Rgba& image, int pixelSize = 32);

// a PNG of 8-bit RGBA (colour type 6) or RGB (2), deflated in stored blocks
std::string pngFile(const Rgba& image, bool alpha = true);

// a baseline 8-bit grey JPG of one 8×8 block per block of the declared size, every block
// flat grey 128 (one DC code, one EOB code); cut leaves the end-of-image marker off
std::string greyJpgFile(int width, int height, int scanBlocks = -1, bool cut = false);

// a Radiance HDR of RGBE pixels: flat for a width below 8, run-length scanlines otherwise
std::string hdrFile(int width, int height, const std::vector<float>& rgb);

// a single-part scanline OpenEXR of float R, G, B (uncompressed), rgb top row first
std::string exrFile(int width, int height, const std::vector<float>& rgb);

} // namespace tb::lit::test
