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

#include "base/Logger.h"
#include "base/Macros.h"
#include "gl/Camera.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "render/RenderContext.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MoveHandleDragTracker.h"
#include "ui/MoveObjectsTool.h"

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

// the radius of a light drawn in a 2D view with nothing selected along the view axis, a
// default point light's
constexpr auto DrawRadius2D = 300.0;

bool startsOnSelectedNode(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  return inputState.pickResult()
    .first(type(mdl::nodeHitType()) && transitivelySelected())
    .isMatch();
}

/**
 * The line along which a corner is dragged in a 2D view (D59, D66): its line from the box
 * frame's origin without the part along the box axis closest to the view direction, so
 * that the two visible axes scale by one factor and the hidden one keeps its extent.
 */
vm::line3d cornerLine2D(
  const LightVolumeBox& volume, const mdl::Hit& hit, const vm::vec3d& viewDirection)
{
  const auto line = lightVolumeHandleLine(volume, hit);
  const auto toBox = vm::transpose(volume.rotation);

  auto diagonal = toBox * line.direction;
  diagonal[vm::find_abs_max_component(toBox * viewDirection)] = 0.0;
  return vm::line3d{line.point, volume.rotation * vm::normalize(diagonal)};
}

/**
 * The handle proposer of a drag (D66), the same with and without Shift or Alt (D65): a
 * side, a face centre, the far cap's centre and a corner along its handle line with
 * absolute snapping, like the scale tool's sides; a corner in 2D along its line in the
 * view plane; an edge, which is picked in 3D only, in the plane through the hit point
 * that faces the camera with relative snapping, like the scale tool's edges in 2D, so
 * that it follows the mouse on its two axes (the box ignores the part along the edge); a
 * surface along its edit line with relative snapping, since its hit point is not on the
 * grid.
 */
HandlePositionProposer makeHandleProposer(
  const InputState& inputState,
  const mdl::Grid& grid,
  const mdl::Hit& hit,
  const LightVolumeBox& volume,
  const vm::vec3d& handleOffset)
{
  const auto& camera = inputState.camera();

  if (hit.type() == LightVolumeTool::EdgeHitType)
  {
    // the picker moves the plane by the handle offset, so that it runs through the hit
    // point (ScaleToolController.cpp); a plane along the view ray, such as the one
    // perpendicular to an edge seen broadside, would put the edge at the eye (D66)
    const auto plane =
      vm::plane3d{hit.hitPoint() + handleOffset, vm::vec3d{camera.direction()} * -1.0};
    return makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset), makeRelativeHandleSnapper(grid));
  }

  if (hit.type() == LightVolumeTool::SurfaceHitType)
  {
    const auto line = lightVolumeHandleLine(volume, hit);
    return makeHandlePositionProposer(
      makeLineHandlePicker(line, handleOffset),
      makeRelativeLineHandleSnapper(grid, line));
  }

  const auto line =
    camera.orthographicProjection() && hit.type() == LightVolumeTool::CornerHitType
      ? cornerLine2D(volume, hit, vm::vec3d{camera.direction()})
      : lightVolumeHandleLine(volume, hit);
  return makeHandlePositionProposer(
    makeLineHandlePicker(line, handleOffset), makeAbsoluteLineHandleSnapper(grid, line));
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
    return makeProposer(inputState, handleOffset);
  }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState&, const DragState&) override
  {
    // Shift and Alt change nothing on a handle or a surface being dragged (D65, D71)
    return std::nullopt;
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
 * The move tool's delegate (MoveObjectsToolController.cpp), which moves the selection
 * when a drag starts on a surface of a selected light (D60): the move tool's translate
 * path, its grid snapping and its one undo step.
 */
class LightVolumeMoveDragDelegate : public MoveHandleDragTrackerDelegate
{
private:
  MoveObjectsTool& m_tool;

public:
  explicit LightVolumeMoveDragDelegate(MoveObjectsTool& tool)
    : m_tool{tool}
  {
  }

  DragStatus move(
    const InputState& inputState,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    switch (
      m_tool.move(inputState, proposedHandlePosition - dragState.currentHandlePosition))
    {
    case MoveObjectsTool::MoveResult::Continue:
      return DragStatus::Continue;
    case MoveObjectsTool::MoveResult::Deny:
      return DragStatus::Deny;
    case MoveObjectsTool::MoveResult::Cancel:
      return DragStatus::End;
      switchDefault();
    }
  }

  void end(const InputState& inputState, const DragState&) override
  {
    m_tool.endMove(inputState);
  }

  void cancel(const DragState&) override { m_tool.cancelMove(); }

  void setRenderOptions(
    const InputState&, render::RenderContext& renderContext) const override
  {
    renderContext.setForceShowSelectionGuide();
  }

  DragHandleSnapper makeDragHandleSnapper(
    const InputState&, const SnapMode) const override
  {
    return makeRelativeHandleSnapper(m_tool.grid());
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
 * bounds of the selected light's handle box, or the handle box of a new light of radius
 * 300 around the map's reference bounds, +-300 t, 273 at the default threshold, so that
 * the drawn light's radius along the view axis is 300 (D61). The draw shape tool's
 * reference bounds would give the selected light's own small box, a flat light.
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
  const auto halfExtent =
    DrawRadius2D * mdl::visibleFalloffT(newDrawnLight(), tool.handleThreshold());
  const auto extent = vm::vec3d{halfExtent, halfExtent, halfExtent};
  return vm::bbox3d{center - extent, center + extent};
}

} // namespace

const mdl::Hit& findLightVolumeHit(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  // 1. a handle of the selected light's box, even behind a node or a surface
  if (const auto& hit = pickResult.first(type(LightVolumeTool::HandleHitType));
      hit.isMatch())
  {
    return hit;
  }

  // 2. the nearest of the surfaces and the nodes: a selected light's surface beats the
  // nodes behind it, an unselected light's surface yields to any node along the ray
  const auto hasNodeHit = pickResult.first(type(mdl::nodeHitType())).isMatch();
  for (const auto& hit : pickResult.all())
  {
    if (hit.hasType(mdl::nodeHitType()))
    {
      return mdl::Hit::NoHit;
    }
    if (
      hit.hasType(LightVolumeTool::SurfaceHitType)
      && (hit.target<LightSurfaceHit>().selected || !hasNodeHit))
    {
      return hit;
    }
  }

  return mdl::Hit::NoHit;
}

LightVolumeToolController::LightVolumeToolController(
  LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document)
  : m_tool{tool}
  , m_moveTool{moveTool}
  , m_document{document}
{
}

LightVolumeToolController::~LightVolumeToolController() = default;

Tool& LightVolumeToolController::tool()
{
  // the controller works whenever the always-on handles do (D56)
  return m_tool.handles();
}

const Tool& LightVolumeToolController::tool() const
{
  return m_tool.handles();
}

void LightVolumeToolController::pick(
  const InputState& inputState, mdl::PickResult& pickResult)
{
  if (!m_tool.drawing())
  {
    doPick(inputState, pickResult);
  }
}

void LightVolumeToolController::modifierKeyChange(const InputState&)
{
  // Shift and Alt change nothing on a handle (D65, D71); the mouse may be over a
  // different handle now
  m_tool.refreshViews();
}

void LightVolumeToolController::mouseMove(const InputState& inputState)
{
  if (!m_tool.drawing() && !inputState.anyToolDragging())
  {
    m_tool.setHighlightedHit(findLightVolumeHit(inputState.pickResult()));
  }
}

bool LightVolumeToolController::mouseClick(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  if (!inputState.mouseButtonsPressed(MouseButtons::Left))
  {
    return false;
  }

  const auto& pickResult = inputState.pickResult();

  if (m_tool.drawing())
  {
    // a click on nothing, or a drag shorter than 100 ms, keeps the light that a volume
    // is drawn for selected
    if (inputState.modifierKeysDown(ModifierKeys::CtrlCmd) || !m_tool.volumeBox())
    {
      return false;
    }

    const auto& editorContext = m_document.map().editorContext();
    const auto isSelectable = [&](const mdl::Hit& hit) {
      auto* node = mdl::hitToNode(hit);
      return node && editorContext.selectable(*mdl::findOutermostClosedGroupOrNode(node));
    };
    return !pickResult.first(type(mdl::nodeHitType()) && isSelectable).isMatch();
  }

  // a copy: selecting the light makes every view pick again, which replaces the pick
  // result that the found hit belongs to
  const auto hit = findLightVolumeHit(pickResult);

  // a click on a handle, or a drag shorter than 100 ms, keeps the light selected and
  // replaces the clicked surface (D68); Ctrl and the left button is the selection tool's
  // toggle
  if (hit.hasType(LightVolumeTool::HandleHitType))
  {
    if (inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      return false;
    }
    m_tool.clearClickedSurface();
    return true;
  }

  return hit.type() == LightVolumeTool::SurfaceHitType && selectSurface(inputState, hit);
}

bool LightVolumeToolController::selectSurface(
  const InputState& inputState, const mdl::Hit& hit)
{
  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  // the selection tool's rule for a click on a node (SelectionTool::mouseClick): Alt is
  // not a click, Shift selects brush faces, Ctrl toggles
  if (
    !editorContext.canChangeSelection()
    || !inputState.checkModifierKeys(
      ModifierKeyPressed::DontCare, ModifierKeyPressed::No, ModifierKeyPressed::No))
  {
    return false;
  }

  auto* node = mdl::findOutermostClosedGroupOrNode(hit.target<LightSurfaceHit>().node);
  if (!editorContext.selectable(*node))
  {
    return false;
  }

  if (inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
  {
    if (node->selected())
    {
      mdl::deselectNodes(map, {node});
    }
    else
    {
      auto transaction = mdl::Transaction{map, "Select Object"};
      if (map.selection().hasBrushFaces())
      {
        mdl::deselectAll(map);
      }
      mdl::selectNodes(map, {node});
      if (!transaction.commit())
      {
        map.logger().error() << "Could not select the light";
      }
    }
  }
  else
  {
    auto transaction = mdl::Transaction{map, "Select Object"};
    mdl::deselectAll(map);
    mdl::selectNodes(map, {node});
    if (!transaction.commit())
    {
      map.logger().error() << "Could not select the light";
    }
  }

  // the clicked element keeps a bold outline (D60)
  m_tool.setClickedSurface(hit);
  return true;
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

  if (m_tool.drawing())
  {
    // a drag that starts on a selected node moves it
    return m_tool.canDraw() && !startsOnSelectedNode(inputState)
             ? acceptDrawDrag(inputState)
             : nullptr;
  }

  // a copy, like the click's: a document change at the drag's start makes the views
  // pick again
  const auto hit = findLightVolumeHit(inputState.pickResult());
  if (hit.type() == LightVolumeTool::SurfaceHitType)
  {
    // the surfaces of a light that is not selected are not dragged, as brushes are not
    if (!hit.target<LightSurfaceHit>().selected)
    {
      return nullptr;
    }

    // a plain drag moves the selection like the move tool, Alt vertically (D60)
    if (
      inputState.modifierKeysPressed(ModifierKeys::None)
      || inputState.modifierKeysPressed(ModifierKeys::Alt))
    {
      return m_moveTool.active() && m_moveTool.startMove(inputState)
               ? createMoveHandleDragTracker(
                   LightVolumeMoveDragDelegate{m_moveTool},
                   inputState,
                   hit.hitPoint(),
                   hit.hitPoint())
               : nullptr;
    }

    // Shift edits the element
    if (!inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      return nullptr;
    }
  }

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
  if (m_tool.drawing())
  {
    m_tool.setMode(LightVolumeTool::Mode::Handles);
    return true;
  }
  return false;
}

// LightVolumeToolController2D

LightVolumeToolController2D::LightVolumeToolController2D(
  LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document)
  : LightVolumeToolController{tool, moveTool, document}
{
}

void LightVolumeToolController2D::doPick(
  const InputState& inputState, mdl::PickResult& pickResult) const
{
  // the 2D views draw no fills, so they pick no surfaces (D57)
  m_tool.pick2D(inputState.pickRay(), inputState.camera(), pickResult);
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
  LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document)
  : LightVolumeToolController{tool, moveTool, document}
{
}

void LightVolumeToolController3D::doPick(
  const InputState& inputState, mdl::PickResult& pickResult) const
{
  m_tool.pick3D(inputState.pickRay(), inputState.camera(), pickResult);

  // a drag changes the lights at every step: their surfaces are picked again after it
  if (!inputState.anyToolDragging())
  {
    m_tool.pickSurfaces(inputState.pickRay(), pickResult);
  }
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
