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
// GPL Source Code and RBDOOM-3-BFG (neo/d3xp/Entity.cpp and idlib/Dict.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/EntityState.h"

#include "lit/NumberText.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <optional>

namespace tb::lit
{
namespace
{

bool isCSpace(const char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// one conversion at pos, in the "C" locale: leading white space, an optional sign; T is
// float for sscanf's %f, double for its %lf and for atof
template <typename T>
std::optional<T> scanNumber(const std::string_view text, std::size_t& pos)
{
  while (pos < text.size() && isCSpace(text[pos]))
  {
    ++pos;
  }
  auto begin = text.data() + pos;
  const auto end = text.data() + text.size();
  if (begin != end && *begin == '+')
  {
    ++begin;
  }
  auto value = T{0};
  const auto* next = parseFloatChars(begin, end, value);
  if (!next)
  {
    return std::nullopt;
  }
  pos = std::size_t(next - text.data());
  return value;
}

// sscanf( text, "%f %f ...", out[0], ... ): the conversions that succeed, in order
void scanFloats(const std::string_view text, float* out, const std::size_t count)
{
  auto pos = std::size_t{0};
  for (auto i = std::size_t{0}; i < count; ++i)
  {
    const auto value = scanNumber<float>(text, pos);
    if (!value)
    {
      return;
    }
    out[i] = *value;
  }
}

// atof: a double, which idDict::GetFloat returns as a float
float cFloat(const std::string_view text)
{
  auto pos = std::size_t{0};
  return float(scanNumber<double>(text, pos).value_or(0.0));
}

// atoi
int cInt(const std::string_view text)
{
  auto pos = std::size_t{0};
  while (pos < text.size() && isCSpace(text[pos]))
  {
    ++pos;
  }
  auto begin = text.data() + pos;
  const auto end = text.data() + text.size();
  if (begin != end && *begin == '+')
  {
    ++begin;
  }
  auto value = 0L;
  std::from_chars(begin, end, value);
  return int(value);
}

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             const auto lower = [](const char c) {
               return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
             };
             return lower(a) == lower(b);
           });
}

std::uint64_t hashBytes(std::uint64_t h, const void* data, const std::size_t size)
{
  // FNV-1a
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (auto i = std::size_t{0}; i < size; ++i)
  {
    h ^= bytes[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

} // namespace

std::string_view spawnString(
  const EntityKeys& keys, const std::string_view key, const std::string_view fallback)
{
  const auto* value = findKey(keys, key);
  return value ? std::string_view{*value} : fallback;
}

float spawnFloat(
  const EntityKeys& keys, const std::string_view key, const std::string_view fallback)
{
  return cFloat(spawnString(keys, key, fallback));
}

bool spawnBool(
  const EntityKeys& keys, const std::string_view key, const std::string_view fallback)
{
  return cInt(spawnString(keys, key, fallback)) != 0;
}

bool spawnVector(
  const EntityKeys& keys,
  const std::string_view key,
  const std::string_view fallback,
  EngineVec3& out)
{
  // Dict.cpp GetVector: found = GetString( key, defaultString, &s ); out.Zero(); sscanf
  const auto* value = findKey(keys, key);
  out = {0, 0, 0};
  scanFloats(value ? std::string_view{*value} : fallback, out.data(), 3);
  return value != nullptr;
}

bool spawnFloat(
  const EntityKeys& keys,
  const std::string_view key,
  const std::string_view fallback,
  float& out)
{
  // Dict.cpp GetFloat( key, defaultString, out ): out = atof( s ); return found
  const auto* value = findKey(keys, key);
  out = cFloat(value ? std::string_view{*value} : fallback);
  return value != nullptr;
}

int spawnInt(
  const EntityKeys& keys, const std::string_view key, const std::string_view fallback)
{
  return cInt(spawnString(keys, key, fallback));
}

int spawnIntOr(const EntityKeys& keys, const std::string_view key, const int fallback)
{
  // Dict.cpp GetInt( key, defaultInt, out ): FindKey, atoi of the value or the default
  const auto* value = findKey(keys, key);
  return value ? cInt(*value) : fallback;
}

bool spawnMatrix(
  const EntityKeys& keys,
  const std::string_view key,
  const std::string_view fallback,
  EngineMat3& out)
{
  // Dict.cpp GetMatrix: out.Identity(); sscanf nine floats into out[0].x ... out[2].z
  const auto* value = findKey(keys, key);
  out = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  scanFloats(value ? std::string_view{*value} : fallback, out.data(), 9);
  return value != nullptr;
}

bool spawnAngles(
  const EntityKeys& keys,
  const std::string_view key,
  const std::string_view fallback,
  std::array<float, 3>& out)
{
  // Dict.cpp GetAngles: out.Zero(); sscanf( s, "%f %f %f", pitch, yaw, roll )
  const auto* value = findKey(keys, key);
  out = {0, 0, 0};
  scanFloats(value ? std::string_view{*value} : fallback, out.data(), 3);
  return value != nullptr;
}

EngineVec3 mapOrigin(const EntityKeys& mapKeys)
{
  // MapFile.cpp:1172-1180: v1 = v2 = v3 = 0; sscanf( value, "%lf %lf %lf", ... ), each
  // assigned to a float
  auto origin = EngineVec3{0, 0, 0};
  if (const auto* value = findKey(mapKeys, "origin"))
  {
    auto pos = std::size_t{0};
    for (auto& component : origin)
    {
      const auto v = scanNumber<double>(*value, pos);
      if (!v)
      {
        break;
      }
      component = float(*v);
    }
  }
  return origin;
}

EntityKeys mergeSpawnArgs(const EntityKeys& mapKeys, const EntityKeys* defKeys)
{
  auto merged = EntityKeys{};
  merged.reserve(mapKeys.size() + (defKeys ? defKeys->size() : 0));
  const auto find =
    [&](const std::string_view key) -> std::pair<std::string, std::string>* {
    for (auto& keyValue : merged)
    {
      if (equalsNoCase(keyValue.first, key))
      {
        return &keyValue;
      }
    }
    return nullptr;
  };
  // idMapEntity::Parse: epairs.Set, which replaces a value in place
  for (const auto& [key, value] : mapKeys)
  {
    if (auto* existing = find(key))
    {
      existing->second = value;
    }
    else
    {
      merged.emplace_back(key, value);
    }
  }
  // SpawnEntityDef: spawnArgs.SetDefaults( &def->dict ) adds only the missing keys
  if (defKeys)
  {
    for (const auto& [key, value] : *defKeys)
    {
      if (!find(key))
      {
        merged.emplace_back(key, value);
      }
    }
  }
  return merged;
}

bool RenderEntityParms::operator==(const RenderEntityParms& other) const
{
  return origin == other.origin && axis == other.axis && shaderParms == other.shaderParms
         && skin == other.skin && shader == other.shader && noShadows == other.noShadows
         && noSelfShadows == other.noSelfShadows
         && noDynamicInteractions == other.noDynamicInteractions;
}

bool RenderEntityParms::operator!=(const RenderEntityParms& other) const
{
  return !(*this == other);
}

std::vector<std::string> removedClassnames(const EntityKeys& worldspawnKeys)
{
  // MapFile.cpp:1759-1765: MatchPrefix( "removeEntities" ) on worldspawn's epairs
  // (idStr::Icmpn), a key given again replacing the value in place
  constexpr auto prefix = std::string_view{"removeEntities"};
  auto classnames = std::vector<std::string>{};
  for (const auto& [key, value] : mergeSpawnArgs(worldspawnKeys, nullptr))
  {
    if (
      key.size() >= prefix.size()
      && equalsNoCase(std::string_view{key}.substr(0, prefix.size()), prefix))
    {
      classnames.push_back(value);
    }
  }
  return classnames;
}

bool isRemovedClassname(
  const std::vector<std::string>& removed, const std::string_view classname)
{
  // MapFile.cpp:2270-2282 RemoveEntities: idStr::Icmp of the classnames
  return std::any_of(removed.begin(), removed.end(), [&](const std::string& c) {
    return equalsNoCase(c, classname);
  });
}

RenderEntityParms parseSpawnArgsToRenderEntity(const EntityKeys& args)
{
  auto parms = RenderEntityParms{};

  // Entity.cpp:301-315: skin, shader (the model def's default skin is LP7's)
  parms.skin = std::string{spawnString(args, "skin")};
  parms.shader = std::string{spawnString(args, "shader")};

  // :317
  spawnVector(args, "origin", "0 0 0", parms.origin);

  // :320-371: the rotation matrix in full form, else angles or angle, and the scale
  if (const auto* rotation = findKey(args, "rotation"))
  {
    // GetMatrix: out.Identity(); sscanf nine floats
    parms.axis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    scanFloats(*rotation, parms.axis.data(), 9);
  }
  else
  {
    auto rotMat = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto scaleMat = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};

    if (const auto* anglesValue = findKey(args, "angles"))
    {
      // GetAngles: pitch, yaw, roll
      auto angles = std::array<float, 3>{0, 0, 0};
      scanFloats(*anglesValue, angles.data(), 3);
      if (angles[0] != 0.0f || angles[1] != 0.0f || angles[2] != 0.0f)
      {
        rotMat = engineAnglesToMat3(angles[0], angles[1], angles[2]);
      }
    }
    else
    {
      const auto angle = spawnFloat(args, "angle");
      if (angle != 0.0f)
      {
        rotMat = engineAnglesToMat3(0.0f, angle, 0.0f);
      }
    }

    auto scaleVec = EngineVec3{};
    if (spawnVector(args, "modelscale_vec", "1 1 1", scaleVec))
    {
      // don't allow very small and negative values
      if (
        (scaleVec[0] != 1.0f || scaleVec[1] != 1.0f || scaleVec[2] != 1.0f)
        && (scaleVec[0] > 0.01f && scaleVec[1] > 0.01f && scaleVec[2] > 0.01f))
      {
        scaleMat[0] = scaleVec[0];
        scaleMat[4] = scaleVec[1];
        scaleMat[8] = scaleVec[2];
      }
    }
    else
    {
      const auto scale = spawnFloat(args, "modelscale", "1");
      if (scale != 1.0f && scale > 0.01f)
      {
        scaleMat[0] = scaleMat[4] = scaleMat[8] = scale;
      }
    }

    parms.axis = engineMat3Multiply(scaleMat, rotMat);
  }

  // :376-391 shader parms
  auto color = EngineVec3{};
  spawnVector(args, "_color", "1 1 1", color);
  parms.shaderParms[0] = color[0];
  parms.shaderParms[1] = color[1];
  parms.shaderParms[2] = color[2];
  parms.shaderParms[3] = spawnFloat(args, "shaderParm3", "1");
  for (auto i = 4; i < MaterialDecls::ShaderParmCount; ++i)
  {
    parms.shaderParms[std::size_t(i)] =
      spawnFloat(args, "shaderParm" + std::to_string(i), "0");
  }

  // :393-397
  parms.noDynamicInteractions = spawnBool(args, "noDynamicInteractions");
  parms.noShadows = spawnBool(args, "noshadows");
  parms.noSelfShadows = spawnBool(args, "noselfshadows");
  return parms;
}

std::uint64_t renderEntityHash(const RenderEntityParms& parms)
{
  auto h = 0xCBF29CE484222325ull;
  h = hashBytes(h, parms.origin.data(), sizeof(parms.origin));
  h = hashBytes(h, parms.axis.data(), sizeof(parms.axis));
  h = hashBytes(h, parms.shaderParms.data(), sizeof(parms.shaderParms));
  h = hashBytes(h, parms.skin.data(), parms.skin.size());
  h = hashBytes(h, "\0", 1);
  h = hashBytes(h, parms.shader.data(), parms.shader.size());
  const auto flags = std::array<bool, 3>{
    parms.noShadows, parms.noSelfShadows, parms.noDynamicInteractions};
  return hashBytes(h, flags.data(), sizeof(flags));
}

} // namespace tb::lit
