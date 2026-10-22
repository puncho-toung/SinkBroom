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

#include "LitMapFixture.h"

#include "LitTestUtils.h"
#include "TestEnvironment.h"

#include "vm/plane.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace tb::lit
{
namespace
{

class Tokens
{
private:
  const std::string& m_text;
  std::size_t m_pos = 0;

public:
  explicit Tokens(const std::string& text)
    : m_text{text}
  {
  }

  bool next(std::string& token)
  {
    for (;;)
    {
      while (m_pos < m_text.size()
             && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
      {
        ++m_pos;
      }
      if (m_pos + 1 < m_text.size() && m_text[m_pos] == '/' && m_text[m_pos + 1] == '/')
      {
        while (m_pos < m_text.size() && m_text[m_pos] != '\n')
        {
          ++m_pos;
        }
        continue;
      }
      break;
    }
    if (m_pos >= m_text.size())
    {
      return false;
    }
    const auto c = m_text[m_pos];
    if (c == '"')
    {
      const auto end = m_text.find('"', m_pos + 1);
      token = m_text.substr(m_pos + 1, end - m_pos - 1);
      m_pos = end + 1;
      return true;
    }
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
    {
      token = std::string{c};
      ++m_pos;
      return true;
    }
    const auto start = m_pos;
    while (m_pos < m_text.size()
           && !std::isspace(static_cast<unsigned char>(m_text[m_pos])))
    {
      ++m_pos;
    }
    token = m_text.substr(start, m_pos - start);
    return true;
  }

  std::string expect()
  {
    auto token = std::string{};
    if (!next(token))
    {
      throw std::runtime_error{"unexpected end of the map"};
    }
    return token;
  }

  double number() { return std::stod(expect()); }

  void expect(const std::string& what)
  {
    if (expect() != what)
    {
      throw std::runtime_error{"expected " + what};
    }
  }
};

struct ParsedFace
{
  std::array<vm::vec3d, 3> points;
  std::string material;
  vm::vec3d u, v;
  double uOffset = 0, vOffset = 0, uScale = 1, vScale = 1;
};

struct ParsedPatch
{
  std::string material;
  std::size_t rows = 0, columns = 0;
  bool explicitSubdivisions = false;
  std::size_t h = 0, v = 0;
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

ParsedPatch parsePatch(Tokens& tokens, const bool explicitSubdivisions)
{
  auto patch = ParsedPatch{};
  patch.explicitSubdivisions = explicitSubdivisions;
  tokens.expect("{");
  patch.material = tokens.expect();
  tokens.expect("(");
  patch.rows = std::size_t(tokens.number());
  patch.columns = std::size_t(tokens.number());
  if (explicitSubdivisions)
  {
    patch.h = std::size_t(tokens.number());
    patch.v = std::size_t(tokens.number());
  }
  for (auto t = std::string{}; (t = tokens.expect()) != ")";)
  {
  }
  tokens.expect("(");
  for (auto r = std::size_t{0}; r < patch.rows; ++r)
  {
    tokens.expect("(");
    for (auto c = std::size_t{0}; c < patch.columns; ++c)
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

std::vector<ParsedEntity> parseMap(const std::string& text)
{
  auto tokens = Tokens{text};
  auto entities = std::vector<ParsedEntity>{};
  auto token = std::string{};
  while (tokens.next(token))
  {
    if (token != "{")
    {
      throw std::runtime_error{"expected an entity"};
    }
    auto entity = ParsedEntity{};
    for (;;)
    {
      token = tokens.expect();
      if (token == "}")
      {
        break;
      }
      if (token == "{")
      {
        // a brush or a patch
        auto first = tokens.expect();
        if (first == "patchDef2" || first == "patchDef3")
        {
          entity.patches.push_back(parsePatch(tokens, first == "patchDef3"));
          entity.order.push_back(false);
          continue;
        }
        auto faces = std::vector<ParsedFace>{};
        while (first != "}")
        {
          if (first != "(")
          {
            throw std::runtime_error{"expected a face"};
          }
          auto face = ParsedFace{};
          const auto x = tokens.number();
          const auto y = tokens.number();
          const auto z = tokens.number();
          tokens.expect(")");
          face.points = {vm::vec3d{x, y, z}, point(tokens), point(tokens)};
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
        entity.brushes.push_back(std::move(faces));
        entity.order.push_back(true);
        continue;
      }
      entity.keys.emplace_back(token, tokens.expect());
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
      const auto n1 = a.normal, n2 = b.normal, n3 = c.normal;
      const auto det = vm::dot(n1, vm::cross(n2, n3));
      if (std::abs(det) < 1e-9)
      {
        continue;
      }
      const auto p = (vm::cross(n2, n3) * a.distance + vm::cross(n3, n1) * b.distance
                      + vm::cross(n1, n2) * c.distance)
                     / det;
      const auto inside =
        std::all_of(planes.begin(), planes.end(), [&](const auto& plane) {
          return vm::dot(plane.normal, p) - plane.distance <= 1e-6;
        });
      const auto known = std::any_of(points.begin(), points.end(), [&](const auto& q) {
        return vm::squared_length(q - p) < 1e-12;
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
  std::sort(points.begin(), points.end(), [&](const auto& p, const auto& q) {
    return std::atan2(vm::dot(p - centre, v), vm::dot(p - centre, u))
           < std::atan2(vm::dot(q - centre, v), vm::dot(q - centre, u));
  });
  // vm::polygon: the least vertex first
  const auto least = std::min_element(points.begin(), points.end());
  std::rotate(points.begin(), least, points.end());
  return points;
}

SnapshotString intern(Snapshot& snapshot, const std::string& text)
{
  const auto offset = std::uint32_t(snapshot.strings.size());
  snapshot.strings += text;
  return SnapshotString{offset, std::uint32_t(text.size())};
}

vm::vec3f toFloat(const vm::vec3d& v)
{
  return vm::vec3f{float(v.x()), float(v.y()), float(v.z())};
}

} // namespace

std::filesystem::path litFixtureFolder()
{
  return getFixtureRoot() / "test" / "lit";
}

Snapshot snapshotOfMapText(const std::string& text)
{
  const auto parsed = parseMap(text);
  auto snapshot = Snapshot{};
  snapshot.generation = 1;
  snapshot.sequence = 1;
  auto nextId = PreviewId{1};
  auto entityIds = std::vector<PreviewId>{};
  for (const auto& entity : parsed)
  {
    auto record = SnapshotEntity{};
    record.id = nextId++;
    const auto classname =
      std::find_if(entity.keys.begin(), entity.keys.end(), [](const auto& kv) {
        return kv.first == "classname";
      });
    record.classname = intern(
      snapshot, classname != entity.keys.end() ? classname->second : std::string{});
    record.keys.first = std::uint32_t(snapshot.keyValues.size());
    for (const auto& [key, value] : entity.keys)
    {
      snapshot.keyValues.push_back(
        SnapshotKeyValue{intern(snapshot, key), intern(snapshot, value)});
    }
    record.keys.count = std::uint32_t(entity.keys.size());
    entityIds.push_back(record.id);
    snapshot.entities.push_back(record);
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
        SnapshotRange{std::uint32_t(e), 1}});
      continue;
    }
    auto brush = std::size_t{0};
    auto patch = std::size_t{0};
    for (const auto isBrush : entity.order)
    {
      auto node = SnapshotNode{};
      node.id = nextId++;
      node.entity = id;
      node.visible = true;
      if (isBrush)
      {
        const auto& faces = entity.brushes[brush++];
        node.kind = SnapshotNodeKind::Brush;
        node.elements.first = std::uint32_t(snapshot.faces.size());
        auto planes = std::vector<vm::plane3d>{};
        for (const auto& face : faces)
        {
          planes.push_back(planeOf(face.points));
        }
        for (auto f = std::size_t{0}; f < faces.size(); ++f)
        {
          const auto& face = faces[f];
          auto record = SnapshotFace{};
          const auto polygon = polygonOf(planes, f);
          record.polygon.first = std::uint32_t(snapshot.vertices.size());
          record.polygon.count = std::uint32_t(polygon.size());
          snapshot.vertices.insert(
            snapshot.vertices.end(), polygon.begin(), polygon.end());
          record.points = face.points;
          record.plane = planes[f];
          record.material = intern(snapshot, face.material);
          record.uAxis = toFloat(face.u);
          record.vAxis = toFloat(face.v);
          record.offset = vm::vec2f{float(face.uOffset), float(face.vOffset)};
          record.scale = vm::vec2f{float(face.uScale), float(face.vScale)};
          snapshot.faces.push_back(record);
        }
        node.elements.count = std::uint32_t(faces.size());
      }
      else
      {
        const auto& source = entity.patches[patch++];
        node.kind = SnapshotNodeKind::Patch;
        node.elements = SnapshotRange{std::uint32_t(snapshot.patches.size()), 1};
        auto record = SnapshotPatch{};
        record.material = intern(snapshot, source.material);
        record.rows = std::uint32_t(source.rows);
        record.columns = std::uint32_t(source.columns);
        record.controlPoints.first = std::uint32_t(snapshot.controlPoints.size());
        record.controlPoints.count = std::uint32_t(source.points.size());
        snapshot.controlPoints.insert(
          snapshot.controlPoints.end(), source.points.begin(), source.points.end());
        record.explicitSubdivisions = source.explicitSubdivisions;
        record.horizontalSubdivisions = std::uint32_t(source.h);
        record.verticalSubdivisions = std::uint32_t(source.v);
        snapshot.patches.push_back(record);
      }
      snapshot.nodes.push_back(node);
    }
  }
  return snapshot;
}

std::string boxBrush(
  const int x0,
  const int y0,
  const int z0,
  const int x1,
  const int y1,
  const int z1,
  const std::string& material)
{
  // TrenchBroom's points: (p3 - p1) x (p2 - p1) is the outward normal
  const auto face = [&](
                      const int ax,
                      const int ay,
                      const int az,
                      const int bx,
                      const int by,
                      const int bz,
                      const int cx,
                      const int cy,
                      const int cz,
                      const std::string& axes) {
    return fmt::format(
      "( {} {} {} ) ( {} {} {} ) ( {} {} {} ) {} {} 0 1 1\n",
      ax,
      ay,
      az,
      bx,
      by,
      bz,
      cx,
      cy,
      cz,
      material,
      axes);
  };
  const auto xAxes = std::string{"[ 0 1 0 0 ] [ 0 0 -1 0 ]"};
  const auto yAxes = std::string{"[ 1 0 0 0 ] [ 0 0 -1 0 ]"};
  const auto zAxes = std::string{"[ 1 0 0 0 ] [ 0 -1 0 0 ]"};
  return "{\n" + face(x0, y0, z0, x0, y0 + 1, z0, x0, y0, z0 + 1, xAxes)
         + face(x1, y1, z1, x1, y1, z1 + 1, x1, y1 + 1, z1, xAxes)
         + face(x0, y0, z0, x0, y0, z0 + 1, x0 + 1, y0, z0, yAxes)
         + face(x1, y1, z1, x1 + 1, y1, z1, x1, y1, z1 + 1, yAxes)
         + face(x0, y0, z0, x0 + 1, y0, z0, x0, y0 + 1, z0, zAxes)
         + face(x1, y1, z1, x1, y1 + 1, z1, x1 + 1, y1, z1, zAxes) + "}\n";
}

std::map<std::string, std::pair<int, int>> listImagesSizes(const std::string& text)
{
  auto sizes = std::map<std::string, std::pair<int, int>>{};
  auto in = std::istringstream{text};
  auto line = std::string{};
  auto inMap = false;
  while (std::getline(in, line))
  {
    // R1.4: a CRLF checkout reads as an LF one
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    if (line.rfind("@@ ", 0) == 0)
    {
      inMap = line == "@@ map";
      continue;
    }
    if (!inMap || line.find("textures/") == std::string::npos)
    {
      continue;
    }
    // " 478:         64   32 DXT5 ... textures/lp1b/floor"
    auto fields = std::istringstream{line.substr(line.find(':') + 1)};
    auto width = 0;
    auto height = 0;
    fields >> width >> height;
    const auto name = line.substr(line.rfind(' ') + 1);
    sizes[name] = {width, height};
  }
  return sizes;
}

std::shared_ptr<const MaterialTable> fixtureMaterialTable(
  const std::map<std::string, std::pair<int, int>>& sizes)
{
  auto files = std::vector<DeclFile>{
    DeclFile{
      "materials/lp1bc.mtr",
      readTextFile(litFixtureFolder() / "materials" / "lp1bc.mtr"),
      0},
    DeclFile{
      "materials/zz_base_stand_ins.mtr",
      "textures/editor/visportal\n{\n\tqer_editorimage textures/editor/visportal\n"
      "\tareaportal\n\tnoshadows\n}\n"
      "textures/common/origin\n{\n\tqer_editorimage textures/common/origin\n"
      "\torigin\n\tnoshadows\n}\n",
      0},
  };
  auto source = MemoryDeclSource{std::move(files)};
  auto decls = MaterialDecls{source};
  if (!decls.constructionError().empty())
  {
    throw std::runtime_error{decls.constructionError()};
  }
  auto table = std::make_shared<MaterialTable>();
  table->registry = decls.id();
  for (const auto* name :
       {"textures/lp1b/floor",
        "textures/lp1b/wall",
        "textures/lp1b/crate",
        "textures/lp1b/grate",
        "textures/lp1b/glass",
        "textures/lp1b/patch",
        "textures/editor/visportal",
        "textures/common/origin"})
  {
    auto found = decls.lookup(name);
    if (!found)
    {
      throw std::runtime_error{found.error};
    }
    auto entry = std::make_shared<MaterialEntry>(**found.value);
    if (const auto size = sizes.find(name); size != sizes.end())
    {
      entry->editorImageSize = EditorImageSize{
        size->second.first, size->second.second, EditorImageSize::State::Ready};
    }
    table->materials = table->materials.set(
      name, std::make_shared<const MaterialSlot>(MaterialSlot{std::move(entry), {}}));
  }
  decls.takeMessages();
  return table;
}

std::shared_ptr<const MaterialTable> lightMaterialTable(
  const std::vector<std::string>& implicitNames)
{
  auto files = std::vector<DeclFile>{
    DeclFile{
      "materials/zz_light_stand_ins.mtr",
      "lights/squarelight1\n{\n\tlightFalloffImage makeIntensity( "
      "lights/squarelight1a.tga )\n"
      "\t{\n\t\tmap lights/squarelight1.tga\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
      "lights/defaultPointLight\n{\n\tlightFalloffImage makeIntensity( "
      "lights/squarelight1a.tga )\n"
      "\t{\n\t\tmap lights/squarelight1.tga\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
      "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n"
      "\t{\n\t\tmap lights/squarelight1.tga\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
      "lights/lp1b_nofalloff\n{\n\t{\n\t\tmap "
      "lights/squarelight1.tga\n\t\tcolored\n\t}\n}\n",
      0},
  };
  auto source = MemoryDeclSource{std::move(files)};
  auto decls = MaterialDecls{source};
  if (!decls.constructionError().empty())
  {
    throw std::runtime_error{decls.constructionError()};
  }
  auto table = std::make_shared<MaterialTable>();
  table->registry = decls.id();
  auto names = std::vector<std::string>{
    "lights/squarelight1",
    "lights/defaultPointLight",
    "lights/defaultProjectedLight",
    "lights/lp1b_nofalloff"};
  names.insert(names.end(), implicitNames.begin(), implicitNames.end());
  for (const auto& name : names)
  {
    auto found = decls.lookup(name);
    if (!found)
    {
      throw std::runtime_error{found.error};
    }
    table->materials = table->materials.set(
      name, std::make_shared<const MaterialSlot>(MaterialSlot{*found.value, {}}));
  }
  decls.takeMessages();
  return table;
}

} // namespace tb::lit
