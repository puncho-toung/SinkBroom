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

#include <QCheckBox>

#include "SidebarTestUtils.h"
#include "ui/CatchConfig.h"
#include "ui/FlagsPopupEditor.h"
#include "ui/IssueBrowser.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("IssueBrowser")
{
  auto fixture = SidebarContextFixture{};
  auto panel = IssueBrowser{fixture.document()};

  SECTION("panelId")
  {
    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "Issues");
    CHECK(panel.panelId() == IssueBrowser::PanelId);
  }

  SECTION("title")
  {
    CHECK(panel.title() == "Issues");
  }

  SECTION("iconPath")
  {
    CHECK(panel.iconPath() == "Issues.svg");
    checkGreyscaleIcon(panel.iconPath());
  }

  SECTION("shownByDefault")
  {
    CHECK(panel.shownByDefault());
  }

  SECTION("defaultArea")
  {
    // where the info panel was, under the map views
    CHECK(panel.defaultArea() == Qt::BottomDockWidgetArea);
  }

  SECTION("minimumHeight")
  {
    // as tall as the info panel that held the page
    CHECK(panel.minimumHeight() == 100);
  }

  SECTION("the controls that sat in the info panel's tab bar are in the panel")
  {
    const auto checkBoxes = panel.findChildren<QCheckBox*>();
    CHECK(std::ranges::any_of(checkBoxes, [](const auto* checkBox) {
      return checkBox->text() == "Show hidden issues";
    }));
    CHECK(panel.findChild<FlagsPopupEditor*>() != nullptr);
  }
}

} // namespace tb::ui
