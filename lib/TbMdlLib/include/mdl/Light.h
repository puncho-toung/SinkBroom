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

#pragma once

#include "mdl/EntityProperties.h"

#include "kd/reflection_decl.h"

#include "vm/vec.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Entity;

/**
 * The properties of an id Tech 4 light (game config key entities.lightEditor "idtech4"),
 * as RBDOOM-3-BFG's idGameEdit::ParseSpawnArgsToRenderLight reads them and its in-game
 * light editor (LightInfo::FromDict and LightInfo::ToDict) reads and writes them.
 */
namespace LightPropertyKeys
{
inline const std::string Light = "light";
inline const std::string LightOrigin = "light_origin";
inline const std::string LightRadius = "light_radius";
inline const std::string LightCenter = "light_center";
inline const std::string LightTarget = "light_target";
inline const std::string LightUp = "light_up";
inline const std::string LightRight = "light_right";
inline const std::string LightStart = "light_start";
inline const std::string LightEnd = "light_end";
inline const std::string Parallel = "parallel";
inline const std::string Color = "_color";
inline const std::string Texture = "texture";
inline const std::string Style = "style";
inline const std::string NoShadows = "noshadows";
inline const std::string NoSpecular = "nospecular";

inline const std::string MathEnabled = "math_enabled";
inline const std::string MathType = "math_type";
inline const std::string MathFalloff = "math_falloff";
inline const std::string MathFalloffExp = "math_falloff_exp";
inline const std::string MathInnerRadius = "math_inner_radius";
inline const std::string MathShapeExp = "math_shape_exp";
inline const std::string MathPenumbra = "math_penumbra";
inline const std::string MathPattern = "math_pattern";
inline const std::string MathPatternStrength = "math_pattern_strength";
inline const std::string MathPatternScale = "math_pattern_scale";
inline const std::string MathPatternPhase = "math_pattern_phase";
inline const std::string MathEnergyClamp = "math_energy_clamp";
} // namespace LightPropertyKeys

/**
 * The twelve keys of a math light, in the order in which the engine's ML_WriteToDict
 * writes them.
 */
const std::vector<std::string>& mathLightPropertyKeys();

enum class LightType
{
  Point,
  Spot,
  Sun,
};

std::ostream& operator<<(std::ostream& lhs, LightType rhs);

enum class LightFalloff
{
  InvSquare,
  Linear,
  Smooth,
  Exponential,
  InvSquareClamped,
};

std::ostream& operator<<(std::ostream& lhs, LightFalloff rhs);

enum class LightPattern
{
  None,
  Subsurface,
  Ice,
  Caustic,
  Banded,
  Granular,
};

std::ostream& operator<<(std::ostream& lhs, LightPattern rhs);

/**
 * The value of "math_type" for the given light type: point, spot or sun.
 */
std::string_view mathTypeName(LightType type);

/**
 * Parses a "math_type" value without regard to case; "parallel" is a sun. Returns
 * nothing for an unknown name.
 */
std::optional<LightType> parseMathTypeName(std::string_view name);

/**
 * The value of "math_falloff" for the given curve: invsquare, linear, smooth, exponential
 * or invsquare_clamped.
 */
std::string_view lightFalloffName(LightFalloff falloff);

/**
 * Parses a "math_falloff" value without regard to case. Returns nothing for an unknown
 * name, which the engine reads as invsquare_clamped.
 */
std::optional<LightFalloff> parseLightFalloffName(std::string_view name);

/**
 * The value of "math_pattern" for the given pattern: none, subsurface, ice, caustic,
 * banded or granular.
 */
std::string_view lightPatternName(LightPattern pattern);

/**
 * Parses a "math_pattern" value without regard to case. Returns nothing for an unknown
 * name, which the engine reads as none.
 */
std::optional<LightPattern> parseLightPatternName(std::string_view name);

/**
 * The math light block of a light (the engine's mathLightData_t) with the defaults of
 * LightInfo::Defaults. It has no type: "math_type" is derived from the light's type.
 */
struct MathLightProperties
{
  bool enabled = false;
  LightFalloff falloff = LightFalloff::InvSquareClamped;
  double falloffExp = 2.0;
  double innerRadius = 0.0;
  double shapeExp = 2.0;
  double penumbra = 0.15;
  LightPattern pattern = LightPattern::None;
  double patternStrength = 0.0;
  double patternScale = 1.0;
  double patternPhase = 0.0;
  bool energyClamp = true;

  kdl_reflect_decl(
    MathLightProperties,
    enabled,
    falloff,
    falloffExp,
    innerRadius,
    shapeExp,
    penumbra,
    pattern,
    patternStrength,
    patternScale,
    patternPhase,
    energyClamp);
};

/**
 * The editable state of a light, like the engine's LightInfo without its gizmo state.
 * The rotation is not part of it: it belongs to the id Tech 4 rotation policy.
 *
 * Of a spot light without explicit start and end points, start is 0 and end is the
 * target, as the engine reads them. The fields that do not belong to the light's type
 * keep their defaults.
 */
struct LightProperties
{
  LightType type = LightType::Point;
  // light_origin when the entity has it, else origin
  bool hasLightOrigin = false;
  vm::vec3d origin = vm::vec3d{0, 0, 0};
  vm::vec3d radius = vm::vec3d{300, 300, 300};
  bool equalRadius = true;
  bool hasCenter = false;
  vm::vec3d center = vm::vec3d{0, 0, 0};
  vm::vec3d target = vm::vec3d{0, 0, 0};
  vm::vec3d right = vm::vec3d{0, 0, 0};
  vm::vec3d up = vm::vec3d{0, 0, 0};
  bool explicitStartEnd = false;
  vm::vec3d start = vm::vec3d{0, 0, 0};
  vm::vec3d end = vm::vec3d{0, 0, 0};
  // HDR: hue times intensity, with components up to 8
  vm::vec3d color = vm::vec3d{1, 1, 1};
  // empty means the engine's default, lights/squarelight1
  std::string texture;
  int style = -1;
  bool castShadows = true;
  bool skipSpecular = false;
  MathLightProperties math;
  // some, but not all, of light_target, light_right and light_up are present; the engine
  // prints "bad target info" and this light is read as a point light
  bool badTargetInfo = false;

  kdl_reflect_decl(
    LightProperties,
    type,
    hasLightOrigin,
    origin,
    radius,
    equalRadius,
    hasCenter,
    center,
    target,
    right,
    up,
    explicitStartEnd,
    start,
    end,
    color,
    texture,
    style,
    castShadows,
    skipSpecular,
    math,
    badTargetInfo);
};

/**
 * Whether the light editor edits the given entity: its classname is "light" without
 * regard to case, or its definition declares "spawnclass" with the default "idLight", or
 * it has no definition and its classname starts with "light_" without regard to case.
 */
bool isLightEntity(const Entity& entity);

/**
 * Reads the given entity's light properties the way the engine reads them, where the
 * engine and the in-game editor disagree (a spot needs all of light_target, light_right
 * and light_up; light "0" is a radius of 0). Vectors are read like the engine's sscanf
 * reads them, so a value with one number reads (v, 0, 0). "math_type" is ignored.
 */
LightProperties parseLightProperties(const Entity& entity);

/**
 * Property changes: the properties to add or update, and the properties to remove.
 */
struct LightPropertyUpdate
{
  std::vector<EntityProperty> set;
  std::vector<std::string> remove;

  kdl_reflect_decl(LightPropertyUpdate, set, remove);
};

/**
 * Returns the properties that the engine's LightInfo::ToDict writes for the given light,
 * and the properties it deletes. Vector components are written with two decimals, _color
 * and the math floats with four, all without trailing zeros, and every vector with three
 * components. The rotation properties are never touched; a light with light_origin gets
 * neither origin nor light_origin.
 */
LightPropertyUpdate lightPropertyUpdate(const LightProperties& light);

/**
 * Returns only the changes that turn the given entity into the given edited light: the
 * properties whose written value differs from the value written for the entity as it is,
 * and the properties to remove that the entity has and that the edit does not leave
 * unread. The changes are checked by reading the changed entity again, so that a property
 * the entity lacks but whose engine default differs from the edited value is written too.
 */
LightPropertyUpdate lightKeyDiff(const Entity& entity, const LightProperties& edited);

/**
 * Returns a copy of the given entity with the given changes applied.
 */
Entity applyLightPropertyUpdate(Entity entity, const LightPropertyUpdate& update);

/**
 * Returns the engine's default light of the given type (LightInfo::DefaultPoint,
 * DefaultProjected and DefaultSun), which keeps the colour, the origin, light_origin and
 * the math block of the given light and resets everything else.
 */
LightProperties defaultLight(LightType type, const LightProperties& keep);

} // namespace tb::mdl
