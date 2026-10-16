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

#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityProperties.h"
#include "mdl/Light.h"
#include "mdl/PropertyDefinition.h"

#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

// the four shapes of light the in-game editor creates
const auto defaultPointProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light", "300"},
};

const auto defaultSpotProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light_target", "0 0 -256"},
  {"light_up", "0 -128 0"},
  {"light_right", "-128 0 0"},
};

const auto sunProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"parallel", "1"},
  {"light_radius", "2048 2048 1024"},
};

// parallel wins over the target keys, which the engine then reads but never uses
const auto sunWithUnreadTargetProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"parallel", "1"},
  {"light_target", "0 0 -256"},
  {"light_up", "0 -128 0"},
  {"light_right", "-100 0 0"},
};

const auto mathLightProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light_radius", "300 300 300"},
  {"math_enabled", "1"},
  {"math_type", "point"},
  {"math_falloff", "linear"},
  {"math_falloff_exp", "3"},
  {"math_inner_radius", "0.1"},
  {"math_shape_exp", "4"},
  {"math_penumbra", "0.2"},
  {"math_pattern", "ice"},
  {"math_pattern_strength", "0.5"},
  {"math_pattern_scale", "2"},
  {"math_pattern_phase", "0.25"},
  {"math_energy_clamp", "0"},
};

std::vector<EntityProperty> with(
  std::vector<EntityProperty> properties, const std::vector<EntityProperty>& changes)
{
  for (const auto& change : changes)
  {
    if (auto it = findEntityProperty(properties, change.key()); it != properties.end())
    {
      it->setValue(change.value());
    }
    else
    {
      properties.push_back(change);
    }
  }
  return properties;
}

bool contains(const std::vector<std::string>& strs, const std::string& str)
{
  return std::ranges::find(strs, str) != strs.end();
}

std::vector<std::string> sorted(std::vector<std::string> strs)
{
  std::ranges::sort(strs);
  return strs;
}

std::vector<std::string> keys(const std::vector<EntityProperty>& properties)
{
  auto result = std::vector<std::string>{};
  for (const auto& property : properties)
  {
    result.push_back(property.key());
  }
  return sorted(std::move(result));
}

// the twelve keys that an enabled math block without a style writes
std::vector<std::string> withMathKeys(std::vector<std::string> strs)
{
  for (const auto& key : mathLightPropertyKeys())
  {
    if (key != "math_style")
    {
      strs.push_back(key);
    }
  }
  return sorted(std::move(strs));
}

std::string valueOf(const std::vector<EntityProperty>& properties, const std::string& key)
{
  const auto it = findEntityProperty(properties, key);
  return it != properties.end() ? it->value() : "<missing>";
}

LightProperties parse(const std::vector<EntityProperty>& properties)
{
  return parseLightProperties(Entity{properties});
}

} // namespace

TEST_CASE("Light")
{
  SECTION("mathLightPropertyKeys")
  {
    CHECK(
      mathLightPropertyKeys()
      == std::vector<std::string>{
        "math_enabled",
        "math_type",
        "math_falloff",
        "math_pattern",
        "math_falloff_exp",
        "math_inner_radius",
        "math_shape_exp",
        "math_penumbra",
        "math_pattern_strength",
        "math_pattern_scale",
        "math_pattern_phase",
        "math_energy_clamp",
        "math_style",
      });
    CHECK(mathLightPropertyKeys().size() == 13);
  }

  SECTION("lightStyleName")
  {
    // predef_lightstylesinfo of the engine's renderer/Material.h
    CHECK(lightStyleName(-1) == "None");
    CHECK(lightStyleName(0) == "Normal");
    CHECK(lightStyleName(1) == "Flicker A");
    CHECK(lightStyleName(2) == "Slow Strong Pulse");
    CHECK(lightStyleName(3) == "Candle A");
    CHECK(lightStyleName(4) == "Fast Strobe");
    CHECK(lightStyleName(5) == "Gentle Pulse");
    CHECK(lightStyleName(6) == "Flicker B");
    CHECK(lightStyleName(7) == "Candle B");
    CHECK(lightStyleName(8) == "Candle C");
    CHECK(lightStyleName(9) == "Slow Strobe");
    CHECK(lightStyleName(10) == "Fluorescent Flicker");
    CHECK(lightStyleName(11) == "Slow Pulse (no black)");
    CHECK(lightStyleName(LightStyleCount).empty());
    CHECK(lightStyleName(-2).empty());
  }

  // the names ML_WriteToDict writes; ML_ParseFromDict compares them without regard to
  // case

  SECTION("mathTypeName")
  {
    CHECK(mathTypeName(LightType::Point) == "point");
    CHECK(mathTypeName(LightType::Spot) == "spot");
    CHECK(mathTypeName(LightType::Sun) == "sun");
  }

  SECTION("parseMathTypeName")
  {
    CHECK(parseMathTypeName("SPOT") == LightType::Spot);
    CHECK(parseMathTypeName("point") == LightType::Point);
    CHECK(parseMathTypeName("Sun") == LightType::Sun);
    CHECK(parseMathTypeName("parallel") == LightType::Sun);
    CHECK(parseMathTypeName("bogus") == std::nullopt);
  }

  SECTION("lightFalloffName")
  {
    CHECK(lightFalloffName(LightFalloff::InvSquare) == "invsquare");
    CHECK(lightFalloffName(LightFalloff::Linear) == "linear");
    CHECK(lightFalloffName(LightFalloff::Smooth) == "smooth");
    CHECK(lightFalloffName(LightFalloff::Exponential) == "exponential");
    CHECK(lightFalloffName(LightFalloff::InvSquareClamped) == "invsquare_clamped");
  }

  SECTION("parseLightFalloffName")
  {
    CHECK(parseLightFalloffName("Invsquare_Clamped") == LightFalloff::InvSquareClamped);
    CHECK(parseLightFalloffName("EXPONENTIAL") == LightFalloff::Exponential);
    CHECK(parseLightFalloffName("linear") == LightFalloff::Linear);
    CHECK(parseLightFalloffName("") == std::nullopt);
  }

  SECTION("lightPatternName")
  {
    CHECK(lightPatternName(LightPattern::None) == "none");
    CHECK(lightPatternName(LightPattern::Subsurface) == "subsurface");
    CHECK(lightPatternName(LightPattern::Ice) == "ice");
    CHECK(lightPatternName(LightPattern::Caustic) == "caustic");
    CHECK(lightPatternName(LightPattern::Banded) == "banded");
    CHECK(lightPatternName(LightPattern::Granular) == "granular");
  }

  SECTION("parseLightPatternName")
  {
    CHECK(parseLightPatternName("Granular") == LightPattern::Granular);
    CHECK(parseLightPatternName("none") == LightPattern::None);
    CHECK(parseLightPatternName("noise") == std::nullopt);
  }

  SECTION("isLightEntity")
  {
    CHECK(isLightEntity(Entity{{{"classname", "light"}}}));
    CHECK(isLightEntity(Entity{{{"classname", "LIGHT"}}}));
    // without a definition, the light_ prefix decides
    CHECK(isLightEntity(Entity{{{"classname", "Light_Fixture"}}}));
    CHECK_FALSE(isLightEntity(Entity{{{"classname", "lightning"}}}));
    CHECK_FALSE(isLightEntity(Entity{{{"classname", "func_static"}}}));
    CHECK_FALSE(isLightEntity(Entity{}));

    const auto makeDefinition = [](std::vector<PropertyDefinition> propertyDefinitions) {
      return EntityDefinition{
        "some_name",
        Color{},
        "",
        std::move(propertyDefinitions),
        PointEntityDefinition{vm::bbox3d{8.0}, {}, {}},
      };
    };

    const auto idLightDefinition = makeDefinition({PropertyDefinition{
      "spawnclass", PropertyValueTypes::String{.defaultValue = "idLight"}, "", ""}});
    const auto otherDefinition = makeDefinition({PropertyDefinition{
      "spawnclass",
      PropertyValueTypes::String{.defaultValue = "idStaticEntity"},
      "",
      ""}});
    const auto emptyDefinition = makeDefinition({});

    // a definition that declares spawnclass idLight makes any class a light
    auto lamp = Entity{{{"classname", "my_lamp"}}};
    lamp.setDefinition(&idLightDefinition);
    CHECK(isLightEntity(lamp));

    // with a definition, the light_ prefix does not count
    auto lightFixture = Entity{{{"classname", "light_fixture"}}};
    lightFixture.setDefinition(&otherDefinition);
    CHECK_FALSE(isLightEntity(lightFixture));

    auto lightFixtureWithoutSpawnclass = Entity{{{"classname", "light_fixture"}}};
    lightFixtureWithoutSpawnclass.setDefinition(&emptyDefinition);
    CHECK_FALSE(isLightEntity(lightFixtureWithoutSpawnclass));

    // the classname light is always a light
    auto light = Entity{{{"classname", "light"}}};
    light.setDefinition(&otherDefinition);
    CHECK(isLightEntity(light));
  }

  SECTION("parseLightProperties")
  {
    SECTION("A default point light")
    {
      CHECK(parse(defaultPointProperties) == LightProperties{});
    }

    SECTION("A default spot light")
    {
      CHECK(
        parse(defaultSpotProperties)
        == LightProperties{
          .type = LightType::Spot,
          .target = {0, 0, -256},
          .right = {-128, 0, 0},
          .up = {0, -128, 0},
          .explicitStartEnd = false,
          .start = {0, 0, 0},
          .end = {0, 0, -256},
        });
    }

    SECTION("light_start makes the start and end points explicit")
    {
      const auto light = parse(with(
        defaultSpotProperties, {{"light_start", "0 0 -32"}, {"light_end", "0 0 -200"}}));
      CHECK(light.type == LightType::Spot);
      CHECK(light.explicitStartEnd);
      CHECK(light.start == vm::vec3d{0, 0, -32});
      CHECK(light.end == vm::vec3d{0, 0, -200});

      // light_end defaults to the target
      const auto withoutEnd =
        parse(with(defaultSpotProperties, {{"light_start", "0 0 -32"}}));
      CHECK(withoutEnd.explicitStartEnd);
      CHECK(withoutEnd.end == vm::vec3d{0, 0, -256});
    }

    SECTION("A sun")
    {
      CHECK(
        parse(sunProperties)
        == LightProperties{
          .type = LightType::Sun,
          .radius = {2048, 2048, 1024},
          .equalRadius = false,
          .hasCenter = false,
        });
    }

    SECTION("A math light")
    {
      CHECK(
        parse(mathLightProperties).math
        == MathLightProperties{
          .enabled = true,
          .falloff = LightFalloff::Linear,
          .falloffExp = 3.0,
          .innerRadius = 0.1,
          .shapeExp = 4.0,
          .penumbra = 0.2,
          .pattern = LightPattern::Ice,
          .patternStrength = 0.5,
          .patternScale = 2.0,
          .patternPhase = 0.25,
          .energyClamp = false,
        });
    }

    SECTION("Partial target keys are bad target info and read as a point light")
    {
      for (const auto& partial : std::vector<std::vector<EntityProperty>>{
             {{"light_target", "0 0 -256"}},
             {{"light_right", "-128 0 0"}},
             {{"light_target", "0 0 -256"}, {"light_up", "0 -128 0"}},
           })
      {
        CAPTURE(partial);
        const auto light = parse(with(defaultPointProperties, partial));
        CHECK(light.badTargetInfo);
        CHECK(light.type == LightType::Point);
        CHECK(light.radius == vm::vec3d{300, 300, 300});
      }
      CHECK_FALSE(parse(defaultSpotProperties).badTargetInfo);
    }

    SECTION("parallel makes a sun even with all three target keys")
    {
      // R_DeriveLightData prefers the parallel projection to the spot projection
      const auto light = parse(with(defaultSpotProperties, {{"parallel", "1"}}));
      CHECK(light.type == LightType::Sun);
      CHECK_FALSE(light.badTargetInfo);
      // the target keys stay unread, like a spot's radius
      CHECK(light.target == vm::vec3d{0, 0, 0});
      CHECK(light.right == vm::vec3d{0, 0, 0});
      CHECK(light.up == vm::vec3d{0, 0, 0});
      CHECK(light.radius == vm::vec3d{300, 300, 300});
    }

    SECTION("Partial target keys leave parallel unread")
    {
      // the engine returns on bad target info before it reads parallel
      const auto light = parse(
        with(defaultPointProperties, {{"light_target", "0 0 -256"}, {"parallel", "1"}}));
      CHECK(light.type == LightType::Point);
      CHECK(light.badTargetInfo);
    }

    SECTION("parallel 0 is no sun")
    {
      CHECK(
        parse(with(defaultPointProperties, {{"parallel", "0"}})).type
        == LightType::Point);
      CHECK(
        parse(with(defaultSpotProperties, {{"parallel", "0"}})).type == LightType::Spot);
    }

    SECTION("light_origin wins over origin")
    {
      const auto light = parse(
        with(defaultPointProperties, {{"origin", "1 2 3"}, {"light_origin", "4 5 6"}}));
      CHECK(light.hasLightOrigin);
      CHECK(light.origin == vm::vec3d{4, 5, 6});

      const auto withoutLightOrigin =
        parse(with(defaultPointProperties, {{"origin", "1 2 3"}}));
      CHECK_FALSE(withoutLightOrigin.hasLightOrigin);
      CHECK(withoutLightOrigin.origin == vm::vec3d{1, 2, 3});
    }

    SECTION("_color is read as HDR")
    {
      CHECK(parse(defaultPointProperties).color == vm::vec3d{1, 1, 1});
      CHECK(
        parse(with(defaultPointProperties, {{"_color", "2 2 2"}})).color
        == vm::vec3d{2, 2, 2});
      CHECK(
        parse(with(defaultPointProperties, {{"_color", "0.5 0.25 1"}})).color
        == vm::vec3d{0.5, 0.25, 1});
      // like the engine's sscanf
      CHECK(
        parse(with(defaultPointProperties, {{"_color", "1"}})).color
        == vm::vec3d{1, 0, 0});
    }

    SECTION("The radius")
    {
      CHECK(
        parse(with(defaultPointProperties, {{"light", "0"}})).radius
        == vm::vec3d{0, 0, 0});
      CHECK(
        parse(with(defaultPointProperties, {{"light", "200"}})).radius
        == vm::vec3d{200, 200, 200});
      CHECK(parse({{"classname", "light"}}).radius == vm::vec3d{300, 300, 300});

      // light_radius wins over light
      const auto light =
        parse(with(defaultPointProperties, {{"light_radius", "100 200 300"}}));
      CHECK(light.radius == vm::vec3d{100, 200, 300});
      CHECK_FALSE(light.equalRadius);

      // a vector with one number reads like the engine's sscanf
      const auto oneNumber =
        parse(with(defaultPointProperties, {{"light_radius", "200"}}));
      CHECK(oneNumber.radius == vm::vec3d{200, 0, 0});
      CHECK_FALSE(oneNumber.equalRadius);
    }

    SECTION("An equal light_radius")
    {
      const auto light =
        parse(with(defaultPointProperties, {{"light_radius", "200 200 200"}}));
      CHECK(light.radius == vm::vec3d{200, 200, 200});
      CHECK(light.equalRadius);
    }

    SECTION("light_center")
    {
      const auto light =
        parse(with(defaultPointProperties, {{"light_center", "0 0 16"}}));
      CHECK(light.hasCenter);
      CHECK(light.center == vm::vec3d{0, 0, 16});
    }

    SECTION("The other keys")
    {
      const auto light = parse(with(
        defaultPointProperties,
        {{"texture", "lights/round"},
         {"style", "3"},
         {"noshadows", "1"},
         {"nospecular", "1"}}));
      CHECK(light.texture == "lights/round");
      CHECK(light.style == 3);
      CHECK_FALSE(light.castShadows);
      CHECK(light.skipSpecular);
    }

    SECTION("math_type never changes the light's type")
    {
      const auto light = parse(with(mathLightProperties, {{"math_type", "sun"}}));
      CHECK(light.type == LightType::Point);
      CHECK(light.math.enabled);
    }

    SECTION("math_enabled 0 disables the math block and keeps its values")
    {
      // the values of a disabled block are read too (D51)
      CHECK(
        parse(with(mathLightProperties, {{"math_enabled", "0"}})).math
        == MathLightProperties{
          .enabled = false,
          .falloff = LightFalloff::Linear,
          .falloffExp = 3.0,
          .innerRadius = 0.1,
          .shapeExp = 4.0,
          .penumbra = 0.2,
          .pattern = LightPattern::Ice,
          .patternStrength = 0.5,
          .patternScale = 2.0,
          .patternPhase = 0.25,
          .energyClamp = false,
        });
      CHECK(parse(defaultPointProperties).math == MathLightProperties{});
    }

    SECTION("math_style is read like style, and an unknown style as none")
    {
      // like the engine's ML_ParseFromDict, not clamped
      CHECK(parse(mathLightProperties).math.style == -1);
      CHECK(parse(with(mathLightProperties, {{"math_style", "4"}})).math.style == 4);
      CHECK(parse(with(mathLightProperties, {{"math_style", "0"}})).math.style == 0);
      CHECK(parse(with(mathLightProperties, {{"math_style", "11"}})).math.style == 11);
      CHECK(parse(with(mathLightProperties, {{"math_style", "12"}})).math.style == -1);
      CHECK(parse(with(mathLightProperties, {{"math_style", "-3"}})).math.style == -1);
      CHECK(parse(with(mathLightProperties, {{"math_style", "bogus"}})).math.style == 0);
      CHECK(
        parse(with(mathLightProperties, {{"math_enabled", "0"}, {"math_style", "4"}}))
          .math.style
        == 4);
    }

    SECTION("The math values are clamped like ML_ParseFromDict clamps them")
    {
      const auto high = parse(with(
                                mathLightProperties,
                                {{"math_falloff_exp", "100"},
                                 {"math_inner_radius", "2"},
                                 {"math_shape_exp", "64"},
                                 {"math_penumbra", "2"},
                                 {"math_pattern_strength", "5"},
                                 {"math_pattern_scale", "-3"},
                                 {"math_pattern_phase", "7.5"}}))
                          .math;
      CHECK(high.falloffExp == 16.0);
      CHECK(high.innerRadius == 1.0);
      CHECK(high.shapeExp == 32.0);
      CHECK(high.penumbra == 1.0);
      CHECK(high.patternStrength == 1.0);
      // scale and phase are not clamped
      CHECK(high.patternScale == -3.0);
      CHECK(high.patternPhase == 7.5);

      const auto low = parse(with(
                               mathLightProperties,
                               {{"math_falloff_exp", "0"},
                                {"math_inner_radius", "-1"},
                                {"math_shape_exp", "1"},
                                {"math_penumbra", "-1"},
                                {"math_pattern_strength", "-1"}}))
                         .math;
      CHECK(low.falloffExp == 0.1);
      CHECK(low.innerRadius == 0.0);
      CHECK(low.shapeExp == 1.5);
      CHECK(low.penumbra == 0.0);
      CHECK(low.patternStrength == 0.0);
    }

    SECTION("Missing and unknown math values")
    {
      const auto light = parse({
        {"classname", "light"},
        {"math_enabled", "1"},
        {"math_falloff", "bogus"},
        {"math_pattern", "CAUSTIC"},
      });
      CHECK(
        light.math
        == MathLightProperties{
          .enabled = true,
          .falloff = LightFalloff::InvSquareClamped,
          .pattern = LightPattern::Caustic,
        });
    }
  }

  SECTION("lightPropertyUpdate")
  {
    const auto pointRemoved = std::vector<std::string>{
      "light",
      "texture",
      "light_center",
      "parallel",
      "light_target",
      "light_up",
      "light_right",
      "light_start",
      "light_end",
      "style",
    };

    SECTION("A point light")
    {
      // a light without a math block says so and deletes none of its keys (D51)
      const auto update = lightPropertyUpdate(parse(defaultPointProperties));
      CHECK(
        keys(update.set)
        == sorted(
          {"origin",
           "noshadows",
           "nospecular",
           "_color",
           "light_radius",
           "math_enabled"}));
      CHECK(sorted(update.remove) == sorted(pointRemoved));
      CHECK(valueOf(update.set, "math_enabled") == "0");

      CHECK(valueOf(update.set, "origin") == "0 0 0");
      CHECK(valueOf(update.set, "noshadows") == "0");
      CHECK(valueOf(update.set, "nospecular") == "0");
      CHECK(valueOf(update.set, "_color") == "1 1 1");
      CHECK(valueOf(update.set, "light_radius") == "300 300 300");
    }

    SECTION("A spot light")
    {
      const auto spotRemoved = std::vector<std::string>{
        "light",
        "texture",
        "light_start",
        "light_end",
        "light_radius",
        "light_center",
        "parallel",
        "style",
      };

      const auto update = lightPropertyUpdate(parse(defaultSpotProperties));
      CHECK(
        keys(update.set)
        == sorted(
          {"origin",
           "noshadows",
           "nospecular",
           "_color",
           "light_target",
           "light_up",
           "light_right",
           "math_enabled"}));
      CHECK(sorted(update.remove) == sorted(spotRemoved));

      CHECK(valueOf(update.set, "light_target") == "0 0 -256");
      CHECK(valueOf(update.set, "light_up") == "0 -128 0");
      CHECK(valueOf(update.set, "light_right") == "-128 0 0");

      const auto explicitUpdate = lightPropertyUpdate(parse(with(
        defaultSpotProperties, {{"light_start", "0 0 -32"}, {"light_end", "0 0 -200"}})));
      CHECK(
        keys(explicitUpdate.set)
        == sorted(
          {"origin",
           "noshadows",
           "nospecular",
           "_color",
           "light_target",
           "light_up",
           "light_right",
           "light_start",
           "light_end",
           "math_enabled"}));
      CHECK(
        sorted(explicitUpdate.remove)
        == sorted(
          {"light", "texture", "light_radius", "light_center", "parallel", "style"}));
      CHECK(valueOf(explicitUpdate.set, "light_start") == "0 0 -32");
      CHECK(valueOf(explicitUpdate.set, "light_end") == "0 0 -200");
    }

    SECTION("A sun")
    {
      const auto update = lightPropertyUpdate(parse(sunProperties));
      CHECK(
        keys(update.set)
        == sorted(
          {"origin",
           "noshadows",
           "nospecular",
           "_color",
           "light_radius",
           "parallel",
           "math_enabled"}));
      CHECK(
        sorted(update.remove)
        == sorted(
          {"light",
           "texture",
           "light_center",
           "light_target",
           "light_up",
           "light_right",
           "light_start",
           "light_end",
           "style"}));
      CHECK(valueOf(update.set, "light_radius") == "2048 2048 1024");
      CHECK(valueOf(update.set, "parallel") == "1");
    }

    SECTION("Numbers are written like the engine writes them")
    {
      auto light = LightProperties{};
      light.origin = vm::vec3d{-0.001, 12.5, 100.0};
      light.radius = vm::vec3d{300.0, 300.0, 300.0};
      light.color = vm::vec3d{0.5, 0.25, 1.0};

      auto update = lightPropertyUpdate(light);
      CHECK(valueOf(update.set, "origin") == "0 12.5 100");
      CHECK(valueOf(update.set, "light_radius") == "300 300 300");
      CHECK(valueOf(update.set, "_color") == "0.5 0.25 1");

      light.color = vm::vec3d{2.0, 2.0, 2.0};
      CHECK(valueOf(lightPropertyUpdate(light).set, "_color") == "2 2 2");

      // _color keeps four decimals
      light.color = vm::vec3d{0.123456, 1.0, 1.0};
      CHECK(valueOf(lightPropertyUpdate(light).set, "_color") == "0.1235 1 1");

      // an equal radius writes its first component three times
      light.radius = vm::vec3d{300.0, 200.0, 100.0};
      light.equalRadius = true;
      CHECK(valueOf(lightPropertyUpdate(light).set, "light_radius") == "300 300 300");
      light.equalRadius = false;
      CHECK(valueOf(lightPropertyUpdate(light).set, "light_radius") == "300 200 100");
    }

    SECTION("The flags, texture, style and center")
    {
      auto light = LightProperties{};
      light.castShadows = false;
      light.skipSpecular = true;
      light.texture = "lights/round";
      light.style = 3;
      light.hasCenter = true;
      light.center = vm::vec3d{0, 0, 16};

      const auto update = lightPropertyUpdate(light);
      CHECK(valueOf(update.set, "noshadows") == "1");
      CHECK(valueOf(update.set, "nospecular") == "1");
      CHECK(valueOf(update.set, "texture") == "lights/round");
      CHECK(valueOf(update.set, "style") == "3");
      CHECK(valueOf(update.set, "light_center") == "0 0 16");

      // style -1 is removed, and a sun never has a style
      light.style = -1;
      CHECK(contains(lightPropertyUpdate(light).remove, "style"));
      light.style = 3;
      light.type = LightType::Sun;
      CHECK(contains(lightPropertyUpdate(light).remove, "style"));
    }

    SECTION("The math block")
    {
      const auto update = lightPropertyUpdate(parse(mathLightProperties));
      CHECK(
        keys(update.set)
        == withMathKeys({"origin", "noshadows", "nospecular", "_color", "light_radius"}));
      auto removed = pointRemoved;
      removed.push_back("math_style");
      CHECK(sorted(update.remove) == sorted(removed));

      CHECK(valueOf(update.set, "math_enabled") == "1");
      CHECK(valueOf(update.set, "math_type") == "point");
      CHECK(valueOf(update.set, "math_falloff") == "linear");
      CHECK(valueOf(update.set, "math_pattern") == "ice");
      CHECK(valueOf(update.set, "math_falloff_exp") == "3");
      CHECK(valueOf(update.set, "math_inner_radius") == "0.1");
      CHECK(valueOf(update.set, "math_shape_exp") == "4");
      CHECK(valueOf(update.set, "math_penumbra") == "0.2");
      CHECK(valueOf(update.set, "math_pattern_strength") == "0.5");
      CHECK(valueOf(update.set, "math_pattern_scale") == "2");
      CHECK(valueOf(update.set, "math_pattern_phase") == "0.25");
      CHECK(valueOf(update.set, "math_energy_clamp") == "0");

      // math_type is written from the light's type
      auto spot = parse(defaultSpotProperties);
      spot.math = parse(mathLightProperties).math;
      CHECK(valueOf(lightPropertyUpdate(spot).set, "math_type") == "spot");
      auto sun = parse(sunProperties);
      sun.math = parse(mathLightProperties).math;
      CHECK(valueOf(lightPropertyUpdate(sun).set, "math_type") == "sun");
    }

    SECTION("math_style is written for a style and removed without one")
    {
      auto light = parse(mathLightProperties);
      light.math.style = 4;
      CHECK(valueOf(lightPropertyUpdate(light).set, "math_style") == "4");
      CHECK_FALSE(contains(lightPropertyUpdate(light).remove, "math_style"));

      // unlike style, a sun has one too: it changes the colour, not the radius
      light.type = LightType::Sun;
      CHECK(valueOf(lightPropertyUpdate(light).set, "math_style") == "4");

      light.math.style = -1;
      CHECK(valueOf(lightPropertyUpdate(light).set, "math_style") == "<missing>");
      CHECK(contains(lightPropertyUpdate(light).remove, "math_style"));

      // a disabled block keeps it
      light.math.style = 4;
      light.math.enabled = false;
      CHECK(valueOf(lightPropertyUpdate(light).set, "math_style") == "<missing>");
      CHECK_FALSE(contains(lightPropertyUpdate(light).remove, "math_style"));
    }

    SECTION("A disabled math block is math_enabled 0 and deletes nothing")
    {
      auto light = parse(mathLightProperties);
      light.math.enabled = false;

      const auto update = lightPropertyUpdate(light);
      CHECK(valueOf(update.set, "math_enabled") == "0");
      for (const auto& key : mathLightPropertyKeys())
      {
        CAPTURE(key);
        CHECK_FALSE(contains(update.remove, key));
        if (key != "math_enabled")
        {
          CHECK(valueOf(update.set, key) == "<missing>");
        }
      }
    }

    SECTION("A light_origin light gets neither origin nor light_origin")
    {
      const auto update = lightPropertyUpdate(
        parse(with(defaultPointProperties, {{"light_origin", "4 5 6"}})));
      CHECK_FALSE(contains(keys(update.set), "origin"));
      CHECK_FALSE(contains(keys(update.set), "light_origin"));
      CHECK_FALSE(contains(update.remove, "origin"));
      CHECK_FALSE(contains(update.remove, "light_origin"));
    }

    SECTION("The rotation properties are never touched")
    {
      for (const auto& properties :
           {defaultPointProperties,
            defaultSpotProperties,
            sunProperties,
            mathLightProperties})
      {
        const auto update = lightPropertyUpdate(parse(with(
          properties,
          {{"angles", "0 90 0"},
           {"angle", "45"},
           {"rotation", "1 0 0 0 1 0 0 0 1"},
           {"light_rotation", "1 0 0 0 1 0 0 0 1"},
           {"light_angles", "0 0 0"}})));
        for (const auto& key :
             {"angles", "angle", "rotation", "light_rotation", "light_angles"})
        {
          CAPTURE(key);
          CHECK_FALSE(contains(keys(update.set), key));
          CHECK_FALSE(contains(update.remove, key));
        }
      }
    }

    SECTION("Written properties are read back as the same light")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             defaultPointProperties,
             defaultSpotProperties,
             with(
               defaultSpotProperties,
               {{"light_start", "0 0 -32"}, {"light_end", "0 0 -200"}}),
             sunProperties,
             mathLightProperties,
             with(mathLightProperties, {{"math_style", "4"}}),
           })
      {
        CAPTURE(properties);
        const auto light = parse(properties);
        const auto written = applyLightPropertyUpdate(
          Entity{{{"classname", "light"}}}, lightPropertyUpdate(light));
        CHECK(parseLightProperties(written) == light);
      }
    }
  }

  SECTION("lightKeyDiff")
  {
    SECTION("A radius edit writes only light_radius")
    {
      const auto entity = Entity{with(
        defaultPointProperties, {{"light_radius", "300 300 300"}, {"_color", "1 1 1"}})};
      auto edited = parseLightProperties(entity);
      edited.radius = vm::vec3d{400, 400, 400};

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"light_radius", "400 400 400"}});
      CHECK(diff.remove.empty());
    }

    SECTION("A radius edit keeps light, which light_radius leaves unread")
    {
      const auto entity = Entity{defaultPointProperties};
      auto edited = parseLightProperties(entity);
      edited.radius = vm::vec3d{400, 400, 400};

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"light_radius", "400 400 400"}});
      CHECK(diff.remove.empty());
    }

    SECTION("A colour edit keeps the radius that light gives")
    {
      const auto entity = Entity{with(defaultPointProperties, {{"light", "500"}})};
      auto edited = parseLightProperties(entity);
      edited.color = vm::vec3d{2, 1, 0.5};

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"_color", "2 1 0.5"}});
      CHECK(diff.remove.empty());
      CHECK(
        parseLightProperties(applyLightPropertyUpdate(entity, diff)).radius
        == vm::vec3d{500, 500, 500});
    }

    SECTION("Two lights keep their origins")
    {
      const auto lightAt0 = Entity{with(defaultPointProperties, {{"origin", "0 0 0"}})};
      const auto lightAt100 =
        Entity{with(defaultPointProperties, {{"origin", "100.000 0 0"}})};

      for (const auto& entity : {lightAt0, lightAt100})
      {
        auto edited = parseLightProperties(entity);
        edited.radius = vm::vec3d{400, 400, 400};

        const auto diff = lightKeyDiff(entity, edited);
        CHECK(keys(diff.set) == std::vector<std::string>{"light_radius"});
        CHECK_FALSE(contains(keys(diff.set), "origin"));
        CHECK(diff.remove.empty());
      }
    }

    SECTION("A no-op edit changes nothing")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             defaultPointProperties,
             defaultSpotProperties,
             sunProperties,
             mathLightProperties,
             with(defaultPointProperties, {{"origin", "100.000 0 0"}}),
             with(mathLightProperties, {{"math_type", "sun"}, {"style", "-1"}}),
             with(mathLightProperties, {{"math_style", "4"}}),
             with(mathLightProperties, {{"math_enabled", "0"}}),
             with(defaultPointProperties, {{"math_enabled", "0"}, {"math_type", "spot"}}),
             sunWithUnreadTargetProperties,
           })
      {
        CAPTURE(properties);
        const auto entity = Entity{properties};
        CHECK(
          lightKeyDiff(entity, parseLightProperties(entity)) == LightPropertyUpdate{});
      }
    }

    SECTION("A switch from point to spot")
    {
      const auto entity =
        Entity{with(defaultPointProperties, {{"light_radius", "300 300 300"}})};

      const auto spot = defaultLight(LightType::Spot, parseLightProperties(entity));
      const auto diff = lightKeyDiff(entity, spot);
      CHECK(keys(diff.set) == sorted({"light_target", "light_up", "light_right"}));
      CHECK(diff.remove == std::vector<std::string>{"light_radius"});

      // with explicit start and end points, the five vectors are written
      auto explicitSpot = spot;
      explicitSpot.explicitStartEnd = true;
      explicitSpot.start = vm::vec3d{0, 0, -32};
      CHECK(
        keys(lightKeyDiff(entity, explicitSpot).set)
        == sorted(
          {"light_target", "light_up", "light_right", "light_start", "light_end"}));

      // light_radius is removed only if present
      const auto lightEntity = Entity{defaultPointProperties};
      const auto lightDiff = lightKeyDiff(
        lightEntity, defaultLight(LightType::Spot, parse(defaultPointProperties)));
      CHECK(keys(lightDiff.set) == sorted({"light_target", "light_up", "light_right"}));
      CHECK(lightDiff.remove.empty());
    }

    SECTION("A switch from point to spot reads back as the edited light")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             defaultPointProperties,
             with(defaultPointProperties, {{"light_radius", "300 300 300"}}),
           })
      {
        CAPTURE(properties);
        const auto entity = Entity{properties};

        const auto spot = defaultLight(LightType::Spot, parseLightProperties(entity));
        CHECK(
          parseLightProperties(
            applyLightPropertyUpdate(entity, lightKeyDiff(entity, spot)))
          == spot);

        auto explicitSpot = spot;
        explicitSpot.explicitStartEnd = true;
        explicitSpot.start = vm::vec3d{0, 0, -32};
        CHECK(
          parseLightProperties(
            applyLightPropertyUpdate(entity, lightKeyDiff(entity, explicitSpot)))
          == explicitSpot);
      }
    }

    SECTION("A colour edit of a sun with unread target keys writes only _color")
    {
      const auto entity = Entity{sunWithUnreadTargetProperties};
      auto edited = parseLightProperties(entity);
      REQUIRE(edited.type == LightType::Sun);
      edited.color = vm::vec3d{2, 2, 2};

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(
        diff
        == LightPropertyUpdate{
          std::vector<EntityProperty>{{"_color", "2 2 2"}}, std::vector<std::string>{}});
      CHECK(parseLightProperties(applyLightPropertyUpdate(entity, diff)) == edited);
    }

    SECTION("A sun with unread target keys switched to a spot")
    {
      const auto entity = Entity{sunWithUnreadTargetProperties};
      const auto spot = defaultLight(LightType::Spot, parseLightProperties(entity));

      // light_target and light_up are already the default spot's
      const auto diff = lightKeyDiff(entity, spot);
      CHECK(diff.set == std::vector<EntityProperty>{{"light_right", "-128 0 0"}});
      CHECK(diff.remove == std::vector<std::string>{"parallel"});
      CHECK(parseLightProperties(applyLightPropertyUpdate(entity, diff)) == spot);
    }

    SECTION("A switch from spot to point removes what would be read again")
    {
      const auto entity =
        Entity{with(defaultSpotProperties, {{"light_center", "0 0 10"}})};

      const auto diff = lightKeyDiff(
        entity, defaultLight(LightType::Point, parseLightProperties(entity)));
      CHECK(diff.set == std::vector<EntityProperty>{{"light_radius", "300 300 300"}});
      CHECK(
        sorted(diff.remove)
        == sorted({"light_target", "light_up", "light_right", "light_center"}));

      const auto changed = applyLightPropertyUpdate(entity, diff);
      CHECK(
        parseLightProperties(changed)
        == defaultLight(LightType::Point, parseLightProperties(entity)));
    }

    SECTION("A missing light_end is written when the target changes")
    {
      const auto entity =
        Entity{with(defaultSpotProperties, {{"light_start", "0 0 -32"}})};
      auto edited = parseLightProperties(entity);
      edited.target = vm::vec3d{0, 0, -300};

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(
        diff.set
        == std::vector<EntityProperty>{
          {"light_target", "0 0 -300"}, {"light_end", "0 0 -256"}});
      CHECK(diff.remove.empty());
    }

    SECTION("Disabling the math block writes math_enabled 0 and keeps the other keys")
    {
      const auto entity = Entity{mathLightProperties};
      auto edited = parseLightProperties(entity);
      edited.math.enabled = false;

      // D51: the other eleven math keys stay
      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"math_enabled", "0"}});
      CHECK(diff.remove.empty());

      const auto disabled = applyLightPropertyUpdate(entity, diff);
      CHECK(disabled.properties() == with(mathLightProperties, {{"math_enabled", "0"}}));
      CHECK(parseLightProperties(disabled) == edited);
    }

    SECTION("Disabling writes nothing on a light that never had a math block")
    {
      // before and after both say math_enabled 0
      const auto entity = Entity{defaultPointProperties};
      auto edited = parseLightProperties(entity);
      REQUIRE_FALSE(edited.math.enabled);
      edited.math.enabled = false;

      CHECK(lightKeyDiff(entity, edited) == LightPropertyUpdate{});
    }

    SECTION("Enabling a disabled math block restores its kept values")
    {
      const auto entity = Entity{with(mathLightProperties, {{"math_enabled", "0"}})};
      auto edited = parseLightProperties(entity);
      edited.math.enabled = true;

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"math_enabled", "1"}});
      CHECK(diff.remove.empty());
      CHECK(
        parseLightProperties(applyLightPropertyUpdate(entity, diff)).math
        == parse(mathLightProperties).math);
    }

    SECTION("Enabling a block that the engine wrote keeps its numbers as they are")
    {
      // ML_WriteToDict writes the floats with "%f"; a value with more than four decimals
      // would be rounded if it were written again
      const auto engineProperties = std::vector<EntityProperty>{
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_radius", "300 300 300"},
        {"math_enabled", "0"},
        {"math_type", "point"},
        {"math_falloff", "linear"},
        {"math_pattern", "none"},
        {"math_falloff_exp", "2.000000"},
        {"math_inner_radius", "0.100000"},
        {"math_shape_exp", "2.000000"},
        {"math_penumbra", "0.150000"},
        {"math_pattern_strength", "0.000000"},
        {"math_pattern_scale", "1.000000"},
        {"math_pattern_phase", "0.333333"},
        {"math_energy_clamp", "1"},
      };
      const auto entity = Entity{engineProperties};
      auto edited = parseLightProperties(entity);
      edited.math.enabled = true;

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"math_enabled", "1"}});
      CHECK(diff.remove.empty());
      CHECK(
        applyLightPropertyUpdate(entity, diff).properties()
        == with(engineProperties, {{"math_enabled", "1"}}));

      // an edit of a kept value while enabling writes that value too
      edited.math.innerRadius = 0.25;
      CHECK(
        lightKeyDiff(entity, edited).set
        == std::vector<EntityProperty>{
          {"math_enabled", "1"}, {"math_inner_radius", "0.25"}});
    }

    SECTION("Enabling a block with an unknown math_style writes no math_style")
    {
      // the engine reads it as none, and so does the editor: it stays unread
      const auto entity =
        Entity{with(mathLightProperties, {{"math_enabled", "0"}, {"math_style", "12"}})};
      auto edited = parseLightProperties(entity);
      REQUIRE(edited.math.style == -1);
      edited.math.enabled = true;

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(diff.set == std::vector<EntityProperty>{{"math_enabled", "1"}});
      CHECK(diff.remove.empty());
    }

    SECTION("A light with math_enabled alone gets the whole seed")
    {
      const auto entity = Entity{with(defaultPointProperties, {{"math_enabled", "0"}})};
      REQUIRE_FALSE(hasMathLightBlock(entity));
      auto edited = parseLightProperties(entity);
      edited.math = seededMathLightProperties();

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(keys(diff.set) == withMathKeys({}));
      CHECK(diff.remove.empty());
    }

    SECTION("The seed writes twelve math keys")
    {
      // D29: the seed has no style, so math_style is neither written nor removed
      const auto entity = Entity{defaultPointProperties};
      auto edited = parseLightProperties(entity);
      edited.math = seededMathLightProperties();

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(keys(diff.set) == withMathKeys({}));
      CHECK(diff.set.size() == 12);
      CHECK(diff.remove.empty());
    }

    SECTION("A math style is written and removed")
    {
      const auto entity = Entity{mathLightProperties};
      auto edited = parseLightProperties(entity);
      REQUIRE(edited.math.style == -1);
      REQUIRE_FALSE(entity.hasProperty("math_style"));

      edited.math.style = 4;
      const auto styleDiff = lightKeyDiff(entity, edited);
      CHECK(styleDiff.set == std::vector<EntityProperty>{{"math_style", "4"}});
      CHECK(styleDiff.remove.empty());

      const auto styled = applyLightPropertyUpdate(entity, styleDiff);
      edited.math.style = -1;
      const auto noStyleDiff = lightKeyDiff(styled, edited);
      CHECK(noStyleDiff.set.empty());
      CHECK(noStyleDiff.remove == std::vector<std::string>{"math_style"});
      CHECK(
        applyLightPropertyUpdate(styled, noStyleDiff).properties()
        == mathLightProperties);
    }
  }

  SECTION("applyLightPropertyUpdate")
  {
    const auto entity = Entity{defaultPointProperties};
    const auto changed = applyLightPropertyUpdate(
      entity,
      LightPropertyUpdate{
        {{"origin", "1 2 3"}, {"light_radius", "1 2 3"}}, {"light", "texture"}});

    CHECK(
      changed.properties()
      == std::vector<EntityProperty>{
        {"classname", "light"},
        {"origin", "1 2 3"},
        {"light_radius", "1 2 3"},
      });
    // the given entity is copied
    CHECK(entity.properties() == defaultPointProperties);
  }

  SECTION("defaultLight")
  {
    const auto keep = LightProperties{
      .type = LightType::Spot,
      .hasLightOrigin = true,
      .origin = {1, 2, 3},
      .radius = {1, 2, 3},
      .equalRadius = false,
      .hasCenter = true,
      .center = {0, 0, 16},
      .target = {0, 0, -100},
      .right = {-50, 0, 0},
      .up = {0, -50, 0},
      .explicitStartEnd = true,
      .start = {0, 0, -32},
      .end = {0, 0, -100},
      .color = {2, 1, 0.5},
      .texture = "lights/round",
      .style = 3,
      .castShadows = false,
      .skipSpecular = true,
      .math = parse(mathLightProperties).math,
      .badTargetInfo = true,
    };

    // the colour, the origin and the math block are kept, everything else is reset
    CHECK(
      defaultLight(LightType::Point, keep)
      == LightProperties{
        .type = LightType::Point,
        .hasLightOrigin = true,
        .origin = {1, 2, 3},
        .radius = {300, 300, 300},
        .equalRadius = true,
        .color = {2, 1, 0.5},
        .math = keep.math,
      });

    CHECK(
      defaultLight(LightType::Spot, keep)
      == LightProperties{
        .type = LightType::Spot,
        .hasLightOrigin = true,
        .origin = {1, 2, 3},
        .target = {0, 0, -256},
        .right = {-128, 0, 0},
        .up = {0, -128, 0},
        .explicitStartEnd = false,
        .start = {0, 0, 0},
        .end = {0, 0, -256},
        .color = {2, 1, 0.5},
        .math = keep.math,
      });

    // the sun's center is set but not written
    const auto sun = defaultLight(LightType::Sun, keep);
    CHECK(
      sun
      == LightProperties{
        .type = LightType::Sun,
        .hasLightOrigin = true,
        .origin = {1, 2, 3},
        .radius = {2048, 2048, 1024},
        .equalRadius = false,
        .hasCenter = false,
        .center = {4, 4, 32},
        .color = {2, 1, 0.5},
        .math = keep.math,
      });
    CHECK(contains(lightPropertyUpdate(sun).remove, "light_center"));
  }

  SECTION("seededMathLightProperties")
  {
    // the in-game editor's auto-seed (LightEditor.cpp), field by field
    const auto math = seededMathLightProperties();
    CHECK(math.enabled);
    CHECK(math.falloff == LightFalloff::Linear);
    CHECK(math.falloffExp == 2.0);
    CHECK(math.innerRadius == 0.1);
    CHECK(math.shapeExp == 2.0);
    CHECK(math.penumbra == 0.15);
    CHECK(math.pattern == LightPattern::None);
    CHECK(math.patternStrength == 0.0);
    CHECK(math.patternScale == 1.0);
    CHECK(math.patternPhase == 0.0);
    CHECK(math.energyClamp);
  }

  SECTION("hasMathLightBlock")
  {
    CHECK_FALSE(hasMathLightBlock(Entity{defaultPointProperties}));
    CHECK_FALSE(
      hasMathLightBlock(Entity{with(defaultPointProperties, {{"math_enabled", "0"}})}));
    CHECK_FALSE(
      hasMathLightBlock(Entity{with(defaultPointProperties, {{"math_enabled", "1"}})}));
    CHECK(hasMathLightBlock(Entity{mathLightProperties}));
    CHECK(hasMathLightBlock(Entity{with(mathLightProperties, {{"math_enabled", "0"}})}));
    for (const auto& key : mathLightPropertyKeys())
    {
      if (key != "math_enabled")
      {
        CAPTURE(key);
        CHECK(hasMathLightBlock(Entity{with(defaultPointProperties, {{key, "1"}})}));
      }
    }
  }
}

} // namespace tb::mdl
