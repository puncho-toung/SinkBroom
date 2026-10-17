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

#include <memory>
#include <vector>

namespace tb::ui
{
class SidebarPanel;
struct SidebarContext;

/**
 * Creates the built-in sidebar panels. Their order is the order of the map window's docks
 * and of the default tab groups built from them; View > Sidebar and the tool sidebar sort
 * the panels by title instead.
 */
std::vector<std::unique_ptr<SidebarPanel>> createSidebarPanels(
  const SidebarContext& context);

} // namespace tb::ui
