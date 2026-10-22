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

#include "lit/EngineLight.h"
#include "lit/MaterialDecls.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{
struct MaterialTable;
struct ParsedLight;
struct ProcTree;

// RenderSystem_init.cpp:1784-1785: tr.defaultPointLight and tr.defaultProjectedLight
constexpr auto DefaultPointLightMaterial = "lights/defaultPointLight";
constexpr auto DefaultProjectedLightMaterial = "lights/defaultProjectedLight";

/**
 * The light shader R_DeriveLightData uses (RenderWorld_defs.cpp:334-378, A15): the
 * material FindMaterial( texture, false ) finds, or, where that is NULL, the default
 * point or projected light; and its falloff image, or the default's of the light's kind.
 * FindMaterial( texture, false ) is NULL when the name has no decl: the table's entry is
 * implicit (the material service makes implicit materials, the engine does not here), or
 * its lookup failed.
 */
struct LightShader
{
  // the material's canonical name, as listRenderLightDefs prints it; while pending, the
  // name looked up
  std::string name;
  std::shared_ptr<const MaterialEntry> material;
  // the default point or projected light stands in for the texture
  bool defaultLight = false;
  std::optional<MaterialImageKey> falloffImage;
  // the falloff image is the default light's
  bool defaultFalloff = false;
  // a material the table does not have yet (Pending): the light is provisional and is
  // derived again when a later table has it
  bool pending = false;
};

/**
 * R_DeriveLightData and R_CreateLightRefs (RenderWorld_defs.cpp:330-470, :694-729, C5,
 * D58) for one parsed light: the shader, the projection (EngineLight.h), areaNum, and
 * the area references.
 */
struct DerivedLight
{
  LightShader shader;
  EngineLightProjection projection;
  // false for a light with bad target info (A15, nothing is derived) or whose matrices
  // did not invert (the engine's warnings; its volume is unknown): no volume, no areas
  bool hasVolume = true;
  // PointInArea( globalLightOrigin ), else PointInArea( origin ); -1 without a .proc
  int areaNum = -1;
  // a .proc tree was there: areas holds the references; without one the light reaches
  // its whole volume (D22 as A1 restates it) and areas is empty
  bool hasAreaTree = false;
  // the areas PushFrustumIntoTree references, in the order it reaches them
  std::vector<int> areas;
  std::vector<std::string> warnings;
};

// CHILDREN_HAVE_MULTIPLE_AREAS (RenderWorld_local.h:131)
constexpr auto MultipleAreas = -2;

/**
 * RenderWorld_load.cpp:627-670 CommonChildrenArea_r for every node of the tree: the one
 * area all of a node's leaves are in (solid matches any), -1 when all are solid, or
 * MultipleAreas. Pure; any thread.
 */
std::vector<int> commonChildrenAreas(const ProcTree& tree);

/**
 * RenderWorld.cpp:2047-2126 PushFrustumIntoTree and its recursion (D58): the corners of
 * frustumBounds through frustumTransform (the copied GetFrustumCorners), pushed down the
 * .proc's area BSP from node 0 with the copied CullFrustumCornersToPlane (a corner with
 * d >= +0 is in front): child 0 unless every corner is behind, child 1 unless none is; a
 * leaf area is referenced once, in the order reached; a node whose children all lie in
 * one area already referenced ends the walk there (commonChildrenArea,
 * r_useNodeCommonChildren 1). A per-call set stands in for tr.viewCount. commonAreas is
 * commonChildrenAreas( tree ). Pure; any thread (C24).
 */
std::vector<int> pushFrustumIntoTree(
  const ProcTree& tree,
  const std::vector<int>& commonAreas,
  const EngineMat4& frustumTransform,
  const EngineBounds& frustumBounds);

/**
 * The light record's derivation: the light shader from the material table (null: every
 * material pending), the projection, areaNum and the area references over the .proc's
 * tree (null: none). commonAreas may be null, and is then computed. Pure; any thread, the
 * CPU pool included: it reads the immutable table and tree and calls only the light
 * facade (C24).
 */
DerivedLight deriveLight(
  const ParsedLight& light,
  const MaterialTable* materials,
  const ProcTree* tree,
  const std::vector<int>* commonAreas = nullptr);

// the materials a light's shader may need looked up: its texture and the default light of
// its kind
std::vector<std::string> lightMaterialNames(const ParsedLight& light);

} // namespace tb::lit
