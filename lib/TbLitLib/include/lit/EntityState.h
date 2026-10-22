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
#include <cstdint>
#include <string>
#include <string_view>

namespace tb::lit
{

// The idDict getters the engine reads spawn args with, on the engine's lookup rule
// (findKey: case-insensitive, the last duplicate wins, A10), parsing as the C library
// does in the "C" locale (sscanf's %f, atof, atoi), whatever the process locale. Pure;
// any thread.

// GetString: the value, or fallback when the key is missing
std::string_view spawnString(
  const EntityKeys& keys, std::string_view key, std::string_view fallback = "");
// GetFloat: atof of the value or of fallback
float spawnFloat(
  const EntityKeys& keys, std::string_view key, std::string_view fallback = "0");
// GetBool: atoi(...) != 0
bool spawnBool(
  const EntityKeys& keys, std::string_view key, std::string_view fallback = "0");
// GetVector: zero, then sscanf "%f %f %f"; whether the key exists
bool spawnVector(
  const EntityKeys& keys,
  std::string_view key,
  std::string_view fallback,
  EngineVec3& out);

/**
 * idDict::Set for each map key (a key given again replaces the value in place, compared
 * without case), then SetDefaults with the entityDef's merged keys, which adds only the
 * missing ones (Game_local.cpp:3909, Dict.cpp:202-220, A10). defKeys may be null: the
 * entity's def is not known (yet). Pure.
 */
EntityKeys mergeSpawnArgs(const EntityKeys& mapKeys, const EntityKeys* defKeys);

/**
 * What the engine's idGameEdit::ParseSpawnArgsToRenderEntity (d3xp/Entity.cpp:232-408,
 * SC51-SC53) makes of an entity's merged spawn args, as far as the preview's chunks need
 * it (S34): the origin, the axis, the shaderParms, the skin and shader remaps and the
 * three flags. Plain data.
 */
struct RenderEntityParms
{
  EngineVec3 origin = {0, 0, 0};
  // scale·rotation, row by row; a rotation key alone (no angles, no scale)
  EngineMat3 axis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<float, MaterialDecls::ShaderParmCount> shaderParms = {};
  std::string skin;
  std::string shader;
  bool noShadows = false;
  bool noSelfShadows = false;
  bool noDynamicInteractions = false;

  bool operator==(const RenderEntityParms& other) const;
  bool operator!=(const RenderEntityParms& other) const;
};

/**
 * The mirror (A7): origin from "origin" ("0 0 0"); the axis from "rotation" alone, else
 * "angles" (all zero is the identity) or "angle" (yaw), times the scale from
 * "modelscale_vec" (each component above 0.01, not all 1) or else "modelscale" (above
 * 0.01, not 1); "_color" fills shaderParms 0-2 ("1 1 1"), "shaderParm3" defaults to 1,
 * "shaderParm4" to "shaderParm11" to 0. Pure; any thread.
 */
RenderEntityParms parseSpawnArgsToRenderEntity(const EntityKeys& spawnArgs);

// a 64-bit hash of every field; equal parms hash equal
std::uint64_t renderEntityHash(const RenderEntityParms& parms);

} // namespace tb::lit
