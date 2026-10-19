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

#include <string>

namespace tb::ui
{
class CreatePrefabTool;

class CreatePrefabToolController : public ToolController
{
protected:
  CreatePrefabTool& m_tool;

protected:
  explicit CreatePrefabToolController(CreatePrefabTool& tool);

public:
  ~CreatePrefabToolController() override;

private:
  Tool& tool() override;
  const Tool& tool() const override;

  bool shouldAcceptDrop(
    const InputState& inputState, const std::string& payload) const override;
  std::unique_ptr<DropTracker> acceptDrop(
    const InputState& inputState, const std::string& payload) override;

  bool cancel() override;

private:
  virtual std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const = 0;
};

class CreatePrefabToolController2D : public CreatePrefabToolController
{
public:
  explicit CreatePrefabToolController2D(CreatePrefabTool& tool);

private:
  std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const override;
};

class CreatePrefabToolController3D : public CreatePrefabToolController
{
public:
  explicit CreatePrefabToolController3D(CreatePrefabTool& tool);

private:
  std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const override;
};

} // namespace tb::ui
