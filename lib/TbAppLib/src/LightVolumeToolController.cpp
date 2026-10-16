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

#include "ui/LightVolumeToolController.h"

#include "gl/Camera.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/PickResult.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/intersection.h"
#include "vm/line.h"
#include "vm/plane.h"

#include <optional>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{

// the view axis extent of a light drawn in a 2D view with nothing selected, a default
// point light's radius
constexpr auto DrawExtent2D = 300.0;

bool startsOnSelectedNode(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  return inputState.pickResult()
    .first(type(mdl::nodeHitType()) && transitivelySelected())
    .isMatch();
}

/**
 * The scale tool's modifier settings: Alt anchors the box at its centre, Shift scales the
 * other axes proportionally, except in 2D the axis of the box frame that is closest to
 * the view direction.
 */
std::pair<AnchorPos, ProportionalAxes> modifierSettingsForInputState(
  const InputState& inputState, const vm::mat3x3d& rotation)
{
  const auto anchorPos = inputState.modifierKeysDown(ModifierKeys::Alt)
                           ? AnchorPos::Center
                           : AnchorPos::Opposite;

  auto proportionalAxes = ProportionalAxes::None();
  if (inputState.modifierKeysDown(ModifierKeys::Shift))
  {
    proportionalAxes = ProportionalAxes::All();

    const auto& camera = inputState.camera();
    if (camera.orthographicProjection())
    {
      const auto boxDirection = vm::transpose(rotation) * vm::vec3d{camera.direction()};
      proportionalAxes.setAxisProportional(
        vm::find_abs_max_component(boxDirection), false);
    }
  }

  return {anchorPos, proportionalAxes};
}

void updateModifierSettings(LightVolumeTool& tool, const InputState& inputState)
{
  const auto volume = tool.isDragging() ? tool.dragStartVolume() : tool.volumeBox();
  if (volume)
  {
    const auto [anchorPos, proportionalAxes] =
      modifierSettingsForInputState(inputState, volume->rotation);
    tool.setAnchorPos(anchorPos);
    tool.setProportionalAxes(proportionalAxes);
  }
}

/**
 * The scale tool's handle proposer: in 2D, a corner, and an edge without Shift, are
 * dragged in the view plane with relative snapping; every other handle along its handle
 * line with absolute snapping.
 */
HandlePositionProposer makeHandleProposer(
  const InputState& inputState,
  const mdl::Grid& grid,
  const mdl::Hit& hit,
  const LightVolumeBox& volume,
  const vm::vec3d& handleOffset)
{
  const auto& camera = inputState.camera();
  const auto viewPlaneDrag =
    camera.orthographicProjection()
    && (hit.type() == LightVolumeTool::CornerHitType
        || (hit.type() == LightVolumeTool::EdgeHitType
            && !inputState.modifierKeysDown(ModifierKeys::Shift)));

  if (viewPlaneDrag)
  {
    const auto plane =
      vm::plane3d{hit.hitPoint() + handleOffset, vm::vec3d{camera.direction()} * -1.0};
    return makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset), makeRelativeHandleSnapper(grid));
  }

  const auto handleLine = lightVolumeHandleLine(volume, hit);
  return makeHandlePositionProposer(
    makeLineHandlePicker(handleLine, handleOffset),
    makeAbsoluteLineHandleSnapper(grid, handleLine));
}

/**
 * The tool's drag can end under the tracker: a deactivation cancels it, and so does an
 * update that cannot change the light. The delegate then proposes no handle position and
 * ends the drag at the next update.
 */
class LightVolumeHandleDragDelegate : public HandleDragTrackerDelegate
{
private:
  LightVolumeTool& m_tool;

public:
  explicit LightVolumeHandleDragDelegate(LightVolumeTool& tool)
    : m_tool{tool}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& /* initialHandlePosition */,
    const vm::vec3d& handleOffset) override
  {
    updateModifierSettings(m_tool, inputState);
    return makeProposer(inputState, handleOffset);
  }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState& inputState, const DragState& dragState) override
  {
    return UpdateDragConfig{
      makeProposer(inputState, dragState.handleOffset), ResetInitialHandlePosition::Keep};
  }

  DragStatus update(
    const InputState&,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    if (!m_tool.isDragging())
    {
      return DragStatus::End;
    }

    // a denied step is not accumulated by the tool, and the tracker keeps the handle
    // where it was
    const auto delta = proposedHandlePosition - dragState.currentHandlePosition;
    return m_tool.update(delta)  ? DragStatus::Continue
           : m_tool.isDragging() ? DragStatus::Deny
                                 : DragStatus::End;
  }

  void end(const InputState& inputState, const DragState&) override
  {
    // nothing if the drag was cancelled under the tracker
    m_tool.endDrag();

    // the mouse is over a different handle now
    m_tool.setHighlightedHit(findLightVolumeHit(inputState.pickResult()));
  }

  void cancel(const DragState&) override { m_tool.cancelDrag(); }

private:
  HandlePositionProposer makeProposer(
    const InputState& inputState, const vm::vec3d& handleOffset) const
  {
    if (const auto& volume = m_tool.dragStartVolume())
    {
      return makeHandleProposer(
        inputState, m_tool.grid(), m_tool.dragHit(), *volume, handleOffset);
    }
    return [](const InputState&, const DragState&) -> std::optional<vm::vec3d> {
      return std::nullopt;
    };
  }
};

/**
 * The draw shape tool's 3D delegate (DrawShapeToolController3D.cpp), drawing the light
 * volume tool's box instead of brushes.
 */
class LightVolumeDrawDragDelegate3D : public HandleDragTrackerDelegate
{
private:
  LightVolumeTool& m_tool;
  vm::bbox3d m_worldBounds;

public:
  LightVolumeDrawDragDelegate3D(LightVolumeTool& tool, const vm::bbox3d& worldBounds)
    : m_tool{tool}
    , m_worldBounds{worldBounds}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) override
  {
    const auto currentBounds =
      makeBounds(inputState, initialHandlePosition, initialHandlePosition);
    m_tool.updateDraw(currentBounds);
    m_tool.refreshViews();

    return makeHandlePositionProposer(
      makePlaneHandlePicker(vm::horizontal_plane(initialHandlePosition), handleOffset),
      makeIdentityHandleSnapper());
  }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState& inputState, const DragState& dragState) override
  {
    if (inputState.modifierKeys() == ModifierKeys::Shift)
    {
      const auto currentBounds = makeBounds(
        inputState, dragState.initialHandlePosition, dragState.currentHandlePosition);

      if (!currentBounds.is_empty())
      {
        m_tool.updateDraw(currentBounds);
        m_tool.refreshViews();
      }
    }

    if (inputState.modifierKeys() == ModifierKeys::Alt)
    {
      return UpdateDragConfig{
        makeHandlePositionProposer(
          makeLineHandlePicker(
            vm::line3d{dragState.currentHandlePosition, vm::vec3d{0, 0, 1}},
            dragState.handleOffset),
          makeIdentityHandleSnapper()),
        ResetInitialHandlePosition::Keep};
    }

    return UpdateDragConfig{
      makeHandlePositionProposer(
        makePlaneHandlePicker(
          vm::horizontal_plane(dragState.currentHandlePosition), dragState.handleOffset),
        makeIdentityHandleSnapper()),
      ResetInitialHandlePosition::Keep};
  }

  DragStatus update(
    const InputState& inputState,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    const auto lastBounds = makeBounds(
      inputState, dragState.initialHandlePosition, dragState.currentHandlePosition);
    const auto currentBounds =
      makeBounds(inputState, dragState.initialHandlePosition, proposedHandlePosition);

    if (
      currentBounds.is_empty() || currentBounds == lastBounds
      || !m_tool.updateDraw(currentBounds))
    {
      return DragStatus::Deny;
    }

    m_tool.refreshViews();
    return DragStatus::Continue;
  }

  void end(const InputState&, const DragState&) override { m_tool.endDraw(); }

  void cancel(const DragState&) override { m_tool.cancelDraw(); }

private:
  vm::bbox3d makeBounds(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& currentHandlePosition) const
  {
    auto bounds = snapBounds(
      inputState,
      vm::bbox3d{
        vm::min(initialHandlePosition, currentHandlePosition),
        vm::max(initialHandlePosition, currentHandlePosition),
      });

    if (inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      const auto includeZAxis = inputState.modifierKeysDown(ModifierKeys::Alt);

      const auto xyAxes = vm::vec3d{1, 0, 0} + vm::vec3d{0, 1, 0};
      const auto zAxis = vm::vec3d{0, 0, 1};
      const auto allAxes = vm::vec3d{1, 1, 1};
      const auto noAxis = vm::vec3d{0, 0, 0};
      const auto maxLengthAxes = includeZAxis ? allAxes : xyAxes;
      const auto zLengthAxis = includeZAxis ? noAxis : zAxis;

      const auto maxLength = vm::get_abs_max_component(bounds.size() * maxLengthAxes);

      const auto lengthDiff = zLengthAxis * bounds.size() + maxLengthAxes * maxLength;

      // The direction in which the user is dragging per component:
      const auto dragDir = vm::step(initialHandlePosition, currentHandlePosition);
      bounds = vm::bbox3d{
        vm::mix(bounds.min, bounds.max - lengthDiff, vm::vec3d{1, 1, 1} - dragDir),
        vm::mix(bounds.max, bounds.min + lengthDiff, dragDir),
      };
    }

    return vm::intersect(bounds, m_worldBounds);
  }

  vm::bbox3d snapBounds(const InputState& inputState, vm::bbox3d bounds) const
  {
    // prevent flickering due to very small rounding errors
    bounds.min = vm::correct(bounds.min);
    bounds.max = vm::correct(bounds.max);

    const auto& grid = m_tool.grid();
    bounds.min = grid.snapDown(bounds.min);
    bounds.max = grid.snapUp(bounds.max);

    const auto& camera = inputState.camera();
    const auto cameraPosition = vm::vec3d{camera.position()};

    for (size_t i = 0; i < 3; i++)
    {
      if (bounds.max[i] <= bounds.min[i])
      {
        if (bounds.min[i] < cameraPosition[i])
        {
          bounds.max[i] = bounds.min[i] + grid.actualSize();
        }
        else
        {
          bounds.min[i] = bounds.max[i] - grid.actualSize();
        }
      }
    }

    return bounds;
  }
};

/**
 * The draw shape tool's 2D delegate (DrawShapeToolController2D.cpp), drawing the light
 * volume tool's box instead of brushes. The extent along the view axis is the reference
 * bounds' extent.
 */
class LightVolumeDrawDragDelegate2D : public HandleDragTrackerDelegate
{
private:
  LightVolumeTool& m_tool;
  vm::bbox3d m_worldBounds;
  vm::bbox3d m_referenceBounds;

public:
  LightVolumeDrawDragDelegate2D(
    LightVolumeTool& tool,
    const vm::bbox3d& worldBounds,
    const vm::bbox3d& referenceBounds)
    : m_tool{tool}
    , m_worldBounds{worldBounds}
    , m_referenceBounds{referenceBounds}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) override
  {
    const auto currentBounds =
      makeBounds(inputState, initialHandlePosition, initialHandlePosition);

    m_tool.updateDraw(currentBounds);
    m_tool.refreshViews();

    const auto& camera = inputState.camera();
    const auto plane = vm::plane3d{
      initialHandlePosition,
      vm::vec3d{vm::get_abs_max_component_axis(camera.direction())}};

    return makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset), makeIdentityHandleSnapper());
  }

  DragStatus update(
    const InputState& inputState,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    const auto lastBounds = makeBounds(
      inputState, dragState.initialHandlePosition, dragState.currentHandlePosition);
    const auto currentBounds =
      makeBounds(inputState, dragState.initialHandlePosition, proposedHandlePosition);

    if (
      currentBounds.is_empty() || currentBounds == lastBounds
      || !m_tool.updateDraw(currentBounds))
    {
      return DragStatus::Deny;
    }

    m_tool.refreshViews();
    return DragStatus::Continue;
  }

  void end(const InputState&, const DragState&) override { m_tool.endDraw(); }

  void cancel(const DragState&) override { m_tool.cancelDraw(); }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState& inputState, const DragState& dragState) override
  {
    const auto currentBounds = makeBounds(
      inputState, dragState.initialHandlePosition, dragState.currentHandlePosition);

    if (!currentBounds.is_empty())
    {
      m_tool.updateDraw(currentBounds);
      m_tool.refreshViews();
    }

    return std::nullopt;
  }

private:
  vm::bbox3d makeBounds(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& currentHandlePosition) const
  {
    auto bounds = snapBounds(
      inputState,
      vm::merge(
        vm::bbox3d{initialHandlePosition, initialHandlePosition}, currentHandlePosition));

    if (inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      const auto viewAxis = vm::abs(vm::vec3d{inputState.camera().direction()});
      const auto orthoAxes = vm::vec3d{1, 1, 1} - viewAxis;

      // The max length of the bounds along any of the ortho axes:
      const auto maxLength = vm::get_abs_max_component(bounds.size() * orthoAxes);

      if (inputState.modifierKeysDown(ModifierKeys::Alt))
      {
        const auto lengthDiff = vm::vec3d{maxLength, maxLength, maxLength};

        // The direction in which the user is dragging per component:
        const auto dragDir = vm::step(initialHandlePosition, currentHandlePosition);
        bounds = vm::bbox3d{
          vm::mix(bounds.min, bounds.max - lengthDiff, vm::vec3d{1, 1, 1} - dragDir),
          vm::mix(bounds.max, bounds.min + lengthDiff, dragDir)};
      }
      else
      {
        // A vector where the ortho axes have maxLength and the view axis has the size of
        // the bounds in that direction
        const auto lengthDiff = viewAxis * bounds.size() + orthoAxes * maxLength;

        // The direction in which the user is dragging per component:
        const auto dragDir = vm::step(initialHandlePosition, currentHandlePosition);
        bounds = vm::bbox3d{
          vm::mix(bounds.min, bounds.max - lengthDiff, vm::vec3d{1, 1, 1} - dragDir),
          vm::mix(bounds.max, bounds.min + lengthDiff, dragDir)};
      }
    }

    return vm::intersect(bounds, m_worldBounds);
  }

  vm::bbox3d snapBounds(const InputState& inputState, const vm::bbox3d& bounds) const
  {
    const auto& grid = m_tool.grid();
    const auto min = grid.snapDown(bounds.min);
    const auto max = grid.snapUp(bounds.max);

    const auto& camera = inputState.camera();
    const auto& refBounds = m_referenceBounds;
    const auto factors =
      vm::vec3d{vm::abs(vm::get_abs_max_component_axis(camera.direction()))};
    return vm::bbox3d{
      vm::mix(min, refBounds.min, factors), vm::mix(max, refBounds.max, factors)};
  }
};

/**
 * The bounds whose extent along the view axis a 2D drag in draw mode takes: the world
 * bounds of the selected light's edit box, or +-300 around the map's reference bounds
 * (the draw shape tool's reference bounds would give the selected light's own small box,
 * a flat light).
 */
vm::bbox3d drawReferenceBounds(const LightVolumeTool& tool, const mdl::Map& map)
{
  if (const auto volume = tool.volumeBox())
  {
    auto corners = std::vector<vm::vec3d>{};
    volume->box.for_each_vertex(
      [&](const auto& corner) { corners.push_back(volume->toWorld(corner)); });
    return *vm::bbox3d::build(corners);
  }

  const auto center = map.referenceBounds().center();
  const auto extent = vm::vec3d{DrawExtent2D, DrawExtent2D, DrawExtent2D};
  return vm::bbox3d{center - extent, center + extent};
}

} // namespace

const mdl::Hit& findLightVolumeHit(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  // 1. a corner, an edge or the far cap's centre, even behind the selected light's box
  if (const auto& hit = pickResult.first(type(LightVolumeTool::HandleHitType));
      hit.isMatch())
  {
    return hit;
  }

  // 2. a selected node: the move tool moves it
  if (pickResult.first(type(mdl::nodeHitType()) && transitivelySelected()).isMatch())
  {
    return mdl::Hit::NoHit;
  }

  // 3. a side, or the back side closest to the mouse
  return pickResult.first(type(LightVolumeTool::SideHitType));
}

LightVolumeToolController::LightVolumeToolController(
  LightVolumeTool& tool, MapDocument& document)
  : m_tool{tool}
  , m_document{document}
{
}

LightVolumeToolController::~LightVolumeToolController() = default;

Tool& LightVolumeToolController::tool()
{
  return m_tool;
}

const Tool& LightVolumeToolController::tool() const
{
  return m_tool;
}

void LightVolumeToolController::pick(
  const InputState& inputState, mdl::PickResult& pickResult)
{
  if (m_tool.mode() == LightVolumeTool::Mode::Handles)
  {
    doPick(inputState.pickRay(), inputState.camera(), pickResult);
  }
}

void LightVolumeToolController::modifierKeyChange(const InputState& inputState)
{
  updateModifierSettings(m_tool, inputState);

  // the mouse may be over a different handle now
  m_tool.refreshViews();
}

void LightVolumeToolController::mouseMove(const InputState& inputState)
{
  if (m_tool.mode() == LightVolumeTool::Mode::Handles && !inputState.anyToolDragging())
  {
    m_tool.setHighlightedHit(findLightVolumeHit(inputState.pickResult()));
  }
}

bool LightVolumeToolController::mouseClick(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  if (
    !inputState.mouseButtonsPressed(MouseButtons::Left)
    || inputState.modifierKeysDown(ModifierKeys::CtrlCmd) || !m_tool.volumeBox())
  {
    return false;
  }

  const auto& pickResult = inputState.pickResult();

  // a click on a handle, or a drag shorter than 100 ms, keeps the light selected
  if (pickResult.first(type(LightVolumeTool::HandleHitType)).isMatch())
  {
    return true;
  }

  // a click on another object selects it; a click on nothing would deselect the light
  const auto& editorContext = m_document.map().editorContext();
  const auto isSelectable = [&](const mdl::Hit& hit) {
    auto* node = mdl::hitToNode(hit);
    return node && editorContext.selectable(*mdl::findOutermostClosedGroupOrNode(node));
  };
  return !pickResult.first(type(mdl::nodeHitType()) && isSelectable).isMatch();
}

std::unique_ptr<GestureTracker> LightVolumeToolController::acceptMouseDrag(
  const InputState& inputState)
{
  // Ctrl and the left button is paint select
  if (
    !inputState.mouseButtonsPressed(MouseButtons::Left)
    || inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
  {
    return nullptr;
  }

  if (m_tool.mode() == LightVolumeTool::Mode::Draw)
  {
    // a drag that starts on a selected node moves it
    return m_tool.canDraw() && !startsOnSelectedNode(inputState)
             ? acceptDrawDrag(inputState)
             : nullptr;
  }

  const auto& hit = findLightVolumeHit(inputState.pickResult());
  if (!hit.isMatch() || !m_tool.beginDrag(hit))
  {
    return nullptr;
  }

  const auto handleLine = lightVolumeHandleLine(*m_tool.dragStartVolume(), hit);
  return createHandleDragTracker(
    LightVolumeHandleDragDelegate{m_tool},
    inputState,
    handleLine.get_origin(),
    hit.hitPoint());
}

void LightVolumeToolController::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  // no hover feedback while another tool drags
  m_tool.render(renderContext, renderBatch, !inputState.anyToolDragging());
}

bool LightVolumeToolController::cancel()
{
  // the chain also asks inactive tools; Esc leaves draw mode first, then the tool box
  // deactivates the tool
  if (m_tool.active() && m_tool.mode() == LightVolumeTool::Mode::Draw)
  {
    m_tool.setMode(LightVolumeTool::Mode::Handles);
    return true;
  }
  return false;
}

// LightVolumeToolController2D

LightVolumeToolController2D::LightVolumeToolController2D(
  LightVolumeTool& tool, MapDocument& document)
  : LightVolumeToolController{tool, document}
{
}

void LightVolumeToolController2D::doPick(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult) const
{
  m_tool.pick2D(pickRay, camera, pickResult);
}

std::unique_ptr<GestureTracker> LightVolumeToolController2D::acceptDrawDrag(
  const InputState& inputState)
{
  if (!inputState.checkModifierKeys(
        ModifierKeyPressed::No,
        ModifierKeyPressed::DontCare,
        ModifierKeyPressed::DontCare))
  {
    return nullptr;
  }

  const auto& map = m_document.map();
  const auto referenceBounds = drawReferenceBounds(m_tool, map);
  const auto& camera = inputState.camera();
  const auto plane = vm::plane3d{
    referenceBounds.min, vm::vec3d{vm::get_abs_max_component_axis(camera.direction())}};

  if (const auto distance = vm::intersect_ray_plane(inputState.pickRay(), plane))
  {
    const auto initialHandlePosition =
      vm::point_at_distance(inputState.pickRay(), *distance);
    return createHandleDragTracker(
      LightVolumeDrawDragDelegate2D{m_tool, map.worldBounds(), referenceBounds},
      inputState,
      initialHandlePosition,
      initialHandlePosition);
  }

  return nullptr;
}

// LightVolumeToolController3D

LightVolumeToolController3D::LightVolumeToolController3D(
  LightVolumeTool& tool, MapDocument& document)
  : LightVolumeToolController{tool, document}
{
}

void LightVolumeToolController3D::doPick(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult) const
{
  m_tool.pick3D(pickRay, camera, pickResult);
}

std::unique_ptr<GestureTracker> LightVolumeToolController3D::acceptDrawDrag(
  const InputState& inputState)
{
  using namespace mdl::HitFilters;

  if (!inputState.checkModifierKeys(
        ModifierKeyPressed::No, ModifierKeyPressed::No, ModifierKeyPressed::DontCare))
  {
    return nullptr;
  }

  const auto& map = m_document.map();
  const auto& hit = inputState.pickResult().first(type(mdl::BrushNode::BrushHitType));
  const auto initialHandlePosition =
    hit.isMatch() ? hit.hitPoint() : inputState.defaultPointUnderMouse();

  return createHandleDragTracker(
    LightVolumeDrawDragDelegate3D{m_tool, map.worldBounds()},
    inputState,
    initialHandlePosition,
    initialHandlePosition);
}

} // namespace tb::ui
