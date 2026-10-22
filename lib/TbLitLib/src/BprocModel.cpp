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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.cpp, renderer/RenderWorld_load.cpp
// and framework/File.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/BprocModel.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace tb::lit
{
namespace
{

// Model.cpp:47-52, RenderWorld_load.cpp:808-816
constexpr std::uint32_t magicOf(
  const char a, const char b, const char c, const std::uint8_t version)
{
  return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8)
         | version;
}
constexpr auto BrmMagicBfg = magicOf('B', 'R', 'M', 108);
constexpr auto BrmMagic = magicOf('B', 'R', 'M', 110);
constexpr auto BprocMagic = magicOf('P', 'R', 'O', 3);

class Reader
{
private:
  std::span<const std::byte> m_data;
  std::size_t& m_offset;
  bool m_failed = false;

public:
  Reader(const std::span<const std::byte> data, std::size_t& offset)
    : m_data{data}
    , m_offset{offset}
  {
  }

  bool failed() const { return m_failed; }

  std::size_t left() const { return m_data.size() - std::min(m_offset, m_data.size()); }

  bool take(void* out, const std::size_t size)
  {
    if (m_failed || size > left())
    {
      m_failed = true;
      std::memset(out, 0, size);
      return false;
    }
    std::memcpy(out, m_data.data() + m_offset, size);
    m_offset += size;
    return true;
  }

  // idFile::ReadBig: big endian
  template <typename T>
  T big()
  {
    auto bytes = std::array<std::uint8_t, sizeof(T)>{};
    take(bytes.data(), bytes.size());
    std::reverse(bytes.begin(), bytes.end());
    auto value = T{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
  }

  // ReadFloat, ReadVec3, ReadInt: little endian
  template <typename T>
  T little()
  {
    auto value = T{};
    take(&value, sizeof(T));
    return value;
  }

  bool boolean() { return big<std::uint8_t>() != 0; }

  EngineVec3 vec3()
  {
    const auto x = little<float>();
    const auto y = little<float>();
    const auto z = little<float>();
    return {x, y, z};
  }

  // idFile::ReadString: a little-endian int32 length, then the bytes
  std::string string()
  {
    const auto length = little<std::int32_t>();
    if (length < 0 || std::size_t(length) > left())
    {
      m_failed = true;
      return {};
    }
    auto text = std::string(std::size_t(length), '\0');
    take(text.data(), text.size());
    return text;
  }

  // a count of elements of elementBytes each that the rest of the data can hold
  std::optional<std::size_t> count(const std::int32_t n, const std::size_t elementBytes)
  {
    if (n < 0 || std::size_t(n) * std::max<std::size_t>(elementBytes, 1) > left())
    {
      m_failed = true;
      return std::nullopt;
    }
    return std::size_t(n);
  }

  void skip(const std::size_t bytes)
  {
    if (bytes > left())
    {
      m_failed = true;
      return;
    }
    m_offset += bytes;
  }
};

Error streamError(const std::string_view what, const std::size_t offset)
{
  return Error{
    fmt::format("The binary model cannot be read: {} (byte {})", what, offset)};
}

Result<BprocSurface> readSurface(Reader& in, const std::uint32_t magic)
{
  auto surface = BprocSurface{};
  surface.id = in.big<std::int32_t>();
  surface.material = in.string();
  surface.geometry = in.boolean();
  if (!surface.geometry)
  {
    return surface;
  }
  const auto min = in.vec3();
  const auto max = in.vec3();
  surface.bounds = {min, max};
  if (magic == BrmMagicBfg)
  {
    // ambientViewCount
    in.skip(4);
  }
  surface.generateNormals = in.boolean();
  surface.tangentsCalculated = in.boolean();
  surface.perfectHull = in.boolean();
  surface.referencedIndexes = in.boolean();
  surface.numVerts = in.big<std::int32_t>();
  if (const auto inFile = in.count(in.big<std::int32_t>(), 32); inFile && *inFile > 0)
  {
    surface.verts.resize(*inFile);
    for (auto& vert : surface.verts)
    {
      vert.xyz = in.vec3();
      const auto s = in.big<std::uint16_t>();
      const auto t = in.big<std::uint16_t>();
      vert.st = {s, t};
      in.take(vert.normal.data(), 4);
      in.take(vert.tangent.data(), 4);
      in.take(vert.color.data(), 4);
      in.take(vert.color2.data(), 4);
    }
  }
  if (magic == BrmMagicBfg)
  {
    // the pre-light shadow vertexes, idVec4 each
    if (const auto n = in.count(in.big<std::int32_t>(), 16))
    {
      in.skip(*n * 16);
    }
  }
  const auto numIndexes = in.count(in.big<std::int32_t>(), 2);
  if (!numIndexes)
  {
    return Error{"bad numIndexes"};
  }
  surface.indexes.resize(*numIndexes);
  for (auto& index : surface.indexes)
  {
    index = in.big<std::uint16_t>();
  }
  if (in.big<std::int32_t>() > 0)
  {
    surface.silIndexes.resize(*numIndexes);
    for (auto& index : surface.silIndexes)
    {
      index = in.big<std::uint16_t>();
    }
  }
  if (const auto n = in.count(in.big<std::int32_t>(), 4))
  {
    surface.mirroredVerts.resize(*n);
    for (auto& v : surface.mirroredVerts)
    {
      v = in.big<std::int32_t>();
    }
  }
  if (const auto n = in.count(in.big<std::int32_t>(), 8))
  {
    surface.dupVerts.resize(*n * 2);
    for (auto& v : surface.dupVerts)
    {
      v = in.big<std::int32_t>();
    }
  }
  if (magic == BrmMagicBfg)
  {
    // the silhouette edges, four triIndex_t each
    if (const auto n = in.count(in.big<std::int32_t>(), 8))
    {
      in.skip(*n * 8);
    }
  }
  surface.dominantTris = in.boolean();
  if (surface.dominantTris)
  {
    // v2, v3 (big triIndex_t) and three little floats per vertex
    if (const auto n = in.count(surface.numVerts, 16))
    {
      in.skip(*n * 16);
    }
  }
  if (magic == BrmMagicBfg)
  {
    in.skip(12);
  }
  if (magic == BrmMagic)
  {
    // the masked occlusion vertexes (idVec4) and indexes (uint32)
    if (const auto n = in.count(in.big<std::int32_t>(), 16))
    {
      in.skip(*n * 16);
    }
    if (const auto n = in.count(in.big<std::int32_t>(), 4))
    {
      in.skip(*n * 4);
    }
  }
  if (in.failed())
  {
    return Error{"the surface is cut short"};
  }
  return surface;
}

} // namespace

Result<BprocModel> readBinaryModel(
  const std::span<const std::byte> data, std::size_t& offset)
{
  auto in = Reader{data, offset};
  auto model = BprocModel{};
  const auto magic = in.big<std::uint32_t>();
  if (magic != BrmMagicBfg && magic != BrmMagic)
  {
    return streamError(fmt::format("bad magic {:08x}", magic), offset);
  }
  model.timeStamp = in.big<std::int64_t>();
  const auto numSurfaces = in.count(in.big<std::int32_t>(), 1);
  if (!numSurfaces)
  {
    return streamError("bad numSurfaces", offset);
  }
  // R1.7: the surfaces through the pipeline, a surface's own error kept
  auto read = Result<void>{kdl::void_success};
  for (auto i = std::size_t{0}; i < *numSurfaces && read.is_success(); ++i)
  {
    read = readSurface(in, magic) | kdl::transform([&](BprocSurface surface) {
             model.surfaces.push_back(std::move(surface));
           })
           | kdl::or_else([&](Error e) -> Result<void> {
               return streamError(fmt::format("surface {}: {}", i, e.msg), offset);
             });
  }
  return std::move(read) | kdl::and_then([&]() -> Result<BprocModel> {
           // the model's bounds, then overlaysAdded, lastModifiedFrame,
           // lastArchivedFrame, the name and nine bools
           in.skip(24 + 12);
           model.name = in.string();
           in.skip(9);
           if (in.failed())
           {
             return streamError("the model is cut short", offset);
           }
           return std::move(model);
         });
}

Result<BprocFile> readBprocFile(const std::span<const std::byte> data)
{
  auto offset = std::size_t{0};
  auto in = Reader{data, offset};
  auto file = BprocFile{};
  file.magic = in.big<std::uint32_t>();
  if (file.magic != BprocMagic)
  {
    return Error{fmt::format("The .bproc cannot be read: magic {:08x}", file.magic)};
  }
  const auto numEntries = in.big<std::int32_t>();
  file.mapName = in.string();
  file.timeStamp = in.big<std::int64_t>();
  // R1.7: the models through the pipeline; the first error ends the read
  auto read = Result<void>{kdl::void_success};
  for (auto i = 0; i < numEntries && !in.failed() && read.is_success(); ++i)
  {
    auto type = in.string();
    std::transform(type.begin(), type.end(), type.begin(), [](const char c) {
      return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    });
    if (type == "model")
    {
      // RenderWorld_load.cpp:128-141 ReadBinaryModel: the name, then the stream
      const auto name = in.string();
      read = readBinaryModel(data, offset) | kdl::transform([&](BprocModel loaded) {
               loaded.name = name;
               file.models.push_back(std::move(loaded));
             });
    }
    else if (type == "interareaportals")
    {
      // :483-545 ReadBinaryAreaPortals
      file.areas.numAreas = in.big<std::int32_t>();
      const auto numPortals = in.count(in.big<std::int32_t>(), 12);
      for (auto p = std::size_t{0}; numPortals && p < *numPortals; ++p)
      {
        auto portal = ProcPortal{};
        const auto numPoints = in.count(in.big<std::int32_t>(), 12);
        portal.positiveArea = in.big<std::int32_t>();
        portal.negativeArea = in.big<std::int32_t>();
        for (auto k = std::size_t{0}; numPoints && k < *numPoints; ++k)
        {
          const auto x = in.big<float>();
          const auto y = in.big<float>();
          const auto z = in.big<float>();
          portal.points.push_back({x, y, z});
        }
        file.areas.portals.push_back(std::move(portal));
      }
    }
    else if (type == "nodes")
    {
      // :606-620 ReadBinaryNodes
      const auto numNodes = in.count(in.big<std::int32_t>(), 24);
      for (auto n = std::size_t{0}; numNodes && n < *numNodes; ++n)
      {
        auto node = ProcNode{};
        for (auto& c : node.plane)
        {
          c = in.big<float>();
        }
        const auto front = in.big<std::int32_t>();
        const auto back = in.big<std::int32_t>();
        node.children = {front, back};
        file.areas.nodes.push_back(node);
      }
    }
    else
    {
      read = Error{fmt::format("The .bproc cannot be read: unexpected type {}", type)};
    }
  }
  return std::move(read) | kdl::and_then([&]() -> Result<BprocFile> {
           if (in.failed())
           {
             return Error{"The .bproc cannot be read: it is cut short"};
           }
           return std::move(file);
         });
}

} // namespace tb::lit
