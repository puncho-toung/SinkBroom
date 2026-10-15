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

#include <QDockWidget>
#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>

#include "SidebarTestUtils.h"
#include "ui/CatchConfig.h"
#include "ui/SidebarManager.h"
#include "ui/SidebarPanel.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("SidebarManager")
{
  auto window = QMainWindow{};
  window.setCentralWidget(new QWidget{});

  auto* manager = new SidebarManager{window};
  const auto stub = registerStubPanel(*manager, "Stub");
  auto* dock = stub.dock;
  auto* panel = stub.panel;

  SECTION("registerPanel")
  {
    CHECK(manager->parent() == &window);

    REQUIRE(dock != nullptr);
    CHECK(dock->objectName() == "MapWindow_Sidebar_Stub");
    CHECK(dock->windowTitle() == "Stub title");
    CHECK(dock->parent() == &window);
    CHECK(dock->widget() == panel);
    CHECK(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
    const auto features = QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable
                          | QDockWidget::DockWidgetFloatable;
    CHECK(dock->features() == features);
    CHECK(dock->allowedAreas() == (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    CHECK(!dock->testAttribute(Qt::WA_DeleteOnClose));
    CHECK(dock->isHidden());

    CHECK(panel->shownCount == 0);
    CHECK(panel->hiddenCount == 0);
  }

  SECTION("toggleAction")
  {
    window.show();

    auto* action = manager->toggleAction("Stub");
    REQUIRE(action != nullptr);
    CHECK(action == dock->toggleViewAction());
    CHECK(manager->toggleAction("Unknown") == nullptr);

    action->trigger();
    CHECK(dock->isVisible());
    CHECK(action->isChecked());
    CHECK(panel->shownCount == 1);
    CHECK(panel->hiddenCount == 0);

    action->trigger();
    CHECK(dock->isHidden());
    CHECK(!action->isChecked());
    CHECK(panel->shownCount == 1);
    CHECK(panel->hiddenCount == 1);

    // closing keeps the dock and its panel
    CHECK(manager->docks() == std::vector<QDockWidget*>{dock});
    CHECK(manager->panels() == std::vector<SidebarPanel*>{panel});
  }

  SECTION("showPanel")
  {
    window.show();

    manager->showPanel("Stub");
    CHECK(dock->isVisible());
    CHECK(panel->shownCount == 1);
    CHECK(panel->hiddenCount == 0);

    // an unknown id is ignored
    manager->showPanel("Unknown");
    CHECK(dock->isVisible());
  }

  SECTION("panels and docks")
  {
    const auto second = registerStubPanel(*manager, "Second");
    CHECK(manager->panels() == std::vector<SidebarPanel*>{panel, second.panel});
    CHECK(manager->docks() == std::vector<QDockWidget*>{dock, second.dock});

    // a dock that Qt destroys takes its panel with it, and the manager forgets both
    delete second.dock;
    CHECK(manager->panels() == std::vector<SidebarPanel*>{panel});
    CHECK(manager->docks() == std::vector<QDockWidget*>{dock});
    CHECK(manager->toggleAction("Second") == nullptr);
  }

  SECTION("redockOffScreenDocks")
  {
    const auto* screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);

    const auto onScreen = registerStubPanel(*manager, "OnScreen");
    const auto docked = registerStubPanel(*manager, "Docked");

    const auto virtualGeometry = screen->virtualGeometry();
    dock->setFloating(true);
    dock->setGeometry(
      QRect{virtualGeometry.right() + 1000, virtualGeometry.bottom() + 1000, 200, 100});

    onScreen.dock->setFloating(true);
    onScreen.dock->setGeometry(
      QRect{screen->availableGeometry().center(), QSize{200, 100}});

    manager->redockOffScreenDocks();

    CHECK(!dock->isFloating());
    CHECK(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
    CHECK(onScreen.dock->isFloating());
    CHECK(!docked.dock->isFloating());
    CHECK(panel->hiddenCount == 0);
  }
}

} // namespace tb::ui
