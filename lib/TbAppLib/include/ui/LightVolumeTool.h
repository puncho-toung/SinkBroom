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

#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "mdl/Hit.h"
#include "mdl/HitType.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "ui/ScaleTool.h"
#include "ui/Tool.h"

#include "vm/bbox.h"
#include "vm/line.h"
#include "vm/mat.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <optional>
#include <vector>

namespace tb
{
namespace gl
{
class Camera;
}

namespace mdl
{
class EntityNode;
class Grid;
class Node;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class MapDocument;

/**
 * The volume of a light as the light volume tool edits it: the light, its frame, the
 * rotation of the box frame (the light's rotation R for a point light or a sun, R B for a
 * spot light, where B is the beam frame) and the light's edit box in the box frame.
 */
struct LightVolumeBox
{
  mdl::LightProperties light;
  mdl::LightFrame frame;
  vm::mat3x3d rotation;
  vm::bbox3d box;

  /**
   * The world position of the given point of the box frame.
   */
  vm::vec3d toWorld(const vm::vec3d& point) const;

  /**
   * The point of the box frame at the given world position.
   */
  vm::vec3d toBox(const vm::vec3d& world) const;

  /**
   * The given pick ray in the box frame; distances along it are unchanged.
   */
  vm::ray3d toBox(const vm::ray3d& ray) const;
};

/**
 * Returns the volume of the given light entity in the given policy's frame, or nothing if
 * the light is degenerate. The box is the light's edit box, a point light's at its full
 * radius.
 */
std::optional<LightVolumeBox> lightVolumeBox(
  const mdl::Entity& entity, mdl::EntityRotationPolicy policy);

/**
 * Returns the volume of the given light entity whose box is the handle box of the given
 * threshold (mdl::lightEditBox with a threshold, D61): a point light's box lies where the
 * light fades below the threshold.
 */
std::optional<LightVolumeBox> lightVolumeBox(
  const mdl::Entity& entity, mdl::EntityRotationPolicy policy, double threshold);

/**
 * A hit on the surface of a light volume (light editor plan, D60): the light, the element
 * that was hit, and where. It carries the light's properties and frame, so that the
 * highlight of the element needs no node.
 */
struct LightSurfaceHit
{
  // identity only, never dereferenced after a notification
  mdl::EntityNode* node = nullptr;
  mdl::LightElement element = mdl::LightElement::OuterShell;
  // the light is selected, or is in a selected group
  bool selected = false;
  mdl::LightProperties light;
  mdl::LightFrame frame;
  // the hit point in the light's local space
  vm::vec3d localPoint;
};

/**
 * The world line along which the handle of the given hit of the light volume tool is
 * dragged: the side's normal through its centre, the line from the opposite edge's centre
 * to the edge's centre, the diagonal from the opposite corner to the corner, the beam
 * through the far cap's centre. A copy of the scale tool's handleLineForHit in the box
 * frame. For a surface, the line through the hit point along which a Shift drag edits
 * the element (D60): the axis of the outer shell's normal there, the ray from the
 * light's origin for the inner shell, the base's widening for the funnel, the beam for
 * the base.
 */
vm::line3d lightVolumeHandleLine(const LightVolumeBox& volume, const mdl::Hit& hit);

/**
 * The light that draw mode creates when nothing is selected: the engine's default point
 * light with the math block that the light editor seeds (D24, D29).
 */
mdl::LightProperties newDrawnLight();

/**
 * The world triangles, three points each and CW seen from outside, of the given element
 * of the given light's volume in the given frame: what a surface hit highlights (D60) and
 * what the funnel of a spot light and the box of a sun are picked on (D57). Built at full
 * alpha, so that an element exists even where the volume is invisible; nothing for an
 * element that the light does not have.
 */
std::vector<vm::vec3d> lightElementTriangles(
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  mdl::LightElement element);

class LightVolumeTool;

/**
 * The always-on part of the light volume tool (light editor plan, D56): the handles of
 * the selected light's box and the surfaces of the shown light volumes work without the
 * tool being activated. It is a tool of its own so that the tool box suppresses it while
 * a modal tool (rotate, scale, the vertex tools, ...) is active, like the move tool; the
 * light volume tool's controllers are active while it is.
 */
class LightVolumeHandles : public Tool
{
private:
  LightVolumeTool& m_tool;

public:
  explicit LightVolumeHandles(LightVolumeTool& tool);

private:
  bool doDeactivate() override;
};

/**
 * Edits the volume of the selected light with the handles of its box, like the scale
 * tool edits a brush, and with the surfaces of the shown light volumes (D60), always,
 * without being activated (D56); and, while it is active, draws a new volume for the
 * selected light, or for a new light, like the draw shape tool draws a brush. The draw
 * mode is reached only through the light editor panel.
 *
 * The tool holds no node across a notification: every call reads the light from the
 * selection again, and the cache of the lights whose surfaces are picked is dropped when
 * the document changes. A drag rolls back and applies the changed light from the light
 * at the start of the drag in one long running transaction, so that it is one undo step.
 */
class LightVolumeTool : public Tool
{
public:
  enum class Mode
  {
    Handles,
    Draw,
  };

  static const mdl::HitType::Type SideHitType;
  static const mdl::HitType::Type EdgeHitType;
  static const mdl::HitType::Type CornerHitType;
  static const mdl::HitType::Type FarCapCenterHitType;
  static const mdl::HitType::Type FaceCenterHitType;
  static const mdl::HitType::Type SurfaceHitType;
  // the handles of the box, which take precedence over everything under the mouse
  static const mdl::HitType::Type HandleHitType;
  static const mdl::HitType::Type AnyHitType;

  Notifier<> modeDidChangeNotifier;

private:
  /**
   * A light whose surfaces can be picked, with its bounding sphere for the pre-test.
   */
  struct PickableLight
  {
    mdl::EntityNode* node = nullptr;
    mdl::LightProperties light;
    mdl::LightFrame frame;
    // its volume is shown now: it is selected, or the volumes of all lights are shown
    bool shown = false;
    bool selected = false;
    // it has a volume that can be picked: not light_origin (D8), not degenerate
    bool pickable = false;
    vm::vec3d centre;
    double radius = 0.0;
    // the world triangles of a spot light's funnel or a sun's box, CW seen from outside
    std::vector<vm::vec3d> triangles;
  };

  MapDocument& m_document;
  LightVolumeHandles m_handles;
  Mode m_mode = Mode::Handles;

  AnchorPos m_anchorPos = AnchorPos::Opposite;
  ProportionalAxes m_proportionalAxes = ProportionalAxes::None();
  mdl::Hit m_highlightedHit = mdl::Hit::NoHit;
  // the surface that was last clicked, kept with a bold outline while it is under the
  // mouse; never dereferenced
  const mdl::EntityNode* m_clickedNode = nullptr;
  std::optional<mdl::LightElement> m_clickedElement;

  bool m_dragging = false;
  mdl::Hit m_dragHit = mdl::Hit::NoHit;
  std::optional<LightVolumeBox> m_dragStart;
  double m_dragThreshold = 0.0;
  vm::bbox3d m_dragBox;
  mdl::LightProperties m_dragLight;
  vm::vec3d m_dragDelta;

  std::optional<vm::bbox3d> m_drawBox;
  std::optional<mdl::LightProperties> m_drawLight;

  // the lights read for the surface picks; a selection change only updates which are
  // shown and selected, a changed entity is read again alone, anything else drops them
  mutable std::vector<PickableLight> m_pickableLights;
  mutable bool m_pickableLightsValid = false;
  mutable bool m_pickableMembershipValid = false;
  mutable std::vector<const mdl::EntityNode*> m_changedLights;
  mutable std::size_t m_pickableLightBuilds = 0;

  NotifierConnection m_notifierConnection;

public:
  explicit LightVolumeTool(MapDocument& document);
  ~LightVolumeTool() override;

  const mdl::Grid& grid() const;

  /**
   * The always-on part of the tool, see LightVolumeHandles.
   */
  LightVolumeHandles& handles();
  const LightVolumeHandles& handles() const;

  /**
   * Whether the light volumes, their handles and surfaces are shown: the game has the
   * light editor and the light volumes are not turned off (D45, D56).
   */
  bool showsVolumes() const;

  /**
   * The handle threshold of the preferences (D53), clamped to [0.01, 0.5].
   */
  double handleThreshold() const;

  Mode mode() const;
  /**
   * Switches the mode; a drawn volume that was not finished is discarded.
   */
  void setMode(Mode mode);

  /**
   * Whether the tool is active in draw mode.
   */
  bool drawing() const;

  /**
   * Whether a handle or surface drag is in progress.
   */
  bool isDragging() const;

  /**
   * The one selected light: the selection holds exactly one node, an entity that the
   * light editor edits, in a game with the light editor. Null otherwise.
   */
  mdl::EntityNode* selectedLight() const;

  /**
   * The selected light's volume with its handle box at the handle threshold, or nothing
   * if there is no selected light, if it has light_origin (read only, D8) or if it is
   * degenerate.
   */
  std::optional<LightVolumeBox> volumeBox() const;

  /**
   * The selected light's handle box in its box frame, see volumeBox.
   */
  std::optional<vm::bbox3d> editBox() const;

  /**
   * The selected light's frame, see volumeBox.
   */
  std::optional<mdl::LightFrame> frame() const;

  /**
   * Picks the handles of the selected light's box: its corners, the centres of its faces
   * (D58; a spot light's far face has the far cap's centre, its apex face none) and its
   * edges, nearest first; nothing while the light volumes are not shown.
   */
  void pick3D(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const;

  /**
   * Picks the handles of the selected light's box in a 2D view: its corners, the far
   * cap's centre, and its outline, whose edges are picked as the sides perpendicular to
   * them in the view plane (D59).
   */
  void pick2D(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const;

  /**
   * Picks the surfaces of the shown light volumes (D57, D60), at most one per light: the
   * outer shell of a point light, or of a selected one the inner shell where the ray
   * enters it before any node hit of the pick result; the funnel or the base of a spot
   * light; the box of a sun. Surfaces are picked from outside only, like brush faces. The
   * surfaces of unselected lights are not picked if the pick result already holds a node
   * hit, because they would yield to it.
   */
  void pickSurfaces(const vm::ray3d& pickRay, mdl::PickResult& pickResult) const;

  /**
   * How many times a light was read for the surface picks since the tool was made: a
   * selection change reads none, a changed light only itself (I7.10).
   */
  std::size_t pickableLightBuildCount() const;

  void setAnchorPos(AnchorPos anchorPos);
  AnchorPos anchorPos() const;

  void setProportionalAxes(const ProportionalAxes& proportionalAxes);
  const ProportionalAxes& proportionalAxes() const;

  const mdl::Hit& highlightedHit() const;
  /**
   * Sets the handle or surface under the mouse and refreshes the views if it changed. A
   * clicked surface loses its bold outline when another one is highlighted.
   */
  void setHighlightedHit(const mdl::Hit& hit);

  /**
   * Marks the given surface hit as clicked: its element keeps a bold outline while it is
   * under the mouse, until the selection changes (D60).
   */
  void setClickedSurface(const mdl::Hit& hit);

  /**
   * Whether the given surface hit is the one that was clicked last.
   */
  bool isClickedSurface(const mdl::Hit& hit) const;

  /**
   * Starts dragging the handle of the given hit in a long running transaction, or, for a
   * surface of the selected light, editing its element (D60). Returns false if no light
   * is selected, if the surface is not the selected light's or if its element cannot be
   * edited (a sun's box). The handle threshold is fixed for the drag (D61).
   */
  bool beginDrag(const mdl::Hit& hit);

  const mdl::Hit& dragHit() const;

  /**
   * The volume at the start of the drag; nothing if no drag is in progress.
   */
  const std::optional<LightVolumeBox>& dragStartVolume() const;

  /**
   * Moves the dragged handle or surface by the given world delta, the step since the last
   * accepted update; the tool adds the steps up to the delta since the start of the drag.
   * Returns false if the step is denied: the moved box is denied, and the light keeps the
   * last accepted box and the step is not added. Also returns false if no drag is in
   * progress, and if the light cannot be changed, in which case the drag is cancelled.
   */
  bool update(const vm::vec3d& delta);

  /**
   * Commits the drag, or cancels it if the light is back at its starting box or, for a
   * surface, its starting properties. Does nothing if no drag is in progress (it was
   * cancelled by a deactivation or a failed update while the drag tracker lived on).
   */
  void endDrag();

  /**
   * Cancels the drag, if one is in progress.
   */
  void cancelDrag();

  /**
   * Whether a volume can be drawn: draw mode with exactly one editable light selected, or
   * with nothing selected and a point entity definition named "light".
   */
  bool canDraw() const;

  /**
   * Sets the world box of the volume being drawn: a selected spot light fills it with its
   * beam pointing down (a box lower than 24 is extended downwards), a selected point
   * light or sun or a new light fills it as its box. Returns false if the box is refused
   * (a half extent below 1), which keeps the last box.
   */
  bool updateDraw(const vm::bbox3d& worldBox);

  /**
   * Applies the drawn volume to the selected light, or creates a new light with it, and
   * returns to the handle mode.
   */
  void endDraw();
  void cancelDraw();

  const std::optional<vm::bbox3d>& drawBox() const;
  const std::optional<mdl::LightProperties>& drawLight() const;

  /**
   * Renders the handle box, the handles, the dragged handle, the highlighted handle or
   * surface if requested (not while another tool drags) and a spot light's frustum edges,
   * or the drawn volume in draw mode.
   */
  void render(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    bool showHighlightedHit) const;

private:
  bool doActivate() override;
  bool doDeactivate() override;

  std::optional<LightVolumeBox> volumeBox(double threshold) const;

  void invalidatePickableLights();
  void lightsDidChange();
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  const std::vector<PickableLight>& pickableLights() const;
  void updatePickableLights() const;
  PickableLight pickableLight(
    mdl::EntityNode* entityNode, mdl::EntityRotationPolicy policy) const;

  void resetDraw();
  bool applyDrawnLight();
  bool createDrawnLight();
};

} // namespace ui
} // namespace tb
