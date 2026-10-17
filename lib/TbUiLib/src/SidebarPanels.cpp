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

#include "ui/SidebarPanels.h"

#include "ui/EntityInspector.h"
#include "ui/FaceInspector.h"
#include "ui/LightEditorPanel.h"
#include "ui/MapInspector.h"
#include "ui/SidebarPanel.h"

namespace tb::ui
{

std::vector<std::unique_ptr<SidebarPanel>> createSidebarPanels(
  const SidebarContext& context)
{
  auto panels = std::vector<std::unique_ptr<SidebarPanel>>{};
  panels.push_back(std::make_unique<MapInspector>(context.document));
  panels.push_back(
    std::make_unique<EntityInspector>(context.appController, context.document));
  panels.push_back(
    std::make_unique<FaceInspector>(context.appController, context.document));
  panels.push_back(std::make_unique<LightEditorPanel>(context));
  // add future panels here, one line each
  return panels;
}

} // namespace tb::ui
