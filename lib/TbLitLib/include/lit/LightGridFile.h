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

#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// RenderWorld_local.h:63-69 lightGridPoint_t, as the files hold it (no SH data:
// STORE_LIGHTGRID_SHDATA is 0)
struct LightGridPoint
{
  // not in the void
  std::uint8_t valid = 0;
  std::array<float, 3> origin = {0, 0, 0};

  bool operator==(const LightGridPoint&) const = default;
};

// one "lightGridPoints" entry: an area's grid
struct LightGridArea
{
  int area = -1;
  // imageSingleProbeSize (the border included) and imageBorderSize
  int probeSize = 0;
  int borderSize = 0;
  // gridMins, the grid's spacing and its point counts per axis
  std::array<float, 3> origin = {0, 0, 0};
  std::array<float, 3> size = {0, 0, 0};
  std::array<int, 3> bounds = {0, 0, 0};
  std::vector<LightGridPoint> points;

  bool operator==(const LightGridArea&) const = default;
};

// a map's light grid: its entries in the file's order
struct LightGrid
{
  std::vector<LightGridArea> areas;

  bool operator==(const LightGrid&) const = default;
};

// RenderWorld_lightgrid.cpp:48-49: 'P' 'R' 'O' and BLGRID_VERSION 4, big-endian
constexpr std::uint32_t LightGridBinaryMagic = (std::uint32_t('P') << 24)
                                               | (std::uint32_t('R') << 16)
                                               | (std::uint32_t('O') << 8) | 4u;

// a .blightgrid's header (LoadLightGridFile's first three reads)
struct LightGridBinaryHeader
{
  std::uint32_t magic = 0;
  // the .lightgrid's time it was made from (Unix seconds, A4)
  std::int64_t storedTime = 0;
  int numEntries = 0;
};

/**
 * C10, A2 (RenderWorld_lightgrid.cpp:548-577): whether LoadLightGridFile takes the
 * binary: its magic is PRO 4, the text file's time (GetTimestamp: -1 when there is none,
 * 0 in a container, else Unix seconds) equals the stored time or is 0, and it has
 * entries. So a missing text file rejects every binary the engine writes. Pure.
 */
bool lightGridBinaryAccepted(const LightGridBinaryHeader& header, std::int64_t textTime);

/**
 * RenderWorld_lightgrid.cpp:548-570, 783-845 ReadBinaryLightGridPoints: the header, then
 * per entry the type string (a little-endian int32 length and the bytes)
 * "lightGridPoints" (any case), the area (0 to numAreas - 1), the point count, probe and
 * border sizes (none negative), the origin and size (three big-endian floats each), the
 * bounds (three big-endian ints), and per point a valid byte and three big-endian floats.
 * Every count is checked against the bytes left; where the engine stops at an
 * idLib::Error, this fails. Pure; any thread.
 */
Result<LightGridBinaryHeader> readLightGridBinaryHeader(std::span<const std::byte> data);
Result<LightGrid> readLightGridBinary(std::span<const std::byte> data, int numAreas);

/**
 * RenderWorld_lightgrid.cpp:578-781 LoadLightGridFile's text branch and
 * ParseLightGridPoints, with the copied idLexer (LEXFL_NOSTRINGCONCAT |
 * LEXFL_NODOLLARPRECOMPILE): "LGRID", the version (3 or 4; the writer quotes it), then
 * "lightGridPoints" entries: { area (0 to numAreas - 1) count probeSize (8 or more)
 * borderSize (0 or more) ( origin ) ( size ) bounds×3, per point valid ( x y z ) }. A
 * failure is the engine's idLexer::Error or Warning. Parse thread (C20: the copied
 * lexer).
 */
Result<LightGrid> readLightGridText(std::string_view text, int numAreas);

// one probe of a .shprobes file (RenderWorld_defs.cpp:743-797 shProbeEntry_t)
struct ShProbe
{
  int area = -1;
  std::array<float, 3> origin = {0, 0, 0};
  // L2 SH irradiance, idVec3[9]
  std::array<float, 27> sh = {};

  bool operator==(const ShProbe&) const = default;
};

/**
 * LoadSHProbesFile: "SHPROBES", version "1", a count (0 to 65536), then per probe its
 * area, ( origin ) and ( 27 floats ). Optional (SC105): retail ships none. Parse thread
 * (C20).
 */
Result<std::vector<ShProbe>> readShProbes(std::string_view text);

/**
 * SC105, RenderWorld_lightgrid.cpp:415-447: the image key of an area's light grid
 * irradiance atlas, "env/<map base>/area<N>_lightgrid_amb", TF_LINEAR, TR_CLAMP,
 * TD_HDR_LIGHTPROBE, CF_2D (cache #__2100). mapBase is the map name without its
 * extension ("maps/admin"). Pure.
 */
MaterialImageKey lightGridImageKey(std::string_view mapBase, int area);

/**
 * SC105, RenderWorld_defs.cpp:840-867 R_DeriveEnvprobeData: the three image keys of an
 * environment probe at a point (snapped to integers, as SnapInt does) in an area:
 * "env/<map base>/area<N>_envprobe_<x>_<y>_<z>_{amb,spec,depth}" with every '-' of the
 * name made '_': amb TF_LINEAR CF_2D_PACKED_MIPCHAIN (#__2105), spec TF_DEFAULT
 * CF_NATIVE_HDR
 * (#__2108), depth TD_HDRI CF_NATIVE_HDR (#__2208), all TR_CLAMP. Pure.
 */
std::array<MaterialImageKey, 3> envProbeImageKeys(
  std::string_view mapBase, int area, const std::array<int, 3>& point);

} // namespace tb::lit
