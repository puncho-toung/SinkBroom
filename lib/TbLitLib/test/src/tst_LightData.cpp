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
#include "lit/CpuPool.h"
#include "lit/EntityState.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <cmath>
#include <regex>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// node 0 splits at x = 0: in front node 1, behind area 1; node 1 splits at y = 0: in
// front area 0, behind solid
ProcTree twoAreaTree()
{
  auto tree = ProcTree{};
  tree.numAreas = 2;
  tree.nodes.push_back(ProcNode{{1, 0, 0, 0}, {1, -2}});
  tree.nodes.push_back(ProcNode{{0, 1, 0, 0}, {-1, 0}});
  return tree;
}

ParsedLight pointLightAt(const std::string& origin, const std::string& radius)
{
  return parseLight({{"origin", origin}, {"light_radius", radius}});
}

bool near(const float a, const float b)
{
  return std::abs(a - b) <= 1.0e-4f * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}

// one line of listRenderLightDefs: "%4i: %3i intr %2i refs %s"
struct ListedLight
{
  int refs = 0;
  std::string shader;
};

std::vector<ListedLight> listedLights(const std::string& text)
{
  static const auto line = std::regex{R"(^\s*\d+:\s+\d+ intr\s+(\d+) refs (\S+)\s*$)"};
  auto lights = std::vector<ListedLight>{};
  auto in = std::istringstream{text};
  auto row = std::string{};
  while (std::getline(in, row))
  {
    auto match = std::smatch{};
    if (std::regex_match(row, match, line))
    {
      lights.push_back({std::stoi(match[1].str()), match[2].str()});
    }
  }
  return lights;
}

} // namespace

TEST_CASE("LightData: commonChildrenArea")
{
  const auto tree = twoAreaTree();
  // node 1: area 0 and solid, which matches any; node 0: area 0 and area 1
  CHECK(commonChildrenAreas(tree) == std::vector<int>{MultipleAreas, 0});

  auto solid = ProcTree{};
  solid.numAreas = 1;
  solid.nodes.push_back(ProcNode{{1, 0, 0, 0}, {0, 0}});
  CHECK(commonChildrenAreas(solid) == std::vector<int>{-1});

  // a malformed tree's cycle (node 1 is its own child) counts as multiple areas and ends
  auto cycle = ProcTree{};
  cycle.numAreas = 2;
  cycle.nodes.push_back(ProcNode{{1, 0, 0, 0}, {1, -2}});
  cycle.nodes.push_back(ProcNode{{0, 1, 0, 0}, {1, -1}});
  CHECK(commonChildrenAreas(cycle) == std::vector<int>{MultipleAreas, MultipleAreas});
}

TEST_CASE("LightData: PushFrustumIntoTree")
{
  const auto tree = twoAreaTree();
  const auto common = commonChildrenAreas(tree);
  const auto areasOf = [&](const ParsedLight& light) {
    return deriveLight(light, nullptr, &tree, &common).areas;
  };

  // in front of both planes: area 0
  CHECK(areasOf(pointLightAt("10 10 0", "5 5 5")) == std::vector<int>{0});
  // across x = 0: child 0's area first, then child 1's
  CHECK(areasOf(pointLightAt("0 10 0", "5 5 5")) == std::vector<int>{0, 1});
  // behind node 1 is solid: nothing
  CHECK(areasOf(pointLightAt("10 -10 0", "5 5 5")).empty());
  // behind node 0 only
  CHECK(areasOf(pointLightAt("-10 10 0", "5 5 5")) == std::vector<int>{1});
  // a box ending exactly on the plane x = 0 has corners with d = +0: in front too
  CHECK(areasOf(pointLightAt("-5 10 0", "5 5 5")) == std::vector<int>{0, 1});

  // guards of a malformed tree: an area beyond numAreas is not referenced, and a cycle
  // (node 1 its own child) is walked once
  auto broken = twoAreaTree();
  broken.nodes[1].children = {-7, 0};
  CHECK(deriveLight(pointLightAt("10 10 0", "5 5 5"), nullptr, &broken, nullptr)
          .areas.empty());
  auto cycle = twoAreaTree();
  cycle.nodes[1].children = {-1, 1};
  CHECK(
    deriveLight(pointLightAt("10 0 0", "5 5 5"), nullptr, &cycle, nullptr).areas
    == std::vector<int>{0});

  // areaNum: the light origin's area; without a tree none, and no references
  const auto derived = deriveLight(pointLightAt("-10 10 0", "5 5 5"), nullptr, &tree);
  CHECK(derived.hasAreaTree);
  CHECK(derived.areaNum == 1);
  const auto noTree = deriveLight(pointLightAt("-10 10 0", "5 5 5"), nullptr, nullptr);
  CHECK_FALSE(noTree.hasAreaTree);
  CHECK(noTree.areaNum == -1);
  CHECK(noTree.areas.empty());
  CHECK(noTree.hasVolume);
}

TEST_CASE("LightData: a point light's projection (C5)")
{
  const auto derived =
    deriveLight(pointLightAt("64 -32 16", "100 50 25"), nullptr, nullptr);
  const auto& p = derived.projection;
  CHECK(derived.hasVolume);
  CHECK(p.zScale == 1.0f);
  // lightProject[0] = ( 0.5 / rx, 0, 0, 0.5 ) moved to the origin, [2] the W row (0 0 0
  // 1)
  CHECK(p.lightProject[0].a == 0.5f / 100.0f);
  CHECK(p.lightProject[0].d == 0.5f - 64.0f * (0.5f / 100.0f));
  CHECK(p.lightProject[2].a == 0.0f);
  CHECK(p.lightProject[2].d == 1.0f);
  CHECK(p.lightProject[3].c == 0.5f / 25.0f);
  CHECK(p.globalLightOrigin == EngineVec3{64, -32, 16});
  // the zero-to-one cube through the inverse: the light's box
  CHECK(near(p.globalLightBounds[0][0], -36.0f));
  CHECK(near(p.globalLightBounds[1][1], 18.0f));
  CHECK(near(p.globalLightBounds[1][2], 41.0f));
}

TEST_CASE("LightData: the light shader and its falloff (A15)")
{
  const auto table = lightMaterialTable({"lights/lp1b_missing"});

  SECTION("the texture's material and its own falloff")
  {
    const auto derived =
      deriveLight(pointLightAt("0 0 0", "10 10 10"), table.get(), nullptr);
    CHECK(derived.shader.name == "lights/squarelight1");
    CHECK_FALSE(derived.shader.defaultLight);
    CHECK_FALSE(derived.shader.pending);
    REQUIRE(derived.shader.falloffImage);
    CHECK_FALSE(derived.shader.defaultFalloff);
  }

  SECTION("no falloff image: the default light's of the kind")
  {
    auto light = pointLightAt("0 0 0", "10 10 10");
    light.parms.texture = "lights/lp1b_nofalloff";
    const auto derived = deriveLight(light, table.get(), nullptr);
    CHECK(derived.shader.name == "lights/lp1b_nofalloff");
    CHECK(derived.shader.defaultFalloff);
    REQUIRE(derived.shader.falloffImage);
  }

  SECTION("a texture without a decl: FindMaterial( texture, false ) is NULL")
  {
    const auto point =
      deriveLight(parseLight({{"texture", "lights/lp1b_missing"}}), table.get(), nullptr);
    CHECK(point.shader.defaultLight);
    CHECK(point.shader.name == "lights/defaultpointlight");
    const auto spot = deriveLight(
      parseLight(
        {{"texture", "lights/lp1b_missing"},
         {"light_target", "0 0 -64"},
         {"light_up", "0 32 0"},
         {"light_right", "32 0 0"}}),
      table.get(),
      nullptr);
    CHECK(spot.shader.name == "lights/defaultprojectedlight");
  }

  SECTION("a material the table does not have yet: pending")
  {
    auto light = pointLightAt("0 0 0", "10 10 10");
    light.parms.texture = "lights/not_looked_up";
    CHECK(deriveLight(light, table.get(), nullptr).shader.pending);
    CHECK(deriveLight(pointLightAt("0 0 0", "1 1 1"), nullptr, nullptr).shader.pending);
  }

  SECTION("bad target info: no shader asked, no volume")
  {
    const auto derived =
      deriveLight(parseLight({{"light_target", "0 0 -64"}}), table.get(), nullptr);
    CHECK_FALSE(derived.hasVolume);
    CHECK(derived.shader.name == "lights/defaultprojectedlight");
    CHECK(derived.areas.empty());
  }

  SECTION("the names to look up")
  {
    CHECK(
      lightMaterialNames(pointLightAt("0 0 0", "1 1 1"))
      == std::vector<std::string>{"lights/squarelight1", DefaultPointLightMaterial});
  }
}

TEST_CASE("LightData: the derivation runs on the CPU pool (C24)")
{
  auto pool =
    CpuPool::create() | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  const auto tree = std::make_shared<const ProcTree>(twoAreaTree());
  auto future = pool->run([tree](const CpuJobContext&) -> Result<DerivedLight> {
    return deriveLight(pointLightAt("0 10 0", "5 5 5"), nullptr, tree.get());
  });
  const auto result = future.get();
  CHECK(result.messages.empty());
  CHECK(
    (result.result | kdl::transform([](const DerivedLight& d) { return d.areas; })
     | kdl::value())
    == std::vector<int>{0, 1});
  pool->stop();
}

TEST_CASE("LightData: area references and shaders equal listRenderLightDefs (D58)")
{
  // goldens/lp1b_geometry/listRenderLightDefs.txt: the engine's listing after map, one
  // line per light in spawn order (the map's order: no light of the fixture starts off)
  const auto root = litFixtureFolder();
  const auto goldens = root / "goldens" / "lp1b_geometry";
  const auto listed = listedLights(readTextFile(goldens / "listRenderLightDefs.txt"));
  const auto tree = readProcFile(readTextFile(goldens / "lp1b_geometry.proc"))
                    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  const auto snapshot =
    snapshotOfMapText(readTextFile(root / "maps" / "lp1b_geometry.map"));
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  // the lights' textures that are not stand-ins have no decl (lights/lp1b_*)
  auto lights = std::vector<ParsedLight>{};
  auto missing = std::vector<std::string>{};
  for (const auto& node : snapshot.nodes)
  {
    if (node.kind != SnapshotNodeKind::Light)
    {
      continue;
    }
    const auto& entity = snapshot.entities[node.elements.first];
    auto keys = EntityKeys{};
    for (auto i = entity.keys.first; i < entity.keys.first + entity.keys.count; ++i)
    {
      const auto& kv = snapshot.keyValues[i];
      keys.emplace_back(snapshot.string(kv.key), snapshot.string(kv.value));
    }
    // the retail light entityDef sets none of the keys the derivation reads (the
    // [retail] test checks it), so the map's keys are the merged dictionary
    lights.push_back(parseLight(mergeSpawnArgs(keys, nullptr)));
    if (lights.back().parms.texture.rfind("lights/lp1b_", 0) == 0)
    {
      missing.push_back(lights.back().parms.texture);
    }
  }
  const auto table = lightMaterialTable(missing);
  const auto common = commonChildrenAreas(tree);

  REQUIRE(lights.size() == listed.size());
  for (auto i = std::size_t{0}; i < lights.size(); ++i)
  {
    const auto derived = deriveLight(lights[i], table.get(), &tree, &common);
    INFO(fmt::format(
      "light {} at ({} {} {}): areas {}",
      i,
      lights[i].parms.origin[0],
      lights[i].parms.origin[1],
      lights[i].parms.origin[2],
      fmt::join(derived.areas, " ")));
    CHECK(int(derived.areas.size()) == listed[i].refs);
    CHECK(derived.shader.name == listed[i].shader);
    CHECK_FALSE(derived.shader.pending);
  }
}

} // namespace tb::lit
