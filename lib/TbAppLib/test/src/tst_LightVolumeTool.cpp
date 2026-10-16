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
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h" // IWYU pragma: keep
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/ScaleTool.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <chrono>
#include <cmath>
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

// the handle threshold's default (D53)
constexpr auto DefaultThreshold = 0.10;

/**
 * The t at which a light without math is visible at the given level, where
 * 1 - smoothstep(t) is the level: smoothstep(t) = 1 - level, whose inverse is
 * 1/2 - sin(asin(1 - 2 y) / 3) (D61). A point light's handle box is +-radius * t.
 */
double legacyVisibleT(const double level = DefaultThreshold)
{
  return 0.5 - std::sin(std::asin(1.0 - 2.0 * (1.0 - level)) / 3.0);
}

// the in-game editor's auto-seed (D29): visible to t = 0.91 at 10 %, u = (t - 0.1) / 0.9
constexpr auto SeededVisibleT = 0.91;

const auto SeededMath = Properties{
  {"math_enabled", "1"},
  {"math_falloff", "linear"},
  {"math_inner_radius", "0.1"},
};

const auto DefaultSpot = Properties{
  {"light_target", "0 0 -256"},
  {"light_right", "-128 0 0"},
  {"light_up", "0 -128 0"},
};

// every rotation property of a light: a drawn volume removes them all (D11)
const auto RotationProperties = Properties{
  {"angle", "45"},
  {"angles", "0 90 0"},
  {"rotation", "1 0 0 0 1 0 0 0 1"},
  {"light_rotation", "1 0 0 0 1 0 0 0 1"},
  {"light_angles", "0 0 0"},
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

mdl::Hit faceCenterHit(const vm::vec3d& normal)
{
  return mdl::Hit{
    LightVolumeTool::FaceCenterHitType, 0.0, vm::vec3d{0, 0, 0}, BBoxSide{normal}};
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

mdl::Hit pick2D(
  const LightVolumeTool& tool, const vm::ray3d& pickRay, const vm::vec3f& up = {0, 1, 0})
{
  const auto camera = gl::OrthographicCamera{
    1.0f,
    8000.0f,
    gl::Camera::Viewport{0, 0, 1920, 1080},
    vm::vec3f{pickRay.origin},
    vm::vec3f{vm::normalize(pickRay.direction)},
    up};
  auto pickResult = mdl::PickResult{};
  tool.pick2D(pickRay, camera, pickResult);
  REQUIRE(pickResult.size() <= 1);
  return pickResult.empty() ? mdl::Hit::NoHit : pickResult.all().front();
}

/**
 * The surface hit of the given ray, with the given hits of nodes already picked.
 */
mdl::Hit pickSurface(
  const LightVolumeTool& tool,
  const vm::ray3d& pickRay,
  const std::vector<mdl::Hit>& nodeHits = {})
{
  auto pickResult = mdl::PickResult{};
  for (const auto& hit : nodeHits)
  {
    pickResult.addHit(hit);
  }
  tool.pickSurfaces(pickRay, pickResult);
  return pickResult.first(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType));
}

std::vector<mdl::Hit> pickSurfaces(const LightVolumeTool& tool, const vm::ray3d& pickRay)
{
  auto pickResult = mdl::PickResult{};
  tool.pickSurfaces(pickRay, pickResult);
  return pickResult.all();
}

mdl::Hit brushHit(mdl::BrushNode* brushNode, const double distance)
{
  return mdl::Hit{
    mdl::BrushNode::BrushHitType, distance, vm::vec3d{distance, 0, 0}, brushNode};
}

/**
 * The value of the given vector property, checked against the expected one within the
 * two decimals that the tool writes.
 */
void checkRadius(const mdl::EntityNode& entityNode, const vm::vec3d& expected)
{
  const auto actual = vectorOf(entityNode, "light_radius");
  CAPTURE(actual, expected);
  CHECK(vm::is_equal(actual, expected, ComponentTolerance));
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

    SECTION("needs a game with the light editor")
    {
      auto quakeFixture = MapDocumentFixture{};
      auto& quakeDocument = quakeFixture.create(mdl::QuakeFixtureConfig);
      auto& quakeMap = quakeDocument.map();
      REQUIRE(!mdl::hasLightEditor(quakeMap.gameInfo().gameConfig));

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
      REQUIRE(!mdl::addNodes(quakeMap, {{&mdl::parentForNodes(quakeMap), {entityNode}}})
                 .empty());
      selectOnly(quakeMap, {entityNode});
      REQUIRE(mdl::isLightEntity(entityNode->entity()));

      auto quakeTool = LightVolumeTool{quakeDocument};
      CHECK(quakeTool.selectedLight() == nullptr);
      CHECK(quakeTool.volumeBox() == std::nullopt);
    }
  }

  SECTION("volumeBox")
  {
    SECTION("a point light's box is where it fades out, in its frame (D61)")
    {
      createLight(map, "16 32 48", {{"light_radius", "100 200 300"}});
      const auto volume = tool.volumeBox();
      REQUIRE(volume);

      // a light without math by the smooth stand-in
      const auto halfExtents = vm::vec3d{100, 200, 300} * legacyVisibleT();
      CHECK(vm::approx{vm::bbox3d{-halfExtents, halfExtents}, 1e-4} == volume->box);
      CHECK(volume->frame.origin == vm::vec3d{16, 32, 48});
      CHECK(volume->rotation == vm::mat3x3d::identity());
    }

    SECTION("the seeded light's box is at 0.91 of its radius (AC-18)")
    {
      createLight(map, "0 0 0", SeededMath);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      CHECK(vm::approx{vm::bbox3d{300.0 * SeededVisibleT}, 1e-4} == volume->box);
    }

    SECTION("the threshold of the preferences moves the box")
    {
      createLight(map, "0 0 0", SeededMath);
      setPref(Preferences::LightVolumeHandleThreshold, 0.5f);
      const auto resetThreshold = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(
          Preferences::LightVolumeHandleThreshold);
      }};

      // u = (t - 0.1) / 0.9 = 0.5
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      CHECK(vm::approx{vm::bbox3d{300.0 * 0.55}, 1e-4} == volume->box);
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
    // the box of a point light is +-radius * t, so a side moved by d moves the half
    // extent by d / 2 and the radius by d / (2 t) (D61); the light's legacy t at 10 %
    const auto t = legacyVisibleT();

    SECTION("a point light's side moves the side and the centre follows")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "16 0 0");
      checkRadius(*lightNode, {300.0 + 16.0 / t, 300, 300});
    }

    SECTION("a turned point light's side moves along its turned axis")
    {
      // angles 0 90 0 turns the box frame's +x to world +Y, so a box frame delta is
      // R^T times the world delta, not R times it
      auto* lightNode = createLight(map, "0 0 0", {{"angles", "0 90 0"}});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{0, 32, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 16 0");
      checkRadius(*lightNode, {300.0 + 16.0 / t, 300, 300});
      CHECK(valueOf(*lightNode, "angles") == "0 90 0");
    }

    SECTION("a face centre moves one side, the seeded light's by 32 (AC-18)")
    {
      // the face centre handles drag like the sides (D58)
      auto* lightNode = createLight(map, "0 0 0", SeededMath);

      SECTION("the opposite side stays")
      {
        CHECK(drag(tool, faceCenterHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

        CHECK(valueOf(*lightNode, "origin") == "16 0 0");
        checkRadius(*lightNode, {300.0 + 16.0 / SeededVisibleT, 300, 300});
        CHECK(valueOf(*lightNode, "light_radius") == "317.58 300 300");
      }

      SECTION("with the centre anchor, both sides move")
      {
        tool.setAnchorPos(AnchorPos::Center);
        CHECK(drag(tool, faceCenterHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        checkRadius(*lightNode, {300.0 + 32.0 / SeededVisibleT, 300, 300});
        CHECK(valueOf(*lightNode, "light_radius") == "335.16 300 300");
      }

      SECTION("each of the six along its own axis")
      {
        for (const auto& normal : std::vector<vm::vec3d>{
               {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}})
        {
          CAPTURE(normal);
          REQUIRE(
            drag(tool, faceCenterHit(normal), {normal * 32.0}) == std::vector{true});

          const auto radius = vectorOf(*lightNode, "light_radius");
          const auto axis = vm::find_abs_max_component(normal);
          for (std::size_t i = 0; i < 3; ++i)
          {
            CHECK(
              radius[i]
              == Catch::Approx(i == axis ? 300.0 + 16.0 / SeededVisibleT : 300.0)
                   .margin(ComponentTolerance));
          }
          CHECK(vm::is_equal(
            vectorOf(*lightNode, "origin"), normal * 16.0, ComponentTolerance));
          map.undoCommand();
        }
      }
    }

    SECTION("the threshold is the one at the start of the drag (D61)")
    {
      auto* lightNode = createLight(map, "0 0 0", SeededMath);
      const auto resetThreshold = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(
          Preferences::LightVolumeHandleThreshold);
      }};

      REQUIRE(tool.beginDrag(faceCenterHit({1, 0, 0})));
      setPref(Preferences::LightVolumeHandleThreshold, 0.5f);
      REQUIRE(tool.update({32, 0, 0}));
      tool.endDrag();

      checkRadius(*lightNode, {300.0 + 16.0 / SeededVisibleT, 300, 300});
    }

    SECTION("a point light's edge moves two sides (AC-10)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      // the edge at +x +y along z, with the opposite edge as the anchor
      CHECK(
        drag(tool, edgeHit({1, 1, -1}, {1, 1, 1}), {{32, 32, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "16 16 0");
      checkRadius(*lightNode, {300.0 + 16.0 / t, 300.0 + 16.0 / t, 300});
    }

    SECTION("a corner with the centre anchor changes the radius only")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      tool.setAnchorPos(AnchorPos::Center);
      CHECK(drag(tool, cornerHit({1, 1, 1}), {{16, 16, 16}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      const auto radius = 300.0 + 16.0 / t;
      checkRadius(*lightNode, {radius, radius, radius});
    }

    SECTION("a corner along its diagonal scales every axis by one factor (AC-19)")
    {
      auto* lightNode = createLight(map, "0 0 0", {{"light_radius", "100 200 300"}});
      const auto diagonal = vm::normalize(vm::vec3d{100, 200, 300});
      CHECK(drag(tool, cornerHit({1, 1, 1}), {diagonal * 64.0}) == std::vector{true});

      const auto radius = vectorOf(*lightNode, "light_radius");
      CHECK(radius.x() > 100.0);
      CHECK(radius.y() / radius.x() == Catch::Approx(2.0).margin(1e-3));
      CHECK(radius.z() / radius.x() == Catch::Approx(3.0).margin(1e-3));
    }

    SECTION("a side with proportional axes scales the other axes")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      tool.setProportionalAxes(ProportionalAxes::All());
      CHECK(drag(tool, sideHit({1, 0, 0}), {{60, 0, 0}}) == std::vector{true});

      // the box grows from 2 * 300 t to 2 * 300 t + 60 on every axis
      CHECK(valueOf(*lightNode, "origin") == "30 0 0");
      const auto radius = 300.0 * (2.0 * 300.0 * t + 60.0) / (2.0 * 300.0 * t);
      checkRadius(*lightNode, {radius, radius, radius});
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

    SECTION("every step starts from the light at the start of the drag")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      CHECK(
        drag(
          tool,
          edgeHit({1, -1, 1}, {1, 1, 1}),
          {{-8, 0, 0}, {-8, 0, 0}, {-48, 0, 0}, {32, 0, 0}})
        == std::vector{true, true, true, true});

      // the strings of the single drag by -32
      CHECK(valueOf(*lightNode, "light_target") == "-15.52 0 -256.01");
      CHECK(valueOf(*lightNode, "light_right") == "-143.26 0 8.68");
      CHECK(valueOf(*lightNode, "light_up") == "0 -128.01 0");
      checkVector(*lightNode, "origin", {0, 0, 0});
    }

    SECTION("a spot light's far side moves like its far cap centre (AC-9)")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      CHECK(drag(tool, sideHit({0, 0, 1}), {{0, 0, -64}}) == std::vector{true});

      checkVector(*lightNode, "origin", {0, 0, 0});
      checkVector(*lightNode, "light_target", {0, 0, -320});
      CHECK(
        lengthOf(*lightNode, "light_right")
        == Catch::Approx(128.7805).margin(LengthTolerance));
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
        CHECK(
          drag(tool, sideHit({1, 0, 0}), {{-(2.0 * 300.0 * t - 1.0), 0, 0}})
          == std::vector{false});

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
        checkRadius(*lightNode, {300.0 + 32.0 / t, 300, 300});
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

    SECTION("a surface edits its element (D60, AC-20)")
    {
      SECTION("the outer shell moves its side along its normal's axis")
      {
        auto* lightNode = createLight(map, "0 0 0", SeededMath);
        const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 100}, {-1, 0, 0}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);

        SECTION("the opposite side stays")
        {
          // the shell is at the full radius: it follows the mouse, the radius grows by
          // half the drag and the centre by the other half
          CHECK(drag(tool, hit, {{32, 0, 0}}) == std::vector{true});
          CHECK(valueOf(*lightNode, "origin") == "16 0 0");
          CHECK(valueOf(*lightNode, "light_radius") == "316 300 300");
        }

        SECTION("with the centre anchor, the radius grows by the drag")
        {
          tool.setAnchorPos(AnchorPos::Center);
          CHECK(drag(tool, hit, {{32, 0, 0}}) == std::vector{true});
          CHECK(valueOf(*lightNode, "origin") == "0 0 0");
          CHECK(valueOf(*lightNode, "light_radius") == "332 300 300");
        }

        // the line of the drag is the axis of the normal at the hit point
        REQUIRE(tool.beginDrag(hit));
        const auto line = lightVolumeHandleLine(*tool.dragStartVolume(), hit);
        CHECK(vm::is_equal(line.direction, vm::vec3d{1, 0, 0}, 1e-9));
        tool.cancelDrag();
      }

      SECTION("the inner shell sets math_inner_radius")
      {
        auto* lightNode = createLight(map, "0 0 0", SeededMath);
        const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerShell);

        // from t = 30 / 300 to 60 / 300
        CHECK(drag(tool, hit, {{30, 0, 0}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "math_inner_radius") == "0.2");
        CHECK(lightNode->entity().property("light_radius") == nullptr);
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");

        // at most 0.98
        map.undoCommand();
        CHECK(drag(tool, hit, {{600, 0, 0}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "math_inner_radius") == "0.98");
      }

      SECTION("the base moves the endpoint, the far cap's size unchanged")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
        const auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);

        // the far cap's centre's numbers (D27 (c)): the far rectangle keeps its half
        // extents of 132, so right and up are 132 * 320 / 328
        CHECK(drag(tool, hit, {{0, 0, -64}}) == std::vector{true});
        checkVector(*lightNode, "origin", {0, 0, 0});
        checkVector(*lightNode, "light_target", {0, 0, -320});
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(132.0 * 320.0 / 328.0).margin(LengthTolerance));
        CHECK(
          lengthOf(*lightNode, "light_up")
          == Catch::Approx(132.0 * 320.0 / 328.0).margin(LengthTolerance));
      }

      SECTION("the funnel scales the base uniformly, the length unchanged")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
        const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);

        // at depth 136 the funnel is 68 from the beam; 34 more is a factor of 1.5
        CHECK(drag(tool, hit, {{34, 0, 0}}) == std::vector{true});
        checkVector(*lightNode, "light_target", {0, 0, -256});
        checkVector(*lightNode, "light_right", {-192, 0, 0});
        checkVector(*lightNode, "light_up", {0, -192, 0});
        checkVector(*lightNode, "origin", {0, 0, 0});
      }

      SECTION("a sun's box and another light's surface are not edited")
      {
        auto* sunNode =
          createLight(map, "0 0 0", {{"parallel", "1"}, {"light_radius", "300 300 300"}});
        const auto sunHit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
        REQUIRE(sunHit.isMatch());
        CHECK_FALSE(tool.beginDrag(sunHit));

        // the hit of a light that is no longer the selected one
        auto* otherNode = createLight(map, "2000 0 0", {});
        const auto otherHit = pickSurface(tool, vm::ray3d{{3000, 0, 0}, {-1, 0, 0}});
        REQUIRE(otherHit.target<LightSurfaceHit>().node == otherNode);
        selectOnly(map, {sunNode});
        CHECK_FALSE(tool.beginDrag(otherHit));
        CHECK_FALSE(tool.isDragging());
      }

      SECTION("a drag back to its start is no edit")
      {
        auto* lightNode = createLight(map, "0 0 0", SeededMath);
        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();
        const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});

        CHECK(drag(tool, hit, {{30, 0, 0}, {-30, 0, 0}}) == std::vector{true, true});
        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }
    }
  }

  SECTION("pick3D")
  {
    auto* lightNode = createLight(map, "0 0 0", {});

    // the default light's box is +-300 t (D61)
    const auto halfExtent = 300.0 * legacyVisibleT();

    SECTION("from inside the box, a corner in front of the camera")
    {
      const auto hit =
        pick3D(tool, vm::ray3d{{0, 0, 0}, vm::normalize(vm::vec3d{1, 1, 1})});
      CHECK(hit.type() == LightVolumeTool::CornerHitType);
      CHECK(hit.target<BBoxCorner>() == BBoxCorner{{1, 1, 1}});
    }

    SECTION("from inside the box, the centre of the side in front (D58)")
    {
      const auto hit = pick3D(tool, vm::ray3d{{0, 0, 0}, {1, 0, 0}});
      CHECK(hit.type() == LightVolumeTool::FaceCenterHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(hit.distance() == Catch::Approx(halfExtent).margin(20.0));
    }

    SECTION("from outside the box, the centre of the side in front (D58)")
    {
      const auto hit = pick3D(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      CHECK(hit.type() == LightVolumeTool::FaceCenterHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(hit.distance() == Catch::Approx(1000.0 - halfExtent).margin(20.0));
    }

    SECTION("the six face centres of a point light")
    {
      for (const auto& normal : std::vector<vm::vec3d>{
             {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}})
      {
        CAPTURE(normal);
        const auto hit = pick3D(tool, vm::ray3d{normal * 1000.0, -normal});
        CHECK(hit.type() == LightVolumeTool::FaceCenterHitType);
        CHECK(hit.target<BBoxSide>() == BBoxSide{normal});
      }
    }

    SECTION("a side is not picked: the click reaches what is behind it (D58)")
    {
      // through the +x side, away from its centre, its edges and its corners
      CHECK_FALSE(
        pick3D(tool, vm::ray3d{{1000, halfExtent / 2.0, halfExtent / 2.0}, {-1, 0, 0}})
          .isMatch());

      // and no back side for a ray past the box
      CHECK_FALSE(pick3D(tool, vm::ray3d{{1000, 0, 500}, {-1, 0, 0}}).isMatch());
    }

    SECTION("from inside the box, nothing behind the camera")
    {
      // with large handles, the camera is inside the spheres of the corner and the
      // edge points behind it, which the ray would leave within 100 units
      setPref(Preferences::HandleRadius, 1000.0f);
      const auto resetHandleRadius = kdl::invoke_later{
        [] { PreferenceManager::instance().resetToDefault(Preferences::HandleRadius); }};

      const auto hit =
        pick3D(tool, vm::ray3d{{-290, -290, -290}, vm::normalize(vm::vec3d{1, 1, 1})});
      REQUIRE(hit.isMatch());
      CHECK(hit.distance() > 100.0);
      if (hit.type() == LightVolumeTool::CornerHitType)
      {
        CHECK(hit.target<BBoxCorner>() != BBoxCorner{{-1, -1, -1}});
      }
    }

    SECTION("a turned light's face centre in world space")
    {
      // the box frame's +x side faces world +Y
      selectOnly(map, {});
      createLight(map, "0 0 0", {{"angles", "0 90 0"}});
      const auto hit = pick3D(tool, vm::ray3d{{0, 1000, 0}, {0, -1, 0}});
      CHECK(hit.type() == LightVolumeTool::FaceCenterHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(hit.distance() == Catch::Approx(1000.0 - halfExtent).margin(20.0));
    }

    SECTION("a spot light's far cap centre")
    {
      selectOnly(map, {});
      createLight(map, "0 0 0", DefaultSpot);
      const auto hit = pick3D(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      CHECK(hit.type() == LightVolumeTool::FarCapCenterHitType);
    }

    SECTION("a spot light's four lateral face centres, none at its apex")
    {
      selectOnly(map, {});
      createLight(map, "0 0 0", DefaultSpot);

      // the box [-132, 132]^2 x [0, 264] along the beam, world -Z; its lateral faces'
      // centres at depth 132
      for (const auto& normal :
           std::vector<vm::vec3d>{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}})
      {
        CAPTURE(normal);
        const auto hit =
          pick3D(tool, vm::ray3d{normal * 1000.0 + vm::vec3d{0, 0, -132}, -normal});
        CHECK(hit.type() == LightVolumeTool::FaceCenterHitType);
      }

      // the apex is the light's own position, where a drag moves the light (R9): from
      // above, the ray reaches the far cap's centre behind it
      const auto apexHit = pick3D(tool, vm::ray3d{{0, 0, 1000}, {0, 0, -1}});
      CHECK(apexHit.type() == LightVolumeTool::FarCapCenterHitType);
    }

    SECTION("nothing without a selected light")
    {
      mdl::deselectNodes(map, {lightNode});
      CHECK_FALSE(pick3D(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).isMatch());
    }

    SECTION("nothing while the light volumes are off (AC-21)")
    {
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesOff});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};
      CHECK_FALSE(pick3D(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).isMatch());
    }
  }

  SECTION("pick2D")
  {
    createLight(map, "0 0 0", {{"light_radius", "300 200 100"}});
    const auto t = legacyVisibleT();
    const auto halfExtents = vm::vec3d{300, 200, 100} * t;

    SECTION("the outline is the side perpendicular to it (D59, AC-19)")
    {
      // the top view: the outline's edges at x = +-hx are the sides +-x, at y = +-hy the
      // sides +-y
      const auto down = vm::vec3d{0, 0, -1};
      auto hit = pick2D(tool, vm::ray3d{{halfExtents.x(), 50, 1000}, down});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});

      hit = pick2D(tool, vm::ray3d{{-50, -halfExtents.y(), 1000}, down});
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{0, -1, 0}});

      // the front view, looking along +Y: at x = +hx the side +x, at z = +hz the side +z
      const auto ahead = vm::vec3d{0, 1, 0};
      const auto up = vm::vec3f{0, 0, 1};
      hit = pick2D(tool, vm::ray3d{{halfExtents.x(), -1000, 20}, ahead}, up);
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});

      hit = pick2D(tool, vm::ray3d{{50, -1000, halfExtents.z()}, ahead}, up);
      CHECK(hit.type() == LightVolumeTool::SideHitType);
      CHECK(hit.target<BBoxSide>() == BBoxSide{{0, 0, 1}});
    }

    SECTION("a corner")
    {
      const auto hit =
        pick2D(tool, vm::ray3d{{halfExtents.x(), halfExtents.y(), 1000}, {0, 0, -1}});
      CHECK(hit.type() == LightVolumeTool::CornerHitType);
    }

    SECTION("nothing off the outline, and no face centre")
    {
      // outside the box, and over the centre of the top side
      CHECK_FALSE(pick2D(tool, vm::ray3d{{600, 0, 1000}, {0, 0, -1}}).isMatch());
      CHECK_FALSE(pick2D(tool, vm::ray3d{{0, 0, 1000}, {0, 0, -1}}).isMatch());
    }
  }

  SECTION("pickSurfaces")
  {
    SECTION("a point light's outer shell, from outside only")
    {
      auto* lightNode = createLight(map, "0 0 0", {});

      const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.distance() == Catch::Approx(700.0).margin(1e-6));
      const auto surface = hit.target<LightSurfaceHit>();
      CHECK(surface.node == lightNode);
      CHECK(surface.element == mdl::LightElement::OuterShell);
      CHECK(surface.selected);
      CHECK(vm::is_equal(surface.localPoint, vm::vec3d{300, 0, 0}, 1e-6));

      // like a brush face, a shell is not picked from inside, so the walls behind it
      // stay clickable
      CHECK_FALSE(pickSurface(tool, vm::ray3d{{0, 0, 0}, {1, 0, 0}}).isMatch());
    }

    SECTION("a math point light's inner shell, which the outer one encloses")
    {
      createLight(map, "0 0 0", SeededMath);

      // the ray enters the outer shell at 700 and the inner one at 970
      auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerShell);
      CHECK(hit.distance() == Catch::Approx(970.0).margin(1e-6));

      // past the inner shell, the outer one
      hit = pickSurface(tool, vm::ray3d{{1000, 100, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);

      // from inside the outer shell
      hit = pickSurface(tool, vm::ray3d{{200, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerShell);
      CHECK(hit.distance() == Catch::Approx(170.0).margin(1e-6));
    }

    SECTION("a spot light's funnel and base")
    {
      createLight(map, "0 0 0", DefaultSpot);

      // without math, the funnel is the frustum: at depth 136 its side is at x = 68
      auto hit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
      CHECK(hit.distance() == Catch::Approx(1000.0 - 68.0).margin(1e-3));

      // the base faces along the beam, world -Z
      hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      CHECK(hit.distance() == Catch::Approx(1000.0 - 264.0).margin(1e-6));

      // from inside the cone, neither
      CHECK_FALSE(pickSurface(tool, vm::ray3d{{0, 0, -100}, {0, 0, -1}}).isMatch());
    }

    SECTION("a sun's box")
    {
      createLight(map, "0 0 0", {{"parallel", "1"}, {"light_radius", "300 300 300"}});
      const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::SunBox);
      CHECK(hit.distance() == Catch::Approx(700.0).margin(1e-3));
    }

    SECTION("in Selected mode only the selected lights, in All mode every one")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      auto* otherNode = createLight(map, "2000 0 0", {});
      selectOnly(map, {lightNode});

      const auto towardsOther = vm::ray3d{{3000, 0, 0}, {-1, 0, 0}};
      auto hits = pickSurfaces(tool, towardsOther);
      REQUIRE(hits.size() == 1);
      CHECK(hits.front().target<LightSurfaceHit>().node == lightNode);

      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      hits = pickSurfaces(tool, towardsOther);
      REQUIRE(hits.size() == 2);
      CHECK(hits.front().target<LightSurfaceHit>().node == otherNode);
      CHECK_FALSE(hits.front().target<LightSurfaceHit>().selected);
      CHECK(hits.back().target<LightSurfaceHit>().node == lightNode);

      // an unselected light's surface yields to any node hit, so it is not even picked
      auto* brushNode = createBrush(map);
      selectOnly(map, {lightNode});
      auto pickResult = mdl::PickResult{};
      pickResult.addHit(brushHit(brushNode, 5000));
      tool.pickSurfaces(towardsOther, pickResult);
      const auto surfaces =
        pickResult.all(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType));
      REQUIRE(surfaces.size() == 1);
      CHECK(surfaces.front().target<LightSurfaceHit>().node == lightNode);

      // a hidden light has no surface
      mdl::hideNodes(map, {otherNode});
      selectOnly(map, {});
      CHECK(pickSurfaces(tool, towardsOther).size() == 1);
    }

    SECTION("nothing while the light volumes are off, or for a light_origin light")
    {
      createLight(map, "0 0 0", {});
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesOff});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};
      CHECK(pickSurfaces(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).empty());

      PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      createLight(map, "0 0 0", {{"light_origin", "10 0 0"}});
      CHECK(pickSurfaces(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).empty());
    }

    SECTION("nothing in a game without the light editor (AC-21)")
    {
      auto quakeFixture = MapDocumentFixture{};
      auto& quakeDocument = quakeFixture.create(mdl::QuakeFixtureConfig);
      auto& quakeMap = quakeDocument.map();

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "light"}}}};
      REQUIRE(!mdl::addNodes(quakeMap, {{&mdl::parentForNodes(quakeMap), {entityNode}}})
                 .empty());
      selectOnly(quakeMap, {entityNode});

      auto quakeTool = LightVolumeTool{quakeDocument};
      CHECK_FALSE(quakeTool.showsVolumes());
      CHECK(pickSurfaces(quakeTool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).empty());
      CHECK_FALSE(pick3D(quakeTool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}).isMatch());
    }

    SECTION("300 lights in All mode, one hover pick (Q17)")
    {
      // the user's 300-light map (LW5): a 20 x 15 grid 192 units apart at z 192, every
      // third light a default spot, the others point lights of radius 200 and 300 in a
      // checkerboard
      auto lights = std::vector<mdl::Node*>{};
      for (std::size_t i = 0; i < 300; ++i)
      {
        const auto column = i % 20;
        const auto row = i / 20;
        auto properties = std::vector<mdl::EntityProperty>{
          {"classname", "light"},
          {"origin",
           std::to_string(int(column) * 192) + " " + std::to_string(int(row) * 192)
             + " 192"},
        };
        if (i % 3 == 0)
        {
          properties.emplace_back("light_target", "0 0 -256");
          properties.emplace_back("light_right", "-128 0 0");
          properties.emplace_back("light_up", "0 -128 0");
        }
        else
        {
          properties.emplace_back(
            "light_radius", (column + row) % 2 == 0 ? "200 200 200" : "300 300 300");
        }
        lights.push_back(new mdl::EntityNode{mdl::Entity{std::move(properties)}});
      }
      REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), lights}}).empty());
      mdl::deselectAll(map);

      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      // along the diagonal of the grid through the lights' plane, the worst case: no node
      // hit, so every light is tested
      const auto ray = vm::ray3d{
        {-500, -500, 192}, vm::normalize(vm::vec3d{19.0 * 192.0, 14.0 * 192.0, 0.0})};

      const auto timePick = [&]() {
        auto pickResult = mdl::PickResult{};
        const auto start = std::chrono::steady_clock::now();
        tool.pickSurfaces(ray, pickResult);
        const auto end = std::chrono::steady_clock::now();
        CHECK(!pickResult.empty());
        return std::chrono::duration<double, std::milli>(end - start).count();
      };

      // the first pick reads the lights, the others use what it read
      const auto firstMillis = timePick();
      auto fastestMillis = timePick();
      for (auto run = 1; run < 5; ++run)
      {
        fastestMillis = std::min(fastestMillis, timePick());
      }
      WARN(
        "300 lights in All mode: the first pick "
        << firstMillis << " ms, then fastest of 5 runs " << fastestMillis << " ms");

#ifdef NDEBUG
      CHECK(fastestMillis <= 2.0);
#endif
    }
  }

  SECTION("setHighlightedHit")
  {
    auto* lightNode = createLight(map, "0 0 0", SeededMath);
    const auto outerHit = pickSurface(tool, vm::ray3d{{1000, 100, 0}, {-1, 0, 0}});
    const auto innerHit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
    REQUIRE(outerHit.isMatch());
    REQUIRE(innerHit.isMatch());

    auto refreshes = 0;
    auto connection = tool.refreshViewsNotifier.connect([&](auto&) { ++refreshes; });

    tool.setHighlightedHit(outerHit);
    CHECK(tool.highlightedHit().type() == LightVolumeTool::SurfaceHitType);
    CHECK(refreshes == 1);

    // the same element elsewhere is no change
    tool.setHighlightedHit(pickSurface(tool, vm::ray3d{{1000, 0, 100}, {-1, 0, 0}}));
    CHECK(refreshes == 1);

    tool.setHighlightedHit(innerHit);
    CHECK(refreshes == 2);
    CHECK(
      tool.highlightedHit().target<LightSurfaceHit>().element
      == mdl::LightElement::InnerShell);

    SECTION("a clicked surface keeps its outline until the hover or selection changes")
    {
      tool.setClickedSurface(innerHit);
      CHECK(tool.isClickedSurface(innerHit));
      CHECK_FALSE(tool.isClickedSurface(outerHit));

      tool.setHighlightedHit(innerHit);
      CHECK(tool.isClickedSurface(innerHit));

      tool.setHighlightedHit(outerHit);
      CHECK_FALSE(tool.isClickedSurface(innerHit));

      tool.setClickedSurface(outerHit);
      mdl::deselectNodes(map, {lightNode});
      CHECK_FALSE(tool.isClickedSurface(outerHit));
    }
  }

  SECTION("showsVolumes")
  {
    CHECK(tool.showsVolumes());

    setPref(Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesOff});
    const auto resetVolumes = kdl::invoke_later{[] {
      PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
    }};
    CHECK_FALSE(tool.showsVolumes());

    setPref(Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
    CHECK(tool.showsVolumes());
  }

  SECTION("handleThreshold")
  {
    const auto resetThreshold = kdl::invoke_later{[] {
      PreferenceManager::instance().resetToDefault(
        Preferences::LightVolumeHandleThreshold);
    }};

    CHECK(tool.handleThreshold() == Catch::Approx(0.1).margin(1e-6));
    setPref(Preferences::LightVolumeHandleThreshold, 0.9f);
    CHECK(tool.handleThreshold() == 0.5);
    setPref(Preferences::LightVolumeHandleThreshold, 0.001f);
    CHECK(tool.handleThreshold() == 0.01);
  }

  SECTION("handles")
  {
    // the always-on part works without the tool (D56)
    CHECK(tool.handles().active());
    CHECK_FALSE(tool.active());

    SECTION("a deactivation cancels a drag, which then does nothing")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      REQUIRE(tool.beginDrag(faceCenterHit({1, 0, 0})));
      REQUIRE(tool.update({32, 0, 0}));
      REQUIRE(tool.handles().deactivate());

      CHECK_FALSE(tool.isDragging());
      CHECK(lightNode->entity().properties() == properties);
      CHECK_FALSE(tool.update({32, 0, 0}));
      tool.endDrag();
      CHECK(*map.undoCommandName() == undoName);

      REQUIRE(tool.handles().activate());
    }
  }

  SECTION("drawing")
  {
    CHECK_FALSE(tool.drawing());
    tool.setMode(LightVolumeTool::Mode::Draw);
    CHECK_FALSE(tool.drawing());
    REQUIRE(tool.activate());
    CHECK(tool.drawing());
    REQUIRE(tool.deactivate());
    CHECK_FALSE(tool.drawing());
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
      for (const auto& [key, value] : RotationProperties)
      {
        REQUIRE(mdl::setEntityProperty(map, key, value));
      }
      REQUIRE(tool.volumeBox());

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      tool.endDraw();

      CHECK(valueOf(*lightNode, "origin") == "64 64 64");
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -56");
      CHECK(valueOf(*lightNode, "light_right") == "-56 0 0");
      CHECK(valueOf(*lightNode, "light_up") == "0 -56 0");
      for (const auto& [key, value] : RotationProperties)
      {
        CAPTURE(key);
        CHECK(lightNode->entity().property(key) == nullptr);
      }
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

    SECTION("a selected point light is visible to the box (D61, AC-18)")
    {
      auto* lightNode = createLight(map, "0 0 0", RotationProperties);
      REQUIRE(tool.volumeBox());

      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      tool.endDraw();

      // its radius is the half extents divided by its t, so that its handle box is the
      // drawn box
      CHECK(valueOf(*lightNode, "origin") == "64 64 32");
      checkRadius(*lightNode, vm::vec3d{64, 64, 32} / legacyVisibleT());
      const auto handleBox = tool.editBox();
      REQUIRE(handleBox);
      CHECK(
        vm::approx{vm::bbox3d{vm::vec3d{-64, -64, -32}, vm::vec3d{64, 64, 32}}, 0.01}
        == *handleBox);
      for (const auto& [key, value] : RotationProperties)
      {
        CAPTURE(key);
        CHECK(lightNode->entity().property(key) == nullptr);
      }
    }

    SECTION("with nothing selected, a new light")
    {
      mdl::deselectAll(map);
      const auto entityCount = map.worldNode().defaultLayer()->childCount();

      CHECK(tool.canDraw());
      CHECK(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));
      // the preview is the would-be light
      REQUIRE(tool.drawLight());
      CHECK(tool.drawLight()->math == mdl::seededMathLightProperties());
      tool.endDraw();

      // the seeded light is visible to t = 0.91 of its radius
      REQUIRE(map.selection().entities.size() == 1);
      const auto* lightNode = map.selection().entities.front();
      CHECK(valueOf(*lightNode, "classname") == "light");
      CHECK(valueOf(*lightNode, "origin") == "64 64 32");
      checkRadius(*lightNode, vm::vec3d{64, 64, 32} / SeededVisibleT);
      CHECK(valueOf(*lightNode, "light_radius") == "70.33 70.33 35.16");

      // the in-game editor's auto-seed (D29)
      CHECK(valueOf(*lightNode, "math_enabled") == "1");
      CHECK(valueOf(*lightNode, "math_falloff") == "linear");
      CHECK(valueOf(*lightNode, "math_inner_radius") == "0.1");
      CHECK(valueOf(*lightNode, "math_falloff_exp") == "2");
      CHECK(valueOf(*lightNode, "math_shape_exp") == "2");
      CHECK(valueOf(*lightNode, "math_penumbra") == "0.15");
      CHECK(valueOf(*lightNode, "math_pattern") == "none");
      CHECK(valueOf(*lightNode, "math_energy_clamp") == "1");
      CHECK(valueOf(*lightNode, "math_type") == "point");
      CHECK(valueOf(*lightNode, "math_pattern_strength") == "0");
      CHECK(valueOf(*lightNode, "math_pattern_scale") == "1");
      CHECK(valueOf(*lightNode, "math_pattern_phase") == "0");
      CHECK(
        mdl::parseLightProperties(lightNode->entity()).math
        == mdl::seededMathLightProperties());

      // one undo step
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Create Light");
      map.undoCommand();
      CHECK(map.worldNode().defaultLayer()->childCount() == entityCount);
    }

    SECTION("a new light around the world origin has an origin")
    {
      mdl::deselectAll(map);

      CHECK(tool.updateDraw(vm::bbox3d{{-64, -64, -32}, {64, 64, 32}}));
      tool.endDraw();

      REQUIRE(map.selection().entities.size() == 1);
      const auto* lightNode = map.selection().entities.front();
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      CHECK(valueOf(*lightNode, "light_radius") == "70.33 70.33 35.16");
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

      auto* otherNode = createLight(map, "256 0 0", {});
      selectOnly(map, {lightNode, otherNode});
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

  SECTION("doDeactivate")
  {
    REQUIRE(tool.activate());

    SECTION("drops the drawn volume and returns to the handle mode")
    {
      createLight(map, "0 0 0", {});
      tool.setMode(LightVolumeTool::Mode::Draw);
      REQUIRE(tool.updateDraw(vm::bbox3d{{0, 0, 0}, {128, 128, 64}}));

      REQUIRE(tool.deactivate());
      CHECK(tool.drawBox() == std::nullopt);
      CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    }

    SECTION("cancels the drag, which then does nothing")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      REQUIRE(tool.beginDrag(sideHit({1, 0, 0})));
      REQUIRE(tool.update({32, 0, 0}));
      REQUIRE(tool.deactivate());

      CHECK_FALSE(tool.isDragging());
      CHECK(tool.dragStartVolume() == std::nullopt);
      CHECK(lightNode->entity().properties() == properties);

      // the drag tracker may call the tool after the deactivation
      CHECK_FALSE(tool.update({32, 0, 0}));
      tool.endDrag();
      tool.cancelDrag();

      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
    }
  }
}

} // namespace tb::ui
