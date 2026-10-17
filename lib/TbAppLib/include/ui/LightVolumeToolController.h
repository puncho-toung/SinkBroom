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

#include "ui/ToolController.h"

#include <memory>

namespace tb
{
namespace mdl
{
class Hit;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class LightVolumeTool;
class MapDocument;
class MoveObjectsTool;

/**
 * Returns the hit of the light volume tool that a click or a drag with the given pick
 * result takes (D57): a handle of the selected light's box, even behind anything else;
 * else the nearest of the surfaces and the nodes, where a surface of a selected light
 * beats the nodes behind it and a surface of an unselected light is passed over if any
 * node is hit at all, so that the brushes seen through a volume stay clickable; nothing
 * if a node is nearest. Returns a copy, which stays valid after the pick result is
 * replaced: a selection or document change makes every view pick again.
 */
mdl::Hit findLightVolumeHit(const mdl::PickResult& pickResult);

/**
 * The light volume tool's controller. It is active whenever the tool's always-on handles
 * are (D56), and it sits right after the camera controller and before the move
 * controller, so that it sees drags of the handles that lie behind the selected light's
 * own box.
 *
 * A handle drag scales the box about the light's origin, with or without Shift and Alt
 * (D64, D65); on a surface (D60), a click selects the light (Ctrl toggles it), a drag on
 * a selected light moves it as the move tool moves the selection, and a Shift drag edits
 * the element. While the tool is active, a drag draws a new volume instead.
 */
class LightVolumeToolController : public ToolController
{
protected:
  LightVolumeTool& m_tool;
  MoveObjectsTool& m_moveTool;
  MapDocument& m_document;

public:
  LightVolumeToolController(
    LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document);
  ~LightVolumeToolController() override;

private:
  Tool& tool() override;
  const Tool& tool() const override;

  void pick(const InputState& inputState, mdl::PickResult& pickResult) override;

  void modifierKeyChange(const InputState& inputState) override;

  void mouseMove(const InputState& inputState) override;
  bool mouseClick(const InputState& inputState) override;

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override;

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override;

  bool cancel() override;

  bool selectSurface(const InputState& inputState, const mdl::Hit& hit);

  virtual void doPick(
    const InputState& inputState, mdl::PickResult& pickResult) const = 0;
  virtual std::unique_ptr<GestureTracker> acceptDrawDrag(
    const InputState& inputState) = 0;
};

class LightVolumeToolController2D : public LightVolumeToolController
{
public:
  LightVolumeToolController2D(
    LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document);

private:
  void doPick(const InputState& inputState, mdl::PickResult& pickResult) const override;
  std::unique_ptr<GestureTracker> acceptDrawDrag(const InputState& inputState) override;
};

class LightVolumeToolController3D : public LightVolumeToolController
{
public:
  LightVolumeToolController3D(
    LightVolumeTool& tool, MoveObjectsTool& moveTool, MapDocument& document);

private:
  void doPick(const InputState& inputState, mdl::PickResult& pickResult) const override;
  std::unique_ptr<GestureTracker> acceptDrawDrag(const InputState& inputState) override;
};

} // namespace ui
} // namespace tb
