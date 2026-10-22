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

// The light and view mirrors against goldens/lp1b_lights/light_goldens.txt, which
// TOOLS/lit-preview/light_goldens.py writes: an independent script of the engine's
// formulas, in step-by-step float. These check the mirrors against the formulas; LP2's
// parity rows against the running engine are the real check (a recorded §6 deviation:
// under the user's ruling the engine gets no patch, even uncommitted, to print its
// derived light data or its matrices). The engine's own numbers for lights are the
// area-reference counts and shaders of listRenderLightDefs (tst_LightData.cpp).

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/MaterialService.h"
#include "lit/NumberText.h"
#include "lit/Projection.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

struct Block
{
  std::string kind;
  std::vector<std::string> header;
  EntityKeys keys;
  std::map<std::string, std::vector<std::string>> expect;
};

std::vector<std::string> words(const std::string& text)
{
  auto in = std::istringstream{text};
  auto result = std::vector<std::string>{};
  auto word = std::string{};
  while (in >> word)
  {
    result.push_back(word);
  }
  return result;
}

std::string goldensText()
{
  return readTextFile(
    litFixtureFolder() / "goldens" / "lp1b_lights" / "light_goldens.txt");
}

std::vector<Block> readGoldens(const std::string& text = goldensText())
{
  auto blocks = std::vector<Block>{};
  auto in = std::istringstream{text};
  auto line = std::string{};
  while (std::getline(in, line))
  {
    // R1.4: a CRLF checkout reads as an LF one
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#')
    {
      continue;
    }
    if (line == "end")
    {
      continue;
    }
    if (line.rfind("key ", 0) == 0)
    {
      const auto tab = line.find('\t');
      blocks.back().keys.emplace_back(line.substr(4, tab - 4), line.substr(tab + 1));
    }
    else if (line.rfind("expect ", 0) == 0)
    {
      auto fields = words(line.substr(7));
      const auto name = fields.front();
      fields.erase(fields.begin());
      blocks.back().expect[name] = fields;
    }
    else
    {
      auto fields = words(line);
      auto block = Block{};
      block.kind = fields.front();
      block.header.assign(fields.begin() + 1, fields.end());
      blocks.push_back(std::move(block));
    }
  }
  return blocks;
}

float parseFloat(const std::string& text)
{
  // R1.1: from_chars, or fast_float's on Apple
  auto value = 0.0f;
  parseFloatChars(text.data(), text.data() + text.size(), value);
  return value;
}

std::vector<float> floats(const std::vector<std::string>& texts)
{
  auto values = std::vector<float>{};
  for (const auto& text : texts)
  {
    values.push_back(parseFloat(text));
  }
  return values;
}

// the distance in units in the last place; +0 and -0 are the same
std::int64_t ulps(const float a, const float b)
{
  const auto ordered = [](const float x) {
    const auto bits = std::int64_t(std::bit_cast<std::int32_t>(x));
    return bits < 0 ? std::int64_t(INT_MIN) - bits : bits;
  };
  return std::abs(ordered(a) - ordered(b));
}

template <std::size_t N>
std::vector<float> values(const std::array<float, N>& array)
{
  return {array.begin(), array.end()};
}

std::vector<float> values(const std::array<EnginePlane, 4>& planes)
{
  auto result = std::vector<float>{};
  for (const auto& p : planes)
  {
    result.insert(result.end(), {p.a, p.b, p.c, p.d});
  }
  return result;
}

std::vector<float> values(const EngineBounds& bounds)
{
  return {
    bounds[0][0], bounds[0][1], bounds[0][2], bounds[1][0], bounds[1][1], bounds[1][2]};
}

// within maxUlps, or within absolute of the field's largest magnitude
bool close(
  const std::vector<float>& actual,
  const std::vector<float>& expected,
  const std::int64_t maxUlps,
  const float relative,
  std::string& why)
{
  if (actual.size() != expected.size())
  {
    why = fmt::format("{} values, expected {}", actual.size(), expected.size());
    return false;
  }
  auto magnitude = 0.0f;
  for (const auto v : expected)
  {
    magnitude = std::max(magnitude, std::abs(v));
  }
  for (auto i = std::size_t{0}; i < actual.size(); ++i)
  {
    const auto a = actual[i];
    const auto e = expected[i];
    if (ulps(a, e) > maxUlps && std::abs(a - e) > relative * magnitude)
    {
      why = fmt::format("[{}]: {} expected {} ({} ulps)", i, a, e, ulps(a, e));
      return false;
    }
  }
  return true;
}

bool operator==(const Block& lhs, const Block& rhs)
{
  return lhs.kind == rhs.kind && lhs.header == rhs.header && lhs.keys == rhs.keys
         && lhs.expect == rhs.expect;
}

std::string withCrlf(const std::string& text)
{
  auto result = std::string{};
  for (const auto c : text)
  {
    if (c == '\n')
    {
      result += '\r';
    }
    result += c;
  }
  return result;
}

} // namespace

TEST_CASE("LightGoldens: a CRLF copy of the goldens reads as the file (R1.4)")
{
  const auto text = goldensText();
  const auto crlf = withCrlf(text);
  REQUIRE(crlf.size() > text.size());
  const auto blocks = readGoldens(text);
  CHECK(blocks.size() > 10u);
  CHECK(readGoldens(crlf) == blocks);
}

// R3.16: the derived data within 16 ulps, or within 1e-6 (lightProject,
// globalLightOrigin) or 1e-5 (baseLightProject, its inverse, the bounds) of the field's
// largest element; the projection and the view within 1 ulp
TEST_CASE(
  "LightGoldens: the light parse and derived data against the formulas within 16 ulps or "
  "1e-6/1e-5 of the largest element")
{
  const auto blocks = readGoldens();
  const auto table = lightMaterialTable({"lights/lp1b_missing"});
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  auto lights = 0;
  for (const auto& block : blocks)
  {
    if (block.kind != "light")
    {
      continue;
    }
    ++lights;
    const auto& name = block.header.front();
    const auto light = parseLight(block.keys);
    const auto derived = deriveLight(light, table.get(), nullptr);
    const auto& p = light.parms;
    const auto expect = [&](const std::string& field) {
      INFO(name << " " << field);
      REQUIRE(block.expect.contains(field));
      return block.expect.at(field);
    };
    const auto integer = [&](const std::string& field) {
      return std::stoi(expect(field).front());
    };

    INFO(name);
    CHECK(int(p.pointLight) == integer("pointLight"));
    CHECK(int(p.parallel) == integer("parallel"));
    CHECK(int(light.badTarget) == integer("badTarget"));
    CHECK(p.mathLight.type == integer("mathType"));
    CHECK(light.spawn.levels == integer("levels"));
    CHECK(int(light.warnings.size()) == integer("warnings"));
    CHECK(derived.shader.name == expect("shader").front());

    // the parse: exact, the axis within the ulps sinf and cosf may differ by
    const auto check = [&](
                         const std::string& field,
                         const std::vector<float>& actual,
                         const std::int64_t maxUlps,
                         const float relative) {
      auto why = std::string{};
      const auto ok = close(actual, floats(expect(field)), maxUlps, relative, why);
      INFO(name << " " << field << ": " << why);
      CHECK(ok);
    };
    check("origin", values(p.origin), 0, 0.0f);
    check("axis", values(p.axis), 4, 0.0f);
    check("target", values(p.target), 0, 0.0f);
    check("right", values(p.right), 0, 0.0f);
    check("up", values(p.up), 0, 0.0f);
    check("start", values(p.start), 0, 0.0f);
    check("end", values(p.end), 0, 0.0f);
    check("lightRadius", values(p.lightRadius), 0, 0.0f);
    check("lightCenter", values(p.lightCenter), 0, 0.0f);
    check("shaderParms", values(p.shaderParms), 0, 0.0f);

    if (light.badTarget)
    {
      CHECK_FALSE(block.expect.contains("lightProject"));
      continue;
    }
    // the derivation: plain float arithmetic within a few ulps (the axis' ulps carry
    // through); what goes through _mm_rcp32_ps (Inverse, ProjectedBounds) against the
    // script's exact inverse, within 1e-5 of the field's magnitude
    const auto& d = derived.projection;
    check("zScale", {d.zScale}, 4, 0.0f);
    check("lightProject", values(d.lightProject), 16, 1.0e-6f);
    check("globalLightOrigin", values(d.globalLightOrigin), 4, 1.0e-6f);
    check("baseLightProject", values(d.baseLightProject), 16, 1.0e-5f);
    check("inverseBaseLightProject", values(d.inverseBaseLightProject), 16, 1.0e-5f);
    check("globalLightBounds", values(d.globalLightBounds), 16, 1.0e-5f);
  }
  CHECK(lights == 16);
}

TEST_CASE("LightGoldens: CalcFov and the projection within 1 ulp (A16)")
{
  auto views = 0;
  for (const auto& block : readGoldens())
  {
    if (block.kind != "view")
    {
      continue;
    }
    ++views;
    auto camera = CameraState{};
    camera.fovDegrees = parseFloat(block.header[0]);
    camera.viewportWidth = std::uint32_t(std::stoul(block.header[1]));
    camera.viewportHeight = std::uint32_t(std::stoul(block.header[2]));
    const auto view = viewProjectionOf(camera);

    INFO(fmt::format(
      "fov {} viewport {} {}", block.header[0], block.header[1], block.header[2]));
    auto why = std::string{};
    CHECK(close({view.gFov}, floats(block.expect.at("gFov")), 1, 0.0f, why));
    CHECK(close({view.fov.x}, floats(block.expect.at("fovX")), 1, 0.0f, why));
    CHECK(close({view.fov.y}, floats(block.expect.at("fovY")), 1, 0.0f, why));
    const auto ok = close(
      values(view.projectionMatrix), floats(block.expect.at("projection")), 1, 0.0f, why);
    INFO(why);
    CHECK(ok);
  }
  // 16:9, 4:3 and square viewports, three fovs each
  CHECK(views == 9);
}

TEST_CASE("LightGoldens: the view axis and matrix within 1 ulp")
{
  auto cameras = 0;
  for (const auto& block : readGoldens())
  {
    if (block.kind != "camera")
    {
      continue;
    }
    ++cameras;
    const auto h = floats(block.header);
    auto camera = CameraState{};
    camera.position = vm::vec3f{h[0], h[1], h[2]};
    camera.direction = vm::vec3f{h[3], h[4], h[5]};
    camera.up = vm::vec3f{h[6], h[7], h[8]};
    camera.viewportWidth = 1920;
    camera.viewportHeight = 1080;
    const auto view = viewProjectionOf(camera);
    auto why = std::string{};
    const auto axisOk =
      close(values(view.viewAxis), floats(block.expect.at("axis")), 1, 0.0f, why);
    INFO(why);
    CHECK(axisOk);
    const auto ok = close(
      values(view.modelViewMatrix), floats(block.expect.at("modelView")), 1, 0.0f, why);
    INFO(why);
    CHECK(ok);
  }
  CHECK(cameras == 2);
}

} // namespace tb::lit
