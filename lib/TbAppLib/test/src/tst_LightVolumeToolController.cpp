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

#include "gl/Camera.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/LayerNode.h" // IWYU pragma: keep
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/LightVolumeTool.h"
#include "ui/LightVolumeToolController.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/PickRequest.h"
#include "ui/ScaleTool.h"
#include "ui/ToolController.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::ui
{
namespace
{

mdl::EntityNode* createLight(mdl::Map& map, const std::string& origin)
{
  const auto* definition = map.entityDefinitionManager().definition("light");
  REQUIRE(definition != nullptr);

  const auto position = vm::parse<double, 3>(origin);
  REQUIRE(position);

  auto* entityNode = mdl::createPointEntity(map, *definition, *position);
  REQUIRE(entityNode != nullptr);
  REQUIRE(mdl::setEntityProperty(map, "origin", origin));
  return entityNode;
}

std::string valueOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  REQUIRE(value != nullptr);
  return *value;
}

mdl::BrushNode* createBrush(mdl::Map& map)
{
  auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto* brushNode = new mdl::BrushNode{
    builder.createCuboid(vm::bbox3d{{256, 256, 0}, {320, 320, 64}}, "material")
    | kdl::value()};
  REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}}).empty());
  return brushNode;
}

mdl::Hit entityHit(mdl::EntityNode* entityNode, const double distance)
{
  return mdl::Hit{
    mdl::EntityNode::EntityHitType, distance, vm::vec3d{distance, 0, 0}, entityNode};
}

mdl::Hit cornerHit(const double distance)
{
  return mdl::Hit{
    LightVolumeTool::CornerHitType,
    distance,
    vm::vec3d{distance, 0, 0},
    BBoxCorner{vm::vec3d{1, 1, 1}}};
}

mdl::Hit sideHit(const double distance)
{
  return mdl::Hit{
    LightVolumeTool::SideHitType,
    distance,
    vm::vec3d{distance, 0, 0},
    BBoxSide{vm::vec3d{1, 0, 0}}};
}

gl::PerspectiveCamera perspectiveCameraFor(const vm::ray3d& pickRay)
{
  const auto viewport = gl::Camera::Viewport{0, 0, 1920, 1080};
  return gl::PerspectiveCamera{
    90.0f,
    1.0f,
    8000.0f,
    viewport,
    vm::vec3f{pickRay.origin},
    vm::vec3f{vm::normalize(pickRay.direction)},
    vm::vec3f{0, 0, 1}};
}

gl::OrthographicCamera orthographicCameraFor(const vm::ray3d& pickRay)
{
  const auto viewport = gl::Camera::Viewport{0, 0, 1920, 1080};
  return gl::OrthographicCamera{
    1.0f,
    8000.0f,
    viewport,
    vm::vec3f{pickRay.origin},
    vm::vec3f{vm::normalize(pickRay.direction)},
    vm::vec3f{0, 1, 0}};
}

InputState inputStateFor(
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const std::vector<mdl::Hit>& hits,
  const ModifierKeyState modifiers = ModifierKeys::None,
  const MouseButtonState button = MouseButtons::Left)
{
  auto inputState = InputState{0.0f, 0.0f};
  inputState.setPickRequest(PickRequest{pickRay, camera});
  auto pickResult = mdl::PickResult{};
  for (const auto& hit : hits)
  {
    pickResult.addHit(hit);
  }
  inputState.setPickResult(std::move(pickResult));
  inputState.setModifierKeys(modifiers);
  inputState.mouseDown(button);
  return inputState;
}

} // namespace

TEST_CASE("LightVolumeToolController")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(mdl::Doom3BFGFixtureConfig);
  auto& map = document.map();

  auto tool = LightVolumeTool{document};
  REQUIRE(tool.activate());

  auto* lightNode = createLight(map, "0 0 0");
  REQUIRE(tool.selectedLight() == lightNode);

  const auto pickRay = vm::ray3d{{1000, 0, 0}, {-1, 0, 0}};
  const auto camera = perspectiveCameraFor(pickRay);

  SECTION("3D controller")
  {
    auto controller3D = LightVolumeToolController3D{tool, document};
    auto& controller = static_cast<ToolController&>(controller3D);

    SECTION("tool returns the wrapped tool")
    {
      CHECK(&controller.tool() == static_cast<Tool*>(&tool));
    }

    SECTION("pick")
    {
      SECTION("adds the tool's hit in handle mode")
      {
        auto inputState = inputStateFor(pickRay, camera, {});
        auto pickResult = mdl::PickResult{};
        controller.pick(inputState, pickResult);
        CHECK(pickResult.first(mdl::HitFilters::type(LightVolumeTool::SideHitType))
                .isMatch());
      }

      SECTION("adds nothing in draw mode")
      {
        tool.setMode(LightVolumeTool::Mode::Draw);
        auto inputState = inputStateFor(pickRay, camera, {});
        auto pickResult = mdl::PickResult{};
        controller.pick(inputState, pickResult);
        CHECK(pickResult.empty());
      }
    }

    SECTION("acceptMouseDrag")
    {
      SECTION("a corner behind a nearer selected node is the tool's")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), cornerHit(20)});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);
        CHECK(tool.isDragging());
        CHECK(tool.dragHit().type() == LightVolumeTool::CornerHitType);

        tracker->cancel();
        CHECK_FALSE(tool.isDragging());
      }

      SECTION("a side behind a selected node is the move tool's")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), sideHit(20)});
        CHECK(controller.acceptMouseDrag(inputState) == nullptr);
        CHECK_FALSE(tool.isDragging());
      }

      SECTION("a side without a selected node is the tool's")
      {
        auto inputState = inputStateFor(pickRay, camera, {sideHit(20)});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);
        CHECK(tool.dragHit().type() == LightVolumeTool::SideHitType);
        tracker->cancel();
      }

      SECTION("Ctrl is paint select")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {cornerHit(20)}, ModifierKeys::CtrlCmd);
        CHECK(controller.acceptMouseDrag(inputState) == nullptr);
        CHECK_FALSE(tool.isDragging());
      }

      SECTION("only the left button")
      {
        auto inputState = inputStateFor(
          pickRay, camera, {cornerHit(20)}, ModifierKeys::None, MouseButtons::Right);
        CHECK(controller.acceptMouseDrag(inputState) == nullptr);
      }

      SECTION("Alt anchors the box at its centre, Shift scales proportionally")
      {
        using T = std::tuple<ModifierKeyState, AnchorPos, ProportionalAxes>;
        const auto [modifiers, anchorPos, proportionalAxes] = GENERATE(values<T>({
          {ModifierKeys::None, AnchorPos::Opposite, ProportionalAxes::None()},
          {ModifierKeys::Alt, AnchorPos::Center, ProportionalAxes::None()},
          {ModifierKeys::Shift, AnchorPos::Opposite, ProportionalAxes::All()},
          {ModifierKeys::Alt | ModifierKeys::Shift,
           AnchorPos::Center,
           ProportionalAxes::All()},
        }));
        CAPTURE(modifiers);

        auto inputState = inputStateFor(pickRay, camera, {cornerHit(20)}, modifiers);
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);
        CHECK(tool.anchorPos() == anchorPos);
        CHECK(tool.proportionalAxes() == proportionalAxes);
        tracker->cancel();
      }

      SECTION("a drag moves the light's side")
      {
        map.grid().toggleSnap(); // for a delta that is not snapped

        // rays across the +x side's handle line, through the hit point and 32 units on
        const auto startRay = vm::ray3d{{300, -100, 0}, vm::vec3d{0, 1, 0}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto inputState = inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::SideHitType,
            100.0,
            vm::vec3d{300, 0, 0},
            BBoxSide{vm::vec3d{1, 0, 0}}}});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);

        const auto dragRay = vm::ray3d{{332, -100, 0}, vm::vec3d{0, 1, 0}};
        auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK_FALSE(tool.isDragging());
        CHECK(lightNode->entity().property("origin") != nullptr);
        CHECK(*lightNode->entity().property("origin") == "16 0 0");
        CHECK(*lightNode->entity().property("light_radius") == "316 300 300");
      }

      SECTION("a drag moves a turned light's side along its turned axis")
      {
        map.grid().toggleSnap();

        auto* turnedNode = createLight(map, "0 0 0");
        REQUIRE(mdl::setEntityProperty(map, "angles", "0 90 0"));
        REQUIRE(tool.selectedLight() == turnedNode);

        // the box frame's +x side faces world +Y; rays across its handle line
        const auto startRay = vm::ray3d{{-100, 300, 0}, vm::vec3d{1, 0, 0}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto inputState = inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::SideHitType,
            100.0,
            vm::vec3d{0, 300, 0},
            BBoxSide{vm::vec3d{1, 0, 0}}}});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);

        const auto dragRay = vm::ray3d{{-100, 332, 0}, vm::vec3d{1, 0, 0}};
        auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*turnedNode, "origin") == "0 16 0");
        CHECK(valueOf(*turnedNode, "light_radius") == "316 300 300");
      }

      SECTION("a drag that the tool's deactivation cancelled ends quietly")
      {
        map.grid().toggleSnap();

        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();

        const auto startRay = vm::ray3d{{300, -100, 0}, vm::vec3d{0, 1, 0}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto inputState = inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::SideHitType,
            100.0,
            vm::vec3d{300, 0, 0},
            BBoxSide{vm::vec3d{1, 0, 0}}}});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);

        const auto dragRay = vm::ray3d{{332, -100, 0}, vm::vec3d{0, 1, 0}};
        auto dragInputState = inputStateFor(dragRay, startCamera, {});
        REQUIRE(tracker->update(dragInputState));
        REQUIRE(lightNode->entity().properties() != properties);

        // e.g. a key that turns on another tool, which the tool box does not route
        // through the tracker
        REQUIRE(tool.deactivate());
        CHECK(lightNode->entity().properties() == properties);

        // the tracker lives on: its next update ends the drag
        const auto laterRay = vm::ray3d{{364, -100, 0}, vm::vec3d{0, 1, 0}};
        CHECK_FALSE(tracker->update(inputStateFor(laterRay, startCamera, {})));

        // and after a modifier change it proposes no handle position at all
        tracker->modifierKeyChange(
          inputStateFor(laterRay, startCamera, {}, ModifierKeys::Shift));
        const auto lastRay = vm::ray3d{{396, -100, 0}, vm::vec3d{0, 1, 0}};
        CHECK(tracker->update(inputStateFor(lastRay, startCamera, {})));
        tracker->end(dragInputState);

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("draw mode")
      {
        tool.setMode(LightVolumeTool::Mode::Draw);

        SECTION("draws for the one selected light")
        {
          auto inputState = inputStateFor(pickRay, camera, {});
          auto tracker = controller.acceptMouseDrag(inputState);
          REQUIRE(tracker != nullptr);
          CHECK(tool.drawBox() != std::nullopt);

          tracker->cancel();
          CHECK(tool.drawBox() == std::nullopt);
        }

        SECTION("a drag that starts on the selected light moves it")
        {
          auto inputState = inputStateFor(pickRay, camera, {entityHit(lightNode, 10)});
          CHECK(controller.acceptMouseDrag(inputState) == nullptr);
        }

        SECTION("a selection other than one light refuses the drag")
        {
          auto* brushNode = createBrush(map);
          mdl::selectNodes(map, {brushNode});

          auto inputState = inputStateFor(pickRay, camera, {});
          CHECK(controller.acceptMouseDrag(inputState) == nullptr);
        }

        SECTION("draws a new light with nothing selected")
        {
          mdl::deselectAll(map);

          auto inputState = inputStateFor(pickRay, camera, {});
          auto tracker = controller.acceptMouseDrag(inputState);
          REQUIRE(tracker != nullptr);
          tracker->cancel();
        }
      }
    }

    SECTION("mouseClick")
    {
      SECTION("a corner keeps the light selected")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), cornerHit(20)});
        CHECK(controller.mouseClick(inputState));
      }

      SECTION("a click on nothing keeps the light selected")
      {
        auto inputState = inputStateFor(pickRay, camera, {});
        CHECK(controller.mouseClick(inputState));
      }

      SECTION("a click on a side over nothing keeps the light selected")
      {
        auto inputState = inputStateFor(pickRay, camera, {sideHit(20)});
        CHECK(controller.mouseClick(inputState));
      }

      SECTION("a click on another object selects it")
      {
        auto* otherNode = createLight(map, "512 0 0");
        mdl::deselectAll(map);
        mdl::selectNodes(map, {lightNode});

        auto inputState = inputStateFor(pickRay, camera, {entityHit(otherNode, 10)});
        CHECK_FALSE(controller.mouseClick(inputState));
      }

      SECTION("Ctrl clicks select as usual")
      {
        auto inputState = inputStateFor(pickRay, camera, {}, ModifierKeys::CtrlCmd);
        CHECK_FALSE(controller.mouseClick(inputState));
      }

      SECTION("without a selected light the selection tool decides")
      {
        mdl::deselectAll(map);
        auto inputState = inputStateFor(pickRay, camera, {});
        CHECK_FALSE(controller.mouseClick(inputState));
      }
    }

    SECTION("mouseMove")
    {
      SECTION("highlights the hit of a drag")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), cornerHit(20)});
        controller.mouseMove(inputState);
        CHECK(tool.highlightedHit().type() == LightVolumeTool::CornerHitType);
      }

      SECTION("highlights nothing over a selected node")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), sideHit(20)});
        controller.mouseMove(inputState);
        CHECK_FALSE(tool.highlightedHit().isMatch());
      }

      SECTION("does nothing while a tool drags")
      {
        auto inputState = inputStateFor(pickRay, camera, {cornerHit(20)});
        inputState.setAnyToolDragging(true);
        controller.mouseMove(inputState);
        CHECK_FALSE(tool.highlightedHit().isMatch());
      }
    }

    SECTION("cancel")
    {
      SECTION("leaves draw mode")
      {
        tool.setMode(LightVolumeTool::Mode::Draw);
        CHECK(controller.cancel());
        CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
      }

      SECTION("has nothing to cancel in handle mode")
      {
        CHECK_FALSE(controller.cancel());
      }

      SECTION("has nothing to cancel while the tool is inactive")
      {
        tool.setMode(LightVolumeTool::Mode::Draw);
        REQUIRE(tool.deactivate());
        tool.setMode(LightVolumeTool::Mode::Draw);
        CHECK_FALSE(controller.cancel());
      }
    }
  }

  SECTION("2D controller")
  {
    auto controller2D = LightVolumeToolController2D{tool, document};
    auto& controller = static_cast<ToolController&>(controller2D);

    const auto topRay = vm::ray3d{{300, 0, 1000}, {0, 0, -1}};
    const auto topCamera = orthographicCameraFor(topRay);

    SECTION("pick adds an edge")
    {
      auto inputState = inputStateFor(topRay, topCamera, {});
      auto pickResult = mdl::PickResult{};
      controller.pick(inputState, pickResult);
      CHECK(
        pickResult.first(mdl::HitFilters::type(LightVolumeTool::EdgeHitType)).isMatch());
    }

    SECTION("Shift is proportional except along the view axis")
    {
      auto inputState =
        inputStateFor(topRay, topCamera, {cornerHit(20)}, ModifierKeys::Shift);
      auto tracker = controller.acceptMouseDrag(inputState);
      REQUIRE(tracker != nullptr);
      CHECK(tool.proportionalAxes() == ProportionalAxes{true, true, false});
      tracker->cancel();
    }

    SECTION("Shift on a turned light keeps the box axis nearest the view direction")
    {
      auto* turnedNode = createLight(map, "0 0 0");
      REQUIRE(mdl::setEntityProperty(map, "angles", "0 90 0"));
      REQUIRE(tool.selectedLight() == turnedNode);

      // the XZ view looks along world +Y, which is the turned box frame's +x: the
      // proportional axes leave out x, not y
      const auto frontRay = vm::ray3d{{0, -1000, 0}, {0, 1, 0}};
      const auto frontCamera = gl::OrthographicCamera{
        1.0f,
        8000.0f,
        gl::Camera::Viewport{0, 0, 1920, 1080},
        vm::vec3f{frontRay.origin},
        vm::vec3f{frontRay.direction},
        vm::vec3f{0, 0, 1}};

      auto inputState =
        inputStateFor(frontRay, frontCamera, {cornerHit(20)}, ModifierKeys::Shift);
      auto tracker = controller.acceptMouseDrag(inputState);
      REQUIRE(tracker != nullptr);
      CHECK(tool.proportionalAxes() == ProportionalAxes{false, true, true});
      tracker->cancel();
    }

    SECTION("draw mode takes the view axis extent of the selected light")
    {
      tool.setMode(LightVolumeTool::Mode::Draw);

      // off the grid, so that the first box is not flat
      const auto drawRay = vm::ray3d{{305, 7, 1000}, {0, 0, -1}};
      const auto drawCamera = orthographicCameraFor(drawRay);
      auto inputState = inputStateFor(drawRay, drawCamera, {});
      auto tracker = controller.acceptMouseDrag(inputState);
      REQUIRE(tracker != nullptr);
      REQUIRE(tool.drawBox());
      CHECK(tool.drawBox()->min.z() == -300.0);
      CHECK(tool.drawBox()->max.z() == 300.0);
      tracker->cancel();
    }

    SECTION("a drawn box replaces the selected light's volume")
    {
      map.grid().setSize(3); // 8 units
      tool.setMode(LightVolumeTool::Mode::Draw);

      const auto startRay = vm::ray3d{{305, 7, 1000}, {0, 0, -1}};
      const auto drawCamera = orthographicCameraFor(startRay);
      auto tracker = controller.acceptMouseDrag(inputStateFor(startRay, drawCamera, {}));
      REQUIRE(tracker != nullptr);

      const auto endRay = vm::ray3d{{330, 50, 1000}, {0, 0, -1}};
      const auto endInputState = inputStateFor(endRay, drawCamera, {});
      CHECK(tracker->update(endInputState));
      tracker->end(endInputState);

      // the snapped box [304, 336] x [0, 56] x [-300, 300], the light's own z extent
      CHECK(valueOf(*lightNode, "origin") == "320 28 0");
      CHECK(valueOf(*lightNode, "light_radius") == "16 28 300");
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Draw Light Volume");
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }

    SECTION("a drawn box with nothing selected creates a light")
    {
      map.grid().setSize(3); // 8 units
      tool.setMode(LightVolumeTool::Mode::Draw);
      mdl::deselectAll(map);
      const auto referenceZ = map.referenceBounds().center().z();

      const auto startRay = vm::ray3d{{305, 7, 1000}, {0, 0, -1}};
      const auto drawCamera = orthographicCameraFor(startRay);
      auto tracker = controller.acceptMouseDrag(inputStateFor(startRay, drawCamera, {}));
      REQUIRE(tracker != nullptr);

      const auto endRay = vm::ray3d{{330, 50, 1000}, {0, 0, -1}};
      const auto endInputState = inputStateFor(endRay, drawCamera, {});
      CHECK(tracker->update(endInputState));
      tracker->end(endInputState);

      // the z extent is +-300 around the reference bounds' centre
      REQUIRE(map.selection().entities.size() == 1);
      const auto* createdNode = map.selection().entities.front();
      CHECK(createdNode != lightNode);
      const auto origin = vm::parse<double, 3>(valueOf(*createdNode, "origin"));
      REQUIRE(origin);
      CHECK(vm::is_equal(*origin, vm::vec3d{320, 28, referenceZ}, 0.005));
      CHECK(valueOf(*createdNode, "light_radius") == "16 28 300");
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Create Light");
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }
  }
}

} // namespace tb::ui
