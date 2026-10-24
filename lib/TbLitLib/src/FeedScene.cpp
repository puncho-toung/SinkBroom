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

#include "lit/FeedScene.h"

#include "lit/NumberText.h"

#include "vm/plane.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>

namespace tb::lit
{
namespace
{

// R3.10: a value as the editor's serialiser writes it (fmt's "{}") and the engine's
// idLexer reads it back (LitSnapshotBuilder.cpp's readBack)
float readBack(const double value)
{
  auto buffer = std::array<char, 64>{};
  const auto written = fmt::format_to_n(buffer.data(), buffer.size(), "{}", value);
  const auto text =
    std::string_view{buffer.data(), std::min(written.size, buffer.size())};
  return lexerFloat(text).value_or(float(value));
}

vm::vec3d readBack(const vm::vec3d& v)
{
  return {double(readBack(v.x())), double(readBack(v.y())), double(readBack(v.z()))};
}

vm::vec3f toFloat(const vm::vec3d& v)
{
  return vm::vec3f{float(v.x()), float(v.y()), float(v.z())};
}

SnapshotString intern(Snapshot& snapshot, const std::string_view text)
{
  const auto offset = std::uint32_t(snapshot.strings.size());
  snapshot.strings += text;
  return SnapshotString{offset, std::uint32_t(text.size())};
}

std::uint32_t count(const std::size_t size)
{
  return std::uint32_t(size);
}

// ----- the map text -----

/**
 * The map's tokens: quoted strings, the six brackets, and runs of anything else; "//"
 * comments skipped. Every call advances or fails, and a failure is sticky (P5.11): the
 * readers below stop at the first one.
 */
class Tokens
{
private:
  std::string_view m_text;
  std::size_t m_pos = 0;
  std::size_t m_line = 1;
  std::optional<std::string> m_error;

  void advance()
  {
    if (m_text[m_pos] == '\n')
    {
      ++m_line;
    }
    ++m_pos;
  }

public:
  explicit Tokens(const std::string_view text)
    : m_text{text}
  {
  }

  bool failed() const { return m_error.has_value(); }
  const std::optional<std::string>& error() const { return m_error; }

  void fail(const std::string_view what)
  {
    if (!m_error)
    {
      m_error = fmt::format("line {}: {}", m_line, what);
    }
  }

  // the next token, or nullopt at the end of the text (or after a failure)
  std::optional<std::string> next()
  {
    if (failed())
    {
      return std::nullopt;
    }
    for (;;)
    {
      while (m_pos < m_text.size()
             && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
      {
        advance();
      }
      if (m_pos + 1 < m_text.size() && m_text[m_pos] == '/' && m_text[m_pos + 1] == '/')
      {
        while (m_pos < m_text.size() && m_text[m_pos] != '\n')
        {
          advance();
        }
        continue;
      }
      break;
    }
    if (m_pos >= m_text.size())
    {
      return std::nullopt;
    }
    const auto c = m_text[m_pos];
    if (c == '"')
    {
      const auto end = m_text.find('"', m_pos + 1);
      if (end == std::string_view::npos)
      {
        fail("a quoted string is not closed");
        m_pos = m_text.size();
        return std::nullopt;
      }
      auto token = std::string{m_text.substr(m_pos + 1, end - m_pos - 1)};
      while (m_pos <= end)
      {
        advance();
      }
      return token;
    }
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
    {
      advance();
      return std::string{c};
    }
    const auto start = m_pos;
    while (m_pos < m_text.size()
           && !std::isspace(static_cast<unsigned char>(m_text[m_pos])))
    {
      advance();
    }
    return std::string{m_text.substr(start, m_pos - start)};
  }

  // the next token; at the end of the text a failure and ""
  std::string expect()
  {
    if (auto token = next())
    {
      return std::move(*token);
    }
    fail("unexpected end of the map");
    return {};
  }

  void expect(const std::string_view what)
  {
    if (const auto token = expect(); !failed() && token != what)
    {
      fail(fmt::format("expected {}, found {}", what, token));
    }
  }

  // R3.10: as the engine's idLexer reads the number back
  double number()
  {
    const auto token = expect();
    if (failed())
    {
      return 0.0;
    }
    if (const auto value = lexerFloat(token))
    {
      return double(*value);
    }
    fail(fmt::format("{} is not a number the engine reads", token));
    return 0.0;
  }
};

struct ParsedFace
{
  std::array<vm::vec3d, 3> points = {};
  std::string material;
  vm::vec3d u = vm::vec3d{0, 0, 0};
  vm::vec3d v = vm::vec3d{0, 0, 0};
  double uOffset = 0;
  double vOffset = 0;
  double uScale = 1;
  double vScale = 1;
};

struct ParsedPatch
{
  std::string material;
  std::size_t rows = 0;
  std::size_t columns = 0;
  bool explicitSubdivisions = false;
  std::size_t h = 0;
  std::size_t v = 0;
  std::vector<vm::vec<double, 5>> points;
};

struct ParsedEntity
{
  std::vector<std::pair<std::string, std::string>> keys;
  std::vector<std::vector<ParsedFace>> brushes;
  std::vector<ParsedPatch> patches;
  // the order of the primitives: true a brush, false a patch
  std::vector<bool> order;
};

vm::vec3d point(Tokens& tokens)
{
  tokens.expect("(");
  const auto x = tokens.number();
  const auto y = tokens.number();
  const auto z = tokens.number();
  tokens.expect(")");
  return {x, y, z};
}

// K19's side bound, so that a size token cannot ask for a huge allocation
constexpr auto MaxPatchSide = std::size_t{99};

ParsedPatch parsePatch(Tokens& tokens, const bool explicitSubdivisions)
{
  auto patch = ParsedPatch{};
  patch.explicitSubdivisions = explicitSubdivisions;
  tokens.expect("{");
  patch.material = tokens.expect();
  tokens.expect("(");
  const auto rows = tokens.number();
  const auto columns = tokens.number();
  if (explicitSubdivisions)
  {
    patch.h = std::size_t(std::max(tokens.number(), 0.0));
    patch.v = std::size_t(std::max(tokens.number(), 0.0));
  }
  if (
    !tokens.failed()
    && (rows < 1 || columns < 1 || rows > double(MaxPatchSide) || columns > double(MaxPatchSide)))
  {
    tokens.fail(fmt::format("a patch of {} x {} control points", rows, columns));
  }
  patch.rows = tokens.failed() ? 0 : std::size_t(rows);
  patch.columns = tokens.failed() ? 0 : std::size_t(columns);
  // the rest of the size block (patchDef2's three zeros, patchDef3's two)
  for (auto token = tokens.expect(); !tokens.failed() && token != ")";
       token = tokens.expect())
  {
  }
  tokens.expect("(");
  for (auto r = std::size_t{0}; r < patch.rows && !tokens.failed(); ++r)
  {
    tokens.expect("(");
    for (auto c = std::size_t{0}; c < patch.columns && !tokens.failed(); ++c)
    {
      tokens.expect("(");
      auto p = vm::vec<double, 5>{};
      for (auto k = std::size_t{0}; k < 5; ++k)
      {
        p[k] = tokens.number();
      }
      tokens.expect(")");
      patch.points.push_back(p);
    }
    tokens.expect(")");
  }
  tokens.expect(")");
  tokens.expect("}");
  tokens.expect("}");
  return patch;
}

std::vector<ParsedFace> parseBrush(Tokens& tokens, std::string first)
{
  auto faces = std::vector<ParsedFace>{};
  while (!tokens.failed() && first != "}")
  {
    if (first != "(")
    {
      tokens.fail(fmt::format("expected a face, found {}", first));
      break;
    }
    auto face = ParsedFace{};
    const auto x = tokens.number();
    const auto y = tokens.number();
    const auto z = tokens.number();
    tokens.expect(")");
    const auto p2 = point(tokens);
    const auto p3 = point(tokens);
    face.points = {vm::vec3d{x, y, z}, p2, p3};
    face.material = tokens.expect();
    tokens.expect("[");
    face.u = {tokens.number(), tokens.number(), tokens.number()};
    face.uOffset = tokens.number();
    tokens.expect("]");
    tokens.expect("[");
    face.v = {tokens.number(), tokens.number(), tokens.number()};
    face.vOffset = tokens.number();
    tokens.expect("]");
    static_cast<void>(tokens.number());
    face.uScale = tokens.number();
    face.vScale = tokens.number();
    faces.push_back(face);
    first = tokens.expect();
  }
  return faces;
}

std::vector<ParsedEntity> parseMap(Tokens& tokens)
{
  auto entities = std::vector<ParsedEntity>{};
  while (const auto token = tokens.next())
  {
    if (*token != "{")
    {
      tokens.fail(fmt::format("expected an entity, found {}", *token));
      break;
    }
    auto entity = ParsedEntity{};
    for (auto key = tokens.expect(); !tokens.failed() && key != "}";
         key = tokens.expect())
    {
      if (key == "{")
      {
        // a brush or a patch
        auto first = tokens.expect();
        if (first == "patchDef2" || first == "patchDef3")
        {
          entity.patches.push_back(parsePatch(tokens, first == "patchDef3"));
          entity.order.push_back(false);
          continue;
        }
        entity.brushes.push_back(parseBrush(tokens, std::move(first)));
        entity.order.push_back(true);
        continue;
      }
      auto value = tokens.expect();
      entity.keys.emplace_back(std::move(key), std::move(value));
    }
    entities.push_back(std::move(entity));
  }
  return entities;
}

// TrenchBroom's face plane: normal = (p3 - p1) x (p2 - p1), normalised
vm::plane3d planeOf(const std::array<vm::vec3d, 3>& p)
{
  const auto n = vm::normalize(vm::cross(p[2] - p[0], p[1] - p[0]));
  return vm::plane3d{vm::dot(n, p[0]), n};
}

constexpr auto PolygonEpsilon = 1e-6;

// the header's rule: the intersections of three planes inside every plane, merged
std::vector<vm::vec3d> polygonOf(
  const std::vector<vm::plane3d>& planes, const std::size_t face)
{
  auto points = std::vector<vm::vec3d>{};
  const auto& a = planes[face];
  for (auto j = std::size_t{0}; j < planes.size(); ++j)
  {
    for (auto k = j + 1; k < planes.size(); ++k)
    {
      if (j == face || k == face)
      {
        continue;
      }
      const auto& b = planes[j];
      const auto& c = planes[k];
      const auto n1 = a.normal;
      const auto n2 = b.normal;
      const auto n3 = c.normal;
      const auto det = vm::dot(n1, vm::cross(n2, n3));
      if (std::abs(det) < 1e-9)
      {
        continue;
      }
      const auto p = (vm::cross(n2, n3) * a.distance + vm::cross(n3, n1) * b.distance
                      + vm::cross(n1, n2) * c.distance)
                     / det;
      const auto inside = std::ranges::all_of(planes, [&](const auto& plane) {
        return vm::dot(plane.normal, p) - plane.distance <= PolygonEpsilon;
      });
      const auto known = std::ranges::any_of(points, [&](const auto& q) {
        return vm::squared_length(q - p) < PolygonEpsilon * PolygonEpsilon;
      });
      if (inside && !known)
      {
        points.push_back(p);
      }
    }
  }
  if (points.size() < 3)
  {
    return {};
  }
  auto centre = vm::vec3d{0, 0, 0};
  for (const auto& p : points)
  {
    centre = centre + p;
  }
  centre = centre / double(points.size());
  const auto n = a.normal;
  const auto u = vm::normalize(points[0] - centre);
  const auto v = vm::cross(n, u);
  // counter-clockwise seen from outside (from the side the normal points to)
  std::ranges::sort(points, [&](const auto& p, const auto& q) {
    return std::atan2(vm::dot(p - centre, v), vm::dot(p - centre, u))
           < std::atan2(vm::dot(q - centre, v), vm::dot(q - centre, u));
  });
  // vm::polygon: the least vertex first
  std::ranges::rotate(points, std::ranges::min_element(points));
  return points;
}

// a face record from its parsed values, the polygon given
SnapshotFace faceRecord(
  Snapshot& snapshot,
  const ParsedFace& face,
  const vm::plane3d& plane,
  const std::vector<vm::vec3d>& polygon)
{
  auto record = SnapshotFace{};
  record.polygon.first = count(snapshot.vertices.size());
  record.polygon.count = count(polygon.size());
  snapshot.vertices.insert(snapshot.vertices.end(), polygon.begin(), polygon.end());
  record.points = face.points;
  record.plane = plane;
  record.material = intern(snapshot, face.material);
  record.uAxis = toFloat(face.u);
  record.vAxis = toFloat(face.v);
  record.offset = vm::vec2f{float(face.uOffset), float(face.vOffset)};
  record.scale = vm::vec2f{float(face.uScale), float(face.vScale)};
  return record;
}

// a brush's faces into the snapshot, its node after them
void addBrush(
  Snapshot& snapshot,
  const PreviewId id,
  const PreviewId entity,
  const std::vector<ParsedFace>& faces)
{
  auto node = SnapshotNode{};
  node.id = id;
  node.entity = entity;
  node.kind = SnapshotNodeKind::Brush;
  node.visible = true;
  node.elements.first = count(snapshot.faces.size());
  auto planes = std::vector<vm::plane3d>{};
  for (const auto& face : faces)
  {
    planes.push_back(planeOf(face.points));
  }
  for (auto f = std::size_t{0}; f < faces.size(); ++f)
  {
    snapshot.faces.push_back(
      faceRecord(snapshot, faces[f], planes[f], polygonOf(planes, f)));
  }
  node.elements.count = count(faces.size());
  snapshot.nodes.push_back(node);
}

void addPatch(
  Snapshot& snapshot,
  const PreviewId id,
  const PreviewId entity,
  const ParsedPatch& source)
{
  auto record = SnapshotPatch{};
  record.material = intern(snapshot, source.material);
  record.rows = count(source.rows);
  record.columns = count(source.columns);
  record.controlPoints.first = count(snapshot.controlPoints.size());
  record.controlPoints.count = count(source.points.size());
  snapshot.controlPoints.insert(
    snapshot.controlPoints.end(), source.points.begin(), source.points.end());
  record.explicitSubdivisions = source.explicitSubdivisions;
  record.horizontalSubdivisions = count(source.h);
  record.verticalSubdivisions = count(source.v);
  snapshot.patches.push_back(record);
  snapshot.nodes.push_back(SnapshotNode{
    id,
    entity,
    SnapshotNodeKind::Patch,
    true,
    SnapshotRange{count(snapshot.patches.size()) - 1, 1},
    false});
}

std::uint32_t addEntity(
  Snapshot& snapshot,
  const PreviewId id,
  const std::string_view classname,
  const std::vector<std::pair<std::string, std::string>>& keys)
{
  auto record = SnapshotEntity{};
  record.id = id;
  record.classname = intern(snapshot, classname);
  record.keys.first = count(snapshot.keyValues.size());
  for (const auto& [key, value] : keys)
  {
    snapshot.keyValues.push_back(
      SnapshotKeyValue{intern(snapshot, key), intern(snapshot, value)});
  }
  record.keys.count = count(keys.size());
  snapshot.entities.push_back(record);
  return count(snapshot.entities.size()) - 1;
}

// the axial axis index of a normal (0 x, 1 y, 2 z), or nullopt
std::optional<std::size_t> axisOf(const vm::vec3d& normal)
{
  for (auto axis = std::size_t{0}; axis < 3; ++axis)
  {
    if (
      std::abs(normal[axis]) == 1.0 && normal[(axis + 1) % 3] == 0.0
      && normal[(axis + 2) % 3] == 0.0)
    {
      return axis;
    }
  }
  return std::nullopt;
}

// TrenchBroom's Valve axes of a new face on this axis (ParaxialUVCoordSystem's base
// axes, as lit fixtures' boxBrush writes them)
std::pair<vm::vec3d, vm::vec3d> valveAxes(const std::size_t axis)
{
  switch (axis)
  {
  case 0:
    return {{0, 1, 0}, {0, 0, -1}};
  case 1:
    return {{1, 0, 0}, {0, 0, -1}};
  default:
    return {{1, 0, 0}, {0, -1, 0}};
  }
}

} // namespace

Result<Snapshot> snapshotOfMapText(
  const std::string& text,
  const std::uint64_t generation,
  const std::uint64_t sequence,
  std::optional<std::string> mapPath)
{
  auto tokens = Tokens{text};
  const auto parsed = parseMap(tokens);
  if (const auto& error = tokens.error())
  {
    return Error{fmt::format("The map text cannot be read: {}.", *error)};
  }
  auto snapshot = Snapshot{};
  snapshot.generation = generation;
  snapshot.sequence = sequence;
  snapshot.mapPath = std::move(mapPath);
  auto nextId = PreviewId{1};
  auto entityIds = std::vector<PreviewId>{};
  for (const auto& entity : parsed)
  {
    const auto classname = std::ranges::find_if(
      entity.keys, [](const auto& kv) { return kv.first == "classname"; });
    const auto id = nextId++;
    addEntity(
      snapshot,
      id,
      classname != entity.keys.end() ? std::string_view{classname->second}
                                     : std::string_view{},
      entity.keys);
    entityIds.push_back(id);
  }

  for (auto e = std::size_t{0}; e < parsed.size(); ++e)
  {
    const auto& entity = parsed[e];
    const auto id = entityIds[e];
    if (entity.brushes.empty() && entity.patches.empty())
    {
      if (e == 0)
      {
        continue;
      }
      const auto classname = snapshot.string(snapshot.entities[e].classname);
      snapshot.nodes.push_back(SnapshotNode{
        id,
        id,
        classname == "light" ? SnapshotNodeKind::Light : SnapshotNodeKind::PointEntity,
        true,
        SnapshotRange{count(e), 1},
        false});
      continue;
    }
    auto brush = std::size_t{0};
    auto patch = std::size_t{0};
    for (const auto isBrush : entity.order)
    {
      const auto nodeId = nextId++;
      if (isBrush)
      {
        addBrush(snapshot, nodeId, id, entity.brushes[brush++]);
      }
      else
      {
        addPatch(snapshot, nodeId, id, entity.patches[patch++]);
      }
    }
  }
  return snapshot;
}

namespace feed
{

void worldspawn(
  Snapshot& snapshot,
  const PreviewId entityId,
  const std::vector<std::pair<std::string, std::string>>& keys)
{
  auto all =
    std::vector<std::pair<std::string, std::string>>{{"classname", "worldspawn"}};
  all.insert(all.end(), keys.begin(), keys.end());
  addEntity(snapshot, entityId, "worldspawn", all);
}

Result<void> quadBrush(
  Snapshot& snapshot,
  const PreviewId entityId,
  const PreviewId nodeId,
  const vm::vec3d& centre,
  const vm::vec3d& normal,
  const double halfSize,
  const std::string& material,
  const std::string& sideMaterial,
  const double textureScale,
  const vm::vec2f& offset,
  const double thickness)
{
  const auto axis = axisOf(normal);
  if (!axis)
  {
    return Error{"A quad brush's normal must be an axis."};
  }
  if (!(halfSize > 0.0) || !(thickness > 0.0) || !(textureScale > 0.0))
  {
    return Error{"A quad brush's size, thickness and texture scale must be positive."};
  }
  // the box: halfSize around the centre in the face's plane, thickness behind it
  const auto sign = normal[*axis];
  auto lo = centre - vm::vec3d{halfSize, halfSize, halfSize};
  auto hi = centre + vm::vec3d{halfSize, halfSize, halfSize};
  lo[*axis] = sign > 0 ? centre[*axis] - thickness : centre[*axis];
  hi[*axis] = sign > 0 ? centre[*axis] : centre[*axis] + thickness;
  lo = readBack(lo);
  hi = readBack(hi);

  auto faces = std::vector<ParsedFace>{};
  for (auto faceAxis = std::size_t{0}; faceAxis < 3; ++faceAxis)
  {
    for (const auto faceSign : {-1.0, 1.0})
    {
      // the face's four corners, counter-clockwise seen from outside
      auto n = vm::vec3d{0, 0, 0};
      n[faceAxis] = faceSign;
      const auto a1 = (faceAxis + 1) % 3;
      const auto a2 = (faceAxis + 2) % 3;
      auto corner = [&](const bool high1, const bool high2) {
        auto p = vm::vec3d{0, 0, 0};
        p[faceAxis] = faceSign > 0 ? hi[faceAxis] : lo[faceAxis];
        p[a1] = high1 ? hi[a1] : lo[a1];
        p[a2] = high2 ? hi[a2] : lo[a2];
        return p;
      };
      // (a1, a2, n) is right-handed, so lo-lo, hi-lo, hi-hi, lo-hi turns
      // counter-clockwise around +n
      auto quad = std::array<vm::vec3d, 4>{
        corner(false, false),
        corner(true, false),
        corner(true, true),
        corner(false, true)};
      if (faceSign < 0)
      {
        std::swap(quad[1], quad[3]);
      }
      auto face = ParsedFace{};
      // TrenchBroom's points: (p3 - p1) x (p2 - p1) = (q1 - q0) x (q3 - q0), outward
      face.points = {quad[0], quad[3], quad[1]};
      const auto front = faceAxis == *axis && faceSign == sign;
      face.material = front ? material : sideMaterial;
      const auto [u, v] = valveAxes(faceAxis);
      face.u = u;
      face.v = v;
      face.uOffset = double(readBack(double(offset.x())));
      face.vOffset = double(readBack(double(offset.y())));
      face.uScale = double(readBack(textureScale));
      face.vScale = double(readBack(textureScale));
      faces.push_back(face);
    }
  }
  addBrush(snapshot, nodeId, entityId, faces);
  return kdl::void_success;
}

Result<void> patchDef3Sphere(
  Snapshot& snapshot,
  const PreviewId entityId,
  const PreviewId nodeId,
  const vm::vec3d& centre,
  const double radius,
  const std::string& material,
  const std::uint32_t horizontalSubdivisions,
  const std::uint32_t verticalSubdivisions)
{
  if (!(radius > 0.0))
  {
    return Error{"A sphere's radius must be positive."};
  }
  // K19 (EngineGeometry.h engineBounds): 1 to 64 subdivisions, at most 65,535 vertices
  constexpr auto Rows = std::uint32_t{9};
  constexpr auto Columns = std::uint32_t{17};
  if (
    horizontalSubdivisions < 1 || horizontalSubdivisions > 64 || verticalSubdivisions < 1
    || verticalSubdivisions > 64
    || std::uint64_t((Rows - 1) / 2 * horizontalSubdivisions + 1)
           * std::uint64_t((Columns - 1) / 2 * verticalSubdivisions + 1)
         > 65535u)
  {
    return Error{"A sphere's subdivisions are outside K19's bounds."};
  }
  // the profile of 4 quadratic segments of 45 degrees from the south pole to the north
  // pole, and the circle of 8 around: even points on the circle, odd points on the
  // tangents' intersection, 1 / cos( 22.5 degrees ) out
  const auto step = std::numbers::pi / 8.0;
  const auto mid = 1.0 / std::cos(step);
  auto record = SnapshotPatch{};
  record.material = intern(snapshot, material);
  record.rows = Rows;
  record.columns = Columns;
  record.controlPoints.first = count(snapshot.controlPoints.size());
  for (auto row = std::uint32_t{0}; row < Rows; ++row)
  {
    const auto latitude = -std::numbers::pi / 2.0 + double(row) * step;
    const auto rowScale = row % 2 == 1 ? mid : 1.0;
    const auto ring = std::cos(latitude) * rowScale;
    const auto z = std::sin(latitude) * rowScale;
    for (auto column = std::uint32_t{0}; column < Columns; ++column)
    {
      // clockwise seen from above, so that the engine's triangles face out
      const auto longitude = -double(column) * step;
      const auto columnScale = column % 2 == 1 ? mid : 1.0;
      // the pole rows pinch to one point
      const auto r = row == 0 || row == Rows - 1 ? 0.0 : ring * columnScale;
      const auto p = readBack(vm::vec3d{
        centre.x() + radius * r * std::cos(longitude),
        centre.y() + radius * r * std::sin(longitude),
        centre.z() + radius * z});
      snapshot.controlPoints.push_back(vm::vec<double, 5>{
        p.x(),
        p.y(),
        p.z(),
        double(readBack(double(column) / double(Columns - 1))),
        double(readBack(double(row) / double(Rows - 1)))});
    }
  }
  record.controlPoints.count = Rows * Columns;
  record.explicitSubdivisions = true;
  record.horizontalSubdivisions = horizontalSubdivisions;
  record.verticalSubdivisions = verticalSubdivisions;
  snapshot.patches.push_back(record);
  snapshot.nodes.push_back(SnapshotNode{
    nodeId,
    entityId,
    SnapshotNodeKind::Patch,
    true,
    SnapshotRange{count(snapshot.patches.size()) - 1, 1},
    false});
  return kdl::void_success;
}

void pointLight(
  Snapshot& snapshot,
  const PreviewId id,
  const vm::vec3d& origin,
  const double radius,
  const vm::vec3f& colour,
  const std::string& texture)
{
  auto keys = std::vector<std::pair<std::string, std::string>>{
    {"classname", "light"},
    {"origin", fmt::format("{} {} {}", origin.x(), origin.y(), origin.z())},
    {"light_radius", fmt::format("{} {} {}", radius, radius, radius)},
    {"_color", fmt::format("{} {} {}", colour.x(), colour.y(), colour.z())},
  };
  if (!texture.empty())
  {
    keys.emplace_back("texture", texture);
  }
  const auto index = addEntity(snapshot, id, "light", keys);
  snapshot.nodes.push_back(
    SnapshotNode{id, id, SnapshotNodeKind::Light, true, SnapshotRange{index, 1}, false});
}

CameraState cameraLookingAt(
  const vm::vec3f& eye, const vm::vec3f& target, const float fovDegrees)
{
  auto camera = CameraState{};
  camera.position = eye;
  camera.direction = vm::normalize(target - eye);
  const auto worldUp =
    std::abs(camera.direction.z()) > 0.999f ? vm::vec3f{1, 0, 0} : vm::vec3f{0, 0, 1};
  const auto right = vm::normalize(vm::cross(camera.direction, worldUp));
  camera.up = vm::cross(right, camera.direction);
  camera.fovDegrees = fovDegrees;
  return camera;
}

} // namespace feed
} // namespace tb::lit
