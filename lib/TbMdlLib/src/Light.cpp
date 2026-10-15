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

#include "mdl/Light.h"

#include "base/Macros.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityRotation.h"
#include "mdl/PropertyDefinition.h"

#include "kd/ranges/contains.h"
#include "kd/reflection_impl.h"
#include "kd/string_compare.h"

#include "vm/scalar.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <ostream>

namespace tb::mdl
{

const std::vector<std::string>& mathLightPropertyKeys()
{
  static const auto keys = std::vector<std::string>{
    LightPropertyKeys::MathEnabled,
    LightPropertyKeys::MathType,
    LightPropertyKeys::MathFalloff,
    LightPropertyKeys::MathPattern,
    LightPropertyKeys::MathFalloffExp,
    LightPropertyKeys::MathInnerRadius,
    LightPropertyKeys::MathShapeExp,
    LightPropertyKeys::MathPenumbra,
    LightPropertyKeys::MathPatternStrength,
    LightPropertyKeys::MathPatternScale,
    LightPropertyKeys::MathPatternPhase,
    LightPropertyKeys::MathEnergyClamp,
  };
  return keys;
}

std::ostream& operator<<(std::ostream& lhs, const LightType rhs)
{
  switch (rhs)
  {
  case LightType::Point:
    lhs << "Point";
    break;
  case LightType::Spot:
    lhs << "Spot";
    break;
  case LightType::Sun:
    lhs << "Sun";
    break;
    switchDefault();
  }
  return lhs;
}

std::ostream& operator<<(std::ostream& lhs, const LightFalloff rhs)
{
  switch (rhs)
  {
  case LightFalloff::InvSquare:
    lhs << "InvSquare";
    break;
  case LightFalloff::Linear:
    lhs << "Linear";
    break;
  case LightFalloff::Smooth:
    lhs << "Smooth";
    break;
  case LightFalloff::Exponential:
    lhs << "Exponential";
    break;
  case LightFalloff::InvSquareClamped:
    lhs << "InvSquareClamped";
    break;
    switchDefault();
  }
  return lhs;
}

std::ostream& operator<<(std::ostream& lhs, const LightPattern rhs)
{
  switch (rhs)
  {
  case LightPattern::None:
    lhs << "None";
    break;
  case LightPattern::Subsurface:
    lhs << "Subsurface";
    break;
  case LightPattern::Ice:
    lhs << "Ice";
    break;
  case LightPattern::Caustic:
    lhs << "Caustic";
    break;
  case LightPattern::Banded:
    lhs << "Banded";
    break;
  case LightPattern::Granular:
    lhs << "Granular";
    break;
    switchDefault();
  }
  return lhs;
}

namespace
{

// the names ML_WriteToDict writes, in the order of the enums
constexpr auto MathTypeNames = std::array<std::string_view, 3>{"point", "spot", "sun"};
constexpr auto FalloffNames = std::array<std::string_view, 5>{
  "invsquare", "linear", "smooth", "exponential", "invsquare_clamped"};
constexpr auto PatternNames = std::array<std::string_view, 6>{
  "none", "subsurface", "ice", "caustic", "banded", "granular"};

template <typename E, std::size_t N>
std::optional<E> parseName(
  const std::array<std::string_view, N>& names, const std::string_view name)
{
  for (std::size_t i = 0; i < N; ++i)
  {
    if (kdl::ci::str_is_equal(names[i], name))
    {
      return static_cast<E>(i);
    }
  }
  return std::nullopt;
}

constexpr auto Whitespace = std::string_view{" \t\n\v\f\r"};

/**
 * Reads the number at the start of the given text like atof (and sscanf's "%f") read
 * it: white space is skipped, a plus sign is read. Returns nothing if the text does not
 * start with a number.
 */
std::optional<double> parseLeadingDouble(const std::string_view text, std::size_t& pos)
{
  pos = text.find_first_not_of(Whitespace, pos);
  if (pos == std::string_view::npos)
  {
    pos = text.size();
    return std::nullopt;
  }

  // std::from_chars does not read a plus sign
  auto first = pos;
  if (text[first] == '+')
  {
    ++first;
  }

  auto result = 0.0;
  const auto [end, error] =
    std::from_chars(text.data() + first, text.data() + text.size(), result);
  if (error != std::errc{})
  {
    return std::nullopt;
  }

  pos = static_cast<std::size_t>(end - text.data());
  return result;
}

/**
 * Reads a number like atof: the leading number of the text, or 0.
 */
double parseIdTech4Float(const std::string_view text)
{
  auto pos = std::size_t{0};
  return parseLeadingDouble(text, pos).value_or(0.0);
}

/**
 * Reads an integer like atoi: the leading integer of the text, or 0.
 */
int parseIdTech4Int(const std::string_view text)
{
  auto pos = text.find_first_not_of(Whitespace);
  if (pos == std::string_view::npos)
  {
    return 0;
  }
  if (text[pos] == '+')
  {
    ++pos;
  }

  auto result = 0;
  const auto [end, error] =
    std::from_chars(text.data() + pos, text.data() + text.size(), result);
  return error == std::errc{} ? result : 0;
}

/**
 * Reads a vector like idDict::GetVector: sscanf "%f %f %f" over 0 0 0, so the numbers
 * that are read are set and the others stay 0.
 */
vm::vec3d parseIdTech4Vector(const std::string_view text)
{
  auto result = vm::vec3d{0, 0, 0};
  auto pos = std::size_t{0};
  for (std::size_t i = 0; i < 3; ++i)
  {
    const auto number = parseLeadingDouble(text, pos);
    if (!number)
    {
      break;
    }
    result[i] = *number;
  }
  return result;
}

std::string formatVector(const vm::vec3d& v, const int decimals)
{
  return formatIdTech4Float(v.x(), decimals) + " " + formatIdTech4Float(v.y(), decimals)
         + " " + formatIdTech4Float(v.z(), decimals);
}

std::string formatBool(const bool b)
{
  return b ? "1" : "0";
}

class LightPropertyReader
{
private:
  const Entity& m_entity;

public:
  explicit LightPropertyReader(const Entity& entity)
    : m_entity{entity}
  {
  }

  bool has(const std::string& key) const { return m_entity.hasProperty(key); }

  std::optional<vm::vec3d> vector(const std::string& key) const
  {
    const auto* value = m_entity.property(key);
    return value ? std::optional{parseIdTech4Vector(*value)} : std::nullopt;
  }

  double number(const std::string& key, const double defaultValue) const
  {
    const auto* value = m_entity.property(key);
    return value ? parseIdTech4Float(*value) : defaultValue;
  }

  bool boolean(const std::string& key, const bool defaultValue) const
  {
    const auto* value = m_entity.property(key);
    return value ? parseIdTech4Int(*value) != 0 : defaultValue;
  }

  const std::string* string(const std::string& key) const
  {
    return m_entity.property(key);
  }
};

/**
 * ML_ParseFromDict, except that a disabled block keeps the defaults of
 * LightInfo::Defaults instead of zeroes, and that "math_type" is ignored.
 */
MathLightProperties parseMathLightProperties(const LightPropertyReader& reader)
{
  using namespace LightPropertyKeys;

  if (!reader.boolean(MathEnabled, false))
  {
    return MathLightProperties{};
  }

  const auto* falloff = reader.string(MathFalloff);
  const auto* pattern = reader.string(MathPattern);

  return MathLightProperties{
    .enabled = true,
    .falloff = (falloff ? parseLightFalloffName(*falloff) : std::nullopt)
                 .value_or(LightFalloff::InvSquareClamped),
    .falloffExp = std::clamp(reader.number(MathFalloffExp, 2.0), 0.1, 16.0),
    .innerRadius = std::clamp(reader.number(MathInnerRadius, 0.0), 0.0, 1.0),
    .shapeExp = std::clamp(reader.number(MathShapeExp, 2.0), 1.5, 32.0),
    .penumbra = std::clamp(reader.number(MathPenumbra, 0.15), 0.0, 1.0),
    .pattern = (pattern ? parseLightPatternName(*pattern) : std::nullopt)
                 .value_or(LightPattern::None),
    .patternStrength = std::clamp(reader.number(MathPatternStrength, 0.0), 0.0, 1.0),
    .patternScale = reader.number(MathPatternScale, 1.0),
    .patternPhase = reader.number(MathPatternPhase, 0.0),
    .energyClamp = reader.boolean(MathEnergyClamp, true),
  };
}

const EntityProperty* findProperty(
  const std::vector<EntityProperty>& properties, const std::string& key)
{
  const auto it = findEntityProperty(properties, key);
  return it != properties.end() ? &*it : nullptr;
}

} // namespace

std::string_view mathTypeName(const LightType type)
{
  return MathTypeNames[static_cast<std::size_t>(type)];
}

std::optional<LightType> parseMathTypeName(const std::string_view name)
{
  if (kdl::ci::str_is_equal(name, "parallel"))
  {
    return LightType::Sun;
  }
  return parseName<LightType>(MathTypeNames, name);
}

std::string_view lightFalloffName(const LightFalloff falloff)
{
  return FalloffNames[static_cast<std::size_t>(falloff)];
}

std::optional<LightFalloff> parseLightFalloffName(const std::string_view name)
{
  return parseName<LightFalloff>(FalloffNames, name);
}

std::string_view lightPatternName(const LightPattern pattern)
{
  return PatternNames[static_cast<std::size_t>(pattern)];
}

std::optional<LightPattern> parseLightPatternName(const std::string_view name)
{
  return parseName<LightPattern>(PatternNames, name);
}

kdl_reflect_impl(MathLightProperties);

kdl_reflect_impl(LightProperties);

kdl_reflect_impl(LightPropertyUpdate);

bool isLightEntity(const Entity& entity)
{
  const auto& classname = entity.classname();
  if (kdl::ci::str_is_equal(classname, "light"))
  {
    return true;
  }

  if (const auto* definition = entity.definition())
  {
    const auto* spawnclass = getPropertyDefinition(definition, "spawnclass");
    return spawnclass && PropertyDefinition::defaultValue(*spawnclass) == "idLight";
  }

  return kdl::ci::str_is_prefix(classname, "light_");
}

LightProperties parseLightProperties(const Entity& entity)
{
  using namespace LightPropertyKeys;

  const auto reader = LightPropertyReader{entity};
  auto light = LightProperties{};

  if (const auto lightOrigin = reader.vector(LightOrigin))
  {
    light.hasLightOrigin = true;
    light.origin = *lightOrigin;
  }
  else
  {
    light.origin = reader.vector(EntityPropertyKeys::Origin).value_or(vm::vec3d{0, 0, 0});
  }

  const auto target = reader.vector(LightTarget);
  const auto up = reader.vector(LightUp);
  const auto right = reader.vector(LightRight);

  if (target && up && right)
  {
    light.type = LightType::Spot;
    light.target = *target;
    light.up = *up;
    light.right = *right;

    if (const auto start = reader.vector(LightStart))
    {
      light.explicitStartEnd = true;
      light.start = *start;
      light.end = reader.vector(LightEnd).value_or(*target);
    }
    else
    {
      light.explicitStartEnd = false;
      light.start = vm::vec3d{0, 0, 0};
      light.end = *target;
    }
  }
  else
  {
    // the engine prints "bad target info" if some, but not all, are present
    light.badTargetInfo = target || up || right;
    light.type = reader.boolean(Parallel, false) ? LightType::Sun : LightType::Point;

    if (const auto radius = reader.vector(LightRadius))
    {
      light.radius = *radius;
    }
    else
    {
      // unlike LightInfo::FromDict, light "0" is a radius of 0, as the engine lights it
      const auto radius1 = reader.number(Light, 300.0);
      light.radius = vm::vec3d{radius1, radius1, radius1};
    }
    light.equalRadius =
      light.radius.x() == light.radius.y() && light.radius.x() == light.radius.z();

    if (const auto center = reader.vector(LightCenter))
    {
      light.hasCenter = true;
      light.center = *center;
    }
  }

  if (const auto* color = reader.string(Color))
  {
    // never parsed as an Rgb, whose float components stop at 1
    light.color = vm::parse<double, 3>(*color).value_or(parseIdTech4Vector(*color));
  }

  if (const auto* texture = reader.string(Texture))
  {
    light.texture = *texture;
  }

  if (const auto* style = reader.string(Style))
  {
    light.style = parseIdTech4Int(*style);
  }

  light.castShadows = !reader.boolean(NoShadows, false);
  light.skipSpecular = reader.boolean(NoSpecular, false);
  light.math = parseMathLightProperties(reader);

  return light;
}

LightPropertyUpdate lightPropertyUpdate(const LightProperties& light)
{
  using namespace LightPropertyKeys;

  auto update = LightPropertyUpdate{};
  const auto set = [&](const std::string& key, std::string value) {
    update.set.emplace_back(key, std::move(value));
  };
  const auto remove = [&](const std::string& key) { update.remove.push_back(key); };

  // a light_origin light is read-only, so neither origin nor light_origin is written
  if (!light.hasLightOrigin)
  {
    set(EntityPropertyKeys::Origin, formatVector(light.origin, 2));
  }

  // light_radius replaces light
  remove(Light);

  set(NoShadows, formatBool(!light.castShadows));
  set(NoSpecular, formatBool(light.skipSpecular));

  if (!light.texture.empty())
  {
    set(Texture, light.texture);
  }
  else
  {
    remove(Texture);
  }

  set(Color, formatVector(light.color, 4));

  switch (light.type)
  {
  case LightType::Point:
  case LightType::Sun: {
    const auto& radius = light.radius;
    set(
      LightRadius,
      formatVector(
        light.equalRadius ? vm::vec3d{radius.x(), radius.x(), radius.x()} : radius, 2));

    if (light.hasCenter)
    {
      set(LightCenter, formatVector(light.center, 2));
    }
    else
    {
      remove(LightCenter);
    }

    if (light.type == LightType::Sun)
    {
      set(Parallel, "1");
    }
    else
    {
      remove(Parallel);
    }

    remove(LightTarget);
    remove(LightUp);
    remove(LightRight);
    remove(LightStart);
    remove(LightEnd);
    break;
  }
  case LightType::Spot:
    set(LightTarget, formatVector(light.target, 2));
    set(LightUp, formatVector(light.up, 2));
    set(LightRight, formatVector(light.right, 2));

    if (light.explicitStartEnd)
    {
      set(LightStart, formatVector(light.start, 2));
      set(LightEnd, formatVector(light.end, 2));
    }
    else
    {
      remove(LightStart);
      remove(LightEnd);
    }

    remove(LightRadius);
    remove(LightCenter);
    remove(Parallel);
    break;
    switchDefault();
  }

  if (light.style != -1 && light.type != LightType::Sun)
  {
    set(Style, fmt::format("{}", light.style));
  }
  else
  {
    remove(Style);
  }

  if (light.math.enabled)
  {
    const auto& math = light.math;
    set(MathEnabled, "1");
    set(MathType, std::string{mathTypeName(light.type)});
    set(MathFalloff, std::string{lightFalloffName(math.falloff)});
    set(MathPattern, std::string{lightPatternName(math.pattern)});
    set(MathFalloffExp, formatIdTech4Float(math.falloffExp, 4));
    set(MathInnerRadius, formatIdTech4Float(math.innerRadius, 4));
    set(MathShapeExp, formatIdTech4Float(math.shapeExp, 4));
    set(MathPenumbra, formatIdTech4Float(math.penumbra, 4));
    set(MathPatternStrength, formatIdTech4Float(math.patternStrength, 4));
    set(MathPatternScale, formatIdTech4Float(math.patternScale, 4));
    set(MathPatternPhase, formatIdTech4Float(math.patternPhase, 4));
    set(MathEnergyClamp, formatBool(math.energyClamp));
  }
  else
  {
    for (const auto& key : mathLightPropertyKeys())
    {
      remove(key);
    }
  }

  return update;
}

LightPropertyUpdate lightKeyDiff(const Entity& entity, const LightProperties& edited)
{
  const auto before = lightPropertyUpdate(parseLightProperties(entity));
  const auto after = lightPropertyUpdate(edited);

  auto diff = LightPropertyUpdate{};

  // only the values that are written differently
  for (const auto& property : after.set)
  {
    const auto* beforeProperty = findProperty(before.set, property.key());
    if (!beforeProperty || beforeProperty->value() != property.value())
    {
      diff.set.push_back(property);
    }
  }

  // A property that the entity has but that is deleted when it is written as it is goes
  // unread (a spot light's light_radius, a light's "light" next to light_radius), so
  // removing it is no edit; removing "light" from a light without light_radius would
  // even change its radius.
  for (const auto& key : after.remove)
  {
    if (entity.hasProperty(key) && !kdl::ranges::contains(before.remove, key))
    {
      diff.remove.push_back(key);
    }
  }

  // Read the changed entity again: a property the entity lacks whose default differs
  // from the edited value (light_end defaults to the target, the radius to "light") is
  // written, and a property left unread before that would be read now (light_start or
  // parallel after a change of type) is removed.
  for (auto changed = true; changed;)
  {
    changed = false;

    const auto changedEntity = applyLightPropertyUpdate(entity, diff);
    const auto check = lightPropertyUpdate(parseLightProperties(changedEntity));

    for (const auto& property : after.set)
    {
      const auto* checkProperty = findProperty(check.set, property.key());
      if (
        (!checkProperty || checkProperty->value() != property.value())
        && !findProperty(diff.set, property.key()))
      {
        diff.set.push_back(property);
        changed = true;
      }
    }

    for (const auto& key : after.remove)
    {
      if (
        changedEntity.hasProperty(key) && findProperty(check.set, key)
        && !kdl::ranges::contains(diff.remove, key))
      {
        diff.remove.push_back(key);
        changed = true;
      }
    }
  }

  return diff;
}

Entity applyLightPropertyUpdate(Entity entity, const LightPropertyUpdate& update)
{
  for (const auto& property : update.set)
  {
    entity.addOrUpdateProperty(property.key(), property.value());
  }
  for (const auto& key : update.remove)
  {
    entity.removeProperty(key);
  }
  return entity;
}

LightProperties defaultLight(const LightType type, const LightProperties& keep)
{
  // LightInfo::Defaults, keeping what DefaultPoint, DefaultProjected and DefaultSun keep
  auto light = LightProperties{};
  light.type = type;
  light.hasLightOrigin = keep.hasLightOrigin;
  light.origin = keep.origin;
  light.color = keep.color;
  light.math = keep.math;

  switch (type)
  {
  case LightType::Point:
    light.radius = vm::vec3d{300, 300, 300};
    light.equalRadius = true;
    break;
  case LightType::Spot:
    light.target = vm::vec3d{0, 0, -256};
    light.up = vm::vec3d{0, -128, 0};
    light.right = vm::vec3d{-128, 0, 0};
    light.explicitStartEnd = false;
    light.start = vm::vec3d{0, 0, 0};
    light.end = light.target;
    break;
  case LightType::Sun:
    // the center is set but not written, so the engine's direction is 0 0 1
    light.center = vm::vec3d{4, 4, 32};
    light.hasCenter = false;
    light.radius = vm::vec3d{2048, 2048, 1024};
    light.equalRadius = false;
    break;
    switchDefault();
  }

  return light;
}

} // namespace tb::mdl
