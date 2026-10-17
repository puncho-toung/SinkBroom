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

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>

#include "SidebarTestUtils.h"
#include "ui/CatchConfig.h"
#include "ui/SidebarManager.h"
#include "ui/SidebarPanel.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("SidebarPanel")
{
  SECTION("iconPath")
  {
    // the default, not the stub's override
    const auto panel = StubPanel{"Stub", "NoTool.svg"};
    CHECK(panel.SidebarPanel::iconPath().empty());
  }

  SECTION("shownByDefault")
  {
    // the default, not the stub's override
    const auto panel = StubPanel{"Stub", {}, true};
    CHECK(!panel.SidebarPanel::shownByDefault());
  }
}

TEST_CASE("SidebarDock")
{
  SECTION("closeEvent")
  {
    // counts the first dock's visibilityChanged(false); outlives the window
    auto firstDockHidden = 0;

    auto window = QMainWindow{};
    window.setCentralWidget(new QWidget{});

    auto* manager = new SidebarManager{window};
    const auto first = registerStubPanel(*manager, "First");
    const auto second = registerStubPanel(*manager, "Second");

    QObject::connect(
      first.dock, &QDockWidget::visibilityChanged, [&](const bool visible) {
        if (!visible)
        {
          ++firstDockHidden;
        }
      });

    window.show();
    manager->showPanel("First");
    manager->showPanel("Second");
    QApplication::processEvents();
    REQUIRE(first.dock->isVisible());
    REQUIRE(second.dock->isVisible());

    SECTION("not when another dock is tabbed over it")
    {
      window.tabifyDockWidget(first.dock, second.dock);
      second.dock->raise();
      QApplication::processEvents();

      CHECK(firstDockHidden > 0);
      CHECK(first.panel->hiddenCount == 0);
      CHECK(second.panel->hiddenCount == 0);

      const auto shownCount = first.panel->shownCount;
      first.dock->raise();
      QApplication::processEvents();

      CHECK(first.panel->shownCount > shownCount);
      CHECK(first.panel->hiddenCount == 0);
      CHECK(second.panel->hiddenCount == 0);
    }

    SECTION("not when it is floated or docked again")
    {
      first.dock->setFloating(true);
      QApplication::processEvents();
      REQUIRE(first.dock->isFloating());

      first.dock->setFloating(false);
      QApplication::processEvents();
      REQUIRE(!first.dock->isFloating());

      CHECK(first.dock->isVisible());
      CHECK(first.panel->hiddenCount == 0);
    }

    SECTION("when it is closed")
    {
      first.dock->close();
      CHECK(first.dock->isHidden());
      CHECK(first.panel->hiddenCount == 1);
      CHECK(second.panel->hiddenCount == 0);
    }

    SECTION("when it is closed from its toggle action")
    {
      second.dock->toggleViewAction()->trigger();
      CHECK(second.dock->isHidden());
      CHECK(second.panel->hiddenCount == 1);
      CHECK(first.panel->hiddenCount == 0);
    }
  }
}

} // namespace tb::ui
