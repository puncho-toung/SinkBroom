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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/MapFile.cpp: ComputeAxisBase and the
// brush primitives' texture vectors of idMapBrushSide::GetTextureVectors):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLayout>
#include <QStackedLayout>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "TestEnvironment.h"
#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "base/PreferenceManager.h"
#include "gl/GlManager.h"
#include "gl/PerspectiveCamera.h"
#include "lit/EngineGeometry.h"
#include "lit/GameDeclSource.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/LitPreviewPanel.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/QPathUtils.h"
#include "ui/SidebarPanel.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/*
 * LP2.4 (plans/lit-preview/27-LP2-PACKAGES.md §4, 00-PLAN.md §10): the lit preview
 * measured on a retail map. Hidden ("[.][measure]") and never part of a package's gate:
 * it needs the retail game (TB_LIT_GAME_PATH, only read), a Vulkan GPU and the desktop,
 * and it writes its frame log, its per-frame CSV and its summary table into
 * TB_LIT_MEASURE_OUT (a temp folder when unset).
 *
 * The map is Q20's maps/game/admin. Its .map in the game is the original "Version 2"
 * text with brushDef3 brushes, which TrenchBroom does not read (StandardMapParser): the
 * user converts it with the engine's convertMapToValve220 first. This harness, which may
 * not run the engine, converts it itself into the Valve 220 text that the engine's
 * conversion writes, geometrically: every brushDef3 plane becomes three plane points, its
 * texture matrix the Valve 220 axes, offsets and scales that give the same texture
 * coordinates through the preview's own editor-image sizes (asked of a material service
 * over the game), every brush entity's origin and rotation baked into its primitives as
 * the engine's conversion bakes them, and a light with primitives split into the light
 * and a func_static. The converted copy lives in a temp folder; the session is told that
 * it stands for maps/game/admin (LitSession::setMapPathOverrideForTesting), so that it
 * finds the map's bake (the .proc's areas) as for a document at base/maps/game.
 */

#ifdef _WIN32

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Q20
constexpr auto MapName = std::string_view{"maps/game/admin"};
// the copy's game-relative path for the scene's bake request
constexpr auto MapOverridePath = "maps/game/admin.map";
// the surface's pixel size (§10's "retail map, 1080p")
constexpr auto SurfaceWidth = 1920;
constexpr auto SurfaceHeight = 1080;
// pm_normalviewheight: the player's eye above the start's origin
constexpr auto EyeHeight = 68.0f;
// the scripted orbit: 10 s at the pacer's 30 Hz, a circle of this radius around the eye
constexpr auto OrbitTime = 10s;
constexpr auto OrbitRate = 30;
constexpr auto OrbitRadius = 64.0f;
// no frame for this long after a settled one: the frame is the last of its trigger
constexpr auto QuietTime = 2000ms;
constexpr auto IdleTime = 5s;

std::filesystem::path outputFolder()
{
  if (const auto value = qEnvironmentVariable("TB_LIT_MEASURE_OUT"); !value.isEmpty())
  {
    return pathFromQString(value);
  }
  return std::filesystem::temp_directory_path() / "TbUiLibTest-lit-measure";
}

double millisecondsBetween(const Clock::time_point from, const Clock::time_point to)
{
  return std::chrono::duration<double, std::milli>(to - from).count();
}

// ----- the Version 2 map and its conversion -----

struct Version2Side
{
  // the plane as the file has it: a x + b y + c z + d = 0, entity-local
  std::array<double, 4> plane = {0, 0, 0, 0};
  std::array<std::array<double, 3>, 2> texMat = {};
  std::string material;
};

struct Version2Patch
{
  bool patchDef3 = false;
  std::string material;
  std::vector<double> info;
  int width = 0;
  int height = 0;
  // the file's order: width rows of height vertices (x y z s t), entity origin included
  std::vector<std::array<double, 5>> vertices;
};

struct Version2Primitive
{
  std::vector<Version2Side> sides;
  std::optional<Version2Patch> patch;
};

struct Version2Entity
{
  std::vector<std::pair<std::string, std::string>> keys;
  std::vector<Version2Primitive> primitives;

  const std::string* find(const std::string_view key) const
  {
    for (const auto& [k, v] : keys)
    {
      if (k == key)
      {
        return &v;
      }
    }
    return nullptr;
  }

  void set(const std::string& key, const std::string& value)
  {
    for (auto& [k, v] : keys)
    {
      if (k == key)
      {
        v = value;
        return;
      }
    }
    keys.emplace_back(key, value);
  }

  void erase(const std::string_view key)
  {
    std::erase_if(keys, [&](const auto& pair) { return pair.first == key; });
  }
};

// the tokens of a .map text: brackets, quoted strings (without their quotes) and words
class MapTokens
{
private:
  std::string_view m_text;
  std::size_t m_at = 0;

public:
  struct Token
  {
    std::string_view text;
    bool quoted = false;
    bool end = false;
  };

  explicit MapTokens(const std::string_view text)
    : m_text{text}
  {
  }

  Token next()
  {
    skipSpace();
    if (m_at >= m_text.size())
    {
      return Token{{}, false, true};
    }
    const auto c = m_text[m_at];
    if (c == '"')
    {
      const auto close = m_text.find('"', m_at + 1);
      if (close == std::string_view::npos)
      {
        throw std::runtime_error{"unterminated string"};
      }
      const auto token = Token{m_text.substr(m_at + 1, close - m_at - 1), true, false};
      m_at = close + 1;
      return token;
    }
    if (c == '{' || c == '}' || c == '(' || c == ')')
    {
      return Token{m_text.substr(m_at++, 1), false, false};
    }
    const auto start = m_at;
    while (m_at < m_text.size() && !std::isspace(static_cast<unsigned char>(m_text[m_at]))
           && m_text[m_at] != '(' && m_text[m_at] != ')' && m_text[m_at] != '{'
           && m_text[m_at] != '}' && m_text[m_at] != '"')
    {
      ++m_at;
    }
    return Token{m_text.substr(start, m_at - start), false, false};
  }

  Token peek()
  {
    const auto at = m_at;
    const auto token = next();
    m_at = at;
    return token;
  }

  void expect(const std::string_view text)
  {
    const auto token = next();
    if (token.quoted || token.text != text)
    {
      throw std::runtime_error{
        fmt::format("expected '{}', found '{}' at {}", text, token.text, m_at)};
    }
  }

  double number()
  {
    const auto token = next();
    try
    {
      return std::stod(std::string{token.text});
    }
    catch (const std::exception&)
    {
      throw std::runtime_error{
        fmt::format("expected a number, found '{}' at {}", token.text, m_at)};
    }
  }

private:
  void skipSpace()
  {
    while (m_at < m_text.size())
    {
      if (std::isspace(static_cast<unsigned char>(m_text[m_at])))
      {
        ++m_at;
      }
      else if (m_text.substr(m_at, 2) == "//")
      {
        const auto end = m_text.find('\n', m_at);
        m_at = end == std::string_view::npos ? m_text.size() : end + 1;
      }
      else
      {
        break;
      }
    }
  }
};

Version2Primitive parseBrushDef3(MapTokens& tokens)
{
  auto primitive = Version2Primitive{};
  tokens.expect("{");
  // optional key/value pairs before the sides
  while (tokens.peek().quoted)
  {
    tokens.next();
    tokens.next();
  }
  while (tokens.peek().text != "}")
  {
    auto side = Version2Side{};
    tokens.expect("(");
    for (auto& value : side.plane)
    {
      value = tokens.number();
    }
    tokens.expect(")");
    tokens.expect("(");
    for (auto& row : side.texMat)
    {
      tokens.expect("(");
      for (auto& value : row)
      {
        value = tokens.number();
      }
      tokens.expect(")");
    }
    tokens.expect(")");
    side.material = std::string{tokens.next().text};
    // the Q2 contents, flags and value that the engine reads and ignores
    while (!tokens.peek().end && tokens.peek().text != "(" && tokens.peek().text != "}")
    {
      tokens.next();
    }
    primitive.sides.push_back(std::move(side));
  }
  tokens.expect("}");
  return primitive;
}

Version2Primitive parsePatch(MapTokens& tokens, const bool patchDef3)
{
  auto patch = Version2Patch{};
  patch.patchDef3 = patchDef3;
  tokens.expect("{");
  patch.material = std::string{tokens.next().text};
  tokens.expect("(");
  for (auto i = 0; i < (patchDef3 ? 7 : 5); ++i)
  {
    patch.info.push_back(tokens.number());
  }
  tokens.expect(")");
  patch.width = int(patch.info[0]);
  patch.height = int(patch.info[1]);
  tokens.expect("(");
  for (auto j = 0; j < patch.width; ++j)
  {
    tokens.expect("(");
    for (auto i = 0; i < patch.height; ++i)
    {
      auto vertex = std::array<double, 5>{};
      tokens.expect("(");
      for (auto& value : vertex)
      {
        value = tokens.number();
      }
      tokens.expect(")");
      patch.vertices.push_back(vertex);
    }
    tokens.expect(")");
  }
  tokens.expect(")");
  tokens.expect("}");
  auto primitive = Version2Primitive{};
  primitive.patch = std::move(patch);
  return primitive;
}

// idMapFile::Parse's reading of a "Version 2" file: entities of keys, brushDef3 brushes
// and patchDef2/3 patches
std::vector<Version2Entity> parseVersion2Map(const std::string_view text)
{
  auto tokens = MapTokens{text};
  tokens.expect("Version");
  if (tokens.number() != 2.0)
  {
    throw std::runtime_error{"not a Version 2 map"};
  }
  auto entities = std::vector<Version2Entity>{};
  while (!tokens.peek().end)
  {
    auto entity = Version2Entity{};
    tokens.expect("{");
    while (true)
    {
      const auto token = tokens.next();
      if (token.end)
      {
        throw std::runtime_error{"unexpected end of the map"};
      }
      if (token.quoted)
      {
        const auto value = tokens.next();
        entity.keys.emplace_back(std::string{token.text}, std::string{value.text});
      }
      else if (token.text == "{")
      {
        const auto kind = tokens.next();
        if (kind.text == "brushDef3")
        {
          entity.primitives.push_back(parseBrushDef3(tokens));
        }
        else if (kind.text == "patchDef2" || kind.text == "patchDef3")
        {
          entity.primitives.push_back(parsePatch(tokens, kind.text == "patchDef3"));
        }
        else
        {
          throw std::runtime_error{fmt::format("unknown primitive '{}'", kind.text)};
        }
        tokens.expect("}");
      }
      else if (token.text == "}")
      {
        break;
      }
      else
      {
        throw std::runtime_error{fmt::format("unexpected '{}'", token.text)};
      }
    }
    entities.push_back(std::move(entity));
  }
  return entities;
}

std::array<double, 3> vectorOf(const std::string* value)
{
  auto result = std::array<double, 3>{0, 0, 0};
  if (value)
  {
    auto stream = std::istringstream{*value};
    stream >> result[0] >> result[1] >> result[2];
  }
  return result;
}

// an entity's transform: world = local.x axis[0] + local.y axis[1] + local.z axis[2] +
// origin, as idMapFile::ConvertToValve220Format builds it (local * rot + origin)
struct EntityTransform
{
  std::array<vm::vec3d, 3> axis = {
    vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}, vm::vec3d{0, 0, 1}};
  vm::vec3d origin = vm::vec3d{0, 0, 0};

  bool rotated() const
  {
    return axis[0] != vm::vec3d{1, 0, 0} || axis[1] != vm::vec3d{0, 1, 0}
           || axis[2] != vm::vec3d{0, 0, 1};
  }

  bool identity() const { return !rotated() && origin == vm::vec3d{0, 0, 0}; }

  vm::vec3d rotate(const vm::vec3d& v) const
  {
    return v.x() * axis[0] + v.y() * axis[1] + v.z() * axis[2];
  }

  vm::vec3d apply(const vm::vec3d& v) const { return rotate(v) + origin; }
};

// DX.6: the copied idAngles::ToMat3 (engineAnglesToMat3, M-GEO-PURE-01), its float rows
// as the engine's ConvertToValve220Format has them
std::array<vm::vec3d, 3> anglesToAxis(
  const double pitch, const double yaw, const double roll)
{
  const auto m = lit::engineAnglesToMat3(float(pitch), float(yaw), float(roll));
  return {
    vm::vec3d{m[0], m[1], m[2]},
    vm::vec3d{m[3], m[4], m[5]},
    vm::vec3d{m[6], m[7], m[8]},
  };
}

// idMapFile::ConvertToValve220Format's entity transform: "rotation", else "angles", else
// "angle", none for a door whose "angle" is its move direction
EntityTransform entityTransform(const Version2Entity& entity, const bool angleIsMoveDir)
{
  auto transform = EntityTransform{};
  const auto* classname = entity.find("classname");
  if (!classname || *classname != "worldspawn")
  {
    const auto origin = vectorOf(entity.find("origin"));
    transform.origin = vm::vec3d{origin[0], origin[1], origin[2]};
  }
  if (angleIsMoveDir)
  {
    return transform;
  }
  if (const auto* rotation = entity.find("rotation"))
  {
    auto stream = std::istringstream{*rotation};
    auto m = std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (auto& value : m)
    {
      stream >> value;
    }
    transform.axis = {
      vm::vec3d{m[0], m[1], m[2]},
      vm::vec3d{m[3], m[4], m[5]},
      vm::vec3d{m[6], m[7], m[8]}};
  }
  else if (const auto* angles = entity.find("angles"))
  {
    const auto a = vectorOf(angles);
    if (a[0] != 0 || a[1] != 0 || a[2] != 0)
    {
      transform.axis = anglesToAxis(a[0], a[1], a[2]);
    }
  }
  else if (const auto* angle = entity.find("angle"))
  {
    if (const auto yaw = std::stod(*angle); yaw != 0)
    {
      transform.axis = anglesToAxis(0, yaw, 0);
    }
  }
  return transform;
}

// ComputeAxisBase (MapFile.cpp:69-97), in the engine's float arithmetic
std::array<vm::vec3d, 2> computeAxisBase(const vm::vec3d& normal)
{
  auto n = std::array<float, 3>{};
  for (std::size_t i = 0; i < 3; ++i)
  {
    n[i] = std::fabs(float(normal[i])) < 1e-6f ? 0.0f : float(normal[i]);
  }
  const auto rotY = -std::atan2(n[2], std::sqrt(n[1] * n[1] + n[0] * n[0]));
  const auto rotZ = std::atan2(n[1], n[0]);
  return {
    vm::vec3d{-std::sin(rotZ), std::cos(rotZ), 0.0f},
    vm::vec3d{
      -std::sin(rotY) * std::cos(rotZ),
      -std::sin(rotY) * std::sin(rotZ),
      -std::cos(rotY)},
  };
}

// the editor image size (C21) the preview divides a side's texture vectors by
struct ImageSize
{
  double width = 8;
  double height = 8;
};

std::string formatNumber(const double value)
{
  // the shortest text that reads back as the same double; no "-0"
  return fmt::format("{}", value == 0.0 ? 0.0 : value);
}

/**
 * One brushDef3 side as a Valve 220 face in world space. The plane is turned and moved by
 * the entity's transform and written as three points whose (p3 - p1) x (p2 - p1) is its
 * outward normal. Its texture: dmap's brush-primitive vectors (GetTextureVectors) give
 * (local + origin) . A + C, in units of the image; the Valve 220 face gives
 * (world . axis / scale + offset) / size; with world = M local + origin, axis / scale =
 * size M A and offset = size (C + origin . A - origin . M A).
 */
std::string valveFace(
  const Version2Side& side, const EntityTransform& transform, const ImageSize& size)
{
  auto normal = vm::vec3d{side.plane[0], side.plane[1], side.plane[2]};
  const auto length = vm::length(normal);
  if (length == 0.0)
  {
    return {};
  }
  normal = normal / length;
  const auto dist = side.plane[3] / length;
  const auto worldNormal = vm::normalize(transform.rotate(normal));
  const auto worldDist = dist - vm::dot(worldNormal, transform.origin);

  const auto p1 = -worldDist * worldNormal;
  const auto helper = std::fabs(worldNormal.x()) < 0.6   ? vm::vec3d{1, 0, 0}
                      : std::fabs(worldNormal.y()) < 0.6 ? vm::vec3d{0, 1, 0}
                                                         : vm::vec3d{0, 0, 1};
  const auto u = vm::normalize(vm::cross(worldNormal, helper));
  const auto v = vm::cross(u, worldNormal);
  const auto p2 = p1 + 128.0 * u;
  const auto p3 = p1 + 128.0 * v;

  const auto base = computeAxisBase(normal);
  auto axes = std::array<vm::vec3d, 2>{};
  auto offsets = std::array<double, 2>{};
  auto scales = std::array<double, 2>{};
  const auto sizes = std::array<double, 2>{size.width, size.height};
  for (std::size_t i = 0; i < 2; ++i)
  {
    const auto a = base[0] * side.texMat[i][0] + base[1] * side.texMat[i][1];
    const auto worldA = transform.rotate(a);
    const auto valve = sizes[i] * worldA;
    offsets[i] =
      sizes[i]
      * (side.texMat[i][2] + vm::dot(transform.origin, a) - vm::dot(transform.origin, worldA));
    const auto valveLength = vm::length(valve);
    if (valveLength > 1e-12)
    {
      axes[i] = valve / valveLength;
      scales[i] = 1.0 / valveLength;
    }
    else
    {
      // a degenerate texture matrix: any axis in the plane, the default scale
      axes[i] = i == 0 ? u : v;
      scales[i] = 1.0;
    }
  }

  auto material = side.material;
  if (material.starts_with("textures/"))
  {
    material = material.substr(9);
  }
  const auto point = [](const vm::vec3d& p) {
    return fmt::format(
      "( {} {} {} )", formatNumber(p.x()), formatNumber(p.y()), formatNumber(p.z()));
  };
  const auto axis = [&](const std::size_t i) {
    return fmt::format(
      "[ {} {} {} {} ]",
      formatNumber(axes[i].x()),
      formatNumber(axes[i].y()),
      formatNumber(axes[i].z()),
      formatNumber(offsets[i]));
  };
  return fmt::format(
    "{} {} {} {} {} {} 0 {} {}\n",
    point(p1),
    point(p2),
    point(p3),
    material,
    axis(0),
    axis(1),
    formatNumber(scales[0]),
    formatNumber(scales[1]));
}

std::string valvePatch(const Version2Patch& patch, const EntityTransform& transform)
{
  auto text = std::string{patch.patchDef3 ? "{\npatchDef3\n{\n" : "{\npatchDef2\n{\n"};
  auto material = patch.material;
  if (material.starts_with("textures/"))
  {
    material = material.substr(9);
  }
  text += fmt::format("\"{}\"\n( ", material);
  for (const auto value : patch.info)
  {
    text += formatNumber(value) + " ";
  }
  text += ")\n(\n";
  auto index = std::size_t{0};
  for (auto j = 0; j < patch.width; ++j)
  {
    text += "( ";
    for (auto i = 0; i < patch.height; ++i, ++index)
    {
      const auto& vertex = patch.vertices[index];
      // the file's vertices are entity-local plus the origin (idMapPatch::Parse)
      const auto local = vm::vec3d{vertex[0], vertex[1], vertex[2]} - transform.origin;
      const auto world = transform.apply(local);
      text += fmt::format(
        "( {} {} {} {} {} ) ",
        formatNumber(world.x()),
        formatNumber(world.y()),
        formatNumber(world.z()),
        formatNumber(vertex[3]),
        formatNumber(vertex[4]));
    }
    text += ")\n";
  }
  text += ")\n}\n}\n";
  return text;
}

struct ConversionStats
{
  std::size_t entities = 0;
  std::size_t brushes = 0;
  std::size_t sides = 0;
  std::size_t patches = 0;
  std::size_t transformedEntities = 0;
  std::size_t rotatedEntities = 0;
  std::size_t splitLights = 0;
};

/**
 * The Valve 220 text of a Version 2 map, as idMapFile::ConvertToValve220Format and
 * idMapFile::Write give it to TrenchBroom, geometrically (see the file's comment): brush
 * entities lose "rotation", "angles", "angle" and, when they move, "origin", which their
 * primitives take; a door's "angle" becomes its "movedir"; a light with primitives keeps
 * its keys but "model", and a func_static of its own takes the primitives.
 */
std::string convertToValve220(
  std::vector<Version2Entity> entities,
  const std::map<std::string, ImageSize>& sizes,
  ConversionStats& stats)
{
  auto text = std::string{"// Game: Doom 3 BFG\n// Format: Doom3 (Valve)\n"};
  auto index = std::size_t{0};
  const auto write = [&](
                       const Version2Entity& entity,
                       const std::vector<Version2Primitive>& primitives,
                       const EntityTransform& transform) {
    text += fmt::format("// entity {}\n{{\n", index++);
    for (const auto& [key, value] : entity.keys)
    {
      text += fmt::format("\"{}\" \"{}\"\n", key, value);
    }
    for (const auto& primitive : primitives)
    {
      if (primitive.patch)
      {
        text += valvePatch(*primitive.patch, transform);
        ++stats.patches;
        continue;
      }
      text += "{\n";
      for (const auto& side : primitive.sides)
      {
        const auto it = sizes.find(side.material);
        text += valveFace(side, transform, it != sizes.end() ? it->second : ImageSize{});
        ++stats.sides;
      }
      text += "}\n";
      ++stats.brushes;
    }
    text += "}\n";
  };

  auto lightModels = 0;
  for (auto& entity : entities)
  {
    const auto classname =
      entity.find("classname") ? *entity.find("classname") : std::string{};
    if (entity.primitives.empty())
    {
      write(entity, {}, EntityTransform{});
      continue;
    }

    const auto angleIsMoveDir =
      classname.starts_with("func_door") && !entity.find("movedir");
    const auto transform = entityTransform(entity, angleIsMoveDir);
    if (classname != "worldspawn")
    {
      ++stats.transformedEntities;
      stats.rotatedEntities += transform.rotated() ? 1 : 0;
    }

    if (classname == "light")
    {
      // the light stays a point entity with its keys; a func_static takes the primitives
      auto model = Version2Entity{};
      model.set("classname", "func_static");
      const auto name = fmt::format("light_model_{}", ++lightModels);
      model.set("name", name);
      model.set("model", name);
      entity.erase("model");
      write(entity, {}, EntityTransform{});
      write(model, entity.primitives, transform);
      ++stats.splitLights;
      continue;
    }

    if (angleIsMoveDir)
    {
      if (const auto* angle = entity.find("angle"))
      {
        entity.set("movedir", *angle);
      }
    }
    if (classname != "worldspawn")
    {
      entity.erase("rotation");
      entity.erase("angles");
      entity.erase("angle");
      if (!transform.identity())
      {
        entity.erase("origin");
      }
    }
    else if (!entity.find("mapversion"))
    {
      entity.set("mapversion", "220");
    }
    write(entity, entity.primitives, transform);
  }
  stats.entities = index;
  return text;
}

/**
 * The editor-image sizes the preview divides each material's texture vectors by
 * (MaterialEntry::editorImageSize, C21), from a material service over the game, as the
 * session's will find them; 8 × 8 for a material the game does not have.
 */
std::map<std::string, ImageSize> editorImageSizes(
  const std::filesystem::path& gamePath, const std::set<std::string>& materials)
{
  auto options = lit::MaterialServiceOptions{};
  options.makeSource = [gamePath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<lit::DeclSource>{
      std::make_unique<lit::GameDeclSource>(gamePath, &cancel)};
  };
  options.pollInterval = 0ms;
  auto service = lit::MaterialService::create(std::move(options))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

  auto sizes = std::map<std::string, ImageSize>{};
  const auto deadline = Clock::now() + 60s;
  auto pending = materials;
  while (!pending.empty() && Clock::now() < deadline)
  {
    for (auto it = pending.begin(); it != pending.end();)
    {
      const auto lookup = service->lookup(*it);
      if (lookup.state == lit::MaterialLookup::State::Pending)
      {
        ++it;
        continue;
      }
      if (lookup.entry)
      {
        sizes[*it] = ImageSize{
          double(lookup.entry->editorImageSize.width),
          double(lookup.entry->editorImageSize.height)};
      }
      it = pending.erase(it);
    }
    if (!pending.empty())
    {
      std::this_thread::sleep_for(20ms);
    }
  }
  service->requestStop();
  service->join();
  CHECK(pending.empty());
  return sizes;
}

// ----- the measurement -----

// what the GUI thread saw of each frame a session showed (LitSession::status)
struct FrameRecord
{
  Clock::time_point at;
  lit::SessionStatus status;
  std::uint64_t geometryBatches = 0;
  std::uint64_t snapshotsTaken = 0;
  std::uint64_t tableVersion = 0;
  lit::CompileTimings compile;
};

/**
 * Processes the GUI thread's events and samples the running session after each pass, so
 * that every frame shown is recorded with the time it was first seen; the session is
 * fetched from the panel every pass, so a session that a teardown removed is never read.
 */
class Sampler
{
public:
  LitPreviewPanel& panel;
  std::vector<FrameRecord> frames;
  std::uint64_t lastFrames = 0;
  std::optional<Clock::time_point> sessionSeen;
  std::optional<Clock::time_point> tableSeen;
  std::optional<Clock::time_point> snapshotSeen;
  std::optional<Clock::time_point> batchSeen;
  // LP4.3: the material table's version as last sampled, and when it last changed
  std::uint64_t tableNow = 0;
  Clock::time_point tableChangedAt = Clock::now();
  std::uint64_t vramPeak = 0;
  std::uint64_t accountPeak = 0;
  // the gaps between two samples, for the resolution the times are known to
  std::vector<double> gaps;
  // the camera script, called every pass
  std::function<void(Clock::time_point)> tick;

private:
  Clock::time_point m_lastSample = Clock::now();
  Clock::time_point m_lastVram = Clock::now();

public:
  explicit Sampler(LitPreviewPanel& p)
    : panel{p}
  {
  }

  void reset()
  {
    frames.clear();
    lastFrames = 0;
    sessionSeen.reset();
    tableSeen.reset();
    snapshotSeen.reset();
    batchSeen.reset();
    tableNow = 0;
    tableChangedAt = Clock::now();
    vramPeak = 0;
    accountPeak = 0;
  }

  void sample()
  {
    const auto now = Clock::now();
    gaps.push_back(millisecondsBetween(m_lastSample, now));
    m_lastSample = now;
    if (!tableSeen && panel.materialsVersionForTesting() > 0)
    {
      tableSeen = now;
    }
    if (const auto table = panel.materialsVersionForTesting(); table != tableNow)
    {
      tableNow = table;
      tableChangedAt = now;
    }
    auto* session = panel.sessionForTesting();
    if (!session)
    {
      return;
    }
    if (!sessionSeen)
    {
      sessionSeen = now;
    }
    if (!snapshotSeen && session->snapshotsTakenForTesting() > 0)
    {
      snapshotSeen = now;
    }
    if (!batchSeen && session->geometryBatchesForTesting() > 0)
    {
      batchSeen = now;
    }
    const auto status = session->status();
    accountPeak = std::max(accountPeak, status.vramBytes);
    if (status.frames != lastFrames)
    {
      lastFrames = status.frames;
      frames.push_back(FrameRecord{
        now,
        status,
        session->geometryBatchesForTesting(),
        session->snapshotsTakenForTesting(),
        panel.materialsVersionForTesting(),
        session->compileTimingsForTesting()});
    }
    if (now - m_lastVram > 250ms)
    {
      m_lastVram = now;
      if (const auto vram = session->vramForTesting())
      {
        vramPeak = std::max(vramPeak, vram->usedBytes);
      }
    }
  }

  // one pass of the event loop and a sample
  void pass()
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    sample();
    if (tick)
    {
      tick(Clock::now());
    }
  }

  template <typename Predicate>
  bool runUntil(Predicate predicate, const Clock::duration timeout)
  {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline)
    {
      pass();
      if (predicate())
      {
        return true;
      }
      std::this_thread::sleep_for(500us);
    }
    return false;
  }

  void runFor(const Clock::duration duration)
  {
    runUntil([] { return false; }, duration);
  }

  bool isLit(const FrameRecord& record) const { return record.status.lightsDrawn > 0; }

  /**
   * D77, settled for good: the last frame is settled (and lit, when asked), no frame
   * came for QuietTime or twice the longest gap between two frames so far (a Debug
   * build's frames may take seconds), and the scene's batches did not change meanwhile.
   * LP4.3: the table is stable for as long (its last change at least that long ago);
   * a table published after the last frame that drew none (the service's bulk parse,
   * which LP4.2a's and LP4.3's light-grid and probe images now let finish after the
   * first settled frame: nothing waited for it) does not hold the measure back.
   */
  bool quietlySettled(const bool requireLit = true) const
  {
    if (frames.empty())
    {
      return false;
    }
    auto quiet = std::chrono::duration_cast<Clock::duration>(QuietTime);
    for (std::size_t i = 1; i < frames.size(); ++i)
    {
      quiet = std::max(quiet, 2 * (frames[i].at - frames[i - 1].at));
    }
    const auto& last = frames.back();
    auto* session = panel.sessionForTesting();
    const auto now = Clock::now();
    return session && last.status.settled && (!requireLit || isLit(last))
           && now - last.at >= quiet && now - tableChangedAt >= quiet
           && session->geometryBatchesForTesting() == last.geometryBatches
           && panel.materialsVersionForTesting() == tableNow;
  }
};

// a frame row of the frame log (C16), by its column names
struct LogRow
{
  std::vector<std::string> fields;

  const std::string& at(const std::size_t column) const
  {
    static const auto empty = std::string{};
    return column < fields.size() ? fields[column] : empty;
  }

  std::optional<double> number(const std::size_t column) const
  {
    const auto& field = at(column);
    if (field.empty())
    {
      return std::nullopt;
    }
    try
    {
      return std::stod(field);
    }
    catch (const std::exception&)
    {
      return std::nullopt;
    }
  }
};

// the frame log's columns (lit/FrameLog.cpp, frameLogHeader)
namespace column
{
constexpr auto Session = std::size_t{0};
constexpr auto Event = std::size_t{1};
constexpr auto Frame = std::size_t{2};
constexpr auto CpuRecordMs = std::size_t{3};
constexpr auto AcquireMs = std::size_t{4};
constexpr auto PresentMs = std::size_t{5};
constexpr auto GpuFrameMs = std::size_t{6};
constexpr auto Width = std::size_t{10};
constexpr auto Height = std::size_t{11};
constexpr auto VramUsedMb = std::size_t{12};
constexpr auto AllocCount = std::size_t{14};
constexpr auto BytesUploaded = std::size_t{15};
constexpr auto TexturesResident = std::size_t{16};
constexpr auto ArenaBytes = std::size_t{17};
constexpr auto GpuPrepassMs = std::size_t{18};
constexpr auto GpuInteractionsMs = std::size_t{19};
constexpr auto GpuTonemapMs = std::size_t{20};
constexpr auto GpuBlitMs = std::size_t{21};
constexpr auto Draws = std::size_t{22};
constexpr auto PrepassDraws = std::size_t{23};
constexpr auto InteractionDraws = std::size_t{24};
constexpr auto SkippedDraws = std::size_t{25};
constexpr auto PendingPipelines = std::size_t{26};
constexpr auto LightsVisible = std::size_t{27};
constexpr auto LightsDrawn = std::size_t{28};
constexpr auto LightsOverflowed = std::size_t{29};
constexpr auto ConstantBytes = std::size_t{30};
constexpr auto VolatileWrites = std::size_t{31};
constexpr auto VolatileWriteBudget = std::size_t{32};
// LP4.1 (C16, P4.27): the view's portal flow
constexpr auto ViewArea = std::size_t{37};
constexpr auto AreasFlowed = std::size_t{38};
constexpr auto LightsInView = std::size_t{39};
constexpr auto PortalChains = std::size_t{40};
constexpr auto FlowMs = std::size_t{41};
// LP4.2a (C34, P4.27): the ambient pass's
constexpr auto AmbientDraws = std::size_t{42};
constexpr auto GpuAmbientMs = std::size_t{43};
// LP4.3 (D86, D92): set 9's writes and the surfaces on the light-grid path
constexpr auto AmbientWrites = std::size_t{44};
constexpr auto GridSurfaces = std::size_t{45};
// LPF.1 (C44, P5.21): and the live feed's three (feed_applied, feed_ms, override_bytes);
// LP5.1 (C45): and the interaction phases' two (math_lights, ambient_lights)
constexpr auto Count = std::size_t{51};
} // namespace column

std::vector<LogRow> readFrameLog(const std::filesystem::path& path)
{
  auto rows = std::vector<LogRow>{};
  auto stream = std::ifstream{path};
  auto line = std::string{};
  while (std::getline(stream, line))
  {
    auto row = LogRow{};
    auto field = std::string{};
    auto lineStream = std::stringstream{line};
    while (std::getline(lineStream, field, ','))
    {
      row.fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',')
    {
      row.fields.emplace_back();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

// a session's frame rows in the order shown: the k-th is the frame whose status counted
// k frames
std::vector<LogRow> sessionFrames(
  const std::vector<LogRow>& rows, const std::uint64_t generation)
{
  auto frames = std::vector<LogRow>{};
  const auto session = std::to_string(generation);
  for (const auto& row : rows)
  {
    if (row.at(column::Session) == session && row.at(column::Event) == "frame")
    {
      frames.push_back(row);
    }
  }
  std::ranges::sort(frames, [](const auto& lhs, const auto& rhs) {
    return std::stoull(lhs.at(column::Frame)) < std::stoull(rhs.at(column::Frame));
  });
  return frames;
}

std::optional<LogRow> sessionRow(
  const std::vector<LogRow>& rows,
  const std::uint64_t generation,
  const std::string_view event)
{
  const auto session = std::to_string(generation);
  for (const auto& row : rows)
  {
    if (row.at(column::Session) == session && row.at(column::Event) == event)
    {
      return row;
    }
  }
  return std::nullopt;
}

struct Distribution
{
  std::size_t count = 0;
  double median = 0;
  double p95 = 0;
  double max = 0;
  double sum = 0;
};

Distribution distributionOf(std::vector<double> values)
{
  auto result = Distribution{};
  if (values.empty())
  {
    return result;
  }
  std::ranges::sort(values);
  result.count = values.size();
  const auto at = [&](const double q) {
    // nearest rank
    const auto rank = std::size_t(std::ceil(q * double(values.size())));
    return values[std::min(values.size() - 1, rank > 0 ? rank - 1 : 0)];
  };
  result.median = values.size() % 2 == 1
                    ? values[values.size() / 2]
                    : 0.5 * (values[values.size() / 2 - 1] + values[values.size() / 2]);
  result.p95 = at(0.95);
  result.max = values.back();
  for (const auto value : values)
  {
    result.sum += value;
  }
  return result;
}

Distribution columnOf(
  const std::vector<LogRow>& rows,
  const std::size_t column,
  const std::size_t first,
  const std::size_t last)
{
  auto values = std::vector<double>{};
  for (auto i = first; i <= last && i < rows.size(); ++i)
  {
    if (const auto value = rows[i].number(column))
    {
      values.push_back(*value);
    }
  }
  return distributionOf(std::move(values));
}

// one Start of the preview, its milestones in ms after the press
struct StartMeasure
{
  std::string label;
  std::uint64_t generation = 0;
  Clock::time_point pressed;
  // the frames the GUI thread saw until the Start had settled for good
  std::vector<FrameRecord> records;
  double phaseAMs = -1;
  double sessionMs = -1;
  double firstFrameMs = -1;
  double firstTableMs = -1;
  double firstSnapshotMs = -1;
  double firstBatchMs = -1;
  // from the frame log (C16): the first frame that drew interactions, and the first
  // that drew them and skipped no draw
  std::uint64_t firstLitFrame = 0;
  double firstLitMs = -1;
  std::uint64_t completeLitFrame = 0;
  double completeLitMs = -1;
  // D77: the first settled frame, and the first frame of the last settled run that was
  // drawn with the scene's last batch (every visible texture at its full mips, the bake
  // adopted): settled for good
  std::uint64_t firstSettledFrame = 0;
  double firstSettledMs = -1;
  std::uint64_t settledFrame = 0;
  double settledMs = -1;
  // the compile thread by the first lit frame and by the end
  lit::CompileTimings compileAtFirstLit;
  lit::CompileTimings compileAtSettled;
  double compileEndAtFirstLitMs = -1;
  double lastCompileEndMs = -1;

  const FrameRecord* record(const std::uint64_t frame) const
  {
    const auto it = std::ranges::find_if(
      records, [&](const auto& r) { return r.status.frames == frame; });
    return it != records.end() ? &*it : nullptr;
  }

  double msOf(const std::uint64_t frame) const
  {
    const auto* r = record(frame);
    return r ? std::chrono::duration<double, std::milli>(r->at - pressed).count() : -1.0;
  }

  // the D77 frames, from the records
  void findSettled()
  {
    const auto first =
      std::ranges::find_if(records, [](const auto& r) { return r.status.settled; });
    if (first != records.end())
    {
      firstSettledFrame = first->status.frames;
      firstSettledMs = msOf(firstSettledFrame);
    }
    if (records.empty() || !records.back().status.settled)
    {
      return;
    }
    auto index = records.size() - 1;
    while (index > 0 && records[index - 1].status.settled)
    {
      --index;
    }
    while (index + 1 < records.size()
           && records[index].geometryBatches != records.back().geometryBatches)
    {
      ++index;
    }
    settledFrame = records[index].status.frames;
    settledMs = msOf(settledFrame);
  }

  // the lit frames, from the session's frame log rows (the k-th row is frame k)
  void findLit(const std::vector<LogRow>& rows)
  {
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
      const auto interactions = rows[i].number(column::InteractionDraws).value_or(0);
      const auto skipped = rows[i].number(column::SkippedDraws).value_or(0);
      if (interactions > 0 && firstLitFrame == 0)
      {
        firstLitFrame = i + 1;
      }
      if (interactions > 0 && skipped == 0 && completeLitFrame == 0)
      {
        completeLitFrame = i + 1;
      }
    }
    firstLitMs = msOf(firstLitFrame);
    completeLitMs = msOf(completeLitFrame);
    if (const auto* r = record(firstLitFrame))
    {
      compileAtFirstLit = r->compile;
      if (r->compile.lastJobEnd)
      {
        compileEndAtFirstLitMs =
          std::chrono::duration<double, std::milli>(*r->compile.lastJobEnd - pressed)
            .count();
      }
    }
  }
};

double msOrMinusOne(
  const std::optional<Clock::time_point>& at, const Clock::time_point from)
{
  return at ? millisecondsBetween(from, *at) : -1.0;
}

// a frame log row's unsigned count, 0 when empty
std::uint64_t countOf(const LogRow& row, const std::size_t column)
{
  return std::uint64_t(row.number(column).value_or(0.0));
}

std::string pass(const bool passed)
{
  return passed ? "pass" : "MISS";
}

/**
 * The window of the measurement: the panel alone in a frameless top-level window, so that
 * its surface can be made 1920 × 1080 pixels on a 1080p screen (a framed window may not
 * be larger than the screen); the surface reaches below the screen's edge. It stays on
 * top, so that it is seen also when Windows keeps the foreground with another process.
 */
class MeasureWindow
{
public:
  AppControllerFixture appControllerFixture;
  std::unique_ptr<MapDocument> document;
  QWidget toolPages;
  std::unique_ptr<MapViewToolBox> toolBox;
  Notifier<const gl::Camera&> cameraNotifier;
  std::unique_ptr<QWidget> window;
  LitPreviewPanel* panel = nullptr;
  gl::PerspectiveCamera camera;
  double loadMs = 0.0;

  // loads the map as MapWindowManager does, the format detected from its header
  MeasureWindow(const mdl::GameInfo& gameInfo, const std::filesystem::path& mapPath)
  {
    auto& appController = appControllerFixture.appController();
    const auto loadStart = Clock::now();
    document = MapDocument::loadDocument(
                 appController.environmentConfig(),
                 gameInfo,
                 mdl::MapFormat::Unknown,
                 MapDocument::worldBoundsForGame(gameInfo.gameConfig),
                 mapPath,
                 appController.taskManager(),
                 appController.glManager().resourceManager())
               | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    loadMs = millisecondsBetween(loadStart, Clock::now());
    toolBox = std::make_unique<MapViewToolBox>(*document, new QStackedLayout{&toolPages});
    window = std::make_unique<QWidget>(
      nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window->setObjectName("LitMeasureWindow");
    auto* layout = new QVBoxLayout{window.get()};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    panel = new LitPreviewPanel{SidebarContext{
      appControllerFixture.appController(), *document, *toolBox, cameraNotifier}};
    layout->addWidget(panel);
    camera.setFov(90.0f);
    camera.setViewport(gl::Camera::Viewport{0, 0, SurfaceWidth, SurfaceHeight});
  }

  ~MeasureWindow()
  {
    // the panel's teardown before the document's
    window.reset();
  }

  /**
   * Sizes the window so that the surface page's container will be SurfaceWidth ×
   * SurfaceHeight device pixels: the page's height less the status line and the page
   * layout's spacing, measured on the Start page, which has the stack's size.
   */
  void sizeForSurface()
  {
    const auto dpr = window->devicePixelRatioF();
    const auto width = int(std::lround(double(SurfaceWidth) / dpr));
    const auto height = int(std::lround(double(SurfaceHeight) / dpr));
    auto* startPage = panel->findChild<QWidget*>("LitPreviewPanel_StartPage");
    auto* surfacePage = panel->findChild<QWidget*>("LitPreviewPanel_SurfacePage");
    auto* status = panel->findChild<QLabel*>("LitPreviewPanel_Status");
    REQUIRE(startPage != nullptr);
    REQUIRE(surfacePage != nullptr);
    REQUIRE(status != nullptr);
    const auto spacing = std::max(surfacePage->layout()->spacing(), 0);
    for (auto i = 0; i < 3; ++i)
    {
      QApplication::processEvents();
      const auto page = startPage->size();
      const auto wantedPage = QSize{width, height + status->maximumHeight() + spacing};
      if (page == wantedPage)
      {
        break;
      }
      window->resize(window->size() + (wantedPage - page));
      window->layout()->activate();
    }
  }

  void notifyCamera() { cameraNotifier(camera); }
};

void printForegroundLine(QWidget& window)
{
  window.activateWindow();
  static_cast<void>(QTest::qWaitForWindowActive(&window));
  QTest::qWait(200);
  auto foregroundProcess = DWORD{0};
  if (const auto foreground = GetForegroundWindow())
  {
    GetWindowThreadProcessId(foreground, &foregroundProcess);
  }
  std::cout << "window-system warm-up: the foreground window "
            << (foregroundProcess == GetCurrentProcessId()
                  ? "belongs to this process"
                  : "does not belong to this process (Windows refused it): repeat "
                    "the run")
            << "\n";
}


/**
 * Counts the validation layer's messages (Debug: Qt's messenger logs them as "vkDebug:"),
 * apart from M-PIPE-DEFECT-01's: the engine's whole vertex input against shaders that
 * read fewer locations, accepted as the engine's own behaviour, with the layer's notice
 * that it stops repeating them (Qt's instance keeps the layer's duplicate limit of 10).
 */
class ValidationCount
{
private:
  static inline std::atomic<int> s_count = 0;
  static inline std::atomic<int> s_mirrored = 0;
  static inline QtMessageHandler s_previous = nullptr;

  static void handler(
    const QtMsgType type, const QMessageLogContext& context, const QString& message)
  {
    if (message.startsWith("vkDebug:"))
    {
      if (
        message.contains("not consumed by vertex shader")
        || message.contains("duplicate_message_limit"))
      {
        ++s_mirrored;
      }
      else
      {
        ++s_count;
        std::cout << message.toStdString() << "\n";
      }
    }
    if (s_previous)
    {
      s_previous(type, context, message);
    }
  }

public:
  ValidationCount()
  {
    s_count = 0;
    s_mirrored = 0;
    s_previous = qInstallMessageHandler(&handler);
  }

  ~ValidationCount() { qInstallMessageHandler(s_previous); }

  ValidationCount(const ValidationCount&) = delete;
  ValidationCount& operator=(const ValidationCount&) = delete;

  int count() const { return s_count; }
  int mirrored() const { return s_mirrored; }
};

// what the loaded document holds
struct DocumentCounts
{
  std::size_t entities = 0;
  std::size_t brushes = 0;
  std::size_t patches = 0;
};

void countNodes(const mdl::Node& node, DocumentCounts& counts)
{
  for (const auto* child : node.children())
  {
    if (dynamic_cast<const mdl::EntityNode*>(child))
    {
      ++counts.entities;
    }
    else if (dynamic_cast<const mdl::BrushNode*>(child))
    {
      ++counts.brushes;
    }
    else if (dynamic_cast<const mdl::PatchNode*>(child))
    {
      ++counts.patches;
    }
    countNodes(*child, counts);
  }
}

std::string mb(const double bytes)
{
  return fmt::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
}

std::string msText(const double ms)
{
  return ms < 0 ? std::string{"-"} : fmt::format("{:.0f} ms", ms);
}

// the textures a Start loaded until it settled, and what it uploaded meanwhile
struct Loaded
{
  std::uint64_t textures = 0;
  std::uint64_t uploadedBytes = 0;
  std::uint64_t arenaBytes = 0;
  std::uint64_t allocations = 0;
};

Loaded loadedBy(const std::vector<LogRow>& log, const StartMeasure& measure)
{
  const auto rows = sessionFrames(log, measure.generation);
  auto loaded = Loaded{};
  for (std::size_t i = 0; i < rows.size() && i < measure.settledFrame; ++i)
  {
    loaded.uploadedBytes += countOf(rows[i], column::BytesUploaded);
  }
  if (measure.settledFrame > 0 && measure.settledFrame <= rows.size())
  {
    const auto& row = rows[measure.settledFrame - 1];
    loaded.textures = countOf(row, column::TexturesResident);
    loaded.arenaBytes = countOf(row, column::ArenaBytes);
    loaded.allocations = countOf(row, column::AllocCount);
  }
  return loaded;
}

std::uint64_t peakAllocations(const std::vector<LogRow>& log, const StartMeasure& measure)
{
  auto peak = std::uint64_t{0};
  for (const auto& row : sessionFrames(log, measure.generation))
  {
    peak = std::max(peak, countOf(row, column::AllocCount));
  }
  return peak;
}

} // namespace

TEST_CASE("LitPreviewPanel measured on a retail map", "[.][measure]")
{
  const auto gamePathValue = qEnvironmentVariable("TB_LIT_GAME_PATH");
  if (gamePathValue.isEmpty())
  {
    SKIP("TB_LIT_GAME_PATH is not set: the measurement needs the game folder");
  }
  const auto runStart = Clock::now();
  const auto validation = ValidationCount{};
  // every line at once, so that a run stopped at its bound keeps what it printed
  std::cout << std::unitbuf;
  const auto restoreBuffering = kdl::invoke_later{[] { std::cout << std::nounitbuf; }};
  const auto gamePath = pathFromQString(gamePathValue);
  const auto out = outputFolder();
  std::filesystem::create_directories(out);
  const auto frameLogPath = out / "frame-log.csv";
  const auto framesPath = out / "measure-frames.csv";
  const auto summaryPath = out / "measure-summary.md";
  for (const auto& path : {frameLogPath, framesPath, summaryPath})
  {
    std::filesystem::remove(path);
  }

  // ----- the map, extracted from the game's containers (read only) -----
  auto mapText = std::string{};
  auto found = lit::GameFileProbe{};
  {
    auto source = lit::GameDeclSource{gamePath};
    auto text = source.readGameFile(std::string{MapName} + ".map", &found);
    REQUIRE(text);
    mapText = std::move(*text);
    source.closeFiles();
  }
  const auto where = found.kind == lit::GameFileProbe::Kind::Loose ? std::string{"loose"}
                     : found.kind == lit::GameFileProbe::Kind::Pk4
                       ? "the .pk4 " + found.container
                       : "the container " + found.container;
  std::cout << fmt::format("{}.map: {} bytes from {}\n", MapName, mapText.size(), where);

  auto entities = std::vector<Version2Entity>{};
  try
  {
    entities = parseVersion2Map(mapText);
  }
  catch (const std::exception& e)
  {
    FAIL("the map does not parse as Version 2: " << e.what());
  }
  auto materials = std::set<std::string>{};
  auto playerStart = std::optional<Version2Entity>{};
  for (const auto& entity : entities)
  {
    if (const auto* classname = entity.find("classname");
        classname && *classname == "info_player_start" && !playerStart)
    {
      playerStart = entity;
    }
    for (const auto& primitive : entity.primitives)
    {
      for (const auto& side : primitive.sides)
      {
        materials.insert(side.material);
      }
    }
  }
  REQUIRE(playerStart);
  const auto convertStart = Clock::now();
  const auto sizes = editorImageSizes(gamePath, materials);
  auto conversion = ConversionStats{};
  const auto converted = convertToValve220(entities, sizes, conversion);
  const auto mapFolder =
    std::filesystem::temp_directory_path() / "TbUiLibTest-lit-measure-map";
  std::filesystem::create_directories(mapFolder);
  const auto removeMapFolder =
    kdl::invoke_later{[&] { std::filesystem::remove_all(mapFolder); }};
  const auto mapPath = mapFolder / "admin.map";
  {
    auto file = std::ofstream{mapPath, std::ios::binary};
    file << converted;
  }
  const auto conversionLine = fmt::format(
    "converted to Valve 220 in {:.0f} ms: {} entities, {} brushes ({} sides), {} "
    "patches, {} side materials ({} sized by the game), {} brush entities baked ({} "
    "turned), {} lights split",
    millisecondsBetween(convertStart, Clock::now()),
    conversion.entities,
    conversion.brushes,
    conversion.sides,
    conversion.patches,
    materials.size(),
    sizes.size(),
    conversion.transformedEntities,
    conversion.rotatedEntities,
    conversion.splitLights);
  std::cout << conversionLine << "\n";

  // ----- the document, with the game's own config, as the user loads it -----
  const auto configPath = getFixtureRoot() / "games" / "Doom3BFG" / "GameConfig.cfg";
  auto configText = std::string{};
  {
    auto stream = std::ifstream{configPath, std::ios::binary};
    REQUIRE(stream);
    configText.assign(std::istreambuf_iterator<char>{stream}, {});
  }
  auto gameInfo = mdl::makeGameInfo(
    mdl::parseGameConfig(configText, configPath)
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value());
  auto& prefs = PreferenceManager::instance();
  setPref(gameInfo.gamePathPreference, gamePath);
  const auto resetGamePath =
    kdl::invoke_later{[&] { prefs.resetToDefault(gameInfo.gamePathPreference); }};

  auto harness = MeasureWindow{gameInfo, mapPath};
  REQUIRE(harness.document->map().gamePath() == gamePath);
  auto documentCounts = DocumentCounts{};
  countNodes(harness.document->map().worldNode(), documentCounts);
  const auto loadLine = fmt::format(
    "TrenchBroom loaded it in {:.0f} ms ({} format): {} entities besides worldspawn, {} "
    "brushes, {} patches",
    harness.loadMs,
    mdl::formatName(harness.document->map().worldNode().mapFormat()),
    documentCounts.entities,
    documentCounts.brushes,
    documentCounts.patches);
  std::cout << loadLine << "\n";
  auto& panel = *harness.panel;

  // the camera at the player's start, at eye height, looking along its angle
  const auto startOrigin = vectorOf(playerStart->find("origin"));
  const auto startYaw =
    playerStart->find("angle") ? std::stod(*playerStart->find("angle")) : 0.0;
  const auto eye = vm::vec3f{
    float(startOrigin[0]), float(startOrigin[1]), float(startOrigin[2]) + EyeHeight};
  const auto yawDirection = [](const double degrees) {
    const auto radians = degrees * std::numbers::pi / 180.0;
    return vm::vec3f{float(std::cos(radians)), float(std::sin(radians)), 0.0f};
  };
  harness.camera.moveTo(eye);
  harness.camera.setDirection(yawDirection(startYaw), vm::vec3f{0, 0, 1});
  harness.notifyCamera();
  std::cout << fmt::format(
    "camera: info_player_start ({} {} {}) + {} up, yaw {}\n",
    startOrigin[0],
    startOrigin[1],
    startOrigin[2],
    EyeHeight,
    startYaw);

  harness.window->move(0, 0);
  harness.window->resize(SurfaceWidth, SurfaceHeight + 120);
  harness.window->show();
  REQUIRE(QTest::qWaitForWindowExposed(harness.window.get()));
  harness.sizeForSurface();
  printForegroundLine(*harness.window);

  qputenv("TB_LIT_FRAME_LOG", pathAsQString(frameLogPath).toUtf8());
  lit::LitSession::setMapPathOverrideForTesting(MapOverridePath);
  const auto unsetHooks = kdl::invoke_later{[] {
    qunsetenv("TB_LIT_FRAME_LOG");
    lit::LitSession::setMapPathOverrideForTesting(nullptr);
  }};

  auto reports = std::vector<LitPreviewReport>{};
  auto connection = NotifierConnection{};
  connection += panel.sessionDidEndNotifier.connect(
    [&](const LitPreviewReport& report) { reports.push_back(report); });

  auto sampler = Sampler{panel};
  auto recordsCsv = std::string{
    "start,frame,ms,settled,lights_visible,lights_drawn,lights_overflowed,draws,"
    "skipped_draws,pending_pipelines,vram_account_mb,geometry_batches,snapshots,"
    "table_version,compile_jobs,compile_job_ms,compile_pipelines,compile_pipeline_ms\n"};
  const auto keepRecords = [&](const StartMeasure& measure) {
    for (const auto& record : sampler.frames)
    {
      recordsCsv += fmt::format(
        "{},{},{:.1f},{},{},{},{},{},{},{},{:.1f},{},{},{},{},{:.1f},{},{:.1f}\n",
        measure.label,
        record.status.frames,
        millisecondsBetween(measure.pressed, record.at),
        record.status.settled ? 1 : 0,
        record.status.lightsVisible,
        record.status.lightsDrawn,
        record.status.lightsOverflowed,
        record.status.draws,
        record.status.skippedDraws,
        record.status.pendingPipelines,
        double(record.status.vramBytes) / (1024.0 * 1024.0),
        record.geometryBatches,
        record.snapshotsTaken,
        record.tableVersion,
        record.compile.jobs,
        record.compile.jobMs,
        record.compile.pipelines,
        record.compile.pipelineMs);
    }
  };

  // one Start: the press, a lit frame that skipped no draw, then settled for good
  const auto startPreview = [&](const std::string& label) {
    sampler.reset();
    auto measure = StartMeasure{};
    measure.label = label;
    measure.pressed = Clock::now();
    panel.start();
    measure.phaseAMs = millisecondsBetween(measure.pressed, Clock::now());
    REQUIRE(sampler.runUntil(
      [&] {
        return std::ranges::any_of(sampler.frames, [&](const auto& r) {
          return sampler.isLit(r) && r.status.skippedDraws == 0;
        });
      },
      60s));
    REQUIRE(sampler.runUntil([&] { return sampler.quietlySettled(); }, 90s));
    auto* session = panel.sessionForTesting();
    REQUIRE(session != nullptr);
    measure.generation = session->generation();
    measure.records = sampler.frames;
    measure.sessionMs = msOrMinusOne(sampler.sessionSeen, measure.pressed);
    measure.firstTableMs = msOrMinusOne(sampler.tableSeen, measure.pressed);
    measure.firstSnapshotMs = msOrMinusOne(sampler.snapshotSeen, measure.pressed);
    measure.firstBatchMs = msOrMinusOne(sampler.batchSeen, measure.pressed);
    measure.firstFrameMs =
      millisecondsBetween(measure.pressed, sampler.frames.front().at);
    measure.findSettled();
    REQUIRE(measure.settledFrame > 0);
    measure.compileAtSettled = session->compileTimingsForTesting();
    if (measure.compileAtSettled.lastJobEnd)
    {
      measure.lastCompileEndMs =
        millisecondsBetween(measure.pressed, *measure.compileAtSettled.lastJobEnd);
    }
    std::cout << fmt::format(
      "{} Start: first frame {}, first table {}, settled for good {} (frame {})\n",
      label,
      msText(measure.firstFrameMs),
      msText(measure.firstTableMs),
      msText(measure.settledMs),
      measure.settledFrame);
    return measure;
  };

  const auto stopPreview = [&] {
    panel.stop();
    REQUIRE(!reports.empty());
    const auto& report = reports.back();
    CHECK(report.problems().empty());
    CHECK(report.deviceReleaseCount == 0u);
    return lit::LitSession::liveDeviceMemoryAllocationsForTesting();
  };

  // ----- the first Start: the driver's shader cache as it is -----
  auto first = startPreview("first");
  {
    auto* container = panel.findChild<QWidget*>("LitPreviewPanel_Container");
    REQUIRE(container != nullptr);
    const auto pixels = container->size() * container->devicePixelRatioF();
    std::cout << fmt::format(
      "surface {}x{} pixels (dpr {})\n",
      pixels.width(),
      pixels.height(),
      container->devicePixelRatioF());
  }
  auto vramAtTextures = std::optional<lit::VramUsage>{};
  if (auto* session = panel.sessionForTesting())
  {
    vramAtTextures = session->vramForTesting();
  }

  // the orbit: a circle around the eye, looking outward, once round in OrbitTime, the
  // camera moved through the notifier at the pacer's rate
  const auto orbitStart = Clock::now();
  auto nextMove = orbitStart;
  auto lastMove = orbitStart;
  auto moves = 0;
  sampler.tick = [&](const Clock::time_point now) {
    if (now < nextMove || now - orbitStart >= OrbitTime)
    {
      return;
    }
    const auto turn = std::chrono::duration<double>(now - orbitStart)
                      / std::chrono::duration<double>(OrbitTime);
    const auto outward = yawDirection(startYaw + 360.0 * turn);
    harness.camera.moveTo(eye + OrbitRadius * outward);
    harness.camera.setDirection(outward, vm::vec3f{0, 0, 1});
    harness.notifyCamera();
    lastMove = now;
    ++moves;
    nextMove += std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / double(OrbitRate)));
  };
  sampler.runFor(OrbitTime + 100ms);
  sampler.tick = {};
  // the frames shown while the camera moved, and the one that drew its last move
  auto orbitFirst = std::uint64_t{0};
  auto orbitLast = std::uint64_t{0};
  auto orbitIntervals = std::vector<double>{};
  {
    auto previous = std::optional<Clock::time_point>{};
    for (const auto& record : sampler.frames)
    {
      if (record.at > orbitStart && record.at <= lastMove + 40ms)
      {
        orbitFirst = orbitFirst == 0 ? record.status.frames : orbitFirst;
        orbitLast = record.status.frames;
        if (previous)
        {
          orbitIntervals.push_back(millisecondsBetween(*previous, record.at));
        }
        previous = record.at;
      }
    }
  }
  REQUIRE(sampler.runUntil([&] { return sampler.quietlySettled(false); }, 60s));
  // §10's settled exact frame: from the last camera move to the first frame after it
  // that is settled and stays so (every frame of the orbit may be settled already)
  const auto afterLastMove =
    std::ranges::find_if(sampler.frames, [&](const auto& r) { return r.at > lastMove; });
  REQUIRE(afterLastMove != sampler.frames.end());
  auto settledAfterMove = afterLastMove;
  for (auto it = afterLastMove; it != sampler.frames.end(); ++it)
  {
    if (!it->status.settled)
    {
      settledAfterMove = std::next(it);
    }
  }
  REQUIRE(settledAfterMove != sampler.frames.end());
  const auto settleAfterMoveMs = millisecondsBetween(lastMove, settledAfterMove->at);
  const auto framesToSettle =
    std::size_t(std::distance(afterLastMove, settledAfterMove)) + 1;

  // idle: no trigger, no frame
  const auto framesBeforeIdle = sampler.lastFrames;
  sampler.runFor(IdleTime);
  const auto idleFrames = sampler.lastFrames - framesBeforeIdle;
  const auto firstVramPeak = sampler.vramPeak;
  const auto firstAccountPeak = sampler.accountPeak;
  keepRecords(first);
  const auto allocationsAfterFirstStop = stopPreview();
  const auto firstReport = reports.back();

  // ----- the second Start: warm; then a material reload in the running session -----
  auto second = startPreview("second");
  auto* secondSession = panel.sessionForTesting();
  REQUIRE(secondSession != nullptr);
  const auto reloadAt = Clock::now();
  const auto batchesBeforeReload = secondSession->geometryBatchesForTesting();
  const auto tableBeforeReload = panel.materialsVersionForTesting();
  const auto recordsBeforeReload = sampler.frames.size();
  harness.document->materialCollectionsDidChangeNotifier();
  const auto afterReload = [&](const FrameRecord& record) {
    return record.tableVersion > tableBeforeReload
           && record.geometryBatches > batchesBeforeReload && sampler.isLit(record);
  };
  auto reloadFirstMs = -1.0;
  REQUIRE(sampler.runUntil(
    [&] {
      for (auto i = recordsBeforeReload; i < sampler.frames.size(); ++i)
      {
        if (afterReload(sampler.frames[i]))
        {
          reloadFirstMs = millisecondsBetween(reloadAt, sampler.frames[i].at);
          return true;
        }
      }
      return false;
    },
    60s));
  REQUIRE(sampler.runUntil(
    [&] { return sampler.quietlySettled() && afterReload(sampler.frames.back()); }, 60s));
  // settled for good after the reload, as a Start's (StartMeasure::findSettled)
  auto reload = StartMeasure{};
  reload.pressed = reloadAt;
  reload.records.assign(
    sampler.frames.begin() + std::ptrdiff_t(recordsBeforeReload), sampler.frames.end());
  reload.findSettled();
  const auto reloadSettledMs = reload.settledMs;
  std::cout << fmt::format(
    "material reload: the first frame on the rebuilt chunks after {:.0f} ms, settled "
    "after {:.0f} ms\n",
    reloadFirstMs,
    reloadSettledMs);
  const auto secondVramPeak = sampler.vramPeak;
  keepRecords(second);
  const auto allocationsAfterSecondStop = stopPreview();

  // ----- the third Start -----
  auto third = startPreview("third");
  keepRecords(third);
  const auto allocationsAfterThirdStop = stopPreview();
  const auto runSeconds = std::chrono::duration<double>(Clock::now() - runStart).count();

  // ----- the numbers, from the frame log -----
  const auto log = readFrameLog(frameLogPath);
  REQUIRE(!log.empty());
  REQUIRE(log.front().fields.size() == column::Count);
  for (auto* measure : {&first, &second, &third})
  {
    measure->findLit(sessionFrames(log, measure->generation));
  }
  const auto firstRows = sessionFrames(log, first.generation);
  REQUIRE(orbitFirst > 0);
  REQUIRE(firstRows.size() >= orbitLast);
  const auto orbitBegin = std::size_t(orbitFirst - 1);
  const auto orbitEnd = std::size_t(orbitLast - 1);
  const auto orbit = [&](const std::size_t c) {
    return columnOf(firstRows, c, orbitBegin, orbitEnd);
  };
  const auto cpu = orbit(column::CpuRecordMs);
  const auto gpu = orbit(column::GpuFrameMs);
  // the CPU record time against the draws a frame records: a least-squares line
  auto perDrawUs = 0.0;
  auto perFrameMs = 0.0;
  {
    auto n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (auto i = orbitBegin; i <= orbitEnd; ++i)
    {
      const auto x = firstRows[i].number(column::Draws);
      const auto y = firstRows[i].number(column::CpuRecordMs);
      if (x && y)
      {
        n += 1.0;
        sx += *x;
        sy += *y;
        sxx += *x * *x;
        sxy += *x * *y;
      }
    }
    if (const auto d = n * sxx - sx * sx; n > 1.0 && d != 0.0)
    {
      const auto slope = (n * sxy - sx * sy) / d;
      perDrawUs = slope * 1000.0;
      perFrameMs = (sy - slope * sx) / n;
    }
  }

  const auto& sizeRow = firstRows[orbitBegin];
  const auto frameWidth = sizeRow.at(column::Width);
  const auto frameHeight = sizeRow.at(column::Height);
  CHECK(frameWidth == std::to_string(SurfaceWidth));
  CHECK(frameHeight == std::to_string(SurfaceHeight));

  // ----- the summary -----
  auto summary =
    fmt::format("# LP2.4 measurement: {} at {}x{}\n\n", MapName, frameWidth, frameHeight);
  const auto gaps = distributionOf(sampler.gaps);
  summary += fmt::format(
    "Map: {} bytes from {}; {}. {}. Run {:.0f} s; "
    "the GUI thread sampled the session {:.2f} ms apart (median), {:.2f} ms (p95).\n\n",
    mapText.size(),
    where,
    conversionLine,
    loadLine,
    runSeconds,
    gaps.median,
    gaps.p95);
  summary += "| §10 case | measured | target | verdict |\n|---|---|---|---|\n";
  for (const auto* measure : {&first, &second, &third})
  {
    summary += fmt::format(
      "| Start → first lit frame, {} Start | {} (frame {}; no draw skipped from {}, "
      "frame {}) | ≤ 1 s warm, ≤ 5 s cold | {} |\n",
      measure->label,
      msText(measure->firstLitMs),
      measure->firstLitFrame,
      msText(measure->completeLitMs),
      measure->completeLitFrame,
      pass(measure->firstLitMs <= 1000.0));
  }
  summary += fmt::format(
    "| material reload → first frame on the rebuilt chunks; → settled | {}; {} | — | — "
    "|\n",
    msText(reloadFirstMs),
    msText(reloadSettledMs));
  for (const auto* measure : {&first, &second, &third})
  {
    const auto loaded = loadedBy(log, *measure);
    summary += fmt::format(
      "| Start → visible textures loaded (settled for good), {} Start | {} (frame {}; "
      "{} textures resident, {} uploaded, arenas {}; D77's first settled frame {}, "
      "frame {}) | set with the user | — |\n",
      measure->label,
      msText(measure->settledMs),
      measure->settledFrame,
      loaded.textures,
      mb(double(loaded.uploadedBytes)),
      mb(double(loaded.arenaBytes)),
      msText(measure->firstSettledMs),
      measure->firstSettledFrame);
  }
  summary += fmt::format(
    "| Idle, preview open | {} frames in {} s | zero frames | {} |\n",
    idleFrames,
    std::chrono::duration_cast<std::chrono::seconds>(IdleTime).count(),
    pass(idleFrames == 0));
  const auto orbitFrames = orbitEnd - orbitBegin + 1;
  const auto orbitSeconds = std::chrono::duration<double>(lastMove - orbitStart).count();
  const auto rate = orbitSeconds > 0 ? double(orbitFrames) / orbitSeconds : 0.0;
  const auto interval = distributionOf(orbitIntervals);
  summary += fmt::format(
    "| Camera moving: CPU record | median {:.2f} ms, p95 {:.2f} ms, max {:.2f} ms ({} "
    "frames; fitted {:.2f} ms + {:.2f} µs per draw) | ≤ 10 ms | {} |\n",
    cpu.median,
    cpu.p95,
    cpu.max,
    cpu.count,
    perFrameMs,
    perDrawUs,
    pass(cpu.p95 <= 10.0));
  // LP4.1 (D92): the start view's portal flow, from the first Start's settled frame
  if (first.settledFrame > 0 && first.settledFrame <= firstRows.size())
  {
    const auto& start = firstRows[first.settledFrame - 1];
    const auto field = [&](const std::size_t c) {
      const auto& text = start.at(c);
      return text.empty() ? std::string{"-"} : text;
    };
    summary += fmt::format(
      "| Start view: the portal flow (first Start, frame {}) | view area {}, {} areas "
      "flowed, {} lights in view, {} with interactions, {} drawn, {} portal chains; "
      "draws "
      "{}, CPU record {} ms; LP4.3: {} ambient draws ({} set-9 writes), {} surfaces on "
      "the light grid, {} textures resident | — | — |\n",
      first.settledFrame,
      field(column::ViewArea),
      field(column::AreasFlowed),
      field(column::LightsInView),
      field(column::LightsVisible),
      field(column::LightsDrawn),
      field(column::PortalChains),
      field(column::Draws),
      field(column::CpuRecordMs),
      field(column::AmbientDraws),
      field(column::AmbientWrites),
      field(column::GridSurfaces),
      field(column::TexturesResident));
  }
  {
    const auto flow = orbit(column::FlowMs);
    summary += fmt::format(
      "| Camera moving: the portal flow's time | median {:.3f} ms, p95 {:.3f} ms, max "
      "{:.3f} ms ({} frames) | — | — |\n",
      flow.median,
      flow.p95,
      flow.max,
      flow.count);
  }
  summary += fmt::format(
    "| Camera moving: GPU frame | median {:.2f} ms, p95 {:.2f} ms, max {:.2f} ms ({} "
    "timed) | ≤ 12 ms | {} |\n",
    gpu.median,
    gpu.p95,
    gpu.max,
    gpu.count,
    pass(gpu.p95 <= 12.0));
  summary += fmt::format(
    "| Camera moving: frame rate | {:.1f} Hz ({} frames for {} camera moves in {:.2f} "
    "s; frame interval median {:.1f} ms, p95 {:.1f} ms) | ≤ 30 Hz | {} |\n",
    rate,
    orbitFrames,
    moves,
    orbitSeconds,
    interval.median,
    interval.p95,
    pass(rate <= 30.5));
  summary += fmt::format(
    "| Settled exact frame after the camera stops | {} ({} frames) | ≤ 250 ms | {} |\n",
    msText(settleAfterMoveMs),
    framesToSettle,
    pass(settleAfterMoveMs <= 250.0));
  summary += fmt::format(
    "| Device memory allocations | peak {} / {} / {} (first, second, third Start); "
    "after Stop {} / {} / {} | far below 4096 | {} |\n",
    peakAllocations(log, first),
    peakAllocations(log, second),
    peakAllocations(log, third),
    allocationsAfterFirstStop,
    allocationsAfterSecondStop,
    allocationsAfterThirdStop,
    pass(peakAllocations(log, first) < 1024));
  const auto startRowOfFirst = sessionRow(log, first.generation, "start");
  const auto stopRowOfFirst = sessionRow(log, first.generation, "stop");
  summary += fmt::format(
    "| VRAM (VK_EXT_memory_budget, the process), first Start | start row {} MB, when "
    "settled {}, peak {}, stop row {} MB, after the release {} | — | — |\n",
    startRowOfFirst ? startRowOfFirst->at(column::VramUsedMb) : std::string{"-"},
    vramAtTextures ? mb(double(vramAtTextures->usedBytes)) : std::string{"-"},
    mb(double(firstVramPeak)),
    stopRowOfFirst ? stopRowOfFirst->at(column::VramUsedMb) : std::string{"-"},
    firstReport.vramAtStopBytes ? mb(double(*firstReport.vramAtStopBytes))
                                : std::string{"-"});
  summary += fmt::format(
    "| VRAM (the session's own account, GpuMemoryAccount) | peak {} (second Start's "
    "process peak {}) | — | — |\n\n",
    mb(double(firstAccountPeak)),
    mb(double(secondVramPeak)));

  summary += "Camera moving, per frame (median / p95 / max, count):\n\n";
  const auto line = [&](const std::string& what, const std::size_t c) {
    const auto d = orbit(c);
    summary += fmt::format(
      "- {}: {:.3f} / {:.3f} / {:.3f} ({})\n", what, d.median, d.p95, d.max, d.count);
    return d;
  };
  line("GPU prepass ms", column::GpuPrepassMs);
  line("GPU interactions ms", column::GpuInteractionsMs);
  line("GPU tonemap ms", column::GpuTonemapMs);
  line("GPU blit ms", column::GpuBlitMs);
  line("acquire ms", column::AcquireMs);
  line("present ms", column::PresentMs);
  line("draws", column::Draws);
  line("prepass draws", column::PrepassDraws);
  line("interaction draws", column::InteractionDraws);
  line("skipped draws", column::SkippedDraws);
  line("pending pipelines", column::PendingPipelines);
  line("lights visible", column::LightsVisible);
  line("lights drawn", column::LightsDrawn);
  // LP4.1: the view's portal flow
  line("lights in view (the flow's)", column::LightsInView);
  line("areas flowed", column::AreasFlowed);
  line("portal chains", column::PortalChains);
  line("flow ms", column::FlowMs);
  const auto overflowed = line("lights overflowed", column::LightsOverflowed);
  line("bytes uploaded", column::BytesUploaded);
  line("volatile writes (D76's basis)", column::VolatileWrites);
  line("constant bytes", column::ConstantBytes);
  // LP4.3 (D86, D92): the ambient pass's draws, GPU time and set-9 writes, and the
  // surfaces on the light grid
  line("ambient draws", column::AmbientDraws);
  line("GPU ambient ms", column::GpuAmbientMs);
  const auto ambientWrites = line("set-9 volatile writes (D86)", column::AmbientWrites);
  line("surfaces on the light grid", column::GridSurfaces);
  line("textures resident", column::TexturesResident);
  const auto writeBudget = firstRows[orbitBegin].number(column::VolatileWriteBudget);
  // D86 with D76's rule: set 9's own buffer keeps maxVersions 8192 while the most writes
  // a frame made, doubled with a margin of 64, stay within the frame's budget
  summary += fmt::format(
    "- set 9's writes per frame: max {:.0f}, p95 {:.0f} against the frame's budget {}: "
    "D76's rule (max x 2 + 64) {} -> {}\n",
    ambientWrites.max,
    ambientWrites.p95,
    writeBudget ? fmt::format("{:.0f}", *writeBudget) : std::string{"-"},
    ambientWrites.max * 2.0 + 64.0,
    writeBudget && ambientWrites.max * 2.0 + 64.0 <= *writeBudget
      ? "maxVersions 8192 stays"
      : "raise maxVersions");
  summary += fmt::format(
    "- lights overflowed over the orbit: {:.0f}; the volatile writes a frame may make: "
    "{}\n",
    overflowed.sum,
    writeBudget ? fmt::format("{:.0f}", *writeBudget) : std::string{"-"});
  summary += "\nStarts (ms after the press):\n\n";
  for (const auto* measure : {&first, &second, &third})
  {
    summary += fmt::format(
      "- {}: phase A {:.0f}, session {}, first frame {}, first table {}, first snapshot "
      "{}, first batch {}, first lit frame {} (frame {}), first settled {}, settled for "
      "good {} (frame {}); the compile thread by the first lit frame: {} jobs, {:.0f} ms "
      "busy, {} pipelines {:.0f} ms, {} shaders {:.0f} ms, its last job so far ended at "
      "{}; by the end: {} jobs, {:.0f} ms, {} pipelines {:.0f} ms, its last job ended "
      "at {}\n",
      measure->label,
      measure->phaseAMs,
      msText(measure->sessionMs),
      msText(measure->firstFrameMs),
      msText(measure->firstTableMs),
      msText(measure->firstSnapshotMs),
      msText(measure->firstBatchMs),
      msText(measure->firstLitMs),
      measure->firstLitFrame,
      msText(measure->firstSettledMs),
      msText(measure->settledMs),
      measure->settledFrame,
      measure->compileAtFirstLit.jobs,
      measure->compileAtFirstLit.jobMs,
      measure->compileAtFirstLit.pipelines,
      measure->compileAtFirstLit.pipelineMs,
      measure->compileAtFirstLit.shaders,
      measure->compileAtFirstLit.shaderMs,
      msText(measure->compileEndAtFirstLitMs),
      measure->compileAtSettled.jobs,
      measure->compileAtSettled.jobMs,
      measure->compileAtSettled.pipelines,
      measure->compileAtSettled.pipelineMs,
      msText(measure->lastCompileEndMs));
  }
#ifdef NDEBUG
  const auto build = std::string{"Release: no validation layer"};
#else
  const auto build = std::string{"Debug: the layer is on"};
#endif
  summary += fmt::format(
    "\nValidation messages: {} ({}); M-PIPE-DEFECT-01's mirrored warnings apart: {}.\n",
    validation.count(),
    build,
    validation.mirrored());

  {
    auto file = std::ofstream{summaryPath, std::ios::binary};
    file << summary;
  }
  {
    auto file = std::ofstream{framesPath, std::ios::binary};
    file << recordsCsv;
  }
  std::cout << "\n" << summary << "\n";
  std::cout << "written: " << summaryPath.string() << ", " << framesPath.string() << ", "
            << frameLogPath.string() << "\n";

  CHECK(idleFrames == 0u);
  CHECK(allocationsAfterFirstStop == 0);
  CHECK(allocationsAfterSecondStop == 0);
  CHECK(allocationsAfterThirdStop == 0);
#ifndef NDEBUG
  CHECK(validation.count() == 0);
#endif
}

} // namespace tb::ui

#endif
