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
#include <QComboBox>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>

#include "TestEnvironment.h"
#include "base/PreferenceManager.h"
#include "base/Result.h"
#include "gl/Camera.h"
#include "gl/GlManager.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "prefs/Preferences.h"
#include "ui/ActionContext.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/LightEditorPanel.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapView2D.h"
#include "ui/MapViewBase.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"

#include "kd/invoke.h"

#include "vm/vec.h"

#include <algorithm>
#include <iterator>
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

TEST_CASE("MapWindow")
{
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

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
    auto* dock = window.findChild<QDockWidget*>("MapWindow_Sidebar_LightEditor");
    REQUIRE(dock != nullptr);
    CHECK(dock->windowTitle() == "Light Editor");
    CHECK(qobject_cast<LightEditorPanel*>(dock->widget()) != nullptr);

    QMenu* viewMenu = nullptr;
    for (auto* action : window.menuBar()->actions())
    {
      if (action->text() == "View")
      {
        viewMenu = QMenu::menuInAction(action);
      }
    }
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
    CHECK(sidebarMenu->actions() == QList<QAction*>{dock->toggleViewAction()});
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