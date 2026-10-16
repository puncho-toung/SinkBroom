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

#include "ui/LightVolumeTool.h"

#include "base/Logger.h"
#include "base/Macros.h"
#include "base/PreferenceManager.h"
#include "gl/ActiveShader.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "gl/PrimType.h"
#include "gl/Shaders.h"
#include "gl/Vertex.h"
#include "gl/VertexArray.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "render/LightVolumeMesh.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "render/RenderService.h"
#include "render/Renderable.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/overload.h"

#include "vm/distance.h"
#include "vm/intersection.h"
#include "vm/polygon.h"
#include "vm/segment.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{

// a drawn spot light is at least this high: its zNear of 8 and the engine's minimum zFar
// of 16
constexpr auto MinDrawnSpotHeight = 24.0;

// a drawn volume has no half extent below this, like a box edit (mdl::lightFromEditBox)
constexpr auto MinDrawnHalfExtent = 1.0;

// in 2D, the far cap's centre is not picked when it faces the camera this directly,
// because the drag runs in the view plane
constexpr auto MaxCosine2D = 0.9;

// a Shift drag of the inner shell keeps a core inside the outer shell (D60)
constexpr auto MaxInnerRadius = 0.98;

// the highlight of a surface under the mouse (D60)
constexpr auto SurfaceHighlightAlpha = 0.25f;

// the guards of the light geometry (mdl/LightVolume.cpp)
constexpr auto DegenerateEpsilon = 1e-6;

const auto FarCapSide = BBoxSide{vm::vec3d{0, 0, 1}};

vm::mat3x3d beamMatrix(const mdl::BeamFrame& beam)
{
  auto result = vm::mat3x3d{};
  result[0] = beam.r;
  result[1] = beam.u;
  result[2] = beam.n;
  return result;
}

mdl::EntityRotationPolicy rotationPolicy(const mdl::Map& map)
{
  return map.gameInfo().gameConfig.entityConfig.rotationPolicy;
}

bool isFarCapSide(const LightVolumeBox& volume)
{
  return volume.light.type == mdl::LightType::Spot;
}

vm::segment3d worldEdge(const LightVolumeBox& volume, const BBoxEdge& edge)
{
  const auto points = pointsForBBoxEdge(volume.box, edge);
  return vm::segment3d{volume.toWorld(points.start()), volume.toWorld(points.end())};
}

std::vector<vm::vec3d> worldSide(const LightVolumeBox& volume, const BBoxSide& side)
{
  const auto polygon = polygonForBBoxSide(volume.box, side);

  auto result = std::vector<vm::vec3d>{};
  for (const auto& vertex : polygon.vertices())
  {
    result.push_back(volume.toWorld(vertex));
  }
  return result;
}

/**
 * The sides of the box whose centres are handles in 3D (D58): every side of a point
 * light's or a sun's box; of a spot light's box, the four lateral sides, because its far
 * side has the far cap's centre and the centre of its apex side is the light's own
 * position, where a drag moves the light (R9).
 */
std::vector<BBoxSide> faceCenterSides(const LightVolumeBox& volume)
{
  if (volume.light.type != mdl::LightType::Spot)
  {
    return allSides();
  }
  return {
    BBoxSide{vm::vec3d{1, 0, 0}},
    BBoxSide{vm::vec3d{-1, 0, 0}},
    BBoxSide{vm::vec3d{0, 1, 0}},
    BBoxSide{vm::vec3d{0, -1, 0}},
  };
}

/**
 * The side that a 2D view picks for the given edge of the box (D59): of the two sides
 * that meet at the edge, the one whose normal lies closest to the view plane, so that
 * dragging the box's outline moves one axis, like a brush's edge in a 2D view.
 */
BBoxSide outlineSide(
  const LightVolumeBox& volume, const BBoxEdge& edge, const vm::vec3d& viewDirection)
{
  // the edge runs along the axis in which its points differ; the sides that meet at it
  // face along the other two
  auto result = std::optional<BBoxSide>{};
  auto closestCosine = std::numeric_limits<double>::max();
  for (std::size_t axis = 0; axis < 3; ++axis)
  {
    if (edge.point0[axis] == edge.point1[axis])
    {
      const auto normal = vm::vec3d::axis(axis) * edge.point0[axis];
      const auto cosine = std::abs(vm::dot(volume.rotation * normal, viewDirection));
      if (cosine < closestCosine)
      {
        result = BBoxSide{normal};
        closestCosine = cosine;
      }
    }
  }

  contract_assert(result.has_value());
  return *result;
}

/**
 * Picks the handles of the given volume with the given world pick ray: the corners and,
 * in 3D, the centres of the sides as spheres of twice the handle radius, the far cap's
 * centre of a spot light, and the edges, in 2D as the sides of the box's outline (D58,
 * D59). The sides themselves are not picked: a click inside a side reaches what is
 * behind it. Everything is tested in world space. If the camera is inside the box, only
 * the handles in front of it are picked.
 */
mdl::PickResult pickVolume(
  const LightVolumeBox& volume,
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const bool twoD)
{
  const auto handleRadius = double(pref(Preferences::HandleRadius));
  const auto boxRay = volume.toBox(pickRay);
  const auto inside = !twoD && volume.box.contains(boxRay.origin);
  const auto inFront = [&](const vm::vec3d& point) {
    return !inside || vm::dot(point - pickRay.origin, pickRay.direction) > 0.0;
  };

  auto result = mdl::PickResult{};

  const auto pickPoint =
    [&](const vm::vec3d& point, const mdl::HitType::Type type, auto target) {
      if (inFront(point))
      {
        if (
          const auto distance =
            camera.pickPointHandle(pickRay, point, handleRadius * 2.0))
        {
          result.addHit(
            mdl::Hit{type, *distance, vm::point_at_distance(pickRay, *distance), target});
        }
      }
    };

  for (const auto& corner : allCorners())
  {
    pickPoint(
      volume.toWorld(pointForBBoxCorner(volume.box, corner)),
      LightVolumeTool::CornerHitType,
      corner);
  }

  if (isFarCapSide(volume))
  {
    const auto beam = volume.rotation * FarCapSide.normal;
    if (!twoD || std::abs(vm::dot(beam, pickRay.direction)) <= MaxCosine2D)
    {
      pickPoint(
        volume.toWorld(centerForBBoxSide(volume.box, FarCapSide)),
        LightVolumeTool::FarCapCenterHitType,
        FarCapSide);
    }
  }

  if (!twoD)
  {
    for (const auto& side : faceCenterSides(volume))
    {
      pickPoint(
        volume.toWorld(centerForBBoxSide(volume.box, side)),
        LightVolumeTool::FaceCenterHitType,
        side);
    }
  }

  for (const auto& edge : allEdges())
  {
    const auto segment = worldEdge(volume, edge);
    if (twoD && vm::is_parallel(segment.direction(), pickRay.direction))
    {
      continue;
    }

    if (
      const auto distance = camera.pickLineSegmentHandle(pickRay, segment, handleRadius))
    {
      const auto closest = vm::distance(pickRay, segment);
      if (inFront(vm::point_at_distance(segment, closest.position2)))
      {
        const auto hitPoint = vm::point_at_distance(pickRay, *distance);
        result.addHit(
          twoD ? mdl::Hit{LightVolumeTool::SideHitType, *distance, hitPoint, outlineSide(volume, edge, pickRay.direction)}
               : mdl::Hit{LightVolumeTool::EdgeHitType, *distance, hitPoint, edge});
      }
    }
  }

  return result;
}

vm::bbox3d moveBox(
  const vm::bbox3d& box,
  const mdl::Hit& hit,
  const vm::vec3d& delta,
  const ProportionalAxes& proportionalAxes,
  const AnchorPos anchorPos)
{
  if (
    hit.type() == LightVolumeTool::SideHitType
    || hit.type() == LightVolumeTool::FaceCenterHitType)
  {
    return moveBBoxSide(box, hit.target<BBoxSide>(), delta, proportionalAxes, anchorPos);
  }
  if (hit.type() == LightVolumeTool::EdgeHitType)
  {
    return moveBBoxEdge(box, hit.target<BBoxEdge>(), delta, proportionalAxes, anchorPos);
  }
  if (hit.type() == LightVolumeTool::CornerHitType)
  {
    return moveBBoxCorner(box, hit.target<BBoxCorner>(), delta, anchorPos);
  }

  // the far cap's centre moves only the far cap along the beam
  contract_assert(hit.type() == LightVolumeTool::FarCapCenterHitType);
  return moveBBoxSide(
    box, FarCapSide, delta, ProportionalAxes::None(), AnchorPos::Opposite);
}

/**
 * The side of a point light's full box whose axis the outer shell's normal at the hit
 * point runs mostly along; the normal of the ellipsoid there is (x/rx^2, y/ry^2, z/rz^2).
 */
BBoxSide shellSide(const LightSurfaceHit& surface)
{
  const auto radius = vm::abs(surface.light.radius);
  const auto& point = surface.localPoint;
  const auto normal = vm::vec3d{
    point.x() / (radius.x() * radius.x()),
    point.y() / (radius.y() * radius.y()),
    point.z() / (radius.z() * radius.z())};
  const auto axis = vm::find_abs_max_component(normal);
  return BBoxSide{vm::vec3d::axis(axis) * (normal[axis] < 0.0 ? -1.0 : 1.0)};
}

/**
 * The hit point of a surface in the box frame of the given volume.
 */
vm::vec3d surfacePointInBox(const LightVolumeBox& volume, const LightSurfaceHit& surface)
{
  return vm::transpose(volume.rotation) * (volume.frame.R * surface.localPoint);
}

/**
 * Whether a Shift drag edits the given element of the given light (D60): a point light's
 * outer shell and, if it is drawn, its inner shell; a spot light's funnel and base.
 */
bool isEditableElement(const mdl::LightElement element, const mdl::LightProperties& light)
{
  switch (element)
  {
  case mdl::LightElement::OuterShell:
    return light.type == mdl::LightType::Point;
  case mdl::LightElement::InnerShell:
    return light.type == mdl::LightType::Point && render::lightVolumeHasInnerShell(light);
  case mdl::LightElement::Funnel:
  case mdl::LightElement::Base:
    return light.type == mdl::LightType::Spot;
  case mdl::LightElement::Penumbra:
  case mdl::LightElement::SunBox:
  case mdl::LightElement::Band:
  case mdl::LightElement::NearCap:
    return false;
    switchDefault();
  }
}

/**
 * The light that a Shift drag of the given surface by the given world delta makes of the
 * light at the start of the drag (D60), or nothing if the step is denied:
 * - the outer shell moves the side of the full box (t = 1) along the axis of its normal
 *   at the hit point, so the shell follows the mouse and the opposite side (or with Alt
 *   the centre) stays;
 * - the inner shell's radius becomes the falloff parameter of the moved hit point;
 * - the funnel widens or narrows the far cap by the factor of the hit point's distance
 *   from the beam, the depth unchanged;
 * - the base moves the far cap along the beam like the far cap's centre.
 */
std::optional<mdl::LightProperties> editSurface(
  const LightVolumeBox& start,
  const LightSurfaceHit& surface,
  const vm::vec3d& delta,
  const AnchorPos anchorPos)
{
  const auto toLocal = vm::transpose(start.frame.R);
  const auto toBox = vm::transpose(start.rotation);

  const auto checked = [&](
                         const vm::bbox3d& from,
                         const vm::bbox3d& to) -> std::optional<mdl::LightProperties> {
    if (to.is_empty())
    {
      return std::nullopt;
    }
    // the rotation is the light's, never the box frame's (I0.2)
    const auto light = mdl::lightFromEditBox(start.light, start.frame.R, to);
    return light == start.light && to != from ? std::nullopt : std::optional{light};
  };

  switch (surface.element)
  {
  case mdl::LightElement::OuterShell: {
    const auto box = mdl::pointLightLocalBox(start.light);
    return checked(
      box,
      moveBBoxSide(
        box, shellSide(surface), toLocal * delta, ProportionalAxes::None(), anchorPos));
  }
  case mdl::LightElement::InnerShell: {
    const auto point = surface.localPoint + toLocal * delta;
    auto light = start.light;
    light.math.innerRadius = std::clamp(
      mdl::pointFalloffT(point, vm::abs(start.light.radius)), 0.0, MaxInnerRadius);
    return light;
  }
  case mdl::LightElement::Funnel: {
    const auto point = surfacePointInBox(start, surface);
    const auto lateral = vm::vec3d{point.x(), point.y(), 0.0};
    const auto distance = vm::length(lateral);
    if (distance < DegenerateEpsilon)
    {
      return std::nullopt;
    }

    const auto factor =
      (distance + vm::dot(lateral / distance, toBox * delta)) / distance;
    if (factor <= 0.0)
    {
      return std::nullopt;
    }

    const auto scale = vm::vec3d{factor, factor, 1.0};
    return checked(start.box, vm::bbox3d{start.box.min * scale, start.box.max * scale});
  }
  case mdl::LightElement::Base:
    return checked(
      start.box,
      moveBBoxSide(
        start.box,
        FarCapSide,
        toBox * delta,
        ProportionalAxes::None(),
        AnchorPos::Opposite));
  case mdl::LightElement::Penumbra:
  case mdl::LightElement::SunBox:
  case mdl::LightElement::Band:
  case mdl::LightElement::NearCap:
    return std::nullopt;
    switchDefault();
  }
}

/**
 * The bounding sphere of a light's volume in the world, for the pre-test of the surface
 * picks (D57).
 */
std::pair<vm::vec3d, double> boundingSphere(
  const mdl::LightProperties& light, const mdl::LightFrame& frame)
{
  switch (light.type)
  {
  case mdl::LightType::Point:
    return {frame.origin, vm::get_max_component(vm::abs(light.radius))};
  case mdl::LightType::Sun:
    return {frame.origin, vm::length(light.radius)};
  case mdl::LightType::Spot: {
    // the near corners lie between the apex and the far corners
    auto points = std::vector<vm::vec3d>{frame.origin};
    if (const auto frustum = mdl::spotLightFrustum(light))
    {
      for (const auto& corner : frustum->farCorners)
      {
        points.push_back(frame.origin + frame.R * corner);
      }
    }

    const auto centre = vm::bbox3d::build(points)->center();
    auto radius = 0.0;
    for (const auto& point : points)
    {
      radius = std::max(radius, vm::length(point - centre));
    }
    return {centre, radius};
  }
    switchDefault();
  }
}

/**
 * The world triangles of a spot light's funnel or a sun's box, which are picked on the
 * mesh (D57); nothing for a point light, whose shells are picked as ellipsoids.
 */
std::vector<vm::vec3d> pickedTriangles(
  const mdl::LightProperties& light, const mdl::LightFrame& frame)
{
  auto result = std::vector<vm::vec3d>{};
  if (light.type != mdl::LightType::Point)
  {
    // one band is the same surface as many: the rings lie on the cone's straight lines
    const auto mesh = render::buildLightVolumeMesh(
      light, frame, render::LightVolumeStyle{1.0, 1, false, true});
    for (const auto& range : mesh.ranges)
    {
      if (
        range.element == mdl::LightElement::Funnel
        || range.element == mdl::LightElement::SunBox)
      {
        for (std::size_t i = 0; i < range.count; ++i)
        {
          result.emplace_back(
            gl::getVertexComponent<0>(mesh.triangles[range.firstTriangleVertex + i]));
        }
      }
    }
  }
  return result;
}

/**
 * The nearest of the given triangles that the given ray enters from outside: the
 * triangles are CW seen from outside, so their normal cross(p3 - p1, p2 - p1) points
 * against the ray.
 */
std::optional<double> intersectFrontTriangles(
  const std::vector<vm::vec3d>& triangles, const vm::ray3d& ray)
{
  auto result = std::optional<double>{};
  for (std::size_t i = 0; i + 2 < triangles.size(); i += 3)
  {
    const auto& p1 = triangles[i];
    const auto& p2 = triangles[i + 1];
    const auto& p3 = triangles[i + 2];
    if (vm::dot(vm::cross(p3 - p1, p2 - p1), ray.direction) < 0.0)
    {
      if (const auto distance = vm::intersect_ray_triangle(ray, p1, p2, p3);
          distance && (!result || *distance < *result))
      {
        result = distance;
      }
    }
  }
  return result;
}

/**
 * The nearest of two optional distances, with the element of each.
 */
std::optional<std::pair<double, mdl::LightElement>> nearer(
  const std::optional<std::pair<double, mdl::LightElement>>& lhs,
  const std::optional<std::pair<double, mdl::LightElement>>& rhs)
{
  if (!lhs)
  {
    return rhs;
  }
  if (!rhs)
  {
    return lhs;
  }
  return rhs->first < lhs->first ? rhs : lhs;
}

std::optional<std::pair<double, mdl::LightElement>> withElement(
  const std::optional<double>& distance, const mdl::LightElement element)
{
  return distance ? std::optional{std::pair{*distance, element}} : std::nullopt;
}

/**
 * The volume of a light being drawn or a highlighted surface, built once for the frame in
 * which it is rendered: its fills with the light volume renderer's state (D43, D46) and
 * the given culling, and its lines.
 */
class VolumeRenderable : public render::DirectRenderable
{
public:
  enum class Cull
  {
    None,
    Back,
    Front,
  };

private:
  gl::VertexArray m_fills;
  gl::VertexArray m_lines;
  Cull m_cull;

public:
  VolumeRenderable(
    std::vector<gl::VertexTypes::P3C4::Vertex> fills,
    std::vector<gl::VertexTypes::P3C4::Vertex> lines,
    const Cull cull)
    : m_fills{gl::VertexArray::move(std::move(fills))}
    , m_lines{gl::VertexArray::move(std::move(lines))}
    , m_cull{cull}
  {
  }

  void prepare(gl::Gl& gl, gl::VboManager& vboManager) override
  {
    m_fills.prepare(gl, vboManager);
    m_lines.prepare(gl, vboManager);
  }

  void render(render::RenderContext& renderContext) override
  {
    auto& gl = renderContext.gl();

    // the state of the light volume renderer's fills (D43, D46)
    gl.pushAttrib(GL_POLYGON_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);
    if (renderContext.render2D())
    {
      gl.disable(GL_DEPTH_TEST);
    }
    else
    {
      gl.enable(GL_DEPTH_TEST);
    }
    gl.depthMask(GL_FALSE);
    gl.frontFace(GL_CW);
    if (m_cull == Cull::None)
    {
      gl.disable(GL_CULL_FACE);
    }
    else
    {
      gl.enable(GL_CULL_FACE);
      gl.cullFace(m_cull == Cull::Front ? GL_FRONT : GL_BACK);
    }
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);

    {
      auto shader =
        gl::ActiveShader{gl, renderContext.shaderManager(), gl::Shaders::VaryingPCShader};
      if (m_fills.setup(gl, shader.program()))
      {
        m_fills.render(gl, gl::PrimType::Triangles);
        m_fills.cleanup(gl, shader.program());
      }
      if (m_lines.setup(gl, shader.program()))
      {
        m_lines.render(gl, gl::PrimType::Lines);
        m_lines.cleanup(gl, shader.program());
      }
    }

    gl.popAttrib();
  }
};

/**
 * Removes the rotation properties, so that a drawn volume is aligned with the world (the
 * light editor plan, D11).
 */
void removeRotationProperties(mdl::Entity& entity)
{
  for (const auto& key :
       {mdl::EntityPropertyKeys::Angle,
        mdl::EntityPropertyKeys::Angles,
        mdl::EntityPropertyKeys::Rotation,
        mdl::EntityPropertyKeys::LightRotation,
        mdl::EntityPropertyKeys::LightAngles})
  {
    if (entity.hasProperty(key))
    {
      entity.removeProperty(key);
    }
  }
}

const mdl::EntityDefinition* lightDefinition(const mdl::Map& map)
{
  const auto* definition = map.entityDefinitionManager().definition("light");
  return definition && mdl::getType(*definition) == mdl::EntityDefinitionType::Point
           ? definition
           : nullptr;
}

std::vector<vm::vec3f> boxEdgeVertices(const LightVolumeBox& volume)
{
  auto result = std::vector<vm::vec3f>{};
  result.reserve(24);
  for (const auto& edge : allEdges())
  {
    const auto segment = worldEdge(volume, edge);
    result.emplace_back(segment.start());
    result.emplace_back(segment.end());
  }
  return result;
}

std::vector<vm::vec3f> frustumEdgeVertices(const LightVolumeBox& volume)
{
  auto result = std::vector<vm::vec3f>{};
  if (const auto frustum = mdl::spotLightFrustum(volume.light))
  {
    const auto toWorld = [&](const vm::vec3d& local) {
      return vm::vec3f{volume.frame.origin + volume.frame.R * local};
    };

    result.reserve(24);
    for (size_t i = 0; i < 4; ++i)
    {
      const auto j = (i + 1) % 4;
      result.push_back(toWorld(frustum->nearCorners[i]));
      result.push_back(toWorld(frustum->nearCorners[j]));
      result.push_back(toWorld(frustum->farCorners[i]));
      result.push_back(toWorld(frustum->farCorners[j]));
      result.push_back(toWorld(frustum->nearCorners[i]));
      result.push_back(toWorld(frustum->farCorners[i]));
    }
  }
  return result;
}

std::vector<vm::vec3f> handlePositions(const LightVolumeBox& volume, const bool threeD)
{
  auto result = std::vector<vm::vec3f>{};
  for (const auto& corner : allCorners())
  {
    result.emplace_back(volume.toWorld(pointForBBoxCorner(volume.box, corner)));
  }
  if (threeD)
  {
    for (const auto& side : faceCenterSides(volume))
    {
      result.emplace_back(volume.toWorld(centerForBBoxSide(volume.box, side)));
    }
  }
  if (isFarCapSide(volume))
  {
    result.emplace_back(volume.toWorld(centerForBBoxSide(volume.box, FarCapSide)));
  }
  return result;
}

void renderHighlight(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const LightVolumeBox& volume,
  const mdl::Hit& hit)
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setForegroundColor(pref(Preferences::LightVolumeHandleColor));
  renderService.setLineWidth(2.0f);

  if (hit.type() == LightVolumeTool::SideHitType)
  {
    auto vertices = std::vector<vm::vec3f>{};
    for (const auto& vertex : worldSide(volume, hit.target<BBoxSide>()))
    {
      vertices.emplace_back(vertex);
    }
    renderService.renderPolygonOutline(vertices);
  }
  else if (hit.type() == LightVolumeTool::EdgeHitType)
  {
    const auto segment = worldEdge(volume, hit.target<BBoxEdge>());
    renderService.renderLine(vm::vec3f{segment.start()}, vm::vec3f{segment.end()});
  }
  else if (hit.type() == LightVolumeTool::CornerHitType)
  {
    renderService.renderHandleHighlight(vm::vec3f{
      volume.toWorld(pointForBBoxCorner(volume.box, hit.target<BBoxCorner>()))});
  }
  else if (
    hit.type() == LightVolumeTool::FarCapCenterHitType
    || hit.type() == LightVolumeTool::FaceCenterHitType)
  {
    renderService.renderHandleHighlight(
      vm::vec3f{volume.toWorld(centerForBBoxSide(volume.box, hit.target<BBoxSide>()))});
  }
}

/**
 * The outline of a surface where it has one (D60): the far rectangle of a spot light's
 * base, the edges of a sun's box.
 */
std::vector<vm::vec3f> surfaceOutline(const LightSurfaceHit& surface)
{
  const auto toWorld = [&](const vm::vec3d& local) {
    return vm::vec3f{surface.frame.origin + surface.frame.R * local};
  };

  auto result = std::vector<vm::vec3f>{};
  if (surface.element == mdl::LightElement::Base)
  {
    if (const auto frustum = mdl::spotLightFrustum(surface.light))
    {
      for (std::size_t i = 0; i < 4; ++i)
      {
        result.push_back(toWorld(frustum->farCorners[i]));
        result.push_back(toWorld(frustum->farCorners[(i + 1) % 4]));
      }
    }
  }
  else if (surface.element == mdl::LightElement::SunBox)
  {
    const auto box = mdl::pointLightLocalBox(surface.light);
    for (const auto& edge : allEdges())
    {
      const auto points = pointsForBBoxEdge(box, edge);
      result.push_back(toWorld(points.start()));
      result.push_back(toWorld(points.end()));
    }
  }
  return result;
}

/**
 * Whether the camera is inside the closed surface of the given element, so that its
 * highlight culls its front faces like the light volume renderer (D43).
 */
bool containsCamera(const LightSurfaceHit& surface, const vm::vec3d& camera)
{
  if (surface.element == mdl::LightElement::InnerShell)
  {
    const auto local = vm::transpose(surface.frame.R) * (camera - surface.frame.origin);
    return mdl::pointFalloffT(local, vm::abs(surface.light.radius))
           <= surface.light.math.innerRadius;
  }
  return mdl::containsPoint(surface.light, surface.frame, camera);
}

/**
 * Renders the highlight of a surface (D60): its element's triangles once more in the
 * selected face colour at alpha 0.25, depth tested and without writing depth, and its
 * outline where it has one, bold if the surface was clicked.
 */
void renderSurfaceHighlight(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const LightSurfaceHit& surface,
  const bool bold)
{
  // the element is built at full alpha, so that it exists even where the volume is
  // invisible, like the far cap of most spot lights
  const auto mesh = render::buildLightVolumeMesh(
    surface.light,
    surface.frame,
    render::LightVolumeStyle{
      1.0, std::clamp(pref(Preferences::LightVolumeShells), 2, 8), false, false});
  const auto range = std::ranges::find_if(
    mesh.ranges, [&](const auto& r) { return r.element == surface.element; });
  if (range == mesh.ranges.end())
  {
    return;
  }

  auto color = pref(Preferences::SelectedFaceColor).to<RgbaF>().toVec();
  color[3] = SurfaceHighlightAlpha;

  auto fills = std::vector<gl::VertexTypes::P3C4::Vertex>{};
  fills.reserve(range->count);
  for (std::size_t i = 0; i < range->count; ++i)
  {
    fills.emplace_back(
      gl::getVertexComponent<0>(mesh.triangles[range->firstTriangleVertex + i]), color);
  }

  // closed surfaces are seen from one side only, open ones from both
  const auto closed = surface.element == mdl::LightElement::OuterShell
                      || surface.element == mdl::LightElement::InnerShell
                      || surface.element == mdl::LightElement::SunBox;
  const auto cull =
    !closed ? VolumeRenderable::Cull::None
    : containsCamera(surface, vm::vec3d{renderContext.camera().position()})
      ? VolumeRenderable::Cull::Front
      : VolumeRenderable::Cull::Back;
  renderBatch.addOneShot(new VolumeRenderable{std::move(fills), {}, cull});

  if (const auto outline = surfaceOutline(surface); !outline.empty())
  {
    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.setForegroundColor(pref(Preferences::SelectedFaceColor));
    renderService.setLineWidth(bold ? 2.0f : 1.0f);
    renderService.renderLines(outline);
  }
}

/**
 * The volume of the given light entity with the given box, or nothing if the box or the
 * beam frame is degenerate.
 */
std::optional<LightVolumeBox> volumeOf(
  const mdl::Entity& entity,
  const mdl::EntityRotationPolicy policy,
  const mdl::LightProperties& light,
  const std::optional<vm::bbox3d>& box)
{
  if (!box)
  {
    return std::nullopt;
  }

  const auto frame = mdl::lightFrame(entity, policy);
  auto rotation = frame.R;
  if (light.type == mdl::LightType::Spot)
  {
    const auto beam = mdl::beamFrame(light);
    if (!beam)
    {
      return std::nullopt;
    }
    rotation = frame.R * beamMatrix(*beam);
  }

  return LightVolumeBox{light, frame, rotation, *box};
}

} // namespace

vm::vec3d LightVolumeBox::toWorld(const vm::vec3d& point) const
{
  return frame.origin + rotation * point;
}

vm::vec3d LightVolumeBox::toBox(const vm::vec3d& world) const
{
  return vm::transpose(rotation) * (world - frame.origin);
}

vm::ray3d LightVolumeBox::toBox(const vm::ray3d& ray) const
{
  return vm::ray3d{toBox(ray.origin), vm::transpose(rotation) * ray.direction};
}

std::optional<LightVolumeBox> lightVolumeBox(
  const mdl::Entity& entity, const mdl::EntityRotationPolicy policy)
{
  const auto light = mdl::parseLightProperties(entity);
  return volumeOf(entity, policy, light, mdl::lightEditBox(light));
}

std::optional<LightVolumeBox> lightVolumeBox(
  const mdl::Entity& entity,
  const mdl::EntityRotationPolicy policy,
  const double threshold)
{
  const auto light = mdl::parseLightProperties(entity);
  return volumeOf(entity, policy, light, mdl::lightEditBox(light, threshold));
}

vm::line3d lightVolumeHandleLine(const LightVolumeBox& volume, const mdl::Hit& hit)
{
  contract_pre(hit.hasType(LightVolumeTool::AnyHitType));

  const auto& box = volume.box;

  if (hit.type() == LightVolumeTool::SurfaceHitType)
  {
    const auto surface = hit.target<LightSurfaceHit>();
    const auto direction = [&]() {
      switch (surface.element)
      {
      case mdl::LightElement::OuterShell:
        return volume.frame.R * shellSide(surface).normal;
      case mdl::LightElement::InnerShell:
        return vm::normalize(volume.frame.R * surface.localPoint);
      case mdl::LightElement::Funnel: {
        const auto point = surfacePointInBox(volume, surface);
        return volume.rotation * vm::normalize(vm::vec3d{point.x(), point.y(), 0.0});
      }
      case mdl::LightElement::Base:
      case mdl::LightElement::Penumbra:
      case mdl::LightElement::SunBox:
      case mdl::LightElement::Band:
      case mdl::LightElement::NearCap:
        return volume.rotation * FarCapSide.normal;
        switchDefault();
      }
    }();
    return vm::line3d{hit.hitPoint(), direction};
  }

  // the lines of the scale tool's handleLineForHit in the box frame; they all run
  // through the box's centre, so the anchor does not change them
  if (
    hit.type() == LightVolumeTool::SideHitType
    || hit.type() == LightVolumeTool::FaceCenterHitType
    || hit.type() == LightVolumeTool::FarCapCenterHitType)
  {
    const auto side = hit.target<BBoxSide>();
    return vm::line3d{
      volume.toWorld(centerForBBoxSide(box, side)), volume.rotation * side.normal};
  }

  auto start = vm::vec3d{};
  auto end = vm::vec3d{};
  if (hit.type() == LightVolumeTool::EdgeHitType)
  {
    const auto edge = hit.target<BBoxEdge>();
    start = pointsForBBoxEdge(box, oppositeEdge(edge)).center();
    end = pointsForBBoxEdge(box, edge).center();
  }
  else
  {
    contract_assert(hit.type() == LightVolumeTool::CornerHitType);

    const auto corner = hit.target<BBoxCorner>();
    start = pointForBBoxCorner(box, oppositeCorner(corner));
    end = pointForBBoxCorner(box, corner);
  }

  return vm::line3d{volume.toWorld(start), volume.rotation * vm::normalize(end - start)};
}

LightVolumeHandles::LightVolumeHandles(LightVolumeTool& tool)
  : Tool{true}
  , m_tool{tool}
{
}

bool LightVolumeHandles::doDeactivate()
{
  // a modal tool replaced the handles: a drag ends, and its tracker ends quietly
  m_tool.cancelDrag();
  m_tool.setHighlightedHit(mdl::Hit::NoHit);
  return true;
}

const mdl::HitType::Type LightVolumeTool::SideHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::EdgeHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::CornerHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::FarCapCenterHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::FaceCenterHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::SurfaceHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::HandleHitType =
  LightVolumeTool::SideHitType | LightVolumeTool::EdgeHitType
  | LightVolumeTool::CornerHitType | LightVolumeTool::FarCapCenterHitType
  | LightVolumeTool::FaceCenterHitType;
const mdl::HitType::Type LightVolumeTool::AnyHitType =
  LightVolumeTool::HandleHitType | LightVolumeTool::SurfaceHitType;

LightVolumeTool::LightVolumeTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
  , m_handles{*this}
{
  // the lights whose surfaces are picked are read again after any change that could
  // remove, move, hide, select or turn a light into something else
  const auto invalidate = [&]() { lightsDidChange(); };
  const auto invalidateNodes = [&](const auto&) { lightsDidChange(); };

  m_notifierConnection += document.documentWasLoadedNotifier.connect(invalidate);
  m_notifierConnection += document.nodesWereAddedNotifier.connect(invalidateNodes);
  m_notifierConnection += document.nodesWillBeRemovedNotifier.connect(invalidateNodes);
  m_notifierConnection += document.nodesWereRemovedNotifier.connect(invalidateNodes);
  m_notifierConnection += document.nodesDidChangeNotifier.connect(invalidateNodes);
  m_notifierConnection +=
    document.nodeVisibilityDidChangeNotifier.connect(invalidateNodes);
  m_notifierConnection += document.editorContextDidChangeNotifier.connect(invalidate);
  m_notifierConnection += document.entityDefinitionsDidChangeNotifier.connect(invalidate);
  m_notifierConnection += document.groupWasOpenedNotifier.connect(invalidate);
  m_notifierConnection += document.groupWasClosedNotifier.connect(invalidate);
  m_notifierConnection += document.selectionDidChangeNotifier.connect([&](const auto&) {
    invalidatePickableLights();
    m_clickedNode = nullptr;
    m_clickedElement = std::nullopt;
  });
  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const auto& path) {
        if (path == Preferences::ShowLightVolumes.path)
        {
          lightsDidChange();
        }
      });
}

LightVolumeTool::~LightVolumeTool() = default;

const mdl::Grid& LightVolumeTool::grid() const
{
  return m_document.map().grid();
}

LightVolumeHandles& LightVolumeTool::handles()
{
  return m_handles;
}

const LightVolumeHandles& LightVolumeTool::handles() const
{
  return m_handles;
}

bool LightVolumeTool::showsVolumes() const
{
  return mdl::hasLightEditor(m_document.map().gameInfo().gameConfig)
         && pref(Preferences::ShowLightVolumes) != Preferences::ShowLightVolumesOff;
}

double LightVolumeTool::handleThreshold() const
{
  return std::clamp(double(pref(Preferences::LightVolumeHandleThreshold)), 0.01, 0.5);
}

LightVolumeTool::Mode LightVolumeTool::mode() const
{
  return m_mode;
}

void LightVolumeTool::setMode(const Mode mode)
{
  if (mode != m_mode)
  {
    resetDraw();
    m_highlightedHit = mdl::Hit::NoHit;
    m_mode = mode;
    modeDidChangeNotifier();
    refreshViews();
  }
}

bool LightVolumeTool::drawing() const
{
  return active() && m_mode == Mode::Draw;
}

bool LightVolumeTool::isDragging() const
{
  return m_dragging;
}

mdl::EntityNode* LightVolumeTool::selectedLight() const
{
  const auto& map = m_document.map();
  if (!mdl::hasLightEditor(map.gameInfo().gameConfig))
  {
    return nullptr;
  }

  const auto& selection = map.selection();
  if (selection.nodes.size() != 1 || selection.entities.size() != 1)
  {
    return nullptr;
  }

  auto* entityNode = selection.entities.front();
  return mdl::isLightEntity(entityNode->entity()) ? entityNode : nullptr;
}

std::optional<LightVolumeBox> LightVolumeTool::volumeBox() const
{
  return volumeBox(handleThreshold());
}

std::optional<LightVolumeBox> LightVolumeTool::volumeBox(const double threshold) const
{
  if (const auto* entityNode = selectedLight())
  {
    auto volume =
      lightVolumeBox(entityNode->entity(), rotationPolicy(m_document.map()), threshold);
    if (volume && !volume->light.hasLightOrigin)
    {
      return volume;
    }
  }
  return std::nullopt;
}

std::optional<vm::bbox3d> LightVolumeTool::editBox() const
{
  if (const auto volume = volumeBox())
  {
    return volume->box;
  }
  return std::nullopt;
}

std::optional<mdl::LightFrame> LightVolumeTool::frame() const
{
  if (const auto volume = volumeBox())
  {
    return volume->frame;
  }
  return std::nullopt;
}

void LightVolumeTool::pick3D(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult) const
{
  if (showsVolumes())
  {
    if (const auto volume = volumeBox())
    {
      const auto hits = pickVolume(*volume, pickRay, camera, false);
      if (!hits.empty())
      {
        pickResult.addHit(hits.all().front());
      }
    }
  }
}

void LightVolumeTool::pick2D(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult) const
{
  if (showsVolumes())
  {
    if (const auto volume = volumeBox())
    {
      const auto hits = pickVolume(*volume, pickRay, camera, true);
      if (!hits.empty())
      {
        pickResult.addHit(hits.all().front());
      }
    }
  }
}

void LightVolumeTool::pickSurfaces(
  const vm::ray3d& pickRay, mdl::PickResult& pickResult) const
{
  using namespace mdl::HitFilters;

  // an unselected light's surface yields to any node along the ray (D57)
  const auto hasNodeHit = pickResult.first(type(mdl::nodeHitType())).isMatch();

  for (const auto& pickable : pickableLights())
  {
    if (
      (!pickable.selected && hasNodeHit)
      || !vm::intersect_ray_sphere(pickRay, pickable.centre, pickable.radius))
    {
      continue;
    }

    const auto& light = pickable.light;
    const auto& frame = pickable.frame;
    auto hit = std::optional<std::pair<double, mdl::LightElement>>{};

    switch (light.type)
    {
    case mdl::LightType::Point: {
      // a shell is picked where the ray enters it, like a brush face; the inner shell,
      // which the outer one encloses, wins where the ray reaches it
      const auto radius = vm::abs(light.radius);
      const auto origin = vm::transpose(frame.R) * (pickRay.origin - frame.origin);
      const auto entering = [&](const double t) {
        return mdl::pointFalloffT(origin, radius) > t
                 ? mdl::intersectRayEllipsoid(frame, radius, t, pickRay)
                 : std::nullopt;
      };

      if (render::lightVolumeHasInnerShell(light))
      {
        hit =
          withElement(entering(light.math.innerRadius), mdl::LightElement::InnerShell);
      }
      if (!hit)
      {
        hit = withElement(entering(1.0), mdl::LightElement::OuterShell);
      }
      break;
    }
    case mdl::LightType::Spot: {
      // the base faces along the beam
      const auto beam = mdl::spotLightFrustum(light);
      const auto base = beam && vm::dot(frame.R * beam->n, pickRay.direction) < 0.0
                          ? mdl::intersectRaySpotBase(light, frame, pickRay)
                          : std::nullopt;
      hit = nearer(
        withElement(base, mdl::LightElement::Base),
        withElement(
          intersectFrontTriangles(pickable.triangles, pickRay),
          mdl::LightElement::Funnel));
      break;
    }
    case mdl::LightType::Sun:
      hit = withElement(
        intersectFrontTriangles(pickable.triangles, pickRay), mdl::LightElement::SunBox);
      break;
      switchDefault();
    }

    if (hit)
    {
      const auto [distance, element] = *hit;
      const auto hitPoint = vm::point_at_distance(pickRay, distance);
      pickResult.addHit(mdl::Hit{
        SurfaceHitType,
        distance,
        hitPoint,
        LightSurfaceHit{
          pickable.node,
          element,
          pickable.selected,
          light,
          frame,
          vm::transpose(frame.R) * (hitPoint - frame.origin),
        }});
    }
  }
}

void LightVolumeTool::setAnchorPos(const AnchorPos anchorPos)
{
  m_anchorPos = anchorPos;
}

AnchorPos LightVolumeTool::anchorPos() const
{
  return m_anchorPos;
}

void LightVolumeTool::setProportionalAxes(const ProportionalAxes& proportionalAxes)
{
  m_proportionalAxes = proportionalAxes;
}

const ProportionalAxes& LightVolumeTool::proportionalAxes() const
{
  return m_proportionalAxes;
}

const mdl::Hit& LightVolumeTool::highlightedHit() const
{
  return m_highlightedHit;
}

void LightVolumeTool::setHighlightedHit(const mdl::Hit& hit)
{
  const auto sameTarget = [&]() {
    if (
      hit.type() == SideHitType || hit.type() == FaceCenterHitType
      || hit.type() == FarCapCenterHitType)
    {
      return hit.target<BBoxSide>() == m_highlightedHit.target<BBoxSide>();
    }
    if (hit.type() == EdgeHitType)
    {
      return hit.target<BBoxEdge>() == m_highlightedHit.target<BBoxEdge>();
    }
    if (hit.type() == CornerHitType)
    {
      return hit.target<BBoxCorner>() == m_highlightedHit.target<BBoxCorner>();
    }
    if (hit.type() == SurfaceHitType)
    {
      const auto& surface = hit.target<LightSurfaceHit>();
      const auto& highlighted = m_highlightedHit.target<LightSurfaceHit>();
      return surface.node == highlighted.node && surface.element == highlighted.element;
    }
    return true;
  };

  if (hit.type() != m_highlightedHit.type() || !sameTarget())
  {
    m_highlightedHit = hit;
    if (!isClickedSurface(hit))
    {
      m_clickedNode = nullptr;
      m_clickedElement = std::nullopt;
    }
    refreshViews();
  }
}

void LightVolumeTool::setClickedSurface(const mdl::Hit& hit)
{
  contract_pre(hit.type() == SurfaceHitType);

  const auto surface = hit.target<LightSurfaceHit>();
  m_clickedNode = surface.node;
  m_clickedElement = surface.element;
  refreshViews();
}

bool LightVolumeTool::isClickedSurface(const mdl::Hit& hit) const
{
  if (hit.type() != SurfaceHitType || !m_clickedElement)
  {
    return false;
  }

  const auto surface = hit.target<LightSurfaceHit>();
  return surface.node == m_clickedNode && surface.element == *m_clickedElement;
}

bool LightVolumeTool::beginDrag(const mdl::Hit& hit)
{
  contract_pre(hit.hasType(AnyHitType));
  contract_pre(!m_dragging);

  // the threshold of the handle box is fixed for the drag (D61)
  const auto threshold = handleThreshold();
  auto volume = volumeBox(threshold);
  if (!volume)
  {
    return false;
  }

  if (hit.type() == SurfaceHitType)
  {
    const auto surface = hit.target<LightSurfaceHit>();
    if (
      surface.node != selectedLight()
      || !isEditableElement(surface.element, volume->light))
    {
      return false;
    }
  }

  m_dragStart = std::move(volume);
  m_dragThreshold = threshold;
  m_dragHit = hit;
  m_dragBox = m_dragStart->box;
  m_dragLight = m_dragStart->light;
  m_dragDelta = vm::vec3d{0, 0, 0};

  m_document.map().startTransaction(
    "Edit Light Volume", mdl::TransactionScope::LongRunning);
  m_dragging = true;
  return true;
}

const mdl::Hit& LightVolumeTool::dragHit() const
{
  return m_dragHit;
}

const std::optional<LightVolumeBox>& LightVolumeTool::dragStartVolume() const
{
  return m_dragStart;
}

bool LightVolumeTool::update(const vm::vec3d& delta)
{
  // the drag was cancelled under the drag tracker, by a deactivation or a failed update
  if (!m_dragging)
  {
    return false;
  }

  const auto& start = *m_dragStart;
  const auto dragDelta = m_dragDelta + delta;

  auto movedBox = start.box;
  auto light = start.light;
  if (m_dragHit.type() == SurfaceHitType)
  {
    const auto edited =
      editSurface(start, m_dragHit.target<LightSurfaceHit>(), dragDelta, m_anchorPos);
    if (!edited)
    {
      return false;
    }
    light = *edited;
  }
  else
  {
    // the box frame's handle functions, never moveBBoxForHit, which accepts only the
    // scale tool's hits
    movedBox = moveBox(
      start.box,
      m_dragHit,
      vm::transpose(start.rotation) * dragDelta,
      m_proportionalAxes,
      m_anchorPos);
    if (movedBox.is_empty())
    {
      return false;
    }

    // the rotation is the light's, never the box frame's: the light composes the beam
    // frame
    light = mdl::lightFromEditBox(start.light, start.frame.R, movedBox, m_dragThreshold);
    if (light == start.light && movedBox != start.box)
    {
      // denied: a spot box shallower than 24 or a half extent below 1
      return false;
    }
  }

  auto* entityNode = selectedLight();
  if (!entityNode)
  {
    return false;
  }

  // rollback and reapply: every update starts from the light at the start of the drag
  auto& map = m_document.map();
  map.rollbackTransaction();

  const auto diff = mdl::lightKeyDiff(entityNode->entity(), light);
  if (
    (!diff.set.empty() || !diff.remove.empty())
    && !mdl::updateNodeContents(
      map,
      "Edit Light Volume",
      {{entityNode,
        mdl::NodeContents{mdl::applyLightPropertyUpdate(entityNode->entity(), diff)}}}))
  {
    // the light is back at the start of the drag, but the drag tracker would keep the
    // last accepted handle: end the drag
    cancelDrag();
    return false;
  }

  m_dragBox = movedBox;
  m_dragLight = light;
  m_dragDelta = dragDelta;
  return true;
}

void LightVolumeTool::endDrag()
{
  if (!m_dragging)
  {
    return;
  }

  auto& map = m_document.map();
  m_dragging = false;

  // also a drag that was denied all the way back to its start is no edit
  const auto unchanged =
    m_dragHit.type() == SurfaceHitType
      ? m_dragLight == m_dragStart->light
      : vm::is_equal(m_dragBox, m_dragStart->box, vm::Cd::almost_zero());
  if (unchanged)
  {
    map.cancelTransaction();
  }
  else if (!map.commitTransaction())
  {
    // commitTransaction has cancelled the transaction
    map.logger().error() << "Could not edit the light volume";
  }

  m_dragStart.reset();
  m_dragHit = mdl::Hit::NoHit;
  refreshViews();
}

void LightVolumeTool::cancelDrag()
{
  if (!m_dragging)
  {
    return;
  }

  m_document.map().cancelTransaction();
  m_dragging = false;
  m_dragStart.reset();
  m_dragHit = mdl::Hit::NoHit;
  refreshViews();
}

bool LightVolumeTool::canDraw() const
{
  const auto& map = m_document.map();
  if (!mdl::hasLightEditor(map.gameInfo().gameConfig))
  {
    return false;
  }

  if (!map.selection().hasAny())
  {
    return lightDefinition(map) != nullptr;
  }

  return volumeBox().has_value();
}

bool LightVolumeTool::updateDraw(const vm::bbox3d& worldBox)
{
  if (!canDraw())
  {
    return false;
  }

  const auto volume = volumeBox();
  auto base = mdl::LightProperties{};
  if (volume)
  {
    base = volume->light;
  }
  else
  {
    base = mdl::defaultLight(mdl::LightType::Point, mdl::LightProperties{});
    base.math = mdl::seededMathLightProperties();
  }

  auto box = worldBox;
  if (base.type == mdl::LightType::Spot && box.size().z() < MinDrawnSpotHeight)
  {
    // the beam points down from the top face, which stays where it was drawn
    box.min[2] = box.max.z() - MinDrawnSpotHeight;
  }

  const auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinDrawnHalfExtent || halfExtents.y() < MinDrawnHalfExtent
    || halfExtents.z() < MinDrawnHalfExtent)
  {
    return false;
  }

  // the drawn box of a point light is where it will be visible (D61)
  m_drawBox = box;
  m_drawLight = base.type == mdl::LightType::Spot
                  ? mdl::spotFromWorldBox(box, base)
                  : mdl::pointFromWorldBox(box, base, handleThreshold());
  refreshViews();
  return true;
}

void LightVolumeTool::endDraw()
{
  if (m_drawLight)
  {
    auto& map = m_document.map();
    const auto applied =
      map.selection().hasAny() ? applyDrawnLight() : createDrawnLight();
    if (!applied)
    {
      map.logger().error() << "Could not apply the drawn light volume";
    }
  }

  resetDraw();
  setMode(Mode::Handles);
  refreshViews();
}

void LightVolumeTool::cancelDraw()
{
  resetDraw();
  refreshViews();
}

const std::optional<vm::bbox3d>& LightVolumeTool::drawBox() const
{
  return m_drawBox;
}

const std::optional<mdl::LightProperties>& LightVolumeTool::drawLight() const
{
  return m_drawLight;
}

void LightVolumeTool::render(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const bool showHighlightedHit) const
{
  if (drawing())
  {
    if (m_drawLight && m_drawBox)
    {
      const auto style = render::LightVolumeStyle{
        std::clamp(double(pref(Preferences::LightVolumeAlpha)), 0.0, 1.0),
        std::clamp(pref(Preferences::LightVolumeShells), 2, 8),
        renderContext.render2D(),
        false,
      };
      // drawn volumes are aligned with the world
      const auto frame = mdl::LightFrame{m_drawLight->origin, vm::mat3x3d::identity()};
      const auto cullFront =
        renderContext.render3D()
        && mdl::containsPoint(
          *m_drawLight, frame, vm::vec3d{renderContext.camera().position()});

      auto mesh = render::buildLightVolumeMesh(*m_drawLight, frame, style);
      renderBatch.addOneShot(new VolumeRenderable{
        std::move(mesh.triangles),
        std::move(mesh.lines),
        cullFront ? VolumeRenderable::Cull::Front : VolumeRenderable::Cull::Back});

      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setForegroundColor(pref(Preferences::LightVolumeHandleColor));
      renderService.renderBounds(vm::bbox3f{*m_drawBox});
    }
    return;
  }

  if (!showsVolumes())
  {
    return;
  }

  const auto& highlight = m_dragging           ? m_dragHit
                          : showHighlightedHit ? m_highlightedHit
                                               : mdl::Hit::NoHit;

  if (const auto volume = volumeBox(m_dragging ? m_dragThreshold : handleThreshold()))
  {
    {
      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setForegroundColor(pref(Preferences::LightVolumeHandleColor));
      renderService.renderLines(boxEdgeVertices(*volume));
      renderService.renderHandles(handlePositions(*volume, renderContext.render3D()));
    }

    if (const auto edges = frustumEdgeVertices(*volume); !edges.empty())
    {
      // edges inside walls show at reduced alpha
      const auto hue = render::lightVolumeHue(volume->light.color);
      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setShowOccludedObjectsTransparent();
      renderService.setForegroundColor(RgbF{
        float(std::clamp(hue.x(), 0.0, 1.0)),
        float(std::clamp(hue.y(), 0.0, 1.0)),
        float(std::clamp(hue.z(), 0.0, 1.0))});
      renderService.setLineWidth(2.0f);
      renderService.renderLines(edges);
    }

    if (highlight.hasType(HandleHitType))
    {
      renderHighlight(renderContext, renderBatch, *volume, highlight);
    }
    else if (m_dragging && highlight.type() == SurfaceHitType)
    {
      // the dragged element as the light is now, not as it was picked
      auto surface = highlight.target<LightSurfaceHit>();
      surface.light = volume->light;
      surface.frame = volume->frame;
      if (renderContext.render3D())
      {
        renderSurfaceHighlight(renderContext, renderBatch, surface, true);
      }
      return;
    }
  }

  // surfaces are highlighted only where they are picked: in the 3D view
  if (highlight.type() == SurfaceHitType && renderContext.render3D())
  {
    renderSurfaceHighlight(
      renderContext,
      renderBatch,
      highlight.target<LightSurfaceHit>(),
      isClickedSurface(highlight));
  }
}

bool LightVolumeTool::doActivate()
{
  m_highlightedHit = mdl::Hit::NoHit;
  return true;
}

bool LightVolumeTool::doDeactivate()
{
  // the selected light is not read: it may be gone (a new map)
  if (m_dragging)
  {
    cancelDrag();
  }
  resetDraw();
  m_highlightedHit = mdl::Hit::NoHit;
  if (m_mode != Mode::Handles)
  {
    m_mode = Mode::Handles;
    modeDidChangeNotifier();
  }
  return true;
}

void LightVolumeTool::invalidatePickableLights()
{
  m_pickableLightsValid = false;
  m_pickableLights.clear();
}

void LightVolumeTool::lightsDidChange()
{
  invalidatePickableLights();

  // the highlighted surface may be of a light that changed or is gone
  if (m_highlightedHit.type() == SurfaceHitType && !m_dragging)
  {
    m_highlightedHit = mdl::Hit::NoHit;
  }
}

const std::vector<LightVolumeTool::PickableLight>& LightVolumeTool::pickableLights() const
{
  if (m_pickableLightsValid)
  {
    return m_pickableLights;
  }

  m_pickableLightsValid = true;
  m_pickableLights.clear();
  if (!showsVolumes())
  {
    return m_pickableLights;
  }

  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  // the lights whose volumes are shown: the selected ones, and in "All" mode every other
  // one that the editor context shows (D56)
  auto lights = std::vector<std::pair<mdl::EntityNode*, bool>>{};
  for (auto* entityNodeBase : map.selection().allEntities())
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(entityNodeBase))
    {
      lights.emplace_back(entityNode, true);
    }
  }
  if (pref(Preferences::ShowLightVolumes) == Preferences::ShowLightVolumesAll)
  {
    map.worldNode().accept(kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.visitChildren(thisLambda);
      },
      [&](mdl::EntityNode& entityNode) {
        if (!entityNode.transitivelySelected())
        {
          lights.emplace_back(&entityNode, false);
        }
      },
      [](mdl::BrushNode&) {},
      [](mdl::PatchNode&) {}));
  }

  const auto policy = rotationPolicy(map);
  for (const auto& [entityNode, selected] : lights)
  {
    const auto& entity = entityNode->entity();
    if (!mdl::isLightEntity(entity) || !editorContext.visible(*entityNode))
    {
      continue;
    }

    // a light_origin light is read only (D8), a degenerate one has no volume
    const auto light = mdl::parseLightProperties(entity);
    if (light.hasLightOrigin || !mdl::lightEditBox(light))
    {
      continue;
    }

    const auto frame = mdl::lightFrame(entity, policy);
    const auto [centre, radius] = boundingSphere(light, frame);
    m_pickableLights.push_back(PickableLight{
      entityNode,
      light,
      frame,
      selected,
      centre,
      radius,
      pickedTriangles(light, frame),
    });
  }

  return m_pickableLights;
}

void LightVolumeTool::resetDraw()
{
  m_drawBox.reset();
  m_drawLight.reset();
}

bool LightVolumeTool::applyDrawnLight()
{
  auto* entityNode = selectedLight();
  if (!entityNode || !volumeBox())
  {
    return false;
  }

  auto& map = m_document.map();
  const auto& entity = entityNode->entity();

  auto changed =
    mdl::applyLightPropertyUpdate(entity, mdl::lightKeyDiff(entity, *m_drawLight));
  removeRotationProperties(changed);
  if (changed == entity)
  {
    return true;
  }

  auto transaction = mdl::Transaction{map, "Draw Light Volume"};
  if (!mdl::updateNodeContents(
        map, "Draw Light Volume", {{entityNode, mdl::NodeContents{std::move(changed)}}}))
  {
    transaction.cancel();
    return false;
  }
  return transaction.commit();
}

bool LightVolumeTool::createDrawnLight()
{
  auto& map = m_document.map();
  const auto* definition = lightDefinition(map);
  if (!definition)
  {
    return false;
  }

  auto transaction = mdl::Transaction{map, "Create Light"};

  auto* entityNode = mdl::createPointEntity(map, *definition, m_drawLight->origin);
  if (!entityNode)
  {
    transaction.cancel();
    return false;
  }

  // every property of the light in the one command after the entity was created
  const auto& entity = entityNode->entity();
  auto changed =
    mdl::applyLightPropertyUpdate(entity, mdl::lightKeyDiff(entity, *m_drawLight));
  removeRotationProperties(changed);

  // a light drawn around the world origin was moved by nothing, so it has no origin
  // property yet, and the diff writes none because it reads the same origin before and
  // after
  if (!changed.hasProperty(mdl::EntityPropertyKeys::Origin))
  {
    for (const auto& property : mdl::lightPropertyUpdate(*m_drawLight).set)
    {
      if (property.key() == mdl::EntityPropertyKeys::Origin)
      {
        changed.addOrUpdateProperty(property.key(), property.value());
      }
    }
  }
  if (
    !(changed == entity)
    && !mdl::updateNodeContents(
      map, "Create Light", {{entityNode, mdl::NodeContents{std::move(changed)}}}))
  {
    transaction.cancel();
    return false;
  }

  if (!entityNode->selected())
  {
    mdl::deselectAll(map);
    mdl::selectNodes(map, {entityNode});
  }

  return transaction.commit();
}

} // namespace tb::ui
