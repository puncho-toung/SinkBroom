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

#include "mdl/LoadIdTech4ObjModel.h"

#include "base/FileLocation.h"
#include "base/Logger.h"
#include "gl/Material.h"
#include "gl/MaterialIndexRangeMap.h"
#include "gl/MaterialIndexRangeMapBuilder.h"
#include "gl/PrimType.h"
#include "mdl/BrushFace.h"
#include "mdl/IdTech4MaterialName.h"

#include "kd/contracts.h"
#include "kd/path_utils.h"
#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"
#include "vm/from_chars.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <system_error>
#include <type_traits>

namespace tb::mdl
{

kdl_reflect_impl(IdTech4ObjCorner);

kdl_reflect_impl(IdTech4ObjTriangle);

kdl_reflect_impl(IdTech4ObjData);

namespace
{

bool isSpace(const char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view str)
{
  while (!str.empty() && isSpace(str.front()))
  {
    str.remove_prefix(1);
  }
  while (!str.empty() && isSpace(str.back()))
  {
    str.remove_suffix(1);
  }
  return str;
}

/**
 * Removes the next whitespace separated token from the front of the given string and
 * returns it, or returns an empty string if there is none.
 */
std::string_view nextToken(std::string_view& str)
{
  auto first = size_t(0);
  while (first < str.size() && isSpace(str[first]))
  {
    ++first;
  }

  auto last = first;
  while (last < str.size() && !isSpace(str[last]))
  {
    ++last;
  }

  const auto token = str.substr(first, last - first);
  str.remove_prefix(last);
  return token;
}

/**
 * Whether the rest of a line has a token that the engine would read as the next
 * statement: its lexer reads tokens, not lines, and skips a comment as it skips a comment
 * line.
 */
bool hasMoreTokens(std::string_view str)
{
  const auto token = nextToken(str);
  return !token.empty() && token.front() != '#';
}

/**
 * Parses the whole token as a number. A leading '+' is accepted, as the engine's sscanf
 * accepts it, though std::from_chars does not. std::from_chars also accepts "nan" and
 * "inf", which are rejected: the engine's lexer reads no such numbers, and a position
 * that is not finite would have no bounds.
 */
template <typename T>
std::optional<T> parseNumber(std::string_view token)
{
  if (
    token.size() > 1 && token.front() == '+'
    && ((token[1] >= '0' && token[1] <= '9') || token[1] == '.'))
  {
    token.remove_prefix(1);
  }

  auto value = T{};
  const auto* last = token.data() + token.size();
  const auto [ptr, ec] = vm::from_chars(token.data(), last, value);
  if (ec != std::errc{} || ptr != last || token.empty())
  {
    return std::nullopt;
  }
  if constexpr (std::is_floating_point_v<T>)
  {
    if (!std::isfinite(value))
    {
      return std::nullopt;
    }
  }
  return value;
}

/**
 * Turns a 1-based or negative (relative to the end) OBJ index into a 0-based index into
 * a list of the given size, or returns nothing if it is out of range.
 */
std::optional<size_t> resolveIndex(const long long index, const size_t count)
{
  if (index > 0 && static_cast<unsigned long long>(index) <= count)
  {
    return static_cast<size_t>(index) - 1u;
  }
  if (index < 0 && index >= -static_cast<long long>(count))
  {
    return count - static_cast<size_t>(-index);
  }
  return std::nullopt;
}

struct FaceCornerTokens
{
  std::string_view position;
  std::string_view uv;
  std::string_view normal;
};

/**
 * Splits a face corner written as p, p/t, p//n or p/t/n.
 */
std::optional<FaceCornerTokens> splitFaceCorner(const std::string_view token)
{
  const auto firstSlash = token.find('/');
  if (firstSlash == std::string_view::npos)
  {
    return FaceCornerTokens{token, {}, {}};
  }

  const auto afterFirstSlash = token.substr(firstSlash + 1);
  const auto secondSlash = afterFirstSlash.find('/');
  if (secondSlash == std::string_view::npos)
  {
    // p/t
    return !afterFirstSlash.empty() ? std::optional{FaceCornerTokens{
                                        token.substr(0, firstSlash), afterFirstSlash, {}}}
                                    : std::nullopt;
  }

  // p//n or p/t/n
  const auto normal = afterFirstSlash.substr(secondSlash + 1);
  return !normal.empty() ? std::optional{FaceCornerTokens{
                             token.substr(0, firstSlash),
                             afterFirstSlash.substr(0, secondSlash),
                             normal}}
                         : std::nullopt;
}

class IdTech4ObjParser
{
private:
  std::string_view m_str;
  size_t m_line = 0;

  std::vector<vm::vec3f> m_positions;
  std::vector<vm::vec2f> m_uvs;
  size_t m_normalCount = 0;
  std::optional<size_t> m_materialIndex;
  std::vector<IdTech4ObjCorner> m_corners;

  IdTech4ObjData m_data;

public:
  explicit IdTech4ObjParser(const std::string_view str)
    : m_str{str}
  {
  }

  Result<IdTech4ObjData> parse()
  {
    while (!m_str.empty())
    {
      const auto endOfLine = m_str.find('\n');
      const auto line = m_str.substr(0, endOfLine);
      m_str.remove_prefix(
        endOfLine != std::string_view::npos ? endOfLine + 1 : m_str.size());
      ++m_line;

      if (auto error = parseLine(line))
      {
        return std::move(*error);
      }
    }

    if (m_positions.empty())
    {
      // like the assimp loader
      return Error{"Model has no vertices. (So no valid bounding box.)"};
    }
    if (m_data.triangles.empty())
    {
      return Error{"Model has no faces"};
    }

    return std::move(m_data);
  }

private:
  Error makeError(const std::string_view msg) const
  {
    return Error{prependLocation(FileLocation{m_line}, msg)};
  }

  /**
   * Notes that the engine's importer would reject the current line or read less of it,
   * unless an earlier line was noted already.
   */
  void noteEngineIncompatibility(const std::string_view reason)
  {
    if (!m_data.engineIncompatibility)
    {
      m_data.engineIncompatibility = prependLocation(FileLocation{m_line}, reason);
    }
  }

  std::optional<Error> parseLine(std::string_view line)
  {
    const auto keyword = nextToken(line);
    if (keyword.empty() || keyword.front() == '#')
    {
      return std::nullopt;
    }

    if (keyword == "v")
    {
      return parsePosition(line);
    }
    if (keyword == "vt")
    {
      return parseUv(line);
    }
    if (keyword == "vn")
    {
      // the engine reads the normals, but entity models have none
      ++m_normalCount;
      return std::nullopt;
    }
    if (keyword == "f")
    {
      return parseFace(line);
    }
    if (keyword == "usemtl")
    {
      parseMaterial(line);
      return std::nullopt;
    }
    if (
      keyword == "o" || keyword == "g" || keyword == "s" || keyword == "l"
      || keyword == "mtllib")
    {
      // the engine skips these too; ExportOBJ writes no mtllib line, and the .mtl file it
      // writes next to the model has no usable paths
      return std::nullopt;
    }

    return makeError(fmt::format("Unknown statement '{}'", keyword));
  }

  std::optional<Error> parsePosition(std::string_view line)
  {
    // v x y z [w]: the engine reads three numbers and fails on a fourth, which it takes
    // for an unknown statement (renderer/Model_obj.cpp); here a fourth number, such as
    // the weight w of the OBJ format, is ignored and noted
    auto position = vm::vec3f{};
    for (size_t i = 0; i < 3; ++i)
    {
      const auto token = nextToken(line);
      const auto value = parseNumber<float>(token);
      if (!value)
      {
        return makeError(
          fmt::format("Expected a vertex coordinate, but got '{}'", token));
      }
      position[i] = *value;
    }

    if (hasMoreTokens(line))
    {
      noteEngineIncompatibility(
        "a vertex has more than three numbers, and the engine fails on the fourth");
    }

    m_positions.push_back(position);
    return std::nullopt;
  }

  std::optional<Error> parseUv(std::string_view line)
  {
    // vt u [v [w]]: the engine wrote v = 1 - t; it reads exactly two numbers and fails
    // on any other count
    auto uv = vm::vec2f{};
    auto count = size_t(0);
    for (; count < 2; ++count)
    {
      const auto token = nextToken(line);
      if (token.empty() && count > 0)
      {
        break;
      }

      const auto value = parseNumber<float>(token);
      if (!value)
      {
        return makeError(
          fmt::format("Expected a texture coordinate, but got '{}'", token));
      }
      uv[count] = *value;
    }

    if (count != 2 || hasMoreTokens(line))
    {
      noteEngineIncompatibility(
        "a texture coordinate doesn't have two numbers, and the engine fails on it");
    }

    m_uvs.emplace_back(uv.x(), 1.0f - uv.y());
    return std::nullopt;
  }

  std::optional<Error> parseFace(std::string_view line)
  {
    m_corners.clear();

    // the engine reads a face with sscanf as exactly three p/t/n corners, fails if one
    // has fewer parts, and indexes its lists with whatever numbers it gets
    auto engineReadsCorners = true;

    for (auto token = nextToken(line); !token.empty(); token = nextToken(line))
    {
      const auto cornerTokens = splitFaceCorner(token);
      if (!cornerTokens)
      {
        return makeError(fmt::format("Malformed face corner '{}'", token));
      }

      const auto positionIndex = parseNumber<long long>(cornerTokens->position);
      const auto uvIndex = !cornerTokens->uv.empty()
                             ? parseNumber<long long>(cornerTokens->uv)
                             : std::optional<long long>{0};
      const auto normalIndex = !cornerTokens->normal.empty()
                                 ? parseNumber<long long>(cornerTokens->normal)
                                 : std::optional<long long>{0};
      if (!positionIndex || !uvIndex || !normalIndex)
      {
        return makeError(fmt::format("Malformed face corner '{}'", token));
      }

      const auto position = resolveIndex(*positionIndex, m_positions.size());
      if (!position)
      {
        return makeError(fmt::format(
          "Vertex index {} is out of range, the file has {} vertices so far",
          *positionIndex,
          m_positions.size()));
      }

      auto uv = vm::vec2f{0, 0};
      if (!cornerTokens->uv.empty())
      {
        const auto uvPosition = resolveIndex(*uvIndex, m_uvs.size());
        if (!uvPosition)
        {
          return makeError(fmt::format(
            "Texture coordinate index {} is out of range, the file has {} texture "
            "coordinates so far",
            *uvIndex,
            m_uvs.size()));
        }
        uv = m_uvs[*uvPosition];
      }

      if (!cornerTokens->normal.empty() && !resolveIndex(*normalIndex, m_normalCount))
      {
        return makeError(fmt::format(
          "Normal index {} is out of range, the file has {} normals so far",
          *normalIndex,
          m_normalCount));
      }

      engineReadsCorners =
        engineReadsCorners && *positionIndex > 0 && *uvIndex > 0 && *normalIndex > 0;
      m_corners.push_back({m_positions[*position], uv});
    }

    if (m_corners.size() < 3)
    {
      return makeError(fmt::format(
        "A face needs at least three corners, but this one has {}", m_corners.size()));
    }

    if (!engineReadsCorners)
    {
      noteEngineIncompatibility(
        "a face corner is not p/t/n with positive indices, and the engine can't read it");
    }
    else if (m_corners.size() > 3)
    {
      noteEngineIncompatibility(fmt::format(
        "a face has {} corners, and the engine reads only the first three",
        m_corners.size()));
    }

    // a fan around the first corner; reversing each triangle turns the file's
    // counter-clockwise order into TrenchBroom's clockwise order
    for (size_t i = 1; i + 1 < m_corners.size(); ++i)
    {
      m_data.triangles.push_back(IdTech4ObjTriangle{
        m_materialIndex, {m_corners[i + 1], m_corners[i], m_corners[0]}});
    }

    return std::nullopt;
  }

  void parseMaterial(const std::string_view line)
  {
    // the engine takes the rest of the line verbatim as the decl name
    const auto materialName = canonicalIdTech4MaterialName(trim(line));
    if (materialName.empty())
    {
      m_materialIndex = std::nullopt;
      return;
    }

    const auto iMaterialName = std::ranges::find(m_data.materialNames, materialName);
    m_materialIndex = size_t(std::distance(m_data.materialNames.begin(), iMaterialName));
    if (iMaterialName == m_data.materialNames.end())
    {
      m_data.materialNames.push_back(materialName);
    }
  }
};

EntityModelData buildModelData(
  const std::string& name,
  const IdTech4ObjData& objData,
  const FindMaterialFunc& findMaterial,
  const std::function<gl::Material(std::string)>& loadDefaultMaterial,
  Logger& logger)
{
  using Vertex = EntityModelVertex;

  auto skins = std::vector<gl::Material>{};
  skins.reserve(objData.materialNames.size() + 1);

  auto unresolvedNames = std::vector<std::string>{};
  for (const auto& materialName : objData.materialNames)
  {
    if (auto material = findMaterial ? findMaterial(materialName) : std::nullopt)
    {
      skins.push_back(std::move(*material));
    }
    else
    {
      unresolvedNames.push_back(materialName.generic_string());
      skins.push_back(loadDefaultMaterial(unresolvedNames.back()));
    }
  }

  // for the faces before the first usemtl
  skins.push_back(loadDefaultMaterial(BrushFace::NoMaterialName));

  if (!unresolvedNames.empty())
  {
    logger.warn() << fmt::format(
      "Found no material or image for {} of model '{}'",
      kdl::str_join(unresolvedNames, ", "),
      name);
  }

  if (objData.engineIncompatibility)
  {
    logger.warn() << fmt::format(
      "Model '{}' may not load in RBDOOM-3-BFG as it is shown here. {}",
      name,
      *objData.engineIncompatibility);
  }

  auto data = EntityModelData{PitchType::Normal, Orientation::Oriented};
  auto& surface = data.addSurface(name, 1);
  surface.setSkins(std::move(skins));

  const auto defaultSkinIndex = surface.skinCount() - 1u;
  const auto skinIndex = [&](const auto& triangle) {
    const auto index = triangle.materialIndex.value_or(defaultSkinIndex);
    contract_assert(index < surface.skinCount());
    return index;
  };

  // count the vertices of each skin and build the bounds
  auto vertexCounts = std::vector<size_t>(surface.skinCount(), 0u);
  auto bounds = vm::bbox3f{objData.triangles.front().corners.front().position};
  for (const auto& triangle : objData.triangles)
  {
    vertexCounts[skinIndex(triangle)] += 3u;
    for (const auto& corner : triangle.corners)
    {
      bounds = vm::merge(bounds, corner.position);
    }
  }

  auto size = gl::MaterialIndexRangeMap::Size{};
  for (size_t i = 0; i < vertexCounts.size(); ++i)
  {
    if (vertexCounts[i] > 0u)
    {
      size.inc(surface.skin(i), gl::PrimType::Triangles, vertexCounts[i]);
    }
  }

  auto& frame = data.addFrame(name, bounds);

  auto builder =
    gl::MaterialIndexRangeMapBuilder<Vertex::Type>{objData.triangles.size() * 3u, size};
  for (const auto& triangle : objData.triangles)
  {
    const auto& [c0, c1, c2] = triangle.corners;
    builder.addTriangle(
      surface.skin(skinIndex(triangle)),
      Vertex{c0.position, c0.uv},
      Vertex{c1.position, c1.uv},
      Vertex{c2.position, c2.uv});
  }
  surface.addMesh(frame, std::move(builder.vertices()), std::move(builder.indices()));

  return data;
}

} // namespace

Result<IdTech4ObjData> parseIdTech4Obj(const std::string_view str)
{
  return IdTech4ObjParser{str}.parse();
}

bool canLoadIdTech4ObjModel(const std::filesystem::path& path)
{
  return kdl::path_has_extension(kdl::path_to_lower(path), ".obj");
}

Result<EntityModelData> loadIdTech4ObjModel(
  std::string name,
  const std::string_view str,
  const FindMaterialFunc& findMaterial,
  const std::function<gl::Material(std::string)>& loadDefaultMaterial,
  Logger& logger)
{
  return parseIdTech4Obj(str) | kdl::transform([&](const auto& objData) {
           return buildModelData(
             name, objData, findMaterial, loadDefaultMaterial, logger);
         });
}

} // namespace tb::mdl
