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
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_lightgrid.cpp and
// renderer/RenderWorld_defs.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/LightGridFile.h"

#include "lit/EngineLexer.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace tb::lit
{
namespace
{

// big-endian reads (idFile::ReadBig), and the little-endian int32 length of ReadString,
// each checked against what is left
class BigReader
{
private:
  std::span<const std::byte> m_data;
  std::size_t m_offset = 0;

public:
  explicit BigReader(const std::span<const std::byte> data)
    : m_data{data}
  {
  }

  std::size_t left() const { return m_data.size() - m_offset; }

  std::optional<std::uint64_t> big(const std::size_t size)
  {
    if (size > left())
    {
      return std::nullopt;
    }
    auto value = std::uint64_t{0};
    for (std::size_t i = 0; i < size; ++i)
    {
      value = (value << 8) | std::uint8_t(m_data[m_offset + i]);
    }
    m_offset += size;
    return value;
  }

  std::optional<std::int32_t> int32()
  {
    const auto value = big(4);
    return value ? std::optional{std::int32_t(std::uint32_t(*value))} : std::nullopt;
  }

  std::optional<float> float32()
  {
    const auto value = big(4);
    if (!value)
    {
      return std::nullopt;
    }
    const auto bits = std::uint32_t(*value);
    auto f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  std::optional<std::array<float, 3>> vec3()
  {
    const auto x = float32();
    const auto y = float32();
    const auto z = float32();
    if (!z)
    {
      return std::nullopt;
    }
    return std::array<float, 3>{*x, *y, *z};
  }

  // idFile::ReadString: a little-endian int32 length, then the bytes
  std::optional<std::string> string()
  {
    if (left() < 4)
    {
      return std::nullopt;
    }
    auto length = std::int32_t{0};
    for (auto i = 3; i >= 0; --i)
    {
      length = std::int32_t(
        (std::uint32_t(length) << 8) | std::uint8_t(m_data[m_offset + std::size_t(i)]));
    }
    m_offset += 4;
    if (length < 0 || std::size_t(length) > left())
    {
      return std::nullopt;
    }
    auto text = std::string(std::size_t(length), '\0');
    std::memcpy(text.data(), m_data.data() + m_offset, text.size());
    m_offset += text.size();
    return text;
  }
};

Error gridError(const std::string_view what)
{
  return Error{fmt::format("The light grid cannot be read: {}", what)};
}

std::string lowerAscii(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  });
  return text;
}

// ReadBinaryLightGridPoints (:783-845)
Result<LightGridArea> readBinaryArea(BigReader& in, const int numAreas)
{
  auto area = LightGridArea{};
  const auto index = in.int32();
  if (!index || *index < 0 || *index >= numAreas)
  {
    return gridError(fmt::format("bad area index {}", index.value_or(-1)));
  }
  area.area = *index;
  const auto numPoints = in.int32();
  if (!numPoints || *numPoints < 0)
  {
    return gridError("bad numLightGridPoints");
  }
  const auto probeSize = in.int32();
  if (!probeSize || *probeSize < 0)
  {
    return gridError(fmt::format("bad imageProbeSize {}", probeSize.value_or(-1)));
  }
  const auto borderSize = in.int32();
  if (!borderSize || *borderSize < 0)
  {
    return gridError(fmt::format("bad imageBorderSize {}", borderSize.value_or(-1)));
  }
  area.probeSize = *probeSize;
  area.borderSize = *borderSize;

  // gridMins
  const auto origin = in.vec3();
  const auto size = in.vec3();
  const auto bx = in.int32();
  const auto by = in.int32();
  const auto bz = in.int32();
  if (!origin || !size || !bz)
  {
    return gridError("an area's header is cut short");
  }
  area.origin = *origin;
  area.size = *size;
  area.bounds = {*bx, *by, *bz};

  // a point takes 13 bytes
  if (std::size_t(*numPoints) > in.left() / 13)
  {
    return gridError("the points are cut short");
  }
  area.points.resize(std::size_t(*numPoints));
  for (auto& point : area.points)
  {
    point.valid = std::uint8_t(*in.big(1));
    point.origin = *in.vec3();
  }
  return area;
}

// a lexer failure as the engine's idLexer::Error reports it
Error lexError(const EngineLexer& lexer, const std::string_view what)
{
  return gridError(fmt::format("{} (line {})", what, lexer.line()));
}

// ParseLightGridPoints (:690-781)
Result<LightGridArea> readTextArea(EngineLexer& lexer, const int numAreas)
{
  auto area = LightGridArea{};
  if (!lexer.expectTokenString("{"))
  {
    return lexError(lexer, "expected {");
  }
  area.area = lexer.parseInt();
  if (area.area < 0 || area.area >= numAreas)
  {
    return lexError(
      lexer, fmt::format("ParseLightGridPoints: bad area index {}", area.area));
  }
  const auto numPoints = lexer.parseInt();
  if (numPoints < 0)
  {
    return lexError(lexer, "ParseLightGridPoints: bad numLightGridPoints");
  }
  area.probeSize = lexer.parseInt();
  if (area.probeSize < 8)
  {
    return lexError(
      lexer,
      fmt::format(
        "ParseLightGridPoints: bad single image probe size {}", area.probeSize));
  }
  area.borderSize = lexer.parseInt();
  if (area.borderSize < 0)
  {
    return lexError(
      lexer,
      fmt::format("ParseLightGridPoints: bad probe border size {}", area.borderSize));
  }

  // gridMins
  if (
    !lexer.parse1DMatrix(3, area.origin.data())
    || !lexer.parse1DMatrix(3, area.size.data()))
  {
    return lexError(lexer, "bad grid origin or size");
  }
  for (auto& bound : area.bounds)
  {
    bound = lexer.parseInt();
  }

  // LP1c fix (S3 Low): appended as they parse, so that a count the text cannot hold
  // ("lightGridPoints { 0 2000000000 ...") allocates nothing for it
  for (auto i = 0; i < numPoints; ++i)
  {
    auto point = LightGridPoint{};
    point.valid = std::uint8_t(lexer.parseInt());
    if (!lexer.parse1DMatrix(3, point.origin.data()))
    {
      return lexError(lexer, "bad grid point");
    }
    area.points.push_back(point);
  }
  if (!lexer.expectTokenString("}") || lexer.hadError())
  {
    return lexError(lexer, "an area does not end");
  }
  return area;
}

} // namespace

bool lightGridBinaryAccepted(
  const LightGridBinaryHeader& header, const std::int64_t textTime)
{
  return header.magic == LightGridBinaryMagic
         && (textTime == header.storedTime || textTime == 0) && header.numEntries > 0;
}

Result<LightGridBinaryHeader> readLightGridBinaryHeader(
  const std::span<const std::byte> data)
{
  auto in = BigReader{data};
  const auto magic = in.big(4);
  const auto time = in.big(8);
  const auto numEntries = in.int32();
  if (!numEntries)
  {
    return gridError("the header is cut short");
  }
  return LightGridBinaryHeader{
    std::uint32_t(*magic), std::int64_t(*time), int(*numEntries)};
}

Result<LightGrid> readLightGridBinary(
  const std::span<const std::byte> data, const int numAreas)
{
  return readLightGridBinaryHeader(data)
         | kdl::and_then([&](const LightGridBinaryHeader& header) -> Result<LightGrid> {
             auto in = BigReader{data};
             static_cast<void>(in.big(16));
             auto grid = LightGrid{};
             for (auto i = 0; i < header.numEntries; ++i)
             {
               const auto type = in.string();
               if (!type || lowerAscii(*type) != "lightgridpoints")
               {
                 return gridError(fmt::format(
                   "Binary proc file failed, unexpected type {}", type.value_or("")));
               }
               auto area = readBinaryArea(in, numAreas);
               if (area.is_error())
               {
                 return std::move(area)
                        | kdl::transform([](auto) { return LightGrid{}; });
               }
               grid.areas.push_back(std::move(area).value());
             }
             return grid;
           });
}

Result<LightGrid> readLightGridText(const std::string_view text, const int numAreas)
{
  auto lexer = EngineLexer{text, "maps/.lightgrid"};
  if (!lexer.expectTokenString("LGRID"))
  {
    return gridError("it is not a light grid file");
  }

  // the version: a number, or the quoted string the writer writes; "-" makes it negative
  auto token = std::string{};
  if (!lexer.readToken(token))
  {
    return lexError(lexer, "couldn't read expected integer");
  }
  auto version = 0;
  if (token == "-")
  {
    version = -lexer.parseInt();
  }
  else
  {
    version = std::atoi(token.c_str());
  }
  if (version != 3 && version != 4)
  {
    return gridError(fmt::format("version {} instead of 3", version));
  }

  auto grid = LightGrid{};
  while (lexer.readToken(token))
  {
    if (token != "lightGridPoints")
    {
      return lexError(
        lexer,
        fmt::format("idRenderWorldLocal::LoadLightGridFile: bad token \"{}\"", token));
    }
    auto area = readTextArea(lexer, numAreas);
    if (area.is_error())
    {
      return std::move(area) | kdl::transform([](auto) { return LightGrid{}; });
    }
    grid.areas.push_back(std::move(area).value());
  }
  if (lexer.hadError())
  {
    return lexError(lexer, "the lexer failed");
  }
  return grid;
}

Result<std::vector<ShProbe>> readShProbes(const std::string_view text)
{
  auto lexer = EngineLexer{text, "maps/.shprobes"};
  if (!lexer.expectTokenString("SHPROBES"))
  {
    return Error{"The SH probes cannot be read: no SHPROBES id"};
  }
  auto token = std::string{};
  if (!lexer.readToken(token) || token != "1")
  {
    return Error{
      fmt::format("The SH probes cannot be read: unsupported version '{}'", token)};
  }
  const auto numProbes = lexer.parseInt();
  if (numProbes < 0 || numProbes > 65536)
  {
    return Error{
      fmt::format("The SH probes cannot be read: bad probe count {}", numProbes)};
  }
  auto probes = std::vector<ShProbe>(std::size_t(numProbes));
  for (auto& probe : probes)
  {
    probe.area = lexer.parseInt();
    if (
      !lexer.parse1DMatrix(3, probe.origin.data())
      || !lexer.parse1DMatrix(27, probe.sh.data()))
    {
      return Error{
        fmt::format("The SH probes cannot be read: bad probe (line {})", lexer.line())};
    }
  }
  if (lexer.hadError())
  {
    return Error{fmt::format(
      "The SH probes cannot be read: the lexer failed (line {})", lexer.line())};
  }
  return probes;
}

MaterialImageKey lightGridImageKey(const std::string_view mapBase, const int area)
{
  auto key = MaterialImageKey{};
  key.name = fmt::format("env/{}/area{}_lightgrid_amb", mapBase, area);
  // TF_LINEAR, TR_CLAMP, TD_HDR_LIGHTPROBE, CF_2D
  key.filter = 0;
  key.repeat = 1;
  key.usage = 21;
  key.cube = 0;
  return key;
}

std::array<MaterialImageKey, 3> envProbeImageKeys(
  const std::string_view mapBase, const int area, const std::array<int, 3>& point)
{
  const auto nameOf = [&](const std::string_view kind) {
    auto name = fmt::format(
      "env/{}/area{}_envprobe_{}_{}_{}_{}",
      mapBase,
      area,
      point[0],
      point[1],
      point[2],
      kind);
    std::replace(name.begin(), name.end(), '-', '_');
    return name;
  };
  auto amb = MaterialImageKey{};
  amb.name = nameOf("amb");
  amb.filter = 0;
  amb.repeat = 1;
  amb.usage = 21;
  amb.cube = 5;
  auto spec = MaterialImageKey{};
  spec.name = nameOf("spec");
  spec.filter = 3;
  spec.repeat = 1;
  spec.usage = 21;
  spec.cube = 8;
  auto depth = MaterialImageKey{};
  depth.name = nameOf("depth");
  depth.filter = 0;
  depth.repeat = 1;
  depth.usage = 22;
  depth.cube = 8;
  return {amb, spec, depth};
}

} // namespace tb::lit
