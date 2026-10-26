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

#include "lit/EngineDecal.h"
#include "lit/EngineLight.h"
#include "lit/MaterialDecls.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{
struct ProcSurface;

// ----- LP7.3 (C54, D132, D138, SC626-SC637): the dress decals' projectors -----

// the classname of dmap's compile-time decal projectors (dress.h FUNC_DECAL_CLASSNAME)
inline constexpr auto FuncDecalClassname = std::string_view{"func_decal"};

/**
 * P7.18: how far a fragment's vertex may lie outside its projector's projectionBounds:
 * ClipTriangle's ClipInPlace keeps a point up to ON_EPSILON 0.1 behind a plane
 * (DecalProjection.cpp:187, Winding.h:79, Plane.h:44), so the bounds are grown by 0.2
 */
inline constexpr auto DressBoundsGrowth = 0.2f;

/**
 * LP7.3 (K42): how far a vertex of dmap's inlined copy of a model may lie outside the
 * instance's world bounds: MatchVert's weld of 0.01 (output.cpp:127-180)
 */
inline constexpr auto InlineBoundsGrowth = 0.01f;

// Material.h:671-675 idMaterial::IsDiscrete: an entity gui, a gui, a deform, sort
// SS_SUBVIEW or SURF_DISCRETE (DECAL_MACRO sets it, Material.cpp:2934). Pure
bool engineMaterialIsDiscrete(const MaterialEntry& material);

enum class DecalProjectorOutcome
{
  // a projector
  Ready,
  // the material is not in the table yet: asked for, decided once it is
  Pending,
  // ParseDecalProjector's refusals (it warns and bakes nothing): no material key, a
  // material that is not discrete (or that the engine could not parse: its
  // FindMaterial's default, which is not), a size or a depth that is not positive, no
  // projection
  NoMaterial,
  NotDiscrete,
  BadSize,
  BadDepth,
  NoProjection,
};

// one projector, world space
struct DecalProjector
{
  EngineDecalParms parms;
  // the material key as the map spells it, and the decl's canonical name
  // (MaterialEntry::name), which dmap writes as its surfaces' material (GetName)
  std::string materialKey;
  std::string material;
  // the target gave the axis (dress_decal.cpp:218-224)
  bool aimed = false;
};

struct DecalProjectorParse
{
  DecalProjectorOutcome outcome = DecalProjectorOutcome::NoMaterial;
  DecalProjector projector;
  // Pending: the material asked for
  std::string pendingMaterial;
  // the engine's warnings, worded as its common->Warning prints them
  std::vector<std::string> warnings;
};

/**
 * What the parse reads beside the entity's spawn args: the table's answer for a material
 * name (nullopt: not in the table yet; a null entry: the name has none, an engine error)
 * and the raw map keys of the entity a target key names (dmap's FindEntity on its map:
 * null for none).
 */
struct DecalProjectorInputs
{
  std::function<std::optional<const MaterialEntry*>(const std::string&)> material;
  std::function<const EntityKeys*(std::string_view)> target;
};

/**
 * dress_decal.cpp:146-273 ParseDecalProjector, transcribed onto EntityState's getters:
 * spawnArgs are the entity's map keys with its entityDef's under them (mergeSpawnArgs, as
 * its SetDefaults, :152-157); name is the entity's name for the messages (DecalName,
 * :93-101). material required (:159-164) and IsDiscrete (:165-170); decalSize "64 64",
 * both positive (:172-177); depth "32", positive, half = depth / 2 (:178-184); fade "8"
 * clamped to [0, depth / 2] (:188-193); parallel "0" (:194); origin (:195); the axis from
 * the target's raw origin key (:201-227, engineDecalTargetAxis) when it is found and not
 * at the origin, else rotation, else angles, else angle, without modelscale (:228-243);
 * then the far cap and CreateParms (engineDecalProjection, :249-270). Pure; any thread.
 */
DecalProjectorParse parseDecalProjector(
  std::string_view name, const EntityKeys& spawnArgs, const DecalProjectorInputs& inputs);

/**
 * LP7.3 (C54, P7.18): every vertex of a .proc surface with its geometry within bounds
 * grown by growth on each side (a NaN bound contains nothing); false for a surface
 * without its geometry or without vertices. Pure; any thread.
 */
bool procSurfaceWithin(
  const ProcSurface& surface, const EngineBounds& bounds, float growth);

} // namespace tb::lit
