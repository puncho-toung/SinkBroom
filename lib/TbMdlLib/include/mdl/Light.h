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

// the emitter block; light_radius, light_center and parallel are shared with the other
// lights
inline const std::string Emitter = "emitter";
inline const std::string EmitShape = "emit_shape";
inline const std::string EmitAutoRange = "emit_auto_range";
inline const std::string EmitFalloff = "emit_falloff";
inline const std::string EmitSize = "emit_size";
inline const std::string EmitStart = "emit_start";
inline const std::string EmitClamp = "emit_clamp";
inline const std::string EmitCone = "emit_cone";
inline const std::string EmitPenumbra = "emit_penumbra";
inline const std::string EmitSquare = "emit_square";
inline const std::string EmitPattern = "emit_pattern";
inline const std::string EmitPatternStrength = "emit_pattern_strength";
inline const std::string EmitPatternScale = "emit_pattern_scale";
inline const std::string EmitPatternPhase = "emit_pattern_phase";
inline const std::string EmitEffect = "emit_effect";
inline const std::string EmitEffectSpeed = "emit_effect_speed";
inline const std::string EmitEffectAmount = "emit_effect_amount";
inline const std::string EmitEffectPhase = "emit_effect_phase";
} // namespace LightPropertyKeys

/**
 * The eighteen keys of an emitter block in the order in which they are written: "emitter"
 * and the seventeen "emit_*" keys. light_radius, light_center and parallel are shared
 * with the other lights and are not among them.
 */
const std::vector<std::string>& emitterPropertyKeys();

/**
 * The number of the engine's predefined light styles (predef_lightstyles in
 * renderer/Material.h), which "style" selects by its index (-1 is no style) and emitter
 * effects 1 to 11 play.
 */
inline constexpr auto LightStyleCount = 12;

/**
 * The name of the given light style as the engine's predef_lightstylesinfo gives it
 * ("Normal", "Flicker A", ...), "None" for -1, and an empty name for any other number.
 */
std::string_view lightStyleName(int style);

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
 * The value of "emit_falloff" for the given curve: invsquare, linear, smooth, exponential
 * or invsquare_clamped.
 */
std::string_view lightFalloffName(LightFalloff falloff);

/**
 * Parses an "emit_falloff" value without regard to case. Returns nothing for an unknown
 * name, which the engine reads as invsquare_clamped.
 */
std::optional<LightFalloff> parseLightFalloffName(std::string_view name);

/**
 * The value of "emit_pattern" for the given pattern: none, subsurface, ice, caustic,
 * banded or granular.
 */
std::string_view lightPatternName(LightPattern pattern);

/**
 * Parses an "emit_pattern" value without regard to case. Returns nothing for an unknown
 * name, which the engine reads as none.
 */
std::optional<LightPattern> parseLightPatternName(std::string_view name);

/**
 * The value of "emit_shape" for the given light type: omni, spot or sun.
 */
std::string_view emitterShapeName(LightType type);

/**
 * Parses an "emit_shape" value without regard to case. Returns nothing for an unknown
 * name, which is read as if the key were absent.
 */
std::optional<LightType> parseEmitterShapeName(std::string_view name);

/**
 * The number of emitter effects: 0 is none, 1 to 11 are the engine's predefined light
 * styles 1 to 11, and 12, 13 and 14 are strobe, pulse and flicker.
 */
inline constexpr auto EmitterEffectCount = 15;

/**
 * The value of "emit_effect" for the given effect (none, flicker_a, ..., flicker), and an
 * empty name for a number out of range.
 */
std::string_view emitterEffectName(int effect);

/**
 * Parses an "emit_effect" value without regard to case. Returns nothing for an unknown
 * name, which the engine reads as none.
 */
std::optional<int> parseEmitterEffectName(std::string_view name);

/**
 * The name the editors show for the given effect: "None", the engine's
 * predef_lightstylesinfo for 1 to 11, then "Strobe", "Pulse" and "Flicker"; an empty name
 * for a number out of range.
 */
std::string_view emitterEffectDisplayName(int effect);

// the limits that every edit of an emitter keeps
inline constexpr auto MinEmitterRange = 24.0;
inline constexpr auto MinEmitterCone = 1.0;
inline constexpr auto MaxEmitterCone = 120.0;
inline constexpr auto MinEmitterSquareness = 1.5;
inline constexpr auto MaxEmitterSquareness = 32.0;

/**
 * The rotation of a new emitter spot as id Tech 4 "angles" (pitch, yaw, roll in degrees):
 * a pitch of 90 points its axis[0] straight down.
 */
inline const auto EmitterSpotPitchYawRoll = vm::vec3d{90, 0, 0};

/**
 * The falloff curve and beam mask inputs that lightFalloff and spotMask read, the
 * shader's falloff curve and spot mask (emitter.inc.hlsl): an emitter's are emitterCurve.
 */
struct LightCurve
{
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
    LightCurve,
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
 * The emitter block of a light (the engine's emitterData_t) with the defaults of its
 * keys. It has no shape: "emit_shape" is the light's type. A disabled block keeps its
 * values, as the entity keeps its keys. While it is enabled it wins over the target keys.
 */
struct EmitterProperties
{
  // the doubles first, then the enumerations, then the flags: no padding between them
  // (RV2-TP-05), so every light's record carries 112 bytes of block, not 128

  // world units
  double size = 96.0;
  double start = 0.0;
  // full angles in degrees; coneH pairs with axis[1], coneV with axis[2]
  double coneH = 60.0;
  double coneV = 60.0;
  double penumbra = 0.2;
  double squareness = 2.0;
  double patternStrength = 0.0;
  double patternScale = 1.0;
  double patternPhase = 0.0;
  double effectSpeed = 1.0;
  double effectAmount = 1.0;
  double effectPhase = 0.0;
  LightFalloff falloff = LightFalloff::InvSquareClamped;
  LightPattern pattern = LightPattern::None;
  // 0 to EmitterEffectCount - 1, see emitterEffectName
  int effect = 0;
  bool enabled = false;
  // the light carries an emitter block: "emitter" or any "emit_*" key; only such a light
  // is written with emitter "0" while the block is disabled
  bool hasBlock = false;
  // editor only: the range follows intensity, size, start and falloff; read as false for
  // an omni or a spot whose range was set by hand (parseLightProperties)
  bool autoRange = true;
  bool energyClamp = true;

  kdl_reflect_decl(
    EmitterProperties,
    enabled,
    hasBlock,
    autoRange,
    falloff,
    size,
    start,
    energyClamp,
    coneH,
    coneV,
    penumbra,
    squareness,
    pattern,
    patternStrength,
    patternScale,
    patternPhase,
    effect,
    effectSpeed,
    effectAmount,
    effectPhase);
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
  // some, but not all, of light_target, light_right and light_up are present; the engine
  // prints "bad target info" and this light is read as a point light
  bool badTargetInfo = false;
  // While enabled: type is the emitter's shape, radius its reach (a spot's three
  // components are its range), and the target vectors stay 0.
  EmitterProperties emitter;

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
    badTargetInfo,
    emitter);
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
 * and light_up, and some but not all of them is bad target info, read as a point light;
 * "parallel" makes a sun even with all three; light "0" is a radius of 0). Vectors are
 * read like the engine's sscanf reads them, so a value with one number reads (v, 0, 0).
 *
 * The emitter keys are read whether "emitter" is set or not; it decides only whether the
 * block is enabled. With "emitter" set, the light is an emitter: its type is "emit_shape"
 * (absent or unknown: a sun with parallel 1, else an omni), the target keys and "light"
 * are not read, light_radius defaults to 300 300 300 with each component at least 1, a
 * spot's radius is its range three times and has no centre. The emitter keys are looked
 * up only when the entity has a key that starts with "emit"; without one the block has
 * its defaults.
 *
 * A hand-set range wins (G0 section 5): an enabled emitter omni or spot whose
 * emit_auto_range is 1 or absent and whose curve has a reach factor, but whose range is
 * not in step with its auto range (emitterRangeInStep), is read with autoRange false, so
 * that no edit moves its range and its next write writes emit_auto_range "0". A sun and
 * a curve without a reach factor keep the stored flag. See emitterRangeSetByHand.
 */
LightProperties parseLightProperties(const Entity& entity);

/**
 * The test of "a hand-set range wins" (G0 section 5): whether the given light is an
 * enabled emitter omni or spot with autoRange on and a curve with a reach factor whose
 * range is out of step with its auto range. Such a light is taken with autoRange off;
 * the test is made where an editor reads a light (parseLightProperties) and where it
 * brings a kept emitter block back (Texture to Emitter on a light that was an emitter).
 */
bool emitterRangeSetByHand(const LightProperties& light);

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
 * and the emitter floats with four, all without trailing zeros, and every vector with
 * three components. The rotation properties are never touched; a light with light_origin
 * gets neither origin nor light_origin.
 *
 * An enabled emitter writes "emitter" "1" and every "emit_*" key, defaults included;
 * light_radius (a spot's range three times); light_center and parallel like a point
 * light or a sun; and deletes the five target vectors. A disabled emitter block that the
 * light carries (hasBlock) is written as emitter "0" alone and none of its keys is
 * deleted; a light without one gets no emitter key at all. A texture spot that carries an
 * emitter block and whose target is 0, an emitter spot whose flag alone was cleared, is
 * written as textureSpotFromEmitterSpot converts it, so that no texture spot is written
 * with a target of 0; asTextureLight is the conversion to call.
 */
LightPropertyUpdate lightPropertyUpdate(const LightProperties& light);

/**
 * Returns only the changes that turn the given entity into the given edited light: the
 * properties whose written value differs from the value written for the entity as it is
 * and from the entity's own value, and the properties to remove that the entity has and
 * that the edit does not leave unread. The changes are checked by reading the changed
 * entity again, so that a property the entity lacks but whose engine default differs from
 * the edited value is written too.
 *
 * Enabling a kept emitter block writes only the keys it lacks and what the edit changes;
 * switching an emitter to a texture light writes emitter "0" and keeps every "emit_*"
 * key. An edit of an emitter that changes anything also writes the "emit_*"
 * keys the entity lacks, so that the block describes itself, and emit_auto_range where
 * the entity's own value differs from the edited light's: a light whose range was set by
 * hand is read with auto range off (parseLightProperties), and its first write writes
 * emit_auto_range "0" over a stored "1".
 */
LightPropertyUpdate lightKeyDiff(const Entity& entity, const LightProperties& edited);

/**
 * Returns a copy of the given entity with the given changes applied.
 */
Entity applyLightPropertyUpdate(Entity entity, const LightPropertyUpdate& update);

/**
 * Returns the engine's default light of the given type (LightInfo::DefaultPoint,
 * DefaultProjected and DefaultSun), which keeps the colour, the origin, light_origin and
 * the emitter block of the given light and resets everything else.
 *
 * For an enabled emitter, a spot has no target vectors and the cone, penumbra and
 * squareness of a new spot; an omni and a spot then take their range from
 * applyEmitterAutoRange(EmitterEdit::Reach), from 300 when the curve has no reach; a sun
 * is today's sun. The caller writes a spot's rotation, EmitterSpotPitchYawRoll.
 */
LightProperties defaultLight(LightType type, const LightProperties& keep);

/**
 * Whether the given entity has an emitter block of its own: any of the emitter keys other
 * than "emitter". It is not the "carries a block" test of the emitter "0" rule; that test
 * is EmitterProperties::hasBlock, which also counts the "emitter" key.
 */
bool hasEmitterBlock(const Entity& entity);

/**
 * Whether the given entity has a key that starts with "emit", found in one pass over its
 * properties that stops at the first: every emitter key does. parseLightProperties reads
 * the emitter keys only when the entity has one; without one the block keeps its
 * defaults, which are what reading its absent keys gives.
 */
bool hasEmitPrefixedKey(const Entity& entity);

/**
 * An enabled emitter block with the defaults of its keys: the block of a new emitter.
 */
EmitterProperties seededEmitterProperties();

/**
 * defaultLight(type, keep) with a new emitter block (seededEmitterProperties): an omni or
 * a spot whose range follows from auto range (288 at intensity 1), or today's sun. A spot
 * also needs the rotation EmitterSpotPitchYawRoll, which the caller writes. The block's
 * effect is emitterEffectForStyle of the kept light's style, and the style is kept.
 */
LightProperties seededEmitterLight(LightType type, const LightProperties& keep);

/**
 * The light that the type radio makes of the given light (G0 section 9, shape switch):
 * defaultLight(type, before), and for an enabled emitter that becomes an omni or a spot:
 * with auto range in force on the given light (emitterAutoRangeInForce) the range is the
 * auto range, as defaultLight gives it; otherwise the range is kept, the new shape's
 * range being the old one's largest radius (an omni gets it on every axis), at least 24,
 * so that a linear or smooth light or a hand-set range does not jump. From a sun, which
 * has no range: the auto range when the flag is on and the curve has a reach factor,
 * else 300. The start then stays below the range. An emitter keeps noshadows and
 * nospecular (castShadows, skipSpecular); a texture light resets them, as today.
 * The rotation is the caller's to write: unchanged, except that a light without a
 * rotation that becomes a spot points down (EmitterSpotPitchYawRoll).
 */
LightProperties emitterShapeSwitch(LightType type, const LightProperties& before);

/**
 * The effect that a light's first emitter block (the light has no "emit_effect" key yet)
 * takes from the light's texture-light style (G0 section 9, kind switch): style 1 to 11
 * seeds the effect of the same number, the same predef_lightstyles entry; style 0, -1 or
 * any other number seeds none. The style key stays on the light.
 */
int emitterEffectForStyle(int style);

/**
 * G0 section 5, the range floor on an omni: an enabled emitter omni whose largest radius
 * is below 24 is scaled as a whole so that its largest component is 24, so 10 5 20
 * becomes 12 6 24. A sun, a spot and a texture light are returned unchanged. The light
 * editor's radius fields apply it; the volume tool clamps instead (emitterFromEditBox).
 */
LightProperties applyEmitterOmniRangeFloor(const LightProperties& light);

/**
 * The range of an emitter, EM_Range: the largest component of light_radius.
 */
double emitterRange(const vm::vec3d& radius);

/**
 * The intensity of a light: the largest component of _color.
 */
double lightIntensity(const LightProperties& light);

/**
 * The intensity of a light colour: its largest component.
 */
double lightIntensity(const vm::vec3d& color);

/**
 * An emitter's start within its range (G0 section 5, 0 <= start <= range - 1): the given
 * start clamped to [0, max(range - 1, 0)].
 */
double clampEmitterStart(double start, double range);

/**
 * tan(cone / 2) of a full cone angle in degrees, after clamping it to 1 to 120.
 */
double emitterTanHalf(double coneDegrees);

/**
 * The full cone angle in degrees whose half angle has the given tangent, clamped to 1 to
 * 120.
 */
double emitterConeFromTanHalf(double tanHalf);

/**
 * sin(cone / 2) of a full cone angle in degrees, after clamping it to 1 to 120: the half
 * width of a spot's lit cone where it is widest, on the sphere of its range, over the
 * range (G0 section 9, the volume tool's edit box).
 */
double emitterSinHalf(double coneDegrees);

/**
 * The full cone angle in degrees whose half angle has the given sine, 2 asin(sinHalf),
 * clamped to 1 to 120; a sine above 1 counts as 1.
 */
double emitterConeFromSinHalf(double sinHalf);

/**
 * EM_ReachFactor: how many sizes past its start the given curve reaches at the given
 * intensity; 0 for linear and smooth, which have no auto range.
 */
double emitterReachFactor(LightFalloff falloff, double intensity);

/**
 * Whether the given curve has a reach factor, and so an auto range: every curve but
 * linear and smooth. emitterReachFactor is above 0 for it at any intensity.
 */
bool emitterCurveHasReach(LightFalloff falloff);

/**
 * EM_AutoRange: max(24, start + size * reach factor), or 0 for a curve without one.
 */
double emitterAutoRange(const EmitterProperties& emitter, double intensity);

/**
 * EM_AutoSize: max(1, (range - start) / reach factor), or the emitter's size for a curve
 * without one.
 */
double emitterAutoSize(const EmitterProperties& emitter, double intensity, double range);

/**
 * EM_RangeInStep (G0 section 3), the last condition of "auto range in force": false for a
 * curve without a reach factor, else whether the given range is within max(1, 5 %) of
 * emitterAutoRange. It looks at neither autoRange nor the shape.
 */
bool emitterRangeInStep(const EmitterProperties& emitter, double intensity, double range);

/**
 * Whether auto range is in force on the given light (G0 section 5): an enabled emitter
 * omni or spot with autoRange on, a curve with a reach factor at its intensity, and its
 * range in step with its auto range.
 */
bool emitterAutoRangeInForce(const LightProperties& light);

enum class EmitterEdit
{
  // intensity, size, start or falloff changed: the range follows
  Reach,
  // the range changed: the size follows
  Range,
};

/**
 * Applies the auto range rule after an edit of the given kind and returns the light.
 * Nothing changes unless the light is an enabled emitter with auto range on, an omni or a
 * spot, and a curve with a reach factor at its intensity. After a Reach edit the range is
 * emitterAutoRange: a spot gets it three times, an omni with an equal radius likewise,
 * and an omni with unequal radii is scaled as a whole. After a Range edit the size is
 * emitterAutoSize for the light's range. Either way the start is clamped to range - 1.
 *
 * A Range edit of an enabled emitter omni or spot whose curve has no reach factor
 * (linear, smooth) turns autoRange off: the mapper has taken the range in hand (G0
 * section 5). Every other light and edit leaves the flag.
 *
 * It does not test whether the range is in step: the edit it follows has just moved the
 * light out of step. That test is made where an editor reads a light
 * (parseLightProperties) and where it brings a kept emitter block back
 * (emitterRangeSetByHand); a light read with its range out of step has autoRange false.
 */
LightProperties applyEmitterAutoRange(const LightProperties& light, EmitterEdit edit);

/**
 * Sets the falloff of an emitter and follows it as a reach edit (G0 section 5, a falloff
 * edit that gives the curve a reach factor applies the flag as it stands): with autoRange
 * on the range becomes the auto range of the new curve, with it off the range is kept;
 * to a curve without a reach factor the range is kept and the flag stays as it is. So
 * trying each curve and coming back changes nothing. A light that is not an enabled
 * emitter gets the falloff and nothing else.
 */
LightProperties applyEmitterFalloff(const LightProperties& light, LightFalloff falloff);

/**
 * Whether the light takes the emitter path: its emitter block is enabled. Code decides
 * the path through this, never by reading the flag.
 */
bool lightUsesEmitter(const LightProperties& light);

} // namespace tb::mdl
