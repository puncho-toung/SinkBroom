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
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 GPL Source
// Code and RBDOOM-3-BFG (neo/tools/compilers/dmap/dress_decal.cpp, renderer/Material.h
// and idlib/math/Math.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/DecalProjectors.h"

#include "lit/EntityState.h"
#include "lit/ProcFile.h"

#include <fmt/format.h>

#include <algorithm>

namespace tb::lit
{
namespace
{

// renderer/Material.h:438 SURF_DISCRETE, :74 DFRM_NONE, :302 SS_SUBVIEW
constexpr auto SurfDiscrete = 1 << 10;
constexpr auto DeformNone = 0;
constexpr auto SortSubview = -3.0f;

// Math.h:1518-1521 idMath::ClampFloat( min, max, value ) = Max( min, Min( max, value ) ),
// Lib.h's Max and Min: ( x > y ) ? x : y and ( x < y ) ? x : y (a NaN value stays NaN)
float engineClampFloat(const float min, const float max, const float value)
{
  const auto upper = max < value ? max : value;
  return min > upper ? min : upper;
}

} // namespace

bool engineMaterialIsDiscrete(const MaterialEntry& material)
{
  // Material.h:671-675
  return material.entityGui != 0 || !material.gui.empty() || material.deform != DeformNone
         || material.sort == SortSubview || (material.surfaceFlags & SurfDiscrete) != 0;
}

DecalProjectorParse parseDecalProjector(
  const std::string_view name, const EntityKeys& args, const DecalProjectorInputs& inputs)
{
  auto result = DecalProjectorParse{};
  auto& projector = result.projector;
  const auto refuse = [&](const DecalProjectorOutcome outcome, std::string warning) {
    result.outcome = outcome;
    result.warnings.push_back(std::move(warning));
    return result;
  };

  // :159-170: the material is required and must be discrete; FindMaterial never returns
  // NULL, its default for a name without a decl is not discrete
  projector.materialKey = std::string{spawnString(args, "material")};
  if (projector.materialKey.empty())
  {
    return refuse(
      DecalProjectorOutcome::NoMaterial,
      fmt::format("func_decal '{}': no material", name));
  }
  const auto answer = inputs.material ? inputs.material(projector.materialKey)
                                      : std::optional<const MaterialEntry*>{nullptr};
  if (!answer)
  {
    result.outcome = DecalProjectorOutcome::Pending;
    result.pendingMaterial = projector.materialKey;
    return result;
  }
  const auto* material = *answer;
  projector.material = material ? material->name : projector.materialKey;
  if (!material || !engineMaterialIsDiscrete(*material))
  {
    return refuse(
      DecalProjectorOutcome::NotDiscrete,
      fmt::format(
        "func_decal '{}': material '{}' is not discrete (use DECAL_MACRO), skipped",
        name,
        projector.material));
  }

  // :172-184
  auto size = EngineVec2{0, 0};
  spawnVec2(args, "decalSize", "64 64", size);
  if (size[0] <= 0.0f || size[1] <= 0.0f)
  {
    return refuse(
      DecalProjectorOutcome::BadSize,
      fmt::format(
        "func_decal '{}': size {:g} {:g} is not positive, skipped",
        name,
        size[0],
        size[1]));
  }
  const auto depth = spawnFloat(args, "depth", "32");
  if (depth <= 0.0f)
  {
    return refuse(
      DecalProjectorOutcome::BadDepth,
      fmt::format("func_decal '{}': depth {:g} is not positive, skipped", name, depth));
  }
  const auto halfDepth = depth * 0.5f;

  // :188-195: the far cap is depth / 2 behind the origin, so a longer fade would put the
  // knee in front of the origin plane
  const auto mapFade = spawnFloat(args, "fade", "8");
  const auto fade = engineClampFloat(0.0f, halfDepth, mapFade);
  if (fade != mapFade)
  {
    result.warnings.push_back(fmt::format(
      "func_decal '{}': fade {:g} clamped to {:g} (0 to depth/2 = {:g})",
      name,
      mapFade,
      fade,
      halfDepth));
  }
  const auto parallel = spawnBool(args, "parallel", "0");
  auto origin = EngineVec3{0, 0, 0};
  spawnVector(args, "origin", "", origin);

  // :197-227: aimed at the target's raw map origin (no def defaults, no origin brush)
  auto axis = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
  const auto targetName = spawnString(args, "target");
  if (!targetName.empty())
  {
    const auto* target = inputs.target ? inputs.target(targetName) : nullptr;
    if (!target)
    {
      result.warnings.push_back(fmt::format(
        "func_decal '{}': target '{}' not found, using its angles", name, targetName));
    }
    else
    {
      auto targetOrigin = EngineVec3{0, 0, 0};
      spawnVector(*target, "origin", "", targetOrigin);
      if (const auto aimed = engineDecalTargetAxis(origin, targetOrigin))
      {
        axis = *aimed;
        projector.aimed = true;
      }
      else
      {
        result.warnings.push_back(fmt::format(
          "func_decal '{}': target '{}' is at its origin, using its angles",
          name,
          targetName));
      }
    }
  }
  if (!projector.aimed)
  {
    // :228-243: the order the inlined models read their axis in, without modelscale
    if (!spawnMatrix(args, "rotation", "1 0 0 0 1 0 0 0 1", axis))
    {
      auto angles = std::array<float, 3>{0, 0, 0};
      if (spawnAngles(args, "angles", "0 0 0", angles))
      {
        axis = engineAnglesToMat3(angles[0], angles[1], angles[2]);
      }
      else
      {
        axis = engineAnglesToMat3(0.0f, spawnFloat(args, "angle", "0"), 0.0f);
      }
    }
  }

  // :249-270
  const auto parms = engineDecalProjection(origin, axis, size, halfDepth, fade, parallel);
  if (!parms)
  {
    return refuse(
      DecalProjectorOutcome::NoProjection,
      fmt::format("func_decal '{}': no projection, skipped", name));
  }
  projector.parms = *parms;
  result.outcome = DecalProjectorOutcome::Ready;
  return result;
}

bool procSurfaceWithin(
  const ProcSurface& surface, const EngineBounds& bounds, const float growth)
{
  // xyz st normal, and rgba for PROC_VERTEX_FORMAT_COLOR (ProcFile.cpp's parseModel)
  const auto floats = std::size_t(surface.vertexFormat == 1 ? 12 : 8);
  const auto numVerts = std::size_t(std::max(surface.numVerts, 0));
  if (numVerts == 0 || surface.verts.size() != numVerts * floats)
  {
    return false;
  }
  for (auto k = std::size_t{0}; k < numVerts; ++k)
  {
    const auto* v = surface.verts.data() + k * floats;
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      // a NaN bound fails both tests
      if (!(v[c] >= bounds[0][c] - growth && v[c] <= bounds[1][c] + growth))
      {
        return false;
      }
    }
  }
  return true;
}

} // namespace tb::lit
