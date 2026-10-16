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
#include "base/PreferenceManager.h"
#include "gl/ActiveShader.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "gl/PrimType.h"
#include "gl/Shaders.h"
#include "gl/VertexArray.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "prefs/Preferences.h"
#include "render/LightVolumeMesh.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "render/RenderService.h"
#include "render/Renderable.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"

#include "vm/distance.h"
#include "vm/intersection.h"
#include "vm/polygon.h"
#include "vm/segment.h"

#include <algorithm>
#include <limits>
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

// in 2D, a side or the far cap's centre is not picked when it faces the camera this
// directly, because the drag runs in the view plane
constexpr auto MaxCosine2D = 0.9;

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
 * The scale tool's pickBackSideOfBox without the sides that face the camera more directly
 * than MaxCosine2D, for the 2D views. The ray and the box are in the box frame.
 */
std::optional<BackSide> pickBackSide2D(const vm::ray3d& pickRay, const vm::bbox3d& box)
{
  auto closestDistToRay = std::numeric_limits<double>::max();
  auto result = std::optional<BackSide>{};

  box.for_each_face(
    [&](const auto& p0, const auto& p1, const auto& p2, const auto& p3, const auto& n) {
      const auto cosAngle = vm::dot(n, pickRay.direction);
      if (cosAngle >= 0.0 && cosAngle <= MaxCosine2D)
      {
        const vm::vec3d points[] = {p0, p1, p2, p3};
        for (size_t i = 0; i < 4; ++i)
        {
          const auto distance =
            vm::distance(pickRay, vm::segment3d{points[i], points[(i + 1) % 4]});
          if (distance.distance < closestDistToRay)
          {
            closestDistToRay = distance.distance;
            result = BackSide{distance.position1, n};
          }
        }
      }
    });

  return result;
}

/**
 * Picks the handles of the given volume with the given world pick ray: the corners as
 * spheres of twice the handle radius, the edges, the far cap's centre of a spot light,
 * and the sides; if none is hit, the back side closest to the ray. Corners, edges and
 * sides are tested in world space. If the camera is inside the box, only the corners and
 * edges in front of it are picked, the sides through the face the ray leaves by, and no
 * back side.
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

  for (const auto& corner : allCorners())
  {
    const auto point = volume.toWorld(pointForBBoxCorner(volume.box, corner));
    if (inFront(point))
    {
      if (
        const auto distance = camera.pickPointHandle(pickRay, point, handleRadius * 2.0))
      {
        result.addHit(mdl::Hit{
          LightVolumeTool::CornerHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          corner});
      }
    }
  }

  if (isFarCapSide(volume))
  {
    const auto point = volume.toWorld(centerForBBoxSide(volume.box, FarCapSide));
    const auto beam = volume.rotation * FarCapSide.normal;
    if (
      inFront(point)
      && (!twoD || std::abs(vm::dot(beam, pickRay.direction)) <= MaxCosine2D))
    {
      if (
        const auto distance = camera.pickPointHandle(pickRay, point, handleRadius * 2.0))
      {
        result.addHit(mdl::Hit{
          LightVolumeTool::FarCapCenterHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          FarCapSide});
      }
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
        result.addHit(mdl::Hit{
          LightVolumeTool::EdgeHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          edge});
      }
    }
  }

  if (!twoD)
  {
    // two-sided, so that from inside the box the ray hits the side it leaves by
    for (const auto& side : allSides())
    {
      const auto vertices = worldSide(volume, side);
      if (
        const auto distance =
          vm::intersect_ray_polygon(pickRay, vertices.begin(), vertices.end()))
      {
        result.addHit(mdl::Hit{
          LightVolumeTool::SideHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          side});
      }
    }
  }

  if (result.empty() && !inside)
  {
    const auto backSide =
      twoD ? pickBackSide2D(boxRay, volume.box)
           : std::optional{pickBackSideOfBox(boxRay, camera, volume.box)};
    if (backSide)
    {
      result.addHit(mdl::Hit{
        LightVolumeTool::SideHitType,
        backSide->distAlongRay,
        vm::point_at_distance(pickRay, backSide->distAlongRay),
        BBoxSide{backSide->pickedSideNormal}});
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
  if (hit.type() == LightVolumeTool::SideHitType)
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

std::vector<vm::vec3f> handlePositions(const LightVolumeBox& volume)
{
  auto result = std::vector<vm::vec3f>{};
  for (const auto& corner : allCorners())
  {
    result.emplace_back(volume.toWorld(pointForBBoxCorner(volume.box, corner)));
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
  else if (hit.type() == LightVolumeTool::FarCapCenterHitType)
  {
    renderService.renderHandleHighlight(
      vm::vec3f{volume.toWorld(centerForBBoxSide(volume.box, FarCapSide))});
  }
}

/**
 * The volume of the light being drawn, built once for the frame in which it is rendered.
 */
class DrawnVolumeRenderable : public render::DirectRenderable
{
private:
  gl::VertexArray m_fills;
  gl::VertexArray m_lines;
  bool m_cullFront;

public:
  DrawnVolumeRenderable(render::LightVolumeMesh mesh, const bool cullFront)
    : m_fills{gl::VertexArray::move(std::move(mesh.triangles))}
    , m_lines{gl::VertexArray::move(std::move(mesh.lines))}
    , m_cullFront{cullFront}
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
    gl.enable(GL_CULL_FACE);
    gl.cullFace(m_cullFront ? GL_FRONT : GL_BACK);
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
  const auto box = mdl::lightEditBox(light);
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

vm::line3d lightVolumeHandleLine(const LightVolumeBox& volume, const mdl::Hit& hit)
{
  contract_pre(hit.hasType(LightVolumeTool::AnyHitType));

  const auto& box = volume.box;

  // the lines of the scale tool's handleLineForHit in the box frame; they all run
  // through the box's centre, so the anchor does not change them
  if (
    hit.type() == LightVolumeTool::SideHitType
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

mdl::MathLightProperties lightVolumeSeedMath()
{
  // the same values as the light editor panel's "Enable" (LightEditorPanel.cpp)
  auto math = mdl::MathLightProperties{};
  math.enabled = true;
  math.falloff = mdl::LightFalloff::Linear;
  math.falloffExp = 2.0;
  math.innerRadius = 0.1;
  math.shapeExp = 2.0;
  math.penumbra = 0.15;
  math.pattern = mdl::LightPattern::None;
  math.patternStrength = 0.0;
  math.patternScale = 1.0;
  math.patternPhase = 0.0;
  math.energyClamp = true;
  return math;
}

const mdl::HitType::Type LightVolumeTool::SideHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::EdgeHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::CornerHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::FarCapCenterHitType = mdl::HitType::freeType();
const mdl::HitType::Type LightVolumeTool::HandleHitType =
  LightVolumeTool::EdgeHitType | LightVolumeTool::CornerHitType
  | LightVolumeTool::FarCapCenterHitType;
const mdl::HitType::Type LightVolumeTool::AnyHitType =
  LightVolumeTool::SideHitType | LightVolumeTool::HandleHitType;

LightVolumeTool::LightVolumeTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

LightVolumeTool::~LightVolumeTool() = default;

const mdl::Grid& LightVolumeTool::grid() const
{
  return m_document.map().grid();
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
  if (const auto* entityNode = selectedLight())
  {
    auto volume = lightVolumeBox(entityNode->entity(), rotationPolicy(m_document.map()));
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
  if (const auto volume = volumeBox())
  {
    const auto hits = pickVolume(*volume, pickRay, camera, false);
    if (!hits.empty())
    {
      pickResult.addHit(hits.all().front());
    }
  }
}

void LightVolumeTool::pick2D(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult) const
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
    if (hit.type() == SideHitType || hit.type() == FarCapCenterHitType)
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
    return true;
  };

  if (hit.type() != m_highlightedHit.type() || !sameTarget())
  {
    m_highlightedHit = hit;
    refreshViews();
  }
}

bool LightVolumeTool::beginDrag(const mdl::Hit& hit)
{
  contract_pre(hit.hasType(AnyHitType));
  contract_pre(!m_dragging);

  auto volume = volumeBox();
  if (!volume)
  {
    return false;
  }

  m_dragStart = std::move(volume);
  m_dragHit = hit;
  m_dragBox = m_dragStart->box;
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

const LightVolumeBox& LightVolumeTool::dragStartVolume() const
{
  contract_pre(m_dragging);

  return *m_dragStart;
}

bool LightVolumeTool::update(const vm::vec3d& delta)
{
  contract_pre(m_dragging);

  const auto& start = *m_dragStart;
  const auto dragDelta = m_dragDelta + delta;

  // the box frame's handle functions, never moveBBoxForHit, which accepts only the scale
  // tool's hits
  const auto movedBox = moveBox(
    start.box,
    m_dragHit,
    vm::transpose(start.rotation) * dragDelta,
    m_proportionalAxes,
    m_anchorPos);
  if (movedBox.is_empty())
  {
    return false;
  }

  // the rotation is the light's, never the box frame's: the light composes the beam frame
  const auto light = mdl::lightFromEditBox(start.light, start.frame.R, movedBox);
  if (light == start.light && movedBox != start.box)
  {
    // denied: a spot box shallower than 24 or a half extent below 1
    return false;
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
    // the rollback left the light at the start of the drag
    m_dragBox = start.box;
    m_dragDelta = vm::vec3d{0, 0, 0};
    refreshViews();
    return false;
  }

  m_dragBox = movedBox;
  m_dragDelta = dragDelta;
  return true;
}

void LightVolumeTool::endDrag()
{
  contract_pre(m_dragging);

  auto& map = m_document.map();
  m_dragging = false;

  // also a drag that was denied all the way back to its start is no edit
  if (vm::is_equal(m_dragBox, m_dragStart->box, vm::Cd::almost_zero()))
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
  contract_pre(m_dragging);

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
    base.math = lightVolumeSeedMath();
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

  m_drawBox = box;
  m_drawLight = base.type == mdl::LightType::Spot ? mdl::spotFromWorldBox(box, base)
                                                  : mdl::pointFromWorldBox(box, base);
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
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  if (m_mode == Mode::Draw)
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

      renderBatch.addOneShot(new DrawnVolumeRenderable{
        render::buildLightVolumeMesh(*m_drawLight, frame, style), cullFront});

      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setForegroundColor(pref(Preferences::LightVolumeHandleColor));
      renderService.renderBounds(vm::bbox3f{*m_drawBox});
    }
    return;
  }

  const auto volume = volumeBox();
  if (!volume)
  {
    return;
  }

  {
    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.setForegroundColor(pref(Preferences::LightVolumeHandleColor));
    renderService.renderLines(boxEdgeVertices(*volume));
    renderService.renderHandles(handlePositions(*volume));
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

  const auto& highlight = m_dragging ? m_dragHit : m_highlightedHit;
  if (highlight.hasType(AnyHitType))
  {
    renderHighlight(renderContext, renderBatch, *volume, highlight);
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
