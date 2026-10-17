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
#include "ui/LightTestUtils.h"
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

// the default spot light with a math core to 0.3 of its length: its inner cap at depth
// 8 + 0.3 * 256 = 84.8 (D69)
const auto InnerCapSpot = Properties{
  {"light_target", "0 0 -256"},
  {"light_right", "-128 0 0"},
  {"light_up", "0 -128 0"},
  {"math_enabled", "1"},
  {"math_inner_radius", "0.3"},
};

// every rotation property of a light: a drawn volume removes them all (D11)
const auto RotationProperties = Properties{
  {"angle", "45"},
  {"angles", "0 90 0"},
  {"rotation", "1 0 0 0 1 0 0 0 1"},
  {"light_rotation", "1 0 0 0 1 0 0 0 1"},
  {"light_angles", "0 0 0"},
};

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

double lengthOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  return vm::length(vectorOf(entityNode, key));
}

/**
 * Whether a type still has the scale tool's anchor and proportional settings, which the
 * light volume tool no longer has (D65, AC-25).
 */
template <typename T>
constexpr auto hasScaleModifierSettings =
  requires(T& tool) { tool.anchorPos(); }
  || requires(T& tool) { tool.setAnchorPos(AnchorPos::Center); }
  || requires(T& tool) { tool.proportionalAxes(); }
  || requires(T& tool) { tool.setProportionalAxes(ProportionalAxes::All()); };

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

    SECTION("the default light's box is at +-241.26 (AC-18)")
    {
      createLight(map, "0 0 0", {});
      const auto box = tool.editBox();
      REQUIRE(box);
      CHECK(vm::approx{vm::bbox3d{241.26}, 0.005} == *box);
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
    // the box of a point light is +-radius * t and scales about the light's origin, so a
    // side moved by d moves the opposite side by -d, the half extent by d and the radius
    // by d / t (D61, D64); the light's legacy t at 10 %
    const auto t = legacyVisibleT();

    SECTION("a point light's side moves both sides, the origin stays (AC-23)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

      // (300 t + 32) / t
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      checkRadius(*lightNode, {300.0 + 32.0 / t, 300, 300});
      CHECK(valueOf(*lightNode, "light_radius") == "339.79 300 300");
    }

    SECTION("a point light's -x side dragged by -32 gives the same light (AC-23)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, sideHit({-1, 0, 0}), {{-32, 0, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      CHECK(valueOf(*lightNode, "light_radius") == "339.79 300 300");
    }

    SECTION("a turned point light's side moves along its turned axis")
    {
      // angles 0 90 0 turns the box frame's +x to world +Y, so a box frame delta is
      // R^T times the world delta, not R times it
      auto* lightNode = createLight(map, "0 0 0", {{"angles", "0 90 0"}});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{0, 32, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      checkRadius(*lightNode, {300.0 + 32.0 / t, 300, 300});
      CHECK(valueOf(*lightNode, "light_radius") == "339.79 300 300");
      CHECK(valueOf(*lightNode, "angles") == "0 90 0");
    }

    SECTION("a face centre moves one side, the seeded light's by 32 (AC-18)")
    {
      // the face centre handles drag like the sides (D58)
      auto* lightNode = createLight(map, "0 0 0", SeededMath);

      SECTION("the opposite side mirrors it, the origin stays (D64, AC-23)")
      {
        CHECK(drag(tool, faceCenterHit({1, 0, 0}), {{32, 0, 0}}) == std::vector{true});

        // (273 + 32) / 0.91
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        checkRadius(
          *lightNode, {(300.0 * SeededVisibleT + 32.0) / SeededVisibleT, 300, 300});
        CHECK(valueOf(*lightNode, "light_radius") == "335.16 300 300");
      }

      SECTION("the opposite face dragged the other way gives the same light")
      {
        CHECK(drag(tool, faceCenterHit({-1, 0, 0}), {{-32, 0, 0}}) == std::vector{true});

        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
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
              == Catch::Approx(i == axis ? 300.0 + 32.0 / SeededVisibleT : 300.0)
                   .margin(ComponentTolerance));
          }
          CHECK(valueOf(*lightNode, "origin") == "0 0 0");
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

      checkRadius(*lightNode, {300.0 + 32.0 / SeededVisibleT, 300, 300});
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    }

    SECTION("a point light's edge follows the drag on its two axes (AC-10, AC-23)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      // the edge at +x +y along z; each of its axes about the origin
      CHECK(
        drag(tool, edgeHit({1, 1, -1}, {1, 1, 1}), {{32, 16, 0}}) == std::vector{true});

      // (300 t + 16) / t = 319.9
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      checkRadius(*lightNode, {300.0 + 32.0 / t, 300.0 + 16.0 / t, 300});
      CHECK(valueOf(*lightNode, "light_radius") == "339.79 319.9 300");
    }

    SECTION("a corner scales every axis about the origin (AC-23)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, cornerHit({1, 1, 1}), {{32, 32, 32}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      const auto radius = 300.0 + 32.0 / t;
      checkRadius(*lightNode, {radius, radius, radius});
      CHECK(valueOf(*lightNode, "light_radius") == "339.79 339.79 339.79");
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
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    }

    SECTION("a side scales its own axis only, Shift or not (D65)")
    {
      auto* lightNode = createLight(map, "0 0 0", {});
      CHECK(drag(tool, sideHit({1, 0, 0}), {{60, 0, 0}}) == std::vector{true});

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      checkRadius(*lightNode, {300.0 + 60.0 / t, 300, 300});

      // the tool has no anchor or proportional axes any more (AC-25)
      STATIC_REQUIRE_FALSE(hasScaleModifierSettings<LightVolumeTool>);
    }

    // the default spot light's box is [-132, 132]^2 x [0, 264] along the beam, its +x
    // world -X, its +y world +Y and its +z world -Z; the light's right and up are its
    // half extents times |target| / (zNear + zFar), 256 / 264 here, and every drag keeps
    // its origin and the direction of its target (D64, D67, AC-24)
    SECTION("a spot light's far cap edge widens the far rectangle, no tilt")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

      SECTION("the box frame's +x edge, world -X")
      {
        // in the box frame, the edge at x = +hx, z = zNear + zFar; hx 164
        CHECK(
          drag(tool, edgeHit({1, -1, 1}, {1, 1, 1}), {{-32, 0, 0}}) == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(164.0 * 256.0 / 264.0).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
      }

      SECTION("the world +X edge")
      {
        CHECK(
          drag(tool, edgeHit({-1, -1, 1}, {-1, 1, 1}), {{32, 0, 0}})
          == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
      }

      SECTION("both of its axes, the far cap's width and the length (AC-24)")
      {
        // by (32, 0, 64) in the box frame: hx 164, the length 328, |target| 320
        CHECK(
          drag(tool, edgeHit({1, -1, 1}, {1, 1, 1}), {{-32, 0, -64}})
          == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -320");
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(164.0 * 320.0 / 328.0).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_right") == "-160 0 0");
        CHECK(
          lengthOf(*lightNode, "light_up")
          == Catch::Approx(132.0 * 320.0 / 328.0).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_up") == "0 -128.78 0");
      }
    }

    SECTION("a turned spot light's handles work in its own frame (review round 4)")
    {
      // angles 45 0 0 turn the beam; the keys are in the light's own frame, so the
      // numbers are the default spot light's
      auto turnedSpot = DefaultSpot;
      turnedSpot.emplace_back("angles", "45 0 0");
      auto* lightNode = createLight(map, "0 0 0", turnedSpot);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      REQUIRE_FALSE(
        vm::is_equal(volume->rotation * vm::vec3d{0, 0, 1}, vm::vec3d{0, 0, -1}, 1e-3));

      SECTION("the +x face")
      {
        CHECK(
          drag(tool, faceCenterHit({1, 0, 0}), {volume->rotation * vm::vec3d{32, 0, 0}})
          == std::vector{true});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
      }

      SECTION("the far cap's +x edge")
      {
        CHECK(
          drag(
            tool,
            edgeHit({1, -1, 1}, {1, 1, 1}),
            {volume->rotation * vm::vec3d{32, 0, 64}})
          == std::vector{true});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -320");
        CHECK(valueOf(*lightNode, "light_right") == "-160 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128.78 0");
      }

      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
      CHECK(valueOf(*lightNode, "angles") == "45 0 0");
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
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
      CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
      CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
      checkVector(*lightNode, "origin", {0, 0, 0});
    }

    SECTION("a spot light's beam-parallel edge scales its two lateral axes (AC-24)")
    {
      // the edge at +x +y along the beam by (32, 16, 0) in the box frame: hx 164, hy 148
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      CHECK(
        drag(tool, edgeHit({1, 1, -1}, {1, 1, 1}), {{-32, 16, 0}}) == std::vector{true});

      checkVector(*lightNode, "origin", {0, 0, 0});
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
      CHECK(
        lengthOf(*lightNode, "light_right")
        == Catch::Approx(164.0 * 256.0 / 264.0).margin(LengthTolerance));
      CHECK(
        lengthOf(*lightNode, "light_up")
        == Catch::Approx(148.0 * 256.0 / 264.0).margin(LengthTolerance));
      CHECK(valueOf(*lightNode, "light_up") == "0 -143.52 0");
    }

    SECTION("a spot light's apex face edge scales its lateral axis only (AC-24)")
    {
      // the edge at +x on the apex face by (32, 0, 16) in the box frame: its coordinate
      // along the beam is 0, so the beam's part of the drag is ignored
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      CHECK(
        drag(tool, edgeHit({1, -1, -1}, {1, 1, -1}), {{-32, 0, -16}})
        == std::vector{true});

      checkVector(*lightNode, "origin", {0, 0, 0});
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
      CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
      CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
    }

    SECTION("a spot light's corners scale about the apex (AC-24)")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

      SECTION("a far corner scales the far cap and the length by one factor")
      {
        // the corner (132, 132, 264) moved along its line from the apex by a tenth of
        // its distance: hx = hy = 145.2, the length 290.4, |target| 282.4
        const auto corner = vm::vec3d{132, 132, 264};
        REQUIRE(vm::length(corner) == Catch::Approx(323.33).margin(0.005));
        const auto boxDelta = corner / 10.0;
        CHECK(
          drag(tool, cornerHit({1, 1, 1}), {{-boxDelta.x(), boxDelta.y(), -boxDelta.z()}})
          == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -282.4");
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(145.2 * 282.4 / 290.4).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_right") == "-141.2 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -141.2 0");
      }

      SECTION("an apex corner scales the far cap, the length unchanged (Q20)")
      {
        // the corner (132, 132, 0) by a tenth of its distance from the apex: hx = hy =
        // 145.2, the length 264
        const auto boxDelta = vm::vec3d{132, 132, 0} / 10.0;
        CHECK(
          drag(tool, cornerHit({1, 1, -1}), {{-boxDelta.x(), boxDelta.y(), 0}})
          == std::vector{true});

        checkVector(*lightNode, "origin", {0, 0, 0});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(145.2 * 256.0 / 264.0).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_right") == "-140.8 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -140.8 0");
      }
    }

    SECTION("a spot light's lateral face centre widens its far rectangle (D58)")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

      // the box frame's +x is world -X for the default spot light
      SECTION("the +x face scales hx about the beam, no tilt (D67, AC-24)")
      {
        // the box [-164, 164] x [-132, 132] x [0, 264]: right = 164 * 256 / 264
        CHECK(drag(tool, faceCenterHit({1, 0, 0}), {{-32, 0, 0}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(
          lengthOf(*lightNode, "light_right")
          == Catch::Approx(164.0 * 256.0 / 264.0).margin(LengthTolerance));
        CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
        checkVector(*lightNode, "origin", {0, 0, 0});
      }

      SECTION("the -x face by -32 gives the same light")
      {
        CHECK(drag(tool, faceCenterHit({-1, 0, 0}), {{32, 0, 0}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(valueOf(*lightNode, "light_right") == "-159.03 0 0");
        CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
        checkVector(*lightNode, "origin", {0, 0, 0});
      }

      SECTION("the +y and -y faces scale hy, the target and right unchanged")
      {
        for (const auto& [normal, delta] : std::vector<std::pair<vm::vec3d, vm::vec3d>>{
               {{0, 1, 0}, {0, 32, 0}}, {{0, -1, 0}, {0, -32, 0}}})
        {
          CAPTURE(normal);
          REQUIRE(drag(tool, faceCenterHit(normal), {delta}) == std::vector{true});
          CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
          CHECK(
            lengthOf(*lightNode, "light_up")
            == Catch::Approx(164.0 * 256.0 / 264.0).margin(LengthTolerance));
          CHECK(valueOf(*lightNode, "light_up") == "0 -159.03 0");
          CHECK(valueOf(*lightNode, "light_right") == "-128 0 0");
          checkVector(*lightNode, "origin", {0, 0, 0});
          map.undoCommand();
        }
      }
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

    SECTION("a spot light's apex face is not a handle (D64)")
    {
      // it lies on the light's position, which the move tool and the keys move
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      const auto properties = lightNode->entity().properties();
      const auto undoName = *map.undoCommandName();

      CHECK(drag(tool, sideHit({0, 0, -1}), {{0, 0, -16}}) == std::vector{false});
      CHECK(drag(tool, faceCenterHit({0, 0, -1}), {{0, 0, 16}}) == std::vector{false});

      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
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

        // the box is 1 wide, half a unit on each side of the origin
        CHECK(
          drag(tool, sideHit({1, 0, 0}), {{-(300.0 * t - 0.5), 0, 0}})
          == std::vector{false});

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("a side dragged to or through the origin (AC-23)")
      {
        auto* lightNode = createLight(map, "0 0 0", {});
        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();

        CHECK(drag(tool, sideHit({1, 0, 0}), {{-300.0 * t, 0, 0}}) == std::vector{false});
        CHECK(drag(tool, sideHit({1, 0, 0}), {{-400, 0, 0}}) == std::vector{false});
        CHECK(drag(tool, cornerHit({1, 1, 1}), {{-400, 0, 0}}) == std::vector{false});

        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
      }

      SECTION("a denied step keeps the last accepted box")
      {
        auto* lightNode = createLight(map, "0 0 0", {});

        CHECK(
          drag(tool, sideHit({1, 0, 0}), {{32, 0, 0}, {-1000, 0, 0}, {32, 0, 0}})
          == std::vector{true, false, true});

        // the denied step is not accumulated: the side by 64 in all
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");
        checkRadius(*lightNode, {300.0 + 64.0 / t, 300, 300});
      }

      SECTION("a drag back to a start box that is itself denied gives the start light")
      {
        // the z half extent of the handle box is 0.5 t, of the full box 0.5
        auto* lightNode = createLight(map, "0 0 0", {{"light_radius", "300 300 0.5"}});
        const auto properties = lightNode->entity().properties();
        const auto undoName = *map.undoCommandName();

        // a handle, out to an accepted box and back
        CHECK(
          drag(tool, sideHit({0, 0, 1}), {{0, 0, 32}, {0, 0, -32}})
          == std::vector{true, true});
        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);

        // the outer shell, by a Shift drag
        const auto hit = pickSurface(tool, vm::ray3d{{0, 0, 1000}, {0, 0, -1}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
        CHECK(drag(tool, hit, {{0, 0, 32}, {0, 0, -32}}) == std::vector{true, true});
        CHECK(lightNode->entity().properties() == properties);
        CHECK(*map.undoCommandName() == undoName);
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

        SECTION("the opposite side mirrors it, the origin stays (D64, AC-23)")
        {
          // the shell is at the full radius: it follows the mouse, the radius grows by
          // the drag
          CHECK(drag(tool, hit, {{32, 0, 0}}) == std::vector{true});
          CHECK(valueOf(*lightNode, "origin") == "0 0 0");
          CHECK(valueOf(*lightNode, "light_radius") == "332 300 300");
        }

        SECTION("the -x side dragged by -32 gives the same light")
        {
          const auto otherHit = pickSurface(tool, vm::ray3d{{-1000, 0, 100}, {1, 0, 0}});
          REQUIRE(
            otherHit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
          CHECK(drag(tool, otherHit, {{-32, 0, 0}}) == std::vector{true});
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

      SECTION("the funnel scales the lateral axis it is grabbed on (D67, AC-24)")
      {
        auto* lightNode = createLight(map, "0 0 0", DefaultSpot);

        SECTION("on world +X, the box frame's -x side: right")
        {
          const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
          REQUIRE(hit.isMatch());
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);

          // at depth 136 the funnel is 68 from the beam; 34 more is a factor of 1.5 on
          // the box's x: hx 198, right 198 * 256 / 264 = 192
          CHECK(drag(tool, hit, {{34, 0, 0}}) == std::vector{true});
          checkVector(*lightNode, "light_target", {0, 0, -256});
          checkVector(*lightNode, "light_right", {-192, 0, 0});
          CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
          checkVector(*lightNode, "origin", {0, 0, 0});

          // the line of the drag runs along that axis through the hit point
          map.undoCommand();
          REQUIRE(tool.beginDrag(hit));
          const auto line = lightVolumeHandleLine(*tool.dragStartVolume(), hit);
          CHECK(vm::is_equal(line.direction, vm::vec3d{1, 0, 0}, 1e-9));
          CHECK(vm::is_equal(line.point, hit.hitPoint(), 1e-9));
          tool.cancelDrag();
        }

        SECTION("on world +Y, the box frame's +y side: up")
        {
          const auto hit = pickSurface(tool, vm::ray3d{{0, 1000, -136}, {0, -1, 0}});
          REQUIRE(hit.isMatch());
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);

          CHECK(drag(tool, hit, {{0, 34, 0}}) == std::vector{true});
          checkVector(*lightNode, "light_target", {0, 0, -256});
          CHECK(valueOf(*lightNode, "light_right") == "-128 0 0");
          checkVector(*lightNode, "light_up", {0, -192, 0});
          checkVector(*lightNode, "origin", {0, 0, 0});
        }

        SECTION("off the axes, the axis it lies farther out on")
        {
          // at depth 136 on the circle of radius 68, 30 degrees off world +X: x 58.9,
          // y 34, so the box's x; the hit point's x moved by 34 along it
          const auto direction = vm::vec3d{std::cos(vm::to_radians(30.0)), 0.5, 0.0};
          const auto hit = pickSurface(
            tool, vm::ray3d{direction * 1000.0 + vm::vec3d{0, 0, -136}, -direction});
          REQUIRE(hit.isMatch());
          REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
          const auto x = hit.hitPoint().x();

          CHECK(drag(tool, hit, {{34, 0, 0}}) == std::vector{true});
          CHECK(
            lengthOf(*lightNode, "light_right")
            == Catch::Approx(128.0 * (x + 34.0) / x).margin(LengthTolerance));
          CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
          checkVector(*lightNode, "light_target", {0, 0, -256});
        }
      }

      SECTION("the inner cap sets math_inner_radius from its depth (D69, AC-22)")
      {
        auto* lightNode = createLight(map, "0 0 0", InnerCapSpot);
        const auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
        REQUIRE(hit.distance() == Catch::Approx(1000.0 - 84.8).margin(1e-6));

        // from depth 84.8 by 32 along the beam: (116.8 - 8) / 256
        CHECK(drag(tool, hit, {{0, 0, -32}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "math_inner_radius") == "0.425");
        CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
        CHECK(valueOf(*lightNode, "origin") == "0 0 0");

        // along the beam, between 0 and 0.98
        map.undoCommand();
        REQUIRE(tool.beginDrag(hit));
        const auto line = lightVolumeHandleLine(*tool.dragStartVolume(), hit);
        CHECK(vm::is_equal(line.direction, vm::vec3d{0, 0, -1}, 1e-9));
        tool.cancelDrag();

        CHECK(drag(tool, hit, {{0, 0, -1000}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "math_inner_radius") == "0.98");
        map.undoCommand();
        CHECK(drag(tool, hit, {{0, 0, 500}}) == std::vector{true});
        CHECK(valueOf(*lightNode, "math_inner_radius") == "0");
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

  SECTION("scaleLightBoxAboutOrigin")
  {
    using Coordinates = std::vector<std::pair<std::size_t, double>>;

    SECTION("a point box scales symmetrically about its centre")
    {
      const auto box = vm::bbox3d{{-100, -200, -300}, {100, 200, 300}};
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}}, {32, 0, 0})
        == vm::bbox3d{{-132, -200, -300}, {132, 200, 300}});
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, -100}}, {-32, 0, 0})
        == vm::bbox3d{{-132, -200, -300}, {132, 200, 300}});
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}, {1, 200}}, {32, 16, 64})
        == vm::bbox3d{{-132, -216, -300}, {132, 216, 300}});
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{}, {32, 16, 64}) == std::optional{box});
    }

    SECTION("a spot box keeps its apex face at z = 0")
    {
      const auto box = vm::bbox3d{{-132, -132, 0}, {132, 132, 264}};
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{2, 264}}, {0, 0, 64})
        == vm::bbox3d{{-132, -132, 0}, {132, 132, 328}});

      const auto corner = scaleLightBoxAboutOrigin(
        box, Coordinates{{0, 132}, {1, 132}, {2, 264}}, vm::vec3d{132, 132, 264} / 10.0);
      REQUIRE(corner);
      CHECK(vm::is_equal(
        *corner, vm::bbox3d{{-145.2, -145.2, 0}, {145.2, 145.2, 290.4}}, 1e-9));
    }

    SECTION("an axis whose coordinate is 0 is left as it is")
    {
      const auto box = vm::bbox3d{{-132, -132, 0}, {132, 132, 264}};
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{2, 0.0}}, {0, 0, 16})
        == std::optional{box});
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 132}, {2, 0.0}}, {32, 0, 16})
        == vm::bbox3d{{-164, -132, 0}, {164, 132, 264}});
    }

    SECTION("a drag to or through the origin is denied")
    {
      const auto box = vm::bbox3d{{-100, -100, -100}, {100, 100, 100}};
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}}, {-100, 0, 0})
        == std::nullopt);
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}}, {-150, 0, 0})
        == std::nullopt);
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}, {1, 100}}, {32, -150, 0})
        == std::nullopt);
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 100}}, {-99, 0, 0})
        == vm::bbox3d{{-1, -100, -100}, {1, 100, 100}});

      // the -x side dragged through the origin, f = (-100 + 150) / -100 = -0.5
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, -100}}, {150, 0, 0})
        == std::nullopt);
    }

    SECTION("no move leaves the box as it is")
    {
      const auto box = vm::bbox3d{{-132, -132, 0}, {132, 132, 264}};
      CHECK(
        scaleLightBoxAboutOrigin(box, Coordinates{{0, 132}, {2, 264}}, {0, 0, 0})
        == std::optional{box});
    }
  }

  SECTION("lightVolumeHandleLine")
  {
    SECTION("a corner lies on the line from the box frame's origin (D66)")
    {
      createLight(map, "16 32 48", {});
      const auto volume = tool.volumeBox();
      REQUIRE(volume);
      const auto halfExtent = volume->box.max.x();

      const auto line = lightVolumeHandleLine(*volume, cornerHit({1, 1, 1}));
      CHECK(vm::is_equal(
        line.point, vm::vec3d{16, 32, 48} + vm::vec3d{1, 1, 1} * halfExtent, 1e-9));
      CHECK(vm::is_equal(line.direction, vm::normalize(vm::vec3d{1, 1, 1}), 1e-9));
    }

    SECTION("a spot light's corners lie on lines from its apex")
    {
      createLight(map, "16 32 48", DefaultSpot);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);

      // the far corner (132, 132, 264) of the box is world (-132, 132, -264) from the
      // apex, the apex corner (132, 132, 0) world (-132, 132, 0)
      auto line = lightVolumeHandleLine(*volume, cornerHit({1, 1, 1}));
      CHECK(vm::is_equal(line.point, vm::vec3d{16 - 132, 32 + 132, 48 - 264}, 1e-9));
      CHECK(
        vm::is_equal(line.direction, vm::normalize(vm::vec3d{-132, 132, -264}), 1e-9));

      line = lightVolumeHandleLine(*volume, cornerHit({1, 1, -1}));
      CHECK(vm::is_equal(line.point, vm::vec3d{16 - 132, 32 + 132, 48}, 1e-9));
      CHECK(vm::is_equal(line.direction, vm::normalize(vm::vec3d{-1, 1, 0}), 1e-9));
    }

    SECTION("an edge through its centre, outwards on its two axes")
    {
      createLight(map, "0 0 0", DefaultSpot);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);

      // the far cap's +x edge, its centre (132, 0, 264) in the box frame
      const auto line = lightVolumeHandleLine(*volume, edgeHit({1, -1, 1}, {1, 1, 1}));
      CHECK(vm::is_equal(line.point, vm::vec3d{-132, 0, -264}, 1e-9));
      CHECK(vm::is_equal(line.direction, vm::normalize(vm::vec3d{-132, 0, -264}), 1e-9));
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

    SECTION("an edge in 3D (D58)")
    {
      // through the middle of the +x +y edge, along z at x = y = 241.26
      const auto hit = pick3D(
        tool,
        vm::ray3d{
          {halfExtent + 500.0, halfExtent + 500.0, 0},
          vm::normalize(vm::vec3d{-1, -1, 0})});
      REQUIRE(hit.type() == LightVolumeTool::EdgeHitType);
      const auto edge = hit.target<BBoxEdge>();
      CHECK(
        ((edge.point0 == vm::vec3d{1, 1, -1} && edge.point1 == vm::vec3d{1, 1, 1})
         || (edge.point0 == vm::vec3d{1, 1, 1} && edge.point1 == vm::vec3d{1, 1, -1})));
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

    SECTION("a spot light's apex line is not picked, its far and side lines are (D67)")
    {
      // the front view, looking along +Y: the box spans world x in [-132, 132] and z in
      // [-264, 0]; its apex line is at z = 0
      selectOnly(map, {});
      auto* spotNode = createLight(map, "0 0 0", DefaultSpot);
      const auto ahead = vm::vec3d{0, 1, 0};
      const auto up = vm::vec3f{0, 0, 1};

      CHECK_FALSE(pick2D(tool, vm::ray3d{{50, -1000, 0}, ahead}, up).isMatch());

      // the far line moves the far cap like its centre (AC-24)
      const auto farHit = pick2D(tool, vm::ray3d{{50, -1000, -264}, ahead}, up);
      REQUIRE(farHit.type() == LightVolumeTool::SideHitType);
      CHECK(farHit.target<BBoxSide>() == BBoxSide{{0, 0, 1}});
      CHECK(drag(tool, farHit, {{0, 0, -64}}) == std::vector{true});
      CHECK(valueOf(*spotNode, "light_target") == "0 0 -320");
      CHECK(
        lengthOf(*spotNode, "light_right")
        == Catch::Approx(132.0 * 320.0 / 328.0).margin(LengthTolerance));
      CHECK(valueOf(*spotNode, "origin") == "0 0 0");
      map.undoCommand();

      // a side line, world x = -132, is the box frame's +x side: one lateral axis
      const auto sideLineHit = pick2D(tool, vm::ray3d{{-132, -1000, -100}, ahead}, up);
      REQUIRE(sideLineHit.type() == LightVolumeTool::SideHitType);
      CHECK(sideLineHit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
      CHECK(drag(tool, sideLineHit, {{-32, 0, 0}}) == std::vector{true});
      CHECK(valueOf(*spotNode, "light_target") == "0 0 -256");
      CHECK(valueOf(*spotNode, "light_right") == "-159.03 0 0");
      CHECK(valueOf(*spotNode, "light_up") == "0 -128 0");
      CHECK(valueOf(*spotNode, "origin") == "0 0 0");
      map.undoCommand();

      // the top view sees the apex face's edges as the lateral sides
      const auto down = vm::vec3d{0, 0, -1};
      const auto topHit = pick2D(tool, vm::ray3d{{-132, 50, 1000}, down});
      REQUIRE(topHit.type() == LightVolumeTool::SideHitType);
      CHECK(topHit.target<BBoxSide>() == BBoxSide{{1, 0, 0}});
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

    SECTION("a big point light's outer shell")
    {
      createLight(map, "0 0 0", {{"light_radius", "1024 1024 1024"}});

      const auto hit = pickSurface(tool, vm::ray3d{{5000, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
      CHECK(hit.distance() == Catch::Approx(5000.0 - 1024.0).margin(1e-6));
    }

    SECTION("a selection change reads no light again, an edit the edited one (I7.10)")
    {
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      auto* firstNode = createLight(map, "0 0 0", {});
      createLight(map, "2000 0 100", DefaultSpot);
      auto* thirdNode = createLight(map, "4000 0 0", {});
      mdl::deselectAll(map);

      // through the three lights, the first one's shell at 700
      const auto ray = vm::ray3d{{-1000, 0, 0}, {1, 0, 0}};
      const auto pickFirst = [&]() {
        const auto hit = pickSurface(tool, ray);
        REQUIRE(hit.isMatch());
        REQUIRE(hit.target<LightSurfaceHit>().node == firstNode);
        return hit;
      };

      const auto buildsBefore = tool.pickableLightBuildCount();
      auto hit = pickFirst();
      CHECK(hit.distance() == Catch::Approx(700.0).margin(1e-6));
      CHECK_FALSE(hit.target<LightSurfaceHit>().selected);
      CHECK(pickSurfaces(tool, ray).size() == 3);
      const auto builds = tool.pickableLightBuildCount();
      CHECK(builds == buildsBefore + 3);

      // a selection change reads nothing, only the flags change
      selectOnly(map, {firstNode});
      CHECK(pickFirst().target<LightSurfaceHit>().selected);
      selectOnly(map, {thirdNode});
      CHECK_FALSE(pickFirst().target<LightSurfaceHit>().selected);
      mdl::deselectAll(map);
      CHECK_FALSE(pickFirst().target<LightSurfaceHit>().selected);
      CHECK(pickSurfaces(tool, ray).size() == 3);
      CHECK(tool.pickableLightBuildCount() == builds);

      // an edit of one light reads that light again, and its pick follows the edit
      selectOnly(map, {firstNode});
      REQUIRE(mdl::setEntityProperty(map, "light_radius", "200 200 200"));
      hit = pickFirst();
      CHECK(hit.distance() == Catch::Approx(800.0).margin(1e-6));
      CHECK(hit.target<LightSurfaceHit>().selected);
      CHECK(tool.pickableLightBuildCount() == builds + 1);
      CHECK(pickSurfaces(tool, ray).size() == 3);

      // a new node reads them all again
      createLight(map, "6000 0 0", {});
      CHECK(pickSurfaces(tool, ray).size() == 4);
      CHECK(tool.pickableLightBuildCount() == builds + 1 + 4);
    }

    SECTION("an unselected light's inner shell is not picked (it is not drawn)")
    {
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      createLight(map, "0 0 0", SeededMath);
      mdl::deselectAll(map);

      const auto hit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
      CHECK(hit.distance() == Catch::Approx(700.0).margin(1e-6));
    }

    SECTION("a node in front of the inner shell leaves the outer shell (D57)")
    {
      createLight(map, "0 0 0", SeededMath);
      auto* brushNode = createBrush(map);
      const auto ray = vm::ray3d{{1000, 0, 0}, {-1, 0, 0}};

      // the brush between the shells, at 800: the selected light's outer shell at 700
      // stays before it
      auto hit = pickSurface(tool, ray, {brushHit(brushNode, 800)});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
      CHECK(hit.distance() == Catch::Approx(700.0).margin(1e-6));

      // the brush behind the inner shell at 970
      hit = pickSurface(tool, ray, {brushHit(brushNode, 990)});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerShell);
    }

    SECTION("a later light's inner shell is held against the node, not an earlier shell")
    {
      auto* oneNode = createLight(map, "0 0 0", SeededMath);
      auto* otherNode = createLight(map, "0 0 0", SeededMath);
      auto* brushNode = createBrush(map);

      // two selected math lights along the ray, both inner shells before the brush at
      // 2990: the near one's at 970, the far one's at 2970, behind the near one's
      const auto pickBoth = [&](mdl::EntityNode* nearNode, mdl::EntityNode* farNode) {
        selectOnly(map, {nearNode});
        REQUIRE(mdl::setEntityProperty(map, "origin", "2000 0 0"));
        selectOnly(map, {farNode});
        REQUIRE(mdl::setEntityProperty(map, "origin", "0 0 0"));
        selectOnly(map, {nearNode, farNode});

        // the brush hit and four hits behind it leave one free slot in the pick result's
        // vector (MSVC grows it to 1, 2, 3, 4, 6): the first light's surface hit is
        // inserted in front of the brush hit without a reallocation, which shifts the
        // brush hit
        auto pickResult = mdl::PickResult{};
        for (const auto distance : {2990.0, 3100.0, 3200.0, 3300.0, 3400.0})
        {
          pickResult.addHit(brushHit(brushNode, distance));
        }
        tool.pickSurfaces(vm::ray3d{{3000, 0, 0}, {-1, 0, 0}}, pickResult);

        const auto surfaces =
          pickResult.all(mdl::HitFilters::type(LightVolumeTool::SurfaceHitType));
        REQUIRE(surfaces.size() == 2);
        const auto nearSurface = surfaces.front().target<LightSurfaceHit>();
        CHECK(nearSurface.node == nearNode);
        CHECK(nearSurface.element == mdl::LightElement::InnerShell);
        CHECK(surfaces.front().distance() == Catch::Approx(970.0).margin(1e-6));
        const auto farSurface = surfaces.back().target<LightSurfaceHit>();
        CHECK(farSurface.node == farNode);
        CHECK(farSurface.element == mdl::LightElement::InnerShell);
        CHECK(surfaces.back().distance() == Catch::Approx(2970.0).margin(1e-6));
      };

      // the tool reads the selected lights in an order of its own, the same for both
      // calls: each light is the far one once, so that once the near one comes first
      pickBoth(oneNode, otherNode);
      pickBoth(otherNode, oneNode);
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

      // without math, the funnel is the stand-in's ellipse (D69): at depth 136 its side
      // is at x = 68, as the frustum's was, on the axis either way
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

    SECTION("a selected math spot light's inner cap, from either side (D69, AC-22)")
    {
      createLight(map, "0 0 0", InnerCapSpot);
      const auto depth = 8.0 + 0.3 * 256.0;

      // from the base's side, behind the base at 736
      auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
      CHECK(hit.distance() == Catch::Approx(1000.0 - depth).margin(1e-6));
      CHECK(vm::is_equal(hit.target<LightSurfaceHit>().localPoint, {0, 0, -depth}, 1e-6));

      // from the apex's side
      hit = pickSurface(tool, vm::ray3d{{0, 0, 1000}, {0, 0, -1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
      CHECK(hit.distance() == Catch::Approx(1000.0 + depth).margin(1e-6));

      // through the funnel, which the ray enters at depth 119
      const auto toCentre = vm::vec3d{0, 0, -depth} - vm::vec3d{200, 0, -200};
      hit = pickSurface(tool, vm::ray3d{{200, 0, -200}, vm::normalize(toCentre)});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
      CHECK(hit.distance() == Catch::Approx(vm::length(toCentre)).margin(1e-6));

      // the cap is the ring's inside, not the square around it: at (40, 40) the ray
      // crosses the cap's plane outside the ring, and the base keeps it
      hit = pickSurface(tool, vm::ray3d{{40, 40, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      CHECK(hit.distance() == Catch::Approx(736.0).margin(1e-6));

      // without a core, no cap
      REQUIRE(mdl::setEntityProperty(map, "math_inner_radius", "0.01"));
      hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
    }

    SECTION("a node in front of the inner cap leaves the base or the funnel (D69)")
    {
      createLight(map, "0 0 0", InnerCapSpot);
      auto* brushNode = createBrush(map);
      const auto ray = vm::ray3d{{0, 0, -1000}, {0, 0, 1}};

      // the brush between the base at 736 and the cap at 915.2
      auto hit = pickSurface(tool, ray, {brushHit(brushNode, 800)});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      CHECK(hit.distance() == Catch::Approx(736.0).margin(1e-6));

      // the brush behind the cap
      hit = pickSurface(tool, ray, {brushHit(brushNode, 950)});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
    }

    SECTION("an unselected spot light's inner cap is not picked (it is not drawn)")
    {
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      createLight(map, "0 0 0", InnerCapSpot);
      mdl::deselectAll(map);

      const auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      CHECK(hit.distance() == Catch::Approx(736.0).margin(1e-6));
    }

    SECTION("a pick follows an edit of the light")
    {
      createLight(map, "0 0 0", {});
      const auto ray = vm::ray3d{{1000, 0, 0}, {-1, 0, 0}};

      CHECK(pickSurface(tool, ray).distance() == Catch::Approx(700.0).margin(1e-6));
      REQUIRE(mdl::setEntityProperty(map, "light_radius", "200 200 200"));
      CHECK(pickSurface(tool, ray).distance() == Catch::Approx(800.0).margin(1e-6));
      REQUIRE(mdl::setEntityProperty(map, "origin", "100 0 0"));
      CHECK(pickSurface(tool, ray).distance() == Catch::Approx(700.0).margin(1e-6));
    }

    SECTION("an unselected spot light's base and funnel in All mode")
    {
      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};

      createLight(map, "0 0 0", DefaultSpot);
      mdl::deselectAll(map);

      // the base is picked as its rectangle, also where the light does not draw it
      auto hit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      CHECK_FALSE(hit.target<LightSurfaceHit>().selected);
      CHECK(hit.distance() == Catch::Approx(736.0).margin(1e-6));

      hit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
      CHECK(hit.distance() == Catch::Approx(932.0).margin(1e-3));
    }

    SECTION("a math spot light's funnel is its superellipse, without math the ellipse")
    {
      auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
      REQUIRE(mdl::setEntityProperty(map, "math_enabled", "1"));
      REQUIRE(mdl::setEntityProperty(map, "math_shape_exp", "4"));
      REQUIRE(mdl::setEntityProperty(map, "math_penumbra", "0.2"));

      // along the diagonal at depth 136, where |u|^4 + |v|^4 = 1 gives u = v = 2^-1/4
      // and the frustum u = v = 1, 68 from the beam per unit
      const auto diagonal = vm::normalize(vm::vec3d{-1, -1, 0});
      const auto ray = vm::ray3d{{1000, 1000, -136}, diagonal};
      auto hit = pickSurface(tool, ray);
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
      CHECK(
        hit.distance()
        == Catch::Approx((1000.0 - 68.0 * std::pow(2.0, -0.25)) * std::sqrt(2.0))
             .margin(1e-2));
      CHECK(hit.distance() == Catch::Approx(1333.3).margin(0.1));

      // between the penumbra at 0.8 and the funnel, inwards: the penumbra is never
      // picked and the funnel's far side faces away
      const auto inside = 0.9 * 68.0 * std::pow(2.0, -0.25);
      CHECK_FALSE(
        pickSurface(tool, vm::ray3d{{inside, inside, -136}, diagonal}).isMatch());

      // without math, the stand-in's ellipse, u = v = 2^-1/2, not the kept shape
      // exponent 4 (D63, D69), and no longer the frustum's corner line at 1318.0
      REQUIRE(mdl::setEntityProperty(map, "math_enabled", "0"));
      REQUIRE(lightNode->entity().property("math_shape_exp") != nullptr);
      hit = pickSurface(tool, ray);
      REQUIRE(hit.isMatch());
      CHECK(hit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
      CHECK(
        hit.distance()
        == Catch::Approx((1000.0 - 68.0 * std::sqrt(0.5)) * std::sqrt(2.0)).margin(1e-2));
      CHECK(hit.distance() == Catch::Approx(1346.2).margin(0.1));
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
#ifdef NDEBUG
      WARN(
        "300 lights in All mode: the first pick "
        << firstMillis << " ms, then fastest of 5 runs " << fastestMillis << " ms");
      CHECK(fastestMillis <= 2.0);
#else
      (void)firstMillis;
      (void)fastestMillis;
#endif
    }
  }

  SECTION("lightElementTriangles")
  {
    const auto frame = mdl::LightFrame{vm::vec3d{64, 32, 16}, vm::mat3x3d::identity()};
    const auto local = [&](const vm::vec3d& point) { return point - frame.origin; };

    auto point = mdl::LightProperties{};
    point.radius = vm::vec3d{300, 200, 100};

    // a point light's outer shell, the ellipsoid of its radius
    auto triangles = lightElementTriangles(point, frame, mdl::LightElement::OuterShell);
    CHECK(triangles.size() == 224 * 3);
    CHECK(std::ranges::all_of(triangles, [&](const auto& p) {
      return std::abs(mdl::pointFalloffT(local(p), point.radius) - 1.0) < 1e-5;
    }));

    // the inner shell only of a math light with an inner radius
    CHECK(lightElementTriangles(point, frame, mdl::LightElement::InnerShell).empty());
    auto seeded = point;
    seeded.math = mdl::seededMathLightProperties();
    triangles = lightElementTriangles(seeded, frame, mdl::LightElement::InnerShell);
    CHECK(triangles.size() == 224 * 3);
    CHECK(std::ranges::all_of(triangles, [&](const auto& p) {
      return std::abs(mdl::pointFalloffT(local(p), point.radius) - 0.1) < 1e-5;
    }));

    // a point light has no funnel or base
    CHECK(lightElementTriangles(point, frame, mdl::LightElement::Funnel).empty());

    // the default spot light's base at depth 264, its funnel, and without math the
    // stand-in's penumbra at 0.85 (D69), which is drawn and highlighted, never picked
    auto spot = mdl::LightProperties{};
    spot.type = mdl::LightType::Spot;
    spot.target = vm::vec3d{0, 0, -256};
    spot.right = vm::vec3d{-128, 0, 0};
    spot.up = vm::vec3d{0, -128, 0};
    triangles = lightElementTriangles(spot, frame, mdl::LightElement::Base);
    CHECK(triangles.size() == 32 * 3);
    CHECK(std::ranges::all_of(
      triangles, [&](const auto& p) { return std::abs(local(p).z() + 264.0) < 1e-3; }));
    CHECK(
      lightElementTriangles(spot, frame, mdl::LightElement::Funnel).size() == 32 * 2 * 3);
    triangles = lightElementTriangles(spot, frame, mdl::LightElement::Penumbra);
    CHECK(triangles.size() == 32 * 2 * 3);
    CHECK(std::ranges::all_of(triangles, [&](const auto& p) {
      // at depth d, the default spot light's cone is the circle of radius d / 2
      const auto q = local(p);
      return std::abs(vm::length(vm::vec3d{q.x(), q.y(), 0}) / (-q.z() / 2.0) - 0.85)
             < 1e-5;
    }));

    // a math spot light's inner cap at depth 8 + 0.3 * 256 = 84.8 (D69), none without
    // its core
    CHECK(lightElementTriangles(spot, frame, mdl::LightElement::InnerCap).empty());
    auto innerCapSpot = spot;
    innerCapSpot.math.enabled = true;
    innerCapSpot.math.innerRadius = 0.3;
    triangles = lightElementTriangles(innerCapSpot, frame, mdl::LightElement::InnerCap);
    CHECK(triangles.size() == 96);
    CHECK(std::ranges::all_of(
      triangles, [&](const auto& p) { return std::abs(local(p).z() + 84.8) < 1e-4; }));
    innerCapSpot.math.innerRadius = 0.01;
    CHECK(
      lightElementTriangles(innerCapSpot, frame, mdl::LightElement::InnerCap).empty());

    // a sun's box
    auto sun = point;
    sun.type = mdl::LightType::Sun;
    CHECK(lightElementTriangles(sun, frame, mdl::LightElement::SunBox).size() == 12 * 3);
  }

  SECTION("setHighlightedHit")
  {
    createLight(map, "0 0 0", SeededMath);
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

    SECTION("the clicked surface stays clicked while the hover moves on (D68)")
    {
      tool.setClickedSurface(innerHit);
      tool.setHighlightedHit(innerHit);
      CHECK(tool.isClickedSurface(innerHit));

      // the outline is still drawn: render draws it bold while it is not the highlighted
      // hit
      tool.setHighlightedHit(outerHit);
      CHECK(tool.isClickedSurface(innerHit));
      CHECK_FALSE(tool.isClickedSurface(tool.highlightedHit()));
      tool.setHighlightedHit(mdl::Hit::NoHit);
      CHECK(tool.isClickedSurface(innerHit));
    }
  }

  SECTION("setClickedSurface")
  {
    auto* lightNode = createLight(map, "0 0 0", SeededMath);
    const auto outerHit = pickSurface(tool, vm::ray3d{{1000, 100, 0}, {-1, 0, 0}});
    const auto innerHit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
    REQUIRE(outerHit.isMatch());
    REQUIRE(innerHit.isMatch());

    tool.setClickedSurface(innerHit);
    CHECK(tool.isClickedSurface(innerHit));
    CHECK_FALSE(tool.isClickedSurface(outerHit));

    // another surface replaces it (D68)
    tool.setClickedSurface(outerHit);
    CHECK(tool.isClickedSurface(outerHit));
    CHECK_FALSE(tool.isClickedSurface(innerHit));

    SECTION("a selection change forgets it")
    {
      mdl::deselectNodes(map, {lightNode});
      CHECK_FALSE(tool.isClickedSurface(outerHit));
    }

    SECTION("a new map forgets it (I10.6)")
    {
      document.documentWasLoadedNotifier();
      CHECK_FALSE(tool.isClickedSurface(outerHit));
    }
  }

  SECTION("isClickedSurface")
  {
    createLight(map, "0 0 0", SeededMath);
    const auto outerHit = pickSurface(tool, vm::ray3d{{1000, 100, 0}, {-1, 0, 0}});
    REQUIRE(outerHit.isMatch());
    CHECK_FALSE(tool.isClickedSurface(outerHit));

    tool.setClickedSurface(outerHit);

    // the same element of the same light elsewhere
    CHECK(
      tool.isClickedSurface(pickSurface(tool, vm::ray3d{{1000, 0, 100}, {-1, 0, 0}})));

    // a handle is never the clicked surface
    CHECK_FALSE(tool.isClickedSurface(faceCenterHit({1, 0, 0})));
    CHECK_FALSE(tool.isClickedSurface(mdl::Hit::NoHit));

    // the same element of another light
    setPref(Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesAll});
    const auto resetVolumes = kdl::invoke_later{[] {
      PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
    }};
    auto* otherNode = createLight(map, "2000 0 100", SeededMath);
    tool.setClickedSurface(outerHit);
    const auto otherHit = pickSurface(tool, vm::ray3d{{3000, 100, 100}, {-1, 0, 0}});
    REQUIRE(otherHit.target<LightSurfaceHit>().node == otherNode);
    REQUIRE(otherHit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
    CHECK_FALSE(tool.isClickedSurface(otherHit));
  }

  SECTION("clearClickedSurface")
  {
    createLight(map, "0 0 0", SeededMath);
    const auto innerHit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
    REQUIRE(innerHit.isMatch());

    auto refreshes = 0;
    auto connection = tool.refreshViewsNotifier.connect([&](auto&) { ++refreshes; });

    tool.setClickedSurface(innerHit);
    refreshes = 0;
    tool.clearClickedSurface();
    CHECK_FALSE(tool.isClickedSurface(innerHit));
    CHECK(refreshes == 1);

    // nothing to forget, nothing to refresh
    tool.clearClickedSurface();
    CHECK(refreshes == 1);

    SECTION("a handle drag forgets the clicked surface, a surface drag keeps it")
    {
      tool.setClickedSurface(innerHit);
      REQUIRE(tool.beginDrag(faceCenterHit({1, 0, 0})));
      CHECK_FALSE(tool.isClickedSurface(innerHit));
      tool.cancelDrag();

      tool.setClickedSurface(innerHit);
      REQUIRE(tool.beginDrag(innerHit));
      CHECK(tool.isClickedSurface(innerHit));
      tool.cancelDrag();
    }
  }

  SECTION("nudgeClickedElement")
  {
    // the default spot light at the origin; its base picked from below and clicked
    auto* lightNode = createLight(map, "0 0 0", DefaultSpot);
    const auto baseHit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
    REQUIRE(baseHit.isMatch());
    REQUIRE(baseHit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
    tool.setClickedSurface(baseHit);
    const auto properties = lightNode->entity().properties();

    SECTION("one step along world +X shears the far cap's centre (D68, AC-26)")
    {
      // the box's far centre (0, 0, 264) moves to (-16, 0, 264), world (16, 0, -264):
      // the beam turns towards it, D' = |(16, 264)|, |target| = D' - 8, the far cap keeps
      // its half extents 132 and right is re-orthogonalised from the old right
      CHECK(tool.nudgeClickedElement({16, 0, 0}));

      const auto distance = vm::length(vm::vec3d{16, 0, 264});
      const auto n = vm::vec3d{16, 0, -264} / distance;
      const auto targetLength = distance - 8.0;
      const auto r = vm::normalize(vm::vec3d{-1, 0, 0} + n.x() * n);
      const auto scale = 132.0 * targetLength / distance;

      checkVector(*lightNode, "light_target", n * targetLength);
      checkVector(*lightNode, "light_right", r * scale);
      checkVector(*lightNode, "light_up", {0, -scale, 0});
      CHECK(valueOf(*lightNode, "light_target") == "15.52 0 -256.01");
      CHECK(valueOf(*lightNode, "light_right") == "-127.77 0 -7.74");
      CHECK(valueOf(*lightNode, "light_up") == "0 -128.01 0");
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");

      // the far cap's centre at (16, 0, -264), within the two written decimals
      const auto frustum =
        mdl::spotLightFrustum(mdl::parseLightProperties(lightNode->entity()));
      REQUIRE(frustum);
      auto farCentre = vm::vec3d{0, 0, 0};
      for (const auto& corner : frustum->farCorners)
      {
        farCentre = farCentre + corner / 4.0;
      }
      CHECK(vm::is_equal(farCentre, vm::vec3d{16, 0, -264}, 0.02));

      // one undo step
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "Shear Light Volume");
      map.undoCommand();
      CHECK(lightNode->entity().properties() == properties);
    }

    SECTION("world up shortens the beam along it (AC-26)")
    {
      // the box's -z: the far cap at 248, |target| 240, right 132 * 240 / 248
      CHECK(tool.nudgeClickedElement({0, 0, 16}));
      CHECK(valueOf(*lightNode, "light_target") == "0 0 -240");
      CHECK(
        lengthOf(*lightNode, "light_right")
        == Catch::Approx(132.0 * 240.0 / 248.0).margin(LengthTolerance));
      CHECK(valueOf(*lightNode, "light_right") == "-127.74 0 0");
      CHECK(valueOf(*lightNode, "origin") == "0 0 0");
    }

    SECTION("a step that leaves a depth below 24 is taken and changes nothing")
    {
      const auto undoName = *map.undoCommandName();
      CHECK(tool.nudgeClickedElement({0, 0, 241}));
      CHECK(tool.nudgeClickedElement({0, 0, 400}));
      CHECK(lightNode->entity().properties() == properties);
      CHECK(*map.undoCommandName() == undoName);
    }

    SECTION("the clicked base survives a hover change and a nudge")
    {
      tool.setHighlightedHit(farCapCenterHit());
      REQUIRE(tool.nudgeClickedElement({16, 0, 0}));
      CHECK(tool.isClickedSurface(baseHit));
      CHECK(tool.nudgeClickedElement({16, 0, 0}));
      CHECK(tool.isClickedSurface(baseHit));
    }

    SECTION("the keys move the light for any other element, or none")
    {
      const auto funnelHit = pickSurface(tool, vm::ray3d{{1000, 0, -136}, {-1, 0, 0}});
      REQUIRE(funnelHit.target<LightSurfaceHit>().element == mdl::LightElement::Funnel);
      tool.setClickedSurface(funnelHit);
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));

      tool.clearClickedSurface();
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));

      // a point light's outer shell
      auto* pointNode = createLight(map, "1000 0 0", {});
      const auto shellHit = pickSurface(tool, vm::ray3d{{2000, 0, 0}, {-1, 0, 0}});
      REQUIRE(shellHit.target<LightSurfaceHit>().node == pointNode);
      REQUIRE(
        shellHit.target<LightSurfaceHit>().element == mdl::LightElement::OuterShell);
      tool.setClickedSurface(shellHit);
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));

      CHECK(lightNode->entity().properties() == properties);
    }

    SECTION("a selection change forgets the base, the keys then move the light")
    {
      auto* otherNode = createLight(map, "1000 0 0", DefaultSpot);
      CHECK_FALSE(tool.isClickedSurface(baseHit));
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));

      // two lights selected
      selectOnly(map, {lightNode});
      tool.setClickedSurface(baseHit);
      mdl::selectNodes(map, {otherNode});
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
    }

    SECTION("nothing while a modal tool suppresses the handles or volumes are off")
    {
      REQUIRE(tool.handles().deactivate());
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      REQUIRE(tool.handles().activate());

      setPref(
        Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesOff});
      const auto resetVolumes = kdl::invoke_later{[] {
        PreferenceManager::instance().resetToDefault(Preferences::ShowLightVolumes);
      }};
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      CHECK(lightNode->entity().properties() == properties);
    }

    SECTION("nothing in draw mode or during a drag (review round 4)")
    {
      REQUIRE(tool.activate());
      tool.setMode(LightVolumeTool::Mode::Draw);
      REQUIRE(tool.drawing());
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      tool.setMode(LightVolumeTool::Mode::Handles);
      REQUIRE(tool.deactivate());

      REQUIRE(tool.beginDrag(baseHit));
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      tool.cancelDrag();
      CHECK(tool.isClickedSurface(baseHit));

      CHECK(lightNode->entity().properties() == properties);
    }

    SECTION("nothing for a clicked inner cap or sun box")
    {
      auto* innerCapNode = createLight(map, "0 0 0", InnerCapSpot);
      const auto innerCapProperties = innerCapNode->entity().properties();
      const auto capHit = pickSurface(tool, vm::ray3d{{0, 0, -1000}, {0, 0, 1}});
      REQUIRE(capHit.target<LightSurfaceHit>().element == mdl::LightElement::InnerCap);
      tool.setClickedSurface(capHit);
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      CHECK(innerCapNode->entity().properties() == innerCapProperties);

      auto* sunNode =
        createLight(map, "0 0 0", {{"parallel", "1"}, {"light_radius", "300 300 300"}});
      const auto sunProperties = sunNode->entity().properties();
      const auto sunHit = pickSurface(tool, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}});
      REQUIRE(sunHit.target<LightSurfaceHit>().element == mdl::LightElement::SunBox);
      tool.setClickedSurface(sunHit);
      CHECK_FALSE(tool.nudgeClickedElement({16, 0, 0}));
      CHECK(sunNode->entity().properties() == sunProperties);
    }

    SECTION("a turned spot light's base shears in its own frame (review round 4)")
    {
      // angles 45 0 0 turn the beam; one step along the box frame's -x, the default spot
      // light's world +X, gives the keys of AC-26 in the light's own frame
      auto turnedSpot = DefaultSpot;
      turnedSpot.emplace_back("angles", "45 0 0");
      auto* turnedNode = createLight(map, "0 0 0", turnedSpot);
      const auto volume = tool.volumeBox();
      REQUIRE(volume);

      const auto beam = volume->rotation * vm::vec3d{0, 0, 1};
      const auto farCentre = volume->toWorld({0, 0, 264});
      const auto turnedBaseHit =
        pickSurface(tool, vm::ray3d{farCentre + beam * 1000.0, -beam});
      REQUIRE(turnedBaseHit.target<LightSurfaceHit>().node == turnedNode);
      REQUIRE(turnedBaseHit.target<LightSurfaceHit>().element == mdl::LightElement::Base);
      tool.setClickedSurface(turnedBaseHit);

      CHECK(tool.nudgeClickedElement(volume->rotation * vm::vec3d{-16, 0, 0}));
      CHECK(valueOf(*turnedNode, "light_target") == "15.52 0 -256.01");
      CHECK(valueOf(*turnedNode, "light_right") == "-127.77 0 -7.74");
      CHECK(valueOf(*turnedNode, "light_up") == "0 -128.01 0");
      CHECK(valueOf(*turnedNode, "origin") == "0 0 0");
      CHECK(valueOf(*turnedNode, "angles") == "45 0 0");
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
      CHECK(valueOf(*lightNode, "light_radius") == "79.58 79.58 39.79");
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
