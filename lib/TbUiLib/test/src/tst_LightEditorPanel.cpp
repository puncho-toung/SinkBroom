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

#include <QApplication>

#include "SidebarTestUtils.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/LightEditorPanel.h"
#include "ui/MapDocument.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("LightEditorPanel")
{
  const auto editorText = QString{"Light selected (editor comes with LW3)"};

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
    // the Quake game config has no entities.lightEditor
    auto fixture = SidebarContextFixture{mdl::QuakeFixtureConfig};
    REQUIRE(!fixture.document().map().gameInfo().gameConfig.entityConfig.lightEditor);

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

    // a group that holds a light: allEntities() would yield the light
    auto* groupedLightNode = mdl::createPointEntity(map, *lightDefinition, {0, 64, 64});
    REQUIRE(groupedLightNode != nullptr);
    auto* groupNode = mdl::groupSelectedNodes(map, "group");
    REQUIRE(groupNode != nullptr);

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
    CHECK(select({groupNode}) == "Select a light");
    CHECK(select({brushNode, lightNode}) == "Select only lights");
    CHECK(select({playerStartNode, lightNode}) == "Select only lights");
    CHECK(select({lightNode}) == editorText);
    CHECK(select({}) == "Select a light");
  }

  SECTION("follows a selected light that stops being a light")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);

    auto panel = LightEditorPanel{fixture.context()};
    REQUIRE(mdl::createPointEntity(map, *lightDefinition, {0, 0, 64}) != nullptr);
    QApplication::processEvents();
    REQUIRE(currentText(panel) == editorText);

    // the selection stays the same; only the document changes
    REQUIRE(mdl::setEntityProperty(map, "classname", "func_static"));
    QApplication::processEvents();
    CHECK(currentText(panel) == "Select a light");
  }

  SECTION("shows the editor at once when a light is already selected")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);
    REQUIRE(mdl::createPointEntity(map, *lightDefinition, {0, 0, 64}) != nullptr);

    // no events processed: the constructor shows the right page
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(currentText(panel) == editorText);
  }
}

} // namespace tb::ui
