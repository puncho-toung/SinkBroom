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

#include "lit/EngineGeometry.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

// mathLightType_t (MathLight.h:43-48)
enum class MathLightType
{
  Point = 0,
  Spot = 1,
  Sun = 2,
};

// the number of light styles math_style indexes (MathLight.h ML_NUM_STYLES)
constexpr auto MathLightStyleCount = 12;

/**
 * mathLightData_t (MathLight.h:77-96) as ML_ParseFromDict (MathLight.cpp:45-118) fills
 * it, before ParseSpawnArgsToRenderLight forces the type: everything zero and style -1
 * unless math_enabled; then math_type (or the type the light's geometry seeds), the
 * falloff and pattern names (an unknown name is the default), the clamped floats, the
 * energy clamp and math_style (-1 outside the table). F16, D20: its quirks are kept.
 */
struct MathLightParms
{
  int type = 0;
  int falloff = 0;
  int pattern = 0;
  int style = 0;
  float falloffExp = 0.0f;
  float innerRadius = 0.0f;
  float shapeExp = 0.0f;
  float penumbra = 0.0f;
  float patternStrength = 0.0f;
  float patternScale = 0.0f;
  float patternPhase = 0.0f;
  bool enabled = false;
  bool energyClamp = false;

  bool operator==(const MathLightParms& other) const = default;
};

/**
 * renderLight_t as idGameEdit::ParseSpawnArgsToRenderLight (d3xp/Light.cpp:84-219, SC54
 * to SC56) fills it from a light's merged spawn args (C22, A10), without the pointers:
 * texture is the name it asks FindMaterial( texture, false ) for, which the light record
 * resolves (LightData.h). The memset's zeros are the defaults.
 */
struct RenderLightParms
{
  // rows; zero only when the target info is bad (the parse returns before the axis)
  EngineMat3 axis = {};
  EngineVec3 origin = {0, 0, 0};
  EngineVec3 target = {0, 0, 0};
  EngineVec3 right = {0, 0, 0};
  EngineVec3 up = {0, 0, 0};
  EngineVec3 start = {0, 0, 0};
  EngineVec3 end = {0, 0, 0};
  EngineVec3 lightRadius = {0, 0, 0};
  EngineVec3 lightCenter = {0, 0, 0};
  bool pointLight = false;
  bool parallel = false;
  bool noShadows = false;
  bool noSpecular = false;
  std::array<float, MaterialDecls::ShaderParmCount> shaderParms = {};
  std::string texture;
  MathLightParms mathLight;

  bool operator==(const RenderLightParms& other) const = default;
};

/**
 * What idLight::Spawn (d3xp/Light.cpp:464-628, SC57) adds to the parsed light, with the
 * light's first SharedThink (:1113-1220) at time 0 (K5).
 */
struct LightSpawnParms
{
  // "levels" ("1"); <= 0 is a game Error, here a warning, and the light is kept
  int levels = 1;
  // "start_off": Off() at spawn, so the colour is the base colour times 0
  bool startOff = false;
  // "style" (-1: none) and "ls_frametime" ("100")
  int style = -1;
  int styleFrameTime = 100;
  // light_styles: "light_style%d" for "num_styles" > 0 (default ""), else the twelve of
  // the Quake table, each overridable by its key
  std::vector<std::string> styles;
  // the colour and the radius as parsed, before Off and the styles
  EngineVec3 baseColor = {0, 0, 0};
  EngineVec3 baseRadius = {0, 0, 0};
  // SharedThink at time 0: the radius style's multiplier (the first character of its
  // string), when style names one of styles
  std::optional<float> radiusStyle;
  // ML_StyleMultiplier( styles[ math_style ], 0, ls_frametime ), when math_style names
  // one
  std::optional<float> mathStyleBrightness;

  bool operator==(const LightSpawnParms& other) const = default;
};

/**
 * A light as the game spawns it at time 0: the render light with Spawn's effects applied
 * (the colour after start_off and the math style, the radius after the style),
 * what Spawn read, and the warnings the engine would print.
 */
struct ParsedLight
{
  RenderLightParms parms;
  LightSpawnParms spawn;
  // A15: some but not all of light_target, light_up and light_right: the engine prints
  // "bad target info" and the light keeps only its origin and target vectors; its colour
  // is zero, so it lights nothing, and the record derives nothing from it
  bool badTarget = false;
  // the light's texture key names a material (non-empty, from the map or the entityDef):
  // the game precached it with FindType( DECL_MATERIAL, texture ) before the light
  // spawned (Game_local.cpp CacheDictionaryMedia, DeclEntityDef.cpp:146-151), so
  // FindMaterial( texture, false ) finds it, an implicit default material when it has no
  // decl; only the "lights/squarelight1" default is looked up without that
  bool texturePrecached = false;
  // keys Spawn reads that the preview does not act on, as found (health, broken, break,
  // count, modelTarget, mat_demonic, midnight_override): the breakable light and the
  // modelTarget link are the game's
  std::vector<std::pair<std::string, std::string>> ignoredKeys;
  std::vector<std::string> warnings;

  bool operator==(const ParsedLight& other) const = default;
};

/**
 * ParseSpawnArgsToRenderLight, then idLight::Spawn's effects at time 0, over a light's
 * merged spawn args (mergeSpawnArgs: the map's keys over its entityDef's, looked up with
 * findKey, A10): light_origin else origin; light_target, light_up, light_right (a partial
 * set: badTarget, A15), light_start "0 0 0", light_end else the target; without a target
 * a point light with light_center "0 0 0" and light_radius "300 300 300", else light
 * "300"; the axis from light_rotation, rotation, light_angles, angles or angle (angles
 * normalised to 0-360), each row through FixDegenerateNormal; _color "1 1 1",
 * shaderParm3 "1", shaderParm4 -time (-0 here, K5), shaderParm5-7 "0"; noshadows,
 * nospecular, parallel; texture "lights/squarelight1"; ML_ParseFromDict and the forced
 * math type (not a point light: spot, else parallel ? sun : point). Then levels,
 * start_off, the light styles at time 0, and the ignored keys.
 *
 * Pure: it calls only the facades' pure functions (AnglesToMat3, FixDegenerateNormal,
 * AngleNormalize360, the style table), so any thread, the CPU pool included (C24).
 */
ParsedLight parseLight(const EntityKeys& spawnArgs);

/**
 * ML_StyleMultiplier (MathLight.cpp:200-221): the brightness a light-style string gives
 * at a time; 'a' = 0, 'm' = 1, 'z' = 25/12, interpolated per frame; "" is 1. Pure.
 */
float mathLightStyleMultiplier(const std::string& style, int timeMs, int frameTimeMs);

} // namespace tb::lit
