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
#include "mdl/LightVolume.h"
#include "mdl/PropertyDefinition.h"

#include "vm/approx.h"
#include "vm/mat.h"
#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
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

// a point light that carries keys no light reads: every writer leaves them as they are
const auto unknownKeyProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light_radius", "300 300 300"},
  {"lamp_label", "kitchen"},
  {"lamp_level", "2"},
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

// the eighteen keys that an enabled emitter block writes
std::vector<std::string> withEmitterKeys(std::vector<std::string> strs)
{
  for (const auto& key : emitterPropertyKeys())
  {
    strs.push_back(key);
  }
  return sorted(std::move(strs));
}

const auto omniEmitterProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light_radius", "288 288 288"},
  {"emitter", "1"},
  {"emit_shape", "omni"},
};

// every key of the block, none at its default
const auto spotEmitterProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 256"},
  {"light_radius", "512 512 512"},
  {"emitter", "1"},
  {"emit_shape", "spot"},
  {"emit_auto_range", "0"},
  {"emit_falloff", "smooth"},
  {"emit_size", "128"},
  {"emit_start", "16"},
  {"emit_clamp", "0"},
  {"emit_cone", "60 40"},
  {"emit_penumbra", "0.3"},
  {"emit_square", "4"},
  {"emit_pattern", "ice"},
  {"emit_pattern_strength", "0.5"},
  {"emit_pattern_scale", "2"},
  {"emit_pattern_phase", "0.25"},
  {"emit_effect", "candle_a"},
  {"emit_effect_speed", "2"},
  {"emit_effect_amount", "0.5"},
  {"emit_effect_phase", "0.125"},
};

const auto sunEmitterProperties = std::vector<EntityProperty>{
  {"classname", "light"},
  {"origin", "0 0 0"},
  {"light_radius", "2048 2048 1024"},
  {"light_center", "0 0 1"},
  {"parallel", "1"},
  {"emitter", "1"},
  {"emit_shape", "sun"},
};

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

  // the names of emit_falloff and emit_pattern; the engine compares them without regard
  // to case

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

    SECTION("A number that is not finite is not read")
    {
      // a vector stops there, with the numbers before it read and the others 0, and a
      // single number is 0
      const auto light = parse(with(
        defaultPointProperties,
        {{"light_radius", "nan 300 300"},
         {"light_center", "0 inf 0"},
         {"_color", "nan 1 1"}}));
      CHECK(light.radius == vm::vec3d{0, 0, 0});
      CHECK(light.hasCenter);
      CHECK(light.center == vm::vec3d{0, 0, 0});
      CHECK(light.color == vm::vec3d{0, 0, 0});

      CHECK(
        parse(with(defaultSpotProperties, {{"light_target", "0 0 -infinity"}})).target
        == vm::vec3d{0, 0, 0});
      CHECK(
        parse(with(defaultPointProperties, {{"light_center", "16 inf 16"}})).center
        == vm::vec3d{16, 0, 0});
      CHECK(
        parse(with(defaultPointProperties, {{"_color", "1 1 -inf"}})).color
        == vm::vec3d{1, 1, 0});
      CHECK(
        parse(with(defaultPointProperties, {{"light", "inf"}})).radius
        == vm::vec3d{0, 0, 0});
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
      const auto update = lightPropertyUpdate(parse(defaultPointProperties));
      CHECK(
        keys(update.set)
        == sorted({"origin", "noshadows", "nospecular", "_color", "light_radius"}));
      CHECK(sorted(update.remove) == sorted(pointRemoved));

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
           "light_right"}));
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
           "light_end"}));
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
          {"origin", "noshadows", "nospecular", "_color", "light_radius", "parallel"}));
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
            unknownKeyProperties})
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
             unknownKeyProperties,
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
             unknownKeyProperties,
             with(defaultPointProperties, {{"origin", "100.000 0 0"}}),
             with(unknownKeyProperties, {{"style", "-1"}}),
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
      .badTargetInfo = true,
    };

    // the colour and the origin are kept, everything else is reset
    CHECK(
      defaultLight(LightType::Point, keep)
      == LightProperties{
        .type = LightType::Point,
        .hasLightOrigin = true,
        .origin = {1, 2, 3},
        .radius = {300, 300, 300},
        .equalRadius = true,
        .color = {2, 1, 0.5},
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
      });
    CHECK(contains(lightPropertyUpdate(sun).remove, "light_center"));
  }

  SECTION("lightUsesEmitter")
  {
    CHECK_FALSE(lightUsesEmitter(parse(defaultPointProperties)));
    CHECK_FALSE(lightUsesEmitter(parse(unknownKeyProperties)));
    CHECK(lightUsesEmitter(parse(omniEmitterProperties)));
    CHECK(lightUsesEmitter(parse(with(unknownKeyProperties, {{"emitter", "1"}}))));
    CHECK_FALSE(lightUsesEmitter(parse(with(omniEmitterProperties, {{"emitter", "0"}}))));
  }

  SECTION("The emitter key of a texture light")
  {
    // a light that never was an emitter is written with no emitter key
    for (const auto& properties : std::vector<std::vector<EntityProperty>>{
           defaultPointProperties,
           defaultSpotProperties,
           sunProperties,
           unknownKeyProperties,
         })
    {
      CAPTURE(properties);
      const auto light = parse(properties);
      CHECK_FALSE(light.emitter.hasBlock);
      const auto update = lightPropertyUpdate(light);
      CHECK(valueOf(update.set, "emitter") == "<missing>");
      CHECK_FALSE(contains(update.remove, "emitter"));
    }

    // a light that carries emit keys is written with emitter 0 and keeps them
    const auto carrying =
      with(defaultPointProperties, {{"emit_size", "120"}, {"emit_cone", "45"}});
    const auto light = parse(carrying);
    CHECK(light.emitter.hasBlock);
    CHECK_FALSE(light.emitter.enabled);
    const auto update = lightPropertyUpdate(light);
    CHECK(valueOf(update.set, "emitter") == "0");
    for (const auto& key : emitterPropertyKeys())
    {
      CAPTURE(key);
      CHECK_FALSE(contains(update.remove, key));
      if (key != "emitter")
      {
        CHECK(valueOf(update.set, key) == "<missing>");
      }
    }
    const auto written = applyLightPropertyUpdate(Entity{carrying}, update);
    CHECK(valueOf(written.properties(), "emit_size") == "120");
    CHECK(valueOf(written.properties(), "emit_cone") == "45");

    // a no-op edit of either writes nothing
    CHECK(
      lightKeyDiff(Entity{defaultPointProperties}, parse(defaultPointProperties))
      == LightPropertyUpdate{});
    CHECK(lightKeyDiff(Entity{carrying}, light) == LightPropertyUpdate{});
  }

  SECTION("emitterPropertyKeys")
  {
    CHECK(
      emitterPropertyKeys()
      == std::vector<std::string>{
        "emitter",
        "emit_shape",
        "emit_auto_range",
        "emit_falloff",
        "emit_size",
        "emit_start",
        "emit_clamp",
        "emit_cone",
        "emit_penumbra",
        "emit_square",
        "emit_pattern",
        "emit_pattern_strength",
        "emit_pattern_scale",
        "emit_pattern_phase",
        "emit_effect",
        "emit_effect_speed",
        "emit_effect_amount",
        "emit_effect_phase",
      });
  }

  SECTION("emitterShapeName")
  {
    CHECK(emitterShapeName(LightType::Point) == "omni");
    CHECK(emitterShapeName(LightType::Spot) == "spot");
    CHECK(emitterShapeName(LightType::Sun) == "sun");
  }

  SECTION("parseEmitterShapeName")
  {
    CHECK(parseEmitterShapeName("OMNI") == LightType::Point);
    CHECK(parseEmitterShapeName("spot") == LightType::Spot);
    CHECK(parseEmitterShapeName("Sun") == LightType::Sun);
    // the names of a texture light's types are not emitter names
    CHECK(parseEmitterShapeName("point") == std::nullopt);
    CHECK(parseEmitterShapeName("parallel") == std::nullopt);
  }

  SECTION("emitterEffectName")
  {
    CHECK(emitterEffectName(0) == "none");
    CHECK(emitterEffectName(1) == "flicker_a");
    CHECK(emitterEffectName(3) == "candle_a");
    CHECK(emitterEffectName(10) == "fluorescent");
    CHECK(emitterEffectName(11) == "slow_pulse");
    CHECK(emitterEffectName(12) == "strobe");
    CHECK(emitterEffectName(13) == "pulse");
    CHECK(emitterEffectName(14) == "flicker");
    CHECK(emitterEffectName(EmitterEffectCount).empty());
    CHECK(emitterEffectName(-1).empty());
  }

  SECTION("parseEmitterEffectName")
  {
    CHECK(parseEmitterEffectName("Candle_B") == 7);
    CHECK(parseEmitterEffectName("none") == 0);
    CHECK(parseEmitterEffectName("FLICKER") == 14);
    CHECK(parseEmitterEffectName("bogus") == std::nullopt);
  }

  SECTION("emitterEffectDisplayName")
  {
    CHECK(emitterEffectDisplayName(0) == "None");
    CHECK(emitterEffectDisplayName(1) == "Flicker A");
    CHECK(emitterEffectDisplayName(10) == "Fluorescent Flicker");
    CHECK(emitterEffectDisplayName(11) == "Slow Pulse (no black)");
    CHECK(emitterEffectDisplayName(12) == "Strobe");
    CHECK(emitterEffectDisplayName(13) == "Pulse");
    CHECK(emitterEffectDisplayName(14) == "Flicker");
    CHECK(emitterEffectDisplayName(15).empty());
  }

  SECTION("parseLightProperties of an emitter")
  {
    SECTION("An omni")
    {
      CHECK(
        parse(omniEmitterProperties)
        == LightProperties{
          .radius = {288, 288, 288},
          .emitter = EmitterProperties{.enabled = true, .hasBlock = true},
        });
    }

    SECTION("Every key")
    {
      const auto light = parse(spotEmitterProperties);
      CHECK(light.type == LightType::Spot);
      CHECK(light.origin == vm::vec3d{0, 0, 256});
      CHECK(light.radius == vm::vec3d{512, 512, 512});
      CHECK(
        light.emitter
        == EmitterProperties{
          .size = 128.0,
          .start = 16.0,
          .coneH = 60.0,
          .coneV = 40.0,
          .penumbra = 0.3,
          .squareness = 4.0,
          .patternStrength = 0.5,
          .patternScale = 2.0,
          .patternPhase = 0.25,
          .effectSpeed = 2.0,
          .effectAmount = 0.5,
          .effectPhase = 0.125,
          .falloff = LightFalloff::Smooth,
          .pattern = LightPattern::Ice,
          .effect = 3,
          .enabled = true,
          .hasBlock = true,
          .autoRange = false,
          .energyClamp = false,
        });
    }

    SECTION("A sun")
    {
      const auto light = parse(sunEmitterProperties);
      CHECK(light.type == LightType::Sun);
      CHECK(light.radius == vm::vec3d{2048, 2048, 1024});
      CHECK_FALSE(light.equalRadius);
      CHECK(light.hasCenter);
      CHECK(light.center == vm::vec3d{0, 0, 1});
    }

    SECTION("emitter 1 wins over the target keys")
    {
      const auto omni = parse(with(defaultSpotProperties, {{"emitter", "1"}}));
      CHECK(omni.type == LightType::Point);
      CHECK(omni.target == vm::vec3d{0, 0, 0});
      CHECK(omni.right == vm::vec3d{0, 0, 0});
      CHECK(omni.up == vm::vec3d{0, 0, 0});
      CHECK(omni.radius == vm::vec3d{300, 300, 300});

      const auto spot =
        parse(with(defaultSpotProperties, {{"emitter", "1"}, {"emit_shape", "spot"}}));
      CHECK(spot.type == LightType::Spot);
      CHECK(spot.target == vm::vec3d{0, 0, 0});
      CHECK(spot.radius == vm::vec3d{300, 300, 300});

      // partial target keys are no bad target info
      const auto partial = parse(
        with(defaultPointProperties, {{"light_target", "0 0 -256"}, {"emitter", "1"}}));
      CHECK_FALSE(partial.badTargetInfo);
      CHECK(partial.type == LightType::Point);
    }

    SECTION("emit_shape, or parallel without a known one")
    {
      CHECK(parse(with(sunProperties, {{"emitter", "1"}})).type == LightType::Sun);
      CHECK(
        parse(with(sunProperties, {{"emitter", "1"}, {"emit_shape", "OMNI"}})).type
        == LightType::Point);
      CHECK(
        parse(with(sunProperties, {{"emitter", "1"}, {"emit_shape", "bogus"}})).type
        == LightType::Sun);
      CHECK(
        parse(with(defaultPointProperties, {{"emitter", "1"}, {"emit_shape", "bogus"}}))
          .type
        == LightType::Point);
    }

    SECTION("A spot's range is its largest radius, and it has no centre")
    {
      const auto light = parse(with(
        omniEmitterProperties,
        {{"emit_shape", "spot"},
         {"light_radius", "100 300 200"},
         {"light_center", "0 0 16"}}));
      CHECK(light.radius == vm::vec3d{300, 300, 300});
      CHECK(light.equalRadius);
      CHECK_FALSE(light.hasCenter);
      CHECK(light.center == vm::vec3d{0, 0, 0});
    }

    SECTION("light is not read")
    {
      CHECK(
        parse(with(defaultPointProperties, {{"light", "500"}, {"emitter", "1"}})).radius
        == vm::vec3d{300, 300, 300});
    }

    SECTION("emit_cone has one or two numbers")
    {
      const auto cone = [](const std::string& value) {
        const auto emitter =
          parse(with(omniEmitterProperties, {{"emit_cone", value}})).emitter;
        return std::pair{emitter.coneH, emitter.coneV};
      };
      CHECK(cone("60") == std::pair{60.0, 60.0});
      CHECK(cone("60 40") == std::pair{60.0, 40.0});
      CHECK(cone("45 bogus") == std::pair{45.0, 45.0});
      CHECK(cone("bogus") == std::pair{60.0, 60.0});
      // clamped to 1 to 120
      CHECK(cone("200") == std::pair{120.0, 120.0});
      CHECK(cone("0.5 300") == std::pair{1.0, 120.0});
    }

    SECTION("The emitter values are clamped like EM_ParseFromDict clamps them")
    {
      const auto high = parse(with(
                                omniEmitterProperties,
                                {{"emit_penumbra", "2"},
                                 {"emit_square", "64"},
                                 {"emit_pattern_strength", "5"},
                                 {"emit_effect_speed", "500"},
                                 {"emit_effect_amount", "2"},
                                 {"emit_pattern_scale", "-3"},
                                 {"emit_effect_phase", "7.5"}}))
                          .emitter;
      CHECK(high.penumbra == 1.0);
      CHECK(high.squareness == 32.0);
      CHECK(high.patternStrength == 1.0);
      CHECK(high.effectSpeed == 100.0);
      CHECK(high.effectAmount == 1.0);
      // the scale and the phases are not clamped
      CHECK(high.patternScale == -3.0);
      CHECK(high.effectPhase == 7.5);

      const auto low = parse(with(
        omniEmitterProperties,
        {{"emit_size", "0"},
         {"emit_start", "-5"},
         {"emit_penumbra", "-1"},
         {"emit_square", "1"},
         {"emit_effect_speed", "-1"},
         {"emit_effect_amount", "-1"},
         {"light_radius", "0 -5 0.5"}}));
      CHECK(low.emitter.size == 1.0);
      CHECK(low.emitter.start == 0.0);
      CHECK(low.emitter.penumbra == 0.0);
      CHECK(low.emitter.squareness == 1.5);
      CHECK(low.emitter.effectSpeed == 0.0);
      CHECK(low.emitter.effectAmount == 0.0);
      // each radius component at least 1
      CHECK(low.radius == vm::vec3d{1, 1, 1});
    }

    SECTION("Unknown names are the defaults")
    {
      const auto emitter = parse(with(
                                   omniEmitterProperties,
                                   {{"emit_falloff", "bogus"},
                                    {"emit_pattern", "noise"},
                                    {"emit_effect", "disco"}}))
                             .emitter;
      CHECK(emitter.falloff == LightFalloff::InvSquareClamped);
      CHECK(emitter.pattern == LightPattern::None);
      CHECK(emitter.effect == 0);
    }

    SECTION("emitter 1 is read whatever other keys the light carries")
    {
      const auto light = parse(with(unknownKeyProperties, {{"emitter", "1"}}));
      CHECK(light.emitter.enabled);
      CHECK(light.type == LightType::Point);
    }

    SECTION("emitter 0 disables the block and keeps its values")
    {
      const auto light = parse(with(spotEmitterProperties, {{"emitter", "0"}}));
      CHECK_FALSE(light.emitter.enabled);
      CHECK(light.emitter.coneV == 40.0);
      CHECK(light.emitter.effect == 3);
      // a texture light, which light_radius makes a point light
      CHECK(light.type == LightType::Point);
      CHECK(light.radius == vm::vec3d{512, 512, 512});
    }
  }

  SECTION("lightPropertyUpdate of an emitter")
  {
    SECTION("An omni writes its whole block, defaults included")
    {
      const auto update = lightPropertyUpdate(parse(omniEmitterProperties));
      CHECK(
        keys(update.set)
        == withEmitterKeys(
          {"origin", "noshadows", "nospecular", "_color", "light_radius"}));
      CHECK(
        sorted(update.remove)
        == sorted(
          {"light",
           "texture",
           "light_center",
           "parallel",
           "light_target",
           "light_up",
           "light_right",
           "light_start",
           "light_end",
           "style"}));

      CHECK(valueOf(update.set, "light_radius") == "288 288 288");
      CHECK(valueOf(update.set, "emitter") == "1");
      CHECK(valueOf(update.set, "emit_shape") == "omni");
      CHECK(valueOf(update.set, "emit_auto_range") == "1");
      CHECK(valueOf(update.set, "emit_falloff") == "invsquare_clamped");
      CHECK(valueOf(update.set, "emit_size") == "96");
      CHECK(valueOf(update.set, "emit_start") == "0");
      CHECK(valueOf(update.set, "emit_clamp") == "1");
      CHECK(valueOf(update.set, "emit_cone") == "60");
      CHECK(valueOf(update.set, "emit_penumbra") == "0.2");
      CHECK(valueOf(update.set, "emit_square") == "2");
      CHECK(valueOf(update.set, "emit_pattern") == "none");
      CHECK(valueOf(update.set, "emit_pattern_strength") == "0");
      CHECK(valueOf(update.set, "emit_pattern_scale") == "1");
      CHECK(valueOf(update.set, "emit_pattern_phase") == "0");
      CHECK(valueOf(update.set, "emit_effect") == "none");
      CHECK(valueOf(update.set, "emit_effect_speed") == "1");
      CHECK(valueOf(update.set, "emit_effect_amount") == "1");
      CHECK(valueOf(update.set, "emit_effect_phase") == "0");
    }

    SECTION("A spot writes R R R and none of the target keys")
    {
      auto light = LightProperties{};
      light.type = LightType::Spot;
      light.radius = vm::vec3d{100, 300, 200};
      light.equalRadius = false;
      light.hasCenter = true;
      light.center = vm::vec3d{0, 0, 16};
      light.target = vm::vec3d{0, 0, -256};
      light.emitter = seededEmitterProperties();

      const auto update = lightPropertyUpdate(light);
      CHECK(valueOf(update.set, "light_radius") == "300 300 300");
      CHECK(valueOf(update.set, "emit_shape") == "spot");
      for (const auto* key :
           {"light_target",
            "light_up",
            "light_right",
            "light_start",
            "light_end",
            "light_center",
            "parallel"})
      {
        CAPTURE(key);
        CHECK(valueOf(update.set, key) == "<missing>");
        CHECK(contains(update.remove, key));
      }
    }

    SECTION("A sun writes parallel")
    {
      const auto update = lightPropertyUpdate(parse(sunEmitterProperties));
      CHECK(valueOf(update.set, "parallel") == "1");
      CHECK(valueOf(update.set, "light_radius") == "2048 2048 1024");
      CHECK(valueOf(update.set, "light_center") == "0 0 1");
      CHECK(valueOf(update.set, "emit_shape") == "sun");
    }

    SECTION("emit_cone is one number for a round cone")
    {
      auto light = parse(omniEmitterProperties);
      CHECK(valueOf(lightPropertyUpdate(light).set, "emit_cone") == "60");
      light.emitter.coneV = 40.0;
      CHECK(valueOf(lightPropertyUpdate(light).set, "emit_cone") == "60 40");
      // equal once written with four decimals
      light.emitter.coneH = 33.33333;
      light.emitter.coneV = 33.33334;
      CHECK(valueOf(lightPropertyUpdate(light).set, "emit_cone") == "33.3333");
    }

    SECTION("An unknown key is neither written nor removed")
    {
      const auto update =
        lightPropertyUpdate(parse(with(unknownKeyProperties, {{"emitter", "1"}})));
      for (const auto& key : {"lamp_label", "lamp_level"})
      {
        CAPTURE(key);
        CHECK(valueOf(update.set, key) == "<missing>");
        CHECK_FALSE(contains(update.remove, key));
      }
    }

    SECTION("A disabled block is emitter 0 and deletes nothing")
    {
      const auto update =
        lightPropertyUpdate(parse(with(spotEmitterProperties, {{"emitter", "0"}})));
      CHECK(valueOf(update.set, "emitter") == "0");
      for (const auto& key : emitterPropertyKeys())
      {
        CAPTURE(key);
        CHECK_FALSE(contains(update.remove, key));
        if (key != "emitter")
        {
          CHECK(valueOf(update.set, key) == "<missing>");
        }
      }
    }

    SECTION("Written properties are read back as the same light")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             omniEmitterProperties,
             spotEmitterProperties,
             sunEmitterProperties,
             with(omniEmitterProperties, {{"emit_shape", "spot"}}),
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

  SECTION("lightKeyDiff of an emitter")
  {
    SECTION("A no-op edit changes nothing")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             omniEmitterProperties,
             spotEmitterProperties,
             sunEmitterProperties,
             with(spotEmitterProperties, {{"emitter", "0"}}),
             with(unknownKeyProperties, {{"emitter", "1"}}),
             with(defaultSpotProperties, {{"emitter", "1"}}),
           })
      {
        CAPTURE(properties);
        const auto entity = Entity{properties};
        CHECK(
          lightKeyDiff(entity, parseLightProperties(entity)) == LightPropertyUpdate{});
      }
    }

    SECTION("Switching an emitter to a texture light writes emitter 0 alone")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             omniEmitterProperties,
             with(unknownKeyProperties, {{"emitter", "1"}}),
           })
      {
        CAPTURE(properties);
        const auto entity = Entity{properties};
        auto edited = parseLightProperties(entity);
        edited.emitter.enabled = false;

        const auto diff = lightKeyDiff(entity, edited);
        CHECK(
          diff
          == LightPropertyUpdate{
            std::vector<EntityProperty>{{"emitter", "0"}}, std::vector<std::string>{}});

        // every emit_* key stays
        CHECK(
          applyLightPropertyUpdate(entity, diff).properties()
          == with(properties, {{"emitter", "0"}}));
      }
    }

    SECTION("Switching a point light to an emitter writes the whole block")
    {
      const auto entity =
        Entity{with(defaultPointProperties, {{"light_radius", "288 288 288"}})};
      auto edited = parseLightProperties(entity);
      edited.emitter = seededEmitterProperties();

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(keys(diff.set) == withEmitterKeys({}));
      // light stays, unread as before
      CHECK(diff.remove.empty());
      CHECK(parseLightProperties(applyLightPropertyUpdate(entity, diff)) == edited);
    }

    SECTION("A light switched to an emitter keeps its unknown keys")
    {
      const auto entity = Entity{unknownKeyProperties};
      auto edited = parseLightProperties(entity);
      edited.emitter = seededEmitterProperties();

      const auto diff = lightKeyDiff(entity, edited);
      for (const auto& key : {"lamp_label", "lamp_level"})
      {
        CAPTURE(key);
        CHECK_FALSE(contains(keys(diff.set), key));
        CHECK_FALSE(contains(diff.remove, key));
      }

      const auto changed = applyLightPropertyUpdate(entity, diff);
      CHECK(parseLightProperties(changed).emitter.enabled);
      CHECK(valueOf(changed.properties(), "lamp_label") == "kitchen");
      CHECK(valueOf(changed.properties(), "lamp_level") == "2");
    }

    SECTION("A partial block is written whole on the first edit")
    {
      const auto entity = Entity{omniEmitterProperties};
      auto edited = parseLightProperties(entity);
      edited.radius = vm::vec3d{300, 300, 300};

      auto expected = withEmitterKeys({"light_radius"});
      std::erase(expected, "emitter");
      std::erase(expected, "emit_shape");

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(keys(diff.set) == expected);
      CHECK(valueOf(diff.set, "light_radius") == "300 300 300");
      CHECK(diff.remove.empty());
    }

    SECTION("Enabling a kept block keeps its numbers as they are")
    {
      // EM_WriteToDict may write "%f"; a kept value is written again only if edited
      const auto properties = std::vector<EntityProperty>{
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_radius", "300 300 300"},
        {"emitter", "0"},
        {"emit_size", "120.000000"},
        {"emit_cone", "45 30"},
      };
      const auto entity = Entity{properties};
      auto edited = parseLightProperties(entity);
      edited.emitter.enabled = true;

      const auto diff = lightKeyDiff(entity, edited);
      CHECK(valueOf(diff.set, "emitter") == "1");
      CHECK(valueOf(diff.set, "emit_size") == "<missing>");
      CHECK(valueOf(diff.set, "emit_cone") == "<missing>");
      // the fifteen keys the block lacks, emit_shape among them, and emitter
      CHECK(diff.set.size() == 16);
      CHECK(valueOf(diff.set, "emit_shape") == "omni");

      const auto changed = applyLightPropertyUpdate(entity, diff);
      CHECK(valueOf(changed.properties(), "emit_size") == "120.000000");
      CHECK(parseLightProperties(changed).emitter.size == 120.0);
      CHECK(parseLightProperties(changed).emitter.coneV == 30.0);
    }
  }

  SECTION("defaultLight of an emitter")
  {
    auto keep = LightProperties{};
    keep.origin = vm::vec3d{1, 2, 3};
    keep.emitter = seededEmitterProperties();
    keep.emitter.coneH = 30.0;
    keep.emitter.penumbra = 0.5;

    // auto range: 96 * 3 at intensity 1
    const auto omni = defaultLight(LightType::Point, keep);
    CHECK(omni.type == LightType::Point);
    CHECK(omni.origin == vm::vec3d{1, 2, 3});
    CHECK(omni.radius == vm::vec3d{288, 288, 288});
    CHECK(omni.emitter == keep.emitter);

    // a spot gets a new spot's cone and no target vectors
    const auto spot = defaultLight(LightType::Spot, keep);
    CHECK(spot.type == LightType::Spot);
    CHECK(spot.radius == vm::vec3d{288, 288, 288});
    CHECK(spot.target == vm::vec3d{0, 0, 0});
    CHECK(spot.right == vm::vec3d{0, 0, 0});
    CHECK(spot.up == vm::vec3d{0, 0, 0});
    CHECK(spot.end == vm::vec3d{0, 0, 0});
    CHECK(spot.emitter.coneH == 60.0);
    CHECK(spot.emitter.coneV == 60.0);
    CHECK(spot.emitter.penumbra == 0.2);
    CHECK(spot.emitter.squareness == 2.0);
    CHECK(spot.emitter.size == 96.0);

    // today's sun, without auto range
    const auto sun = defaultLight(LightType::Sun, keep);
    CHECK(sun.radius == vm::vec3d{2048, 2048, 1024});
    CHECK(sun.emitter.enabled);

    // the reach follows the kept size; a curve without one keeps 300
    keep.emitter.size = 50.0;
    CHECK(defaultLight(LightType::Point, keep).radius == vm::vec3d{150, 150, 150});
    keep.emitter.falloff = LightFalloff::Linear;
    CHECK(defaultLight(LightType::Point, keep).radius == vm::vec3d{300, 300, 300});
  }

  SECTION("seededEmitterProperties")
  {
    CHECK(
      seededEmitterProperties() == EmitterProperties{.enabled = true, .hasBlock = true});
  }

  SECTION("seededEmitterLight")
  {
    const auto omni = seededEmitterLight(LightType::Point, LightProperties{});
    CHECK(omni.radius == vm::vec3d{288, 288, 288});
    CHECK(omni.emitter.enabled);

    const auto spot = seededEmitterLight(LightType::Spot, LightProperties{});
    CHECK(spot.type == LightType::Spot);
    CHECK(spot.radius == vm::vec3d{288, 288, 288});

    const auto update = lightPropertyUpdate(omni);
    CHECK(valueOf(update.set, "light_radius") == "288 288 288");
    CHECK(valueOf(update.set, "emit_size") == "96");
    CHECK(valueOf(update.set, "emit_start") == "0");
    CHECK(valueOf(update.set, "emit_auto_range") == "1");
    CHECK(valueOf(update.set, "emit_effect") == "none");

    // the new block's effect follows the light's style, which stays (F-T-05)
    auto styled = LightProperties{};
    styled.style = 3;
    const auto candle = seededEmitterLight(LightType::Point, styled);
    CHECK(candle.emitter.effect == 3);
    CHECK(candle.style == 3);
    CHECK(valueOf(lightPropertyUpdate(candle).set, "emit_effect") == "candle_a");
    CHECK(valueOf(lightPropertyUpdate(candle).set, "style") == "3");
  }

  SECTION("emitterShapeSwitch")
  {
    auto omni = LightProperties{};
    omni.emitter = seededEmitterProperties();
    omni.radius = vm::vec3d{288, 144, 216};
    omni.equalRadius = false;

    // with auto range in force the range is the auto range (F-T-17)
    REQUIRE(emitterAutoRangeInForce(omni));
    CHECK(
      emitterShapeSwitch(LightType::Spot, omni) == defaultLight(LightType::Spot, omni));
    CHECK(emitterShapeSwitch(LightType::Spot, omni).radius == vm::vec3d{288, 288, 288});

    // the flag on but the range out of step, as a hand-set range is: kept (G0 section 9)
    omni.radius = vm::vec3d{400, 200, 300};
    REQUIRE_FALSE(emitterAutoRangeInForce(omni));
    CHECK(emitterShapeSwitch(LightType::Spot, omni).radius == vm::vec3d{400, 400, 400});

    // a linear or smooth curve with the flag on keeps its range too, no longer 300
    // (DP-T4-03)
    for (const auto falloff : {LightFalloff::Linear, LightFalloff::Smooth})
    {
      auto linear = omni;
      linear.emitter.falloff = falloff;
      const auto linearSpot = emitterShapeSwitch(LightType::Spot, linear);
      CHECK(linearSpot.radius == vm::vec3d{400, 400, 400});
      CHECK(linearSpot.emitter.autoRange);
      CHECK(
        emitterShapeSwitch(LightType::Point, linearSpot).radius
        == vm::vec3d{400, 400, 400});
    }

    // with it off the range is kept: the omni's largest radius, and back
    omni.emitter.autoRange = false;
    const auto spot = emitterShapeSwitch(LightType::Spot, omni);
    CHECK(spot.type == LightType::Spot);
    CHECK(spot.radius == vm::vec3d{400, 400, 400});
    CHECK(spot.emitter.size == omni.emitter.size);
    const auto back = emitterShapeSwitch(LightType::Point, spot);
    CHECK(back.type == LightType::Point);
    CHECK(back.radius == vm::vec3d{400, 400, 400});
    CHECK(back.equalRadius);

    // the start stays below the kept range, and the range is at least 24
    auto small = omni;
    small.radius = vm::vec3d{10, 10, 10};
    small.emitter.start = 50.0;
    const auto smallSpot = emitterShapeSwitch(LightType::Spot, small);
    CHECK(smallSpot.radius == vm::vec3d{24, 24, 24});
    CHECK(smallSpot.emitter.start == 23.0);

    // from a sun, which has no range: the auto range when the flag is on and the curve
    // has a reach, else 300 (G0 section 9)
    auto sun = omni;
    sun.type = LightType::Sun;
    sun.radius = vm::vec3d{2048, 2048, 1024};
    sun.emitter.autoRange = true;
    CHECK(emitterShapeSwitch(LightType::Point, sun).radius.x() == Catch::Approx(288.0));
    CHECK(emitterShapeSwitch(LightType::Spot, sun).radius.x() == Catch::Approx(288.0));
    sun.emitter.autoRange = false;
    CHECK(emitterShapeSwitch(LightType::Point, sun).radius == vm::vec3d{300, 300, 300});
    sun.emitter.autoRange = true;
    sun.emitter.falloff = LightFalloff::Linear;
    CHECK(emitterShapeSwitch(LightType::Point, sun).radius == vm::vec3d{300, 300, 300});

    // to a sun, and a texture light: defaultLight
    CHECK(emitterShapeSwitch(LightType::Sun, omni) == defaultLight(LightType::Sun, omni));
    const auto texture = LightProperties{};
    CHECK(
      emitterShapeSwitch(LightType::Spot, texture)
      == defaultLight(LightType::Spot, texture));

    // an emitter keeps noshadows and nospecular, to every shape (DP-E3-05); a texture
    // light resets them, as today
    auto flagged = omni;
    flagged.castShadows = false;
    flagged.skipSpecular = true;
    for (const auto type : {LightType::Spot, LightType::Sun})
    {
      CAPTURE(type);
      const auto switched = emitterShapeSwitch(type, flagged);
      CHECK_FALSE(switched.castShadows);
      CHECK(switched.skipSpecular);
      const auto written = lightPropertyUpdate(switched);
      CHECK(valueOf(written.set, "noshadows") == "1");
      CHECK(valueOf(written.set, "nospecular") == "1");
    }
    auto flaggedTexture = flagged;
    flaggedTexture.emitter.enabled = false;
    const auto textureSwitched = emitterShapeSwitch(LightType::Spot, flaggedTexture);
    CHECK(textureSwitched.castShadows);
    CHECK_FALSE(textureSwitched.skipSpecular);
  }

  SECTION("emitterEffectForStyle")
  {
    for (int style = 1; style < LightStyleCount; ++style)
    {
      CAPTURE(style);
      CHECK(emitterEffectForStyle(style) == style);
      // the same predef_lightstyles entry under both names
      CHECK(emitterEffectDisplayName(style) == lightStyleName(style));
    }
    CHECK(emitterEffectForStyle(0) == 0);
    CHECK(emitterEffectForStyle(-1) == 0);
    CHECK(emitterEffectForStyle(LightStyleCount) == 0);
    CHECK(emitterEffectForStyle(99) == 0);
  }

  SECTION("applyEmitterOmniRangeFloor")
  {
    auto omni = LightProperties{};
    omni.emitter = seededEmitterProperties();
    omni.radius = vm::vec3d{10, 5, 20};
    omni.equalRadius = false;
    CHECK(
      applyEmitterOmniRangeFloor(omni).radius == vm::approx{vm::vec3d{12, 6, 24}, 1e-9});

    // at the floor or above, unchanged
    omni.radius = vm::vec3d{24, 5, 5};
    CHECK(applyEmitterOmniRangeFloor(omni) == omni);

    // a sun, a spot and a texture light are not held to it
    auto small = omni;
    small.radius = vm::vec3d{10, 10, 10};
    for (const auto type : {LightType::Sun, LightType::Spot})
    {
      auto other = small;
      other.type = type;
      CHECK(applyEmitterOmniRangeFloor(other) == other);
    }
    auto texture = small;
    texture.emitter.enabled = false;
    CHECK(applyEmitterOmniRangeFloor(texture) == texture);
  }

  SECTION("The emitter keys are read only with a key that starts with emit")
  {
    // without one the block has its defaults (F-T-01)
    const auto texture = parseLightProperties(Entity{defaultPointProperties});
    CHECK(texture.emitter == EmitterProperties{});

    // a key that only looks like an emitter key gives the defaults too
    const auto typo =
      parseLightProperties(Entity{with(defaultPointProperties, {{"emit_typo", "1"}})});
    CHECK(typo.emitter == EmitterProperties{});

    // any emitter key is read, with the block
    const auto sized =
      parseLightProperties(Entity{with(defaultPointProperties, {{"emit_size", "50"}})});
    CHECK(sized.emitter.size == 50.0);
    CHECK(sized.emitter.hasBlock);
    CHECK_FALSE(sized.emitter.enabled);
  }

  SECTION("hasEmitPrefixedKey")
  {
    // one pass decides whether parseLightProperties reads the emitter block (RV-C-TP-02):
    // a texture light that never was an emitter does not
    const auto blocks = [](const std::vector<EntityProperty>& properties) {
      return hasEmitPrefixedKey(Entity{properties});
    };
    CHECK_FALSE(blocks(defaultPointProperties));
    CHECK_FALSE(blocks(unknownKeyProperties));
    CHECK(blocks(omniEmitterProperties));
    CHECK(blocks(with(defaultPointProperties, {{"emit_typo", "1"}})));
    // keys are compared as they are spelled, as every lookup of the parse compares them
    CHECK_FALSE(blocks(with(defaultPointProperties, {{"Emit_Size", "1"}, {"emi", "1"}})));
  }

  SECTION("hasEmitterBlock")
  {
    CHECK_FALSE(hasEmitterBlock(Entity{defaultPointProperties}));
    CHECK_FALSE(
      hasEmitterBlock(Entity{with(defaultPointProperties, {{"emitter", "1"}})}));
    CHECK(hasEmitterBlock(Entity{omniEmitterProperties}));
    for (const auto& key : emitterPropertyKeys())
    {
      if (key != "emitter")
      {
        CAPTURE(key);
        CHECK(hasEmitterBlock(Entity{with(defaultPointProperties, {{key, "1"}})}));
      }
    }
  }

  SECTION("emitterRange")
  {
    CHECK(emitterRange(vm::vec3d{100, 300, 200}) == 300.0);
  }

  SECTION("lightIntensity")
  {
    auto light = LightProperties{};
    light.color = vm::vec3d{0.5, 2, 1};
    CHECK(lightIntensity(light) == 2.0);
    CHECK(lightIntensity(vm::vec3d{0.5, 2, 1}) == 2.0);
    // not clamped: the panel's display clamps at 0
    CHECK(lightIntensity(vm::vec3d{-1, -2, -0.5}) == -0.5);
  }

  SECTION("clampEmitterStart")
  {
    // 0 <= start <= range - 1 (G0 section 5)
    CHECK(clampEmitterStart(50.0, 288.0) == 50.0);
    CHECK(clampEmitterStart(300.0, 288.0) == 287.0);
    CHECK(clampEmitterStart(-5.0, 288.0) == 0.0);
    // a range below 1 leaves no room: the start is 0
    CHECK(clampEmitterStart(5.0, 0.5) == 0.0);
  }

  SECTION("emitterCurveHasReach")
  {
    for (const auto falloff :
         {LightFalloff::InvSquare,
          LightFalloff::Linear,
          LightFalloff::Smooth,
          LightFalloff::Exponential,
          LightFalloff::InvSquareClamped})
    {
      CAPTURE(falloff);
      // the same test as a reach factor above 0, at any intensity
      for (const auto intensity : {0.0, 0.05, 1.0, 8.0, -1.0, std::nan("")})
      {
        CAPTURE(intensity);
        CHECK(
          emitterCurveHasReach(falloff)
          == (emitterReachFactor(falloff, intensity) > 0.0));
      }
    }
    CHECK_FALSE(emitterCurveHasReach(LightFalloff::Linear));
    CHECK_FALSE(emitterCurveHasReach(LightFalloff::Smooth));
  }

  SECTION("emitterTanHalf")
  {
    CHECK(emitterTanHalf(90.0) == Catch::Approx(1.0));
    CHECK(emitterTanHalf(60.0) == Catch::Approx(std::tan(vm::to_radians(30.0))));
    // clamped to 1 to 120 first
    CHECK(emitterTanHalf(200.0) == Catch::Approx(std::sqrt(3.0)));
    CHECK(emitterTanHalf(0.0) == Catch::Approx(std::tan(vm::to_radians(0.5))));
  }

  SECTION("emitterConeFromTanHalf")
  {
    CHECK(emitterConeFromTanHalf(1.0) == Catch::Approx(90.0));
    CHECK(emitterConeFromTanHalf(100.0) == 120.0);
    CHECK(emitterConeFromTanHalf(0.0) == 1.0);
  }

  SECTION("emitterSinHalf")
  {
    CHECK(emitterSinHalf(60.0) == Catch::Approx(0.5));
    CHECK(emitterSinHalf(90.0) == Catch::Approx(std::sqrt(0.5)));
    // clamped to 1 to 120 first
    CHECK(emitterSinHalf(200.0) == Catch::Approx(std::sqrt(3.0) / 2.0));
    CHECK(emitterSinHalf(0.0) == Catch::Approx(std::sin(vm::to_radians(0.5))));
  }

  SECTION("emitterConeFromSinHalf")
  {
    CHECK(emitterConeFromSinHalf(0.5) == Catch::Approx(60.0));
    CHECK(emitterConeFromSinHalf(std::sqrt(0.5)) == Catch::Approx(90.0));
    // clamped to 1 to 120, a sine above 1 counting as 1
    CHECK(emitterConeFromSinHalf(0.9) == 120.0);
    CHECK(emitterConeFromSinHalf(5.0) == 120.0);
    CHECK(emitterConeFromSinHalf(0.0) == 1.0);
    CHECK(emitterConeFromSinHalf(-1.0) == 1.0);
    // the inverse of emitterSinHalf
    for (const auto cone : {1.0, 30.0, 60.0, 119.0, 120.0})
    {
      CAPTURE(cone);
      CHECK(emitterConeFromSinHalf(emitterSinHalf(cone)) == Catch::Approx(cone));
    }
  }

  SECTION("emitterRangeInStep")
  {
    auto emitter = seededEmitterProperties();
    // auto range 288 at intensity 1: within max(1, 5 %), 14.4
    CHECK(emitterRangeInStep(emitter, 1.0, 288.0));
    CHECK(emitterRangeInStep(emitter, 1.0, 288.0 + 14.3));
    CHECK(emitterRangeInStep(emitter, 1.0, 288.0 - 14.3));
    CHECK_FALSE(emitterRangeInStep(emitter, 1.0, 288.0 + 14.5));
    CHECK_FALSE(emitterRangeInStep(emitter, 1.0, 288.0 - 14.5));
    CHECK_FALSE(emitterRangeInStep(emitter, 1.0, 500.0));

    // the smallest auto range, 24, accepts 1.2 either way
    emitter.size = 1.0;
    CHECK(emitterRangeInStep(emitter, 1.0, 25.0));
    CHECK_FALSE(emitterRangeInStep(emitter, 1.0, 25.5));

    // never for a curve without a reach factor
    emitter = seededEmitterProperties();
    emitter.falloff = LightFalloff::Linear;
    CHECK_FALSE(emitterRangeInStep(emitter, 1.0, 288.0));
  }

  SECTION("emitterAutoRangeInForce")
  {
    auto omni = LightProperties{};
    omni.radius = vm::vec3d{288, 288, 288};
    omni.emitter = seededEmitterProperties();
    CHECK(emitterAutoRangeInForce(omni));

    auto spot = omni;
    spot.type = LightType::Spot;
    CHECK(emitterAutoRangeInForce(spot));

    auto sun = omni;
    sun.type = LightType::Sun;
    CHECK_FALSE(emitterAutoRangeInForce(sun));

    auto off = omni;
    off.emitter.autoRange = false;
    CHECK_FALSE(emitterAutoRangeInForce(off));

    auto linear = omni;
    linear.emitter.falloff = LightFalloff::Linear;
    CHECK_FALSE(emitterAutoRangeInForce(linear));

    auto handSet = omni;
    handSet.radius = vm::vec3d{500, 500, 500};
    CHECK_FALSE(emitterAutoRangeInForce(handSet));

    auto texture = omni;
    texture.emitter.enabled = false;
    CHECK_FALSE(emitterAutoRangeInForce(texture));
  }

  SECTION("A hand-set range wins over auto range")
  {
    // no emit_auto_range key, a range of 500 where the auto range is 288
    const auto handSetProperties = std::vector<EntityProperty>{
      {"classname", "light"},
      {"origin", "0 0 0"},
      {"light_radius", "500 500 500"},
      {"emitter", "1"},
      {"emit_shape", "omni"},
      {"emit_size", "96"},
    };

    SECTION("It reads with auto range off")
    {
      const auto light = parseLightProperties(Entity{handSetProperties});
      CHECK_FALSE(light.emitter.autoRange);
      CHECK(light.radius == vm::vec3d{500, 500, 500});
      CHECK_FALSE(emitterAutoRangeInForce(light));

      // the same with emit_auto_range "1", and for a spot
      CHECK_FALSE(
        parseLightProperties(Entity{with(handSetProperties, {{"emit_auto_range", "1"}})})
          .emitter.autoRange);
      CHECK_FALSE(
        parseLightProperties(Entity{with(handSetProperties, {{"emit_shape", "spot"}})})
          .emitter.autoRange);

      // in step it reads with auto range on
      CHECK(parseLightProperties(
              Entity{with(handSetProperties, {{"light_radius", "288 288 288"}})})
              .emitter.autoRange);
      CHECK(parseLightProperties(
              Entity{with(handSetProperties, {{"light_radius", "300 300 300"}})})
              .emitter.autoRange);
    }

    SECTION("A sun and a curve without a reach keep the stored flag")
    {
      CHECK(parseLightProperties(
              Entity{with(
                handSetProperties, {{"emit_shape", "sun"}, {"emit_auto_range", "1"}})})
              .emitter.autoRange);
      CHECK(
        parseLightProperties(
          Entity{with(
            handSetProperties, {{"emit_falloff", "linear"}, {"emit_auto_range", "1"}})})
          .emitter.autoRange);
      CHECK_FALSE(
        parseLightProperties(
          Entity{with(
            handSetProperties, {{"emit_falloff", "smooth"}, {"emit_auto_range", "0"}})})
          .emitter.autoRange);
      // a texture light's kept block is not tested
      CHECK(parseLightProperties(Entity{with(handSetProperties, {{"emitter", "0"}})})
              .emitter.autoRange);
    }

    SECTION("An intensity edit leaves the range and writes emit_auto_range 0")
    {
      for (const auto& properties : std::vector<std::vector<EntityProperty>>{
             handSetProperties,
             with(handSetProperties, {{"emit_auto_range", "1"}}),
           })
      {
        CAPTURE(properties);
        const auto entity = Entity{properties};
        auto edited = parseLightProperties(entity);
        edited.color = vm::vec3d{2, 2, 2};
        edited = applyEmitterAutoRange(edited, EmitterEdit::Reach);
        CHECK(edited.radius == vm::vec3d{500, 500, 500});

        const auto diff = lightKeyDiff(entity, edited);
        CHECK(valueOf(diff.set, "emit_auto_range") == "0");
        CHECK(valueOf(diff.set, "light_radius") == "<missing>");

        const auto changed = parseLightProperties(applyLightPropertyUpdate(entity, diff));
        CHECK_FALSE(changed.emitter.autoRange);
        CHECK(changed.radius == vm::vec3d{500, 500, 500});
        CHECK(changed.color == vm::vec3d{2, 2, 2});
      }
    }

    SECTION("A no-op edit writes nothing, emit_auto_range included")
    {
      const auto entity = Entity{with(handSetProperties, {{"emit_auto_range", "1"}})};
      CHECK(lightKeyDiff(entity, parseLightProperties(entity)) == LightPropertyUpdate{});
    }

    SECTION("Ticking auto range sets the range to the auto range and keeps the size")
    {
      const auto entity = Entity{with(handSetProperties, {{"emit_auto_range", "0"}})};
      auto edited = parseLightProperties(entity);
      edited.emitter.autoRange = true;
      edited = applyEmitterAutoRange(edited, EmitterEdit::Reach);
      CHECK(edited.radius == vm::vec3d{288, 288, 288});
      CHECK(edited.emitter.size == 96.0);

      const auto changed = parseLightProperties(
        applyLightPropertyUpdate(entity, lightKeyDiff(entity, edited)));
      CHECK(changed.emitter.autoRange);
      CHECK(changed.radius == vm::vec3d{288, 288, 288});
    }

    SECTION("A dim light written under auto range reads back in force")
    {
      // intensity 0.23, size 96: _color is written with the precision the editor writes
      auto light = LightProperties{};
      light.emitter = seededEmitterProperties();
      light.color = vm::vec3d{0.23, 0.1771, 0.0437};
      light = applyEmitterAutoRange(light, EmitterEdit::Reach);
      REQUIRE(light.radius.x() == Catch::Approx(96.0 * std::sqrt(1.3)));

      const auto entity = applyLightPropertyUpdate(
        Entity{{{"classname", "light"}}}, lightPropertyUpdate(light));
      const auto readBack = parseLightProperties(entity);
      CHECK(readBack.emitter.autoRange);
      CHECK(emitterAutoRangeInForce(readBack));

      // also with _color at two decimals, which moves the auto range by about 2 %
      const auto rounded = parseLightProperties(applyLightPropertyUpdate(
        entity,
        LightPropertyUpdate{
          std::vector<EntityProperty>{{"_color", "0.225 0.17 0.04"}},
          std::vector<std::string>{}}));
      CHECK(rounded.emitter.autoRange);
    }

    SECTION("A falloff edit to a curve with a reach applies the flag as it stands")
    {
      const auto linearProperties =
        with(handSetProperties, {{"emit_falloff", "linear"}, {"emit_auto_range", "1"}});

      // G0 section 5 as corrected at 01:00 (DP-T5-01): no test of the range at this
      // edit; the flag is on, so the range becomes the new curve's auto range
      const auto flagOn = parseLightProperties(Entity{linearProperties});
      REQUIRE(flagOn.emitter.autoRange);
      const auto followed = applyEmitterFalloff(flagOn, LightFalloff::InvSquareClamped);
      CHECK(followed.emitter.falloff == LightFalloff::InvSquareClamped);
      CHECK(followed.emitter.autoRange);
      CHECK(followed.radius == vm::approx{vm::vec3d{288, 288, 288}, 1e-9});

      // with the flag off the range is kept
      const auto flagOff =
        parseLightProperties(Entity{with(linearProperties, {{"emit_auto_range", "0"}})});
      REQUIRE_FALSE(flagOff.emitter.autoRange);
      const auto kept = applyEmitterFalloff(flagOff, LightFalloff::InvSquareClamped);
      CHECK_FALSE(kept.emitter.autoRange);
      CHECK(kept.radius == vm::vec3d{500, 500, 500});

      // near the new auto range it is the same: the range follows the curve
      const auto inStep = parseLightProperties(
        Entity{with(linearProperties, {{"light_radius", "290 290 290"}})});
      const auto on = applyEmitterFalloff(inStep, LightFalloff::InvSquareClamped);
      CHECK(on.emitter.autoRange);
      CHECK(on.radius == vm::approx{vm::vec3d{288, 288, 288}, 1e-9});

      // from a curve with a reach to another, no test: auto range in force follows it
      auto inForce = on;
      inForce = applyEmitterFalloff(inForce, LightFalloff::InvSquare);
      CHECK(inForce.emitter.autoRange);
      CHECK(inForce.radius.x() == Catch::Approx(96.0 * std::sqrt(10.0)));

      // to linear the flag stays as it is and the range is kept
      const auto linear = applyEmitterFalloff(inForce, LightFalloff::Linear);
      CHECK(linear.emitter.autoRange);
      CHECK(linear.radius == inForce.radius);

      // a texture light gets the falloff and nothing else
      auto texture = flagOn;
      texture.emitter.enabled = false;
      const auto textureFalloff = applyEmitterFalloff(texture, LightFalloff::Smooth);
      auto expected = texture;
      expected.emitter.falloff = LightFalloff::Smooth;
      CHECK(textureFalloff == expected);
    }

    // each step is written and read back, as the panel does
    const auto editAndRead = [](const Entity& entity, const auto& edit) {
      auto edited = parseLightProperties(entity);
      edit(edited);
      return applyLightPropertyUpdate(entity, lightKeyDiff(entity, edited));
    };
    const auto setFalloff = [&](const Entity& entity, const LightFalloff falloff) {
      return editAndRead(entity, [&](LightProperties& light) {
        light = applyEmitterFalloff(light, falloff);
      });
    };

    SECTION("Every curve in turn and back ends where it started")
    {
      // in force at 288 under the clamped inverse square
      auto entity = Entity{with(handSetProperties, {{"light_radius", "288 288 288"}})};
      REQUIRE(emitterAutoRangeInForce(parseLightProperties(entity)));

      // 96 ln(10) = 221.05
      entity = setFalloff(entity, LightFalloff::Exponential);
      CHECK(valueOf(entity.properties(), "light_radius") == "221.05 221.05 221.05");
      entity = setFalloff(entity, LightFalloff::Linear);
      entity = setFalloff(entity, LightFalloff::Smooth);
      CHECK(parseLightProperties(entity).emitter.autoRange);
      CHECK(valueOf(entity.properties(), "light_radius") == "221.05 221.05 221.05");
      CHECK(valueOf(entity.properties(), "emit_auto_range") == "1");
      entity = setFalloff(entity, LightFalloff::InvSquareClamped);

      const auto light = parseLightProperties(entity);
      CHECK(light.emitter.falloff == LightFalloff::InvSquareClamped);
      CHECK(light.radius == vm::vec3d{288, 288, 288});
      CHECK(light.emitter.autoRange);
      CHECK(emitterAutoRangeInForce(light));
      CHECK(valueOf(entity.properties(), "emit_auto_range") == "1");
    }

    SECTION("A range typed under a linear curve is kept by a curve with a reach")
    {
      auto entity = Entity{with(
        handSetProperties,
        {{"light_radius", "288 288 288"},
         {"emit_falloff", "linear"},
         {"emit_auto_range", "1"}})};
      REQUIRE(parseLightProperties(entity).emitter.autoRange);

      // the range edit turns auto range off: the mapper has taken the range in hand
      entity = editAndRead(entity, [](LightProperties& light) {
        light.radius = vm::vec3d{400, 400, 400};
        light = applyEmitterAutoRange(light, EmitterEdit::Range);
      });
      CHECK(valueOf(entity.properties(), "emit_auto_range") == "0");
      CHECK(valueOf(entity.properties(), "light_radius") == "400 400 400");

      entity = setFalloff(entity, LightFalloff::InvSquareClamped);
      const auto light = parseLightProperties(entity);
      CHECK(light.emitter.falloff == LightFalloff::InvSquareClamped);
      CHECK(light.radius == vm::vec3d{400, 400, 400});
      CHECK_FALSE(light.emitter.autoRange);
      CHECK(valueOf(entity.properties(), "emit_auto_range") == "0");
    }

    SECTION("emitterRangeSetByHand")
    {
      // the light as its keys say, before the read's own test turns the flag off
      const auto asStored = [](const std::vector<EntityProperty>& properties) {
        auto light = parseLightProperties(Entity{properties});
        light.emitter.autoRange = valueOf(properties, "emit_auto_range") != "0";
        return light;
      };

      CHECK(emitterRangeSetByHand(asStored(handSetProperties)));
      CHECK_FALSE(emitterRangeSetByHand(
        asStored(with(handSetProperties, {{"light_radius", "288 288 288"}}))));
      // the flag off, a linear curve, a sun and a texture light are not tested
      for (const auto& changes : std::vector<std::vector<EntityProperty>>{
             {{"emit_auto_range", "0"}},
             {{"emit_falloff", "linear"}},
             {{"emit_shape", "sun"}},
             {{"emitter", "0"}},
           })
      {
        CAPTURE(changes);
        CHECK_FALSE(emitterRangeSetByHand(asStored(with(handSetProperties, changes))));
      }
    }
  }

  SECTION("emitterReachFactor")
  {
    CHECK(emitterReachFactor(LightFalloff::InvSquareClamped, 1.0) == Catch::Approx(3.0));
    CHECK(
      emitterReachFactor(LightFalloff::InvSquare, 1.0) == Catch::Approx(std::sqrt(10.0)));
    CHECK(
      emitterReachFactor(LightFalloff::Exponential, 1.0)
      == Catch::Approx(std::log(10.0)));
    CHECK(emitterReachFactor(LightFalloff::Linear, 1.0) == 0.0);
    CHECK(emitterReachFactor(LightFalloff::Smooth, 1.0) == 0.0);
    // at least 1 for a dim light
    CHECK(emitterReachFactor(LightFalloff::InvSquareClamped, 0.05) == 1.0);
    CHECK(emitterReachFactor(LightFalloff::Exponential, 0.05) == 1.0);
  }

  SECTION("emitterAutoRange")
  {
    auto emitter = seededEmitterProperties();
    REQUIRE(emitter.falloff == LightFalloff::InvSquareClamped);
    REQUIRE(emitter.size == 96.0);
    REQUIRE(emitter.start == 0.0);

    CHECK(emitterAutoRange(emitter, 1.0) == Catch::Approx(288.0));
    CHECK(emitterAutoRange(emitter, 2.0) == Catch::Approx(96.0 * std::sqrt(19.0)));

    // at least 24
    emitter.size = 1.0;
    CHECK(emitterAutoRange(emitter, 1.0) == 24.0);

    emitter.size = 96.0;
    emitter.start = 32.0;
    CHECK(emitterAutoRange(emitter, 1.0) == Catch::Approx(320.0));

    // no auto range for linear
    emitter.falloff = LightFalloff::Linear;
    CHECK(emitterAutoRange(emitter, 1.0) == 0.0);
  }

  SECTION("emitterAutoSize")
  {
    auto emitter = seededEmitterProperties();
    CHECK(emitterAutoSize(emitter, 1.0, 288.0) == Catch::Approx(96.0));
    emitter.start = 24.0;
    CHECK(emitterAutoSize(emitter, 1.0, 600.0) == Catch::Approx(192.0));
    // at least 1
    CHECK(emitterAutoSize(emitter, 1.0, 25.0) == 1.0);
    // a curve without a reach keeps its size
    emitter.falloff = LightFalloff::Smooth;
    CHECK(emitterAutoSize(emitter, 1.0, 600.0) == 96.0);
  }

  SECTION("applyEmitterAutoRange")
  {
    auto omni = LightProperties{};
    omni.radius = vm::vec3d{500, 500, 500};
    omni.emitter = seededEmitterProperties();

    SECTION("A reach edit sets the range")
    {
      CHECK(
        applyEmitterAutoRange(omni, EmitterEdit::Reach).radius
        == vm::vec3d{288, 288, 288});

      // brighter reaches further
      auto bright = omni;
      bright.color = vm::vec3d{2, 2, 2};
      CHECK(
        applyEmitterAutoRange(bright, EmitterEdit::Reach).radius.x()
        == Catch::Approx(96.0 * std::sqrt(19.0)));

      // an omni with unequal radii is scaled as a whole
      auto ellipsoid = omni;
      ellipsoid.radius = vm::vec3d{100, 200, 400};
      ellipsoid.equalRadius = false;
      const auto scaled = applyEmitterAutoRange(ellipsoid, EmitterEdit::Reach);
      CHECK(scaled.radius.x() == Catch::Approx(72.0));
      CHECK(scaled.radius.y() == Catch::Approx(144.0));
      CHECK(scaled.radius.z() == Catch::Approx(288.0));

      // a spot gets R R R
      auto spot = omni;
      spot.type = LightType::Spot;
      CHECK(
        applyEmitterAutoRange(spot, EmitterEdit::Reach).radius
        == vm::vec3d{288, 288, 288});
    }

    SECTION("A range edit sets the size")
    {
      const auto light = applyEmitterAutoRange(omni, EmitterEdit::Range);
      CHECK(light.radius == vm::vec3d{500, 500, 500});
      CHECK(light.emitter.size == Catch::Approx(500.0 / 3.0));

      // the start stays below the range
      auto small = omni;
      small.radius = vm::vec3d{30, 30, 30};
      small.emitter.start = 50.0;
      const auto clamped = applyEmitterAutoRange(small, EmitterEdit::Range);
      CHECK(clamped.emitter.start == 29.0);
      CHECK(clamped.emitter.size == 1.0);
    }

    SECTION("Nothing changes without auto range")
    {
      auto linear = omni;
      linear.emitter.falloff = LightFalloff::Linear;
      CHECK(applyEmitterAutoRange(linear, EmitterEdit::Reach) == linear);

      auto off = omni;
      off.emitter.autoRange = false;
      CHECK(applyEmitterAutoRange(off, EmitterEdit::Reach) == off);
      CHECK(applyEmitterAutoRange(off, EmitterEdit::Range) == off);

      // a sun has no auto range
      auto sun = omni;
      sun.type = LightType::Sun;
      CHECK(applyEmitterAutoRange(sun, EmitterEdit::Reach) == sun);
      CHECK(applyEmitterAutoRange(sun, EmitterEdit::Range) == sun);

      auto texture = omni;
      texture.emitter.enabled = false;
      CHECK(applyEmitterAutoRange(texture, EmitterEdit::Reach) == texture);
      CHECK(applyEmitterAutoRange(texture, EmitterEdit::Range) == texture);
    }

    SECTION("A range edit on a curve without a reach turns auto range off")
    {
      // G0 section 5: the mapper has taken the range in hand; the size stays
      for (const auto falloff : {LightFalloff::Linear, LightFalloff::Smooth})
      {
        CAPTURE(falloff);
        auto light = omni;
        light.emitter.falloff = falloff;
        auto expected = light;
        expected.emitter.autoRange = false;
        CHECK(applyEmitterAutoRange(light, EmitterEdit::Range) == expected);

        auto spot = light;
        spot.type = LightType::Spot;
        CHECK_FALSE(applyEmitterAutoRange(spot, EmitterEdit::Range).emitter.autoRange);
      }
    }
  }
}

} // namespace tb::mdl
