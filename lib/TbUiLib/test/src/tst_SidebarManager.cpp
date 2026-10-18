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
#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>
#include <QTabBar>
#include <QTest>

#include "SidebarTestUtils.h"
#include "ui/CatchConfig.h"
#include "ui/ImageUtils.h"
#include "ui/SidebarManager.h"
#include "ui/SidebarPanel.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/**
 * Clicks the tab of the given dock in the tab bar of its tab group.
 */
void clickTab(const QMainWindow& window, const QDockWidget& dock)
{
  QTabBar* tabBar = nullptr;
  auto index = -1;
  const auto findTab = [&]() {
    for (auto* candidate : window.findChildren<QTabBar*>())
    {
      for (int i = 0; candidate->isVisible() && i < candidate->count(); ++i)
      {
        if (candidate->tabText(i) == dock.windowTitle())
        {
          tabBar = candidate;
          index = i;
          return true;
        }
      }
    }
    return false;
  };

  // the window shows the tab bar of a new tab group once the geometry animations of its
  // layout have gone, which takes deferred deletes
  REQUIRE(QTest::qWaitFor(findTab));
  QTest::mouseClick(tabBar, Qt::LeftButton, {}, tabBar->tabRect(index).center());
  QApplication::processEvents();
}

} // namespace

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
    // every edge, as grouped dragging needs
    CHECK(dock->allowedAreas() == Qt::AllDockWidgetAreas);
    CHECK(!dock->testAttribute(Qt::WA_DeleteOnClose));
    CHECK(dock->isHidden());

    CHECK(panel->shownCount == 0);
    CHECK(panel->hiddenCount == 0);

    // a panel without an icon leaves its toggle action without one
    CHECK(dock->toggleViewAction()->icon().isNull());

    // a panel's icon goes to its toggle action, for the tool sidebar's button
    const auto withIcon = registerStubPanel(*manager, "WithIcon", "NoTool.svg");
    CHECK(
      withIcon.dock->toggleViewAction()->icon().cacheKey()
      == loadSVGIcon("NoTool.svg").cacheKey());
  }

  SECTION("registerPanel hides the dock of a panel that is not shown by default")
  {
    const auto shown = registerStubPanel(*manager, "Shown", {}, true);
    const auto hidden = registerStubPanel(*manager, "Hidden", {}, false);

    CHECK(!shown.dock->isHidden());
    CHECK(window.dockWidgetArea(shown.dock) == Qt::RightDockWidgetArea);
    CHECK(hidden.dock->isHidden());

    // registering reports no panel as shown; the window shows the dock later
    CHECK(shown.panel->shownCount == 0);
    window.show();
    QApplication::processEvents();
    CHECK(shown.dock->isVisible());
    CHECK(shown.panel->shownCount == 1);
    CHECK(hidden.dock->isHidden());
    CHECK(hidden.panel->shownCount == 0);
  }

  SECTION("registerPanel adds the dock at the panel's default area")
  {
    const auto bottom =
      registerStubPanel(*manager, "Bottom", {}, false, Qt::BottomDockWidgetArea);
    CHECK(window.dockWidgetArea(bottom.dock) == Qt::BottomDockWidgetArea);
    CHECK(bottom.dock->allowedAreas() == Qt::AllDockWidgetAreas);
    CHECK(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
  }

  SECTION("registerPanel hides a panel while its dock is tabbed behind another")
  {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(800, 600);
    const auto second = registerStubPanel(*manager, "Second");
    panel->setMinimumWidth(300);

    window.show();
    manager->showPanel("Stub");
    manager->showPanel("Second");
    window.tabifyDockWidget(dock, second.dock);
    clickTab(window, *second.dock);

    // the dock behind the tab stays open, but its panel is hidden
    CHECK(!dock->isHidden());
    CHECK(panel->isHidden());
    CHECK(second.panel->isVisible());

    // the hidden panel keeps its place in the layout: the front dock fills the column,
    // and the panel behind still holds the column at its minimum width
    window.resizeDocks({second.dock}, {100}, Qt::Horizontal);
    QApplication::processEvents();
    CHECK(second.dock->height() > window.height() / 2);
    CHECK(second.dock->width() >= 300);

    // the other tab swaps them, and the panel that comes to the front is visible when it
    // is told
    const auto shownCount = panel->shownCount;
    panel->visibleWhenShown = false;
    clickTab(window, *dock);

    CHECK(panel->isVisible());
    CHECK(panel->shownCount == shownCount + 1);
    CHECK(panel->visibleWhenShown);
    CHECK(second.panel->isHidden());

    // neither was closed
    CHECK(panel->hiddenCount == 0);
    CHECK(second.panel->hiddenCount == 0);
  }

  SECTION("registerPanel hides a panel while its dock is closed")
  {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    manager->showPanel("Stub");
    QApplication::processEvents();
    REQUIRE(panel->isVisible());

    auto* action = dock->toggleViewAction();
    action->trigger();
    CHECK(dock->isHidden());
    CHECK(panel->isHidden());
    CHECK(panel->hiddenCount == 1);

    panel->visibleWhenShown = false;
    action->trigger();
    QApplication::processEvents();
    CHECK(dock->isVisible());
    CHECK(panel->isVisible());
    CHECK(panel->visibleWhenShown);
  }

  SECTION("registerPanel starts a panel hidden whose dock is behind a tab")
  {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    const auto second = registerStubPanel(*manager, "Second");

    // tabbed before the window is shown, with the second dock in front
    dock->show();
    second.dock->show();
    window.tabifyDockWidget(dock, second.dock);
    second.dock->raise();

    window.show();
    QApplication::processEvents();

    REQUIRE(!dock->toggleViewAction()->isChecked());
    REQUIRE(second.dock->toggleViewAction()->isChecked());
    CHECK(panel->isHidden());
    CHECK(panel->shownCount == 0);
    CHECK(second.panel->isVisible());
    CHECK(second.panel->shownCount == 1);
  }

  SECTION("panel")
  {
    const auto second = registerStubPanel(*manager, "Second");
    CHECK(manager->panel("Stub") == panel);
    CHECK(manager->panel("Second") == second.panel);
    CHECK(manager->panel("Unknown") == nullptr);

    // a panel that Qt destroys with its dock is forgotten
    delete second.dock;
    CHECK(manager->panel("Second") == nullptr);
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

  SECTION("toggleAction of a dock tabbed behind another")
  {
    const auto second = registerStubPanel(*manager, "Second");

    window.show();
    manager->showPanel("Stub");
    manager->showPanel("Second");
    window.tabifyDockWidget(dock, second.dock);
    second.dock->raise();
    QApplication::processEvents();

    // the dock behind the tab is open but out of sight, and its action says so
    auto* action = dock->toggleViewAction();
    REQUIRE(!dock->isHidden());
    CHECK(!action->isChecked());
    CHECK(second.dock->toggleViewAction()->isChecked());

    // one trigger brings it to the front instead of closing it
    const auto shownCount = panel->shownCount;
    action->trigger();
    QApplication::processEvents();

    CHECK(!dock->isHidden());
    CHECK(action->isChecked());
    CHECK(panel->shownCount > shownCount);
    CHECK(panel->hiddenCount == 0);
    CHECK(!second.dock->toggleViewAction()->isChecked());
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
