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

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tb::lit
{
struct ProcModel;
struct ProcTree;

/**
 * C31 (LP4.0): what the bake read asks of a surface's material: the entry
 * declManager->FindMaterial( name ) gives with makeDefault true, as ParseModel asks it
 * (RenderWorld_load.cpp:245; an unknown name is an implicit material, which has no
 * deform); null when none is known, which counts as no deform. The material service
 * answers it on its parse thread from the copied registry (MaterialDecls::lookup).
 */
using AreaMaterialLookup =
  std::function<std::shared_ptr<const MaterialEntry>(const std::string&)>;

// one portal area as the engine's world holds it once the map is loaded
struct AreaInfo
{
  // the _areaN model's bounds (idRenderModelStatic::FinishSurfaces, Model.cpp:1203-1208
  // and :1330-1363; a .bproc's stored ones as they are): Zero() without a surface
  EngineBounds modelBounds = {};
  // area->globalBounds (RenderWorld_load.cpp:1081-1094 with RenderWorld_defs.cpp:49-60):
  // ProjectedBounds( inverseBaseModelProject, bounds_unitCube, false )
  EngineBounds globalBounds = {};
  // the world entity's OffsetScaleForBounds( identity, modelBounds ): its axis is the
  // identity at the origin (RenderWorld_load.cpp:1074-1086)
  EngineMat4 inverseBaseModelProject = {};
};

/**
 * C31 (LP4.0): the per-area data of a .proc (or .bproc) tree the view's portal flow and
 * the references read: per area its world entity's bounds and inverseBaseModelProject,
 * and commonChildrenAreas( tree ) for pushing reference bounds down the tree. Plain
 * data, immutable once built: it is built on the parse thread inside the bake read
 * (lit/MapBake.h), where the materials are at hand, published with the bake, and read
 * by any thread.
 */
struct AreaData
{
  // one per area of the tree (ProcTree::numAreas)
  std::vector<AreaInfo> areas;
  // lit/LightData.h commonChildrenAreas( tree ), one per node
  std::vector<int> commonAreas;
};

/**
 * idRenderModelStatic::FinishSurfaces' model bounds (Model.cpp:1203-1208, :1330-1363) of
 * a .proc model: a model without a surface has Zero() bounds (a point at the origin, not
 * cleared); otherwise Clear(), then per surface AddBounds of its bounds (ProcSurface::
 * bounds, R_BoundTriSurf's), a surface whose material has a deform (MaterialEntry::deform
 * != DFRM_NONE) first grown by engineDeformSurfaceBounds. A model read from a .bproc
 * (ProcModel::boundsKnown) keeps its stored bounds, which the engine wrote already grown.
 * The calling thread runs the lookup; parse thread in the bake read (C20).
 */
EngineBounds procModelBounds(const ProcModel& model, const AreaMaterialLookup& materials);

/**
 * The AreaData of a tree (C31): per area N the bounds of the tree's model "_areaN"
 * (procModelBounds; the last model of that name, as FindModel's hash finds the last one
 * added; an area without a model has Zero() bounds where the engine's InitFromMap stops
 * with "bad area model lookup", a guard), then the world entity as R_DeriveEntityData
 * makes it (RenderWorld_defs.cpp:49-60 with the identity axis at the origin):
 * inverseBaseModelProject = OffsetScaleForBounds( identity, modelBounds ) and
 * globalBounds = ProjectedBounds( inverseBaseModelProject, bounds_unitCube, false ),
 * through the copied idRenderMatrix. The materials are asked once per name. Parse thread
 * in the bake read (C20: the lookup runs the copied registry); with an empty lookup (no
 * deform) any thread.
 */
AreaData buildAreaData(const ProcTree& tree, const AreaMaterialLookup& materials = {});

} // namespace tb::lit
