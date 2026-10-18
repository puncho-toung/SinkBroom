/*
 Copyright (C) 2026 Kristian Duske

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
#include <QByteArray>
#include <QComboBox>
#include <QDataStream>
#include <QDockWidget>
#include <QIODevice>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTabWidget>
#include <QTest>
#include <QTextEdit>
#include <QToolBar>

#include "TestEnvironment.h"
#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "base/Result.h"
#include "gl/Camera.h"
#include "gl/GlManager.h"
#include "gl/Material.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/ActionContext.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/Console.h"
#include "ui/EntityInspector.h"
#include "ui/FaceInspector.h"
#include "ui/IssueBrowser.h"
#include "ui/LightEditorPanel.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapInspector.h"
#include "ui/MapView2D.h"
#include "ui/MapViewBase.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/RenderView.h"
#include "ui/SwitchableMapViewContainer.h"
#include "ui/ToolSidebar.h"

#include "kd/invoke.h"

#include "vm/ray.h"
#include "vm/vec.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

// Simple resource type for testing resource processing
struct TestResource
{
  void upload(gl::Gl&) const {}
  void drop(gl::Gl&) const {}
};

namespace
{

const auto ToolSidebarExpandedKey = QString{"Windows/MapWindow_ToolSidebar/Expanded"};

const auto DockLayoutVersionKey = QString{"Windows/MapWindow/DockLayoutVersion"};
const auto OldInspectorKey = QString{"Windows/Inspector/State"};
const auto OldInspectorPageKey = QString{"Windows/Inspector_TabBook/State"};
const auto OldInfoPanelKey = QString{"Windows/MapWindow_InfoPanel/State"};
const auto OldInfoPanelPageKey = QString{"Windows/InfoPanel_TabBook/State"};

/**
 * The keys that the window restores when it is built and that saveWindowState writes,
 * the Face inspector's material tab, which the panel restores when it is built and saves
 * when it goes, and the keys of the inspector and of the info panel from before their
 * pages were docks, which the window reads once and which the test settings may hold
 * from a build of that time.
 */
const auto WindowStateKeys = std::vector<QString>{
  "Windows/MapWindow/Geometry",
  "Windows/MapWindow/State",
  DockLayoutVersionKey,
  "Windows/MapWindow_VerticalSplitterSplitter/State",
  "Windows/FaceInspector_TabBook/State",
  OldInspectorKey,
  OldInspectorPageKey,
  OldInfoPanelKey,
  OldInfoPanelPageKey,
};

void removeWindowStateKeys()
{
  auto settings = QSettings{};
  for (const auto& key : WindowStateKeys)
  {
    settings.remove(key);
  }
}

/**
 * Writes the state of the inspector or of the info panel from before their pages were
 * docks, as Inspector::saveState, InfoPanel::saveState and TabBook::saveState wrote it.
 */
template <typename T>
void writeOldState(const QString& key, const T& value)
{
  auto state = QByteArray{};
  {
    auto stream = QDataStream{&state, QIODevice::WriteOnly};
    stream << value;
  }
  QSettings{}.setValue(key, state);
}

QMenu* findViewMenu(const MapWindow& window)
{
  for (auto* action : window.menuBar()->actions())
  {
    if (action->text() == "View")
    {
      return QMenu::menuInAction(action);
    }
  }
  return nullptr;
}

QAction* findViewMenuAction(const MapWindow& window, const QString& text)
{
  auto* viewMenu = findViewMenu(window);
  REQUIRE(viewMenu != nullptr);
  const auto actions = viewMenu->actions();
  const auto iAction = std::ranges::find_if(
    actions, [&](const auto* action) { return action->text() == text; });
  REQUIRE(iAction != actions.end());
  return *iAction;
}

QMenu* findSidebarMenu(const MapWindow& window)
{
  auto* sidebarMenu = QMenu::menuInAction(findViewMenuAction(window, "Sidebar"));
  REQUIRE(sidebarMenu != nullptr);
  return sidebarMenu;
}

struct SidebarDocks
{
  QDockWidget* map;
  QDockWidget* entity;
  QDockWidget* face;
  QDockWidget* console;
  QDockWidget* issues;
  QDockWidget* lightEditor;

  std::vector<QDockWidget*> inspectors() const { return {map, entity, face}; }
  std::vector<QDockWidget*> infoPanels() const { return {console, issues}; }

  /**
   * The toggle actions in the order of the tool sidebar's buttons: sorted by title.
   */
  std::vector<QAction*> sortedToggleActions() const
  {
    return {
      console->toggleViewAction(),
      entity->toggleViewAction(),
      face->toggleViewAction(),
      issues->toggleViewAction(),
      lightEditor->toggleViewAction(),
      map->toggleViewAction(),
    };
  }
};

SidebarDocks findSidebarDocks(const MapWindow& window)
{
  const auto docks = SidebarDocks{
    window.findChild<QDockWidget*>("MapWindow_Sidebar_MapInspector"),
    window.findChild<QDockWidget*>("MapWindow_Sidebar_EntityInspector"),
    window.findChild<QDockWidget*>("MapWindow_Sidebar_FaceInspector"),
    window.findChild<QDockWidget*>("MapWindow_Sidebar_Console"),
    window.findChild<QDockWidget*>("MapWindow_Sidebar_Issues"),
    window.findChild<QDockWidget*>("MapWindow_Sidebar_LightEditor"),
  };
  REQUIRE(docks.map != nullptr);
  REQUIRE(docks.entity != nullptr);
  REQUIRE(docks.face != nullptr);
  REQUIRE(docks.console != nullptr);
  REQUIRE(docks.issues != nullptr);
  REQUIRE(docks.lightEditor != nullptr);
  return docks;
}

/**
 * Builds a second window on a new document, as the app does for the next map: it
 * restores the layout that the settings hold.
 */
std::unique_ptr<MapWindow> createNextWindow(AppController& appController)
{
  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    mdl::QuakeGameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();
  return std::make_unique<MapWindow>(appController, std::move(document));
}

/**
 * Shows the window with its GL views hidden: they would create GL contexts and load the
 * renderer's resources, which the tests do not provide, and the docks do not need them to
 * be seen.
 */
void showWithoutGlViews(MapWindow& window)
{
  for (auto* view : window.findChildren<RenderView*>())
  {
    view->hide();
  }
  window.show();
  QApplication::processEvents();
}

} // namespace

TEST_CASE("MapWindow")
{
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  // the window's tool sidebar reads its Expanded key: start without it, and leave none
  // behind for the other cases and the next run
  QSettings{}.remove(ToolSidebarExpandedKey);
  const auto removeToolSidebarExpandedKey =
    kdl::invoke_later{[] { QSettings{}.remove(ToolSidebarExpandedKey); }};

  // the window restores its layout when it is built, and saveWindowState writes it: start
  // without a saved layout, and leave none behind; tests never close the window, which
  // would also write it, and call saveWindowState instead
  removeWindowStateKeys();
  const auto removeWindowStateKeysLater =
    kdl::invoke_later{[] { removeWindowStateKeys(); }};

  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    mdl::QuakeGameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();

  auto window = MapWindow{appController, std::move(document)};

  SECTION("load resets grid size dropdown")
  {
    auto* gridChoice = window.findChild<QComboBox*>("MapWindow_GridChoice");
    REQUIRE(gridChoice != nullptr);

    const auto changedGridSize = 5;
    const auto changedGridIndex = changedGridSize - mdl::Grid::MinSize;
    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    window.setGridSize(changedGridSize);
    QApplication::processEvents();

    REQUIRE(window.document().map().grid().size() == changedGridSize);
    REQUIRE(gridChoice->currentIndex() == changedGridIndex);

    REQUIRE(window.document().load(
      appController.environmentConfig(),
      mdl::QuakeGameInfo,
      mdl::MapFormat::Unknown,
      vm::bbox3d{8192.0},
      path));

    QApplication::processEvents();

    const auto loadedGridSize = window.document().map().grid().size();
    const auto loadedGridIndex = loadedGridSize - mdl::Grid::MinSize;

    CHECK(loadedGridSize != changedGridSize);
    CHECK(gridChoice->currentIndex() == loadedGridIndex);
    CHECK(gridChoice->currentData().toInt() == loadedGridSize);
  }

  SECTION("load moves the 2D cameras to the new world bounds")
  {
    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    const auto views = window.findChildren<MapView2D*>();
    REQUIRE(!views.empty());

    const auto loadMap = [&](const vm::bbox3d& worldBounds) {
      REQUIRE(window.document().load(
        appController.environmentConfig(),
        mdl::QuakeGameInfo,
        mdl::MapFormat::Unknown,
        worldBounds,
        path));

      QApplication::processEvents();
    };

    // a 2D camera sits at the near side of the world bounds, and its far plane reaches
    // the far side of them
    const auto checkCameras = [&](const vm::bbox3d& worldBounds) {
      for (auto* view : views)
      {
        const auto& camera = view->camera();
        const auto axis = vm::find_abs_max_component(camera.direction());
        const auto& nearSide =
          camera.direction()[axis] < 0.0f ? worldBounds.max : worldBounds.min;
        CHECK(camera.position()[axis] == float(nearSide[axis]));
        CHECK(camera.farPlane() == float(worldBounds.size()[axis]) + 16.0f);
      }
    };

    checkCameras(vm::bbox3d{8192.0});

    SECTION("larger bounds")
    {
      loadMap(vm::bbox3d{131072.0});
      checkCameras(vm::bbox3d{131072.0});
    }

    SECTION("smaller bounds")
    {
      loadMap(vm::bbox3d{4096.0});
      checkCameras(vm::bbox3d{4096.0});
    }

    SECTION("bounds that are not a cube")
    {
      const auto worldBounds =
        vm::bbox3d{{-8192.0, -4096.0, -2048.0}, {8192.0, 4096.0, 2048.0}};

      loadMap(worldBounds);
      checkCameras(worldBounds);

      // each camera sits on the near side of the axis it looks along, and its far plane
      // is the extent of the bounds along that axis, not along x
      auto checkedViews = size_t{0};
      for (auto* view : views)
      {
        const auto& camera = view->camera();
        if (view->objectName() == "XY View")
        {
          CHECK(camera.position().z() == 2048.0f);
          CHECK(camera.farPlane() == 4096.0f + 16.0f);
          ++checkedViews;
        }
        else if (view->objectName() == "XZ View")
        {
          CHECK(camera.position().y() == -4096.0f);
          CHECK(camera.farPlane() == 8192.0f + 16.0f);
          ++checkedViews;
        }
        else if (view->objectName() == "YZ View")
        {
          CHECK(camera.position().x() == 8192.0f);
          CHECK(camera.farPlane() == 16384.0f + 16.0f);
          ++checkedViews;
        }
      }
      CHECK(checkedViews == size_t(views.size()));
    }

    SECTION("unchanged bounds keep the cameras where they are")
    {
      auto& camera = views.front()->camera();
      camera.moveBy(camera.right() * 100.0f);
      const auto position = camera.position();

      loadMap(vm::bbox3d{8192.0});

      CHECK(camera.position() == position);
      checkCameras(vm::bbox3d{8192.0});
    }
  }

  SECTION("sidebar docks are listed in View > Sidebar")
  {
    const auto docks = findSidebarDocks(window);
    CHECK(docks.map->windowTitle() == "Map Inspector");
    CHECK(qobject_cast<MapInspector*>(docks.map->widget()) != nullptr);
    CHECK(docks.entity->windowTitle() == "Entity Inspector");
    CHECK(qobject_cast<EntityInspector*>(docks.entity->widget()) != nullptr);
    CHECK(docks.face->windowTitle() == "Face Inspector");
    CHECK(qobject_cast<FaceInspector*>(docks.face->widget()) != nullptr);
    CHECK(docks.console->windowTitle() == "Console");
    CHECK(qobject_cast<Console*>(docks.console->widget()) != nullptr);
    CHECK(docks.issues->windowTitle() == "Issues");
    CHECK(qobject_cast<IssueBrowser*>(docks.issues->widget()) != nullptr);
    CHECK(docks.lightEditor->windowTitle() == "Light Editor");
    CHECK(qobject_cast<LightEditorPanel*>(docks.lightEditor->widget()) != nullptr);

    auto* viewMenu = findViewMenu(window);
    REQUIRE(viewMenu != nullptr);

    // the submenu follows Maximize Current View
    const auto viewActions = viewMenu->actions();
    const auto iMaximize = std::ranges::find_if(viewActions, [](const auto* action) {
      return action->text() == "Maximize Current View";
    });
    REQUIRE(iMaximize != viewActions.end());
    REQUIRE(std::next(iMaximize) != viewActions.end());

    auto* sidebarMenu = QMenu::menuInAction(*std::next(iMaximize));
    REQUIRE(sidebarMenu != nullptr);
    CHECK(sidebarMenu->title() == "Sidebar");

    // sorted by title, like the tool sidebar's buttons
    const auto toggleActions = docks.sortedToggleActions();
    CHECK(
      sidebarMenu->actions()
      == QList<QAction*>{toggleActions.begin(), toggleActions.end()});
  }

  SECTION("the toolbar context menu lists the sidebar docks sorted by title")
  {
    const auto docks = findSidebarDocks(window);
    auto* toolBar = window.findChild<QToolBar*>("MapWindowToolBar");
    auto* toolSidebar = window.findChild<ToolSidebar*>("MapWindow_ToolSidebar");
    REQUIRE(toolBar != nullptr);
    REQUIRE(toolSidebar != nullptr);

    auto* menu = window.createPopupMenu();
    REQUIRE(menu != nullptr);
    const auto deleteMenu = kdl::invoke_later{[&] { delete menu; }};

    // the docks as the tool sidebar and View > Sidebar list them, then the toolbars
    const auto toggleActions = docks.sortedToggleActions();
    const auto actions = menu->actions();
    REQUIRE(actions.size() == 9);
    CHECK(
      actions.mid(0, 6) == QList<QAction*>{toggleActions.begin(), toggleActions.end()});
    CHECK(actions[6]->isSeparator());
    CHECK(actions.mid(7).contains(toolBar->toggleViewAction()));
    CHECK(actions.mid(7).contains(toolSidebar->toggleViewAction()));
  }

  SECTION("the tool sidebar has a button for each sidebar dock")
  {
    auto* toolSidebar = window.findChild<ToolSidebar*>("MapWindow_ToolSidebar");
    REQUIRE(toolSidebar != nullptr);
    CHECK(window.toolBarArea(toolSidebar) == Qt::LeftToolBarArea);
    CHECK(!toolSidebar->isMovable());
    CHECK(toolSidebar->expanded());

    // sorted by title; the Quake game has no light editor, and its button is there all
    // the same
    const auto docks = findSidebarDocks(window);
    const auto toggleActions = docks.sortedToggleActions();
    CHECK(toolSidebar->entries() == toggleActions);
    for (const auto* action : toggleActions)
    {
      CHECK(!action->icon().isNull());
    }

    const auto actions = toolSidebar->actions();
    REQUIRE(actions.size() == 8);
    CHECK(actions.front() == toggleActions.front());
    CHECK(actions.back() == toolSidebar->expandAction());

    auto* dock = docks.lightEditor;
    auto* toggleAction = dock->toggleViewAction();
    CHECK(toggleAction->text() == "Light Editor");

    toggleAction->trigger();
    CHECK(!dock->isHidden());
    CHECK(toggleAction->isChecked());

    dock->close();
    CHECK(dock->isHidden());
    CHECK(!toggleAction->isChecked());

    // collapsing takes the buttons out of the strip, not out of View > Sidebar
    toolSidebar->setExpanded(false);
    CHECK(toolSidebar->actions() == QList<QAction*>{actions[6], actions[7]});
    CHECK(toggleAction->isVisible());

    CHECK(
      findSidebarMenu(window)->actions()
      == QList<QAction*>{toggleActions.begin(), toggleActions.end()});
  }

  SECTION("the inspector docks start tabbed on the right, the light editor hidden")
  {
    const auto docks = findSidebarDocks(window);
    for (auto* dock : docks.inspectors())
    {
      CAPTURE(dock->objectName());
      CHECK(!dock->isHidden());
      CHECK(!dock->isFloating());
      CHECK(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
    }
    CHECK(docks.lightEditor->isHidden());

    // in the order of the old inspector's pages, with the tab bar at the top
    CHECK(
      window.tabifiedDockWidgets(docks.map)
      == QList<QDockWidget*>{docks.entity, docks.face});
    CHECK(window.tabPosition(Qt::RightDockWidgetArea) == QTabWidget::North);
  }

  SECTION("the console and issues docks start tabbed at the bottom")
  {
    const auto docks = findSidebarDocks(window);
    for (auto* dock : docks.infoPanels())
    {
      CAPTURE(dock->objectName());
      CHECK(!dock->isHidden());
      CHECK(!dock->isFloating());
      CHECK(window.dockWidgetArea(dock) == Qt::BottomDockWidgetArea);
    }

    // in the order of the old info panel's pages, under the map views, which fill the
    // rest of the window
    CHECK(window.tabifiedDockWidgets(docks.console) == QList<QDockWidget*>{docks.issues});
    CHECK(window.centralWidget() == window.findChild<SwitchableMapViewContainer*>());
  }

  SECTION("the console panel is the window's logger")
  {
    const auto docks = findSidebarDocks(window);
    auto* console = qobject_cast<Console*>(docks.console->widget());
    REQUIRE(console != nullptr);
    CHECK(&window.logger() == static_cast<Logger*>(console));

    // the document logs to it
    auto* textView = console->findChild<QTextEdit*>();
    REQUIRE(textView != nullptr);
    window.document().logger().info() << "a line from the document";
    CHECK(QTest::qWaitFor(
      [&] { return textView->toPlainText().contains("a line from the document"); }));
  }

  SECTION("every sidebar dock can be tabbed with any other at every edge")
  {
    // grouped dragging moves a whole tab group by its title bar and a single dock by its
    // tab; it wants every dock allowed in the same areas
    CHECK(window.dockOptions().testFlag(QMainWindow::AnimatedDocks));
    CHECK(window.dockOptions().testFlag(QMainWindow::AllowTabbedDocks));
    CHECK(window.dockOptions().testFlag(QMainWindow::GroupedDragging));
    CHECK(!window.dockOptions().testFlag(QMainWindow::AllowNestedDocks));
    CHECK(!window.dockOptions().testFlag(QMainWindow::VerticalTabs));

    const auto docks = findSidebarDocks(window);
    for (auto* dock :
         {docks.map,
          docks.entity,
          docks.face,
          docks.console,
          docks.issues,
          docks.lightEditor})
    {
      CAPTURE(dock->objectName());
      CHECK(dock->allowedAreas() == Qt::AllDockWidgetAreas);
    }

    // the tab bar of every group at its top
    for (const auto area :
         {Qt::LeftDockWidgetArea,
          Qt::RightDockWidgetArea,
          Qt::TopDockWidgetArea,
          Qt::BottomDockWidgetArea})
    {
      CAPTURE(area);
      CHECK(window.tabPosition(area) == QTabWidget::North);
    }

    // the side edges own the corners: a group at the bottom sits under the map views
    CHECK(window.corner(Qt::TopLeftCorner) == Qt::LeftDockWidgetArea);
    CHECK(window.corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea);
    CHECK(window.corner(Qt::TopRightCorner) == Qt::RightDockWidgetArea);
    CHECK(window.corner(Qt::BottomRightCorner) == Qt::RightDockWidgetArea);
  }

  SECTION("Show ... Inspector shows a hidden inspector dock")
  {
    const auto docks = findSidebarDocks(window);
    const auto showActions = std::vector<std::pair<QString, QDockWidget*>>{
      {"Show Map Inspector", docks.map},
      {"Show Entity Inspector", docks.entity},
      {"Show Face Inspector", docks.face},
    };

    for (const auto& [text, dock] : showActions)
    {
      CAPTURE(text);
      auto* action = findViewMenuAction(window, text);
      CHECK(!action->isCheckable());

      dock->hide();
      action->trigger();
      CHECK(!dock->isHidden());
    }
  }

  SECTION("Toggle Inspector hides the inspector docks and shows them again")
  {
    const auto docks = findSidebarDocks(window);
    auto* action = findViewMenuAction(window, "Toggle Inspector");
    REQUIRE(action->isCheckable());

    // the light editor is not one of them
    docks.lightEditor->show();

    QApplication::processEvents();
    REQUIRE(action->isChecked());

    action->trigger();
    QApplication::processEvents();
    for (auto* dock : docks.inspectors())
    {
      CHECK(dock->isHidden());
    }
    CHECK(!action->isChecked());
    CHECK(!docks.lightEditor->isHidden());

    action->trigger();
    QApplication::processEvents();
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
    CHECK(action->isChecked());
    CHECK(!docks.lightEditor->isHidden());
  }

  SECTION("Toggle Inspector brings back only the inspector docks that were open")
  {
    const auto docks = findSidebarDocks(window);
    auto* action = findViewMenuAction(window, "Toggle Inspector");

    docks.face->hide();

    action->trigger();
    for (auto* dock : docks.inspectors())
    {
      CHECK(dock->isHidden());
    }

    action->trigger();
    CHECK(!docks.map->isHidden());
    CHECK(!docks.entity->isHidden());
    CHECK(docks.face->isHidden());
  }

  SECTION("Toggle Inspector shows all inspector docks when none was open")
  {
    const auto docks = findSidebarDocks(window);
    auto* action = findViewMenuAction(window, "Toggle Inspector");

    for (auto* dock : docks.inspectors())
    {
      dock->close();
    }
    REQUIRE(!window.inspectorPanelsVisible());

    action->trigger();
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
  }

  SECTION("Toggle Info Panel hides the console and issues docks and shows them again")
  {
    const auto docks = findSidebarDocks(window);
    auto* action = findViewMenuAction(window, "Toggle Info Panel");
    REQUIRE(action->isCheckable());

    QApplication::processEvents();
    REQUIRE(action->isChecked());

    action->trigger();
    QApplication::processEvents();
    for (auto* dock : docks.infoPanels())
    {
      CHECK(dock->isHidden());
    }
    CHECK(!action->isChecked());

    // the inspectors are not part of it
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }

    action->trigger();
    QApplication::processEvents();
    for (auto* dock : docks.infoPanels())
    {
      CHECK(!dock->isHidden());
    }
    CHECK(action->isChecked());
  }

  SECTION("Toggle Info Panel brings back only the docks that were open")
  {
    const auto docks = findSidebarDocks(window);
    auto* action = findViewMenuAction(window, "Toggle Info Panel");

    docks.issues->hide();

    action->trigger();
    CHECK(docks.console->isHidden());
    CHECK(docks.issues->isHidden());

    action->trigger();
    CHECK(!docks.console->isHidden());
    CHECK(docks.issues->isHidden());
  }

  SECTION("revealMaterial shows the face inspector")
  {
    const auto docks = findSidebarDocks(window);
    docks.face->hide();

    // the browser does not hold this material, which it tolerates
    const auto material =
      gl::Material{"base/plain", gl::createTextureResource(gl::Texture{4, 4})};
    window.revealMaterial(&material);
    CHECK(!docks.face->isHidden());
  }

  SECTION("positionOnScreen arranges the inspector docks again")
  {
    // the app restores the window state a second time when it places the window; the
    // layout was not saved with the dock layout version, so the docks take the place of
    // the old inspector again, wherever they are
    const auto docks = findSidebarDocks(window);
    for (auto* dock : {docks.entity, docks.face})
    {
      window.removeDockWidget(dock);
      window.addDockWidget(Qt::RightDockWidgetArea, dock);
      dock->show();
    }
    REQUIRE(window.tabifiedDockWidgets(docks.map).isEmpty());

    window.positionOnScreen(nullptr);
    CHECK(
      window.tabifiedDockWidgets(docks.map)
      == QList<QDockWidget*>{docks.entity, docks.face});
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
  }

  SECTION("positionOnScreen arranges the console and issues docks again")
  {
    // like the inspector docks, in the old info panel's place
    const auto docks = findSidebarDocks(window);
    window.removeDockWidget(docks.issues);
    window.addDockWidget(Qt::BottomDockWidgetArea, docks.issues);
    docks.issues->show();
    REQUIRE(window.tabifiedDockWidgets(docks.console).isEmpty());

    window.positionOnScreen(nullptr);
    CHECK(window.tabifiedDockWidgets(docks.console) == QList<QDockWidget*>{docks.issues});
    for (auto* dock : docks.infoPanels())
    {
      CHECK(!dock->isHidden());
      CHECK(window.dockWidgetArea(dock) == Qt::BottomDockWidgetArea);
    }
  }

  SECTION("a hidden old inspector hides the inspector docks")
  {
    writeOldState(OldInspectorKey, false);

    const auto docks = findSidebarDocks(window);
    window.positionOnScreen(nullptr);
    for (auto* dock : docks.inspectors())
    {
      CHECK(dock->isHidden());
    }
    CHECK(docks.lightEditor->isHidden());
    for (auto* dock : docks.infoPanels())
    {
      CHECK(!dock->isHidden());
    }
  }

  SECTION("a hidden old info panel hides the console and issues docks")
  {
    writeOldState(OldInfoPanelKey, false);

    const auto docks = findSidebarDocks(window);
    window.positionOnScreen(nullptr);
    for (auto* dock : docks.infoPanels())
    {
      CHECK(dock->isHidden());
    }
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
  }

  SECTION("saveWindowState writes the dock layout version")
  {
    REQUIRE(!QSettings{}.contains(DockLayoutVersionKey));

    window.saveWindowState();
    CHECK(QSettings{}.value(DockLayoutVersionKey).toInt() == 2);
    CHECK(QSettings{}.contains("Windows/MapWindow/State"));

    // a layout saved with the version knows the docks: the states of the old inspector
    // and of the old info panel are not read any more
    writeOldState(OldInspectorKey, false);
    writeOldState(OldInfoPanelKey, false);
    const auto docks = findSidebarDocks(window);
    window.positionOnScreen(nullptr);
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
    for (auto* dock : docks.infoPanels())
    {
      CHECK(!dock->isHidden());
    }
  }

  SECTION("a layout saved when only the inspectors were docks gets the info docks once")
  {
    // version 1: the inspectors are known, so the face inspector stays on its own at the
    // left edge; the console and issues docks are not, so Qt leaves them side by side,
    // and they take the place of the old info panel, hidden on its Issues page
    const auto docks = findSidebarDocks(window);
    window.removeDockWidget(docks.face);
    window.addDockWidget(Qt::LeftDockWidgetArea, docks.face);
    docks.face->show();
    window.removeDockWidget(docks.issues);
    window.addDockWidget(Qt::BottomDockWidgetArea, docks.issues);
    docks.issues->show();
    REQUIRE(window.tabifiedDockWidgets(docks.console).isEmpty());

    QSettings{}.setValue(DockLayoutVersionKey, 1);
    writeOldState(OldInspectorKey, false);
    writeOldState(OldInfoPanelKey, false);
    writeOldState(OldInfoPanelPageKey, 1);

    window.positionOnScreen(nullptr);
    for (auto* dock : docks.infoPanels())
    {
      CHECK(dock->isHidden());
      CHECK(window.dockWidgetArea(dock) == Qt::BottomDockWidgetArea);
    }

    // the inspectors keep their layout
    CHECK(window.dockWidgetArea(docks.face) == Qt::LeftDockWidgetArea);
    CHECK(window.tabifiedDockWidgets(docks.map) == QList<QDockWidget*>{docks.entity});
    for (auto* dock : docks.inspectors())
    {
      CHECK(!dock->isHidden());
    }

    // Toggle Info Panel brings both back, tabbed, with the old page in front; a tab bar
    // lists only docks that are not hidden, so this is seen in a shown window
    showWithoutGlViews(window);
    findViewMenuAction(window, "Toggle Info Panel")->trigger();
    QApplication::processEvents();
    CHECK(window.tabifiedDockWidgets(docks.console) == QList<QDockWidget*>{docks.issues});
    CHECK(docks.issues->toggleViewAction()->isChecked());
    CHECK(!docks.console->toggleViewAction()->isChecked());

    // saved with the current version, the layout is not arranged again
    window.saveWindowState();
    CHECK(QSettings{}.value(DockLayoutVersionKey).toInt() == 2);
  }

  SECTION("a saved layout comes back in the next window")
  {
    // the face inspector is moved to the left edge and the issue browser above the map
    // views, and the layout is saved
    const auto docks = findSidebarDocks(window);
    window.addDockWidget(Qt::LeftDockWidgetArea, docks.face);
    REQUIRE(window.dockWidgetArea(docks.face) == Qt::LeftDockWidgetArea);
    window.addDockWidget(Qt::TopDockWidgetArea, docks.issues);
    REQUIRE(window.dockWidgetArea(docks.issues) == Qt::TopDockWidgetArea);
    window.saveWindowState();

    auto nextWindow = createNextWindow(appController);
    nextWindow->positionOnScreen(nullptr);

    // the layout was saved with the dock layout version, so it is not arranged again
    const auto nextDocks = findSidebarDocks(*nextWindow);
    CHECK(nextWindow->dockWidgetArea(nextDocks.face) == Qt::LeftDockWidgetArea);
    CHECK(nextWindow->dockWidgetArea(nextDocks.map) == Qt::RightDockWidgetArea);
    CHECK(nextWindow->dockWidgetArea(nextDocks.entity) == Qt::RightDockWidgetArea);
    CHECK(nextWindow->dockWidgetArea(nextDocks.issues) == Qt::TopDockWidgetArea);
    CHECK(nextWindow->dockWidgetArea(nextDocks.console) == Qt::BottomDockWidgetArea);

    // shown, saved and hidden as the app does when it closes a window; a restored tab
    // group must not break the window's teardown
    showWithoutGlViews(*nextWindow);
    CHECK(
      nextWindow->tabifiedDockWidgets(nextDocks.map)
      == QList<QDockWidget*>{nextDocks.entity});
    nextWindow->saveWindowState();
    nextWindow->hide();
    QApplication::processEvents();
  }

  SECTION("Toggle Inspector shows the hidden old inspector's page in front")
  {
    // a layout saved before the inspectors were docks, with the inspector hidden on its
    // Face page; the next window restores it before it is shown, as the app does
    writeOldState(OldInspectorKey, false);
    writeOldState(OldInspectorPageKey, 2);

    auto nextWindow = createNextWindow(appController);
    nextWindow->positionOnScreen(nullptr);

    const auto nextDocks = findSidebarDocks(*nextWindow);
    for (auto* dock : nextDocks.inspectors())
    {
      REQUIRE(dock->isHidden());
    }

    showWithoutGlViews(*nextWindow);
    findViewMenuAction(*nextWindow, "Toggle Inspector")->trigger();
    QApplication::processEvents();

    for (auto* dock : nextDocks.inspectors())
    {
      CHECK(!dock->isHidden());
    }
    CHECK(nextDocks.face->toggleViewAction()->isChecked());
    CHECK(!nextDocks.map->toggleViewAction()->isChecked());
    CHECK(!nextDocks.entity->toggleViewAction()->isChecked());
  }

  SECTION("Toggle Info Panel shows the hidden old info panel's page in front")
  {
    // a layout saved before the console and the issue browser were docks, with the info
    // panel hidden on its Issues page
    writeOldState(OldInfoPanelKey, false);
    writeOldState(OldInfoPanelPageKey, 1);

    auto nextWindow = createNextWindow(appController);
    nextWindow->positionOnScreen(nullptr);

    const auto nextDocks = findSidebarDocks(*nextWindow);
    for (auto* dock : nextDocks.infoPanels())
    {
      REQUIRE(dock->isHidden());
    }

    showWithoutGlViews(*nextWindow);
    findViewMenuAction(*nextWindow, "Toggle Info Panel")->trigger();
    QApplication::processEvents();

    for (auto* dock : nextDocks.infoPanels())
    {
      CHECK(!dock->isHidden());
    }
    CHECK(nextDocks.issues->toggleViewAction()->isChecked());
    CHECK(!nextDocks.console->toggleViewAction()->isChecked());
  }

  SECTION("in a shown window")
  {
    const auto docks = findSidebarDocks(window);
    auto* toggleInspector = findViewMenuAction(window, "Toggle Inspector");
    showWithoutGlViews(window);

    // a toggle action is checked while its dock can be seen
    REQUIRE(docks.map->toggleViewAction()->isChecked());
    REQUIRE(!docks.entity->toggleViewAction()->isChecked());
    REQUIRE(!docks.face->toggleViewAction()->isChecked());
    REQUIRE(docks.console->toggleViewAction()->isChecked());
    REQUIRE(!docks.issues->toggleViewAction()->isChecked());

    SECTION("Toggle Info Panel brings the dock that was in front back in front")
    {
      auto* toggleInfoPanel = findViewMenuAction(window, "Toggle Info Panel");
      docks.issues->raise();
      QApplication::processEvents();
      REQUIRE(docks.issues->toggleViewAction()->isChecked());
      REQUIRE(!docks.console->toggleViewAction()->isChecked());

      toggleInfoPanel->trigger();
      QApplication::processEvents();
      for (auto* dock : docks.infoPanels())
      {
        CHECK(dock->isHidden());
      }

      toggleInfoPanel->trigger();
      QApplication::processEvents();
      CHECK(docks.issues->toggleViewAction()->isChecked());
      CHECK(!docks.console->toggleViewAction()->isChecked());
      CHECK(
        window.tabifiedDockWidgets(docks.console) == QList<QDockWidget*>{docks.issues});
    }

    SECTION("the old info panel's page comes to the front")
    {
      writeOldState(OldInfoPanelPageKey, 1);

      window.positionOnScreen(nullptr);
      QApplication::processEvents();
      CHECK(docks.issues->toggleViewAction()->isChecked());
      CHECK(!docks.console->toggleViewAction()->isChecked());
    }

    SECTION("closing the console and issues docks unchecks Toggle Info Panel")
    {
      auto* toggleInfoPanel = findViewMenuAction(window, "Toggle Info Panel");
      QApplication::processEvents();
      REQUIRE(toggleInfoPanel->isChecked());

      // the console in front first, then the issue browser that comes to the front
      docks.console->toggleViewAction()->trigger();
      QApplication::processEvents();
      REQUIRE(docks.issues->toggleViewAction()->isChecked());
      docks.issues->toggleViewAction()->trigger();
      QApplication::processEvents();

      for (auto* dock : docks.infoPanels())
      {
        CHECK(dock->isHidden());
      }
      CHECK(!toggleInfoPanel->isChecked());
    }

    SECTION("Toggle Inspector brings the dock that was in front back in front")
    {
      docks.entity->raise();
      QApplication::processEvents();
      REQUIRE(docks.entity->toggleViewAction()->isChecked());
      REQUIRE(!docks.map->toggleViewAction()->isChecked());

      toggleInspector->trigger();
      QApplication::processEvents();
      for (auto* dock : docks.inspectors())
      {
        CHECK(dock->isHidden());
      }

      toggleInspector->trigger();
      QApplication::processEvents();
      CHECK(docks.entity->toggleViewAction()->isChecked());
      CHECK(!docks.map->toggleViewAction()->isChecked());
      CHECK(!docks.face->toggleViewAction()->isChecked());
      CHECK(
        window.tabifiedDockWidgets(docks.map)
        == QList<QDockWidget*>{docks.entity, docks.face});
    }

    SECTION("Toggle Inspector brings every dock that was in front back in front")
    {
      // the face inspector on its own at the left edge: Map in front of the group on the
      // right and Face can both be seen, Entity is behind Map
      window.removeDockWidget(docks.face);
      window.addDockWidget(Qt::LeftDockWidgetArea, docks.face);
      docks.face->show();
      QApplication::processEvents();
      REQUIRE(docks.map->toggleViewAction()->isChecked());
      REQUIRE(docks.face->toggleViewAction()->isChecked());
      REQUIRE(!docks.entity->toggleViewAction()->isChecked());

      toggleInspector->trigger();
      QApplication::processEvents();
      for (auto* dock : docks.inspectors())
      {
        CHECK(dock->isHidden());
      }

      toggleInspector->trigger();
      QApplication::processEvents();
      for (auto* dock : docks.inspectors())
      {
        CHECK(!dock->isHidden());
      }
      CHECK(docks.map->toggleViewAction()->isChecked());
      CHECK(docks.face->toggleViewAction()->isChecked());
      CHECK(!docks.entity->toggleViewAction()->isChecked());
      CHECK(window.dockWidgetArea(docks.face) == Qt::LeftDockWidgetArea);
    }

    SECTION("Show Face Inspector brings a dock behind a tab to the front")
    {
      findViewMenuAction(window, "Show Face Inspector")->trigger();
      QApplication::processEvents();
      CHECK(docks.face->toggleViewAction()->isChecked());
      CHECK(!docks.map->toggleViewAction()->isChecked());
    }

    SECTION("the old inspector's page comes to the front")
    {
      writeOldState(OldInspectorPageKey, 2);

      window.positionOnScreen(nullptr);
      QApplication::processEvents();
      CHECK(docks.face->toggleViewAction()->isChecked());
      CHECK(!docks.map->toggleViewAction()->isChecked());
      CHECK(!docks.entity->toggleViewAction()->isChecked());
    }

    SECTION("closing the inspector docks unchecks Toggle Inspector")
    {
      QApplication::processEvents();
      REQUIRE(toggleInspector->isChecked());

      // a click on the checked button of the dock in front closes it, and the next one
      // comes to the front
      for (auto i = 0; i < 3; ++i)
      {
        const auto inspectors = docks.inspectors();
        const auto iFront = std::ranges::find_if(inspectors, [](const auto* dock) {
          return dock->toggleViewAction()->isChecked();
        });
        REQUIRE(iFront != inspectors.end());
        (*iFront)->toggleViewAction()->trigger();
        QApplication::processEvents();
      }

      for (auto* dock : docks.inspectors())
      {
        CHECK(dock->isHidden());
      }
      CHECK(!toggleInspector->isChecked());
    }
  }

  SECTION("View > Toggle Tool Sidebar")
  {
    auto* toolSidebar = window.findChild<ToolSidebar*>("MapWindow_ToolSidebar");
    REQUIRE(toolSidebar != nullptr);

    auto* viewMenu = findViewMenu(window);
    REQUIRE(viewMenu != nullptr);

    // the entry follows Toggle Toolbar
    const auto viewActions = viewMenu->actions();
    const auto iToolbar = std::ranges::find_if(
      viewActions, [](const auto* action) { return action->text() == "Toggle Toolbar"; });
    REQUIRE(iToolbar != viewActions.end());
    REQUIRE(std::next(iToolbar) != viewActions.end());

    auto* action = *std::next(iToolbar);
    CHECK(action->text() == "Toggle Tool Sidebar");
    CHECK(action->isCheckable());
    CHECK(std::ranges::all_of(
      action->shortcuts(), [](const auto& shortcut) { return shortcut.isEmpty(); }));

    REQUIRE(!toolSidebar->isHidden());
    action->trigger();
    CHECK(toolSidebar->isHidden());
    action->trigger();
    CHECK(!toolSidebar->isHidden());
  }

  SECTION("View > Light Volumes")
  {
    auto& prefs = PreferenceManager::instance();
    prefs.resetToDefault(Preferences::ShowLightVolumes);

    // also when a REQUIRE fails, so that no later test case sees a changed preference
    const auto resetShowLightVolumes =
      kdl::invoke_later{[&] { prefs.resetToDefault(Preferences::ShowLightVolumes); }};

    const auto findLightVolumeActions = [&]() {
      QMenu* lightVolumesMenu = nullptr;
      for (auto* menuAction : window.menuBar()->actions())
      {
        if (menuAction->text() == "View")
        {
          for (auto* action : QMenu::menuInAction(menuAction)->actions())
          {
            if (action->text() == "Light Volumes")
            {
              lightVolumesMenu = QMenu::menuInAction(action);
            }
          }
        }
      }
      REQUIRE(lightVolumesMenu != nullptr);
      return lightVolumesMenu->actions();
    };

    SECTION("the check marks follow the preference")
    {
      const auto actions = findLightVolumeActions();
      REQUIRE(actions.size() == 3);

      setPref(Preferences::ShowLightVolumes, Preferences::ShowLightVolumesAll);
      QApplication::processEvents();
      CHECK(!actions[0]->isChecked());
      CHECK(!actions[1]->isChecked());
      CHECK(actions[2]->isChecked());

      // the Quake game has no light editor
      CHECK(!actions[0]->isEnabled());
      CHECK(!actions[1]->isEnabled());
      CHECK(!actions[2]->isEnabled());

      prefs.resetToDefault(Preferences::ShowLightVolumes);
      QApplication::processEvents();
      CHECK(actions[1]->isChecked());
      CHECK(!actions[2]->isChecked());
    }

    SECTION("the actions write ShowLightVolumes in a game with the light editor")
    {
      REQUIRE(window.document().create(
        appController.environmentConfig(),
        mdl::Doom3BFGGameInfo,
        mdl::MapFormat::Doom3_Valve,
        vm::bbox3d{8192.0}));
      QApplication::processEvents();

      const auto actions = findLightVolumeActions();
      REQUIRE(actions.size() == 3);
      CHECK(actions[0]->isEnabled());
      CHECK(actions[1]->isEnabled());
      CHECK(actions[2]->isEnabled());

      const auto faceRenderMode = pref(Preferences::FaceRenderMode);
      const auto values = std::vector<std::string>{
        Preferences::ShowLightVolumesOff,
        Preferences::ShowLightVolumesSelected,
        Preferences::ShowLightVolumesAll,
      };

      for (const auto i : {0, 2, 1})
      {
        actions[i]->trigger();
        QApplication::processEvents();

        CHECK(pref(Preferences::ShowLightVolumes) == values[size_t(i)]);
        CHECK(pref(Preferences::FaceRenderMode) == faceRenderMode);
        for (auto j = 0; j < 3; ++j)
        {
          CHECK(actions[j]->isChecked() == (j == i));
        }
      }
    }
  }

  SECTION("the light volume tool leaves the action context at no tool")
  {
    // the actions of Delete, Cut, Group, Flip, Edit > Move and the keyboard nudges
    // require NoTool, so they keep working while the tool is active (D31)
    const auto views = window.findChildren<MapViewBase*>();
    REQUIRE(!views.empty());

    auto& toolBox = window.toolBox();
    const auto contexts = [&]() {
      auto result = std::vector<ActionContext::Type>{};
      for (const auto* view : views)
      {
        result.push_back(view->actionContext());
      }
      return result;
    };

    const auto contextsWithoutTool = contexts();
    for (const auto context : contextsWithoutTool)
    {
      CHECK((context & ActionContext::NoTool) != 0);
    }

    toolBox.toggleTool(toolBox.lightVolumeTool());
    REQUIRE(toolBox.lightVolumeToolActive());

    CHECK(contexts() == contextsWithoutTool);
    CHECK_FALSE(toolBox.anyModalToolActive());

    toolBox.toggleTool(toolBox.lightVolumeTool());
    CHECK_FALSE(toolBox.lightVolumeToolActive());
  }

  SECTION("the light volume tool leaves the action context at no tool with a light")
  {
    REQUIRE(window.document().create(
      appController.environmentConfig(),
      mdl::Doom3BFGGameInfo,
      mdl::MapFormat::Doom3_Valve,
      vm::bbox3d{8192.0}));
    QApplication::processEvents();

    auto& map = window.document().map();
    auto* lightNode = new mdl::EntityNode{mdl::Entity{{
      {"classname", "light"},
      {"origin", "0 0 0"},
    }}};
    REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {lightNode}}}).empty());
    mdl::selectNodes(map, {lightNode});
    QApplication::processEvents();

    const auto views = window.findChildren<MapViewBase*>();
    REQUIRE(!views.empty());

    auto& toolBox = window.toolBox();
    REQUIRE(toolBox.lightVolumeTool().selectedLight() == lightNode);

    const auto contexts = [&]() {
      auto result = std::vector<ActionContext::Type>{};
      for (const auto* view : views)
      {
        result.push_back(view->actionContext());
      }
      return result;
    };

    const auto contextsWithoutTool = contexts();
    for (const auto context : contextsWithoutTool)
    {
      CHECK((context & ActionContext::NoTool) != 0);
      CHECK((context & ActionContext::NodeSelection) != 0);
    }

    toolBox.toggleTool(toolBox.lightVolumeTool());
    REQUIRE(toolBox.lightVolumeToolActive());
    CHECK(contexts() == contextsWithoutTool);

    toolBox.toggleTool(toolBox.lightVolumeTool());
    CHECK(contexts() == contextsWithoutTool);
  }

  SECTION("the keys that move objects shear a spot light's clicked base (D68)")
  {
    REQUIRE(window.document().create(
      appController.environmentConfig(),
      mdl::Doom3BFGGameInfo,
      mdl::MapFormat::Doom3_Valve,
      vm::bbox3d{8192.0}));
    QApplication::processEvents();

    auto& map = window.document().map();
    map.grid().setSize(4); // 16 units
    auto* lightNode = new mdl::EntityNode{mdl::Entity{{
      {"classname", "light"},
      {"origin", "0 0 0"},
      {"light_target", "0 0 -256"},
      {"light_right", "-128 0 0"},
      {"light_up", "0 -128 0"},
    }}};
    REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {lightNode}}}).empty());
    mdl::selectNodes(map, {lightNode});
    QApplication::processEvents();

    auto& tool = window.toolBox().lightVolumeTool();
    REQUIRE(tool.selectedLight() == lightNode);
    const auto clickBase = [&]() {
      auto pickResult = mdl::PickResult{};
      tool.pickSurfaces(vm::ray3d{{0, 0, -1000}, {0, 0, 1}}, pickResult);
      const auto& hit =
        pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType));
      REQUIRE(hit.isMatch());
      REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      tool.setClickedSurface(hit);
    };
    const auto valueOf = [](const mdl::EntityNode& node, const std::string& key) {
      const auto* value = node.entity().property(key);
      REQUIRE(value != nullptr);
      return *value;
    };

    // a 2D view whose "right" is world +X, one grid step of 16
    const auto views = window.findChildren<MapView2D*>();
    const auto iView = std::ranges::find_if(views, [](const auto* view) {
      return view->moveDirection(vm::direction::right) == vm::vec3d{1, 0, 0};
    });
    REQUIRE(iView != views.end());
    auto* view = *iView;
    const auto delta =
      view->moveDirection(vm::direction::right) * double(map.grid().actualSize());
    REQUIRE(delta == vm::vec3d{16, 0, 0});

    // the clicked base takes the step: the light stays, its beam turns (AC-26)
    clickBase();
    view->moveObjects(vm::direction::right);
    CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    CHECK(valueOf(*lightNode, "light_target") == "15.52 0 -256.01");

    // without it, the step moves the light
    tool.clearClickedSurface();
    view->moveObjects(vm::direction::right);
    CHECK(valueOf(*lightNode, "origin") == "16 0 0");
    CHECK(valueOf(*lightNode, "light_target") == "15.52 0 -256.01");

    // Duplicate and move: the duplicate is the new selection, which forgets the clicked
    // base, so it is moved
    map.undoCommand();
    map.undoCommand();
    REQUIRE(valueOf(*lightNode, "origin") == "0 0 0");
    REQUIRE(valueOf(*lightNode, "light_target") == "0 0 -256");
    clickBase();
    const auto lightCount = map.worldNode().defaultLayer()->childCount();
    view->duplicateAndMoveObjects(vm::direction::right);
    CHECK(map.worldNode().defaultLayer()->childCount() == lightCount + 1);
    REQUIRE(map.selection().entities.size() == 1);
    const auto* duplicateNode = map.selection().entities.front();
    CHECK(duplicateNode != lightNode);
    CHECK(valueOf(*duplicateNode, "origin") == "16 0 0");
    CHECK(valueOf(*duplicateNode, "light_target") == "0 0 -256");
    CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
  }

  SECTION("Deactivate Current Tool is unchecked while the light volume tool is active")
  {
    QAction* deactivateAction = nullptr;
    for (auto* menuAction : window.menuBar()->actions())
    {
      if (menuAction->text() == "Tools")
      {
        for (auto* action : QMenu::menuInAction(menuAction)->actions())
        {
          if (action->text() == "Deactivate Current Tool")
          {
            deactivateAction = action;
          }
        }
      }
    }
    REQUIRE(deactivateAction != nullptr);

    auto& toolBox = window.toolBox();
    QApplication::processEvents();
    REQUIRE(deactivateAction->isChecked());

    toolBox.toggleTool(toolBox.lightVolumeTool());
    QApplication::processEvents();
    CHECK_FALSE(deactivateAction->isChecked());

    toolBox.toggleTool(toolBox.lightVolumeTool());
    QApplication::processEvents();
    CHECK(deactivateAction->isChecked());
  }

  SECTION("canReloadMaterialCollections returns false when resources need processing")
  {
    using TestResourceT = gl::Resource<TestResource>;

    // Verify initial state: no resources pending processing
    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadMaterialCollections());

    // Add a resource that needs processing
    auto testResource = std::make_shared<TestResourceT>(
      []() { return Result<TestResource>{TestResource{}}; });
    appController.glManager().resourceManager().addResource(testResource);

    REQUIRE(appController.glManager().resourceManager().needsProcessing());
    CHECK(!window.canReloadMaterialCollections());

    // Process all resources synchronously
    auto testGl = gl::TestGl{};
    const auto processContext = gl::ProcessContext{testGl, [](auto, auto) {}};
    gl::processResourcesSync(appController.glManager().resourceManager(), processContext);

    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadMaterialCollections());
  }

  SECTION("canReloadEntityDefinitions returns false when resources need processing")
  {
    using TestResourceT = gl::Resource<TestResource>;

    // Verify initial state: no resources pending processing
    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadEntityDefinitions());

    // Add a resource that needs processing
    auto testResource = std::make_shared<TestResourceT>(
      []() { return Result<TestResource>{TestResource{}}; });
    appController.glManager().resourceManager().addResource(testResource);

    REQUIRE(appController.glManager().resourceManager().needsProcessing());
    CHECK(!window.canReloadEntityDefinitions());

    // Process all resources synchronously
    auto testGl = gl::TestGl{};
    const auto processContext = gl::ProcessContext{testGl, [](auto, auto) {}};
    gl::processResourcesSync(appController.glManager().resourceManager(), processContext);

    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadEntityDefinitions());
  }
}

} // namespace tb::ui