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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_defs.cpp,
// renderer/RenderWorld.cpp and renderer/RenderWorld_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/LightData.h"

#include "lit/LightParse.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace tb::lit
{
namespace
{

// what a FindMaterial call returns from the table
enum class FindState
{
  // the table has no answer yet
  Pending,
  // NULL
  Null,
  Found,
};

struct Found
{
  FindState state = FindState::Pending;
  std::shared_ptr<const MaterialEntry> entry;
};

// FindMaterial( name, false ): NULL without a decl (an implicit entry, or none: a failed
// lookup)
Found findMaterialNoDefault(const MaterialTable* table, const std::string_view name)
{
  const auto slot = table ? table->find(name) : nullptr;
  if (!slot)
  {
    return {};
  }
  if (!slot->entry || slot->entry->implicit)
  {
    return {FindState::Null, nullptr};
  }
  return {FindState::Found, slot->entry};
}

// FindMaterial( name ) with makeDefault, as RenderSystem_init finds the default lights:
// an implicit material is the material; only a failed parse (where the engine stops) is
// NULL
Found findMaterialDefault(const MaterialTable* table, const std::string_view name)
{
  const auto slot = table ? table->find(name) : nullptr;
  if (!slot)
  {
    return {};
  }
  if (!slot->entry)
  {
    return {FindState::Null, nullptr};
  }
  return {FindState::Found, slot->entry};
}

// RenderWorld_defs.cpp:334-378
LightShader resolveShader(const ParsedLight& light, const MaterialTable* table)
{
  auto shader = LightShader{};
  const auto* defaultName =
    light.parms.pointLight ? DefaultPointLightMaterial : DefaultProjectedLightMaterial;

  // decide which light shader we are going to use; a light with bad target info never
  // asked FindMaterial, so its shader is the memset's NULL; a texture key's material was
  // precached with makeDefault, so FindMaterial( texture, false ) finds even an implicit
  // one (CacheDictionaryMedia); only the lights/squarelight1 default can be NULL
  // an empty texture is FindType's "_emptyName", which no decl has
  const auto texture =
    light.badTarget || light.parms.texture.empty() ? Found{FindState::Null, nullptr}
    : light.texturePrecached ? findMaterialDefault(table, light.parms.texture)
                             : findMaterialNoDefault(table, light.parms.texture);
  if (texture.state == FindState::Found)
  {
    shader.material = texture.entry;
    shader.name = texture.entry->name;
  }
  else if (texture.state == FindState::Pending)
  {
    shader.name = light.parms.texture;
    shader.pending = true;
  }
  else
  {
    shader.defaultLight = true;
    const auto fallback = findMaterialDefault(table, defaultName);
    shader.material = fallback.entry;
    shader.name = fallback.entry ? fallback.entry->name : std::string{defaultName};
    shader.pending = fallback.state == FindState::Pending;
  }

  // get the falloff image, or the default shader's of the correct type
  if (shader.material)
  {
    shader.falloffImage = shader.material->lightFalloffImage;
  }
  if (!shader.pending && !shader.falloffImage)
  {
    const auto fallback = findMaterialDefault(table, defaultName);
    if (fallback.state == FindState::Pending)
    {
      shader.pending = true;
    }
    else if (fallback.entry)
    {
      shader.falloffImage = fallback.entry->lightFalloffImage;
      shader.defaultFalloff = true;
    }
  }
  return shader;
}

} // namespace

std::vector<int> commonChildrenAreas(const ProcTree& tree)
{
  // RenderWorld_load.cpp:627-670, as a post-order walk from node 0; a node reached again
  // while open (a malformed tree's cycle, never in dmap's output) counts as multiple
  // areas (M-LIGHT-GUARD-04)
  const auto count = tree.nodes.size();
  auto common = std::vector<int>(count, MultipleAreas);
  if (count == 0)
  {
    return common;
  }
  enum : std::uint8_t
  {
    New,
    Open,
    Done,
  };
  auto state = std::vector<std::uint8_t>(count, New);
  auto stack = std::vector<std::size_t>{0};
  while (!stack.empty())
  {
    const auto index = stack.back();
    const auto& node = tree.nodes[index];
    if (state[index] == New)
    {
      state[index] = Open;
      for (const auto child : node.children)
      {
        if (child > 0 && state[std::size_t(child)] == New)
        {
          stack.push_back(std::size_t(child));
        }
      }
      continue;
    }
    stack.pop_back();
    if (state[index] == Done)
    {
      continue;
    }

    int nums[2];
    for (auto i = 0; i < 2; ++i)
    {
      const auto child = node.children[std::size_t(i)];
      if (child <= 0)
      {
        nums[i] = -1 - child;
      }
      else
      {
        nums[i] =
          state[std::size_t(child)] == Done ? common[std::size_t(child)] : MultipleAreas;
      }
    }

    // solid nodes will match any area
    if (nums[0] == -1)
    {
      nums[0] = nums[1];
    }
    if (nums[1] == -1)
    {
      nums[1] = nums[0];
    }

    common[index] = nums[0] == nums[1] ? nums[0] : MultipleAreas;
    state[index] = Done;
  }
  return common;
}

std::vector<int> pushFrustumIntoTree(
  const ProcTree& tree,
  const std::vector<int>& commonAreas,
  const EngineMat4& frustumTransform,
  const EngineBounds& frustumBounds)
{
  auto areas = std::vector<int>{};
  // RenderWorld.cpp:2115-2126: without areaNodes nothing is referenced
  if (tree.nodes.empty())
  {
    return areas;
  }

  // calculate the corners of the frustum in world space
  const auto corners = engineFrustumCorners(frustumTransform, frustumBounds);

  // :2047-2108 PushFrustumIntoTree_r with an explicit stack in the recursion's order:
  // child 0's subtree before child 1. The set of referenced areas stands in for
  // tr.viewCount; a node is visited once (M-LIGHT-GUARD-04)
  const auto numAreas = std::size_t(std::max(tree.numAreas, 0));
  auto referenced = std::vector<bool>(numAreas, false);
  auto visited = std::vector<bool>(tree.nodes.size(), false);
  auto stack = std::vector<int>{0};
  while (!stack.empty())
  {
    const auto nodeNum = stack.back();
    stack.pop_back();

    if (nodeNum < 0)
    {
      const auto areaNum = std::size_t(-1 - nodeNum);
      // an area beyond numAreas would index past the engine's portalAreas
      if (areaNum >= numAreas || referenced[areaNum])
      {
        continue; // already added a reference here
      }
      referenced[areaNum] = true;
      areas.push_back(int(areaNum));
      continue;
    }

    const auto index = std::size_t(nodeNum);
    if (visited[index])
    {
      continue;
    }
    visited[index] = true;
    const auto& node = tree.nodes[index];

    // if we know that all possible children nodes only touch an area we have already
    // marked, we can early out; all solid (-1) indexes the engine's portalAreas[ -1 ] and
    // has no area to add, so it goes on to the solid children, which add nothing
    const auto common = index < commonAreas.size() ? commonAreas[index] : MultipleAreas;
    if (common >= 0 && std::size_t(common) < numAreas && referenced[std::size_t(common)])
    {
      continue;
    }

    // exact check all the corners against the node plane
    const auto cull = engineCullFrustumCorners(
      corners, {node.plane[0], node.plane[1], node.plane[2], node.plane[3]});

    // 0 = solid; child 1 is pushed first, so that child 0's subtree is walked first
    if (cull != EngineFrustumCull::Front && node.children[1] != 0)
    {
      stack.push_back(node.children[1]);
    }
    if (cull != EngineFrustumCull::Back && node.children[0] != 0)
    {
      stack.push_back(node.children[0]);
    }
  }
  return areas;
}

DerivedLight deriveLight(
  const ParsedLight& light,
  const MaterialTable* materials,
  const ProcTree* tree,
  const std::vector<int>* commonAreas)
{
  auto derived = DerivedLight{};
  derived.shader = resolveShader(light, materials);

  const auto hasTree = tree && !tree->nodes.empty();
  derived.hasAreaTree = hasTree;

  // A15: a light with bad target info has a zero axis and colour; the engine derives a
  // projection from it that no inverse survives, and it lights nothing (M-LIGHT-DIFF-03)
  if (light.badTarget)
  {
    derived.hasVolume = false;
    if (hasTree)
    {
      derived.areaNum = pointInArea(*tree, light.parms.origin);
    }
    return derived;
  }

  const auto& parms = light.parms;
  auto geometry = EngineLightGeometry{};
  geometry.origin = parms.origin;
  geometry.axis = parms.axis;
  geometry.target = parms.target;
  geometry.right = parms.right;
  geometry.up = parms.up;
  geometry.start = parms.start;
  geometry.end = parms.end;
  geometry.lightRadius = parms.lightRadius;
  geometry.lightCenter = parms.lightCenter;
  geometry.pointLight = parms.pointLight;
  geometry.parallel = parms.parallel;
  derived.projection = engineDeriveLightProjection(geometry);

  // the engine's idLib::Warning texts (RenderWorld_defs.cpp:455, :465)
  if (!derived.projection.lightMatrixInverted)
  {
    derived.warnings.emplace_back("lightMatrix invert failed");
  }
  if (!derived.projection.baseLightProjectInverted)
  {
    derived.warnings.emplace_back("baseLightProject invert failed");
  }
  derived.hasVolume =
    derived.projection.lightMatrixInverted && derived.projection.baseLightProjectInverted;

  // RenderWorld_defs.cpp:694-729 R_CreateLightRefs: the areaNum of the light origin (the
  // center offset origin, else the entity's; never the parallel offset), then the light
  // frustum down the BSP tree into areas (nothing floods through portals, A1)
  if (hasTree)
  {
    derived.areaNum = pointInArea(*tree, derived.projection.globalLightOrigin);
    if (derived.areaNum == -1)
    {
      derived.areaNum = pointInArea(*tree, parms.origin);
    }
    if (derived.hasVolume)
    {
      const auto computed = commonAreas ? std::vector<int>{} : commonChildrenAreas(*tree);
      derived.areas = pushFrustumIntoTree(
        *tree,
        commonAreas ? *commonAreas : computed,
        derived.projection.inverseBaseLightProject,
        engineZeroOneCube());
    }
  }
  return derived;
}

std::vector<std::string> lightMaterialNames(const ParsedLight& light)
{
  auto names = std::vector<std::string>{};
  if (!light.badTarget && !light.parms.texture.empty())
  {
    names.push_back(light.parms.texture);
  }
  names.emplace_back(
    light.parms.pointLight ? DefaultPointLightMaterial : DefaultProjectedLightMaterial);
  return names;
}

} // namespace tb::lit
