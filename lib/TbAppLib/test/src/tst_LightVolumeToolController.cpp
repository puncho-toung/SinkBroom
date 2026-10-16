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

#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h" // IWYU pragma: keep
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/LightTestUtils.h"
#include "ui/LightVolumeTool.h"
#include "ui/LightVolumeToolController.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MoveObjectsTool.h"
#include "ui/PickRequest.h"
#include "ui/ScaleTool.h"
#include "ui/ToolController.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/intersection.h"
#include "vm/plane.h"
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::ui
{
namespace
{

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

mdl::Hit faceCenterHit(const double distance)
{
  return mdl::Hit{
    LightVolumeTool::FaceCenterHitType,
    distance,
    vm::vec3d{distance, 0, 0},
    BBoxSide{vm::vec3d{1, 0, 0}}};
}

/**
 * A surface hit at the given distance for the precedence of findLightVolumeHit, which
 * reads only whether the light is selected.
 */
mdl::Hit surfaceHit(
  mdl::EntityNode* entityNode, const double distance, const bool selected)
{
  return mdl::Hit{
    LightVolumeTool::SurfaceHitType,
    distance,
    vm::vec3d{distance, 0, 0},
    LightSurfaceHit{
      entityNode, mdl::LightElement::OuterShell, selected, {}, {}, vm::vec3d{}}};
}

/**
 * The surface hit that the tool picks along the given ray.
 */
mdl::Hit pickSurface(const LightVolumeTool& tool, const vm::ray3d& pickRay)
{
  auto pickResult = mdl::PickResult{};
  tool.pickSurfaces(pickRay, pickResult);
  const auto& hit =
    pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType));
  REQUIRE(hit.isMatch());
  return hit;
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

  // the handles and surfaces work without the tool being activated (D56); the tool is
  // active only to draw
  auto tool = LightVolumeTool{document};
  auto moveTool = MoveObjectsTool{document};

  auto* lightNode = createLight(map, "0 0 0");
  REQUIRE(tool.selectedLight() == lightNode);

  const auto pickRay = vm::ray3d{{1000, 0, 0}, {-1, 0, 0}};
  const auto camera = perspectiveCameraFor(pickRay);

  // the default light's handle box is +-300 t (D61)
  const auto t = legacyVisibleT();
  const auto halfExtent = 300.0 * t;

  SECTION("3D controller")
  {
    auto controller3D = LightVolumeToolController3D{tool, moveTool, document};
    auto& controller = static_cast<ToolController&>(controller3D);

    SECTION("tool returns the always-on handles")
    {
      CHECK(&controller.tool() == static_cast<Tool*>(&tool.handles()));
      CHECK(controller.toolActive());
      CHECK_FALSE(tool.active());
    }

    SECTION("pick")
    {
      SECTION("adds the face centre in front and the surfaces")
      {
        auto inputState = inputStateFor(pickRay, camera, {});
        auto pickResult = mdl::PickResult{};
        controller.pick(inputState, pickResult);
        CHECK(pickResult.first(mdl::HitFilters::type(LightVolumeTool::FaceCenterHitType))
                .isMatch());
        CHECK(pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType))
                .isMatch());
      }

      SECTION("adds no surface while a tool drags")
      {
        auto inputState = inputStateFor(pickRay, camera, {});
        inputState.setAnyToolDragging(true);
        auto pickResult = mdl::PickResult{};
        controller.pick(inputState, pickResult);
        CHECK_FALSE(
          pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType))
            .isMatch());
      }

      SECTION("adds nothing in draw mode")
      {
        REQUIRE(tool.activate());
        tool.setMode(LightVolumeTool::Mode::Draw);
        auto inputState = inputStateFor(pickRay, camera, {});
        auto pickResult = mdl::PickResult{};
        controller.pick(inputState, pickResult);
        CHECK(pickResult.empty());
      }

      SECTION("adds nothing while the light volumes are off (AC-21)")
      {
        setPref(
          Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesOff});
        const auto resetVolumes = kdl::invoke_later{[] {
          PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
        }};
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

      SECTION("a face centre behind a nearer selected node is the tool's (D57)")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), faceCenterHit(20)});
        auto tracker = controller.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);
        CHECK(tool.dragHit().type() == LightVolumeTool::FaceCenterHitType);
        tracker->cancel();
      }

      SECTION("a surface behind a nearer selected node is the move tool's")
      {
        auto inputState = inputStateFor(
          pickRay, camera, {entityHit(lightNode, 10), surfaceHit(lightNode, 20, true)});
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

      SECTION("Shift and Alt change nothing on a handle (D65, D71, AC-25)")
      {
        map.grid().toggleSnap(); // for a delta that is not snapped

        const auto modifiers = GENERATE(values<ModifierKeyState>({
          ModifierKeys::None,
          ModifierKeys::Alt,
          ModifierKeys::Shift,
          ModifierKeys::Alt | ModifierKeys::Shift,
        }));
        CAPTURE(modifiers);

        // the +x face centre, dragged across its handle line by 32, with the modifiers
        // held from the start and pressed again during the drag
        const auto startRay = vm::ray3d{{halfExtent, -100, 0}, vm::vec3d{0, 1, 0}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::FaceCenterHitType,
            100.0,
            vm::vec3d{halfExtent, 0, 0},
            BBoxSide{vm::vec3d{1, 0, 0}}}},
          modifiers));
        REQUIRE(tracker != nullptr);
        CHECK(tool.isDragging());

        const auto dragRay = vm::ray3d{{halfExtent + 32, -100, 0}, vm::vec3d{0, 1, 0}};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {}, modifiers);
        tracker->modifierKeyChange(dragInputState);
        CHECK(tracker->update(dragInputState));
        tracker->modifierKeyChange(inputStateFor(dragRay, startCamera, {}));
        tracker->end(dragInputState);

        // the numbers of a plain drag: (300 t + 32) / t, the origin unchanged
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        CHECK(valueOf(*lightNode, "light_radius") == "339.79 300 300");
      }

      SECTION("a 3D edge follows the mouse on its two axes (D66, AC-23)")
      {
        map.grid().toggleSnap();

        const auto modifiers = GENERATE(values<ModifierKeyState>(
          {ModifierKeys::None,
           ModifierKeys::Shift,
           ModifierKeys::Alt,
           ModifierKeys::Alt | ModifierKeys::Shift}));
        CAPTURE(modifiers);

        // the +x +y edge along z, seen end-on from above: it is dragged in the plane
        // through the hit point that faces the camera, the horizontal one, by (32, 16)
        const auto startRay =
          vm::ray3d{{halfExtent, halfExtent, 1000}, vm::vec3d{0, 0, -1}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::EdgeHitType,
            1000.0,
            vm::vec3d{halfExtent, halfExtent, 0},
            BBoxEdge{vm::vec3d{1, 1, -1}, vm::vec3d{1, 1, 1}}}},
          modifiers));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{{halfExtent + 32, halfExtent + 16, 1000}, vm::vec3d{0, 0, -1}};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {}, modifiers);
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        checkVector(
          *lightNode, "light_radius", {300.0 + 32.0 / t, 300.0 + 16.0 / t, 300});
        CHECK(valueOf(*lightNode, "light_radius") == "339.79 319.9 300");
      }

      SECTION("a 3D corner scales about the origin, with or without modifiers (AC-25)")
      {
        map.grid().toggleSnap();

        const auto modifiers = GENERATE(values<ModifierKeyState>({
          ModifierKeys::None,
          ModifierKeys::Shift,
          ModifierKeys::Alt,
          ModifierKeys::Alt | ModifierKeys::Shift,
        }));
        CAPTURE(modifiers);

        // the corner (1, 1, 1), dragged by (16, 16, 16) along its line from the origin
        // with a ray across that line
        const auto corner = vm::vec3d{halfExtent, halfExtent, halfExtent};
        const auto across = vm::normalize(vm::vec3d{1, -1, 0});
        const auto startRay = vm::ray3d{corner - across * 500.0, across};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::CornerHitType,
            500.0,
            corner,
            BBoxCorner{vm::vec3d{1, 1, 1}}}},
          modifiers));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{startRay.origin + vm::vec3d{16, 16, 16}, startRay.direction};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {}, modifiers);
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "light_radius") == "319.9 319.9 319.9");
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      }

      SECTION("a 3D edge seen broadside stays under the mouse (D66, review round 4)")
      {
        map.grid().toggleSnap();

        // the -y +z edge along x, grabbed at its centre and seen from the front above:
        // the plane perpendicular to the edge would hold the view ray and put the edge at
        // the eye; the plane that faces the camera takes the mouse's move towards the
        // hit point + (4, 0, 4)
        const auto volume = tool.volumeBox();
        REQUIRE(volume);
        const auto hitPoint = volume->toWorld(
          {0, volume->box.min.y(), volume->box.max.z()}); // (0, -241.26, 241.26)
        const auto eye = vm::vec3d{0, -1000, 400};
        const auto startRay = vm::ray3d{eye, vm::normalize(hitPoint - eye)};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::EdgeHitType,
            vm::length(hitPoint - eye),
            hitPoint,
            BBoxEdge{vm::vec3d{-1, -1, 1}, vm::vec3d{1, -1, 1}}}}));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{eye, vm::normalize(hitPoint + vm::vec3d{4, 0, 4} - eye)};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        // the move in that plane: (4.00, 0.80, 3.84); the edge's two axes take its y and
        // z, the -y side by -0.80 and the +z side by 3.84, so y 300 - 0.80 / t = 299 and
        // z 300 + 3.84 / t = 304.77
        const auto plane =
          vm::plane3d{hitPoint, vm::vec3d{startCamera.direction()} * -1.0};
        const auto distance = vm::intersect_ray_plane(dragRay, plane);
        REQUIRE(distance);
        const auto delta = vm::point_at_distance(dragRay, *distance) - hitPoint;
        CHECK(delta.y() == Catch::Approx(0.8026).margin(1e-3));
        CHECK(delta.z() == Catch::Approx(3.8363).margin(1e-3));

        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        checkVector(
          *lightNode,
          "light_radius",
          {300, 300.0 - delta.y() / t, 300.0 + delta.z() / t});
        CHECK(valueOf(*lightNode, "light_radius") == "300 299 304.77");
      }

      SECTION("a 3D edge snaps its move relative to the grab (D66, review round 4)")
      {
        // the grid of 16 snaps the move (30, 10) of the +x +y edge seen from above to
        // (32, 16); without snapping it would stay (30, 10), and absolute snapping would
        // put the edge's centre (241.26 + 30, 241.26 + 10) on (272, 256), a move of
        // (30.74, 14.74)
        REQUIRE(map.grid().snap());
        REQUIRE(map.grid().actualSize() == 16.0);

        const auto startRay =
          vm::ray3d{{halfExtent, halfExtent, 1000}, vm::vec3d{0, 0, -1}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::EdgeHitType,
            1000.0,
            vm::vec3d{halfExtent, halfExtent, 0},
            BBoxEdge{vm::vec3d{1, 1, -1}, vm::vec3d{1, 1, 1}}}}));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{{halfExtent + 30, halfExtent + 10, 1000}, vm::vec3d{0, 0, -1}};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "light_radius") == "339.79 319.9 300");
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      }

      SECTION("a 3D edge grabbed off its centre moves by the mouse's move")
      {
        // the +x +y edge seen from above, grabbed at z = 100: the plane runs through the
        // hit point, and the edge moves by the mouse's (32, 16)
        const auto hitPoint = vm::vec3d{halfExtent, halfExtent, 100};
        const auto startRay = vm::ray3d{hitPoint + vm::vec3d{0, 0, 900}, {0, 0, -1}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::EdgeHitType,
            900.0,
            hitPoint,
            BBoxEdge{vm::vec3d{1, 1, -1}, vm::vec3d{1, 1, 1}}}}));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{startRay.origin + vm::vec3d{32, 16, 0}, startRay.direction};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "light_radius") == "339.79 319.9 300");
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      }

      SECTION("a spot light's far cap edge seen end-on (D66, AC-24)")
      {
        // the box frame's +x far cap edge, along world Y at world (-132, y, -264), seen
        // along +Y and moved by (-32, 0, -64): hx 164, the length 328
        auto* spotNode = createLight(
          map,
          "0 0 0",
          {{"light_target", "0 0 -256"},
           {"light_right", "-128 0 0"},
           {"light_up", "0 -128 0"}});
        REQUIRE(tool.selectedLight() == spotNode);

        const auto hitPoint = vm::vec3d{-132, 0, -264};
        const auto startRay = vm::ray3d{{-132, -1000, -264}, {0, 1, 0}};
        const auto startCamera = perspectiveCameraFor(startRay);
        auto tracker = controller.acceptMouseDrag(inputStateFor(
          startRay,
          startCamera,
          {mdl::Hit{
            LightVolumeTool::EdgeHitType,
            1000.0,
            hitPoint,
            BBoxEdge{vm::vec3d{1, -1, 1}, vm::vec3d{1, 1, 1}}}}));
        REQUIRE(tracker != nullptr);

        const auto dragRay =
          vm::ray3d{startRay.origin + vm::vec3d{-32, 0, -64}, startRay.direction};
        const auto dragInputState = inputStateFor(dragRay, startCamera, {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*spotNode, "light_target") == "0 0 -320");
        CHECK(valueOf(*spotNode, "light_right") == "-160 0 0");
        CHECK(valueOf(*spotNode, "light_up") == "0 -128.78 0");
        CHECK(valueOf(*spotNode, "origin") == "0 0 0");
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

        // the box is +-300 t and scales about the origin: the radius grows by 32 / t
        // (D61, D64)
        CHECK_FALSE(tool.isDragging());
        CHECK(lightNode->entity().property("origin") != nullptr);
        CHECK(*lightNode->entity().property("origin") == "0 0 0");
        checkVector(*lightNode, "light_radius", {300.0 + 32.0 / t, 300, 300});
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

        CHECK(valueOf(*turnedNode, "origin") == "0 0 0");
        checkVector(*turnedNode, "light_radius", {300.0 + 32.0 / t, 300, 300});
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

        // e.g. a key that turns on a modal tool, which suppresses the handles without
        // going through the tracker
        REQUIRE(tool.handles().deactivate());
        CHECK(lightNode->entity().properties() == properties);

        // the tracker lives on: its next update ends the drag
        const auto laterRay = vm::ray3d{{364, -100, 0}, vm::vec3d{0, 1, 0}};
        CHECK_FALSE(tracker->update(inputStateFor(laterRay, startCamera, {})));

        // a modifier change changes nothing (D71): the update after it ends it again
        tracker->modifierKeyChange(
          inputStateFor(laterRay, startCamera, {}, ModifierKeys::Shift));
        const auto lastRay = vm::ray3d{{396, -100, 0}, vm::vec3d{0, 1, 0}};
        CHECK_FALSE(tracker->update(inputStateFor(lastRay, startCamera, {})));
        tracker->end(dragInputState);

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("a plain drag on the selected light's shell moves it (D60, AC-20)")
      {
        // the shell's +x pole, 300 from the light
        const auto hit = pickSurface(tool, pickRay);
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
        REQUIRE(vm::is_equal(hit.hitPoint(), vm::vec3d{300, 0, 0}, 1e-6));

        auto tracker = controller.acceptMouseDrag(inputStateFor(pickRay, camera, {hit}));
        REQUIRE(tracker != nullptr);
        CHECK_FALSE(tool.isDragging());

        // the move tool's drag runs in the horizontal plane through the hit point: 30 on,
        // which the grid of 16 snaps to 32
        const auto dragRay = vm::ray3d{{330, 0, 1000}, vm::vec3d{0, 0, -1}};
        const auto dragInputState =
          inputStateFor(dragRay, perspectiveCameraFor(dragRay), {});
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "origin") == "32 0 0");
        CHECK(lightNode->entity().property("light_radius") == nullptr);

        // one undo step, the move tool's
        REQUIRE(map.undoCommandName() != nullptr);
        CHECK(*map.undoCommandName() == "Move Objects");
        map.undoCommand();
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      }

      SECTION("a plain drag on an unselected light's surface is not taken")
      {
        auto* otherNode = createLight(map, "2000 0 0");
        mdl::deselectAll(map);
        mdl::selectNodes(map, {lightNode});

        auto inputState =
          inputStateFor(pickRay, camera, {surfaceHit(otherNode, 700, false)});
        CHECK(controller.acceptMouseDrag(inputState) == nullptr);
        inputState = inputStateFor(
          pickRay, camera, {surfaceHit(otherNode, 700, false)}, ModifierKeys::Shift);
        CHECK(controller.acceptMouseDrag(inputState) == nullptr);
      }

      SECTION("an Alt drag on the selected light's shell moves it vertically")
      {
        const auto hit = pickSurface(tool, pickRay);
        auto tracker = controller.acceptMouseDrag(
          inputStateFor(pickRay, camera, {hit}, ModifierKeys::Alt));
        REQUIRE(tracker != nullptr);

        // across the vertical line through the hit point (300, 0, 0): 30 up, which the
        // grid of 16 snaps to 32
        const auto dragRay = vm::ray3d{{300, -1000, 30}, vm::vec3d{0, 1, 0}};
        const auto dragInputState =
          inputStateFor(dragRay, perspectiveCameraFor(dragRay), {}, ModifierKeys::Alt);
        CHECK(tracker->update(dragInputState));
        tracker->end(dragInputState);

        CHECK(valueOf(*lightNode, "origin") == "0 0 32");
      }

      SECTION("a plain drag on a surface is not taken while the move tool is off")
      {
        REQUIRE(moveTool.deactivate());
        const auto hit = pickSurface(tool, pickRay);
        CHECK(
          controller.acceptMouseDrag(inputStateFor(pickRay, camera, {hit})) == nullptr);
        REQUIRE(moveTool.activate());
      }

      SECTION("a Shift drag on a surface of one of two selected lights edits nothing")
      {
        auto* otherNode = createLight(map, "2000 0 0");
        mdl::selectNodes(map, {lightNode});
        REQUIRE(map.selection().nodes.size() == 2);
        const auto properties = lightNode->entity().properties();
        const auto otherProperties = otherNode->entity().properties();

        const auto hit = pickSurface(tool, pickRay);
        REQUIRE(hit.target<LightSurfaceHit>().node == lightNode);
        CHECK(
          controller.acceptMouseDrag(
            inputStateFor(pickRay, camera, {hit}, ModifierKeys::Shift))
          == nullptr);
        CHECK_FALSE(tool.isDragging());
        CHECK(lightNode->entity().properties() == properties);
        CHECK(otherNode->entity().properties() == otherProperties);
      }

      SECTION("Shift on a surface edits its element (D60, AC-20)")
      {
        map.grid().toggleSnap(); // for a delta that is not snapped

        SECTION("the outer shell moves its side, the opposite side mirrors it")
        {
          const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 100}, {-1, 0, 0}});
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);

          // rays across the line of the drag, along +x through the hit point
          const auto start = hit.hitPoint();
          const auto startRay = vm::ray3d{start + vm::vec3d{0, -100, 0}, {0, 1, 0}};
          const auto startCamera = perspectiveCameraFor(startRay);
          auto tracker = controller.acceptMouseDrag(
            inputStateFor(startRay, startCamera, {hit}, ModifierKeys::Shift));
          REQUIRE(tracker != nullptr);
          CHECK(tool.dragHit().type() == LightVolumeTool::SurfaceHitType);

          const auto dragRay =
            vm::ray3d{start + vm::vec3d{32, -100, 0}, vm::vec3d{0, 1, 0}};
          const auto dragInputState =
            inputStateFor(dragRay, startCamera, {}, ModifierKeys::Shift);
          CHECK(tracker->update(dragInputState));
          tracker->end(dragInputState);

          // the shell at the full radius follows the mouse about the origin (D64)
          CHECK(valueOf(*lightNode, "origin") == "0 0 0");
          CHECK(valueOf(*lightNode, "light_radius") == "332 300 300");
          REQUIRE(map.undoCommandName() != nullptr);
          CHECK(*map.undoCommandName() == "Edit Light Volume");
        }

        SECTION("the inner shell writes math_inner_radius")
        {
          REQUIRE(mdl::setEntityProperty(map, "math_enabled", "1"));
          REQUIRE(mdl::setEntityProperty(map, "math_falloff", "linear"));
          REQUIRE(mdl::setEntityProperty(map, "math_inner_radius", "0.1"));

          const auto hit = pickSurface(tool, pickRay);
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerShell);

          const auto startRay = vm::ray3d{{30, -100, 0}, {0, 1, 0}};
          const auto startCamera = perspectiveCameraFor(startRay);
          auto tracker = controller.acceptMouseDrag(
            inputStateFor(startRay, startCamera, {hit}, ModifierKeys::Shift));
          REQUIRE(tracker != nullptr);

          const auto dragInputState = inputStateFor(
            vm::ray3d{{60, -100, 0}, {0, 1, 0}}, startCamera, {}, ModifierKeys::Shift);
          CHECK(tracker->update(dragInputState));
          tracker->end(dragInputState);

          CHECK(valueOf(*lightNode, "math_inner_radius") == "0.2");
          CHECK(lightNode->entity().property("light_radius") == nullptr);
        }

        SECTION("the base moves the endpoint with the far cap's size unchanged")
        {
          auto* spotNode = createLight(
            map,
            "0 0 0",
            {{"light_target", "0 0 -256"},
             {"light_right", "-128 0 0"},
             {"light_up", "0 -128 0"}});
          const auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);

          const auto startRay = vm::ray3d{{0, -100, -264}, {0, 1, 0}};
          const auto startCamera = perspectiveCameraFor(startRay);
          auto tracker = controller.acceptMouseDrag(
            inputStateFor(startRay, startCamera, {hit}, ModifierKeys::Shift));
          REQUIRE(tracker != nullptr);

          const auto dragInputState = inputStateFor(
            vm::ray3d{{0, -100, -328}, {0, 1, 0}}, startCamera, {}, ModifierKeys::Shift);
          CHECK(tracker->update(dragInputState));
          tracker->end(dragInputState);

          // the far cap's centre's numbers (D27 (c))
          checkVector(*spotNode, "light_target", {0, 0, -320});
          checkVector(*spotNode, "light_right", {-132.0 * 320.0 / 328.0, 0, 0});
          checkVector(*spotNode, "light_up", {0, -132.0 * 320.0 / 328.0, 0});
          checkVector(*spotNode, "origin", {0, 0, 0});
        }

        SECTION("the funnel scales the base on one axis, the length unchanged (D67)")
        {
          auto* spotNode = createLight(
            map,
            "0 0 0",
            {{"light_target", "0 0 -256"},
             {"light_right", "-128 0 0"},
             {"light_up", "0 -128 0"}});

          // at depth 136 the funnel is 68 from the beam
          const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);

          const auto startRay = vm::ray3d{{68, -100, -136}, {0, 1, 0}};
          const auto startCamera = perspectiveCameraFor(startRay);
          auto tracker = controller.acceptMouseDrag(
            inputStateFor(startRay, startCamera, {hit}, ModifierKeys::Shift));
          REQUIRE(tracker != nullptr);

          // 34 on is a factor of 1.5 on the axis the hit point lies along, world X
          const auto dragInputState = inputStateFor(
            vm::ray3d{{102, -100, -136}, {0, 1, 0}},
            startCamera,
            {},
            ModifierKeys::Shift);
          CHECK(tracker->update(dragInputState));
          tracker->end(dragInputState);

          checkVector(*spotNode, "light_target", {0, 0, -256});
          checkVector(*spotNode, "light_right", {-192, 0, 0});
          checkVector(*spotNode, "light_up", {0, -128, 0});
          checkVector(*spotNode, "origin", {0, 0, 0});
        }
      }

      SECTION("draw mode")
      {
        REQUIRE(tool.activate());
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
        // and replaces the clicked surface, unless Ctrl leaves the click to the
        // selection tool (D68)
        const auto surface = pickSurface(tool, pickRay);
        tool.setClickedSurface(surface);

        CHECK_FALSE(controller.mouseClick(inputStateFor(
          pickRay,
          camera,
          {entityHit(lightNode, 10), cornerHit(20)},
          ModifierKeys::CtrlCmd)));
        CHECK(tool.isClickedSurface(surface));

        auto inputState =
          inputStateFor(pickRay, camera, {entityHit(lightNode, 10), cornerHit(20)});
        CHECK(controller.mouseClick(inputState));
        CHECK_FALSE(tool.isClickedSurface(surface));
      }

      SECTION("a click on nothing is left to the selection tool (D56)")
      {
        auto inputState = inputStateFor(pickRay, camera, {});
        CHECK_FALSE(controller.mouseClick(inputState));
      }

      SECTION("a surface click with Alt, or while the selection is blocked, is not taken")
      {
        auto* otherNode = createLight(map, "2000 0 0");
        mdl::deselectAll(map);

        CHECK_FALSE(controller.mouseClick(inputStateFor(
          pickRay, camera, {surfaceHit(otherNode, 700, false)}, ModifierKeys::Alt)));

        map.editorContext().setBlockSelection(true);
        CHECK_FALSE(map.editorContext().canChangeSelection());
        CHECK_FALSE(controller.mouseClick(
          inputStateFor(pickRay, camera, {surfaceHit(otherNode, 700, false)})));
        map.editorContext().setBlockSelection(false);

        CHECK(map.selection().nodes.empty());
      }

      SECTION("a click on a light in a closed group selects the group")
      {
        setPref(
          Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
        const auto resetVolumes = kdl::invoke_later{[] {
          PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
        }};

        auto* groupedNode = createLight(map, "2000 0 0");
        auto* groupNode = mdl::groupSelectedNodes(map, "lights");
        REQUIRE(groupNode != nullptr);
        REQUIRE_FALSE(groupNode->opened());
        mdl::deselectAll(map);

        const auto towardsGrouped = vm::ray3d{{3000, 0, 0}, {-1, 0, 0}};
        const auto groupedCamera = perspectiveCameraFor(towardsGrouped);
        auto hit = pickSurface(tool, towardsGrouped);
        REQUIRE(hit.target<LightSurfaceHit>().node == groupedNode);
        CHECK_FALSE(hit.target<LightSurfaceHit>().selected);

        CHECK(controller.mouseClick(inputStateFor(towardsGrouped, groupedCamera, {hit})));
        CHECK(map.selection().nodes == std::vector<mdl::Node*>{groupNode});

        // the light of the selected group is selected, and a plain drag moves the group
        hit = pickSurface(tool, towardsGrouped);
        CHECK(hit.target<LightSurfaceHit>().selected);
        auto tracker =
          controller.acceptMouseDrag(inputStateFor(towardsGrouped, groupedCamera, {hit}));
        CHECK(tracker != nullptr);
        if (tracker)
        {
          tracker->cancel();
        }

        // in a group that is not selected, the light is not
        mdl::deselectAll(map);
        CHECK_FALSE(pickSurface(tool, towardsGrouped).target<LightSurfaceHit>().selected);
      }

      SECTION("a click on nothing keeps the light selected while drawing")
      {
        REQUIRE(tool.activate());
        tool.setMode(LightVolumeTool::Mode::Draw);
        auto inputState = inputStateFor(pickRay, camera, {});
        CHECK(controller.mouseClick(inputState));
      }

      SECTION("a click on a surface selects its light, Ctrl toggles it (D60, AC-20)")
      {
        auto* otherNode = createLight(map, "2000 0 0");
        mdl::deselectAll(map);
        mdl::selectNodes(map, {lightNode});

        auto inputState =
          inputStateFor(pickRay, camera, {surfaceHit(otherNode, 700, false)});
        CHECK(controller.mouseClick(inputState));
        CHECK(map.selection().nodes == std::vector<mdl::Node*>{otherNode});
        CHECK(tool.isClickedSurface(surfaceHit(otherNode, 700, false)));

        inputState = inputStateFor(
          pickRay, camera, {surfaceHit(lightNode, 700, false)}, ModifierKeys::CtrlCmd);
        CHECK(controller.mouseClick(inputState));
        CHECK(map.selection().nodes.size() == 2);

        inputState = inputStateFor(
          pickRay, camera, {surfaceHit(lightNode, 700, true)}, ModifierKeys::CtrlCmd);
        CHECK(controller.mouseClick(inputState));
        CHECK(map.selection().nodes == std::vector<mdl::Node*>{otherNode});

        // Shift is the selection tool's face click
        inputState = inputStateFor(
          pickRay, camera, {surfaceHit(lightNode, 700, false)}, ModifierKeys::Shift);
        CHECK_FALSE(controller.mouseClick(inputState));
      }

      SECTION("a click on an unselected light's surface over a node is the node's")
      {
        auto* otherNode = createLight(map, "2000 0 0");
        auto* brushNode = createBrush(map);
        mdl::deselectAll(map);

        auto inputState = inputStateFor(
          pickRay, camera, {surfaceHit(otherNode, 700, false), brushHit(brushNode, 900)});
        CHECK_FALSE(controller.mouseClick(inputState));
        CHECK(map.selection().nodes.empty());
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

    SECTION("modifierKeyChange only refreshes the views (D71)")
    {
      const auto properties = lightNode->entity().properties();
      auto refreshes = 0;
      auto connection = tool.refreshViewsNotifier.connect([&](auto&) { ++refreshes; });

      controller.modifierKeyChange(
        inputStateFor(pickRay, camera, {cornerHit(20)}, ModifierKeys::Shift));
      CHECK(refreshes == 1);
      CHECK_FALSE(tool.isDragging());
      CHECK(lightNode->entity().properties() == properties);
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

      SECTION("highlights nothing over a nearer node")
      {
        auto inputState = inputStateFor(
          pickRay, camera, {entityHit(lightNode, 10), surfaceHit(lightNode, 20, true)});
        controller.mouseMove(inputState);
        CHECK_FALSE(tool.highlightedHit().isMatch());
      }

      SECTION("highlights a surface")
      {
        auto inputState =
          inputStateFor(pickRay, camera, {surfaceHit(lightNode, 20, true)});
        controller.mouseMove(inputState);
        CHECK(tool.highlightedHit().type() == LightVolumeTool::SurfaceHitType);
      }

      SECTION("keeps the clicked surface when the hover moves on (D68)")
      {
        const auto surface = pickSurface(tool, pickRay);
        tool.setClickedSurface(surface);

        controller.mouseMove(inputStateFor(pickRay, camera, {cornerHit(20)}));
        CHECK(tool.highlightedHit().type() == LightVolumeTool::CornerHitType);
        CHECK(tool.isClickedSurface(surface));

        controller.mouseMove(inputStateFor(pickRay, camera, {}));
        CHECK(tool.isClickedSurface(surface));
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
        REQUIRE(tool.activate());
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
        CHECK_FALSE(controller.cancel());
      }
    }
  }

  SECTION("findLightVolumeHit")
  {
    auto* brushNode = createBrush(map);
    auto* otherNode = createLight(map, "2000 0 0");
    mdl::deselectAll(map);
    mdl::selectNodes(map, {lightNode});

    const auto find = [](const std::vector<mdl::Hit>& hits) {
      auto pickResult = mdl::PickResult::byDistance();
      for (const auto& hit : hits)
      {
        pickResult.addHit(hit);
      }
      return findLightVolumeHit(pickResult);
    };

    // D57: handles first, even behind anything
    CHECK(
      find({brushHit(brushNode, 10), surfaceHit(lightNode, 15, true), cornerHit(20)})
        .type()
      == LightVolumeTool::CornerHitType);

    // a brush hit nearer than an unselected light's shell wins
    CHECK_FALSE(
      find({brushHit(brushNode, 10), surfaceHit(otherNode, 20, false)}).isMatch());

    // an unselected light's shell yields to a node behind it too, so that the brushes
    // seen through a volume stay clickable
    CHECK_FALSE(
      find({surfaceHit(otherNode, 10, false), brushHit(brushNode, 20)}).isMatch());

    // with no node along the ray it is picked
    CHECK(
      find({surfaceHit(otherNode, 10, false)}).type() == LightVolumeTool::SurfaceHitType);

    // a brush hit farther than the selected light's shell loses
    const auto& nearSelected =
      find({surfaceHit(lightNode, 10, true), brushHit(brushNode, 20)});
    REQUIRE(nearSelected.type() == LightVolumeTool::SurfaceHitType);
    CHECK(nearSelected.target<LightSurfaceHit>().node == lightNode);

    // a brush hit nearer than the selected light's shell wins
    CHECK_FALSE(
      find({brushHit(brushNode, 10), surfaceHit(lightNode, 20, true)}).isMatch());

    // past an unselected light's shell, the selected light's shell before the brush
    const auto& throughOther = find(
      {surfaceHit(otherNode, 10, false),
       surfaceHit(lightNode, 20, true),
       brushHit(brushNode, 30)});
    REQUIRE(throughOther.type() == LightVolumeTool::SurfaceHitType);
    CHECK(throughOther.target<LightSurfaceHit>().node == lightNode);
  }

  SECTION("2D controller")
  {
    auto controller2D = LightVolumeToolController2D{tool, moveTool, document};
    auto& controller = static_cast<ToolController&>(controller2D);

    const auto topRay = vm::ray3d{{halfExtent, 0, 1000}, {0, 0, -1}};
    const auto topCamera = orthographicCameraFor(topRay);

    SECTION("pick adds the outline as a side, and no surface (D57, D59)")
    {
      auto inputState = inputStateFor(topRay, topCamera, {});
      auto pickResult = mdl::PickResult{};
      controller.pick(inputState, pickResult);
      const auto& hit =
        pickResult.first(mdl::HitFilters::type(LightVolumeTool::SideHitType));
      REQUIRE(hit.isMatch());
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK_FALSE(pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType))
                    .isMatch());
    }

    SECTION(
      "a drag of the outline changes one axis, in the top and the side view (AC-19)")
    {
      map.grid().toggleSnap(); // for a delta that is not snapped
      REQUIRE(mdl::setEntityProperty(map, "light_radius", "300 200 100"));
      const auto halfExtents = vm::vec3d{300, 200, 100} * t;

      const auto dragOutline =
        [&](const vm::ray3d& startRay, const vm::vec3d& delta, const vm::vec3f& up) {
          const auto viewCamera = gl::OrthographicCamera{
            1.0f,
            8000.0f,
            gl::Camera::Viewport{0, 0, 1920, 1080},
            vm::vec3f{startRay.origin},
            vm::vec3f{startRay.direction},
            up};
          auto pickResult = mdl::PickResult{};
          controller.pick(inputStateFor(startRay, viewCamera, {}), pickResult);
          const auto& hit =
            pickResult.first(mdl::HitFilters::type(LightVolumeTool::SideHitType));
          REQUIRE(hit.isMatch());

          auto tracker =
            controller.acceptMouseDrag(inputStateFor(startRay, viewCamera, {hit}));
          REQUIRE(tracker != nullptr);
          const auto dragInputState = inputStateFor(
            vm::ray3d{startRay.origin + delta, startRay.direction}, viewCamera, {});
          CHECK(tracker->update(dragInputState));
          tracker->end(dragInputState);
        };

      // the opposite outline mirrors the dragged one, the origin stays (D64, AC-23)
      SECTION("the top view, the +x outline")
      {
        dragOutline(
          vm::ray3d{{halfExtents.x(), 50, 1000}, {0, 0, -1}}, {32, 0, 0}, {0, 1, 0});
        checkVector(*lightNode, "light_radius", {300.0 + 32.0 / t, 200, 100});
        CHECK(valueOf(*lightNode, "light_radius") == "339.79 200 100");
        checkVector(*lightNode, "origin", {0, 0, 0});
      }

      SECTION("the front view, the +z outline")
      {
        dragOutline(
          vm::ray3d{{50, -1000, halfExtents.z()}, {0, 1, 0}}, {0, 0, 32}, {0, 0, 1});
        checkVector(*lightNode, "light_radius", {300, 200, 100.0 + 32.0 / t});
        checkVector(*lightNode, "origin", {0, 0, 0});
      }

      SECTION("the side view, the -y outline")
      {
        dragOutline(
          vm::ray3d{{-1000, -halfExtents.y(), 20}, {1, 0, 0}}, {0, -32, 0}, {0, 0, 1});
        checkVector(*lightNode, "light_radius", {300, 200.0 + 32.0 / t, 100});
        checkVector(*lightNode, "origin", {0, 0, 0});
      }
    }

    SECTION("a corner scales the two visible axes by one factor (D59, AC-19)")
    {
      map.grid().toggleSnap();
      REQUIRE(mdl::setEntityProperty(map, "light_radius", "100 200 300"));
      const auto halfExtents = vm::vec3d{100, 200, 300} * t;

      const auto startRay =
        vm::ray3d{{halfExtents.x(), halfExtents.y(), 1000}, {0, 0, -1}};
      const auto viewCamera = orthographicCameraFor(startRay);
      auto pickResult = mdl::PickResult{};
      controller.pick(inputStateFor(startRay, viewCamera, {}), pickResult);
      const auto& hit =
        pickResult.first(mdl::HitFilters::type(LightVolumeTool::CornerHitType));
      REQUIRE(hit.isMatch());

      auto tracker =
        controller.acceptMouseDrag(inputStateFor(startRay, viewCamera, {hit}));
      REQUIRE(tracker != nullptr);

      // the mouse moves along x only; the corner follows the diagonal in the view plane
      const auto dragInputState = inputStateFor(
        vm::ray3d{startRay.origin + vm::vec3d{40, 0, 0}, startRay.direction},
        viewCamera,
        {});
      CHECK(tracker->update(dragInputState));
      tracker->end(dragInputState);

      const auto radius = vectorOf(*lightNode, "light_radius");
      CHECK(radius.x() > 100.0);
      CHECK(radius.y() / radius.x() == Catch::Approx(2.0).margin(1e-3));
      CHECK(radius.z() == Catch::Approx(300.0).margin(0.005));
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    }

    SECTION("Shift changes nothing in 2D either (D65, AC-25)")
    {
      map.grid().toggleSnap();

      const auto modifiers = GENERATE(values<ModifierKeyState>(
        {ModifierKeys::None,
         ModifierKeys::Shift,
         ModifierKeys::Alt,
         ModifierKeys::Alt | ModifierKeys::Shift}));
      CAPTURE(modifiers);

      // the top view's +x outline by 32: one axis, not the view plane's two
      auto pickResult = mdl::PickResult{};
      controller.pick(inputStateFor(topRay, topCamera, {}), pickResult);
      const auto& hit =
        pickResult.first(mdl::HitFilters::type(LightVolumeTool::SideHitType));
      REQUIRE(hit.isMatch());

      auto tracker =
        controller.acceptMouseDrag(inputStateFor(topRay, topCamera, {hit}, modifiers));
      REQUIRE(tracker != nullptr);
      const auto dragInputState = inputStateFor(
        vm::ray3d{topRay.origin + vm::vec3d{32, 0, 0}, topRay.direction},
        topCamera,
        {},
        modifiers);
      CHECK(tracker->update(dragInputState));
      tracker->end(dragInputState);

      CHECK(valueOf(*lightNode, "light_radius") == "339.79 300 300");
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    }

    SECTION("Shift on a turned light's corner changes nothing (D65, AC-25)")
    {
      map.grid().toggleSnap();

      const auto modifiers = GENERATE(values<ModifierKeyState>(
        {ModifierKeys::None,
         ModifierKeys::Shift,
         ModifierKeys::Alt,
         ModifierKeys::Alt | ModifierKeys::Shift}));
      CAPTURE(modifiers);

      auto* turnedNode = createLight(map, "0 0 0");
      REQUIRE(mdl::setEntityProperty(map, "angles", "0 90 0"));
      REQUIRE(tool.selectedLight() == turnedNode);

      // the XZ view looks along world +Y, which is the turned box frame's +x: the corner
      // (1, 1, 1) at world (-300 t, 300 t, 300 t) moves along world (-1, 0, 1), its
      // line from the origin without the view axis, which scales the box's y and z
      const auto frontRay = vm::ray3d{{-halfExtent, -1000, halfExtent}, {0, 1, 0}};
      const auto frontCamera = gl::OrthographicCamera{
        1.0f,
        8000.0f,
        gl::Camera::Viewport{0, 0, 1920, 1080},
        vm::vec3f{frontRay.origin},
        vm::vec3f{frontRay.direction},
        vm::vec3f{0, 0, 1}};

      auto tracker = controller.acceptMouseDrag(inputStateFor(
        frontRay,
        frontCamera,
        {mdl::Hit{
          LightVolumeTool::CornerHitType,
          1000.0,
          vm::vec3d{-halfExtent, 0, halfExtent},
          BBoxCorner{vm::vec3d{1, 1, 1}}}},
        modifiers));
      REQUIRE(tracker != nullptr);
      const auto dragInputState = inputStateFor(
        vm::ray3d{frontRay.origin + vm::vec3d{-16, 0, 16}, frontRay.direction},
        frontCamera,
        {},
        modifiers);
      CHECK(tracker->update(dragInputState));
      tracker->end(dragInputState);

      checkVector(*turnedNode, "light_radius", {300, 300.0 + 16.0 / t, 300.0 + 16.0 / t});
      CHECK(valueOf(*turnedNode, "origin") == "0 0 0");
    }

    SECTION("draw mode takes the view axis extent of the selected light's handle box")
    {
      REQUIRE(tool.activate());
      tool.setMode(LightVolumeTool::Mode::Draw);

      // off the grid, so that the first box is not flat
      const auto drawRay = vm::ray3d{{305, 7, 1000}, {0, 0, -1}};
      const auto drawCamera = orthographicCameraFor(drawRay);
      auto inputState = inputStateFor(drawRay, drawCamera, {});
      auto tracker = controller.acceptMouseDrag(inputState);
      REQUIRE(tracker != nullptr);
      REQUIRE(tool.drawBox());
      CHECK(tool.drawBox()->min.z() == Catch::Approx(-halfExtent).margin(1e-6));
      CHECK(tool.drawBox()->max.z() == Catch::Approx(halfExtent).margin(1e-6));
      tracker->cancel();
    }

    SECTION("a drawn box replaces the selected light's volume")
    {
      map.grid().setSize(3); // 8 units
      REQUIRE(tool.activate());
      tool.setMode(LightVolumeTool::Mode::Draw);

      const auto startRay = vm::ray3d{{305, 7, 1000}, {0, 0, -1}};
      const auto drawCamera = orthographicCameraFor(startRay);
      auto tracker = controller.acceptMouseDrag(inputStateFor(startRay, drawCamera, {}));
      REQUIRE(tracker != nullptr);

      const auto endRay = vm::ray3d{{330, 50, 1000}, {0, 0, -1}};
      const auto endInputState = inputStateFor(endRay, drawCamera, {});
      CHECK(tracker->update(endInputState));
      tracker->end(endInputState);

      // the snapped box [304, 336] x [0, 56] x [-300 t, 300 t], the light's own z
      // extent; the light is visible to the box, its radius the half extents over t
      CHECK(valueOf(*lightNode, "origin") == "320 28 0");
      checkVector(*lightNode, "light_radius", {16.0 / t, 28.0 / t, 300});
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Draw Light Volume");
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }

    SECTION("a drawn box with nothing selected creates a light")
    {
      map.grid().setSize(3); // 8 units
      REQUIRE(tool.activate());
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

      // the z extent is +-300 t around the reference bounds' centre, 273 for the seeded
      // light, so that the new light's radius along the view axis is 300
      REQUIRE(map.selection().entities.size() == 1);
      const auto* createdNode = map.selection().entities.front();
      CHECK(createdNode != lightNode);
      const auto origin = vm::parse<double, 3>(valueOf(*createdNode, "origin"));
      REQUIRE(origin);
      CHECK(vm::is_equal(*origin, vm::vec3d{320, 28, referenceZ}, 0.005));
      checkVector(
        *createdNode,
        "light_radius",
        {16.0 / SeededVisibleT, 28.0 / SeededVisibleT, 300.0});
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Create Light");
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }
  }
}

} // namespace tb::ui
