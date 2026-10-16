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

#include "gl/Vertex.h"
#include "mdl/Entity.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "render/LightVolumeMesh.h"

#include "vm/intersection.h"
#include "vm/mat.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <set>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::render
{
namespace
{

using Vertex = gl::VertexTypes::P3C4::Vertex;

constexpr auto Tolerance = 1e-4;

const auto identity = vm::mat3x3d{};
const auto atOrigin = mdl::LightFrame{vm::vec3d{0, 0, 0}, identity};

vm::vec3d positionOf(const Vertex& vertex)
{
  return vm::vec3d{gl::getVertexComponent<0>(vertex)};
}

double alphaOf(const Vertex& vertex)
{
  return double(gl::getVertexComponent<1>(vertex).w());
}

/**
 * A point light with a linear math falloff, inner radius 0 and _color 1 1 1.
 */
mdl::LightProperties linearPointLight()
{
  auto light = mdl::LightProperties{};
  light.math.enabled = true;
  light.math.falloff = mdl::LightFalloff::Linear;
  light.math.innerRadius = 0.0;
  return light;
}

/**
 * The engine's default spot light (DefaultProjected), with a linear math falloff.
 */
mdl::LightProperties linearSpotLight()
{
  auto light = linearPointLight();
  light.type = mdl::LightType::Spot;
  light.target = vm::vec3d{0, 0, -256};
  light.right = vm::vec3d{-128, 0, 0};
  light.up = vm::vec3d{0, -128, 0};
  return light;
}

std::vector<double> peel(const std::vector<double>& means)
{
  auto alphas = std::vector<double>{};
  for (std::size_t j = 0; j < means.size(); ++j)
  {
    const auto next = j + 1 < means.size() ? means[j + 1] : 0.0;
    alphas.push_back((means[j] - next) / (1.0 - next));
  }
  return alphas;
}

bool approxEqual(const std::vector<double>& lhs, const std::vector<double>& rhs)
{
  return lhs.size() == rhs.size()
         && std::ranges::equal(lhs, rhs, [](const auto l, const auto r) {
              return std::abs(l - r) < Tolerance;
            });
}

/**
 * The alpha that a ray composites through the given triangles under the cull rule with
 * the camera outside: only the triangles that face the ray (CW seen from the ray's
 * origin, i.e. whose normal cross(p3 - p1, p2 - p1) points against the ray) are drawn.
 */
double rayComposite(const LightVolumeMesh& mesh, const vm::ray3d& ray)
{
  auto transmittance = 1.0;
  for (std::size_t i = 0; i + 2 < mesh.triangles.size(); i += 3)
  {
    const auto p1 = positionOf(mesh.triangles[i]);
    const auto p2 = positionOf(mesh.triangles[i + 1]);
    const auto p3 = positionOf(mesh.triangles[i + 2]);
    const auto normal = vm::cross(p3 - p1, p2 - p1);
    if (
      vm::dot(normal, ray.direction) < 0.0 && vm::intersect_ray_triangle(ray, p1, p2, p3))
    {
      transmittance *= 1.0 - alphaOf(mesh.triangles[i]);
    }
  }
  return 1.0 - transmittance;
}

/**
 * Whether every triangle is CW seen from outside: its normal cross(p3 - p1, p2 - p1)
 * points away from the given interior point of the (convex) volume.
 */
bool allTrianglesFaceOutwards(const LightVolumeMesh& mesh, const vm::vec3d& interior)
{
  for (std::size_t i = 0; i + 2 < mesh.triangles.size(); i += 3)
  {
    const auto p1 = positionOf(mesh.triangles[i]);
    const auto p2 = positionOf(mesh.triangles[i + 1]);
    const auto p3 = positionOf(mesh.triangles[i + 2]);
    const auto normal = vm::cross(p3 - p1, p2 - p1);
    const auto centroid = (p1 + p2 + p3) / 3.0;
    if (vm::dot(normal, centroid - interior) <= 0.0)
    {
      return false;
    }
  }
  return true;
}

bool hasLine(const LightVolumeMesh& mesh, const vm::vec3d& start, const vm::vec3d& end)
{
  for (std::size_t i = 0; i + 1 < mesh.lines.size(); i += 2)
  {
    const auto p1 = positionOf(mesh.lines[i]);
    const auto p2 = positionOf(mesh.lines[i + 1]);
    if (
      (vm::length(p1 - start) < 1e-3 && vm::length(p2 - end) < 1e-3)
      || (vm::length(p1 - end) < 1e-3 && vm::length(p2 - start) < 1e-3))
    {
      return true;
    }
  }
  return false;
}

bool hasLineVertex(const LightVolumeMesh& mesh, const vm::vec3d& point)
{
  return std::ranges::any_of(mesh.lines, [&](const auto& vertex) {
    return vm::length(positionOf(vertex) - point) < 1e-3;
  });
}

/**
 * The shells of a point light at the origin with the given radius: the index j of the
 * shell at t_j = (j + 1) / shells that each triangle vertex lies on.
 */
std::vector<std::size_t> shellIndices(
  const LightVolumeMesh& mesh, const double radius, const std::size_t shells)
{
  auto result = std::vector<std::size_t>{};
  for (const auto& vertex : mesh.triangles)
  {
    const auto t = vm::length(positionOf(vertex)) / radius;
    result.push_back(std::size_t(std::lround(t * double(shells))) - 1);
  }
  return result;
}

/**
 * The projected position (u, v) and the depth d of a point of the default spot light.
 */
std::array<double, 3> defaultSpotCoordinates(const vm::vec3d& point)
{
  const auto d = -point.z();
  const auto projected = point * (256.0 / d);
  return {-projected.x() / 128.0, -projected.y() / 128.0, d};
}

double superellipseRadius(const double u, const double v, const double k)
{
  return std::pow(std::pow(std::abs(u), k) + std::pow(std::abs(v), k), 1.0 / k);
}

} // namespace

TEST_CASE("LightVolumeMesh")
{
  const auto style = LightVolumeStyle{0.45, 4, false, false};

  // the band means 0.45 * {0.875, 0.625, 0.375, 0.125} of a linear light with 4 shells
  const auto linearMeans =
    std::vector<double>{0.45 * 0.875, 0.45 * 0.625, 0.45 * 0.375, 0.45 * 0.125};

  SECTION("lightVolumeFalloff")
  {
    auto math = mdl::MathLightProperties{};

    SECTION("a math light is its falloff curve, at most 1")
    {
      math.enabled = true;
      math.falloff = mdl::LightFalloff::Linear;
      CHECK(lightVolumeFalloff(0.25, math) == Catch::Approx(0.75));

      math.falloff = mdl::LightFalloff::InvSquare;
      math.energyClamp = false;
      CHECK(mdl::lightFalloff(0.0, math) > 1.0);
      CHECK(lightVolumeFalloff(0.0, math) == 1.0);
    }

    SECTION("an inner radius of 0.999 is 1 everywhere")
    {
      math.enabled = true;
      math.falloff = mdl::LightFalloff::Linear;
      math.innerRadius = 0.999;
      CHECK(mdl::lightFalloff(1.0, math) == 0.0);
      CHECK(lightVolumeFalloff(0.0, math) == 1.0);
      CHECK(lightVolumeFalloff(1.0, math) == 1.0);
    }

    SECTION("an inner radius of 0.99 is not sharp yet")
    {
      math.enabled = true;
      math.falloff = mdl::LightFalloff::Linear;
      math.innerRadius = 0.99;
      CHECK(lightVolumeFalloff(0.0, math) == 1.0);
      CHECK(lightVolumeFalloff(1.0, math) == 0.0);
    }

    SECTION("a light without math is the smooth stand-in")
    {
      CHECK(lightVolumeFalloff(0.25, math) == Catch::Approx(mdl::legacyFalloff(0.25)));
      CHECK(lightVolumeFalloff(0.5, math) == Catch::Approx(0.5));
    }
  }

  SECTION("lightVolumeAlphaCap")
  {
    auto light = mdl::LightProperties{};
    CHECK(lightVolumeAlphaCap(light, 0.45) == 0.45);

    // the energy clamp of a legacy light means nothing
    light.math.energyClamp = false;
    CHECK(lightVolumeAlphaCap(light, 0.45) == 0.45);
    light.math.energyClamp = true;

    light.math.enabled = true;
    CHECK(lightVolumeAlphaCap(light, 0.45) == 0.45);

    light.math.energyClamp = false;
    CHECK(lightVolumeAlphaCap(light, 0.45) == 1.0);

    light.type = mdl::LightType::Spot;
    CHECK(lightVolumeAlphaCap(light, 0.45) == 1.0);

    // a sun's energy clamp bounds only its colour
    light.type = mdl::LightType::Sun;
    CHECK(lightVolumeAlphaCap(light, 0.45) == 0.45);
  }

  SECTION("lightVolumeHue")
  {
    CHECK(lightVolumeHue(vm::vec3d{2, 1, 0}) == vm::vec3d{1, 0.5, 0});
    CHECK(lightVolumeHue(vm::vec3d{0.5, 0.25, 0}) == vm::vec3d{1, 0.5, 0});
    CHECK(lightVolumeHue(vm::vec3d{0, 0, 0}) == vm::vec3d{0, 0, 0});
  }

  SECTION("lightVolumeBrightness")
  {
    CHECK(lightVolumeBrightness(vm::vec3d{2, 1, 0}) == 1.0);
    CHECK(lightVolumeBrightness(vm::vec3d{0.5, 0.25, 0}) == 0.5);
    CHECK(lightVolumeBrightness(vm::vec3d{0.1, 0, 0}) == 0.25);
  }

  SECTION("lightVolumeBandMeans")
  {
    CHECK(approxEqual(lightVolumeBandMeans(linearPointLight(), style), linearMeans));
  }

  SECTION("lightVolumeShellAlphas")
  {
    SECTION("a linear light peels its band means")
    {
      const auto alphas = lightVolumeShellAlphas(linearPointLight(), style);
      CHECK(approxEqual(alphas, peel(linearMeans)));
      CHECK(approxEqual(alphas, {0.1565, 0.1353, 0.1192, 0.0563}));
    }

    SECTION("an inner radius of 0.999 gives one sharp shell at the cap")
    {
      auto light = linearPointLight();
      light.math.innerRadius = 0.999;
      CHECK(approxEqual(lightVolumeShellAlphas(light, style), {0, 0, 0, 0.45}));
    }

    SECTION("outerShellOnly gives one shell at C_1")
    {
      auto outerOnly = style;
      outerOnly.outerShellOnly = true;
      CHECK(approxEqual(
        lightVolumeShellAlphas(linearPointLight(), outerOnly), {linearMeans.front()}));
    }

    SECTION("a legacy light peels the means of the smooth stand-in")
    {
      auto light = linearPointLight();
      light.math.enabled = false;

      auto means = std::vector<double>{};
      for (const auto j : {1.0, 2.0, 3.0, 4.0})
      {
        means.push_back(
          0.45 * (mdl::legacyFalloff((j - 1.0) / 4.0) + mdl::legacyFalloff(j / 4.0))
          / 2.0);
      }
      CHECK(approxEqual(lightVolumeShellAlphas(light, style), peel(means)));
    }

    SECTION("twoD scales the alphas by 0.6")
    {
      auto twoD = style;
      twoD.twoD = true;

      auto expected = peel(linearMeans);
      std::ranges::transform(
        expected, expected.begin(), [](const auto alpha) { return 0.6 * alpha; });
      CHECK(approxEqual(lightVolumeShellAlphas(linearPointLight(), twoD), expected));
    }
  }

  SECTION("buildLightVolumeMesh")
  {
    // rays along +x that avoid every edge of the tessellation
    const auto throughCentre = vm::ray3d{vm::vec3d{-1000, 11, -4}, vm::vec3d{1, 0, 0}};
    const auto throughRim = vm::ray3d{vm::vec3d{-1000, 262, -4}, vm::vec3d{1, 0, 0}};

    SECTION("a point light draws its shells with the peeled alphas")
    {
      const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, style);

      // four 16 x 8 shells of 224 triangles each
      REQUIRE(mesh.triangles.size() == 4 * 224 * 3);

      const auto expectedAlphas = peel(linearMeans);
      const auto shells = shellIndices(mesh, 300.0, 4);
      for (std::size_t i = 0; i < mesh.triangles.size(); ++i)
      {
        REQUIRE(shells[i] < 4);
        CHECK(alphaOf(mesh.triangles[i]) == Catch::Approx(expectedAlphas[shells[i]]));
      }

      CHECK(allTrianglesFaceOutwards(mesh, vm::vec3d{0, 0, 0}));
    }

    SECTION("a ray composites C_1 through the centre and C_K through the rim")
    {
      const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, style);
      CHECK(rayComposite(mesh, throughCentre) == Catch::Approx(0.3938).margin(1e-4));
      CHECK(rayComposite(mesh, throughCentre) == Catch::Approx(linearMeans[0]));
      CHECK(rayComposite(mesh, throughRim) == Catch::Approx(0.0563).margin(1e-4));
      CHECK(rayComposite(mesh, throughRim) == Catch::Approx(linearMeans[3]));
    }

    SECTION("an unclamped inverse square light is opaque at its core only at b = 1")
    {
      auto light = linearPointLight();
      light.math.falloff = mdl::LightFalloff::InvSquare;
      light.math.falloffExp = 2.0;
      light.math.energyClamp = false;

      const auto bright = buildLightVolumeMesh(light, atOrigin, style);
      CHECK(lightVolumeBandMeans(light, style).front() == Catch::Approx(1.0));
      CHECK(rayComposite(bright, throughCentre) == Catch::Approx(1.0));

      light.color = vm::vec3d{0.5, 0.5, 0.5};
      const auto dim = buildLightVolumeMesh(light, atOrigin, style);
      CHECK(rayComposite(dim, throughCentre) == Catch::Approx(0.5));
    }

    SECTION("an inner radius of 0.999 draws one sharp shell at the cap")
    {
      auto light = linearPointLight();
      light.math.innerRadius = 0.999;

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      REQUIRE(mesh.triangles.size() == 224 * 3);
      CHECK(std::ranges::all_of(mesh.triangles, [](const auto& vertex) {
        return std::abs(vm::length(positionOf(vertex)) - 300.0) < 1e-3
               && std::abs(alphaOf(vertex) - 0.45) < Tolerance;
      }));
      CHECK(rayComposite(mesh, throughCentre) == Catch::Approx(0.45));
    }

    SECTION("outerShellOnly draws one shell at C_1")
    {
      auto outerOnly = style;
      outerOnly.outerShellOnly = true;

      const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, outerOnly);
      REQUIRE(mesh.triangles.size() == 224 * 3);
      CHECK(rayComposite(mesh, throughCentre) == Catch::Approx(linearMeans[0]));
    }

    SECTION("a point light draws its clip box and centre cross")
    {
      auto light = linearPointLight();
      light.radius = vm::vec3d{32, 16, 8};
      light.hasCenter = true;
      light.center = vm::vec3d{0, 0, 4};

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);

      // 12 box edges and the 3 lines of the cross
      CHECK(mesh.lines.size() == (12 + 3) * 2);
      CHECK(hasLine(mesh, vm::vec3d{-32, -16, -8}, vm::vec3d{32, -16, -8}));
      CHECK(hasLine(mesh, vm::vec3d{32, 16, -8}, vm::vec3d{32, 16, 8}));
      CHECK(hasLine(mesh, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 8}));
      CHECK(std::ranges::all_of(
        mesh.lines, [](const auto& vertex) { return alphaOf(vertex) == 0.9f; }));
    }

    SECTION("a point light without light_center draws its clip box only")
    {
      const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, style);
      CHECK(mesh.lines.size() == 12 * 2);
    }

    SECTION("the frame places and turns the shells, scaled per axis")
    {
      const auto frame = mdl::lightFrame(
        mdl::Entity{{
          {"classname", "light"},
          {"origin", "10 20 30"},
          {"angles", "0 90 0"},
        }},
        mdl::EntityRotationPolicy::IdTech4);
      REQUIRE(frame.origin == vm::vec3d{10, 20, 30});
      REQUIRE(vm::length(frame.R * vm::vec3d{1, 0, 0} - vm::vec3d{1, 0, 0}) > 0.5);

      auto light = linearPointLight();
      light.radius = vm::vec3d{200, 100, 50};

      const auto mesh = buildLightVolumeMesh(light, frame, style);
      REQUIRE(mesh.triangles.size() == 4 * 224 * 3);
      CHECK(std::ranges::all_of(mesh.triangles, [&](const auto& vertex) {
        const auto local = vm::transpose(frame.R) * (positionOf(vertex) - frame.origin);
        const auto t = vm::length(vm::vec3d{
          local.x() / light.radius.x(),
          local.y() / light.radius.y(),
          local.z() / light.radius.z()});
        return std::ranges::any_of(std::array{0.25, 0.5, 0.75, 1.0}, [&](const auto tj) {
          return std::abs(t - tj) < 1e-3;
        });
      }));
      CHECK(allTrianglesFaceOutwards(mesh, frame.origin));

      const auto spot = linearSpotLight();
      const auto spotMesh = buildLightVolumeMesh(spot, frame, style);
      for (const auto x : {-132.0, 132.0})
      {
        for (const auto y : {-132.0, 132.0})
        {
          CHECK(hasLineVertex(spotMesh, frame.R * vm::vec3d{x, y, -264} + frame.origin));
        }
      }
      CHECK(allTrianglesFaceOutwards(
        spotMesh, frame.R * mdl::spotPoint(spot, 0, 0, 136) + frame.origin));
    }

    SECTION("twoD scales the fill alphas by 0.6 and keeps the lines")
    {
      auto twoD = style;
      twoD.twoD = true;

      const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, style);
      const auto mesh2D = buildLightVolumeMesh(linearPointLight(), atOrigin, twoD);
      REQUIRE(mesh2D.triangles.size() == mesh.triangles.size());
      for (std::size_t i = 0; i < mesh.triangles.size(); ++i)
      {
        CHECK(
          alphaOf(mesh2D.triangles[i])
          == Catch::Approx(0.6 * alphaOf(mesh.triangles[i])).margin(1e-6));
      }
      CHECK(mesh2D.lines.size() == mesh.lines.size());
      CHECK(std::ranges::all_of(
        mesh2D.lines, [](const auto& vertex) { return alphaOf(vertex) == 0.9f; }));
    }

    SECTION("a sun draws one uniform box shell and its direction arrow")
    {
      auto light = mdl::LightProperties{};
      light.type = mdl::LightType::Sun;
      light.radius = vm::vec3d{128, 64, 32};
      light.color = vm::vec3d{0.5, 0.5, 0.5};

      const auto frame = mdl::LightFrame{vm::vec3d{10, 20, 30}, identity};
      const auto mesh = buildLightVolumeMesh(light, frame, style);

      REQUIRE(mesh.triangles.size() == 12 * 3);
      CHECK(std::ranges::all_of(mesh.triangles, [](const auto& vertex) {
        return std::abs(alphaOf(vertex) - 0.45 * 0.5) < Tolerance;
      }));
      CHECK(allTrianglesFaceOutwards(mesh, frame.origin));

      // without light_center, the arrow points down
      CHECK(hasLine(mesh, vm::vec3d{10, 20, 30}, vm::vec3d{10, 20, -18}));
      CHECK(hasLine(mesh, vm::vec3d{-118, -44, -2}, vm::vec3d{138, -44, -2}));

      // light_center is the direction to the light in world space
      light.hasCenter = true;
      light.center = vm::vec3d{2, 0, 0};
      const auto turned = buildLightVolumeMesh(light, frame, style);
      CHECK(hasLine(turned, vm::vec3d{10, 20, 30}, vm::vec3d{-38, 20, 30}));
    }

    SECTION("the hue colours the triangles and the lines")
    {
      auto light = mdl::LightProperties{};
      light.type = mdl::LightType::Sun;
      light.color = vm::vec3d{2, 1, 0};

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      REQUIRE(!mesh.triangles.empty());
      REQUIRE(!mesh.lines.empty());

      const auto hasHue = [](const auto& vertex) {
        const auto color = gl::getVertexComponent<1>(vertex);
        return color.x() == 1.0f && color.y() == 0.5f && color.z() == 0.0f;
      };
      CHECK(std::ranges::all_of(mesh.triangles, hasHue));
      CHECK(std::ranges::all_of(mesh.lines, hasHue));
    }

    SECTION("an unclamped math sun at b = 1 stays at the alpha cap")
    {
      auto light = mdl::LightProperties{};
      light.type = mdl::LightType::Sun;
      light.math.enabled = true;
      light.math.energyClamp = false;

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      REQUIRE(mesh.triangles.size() == 12 * 3);
      CHECK(std::ranges::all_of(mesh.triangles, [](const auto& vertex) {
        return std::abs(alphaOf(vertex) - 0.45) < Tolerance;
      }));
    }

    SECTION("a spot light draws its cone, penumbra surface, caps and frustum")
    {
      const auto light = linearSpotLight();
      const auto frustum = mdl::spotLightFrustum(light);
      REQUIRE(frustum);
      REQUIRE(frustum->zNear == 8.0);
      REQUIRE(frustum->zFar == 256.0);

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);

      // the ring depths d_j = zNear + zFar j / K and their alphas cap Fvis(t(d_j))
      const auto ringDepths = std::vector<double>{8, 72, 136, 200, 264};
      const auto ringAlpha = [&](const double d) {
        const auto t = mdl::spotFalloffT(d, frustum->zNear, frustum->zFar);
        return 0.45 * lightVolumeFalloff(t, light.math);
      };
      const auto ringOf = [&](const double d) {
        const auto iRing = std::ranges::find_if(
          ringDepths, [&](const auto depth) { return std::abs(depth - d) < 1e-3; });
        return iRing != ringDepths.end()
                 ? std::size_t(std::distance(ringDepths.begin(), iRing))
                 : ringDepths.size();
      };

      auto outerPoints = std::set<std::array<float, 3>>{};
      auto innerPoints = std::set<std::array<float, 3>>{};
      auto nearCapTriangles = 0;
      auto farCapTriangles = 0;
      auto maxDepth = 0.0;

      for (std::size_t i = 0; i + 2 < mesh.triangles.size(); i += 3)
      {
        const auto depths = std::array{
          defaultSpotCoordinates(positionOf(mesh.triangles[i]))[2],
          defaultSpotCoordinates(positionOf(mesh.triangles[i + 1]))[2],
          defaultSpotCoordinates(positionOf(mesh.triangles[i + 2]))[2],
        };
        maxDepth = std::max(maxDepth, std::ranges::max(depths));

        if (
          std::abs(depths[0] - depths[1]) < 1e-3
          && std::abs(depths[0] - depths[2]) < 1e-3)
        {
          // a cap: half the alpha of its ring
          const auto ring = ringOf(depths[0]);
          REQUIRE((ring == 0 || ring == 4));
          (ring == 0 ? nearCapTriangles : farCapTriangles) += 1;
          for (std::size_t k = 0; k < 3; ++k)
          {
            CHECK(
              alphaOf(mesh.triangles[i + k])
              == Catch::Approx(0.5 * ringAlpha(depths[0])).margin(1e-6));
          }
          continue;
        }

        for (std::size_t k = 0; k < 3; ++k)
        {
          const auto& vertex = mesh.triangles[i + k];
          const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
          const auto r = superellipseRadius(u, v, light.math.shapeExp);

          REQUIRE(ringOf(d) < ringDepths.size());
          CHECK(alphaOf(vertex) == Catch::Approx(ringAlpha(d)).margin(1e-6));

          const auto p = gl::getVertexComponent<0>(vertex);
          if (std::abs(r - 1.0) < 1e-4)
          {
            outerPoints.insert({p.x(), p.y(), p.z()});
          }
          else
          {
            CHECK(r == Catch::Approx(1.0 - light.math.penumbra));
            innerPoints.insert({p.x(), p.y(), p.z()});
          }
        }
      }

      CHECK(outerPoints.size() == 32 * (4 + 1));
      CHECK(innerPoints.size() == 32 * (4 + 1));

      // the far cap's alpha is 0 (F(1) = 0), but it is drawn, so that it can be picked
      // and highlighted (D55)
      CHECK(nearCapTriangles == 32);
      CHECK(farCapTriangles == 32);
      CHECK(mesh.triangles.size() == (2 * 32 * 4 * 2 + 2 * 32) * 3);

      // the last ring is on the far cap at zNear + zFar
      CHECK(maxDepth == Catch::Approx(frustum->zNear + frustum->zFar));

      // the alphas decrease along the beam
      for (std::size_t j = 0; j + 1 < ringDepths.size(); ++j)
      {
        CHECK(ringAlpha(ringDepths[j]) > ringAlpha(ringDepths[j + 1]));
      }

      for (const auto x : {-132.0, 132.0})
      {
        for (const auto y : {-132.0, 132.0})
        {
          CHECK(hasLineVertex(mesh, vm::vec3d{x, y, -264}));
          CHECK(hasLine(mesh, vm::vec3d{x / 33.0, y / 33.0, -8}, vm::vec3d{x, y, -264}));
        }
      }

      CHECK(allTrianglesFaceOutwards(mesh, mdl::spotPoint(light, 0, 0, 136)));
    }

    SECTION("a spot light draws its frustum and its three outlines")
    {
      const auto mesh = buildLightVolumeMesh(linearSpotLight(), atOrigin, style);

      // 4 frustum edges, the near and far rectangles, and the near, far and penumbra
      // outlines of 32 segments each, the penumbra outline at the far cap only
      CHECK(mesh.lines.size() == (4 + 4 + 4 + 3 * 32) * 2);
      CHECK(std::ranges::all_of(
        mesh.lines, [](const auto& vertex) { return alphaOf(vertex) == 0.9f; }));

      const auto penumbraVertices =
        std::ranges::count_if(mesh.lines, [&](const auto& vertex) {
          const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
          return std::abs(superellipseRadius(u, v, 2.0) - 0.85) < 1e-4;
        });
      CHECK(penumbraVertices == 32 * 2);
      CHECK(std::ranges::all_of(mesh.lines, [&](const auto& vertex) {
        const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
        return std::abs(superellipseRadius(u, v, 2.0) - 0.85) >= 1e-4
               || std::abs(d - 264.0) < 1e-3;
      }));
    }

    SECTION("a penumbra of 0 or 1 draws no inner surface")
    {
      for (const auto penumbra : {0.0, 1.0})
      {
        auto light = linearSpotLight();
        light.math.penumbra = penumbra;

        const auto mesh = buildLightVolumeMesh(light, atOrigin, style);

        // the outer cone and the two caps; no penumbra outline
        CHECK(mesh.triangles.size() == (32 * 4 * 2 + 2 * 32) * 3);
        CHECK(mesh.lines.size() == (4 + 4 + 4 + 2 * 32) * 2);

        const auto isFinite = [](const auto& vertex) {
          const auto p = gl::getVertexComponent<0>(vertex);
          const auto c = gl::getVertexComponent<1>(vertex);
          return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z())
                 && std::isfinite(c.w());
        };
        CHECK(std::ranges::all_of(mesh.triangles, isFinite));
        CHECK(std::ranges::all_of(mesh.lines, isFinite));
      }
    }

    SECTION("a sharp spot light draws its far cap at half the cap")
    {
      auto light = linearSpotLight();
      light.math.innerRadius = 0.999;

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      REQUIRE(mesh.triangles.size() == (2 * 32 * 4 * 2 + 2 * 32) * 3);

      auto farCapVertices = std::size_t{0};
      for (const auto& vertex : mesh.triangles)
      {
        if (std::abs(positionOf(vertex).z() + 264.0) < 1e-3)
        {
          ++farCapVertices;
        }
      }
      // the far cap's 32 triangles plus the far ring's vertices of both cone surfaces
      CHECK(farCapVertices == 32 * 3 + 2 * 32 * 3);

      const auto farCapAlpha = std::ranges::count_if(mesh.triangles, [](const auto& v) {
        return std::abs(positionOf(v).z() + 264.0) < 1e-3
               && std::abs(alphaOf(v) - 0.5 * 0.45) < Tolerance;
      });
      CHECK(farCapAlpha == 32 * 3);
    }

    SECTION("an unselected spot light draws its outer cone only")
    {
      auto outerOnly = style;
      outerOnly.outerShellOnly = true;

      const auto mesh = buildLightVolumeMesh(linearSpotLight(), atOrigin, outerOnly);

      // one band of 32 quads from the near to the far cap
      CHECK(mesh.triangles.size() == 32 * 2 * 3);
      CHECK(allTrianglesFaceOutwards(mesh, mdl::spotPoint(linearSpotLight(), 0, 0, 136)));
    }

    SECTION("shapeExp shapes the cone")
    {
      auto light = linearSpotLight();
      light.math.shapeExp = 16.0;

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      for (const auto& vertex : mesh.triangles)
      {
        const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
        const auto r = superellipseRadius(u, v, 16.0);
        CHECK(
          (r < 1e-6 || std::abs(r - 1.0) < 1e-4
           || std::abs(r - (1.0 - light.math.penumbra)) < 1e-4));
      }
    }

    SECTION("triangles stay CW seen from outside in a mirrored frame")
    {
      const auto mirrored = mdl::LightFrame{
        vm::vec3d{0, 0, 0},
        vm::mat3x3d{-1, 0, 0, 0, 1, 0, 0, 0, 1},
      };

      CHECK(allTrianglesFaceOutwards(
        buildLightVolumeMesh(linearPointLight(), mirrored, style), vm::vec3d{0, 0, 0}));

      const auto spot = linearSpotLight();
      CHECK(allTrianglesFaceOutwards(
        buildLightVolumeMesh(spot, mirrored, style),
        mirrored.R * mdl::spotPoint(spot, 0, 0, 136)));

      auto sun = mdl::LightProperties{};
      sun.type = mdl::LightType::Sun;
      CHECK(allTrianglesFaceOutwards(
        buildLightVolumeMesh(sun, mirrored, style), vm::vec3d{0, 0, 0}));
    }

    SECTION("a light with light_origin links the entity's origin to its volume")
    {
      auto light = linearPointLight();
      light.hasLightOrigin = true;

      const auto frame = mdl::LightFrame{vm::vec3d{0, 0, 64}, identity};
      const auto linked = buildLightVolumeMesh(light, frame, style, vm::vec3d{16, 0, 0});
      CHECK(hasLine(linked, vm::vec3d{16, 0, 0}, vm::vec3d{0, 0, 64}));

      light.hasLightOrigin = false;
      const auto unlinked =
        buildLightVolumeMesh(light, frame, style, vm::vec3d{16, 0, 0});
      CHECK(!hasLine(unlinked, vm::vec3d{16, 0, 0}, vm::vec3d{0, 0, 64}));
    }

    SECTION("a degenerate spot light draws nothing")
    {
      auto light = linearSpotLight();
      light.right = vm::vec3d{0, 0, 0};

      const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
      CHECK(mesh.triangles.empty());
      CHECK(mesh.lines.empty());
      CHECK(mesh.ranges.empty());
    }

    SECTION("the element ranges tile the triangles, each on its surface (D55)")
    {
      auto outerOnly = style;
      outerOnly.outerShellOnly = true;

      auto seededPoint = mdl::LightProperties{};
      seededPoint.math = mdl::seededMathLightProperties();

      auto sun = mdl::LightProperties{};
      sun.type = mdl::LightType::Sun;
      sun.radius = vm::vec3d{300, 200, 100};

      SECTION("the ranges tile the triangles in order")
      {
        auto innerCapSpot = linearSpotLight();
        innerCapSpot.math.innerRadius = 0.3;
        auto textureSpot = linearSpotLight();
        textureSpot.math.enabled = false;

        for (const auto& light :
             {linearPointLight(),
              seededPoint,
              linearSpotLight(),
              innerCapSpot,
              textureSpot,
              sun})
        {
          for (const auto& rangeStyle : {style, outerOnly})
          {
            const auto mesh = buildLightVolumeMesh(light, atOrigin, rangeStyle);
            auto next = std::size_t{0};
            for (const auto& range : mesh.ranges)
            {
              CAPTURE(range.element);
              CHECK(range.firstTriangleVertex == next);
              CHECK(range.count > 0);
              CHECK(range.count % 3 == 0);
              next = range.firstTriangleVertex + range.count;
            }
            CHECK(next == mesh.triangles.size());
          }
        }
      }

      SECTION("a point light's shells, its outer shell last")
      {
        const auto mesh = buildLightVolumeMesh(linearPointLight(), atOrigin, style);
        REQUIRE(mesh.ranges.size() == 4);
        for (std::size_t j = 0; j < 4; ++j)
        {
          const auto& range = mesh.ranges[j];
          CHECK(
            range.element
            == (j == 3 ? mdl::LightElement::OuterShell : mdl::LightElement::Band));
          const auto t = double(j + 1) / 4.0;
          for (std::size_t k = 0; k < range.count; ++k)
          {
            const auto p = positionOf(mesh.triangles[range.firstTriangleVertex + k]);
            CHECK(vm::length(p) / 300.0 == Catch::Approx(t).margin(1e-6));
          }
        }

        const auto outer = buildLightVolumeMesh(linearPointLight(), atOrigin, outerOnly);
        REQUIRE(outer.ranges.size() == 1);
        CHECK(outer.ranges.front().element == mdl::LightElement::OuterShell);
      }

      SECTION("a math point light's inner shell at t = inner radius")
      {
        // the seeded light, inner radius 0.1, in a turned frame and with three radii
        auto light = seededPoint;
        light.radius = vm::vec3d{300, 200, 100};
        const auto frame =
          mdl::LightFrame{vm::vec3d{64, 32, 16}, vm::mat3x3d{0, -1, 0, 1, 0, 0, 0, 0, 1}};

        const auto mesh = buildLightVolumeMesh(light, frame, style);
        REQUIRE(mesh.ranges.size() == 5);
        const auto& inner = mesh.ranges.back();
        CHECK(inner.element == mdl::LightElement::InnerShell);
        CHECK(inner.count == 224 * 3);

        const auto alphas = lightVolumeShellAlphas(light, style);
        for (std::size_t k = 0; k < inner.count; ++k)
        {
          const auto& vertex = mesh.triangles[inner.firstTriangleVertex + k];
          const auto local = vm::transpose(frame.R) * (positionOf(vertex) - frame.origin);
          CHECK(
            mdl::pointFalloffT(local, light.radius) == Catch::Approx(0.1).margin(1e-6));
          // the alpha of the band that t = 0.1 is in, the innermost of four
          CHECK(alphaOf(vertex) == Catch::Approx(alphas[0]).margin(1e-6));
        }

        // the outer shell is still last of the bands
        CHECK(mesh.ranges[3].element == mdl::LightElement::OuterShell);

        // not drawn for an unselected light, below an inner radius of 0.02, from 0.999
        // on, or without math
        CHECK(buildLightVolumeMesh(light, frame, outerOnly).ranges.size() == 1);
        for (const auto innerRadius : {0.0, 0.01, 0.999})
        {
          auto other = light;
          other.math.innerRadius = innerRadius;
          const auto otherMesh = buildLightVolumeMesh(other, frame, style);
          CHECK(std::ranges::none_of(otherMesh.ranges, [](const auto& range) {
            return range.element == mdl::LightElement::InnerShell;
          }));
        }
        auto disabled = light;
        disabled.math.enabled = false;
        CHECK(std::ranges::none_of(
          buildLightVolumeMesh(disabled, frame, style).ranges, [](const auto& range) {
            return range.element == mdl::LightElement::InnerShell;
          }));

        // the boundaries: from 0.02 to below 0.999
        for (const auto innerRadius : {0.02, 0.98})
        {
          CAPTURE(innerRadius);
          auto other = light;
          other.math.innerRadius = innerRadius;
          CHECK(lightVolumeHasInnerShell(other));
          const auto otherMesh = buildLightVolumeMesh(other, frame, style);
          REQUIRE(!otherMesh.ranges.empty());
          CHECK(otherMesh.ranges.back().element == mdl::LightElement::InnerShell);
        }
        for (const auto innerRadius : {0.0199, 0.999})
        {
          auto other = light;
          other.math.innerRadius = innerRadius;
          CHECK_FALSE(lightVolumeHasInnerShell(other));
        }
        CHECK(lightVolumeHasInnerShell(light));
        CHECK_FALSE(lightVolumeHasInnerShell(disabled));

        // drawn even at alpha 0, where no band is drawn
        auto invisible = style;
        invisible.alphaCap = 0.0;
        const auto invisibleMesh = buildLightVolumeMesh(light, frame, invisible);
        REQUIRE(invisibleMesh.ranges.size() == 1);
        CHECK(invisibleMesh.ranges.front().element == mdl::LightElement::InnerShell);
        CHECK(std::ranges::all_of(invisibleMesh.triangles, [](const auto& vertex) {
          return alphaOf(vertex) == 0.0;
        }));
        CHECK(lightVolumeMeshSize(light, invisible).ranges == 1);
      }

      SECTION("a spot light's funnel, penumbra, caps and base")
      {
        const auto light = linearSpotLight();
        const auto mesh = buildLightVolumeMesh(light, atOrigin, style);

        auto elements = std::vector<mdl::LightElement>{};
        for (const auto& range : mesh.ranges)
        {
          elements.push_back(range.element);

          for (std::size_t k = 0; k < range.count; ++k)
          {
            const auto& vertex = mesh.triangles[range.firstTriangleVertex + k];
            const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
            const auto r = superellipseRadius(u, v, light.math.shapeExp);

            CAPTURE(range.element, u, v, d);
            switch (range.element)
            {
            case mdl::LightElement::Funnel:
              CHECK(r == Catch::Approx(1.0).margin(1e-6));
              break;
            case mdl::LightElement::Penumbra:
              CHECK(r == Catch::Approx(1.0 - light.math.penumbra).margin(1e-6));
              break;
            case mdl::LightElement::NearCap:
              CHECK(d == Catch::Approx(8.0).margin(1e-6));
              CHECK(r <= 1.0 + 1e-6);
              break;
            case mdl::LightElement::Base:
              CHECK(d == Catch::Approx(264.0).margin(1e-6));
              CHECK(r <= 1.0 + 1e-6);
              break;
            case mdl::LightElement::InnerCap:
              CHECK(
                d == Catch::Approx(8.0 + light.math.innerRadius * 256.0).margin(1e-6));
              CHECK(r <= 1.0 + 1e-6);
              break;
            case mdl::LightElement::OuterShell:
            case mdl::LightElement::InnerShell:
            case mdl::LightElement::SunBox:
            case mdl::LightElement::Band:
              FAIL("not an element of a spot light");
              break;
            }
          }
        }

        CHECK(
          elements
          == std::vector{
            mdl::LightElement::Funnel,
            mdl::LightElement::Penumbra,
            mdl::LightElement::NearCap,
            mdl::LightElement::Base,
          });
        // the base at alpha 0 (F(1) = 0)
        CHECK(alphaOf(mesh.triangles[mesh.ranges.back().firstTriangleVertex]) == 0.0);

        // an unselected spot light has its funnel only
        const auto outer = buildLightVolumeMesh(light, atOrigin, outerOnly);
        REQUIRE(outer.ranges.size() == 1);
        CHECK(outer.ranges.front().element == mdl::LightElement::Funnel);
      }

      SECTION("a spot light without math draws the stand-in cone and penumbra (D69)")
      {
        // a texture light whose math keys were kept when it left the math page (D51):
        // they are not read (D63), the stand-in's ellipse and penumbra 0.15 are
        auto light = linearSpotLight();
        light.math.enabled = false;
        light.math.shapeExp = 16.0;
        light.math.penumbra = 0.4;
        light.math.innerRadius = 0.3;

        const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
        auto elements = std::vector<mdl::LightElement>{};
        for (const auto& range : mesh.ranges)
        {
          elements.push_back(range.element);
        }
        CHECK(
          elements
          == std::vector{
            mdl::LightElement::Funnel,
            mdl::LightElement::Penumbra,
            mdl::LightElement::NearCap,
            mdl::LightElement::Base,
          });

        // the funnel's rings are the unit ellipse, the penumbra's at 0.85
        const auto radiiOf = [&](const LightElementRange& range) {
          auto radii = std::vector<double>{};
          for (std::size_t k = 0; k < range.count; ++k)
          {
            const auto [u, v, d] = defaultSpotCoordinates(
              positionOf(mesh.triangles[range.firstTriangleVertex + k]));
            radii.push_back(superellipseRadius(u, v, 2.0));
          }
          return radii;
        };
        CHECK(std::ranges::all_of(radiiOf(mesh.ranges[0]), [](const auto r) {
          return std::abs(r - 1.0) <= 1e-6;
        }));
        CHECK(std::ranges::all_of(radiiOf(mesh.ranges[1]), [](const auto r) {
          return std::abs(r - 0.85) <= 1e-6;
        }));

        // the frustum edges, the two rectangles, the near, far and penumbra outlines, the
        // penumbra's at the far cap
        CHECK(mesh.lines.size() == (4 + 4 + 4 + 3 * 32) * 2);
        const auto penumbraVertices =
          std::ranges::count_if(mesh.lines, [&](const auto& vertex) {
            const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
            return std::abs(superellipseRadius(u, v, 2.0) - 0.85) < 1e-4
                   && std::abs(d - 264.0) < 1e-3;
          });
        CHECK(penumbraVertices == 32 * 2);

        // an unselected one draws its outer cone only, no band
        const auto outer = buildLightVolumeMesh(light, atOrigin, outerOnly);
        REQUIRE(outer.ranges.size() == 1);
        CHECK(outer.ranges.front().element == mdl::LightElement::Funnel);
        CHECK(outer.triangles.size() == 32 * 2 * 3);
      }

      SECTION("a math spot light's inner cap at the end of its core (D69)")
      {
        auto light = linearSpotLight();
        light.math.innerRadius = 0.3;
        REQUIRE(lightVolumeHasInnerCap(light));

        const auto mesh = buildLightVolumeMesh(light, atOrigin, style);
        auto elements = std::vector<mdl::LightElement>{};
        for (const auto& range : mesh.ranges)
        {
          elements.push_back(range.element);
        }
        CHECK(
          elements
          == std::vector{
            mdl::LightElement::Funnel,
            mdl::LightElement::Penumbra,
            mdl::LightElement::NearCap,
            mdl::LightElement::Base,
            mdl::LightElement::InnerCap,
          });

        // 32 triangles at depth zNear + 0.3 zFar, at the cap alpha of t = 0.3: half the
        // alpha cap times the brightness, the falloff being 1 at the inner radius
        const auto depth = 8.0 + 0.3 * 256.0;
        const auto alpha = 0.5 * 0.45 * 1.0;
        REQUIRE(depth == Catch::Approx(84.8));
        REQUIRE(alpha == Catch::Approx(0.225));
        const auto& cap = mesh.ranges.back();
        CHECK(cap.count == 32 * 3);
        for (std::size_t k = 0; k < cap.count; ++k)
        {
          const auto& vertex = mesh.triangles[cap.firstTriangleVertex + k];
          const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
          CHECK(d == Catch::Approx(depth).margin(1e-4));
          CHECK(superellipseRadius(u, v, 2.0) <= 1.0 + 1e-6);
          CHECK(alphaOf(vertex) == Catch::Approx(alpha).margin(1e-6));
        }

        // wound like the far cap, facing the base along the beam, world -Z
        for (auto i = cap.firstTriangleVertex; i < cap.firstTriangleVertex + cap.count;
             i += 3)
        {
          const auto p1 = positionOf(mesh.triangles[i]);
          const auto p2 = positionOf(mesh.triangles[i + 1]);
          const auto p3 = positionOf(mesh.triangles[i + 2]);
          CHECK(vm::dot(vm::cross(p3 - p1, p2 - p1), vm::vec3d{0, 0, -1}) > 0.0);
        }

        // and still facing the base in a mirrored frame, where the beam is R (0, 0, -1)
        const auto mirrored = mdl::LightFrame{
          vm::vec3d{0, 0, 0},
          vm::mat3x3d{-1, 0, 0, 0, 1, 0, 0, 0, 1},
        };
        const auto mirroredMesh = buildLightVolumeMesh(light, mirrored, style);
        REQUIRE(!mirroredMesh.ranges.empty());
        const auto& mirroredCap = mirroredMesh.ranges.back();
        REQUIRE(mirroredCap.element == mdl::LightElement::InnerCap);
        for (auto i = mirroredCap.firstTriangleVertex;
             i < mirroredCap.firstTriangleVertex + mirroredCap.count;
             i += 3)
        {
          const auto p1 = positionOf(mirroredMesh.triangles[i]);
          const auto p2 = positionOf(mirroredMesh.triangles[i + 1]);
          const auto p3 = positionOf(mirroredMesh.triangles[i + 2]);
          CHECK(
            vm::dot(vm::cross(p3 - p1, p2 - p1), mirrored.R * vm::vec3d{0, 0, -1}) > 0.0);
        }

        // its ring outline of 32 segments on the cone at that depth
        CHECK(mesh.lines.size() == (4 + 4 + 4 + 4 * 32) * 2);
        const auto ringVertices =
          std::ranges::count_if(mesh.lines, [&](const auto& vertex) {
            const auto [u, v, d] = defaultSpotCoordinates(positionOf(vertex));
            return std::abs(d - depth) < 1e-3
                   && std::abs(superellipseRadius(u, v, 2.0) - 1.0) < 1e-4;
          });
        CHECK(ringVertices == 32 * 2);

        // drawn even at alpha 0, only with the bands
        auto invisible = style;
        invisible.alphaCap = 0.0;
        const auto invisibleMesh = buildLightVolumeMesh(light, atOrigin, invisible);
        REQUIRE(!invisibleMesh.ranges.empty());
        CHECK(invisibleMesh.ranges.back().element == mdl::LightElement::InnerCap);
        CHECK(
          alphaOf(
            invisibleMesh.triangles[invisibleMesh.ranges.back().firstTriangleVertex])
          == 0.0);
        CHECK(std::ranges::none_of(
          buildLightVolumeMesh(light, atOrigin, outerOnly).ranges, [](const auto& range) {
            return range.element == mdl::LightElement::InnerCap;
          }));

        // none below an inner radius of 0.02 or from 0.999 on (lightVolumeHasInnerCap)
        for (const auto innerRadius : {0.0, 0.01, 0.0199, 0.999})
        {
          CAPTURE(innerRadius);
          auto other = light;
          other.math.innerRadius = innerRadius;
          CHECK(std::ranges::none_of(
            buildLightVolumeMesh(other, atOrigin, style).ranges, [](const auto& range) {
              return range.element == mdl::LightElement::InnerCap;
            }));
        }
      }

      SECTION("a sun's box")
      {
        const auto mesh = buildLightVolumeMesh(sun, atOrigin, style);
        REQUIRE(mesh.ranges.size() == 1);
        CHECK(mesh.ranges.front().element == mdl::LightElement::SunBox);
        CHECK(mesh.ranges.front().count == 12 * 3);
        for (const auto& vertex : mesh.triangles)
        {
          const auto p = positionOf(vertex);
          CHECK(
            vm::get_max_component(vm::abs(p / sun.radius))
            == Catch::Approx(1.0).margin(1e-6));
        }
      }
    }
  }

  SECTION("lightVolumeHasInnerCap")
  {
    // a math spot light with an inner radius from 0.02 to below 0.999 (D69)
    auto light = linearSpotLight();
    for (const auto innerRadius : {0.02, 0.3, 0.98})
    {
      CAPTURE(innerRadius);
      light.math.innerRadius = innerRadius;
      CHECK(lightVolumeHasInnerCap(light));
    }
    for (const auto innerRadius : {0.0, 0.01, 0.0199, 0.999})
    {
      CAPTURE(innerRadius);
      light.math.innerRadius = innerRadius;
      CHECK_FALSE(lightVolumeHasInnerCap(light));
    }

    // not without math, not of a point light or a sun
    light.math.innerRadius = 0.3;
    auto disabled = light;
    disabled.math.enabled = false;
    CHECK_FALSE(lightVolumeHasInnerCap(disabled));
    for (const auto type : {mdl::LightType::Point, mdl::LightType::Sun})
    {
      auto other = light;
      other.type = type;
      CHECK_FALSE(lightVolumeHasInnerCap(other));
    }
  }

  SECTION("lightVolumeMeshSize")
  {
    auto outerOnly = style;
    outerOnly.outerShellOnly = true;
    auto twoD = style;
    twoD.twoD = true;

    auto lights = std::vector<mdl::LightProperties>{};
    lights.push_back(linearPointLight());

    auto sharpPoint = linearPointLight();
    sharpPoint.math.innerRadius = 0.999;
    lights.push_back(sharpPoint);

    auto centredPoint = linearPointLight();
    centredPoint.hasCenter = true;
    lights.push_back(centredPoint);

    auto flatPoint = linearPointLight();
    flatPoint.radius = vm::vec3d{300, 300, 0};
    lights.push_back(flatPoint);

    auto legacyPoint = linearPointLight();
    legacyPoint.math.enabled = false;
    legacyPoint.hasLightOrigin = true;
    lights.push_back(legacyPoint);

    lights.push_back(linearSpotLight());

    auto sharpSpot = linearSpotLight();
    sharpSpot.math.innerRadius = 0.999;
    lights.push_back(sharpSpot);

    auto sharpEdgedSpot = linearSpotLight();
    sharpEdgedSpot.math.penumbra = 0.0;
    lights.push_back(sharpEdgedSpot);

    auto degenerateSpot = linearSpotLight();
    degenerateSpot.right = vm::vec3d{0, 0, 0};
    lights.push_back(degenerateSpot);

    auto sun = mdl::LightProperties{};
    sun.type = mdl::LightType::Sun;
    sun.hasLightOrigin = true;
    lights.push_back(sun);

    // the inner shell of a math light's core (D55)
    auto seededPoint = mdl::LightProperties{};
    seededPoint.math = mdl::seededMathLightProperties();
    lights.push_back(seededPoint);

    // a texture spot light that kept its math keys (D63)
    auto textureSpot = linearSpotLight();
    textureSpot.math.enabled = false;
    textureSpot.math.innerRadius = 0.3;
    lights.push_back(textureSpot);

    // the inner cap of a math spot light's core (D69), also at alpha 0
    auto innerCapSpot = linearSpotLight();
    innerCapSpot.math.innerRadius = 0.3;
    lights.push_back(innerCapSpot);

    auto edgeInnerCapSpot = linearSpotLight();
    edgeInnerCapSpot.math.innerRadius = 0.02;
    edgeInnerCapSpot.color = vm::vec3d{0, 0, 0};
    lights.push_back(edgeInnerCapSpot);

    for (const auto& light : lights)
    {
      for (const auto& sizeStyle : {style, outerOnly, twoD})
      {
        for (const auto& entityOrigin :
             {std::optional<vm::vec3d>{}, std::optional{vm::vec3d{16, 0, 0}}})
        {
          const auto mesh =
            buildLightVolumeMesh(light, atOrigin, sizeStyle, entityOrigin);
          const auto size = lightVolumeMeshSize(light, sizeStyle, entityOrigin);
          CHECK(size.triangles == mesh.triangles.size());
          CHECK(size.lines == mesh.lines.size());
          CHECK(size.ranges == mesh.ranges.size());
        }
      }
    }
  }

  SECTION("appendLightVolumeMesh")
  {
    // the performance gate (D44, I2.13): 500 point lights in "All" mode, sized and
    // reserved once and then appended, as LightVolumeRenderer builds them
    const auto timeBuild = [](const LightVolumeStyle& buildStyle) {
      auto lights = std::vector<std::pair<mdl::LightProperties, mdl::LightFrame>>{};
      for (std::size_t i = 0; i < 500; ++i)
      {
        auto light = linearPointLight();
        light.radius = vm::vec3d{200.0 + double(i % 7), 300, 250};
        lights.emplace_back(
          light, mdl::LightFrame{vm::vec3d{double(i) * 64.0, 0, 0}, identity});
      }

      auto mesh = LightVolumeMesh{};
      const auto start = std::chrono::steady_clock::now();
      auto size = LightVolumeMeshSize{};
      for (const auto& [light, frame] : lights)
      {
        const auto lightSize = lightVolumeMeshSize(light, buildStyle);
        size.triangles += lightSize.triangles;
        size.lines += lightSize.lines;
      }
      mesh.triangles.reserve(size.triangles);
      mesh.lines.reserve(size.lines);
      for (const auto& [light, frame] : lights)
      {
        appendLightVolumeMesh(mesh, light, frame, buildStyle);
      }
      const auto end = std::chrono::steady_clock::now();

      CHECK(mesh.triangles.size() == 500 * 224 * 3 * std::size_t(buildStyle.shells));
      return std::chrono::duration<double, std::milli>(end - start).count();
    };

    auto unselected = style;
    unselected.outerShellOnly = true;
    unselected.shells = 1;

    // the fastest of five runs, so that a scheduling hiccup does not fail the gate
    const auto fastestBuild = [&](const LightVolumeStyle& buildStyle) {
      auto fastest = timeBuild(buildStyle);
      for (auto run = 1; run < 5; ++run)
      {
        fastest = std::min(fastest, timeBuild(buildStyle));
      }
      return fastest;
    };

    const auto selectedMillis = fastestBuild(style);
    const auto unselectedMillis = fastestBuild(unselected);
    WARN(
      "500 point lights, fastest of 5 runs: " << selectedMillis << " ms at K = 4, "
                                              << unselectedMillis
                                              << " ms at K = 1 (outer shell only)");

    // 8 ms is half a frame at 60 fps. The rebuild runs only when a light, the selection
    // or a preference changes (I2.4), never per frame, so this keeps "All" mode
    // interactive with room to spare; Debug builds only report (I2.13)
#ifdef NDEBUG
    CHECK(unselectedMillis <= 8.0);
#endif
  }
}

} // namespace tb::render
