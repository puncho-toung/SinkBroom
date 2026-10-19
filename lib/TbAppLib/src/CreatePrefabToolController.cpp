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

#include "ui/CreatePrefabToolController.h"

#include "ui/CreatePrefabTool.h"
#include "ui/DropTracker.h"
#include "ui/InputState.h"

#include "kd/string_compare.h"

#include <functional>
#include <string>
#include <string_view>

namespace tb::ui
{
namespace
{

// C4: "prefab:user/<relative path>" or "prefab:mod/<relative path>"
constexpr auto PayloadPrefix = std::string_view{"prefab:"};

class CreatePrefabDropTracker : public DropTracker
{
private:
  using UpdatePrefabPosition = std::function<void(const InputState&, CreatePrefabTool&)>;

  CreatePrefabTool& m_tool;
  UpdatePrefabPosition m_updatePrefabPosition;

public:
  explicit CreatePrefabDropTracker(
    const InputState& inputState,
    CreatePrefabTool& tool,
    UpdatePrefabPosition updatePrefabPosition)
    : m_tool{tool}
    , m_updatePrefabPosition{std::move(updatePrefabPosition)}
  {
    m_updatePrefabPosition(inputState, m_tool);
  }

  bool move(const InputState& inputState) override
  {
    m_updatePrefabPosition(inputState, m_tool);
    return true;
  }

  bool drop(const InputState&) override
  {
    m_tool.commit();
    return true;
  }

  void leave(const InputState&) override { m_tool.cancel(); }
};

} // namespace

CreatePrefabToolController::CreatePrefabToolController(CreatePrefabTool& tool)
  : m_tool{tool}
{
}

CreatePrefabToolController::~CreatePrefabToolController() = default;

Tool& CreatePrefabToolController::tool()
{
  return m_tool;
}

const Tool& CreatePrefabToolController::tool() const
{
  return m_tool;
}

bool CreatePrefabToolController::shouldAcceptDrop(
  const InputState&, const std::string& payload) const
{
  // DD-29: refused at drag enter, so the view shows the no-drop cursor
  return m_tool.placementEnabled() && kdl::cs::str_is_prefix(payload, PayloadPrefix);
}

std::unique_ptr<DropTracker> CreatePrefabToolController::acceptDrop(
  const InputState& inputState, const std::string& payload)
{
  return m_tool.createPrefab(payload) ? createDropTracker(inputState) : nullptr;
}

bool CreatePrefabToolController::cancel()
{
  return false;
}

CreatePrefabToolController2D::CreatePrefabToolController2D(CreatePrefabTool& tool)
  : CreatePrefabToolController{tool}
{
}

std::unique_ptr<DropTracker> CreatePrefabToolController2D::createDropTracker(
  const InputState& inputState) const
{
  return std::make_unique<CreatePrefabDropTracker>(
    inputState, m_tool, [](const auto& is, auto& t) {
      t.updatePrefabPosition2D(is.pickRay());
    });
}

CreatePrefabToolController3D::CreatePrefabToolController3D(CreatePrefabTool& tool)
  : CreatePrefabToolController{tool}
{
}

std::unique_ptr<DropTracker> CreatePrefabToolController3D::createDropTracker(
  const InputState& inputState) const
{
  return std::make_unique<CreatePrefabDropTracker>(
    inputState, m_tool, [](const auto& is, auto& t) {
      t.updatePrefabPosition3D(is.pickRay(), is.pickResult());
    });
}

} // namespace tb::ui
