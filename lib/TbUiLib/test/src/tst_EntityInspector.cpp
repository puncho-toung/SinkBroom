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
#include "ui/EntityInspector.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("EntityInspector")
{
  auto fixture = SidebarContextFixture{};
  const auto context = fixture.context();
  auto panel = EntityInspector{context.appController, context.document};

  SECTION("panelId")
  {
    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "EntityInspector");
    CHECK(panel.panelId() == EntityInspector::PanelId);
  }

  SECTION("title")
  {
    CHECK(panel.title() == "Entity Inspector");
  }

  SECTION("iconPath")
  {
    CHECK(panel.iconPath() == "EntityInspector.svg");
    checkGreyscaleIcon(panel.iconPath());
  }

  SECTION("shownByDefault")
  {
    CHECK(panel.shownByDefault());
  }

  SECTION("minimumWidth")
  {
    // as wide as the inspector that held the page
    CHECK(panel.minimumWidth() == 350);
  }
}

} // namespace tb::ui
