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

#include <cstddef>
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

/**
 * One side of an inter-area portal as ParseInterAreaPortals (and ReadBinaryAreaPortals)
 * link it (RenderWorld_load.cpp:440-472, :483-545): the positive area's portal_t keeps
 * the file's winding and leads into the negative area; the negative area's has the
 * winding's Reverse() and leads into the positive one; each side's plane is GetPlane of
 * its own winding (P4.17), so the two planes are each other's negation only up to the
 * rounding of GetPlane's centre.
 */
struct AreaPortal
{
  // the portal's index in the tree (its doublePortal_t)
  std::size_t portal = 0;
  // 0 the positive area's side (doublePortals[i].portals[0]), 1 the negative area's
  int side = 0;
  // p->intoArea
  int intoArea = 0;
  // p->w: this side's winding
  std::vector<EngineVec3> points;
  // p->plane: idWinding::GetPlane of points (plane.Zero() below three points)
  EnginePlane plane = {};
};

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
  // portalAreas[N].portals in the engine's list order: each portal is prepended to both
  // its areas' lists, so the .proc's order reversed (RenderWorld_load.cpp:457-458,
  // :469-470)
  std::vector<AreaPortal> portals;
};

/**
 * C31 (LP4.0): the per-area data of a .proc (or .bproc) tree the view's portal flow and
 * the references read: per area its world entity's bounds and inverseBaseModelProject
 * and its portal sides, the areas' connected components, and commonChildrenAreas( tree )
 * for pushing reference bounds down the tree. Plain
 * data, immutable once built: it is built on the parse thread inside the bake read
 * (lit/MapBake.h), where the materials are at hand, published with the bake, and read
 * by any thread.
 */
struct AreaData
{
  // one per area of the tree (ProcTree::numAreas)
  std::vector<AreaInfo> areas;
  // connectedComponents( tree ): one component id per area (D82, K30)
  std::vector<int> components;
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

/**
 * RenderWorld_load.cpp:440-472 (and the binary reader's :483-545): every area's portal
 * sides in its list order, the windings and planes through the copied idWinding::Reverse
 * and GetPlane; a portal naming an area past the count is skipped (the readers refuse
 * such a file). Pure: the copied winding code reads only its arguments (mirror.json
 * M-GEO-PURE-01); any thread.
 */
std::vector<std::vector<AreaPortal>> areaPortalSides(const ProcTree& tree);

/**
 * D82, K30: the areas' connectivity with every portal open, as ClearPortalStates leaves
 * it (RenderWorld_load.cpp:1005-1022: FloodConnectedAreas, RenderWorld_portals.cpp
 * :1050-1067, through every portal of an area's list, blockingBits 0 for all, for each
 * portal attribute alike): one component id per area, the same id for the areas a flood
 * from one reaches. The engine numbers a component by the counter of the last of its
 * areas' floods; only equality is read (AreasAreConnected), so the ids here count from
 * 0 in the order of each component's lowest area. Pure; any thread.
 */
std::vector<int> connectedComponents(const ProcTree& tree);

/**
 * idRenderWorldLocal::AreasAreConnected (RenderWorld_portals.cpp:1075-1098) with every
 * portal open: false when either area is -1; an area outside the components (the
 * engine's Error "bad parms", a guard) is not connected either; otherwise the same
 * component. Pure; any thread.
 */
bool areasConnected(const std::vector<int>& components, int a, int b);

} // namespace tb::lit
