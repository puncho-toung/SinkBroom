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

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/mat.h"
#include "vm/mat_io.h" // IWYU pragma: keep
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>
#include <optional>
#include <sstream>
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
 * A point light of radius 300 with the math block that the light editor seeds (D29):
 * linear, inner radius 0.1.
 */
LightProperties seededPoint()
{
  auto light = defaultPoint;
  light.math = seededMathLightProperties();
  return light;
}

/**
 * The t at which 1 - smoothstep(t) is the given level: smoothstep(t) = 1 - level, whose
 * inverse is 1/2 - sin(asin(1 - 2 y) / 3).
 */
double distanceOf(const std::optional<double>& distance)
{
  REQUIRE(distance);
  return *distance;
}

double legacyVisibleT(const double level)
{
  const auto y = 1.0 - level;
  return 0.5 - std::sin(std::asin(1.0 - 2.0 * y) / 3.0);
}

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
      CHECK(lightFromEditBox(noTarget, identity, vm::bbox3d{100.0}) == noTarget);

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
      // the seeded light is visible to t = 0.91 at 10 %
      const auto seededBox = lightEditBox(seededPoint(), DefaultThreshold);
      REQUIRE(seededBox);
      CHECK(vm::approx{vm::bbox3d{273.0}, 1e-4} == *seededBox);

      // a light without math, by the smooth stand-in
      const auto legacyT = legacyVisibleT(DefaultThreshold);
      const auto legacyBox = lightEditBox(defaultPoint, DefaultThreshold);
      REQUIRE(legacyBox);
      CHECK(vm::approx{vm::bbox3d{300.0 * legacyT}, 1e-4} == *legacyBox);

      // per axis
      auto ellipsoid = seededPoint();
      ellipsoid.radius = vm::vec3d{100, 200, 300};
      CHECK(
        vm::approx{vm::bbox3d{vm::vec3d{-91, -182, -273}, vm::vec3d{91, 182, 273}}, 1e-4}
        == *lightEditBox(ellipsoid, DefaultThreshold));

      // spot lights and suns keep their full box (Q15)
      CHECK(lightEditBox(defaultSpot, DefaultThreshold) == lightEditBox(defaultSpot));
      CHECK(lightEditBox(sun, DefaultThreshold) == lightEditBox(sun));

      CHECK(lightEditBox(flat, DefaultThreshold) == std::nullopt);
    }
  }

  SECTION("visibleFalloffT")
  {
    const auto withMath = [](const MathLightProperties& math) {
      auto light = defaultPoint;
      light.math = math;
      return light;
    };

    const auto linear = MathLightProperties{
      .enabled = true, .falloff = LightFalloff::Linear, .innerRadius = 0.0};
    CHECK(
      visibleFalloffT(withMath(linear), DefaultThreshold)
      == Catch::Approx(0.9).margin(Exact));

    // u = (t - 0.1) / 0.9 = 0.9
    CHECK(
      visibleFalloffT(seededPoint(), DefaultThreshold)
      == Catch::Approx(0.91).margin(Exact));

    SECTION("The windowed inverse square, solved longhand")
    {
      // the default math block: invsquare_clamped, k = 2, inner radius 0, so u = t
      const auto math = MathLightProperties{.enabled = true};
      REQUIRE(math.falloff == LightFalloff::InvSquareClamped);
      const auto falloff = [](const double u) {
        const auto w = 1.0 - u * u * u * u;
        const auto d = 2.0 * u + 1e-3;
        return w * w / (d * d + 1.0);
      };

      // 60 bisection steps of the curve as written out here
      auto lo = 0.0;
      auto hi = 1.0;
      for (auto i = 0; i < 60; ++i)
      {
        const auto mid = (lo + hi) / 2.0;
        (falloff(mid) >= DefaultThreshold ? lo : hi) = mid;
      }
      REQUIRE(falloff(lo) == Catch::Approx(DefaultThreshold).margin(1e-9));
      CHECK(
        visibleFalloffT(withMath(math), DefaultThreshold)
        == Catch::Approx(lo).margin(Exact));
    }

    SECTION("A light without math by the smooth stand-in")
    {
      CHECK(
        visibleFalloffT(defaultPoint, DefaultThreshold)
        == Catch::Approx(legacyVisibleT(DefaultThreshold)).margin(Exact));
      CHECK(visibleFalloffT(defaultPoint, 0.5) == Catch::Approx(0.5).margin(Exact));

      // a disabled math block is not read
      auto disabled = seededPoint();
      disabled.math.enabled = false;
      CHECK(
        visibleFalloffT(disabled, DefaultThreshold)
        == Catch::Approx(legacyVisibleT(DefaultThreshold)).margin(Exact));
    }

    SECTION("An inner radius of 0.999 is visible to its edge")
    {
      auto sharp = linear;
      sharp.innerRadius = 0.999;
      CHECK(visibleFalloffT(withMath(sharp), DefaultThreshold) == 1.0);
    }

    SECTION("The threshold is clamped to [0.01, 0.5]")
    {
      CHECK(visibleFalloffT(withMath(linear), 0.9) == Catch::Approx(0.5).margin(Exact));
      CHECK(
        visibleFalloffT(withMath(linear), 0.001) == Catch::Approx(0.99).margin(Exact));
    }
  }

  SECTION("lightFromEditBox")
  {
    SECTION("Round trips")
    {
      // the default point light
      CHECK(
        lightFromEditBox(defaultPoint, identity, *lightEditBox(defaultPoint))
        == defaultPoint);

      // a rotated point light
      auto rotatedPoint = defaultPoint;
      rotatedPoint.origin = vm::vec3d{64, 32, 16};
      rotatedPoint.radius = vm::vec3d{300, 200, 100};
      rotatedPoint.equalRadius = false;
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto rotatedResult =
        lightFromEditBox(rotatedPoint, R, *lightEditBox(rotatedPoint));
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

      const auto moved = lightFromEditBox(rotatedPoint, R, box);
      CHECK(moved.origin == vm::approx{vm::vec3d{64 - 20, 32 + 10, 16 + 30}, Exact});
      CHECK(moved.radius == vm::approx{rotatedPoint.radius, Exact});

      // and back
      const auto back =
        lightFromEditBox(moved, R, lightEditBox(moved)->translate(-offset));
      CHECK(back.origin == vm::approx{rotatedPoint.origin, Exact});
      CHECK(back.radius == vm::approx{rotatedPoint.radius, Exact});
    }

    SECTION("Round trips of spot lights")
    {

      // the default spot light
      const auto defaultBox = *lightEditBox(defaultSpot);
      const auto spot = lightFromEditBox(defaultSpot, identity, defaultBox);
      CHECK(vm::approx{defaultBox, Exact} == *lightEditBox(spot));
      CHECK(spot.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK(spot.target == vm::approx{defaultSpot.target, Exact});
      CHECK(spot.right == vm::approx{defaultSpot.right, Exact});
      CHECK(spot.up == vm::approx{defaultSpot.up, Exact});
      CHECK_FALSE(spot.explicitStartEnd);

      // the explicit spot light: the same lit volume with a shorter target
      const auto explicitBox = *lightEditBox(explicitSpot);
      const auto explicitResult = lightFromEditBox(explicitSpot, identity, explicitBox);
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
      const auto light =
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {164, 132, 264}));
      CHECK(vm::length(light.right) == Catch::Approx(143.5234).margin(Tolerance));
      CHECK(light.target == vm::approx{vm::vec3d{-15.516, 0, -256.015}, Tolerance});
      CHECK(vm::length(light.up) == Catch::Approx(128.0073).margin(Tolerance));
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK(vm::length(light.target) == Catch::Approx(256.484).margin(Tolerance));
      CHECK(light.up.y() < 0.0);
    }

    SECTION("(a) The world +X face dragged by 32")
    {
      const auto light =
        lightFromEditBox(defaultSpot, identity, box({-164, -132, 0}, {132, 132, 264}));
      CHECK(light.target == vm::approx{vm::vec3d{15.516, 0, -256.015}, Tolerance});
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
    }

    SECTION("(b) The apex face dragged by 16")
    {
      const auto light =
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 16}, {132, 132, 264}));
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
      const auto light =
        lightFromEditBox(defaultSpot, R, box({-132, -132, 16}, {132, 132, 264}));
      CHECK(light.origin == vm::approx{16.0 * (R * vm::vec3d{0, 0, -1}), Exact});
    }

    SECTION("(c) The far face dragged by 64")
    {
      const auto light =
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 328}));
      CHECK(light.target == vm::approx{vm::vec3d{0, 0, -320}, Tolerance});
      CHECK(vm::length(light.right) == Catch::Approx(128.7805).margin(Tolerance));
      CHECK(light.origin == vm::approx{defaultSpot.origin, Exact});
      CHECK_FALSE(light.explicitStartEnd);
    }

    SECTION("(c') The far face of an explicit spot light dragged by 64")
    {
      const auto light =
        lightFromEditBox(explicitSpot, identity, box({-116, -116, 0}, {116, 116, 296}));
      CHECK(light.explicitStartEnd);
      CHECK(light.end == vm::approx{vm::vec3d{0, 0, -264}, Tolerance});
      CHECK(light.start == vm::approx{vm::vec3d{0, 0, -32}, Tolerance});
      CHECK(light.target == vm::approx{vm::vec3d{0, 0, -264}, Tolerance});
      CHECK(vm::length(light.right) == Catch::Approx(103.4595).margin(Tolerance));
      CHECK(light.origin == vm::approx{explicitSpot.origin, Exact});
    }

    SECTION("(d) A point light's side dragged along +x by 32")
    {
      const auto light = lightFromEditBox(
        defaultPoint, identity, box({-300, -300, -300}, {332, 300, 300}));
      CHECK(light.origin == vm::approx{vm::vec3d{16, 0, 0}, Exact});
      CHECK(light.radius == vm::approx{vm::vec3d{316, 300, 300}, Exact});
      CHECK_FALSE(light.equalRadius);

      // the centre moves in the light's frame
      const auto R = rotationOf({{"classname", "light"}, {"angles", "0 90 0"}});
      const auto rotated =
        lightFromEditBox(defaultPoint, R, box({-300, -300, -300}, {332, 300, 300}));
      CHECK(rotated.origin == vm::approx{vm::vec3d{0, 16, 0}, Exact});
    }

    SECTION("Denied boxes return the base")
    {
      // depth 23
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 23}))
        == defaultSpot);
      // half extent 0.5
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-0.5, -132, 0}, {0.5, 132, 264}))
        == defaultSpot);
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-132, -0.5, 0}, {132, 0.5, 264}))
        == defaultSpot);
      CHECK(
        lightFromEditBox(defaultPoint, identity, box({-300, -300, -0.5}, {300, 300, 0.5}))
        == defaultPoint);

      // depth 24 is allowed
      CHECK(
        lightFromEditBox(defaultSpot, identity, box({-132, -132, 0}, {132, 132, 24}))
        != defaultSpot);
    }

    SECTION("With a threshold, the handle box divides by the visible t (D61)")
    {
      SECTION("Round trips")
      {
        for (const auto& light : {defaultPoint, seededPoint()})
        {
          const auto handleBox = lightEditBox(light, DefaultThreshold);
          REQUIRE(handleBox);
          const auto result =
            lightFromEditBox(light, identity, *handleBox, DefaultThreshold);
          CHECK(result.origin == vm::approx{light.origin, Exact});
          CHECK(result.radius == vm::approx{light.radius, Exact});
          CHECK(result.equalRadius);
        }

        // spot lights and suns as without a threshold
        CHECK(
          lightFromEditBox(
            defaultSpot, identity, *lightEditBox(defaultSpot), DefaultThreshold)
          == lightFromEditBox(defaultSpot, identity, *lightEditBox(defaultSpot)));
        CHECK(
          lightFromEditBox(sun, identity, *lightEditBox(sun), DefaultThreshold)
          == lightFromEditBox(sun, identity, *lightEditBox(sun)));
      }

      SECTION("The seeded light's +x face dragged by 32")
      {
        const auto t = 0.91;

        // the scale tool's face drag: the +x face follows the handle by 32, the -x face
        // stays, so the half extent grows by 16 and the radius by 16 / t; the centre
        // follows by 16
        const auto light = lightFromEditBox(
          seededPoint(),
          identity,
          box({-273, -273, -273}, {273 + 32, 273, 273}),
          DefaultThreshold);
        CHECK(light.radius.x() == Catch::Approx(300.0 + 16.0 / t).margin(Tolerance));
        CHECK(light.radius.y() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(light.radius.z() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(light.origin == vm::approx{vm::vec3d{16, 0, 0}, Tolerance});
        CHECK_FALSE(light.equalRadius);

        // both x faces by 32 (anchored at the centre): the radius grows by 32 / t, the
        // centre stays
        const auto centred = lightFromEditBox(
          seededPoint(),
          identity,
          box({-273 - 32, -273, -273}, {273 + 32, 273, 273}),
          DefaultThreshold);
        CHECK(centred.radius.x() == Catch::Approx(300.0 + 32.0 / t).margin(Tolerance));
        CHECK(centred.radius.x() == Catch::Approx(335.165).margin(Tolerance));
        CHECK(centred.radius.y() == Catch::Approx(300.0).margin(Tolerance));
        CHECK(centred.origin == vm::approx{vm::vec3d{0, 0, 0}, Exact});
      }

      SECTION("A half extent below 1 is denied")
      {
        CHECK(
          lightFromEditBox(
            seededPoint(),
            identity,
            box({-273, -273, -0.5}, {273, 273, 0.5}),
            DefaultThreshold)
          == seededPoint());
      }
    }
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

  SECTION("lightFalloff")
  {
    const auto ts = std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0};
    const auto k = 2.0;

    const auto mathLight = [&](const LightFalloff falloff, const double inner = 0.0) {
      return MathLightProperties{
        .enabled = true, .falloff = falloff, .falloffExp = k, .innerRadius = inner};
    };

    // the curves of MLFalloffCurve, written out for inner = 0, where u = t
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

        const auto actual = lightFalloff(t, mathLight(falloff));
        CHECK(actual == Catch::Approx(expected(falloff, t)).margin(Exact).epsilon(0));

        const auto atEnd = i == 0 || i == ts.size() - 1;
        CHECK(
          actual
          == Catch::Approx(values[i]).epsilon(0).margin(atEnd ? Exact : Tolerance));
      }
    }

    // inner shifts the curve: u = (t - inner) / (1 - inner)
    CHECK(lightFalloff(0.25, mathLight(LightFalloff::Linear, 0.5)) == 1.0);
    CHECK(lightFalloff(0.5, mathLight(LightFalloff::Linear, 0.5)) == 1.0);
    CHECK(
      lightFalloff(0.75, mathLight(LightFalloff::Linear, 0.5))
      == Catch::Approx(0.5).margin(Exact));
    CHECK(
      lightFalloff(0.75, mathLight(LightFalloff::Smooth, 0.5))
      == Catch::Approx(0.5).margin(Exact));
    CHECK(
      lightFalloff(0.875, mathLight(LightFalloff::InvSquareClamped, 0.5))
      == Catch::Approx(expected(LightFalloff::InvSquareClamped, 0.75)).margin(Exact));
    CHECK(lightFalloff(1.0, mathLight(LightFalloff::Exponential, 0.5)) == 0.0);

    // an inner radius of 1 is a hard edge
    CHECK(lightFalloff(0.999, mathLight(LightFalloff::Linear, 1.0)) == 1.0);
    CHECK(lightFalloff(1.0, mathLight(LightFalloff::Linear, 1.0)) == 1.0);

    // t outside 0..1 is saturated
    CHECK(lightFalloff(-1.0, mathLight(LightFalloff::Linear)) == 1.0);
    CHECK(lightFalloff(2.0, mathLight(LightFalloff::Linear)) == 0.0);
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
      return MathLightProperties{.shapeExp = shapeExp, .penumbra = penumbra};
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
      const auto seeded = pointFromWorldBox(W, seededPoint(), DefaultThreshold);
      CHECK(seeded.origin == vm::approx{vm::vec3d{64, 64, 32}, Exact});
      CHECK(seeded.radius == vm::approx{vm::vec3d{64, 64, 32} / 0.91, Tolerance});
      CHECK(
        vm::approx{box({0, 0, 0}, {128, 128, 64}).translate(-seeded.origin), 1e-4}
        == *lightEditBox(seeded, DefaultThreshold));

      const auto legacy = pointFromWorldBox(W, defaultPoint, DefaultThreshold);
      CHECK(
        legacy.radius
        == vm::approx{
          vm::vec3d{64, 64, 32} / legacyVisibleT(DefaultThreshold), Tolerance});

      // a sun fills the box
      CHECK(
        pointFromWorldBox(W, sun, DefaultThreshold).radius
        == vm::approx{vm::vec3d{64, 64, 32}, Exact});

      CHECK(
        pointFromWorldBox(box({0, 0, 0}, {128, 128, 1}), seededPoint(), DefaultThreshold)
        == seededPoint());
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
}

} // namespace tb::mdl
