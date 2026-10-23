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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_load.cpp,
// renderer/RenderWorld.cpp, renderer/RenderWorld.h and renderer/tr_trisurf.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ProcFile.h"

#include "lit/EngineGeometry.h"
#include "lit/EngineLexer.h"
#include "lit/NumberText.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

namespace tb::lit
{
namespace
{

// RenderWorld.h:44-51
constexpr auto ProcFileId = std::string_view{"mapProcFile004"};
constexpr auto ProcFileId003 = std::string_view{"mapProcFile003"};
constexpr auto FloatsBasic = 8;
constexpr auto FloatsColor = 12;

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             return std::tolower(static_cast<unsigned char>(a))
                    == std::tolower(static_cast<unsigned char>(b));
           });
}

// RenderWorld.h ProcFileVersion: 4, 3, or 0
int procFileVersion(const std::string_view token)
{
  return equalsNoCase(token, ProcFileId) ? 4 : equalsNoCase(token, ProcFileId003) ? 3 : 0;
}

int areaOfModel(const std::string_view name)
{
  constexpr auto prefix = std::string_view{"_area"};
  if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix)
  {
    return -1;
  }
  auto area = -1;
  const auto digits = name.substr(prefix.size());
  const auto [end, error] =
    std::from_chars(digits.data(), digits.data() + digits.size(), area);
  return error == std::errc{} && end == digits.data() + digits.size() ? area : -1;
}

Error lexError(const EngineLexer& lexer, const std::string_view what)
{
  return Error{
    fmt::format("The .proc file cannot be read: {} (line {})", what, lexer.line())};
}

// RenderWorld_load.cpp:150-346 ParseModel, the parse only
Result<ProcModel> parseModel(EngineLexer& lexer, const int version, const bool geometry)
{
  auto model = ProcModel{};
  if (!lexer.expectTokenString("{") || !lexer.expectAnyToken(model.name))
  {
    return lexError(lexer, "a model has no name");
  }
  model.area = areaOfModel(model.name);
  const auto numSurfaces = lexer.parseInt();
  if (numSurfaces < 0 || lexer.hadError())
  {
    return lexError(lexer, "bad numSurfaces");
  }
  for (auto i = 0; i < numSurfaces; ++i)
  {
    auto surface = ProcSurface{};
    if (!lexer.expectTokenString("{") || !lexer.expectAnyToken(surface.material))
    {
      return lexError(lexer, "a surface has no material");
    }
    surface.numVerts = lexer.parseInt();
    surface.numIndexes = lexer.parseInt();
    if (version >= 4)
    {
      surface.vertexFormat = lexer.parseInt();
      if (surface.vertexFormat != 0 && surface.vertexFormat != 1)
      {
        return lexError(lexer, fmt::format("bad vertexFormat {}", surface.vertexFormat));
      }
    }
    if (surface.numVerts < 0 || surface.numIndexes < 0 || lexer.hadError())
    {
      return lexError(lexer, "bad vertex or index count");
    }
    const auto floats = surface.vertexFormat == 1 ? FloatsColor : FloatsBasic;
    auto verts = std::vector<float>(std::size_t(surface.numVerts) * std::size_t(floats));
    for (auto j = 0; j < surface.numVerts; ++j)
    {
      if (!lexer.parse1DMatrix(floats, verts.data() + std::size_t(j * floats)))
      {
        return lexError(lexer, "bad vertex");
      }
    }
    // tr_trisurf.cpp:606-609 R_BoundTriSurf, which FinishSurfaces' R_CleanupTriangles
    // runs on the parsed positions: MinMax over every vertex, a surface without one
    // cleared (C31)
    auto points = std::vector<EngineVec3>{};
    points.reserve(std::size_t(surface.numVerts));
    for (auto j = std::size_t{0}; j < std::size_t(surface.numVerts); ++j)
    {
      const auto* v = verts.data() + j * std::size_t(floats);
      points.push_back({v[0], v[1], v[2]});
    }
    surface.bounds = engineBoundsOfPoints(points);
    auto indexes = std::vector<int>(std::size_t(surface.numIndexes));
    for (auto& index : indexes)
    {
      index = lexer.parseInt();
      if (index < 0 || index >= surface.numVerts)
      {
        return lexError(lexer, fmt::format("index {} out of range", index));
      }
    }
    if (!lexer.expectTokenString("}"))
    {
      return lexError(lexer, "a surface does not end");
    }
    if (geometry)
    {
      surface.verts = std::move(verts);
      surface.indexes = std::move(indexes);
    }
    model.surfaces.push_back(std::move(surface));
  }
  if (!lexer.expectTokenString("}") || lexer.hadError())
  {
    return lexError(lexer, "a model does not end");
  }
  return model;
}

// RenderWorld_load.cpp:376-476 ParseInterAreaPortals
Result<void> parsePortals(EngineLexer& lexer, ProcTree& tree)
{
  if (!lexer.expectTokenString("{"))
  {
    return lexError(lexer, "interAreaPortals");
  }
  tree.numAreas = lexer.parseInt();
  const auto numPortals = lexer.parseInt();
  if (tree.numAreas < 0 || numPortals < 0 || lexer.hadError())
  {
    return lexError(lexer, "bad numPortalAreas or numInterAreaPortals");
  }
  for (auto i = 0; i < numPortals; ++i)
  {
    auto portal = ProcPortal{};
    const auto numPoints = lexer.parseInt();
    portal.positiveArea = lexer.parseInt();
    portal.negativeArea = lexer.parseInt();
    if (
      numPoints < 0 || portal.positiveArea < 0 || portal.positiveArea >= tree.numAreas
      || portal.negativeArea < 0 || portal.negativeArea >= tree.numAreas
      || lexer.hadError())
    {
      return lexError(lexer, "bad portal");
    }
    portal.points.resize(std::size_t(numPoints));
    for (auto& point : portal.points)
    {
      if (!lexer.parse1DMatrix(3, point.data()))
      {
        return lexError(lexer, "bad portal point");
      }
    }
    tree.portals.push_back(std::move(portal));
  }
  return lexer.expectTokenString("}") ? Result<void>{kdl::void_success}
                                      : lexError(lexer, "portals do not end");
}

// RenderWorld_load.cpp:553-599 ParseNodes
Result<void> parseNodes(EngineLexer& lexer, ProcTree& tree)
{
  if (!lexer.expectTokenString("{"))
  {
    return lexError(lexer, "nodes");
  }
  const auto numNodes = lexer.parseInt();
  if (numNodes < 0 || lexer.hadError())
  {
    return lexError(lexer, "bad numAreaNodes");
  }
  tree.nodes.resize(std::size_t(numNodes));
  for (auto& node : tree.nodes)
  {
    if (!lexer.parse1DMatrix(4, node.plane.data()))
    {
      return lexError(lexer, "bad node plane");
    }
    node.children[0] = lexer.parseInt();
    node.children[1] = lexer.parseInt();
  }
  // a child that names no node could send PointInArea off the array
  for (const auto& node : tree.nodes)
  {
    for (const auto child : node.children)
    {
      if (child >= numNodes)
      {
        return lexError(lexer, fmt::format("node child {} out of range", child));
      }
    }
  }
  return lexer.expectTokenString("}") ? Result<void>{kdl::void_success}
                                      : lexError(lexer, "nodes do not end");
}

} // namespace

Result<ProcTree> readProcFile(const std::string_view text, const bool geometry)
{
  // RenderWorld_load.cpp:764-998 InitFromMap, the .proc branch
  auto lexer = EngineLexer{text, "maps/.proc"};
  auto tree = ProcTree{};
  auto token = std::string{};
  if (!lexer.readToken(token) || (tree.version = procFileVersion(token)) == 0)
  {
    return Error{fmt::format(
      "The .proc file cannot be read: bad id '{}' instead of '{}' or '{}'",
      token,
      ProcFileId,
      ProcFileId003)};
  }
  // R1.7: each section through the pipeline; the first error ends the read
  auto read = Result<void>{kdl::void_success};
  while (read.is_success() && lexer.readToken(token))
  {
    read = std::move(read) | kdl::and_then([&]() -> Result<void> {
             if (token == "model")
             {
               return parseModel(lexer, tree.version, geometry)
                      | kdl::transform([&](ProcModel model) {
                          tree.models.push_back(std::move(model));
                        });
             }
             if (token == "shadowModel")
             {
               // the engine just skips it
               return lexer.skipBracedSection() ? Result<void>{kdl::void_success}
                                                : lexError(lexer, "shadowModel");
             }
             if (token == "interAreaPortals")
             {
               return parsePortals(lexer, tree);
             }
             if (token == "nodes")
             {
               return parseNodes(lexer, tree);
             }
             return lexError(lexer, fmt::format("bad token \"{}\"", token));
           });
  }
  return std::move(read) | kdl::and_then([&]() -> Result<ProcTree> {
           if (lexer.hadError())
           {
             return lexError(lexer, "the lexer failed");
           }
           return std::move(tree);
         });
}

int procModelArea(const std::string_view name)
{
  return areaOfModel(name);
}

int pointInArea(const ProcTree& tree, const std::array<float, 3>& point)
{
  // the engine's areaNodes is null: -1
  if (tree.nodes.empty())
  {
    return -1;
  }
  auto nodeNum = 0;
  // a pruned tree is shallow; the bound only keeps a malformed one from looping
  for (auto steps = std::size_t{0}; steps <= tree.nodes.size(); ++steps)
  {
    const auto& node = tree.nodes[std::size_t(nodeNum)];
    const auto d = point[0] * node.plane[0] + point[1] * node.plane[1]
                   + point[2] * node.plane[2] + node.plane[3];
    nodeNum = d > 0 ? node.children[0] : node.children[1];
    if (nodeNum == 0)
    {
      // in solid
      return -1;
    }
    if (nodeNum < 0)
    {
      return -1 - nodeNum;
    }
  }
  return -1;
}

float procFloat(const float value)
{
  return engineLexFloat(engineProcFloatText(value)).value_or(value);
}

float procReadBack(const float value)
{
  return lexerFloat(engineProcFloatText(value)).value_or(value);
}

} // namespace tb::lit

namespace tb::lit
{
namespace
{

// the islands of triangles that share a vertex (ParseModel's search, as a union-find,
// which makes the same partition): per vertex its island's representative, -1 for a
// vertex no triangle uses
template <typename Index>
std::vector<int> findIslands(
  const std::size_t numVerts, const std::vector<Index>& indexes)
{
  auto parent = std::vector<int>(numVerts);
  for (auto i = std::size_t{0}; i < numVerts; ++i)
  {
    parent[i] = int(i);
  }
  const auto root = [&](int v) {
    while (parent[std::size_t(v)] != v)
    {
      parent[std::size_t(v)] = parent[std::size_t(parent[std::size_t(v)])];
      v = parent[std::size_t(v)];
    }
    return v;
  };
  auto used = std::vector<bool>(numVerts, false);
  for (auto i = std::size_t{0}; i + 2 < indexes.size(); i += 3)
  {
    const auto a = root(int(indexes[i]));
    for (auto k = std::size_t{0}; k < 3; ++k)
    {
      used[std::size_t(indexes[i + k])] = true;
      const auto b = root(int(indexes[i + k]));
      if (a != b)
      {
        parent[std::size_t(b)] = a;
      }
    }
  }
  auto island = std::vector<int>(numVerts, -1);
  for (auto i = std::size_t{0}; i < numVerts; ++i)
  {
    if (used[i])
    {
      island[i] = root(int(i));
    }
  }
  return island;
}

// :269-297 for one set of st pairs, in float
template <typename Index, typename Get, typename Set>
void recentre(
  const std::size_t numVerts, const std::vector<Index>& indexes, Get get, Set set)
{
  const auto island = findIslands(numVerts, indexes);
  struct Range
  {
    float minS = std::numeric_limits<float>::infinity();
    float minT = std::numeric_limits<float>::infinity();
    float maxS = -std::numeric_limits<float>::infinity();
    float maxT = -std::numeric_limits<float>::infinity();
  };
  auto ranges = std::vector<Range>(numVerts);
  for (auto k = std::size_t{0}; k < numVerts; ++k)
  {
    if (island[k] >= 0)
    {
      auto& range = ranges[std::size_t(island[k])];
      const auto st = get(k);
      range.minS = std::min(range.minS, st[0]);
      range.maxS = std::max(range.maxS, st[0]);
      range.minT = std::min(range.minT, st[1]);
      range.maxT = std::max(range.maxT, st[1]);
    }
  }
  for (auto k = std::size_t{0}; k < numVerts; ++k)
  {
    if (island[k] >= 0)
    {
      const auto& range = ranges[std::size_t(island[k])];
      const auto averageS = float(engineFtoi((range.minS + range.maxS) * 0.5f));
      const auto averageT = float(engineFtoi((range.minT + range.maxT) * 0.5f));
      const auto st = get(k);
      set(k, EngineVec2{st[0] - averageS, st[1] - averageT});
    }
  }
}

} // namespace

std::vector<EngineDrawVert> loadProcSurface(const ProcSurface& surface)
{
  const auto floats = std::size_t(surface.vertexFormat == 1 ? FloatsColor : FloatsBasic);
  auto raw = surface.verts;
  const auto numVerts = std::size_t(surface.numVerts);
  recentre(
    numVerts,
    surface.indexes,
    [&](const std::size_t k) {
      return EngineVec2{raw[k * floats + 3], raw[k * floats + 4]};
    },
    [&](const std::size_t k, const EngineVec2& st) {
      raw[k * floats + 3] = st[0];
      raw[k * floats + 4] = st[1];
    });

  auto verts = std::vector<EngineDrawVert>{};
  verts.reserve(numVerts);
  for (auto j = std::size_t{0}; j < numVerts; ++j)
  {
    const auto* v = raw.data() + j * floats;
    auto vert = engineClearedVert();
    vert.xyz = {v[0], v[1], v[2]};
    engineSetTexCoord(vert, v[3], v[4]);
    engineSetNormal(vert, {v[5], v[6], v[7]});
    // the colour format's colours are LP7's (PackColorRound); the fixtures have none
    verts.push_back(vert);
  }
  return verts;
}

void recentreTexCoords(
  std::vector<EngineDrawVert>& verts, const std::vector<std::uint16_t>& indexes)
{
  recentre(
    verts.size(),
    indexes,
    [&](const std::size_t k) {
      // R3.9: the st dmap wrote (WriteFloat) as ParseModel reads them back
      const auto st = engineTexCoord(verts[k]);
      return EngineVec2{procReadBack(st[0]), procReadBack(st[1])};
    },
    [&](const std::size_t k, const EngineVec2& st) {
      engineSetTexCoord(verts[k], st[0], st[1]);
    });
}

} // namespace tb::lit
