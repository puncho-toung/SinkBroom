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
#include "lit/EngineGeometry.h"
#include "lit/ProcFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tb::lit
{

// one surface of a binary render model (Model.cpp LoadBinaryModel)
struct BprocSurface
{
  int id = 0;
  std::string material;
  bool geometry = false;
  std::array<EngineVec3, 2> bounds = {};
  bool generateNormals = false;
  bool tangentsCalculated = false;
  bool perfectHull = false;
  bool referencedIndexes = false;
  int numVerts = 0;
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
  std::vector<std::uint16_t> silIndexes;
  std::vector<int> mirroredVerts;
  std::vector<int> dupVerts;
  bool dominantTris = false;
};

struct BprocModel
{
  std::string name;
  std::int64_t timeStamp = 0;
  std::vector<BprocSurface> surfaces;
};

/**
 * Model.cpp:386-623 LoadBinaryModel's stream at offset, BRM magic 108 (BFG) or 110 (with
 * the masked occlusion data), which has no length prefix (A9, SC72): big-endian numbers
 * (idFile::ReadBig), little-endian floats of ReadVec3, ReadVec4 and ReadFloat, strings as
 * a little-endian int32 length and the bytes. offset moves past the stream. Every count
 * is checked against the bytes left. Pure; any thread. LP1c.1's .bproc reader reuses it.
 */
Result<BprocModel> readBinaryModel(std::span<const std::byte> data, std::size_t& offset);

// a .bproc (RenderWorld_load.cpp:764-998 InitFromMap's binary branch, version 3)
struct BprocFile
{
  std::uint32_t magic = 0;
  std::string mapName;
  std::int64_t timeStamp = 0;
  std::vector<BprocModel> models;
  // the portals and nodes; its models stay empty
  ProcTree areas;
};

// the whole file: its header, then each entry ("model", "interareaportals", "nodes");
// pure; any thread
Result<BprocFile> readBprocFile(std::span<const std::byte> data);

} // namespace tb::lit
