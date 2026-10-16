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
#include "mdl/Hit.h"
#include "mdl/LayerNode.h" // IWYU pragma: keep
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/ScaleTool.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

// The tool writes vectors with two decimals (light editor plan, D6), so a value read back
// is within half of the last decimal of the exact one; lengths of such vectors are
// within twice that.
constexpr auto ComponentTolerance = 0.005;
constexpr auto LengthTolerance = 0.01;

using Properties = std::vector<std::pair<std::string, std::string>>;

const auto DefaultSpot = Properties{
  {"light_target", "0 0 -256"},
  {"light_right", "-128 0 0"},
  {"light_up", "0 -128 0"},
};

mdl::EntityNode* createLight(
  mdl::Map& map, const std::string& origin, const Properties& properties)
{
  const auto* definition = map.entityDefinitionManager().definition("light");
  REQUIRE(definition != nullptr);

  const auto position = vm::parse<double, 3>(origin);
  REQUIRE(position);

  // the new light is selected, and the properties are set on the selection; a light
  // created at 0 0 0 has no origin property until it is set
  auto* entityNode = mdl::createPointEntity(map, *definition, *position);
  REQUIRE(entityNode != nullptr);
  REQUIRE(mdl::setEntityProperty(map, "origin", origin));
  for (const auto& [key, value] : properties)
  {
    REQUIRE(mdl::setEntityProperty(map, key, value));
  }
  return entityNode;
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

void selectOnly(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
}

std::string valueOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  REQUIRE(value != nullptr);
  return *value;
}

vm::vec3d vectorOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto value = vm::parse<double, 3>(valueOf(entityNode, key));
  REQUIRE(value);
  return *value;
}

void checkVector(
  const mdl::EntityNode& entityNode, const std::string& key, const vm::vec3d& expected)
{
  const auto actual = vectorOf(entityNode, key);
  CAPTURE(key, actual, expected);
  CHECK(vm::is_equal(actual, expected, ComponentTolerance));
}

double lengthOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  return vm::length(vectorOf(entityNode, key));
}

mdl::Hit sideHit(const vm::vec3d& normal)
{
  return mdl::Hit{
    LightVolumeTool::SideHitType, 0.0, vm::vec3d{0, 0, 0}, BBoxSide{normal}};
}

mdl::Hit edgeHit(const vm::vec3d& point0, const vm::vec3d& point1)
{
  return mdl::Hit{
    LightVolumeTool::EdgeHitType, 0.0, vm::vec3d{0, 0, 0}, BBoxEdge{point0, point1}};
}

mdl::Hit cornerHit(const vm::vec3d& corner)
{
  return mdl::Hit{
    LightVolumeTool::CornerHitType, 0.0, vm::vec3d{0, 0, 0}, BBoxCorner{corner}};
}

mdl::Hit farCapCenterHit()
{
  return mdl::Hit{
    LightVolumeTool::FarCapCenterHitType,
    0.0,
    vm::vec3d{0, 0, 0},
    BBoxSide{vm::vec3d{0, 0, 1}}};
}

/**
 * Drags the given handle by the given world deltas, one update each, and ends the drag.
 * Returns the results of the updates.
 */
std::vector<bool> drag(
  LightVolumeTool& tool, const mdl::Hit& hit, const std::vector<vm::vec3d>& deltas)
{
  REQUIRE(tool.beginDrag(hit));
  CHECK(tool.isDragging());

  auto result = std::vector<bool>{};
  for (const auto& delta : deltas)
  {
    result.push_back(tool.update(delta));
  }

  tool.endDrag();
  CHECK_FALSE(tool.isDragging());
  return result;
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

mdl::Hit pick3D(const LightVolumeTool& tool, const vm::ray3d& pickRay)
{
  const auto camera = perspectiveCameraFor(pickRay);
  auto pickResult = mdl::PickResult{};
  tool.pick3D(pickRay, camera, pickResult);
  REQUIRE(pickResult.size() <= 1);
  return pickResult.empty() ? mdl::Hit::NoHit : pickResult.all().front();
}

mdl::Hit pick2D(const LightVolumeTool& tool, const vm::ray3d& pickRay)
{
  const auto camera = orthographicCameraFor(pickRay);
  auto pickResult = mdl::PickResult{};
  tool.pick2D(pickRay, camera, pickResult);
  REQUIRE(pickResult.size() <= 1);
  return pickResult.empty() ? mdl::Hit::NoHit : pickResult.all().front();
}

} // namespace

TEST_CASE("LightVolumeTool")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(mdl::Doom3BFGFixtureConfig);
  auto& map = document.map();

  auto tool = LightVolumeTool{document};

  SECTION("selectedLight")
  {
    auto* pointNode = createLight(map, "0 0 0", {});
    auto* spotNode = createLight(map, "512 0 0", DefaultSpot);
    auto* brushNode = createBrush(map);

    SECTION("no selection")
    {
      mdl::deselectAll(map);
      CHECK(tool.selectedLight() == nullptr);
    }

    SECTION("one light")
    {
      selectOnly(map, {spotNode});
      CHECK(tool.selectedLight() == spotNode);
    }

    SECTION("two lights")
    {
      selectOnly(map, {pointNode, spotNode});
      CHECK(tool.selectedLight() == nullptr);
    }

    SECTION("a brush")
    {
      selectOnly(map, {brushNode});
      CHECK(tool.selectedLight() == nullptr);
    }

    SECTION("a light and a brush")
    {
      selectOnly(map, {pointNode, brushNode});
      CHECK(tool.selectedLight() == nullptr);
    }

    SECTION("is read from the selection every time")
    {
      selectOnly(map, {pointNode});
      CHECK(tool.selectedLight() == pointNode);
      selectOnly(map, {spotNode});
      CHECK(tool.selectedLight() == spotNode);
    }
  }

  SECTION("selectedLight needs a game with the light editor")
  {
    auto quakeFixture = MapDocumentFixture{};
    auto& quakeDocument = quakeFixture.create(mdl::QuakeFixtureConfig);
    auto& quakeMap = quakeDocument.map();
    REQUIRE(!mdl::hasLightEditor(quakeMap.gameInfo().gameConfig));

    auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
    REQUIRE(
      !mdl::addNodes(quakeMap, {{&mdl::parentForNodes(quakeMap), {entityNode}}}).empty());
    selectOnly(quakeMap, {entityNode});
    REQUIRE(mdl::isLightEntity(entityNode->entity()));

    auto quakeTool = LightVolumeTool{quakeDocument};
    CHECK(quakeTool.selectedLight() == nullptr);
    CHECK(quakeTool.volumeBox() == std::nullopt);
  }

  SECTION("volumeBox")
  {
    SECTION("a point light's box is its radius in its frame")
    {
      createLight(map, "16 32 48", {{"light_radius", "100 200 300"}});
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      CHECK(volume->box == vm::bbox3d{{-100, -200, -300}, {100, 200, 300}});
      CHECK(volume->frame.origin == vm::vec3d{16, 32, 48});
      CHECK(volume->rotation == vm::mat3x3d::identity());
    }

    SECTION("a spot light's box is in the beam frame")
    {
      createLight(map, "0 0 0", DefaultSpot);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      CHECK(volume->box == vm::bbox3d{{-132, -132, 0}, {132, 132, 264}});
      CHECK(
        vm::is_equal(volume->rotation * vm::vec3d{1, 0, 0}, vm::vec3d{-1, 0, 0}, 1e-9));
      CHECK(
        vm::is_equal(volume->rotation * vm::vec3d{0, 1, 0}, vm::vec3d{0, 1, 0}, 1e-9));
      CHECK(
        vm::is_equal(volume->rotation * vm::vec3d{0, 0, 1}, vm::vec3d{0, 0, -1}, 1e-9));
      CHECK(vm::is_equal(volume->toWorld({0, 0, 264}), vm::vec3d{0, 0, -264}, 1e-9));
    }

    SECTION("a rotated light's box frame turns with it")
    {
      createLight(map, "0 0 0", {{"light_radius", "100 200 300"}, {"angles", "0 90 0"}});
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      CHECK(
        vm::is_equal(volume->rotation * vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}, 1e-9));
      CHECK(volume->rotation == volume->frame.R);
    }

    SECTION("a light with light_origin is read only")
    {
      createLight(
        map, "0 0 0", {{"light_origin", "10 20 30"}, {"light_radius", "64 64 64"}});
      CHECK(tool.selectedLight() != nullptr);
      CHECK(tool.volumeBox() == std::nullopt);
      CHECK(tool.editBox() == std::nullopt);
      CHECK(tool.frame() == std::nullopt);
    }
  }

  SECTION("editBox")
  {
    CHECK(tool.editBox() == std::nullopt);

    createLight(map, "0 0 0", {{"light_start", "0 0 -32"}, {"light_end", "0 0 -200"}});
    REQUIRE(mdl::setEntityProperty(map, "light_target", "0 0 -256"));
    REQUIRE(mdl::setEntityProperty(map, "light_right", "-128 0 0"));
    REQUIRE(mdl::setEntityProperty(map, "light_up", "0 -128 0"));
    CHECK(tool.editBox() == vm::bbox3d{{-116, -116, 0}, {116, 116, 232}});
  }

  SECTION("frame")
  {
    CHECK(tool.frame() == std::nullopt);

    createLight(map, "16 32 48", DefaultSpot);
    const auto frame = tool.frame();
    REQUIRE(frame);
    CHECK(frame->origin == vm::vec3d{16, 32, 48});
    CHECK(frame->R == vm::mat3x3d::identity());
  }

  SECTION("update")
  {
    SECTION("a point light's side moves the side and the centre follows")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "16 0 0");
      CHECK(valueOf(*lightNode, "light_radius") == "316 300 300");
    }

    SECTION("a corner with the centre anchor changes the radius only")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      tool.setAnchorPos(AnchorPos::Center);
      CHECK(drag(tool, cornerHit({1, 1, 1}), {{16, 16, 16}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      CHECK(valueOf(*lightNode, "light_radius") == "316 316 316");
    }

    SECTION("a side with proportional axes scales the other axes")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      tool.setProportionalAxes(ProportionalAxes::All());
      CHECK(drag(tool, sideHit({1, 0, 0}), {{60, 0, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "30 0 0");
      CHECK(valueOf(*lightNode, "light_radius") == "330 330 330");
    }

    SECTION("a spot light's far cap edge widens the far rectangle")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

      SECTION("the box frame's +x edge, world -X")
      {
        // in the box frame, the edge at x = +hx, z = zNear + zFar
        CHECK(
          drag(tool, edgeHit({1, -1, 1}, {1, 1, 1}), {{-32, 0, 0}}) == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        checkVector(*lightNode, "light_target", {-15.516, 0, -256.015});
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(143.5234).margin(LengthTolerance));
        CHECK(
          lengthOf(*lightNode, "light_up")
          == Catch::Approx(128.0073).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_target") == "-15.52 0 -256.01");
        CHECK(valueOf(*lightNode, "light_right") == "-143.26 0 8.68");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128.01 0");
      }

      SECTION("the world +X edge")
      {
        CHECK(
          drag(tool, edgeHit({-1, -1, 1}, {-1, 1, 1}), {{32, 0, 0}})
          == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        checkVector(*lightNode, "light_target", {15.516, 0, -256.015});
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(143.5234).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_target") == "15.52 0 -256.01");
      }
    }

    SECTION("a spot light's far cap centre moves along the beam")
    {
      SECTION("without start and end")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
        CHECK(drag(tool, farCapCenterHit(), {{0, 0, -64}}) == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        checkVector(*lightNode, "light_target", {0, 0, -320});
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(128.7805).margin(LengthTolerance));
        CHECK(
          lengthOf(*lightNode, "light_up")
          == Catch::Approx(128.7805).margin(LengthTolerance));
      }

      SECTION("with start and end")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
        REQUIRE(mdl::setEntityProperty(map, "light_start", "0 0 -32"));
        REQUIRE(mdl::setEntityProperty(map, "light_end", "0 0 -200"));
        CHECK(drag(tool, farCapCenterHit(), {{0, 0, -64}}) == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        checkVector(*lightNode, "light_end", {0, 0, -264});
        checkVector(*lightNode, "light_start", {0, 0, -32});
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(103.4595).margin(LengthTolerance));
      }
    }

    SECTION("a spot light's apex face moves the apex along the beam")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      CHECK(drag(tool, sideHit({0, 0, -1}), {{0, 0, -16}}) == std::vector{true});

      checkVector(*lightNode, "origin", {0, 0, -16});
      checkVector(*lightNode, "light_target", {0, 0, -240});
      CHECK(
        lengthOf(*lightNode, "light_right")
        == Catch::Approx(127.7419).margin(LengthTolerance));
      CHECK(
        lengthOf(*lightNode, "light_up")
        == Catch::Approx(127.7419).margin(LengthTolerance));
    }

    SECTION("a denied box leaves the light and the undo stack as they are")
    {
      SECTION("a spot box shallower than 24")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();

        // the far cap at depth 23
        CHECK(drag(tool, farCapCenterHit(), {{0, 0, 241}}) == std::vector{false});

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("a half extent below 1")
      {
        auto* lightNode = createLight(map, "0 0 0", {});
        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();

        // the box is 1 wide
        CHECK(drag(tool, sideHit({1, 0, 0}), {{-599, 0, 0}}) == std::vector{false});

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("a denied step keeps the last accepted box")
      {
        auto* lightNode = createLight(map, "0 0 0", {});

        CHECK(
          drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}, {-1000, 0, 0}, {32, 0, 0}})
          == std::vector{true, false, true});

        // the denied step is not accumulated
        CHECK(valueOf(*lightNode, "origin") == "32 0 0");
        CHECK(valueOf(*lightNode, "light_radius") == "332 300 300");
      }
    }

    SECTION("a drag back to its start is no edit")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      CHECK(
        drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}, {-32, 0, 0}})
        == std::vector{true, true});

      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
    }

    SECTION("a drag is one undo step that restores every property")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      const auto properties = lightNode->entity().properties();

      CHECK(
        drag(tool, edgeHit({1, -1, 1}, {1, 1, 1}), {{-16, 0, 0}, {-16, 0, 0}})
        == std::vector{true, true});
      REQUIRE(lightNode->entity().properties() != properties);
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Edit Light Volume");

      map.undoCommand();
      CHECK(lightNode->entity().properties() == properties);
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() != "Edit Light Volume");
    }

    SECTION("cancelDrag restores the light")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      REQUIRE(tool.beginDrag(sideHit({1, 0, 0})));
      REQUIRE(tool.update({32, 0, 0}));
      REQUIRE(lightNode->entity().properties() != properties);
      tool.cancelDrag();

      CHECK_FALSE(tool.isDragging());
      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
    }

    SECTION("a drag needs a selected light")
    {
      createLight(map, "0 0 0", {});
      mdl::deselectAll(map);
      CHECK_FALSE(tool.beginDrag(sideHit({1, 0, 0})));
      CHECK_FALSE(tool.isDragging());
    }
  }

  SECTION("pick3D")
  {
    auto* lightNode = createLight(map, "0 0 0", {});

    SECTION("from inside the box, a corner in front of the camera")
    {
      const auto hit =
        pick3D(tool, vm::ray3d{{0, 0, 0}, vm::normalize(vm::vec3d{1, 1, 1})});
      CHECK(hit.type() == LightVolumeTool::CornerHitType);
      CHECK(hit.target<BBoxCorner>() == BBoxCorner{{1, 1, 1}});
    }

    SECTION("from inside the box, the side that the ray leaves by")
    {
      const auto hit = pick3D(tool, vm::ray3d{{0, 0, 0}, {1, 0, 0}});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(hit.distance() == Catch::Approx(300.0));
    }

    SECTION("from outside the box, the side in front")
    {
      const auto hit = pick3D(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(hit.distance() == Catch::Approx(700.0));
    }

    SECTION("from outside the box, a ray past it hits a back side")
    {
      const auto hit = pick3D(tool, vm::ray3d{{1000, 0, 500}, {-1, 0, 0}});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
    }

    SECTION("a spot light's far cap centre")
    {
      selectOnly(map, {});
      createLight(map, "0 0 0", DefaultSpot);
      const auto hit = pick3D(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      CHECK(hit.type() == LightVolumeTool::FarCapCenterHitType);
    }

    SECTION("nothing without a selected light")
    {
      mdl::deselectNodes(map, {lightNode});
      CHECK_FALSE(pick3D(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).isMatch());
    }
  }

  SECTION("pick2D")
  {
    createLight(map, "0 0 0", {});

    SECTION("an edge that is not parallel to the view direction")
    {
      const auto hit = pick2D(tool, vm::ray3d{{300, 0, 1000}, {0, 0, -1}});
      CHECK(hit.type() == LightVolumeTool::EdgeHitType);
    }

    SECTION("a corner")
    {
      const auto hit = pick2D(tool, vm::ray3d{{300, 300, 1000}, {0, 0, -1}});
      CHECK(hit.type() == LightVolumeTool::CornerHitType);
    }

    SECTION("a side seen edge on, never the side facing the camera")
    {
      const auto hit = pick2D(tool, vm::ray3d{{600, 0, 1000}, {0, 0, -1}});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
    }
  }

  SECTION("setMode")
  {
    auto notifications = 0;
    auto connection = tool.modeDidChangeNotifier.connect([&]() { ++notifications; });

    CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    tool.setMode(LightVolumeTool::Mode::Draw);
    CHECK(tool.mode() == LightVolumeTool::Mode::Draw);
    CHECK(notifications == 1);
    tool.setMode(LightVolumeTool::Mode::Draw);
    CHECK(notifications == 1);
  }

  SECTION("updateDraw")
  {
    tool.setMode(LightVolumeTool::Mode::Draw);

    SECTION("a selected spot light fills the box with its beam pointing down")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      REQUIRE(mdl::setEntityProperty(map, "angles", "0 90 0"));

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      tool.endDraw();

      CHECK(valueOf(*lightNode, "origin") == "64 64 64");
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -56");
      CHECK(valueOf(*lightNode, "light_right") == "-56 0 0");
      CHECK(valueOf(*lightNode, "light_up") == "0 -56 0");
      CHECK(lightNode->entity().property("angles") == nullptr);
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Draw Light Volume");
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }

    SECTION("a spot light's height is at least 24")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 16}}));
      CHECK(tool.drawBox() == vm::bbox3d{{0, 0, -8}, {128, 128, 16}});
      tool.endDraw();

      // right = -hx 16 / 24
      CHECK(valueOf(*lightNode, "origin") == "64 64 16");
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -16");
      CHECK(valueOf(*lightNode, "light_right") == "-42.67 0 0");
      CHECK(valueOf(*lightNode, "light_up") == "0 -42.67 0");
    }

    SECTION("a selected point light fills the box")
    {
      auto* lightNode = createLight(map, "0 0 0", {{"angles", "0 90 0"}});

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      tool.endDraw();

      CHECK(valueOf(*lightNode, "origin") == "64 64 32");
      CHECK(valueOf(*lightNode, "light_radius") == "64 64 32");
      CHECK(lightNode->entity().property("angles") == nullptr);
    }

    SECTION("with nothing selected, a new light")
    {
      mdl::deselectAll(map);
      const auto entityCount = map.worldNode().defaultLayer()->childCount();

      CHECK(tool.canDraw());
      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      // the preview is the would-be light
      REQUIRE(tool.drawLight());
      CHECK(tool.drawLight()->math == lightVolumeSeedMath());
      tool.endDraw();

      REQUIRE(map.selection().entities.size() == 1);
      const auto* lightNode = map.selection().entities.front();
      CHECK(valueOf(*lightNode, "classname") == "light");
      CHECK(valueOf(*lightNode, "origin") == "64 64 32");
      CHECK(valueOf(*lightNode, "light_radius") == "64 64 32");

      // the in-game editor's auto-seed (D29)
      CHECK(valueOf(*lightNode, "math_enabled") == "1");
      CHECK(valueOf(*lightNode, "math_falloff") == "linear");
      CHECK(valueOf(*lightNode, "math_inner_radius") == "0.1");
      CHECK(valueOf(*lightNode, "math_falloff_exp") == "2");
      CHECK(valueOf(*lightNode, "math_shape_exp") == "2");
      CHECK(valueOf(*lightNode, "math_penumbra") == "0.15");
      CHECK(valueOf(*lightNode, "math_pattern") == "none");
      CHECK(mdl::parseLightProperties(lightNode->entity()).math == lightVolumeSeedMath());

      // one undo step
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Create Light");
      map.undoCommand();
      CHECK(map.worldNode().defaultLayer()->childCount() == entityCount);
    }

    SECTION("a half extent below 1 is refused")
    {
      createLight(map, "0 0 0", {});
      CHECK_FALSE(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {1, 128, 64}}));
      CHECK(tool.drawBox() == std::nullopt);
    }

    SECTION("a mixed selection is refused")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      auto* brushNode = createBrush(map);
      selectOnly(map, {lightNode, brushNode});

      CHECK_FALSE(tool.canDraw());
      CHECK_FALSE(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
    }

    SECTION("cancelDraw discards the box")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      const auto properties = lightNode->entity().properties();

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      tool.cancelDraw();
      CHECK(tool.drawBox() == std::nullopt);
      CHECK(lightNode->entity().properties() == properties);
    }
  }

  SECTION("deactivate drops the drawn volume and returns to the handle mode")
  {
    createLight(map, "0 0 0", {});
    tool.activate();
    tool.setMode(LightVolumeTool::Mode::Draw);
    REQUIRE(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));

    tool.deactivate();
    CHECK(tool.drawBox() == std::nullopt);
    CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
  }
}

} // namespace tb::ui
