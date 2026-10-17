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

#include "SidebarTestUtils.h"
#include "ui/CatchConfig.h"
#include "ui/SidebarPanel.h"
#include "ui/SidebarPanels.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("SidebarPanels")
{
  SECTION("createSidebarPanels")
  {
    auto fixture = SidebarContextFixture{};
    const auto panels = createSidebarPanels(fixture.context());

    // a new panel is one more entry, so only the built-in ones are pinned
    CHECK(std::ranges::any_of(
      panels, [](const auto& panel) { return panel->panelId() == "LightEditor"; }));

    // the inspectors come first, in the order of the old inspector's pages, which is the
    // order of their tabs
    REQUIRE(panels.size() >= 4);
    CHECK(panels[0]->panelId() == "MapInspector");
    CHECK(panels[1]->panelId() == "EntityInspector");
    CHECK(panels[2]->panelId() == "FaceInspector");
  }

  SECTION("the inspector icons are greyscale 24x24 images")
  {
    for (const auto* name :
         {"MapInspector.svg", "EntityInspector.svg", "FaceInspector.svg"})
    {
      checkGreyscaleIcon(name);
    }
  }
}

} // namespace tb::ui
