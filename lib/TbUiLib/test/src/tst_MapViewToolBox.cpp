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
#include <QStackedLayout>
#include <QWidget>

#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Hit.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "ui/AssembleBrushTool.h"
#include "ui/CatchConfig.h"
#include "ui/ClipTool.h" // IWYU pragma: keep
#include "ui/DrawShapeTool.h"
#include "ui/EdgeTool.h"
#include "ui/ExtrudeTool.h"
#include "ui/FaceTool.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapViewToolBox.h"
#include "ui/MoveObjectsTool.h"
#include "ui/RotateTool.h" // IWYU pragma: keep
#include "ui/ScaleTool.h"
#include "ui/ShearTool.h"
#include "ui/VertexTool.h" // IWYU pragma: keep

#include "vm/vec.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MapViewToolBox")
{
  auto documentFixture = MapDocumentFixture{};
  auto& document = documentFixture.create(mdl::Doom3BFGFixtureConfig);

  auto toolPages = QWidget{};
  auto* toolPageLayout = new QStackedLayout{&toolPages};
  auto toolBox = MapViewToolBox{document, toolPageLayout};

  auto& lightVolumeTool = toolBox.lightVolumeTool();

  SECTION("lightVolumeTool")
  {
    SECTION("is inactive until the light editor panel activates it")
    {
      CHECK_FALSE(lightVolumeTool.active());
    }

    SECTION("its handles are active without it, and the modal tools suppress them")
    {
      // the handles work whenever the move tool does (D56)
      auto& handles = lightVolumeTool.handles();
      CHECK(handles.active());

      toolBox.toggleTool(lightVolumeTool);
      CHECK(handles.active());
      toolBox.toggleTool(lightVolumeTool);

      for (auto* modalTool : std::vector<Tool*>{
             &toolBox.rotateTool(),
             &toolBox.scaleTool(),
             &toolBox.shearTool(),
             &toolBox.vertexTool(),
             &toolBox.edgeTool(),
             &toolBox.faceTool(),
             &toolBox.assembleBrushTool()})
      {
        toolBox.toggleTool(*modalTool);
        REQUIRE(modalTool->active());
        CHECK_FALSE(handles.active());
        CHECK(handles.active() == toolBox.moveObjectsTool().active());

        toolBox.toggleTool(*modalTool);
        CHECK(handles.active());
      }
    }

    SECTION("is in both exclusive groups")
    {
      // the vertex tool is only in the second group
      toolBox.toggleTool(toolBox.vertexTool());
      REQUIRE(toolBox.vertexToolActive());

      toolBox.toggleTool(lightVolumeTool);
      CHECK(toolBox.lightVolumeToolActive());
      CHECK_FALSE(toolBox.vertexToolActive());

      // the rotate tool is only in the first group
      toolBox.toggleTool(toolBox.rotateTool());
      CHECK(toolBox.rotateToolActive());
      CHECK_FALSE(toolBox.lightVolumeToolActive());
    }

    SECTION("suppresses the extrude and draw shape tools, not the move tool")
    {
      REQUIRE(toolBox.moveObjectsTool().active());
      REQUIRE(toolBox.extrudeTool().active());
      REQUIRE(toolBox.drawShapeTool().active());

      toolBox.toggleTool(lightVolumeTool);
      CHECK(toolBox.moveObjectsTool().active());
      CHECK_FALSE(toolBox.extrudeTool().active());
      CHECK_FALSE(toolBox.drawShapeTool().active());

      toolBox.toggleTool(lightVolumeTool);
      CHECK(toolBox.moveObjectsTool().active());
      CHECK(toolBox.extrudeTool().active());
      CHECK(toolBox.drawShapeTool().active());
    }

    SECTION("is deactivated by Esc")
    {
      toolBox.toggleTool(lightVolumeTool);
      REQUIRE(toolBox.deactivateCurrentTool());
      CHECK_FALSE(lightVolumeTool.active());
    }

    SECTION("a handle drag ends quietly when a modal tool suppresses the handles")
    {
      auto& map = document.map();
      auto* lightNode = new mdl::EntityNode{mdl::Entity{{
        {"classname", "light"},
        {"origin", "0 0 0"},
      }}};
      REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {lightNode}}}).empty());
      mdl::selectNodes(map, {lightNode});
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      // the handles work without the tool (D56)
      REQUIRE(lightVolumeTool.beginDrag(mdl::Hit{
        LightVolumeTool::FaceCenterHitType,
        0.0,
        vm::vec3d{0, 0, 0},
        BBoxSide{vm::vec3d{1, 0, 0}}}));
      REQUIRE(lightVolumeTool.update(vm::vec3d{32, 0, 0}));

      toolBox.toggleTool(toolBox.rotateTool());
      REQUIRE_FALSE(lightVolumeTool.handles().active());
      CHECK_FALSE(lightVolumeTool.isDragging());
      CHECK(lightNode->entity().properties() == properties);

      CHECK_FALSE(lightVolumeTool.update(vm::vec3d{32, 0, 0}));
      lightVolumeTool.endDrag();
      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
    }

    SECTION("turns off when it leaves draw mode, once the drag has gone (D56)")
    {
      // Esc, or the end of a drawn volume
      toolBox.toggleTool(lightVolumeTool);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Draw);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Handles);
      CHECK(lightVolumeTool.active());

      QApplication::processEvents();
      CHECK_FALSE(lightVolumeTool.active());
      CHECK(toolBox.extrudeTool().active());
      CHECK(toolBox.drawShapeTool().active());

      // drawing again in the meantime keeps it on
      toolBox.toggleTool(lightVolumeTool);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Draw);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Handles);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Draw);
      QApplication::processEvents();
      CHECK(lightVolumeTool.active());
    }

    SECTION("is deactivated when a map is loaded")
    {
      toolBox.toggleTool(lightVolumeTool);
      lightVolumeTool.setMode(LightVolumeTool::Mode::Draw);
      document.documentWasLoadedNotifier();
      CHECK_FALSE(lightVolumeTool.active());
      CHECK(lightVolumeTool.mode() == LightVolumeTool::Mode::Handles);
    }
  }

  SECTION("lightVolumeToolActive")
  {
    CHECK_FALSE(toolBox.lightVolumeToolActive());
    toolBox.toggleTool(lightVolumeTool);
    CHECK(toolBox.lightVolumeToolActive());
  }

  SECTION("anyModalToolActive")
  {
    // Delete, Cut, Group, Flip and Edit > Move keep working (D31)
    toolBox.toggleTool(lightVolumeTool);
    REQUIRE(toolBox.lightVolumeToolActive());
    CHECK_FALSE(toolBox.anyModalToolActive());
  }

  SECTION("updateToolPage")
  {
    // the draw shape tool's page is shown when no modal tool is active
    const auto* drawShapePage = toolPageLayout->currentWidget();
    REQUIRE(drawShapePage != nullptr);

    toolBox.toggleTool(lightVolumeTool);
    CHECK(toolPageLayout->currentIndex() == 0);
    CHECK(toolPageLayout->currentWidget() != drawShapePage);

    toolBox.toggleTool(lightVolumeTool);
    CHECK(toolPageLayout->currentWidget() == drawShapePage);
  }
}

} // namespace tb::ui
