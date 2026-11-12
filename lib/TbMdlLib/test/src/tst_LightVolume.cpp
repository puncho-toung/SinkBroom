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

#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "mdl/TestFactory.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/mat_io.h" // IWYU pragma: keep
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

constexpr auto Tolerance = 1e-3;
constexpr auto Exact = 1e-6;

const auto defaultPoint = LightProperties{};

// the engine's DefaultProjected
const auto defaultSpot = LightProperties{
  .type = LightType::Spot,
  .target = {0, 0, -256},
  .right = {-128, 0, 0},
  .up = {0, -128, 0},
  .explicitStartEnd = false,
  .start = {0, 0, 0},
  .end = {0, 0, -256},
};

const auto explicitSpot = LightProperties{
  .type = LightType::Spot,
  .target = {0, 0, -256},
  .right = {-128, 0, 0},
  .up = {0, -128, 0},
  .explicitStartEnd = true,
  .start = {0, 0, -32},
  .end = {0, 0, -200},
};

const auto sun = LightProperties{
  .type = LightType::Sun,
  .radius = {2048, 2048, 1024},
  .equalRadius = false,
};

const auto identity = vm::mat3x3d{};

double det(const vm::mat3x3d& m)
{
  return vm::dot(m[0], vm::cross(m[1], m[2]));
}

vm::mat3x3d columns(const vm::vec3d& c0, const vm::vec3d& c1, const vm::vec3d& c2)
{
  auto m = vm::mat3x3d{};
  m[0] = c0;
  m[1] = c1;
  m[2] = c2;
  return m;
}

vm::mat3x3d rotationOf(const std::vector<EntityProperty>& properties)
{
  return lightFrame(Entity{properties}, EntityRotationPolicy::IdTech4).R;
}

LightProperties withRight(LightProperties light, const vm::vec3d& right)
{
  light.right = right;
  return light;
}

// the moved box of the drag cases
vm::bbox3d box(const vm::vec3d& min, const vm::vec3d& max)
{
  return vm::bbox3d{min, max};
}

// the handle threshold's default (D53)
constexpr auto DefaultThreshold = 0.10;

/**
 * The distance of a ray intersection, which must exist.
 */
double distanceOf(const std::optional<double>& distance)
{
  REQUIRE(distance);
  return *distance;
}

/**
 * The light of an edit box, which must not be denied.
 */
LightProperties editedLight(const std::optional<LightProperties>& light)
{
  REQUIRE(light);
  return *light;
}

/**
 * The t at which 1 - smoothstep(t) is the given level: smoothstep(t) = 1 - level, whose
 * inverse is 1/2 - sin(asin(1 - 2 y) / 3).
 */
double legacyVisibleT(const double level)
{
  const auto y = 1.0 - level;
  return 0.5 - std::sin(std::asin(1.0 - 2.0 * y) / 3.0);
}

/**
 * An emitter spot of range 256 along local +x, with a cone of 60 by 60 degrees and the
 * size that auto range gives that range at intensity 1.
 */
LightProperties emitterSpot()
{
  auto light = makeEmitterSpot(256.0);
  light.emitter.size = 256.0 / 3.0;
  return light;
}

// the half width of emitterSpot()'s box across the beam: R sin(cone / 2), where its lit
// cone is widest (G0 section 9)
const auto emitterHalfWidth = 256.0 * std::sin(vm::to_radians(30.0));

} // namespace

TEST_CASE("LightVolume")
{
  SECTION("lightFrame")
  {
    const auto frame = lightFrame(
      Entity{{{"classname", "light"}, {"origin", "1 2 3"}}},
      EntityRotationPolicy::IdTech4);
    CHECK(frame.origin == vm::vec3d{1, 2, 3});
    CHECK(frame.R == identity);

    // light_origin wins over origin
    CHECK(
      lightFrame(
        Entity{{{"classname", "light"}, {"origin", "1 2 3"}, {"light_origin", "4 5 6"}}},
        EntityRotationPolicy::IdTech4)
        .origin
      == vm::vec3d{4, 5, 6});

    // angles 0 90 0 turns +X to +Y
    const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
    CHECK(R * vm::vec3d{1, 0, 0} == vm::approx{vm::vec3d{0, 1, 0}, Exact});

    // the policy is passed: only the id Tech 4 policy reads light_rotation
    const auto lightRotation =
      Entity{{{"classname", "light"}, {"light_rotation", "0 1 0 -1 0 0 0 0 1"}}};
    CHECK(
      lightFrame(lightRotation, EntityRotationPolicy::IdTech4).R * vm::vec3d{1, 0, 0}
      == vm::approx{vm::vec3d{0, 1, 0}, Exact});
    CHECK(lightFrame(lightRotation, EntityRotationPolicy::Quake).R == identity);

    // a rotation that is not finite is none, like an angle that is not read
    const auto unturned = [](
                            const std::string& key,
                            const std::string& value,
                            const EntityRotationPolicy policy) {
      return lightFrame(Entity{{{"classname", "light"}, {key, value}}}, policy).R
             == identity;
    };
    CHECK(unturned("angle", "nan", EntityRotationPolicy::IdTech4));
    CHECK(unturned("angle", "inf", EntityRotationPolicy::IdTech4));
    CHECK(unturned("angles", "0 nan 0", EntityRotationPolicy::IdTech4));
    CHECK(unturned("angle", "nan", EntityRotationPolicy::Quake));
    CHECK(unturned("angle", "inf", EntityRotationPolicy::Quake));
    CHECK(unturned("angles", "0 nan 0", EntityRotationPolicy::Quake));
  }

  SECTION("operator<<(LightElement)")
  {
    const auto name = [](const LightElement element) {
      auto str = std::ostringstream{};
      str << element;
      return str.str();
    };
    CHECK(name(LightElement::OuterShell) == "OuterShell");
    CHECK(name(LightElement::InnerShell) == "InnerShell");
    CHECK(name(LightElement::Funnel) == "Funnel");
    CHECK(name(LightElement::Base) == "Base");
    CHECK(name(LightElement::InnerCap) == "InnerCap");
    CHECK(name(LightElement::Penumbra) == "Penumbra");
    CHECK(name(LightElement::SunBox) == "SunBox");
    CHECK(name(LightElement::Band) == "Band");
    CHECK(name(LightElement::NearCap) == "NearCap");
  }

  SECTION("pointLightLocalBox")
  {
    CHECK(pointLightLocalBox(defaultPoint) == vm::bbox3d{300.0});
    CHECK(
      pointLightLocalBox(sun)
      == vm::bbox3d{vm::vec3d{-2048, -2048, -1024}, vm::vec3d{2048, 2048, 1024}});
  }

  SECTION("spotLightFrustum")
  {
    SECTION("The default spot light")
    {
      const auto frustum = spotLightFrustum(defaultSpot);
      REQUIRE(frustum);
      CHECK(frustum->n == vm::approx{vm::vec3d{0, 0, -1}, Exact});
      CHECK(frustum->zNear == Catch::Approx(8.0));
      CHECK(frustum->zFar == Catch::Approx(256.0));

      // (u, v) = (-1, -1), (1, -1), (1, 1), (-1, 1); right is -X and up is -Y
      CHECK(frustum->nearCorners[0] == vm::approx{vm::vec3d{4, 4, -8}, Tolerance});
      CHECK(frustum->nearCorners[1] == vm::approx{vm::vec3d{-4, 4, -8}, Tolerance});
      CHECK(frustum->nearCorners[2] == vm::approx{vm::vec3d{-4, -4, -8}, Tolerance});
      CHECK(frustum->nearCorners[3] == vm::approx{vm::vec3d{4, -4, -8}, Tolerance});

      // the far cap is at zNear + zFar = 264
      CHECK(frustum->farCorners[0] == vm::approx{vm::vec3d{132, 132, -264}, Tolerance});
      CHECK(frustum->farCorners[1] == vm::approx{vm::vec3d{-132, 132, -264}, Tolerance});
      CHECK(frustum->farCorners[2] == vm::approx{vm::vec3d{-132, -132, -264}, Tolerance});
      CHECK(frustum->farCorners[3] == vm::approx{vm::vec3d{132, -132, -264}, Tolerance});
    }

    SECTION("Explicit start and end points")
    {
      const auto frustum = spotLightFrustum(explicitSpot);
      REQUIRE(frustum);
      CHECK(frustum->zNear == Catch::Approx(32.0));
      CHECK(frustum->zFar == Catch::Approx(200.0));
      CHECK(frustum->nearCorners[2] == vm::approx{vm::vec3d{-16, -16, -32}, Tolerance});
      CHECK(frustum->farCorners[2] == vm::approx{vm::vec3d{-116, -116, -232}, Tolerance});
    }

    SECTION("zNear above zFar is valid")
    {
      auto light = explicitSpot;
      light.start = vm::vec3d{0, 0, -100};
      light.end = vm::vec3d{0, 0, -20};

      const auto frustum = spotLightFrustum(light);
      REQUIRE(frustum);
      CHECK(frustum->zNear == Catch::Approx(100.0));
      CHECK(frustum->zFar == Catch::Approx(20.0));
      CHECK(frustum->farCorners[0].z() == Catch::Approx(-120.0));
    }

    SECTION("The engine's minima apply to explicit start and end points")
    {
      // SPOT_LIGHT_MIN_Z_NEAR 8 and SPOT_LIGHT_MIN_Z_FAR 16 of
      // R_ComputeSpotLightProjectionMatrix
      auto light = explicitSpot;
      light.start = vm::vec3d{0, 0, 50};
      light.end = vm::vec3d{0, 0, -10};

      const auto frustum = spotLightFrustum(light);
      REQUIRE(frustum);
      CHECK(frustum->zNear == 8.0);
      CHECK(frustum->zFar == 16.0);
      for (const auto& corner : frustum->farCorners)
      {
        CHECK(vm::dot(corner, frustum->n) == Catch::Approx(24.0));
      }
      CHECK(frustum->farCorners[2] == vm::approx{vm::vec3d{-12, -12, -24}, Tolerance});
    }

    SECTION("Degenerate spot lights have no frustum")
    {
      auto noTarget = defaultSpot;
      noTarget.target = vm::vec3d{0, 0, 0};
      CHECK(spotLightFrustum(noTarget) == std::nullopt);
      CHECK(beamFrame(noTarget) == std::nullopt);
      CHECK(lightEditBox(noTarget) == std::nullopt);
      CHECK(lightFromEditBox(noTarget, identity, vm::bbox3d{100.0}) == std::nullopt);

      const auto rightIsUp = withRight(defaultSpot, defaultSpot.up);
      CHECK(spotLightFrustum(rightIsUp) == std::nullopt);
      CHECK(beamFrame(rightIsUp) == std::nullopt);
      CHECK(lightEditBox(rightIsUp) == std::nullopt);

      const auto noRight = withRight(defaultSpot, vm::vec3d{0, 0, 0});
      CHECK(spotLightFrustum(noRight) == std::nullopt);
      CHECK(beamFrame(noRight) == std::nullopt);

      auto noUp = defaultSpot;
      noUp.up = vm::vec3d{0, 0, 0};
      CHECK(spotLightFrustum(noUp) == std::nullopt);

      // right along the beam
      const auto rightAlongBeam = withRight(defaultSpot, vm::vec3d{0, 0, -128});
      CHECK(spotLightFrustum(rightAlongBeam) == std::nullopt);
    }
  }

  SECTION("beamFrame")
  {
    SECTION("The default spot light")
    {
      const auto frame = beamFrame(defaultSpot);
      REQUIRE(frame);
      CHECK(frame->r == vm::approx{vm::vec3d{-1, 0, 0}, Exact});
      CHECK(frame->u == vm::approx{vm::vec3d{0, 1, 0}, Exact});
      CHECK(frame->n == vm::approx{vm::vec3d{0, 0, -1}, Exact});
      CHECK(frame->upSign == -1.0);

      // a proper rotation, also in the world
      const auto B = columns(frame->r, frame->u, frame->n);
      CHECK(det(B) == Catch::Approx(1.0));
      CHECK(det(identity * B) == Catch::Approx(1.0));
      const auto R = rotationOf({{"classname", "light"}, {"angles", "30 45 60"}});
      CHECK(det(R * B) == Catch::Approx(1.0));
    }

    SECTION("upSign follows up")
    {
      auto light = defaultSpot;
      light.up = vm::vec3d{0, 128, 0};
      const auto frame = beamFrame(light);
      REQUIRE(frame);
      CHECK(frame->u == vm::approx{vm::vec3d{0, 1, 0}, Exact});
      CHECK(frame->upSign == 1.0);
    }

    SECTION("A right that is nearly parallel to the beam")
    {
      // |right - (right * n) n| is below 1e-6 but the determinant is not
      const auto light = withRight(defaultSpot, vm::vec3d{1e-7, 0, -5});
      const auto frame = beamFrame(light);
      REQUIRE(frame);
      // the normalized cross product of n and the axis of its smallest component, X
      CHECK(frame->r == vm::approx{vm::vec3d{0, -1, 0}, Exact});
      CHECK(frame->u == vm::approx{vm::vec3d{-1, 0, 0}, Exact});
      CHECK(frame->upSign == 1.0);
      CHECK(det(columns(frame->r, frame->u, frame->n)) == Catch::Approx(1.0));
    }
  }

  SECTION("isCentredSpot")
  {
    CHECK(isCentredSpot(defaultSpot));

    // right turned 30 degrees towards the beam
    const auto skewed = withRight(
      defaultSpot,
      128.0
        * vm::vec3d{-std::cos(vm::to_radians(30.0)), 0, -std::sin(vm::to_radians(30.0))});
    CHECK_FALSE(isCentredSpot(skewed));

    // a two decimal value read back after a lateral drag
    const auto almost = withRight(defaultSpot, vm::vec3d{-128, 0, -0.0025});
    CHECK(vm::dot(almost.right, vm::vec3d{0, 0, -1}) == Catch::Approx(0.0025));
    CHECK(isCentredSpot(almost));

    auto upSkewed = defaultSpot;
    upSkewed.up = vm::vec3d{0, -128, -10};
    CHECK_FALSE(isCentredSpot(upSkewed));

    auto rightNotOrthogonalToUp = defaultSpot;
    rightNotOrthogonalToUp.right = vm::vec3d{-128, -10, 0};
    CHECK_FALSE(isCentredSpot(rightNotOrthogonalToUp));
  }

  SECTION("spotPoint")
  {
    SECTION("A centred spot light reduces to P (d / (P * n))")
    {
      const auto n = vm::vec3d{0, 0, -1};
      for (const auto u : {-1.0, -0.5, 0.0, 0.25, 1.0})
      {
        for (const auto v : {-1.0, 0.0, 0.75, 1.0})
        {
          for (const auto d : {8.0, 100.0, 264.0})
          {
            CAPTURE(u, v, d);
            const auto P =
              defaultSpot.target + u * defaultSpot.right + v * defaultSpot.up;
            CHECK(
              spotPoint(defaultSpot, u, v, d)
              == vm::approx{P * (d / vm::dot(P, n)), Exact});
          }
        }
      }
    }

    SECTION("The corners of an off-centre spot light solve the row system")
    {
      const auto light = withRight(
        defaultSpot,
        128.0
          * vm::vec3d{
            -std::cos(vm::to_radians(30.0)), 0, -std::sin(vm::to_radians(30.0))});
      const auto frustum = spotLightFrustum(light);
      REQUIRE(frustum);

      const auto targetLength = vm::length(light.target);
      const auto d = frustum->zNear + frustum->zFar;
      const auto uvs = std::vector<std::pair<double, double>>{
        {-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
      for (size_t i = 0; i < 4; ++i)
      {
        const auto [u, v] = uvs[i];
        const auto& p = frustum->farCorners[i];
        CAPTURE(i, p);
        CHECK(
          vm::dot(light.right, p)
          == Catch::Approx(
               d
               * (vm::dot(light.right, light.target) + u * vm::squared_length(light.right))
               / targetLength)
               .margin(Exact));
        CHECK(
          vm::dot(light.up, p)
          == Catch::Approx(
               d * (vm::dot(light.up, light.target) + v * vm::squared_length(light.up))
               / targetLength)
               .margin(Exact));
        CHECK(vm::dot(frustum->n, p) == Catch::Approx(d).margin(Exact));
      }

      // not the corners of the centred formula
      const auto P = light.target + light.right + light.up;
      CHECK(
        frustum->farCorners[2]
        != vm::approx{P * (d / vm::dot(P, frustum->n)), Tolerance});
    }

    SECTION("A degenerate spot light gives the apex")
    {
      auto noTarget = defaultSpot;
      noTarget.target = vm::vec3d{0, 0, 0};
      CHECK(spotPoint(noTarget, 1.0, 1.0, 100.0) == vm::vec3d{0, 0, 0});
      CHECK(
        spotPoint(withRight(defaultSpot, defaultSpot.up), 1.0, -1.0, 100.0)
        == vm::vec3d{0, 0, 0});
    }
  }

  SECTION("lightEditBox")
  {
    CHECK(lightEditBox(defaultPoint) == vm::bbox3d{300.0});
    CHECK(
      lightEditBox(sun)
      == vm::bbox3d{vm::vec3d{-2048, -2048, -1024}, vm::vec3d{2048, 2048, 1024}});

    const auto defaultBox = lightEditBox(defaultSpot);
    REQUIRE(defaultBox);
    CHECK(
      vm::approx{vm::bbox3d{vm::vec3d{-132, -132, 0}, vm::vec3d{132, 132, 264}}, Exact}
      == *defaultBox);

    const auto explicitBox = lightEditBox(explicitSpot);
    REQUIRE(explicitBox);
    CHECK(
      vm::approx{vm::bbox3d{vm::vec3d{-116, -116, 0}, vm::vec3d{116, 116, 232}}, Exact}
      == *explicitBox);

    // a radius component below 1e-6 is degenerate
    auto flat = defaultPoint;
    flat.radius = vm::vec3d{300, 300, 0};
    CHECK(lightEditBox(flat) == std::nullopt);

    SECTION("With a threshold, a point light's box is where it fades out (D61)")
    {
      // a texture light, by the smooth stand-in
      const auto legacyT = legacyVisibleT(DefaultThreshold);
      const auto legacyBox = lightEditBox(defaultPoint, DefaultThreshold);
      REQUIRE(legacyBox);
      CHECK(vm::approx{vm::bbox3d{300.0 * legacyT}, 1e-4} == *legacyBox);

      // per axis
      auto ellipsoid = defaultPoint;
      ellipsoid.radius = vm::vec3d{100, 200, 300};
      CHECK(
        vm::approx{
          vm::bbox3d{
            vm::vec3d{-100, -200, -300} * legacyT, vm::vec3d{100, 200, 300} * legacyT},
          1e-4}
        == *lightEditBox(ellipsoid, DefaultThreshold));

      // spot lights and suns keep their full box (Q15)
      CHECK(lightEditBox(defaultSpot, DefaultThreshold) == lightEditBox(defaultSpot));
      CHECK(lightEditBox(sun, DefaultThreshold) == lightEditBox(sun));

      CHECK(lightEditBox(flat, DefaultThreshold) == std::nullopt);
    }
  }

  SECTION("visibleFalloffT")
  {
    // every light by the smooth stand-in
    CHECK(
      visibleFalloffT(defaultPoint, DefaultThreshold)
      == Catch::Approx(legacyVisibleT(DefaultThreshold)).margin(Exact));
    CHECK(
      visibleFalloffT(defaultPoint, DefaultThreshold)
      == Catch::Approx(0.8042).margin(1e-4));
    CHECK(visibleFalloffT(defaultPoint, 0.5) == Catch::Approx(0.5).margin(Exact));

    SECTION("The threshold is clamped to [0.01, 0.5]")
    {
      CHECK(visibleFalloffT(defaultPoint, 0.9) == Catch::Approx(0.5).margin(Exact));
      CHECK(
        visibleFalloffT(defaultPoint, 0.001)
        == Catch::Approx(legacyVisibleT(0.01)).margin(Exact));
    }
  }

  SECTION("lightFromEditBox")
  {
    SECTION("Round trips")
    {
      // the default point light: an edit that gives the base is not a denial
      const auto same =
        lightFromEditBox(defaultPoint, identity, *lightEditBox(defaultPoint));
      REQUIRE(same);
      CHECK(*same == defaultPoint);

      // a rotated point light
      auto rotatedPoint = defaultPoint;
      rotatedPoint.origin = vm::vec3d{64, 32, 16};
      rotatedPoint.radius = vm::vec3d{300, 200, 100};
      rotatedPoint.equalRadius = false;
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto rotatedResult =
        editedLight(lightFromEditBox(rotatedPoint, R, *lightEditBox(rotatedPoint)));
      CHECK(rotatedResult.origin == vm::approx{rotatedPoint.origin, Exact});
      CHECK(rotatedResult.radius == vm::approx{rotatedPoint.radius, Exact});
    }

    SECTION("An off-centre box moves a rotated point light in its own frame")
    {
      auto rotatedPoint = defaultPoint;
      rotatedPoint.origin = vm::vec3d{64, 32, 16};
      rotatedPoint.radius = vm::vec3d{300, 200, 100};
      rotatedPoint.equalRadius = false;

      // angles 0 90 0 turn local +X to world +Y and local +Y to world -X
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto offset = vm::vec3d{10, 20, 30};
      const auto box = lightEditBox(rotatedPoint)->translate(offset);

      const auto moved = editedLight(lightFromEditBox(rotatedPoint, R, box));
      CHECK(moved.origin == vm::approx{vm::vec3d{64 - 20, 32 + 10, 16 + 30}, Exact});
      CHECK(moved.radius == vm::approx{rotatedPoint.radius, Exact});

      // and back
      const auto back =
        editedLight(lightFromEditBox(moved, R, lightEditBox(moved)->translate(-offset)));
      CHECK(back.origin == vm::approx{rotatedPoint.origin, Exact});
      CHECK(back.radius == vm::approx{rotatedPoint.radius, Exact});
    }

    SECTION("Round trips of spot lights")
    {

      // the default spot light
      const auto defaultBox = *lightEditBox(defaultSpot);
      const auto spot = editedLight(lightFromEditBox(defaultSpot, identity, defaultBox));
      CHECK(vm::approx{defaultBox, Exact} == *lightEditBox(spot));
      CHECK(spot.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK(spot.target == vm::approx{defaultSpot.target, Exact});
      CHECK(spot.right == vm::approx{defaultSpot.right, Exact});
      CHECK(spot.up == vm::approx{defaultSpot.up, Exact});
      CHECK_FALSE(spot.explicitStartEnd);

      // the explicit spot light: the same lit volume with a shorter target
      const auto explicitBox = *lightEditBox(explicitSpot);
      const auto explicitResult =
        editedLight(lightFromEditBox(explicitSpot, identity, explicitBox));
      CHECK(vm::approx{explicitBox, Exact} == *lightEditBox(explicitResult));
      CHECK(explicitResult.explicitStartEnd);
      CHECK(explicitResult.start == vm::approx{explicitSpot.start, Exact});
      CHECK(explicitResult.end == vm::approx{explicitSpot.end, Exact});
      CHECK(explicitResult.target == vm::approx{vm::vec3d{0, 0, -200}, Exact});
      CHECK(explicitResult.right == vm::approx{vm::vec3d{-100, 0, 0}, Exact});
      CHECK(explicitResult.up == vm::approx{vm::vec3d{0, -100, 0}, Exact});
    }

    SECTION("(a) A far cap edge dragged along the box's +x by 32")
    {
      // the box's +x is the beam frame's r, world -X for the default spot
      const auto light = editedLight(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {164, 132, 264})));
      CHECK(vm::length(light.right) == Catch::Approx(143.5234).margin(Tolerance));
      CHECK(light.target == vm::approx{vm::vec3d{-15.516, 0, -256.015}, Tolerance});
      CHECK(vm::length(light.up) == Catch::Approx(128.0073).margin(Tolerance));
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK(vm::length(light.target) == Catch::Approx(256.484).margin(Tolerance));
      CHECK(light.up.y() < 0.0);
    }

    SECTION("(a) The world +X face dragged by 32")
    {
      const auto light = editedLight(
        lightFromEditBox(defaultSpot, identity, box({-164, -132, 0}, {132, 132, 264})));
      CHECK(light.target == vm::approx{vm::vec3d{15.516, 0, -256.015}, Tolerance});
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
    }

    SECTION("(b) The apex face dragged by 16")
    {
      const auto light = editedLight(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 16}, {132, 132, 264})));
      CHECK(light.origin == vm::approx{vm::vec3d{0, 0, -16}, Exact});
      CHECK(light.target == vm::approx{vm::vec3d{0, 0, -240}, Tolerance});
      CHECK(vm::length(light.right) == Catch::Approx(127.7419).margin(Tolerance));
      CHECK(vm::length(light.up) == Catch::Approx(127.7419).margin(Tolerance));
      CHECK(vm::length(light.right) == Catch::Approx(132.0 * 240.0 / 248.0));

      // the far cap stays at world -264
      const auto frustum = spotLightFrustum(light);
      REQUIRE(frustum);
      CHECK(light.origin.z() - frustum->zNear - frustum->zFar == Catch::Approx(-264.0));
    }

    SECTION("(b) The apex face of a rotated light moves along the world beam")
    {
      // angles 0 90 0 turns the beam, local -Z, to world -Z: try a pitch instead
      const auto R = rotationOf({{"classname", "light"}, {"angles", "90 0 0"}});
      const auto light = editedLight(
        lightFromEditBox(defaultSpot, R, box({-132, -132, 16}, {132, 132, 264})));
      CHECK(light.origin == vm::approx{16.0 * (R * vm::vec3d{0, 0, -1}), Exact});
    }

    SECTION("(c) The far face dragged by 64")
    {
      const auto light = editedLight(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 328})));
      CHECK(light.target == vm::approx{vm::vec3d{0, 0, -320}, Tolerance});
      CHECK(vm::length(light.right) == Catch::Approx(128.7805).margin(Tolerance));
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK_FALSE(light.explicitStartEnd);
    }

    SECTION("(c') The far face of an explicit spot light dragged by 64")
    {
      const auto light = editedLight(
        lightFromEditBox(explicitSpot, identity, box({-116, -116, 0}, {116, 116, 296})));
      CHECK(light.explicitStartEnd);
      CHECK(light.end == vm::approx{vm::vec3d{0, 0, -264}, Tolerance});
      CHECK(light.start == vm::approx{vm::vec3d{0, 0, -32}, Tolerance});
      CHECK(light.target == vm::approx{vm::vec3d{0, 0, -264}, Tolerance});
      CHECK(vm::length(light.right) == Catch::Approx(103.4595).margin(Tolerance));
      CHECK(light.origin == vm::approx{explicitSpot.origin, Exact});
    }

    SECTION("(d) A point light's side dragged along +x by 32")
    {
      const auto light = editedLight(lightFromEditBox(
        defaultPoint, identity, box({-300, -300, -300}, {332, 300, 300})));
      CHECK(light.origin == vm::approx{vm::vec3d{16, 0, 0}, Exact});
      CHECK(light.radius == vm::approx{vm::vec3d{316, 300, 300}, Exact});
      CHECK_FALSE(light.equalRadius);

      // the centre moves in the light's frame
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto rotated = editedLight(
        lightFromEditBox(defaultPoint, R, box({-300, -300, -300}, {332, 300, 300})));
      CHECK(rotated.origin == vm::approx{vm::vec3d{0, 16, 0}, Exact});
    }

    SECTION("Denied boxes return nothing")
    {
      // depth 23
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 23}))
        == std::nullopt);
      // half extent 0.5
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-0.5, -132, 0}, {0.5, 132, 264}))
        == std::nullopt);
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-132, -0.5, 0}, {132, 0.5, 264}))
        == std::nullopt);
      CHECK(
        lightFromEditBox(defaultPoint, identity, box({-300, -300, -0.5}, {300, 300, 0.5}))
        == std::nullopt);

      // depth 24 is allowed
      CHECK(
        editedLight(
          lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 24})))
        != defaultSpot);
    }

    SECTION("With a threshold, the handle box divides by the visible t (D61)")
    {
      SECTION("Round trips")
      {
        for (const auto& light : {defaultPoint})
        {
          const auto handleBox = lightEditBox(light, DefaultThreshold);
          REQUIRE(handleBox);
          const auto result =
            editedLight(lightFromEditBox(light, identity, *handleBox, DefaultThreshold));
          CHECK(result.origin == vm::approx{light.origin, Exact});
          CHECK(result.radius == vm::approx{light.radius, Exact});
          CHECK(result.equalRadius);
        }

        // spot lights and suns as without a threshold
        CHECK(
          editedLight(lightFromEditBox(
            defaultSpot, identity, *lightEditBox(defaultSpot), DefaultThreshold))
          == lightFromEditBox(defaultSpot, identity, *lightEditBox(defaultSpot)));
        CHECK(
          editedLight(
            lightFromEditBox(sun, identity, *lightEditBox(sun), DefaultThreshold))
          == lightFromEditBox(sun, identity, *lightEditBox(sun)));
      }

      SECTION("A point light's +x face dragged by 32")
      {
        const auto t = legacyVisibleT(DefaultThreshold);
        const auto h = 300.0 * t;

        // the scale tool's face drag: the +x face follows the handle by 32, the -x face
        // stays, so the half extent grows by 16 and the radius by 16 / t; the centre
        // follows by 16
        const auto light = editedLight(lightFromEditBox(
          defaultPoint, identity, box({-h, -h, -h}, {h + 32, h, h}), DefaultThreshold));
        CHECK(light.radius.x() == Catch::Approx(300.0 + 16.0 / t).margin(Tolerance));
        CHECK(light.radius.y() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(light.radius.z() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(light.origin == vm::approx{vm::vec3d{16, 0, 0}, Tolerance});
        CHECK_FALSE(light.equalRadius);

        // both x faces by 32 (anchored at the centre): the radius grows by 32 / t, the
        // centre stays
        const auto centred = editedLight(lightFromEditBox(
          defaultPoint,
          identity,
          box({-h - 32, -h, -h}, {h + 32, h, h}),
          DefaultThreshold));
        CHECK(centred.radius.x() == Catch::Approx(300.0 + 32.0 / t).margin(Tolerance));
        CHECK(centred.radius.y() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(centred.origin == vm::approx{vm::vec3d{0, 0, 0}, Exact});
      }

      SECTION("A half extent below 1 is denied")
      {
        const auto h = 300.0 * legacyVisibleT(DefaultThreshold);
        CHECK(
          lightFromEditBox(
            defaultPoint, identity, box({-h, -h, -0.5}, {h, h, 0.5}), DefaultThreshold)
          == std::nullopt);
      }
    }
  }

  SECTION("spotProjectedUV")
  {
    // the corners of the default spot light's caps, in the order of spotLightFrustum
    const auto frustum = spotLightFrustum(defaultSpot);
    REQUIRE(frustum);
    const auto cornerUVs = std::array<vm::vec2d, 4>{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (std::size_t i = 0; i < 4; ++i)
    {
      CAPTURE(i);
      for (const auto& corner : {frustum->nearCorners[i], frustum->farCorners[i]})
      {
        const auto uv = spotProjectedUV(defaultSpot, corner);
        REQUIRE(uv);
        CHECK(vm::is_equal(*uv, cornerUVs[i], Exact));
      }
    }

    // the beam, and the inverse of spotPoint off the corners of an off-centre light
    const auto onBeam = spotProjectedUV(defaultSpot, {0, 0, -84.8});
    REQUIRE(onBeam);
    CHECK(vm::is_equal(*onBeam, vm::vec2d{0, 0}, Exact));
    const auto offCentre = withRight(defaultSpot, {-128, 0, 30});
    const auto uv = spotProjectedUV(offCentre, spotPoint(offCentre, 0.3, -0.7, 100.0));
    REQUIRE(uv);
    CHECK(vm::is_equal(*uv, vm::vec2d{0.3, -0.7}, Exact));

    // nothing at the apex, behind it, or of a degenerate light
    CHECK(spotProjectedUV(defaultSpot, {0, 0, 0}) == std::nullopt);
    CHECK(spotProjectedUV(defaultSpot, {0, 0, 10}) == std::nullopt);
    CHECK(
      spotProjectedUV(withRight(defaultSpot, {0, 0, 0}), {0, 0, -100}) == std::nullopt);
  }

  SECTION("superellipseRadius")
  {
    CHECK(superellipseRadius(1, 0, 2) == Catch::Approx(1.0));
    CHECK(superellipseRadius(0.6, -0.8, 2) == Catch::Approx(1.0));
    CHECK(superellipseRadius(-0.5, 0.5, 2) == Catch::Approx(std::sqrt(0.5)));
    CHECK(superellipseRadius(1, 1, 4) == Catch::Approx(std::pow(2.0, 0.25)));

    // the exponent is at least 1e-3: with 0, |0|^0 = 1 would give an infinite radius
    CHECK(superellipseRadius(1, 0, 0) == Catch::Approx(1.0));
  }

  SECTION("containsPoint")
  {
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};
    CHECK(containsPoint(defaultPoint, atOrigin, vm::vec3d{0, 0, 0}));
    CHECK_FALSE(containsPoint(defaultPoint, atOrigin, vm::vec3d{400, 0, 0}));
    // inside the box, outside the ellipsoid
    CHECK_FALSE(containsPoint(defaultPoint, atOrigin, vm::vec3d{250, 250, 0}));

    CHECK(containsPoint(defaultSpot, atOrigin, vm::vec3d{0, 0, -100}));
    // behind the apex
    CHECK_FALSE(containsPoint(defaultSpot, atOrigin, vm::vec3d{0, 0, 100}));
    // beyond the far cap at 264
    CHECK_FALSE(containsPoint(defaultSpot, atOrigin, vm::vec3d{0, 0, -270}));
    // between the apex and the near cap
    CHECK_FALSE(containsPoint(defaultSpot, atOrigin, vm::vec3d{0, 0, -4}));
    // just inside and outside the side
    CHECK(containsPoint(defaultSpot, atOrigin, vm::vec3d{-60, 0, -128}));
    CHECK_FALSE(containsPoint(defaultSpot, atOrigin, vm::vec3d{-70, 0, -128}));

    CHECK(containsPoint(sun, atOrigin, vm::vec3d{2000, 2000, 1000}));
    CHECK_FALSE(containsPoint(sun, atOrigin, vm::vec3d{0, 0, 1100}));

    // the point is mapped into the light's frame
    auto longPoint = defaultPoint;
    longPoint.radius = vm::vec3d{300, 100, 100};
    const auto turned = LightFrame{
      vm::vec3d{1000, 0, 0}, rotationOf({{"classname", "light"}, {"angles", "0 90 0"}})};
    CHECK(containsPoint(longPoint, turned, vm::vec3d{1000, 250, 0}));
    CHECK_FALSE(containsPoint(longPoint, turned, vm::vec3d{1250, 0, 0}));

    SECTION("An off-centre spot light")
    {
      const auto skewed = withRight(
        defaultSpot,
        128.0
          * vm::vec3d{
            -std::cos(vm::to_radians(30.0)), 0, -std::sin(vm::to_radians(30.0))});
      REQUIRE_FALSE(isCentredSpot(skewed));

      CHECK(containsPoint(skewed, atOrigin, spotPoint(skewed, 0.99, 0.99, 200.0)));
      CHECK_FALSE(containsPoint(skewed, atOrigin, spotPoint(skewed, 1.05, 0.0, 200.0)));
    }

    SECTION("A turned spot light")
    {
      // a pitch of 90 turns the beam, local -Z, to world -X
      const auto R = rotationOf({{"classname", "light"}, {"angles", "90 0 0"}});
      REQUIRE(R * vm::vec3d{0, 0, -1} == vm::approx{vm::vec3d{-1, 0, 0}, Exact});
      const auto frame = LightFrame{vm::vec3d{1000, 0, 0}, R};

      CHECK(containsPoint(defaultSpot, frame, vm::vec3d{900, 0, 0}));
      // inside if the rotation were ignored
      CHECK_FALSE(containsPoint(defaultSpot, frame, vm::vec3d{1000, 0, -100}));
      CHECK_FALSE(containsPoint(defaultSpot, frame, vm::vec3d{1100, 0, 0}));
    }
  }

  SECTION("lightBoundingSphere")
  {
    // The renderer skips the exact test of a light whose sphere the camera is outside,
    // so every point that containsPoint accepts must lie within the radius of the
    // centre (RV-C-TP-04), also in a frame that is orthonormal only to the two decimals
    // that the engine writes a matrix key with, and in worse ones (RV2-TP-01).
    auto texturePoint = defaultPoint;
    texturePoint.radius = vm::vec3d{300, 200, 100};
    texturePoint.equalRadius = false;
    auto texturePointWithCentre = defaultPoint;
    texturePointWithCentre.hasCenter = true;
    texturePointWithCentre.center = vm::vec3d{100, 0, 0};
    auto textureSun = sun;
    textureSun.radius = vm::vec3d{300, 200, 100};
    auto omni = makeEmitterOmni();
    omni.radius = vm::vec3d{300, 200, 100};
    omni.equalRadius = false;
    omni.hasCenter = true;
    omni.center = vm::vec3d{150, -80, 60};
    auto spot = makeEmitterSpot(288.0);
    spot.emitter.coneH = 100.0;
    spot.emitter.coneV = 40.0;
    spot.emitter.squareness = 8.0;
    auto emitterSun = makeEmitterOmni();
    emitterSun.type = LightType::Sun;
    emitterSun.radius = vm::vec3d{300, 200, 100};
    emitterSun.equalRadius = false;
    emitterSun.hasCenter = true;
    emitterSun.center = vm::vec3d{0, 0, 1};

    const auto lights = std::vector<std::pair<std::string, LightProperties>>{
      {"texture point", texturePoint},
      {"texture point with a centre", texturePointWithCentre},
      {"texture spot", explicitSpot},
      {"texture sun", textureSun},
      {"emitter omni about its bulb", omni},
      {"emitter spot", spot},
      {"emitter sun", emitterSun},
    };

    // local points on and inside each volume's boundary, its farthest points among them
    const auto candidates = [](const LightProperties& light) {
      auto points = std::vector<vm::vec3d>{};
      const auto radius = vm::abs(light.radius);
      const auto bulb = light.emitter.enabled ? emitterBulb(light) : vm::vec3d{0, 0, 0};
      constexpr auto Directions = 60;
      for (auto i = 0; i < Directions; ++i)
      {
        // a Fibonacci sphere
        const auto z = 1.0 - 2.0 * (double(i) + 0.5) / double(Directions);
        const auto r = std::sqrt(1.0 - z * z);
        const auto phi = 2.399963229728653 * double(i);
        const auto d = vm::vec3d{r * std::cos(phi), r * std::sin(phi), z};
        const auto onBox = d / vm::get_max_component(vm::abs(d));
        for (const auto s : {0.9, 0.999999, 1.0})
        {
          points.push_back(bulb + s * radius * d);
          points.push_back(s * radius * onBox);
          points.push_back(s * emitterRange(light.radius) * d);
        }
      }
      if (light.emitter.enabled && light.type == LightType::Spot)
      {
        // the rim of the cone on the sphere of the range
        const auto tanH = emitterTanHalf(light.emitter.coneH);
        const auto tanV = emitterTanHalf(light.emitter.coneV);
        for (auto i = 0; i < 64; ++i)
        {
          const auto a = 2.0 * std::numbers::pi * double(i) / 64.0;
          const auto k = superellipseRadius(std::cos(a), std::sin(a), 8.0);
          const auto u = 0.999 * std::cos(a) / k;
          const auto v = 0.999 * std::sin(a) / k;
          const auto d = vm::normalize(vm::vec3d{1.0, u * tanH, v * tanV});
          for (const auto s : {0.5, 0.999999, 1.0})
          {
            points.push_back(s * emitterRange(light.radius) * d);
          }
        }
      }
      if (!light.emitter.enabled && light.type == LightType::Spot)
      {
        if (const auto frustum = spotLightFrustum(light))
        {
          for (const auto u : {-0.999999, -0.5, 0.0, 0.5, 0.999999})
          {
            for (const auto v : {-0.999999, -0.5, 0.0, 0.5, 0.999999})
            {
              for (const auto t : {0.0, 0.25, 0.5, 0.75, 1.0})
              {
                const auto d = frustum->zNear + t * frustum->zFar;
                points.push_back(spotPoint(light, u, v, d));
              }
            }
          }
        }
      }
      return points;
    };

    const auto rounded = [](const vm::mat3x3d& R, const double scale) {
      auto result = R;
      for (std::size_t c = 0; c < 3; ++c)
      {
        for (std::size_t r = 0; r < 3; ++r)
        {
          result[c][r] = std::round(R[c][r] * scale) / scale;
        }
      }
      return result;
    };
    const auto perturbed = [](const vm::mat3x3d& R) {
      auto result = R;
      for (std::size_t c = 0; c < 3; ++c)
      {
        for (std::size_t r = 0; r < 3; ++r)
        {
          result[c][r] += 0.02 * std::sin(double(3 * c + r) * 1.7 + R[c][r]);
        }
      }
      return result;
    };

    const auto origin = vm::vec3d{16, -32, 8};
    auto boundedRounded = 0;
    for (const auto& [name, light] : lights)
    {
      const auto points = candidates(light);
      auto inside = 0;
      for (auto yaw = 0.0; yaw < 360.0; yaw += 45.0)
      {
        for (const auto pitch : {-90.0, -50.0, 0.0, 35.0, 90.0})
        {
          for (const auto roll : {0.0, 40.0})
          {
            const auto exact = vm::slice<3, 3>(
              vm::rotation_matrix(
                vm::to_radians(roll), vm::to_radians(pitch), vm::to_radians(yaw)),
              0,
              0);
            const auto frames = std::vector<std::pair<std::string, vm::mat3x3d>>{
              {"exact", exact},
              {"two decimals", rounded(exact, 100.0)},
              {"one decimal", rounded(exact, 10.0)},
              {"perturbed by 0.02", perturbed(exact)},
              {"scaled by 1.04", exact * 1.04},
            };
            for (const auto& [frameName, R] : frames)
            {
              CAPTURE(name, yaw, pitch, roll, frameName);
              const auto frame = LightFrame{origin, R};
              const auto [centre, reach] = lightBoundingSphere(light, frame);

              // an emitter omni's centre is its bulb in the world
              CHECK(
                centre
                == (light.emitter.enabled ? origin + R * emitterBulb(light) : origin));
              if (!std::isfinite(reach))
              {
                continue;
              }
              if (frameName == "two decimals")
              {
                ++boundedRounded;
              }

              // the world point whose local point (R^T (world - origin)) is the given one
              const auto inverse = vm::invert(vm::transpose(R));
              REQUIRE(inverse);
              for (const auto& local : points)
              {
                const auto world = origin + *inverse * local;
                if (containsPoint(light, frame, world))
                {
                  ++inside;
                  if (vm::squared_distance(centre, world) > reach * reach)
                  {
                    FAIL_CHECK("outside the reach: " << local);
                  }
                }
                // and the mesh's placing of it, origin + R * local
                if (containsPoint(light, LightFrame{{0, 0, 0}, identity}, local))
                {
                  const auto placed = origin + R * local;
                  if (vm::squared_distance(centre, placed) > reach * reach)
                  {
                    FAIL_CHECK("placed outside the reach: " << local);
                  }
                }
              }
            }
          }
        }
      }
      CAPTURE(name);
      CHECK(inside > 0);
    }
    // a two-decimal frame keeps its bound, so the renderer keeps its shortcut
    CHECK(boundedRounded == 7 * 8 * 5 * 2);

    SECTION("a frame that is not near a rotation has no bound")
    {
      const auto [centre, reach] = lightBoundingSphere(
        texturePoint, LightFrame{vm::vec3d{0, 0, 0}, vm::mat3x3d::identity() * 0.5});
      CHECK(reach == std::numeric_limits<double>::infinity());
    }

    SECTION("a rotation's sphere is the volume's, a rounded one's slightly wider")
    {
      const auto R = rotationOf({{"classname", "light"}, {"angles", "20 35 10"}});
      CHECK(
        lightBoundingSphere(texturePoint, LightFrame{origin, R}).second
        == Catch::Approx(300.0).margin(1e-3));
      const auto [centre, reach] =
        lightBoundingSphere(texturePoint, LightFrame{origin, rounded(R, 100.0)});
      CHECK(reach > 300.0);
      CHECK(reach < 310.0);
    }
  }

  SECTION("lightFalloff")
  {
    const auto ts = std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0};
    const auto k = 2.0;

    const auto curveOf = [&](const LightFalloff falloff, const double inner = 0.0) {
      return LightCurve{.falloff = falloff, .falloffExp = k, .innerRadius = inner};
    };

    // the falloff curves of emitter.inc.hlsl, written out for inner = 0, where u = t
    const auto window = [](const double u) { return 1.0 - u * u * u * u; };
    const auto expected = [&](const LightFalloff falloff, const double u) {
      switch (falloff)
      {
      case LightFalloff::Linear:
        return 1.0 - u;
      case LightFalloff::Smooth:
        return 1.0 - u * u * (3.0 - 2.0 * u);
      case LightFalloff::Exponential:
        return window(u) * window(u) * std::exp(-k * u);
      case LightFalloff::InvSquareClamped:
        return window(u) * window(u) / ((u * k + 1e-3) * (u * k + 1e-3) + 1.0);
      case LightFalloff::InvSquare:
        return window(u) * window(u) / ((u * k + 1e-3) * (u * k + 1e-3));
      }
      return 0.0;
    };

    // the review's table
    const auto table = std::vector<std::pair<LightFalloff, std::vector<double>>>{
      {LightFalloff::Linear, {1.0, 0.75, 0.5, 0.25, 0.0}},
      {LightFalloff::Smooth, {1.0, 0.84375, 0.5, 0.15625, 0.0}},
      {LightFalloff::Exponential, {1.0, 0.6018, 0.3233, 0.1043, 0.0}},
      {LightFalloff::InvSquareClamped, {0.999999, 0.7931, 0.4390, 0.1437, 0.0}},
      {LightFalloff::InvSquare, {1e6, 3.9530, 0.8772, 0.2074, 0.0}},
    };

    for (const auto& [falloff, values] : table)
    {
      for (size_t i = 0; i < ts.size(); ++i)
      {
        const auto t = ts[i];
        CAPTURE(falloff, t);

        const auto actual = lightFalloff(t, curveOf(falloff));
        CHECK(actual == Catch::Approx(expected(falloff, t)).margin(Exact).epsilon(0));

        const auto atEnd = i == 0 || i == ts.size() - 1;
        CHECK(
          actual
          == Catch::Approx(values[i]).epsilon(0).margin(atEnd ? Exact : Tolerance));
      }
    }

    // inner shifts the curve: u = (t - inner) / (1 - inner)
    CHECK(lightFalloff(0.25, curveOf(LightFalloff::Linear, 0.5)) == 1.0);
    CHECK(lightFalloff(0.5, curveOf(LightFalloff::Linear, 0.5)) == 1.0);
    CHECK(
      lightFalloff(0.75, curveOf(LightFalloff::Linear, 0.5))
      == Catch::Approx(0.5).margin(Exact));
    CHECK(
      lightFalloff(0.75, curveOf(LightFalloff::Smooth, 0.5))
      == Catch::Approx(0.5).margin(Exact));
    CHECK(
      lightFalloff(0.875, curveOf(LightFalloff::InvSquareClamped, 0.5))
      == Catch::Approx(expected(LightFalloff::InvSquareClamped, 0.75)).margin(Exact));
    CHECK(lightFalloff(1.0, curveOf(LightFalloff::Exponential, 0.5)) == 0.0);

    // an inner radius of 1 is a hard edge
    CHECK(lightFalloff(0.999, curveOf(LightFalloff::Linear, 1.0)) == 1.0);
    CHECK(lightFalloff(1.0, curveOf(LightFalloff::Linear, 1.0)) == 1.0);

    // t outside 0..1 is saturated
    CHECK(lightFalloff(-1.0, curveOf(LightFalloff::Linear)) == 1.0);
    CHECK(lightFalloff(2.0, curveOf(LightFalloff::Linear)) == 0.0);
  }

  SECTION("legacyFalloff")
  {
    CHECK(legacyFalloff(0.0) == 1.0);
    CHECK(legacyFalloff(0.25) == Catch::Approx(0.84375).margin(Exact));
    CHECK(legacyFalloff(0.5) == Catch::Approx(0.5).margin(Exact));
    CHECK(legacyFalloff(0.75) == Catch::Approx(0.15625).margin(Exact));
    CHECK(legacyFalloff(1.0) == 0.0);
    CHECK(legacyFalloff(2.0) == 0.0);
  }

  SECTION("spotMask")
  {
    const auto mask = [](const double shapeExp, const double penumbra) {
      return LightCurve{.shapeExp = shapeExp, .penumbra = penumbra};
    };

    CHECK(spotMask(0.0, 0.0, mask(2.0, 0.15)) == 1.0);
    CHECK(spotMask(1.0, 0.0, mask(2.0, 0.15)) == Catch::Approx(0.0).margin(Exact));
    CHECK(spotMask(0.85, 0.0, mask(2.0, 0.15)) == Catch::Approx(1.0).margin(Exact));
    CHECK(spotMask(0.0, -0.85, mask(2.0, 0.15)) == Catch::Approx(1.0).margin(Exact));

    // 16 is nearly a square, 2 a circle
    CHECK(spotMask(0.8, 0.8, mask(16.0, 0.15)) == Catch::Approx(1.0).margin(Exact));
    CHECK(spotMask(0.8, 0.8, mask(2.0, 0.15)) == Catch::Approx(0.0).margin(Exact));

    // half way through the penumbra ring
    CHECK(spotMask(0.925, 0.0, mask(2.0, 0.15)) == Catch::Approx(0.5).margin(Exact));

    // a penumbra of 0 is a sharp edge, never NaN
    for (const auto& [u, v] : std::vector<std::pair<double, double>>{
           {0.0, 0.0}, {0.5, 0.0}, {0.999, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}})
    {
      CAPTURE(u, v);
      const auto value = spotMask(u, v, mask(2.0, 0.0));
      CHECK_FALSE(std::isnan(value));
      CHECK(value == (std::hypot(u, v) >= 1.0 ? 0.0 : 1.0));
    }
  }

  SECTION("spotFalloffT")
  {
    CHECK(spotFalloffT(8.0, 8.0, 256.0) == 0.0);
    CHECK(spotFalloffT(136.0, 8.0, 256.0) == 0.5);
    // exactly 1 at the far cap, zNear + zFar
    CHECK(spotFalloffT(264.0, 8.0, 256.0) == 1.0);
    CHECK(spotFalloffT(232.0, 32.0, 200.0) == 1.0);
  }

  SECTION("pointFalloffT")
  {
    const auto radius = vm::vec3d{300, 300, 300};
    CHECK(pointFalloffT(vm::vec3d{0, 0, 0}, radius) == 0.0);
    CHECK(pointFalloffT(vm::vec3d{150, 0, 0}, radius) == 0.5);
    CHECK(pointFalloffT(vm::vec3d{0, 0, -300}, radius) == 1.0);
    // an ellipsoid inside the box
    CHECK(pointFalloffT(vm::vec3d{0, 50, 0}, vm::vec3d{300, 100, 100}) == 0.5);
    CHECK(pointFalloffT(vm::vec3d{300, 300, 0}, radius) == Catch::Approx(std::sqrt(2.0)));
    CHECK(std::isinf(pointFalloffT(vm::vec3d{0, 0, 0}, vm::vec3d{300, 0, 300})));
  }

  SECTION("spotFromWorldBox")
  {
    const auto W = box({0, 0, 0}, {128, 128, 64});

    const auto light = spotFromWorldBox(W, defaultPoint);
    CHECK(light.type == LightType::Spot);
    CHECK(light.origin == vm::approx{vm::vec3d{64, 64, 64}, Exact});
    CHECK(light.target == vm::approx{vm::vec3d{0, 0, -56}, Exact});
    CHECK(light.right == vm::approx{vm::vec3d{-56, 0, 0}, Exact});
    CHECK(light.up == vm::approx{vm::vec3d{0, -56, 0}, Exact});
    CHECK_FALSE(light.explicitStartEnd);

    // the drawn box is the light's edit box, with the far cap on the bottom face
    CHECK(
      vm::approx{vm::bbox3d{vm::vec3d{-64, -64, 0}, vm::vec3d{64, 64, 64}}, Exact}
      == *lightEditBox(light));

    // an explicit spot keeps its zNear up to H - 16, and its far cap on the bottom face
    const auto explicitLight = spotFromWorldBox(W, explicitSpot);
    CHECK(explicitLight.explicitStartEnd);
    CHECK(explicitLight.start == vm::approx{vm::vec3d{0, 0, -32}, Exact});
    CHECK(explicitLight.target == vm::approx{vm::vec3d{0, 0, -32}, Exact});
    CHECK(explicitLight.end == vm::approx{vm::vec3d{0, 0, -32}, Exact});
    CHECK(explicitLight.right == vm::approx{vm::vec3d{-32, 0, 0}, Exact});
    CHECK(
      vm::approx{vm::bbox3d{vm::vec3d{-64, -64, 0}, vm::vec3d{64, 64, 64}}, Exact}
      == *lightEditBox(explicitLight));

    const auto lowBox = box({0, 0, 0}, {128, 128, 24});
    const auto lowExplicit = spotFromWorldBox(lowBox, explicitSpot);
    CHECK(lowExplicit.start == vm::approx{vm::vec3d{0, 0, -8}, Exact});
    CHECK(lowExplicit.target == vm::approx{vm::vec3d{0, 0, -16}, Exact});

    // a half extent below 1 is denied
    CHECK(spotFromWorldBox(box({0, 0, 0}, {1, 128, 64}), defaultPoint) == defaultPoint);
  }

  SECTION("pointFromWorldBox")
  {
    const auto W = box({0, 0, 0}, {128, 128, 64});

    const auto light = pointFromWorldBox(W, defaultSpot);
    CHECK(light.type == LightType::Point);
    CHECK(light.origin == vm::approx{vm::vec3d{64, 64, 32}, Exact});
    CHECK(light.radius == vm::approx{vm::vec3d{64, 64, 32}, Exact});
    CHECK_FALSE(light.equalRadius);
    CHECK(
      lightPropertyUpdate(pointFromWorldBox(W, defaultPoint)).set
      == lightPropertyUpdate(light).set);

    // a sun stays a sun
    CHECK(pointFromWorldBox(W, sun).type == LightType::Sun);

    const auto cube = pointFromWorldBox(box({0, 0, 0}, {64, 64, 64}), defaultPoint);
    CHECK(cube.equalRadius);

    // a half extent below 1 is denied
    CHECK(pointFromWorldBox(box({0, 0, 0}, {128, 128, 1}), defaultPoint) == defaultPoint);

    SECTION("With a threshold, the drawn box is where the light is visible (D61)")
    {
      const auto legacy = pointFromWorldBox(W, defaultPoint, DefaultThreshold);
      CHECK(legacy.origin == vm::approx{vm::vec3d{64, 64, 32}, Exact});
      CHECK(
        vm::approx{box({0, 0, 0}, {128, 128, 64}).translate(-legacy.origin), 1e-4}
        == *lightEditBox(legacy, DefaultThreshold));
      CHECK(
        legacy.radius
        == vm::approx{
          vm::vec3d{64, 64, 32} / legacyVisibleT(DefaultThreshold), Tolerance});

      // a sun fills the box
      CHECK(
        pointFromWorldBox(W, sun, DefaultThreshold).radius
        == vm::approx{vm::vec3d{64, 64, 32}, Exact});

      CHECK(
        pointFromWorldBox(box({0, 0, 0}, {128, 128, 1}), defaultPoint, DefaultThreshold)
        == defaultPoint);
    }
  }

  SECTION("intersectRayEllipsoid")
  {
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};
    const auto radius = defaultPoint.radius;

    // from outside, the near side
    CHECK(
      distanceOf(
        intersectRayEllipsoid(atOrigin, radius, 1.0, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(700.0).margin(Exact));
    // a shell at t = 0.5
    CHECK(
      distanceOf(
        intersectRayEllipsoid(atOrigin, radius, 0.5, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(850.0).margin(Exact));
    // from inside, the far side
    CHECK(
      distanceOf(
        intersectRayEllipsoid(atOrigin, radius, 1.0, vm::ray3d{{0, 0, 0}, {1, 0, 0}}))
      == Catch::Approx(300.0).margin(Exact));
    // a miss, and an ellipsoid behind the ray
    CHECK(
      intersectRayEllipsoid(atOrigin, radius, 1.0, vm::ray3d{{1000, 400, 0}, {-1, 0, 0}})
      == std::nullopt);
    CHECK(
      intersectRayEllipsoid(atOrigin, radius, 1.0, vm::ray3d{{1000, 0, 0}, {1, 0, 0}})
      == std::nullopt);

    // per axis, in a turned frame: local +x is world +Y
    const auto turned = LightFrame{
      vm::vec3d{64, 0, 0}, rotationOf({{"classname", "light"}, {"angles", "0 90 0"}})};
    const auto ellipsoid = vm::vec3d{300, 200, 100};
    CHECK(
      distanceOf(intersectRayEllipsoid(
        turned, ellipsoid, 1.0, vm::ray3d{{64, 1000, 0}, {0, -1, 0}}))
      == Catch::Approx(700.0).margin(Exact));
    CHECK(
      distanceOf(intersectRayEllipsoid(
        turned, ellipsoid, 1.0, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(1000.0 - 64.0 - 200.0).margin(Exact));

    // a flat ellipsoid
    CHECK(
      intersectRayEllipsoid(
        atOrigin, vm::vec3d{300, 300, 0}, 1.0, vm::ray3d{{1000, 0, 0}, {-1, 0, 0}})
      == std::nullopt);

    // big lights, whose unit sphere's space shrinks a ray's direction to about 1 / r:
    // along an axis and along a diagonal
    const auto big = vm::vec3d{1024, 1024, 1024};
    CHECK(
      distanceOf(
        intersectRayEllipsoid(atOrigin, big, 1.0, vm::ray3d{{5000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(5000.0 - 1024.0).margin(Exact));
    CHECK(
      distanceOf(intersectRayEllipsoid(
        atOrigin,
        big,
        1.0,
        vm::ray3d{{3000, 3000, 3000}, vm::normalize(vm::vec3d{-1, -1, -1})}))
      == Catch::Approx(3000.0 * std::sqrt(3.0) - 1024.0).margin(Exact));

    // a wide, flat light along a horizontal ray
    CHECK(
      distanceOf(intersectRayEllipsoid(
        atOrigin, vm::vec3d{2048, 2048, 512}, 1.0, vm::ray3d{{5000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(5000.0 - 2048.0).margin(Exact));

    // in the turned frame, world -X is local +y
    CHECK(
      distanceOf(intersectRayEllipsoid(
        turned, vm::vec3d{2048, 1024, 512}, 1.0, vm::ray3d{{5000, 0, 0}, {-1, 0, 0}}))
      == Catch::Approx(5000.0 - 64.0 - 1024.0).margin(Exact));
  }

  SECTION("intersectRaySpotCap")
  {
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};

    // the cap at 0.3 of the default spot light's depth (D69): at depth
    // zNear + 0.3 zFar = 8 + 0.3 * 256, where the default spot light's cap is the square
    // of half extent d / 2
    const auto depth = 8.0 + 0.3 * 256.0;
    REQUIRE(depth == Catch::Approx(84.8));
    const auto halfExtent = depth / 2.0;

    // from the base's side and from the apex's side
    CHECK(
      distanceOf(intersectRaySpotCap(
        defaultSpot, atOrigin, depth, vm::ray3d{{0, 0, -1000}, {0, 0, 1}}))
      == Catch::Approx(1000.0 - depth).margin(Exact));
    CHECK(
      distanceOf(intersectRaySpotCap(
        defaultSpot, atOrigin, depth, vm::ray3d{{0, 0, 1000}, {0, 0, -1}}))
      == Catch::Approx(1000.0 + depth).margin(Exact));
    CHECK(
      distanceOf(intersectRaySpotCap(
        defaultSpot,
        atOrigin,
        depth,
        vm::ray3d{{halfExtent - 0.5, -(halfExtent - 0.5), 0}, {0, 0, -1}}))
      == Catch::Approx(depth).margin(Exact));

    // past its edge, which the far cap would still have
    const auto pastEdge = vm::ray3d{{halfExtent + 0.5, 0, 1000}, {0, 0, -1}};
    CHECK(intersectRaySpotCap(defaultSpot, atOrigin, depth, pastEdge) == std::nullopt);
    CHECK(intersectRaySpotBase(defaultSpot, atOrigin, pastEdge) != std::nullopt);

    // in the light's frame, turned: angles 0 90 0 turns the beam -Z, which stays -Z
    const auto turned = LightFrame{
      vm::vec3d{0, 0, 100}, rotationOf({{"classname", "light"}, {"angles", "0 90 0"}})};
    CHECK(
      distanceOf(intersectRaySpotCap(
        defaultSpot, turned, depth, vm::ray3d{{0, 0, -1000}, {0, 0, 1}}))
      == Catch::Approx(1100.0 - depth).margin(Exact));

    // at the far cap's depth, the far cap
    const auto ray = vm::ray3d{{50, -60, -1000}, {0, 0, 1}};
    CHECK(
      distanceOf(intersectRaySpotCap(defaultSpot, atOrigin, 264.0, ray))
      == Catch::Approx(distanceOf(intersectRaySpotBase(defaultSpot, atOrigin, ray)))
           .margin(Exact));

    // no cap at the apex or behind it, nor of a degenerate light
    CHECK(
      intersectRaySpotCap(defaultSpot, atOrigin, 0.0, vm::ray3d{{0, 0, 1000}, {0, 0, -1}})
      == std::nullopt);
    CHECK(
      intersectRaySpotCap(
        defaultSpot, atOrigin, -10.0, vm::ray3d{{0, 0, 1000}, {0, 0, -1}})
      == std::nullopt);
    CHECK(
      intersectRaySpotCap(
        withRight(defaultSpot, {0, 0, 0}),
        atOrigin,
        depth,
        vm::ray3d{{0, 0, -1000}, {0, 0, 1}})
      == std::nullopt);
  }

  SECTION("intersectRaySpotBase")
  {
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};

    // the far cap of the default spot light, [-132, 132]^2 at z = -264
    CHECK(
      distanceOf(
        intersectRaySpotBase(defaultSpot, atOrigin, vm::ray3d{{0, 0, -1000}, {0, 0, 1}}))
      == Catch::Approx(736.0).margin(Exact));
    CHECK(
      distanceOf(intersectRaySpotBase(
        defaultSpot, atOrigin, vm::ray3d{{131, -131, -1000}, {0, 0, 1}}))
      == Catch::Approx(736.0).margin(Exact));
    // from the apex's side
    CHECK(
      distanceOf(
        intersectRaySpotBase(defaultSpot, atOrigin, vm::ray3d{{0, 0, 0}, {0, 0, -1}}))
      == Catch::Approx(264.0).margin(Exact));
    // past its edge
    CHECK(
      intersectRaySpotBase(defaultSpot, atOrigin, vm::ray3d{{133, 0, -1000}, {0, 0, 1}})
      == std::nullopt);

    // in the light's frame
    const auto raised = LightFrame{vm::vec3d{0, 0, 100}, identity};
    CHECK(
      distanceOf(
        intersectRaySpotBase(defaultSpot, raised, vm::ray3d{{0, 0, -1000}, {0, 0, 1}}))
      == Catch::Approx(836.0).margin(Exact));

    // the explicit spot light's far cap at 232
    CHECK(
      distanceOf(
        intersectRaySpotBase(explicitSpot, atOrigin, vm::ray3d{{0, 0, -1000}, {0, 0, 1}}))
      == Catch::Approx(1000.0 - 232.0).margin(Exact));

    CHECK(
      intersectRaySpotBase(
        withRight(defaultSpot, {0, 0, 0}), atOrigin, vm::ray3d{{0, 0, -1000}, {0, 0, 1}})
      == std::nullopt);
  }

  SECTION("emitterBulb")
  {
    auto omni = makeEmitterOmni();
    CHECK(emitterBulb(omni) == vm::vec3d{0, 0, 0});
    omni.hasCenter = true;
    omni.center = vm::vec3d{0, 0, 16};
    CHECK(emitterBulb(omni) == vm::vec3d{0, 0, 16});

    // a spot is lit from its origin
    auto spot = emitterSpot();
    spot.hasCenter = true;
    spot.center = vm::vec3d{0, 0, 16};
    CHECK(emitterBulb(spot) == vm::vec3d{0, 0, 0});
  }

  SECTION("intersectRayEmitterCap")
  {
    // range 288 along local +x, cone 60 by 60, squareness 2
    const auto spot = makeEmitterSpot(288.0);
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};

    // the round cap at the range, entered from outside on the beam
    CHECK(
      distanceOf(intersectRayEmitterCap(
        spot, atOrigin, 288.0, false, vm::ray3d{{1000, 3, 2}, {-1, 0, 0}}))
      == Catch::Approx(1000.0 - std::sqrt(288.0 * 288.0 - 13.0)));

    // from inside the sphere only with fromInside: the crossing where the ray leaves it
    const auto fromBulb = vm::ray3d{{50, 0, 0}, {1, 0, 0}};
    CHECK(intersectRayEmitterCap(spot, atOrigin, 96.0, false, fromBulb) == std::nullopt);
    CHECK(
      distanceOf(intersectRayEmitterCap(spot, atOrigin, 96.0, true, fromBulb))
      == Catch::Approx(46.0));

    // outside the ring of the cone nothing; a crossing behind the bulb is not on the cap,
    // so a ray from behind meets the cap only where it leaves the sphere
    CHECK(
      intersectRayEmitterCap(
        spot, atOrigin, 288.0, true, vm::ray3d{{0, 1000, 0}, {0, -1, 0}})
      == std::nullopt);
    CHECK(
      distanceOf(intersectRayEmitterCap(
        spot, atOrigin, 288.0, true, vm::ray3d{{-1000, 0, 0}, {1, 0, 0}}))
      == Catch::Approx(1288.0));
    CHECK(
      intersectRayEmitterCap(
        spot, atOrigin, 288.0, false, vm::ray3d{{-1000, 0, 0}, {1, 0, 0}})
      == std::nullopt);
    // just inside and just outside the ring at 30 degrees off the beam, along y
    const auto rim = [&](const double degrees) {
      const auto a = vm::to_radians(degrees);
      const auto point = 288.0 * vm::vec3d{std::cos(a), std::sin(a), 0};
      return intersectRayEmitterCap(
        spot, atOrigin, 288.0, false, vm::ray3d{point + vm::vec3d{1, 0, 0}, {-1, 0, 0}});
    };
    CHECK(rim(29.9));
    CHECK_FALSE(rim(30.1));

    // in a turned frame: a spot pointing down, its core picked from below
    const auto down = LightFrame{vm::vec3d{0, 0, 0}, emitterSpotRotation()};
    CHECK(
      distanceOf(intersectRayEmitterCap(
        spot, down, 96.0, true, vm::ray3d{{3, 2, -1000}, {0, 0, 1}}))
      == Catch::Approx(1000.0 - std::sqrt(96.0 * 96.0 - 13.0)));
  }

  SECTION("emitterEditBox")
  {
    const auto h = emitterHalfWidth;
    const auto spot = emitterSpot();
    const auto spotBox = emitterEditBox(spot);
    REQUIRE(spotBox);
    CHECK(vm::approx{box({0, -h, -h}, {256, h, h}), Exact} == *spotBox);

    // the bounding box of the lit cone: a 60 degree spot of range 100 is 50 to each side
    auto hundred = spot;
    hundred.radius = vm::vec3d{100, 100, 100};
    CHECK(
      vm::approx{box({0, -50, -50}, {100, 50, 50}), Exact} == *emitterEditBox(hundred));

    // cone v pairs with local z
    auto oval = spot;
    oval.emitter.coneV = 40.0;
    const auto v = 256.0 * std::sin(vm::to_radians(20.0));
    CHECK(vm::approx{box({0, -h, -v}, {256, h, v}), Exact} == *emitterEditBox(oval));

    // the widest cone: sin 60 of the range
    auto wide = spot;
    wide.emitter.coneH = 120.0;
    const auto w = 256.0 * std::sin(vm::to_radians(60.0));
    CHECK(vm::approx{box({0, -w, -h}, {256, w, h}), Exact} == *emitterEditBox(wide));

    // an omni's box is +-light_radius about its bulb
    auto omni = makeEmitterOmni();
    omni.hasCenter = true;
    omni.center = vm::vec3d{0, 0, 16};
    CHECK(
      vm::approx{box({-288, -288, -272}, {288, 288, 304}), Exact}
      == *emitterEditBox(omni));

    // the tool's functions give the same boxes, at any threshold
    CHECK(lightEditBox(spot) == spotBox);
    CHECK(lightEditBox(spot, DefaultThreshold) == spotBox);
    CHECK(lightEditBox(omni, DefaultThreshold) == emitterEditBox(omni));

    auto flat = makeEmitterOmni();
    flat.radius = vm::vec3d{288, 288, 0};
    CHECK(emitterEditBox(flat) == std::nullopt);
  }

  SECTION("emitterFromEditBox")
  {
    const auto h = emitterHalfWidth;
    const auto spot = emitterSpot();

    SECTION("The far face sets the range and keeps both cone angles")
    {
      const auto light =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {320, h, h})));
      CHECK(light.radius == vm::approx{vm::vec3d{320, 320, 320}, Exact});
      CHECK(light.emitter.coneH == spot.emitter.coneH);
      CHECK(light.emitter.coneV == spot.emitter.coneV);
      CHECK(light.origin == vm::approx{spot.origin, Exact});

      // so the sides moved with the far face
      const auto h320 = 320.0 * std::sin(vm::to_radians(30.0));
      CHECK(
        vm::approx{box({0, -h320, -h320}, {320, h320, h320}), Exact}
        == *emitterEditBox(light));

      // auto range: the size follows the range
      CHECK(light.emitter.size == Catch::Approx(320.0 / 3.0));
    }

    SECTION("A side face sets its cone angle and keeps the range")
    {
      // a face at e from the axis sets 2 asin(e / R): at R sin 45, 90 degrees
      const auto e = 256.0 * std::sin(vm::to_radians(45.0));
      const auto light =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {256, e, h})));
      CHECK(light.radius == vm::approx{vm::vec3d{256, 256, 256}, Exact});
      CHECK(light.emitter.coneH == Catch::Approx(90.0));
      CHECK(light.emitter.coneV == spot.emitter.coneV);
      CHECK(light.emitter.size == spot.emitter.size);

      // a face on the other side and the other axis
      const auto other =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -e}, {256, h, h})));
      CHECK(other.emitter.coneV == Catch::Approx(90.0));
      CHECK(other.emitter.coneH == spot.emitter.coneH);
    }

    SECTION("Box to light to box changes nothing")
    {
      // the face moved to e is where it stays, and the cone is symmetric
      for (const auto e : {20.0, 64.0, 150.0, 200.0})
      {
        CAPTURE(e);
        const auto edited = box({0, -h, -h}, {256, e, h});
        const auto light = editedLight(emitterFromEditBox(spot, identity, edited));
        CHECK(vm::approx{box({0, -e, -h}, {256, e, h}), Exact} == *emitterEditBox(light));

        const auto symmetric = box({0, -e, -h}, {256, e, h});
        CHECK(
          vm::approx{symmetric, Exact}
          == *emitterEditBox(editedLight(emitterFromEditBox(spot, identity, symmetric))));
      }
    }

    SECTION("An edge applies both rules")
    {
      const auto e = 320.0 * std::sin(vm::to_radians(45.0));
      const auto light =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {320, e, h})));
      CHECK(light.radius == vm::approx{vm::vec3d{320, 320, 320}, Exact});
      CHECK(light.emitter.coneH == Catch::Approx(90.0));
      CHECK(light.emitter.coneV == spot.emitter.coneV);
    }

    SECTION("Moving the box moves the origin and nothing else")
    {
      // angles 0 90 0 turn local +X to world +Y and local +Y to world -X
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto moved = editedLight(emitterFromEditBox(
        spot, R, emitterEditBox(spot)->translate(vm::vec3d{10, 20, 30})));
      CHECK(moved.origin == vm::approx{vm::vec3d{-20, 10, 30}, Exact});
      CHECK(moved.radius == spot.radius);
      CHECK(moved.emitter == spot.emitter);
    }

    SECTION("Limits")
    {
      // a range below 24 ends on 24, the cone kept (F-T-09)
      const auto floored =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {23, h, h})));
      CHECK(floored.radius == vm::approx{vm::vec3d{24, 24, 24}, Exact});
      CHECK(floored.emitter.coneH == spot.emitter.coneH);
      CHECK(floored.emitter.coneV == spot.emitter.coneV);
      CHECK(floored.origin == vm::approx{spot.origin, Exact});
      // a half extent below 1
      CHECK(
        emitterFromEditBox(spot, identity, box({0, -0.4, -h}, {256, 0.4, h}))
        == std::nullopt);
      // a cone angle is clamped to 120 degrees, a face past the range counting as the
      // range
      const auto wide =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {256, 1000, h})));
      CHECK(wide.emitter.coneH == 120.0);
      const auto past120 =
        editedLight(emitterFromEditBox(spot, identity, box({0, -h, -h}, {256, 240, h})));
      CHECK(past120.emitter.coneH == 120.0);
      // and to 1 degree: 2 asin(1 / 256) is below it
      const auto narrow =
        editedLight(emitterFromEditBox(spot, identity, box({0, -1, -h}, {256, 1, h})));
      CHECK(narrow.emitter.coneH == 1.0);
      const auto oneDegree = 256.0 * std::sin(vm::to_radians(0.5));
      CHECK(oneDegree > 1.0);
      const auto atOne = editedLight(emitterFromEditBox(
        spot, identity, box({0, -oneDegree, -h}, {256, oneDegree, h})));
      CHECK(atOne.emitter.coneH == Catch::Approx(1.0));
    }

    SECTION("An omni keeps its light_center")
    {
      auto omni = makeEmitterOmni();
      omni.hasCenter = true;
      omni.center = vm::vec3d{0, 0, 16};

      // the +x face by 32
      const auto light = editedLight(
        emitterFromEditBox(omni, identity, box({-288, -288, -272}, {320, 288, 304})));
      CHECK(light.origin == vm::approx{vm::vec3d{16, 0, 0}, Exact});
      CHECK(light.center == vm::vec3d{0, 0, 16});
      CHECK(light.radius == vm::approx{vm::vec3d{304, 288, 288}, Exact});
      CHECK_FALSE(light.equalRadius);
      CHECK(light.emitter.size == Catch::Approx(304.0 / 3.0));

      // a range below 24 ends on 24: the dragged radii are held so that the largest is
      // 24, here all three about the bulb, which stays (F-T-09)
      const auto floored = editedLight(
        emitterFromEditBox(omni, identity, box({-20, -20, -4}, {20, 20, 36})));
      CHECK(floored.radius == vm::approx{vm::vec3d{24, 24, 24}, Exact});
      CHECK(floored.origin == vm::approx{vm::vec3d{0, 0, 0}, Exact});

      // one face of an omni that is at the floor: the face that stayed stays, the
      // dragged one ends where the largest radius is 24
      auto small = makeEmitterOmni();
      small.radius = vm::vec3d{24, 10, 10};
      small.equalRadius = false;
      const auto held = editedLight(
        emitterFromEditBox(small, identity, box({-24, -10, -10}, {10, 10, 10})));
      CHECK(held.radius == vm::approx{vm::vec3d{24, 10, 10}, Exact});
      CHECK(held.origin == vm::approx{vm::vec3d{0, 0, 0}, Exact});
      // a radius that is not the largest is not held
      const auto narrow = editedLight(
        emitterFromEditBox(small, identity, box({-24, -10, -10}, {24, 5, 10})));
      CHECK(narrow.radius == vm::approx{vm::vec3d{24, 7.5, 10}, Exact});
    }

    SECTION("lightFromEditBox gives the same light")
    {
      const auto b = box({0, -h, -h}, {320, h, h});
      CHECK(lightFromEditBox(spot, identity, b) == emitterFromEditBox(spot, identity, b));
      CHECK(
        lightFromEditBox(spot, identity, b, DefaultThreshold)
        == emitterFromEditBox(spot, identity, b));

      const auto omniBox = box({-288, -288, -288}, {320, 288, 288});
      CHECK(
        lightFromEditBox(makeEmitterOmni(), identity, omniBox, DefaultThreshold)
        == emitterFromEditBox(makeEmitterOmni(), identity, omniBox));
    }
  }

  SECTION("emitterSpotRotation")
  {
    const auto R = emitterSpotRotation();
    CHECK(R * vm::vec3d{1, 0, 0} == vm::approx{vm::vec3d{0, 0, -1}, Exact});
    CHECK(det(R) == Catch::Approx(1.0));
    // what entityRotation reads from angles "90 0 0"
    CHECK(
      vm::is_equal(R, rotationOf({{"classname", "light"}, {"angles", "90 0 0"}}), Exact));
  }

  SECTION("emitterSpotFromWorldBox")
  {
    auto base = LightProperties{};
    base.emitter = seededEmitterProperties();
    const auto W = box({0, 0, 0}, {128, 64, 96});

    const auto light = emitterSpotFromWorldBox(W, base);
    CHECK(light.type == LightType::Spot);
    CHECK(light.emitter.enabled);
    CHECK(light.origin == vm::approx{vm::vec3d{64, 32, 96}, Exact});
    CHECK(light.radius == vm::approx{vm::vec3d{96, 96, 96}, Exact});
    // pointing down, axis[1] is world +Y and axis[2] world +X; each cone angle is
    // 2 asin(half extent / height)
    CHECK(
      light.emitter.coneH == Catch::Approx(2.0 * vm::to_degrees(std::asin(32.0 / 96.0))));
    CHECK(
      light.emitter.coneV == Catch::Approx(2.0 * vm::to_degrees(std::asin(64.0 / 96.0))));
    CHECK(light.emitter.size == Catch::Approx(32.0));

    // the edit box, turned by the new spot's rotation, is the drawn box
    const auto R = emitterSpotRotation();
    const auto editBox = emitterEditBox(light);
    REQUIRE(editBox);
    const auto a = light.origin + R * editBox->min;
    const auto b = light.origin + R * editBox->max;
    CHECK(vm::approx{vm::bbox3d{vm::min(a, b), vm::max(a, b)}, Exact} == W);

    // a box 100 wide and 100 high makes a 60 degree spot whose edit box is the drawn box
    const auto cube = box({0, 0, 0}, {100, 100, 100});
    const auto sixty = emitterSpotFromWorldBox(cube, base);
    CHECK(sixty.emitter.coneH == Catch::Approx(60.0));
    CHECK(sixty.emitter.coneV == Catch::Approx(60.0));
    const auto sixtyBox = emitterEditBox(sixty);
    REQUIRE(sixtyBox);
    const auto c0 = sixty.origin + R * sixtyBox->min;
    const auto c1 = sixty.origin + R * sixtyBox->max;
    CHECK(vm::approx{vm::bbox3d{vm::min(c0, c1), vm::max(c0, c1)}, Exact} == cube);

    // a side wider than sin 60 of the height is clamped to 120 degrees, and the edit box
    // is then narrower than the drawn box
    const auto flat = emitterSpotFromWorldBox(box({0, 0, 0}, {100, 200, 100}), base);
    CHECK(flat.emitter.coneH == 120.0);
    CHECK(
      emitterEditBox(flat)->max.y()
      == Catch::Approx(100.0 * std::sin(vm::to_radians(60.0))));

    // a half extent below 1 is denied
    CHECK(emitterSpotFromWorldBox(box({0, 0, 0}, {1, 64, 96}), base) == base);
  }

  SECTION("emitterOmniFromWorldBox")
  {
    auto base = LightProperties{};
    base.emitter = seededEmitterProperties();
    const auto W = box({0, 0, 0}, {128, 128, 64});

    const auto light = emitterOmniFromWorldBox(W, base);
    CHECK(light.type == LightType::Point);
    CHECK(light.emitter.enabled);
    CHECK(light.origin == vm::approx{vm::vec3d{64, 64, 32}, Exact});
    CHECK(light.radius == vm::approx{vm::vec3d{64, 64, 32}, Exact});
    CHECK_FALSE(light.equalRadius);
    CHECK(light.emitter.size == Catch::Approx(64.0 / 3.0));

    // the bulb, not the origin, is the box's centre
    auto centred = base;
    centred.hasCenter = true;
    centred.center = vm::vec3d{0, 0, 16};
    CHECK(
      emitterOmniFromWorldBox(W, centred).origin
      == vm::approx{vm::vec3d{64, 64, 16}, Exact});

    // a sun stays a sun; an omni's largest half extent below 24 is denied, a sun's box
    // may be any size (F-T-10)
    auto sunBase = base;
    sunBase.type = LightType::Sun;
    CHECK(emitterOmniFromWorldBox(W, sunBase).type == LightType::Sun);
    CHECK(emitterOmniFromWorldBox(box({0, 0, 0}, {40, 40, 40}), base) == base);
    const auto smallSun = emitterOmniFromWorldBox(box({0, 0, 0}, {40, 40, 40}), sunBase);
    CHECK(smallSun.type == LightType::Sun);
    CHECK(smallSun.radius == vm::approx{vm::vec3d{20, 20, 20}, Exact});
  }

  SECTION("emitterSpotFromTextureSpot")
  {
    SECTION("The default spot light")
    {
      const auto result = emitterSpotFromTextureSpot(defaultSpot, identity);
      REQUIRE(result);
      const auto& light = result->light;

      // forward along the beam, axis[1] = -right, axis[2] = axis[0] x axis[1]
      CHECK(vm::is_equal(
        result->rotation, columns({0, 0, -1}, {1, 0, 0}, {0, -1, 0}), Exact));
      CHECK(det(result->rotation) == Catch::Approx(1.0));

      CHECK(light.type == LightType::Spot);
      CHECK(light.emitter.enabled);
      CHECK(light.emitter.coneH == Catch::Approx(2.0 * vm::to_degrees(std::atan(0.5))));
      CHECK(light.emitter.coneV == Catch::Approx(53.1301).margin(1e-4));
      // zNear + zFar = 8 + 256
      CHECK(light.radius == vm::approx{vm::vec3d{264, 264, 264}, Exact});
      CHECK(light.origin == defaultSpot.origin);
      CHECK(light.target == vm::vec3d{0, 0, 0});
      CHECK(light.right == vm::vec3d{0, 0, 0});
      CHECK(light.up == vm::vec3d{0, 0, 0});
      // the size follows the new range; without light_start a new block starts at 0
      CHECK(light.emitter.size == Catch::Approx(88.0));
      CHECK(light.emitter.start == 0.0);
    }

    SECTION("Without light_start a kept block keeps its start")
    {
      auto kept = defaultSpot;
      kept.emitter.hasBlock = true;
      kept.emitter.start = 50.0;
      const auto result = emitterSpotFromTextureSpot(kept, identity);
      REQUIRE(result);
      CHECK(result->light.emitter.start == 50.0);
      CHECK(result->light.emitter.size == Catch::Approx((264.0 - 50.0) / 3.0));
    }

    SECTION("A kept linear block keeps its auto range flag")
    {
      // the kind switch is no range edit (G0 section 5)
      auto texture = defaultSpot;
      texture.emitter = seededEmitterProperties();
      texture.emitter.enabled = false;
      texture.emitter.falloff = LightFalloff::Linear;
      REQUIRE(texture.emitter.autoRange);

      const auto emitter = emitterSpotFromTextureSpot(texture, identity);
      REQUIRE(emitter);
      CHECK(emitter->light.emitter.autoRange);
      CHECK(emitter->light.radius == vm::vec3d{264, 264, 264});
    }

    SECTION("Explicit start and end points, in a turned light")
    {
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto result = emitterSpotFromTextureSpot(explicitSpot, R);
      REQUIRE(result);
      // zNear + zFar = 32 + 200
      CHECK(result->light.radius == vm::approx{vm::vec3d{232, 232, 232}, Exact});
      // the emitter starts where the explicit light_start was (F-T-03), and the size
      // follows the range from there
      CHECK(result->light.emitter.start == Catch::Approx(32.0));
      CHECK(result->light.emitter.size == Catch::Approx(200.0 / 3.0));
      CHECK(vm::is_equal(
        result->rotation, R * columns({0, 0, -1}, {1, 0, 0}, {0, -1, 0}), Exact));
      // the beam still points down in the world
      CHECK(
        result->rotation * vm::vec3d{1, 0, 0} == vm::approx{vm::vec3d{0, 0, -1}, Exact});
    }

    SECTION("Nothing for a degenerate spot, a point light or an emitter")
    {
      auto noTarget = defaultSpot;
      noTarget.target = vm::vec3d{0, 0, 0};
      CHECK_FALSE(emitterSpotFromTextureSpot(noTarget, identity).has_value());
      CHECK_FALSE(emitterSpotFromTextureSpot(defaultPoint, identity).has_value());
      CHECK_FALSE(emitterSpotFromTextureSpot(emitterSpot(), identity).has_value());
    }
  }

  SECTION("textureSpotFromEmitterSpot")
  {
    auto spot = emitterSpot();
    spot.radius = vm::vec3d{512, 512, 512};
    spot.emitter.coneV = 40.0;

    // T = R - 8
    const auto light = textureSpotFromEmitterSpot(spot);
    CHECK(light.type == LightType::Spot);
    CHECK_FALSE(light.emitter.enabled);
    CHECK(light.target == vm::approx{vm::vec3d{504, 0, 0}, Exact});
    CHECK(
      light.right
      == vm::approx{vm::vec3d{0, -504 * std::tan(vm::to_radians(30.0)), 0}, Exact});
    CHECK(
      light.up
      == vm::approx{vm::vec3d{0, 0, 504 * std::tan(vm::to_radians(20.0))}, Exact});
    CHECK_FALSE(light.explicitStartEnd);
    // the engine lights it from 8 to 8 + T = R
    const auto frustum = spotLightFrustum(light);
    REQUIRE(frustum);
    CHECK(frustum->zNear + frustum->zFar == Catch::Approx(512.0));
    // the emitter keeps its values while it is switched off
    CHECK(light.emitter.coneV == 40.0);
    CHECK(light.emitter.size == spot.emitter.size);

    // T is at least 16
    auto shortSpot = emitterSpot();
    shortSpot.radius = vm::vec3d{20, 20, 20};
    CHECK(
      textureSpotFromEmitterSpot(shortSpot).target
      == vm::approx{vm::vec3d{16, 0, 0}, Exact});
  }

  SECTION("asTextureLight")
  {
    // a spot through textureSpotFromEmitterSpot (F-T-02)
    auto spot = emitterSpot();
    spot.emitter.coneV = 40.0;
    CHECK(asTextureLight(spot) == textureSpotFromEmitterSpot(spot));
    CHECK_FALSE(asTextureLight(spot).emitter.enabled);

    // an omni and a sun only clear the flag
    auto omni = makeEmitterOmni();
    omni.hasCenter = true;
    omni.center = vm::vec3d{0, 0, 16};
    auto expected = omni;
    expected.emitter.enabled = false;
    CHECK(asTextureLight(omni) == expected);

    auto emitterSun = sun;
    emitterSun.emitter = seededEmitterProperties();
    auto expectedSun = emitterSun;
    expectedSun.emitter.enabled = false;
    CHECK(asTextureLight(emitterSun) == expectedSun);

    // a texture light is unchanged
    CHECK(asTextureLight(defaultSpot) == defaultSpot);
    CHECK(asTextureLight(defaultPoint) == defaultPoint);
  }

  SECTION("asEmitterLight")
  {
    const auto policy = EntityRotationPolicy::IdTech4;
    const auto entityOf = [](std::vector<EntityProperty> more) {
      auto properties = std::vector<EntityProperty>{
        {"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}};
      properties.insert(properties.end(), more.begin(), more.end());
      return Entity{std::move(properties)};
    };

    SECTION("A point light without a block gets a new one, its effect its style's")
    {
      const auto entity = entityOf({{"style", "3"}});
      const auto light = parseLightProperties(entity);
      const auto [emitter, rotation] = asEmitterLight(entity, light, policy);

      auto expected = light;
      expected.emitter = seededEmitterProperties();
      expected.emitter.effect = 3;
      // the size follows the kept range of 300: 300 / 3 at intensity 1
      expected.emitter.size = 100.0;
      CHECK(emitter == expected);
      CHECK(rotation == std::nullopt);
    }

    SECTION("A kept block comes back, a range out of step with auto range off")
    {
      const auto entity = entityOf({{"emitter", "0"}, {"emit_size", "50"}});
      const auto light = parseLightProperties(entity);
      REQUIRE(light.emitter.hasBlock);
      const auto [emitter, rotation] = asEmitterLight(entity, light, policy);

      CHECK(emitter.emitter.enabled);
      CHECK(emitter.emitter.hasBlock);
      CHECK(emitter.emitter.size == 50.0);
      CHECK(emitter.radius == vm::vec3d{300, 300, 300});
      // its auto range would be 150 (G0 section 5, a hand-set range wins)
      CHECK_FALSE(emitter.emitter.autoRange);
      CHECK(rotation == std::nullopt);
    }

    SECTION("A kept effect is not taken from the style")
    {
      const auto entity = entityOf({{"style", "3"}, {"emit_effect", "pulse"}});
      const auto [emitter, rotation] =
        asEmitterLight(entity, parseLightProperties(entity), policy);
      CHECK(emitter.emitter.effect == 13);
    }

    SECTION("A texture spot's frustum becomes its axis, cone and range")
    {
      const auto entity = Entity{{
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      }};
      const auto [emitter, rotation] =
        asEmitterLight(entity, parseLightProperties(entity), policy);

      CHECK(emitter.type == LightType::Spot);
      CHECK(emitter.emitter.enabled);
      CHECK(emitter.radius == vm::approx{vm::vec3d{264, 264, 264}, Exact});
      CHECK(emitter.emitter.size == Catch::Approx(88.0));
      REQUIRE(rotation);
      CHECK(vm::is_equal(*rotation, columns({0, 0, -1}, {1, 0, 0}, {0, -1, 0}), Exact));
    }

    SECTION("A spot without a frustum becomes a new spot pointing down")
    {
      const auto entity = Entity{{
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_target", "0 0 0"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      }};
      const auto light = parseLightProperties(entity);
      REQUIRE(light.type == LightType::Spot);
      const auto [emitter, rotation] = asEmitterLight(entity, light, policy);

      CHECK(emitter == seededEmitterLight(LightType::Spot, light));
      CHECK(rotation == emitterSpotRotation());
    }
  }

  SECTION("withEmitterRange")
  {
    // a spot's three radii are its range
    auto spot = emitterSpot();
    spot.radius = vm::vec3d{100, 200, 300};
    spot.equalRadius = false;
    const auto ranged = withEmitterRange(spot, 400.0);
    CHECK(ranged.radius == vm::vec3d{400, 400, 400});
    CHECK(ranged.equalRadius);

    // an omni's box scales as a whole (G0 section 5)
    auto omni = makeEmitterOmni();
    omni.radius = vm::vec3d{100, 50, 200};
    omni.equalRadius = false;
    CHECK(withEmitterRange(omni, 400.0).radius == vm::vec3d{200, 100, 400});
    CHECK_FALSE(withEmitterRange(omni, 400.0).equalRadius);
    omni.radius = vm::vec3d{0, 0, 0};
    CHECK(withEmitterRange(omni, 400.0).radius == vm::vec3d{400, 400, 400});

    // a sun has no range
    auto emitterSun = sun;
    emitterSun.emitter = seededEmitterProperties();
    CHECK(withEmitterRange(emitterSun, 400.0) == emitterSun);
  }

  SECTION("The writer never writes a texture spot whose target is 0")
  {
    // an emitter spot whose flag alone was cleared (F-T-02)
    auto cleared = emitterSpot();
    cleared.emitter.enabled = false;
    CHECK(
      lightPropertyUpdate(cleared)
      == lightPropertyUpdate(textureSpotFromEmitterSpot(emitterSpot())));

    const auto entity = applyLightPropertyUpdate(
      Entity{std::vector<EntityProperty>{{"classname", "light"}}},
      lightPropertyUpdate(cleared));
    CHECK(*entity.property("light_target") == "248 0 0");
    CHECK(*entity.property("emitter") == "0");

    // a texture spot without an emitter block is written as it is
    auto bare = defaultSpot;
    bare.target = vm::vec3d{0, 0, 0};
    const auto bareEntity = applyLightPropertyUpdate(
      Entity{std::vector<EntityProperty>{{"classname", "light"}}},
      lightPropertyUpdate(bare));
    CHECK(*bareEntity.property("light_target") == "0 0 0");
  }

  SECTION("emitterAimRotation")
  {
    // a level beam: axis[1] = worldUp x axis[0] (F-T-08)
    const auto level = emitterAimRotation(vm::vec3d{2, 0, 0}, vm::vec3d{0, 0, 1});
    CHECK(vm::is_equal(level, columns({1, 0, 0}, {0, 1, 0}, {0, 0, 1}), Exact));

    // a tilted beam keeps axis[1] level, whatever the previous one was
    const auto forward = vm::normalize(vm::vec3d{1, 1, -1});
    const auto tilted = emitterAimRotation(forward, vm::vec3d{0, 0, 1});
    CHECK(tilted[0] == vm::approx{forward, Exact});
    CHECK(tilted[1].z() == Catch::Approx(0.0).margin(Exact));
    CHECK(det(tilted) == Catch::Approx(1.0));

    // straight down: the previous axis[1], made perpendicular to the beam
    const auto down = emitterAimRotation(vm::vec3d{0, 0, -1}, vm::vec3d{0.1, 1, 0.5});
    CHECK(down[0] == vm::approx{vm::vec3d{0, 0, -1}, Exact});
    CHECK(down[1] == vm::approx{vm::normalize(vm::vec3d{0.1, 1, 0}), Exact});
    CHECK(det(down) == Catch::Approx(1.0));
  }

  SECTION("A kind switch there and back changes nothing")
  {
    const auto turned = rotationOf({{"classname", "light"}, {"angles", "30 45 60"}});

    SECTION("Emitter to texture and back")
    {
      for (const auto range : {24.0, 256.0, 4096.0})
      {
        CAPTURE(range);
        auto spot = emitterSpot();
        spot.radius = vm::vec3d{range, range, range};
        spot.emitter.coneV = 40.0;
        spot.emitter.autoRange = false;

        // the rotation is unchanged on the way there
        const auto back =
          emitterSpotFromTextureSpot(textureSpotFromEmitterSpot(spot), turned);
        REQUIRE(back);
        CHECK(vm::is_equal(back->rotation, turned, Exact));
        CHECK(back->light.emitter.coneH == Catch::Approx(60.0));
        CHECK(back->light.emitter.coneV == Catch::Approx(40.0));
        CHECK(back->light.radius == vm::approx{spot.radius, Exact});
      }
    }

    SECTION("A centred, unmirrored texture spot comes back with the same world vectors")
    {
      // a sheared spot comes back centred, a mirrored one with its up flipped (G0 section
      // 9)
      const auto emitter = emitterSpotFromTextureSpot(defaultSpot, turned);
      REQUIRE(emitter);
      const auto back = textureSpotFromEmitterSpot(emitter->light);

      // the texture spot's vectors, in the world, before and after
      CHECK(
        emitter->rotation * back.target
        == vm::approx{turned * defaultSpot.target, Tolerance});
      CHECK(
        emitter->rotation * back.right
        == vm::approx{turned * defaultSpot.right, Tolerance});
      CHECK(
        emitter->rotation * back.up == vm::approx{turned * defaultSpot.up, Tolerance});
    }
  }

  SECTION("A texture spot switched to an emitter on an entity")
  {
    auto entity = Entity{std::vector<EntityProperty>{
      {"classname", "light"},
      {"origin", "0 0 0"},
      {"light_target", "0 0 -256"},
      {"light_up", "0 -128 0"},
      {"light_right", "-128 0 0"},
    }};
    const auto frame = lightFrame(entity, EntityRotationPolicy::IdTech4);
    const auto result = emitterSpotFromTextureSpot(parseLightProperties(entity), frame.R);
    REQUIRE(result);

    entity = applyLightPropertyUpdate(entity, lightKeyDiff(entity, result->light));
    applyEntityRotationUpdate(entity, lightRotationUpdate(entity, result->rotation));

    // the five vectors are deleted, the range is written R R R
    CHECK_FALSE(entity.hasProperty("light_target"));
    CHECK_FALSE(entity.hasProperty("light_up"));
    CHECK_FALSE(entity.hasProperty("light_right"));
    CHECK(*entity.property("light_radius") == "264 264 264");
    CHECK(*entity.property("emitter") == "1");

    const auto light = parseLightProperties(entity);
    CHECK(light.type == LightType::Spot);
    CHECK(light.emitter.coneH == Catch::Approx(53.1301).margin(1e-4));
    CHECK(vm::is_equal(
      lightFrame(entity, EntityRotationPolicy::IdTech4).R, result->rotation, Tolerance));

    SECTION("and back to Texture, which keeps every emit_ key")
    {
      const auto diff = lightKeyDiff(entity, textureSpotFromEmitterSpot(light));
      CHECK(std::ranges::none_of(
        diff.remove, [](const auto& key) { return key.starts_with("emit"); }));

      const auto changed = applyLightPropertyUpdate(entity, diff);
      for (const auto& key : emitterPropertyKeys())
      {
        CAPTURE(key);
        CHECK(changed.hasProperty(key));
      }
      CHECK(*changed.property("emitter") == "0");
      CHECK(*changed.property("light_target") == "256 0 0");
      CHECK_FALSE(changed.hasProperty("light_radius"));
    }
  }

  SECTION("An emitter spot pointing straight down")
  {
    const auto entity = Entity{std::vector<EntityProperty>{
      {"classname", "light"},
      {"origin", "0 0 256"},
      {"emitter", "1"},
      {"emit_shape", "spot"},
      {"light_radius", "256 256 256"},
      {"emit_cone", "60 30"},
      {"angles", "90 0 0"},
    }};
    const auto light = parseLightProperties(entity);
    const auto frame = lightFrame(entity, EntityRotationPolicy::IdTech4);
    CHECK(frame.R * vm::vec3d{1, 0, 0} == vm::approx{vm::vec3d{0, 0, -1}, Exact});
    CHECK(vm::is_equal(frame.R, emitterSpotRotation(), Exact));

    // lit below the bulb, not above it
    CHECK(containsPoint(light, frame, vm::vec3d{0, 0, 100}));
    CHECK_FALSE(containsPoint(light, frame, vm::vec3d{0, 0, 300}));
    // cone h (60) runs along axis[1], world +Y; cone v (30) along axis[2], world +X
    CHECK(containsPoint(light, frame, vm::vec3d{0, 80, 100}));
    CHECK_FALSE(containsPoint(light, frame, vm::vec3d{80, 0, 100}));

    SECTION("Its rotation is written and read back as a matrix")
    {
      // at pitch 90 the angles fold roll into yaw; the matrix survives
      auto turned = Entity{std::vector<EntityProperty>{
        {"classname", "light"},
        {"angles", "90 0 30"},
      }};
      const auto R = lightFrame(turned, EntityRotationPolicy::IdTech4).R;
      // the same matrix is no edit, so the angles stay as written (F-T-04)
      const auto same = lightRotationUpdate(turned, R);
      CHECK(same == EntityRotationUpdate{});
      applyEntityRotationUpdate(turned, same);
      CHECK(*turned.property("angles") == "90 0 30");

      // a different matrix is written and read back
      const auto other = rotationOf({{"classname", "light"}, {"angles", "45 10 20"}});
      applyEntityRotationUpdate(turned, lightRotationUpdate(turned, other));
      CHECK(vm::is_equal(
        lightFrame(turned, EntityRotationPolicy::IdTech4).R, other, Tolerance));

      // light_rotation that holds the given matrix already stays
      const auto kept = Entity{std::vector<EntityProperty>{
        {"classname", "light"},
        {"light_rotation", "0 1 0 -1 0 0 0 0 1"},
      }};
      CHECK(
        lightRotationUpdate(kept, lightFrame(kept, EntityRotationPolicy::IdTech4).R)
        == EntityRotationUpdate{});

      // light_rotation is replaced by angles, as the editors write rotation today
      auto matrix = Entity{std::vector<EntityProperty>{
        {"classname", "light"},
        {"light_rotation", "0 1 0 -1 0 0 0 0 1"},
      }};
      const auto update = lightRotationUpdate(matrix, emitterSpotRotation());
      REQUIRE(update.set);
      CHECK(update.set->key() == "angles");
      CHECK(update.remove == std::vector<std::string>{"light_rotation"});
      applyEntityRotationUpdate(matrix, update);
      CHECK(vm::is_equal(
        lightFrame(matrix, EntityRotationPolicy::IdTech4).R,
        emitterSpotRotation(),
        Tolerance));
    }
  }

  SECTION("emitterCurve")
  {
    const auto curve = emitterCurve(makeEmitterOmni());
    CHECK(curve.falloff == LightFalloff::InvSquareClamped);
    CHECK(curve.falloffExp == Catch::Approx(3.0));
    CHECK(curve.innerRadius == 0.0);
    CHECK(curve.shapeExp == 2.0);
    CHECK(curve.penumbra == 0.2);

    // inner = start / R, k = (R - start) / size
    auto started = makeEmitterOmni();
    started.emitter.start = 48.0;
    CHECK(emitterCurve(started).innerRadius == Catch::Approx(48.0 / 288.0));
    CHECK(emitterCurve(started).falloffExp == Catch::Approx(2.5));

    // the start is clamped to R - 1
    started.emitter.start = 500.0;
    CHECK(emitterCurve(started).innerRadius == Catch::Approx(287.0 / 288.0));
    CHECK(emitterCurve(started).falloffExp == Catch::Approx(1.0 / 96.0));
  }

  SECTION("emitterFalloffT")
  {
    auto omni = makeEmitterOmni();
    omni.hasCenter = true;
    omni.center = vm::vec3d{0, 0, 16};
    CHECK(emitterFalloffT(omni, vm::vec3d{0, 0, 16}) == 0.0);
    CHECK(emitterFalloffT(omni, vm::vec3d{0, 0, 16 + 144}) == Catch::Approx(0.5));

    // a spot's t runs along its range from the bulb in every direction
    CHECK(emitterFalloffT(emitterSpot(), vm::vec3d{128, 0, 0}) == Catch::Approx(0.5));
    CHECK(emitterFalloffT(emitterSpot(), vm::vec3d{0, 128, 0}) == Catch::Approx(0.5));
  }

  SECTION("emitterSpotMask")
  {
    const auto spot = emitterSpot();
    const auto tanHalf = std::tan(vm::to_radians(30.0));
    CHECK(emitterSpotMask(spot, vm::vec3d{100, 0, 0}) == 1.0);
    // on the cone, and half way through the penumbra ring from 0.8 to 1
    CHECK(
      emitterSpotMask(spot, vm::vec3d{100, 100 * tanHalf, 0})
      == Catch::Approx(0.0).margin(Exact));
    CHECK(
      emitterSpotMask(spot, vm::vec3d{100, 0, 90 * tanHalf})
      == Catch::Approx(0.5).margin(Exact));
    // behind the bulb
    CHECK(emitterSpotMask(spot, vm::vec3d{-10, 0, 0}) == 0.0);
    // an omni has no mask
    CHECK(emitterSpotMask(makeEmitterOmni(), vm::vec3d{-10, 0, 0}) == 1.0);
  }

  SECTION("emitterFalloff")
  {
    // the windowed inverse square written out at u = t = 96 / 288 and k = 3
    const auto u = 1.0 / 3.0;
    const auto w = 1.0 - u * u * u * u;
    const auto d = u * 3.0 + 1e-3;
    CHECK(
      emitterFalloff(makeEmitterOmni(), vm::vec3d{96, 0, 0})
      == Catch::Approx(w * w / (d * d + 1.0)).margin(Exact));
    CHECK(emitterFalloff(makeEmitterOmni(), vm::vec3d{288, 0, 0}) == 0.0);

    // nothing behind a spot's bulb
    CHECK(emitterFalloff(emitterSpot(), vm::vec3d{-10, 0, 0}) == 0.0);
    CHECK(emitterFalloff(emitterSpot(), vm::vec3d{128, 0, 0}) > 0.0);
  }

  SECTION("containsPoint of an emitter")
  {
    const auto atOrigin = LightFrame{vm::vec3d{0, 0, 0}, identity};
    const auto spot = emitterSpot();
    const auto tanHalf = std::tan(vm::to_radians(30.0));
    CHECK(containsPoint(spot, atOrigin, vm::vec3d{100, 0, 0}));
    CHECK(containsPoint(spot, atOrigin, vm::vec3d{250, 0, 0}));
    // behind the bulb, beyond the range, outside the cone
    CHECK_FALSE(containsPoint(spot, atOrigin, vm::vec3d{-10, 0, 0}));
    CHECK_FALSE(containsPoint(spot, atOrigin, vm::vec3d{260, 0, 0}));
    CHECK_FALSE(containsPoint(spot, atOrigin, vm::vec3d{100, 101 * tanHalf, 0}));

    // an omni's ellipsoid is centred on its bulb
    auto omni = makeEmitterOmni();
    omni.hasCenter = true;
    omni.center = vm::vec3d{0, 0, 100};
    CHECK(containsPoint(omni, atOrigin, vm::vec3d{0, 0, 380}));
    CHECK_FALSE(containsPoint(omni, atOrigin, vm::vec3d{0, 0, -200}));
  }
}

} // namespace tb::mdl
