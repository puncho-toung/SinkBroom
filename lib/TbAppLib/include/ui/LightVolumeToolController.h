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

#include "vm/ray.h"

#include <memory>

namespace tb
{
namespace gl
{
class Camera;
}

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

/**
 * Returns the hit of the light volume tool that a click or a drag with the given pick
 * result takes: a corner, an edge or the far cap's centre, even behind a selected node;
 * else nothing if a selected node is hit, so that the move tool moves it; else a side.
 */
const mdl::Hit& findLightVolumeHit(const mdl::PickResult& pickResult);

/**
 * The light volume tool's controller. It sits right after the camera controller and
 * before the move controller, so that it sees drags of the handles that lie behind the
 * selected light's own box, and it declines drags that start on a selected node unless a
 * corner, an edge or the far cap's centre is hit.
 */
class LightVolumeToolController : public ToolController
{
protected:
  LightVolumeTool& m_tool;
  MapDocument& m_document;

public:
  LightVolumeToolController(LightVolumeTool& tool, MapDocument& document);
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

  virtual void doPick(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const = 0;
  virtual std::unique_ptr<GestureTracker> acceptDrawDrag(
    const InputState& inputState) = 0;
};

class LightVolumeToolController2D : public LightVolumeToolController
{
public:
  LightVolumeToolController2D(LightVolumeTool& tool, MapDocument& document);

private:
  void doPick(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const override;
  std::unique_ptr<GestureTracker> acceptDrawDrag(const InputState& inputState) override;
};

class LightVolumeToolController3D : public LightVolumeToolController
{
public:
  LightVolumeToolController3D(LightVolumeTool& tool, MapDocument& document);

private:
  void doPick(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    mdl::PickResult& pickResult) const override;
  std::unique_ptr<GestureTracker> acceptDrawDrag(const InputState& inputState) override;
};

} // namespace ui
} // namespace tb
