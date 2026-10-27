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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_portals.cpp,
// renderer/RenderWorld_load.cpp, renderer/tr_frontend_main.cpp,
// renderer/tr_frontend_addlights.cpp, renderer/tr_frontend_addmodels.cpp and
// renderer/RenderCommon.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "ViewFlow.h"

#include "LitStages.h"
#include "lit/EngineTables.h"
#include "lit/LightRecord.h"
#include "lit/ProcFile.h"

#include <algorithm>
#include <limits>

namespace tb::lit
{
namespace
{

// the cvars the flow mirrors at their defaults; the other branches (r_usePortals 0,
// r_singleArea 1, the exact winding culls of r_use*PortalCulling 2) are debug paths and
// not mirrored
static_assert(EngineFrame.usePortals);
static_assert(!EngineFrame.singleArea);
static_assert(EngineFrame.useLightPortalCulling == 1);
static_assert(EngineFrame.useEntityPortalCulling == 1);
static_assert(EngineFrame.useLightAreaCulling);
static_assert(EngineFrame.useAreasConnectedForShadowCulling == 2);

// the view's own stack went through no portal
constexpr auto ViewSide = std::numeric_limits<std::size_t>::max();

/**
 * portalStack_t (RenderWorld_portals.cpp:39-47): the portal side the stack went through
 * (portal index · 2 + side; ViewSide for the view's own), its planes, whose positive side
 * is outside the visible volume, and its rect. Held in a vector, so on the heap.
 */
struct PortalStack
{
  std::size_t side = ViewSide;
  std::array<EnginePlane, MaxPortalPlanes + 1> planes = {};
  std::size_t numPlanes = 0;
  ScreenRect rect;

  std::span<const EnginePlane> planeSpan() const { return {planes.data(), numPlanes}; }
};

// a frame of FloodViewThroughArea_r: its area and the next portal of the area's list
struct FloodFrame
{
  int area = -1;
  std::size_t nextPortal = 0;
};

class Flow
{
private:
  const ViewFlowInputs& m_inputs;
  const AreaData& m_areas;
  ViewFlowResult& m_result;
  EngineVec3 m_origin;
  std::size_t m_numAreas;

  // the area lists (portalArea_t::entityRefs and lightRefs): indexes into the inputs
  std::vector<std::vector<std::size_t>> m_areaEntities;
  std::vector<std::vector<std::size_t>> m_areaLights;
  // the corners of each entity, light and world entity, the same for every chain
  std::vector<EngineFrustumCorners> m_entityCorners;
  std::vector<EngineFrustumCorners> m_lightCorners;
  std::vector<EngineFrustumCorners> m_worldCorners;
  // R_SetEntityDefViewEntity / R_SetLightDefViewLight: the scissor of each, cleared when
  // first set up, and whether it was
  std::vector<ScreenRect> m_entityScissors;
  std::vector<bool> m_entitySet;
  std::vector<ScreenRect> m_lightScissors;
  std::vector<bool> m_lightSet;
  // LP3.0 (D148): the lights in the order the flood first accepted them
  std::vector<std::size_t> m_acceptedLights;
  std::vector<ScreenRect> m_worldScissors;
  std::vector<bool> m_worldSet;

  // the flood's stack (the portalStack_t chain) and its frames
  std::vector<PortalStack> m_stacks;
  std::vector<FloodFrame> m_frames;
  std::vector<bool> m_onStack;

public:
  Flow(const ViewFlowInputs& inputs, const AreaData& areas, ViewFlowResult& result)
    : m_inputs{inputs}
    , m_areas{areas}
    , m_result{result}
    , m_origin{inputs.view.viewOrigin}
    , m_numAreas{areas.areas.size()}
  {
    m_areaEntities.resize(m_numAreas);
    m_areaLights.resize(m_numAreas);
    const auto unitCube = engineUnitCube();
    const auto zeroOneCube = engineZeroOneCube();
    for (auto i = std::size_t{0}; i < inputs.entities.size(); ++i)
    {
      const auto& entity = inputs.entities[i];
      m_entityCorners.push_back(
        engineFrustumCorners(entity.inverseBaseModelProject, unitCube));
      for (const auto area : entity.areas)
      {
        if (area >= 0 && std::size_t(area) < m_numAreas)
        {
          m_areaEntities[std::size_t(area)].push_back(i);
        }
      }
    }
    for (auto i = std::size_t{0}; i < inputs.lights.size(); ++i)
    {
      m_lightCorners.push_back(
        engineFrustumCorners(inputs.lights[i].inverseBaseLightProject, zeroOneCube));
    }
    // LP3.0 (D148, P3.2): area->lightRefs, newest reference first (AddLightRefToArea
    // prepends, RenderWorld.cpp:1873-1876): the lights in reverse of their input order
    for (auto i = inputs.lights.size(); i-- > 0;)
    {
      for (const auto area : inputs.lights[i].areas)
      {
        if (area >= 0 && std::size_t(area) < m_numAreas)
        {
          m_areaLights[std::size_t(area)].push_back(i);
        }
      }
    }
    for (const auto& area : areas.areas)
    {
      m_worldCorners.push_back(
        engineFrustumCorners(area.inverseBaseModelProject, unitCube));
    }
    m_entityScissors.resize(inputs.entities.size());
    m_entitySet.assign(inputs.entities.size(), false);
    m_lightScissors.resize(inputs.lights.size());
    m_lightSet.assign(inputs.lights.size(), false);
    m_worldScissors.resize(m_numAreas);
    m_worldSet.assign(m_numAreas, false);
    auto portals = std::size_t{0};
    for (const auto& area : areas.areas)
    {
      for (const auto& side : area.portals)
      {
        portals = std::max(portals, side.portal + 1);
      }
    }
    m_onStack.assign(portals * 2, false);
  }

  /**
   * FlowViewThroughPortals (RenderWorld_portals.cpp:658-687): the view's stack is the
   * planes and the view's scissor; outside the world every area is added once with it,
   * else the flood goes from the view's area.
   */
  void flowViewThroughPortals(
    std::span<const EnginePlane> planes, const ScreenRect& scissor)
  {
    auto ps = PortalStack{};
    ps.side = ViewSide;
    for (auto i = std::size_t{0}; i < planes.size() && i < ps.planes.size(); ++i)
    {
      ps.planes[i] = planes[i];
      ++ps.numPlanes;
    }
    ps.rect = scissor;

    // if outside the world, mark everything
    if (m_result.viewArea < 0)
    {
      for (auto i = std::size_t{0}; i < m_numAreas; ++i)
      {
        addAreaToView(int(i), ps);
      }
    }
    else
    {
      // flood out through portals, setting area viewCount
      floodViewThroughArea(m_result.viewArea, ps);
    }
  }

  // the viewEntity and viewLight scissors that are not empty, into the result
  void finish()
  {
    m_result.worldRects.assign(m_numAreas, std::nullopt);
    for (auto i = std::size_t{0}; i < m_numAreas; ++i)
    {
      if (m_worldSet[i] && !m_worldScissors[i].isEmpty())
      {
        m_result.worldRects[i] = m_worldScissors[i];
      }
    }
    for (auto i = std::size_t{0}; i < m_inputs.entities.size(); ++i)
    {
      if (m_entitySet[i] && !m_entityScissors[i].isEmpty())
      {
        m_result.entityRects[m_inputs.entities[i].id] = m_entityScissors[i];
      }
    }
    for (auto i = std::size_t{0}; i < m_inputs.lights.size(); ++i)
    {
      if (m_lightSet[i] && !m_lightScissors[i].isEmpty())
      {
        m_result.lightRects[m_inputs.lights[i].id] = m_lightScissors[i];
      }
      else if (m_lightSet[i])
      {
        m_result.emptyLightRects[m_inputs.lights[i].id] = m_lightScissors[i];
      }
    }
    // LP3.0 (D148): R_SetLightDefViewLight prepends each light at its first acceptance
    // (RenderWorld_portals.cpp:84-85), so viewLights holds them last accepted first
    m_result.lightOrder.clear();
    for (auto i = m_acceptedLights.size(); i-- > 0;)
    {
      m_result.lightOrder.push_back(m_inputs.lights[m_acceptedLights[i]].id);
    }
  }

private:
  /**
   * AddAreaToView (RenderWorld_portals.cpp:412-421): marks the area (its viewCount),
   * then its entities and lights culled to the chain; AddAreaViewEnvprobes is a debug
   * list (its culls are #if 0, SC306) and is left out.
   */
  void addAreaToView(const int areaNum, const PortalStack& ps)
  {
    m_result.areaFlowed[std::size_t(areaNum)] = true;
    ++m_result.chains;
    if (m_inputs.recordChainsForTesting)
    {
      m_result.chainsForTesting.push_back(ViewFlowChain{areaNum, ps.numPlanes, ps.rect});
    }
    addAreaViewEntities(areaNum, ps);
    addAreaViewLights(areaNum, ps);
  }

  /**
   * AddAreaViewEntities (RenderWorld_portals.cpp:222-267) over the area's entity
   * references: the area's world entity (AddWorldModelEntities references it in its own
   * area alone, RenderWorld_load.cpp:1091; a model of cleared bounds, which no .proc
   * area model has, is skipped as a guard) and every brush entity whose areas hold it;
   * r_singleEntity -1, no faded decals, no suppressSurfaceInViewID. CullEntityByPortals,
   * else the scissor (R_SetEntityDefViewEntity's, cleared when first set up) united with
   * the chain's rect.
   */
  void addAreaViewEntities(const int areaNum, const PortalStack& ps)
  {
    const auto area = std::size_t(areaNum);
    if (
      !engineBoundsIsCleared(m_areas.areas[area].modelBounds)
      && !engineCullCornersByPortals(m_worldCorners[area], ps.planeSpan()))
    {
      unite(m_worldScissors[area], m_worldSet[area], ps.rect);
    }
    for (const auto index : m_areaEntities[area])
    {
      // cull reference bounds
      if (engineCullCornersByPortals(m_entityCorners[index], ps.planeSpan()))
      {
        // we are culled out through this portal chain, but it might still be visible
        // through others
        continue;
      }
      unite(m_entityScissors[index], m_entitySet[index], ps.rect);
    }
  }

  /**
   * AddAreaViewLights (RenderWorld_portals.cpp:364-402) over the area's light references
   * (every light whose areas hold it): a light that casts no shadows, whose own area is
   * not -1 and not connected to the view's, is dropped (r_useLightAreaCulling 1; the
   * engine's comment says the opposite of its code); then CullLightByPortals, else the
   * scissor (R_SetLightDefViewLight's) united with the chain's rect. A light area past
   * the count (the engine would read past connectedAreas) counts as not connected.
   */
  void addAreaViewLights(const int areaNum, const PortalStack& ps)
  {
    for (const auto index : m_areaLights[std::size_t(areaNum)])
    {
      const auto& light = m_inputs.lights[index];
      // check for being closed off behind a door; a light that doesn't cast shadows
      // will still light even if it is behind a door
      const auto connected = light.areaNum >= 0 && std::size_t(light.areaNum) < m_numAreas
                             && m_result.connectedAreas[std::size_t(light.areaNum)];
      if (!light.castsShadows && light.areaNum != -1 && !connected)
      {
        continue;
      }
      // cull frustum
      if (engineCullCornersByPortals(m_lightCorners[index], ps.planeSpan()))
      {
        // we are culled out through this portal chain, but it might still be visible
        // through others
        continue;
      }
      // R_SetLightDefViewLight: a light's first acceptance links it (LP3.0, D148)
      if (!m_lightSet[index])
      {
        m_acceptedLights.push_back(index);
      }
      unite(m_lightScissors[index], m_lightSet[index], ps.rect);
    }
  }

  static void unite(
    ScreenRect& scissor, std::vector<bool>::reference set, const ScreenRect& rect)
  {
    if (!set)
    {
      // the scissorRect will be expanded as the bounds are accepted into chains
      scissor.clear();
      set = true;
    }
    scissor.unite(rect);
  }

  /**
   * FloodViewThroughArea_r (RenderWorld_portals.cpp:514-647) as an explicit stack in the
   * recursion's order: AddAreaToView, then per portal of the area's list (the .proc's
   * order reversed, AreaInfo::portals): no portal blocks (K30); skip one whose plane has
   * the origin more than 0.1 behind it; skip one whose side is on the stack (the
   * engine's check->p == p compares the side's portal_t, so the other side of a portal
   * on the stack is not skipped by it); within 1.0 of its plane go through with a copy of
   * the stack (the planes and the rect, no clip); else clip a copy of its winding by
   * every -plane of the stack with epsilon 0 (an empty result skips it),
   * PortalIsFoggedOut false (D83), the rect ScreenRectFromWinding intersected with the
   * stack's, the planes through the origin and each edge (at most MAX_PORTAL_PLANES, a
   * degenerate one skipped), the portal's own plane last, and go through into its area.
   */
  void floodViewThroughArea(const int startArea, const PortalStack& ps)
  {
    m_stacks.push_back(ps);
    m_frames.push_back(FloodFrame{startArea, 0});
    addAreaToView(startArea, m_stacks.back());

    while (!m_frames.empty())
    {
      const auto frame = m_frames.size() - 1;
      const auto& list = m_areas.areas[std::size_t(m_frames[frame].area)].portals;
      if (m_frames[frame].nextPortal >= list.size())
      {
        if (const auto side = m_stacks.back().side; side != ViewSide)
        {
          m_onStack[side] = false;
        }
        m_frames.pop_back();
        m_stacks.pop_back();
        continue;
      }
      const auto& p = list[m_frames[frame].nextPortal++];
      const auto side = p.portal * 2 + std::size_t(p.side);
      if (p.intoArea < 0 || std::size_t(p.intoArea) >= m_numAreas)
      {
        continue;
      }

      // make sure this portal is facing away from the view
      const float d = enginePlaneDistance(p.plane, m_origin);
      if (d < -0.1f)
      {
        continue;
      }

      // make sure the portal isn't in our stack trace, which would cause an infinite
      // loop
      if (m_onStack[side])
      {
        continue; // already in stack
      }

      // if we are very close to the portal surface, don't bother clipping it, which
      // tends to give epsilon problems that make the area vanish
      if (d < 1.0f)
      {
        // go through this portal
        auto newStack = m_stacks.back();
        newStack.side = side;
        enter(std::move(newStack), p.intoArea);
        continue;
      }

      // clip the portal winding to all of the planes
      const auto& parent = m_stacks.back();
      auto w = p.points;
      for (auto j = std::size_t{0}; j < parent.numPlanes && !w.empty(); ++j)
      {
        const auto& plane = parent.planes[j];
        auto clipped = engineClipWindingInPlace(
          w, EnginePlane{-plane.a, -plane.b, -plane.c, -plane.d}, 0.0f);
        if (!clipped)
        {
          w.clear();
          break;
        }
        w = std::move(*clipped);
      }
      if (w.empty())
      {
        continue; // portal not visible
      }

      // see if it is fogged out (LP5.3, D123: a pending fog light's portal stays open)
      if (p.portal < m_inputs.portalFog.size())
      {
        const auto& fog = m_inputs.portalFog[p.portal];
        if (
          fog.assigned && !fog.pending
          && enginePortalIsFoggedOut(m_inputs.view, fog.a, p.points))
        {
          ++m_result.portalsFogged;
          continue;
        }
      }

      // go through this portal
      auto newStack = PortalStack{};
      newStack.side = side;

      // find the screen pixel bounding box of the remaining portal so we can scissor
      // things outside it
      newStack.rect = engineScreenRectFromWinding(m_inputs.view, w);

      // slop might have spread it a pixel outside, so trim it back
      newStack.rect.intersect(parent.rect);

      // generate a set of clipping planes that will further restrict the visible view
      // beyond just the scissor rect
      const auto addPlanes = std::min(w.size(), MaxPortalPlanes);
      newStack.numPlanes = 0;
      for (auto i = std::size_t{0}; i < addPlanes; ++i)
      {
        auto j = i + 1;
        if (j == w.size())
        {
          j = 0;
        }
        const auto v1 =
          EngineVec3{m_origin[0] - w[i][0], m_origin[1] - w[i][1], m_origin[2] - w[i][2]};
        const auto v2 =
          EngineVec3{m_origin[0] - w[j][0], m_origin[1] - w[j][1], m_origin[2] - w[j][2]};
        // if it is degenerate, skip the plane
        if (const auto plane = enginePlaneFromCrossThroughPoint(v2, v1, m_origin))
        {
          newStack.planes[newStack.numPlanes++] = *plane;
        }
      }

      // the last stack plane is the portal plane
      newStack.planes[newStack.numPlanes++] = p.plane;

      enter(std::move(newStack), p.intoArea);
    }
  }

  // the recursion's call: the side goes on the stack, the frame's area is added
  void enter(PortalStack stack, const int area)
  {
    m_onStack[stack.side] = true;
    ++m_result.portalsVisited;
    m_stacks.push_back(std::move(stack));
    m_frames.push_back(FloodFrame{area, 0});
    addAreaToView(area, m_stacks.back());
  }
};

} // namespace

std::vector<bool> engineBuildConnectedAreas(const AreaData& areas, const int viewArea)
{
  const auto numAreas = areas.areas.size();
  // if we are outside the world, we can see all areas
  if (viewArea == -1)
  {
    return std::vector<bool>(numAreas, true);
  }

  // start with none visible, and flood fill from the current area
  auto connected = std::vector<bool>(numAreas, false);
  if (viewArea < 0 || std::size_t(viewArea) >= numAreas)
  {
    return connected;
  }
  auto stack = std::vector<std::size_t>{std::size_t(viewArea)};
  while (!stack.empty())
  {
    const auto area = stack.back();
    stack.pop_back();
    if (connected[area])
    {
      continue;
    }
    connected[area] = true;
    // flood through all non-blocked portals
    for (const auto& portal : areas.areas[area].portals)
    {
      if (
        portal.intoArea >= 0 && std::size_t(portal.intoArea) < numAreas
        && !connected[std::size_t(portal.intoArea)])
      {
        stack.push_back(std::size_t(portal.intoArea));
      }
    }
  }
  return connected;
}

ScreenRect engineScreenRectFromWinding(
  const LitView& view, const std::vector<EngineVec3>& winding)
{
  // the viewport is ( 0, 0, width - 1, height - 1 )
  const float viewWidth = float(view.width - 1) - 0.0f;
  const float viewHeight = float(view.height - 1) - 0.0f;

  auto r = ScreenRect{};
  r.clear();
  for (const auto& point : winding)
  {
    const auto ndc = engineGlobalToNdc(view, point);

    const float windowX = (ndc[0] * 0.5f + 0.5f) * viewWidth;
    const float windowY = (ndc[1] * 0.5f + 0.5f) * viewHeight;

    r.addPoint(windowX, windowY);
  }

  r.expand();

  return r;
}

bool engineCullCornersByPortals(
  const EngineFrustumCorners& corners, const std::span<const EnginePlane> planes)
{
  for (const auto& plane : planes)
  {
    if (engineCullFrustumCorners(corners, plane) == EngineFrustumCull::Front)
    {
      return true;
    }
  }
  return false;
}

std::optional<bool> lightCastsShadows(const LightRecord& light)
{
  const auto& shader = light.derived.shader;
  if (shader.pending || !shader.material)
  {
    return std::nullopt;
  }
  // parms.forceShadows || ( ... ): a map light's forceShadows is false
  // (ParseSpawnArgsToRenderLight's memset), so the shader and noShadows decide; LP3.0:
  // the noShadows UpdateLightDef leaves (a spectrum light's forced)
  return !engineLightNoShadows(light) && engineLightCastsShadows(*shader.material);
}

bool engineLightNoShadows(const LightRecord& light)
{
  // new for BFG edition: force noShadows on spectrum lights so teleport spawns don't
  // cause such a slowdown (RenderWorld.cpp:524-529)
  const auto& shader = light.derived.shader;
  const auto spectrum =
    !shader.pending && shader.material && shader.material->spectrum != 0;
  return light.light.parms.noShadows || spectrum;
}

bool lightReachesSurface(
  const AreaLight& light, const AreaSurface& surface, const AreaData& areas)
{
  // D81: the outside set is lit by volume, by every light
  const auto outside =
    surface.kind == AreaSurfaceKind::WorldOutside
    || (surface.kind == AreaSurfaceKind::Entity && surface.refs.empty());
  if (outside)
  {
    return true;
  }
  // a light without references lights only the outside set
  if (light.areas.empty() || !surface.directlyVisible)
  {
    return false;
  }
  // tr_frontend_addlights.cpp:492-501: some lights have their center of projection
  // outside the world, but otherwise areas not connected to the light center (behind a
  // closed door) are ignored
  const auto reaches = [&](const int area) {
    if (std::ranges::find(light.areas, area) == light.areas.end())
    {
      return false;
    }
    // r_useAreasConnectedForShadowCulling 2
    if (light.areaNum != -1)
    {
      return areasConnected(areas.components, light.areaNum, area);
    }
    return true;
  };
  if (surface.kind == AreaSurfaceKind::WorldArea)
  {
    // the world entity N is referenced in area N alone
    return surface.area >= 0 && reaches(surface.area);
  }
  // the entity's references, in its chain order
  return std::ranges::any_of(surface.refs, reaches);
}

bool lightReachesCaster(
  const AreaLight& light, const AreaSurface& surface, const AreaData& areas)
{
  // D81: the outside set by volume, for every light
  const auto outside =
    surface.kind == AreaSurfaceKind::WorldOutside
    || (surface.kind == AreaSurfaceKind::Entity && surface.refs.empty());
  if (outside)
  {
    return true;
  }
  // a light without references casts only on the outside set
  if (light.areas.empty())
  {
    return false;
  }
  // tr_frontend_addlights.cpp:488-501: the areas the light references, those not
  // connected to the light's own area skipped unless its area is -1
  const auto reaches = [&](const int area) {
    if (std::ranges::find(light.areas, area) == light.areas.end())
    {
      return false;
    }
    if (light.areaNum != -1)
    {
      return areasConnected(areas.components, light.areaNum, area);
    }
    return true;
  };
  if (surface.kind == AreaSurfaceKind::WorldArea)
  {
    return surface.area >= 0 && reaches(surface.area);
  }
  return std::ranges::any_of(surface.refs, reaches);
}

std::optional<int> surfaceGridArea(
  const ProcTree& tree,
  const EngineMat4& modelRenderMatrix,
  const EngineBounds& materialBounds,
  const std::span<const int> refs,
  const std::function<bool(int area)>& gridValid)
{
  // RB: use area the surface is in because a model can span multiple areas #965
  const auto triCenter = engineBoundsCenter(materialBounds);
  const auto surfaceCenter = engineTransformPoint(modelRenderMatrix, triCenter);
  const auto surfaceArea = pointInArea(tree, surfaceCenter);

  for (const auto area : refs)
  {
    if (surfaceArea == area && gridValid(area))
    {
      return area;
    }
  }

  // RB: use first valid lightgrid; this would be wrong but less wrong than a flickering
  // env_probe fallback
  for (const auto area : refs)
  {
    if (gridValid(area))
    {
      return area;
    }
  }
  return std::nullopt;
}

PortalFogTable buildPortalFogTable(
  const LightTable& lights, const AreaData& areas, const std::uint64_t lightsVersion)
{
  auto table = PortalFogTable{};
  table.lightsVersion = lightsVersion;
  table.areas = &areas;
  auto portals = std::size_t{0};
  for (const auto& area : areas.areas)
  {
    for (const auto& side : area.portals)
    {
      portals = std::max(portals, side.portal + 1);
    }
  }
  table.fogLights.assign(portals, NoPreviewId);

  // the lights in preview-id order, the engine's creation order's stand-in
  // (M-FLOW-DIFF-01)
  auto records = std::vector<std::shared_ptr<const LightRecord>>{};
  lights.records.forEach(
    [&](const PreviewId&, const std::shared_ptr<const LightRecord>& record) {
      if (record)
      {
        records.push_back(record);
      }
    });
  std::ranges::sort(
    records, [](const auto& lhs, const auto& rhs) { return lhs->id < rhs->id; });

  for (const auto& record : records)
  {
    const auto& shader = record->derived.shader;
    if (
      !record->visible || !record->derived.hasVolume || shader.pending
      || !shader.material)
    {
      continue;
    }
    // R_CreateLightDefFogPortals: only fog lights, and some fog lights will explicitly
    // disallow portal fogging
    if (
      !shader.material->fogLight
      || (shader.material->materialFlags & engine_material::FlagNoPortalFog) != 0)
    {
      continue;
    }
    const auto& baseLightProject = record->derived.projection.baseLightProject;
    for (const auto areaNum : record->derived.areas)
    {
      if (areaNum < 0 || std::size_t(areaNum) >= areas.areas.size())
      {
        continue;
      }
      for (const auto& prt : areas.areas[std::size_t(areaNum)].portals)
      {
        // we only handle a single fog volume covering a portal; this will never cause
        // incorrect drawing, but it may fail to cull a portal
        if (table.fogLights[prt.portal] != NoPreviewId)
        {
          continue;
        }
        // WindingCompletelyInsideLight
        const auto inside = std::ranges::none_of(prt.points, [&](const EngineVec3& p) {
          return engineCullPointToMvp(baseLightProject, p, true);
        });
        if (inside)
        {
          table.fogLights[prt.portal] = record->id;
        }
      }
    }
  }
  return table;
}

bool enginePortalIsFoggedOut(
  const LitView& view, const float a, const std::span<const EngineVec3> winding)
{
  const auto& mv = view.worldModelViewMatrix;
  const auto forward = EnginePlane{
    a * mv[0 * 4 + 2], a * mv[1 * 4 + 2], a * mv[2 * 4 + 2], a * mv[3 * 4 + 2]};
  for (const auto& point : winding)
  {
    const float d = enginePlaneDistance(forward, point);
    if (d < 0.5f)
    {
      return false; // a point not clipped off
    }
  }
  return true;
}

ViewFlowResult flowView(const ViewFlowInputs& inputs)
{
  auto result = ViewFlowResult{};
  // D94: without a tree nothing is flowed; the caller keeps LP2's rules
  if (
    !inputs.tree || inputs.tree->nodes.empty() || !inputs.areas
    || inputs.areas->areas.empty())
  {
    return result;
  }
  const auto& areas = *inputs.areas;
  const auto numAreas = areas.areas.size();
  result.flowed = true;
  result.areaFlowed.assign(numAreas, false);

  // RenderWorld_portals.cpp:775-784: the area to start the portal flooding in
  // (r_usePortals 1), the initialViewAreaOrigin being the vieworg (RenderWorld.cpp:1083)
  result.viewArea = pointInArea(*inputs.tree, inputs.view.viewOrigin);
  if (result.viewArea >= int(numAreas))
  {
    result.viewArea = -1;
  }

  // determine all possible connected areas for light-behind-door culling
  result.connectedAreas = engineBuildConnectedAreas(areas, result.viewArea);

  // tr_frontend_main.cpp:912-921 and viewDef->scissor
  const auto frustum = engineViewFrustumPlanes(inputs.view);
  const auto scissor =
    inputs.scissorForTesting ? *inputs.scissorForTesting : viewScissor(inputs.view);

  // flow through all the portals and add models / lights (r_singleArea 0): the first
  // five frustum planes, the far one left out
  auto flow = Flow{inputs, areas, result};
  flow.flowViewThroughPortals({frustum.data(), 5}, scissor);
  flow.finish();
  return result;
}

} // namespace tb::lit
