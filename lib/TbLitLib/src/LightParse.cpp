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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/d3xp/Light.cpp and renderer/MathLight.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/LightParse.h"

#include "lit/EngineLight.h"
#include "lit/EntityState.h"

#include <fmt/format.h>

#include <algorithm>
#include <string_view>

namespace tb::lit
{
namespace
{

// the most light_style%d keys num_styles may ask for; the engine has no bound (a guard,
// mirror.json M-LIGHT-GUARD-01)
constexpr auto MaxLightStyles = 1024;

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  // idStr::Icmp == 0: ASCII letters compared without case
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             const auto lower = [](const char c) {
               return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
             };
             return lower(a) == lower(b);
           });
}

// idMath::ClampInt( min, max, value )
int clampInt(const int min, const int max, const int value)
{
  return value < min ? min : (value > max ? max : value);
}

// idMath::ClampFloat( min, max, value ) = Max( min, Min( max, value ) ), which keeps a
// NaN
float clampFloat(const float min, const float max, const float value)
{
  const auto inner = max < value ? max : value;
  return min > inner ? min : inner;
}

// MathLight.cpp:45-130 ML_ParseFromDict; m->type holds the type the caller seeded
void parseMathLight(
  const EntityKeys& args, MathLightParms& m, std::vector<std::string>& warnings)
{
  const auto seededType = clampInt(0, 2, m.type);

  // Clear first so toggling math_enabled off leaves no stale state
  m = MathLightParms{};
  m.style = -1;

  m.enabled = spawnBool(args, "math_enabled", "0");
  if (!m.enabled)
  {
    return;
  }

  const auto typeStr = spawnString(args, "math_type", "");
  if (equalsNoCase(typeStr, "spot"))
  {
    m.type = int(MathLightType::Spot);
  }
  else if (equalsNoCase(typeStr, "sun") || equalsNoCase(typeStr, "parallel"))
  {
    m.type = int(MathLightType::Sun);
  }
  else if (equalsNoCase(typeStr, "point"))
  {
    m.type = int(MathLightType::Point);
  }
  else
  {
    if (!typeStr.empty())
    {
      // idLib::Warning
      warnings.push_back(fmt::format(
        "math_type '{}' not recognized; falling back to inferred light type", typeStr));
    }
    m.type = seededType;
  }

  const auto falloffStr = spawnString(args, "math_falloff", "invsquare_clamped");
  if (equalsNoCase(falloffStr, "linear"))
  {
    m.falloff = 1;
  }
  else if (equalsNoCase(falloffStr, "smooth"))
  {
    m.falloff = 2;
  }
  else if (equalsNoCase(falloffStr, "exponential"))
  {
    m.falloff = 3;
  }
  else if (equalsNoCase(falloffStr, "invsquare"))
  {
    m.falloff = 0;
  }
  else
  {
    m.falloff = 4;
  }

  spawnFloat(args, "math_falloff_exp", "2.0", m.falloffExp);
  spawnFloat(args, "math_inner_radius", "0.0", m.innerRadius);
  spawnFloat(args, "math_shape_exp", "2.0", m.shapeExp);
  spawnFloat(args, "math_penumbra", "0.15", m.penumbra);

  m.shapeExp = clampFloat(1.5f, 32.0f, m.shapeExp);
  m.penumbra = clampFloat(0.0f, 1.0f, m.penumbra);
  m.falloffExp = clampFloat(0.1f, 16.0f, m.falloffExp);
  m.innerRadius = clampFloat(0.0f, 1.0f, m.innerRadius);

  const auto patternStr = spawnString(args, "math_pattern", "none");
  if (equalsNoCase(patternStr, "subsurface"))
  {
    m.pattern = 1;
  }
  else if (equalsNoCase(patternStr, "ice"))
  {
    m.pattern = 2;
  }
  else if (equalsNoCase(patternStr, "caustic"))
  {
    m.pattern = 3;
  }
  else if (equalsNoCase(patternStr, "banded"))
  {
    m.pattern = 4;
  }
  else if (equalsNoCase(patternStr, "granular"))
  {
    m.pattern = 5;
  }
  else
  {
    m.pattern = 0;
  }

  spawnFloat(args, "math_pattern_strength", "0.0", m.patternStrength);
  spawnFloat(args, "math_pattern_scale", "1.0", m.patternScale);
  spawnFloat(args, "math_pattern_phase", "0.0", m.patternPhase);

  m.patternStrength = clampFloat(0.0f, 1.0f, m.patternStrength);

  m.energyClamp = spawnBool(args, "math_energy_clamp", "1");

  // an index outside the table is no style
  const auto style = spawnInt(args, "math_style", "-1");
  m.style = (style >= 0 && style < MathLightStyleCount) ? style : -1;
}

// Light.cpp:131-174: the rotation matrix in full form, or in angle form
EngineMat3 parseLightAxis(const EntityKeys& args)
{
  auto mat = EngineMat3{};
  if (!spawnMatrix(args, "light_rotation", "1 0 0 0 1 0 0 0 1", mat))
  {
    if (!spawnMatrix(args, "rotation", "1 0 0 0 1 0 0 0 1", mat))
    {
      auto angles = std::array<float, 3>{};
      const auto normalised = [&]() {
        return engineAnglesToMat3(
          engineAngleNormalize360(angles[0]),
          engineAngleNormalize360(angles[1]),
          engineAngleNormalize360(angles[2]));
      };
      // light_angles: lights modified by the editLights command
      if (spawnAngles(args, "light_angles", "0 0 0", angles))
      {
        mat = normalised();
      }
      // "angles" like in Quake 3 (TrenchBroom interop)
      else if (spawnAngles(args, "angles", "0 0 0", angles))
      {
        mat = normalised();
      }
      else
      {
        spawnFloat(args, "angle", "0", angles[1]);
        angles[0] = 0.0f;
        angles[1] = engineAngleNormalize360(angles[1]);
        angles[2] = 0.0f;
        mat = engineAnglesToMat3(angles[0], angles[1], angles[2]);
      }
    }
  }

  // fix degenerate identity matrices, row by row
  for (auto row = std::size_t{0}; row < 3; ++row)
  {
    const auto fixed =
      engineFixDegenerateNormal({mat[row * 3], mat[row * 3 + 1], mat[row * 3 + 2]});
    mat[row * 3] = fixed[0];
    mat[row * 3 + 1] = fixed[1];
    mat[row * 3 + 2] = fixed[2];
  }
  return mat;
}

// Light.cpp:114-218: the rest of ParseSpawnArgsToRenderLight, once the target info is
// good
void parseGoodTarget(
  const EntityKeys& args,
  const bool gotTarget,
  RenderLightParms& parms,
  std::vector<std::string>& warnings)
{
  if (!gotTarget)
  {
    parms.pointLight = true;

    // an optional relative center of light and shadow offset
    spawnVector(args, "light_center", "0 0 0", parms.lightCenter);

    if (!spawnVector(args, "light_radius", "300 300 300", parms.lightRadius))
    {
      auto radius = 0.0f;
      spawnFloat(args, "light", "300", radius);
      parms.lightRadius = {radius, radius, radius};
    }
  }

  parms.axis = parseLightAxis(args);

  auto color = EngineVec3{};
  spawnVector(args, "_color", "1 1 1", color);
  parms.shaderParms[0] = color[0];
  parms.shaderParms[1] = color[1];
  parms.shaderParms[2] = color[2];
  spawnFloat(args, "shaderParm3", "1", parms.shaderParms[3]);
  if (!spawnFloat(args, "shaderParm4", "0", parms.shaderParms[4]))
  {
    // -MS2SEC( gameLocal.time ) with the time frozen at 0 (K5): 0 * 0.001f, negated
    parms.shaderParms[4] = -0.0f;
  }
  spawnFloat(args, "shaderParm5", "0", parms.shaderParms[5]);
  spawnFloat(args, "shaderParm6", "0", parms.shaderParms[6]);
  spawnFloat(args, "shaderParm7", "0", parms.shaderParms[7]);
  parms.noShadows = spawnBool(args, "noshadows", "0");
  parms.noSpecular = spawnBool(args, "nospecular", "0");
  parms.parallel = spawnBool(args, "parallel", "0");

  // FindMaterial( texture, false ), which may return NULL (LightData.h resolves it)
  parms.texture = std::string{spawnString(args, "texture", "lights/squarelight1")};

  // the MATH LIGHT extension: the seed, the parse, then the type the geometry forces
  // (D45: projected first)
  if (parms.parallel)
  {
    parms.mathLight.type = int(MathLightType::Sun);
  }
  else if (!parms.pointLight)
  {
    parms.mathLight.type = int(MathLightType::Spot);
  }
  else
  {
    parms.mathLight.type = int(MathLightType::Point);
  }
  parseMathLight(args, parms.mathLight, warnings);
  if (!parms.pointLight)
  {
    parms.mathLight.type = int(MathLightType::Spot);
  }
  else
  {
    parms.mathLight.type =
      int(parms.parallel ? MathLightType::Sun : MathLightType::Point);
  }
}

// Light.cpp:464-628 idLight::Spawn after the parse, and :1113-1220 SharedThink at time 0
void applySpawn(const EntityKeys& args, ParsedLight& light)
{
  auto& parms = light.parms;
  auto& spawn = light.spawn;

  // the original radius for the light style; the base colour for SetLightLevel and the
  // math style
  spawn.baseRadius = parms.lightRadius;
  spawn.baseColor = {parms.shaderParms[0], parms.shaderParms[1], parms.shaderParms[2]};
  auto mathStyleBase = spawn.baseColor;

  spawn.levels = spawnInt(args, "levels", "1");
  if (spawn.levels <= 0)
  {
    // gameLocal.Error: the game stops the map here; the preview keeps the light
    // (M-LIGHT-DIFF-01)
    light.warnings.push_back(fmt::format(
      "Invalid light level set on the light (levels {}): the game stops the map here",
      spawn.levels));
  }

  spawn.startOff = spawnBool(args, "start_off", "0");
  if (spawn.startOff)
  {
    // Off: currentLevel = 0, then SetLightLevel: intensity = currentLevel / levels; with
    // levels 0 the engine's 0 / 0 is guarded to 0 (M-LIGHT-GUARD-02)
    const auto intensity = spawn.levels != 0 ? float(0) / float(spawn.levels) : 0.0f;
    const auto color = EngineVec3{
      spawn.baseColor[0] * intensity,
      spawn.baseColor[1] * intensity,
      spawn.baseColor[2] * intensity};
    parms.shaderParms[0] = color[0];
    parms.shaderParms[1] = color[1];
    parms.shaderParms[2] = color[2];
    mathStyleBase = color;
  }

  // the breakable light, the demonic shader and the modelTarget link are the game's
  for (const auto* key :
       {"mat_demonic", "health", "broken", "break", "count", "modelTarget"})
  {
    if (const auto* value = findKey(args, key))
    {
      light.ignoredKeys.emplace_back(key, *value);
    }
  }

  spawn.styleFrameTime = spawnInt(args, "ls_frametime", "100");
  spawn.style = spawnIntOr(args, "style", -1);

  const auto numStyles = spawnInt(args, "num_styles", "0");
  if (numStyles > 0)
  {
    if (numStyles > MaxLightStyles)
    {
      light.warnings.push_back(fmt::format(
        "num_styles {} is more than the preview reads; it reads {}",
        numStyles,
        MaxLightStyles));
    }
    for (auto i = 0; i < std::min(numStyles, MaxLightStyles); ++i)
    {
      spawn.styles.emplace_back(spawnString(args, fmt::format("light_style{}", i), ""));
    }
  }
  else
  {
    // not defined in entityDef light, so the predefined Quake 1 table
    const auto defaults = engineDefaultLightStyles();
    for (auto i = std::size_t{0}; i < defaults.size(); ++i)
    {
      spawn.styles.emplace_back(
        spawnString(args, fmt::format("light_style{}", i), defaults[i]));
    }
  }

  // SharedThink at time 0 (K5): the math style first, on the colour
  const auto mathStyle = parms.mathLight.style;
  if (mathStyle >= 0 && std::size_t(mathStyle) < spawn.styles.size())
  {
    const auto brightness = mathLightStyleMultiplier(
      spawn.styles[std::size_t(mathStyle)], 0, spawn.styleFrameTime);
    spawn.mathStyleBrightness = brightness;
    parms.shaderParms[0] = mathStyleBase[0] * brightness;
    parms.shaderParms[1] = mathStyleBase[1] * brightness;
    parms.shaderParms[2] = mathStyleBase[2] * brightness;
  }

  // then the radius style: at time 0 the first frame, without interpolation, so lightval
  // is the first character's value; ls_frametime 0 makes the engine's offset 0 / 0, which
  // the preview does not divide (M-LIGHT-GUARD-02)
  if (spawn.style >= 0 && std::size_t(spawn.style) < spawn.styles.size())
  {
    if (spawn.styleFrameTime == 0)
    {
      light.warnings.push_back(
        "ls_frametime 0: the engine's light style divides by zero; the preview takes the "
        "style's first character");
    }
    const auto& style = spawn.styles[std::size_t(spawn.style)];
    // idStr::operator[]( 0 ) of an empty string is its terminator
    const auto lastch = int(style.empty() ? '\0' : style[0]) - 'a';
    auto lightval = float(lastch);
    lightval *= 0.071429f;
    lightval = 0.0f > lightval ? 0.0f : lightval;   // Max( 0.0f, lightval )
    lightval = 20.0f < lightval ? 20.0f : lightval; // Min( 20.0f, lightval )
    spawn.radiusStyle = lightval;
    parms.lightRadius = {
      lightval * spawn.baseRadius[0],
      lightval * spawn.baseRadius[1],
      lightval * spawn.baseRadius[2]};
  }
}

} // namespace

ParsedLight parseLight(const EntityKeys& args)
{
  auto light = ParsedLight{};
  auto& parms = light.parms;

  // Light.cpp:92-104
  if (!spawnVector(args, "light_origin", "", parms.origin))
  {
    spawnVector(args, "origin", "", parms.origin);
  }

  const auto gotTarget = spawnVector(args, "light_target", "", parms.target);
  const auto gotUp = spawnVector(args, "light_up", "", parms.up);
  const auto gotRight = spawnVector(args, "light_right", "", parms.right);
  spawnVector(args, "light_start", "0 0 0", parms.start);
  if (!spawnVector(args, "light_end", "", parms.end))
  {
    parms.end = parms.target;
  }

  // :106-112: all of the target, right and up or none of them
  if ((gotTarget || gotUp || gotRight) != (gotTarget && gotUp && gotRight))
  {
    // gameLocal.Printf, then the parse returns: the rest stays as the memset left it,
    // the math light's style 0 included
    light.badTarget = true;
    light.warnings.push_back(fmt::format(
      "Light at ({:f},{:f},{:f}) has bad target info",
      parms.origin[0],
      parms.origin[1],
      parms.origin[2]));
  }
  else
  {
    parseGoodTarget(args, gotTarget, parms, light.warnings);
  }

  applySpawn(args, light);
  return light;
}

float mathLightStyleMultiplier(const std::string& style, int timeMs, int frameTimeMs)
{
  // idStr::Length: up to the first terminator
  const auto length = int(std::string_view{style.c_str()}.size());
  if (length <= 0)
  {
    return 1.0f;
  }

  // ls_frametime comes from the map; zero would divide by zero
  frameTimeMs = std::max(frameTimeMs, 1);
  timeMs = std::max(timeMs, 0);

  const auto step = timeMs / frameTimeMs;
  const auto frame = step % length;
  const auto nextFrame = (frame + 1) % length;
  const auto backLerp = float(timeMs - step * frameTimeMs) / float(frameTimeMs);

  const auto lastCh = clampInt(0, 25, style[std::size_t(frame)] - 'a');
  const auto nextCh = clampInt(0, 25, style[std::size_t(nextFrame)] - 'a');

  return (float(lastCh) * (1.0f - backLerp) + float(nextCh) * backLerp) * (1.0f / 12.0f);
}

} // namespace tb::lit
