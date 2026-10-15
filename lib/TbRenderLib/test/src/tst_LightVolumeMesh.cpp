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

  SECTION("lightVolumeHue and lightVolumeBrightness")
  {
    CHECK(lightVolumeHue(vm::vec3d{2, 1, 0}) == vm::vec3d{1, 0.5, 0});
    CHECK(lightVolumeHue(vm::vec3d{0.5, 0.25, 0}) == vm::vec3d{1, 0.5, 0});
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

      // the far cap's alpha is 0 (F(1) = 0), so it is not drawn
      CHECK(nearCapTriangles == 32);
      CHECK(farCapTriangles == 0);
      CHECK(mesh.triangles.size() == (2 * 32 * 4 * 2 + 32) * 3);

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
    }
  }

  SECTION("appendLightVolumeMesh")
  {
    // the performance gate (D44): 500 point lights in "All" mode
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

    const auto selectedMillis = timeBuild(style);
    const auto unselectedMillis = timeBuild(unselected);
    WARN(
      "500 point lights: " << selectedMillis << " ms at K = 4, " << unselectedMillis
                           << " ms at K = 1 (outer shell only)");

#ifdef NDEBUG
    CHECK(unselectedMillis <= 2.0);
#endif
  }
}

} // namespace tb::render
