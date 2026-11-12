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
#include "mdl/LightVolume.h"
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
#include <cmath>
#include <ostream>
#include <utility>

namespace tb::mdl
{

const std::vector<std::string>& emitterPropertyKeys()
{
  static const auto keys = std::vector<std::string>{
    LightPropertyKeys::Emitter,
    LightPropertyKeys::EmitShape,
    LightPropertyKeys::EmitAutoRange,
    LightPropertyKeys::EmitFalloff,
    LightPropertyKeys::EmitSize,
    LightPropertyKeys::EmitStart,
    LightPropertyKeys::EmitClamp,
    LightPropertyKeys::EmitCone,
    LightPropertyKeys::EmitPenumbra,
    LightPropertyKeys::EmitSquare,
    LightPropertyKeys::EmitPattern,
    LightPropertyKeys::EmitPatternStrength,
    LightPropertyKeys::EmitPatternScale,
    LightPropertyKeys::EmitPatternPhase,
    LightPropertyKeys::EmitEffect,
    LightPropertyKeys::EmitEffectSpeed,
    LightPropertyKeys::EmitEffectAmount,
    LightPropertyKeys::EmitEffectPhase,
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

// the names of emit_falloff and emit_pattern, in the order of the enums
constexpr auto FalloffNames = std::array<std::string_view, 5>{
  "invsquare", "linear", "smooth", "exponential", "invsquare_clamped"};
constexpr auto PatternNames = std::array<std::string_view, 6>{
  "none", "subsurface", "ice", "caustic", "banded", "granular"};
// emit_shape, in the order of LightType, which is the order of emitterShape_t
constexpr auto EmitterShapeNames = std::array<std::string_view, 3>{"omni", "spot", "sun"};
// emit_effect, in the order of emitterEffect_t
constexpr auto EmitterEffectNames = std::array<std::string_view, EmitterEffectCount>{
  "none",
  "flicker_a",
  "slow_strong_pulse",
  "candle_a",
  "fast_strobe",
  "gentle_pulse",
  "flicker_b",
  "candle_b",
  "candle_c",
  "slow_strobe",
  "fluorescent",
  "slow_pulse",
  "strobe",
  "pulse",
  "flicker",
};

// predef_lightstylesinfo of the engine's renderer/Material.h
constexpr auto LightStyleNames = std::array<std::string_view, LightStyleCount>{
  "Normal",
  "Flicker A",
  "Slow Strong Pulse",
  "Candle A",
  "Fast Strobe",
  "Gentle Pulse",
  "Flicker B",
  "Candle B",
  "Candle C",
  "Slow Strobe",
  "Fluorescent Flicker",
  "Slow Pulse (no black)",
};

// the display names of the effects that follow the eleven predefined styles
constexpr auto EmitterWaveformNames =
  std::array<std::string_view, 3>{"Strobe", "Pulse", "Flicker"};

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

// a spot's target shorter than this is no target: the light volume's degenerate guard
constexpr auto ZeroTargetEpsilon = 1e-6;

/**
 * Reads the number at the start of the given text like atof (and sscanf's "%f") read
 * it: white space is skipped, a plus sign is read. Returns nothing if the text does not
 * start with a number. std::from_chars also accepts "nan", "inf" and "infinity", which
 * are rejected like any other text that is not a number: atof would read them, but no
 * light geometry is defined by them.
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
  if (error != std::errc{} || !vm::is_finite(result))
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

  int integer(const std::string& key, const int defaultValue) const
  {
    const auto* value = m_entity.property(key);
    return value ? parseIdTech4Int(*value) : defaultValue;
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
 * EM_ParseFromDict without the derived tangents: every key with its default and clamp.
 * The keys of a disabled block are read too, because a disabled block keeps them.
 */
EmitterProperties parseEmitterProperties(const LightPropertyReader& reader)
{
  using namespace LightPropertyKeys;

  const auto* falloff = reader.string(EmitFalloff);
  const auto* pattern = reader.string(EmitPattern);
  const auto* effect = reader.string(EmitEffect);

  // like sscanf "%f %f": one number sets both angles
  auto coneH = 60.0;
  auto coneV = 60.0;
  if (const auto* cone = reader.string(EmitCone))
  {
    auto pos = std::size_t{0};
    if (const auto h = parseLeadingDouble(*cone, pos))
    {
      coneH = *h;
      coneV = parseLeadingDouble(*cone, pos).value_or(*h);
    }
  }

  return EmitterProperties{
    .size = std::max(reader.number(EmitSize, 96.0), 1.0),
    .start = std::max(reader.number(EmitStart, 0.0), 0.0),
    .coneH = std::clamp(coneH, MinEmitterCone, MaxEmitterCone),
    .coneV = std::clamp(coneV, MinEmitterCone, MaxEmitterCone),
    .penumbra = std::clamp(reader.number(EmitPenumbra, 0.2), 0.0, 1.0),
    .squareness = std::clamp(
      reader.number(EmitSquare, 2.0), MinEmitterSquareness, MaxEmitterSquareness),
    .patternStrength = std::clamp(reader.number(EmitPatternStrength, 0.0), 0.0, 1.0),
    .patternScale = reader.number(EmitPatternScale, 1.0),
    .patternPhase = reader.number(EmitPatternPhase, 0.0),
    .effectSpeed = std::clamp(reader.number(EmitEffectSpeed, 1.0), 0.0, 100.0),
    .effectAmount = std::clamp(reader.number(EmitEffectAmount, 1.0), 0.0, 1.0),
    .effectPhase = reader.number(EmitEffectPhase, 0.0),
    .falloff = (falloff ? parseLightFalloffName(*falloff) : std::nullopt)
                 .value_or(LightFalloff::InvSquareClamped),
    .pattern = (pattern ? parseLightPatternName(*pattern) : std::nullopt)
                 .value_or(LightPattern::None),
    .effect = (effect ? parseEmitterEffectName(*effect) : std::nullopt).value_or(0),
    .enabled = reader.boolean(Emitter, false),
    .hasBlock = std::ranges::any_of(
      emitterPropertyKeys(), [&](const auto& key) { return reader.has(key); }),
    .autoRange = reader.boolean(EmitAutoRange, true),
    .energyClamp = reader.boolean(EmitClamp, true),
  };
}

/**
 * The volume of an enabled emitter as the game sets it: the target keys and "light" are
 * not read, and the shape comes from emit_shape or, without a known one, from parallel.
 */
void parseEmitterVolume(const LightPropertyReader& reader, LightProperties& light)
{
  using namespace LightPropertyKeys;

  const auto* shape = reader.string(EmitShape);
  const auto inferred =
    reader.boolean(Parallel, false) ? LightType::Sun : LightType::Point;
  light.type = (shape ? parseEmitterShapeName(*shape) : std::nullopt).value_or(inferred);
  light.badTargetInfo = false;

  // each component at least 1
  const auto radius = vm::max(
    reader.vector(LightRadius).value_or(vm::vec3d{300, 300, 300}), vm::vec3d{1, 1, 1});

  if (light.type == LightType::Spot)
  {
    const auto range = emitterRange(radius);
    light.radius = vm::vec3d{range, range, range};
    light.equalRadius = true;
  }
  else
  {
    light.radius = radius;
    light.equalRadius = radius.x() == radius.y() && radius.x() == radius.z();

    if (const auto center = reader.vector(LightCenter))
    {
      light.hasCenter = true;
      light.center = *center;
    }
  }
}

/**
 * The volume of a light that is not an emitter, as the engine reads it.
 */
void parseTextureLightVolume(const LightPropertyReader& reader, LightProperties& light)
{
  using namespace LightPropertyKeys;

  const auto target = reader.vector(LightTarget);
  const auto up = reader.vector(LightUp);
  const auto right = reader.vector(LightRight);

  const auto hasAllTargetInfo = target && up && right;
  const auto hasSomeTargetInfo = target || up || right;

  // The order of the engine: with some, but not all, of the target keys it prints "bad
  // target info" and returns before it reads "parallel"; R_DeriveLightData then prefers
  // the parallel projection to the spot projection, so "parallel" makes a sun even with
  // all three target keys, which then stay unread here like a spot's radius.
  if (hasSomeTargetInfo && !hasAllTargetInfo)
  {
    light.badTargetInfo = true;
    light.type = LightType::Point;
  }
  else
  {
    const auto parallel = reader.boolean(Parallel, false);
    light.type = parallel           ? LightType::Sun
                 : hasAllTargetInfo ? LightType::Spot
                                    : LightType::Point;
  }

  if (light.type == LightType::Spot)
  {
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
}

const EntityProperty* findProperty(
  const std::vector<EntityProperty>& properties, const std::string& key)
{
  const auto it = findEntityProperty(properties, key);
  return it != properties.end() ? &*it : nullptr;
}

/**
 * The emitter keys of the given block, written for a light of the given type, in the
 * order of emitterPropertyKeys. emit_cone is one number when both angles write the same.
 */
std::vector<EntityProperty> emitterPropertyValues(
  const EmitterProperties& emitter, const LightType type)
{
  using namespace LightPropertyKeys;

  const auto coneH = formatIdTech4Float(emitter.coneH, 4);
  const auto coneV = formatIdTech4Float(emitter.coneV, 4);

  // built in place: a braced list would copy each of its elements (RV2-TP-02)
  auto values = std::vector<EntityProperty>{};
  values.reserve(emitterPropertyKeys().size());
  values.emplace_back(Emitter, formatBool(emitter.enabled));
  values.emplace_back(EmitShape, std::string{emitterShapeName(type)});
  values.emplace_back(EmitAutoRange, formatBool(emitter.autoRange));
  values.emplace_back(EmitFalloff, std::string{lightFalloffName(emitter.falloff)});
  values.emplace_back(EmitSize, formatIdTech4Float(emitter.size, 4));
  values.emplace_back(EmitStart, formatIdTech4Float(emitter.start, 4));
  values.emplace_back(EmitClamp, formatBool(emitter.energyClamp));
  values.emplace_back(EmitCone, coneH == coneV ? coneH : coneH + " " + coneV);
  values.emplace_back(EmitPenumbra, formatIdTech4Float(emitter.penumbra, 4));
  values.emplace_back(EmitSquare, formatIdTech4Float(emitter.squareness, 4));
  values.emplace_back(EmitPattern, std::string{lightPatternName(emitter.pattern)});
  values.emplace_back(
    EmitPatternStrength, formatIdTech4Float(emitter.patternStrength, 4));
  values.emplace_back(EmitPatternScale, formatIdTech4Float(emitter.patternScale, 4));
  values.emplace_back(EmitPatternPhase, formatIdTech4Float(emitter.patternPhase, 4));
  values.emplace_back(EmitEffect, std::string{emitterEffectName(emitter.effect)});
  values.emplace_back(EmitEffectSpeed, formatIdTech4Float(emitter.effectSpeed, 4));
  values.emplace_back(EmitEffectAmount, formatIdTech4Float(emitter.effectAmount, 4));
  values.emplace_back(EmitEffectPhase, formatIdTech4Float(emitter.effectPhase, 4));
  return values;
}

/**
 * The volume keys of a light that is not an emitter, as LightInfo::ToDict writes them.
 */
void appendTextureLightVolume(const LightProperties& light, LightPropertyUpdate& update)
{
  using namespace LightPropertyKeys;

  const auto set = [&](const std::string& key, std::string value) {
    update.set.emplace_back(key, std::move(value));
  };
  const auto remove = [&](const std::string& key) { update.remove.push_back(key); };

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
}

/**
 * The volume keys of an enabled emitter: a spot's range three times, an omni's or a sun's
 * light_radius, light_center and parallel as a point light and a sun write them, and none
 * of the five target vectors, which an emitter ignores.
 */
void appendEmitterVolume(const LightProperties& light, LightPropertyUpdate& update)
{
  using namespace LightPropertyKeys;

  const auto set = [&](const std::string& key, std::string value) {
    update.set.emplace_back(key, std::move(value));
  };
  const auto remove = [&](const std::string& key) { update.remove.push_back(key); };

  if (light.type == LightType::Spot)
  {
    const auto range = emitterRange(light.radius);
    set(LightRadius, formatVector(vm::vec3d{range, range, range}, 2));
    remove(LightCenter);
    remove(Parallel);
  }
  else
  {
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
  }

  remove(LightTarget);
  remove(LightUp);
  remove(LightRight);
  remove(LightStart);
  remove(LightEnd);
}

/**
 * The properties that the given entity is written with, as the diff towards the edited
 * light compares them. A disabled emitter block that the edit enables is read with the
 * values its present keys keep, so that they are written only where the edit changes
 * them.
 */
LightPropertyUpdate writtenForDiff(const Entity& entity, const LightProperties& edited)
{
  const auto light = parseLightProperties(entity);
  auto update = lightPropertyUpdate(light);

  if (!light.emitter.enabled && edited.emitter.enabled)
  {
    for (auto& property : emitterPropertyValues(light.emitter, edited.type))
    {
      const auto& key = property.key();
      if (
        key != LightPropertyKeys::Emitter && key != LightPropertyKeys::EmitShape
        && entity.hasProperty(key))
      {
        update.set.push_back(std::move(property));
      }
    }
  }

  return update;
}

} // namespace

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

std::string_view lightStyleName(const int style)
{
  if (style == -1)
  {
    return "None";
  }
  return style >= 0 && style < LightStyleCount
           ? LightStyleNames[static_cast<std::size_t>(style)]
           : std::string_view{};
}

kdl_reflect_impl(LightCurve);

kdl_reflect_impl(EmitterProperties);

std::string_view emitterShapeName(const LightType type)
{
  return EmitterShapeNames[static_cast<std::size_t>(type)];
}

std::optional<LightType> parseEmitterShapeName(const std::string_view name)
{
  return parseName<LightType>(EmitterShapeNames, name);
}

std::string_view emitterEffectName(const int effect)
{
  return effect >= 0 && effect < EmitterEffectCount
           ? EmitterEffectNames[static_cast<std::size_t>(effect)]
           : std::string_view{};
}

std::optional<int> parseEmitterEffectName(const std::string_view name)
{
  return parseName<int>(EmitterEffectNames, name);
}

std::string_view emitterEffectDisplayName(const int effect)
{
  if (effect == 0)
  {
    return "None";
  }
  if (effect > 0 && effect < LightStyleCount)
  {
    return LightStyleNames[static_cast<std::size_t>(effect)];
  }
  if (effect >= LightStyleCount && effect < EmitterEffectCount)
  {
    return EmitterWaveformNames[static_cast<std::size_t>(effect - LightStyleCount)];
  }
  return {};
}

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

  // one pass decides the block: a light without an emitter key (every texture light that
  // never was an emitter) keeps the block's defaults without a lookup of its keys
  light.emitter =
    hasEmitPrefixedKey(entity) ? parseEmitterProperties(reader) : EmitterProperties{};
  if (light.emitter.enabled)
  {
    // an emitter wins over the target keys
    parseEmitterVolume(reader, light);
  }
  else
  {
    parseTextureLightVolume(reader, light);
  }

  if (const auto* color = reader.string(Color))
  {
    // never parsed as an Rgb, whose float components stop at 1; a number that is not
    // finite is read like parseIdTech4Vector reads it
    const auto parsed = vm::parse<double, 3>(*color);
    light.color = parsed && vm::is_finite(*parsed) ? *parsed : parseIdTech4Vector(*color);
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

  // a hand-set range wins (G0 section 5): an omni or a spot whose range is out of step
  // with its auto range is read with auto range off; a sun's radius is a box, and a
  // curve without a reach factor keeps the stored flag
  if (emitterRangeSetByHand(light))
  {
    light.emitter.autoRange = false;
  }

  return light;
}

bool emitterRangeSetByHand(const LightProperties& light)
{
  return light.emitter.enabled && light.emitter.autoRange && light.type != LightType::Sun
         && emitterCurveHasReach(light.emitter.falloff)
         && !emitterRangeInStep(
           light.emitter, lightIntensity(light), emitterRange(light.radius));
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

  if (light.emitter.enabled)
  {
    appendEmitterVolume(light, update);
  }
  else if (
    light.type == LightType::Spot && light.emitter.hasBlock
    && vm::length(light.target) < ZeroTargetEpsilon)
  {
    // an emitter spot whose flag alone was cleared still has no target vectors: it is
    // written as the texture spot round its cone, never with a target of 0
    appendTextureLightVolume(textureSpotFromEmitterSpot(light), update);
  }
  else
  {
    appendTextureLightVolume(light, update);
  }

  if (light.style != -1 && light.type != LightType::Sun)
  {
    set(Style, fmt::format("{}", light.style));
  }
  else
  {
    remove(Style);
  }

  // an emitter writes its whole block, defaults included, so that the block describes
  // itself
  if (light.emitter.enabled)
  {
    for (auto& property : emitterPropertyValues(light.emitter, light.type))
    {
      update.set.push_back(std::move(property));
    }
  }
  else if (light.emitter.hasBlock)
  {
    // the emit_* keys stay, so that a light switched to a texture light and back gets its
    // emitter block again
    set(Emitter, "0");
  }

  return update;
}

LightPropertyUpdate lightKeyDiff(const Entity& entity, const LightProperties& edited)
{
  const auto before = writtenForDiff(entity, edited);
  const auto after = lightPropertyUpdate(edited);

  auto diff = LightPropertyUpdate{};

  // only the values that are written differently and that the entity does not already
  // have verbatim (an unread light_target of a sun that becomes a spot)
  for (const auto& property : after.set)
  {
    const auto* beforeProperty = findProperty(before.set, property.key());
    const auto* entityValue = entity.property(property.key());
    if (
      (!beforeProperty || beforeProperty->value() != property.value())
      && !(entityValue && *entityValue == property.value()))
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
    const auto check = writtenForDiff(changedEntity, edited);

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

  // an emitter's block is written whole once an edit changes anything, so that it
  // describes itself; emit_auto_range also where the entity's own value differs, since a
  // hand-set range is read with auto range off over a stored "1" (G0 section 5)
  if (edited.emitter.enabled && (!diff.set.empty() || !diff.remove.empty()))
  {
    for (const auto& property : after.set)
    {
      const auto& key = property.key();
      const auto* entityValue = entity.property(key);
      const auto stale =
        !entityValue
        || (key == LightPropertyKeys::EmitAutoRange && *entityValue != property.value());
      // every emitter key starts with "emit", and no other key a light is written with
      if (key.starts_with("emit") && stale && !findProperty(diff.set, key))
      {
        diff.set.push_back(property);
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
  light.emitter = keep.emitter;

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

  // an emitter spot has no target vectors, its cone is a new spot's, and the reach of an
  // omni or a spot follows from auto range
  if (light.emitter.enabled)
  {
    if (type == LightType::Spot)
    {
      const auto defaults = EmitterProperties{};
      light.target = vm::vec3d{0, 0, 0};
      light.right = vm::vec3d{0, 0, 0};
      light.up = vm::vec3d{0, 0, 0};
      light.end = vm::vec3d{0, 0, 0};
      light.radius = vm::vec3d{300, 300, 300};
      light.equalRadius = true;
      light.emitter.coneH = defaults.coneH;
      light.emitter.coneV = defaults.coneV;
      light.emitter.penumbra = defaults.penumbra;
      light.emitter.squareness = defaults.squareness;
    }
    return applyEmitterAutoRange(light, EmitterEdit::Reach);
  }

  return light;
}

bool lightUsesEmitter(const LightProperties& light)
{
  return light.emitter.enabled;
}

bool hasEmitterBlock(const Entity& entity)
{
  return std::ranges::any_of(emitterPropertyKeys(), [&](const auto& key) {
    return key != LightPropertyKeys::Emitter && entity.hasProperty(key);
  });
}

bool hasEmitPrefixedKey(const Entity& entity)
{
  return std::ranges::any_of(entity.properties(), [](const auto& property) {
    return property.key().starts_with("emit");
  });
}

EmitterProperties seededEmitterProperties()
{
  auto emitter = EmitterProperties{};
  emitter.enabled = true;
  // written whole, so the light carries the block from now on
  emitter.hasBlock = true;
  return emitter;
}

LightProperties seededEmitterLight(const LightType type, const LightProperties& keep)
{
  auto seeded = keep;
  seeded.emitter = seededEmitterProperties();
  seeded.emitter.effect = emitterEffectForStyle(keep.style);
  // the style stays on the light, so that switching back to Texture restores it
  auto light = defaultLight(type, seeded);
  light.style = keep.style;
  return light;
}

LightProperties emitterShapeSwitch(const LightType type, const LightProperties& before)
{
  auto light = defaultLight(type, before);
  if (!lightUsesEmitter(light))
  {
    return light;
  }

  // an emitter keeps its shadow and specular flags (DP-E3-05)
  light.castShadows = before.castShadows;
  light.skipSpecular = before.skipSpecular;
  if (type == LightType::Sun)
  {
    return light;
  }

  // With auto range in force defaultLight took the range from it. Otherwise the range is
  // kept, which a sun, whose radius is a box, does not have: from a sun defaultLight gave
  // the auto range when the flag is on and the curve has a reach, else 300.
  if (before.type != LightType::Sun && !emitterAutoRangeInForce(before))
  {
    const auto range = std::max(emitterRange(before.radius), MinEmitterRange);
    light.radius = vm::vec3d{range, range, range};
    light.equalRadius = true;
  }
  else if (before.type == LightType::Sun && !before.emitter.autoRange)
  {
    light.radius = vm::vec3d{300, 300, 300};
    light.equalRadius = true;
  }

  light.emitter.start =
    clampEmitterStart(light.emitter.start, emitterRange(light.radius));
  return light;
}

int emitterEffectForStyle(const int style)
{
  // effects 1 to 11 are predef_lightstyles 1 to 11, which style selects by the same
  // number
  return style > 0 && style < LightStyleCount ? style : 0;
}

LightProperties applyEmitterOmniRangeFloor(const LightProperties& light)
{
  if (!lightUsesEmitter(light) || light.type != LightType::Point)
  {
    return light;
  }

  // the components are at least 1 (G0 section 2); the guard keeps a 0 from dividing
  const auto largest = emitterRange(light.radius);
  if (largest <= 0.0 || largest >= MinEmitterRange)
  {
    return light;
  }

  auto result = light;
  result.radius = light.radius * (MinEmitterRange / largest);
  return result;
}

double emitterRange(const vm::vec3d& radius)
{
  return vm::get_max_component(radius);
}

double lightIntensity(const LightProperties& light)
{
  return lightIntensity(light.color);
}

double lightIntensity(const vm::vec3d& color)
{
  return vm::get_max_component(color);
}

double clampEmitterStart(const double start, const double range)
{
  return std::clamp(start, 0.0, std::max(range - 1.0, 0.0));
}

double emitterTanHalf(const double coneDegrees)
{
  return std::tan(
    vm::to_radians(std::clamp(coneDegrees, MinEmitterCone, MaxEmitterCone)) / 2.0);
}

double emitterConeFromTanHalf(const double tanHalf)
{
  return std::clamp(
    2.0 * vm::to_degrees(std::atan(tanHalf)), MinEmitterCone, MaxEmitterCone);
}

double emitterSinHalf(const double coneDegrees)
{
  return std::sin(
    vm::to_radians(std::clamp(coneDegrees, MinEmitterCone, MaxEmitterCone)) / 2.0);
}

double emitterConeFromSinHalf(const double sinHalf)
{
  return std::clamp(
    2.0 * vm::to_degrees(std::asin(std::clamp(sinHalf, 0.0, 1.0))),
    MinEmitterCone,
    MaxEmitterCone);
}

double emitterReachFactor(const LightFalloff falloff, const double intensity)
{
  // where the curve falls to 0.1 of the light's intensity, in sizes past its start
  switch (falloff)
  {
  case LightFalloff::InvSquareClamped:
    return std::max(1.0, std::sqrt(std::max(intensity / 0.1 - 1.0, 0.0)));
  case LightFalloff::InvSquare:
    return std::max(1.0, std::sqrt(intensity / 0.1));
  case LightFalloff::Exponential:
    return std::max(1.0, std::log(std::max(intensity / 0.1, 1.0)));
  case LightFalloff::Linear:
  case LightFalloff::Smooth:
    return 0.0;
    switchDefault();
  }
}

bool emitterCurveHasReach(const LightFalloff falloff)
{
  // emitterReachFactor is at least 1 for the other curves, NaN intensities included
  return falloff != LightFalloff::Linear && falloff != LightFalloff::Smooth;
}

double emitterAutoRange(const EmitterProperties& emitter, const double intensity)
{
  const auto c = emitterReachFactor(emitter.falloff, intensity);
  return c > 0.0 ? std::max(MinEmitterRange, emitter.start + emitter.size * c) : 0.0;
}

double emitterAutoSize(
  const EmitterProperties& emitter, const double intensity, const double range)
{
  const auto c = emitterReachFactor(emitter.falloff, intensity);
  return c > 0.0 ? std::max(1.0, (range - emitter.start) / c) : emitter.size;
}

bool emitterRangeInStep(
  const EmitterProperties& emitter, const double intensity, const double range)
{
  // the tolerance is wide on purpose: _color is stored with few decimals, which moves a
  // dim lamp's auto range by a few percent, and a light written under auto range must
  // read back in step
  const auto autoRange = emitterAutoRange(emitter, intensity);
  return autoRange > 0.0
         && std::abs(range - autoRange) <= std::max(1.0, 0.05 * autoRange);
}

bool emitterAutoRangeInForce(const LightProperties& light)
{
  const auto intensity = lightIntensity(light);
  return lightUsesEmitter(light) && light.type != LightType::Sun
         && light.emitter.autoRange
         && emitterRangeInStep(light.emitter, intensity, emitterRange(light.radius));
}

LightProperties applyEmitterAutoRange(
  const LightProperties& light, const EmitterEdit edit)
{
  const auto& emitter = light.emitter;
  const auto intensity = lightIntensity(light);

  // a range set on a curve without a reach factor turns auto range off: the mapper has
  // taken the range in hand (G0 section 5)
  if (
    edit == EmitterEdit::Range && emitter.enabled && emitter.autoRange
    && light.type != LightType::Sun && !emitterCurveHasReach(emitter.falloff))
  {
    auto result = light;
    result.emitter.autoRange = false;
    return result;
  }

  // a sun's light_radius is a box, not a reach
  if (
    !emitter.enabled || !emitter.autoRange || light.type == LightType::Sun
    || !emitterCurveHasReach(emitter.falloff))
  {
    return light;
  }

  auto result = light;
  switch (edit)
  {
  case EmitterEdit::Reach: {
    const auto range = emitterAutoRange(emitter, intensity);
    const auto largest = emitterRange(light.radius);
    if (light.type == LightType::Spot || light.equalRadius || largest <= 0.0)
    {
      result.radius = vm::vec3d{range, range, range};
      result.equalRadius = true;
    }
    else
    {
      // an omni with unequal radii keeps their proportions
      result.radius = light.radius * (range / largest);
    }
    break;
  }
  case EmitterEdit::Range: {
    const auto range = emitterRange(light.radius);
    result.emitter.start = clampEmitterStart(emitter.start, range);
    result.emitter.size = emitterAutoSize(result.emitter, intensity, range);
    break;
  }
    switchDefault();
  }

  result.emitter.start =
    clampEmitterStart(result.emitter.start, emitterRange(result.radius));
  return result;
}

LightProperties applyEmitterFalloff(
  const LightProperties& light, const LightFalloff falloff)
{
  auto result = light;
  result.emitter.falloff = falloff;
  if (!lightUsesEmitter(light))
  {
    return result;
  }

  // the flag as it stands (G0 section 5, corrected for DP-T5-01): on, the range follows
  // the new curve; off, it is kept; a curve without a reach factor keeps the range
  return applyEmitterAutoRange(result, EmitterEdit::Reach);
}

} // namespace tb::mdl
