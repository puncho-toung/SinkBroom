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
#include "lit/FeedScene.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace tb::lit
{

std::filesystem::path litFixtureFolder()
{
  return getFixtureRoot() / "test" / "lit";
}

Snapshot snapshotOfMapText(const std::string& text)
{
  // LPF.2 (D107): the product's reader, generation and sequence 1, no map path
  return lit::snapshotOfMapText(text, 1, 1)
         | kdl::if_error([](const auto& e) { throw std::runtime_error{e.msg}; })
         | kdl::value();
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
        size->second.first, size->second.second, EditorImageSize::State::Ready, {}};
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
