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

// ---- engine notice ----
// Parts of this file follow, altered for SinkBroom, code of the Doom 3 BFG Edition GPL
// Source Code and RBDOOM-3-BFG (neo/renderer/Image_files.cpp's LoadTGA) and of the
// libraries the engine vendors (neo/libs/stb/stb_image.h, public domain; the HDR and PNG
// header rules):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c.2 (D51, R5.3, C17, M-IMG-GUARD-01): the loader pre-validation. Every image file a
// copied loader reads is checked here first, outside the copied code, so that no loader
// reaches one of its Error sites or reads past its buffer: LoadTGA checks nothing against
// the file's length after its Error returns, and walks RLE packets unchecked; LoadHDR
// trusts stb_image to give three channels. A file that fails is not handed to the loader.
// Pure functions of the bytes; any thread (the file system calls them on the parse
// thread).

#include "lit/MaterialDecls.h"

// the declarations of the engine-vendored tinyexr, whose implementation the copied
// Image_files.cpp compiles (shim/src/ShimImageFiles.cpp)
#include "libs/tinyexr/tinyexr.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{
namespace
{

using Problem = std::optional<std::string>;

std::uint32_t le16(const std::string_view bytes, const std::size_t at)
{
  return std::uint32_t(std::uint8_t(bytes[at]))
         | (std::uint32_t(std::uint8_t(bytes[at + 1])) << 8);
}

std::uint32_t le32(const std::string_view bytes, const std::size_t at)
{
  return le16(bytes, at) | (le16(bytes, at + 2) << 16);
}

std::uint64_t le64(const std::string_view bytes, const std::size_t at)
{
  return std::uint64_t(le32(bytes, at)) | (std::uint64_t(le32(bytes, at + 4)) << 32);
}

std::uint32_t be16(const std::string_view bytes, const std::size_t at)
{
  return (std::uint32_t(std::uint8_t(bytes[at])) << 8) | std::uint8_t(bytes[at + 1]);
}

std::uint32_t be32(const std::string_view bytes, const std::size_t at)
{
  return (be16(bytes, at) << 16) | be16(bytes, at + 2);
}

Problem sizeProblem(const std::uint64_t width, const std::uint64_t height)
{
  if (width < 1 || height < 1)
  {
    return "it declares an empty image (" + std::to_string(width) + " x "
           + std::to_string(height) + ")";
  }
  if (
    width > std::uint64_t(MaxImageFileDimension)
    || height > std::uint64_t(MaxImageFileDimension)
    || width * height > std::uint64_t(MaxImageFilePixels))
  {
    return "it declares " + std::to_string(width) + " x " + std::to_string(height)
           + " pixels, more than the preview loads ("
           + std::to_string(MaxImageFileDimension) + " a side, "
           + std::to_string(MaxImageFilePixels) + " in all)";
  }
  return std::nullopt;
}

/**
 * Image_files.cpp:162-423 LoadTGA, every read it makes, without writing: the 18-byte
 * header, the sizes and types its Error sites refuse (M-IMG-FATAL-01..07), the declared
 * size of an uncompressed image against the file's length (its "incomplete file" check,
 * which only reports), and the RLE packets walked as the decoder walks them, each
 * packet's bytes inside the file, up to the last pixel (the decoder stops there, inside a
 * packet if need be).
 */
Problem tgaProblem(const std::string_view bytes)
{
  if (bytes.size() < 18)
  {
    return "it is shorter than a TGA header (" + std::to_string(bytes.size()) + " bytes)";
  }
  const auto idLength = std::size_t(std::uint8_t(bytes[0]));
  const auto colormapType = std::uint8_t(bytes[1]);
  const auto imageType = std::uint8_t(bytes[2]);
  const auto width = le16(bytes, 12);
  const auto height = le16(bytes, 14);
  const auto pixelSize = std::uint8_t(bytes[16]);

  if (imageType != 2 && imageType != 10 && imageType != 3)
  {
    return "its TGA type " + std::to_string(imageType)
           + " is not 2 (RGB), 3 (grey) or 10 (RGB, run-length encoded)";
  }
  if (colormapType != 0)
  {
    return "it is a colour-mapped TGA";
  }
  // the pixel sizes each decoder reads: the uncompressed one 8 (grey), 24 and 32, the RLE
  // one 24 and 32; the header check lets only a grey image through with another size
  const auto sizeOk = imageType == 3
                        ? (pixelSize == 8 || pixelSize == 24 || pixelSize == 32)
                        : (pixelSize == 24 || pixelSize == 32);
  if (!sizeOk)
  {
    return "its TGA pixel size " + std::to_string(pixelSize) + " is not "
           + (imageType == 3 ? "8, 24 or 32" : "24 or 32");
  }
  if (const auto problem = sizeProblem(width, height))
  {
    return problem;
  }

  const auto dataStart = std::size_t(18) + idLength;
  if (dataStart > bytes.size())
  {
    return "its TGA image comment runs past the end of the file";
  }
  const auto available = bytes.size() - dataStart;
  const auto bytesPerPixel = std::size_t(pixelSize >> 3);
  const auto pixels = std::size_t(width) * std::size_t(height);

  if (imageType == 2 || imageType == 3)
  {
    const auto needed = pixels * bytesPerPixel;
    if (needed > available)
    {
      return "it is cut short: its " + std::to_string(width) + " x "
             + std::to_string(height) + " pixels need " + std::to_string(needed)
             + " bytes, the file holds " + std::to_string(available);
    }
    return std::nullopt;
  }

  // type 10: the packets, as the decoder reads them
  auto at = dataStart;
  auto left = pixels;
  while (left > 0)
  {
    if (at >= bytes.size())
    {
      return "its run-length data ends with " + std::to_string(left)
             + " pixels still to come";
    }
    const auto header = std::uint8_t(bytes[at++]);
    const auto count = std::size_t(1 + (header & 0x7f));
    const auto used = std::min(count, left);
    const auto needed = (header & 0x80) ? bytesPerPixel : used * bytesPerPixel;
    if (needed > bytes.size() - at)
    {
      return "a run-length packet runs past the end of the file, with "
             + std::to_string(left) + " pixels still to come";
    }
    at += needed;
    left -= used;
  }
  return std::nullopt;
}

/**
 * stb_image.h's PNG header rules (stbi__parse_png_file: the signature, IHDR first and 13
 * bytes long, its depth, colour type, compression, filter and interlace methods, a PLTE
 * for a palette image, IDAT, an unknown critical chunk refused) and every chunk inside
 * the file up to IEND, so that a file cut short is refused rather than half decoded. The
 * declared pixels must fit what the IDAT bytes can inflate to: deflate makes at most
 * 1032 bytes of each (258-byte matches in 2 bits), so a header that lies about the size
 * is refused before stb_image allocates for it.
 */
Problem pngProblem(const std::string_view bytes)
{
  static constexpr unsigned char Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() < 8 || memcmp(bytes.data(), Signature, 8) != 0)
  {
    return std::string{"it has no PNG signature"};
  }

  auto at = std::size_t(8);
  auto first = true;
  auto width = std::uint32_t{0};
  auto height = std::uint32_t{0};
  auto channels = 0;
  auto depth = 0;
  auto palette = false;
  auto hasPalette = false;
  auto idatBytes = std::uint64_t{0};
  for (;;)
  {
    if (bytes.size() - at < 12)
    {
      return std::string{"it is cut short before its IEND chunk"};
    }
    const auto length = be32(bytes, at);
    const auto type = bytes.substr(at + 4, 4);
    if (length > (1u << 30) || length > bytes.size() - at - 12)
    {
      return "its " + std::string{type} + " chunk runs past the end of the file";
    }
    const auto data = bytes.substr(at + 8, length);
    at += 12 + std::size_t(length);

    if (first)
    {
      if (type != "IHDR" || length != 13)
      {
        return std::string{"its first chunk is not a 13-byte IHDR"};
      }
      first = false;
      width = be32(data, 0);
      height = be32(data, 4);
      depth = std::uint8_t(data[8]);
      const auto color = std::uint8_t(data[9]);
      if (const auto problem = sizeProblem(width, height))
      {
        return problem;
      }
      if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
      {
        return "its bit depth " + std::to_string(depth) + " is not 1, 2, 4, 8 or 16";
      }
      if (color > 6 || (color == 3 && depth == 16) || (color != 3 && (color & 1)))
      {
        return "its colour type " + std::to_string(color) + " with depth "
               + std::to_string(depth) + " is not one stb_image reads";
      }
      palette = color == 3;
      channels = color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
      if (data[10] != 0 || data[11] != 0 || std::uint8_t(data[12]) > 1)
      {
        return std::string{"its compression, filter or interlace method is unknown"};
      }
      continue;
    }

    if (type == "IHDR")
    {
      return std::string{"it has a second IHDR chunk"};
    }
    if (type == "PLTE")
    {
      hasPalette = true;
    }
    else if (type == "IDAT")
    {
      if (palette && !hasPalette)
      {
        return std::string{"its palette image has no PLTE chunk before its data"};
      }
      idatBytes += length;
    }
    else if (type == "IEND")
    {
      break;
    }
    else if (type != "tRNS" && type != "CgBI" && !(std::uint8_t(type[0]) & 32))
    {
      return "it has an unknown critical chunk " + std::string{type};
    }
  }

  if (idatBytes == 0)
  {
    return std::string{"it has no IDAT chunk"};
  }
  const auto rowBytes =
    (std::uint64_t(width) * std::uint64_t(channels) * std::uint64_t(depth) + 7) / 8;
  const auto imageBytes = rowBytes * height;
  if (imageBytes > idatBytes * 1032 + 1032)
  {
    return "its IHDR declares " + std::to_string(width) + " x " + std::to_string(height)
           + " pixels, which its " + std::to_string(idatBytes)
           + " bytes of IDAT data cannot hold";
  }
  return std::nullopt;
}

/**
 * stb_image.h's baseline and progressive JPEG headers (stbi__process_frame_header: 8-bit
 * samples, a height and width, 1, 3 or 4 components with sampling factors 1 to 4 and
 * quantisation tables 0 to 3) and every marker segment inside the file, each scan's
 * entropy-coded bytes walked to the next marker, up to EOI, so that a file cut short is
 * refused rather than decoded from zeros. Every 8×8 block of the most finely sampled
 * component costs at least one Huffman bit, so the declared size must fit the scan bytes.
 */
Problem jpgProblem(const std::string_view bytes)
{
  if (
    bytes.size() < 4 || std::uint8_t(bytes[0]) != 0xFF || std::uint8_t(bytes[1]) != 0xD8)
  {
    return std::string{"it has no JPEG start-of-image marker"};
  }

  auto at = std::size_t(2);
  auto frame = false;
  auto width = std::uint32_t{0};
  auto height = std::uint32_t{0};
  auto scanBytes = std::uint64_t{0};
  auto scans = 0;
  for (;;)
  {
    // fill bytes (0xFF) before a marker are skipped, as stb_image skips them
    if (at >= bytes.size() || std::uint8_t(bytes[at]) != 0xFF)
    {
      return std::string{"it is cut short or broken before its end-of-image marker"};
    }
    while (at < bytes.size() && std::uint8_t(bytes[at]) == 0xFF)
    {
      ++at;
    }
    if (at >= bytes.size())
    {
      return std::string{"it is cut short before its end-of-image marker"};
    }
    const auto marker = std::uint8_t(bytes[at++]);
    if (marker == 0xD9)
    {
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
    {
      continue;
    }
    if (bytes.size() - at < 2)
    {
      return std::string{"a marker segment is cut short"};
    }
    const auto length = std::size_t(be16(bytes, at));
    if (length < 2 || length > bytes.size() - at)
    {
      return std::string{"a marker segment runs past the end of the file"};
    }
    const auto segment = bytes.substr(at + 2, length - 2);
    at += length;

    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2)
    {
      if (frame)
      {
        return std::string{"it has a second frame header"};
      }
      frame = true;
      if (segment.size() < 6)
      {
        return std::string{"its frame header is cut short"};
      }
      if (std::uint8_t(segment[0]) != 8)
      {
        return "its samples have " + std::to_string(std::uint8_t(segment[0]))
               + " bits; stb_image reads 8";
      }
      height = be16(segment, 1);
      width = be16(segment, 3);
      if (const auto problem = sizeProblem(width, height))
      {
        return problem;
      }
      const auto components = std::size_t(std::uint8_t(segment[5]));
      if (components != 1 && components != 3 && components != 4)
      {
        return "it has " + std::to_string(components) + " components";
      }
      if (segment.size() != 6 + 3 * components)
      {
        return std::string{"its frame header has the wrong length"};
      }
      for (std::size_t c = 0; c < components; ++c)
      {
        const auto sampling = std::uint8_t(segment[6 + 3 * c + 1]);
        const auto h = sampling >> 4;
        const auto v = sampling & 15;
        if (h < 1 || h > 4 || v < 1 || v > 4 || std::uint8_t(segment[6 + 3 * c + 2]) > 3)
        {
          return std::string{"a component has a bad sampling factor or table"};
        }
      }
      continue;
    }
    if ((marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8
         && marker != 0xCC))
    {
      return std::string{"it is a lossless, hierarchical or arithmetic-coded JPEG"};
    }
    if (marker != 0xDA)
    {
      // tables, restart interval, comments, application data
      continue;
    }

    // a scan: its header, then its entropy-coded bytes up to the next marker
    if (!frame)
    {
      return std::string{"a scan comes before the frame header"};
    }
    const auto scanComponents = segment.empty() ? 0u : std::uint8_t(segment[0]);
    if (
      scanComponents < 1 || scanComponents > 4
      || segment.size() != 4 + 2 * scanComponents)
    {
      return std::string{"a scan header is malformed"};
    }
    ++scans;
    for (;;)
    {
      if (at >= bytes.size())
      {
        return std::string{"it is cut short inside a scan"};
      }
      if (std::uint8_t(bytes[at]) != 0xFF)
      {
        ++at;
        ++scanBytes;
        continue;
      }
      if (at + 1 >= bytes.size())
      {
        return std::string{"it is cut short inside a scan"};
      }
      const auto next = std::uint8_t(bytes[at + 1]);
      if (next == 0x00 || (next >= 0xD0 && next <= 0xD7))
      {
        // a stuffed 0xFF or a restart marker inside the scan
        at += 2;
        scanBytes += next == 0x00 ? 1 : 0;
        continue;
      }
      if (next == 0xFF)
      {
        ++at;
        continue;
      }
      break;
    }
  }

  if (!frame || scans == 0)
  {
    return std::string{"it has no frame or no scan"};
  }
  const auto blocks = std::uint64_t((width + 7) / 8) * std::uint64_t((height + 7) / 8);
  if (blocks > scanBytes * 8 + 8)
  {
    return "its frame header declares " + std::to_string(width) + " x "
           + std::to_string(height) + " pixels, which its " + std::to_string(scanBytes)
           + " bytes of scan data cannot hold";
  }
  return std::nullopt;
}

// stb_image.h:9447-9469 stbi__hdr_gettoken: a line up to '\n' or the end, cut at 1023
std::string hdrToken(const std::string_view bytes, std::size_t& at)
{
  auto token = std::string{};
  while (at < bytes.size())
  {
    const auto c = bytes[at++];
    if (c == '\n')
    {
      break;
    }
    if (token.size() < 1023)
    {
      token += c;
    }
  }
  return token;
}

/**
 * stb_image.h:9515-9721 stbi__hdr_load, every read it makes, without writing: the
 * Radiance identifier, the header up to its empty line with FORMAT=32-bit_rle_rgbe, the
 * "-Y <height> +X <width>" line, then the pixels: flat RGBE for a width below 8 or of
 * 32768 and more, else run-length scanlines (each "2 2 <length>" with the width, four
 * channels of runs and dumps that fill it exactly), and the flat fallback stb_image takes
 * at a scanline without that header. A file stb_image reads this way always gives three
 * channels, so LoadHDR's channel test (M-IMG-FATAL-09) cannot fail, and no read runs out.
 */
Problem hdrProblem(const std::string_view bytes)
{
  auto at = std::size_t(0);
  const auto magic = hdrToken(bytes, at);
  if (magic != "#?RADIANCE" && magic != "#?RGBE")
  {
    return std::string{"it is not a Radiance HDR file"};
  }
  auto format = false;
  for (;;)
  {
    if (at >= bytes.size())
    {
      return std::string{"its header has no end"};
    }
    const auto token = hdrToken(bytes, at);
    if (token.empty())
    {
      break;
    }
    format = format || token == "FORMAT=32-bit_rle_rgbe";
  }
  if (!format)
  {
    return std::string{"it is not in the 32-bit_rle_rgbe format"};
  }

  const auto resolution = hdrToken(bytes, at);
  if (resolution.compare(0, 3, "-Y ") != 0)
  {
    return std::string{"its resolution line is not \"-Y <height> +X <width>\""};
  }
  char* end = nullptr;
  const auto height = std::strtol(resolution.c_str() + 3, &end, 10);
  while (*end == ' ')
  {
    ++end;
  }
  if (strncmp(end, "+X ", 3) != 0)
  {
    return std::string{"its resolution line is not \"-Y <height> +X <width>\""};
  }
  const auto width = std::strtol(end + 3, nullptr, 10);
  if (width < 1 || height < 1)
  {
    return std::string{"it declares an empty image"};
  }
  if (const auto problem = sizeProblem(std::uint64_t(width), std::uint64_t(height)))
  {
    return problem;
  }

  const auto pixels = std::uint64_t(width) * std::uint64_t(height);
  const auto flat = [&](const std::uint64_t count) -> Problem {
    if (count * 4 > bytes.size() - at)
    {
      return std::string{"its pixels are cut short"};
    }
    return std::nullopt;
  };
  if (width < 8 || width >= 32768)
  {
    return flat(pixels);
  }
  for (long j = 0; j < height; ++j)
  {
    if (bytes.size() - at < 4)
    {
      return std::string{"a scanline is cut short"};
    }
    const auto c1 = std::uint8_t(bytes[at]);
    const auto c2 = std::uint8_t(bytes[at + 1]);
    const auto lengthHigh = std::uint8_t(bytes[at + 2]);
    if (c1 != 2 || c2 != 2 || (lengthHigh & 0x80))
    {
      // stb_image reads these four bytes as the first pixel and the rest of the image
      // flat
      at += 4;
      return flat(pixels - 1);
    }
    const auto length = (std::uint32_t(lengthHigh) << 8) | std::uint8_t(bytes[at + 3]);
    at += 4;
    if (length != std::uint32_t(width))
    {
      return std::string{"a scanline's length is not the image's width"};
    }
    for (auto k = 0; k < 4; ++k)
    {
      auto i = long{0};
      while (i < width)
      {
        if (at >= bytes.size())
        {
          return std::string{"a scanline is cut short"};
        }
        auto count = std::uint32_t(std::uint8_t(bytes[at++]));
        const auto run = count > 128;
        count -= run ? 128 : 0;
        if (count == 0 || long(count) > width - i)
        {
          return std::string{"a scanline's run-length data is corrupt"};
        }
        const auto needed = run ? std::size_t(1) : std::size_t(count);
        if (needed > bytes.size() - at)
        {
          return std::string{"a scanline is cut short"};
        }
        at += needed;
        i += long(count);
      }
    }
  }
  return std::nullopt;
}

// tinyexr's lines per scanline block, by compression (LoadEXRImageFromMemory)
int exrLinesPerBlock(const int compression)
{
  switch (compression)
  {
  case TINYEXR_COMPRESSIONTYPE_ZIP:
    return 16;
  case TINYEXR_COMPRESSIONTYPE_PIZ:
    return 32;
  default:
    return 1;
  }
}

/**
 * The OpenEXR header through tinyexr's own parsers (ParseEXRVersionFromMemory,
 * ParseEXRHeaderFromMemory), and what LoadEXRFromMemory then needs to succeed: a
 * single-part image, one channel or R, G and B, a data window inside the size limit; for
 * a scanline image the offset table and every block it points to inside the file, so a
 * file cut short is refused rather than reaching LoadEXR's error return
 * (M-IMG-FATAL-08). Tiled images are left to tinyexr's own offset checks.
 */
Problem exrProblem(const std::string_view bytes)
{
  const auto* memory = reinterpret_cast<const unsigned char*>(bytes.data());
  auto version = EXRVersion{};
  if (ParseEXRVersionFromMemory(&version, memory, bytes.size()) != TINYEXR_SUCCESS)
  {
    return std::string{"it is not an OpenEXR file"};
  }
  if (version.multipart || version.non_image)
  {
    return std::string{"it is a multi-part or deep OpenEXR file"};
  }

  auto header = EXRHeader{};
  InitEXRHeader(&header);
  const char* error = nullptr;
  if (
    ParseEXRHeaderFromMemory(&header, &version, memory, bytes.size(), &error)
    != TINYEXR_SUCCESS)
  {
    auto problem = std::string{"its OpenEXR header is malformed"};
    if (error)
    {
      problem += std::string{": "} + error;
      FreeEXRErrorMessage(error);
    }
    FreeEXRHeader(&header);
    return problem;
  }

  auto problem = Problem{};
  auto rgb = 0;
  for (int c = 0; c < header.num_channels; ++c)
  {
    const auto name = std::string_view{header.channels[c].name};
    rgb += (name == "R" || name == "G" || name == "B") ? 1 : 0;
  }
  const auto width =
    std::int64_t(header.data_window.max_x) - std::int64_t(header.data_window.min_x) + 1;
  const auto height =
    std::int64_t(header.data_window.max_y) - std::int64_t(header.data_window.min_y) + 1;
  if (header.num_channels != 1 && rgb != 3)
  {
    problem = "it has neither one channel nor R, G and B";
  }
  else if (width < 1 || height < 1)
  {
    problem = "its data window is empty";
  }
  else if (
    const auto sizeError = sizeProblem(std::uint64_t(width), std::uint64_t(height)))
  {
    problem = sizeError;
  }
  else if (!header.tiled)
  {
    const auto lines = exrLinesPerBlock(header.compression_type);
    const auto blocks = std::uint64_t(
      header.chunk_count > 0 ? header.chunk_count : (height + lines - 1) / lines);
    const auto table = std::uint64_t(8) + std::uint64_t(header.header_len);
    if (table + blocks * 8 > bytes.size())
    {
      problem = "its offset table is cut short";
    }
    for (std::uint64_t b = 0; !problem && b < blocks; ++b)
    {
      const auto offset = le64(bytes, std::size_t(table + b * 8));
      if (offset < table + blocks * 8 || offset > bytes.size() - 8)
      {
        problem = "a block lies outside the file";
      }
      else if (le32(bytes, std::size_t(offset + 4)) > bytes.size() - offset - 8)
      {
        problem = "a block is cut short";
      }
    }
  }
  FreeEXRHeader(&header);
  return problem;
}

std::string lowerExtension(const std::string_view path)
{
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string_view::npos || (slash != std::string_view::npos && slash > dot))
  {
    return {};
  }
  auto extension = std::string{path.substr(dot + 1)};
  for (auto& c : extension)
  {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return extension;
}

} // namespace

bool isImageLoaderFile(const std::string_view path)
{
  const auto extension = lowerExtension(path);
  return extension == "tga" || extension == "png" || extension == "jpg"
         || extension == "exr" || extension == "hdr";
}

std::optional<std::string> imageFileProblem(
  const std::string_view path, const std::string_view bytes)
{
  const auto extension = lowerExtension(path);
  if (!isImageLoaderFile(path))
  {
    return std::nullopt;
  }
  if (bytes.empty())
  {
    return std::string{"it is empty"};
  }
  // the imageLoaders table (Image_files.cpp:959-966): the extension picks the loader
  return extension == "tga"   ? tgaProblem(bytes)
         : extension == "png" ? pngProblem(bytes)
         : extension == "jpg" ? jpgProblem(bytes)
         : extension == "exr" ? exrProblem(bytes)
                              : hdrProblem(bytes);
}

} // namespace tb::lit
