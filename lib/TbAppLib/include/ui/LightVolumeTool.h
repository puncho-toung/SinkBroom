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
 * the light is degenerate.
 */
std::optional<LightVolumeBox> lightVolumeBox(
  const mdl::Entity& entity, mdl::EntityRotationPolicy policy);

/**
 * The world line along which the handle of the given hit of the light volume tool is
 * dragged: the side's normal through its centre, the line from the opposite edge's centre
 * to the edge's centre, the diagonal from the opposite corner to the corner, the beam
 * through the far cap's centre. A copy of the scale tool's handleLineForHit in the box
 * frame.
 */
vm::line3d lightVolumeHandleLine(const LightVolumeBox& volume, const mdl::Hit& hit);

/**
 * The math block of a light that draw mode creates: the in-game editor's auto-seed, a
 * soft linear falloff (light editor plan, D29).
 */
mdl::MathLightProperties lightVolumeSeedMath();

/**
 * Edits the volume of the one selected light with the handles of its edit box, like the
 * scale tool edits a brush, or draws a new volume for it, or for a new light, like the
 * draw shape tool draws a brush. It is reached only through the light editor panel.
 *
 * The tool holds no node: every call reads the light from the selection again. A drag
 * rolls back and applies the changed light from the light at the start of the drag in one
 * long running transaction, so that it is one undo step.
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
  // the hits that take precedence over a selected node under the mouse
  static const mdl::HitType::Type HandleHitType;
  static const mdl::HitType::Type AnyHitType;

  Notifier<> modeDidChangeNotifier;

private:
  MapDocument& m_document;
  Mode m_mode = Mode::Handles;

  AnchorPos m_anchorPos = AnchorPos::Opposite;
  ProportionalAxes m_proportionalAxes = ProportionalAxes::None();
  mdl::Hit m_highlightedHit = mdl::Hit::NoHit;

  bool m_dragging = false;
  mdl::Hit m_dragHit = mdl::Hit::NoHit;
  std::optional<LightVolumeBox> m_dragStart;
  vm::bbox3d m_dragBox;
  vm::vec3d m_dragDelta;

  std::optional<vm::bbox3d> m_drawBox;
  std::optional<mdl::LightProperties> m_drawLight;

public:
  explicit LightVolumeTool(MapDocument& document);
  ~LightVolumeTool() override;

  const mdl::Grid& grid() const;

  Mode mode() const;
  /**
   * Switches the mode; a drawn volume that was not finished is discarded.
   */
  void setMode(Mode mode);

  /**
   * Whether a handle drag is in progress.
   */
  bool isDragging() const;

  /**
   * The one selected light: the selection holds exactly one node, an entity that the
   * light editor edits, in a game with the light editor. Null otherwise.
   */
  mdl::EntityNode* selectedLight() const;

  /**
   * The selected light's volume, or nothing if there is no selected light, if it has
   * light_origin (read only, D8) or if it is degenerate.
   */
  std::optional<LightVolumeBox> volumeBox() const;

  /**
   * The selected light's edit box in its box frame, see volumeBox.
   */
  std::optional<vm::bbox3d> editBox() const;

  /**
   * The selected light's frame, see volumeBox.
   */
  std::optional<mdl::LightFrame> frame() const;

  void pick3D(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const;
  void pick2D(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const;

  void setAnchorPos(AnchorPos anchorPos);
  AnchorPos anchorPos() const;

  void setProportionalAxes(const ProportionalAxes& proportionalAxes);
  const ProportionalAxes& proportionalAxes() const;

  const mdl::Hit& highlightedHit() const;
  /**
   * Sets the handle under the mouse and refreshes the views if it changed.
   */
  void setHighlightedHit(const mdl::Hit& hit);

  /**
   * Starts dragging the handle of the given hit in a long running transaction. Returns
   * false if no light is selected.
   */
  bool beginDrag(const mdl::Hit& hit);

  const mdl::Hit& dragHit() const;

  /**
   * The volume at the start of the drag. Only while dragging.
   */
  const LightVolumeBox& dragStartVolume() const;

  /**
   * Moves the dragged handle by the given world delta, accumulated since the start of the
   * drag. Returns false if the moved box is denied (the light keeps the last accepted
   * box) and the delta is then not accumulated.
   */
  bool update(const vm::vec3d& delta);

  /**
   * Commits the drag, or cancels it if the light is back at its starting box.
   */
  void endDrag();
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
   * Renders the handle box, the handles, the highlighted handle and a spot light's
   * frustum edges in handle mode, the drawn volume in draw mode.
   */
  void render(
    render::RenderContext& renderContext, render::RenderBatch& renderBatch) const;

private:
  bool doActivate() override;
  bool doDeactivate() override;

  void resetDraw();
  bool applyDrawnLight();
  bool createDrawnLight();
};

} // namespace ui
} // namespace tb
