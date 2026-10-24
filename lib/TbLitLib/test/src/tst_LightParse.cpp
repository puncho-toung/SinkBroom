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

#include "lit/EngineLight.h"
#include "lit/EntityState.h"
#include "lit/LightParse.h"

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

const auto Identity = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};

// the parse's last step: FixDegenerateNormal on each row
EngineMat3 fixedRows(const EngineMat3& m)
{
  auto result = m;
  for (auto row = std::size_t{0}; row < 3; ++row)
  {
    const auto fixed =
      engineFixDegenerateNormal({m[row * 3], m[row * 3 + 1], m[row * 3 + 2]});
    result[row * 3] = fixed[0];
    result[row * 3 + 1] = fixed[1];
    result[row * 3 + 2] = fixed[2];
  }
  return result;
}

} // namespace

// These check the parse's control flow and its fallbacks; the values of each key path are
// compared with an independent script in tst_LightGoldens.cpp.

TEST_CASE("LightParse: a light with no keys")
{
  const auto light = parseLight({{"classname", "light"}, {"origin", "1 2 3"}});
  const auto& parms = light.parms;
  CHECK_FALSE(light.badTarget);
  CHECK(light.warnings.empty());
  CHECK(parms.origin == EngineVec3{1, 2, 3});
  CHECK(parms.pointLight);
  CHECK_FALSE(parms.parallel);
  CHECK(parms.lightRadius == EngineVec3{300, 300, 300});
  CHECK(parms.lightCenter == EngineVec3{0, 0, 0});
  CHECK(parms.axis == Identity);
  CHECK(parms.shaderParms[0] == 1.0f);
  CHECK(parms.shaderParms[3] == 1.0f);
  // -MS2SEC( 0 ): negative zero
  CHECK(parms.shaderParms[4] == 0.0f);
  CHECK(std::signbit(parms.shaderParms[4]));
  CHECK(parms.texture == "lights/squarelight1");
  CHECK(parms.mathLight.type == int(MathLightType::Point));
  CHECK_FALSE(parms.mathLight.enabled);
  CHECK(parms.mathLight.style == -1);
  CHECK(light.spawn.levels == 1);
  CHECK(light.spawn.styles == engineDefaultLightStyles());
  CHECK_FALSE(light.spawn.radiusStyle);
  CHECK_FALSE(light.spawn.mathStyleBrightness);
}

TEST_CASE("LightParse: the merged dictionary's lookups")
{
  // "light" when light_radius is missing; keys compared without case, the last one wins
  const auto defKeys = EntityKeys{{"light", "200"}, {"texture", "lights/def_texture"}};
  const auto merged =
    mergeSpawnArgs({{"origin", "0 0 0"}, {"LIGHT", "100"}, {"light", "150"}}, &defKeys);
  const auto light = parseLight(merged);
  CHECK(light.parms.lightRadius == EngineVec3{150, 150, 150});
  // the entityDef's keys fill what the map leaves out (A10)
  CHECK(light.parms.texture == "lights/def_texture");
}

TEST_CASE("LightParse: a partial target set (A15)")
{
  const auto light =
    parseLight({{"origin", "1 2 3"}, {"light_target", "0 0 -64"}, {"_color", "1 0 0"}});
  CHECK(light.badTarget);
  REQUIRE(light.warnings.size() == 1u);
  CHECK(light.warnings[0] == "Light at (1.000000,2.000000,3.000000) has bad target info");
  // what was parsed before the check stays; everything else is the memset's
  CHECK(light.parms.origin == EngineVec3{1, 2, 3});
  CHECK(light.parms.target == EngineVec3{0, 0, -64});
  CHECK(light.parms.end == EngineVec3{0, 0, -64});
  CHECK(light.parms.axis == EngineMat3{});
  CHECK_FALSE(light.parms.pointLight);
  CHECK(light.parms.shaderParms == std::array<float, 12>{});
  CHECK(light.parms.texture.empty());
  CHECK(light.parms.mathLight.style == 0);
}

TEST_CASE("LightParse: projected and parallel lights and the math type")
{
  const auto spot = parseLight(
    {{"light_target", "0 0 -100"},
     {"light_up", "0 50 0"},
     {"light_right", "-50 0 0"},
     {"math_enabled", "1"},
     {"math_type", "point"}});
  CHECK_FALSE(spot.parms.pointLight);
  CHECK(spot.parms.end == spot.parms.target);
  CHECK(spot.parms.lightRadius == EngineVec3{0, 0, 0});
  // D45: the geometry forces the type
  CHECK(spot.parms.mathLight.type == int(MathLightType::Spot));
  CHECK(spot.parms.mathLight.enabled);

  const auto sun =
    parseLight({{"parallel", "1"}, {"math_enabled", "1"}, {"math_type", "?"}});
  CHECK(sun.parms.parallel);
  CHECK(sun.parms.mathLight.type == int(MathLightType::Sun));
  REQUIRE(sun.warnings.size() == 1u);
  CHECK(
    sun.warnings[0]
    == "math_type '?' not recognized; falling back to inferred light type");

  // LP5.0 (D113, P6.12; Light.cpp:207-216): math_type is read and then ignored, the
  // geometry decides: "sun" on a point light (no light_target, no parallel) is a point
  // light, and "point" on a spot light is a spot light, with no warning either way
  const auto point = parseLight(
    {{"light_radius", "64 64 64"}, {"math_enabled", "1"}, {"math_type", "sun"}});
  CHECK(point.parms.pointLight);
  CHECK_FALSE(point.parms.parallel);
  CHECK(point.parms.mathLight.enabled);
  CHECK(point.parms.mathLight.type == int(MathLightType::Point));
  CHECK(point.warnings.empty());
  CHECK(spot.parms.mathLight.type == int(MathLightType::Spot));
  CHECK(spot.warnings.empty());
  // "spot" on a parallel light is a sun
  const auto parallel =
    parseLight({{"parallel", "1"}, {"math_enabled", "1"}, {"math_type", "spot"}});
  CHECK(parallel.parms.pointLight);
  CHECK(parallel.parms.mathLight.type == int(MathLightType::Sun));
}

TEST_CASE("LightParse: the axis keys in their order")
{
  // light_rotation before rotation
  CHECK(
    parseLight(
      {{"light_rotation", "0 1 0 -1 0 0 0 0 1"}, {"rotation", "1 0 0 0 1 0 0 0 1"}})
      .parms.axis
    == EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1});
  // FixDegenerateNormal on each row: a zero row becomes (0 0 -1), an axial one a unit
  // axis
  CHECK(
    parseLight({{"rotation", "0 0 0 0 2 0 0 0 0"}}).parms.axis
    == EngineMat3{0, 0, -1, 0, 1, 0, 0, 0, -1});
  // angle is normalised to 0-360; FixDegenerateNormal snaps the near-axial rows
  CHECK(
    parseLight({{"angle", "-90"}}).parms.axis
    == fixedRows(engineAnglesToMat3(0, engineAngleNormalize360(-90.0f), 0)));
  // light_angles before angles, both normalised
  CHECK(
    parseLight({{"light_angles", "10 -20 400"}, {"angles", "0 90 0"}}).parms.axis
    == fixedRows(engineAnglesToMat3(10.0f, 340.0f, engineAngleNormalize360(400.0f))));
  CHECK(
    parseLight({{"angles", "0 90 0"}, {"angle", "45"}}).parms.axis
    == fixedRows(engineAnglesToMat3(0, 90, 0)));
}

TEST_CASE("LightParse: idLight::Spawn's effects at time 0")
{
  SECTION("start_off makes the colour zero")
  {
    const auto light = parseLight({{"_color", "0.5 1 -1"}, {"start_off", "1"}});
    CHECK(light.spawn.startOff);
    CHECK(light.spawn.baseColor == EngineVec3{0.5f, 1, -1});
    CHECK(light.parms.shaderParms[0] == 0.0f);
    CHECK(light.parms.shaderParms[2] == 0.0f);
    // -1 * 0
    CHECK(std::signbit(light.parms.shaderParms[2]));
  }

  SECTION("levels 0 is a game Error: a warning, and the light is kept")
  {
    const auto light = parseLight({{"levels", "0"}, {"start_off", "1"}});
    REQUIRE(light.warnings.size() == 1u);
    CHECK(light.parms.shaderParms[0] == 0.0f);
    CHECK(light.parms.lightRadius == EngineVec3{300, 300, 300});
  }

  SECTION("the radius style plays its first character")
  {
    const auto light = parseLight({{"light_radius", "100 100 50"}, {"style", "0"}});
    // predef_lightstyles[0] = "m": 12 * 0.071429
    const auto value = 12.0f * 0.071429f;
    CHECK(light.spawn.radiusStyle == value);
    CHECK(
      light.parms.lightRadius
      == EngineVec3{value * 100.0f, value * 100.0f, value * 50.0f});
    CHECK(light.spawn.baseRadius == EngineVec3{100, 100, 50});
  }

  SECTION("num_styles replaces the table; an empty style string gives radius 0")
  {
    const auto light = parseLight(
      {{"num_styles", "2"}, {"light_style1", ""}, {"light_style0", "z"}, {"style", "1"}});
    CHECK(light.spawn.styles == std::vector<std::string>{"z", ""});
    CHECK(light.spawn.radiusStyle == 0.0f);
    CHECK(light.parms.lightRadius == EngineVec3{0, 0, 0});
  }

  SECTION("the math style scales the colour")
  {
    const auto light =
      parseLight({{"math_enabled", "1"}, {"math_style", "4"}, {"_color", "0.5 0.5 0.5"}});
    // predef_lightstyles[4] = "mamamamamama": 'm' / 12
    REQUIRE(light.spawn.mathStyleBrightness);
    CHECK(*light.spawn.mathStyleBrightness == 12.0f * (1.0f / 12.0f));
    CHECK(light.parms.shaderParms[0] == 0.5f * *light.spawn.mathStyleBrightness);
    // a style outside the table is none
    CHECK(
      parseLight({{"math_enabled", "1"}, {"math_style", "12"}}).parms.mathLight.style
      == -1);
  }

  SECTION("the game's keys are recorded, not acted on")
  {
    const auto light =
      parseLight({{"health", "10"}, {"modelTarget", "lamp"}, {"broken", "x.lwo"}});
    CHECK(
      light.ignoredKeys
      == std::vector<std::pair<std::string, std::string>>{
        {"health", "10"}, {"broken", "x.lwo"}, {"modelTarget", "lamp"}});
  }
}

TEST_CASE("LightParse: ML_StyleMultiplier")
{
  CHECK(mathLightStyleMultiplier("", 500, 100) == 1.0f);
  CHECK(mathLightStyleMultiplier("a", 0, 100) == 0.0f);
  CHECK(mathLightStyleMultiplier("z", 0, 100) == 25.0f * (1.0f / 12.0f));
  // half a frame between 'a' and 'm'
  CHECK(
    mathLightStyleMultiplier("am", 50, 100)
    == (0.0f * 0.5f + 12.0f * 0.5f) * (1.0f / 12.0f));
  // characters outside a-z are clamped
  CHECK(mathLightStyleMultiplier("~", 0, 0) == 25.0f * (1.0f / 12.0f));
}

} // namespace tb::lit
