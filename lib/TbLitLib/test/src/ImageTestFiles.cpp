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

#include "ImageTestFiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tb::lit::test
{
namespace
{

void put8(std::string& out, const unsigned value)
{
  out.push_back(char(value & 0xFF));
}

void putLe16(std::string& out, const unsigned value)
{
  put8(out, value);
  put8(out, value >> 8);
}

void putLe32(std::string& out, const std::uint32_t value)
{
  putLe16(out, value & 0xFFFF);
  putLe16(out, value >> 16);
}

void putLe64(std::string& out, const std::uint64_t value)
{
  putLe32(out, std::uint32_t(value & 0xFFFFFFFFu));
  putLe32(out, std::uint32_t(value >> 32));
}

void putBe16(std::string& out, const unsigned value)
{
  put8(out, value >> 8);
  put8(out, value);
}

void putBe32(std::string& out, const std::uint32_t value)
{
  putBe16(out, value >> 16);
  putBe16(out, value & 0xFFFF);
}

void putFloat(std::string& out, const float value)
{
  auto bits = std::uint32_t{0};
  std::memcpy(&bits, &value, 4);
  putLe32(out, bits);
}

std::string tgaHeader(const Rgba& image, const int type, const int pixelSize)
{
  auto out = std::string{};
  put8(out, 0);
  put8(out, 0);
  put8(out, unsigned(type));
  out.append(5, '\0');
  putLe16(out, 0);
  putLe16(out, 0);
  putLe16(out, unsigned(image.width));
  putLe16(out, unsigned(image.height));
  put8(out, unsigned(pixelSize));
  put8(out, 0);
  return out;
}

// the pixels in a TGA's order: bottom row first, BGR(A)
std::vector<std::string> tgaPixels(const Rgba& image, const int pixelSize)
{
  auto pixels = std::vector<std::string>{};
  for (auto y = image.height - 1; y >= 0; --y)
  {
    for (auto x = 0; x < image.width; ++x)
    {
      auto pixel = std::string{};
      put8(pixel, image.at(x, y, 2));
      put8(pixel, image.at(x, y, 1));
      put8(pixel, image.at(x, y, 0));
      if (pixelSize == 32)
      {
        put8(pixel, image.at(x, y, 3));
      }
      pixels.push_back(pixel);
    }
  }
  return pixels;
}

std::uint32_t crc32(const std::string& bytes)
{
  auto crc = 0xFFFFFFFFu;
  for (const auto c : bytes)
  {
    crc ^= std::uint8_t(c);
    for (auto k = 0; k < 8; ++k)
    {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

void pngChunk(std::string& out, const std::string& type, const std::string& data)
{
  putBe32(out, std::uint32_t(data.size()));
  out += type;
  out += data;
  putBe32(out, crc32(type + data));
}

} // namespace

std::uint8_t Rgba::at(const int x, const int y, const int channel) const
{
  return pixels[std::size_t((y * width + x) * 4 + channel)];
}

Rgba gradient(const int width, const int height)
{
  auto image = Rgba{width, height, {}};
  for (auto y = 0; y < height; ++y)
  {
    for (auto x = 0; x < width; ++x)
    {
      image.pixels.push_back(std::uint8_t((16 * x) & 0xFF));
      image.pixels.push_back(std::uint8_t((255 - 16 * y) & 0xFF));
      image.pixels.push_back(std::uint8_t((8 * (x + y)) & 0xFF));
      image.pixels.push_back(std::uint8_t((255 - 8 * x) & 0xFF));
    }
  }
  return image;
}

Rgba flat(
  const int width,
  const int height,
  const std::uint8_t r,
  const std::uint8_t g,
  const std::uint8_t b,
  const std::uint8_t a)
{
  auto image = Rgba{width, height, {}};
  for (auto i = 0; i < width * height; ++i)
  {
    image.pixels.insert(image.pixels.end(), {r, g, b, a});
  }
  return image;
}

std::string tgaFile(const Rgba& image, const int pixelSize)
{
  auto out = tgaHeader(image, 2, pixelSize);
  for (const auto& pixel : tgaPixels(image, pixelSize))
  {
    out += pixel;
  }
  return out;
}

std::string greyTgaFile(const Rgba& image)
{
  auto out = tgaHeader(image, 3, 8);
  for (auto y = image.height - 1; y >= 0; --y)
  {
    for (auto x = 0; x < image.width; ++x)
    {
      put8(out, image.at(x, y, 0));
    }
  }
  return out;
}

std::string rleTgaFile(const Rgba& image, const int pixelSize)
{
  auto out = tgaHeader(image, 10, pixelSize);
  const auto pixels = tgaPixels(image, pixelSize);
  auto i = std::size_t{0};
  while (i < pixels.size())
  {
    auto run = std::size_t{1};
    while (i + run < pixels.size() && run < 128 && pixels[i + run] == pixels[i])
    {
      ++run;
    }
    if (run > 1)
    {
      put8(out, unsigned(0x80 | (run - 1)));
      out += pixels[i];
      i += run;
      continue;
    }
    auto raw = std::size_t{1};
    while (i + raw < pixels.size() && raw < 128
           && (i + raw + 1 >= pixels.size() || pixels[i + raw] != pixels[i + raw + 1]))
    {
      ++raw;
    }
    put8(out, unsigned(raw - 1));
    for (auto k = std::size_t{0}; k < raw; ++k)
    {
      out += pixels[i + k];
    }
    i += raw;
  }
  return out;
}

std::string pngFile(const Rgba& image, const bool alpha)
{
  auto raw = std::string{};
  for (auto y = 0; y < image.height; ++y)
  {
    put8(raw, 0);
    for (auto x = 0; x < image.width; ++x)
    {
      for (auto c = 0; c < (alpha ? 4 : 3); ++c)
      {
        put8(raw, image.at(x, y, c));
      }
    }
  }

  // zlib: stored deflate blocks, then the Adler-32
  auto zlib = std::string{"\x78\x01"};
  auto at = std::size_t{0};
  do
  {
    const auto size = std::min<std::size_t>(65535, raw.size() - at);
    put8(zlib, at + size == raw.size() ? 1 : 0);
    putLe16(zlib, unsigned(size));
    putLe16(zlib, unsigned(~size & 0xFFFF));
    zlib += raw.substr(at, size);
    at += size;
  } while (at < raw.size());
  auto a = 1u;
  auto b = 0u;
  for (const auto c : raw)
  {
    a = (a + std::uint8_t(c)) % 65521u;
    b = (b + a) % 65521u;
  }
  putBe32(zlib, (b << 16) | a);

  auto out = std::string{"\x89PNG\r\n\x1a\n", 8};
  auto header = std::string{};
  putBe32(header, std::uint32_t(image.width));
  putBe32(header, std::uint32_t(image.height));
  put8(header, 8);
  put8(header, alpha ? 6 : 2);
  header.append(3, '\0');
  pngChunk(out, "IHDR", header);
  pngChunk(out, "IDAT", zlib);
  pngChunk(out, "IEND", {});
  return out;
}

std::string greyJpgFile(
  const int width, const int height, const int scanBlocks, const bool cut)
{
  auto out = std::string{"\xFF\xD8", 2};
  // DQT: one table of ones
  out += std::string{"\xFF\xDB\x00\x43\x00", 5};
  out.append(64, '\x01');
  // SOF0: 8-bit, one component, 1×1 sampling, table 0
  out += std::string{"\xFF\xC0\x00\x0B\x08", 5};
  putBe16(out, unsigned(height));
  putBe16(out, unsigned(width));
  out += std::string{"\x01\x01\x11\x00", 4};
  // DHT: DC table 0 and AC table 0, each one code of length 1 (category 0, EOB)
  for (const auto tableClass : {0x00, 0x10})
  {
    out += std::string{"\xFF\xC4\x00\x14", 4};
    put8(out, unsigned(tableClass));
    put8(out, 1);
    out.append(15, '\0');
    put8(out, 0);
  }
  // SOS
  out += std::string{"\xFF\xDA\x00\x08\x01\x01\x00\x00\x3F\x00", 10};
  // every block: DC difference 0 ('0') and EOB ('0'), padded with ones
  const auto blocks =
    scanBlocks >= 0 ? scanBlocks : ((width + 7) / 8) * ((height + 7) / 8);
  const auto bits = 2 * blocks;
  for (auto i = 0; i < bits / 8; ++i)
  {
    put8(out, 0x00);
  }
  if (bits % 8 != 0)
  {
    put8(out, 0xFFu >> (bits % 8));
  }
  if (!cut)
  {
    out += std::string{"\xFF\xD9", 2};
  }
  return out;
}

std::string hdrFile(const int width, const int height, const std::vector<float>& rgb)
{
  auto out = std::string{"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y "};
  out += std::to_string(height) + " +X " + std::to_string(width) + "\n";
  const auto rgbe = [&](const int i) {
    const auto r = rgb[std::size_t(3 * i)];
    const auto g = rgb[std::size_t(3 * i + 1)];
    const auto b = rgb[std::size_t(3 * i + 2)];
    const auto v = std::max({r, g, b});
    if (v < 1e-32f)
    {
      return std::array<std::uint8_t, 4>{0, 0, 0, 0};
    }
    auto e = 0;
    const auto scale = float(std::frexp(v, &e) * 256.0 / v);
    return std::array<std::uint8_t, 4>{
      std::uint8_t(r * scale),
      std::uint8_t(g * scale),
      std::uint8_t(b * scale),
      std::uint8_t(e + 128)};
  };
  if (width < 8)
  {
    for (auto i = 0; i < width * height; ++i)
    {
      for (const auto c : rgbe(i))
      {
        put8(out, c);
      }
    }
    return out;
  }
  for (auto y = 0; y < height; ++y)
  {
    put8(out, 2);
    put8(out, 2);
    putBe16(out, unsigned(width));
    for (auto k = 0; k < 4; ++k)
    {
      for (auto x = 0; x < width; x += 128)
      {
        const auto count = std::min(128, width - x);
        put8(out, unsigned(count));
        for (auto i = 0; i < count; ++i)
        {
          put8(out, rgbe(y * width + x + i)[std::size_t(k)]);
        }
      }
    }
  }
  return out;
}

std::string exrFile(const int width, const int height, const std::vector<float>& rgb)
{
  auto out = std::string{"\x76\x2f\x31\x01\x02\x00\x00\x00", 8};
  const auto attribute =
    [&](const std::string& name, const std::string& type, const std::string& value) {
      out += name;
      out.push_back('\0');
      out += type;
      out.push_back('\0');
      putLe32(out, std::uint32_t(value.size()));
      out += value;
    };

  auto channels = std::string{};
  for (const auto* name : {"B", "G", "R"})
  {
    channels += name;
    channels.push_back('\0');
    putLe32(channels, 2);
    channels.append(4, '\0');
    putLe32(channels, 1);
    putLe32(channels, 1);
  }
  channels.push_back('\0');
  auto box = std::string{};
  putLe32(box, 0);
  putLe32(box, 0);
  putLe32(box, std::uint32_t(width - 1));
  putLe32(box, std::uint32_t(height - 1));
  auto one = std::string{};
  putFloat(one, 1.0f);
  auto center = std::string{};
  putFloat(center, 0.0f);
  putFloat(center, 0.0f);

  attribute("channels", "chlist", channels);
  attribute("compression", "compression", std::string(1, '\0'));
  attribute("dataWindow", "box2i", box);
  attribute("displayWindow", "box2i", box);
  attribute("lineOrder", "lineOrder", std::string(1, '\0'));
  attribute("pixelAspectRatio", "float", one);
  attribute("screenWindowCenter", "v2f", center);
  attribute("screenWindowWidth", "float", one);
  out.push_back('\0');

  // one scanline per block (no compression)
  const auto lineBytes = std::uint32_t(width * 3 * 4);
  const auto tableEnd = out.size() + std::size_t(height) * 8;
  for (auto y = 0; y < height; ++y)
  {
    putLe64(out, tableEnd + std::size_t(y) * (8 + lineBytes));
  }
  for (auto y = 0; y < height; ++y)
  {
    putLe32(out, std::uint32_t(y));
    putLe32(out, lineBytes);
    for (const auto channel : {2, 1, 0})
    {
      for (auto x = 0; x < width; ++x)
      {
        putFloat(out, rgb[std::size_t(3 * (y * width + x) + channel)]);
      }
    }
  }
  return out;
}

} // namespace tb::lit::test
