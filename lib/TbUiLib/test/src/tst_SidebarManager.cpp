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
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QStackedLayout>
#include <QStackedWidget>

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/LightEditorPanel.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapViewToolBox.h"
#include "ui/SidebarManager.h"
#include "ui/SidebarPanel.h"
#include "ui/SidebarPanels.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

// Test sources are not moc'd, so the stub declares no Q_OBJECT.
class StubPanel : public SidebarPanel
{
private:
  QString m_panelId;

public:
  int shownCount = 0;
  int hiddenCount = 0;

  explicit StubPanel(QString panelId)
    : m_panelId{std::move(panelId)}
  {
  }

  QString panelId() const override { return m_panelId; }
  QString title() const override { return m_panelId + " title"; }
  void panelShown() override { ++shownCount; }
  void panelHidden() override { ++hiddenCount; }
};

struct RegisteredStub
{
  QDockWidget* dock;
  StubPanel* panel;
};

RegisteredStub registerStubPanel(SidebarManager& manager, const QString& panelId)
{
  auto panel = std::make_unique<StubPanel>(panelId);
  auto* panelPtr = panel.get();
  auto* dock = manager.registerPanel(std::move(panel));
  return {dock, panelPtr};
}

/**
 * A document of the given game with a tool box, as a map window provides them.
 */
class SidebarContextFixture
{
private:
  AppControllerFixture m_appControllerFixture;
  MapDocumentFixture m_documentFixture;
  MapDocument& m_document;
  QWidget m_toolPages;
  MapViewToolBox m_toolBox;

public:
  explicit SidebarContextFixture(mdl::MapFixtureConfig config = {})
    : m_document{m_documentFixture.create(std::move(config))}
    , m_toolBox{m_document, new QStackedLayout{&m_toolPages}}
  {
  }

  MapDocument& document() { return m_document; }

  SidebarContext context()
  {
    return {m_appControllerFixture.appController(), m_document, m_toolBox};
  }
};

QString currentText(const LightEditorPanel& panel)
{
  const auto* stack = panel.findChild<QStackedWidget*>("LightEditorPanel_Stack");
  REQUIRE(stack != nullptr);
  const auto* label = stack->currentWidget()->findChild<QLabel*>();
  REQUIRE(label != nullptr);
  return label->text();
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

TEST_CASE("SidebarPanels")
{
  SECTION("createSidebarPanels")
  {
    auto fixture = SidebarContextFixture{};
    const auto panels = createSidebarPanels(fixture.context());

    auto panelIds = std::vector<QString>{};
    for (const auto& panel : panels)
    {
      panelIds.push_back(panel->panelId());
    }
    CHECK(panelIds == std::vector<QString>{"LightEditor"});
  }
}

TEST_CASE("LightEditorPanel")
{
  SECTION("panelId")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(panel.panelId() == "LightEditor");
  }

  SECTION("title")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(panel.title() == "Light Editor");
  }

  SECTION("needs a game with a light editor")
  {
    // the default test game's config has no entities.lightEditor
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(currentText(panel) == "This game has no light editor");
  }

  SECTION("follows the selection")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    const auto* playerStartDefinition =
      map.entityDefinitionManager().definition("info_player_start");
    REQUIRE(lightDefinition != nullptr);
    REQUIRE(playerStartDefinition != nullptr);

    auto panel = LightEditorPanel{fixture.context()};
    CHECK(currentText(panel) == "Select a light");

    auto* lightNode = mdl::createPointEntity(map, *lightDefinition, {0, 0, 64});
    auto* playerStartNode =
      mdl::createPointEntity(map, *playerStartDefinition, {64, 0, 64});
    REQUIRE(lightNode != nullptr);
    REQUIRE(playerStartNode != nullptr);

    const auto builder =
      mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    auto* brushNode = new mdl::BrushNode{
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 32, 32}}, "wall") | kdl::value()};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});

    const auto select = [&](const std::vector<mdl::Node*>& nodes) {
      mdl::deselectAll(map);
      if (!nodes.empty())
      {
        mdl::selectNodes(map, nodes);
      }
      QApplication::processEvents();
      return currentText(panel);
    };

    CHECK(select({}) == "Select a light");
    CHECK(select({brushNode}) == "Select a light");
    CHECK(select({playerStartNode}) == "Select a light");
    CHECK(select({brushNode, lightNode}) == "Select only lights");
    CHECK(select({playerStartNode, lightNode}) == "Select only lights");
    CHECK(select({lightNode}) == "Light selected (editor comes with LW3)");
    CHECK(select({}) == "Select a light");
  }
}

} // namespace tb::ui
