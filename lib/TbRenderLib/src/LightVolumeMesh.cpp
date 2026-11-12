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

#include "render/LightVolumeMesh.h"

#include "base/Macros.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace tb::render
{
namespace
{

using Vertex = gl::VertexTypes::P3C4::Vertex;

// the tessellation (D25): a point light's shells are 16 x 8 lat-long spheres, a spot
// light's cone has 32 segments around
constexpr auto PointSlices = std::size_t{16};
constexpr auto PointStacks = std::size_t{8};
constexpr auto PointUniquePoints = 2 + (PointStacks - 1) * PointSlices;
constexpr auto SpotSegments = std::size_t{32};
// an emitter spot's round caps (the base's and the core's) have this many rings from
// their centre to the cone's edge: a fan and one band of quads (RV-C-TP-03)
constexpr auto EmitterCapRings = std::size_t{2};
// each of the two arcs that outline an emitter spot's round cap has this many segments
constexpr auto EmitterCapArcSegments = SpotSegments / 2;

constexpr auto LineAlpha = 0.9;
constexpr auto CapAlphaScale = 0.5;
constexpr auto TwoDFillScale = 0.6;
constexpr auto MinBrightness = 0.25;
constexpr auto MinHueDivisor = 1e-3;
constexpr auto InnerRadiusSharp = 0.999;
constexpr auto MinPenumbra = 1e-3;
// an emitter's core gets its own shell or cap from this inner radius on (D55, D69)
constexpr auto MinInnerShellRadius = 0.02;
// the stand-in shape of a texture spot (D29, D69)
constexpr auto LegacySpotShapeExp = 2.0;
constexpr auto LegacySpotPenumbra = 0.15;
// like mdl::pointFalloffT
constexpr auto DegenerateRadius = 1e-6;

constexpr auto SunArrowLength = 48.0;
constexpr auto ArrowHeadLength = 8.0;
constexpr auto ArrowHeadWidth = 4.0;
constexpr auto CentreCrossSize = 4.0;

/**
 * A unit sphere tessellated in PointSlices x PointStacks: its unique points and the
 * triangles as indices into them, CW seen from outside.
 */
struct UnitSphere
{
  std::array<vm::vec3d, PointUniquePoints> points;
  std::vector<std::size_t> triangles;
};

const UnitSphere& unitSphere()
{
  static const auto sphere = [] {
    auto result = UnitSphere{};

    const auto topPole = std::size_t{0};
    const auto bottomPole = PointUniquePoints - 1;

    result.points[topPole] = vm::vec3d{0, 0, 1};
    result.points[bottomPole] = vm::vec3d{0, 0, -1};
    for (std::size_t j = 1; j < PointStacks; ++j)
    {
      const auto theta = std::numbers::pi * double(j) / double(PointStacks);
      for (std::size_t i = 0; i < PointSlices; ++i)
      {
        const auto phi = 2.0 * std::numbers::pi * double(i) / double(PointSlices);
        result.points[1 + (j - 1) * PointSlices + i] = vm::vec3d{
          std::sin(theta) * std::cos(phi),
          std::sin(theta) * std::sin(phi),
          std::cos(theta)};
      }
    }

    const auto index = [&](const std::size_t i, const std::size_t j) {
      return j == 0             ? topPole
             : j == PointStacks ? bottomPole
                                : 1 + (j - 1) * PointSlices + (i % PointSlices);
    };

    // with a, b east of each other and d south of a, (a, b, c) and (a, c, d) are CW seen
    // from outside: cross(c - a, b - a) = cross(south, east) points outwards
    for (std::size_t j = 0; j < PointStacks; ++j)
    {
      for (std::size_t i = 0; i < PointSlices; ++i)
      {
        const auto a = index(i, j);
        const auto b = index(i + 1, j);
        const auto c = index(i + 1, j + 1);
        const auto d = index(i, j + 1);
        if (j != 0)
        {
          result.triangles.insert(result.triangles.end(), {a, b, c});
        }
        if (j != PointStacks - 1)
        {
          result.triangles.insert(result.triangles.end(), {a, c, d});
        }
      }
    }

    return result;
  }();
  return sphere;
}

vm::vec4f vertexColor(const vm::vec3d& hue, const double alpha)
{
  return vm::vec4f{float(hue.x()), float(hue.y()), float(hue.z()), float(alpha)};
}

vm::vec3d toWorld(const mdl::LightFrame& frame, const vm::vec3d& local)
{
  return frame.origin + frame.R * local;
}

/**
 * Appends a triangle, or its mirror image if flip is set, so that it stays CW seen from
 * outside under a transformation with a negative determinant.
 */
void addTriangle(
  LightVolumeMesh& mesh,
  const bool flip,
  const vm::vec3d& p1,
  const vm::vec3d& p2,
  const vm::vec3d& p3,
  const vm::vec4f& c1,
  const vm::vec4f& c2,
  const vm::vec4f& c3)
{
  mesh.triangles.emplace_back(vm::vec3f{p1}, c1);
  if (flip)
  {
    mesh.triangles.emplace_back(vm::vec3f{p3}, c3);
    mesh.triangles.emplace_back(vm::vec3f{p2}, c2);
  }
  else
  {
    mesh.triangles.emplace_back(vm::vec3f{p2}, c2);
    mesh.triangles.emplace_back(vm::vec3f{p3}, c3);
  }
}

/**
 * Records the triangles appended since the given first vertex as one element.
 */
void addRange(
  LightVolumeMesh& mesh, const mdl::LightElement element, const std::size_t first)
{
  if (mesh.triangles.size() > first)
  {
    mesh.ranges.push_back({element, first, mesh.triangles.size() - first});
  }
}

void addLine(
  LightVolumeMesh& mesh, const vm::vec3d& p1, const vm::vec3d& p2, const vm::vec4f& color)
{
  mesh.lines.emplace_back(vm::vec3f{p1}, color);
  mesh.lines.emplace_back(vm::vec3f{p2}, color);
}

/**
 * Appends the ellipsoid shell origin + R * (scale * unit sphere) with one colour.
 */
void addEllipsoid(
  LightVolumeMesh& mesh,
  const mdl::LightFrame& frame,
  const bool flip,
  const vm::vec3d& scale,
  const vm::vec4f& color)
{
  const auto& sphere = unitSphere();

  auto points = std::array<vm::vec3f, PointUniquePoints>{};
  for (std::size_t k = 0; k < PointUniquePoints; ++k)
  {
    const auto& u = sphere.points[k];
    points[k] = vm::vec3f{
      toWorld(frame, vm::vec3d{u.x() * scale.x(), u.y() * scale.y(), u.z() * scale.z()})};
  }

  for (std::size_t t = 0; t < sphere.triangles.size(); t += 3)
  {
    mesh.triangles.emplace_back(points[sphere.triangles[t]], color);
    mesh.triangles.emplace_back(points[sphere.triangles[t + (flip ? 2 : 1)]], color);
    mesh.triangles.emplace_back(points[sphere.triangles[t + (flip ? 1 : 2)]], color);
  }
}

/**
 * The box of the given half extents in the given frame: its corners in world space,
 * indexed by the bits x = 1, y = 2, z = 4 for the positive side.
 */
std::array<vm::vec3d, 8> boxCorners(
  const mdl::LightFrame& frame, const vm::vec3d& halfExtents)
{
  auto corners = std::array<vm::vec3d, 8>{};
  for (std::size_t k = 0; k < 8; ++k)
  {
    corners[k] = toWorld(
      frame,
      vm::vec3d{
        (k & 1) ? halfExtents.x() : -halfExtents.x(),
        (k & 2) ? halfExtents.y() : -halfExtents.y(),
        (k & 4) ? halfExtents.z() : -halfExtents.z()});
  }
  return corners;
}

void addBoxFill(
  LightVolumeMesh& mesh,
  const std::array<vm::vec3d, 8>& corners,
  const bool flip,
  const vm::vec4f& color)
{
  // the faces, CW seen from outside in a right-handed frame
  static constexpr auto faces = std::array<std::array<std::size_t, 4>, 6>{{
    {0, 2, 6, 4}, // -x
    {1, 5, 7, 3}, // +x
    {0, 4, 5, 1}, // -y
    {2, 3, 7, 6}, // +y
    {0, 1, 3, 2}, // -z
    {4, 6, 7, 5}, // +z
  }};

  for (const auto& [a, b, c, d] : faces)
  {
    addTriangle(mesh, flip, corners[a], corners[b], corners[c], color, color, color);
    addTriangle(mesh, flip, corners[a], corners[c], corners[d], color, color, color);
  }
}

void addBoxLines(
  LightVolumeMesh& mesh, const std::array<vm::vec3d, 8>& corners, const vm::vec4f& color)
{
  for (std::size_t k = 0; k < 8; ++k)
  {
    for (const auto bit : {std::size_t{1}, std::size_t{2}, std::size_t{4}})
    {
      if ((k & bit) == 0)
      {
        addLine(mesh, corners[k], corners[k | bit], color);
      }
    }
  }
}

/**
 * The points of the unit superellipse |u|^k + |v|^k = 1 at the angles 2 pi i / n.
 */
std::array<vm::vec2d, SpotSegments> superellipse(const double shapeExp)
{
  auto points = std::array<vm::vec2d, SpotSegments>{};
  for (std::size_t i = 0; i < SpotSegments; ++i)
  {
    const auto psi = 2.0 * std::numbers::pi * double(i) / double(SpotSegments);
    const auto c = std::cos(psi);
    const auto s = std::sin(psi);
    const auto r = mdl::superellipseRadius(c, s, shapeExp);
    points[i] = vm::vec2d{c / r, s / r};
  }
  return points;
}

/**
 * The shape exponent of a spot light's cone: an emitter's squareness, or the stand-in's
 * ellipse for a texture spot.
 */
double shapeExpOf(const mdl::LightProperties& light)
{
  return mdl::lightUsesEmitter(light) ? light.emitter.squareness : LegacySpotShapeExp;
}

/**
 * The penumbra of a spot light: an emitter's, or the stand-in's for a texture spot.
 */
double penumbraOf(const mdl::LightProperties& light)
{
  return mdl::lightUsesEmitter(light) ? light.emitter.penumbra : LegacySpotPenumbra;
}

/**
 * The ring of a spot light's cone, the unit superellipse of its shape exponent.
 */
std::array<vm::vec2d, SpotSegments> spotRing(const mdl::LightProperties& light)
{
  return superellipse(shapeExpOf(light));
}

/**
 * The world points of a spot light's cone surface at the given scale of the unit
 * superellipse, ring by ring at the given depths.
 */
std::vector<vm::vec3d> coneRings(
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const std::array<vm::vec2d, SpotSegments>& ring,
  const double scale,
  const std::vector<double>& depths)
{
  auto points = std::vector<vm::vec3d>{};
  points.reserve(depths.size() * SpotSegments);
  for (const auto d : depths)
  {
    for (const auto& uv : ring)
    {
      points.push_back(
        toWorld(frame, mdl::spotPoint(light, scale * uv.x(), scale * uv.y(), d)));
    }
  }
  return points;
}

/**
 * Appends the lateral surface between the given rings with one colour per ring.
 *
 * In a right-handed (u, v, d) space, with the angle increasing counter-clockwise, (a, b,
 * c) of a quad between two rings faces the axis, so it is flipped unless the map to the
 * world reverses orientation.
 */
void addConeSurface(
  LightVolumeMesh& mesh,
  const std::vector<vm::vec3d>& points,
  const std::vector<vm::vec4f>& ringColors,
  const bool flip)
{
  const auto rings = ringColors.size();
  const auto at = [&](const std::size_t i, const std::size_t j) -> const vm::vec3d& {
    return points[j * SpotSegments + (i % SpotSegments)];
  };

  for (std::size_t j = 0; j + 1 < rings; ++j)
  {
    for (std::size_t i = 0; i < SpotSegments; ++i)
    {
      const auto& a = at(i, j);
      const auto& b = at(i + 1, j);
      const auto& c = at(i + 1, j + 1);
      const auto& d = at(i, j + 1);
      const auto& cj = ringColors[j];
      const auto& cj1 = ringColors[j + 1];
      addTriangle(mesh, flip, a, b, c, cj, cj, cj1);
      addTriangle(mesh, flip, a, c, d, cj, cj1, cj1);
    }
  }
}

/**
 * Appends a cap as a fan around its centre. In (u, v, d) space, (centre, p_i, p_{i+1})
 * faces towards the apex.
 */
void addCap(
  LightVolumeMesh& mesh,
  const vm::vec3d& centre,
  const std::span<const vm::vec3d> points,
  const std::size_t ring,
  const vm::vec4f& color,
  const bool flip)
{
  for (std::size_t i = 0; i < SpotSegments; ++i)
  {
    const auto& p = points[ring * SpotSegments + i];
    const auto& q = points[ring * SpotSegments + (i + 1) % SpotSegments];
    addTriangle(mesh, flip, centre, p, q, color, color, color);
  }
}

void addRingOutline(
  LightVolumeMesh& mesh,
  const std::span<const vm::vec3d> points,
  const std::size_t ring,
  const vm::vec4f& color)
{
  for (std::size_t i = 0; i < SpotSegments; ++i)
  {
    addLine(
      mesh,
      points[ring * SpotSegments + i],
      points[ring * SpotSegments + (i + 1) % SpotSegments],
      color);
  }
}

double fillScale(const LightVolumeStyle& style)
{
  return style.twoD ? TwoDFillScale : 1.0;
}

/**
 * Whether an ellipsoid or box of the given half extents has an inside (pointFalloffT is
 * infinite everywhere for a flat one).
 */
bool hasVolume(const vm::vec3d& radius)
{
  return radius.x() >= DegenerateRadius && radius.y() >= DegenerateRadius
         && radius.z() >= DegenerateRadius;
}

bool hasPenumbraSurface(const mdl::LightProperties& light)
{
  const auto penumbra = penumbraOf(light);
  return penumbra > MinPenumbra && penumbra < 1.0;
}

// The functions below take the light's curve, lightVolumeCurve(light), which a mesh
// build makes once and passes down (DP-G1T-05); the public functions of the same rules
// make it themselves.

/**
 * Whether the given curve has a core that is drawn: an emitter's, with an inner radius in
 * [0.02, 0.999).
 */
bool hasCore(const std::optional<mdl::LightCurve>& curve)
{
  return curve && curve->innerRadius >= MinInnerShellRadius
         && curve->innerRadius < InnerRadiusSharp;
}

/**
 * lightVolumeAlphaCap with the light's curve.
 */
double alphaCapOf(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const double alphaCap)
{
  // a sun's energy clamp bounds only its HDR colour, not an unbounded core (I2.12)
  return light.type != mdl::LightType::Sun && curve && !curve->energyClamp ? 1.0
                                                                           : alphaCap;
}

/**
 * lightVolumeBandMeans with the light's curve.
 */
std::vector<double> bandMeans(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style)
{
  const auto shells = std::max(style.shells, 1);
  const auto scale =
    alphaCapOf(light, curve, style.alphaCap) * lightVolumeBrightness(light.color);

  auto means = std::vector<double>{};
  means.reserve(size_t(shells));

  auto inner = lightVolumeFalloff(0.0, curve);
  for (int j = 1; j <= shells; ++j)
  {
    const auto outer = lightVolumeFalloff(double(j) / double(shells), curve);
    means.push_back(scale * (inner + outer) / 2.0);
    inner = outer;
  }
  return means;
}

/**
 * lightVolumeShellAlphas with the light's curve.
 */
std::vector<double> shellAlphas(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style)
{
  const auto means = bandMeans(light, curve, style);
  const auto scale = fillScale(style);

  if (style.outerShellOnly)
  {
    return {std::clamp(means.front(), 0.0, 1.0) * scale};
  }

  auto alphas = std::vector<double>(means.size(), 0.0);
  for (std::size_t j = 0; j < means.size(); ++j)
  {
    const auto next = j + 1 < means.size() ? means[j + 1] : 0.0;
    // behind an opaque outer shell, an inner shell adds nothing
    const auto alpha = next < 1.0 ? (means[j] - next) / (1.0 - next) : 0.0;
    alphas[j] = std::clamp(alpha, 0.0, 1.0) * scale;
  }
  return alphas;
}

/**
 * Whether a spot light draws the cap of its core at t = inner radius: an emitter spot
 * with an inner radius in [0.02, 0.999), drawn with its translucency bands (D69).
 */
bool hasInnerCap(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style)
{
  return !style.outerShellOnly && light.type == mdl::LightType::Spot && hasCore(curve);
}

/**
 * Whether a point light draws the shell of its core at t = inner radius: an emitter omni
 * with an inner radius in [0.02, 0.999), drawn with its translucency bands (D55).
 */
bool hasInnerShell(
  const std::optional<mdl::LightCurve>& curve, const LightVolumeStyle& style)
{
  return !style.outerShellOnly && hasCore(curve);
}

/**
 * The alpha of a point light's inner shell: the alpha of the band that its t is in.
 */
double innerShellAlpha(const std::vector<double>& alphas, const double t)
{
  const auto shells = double(alphas.size());
  const auto band = std::clamp(std::ceil(t * shells) - 1.0, 0.0, shells - 1.0);
  return alphas[std::size_t(band)];
}

int spotBands(const LightVolumeStyle& style)
{
  return style.outerShellOnly ? 1 : std::max(style.shells, 1);
}

/**
 * The factor of a spot light's falloff in its ring alphas: cap b, scaled in 2D.
 */
double spotAlphaScale(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style)
{
  return alphaCapOf(light, curve, style.alphaCap) * lightVolumeBrightness(light.color)
         * fillScale(style);
}

/**
 * The alpha of a spot light's cap at the falloff parameter t, as its vertices store it;
 * a cap whose alpha is 0 is not drawn.
 */
float spotCapAlpha(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style,
  const double t)
{
  const auto ringAlpha =
    float(spotAlphaScale(light, curve, style) * lightVolumeFalloff(t, curve));
  return float(CapAlphaScale * ringAlpha);
}

void appendPointLight(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const vm::vec3d& hue)
{
  const auto radius = vm::abs(light.radius);
  const auto flip = vm::compute_determinant(frame.R) < 0.0;

  // an emitter omni's shells and box are centred on its bulb, origin + R light_center
  const auto bulb =
    mdl::lightUsesEmitter(light) ? mdl::emitterBulb(light) : vm::vec3d{0, 0, 0};
  const auto shellFrame = mdl::LightFrame{toWorld(frame, bulb), frame.R};

  if (hasVolume(radius))
  {
    const auto alphas = shellAlphas(light, curve, style);
    const auto shells = alphas.size();
    for (std::size_t j = 0; j < shells; ++j)
    {
      // a shell that adds nothing to any ray is not drawn
      if (alphas[j] > 0.0)
      {
        const auto first = mesh.triangles.size();
        const auto t = style.outerShellOnly ? 1.0 : double(j + 1) / double(shells);
        addEllipsoid(mesh, shellFrame, flip, radius * t, vertexColor(hue, alphas[j]));
        addRange(
          mesh,
          j + 1 == shells ? mdl::LightElement::OuterShell : mdl::LightElement::Band,
          first);
      }
    }

    if (hasInnerShell(curve, style))
    {
      const auto first = mesh.triangles.size();
      const auto t = curve->innerRadius;
      addEllipsoid(
        mesh, shellFrame, flip, radius * t, vertexColor(hue, innerShellAlpha(alphas, t)));
      addRange(mesh, mdl::LightElement::InnerShell, first);
    }
  }

  const auto lineColor = vertexColor(hue, LineAlpha);
  addBoxLines(mesh, boxCorners(shellFrame, radius), lineColor);

  if (light.hasCenter)
  {
    const auto centre = toWorld(frame, light.center);
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
      const auto arm = CentreCrossSize * frame.R * vm::vec3d::axis(axis);
      addLine(mesh, centre - arm, centre + arm, lineColor);
    }
  }
}

void appendSpotLight(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const vm::vec3d& hue)
{
  const auto frustum = mdl::spotLightFrustum(light);
  if (!frustum)
  {
    return;
  }

  // the cone is wound for a right-handed (u, v, d) space; the map to the world keeps or
  // reverses that orientation with the sign of det [right; up; n] times det R
  const auto frameDet = vm::dot(light.right, vm::cross(light.up, frustum->n));
  const auto flip = (frameDet > 0.0) == (vm::compute_determinant(frame.R) > 0.0);

  const auto bands = spotBands(style);
  auto depths = std::vector<double>{};
  auto ringColors = std::vector<vm::vec4f>{};
  const auto alphaScale = spotAlphaScale(light, curve, style);
  for (int j = 0; j <= bands; ++j)
  {
    const auto t = double(j) / double(bands);
    depths.push_back(frustum->zNear + frustum->zFar * t);
    // a texture spot: the stand-in curve
    ringColors.push_back(
      vertexColor(hue, alphaScale * lightVolumeFalloff(t, std::nullopt)));
  }

  const auto ring = spotRing(light);
  const auto outer = coneRings(light, frame, ring, 1.0, depths);
  const auto lastRing = depths.size() - 1;

  const auto firstFunnel = mesh.triangles.size();
  addConeSurface(mesh, outer, ringColors, flip);
  addRange(mesh, mdl::LightElement::Funnel, firstFunnel);

  const auto hasPenumbra = hasPenumbraSurface(light);
  if (!style.outerShellOnly)
  {
    if (hasPenumbra)
    {
      const auto first = mesh.triangles.size();
      const auto inner = coneRings(light, frame, ring, 1.0 - penumbraOf(light), depths);
      addConeSurface(mesh, inner, ringColors, flip);
      addRange(mesh, mdl::LightElement::Penumbra, first);
    }

    const auto capColor = [&](const std::size_t j, const double t) {
      auto color = ringColors[j];
      color[3] = spotCapAlpha(light, curve, style, t);
      return color;
    };

    // the near cap faces the apex, the far cap away from it; a near cap that adds
    // nothing to any ray is not drawn, the far cap is drawn even at the alpha 0 of every
    // light but a sharp one (F(1) = 0), so that it can be picked and highlighted (D55)
    if (const auto nearColor = capColor(0, 0.0); nearColor[3] > 0.0f)
    {
      const auto first = mesh.triangles.size();
      const auto nearCentre = toWorld(frame, mdl::spotPoint(light, 0, 0, depths.front()));
      addCap(mesh, nearCentre, outer, 0, nearColor, !flip);
      addRange(mesh, mdl::LightElement::NearCap, first);
    }

    const auto first = mesh.triangles.size();
    const auto farCentre = toWorld(frame, mdl::spotPoint(light, 0, 0, depths.back()));
    addCap(mesh, farCentre, outer, lastRing, capColor(lastRing, 1.0), flip);
    addRange(mesh, mdl::LightElement::Base, first);
  }

  const auto lineColor = vertexColor(hue, LineAlpha);
  for (std::size_t i = 0; i < 4; ++i)
  {
    const auto& near0 = frustum->nearCorners[i];
    const auto& near1 = frustum->nearCorners[(i + 1) % 4];
    const auto& far0 = frustum->farCorners[i];
    const auto& far1 = frustum->farCorners[(i + 1) % 4];
    addLine(mesh, toWorld(frame, near0), toWorld(frame, far0), lineColor);
    addLine(mesh, toWorld(frame, near0), toWorld(frame, near1), lineColor);
    addLine(mesh, toWorld(frame, far0), toWorld(frame, far1), lineColor);
  }

  addRingOutline(mesh, outer, 0, lineColor);
  addRingOutline(mesh, outer, lastRing, lineColor);
  if (hasPenumbra)
  {
    const auto penumbra =
      coneRings(light, frame, ring, 1.0 - penumbraOf(light), std::vector{depths.back()});
    addRingOutline(mesh, penumbra, 0, lineColor);
  }
}

void appendSun(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const vm::vec3d& hue)
{
  const auto radius = vm::abs(light.radius);
  const auto flip = vm::compute_determinant(frame.R) < 0.0;
  const auto corners = boxCorners(frame, radius);

  // the falloff of a sun is 1 everywhere
  const auto alpha = alphaCapOf(light, curve, style.alphaCap)
                     * lightVolumeBrightness(light.color) * fillScale(style);
  const auto first = mesh.triangles.size();
  addBoxFill(mesh, corners, flip, vertexColor(hue, alpha));
  addRange(mesh, mdl::LightElement::SunBox, first);

  const auto lineColor = vertexColor(hue, LineAlpha);
  addBoxLines(mesh, corners, lineColor);

  // light_center of a sun is its direction to the light in world space
  const auto dirToLight = light.hasCenter && vm::length(light.center) > 0.0
                            ? vm::normalize(light.center)
                            : vm::vec3d{0, 0, 1};
  const auto direction = -dirToLight;
  const auto tip = frame.origin + SunArrowLength * direction;
  addLine(mesh, frame.origin, tip, lineColor);

  // any direction orthogonal to the arrow: the cross product with the axis of its
  // smallest component
  const auto side = vm::normalize(
    vm::cross(direction, vm::vec3d::axis(vm::find_abs_max_component(direction, 2))));
  const auto otherSide = vm::cross(direction, side);
  const auto headBase = tip - ArrowHeadLength * direction;
  for (const auto& offset : {side, -side, otherSide, -otherSide})
  {
    addLine(mesh, tip, headBase + ArrowHeadWidth * offset, lineColor);
  }
}

/**
 * The local direction from an emitter spot's bulb through the point (u, v) of the unit
 * superellipse scaled by the given factor: forward along x, cone h along y, cone v
 * along z.
 */
vm::vec3d emitterSpotDirection(
  const double tanHalfH, const double tanHalfV, const vm::vec2d& uv, const double scale)
{
  return vm::normalize(
    vm::vec3d{1.0, scale * uv.x() * tanHalfH, scale * uv.y() * tanHalfV});
}

/**
 * The tangents of an emitter spot's two half angles, computed once per light.
 */
struct EmitterCone
{
  double tanHalfH;
  double tanHalfV;
};

EmitterCone emitterCone(const mdl::LightProperties& light)
{
  return {
    mdl::emitterTanHalf(light.emitter.coneH), mdl::emitterTanHalf(light.emitter.coneV)};
}

using EmitterRing = std::array<vm::vec3d, SpotSegments>;

/**
 * The world points of one ring of an emitter spot's cone, scaled by the given factor, at
 * the given distance from its bulb, on the sphere of that distance.
 */
EmitterRing emitterConeRing(
  const EmitterCone& cone,
  const mdl::LightFrame& frame,
  const std::array<vm::vec2d, SpotSegments>& ring,
  const double scale,
  const double distance)
{
  auto points = EmitterRing{};
  for (std::size_t i = 0; i < SpotSegments; ++i)
  {
    points[i] = toWorld(
      frame,
      distance * emitterSpotDirection(cone.tanHalfH, cone.tanHalfV, ring[i], scale));
  }
  return points;
}

/**
 * The world points of an emitter spot's cone, scaled by the given factor, ring by ring at
 * the given distances from its bulb: each ring lies on the sphere of its distance.
 */
std::vector<vm::vec3d> emitterConeRings(
  const EmitterCone& cone,
  const mdl::LightFrame& frame,
  const std::array<vm::vec2d, SpotSegments>& ring,
  const double scale,
  const std::vector<double>& distances)
{
  auto points = std::vector<vm::vec3d>{};
  points.reserve(distances.size() * SpotSegments);
  for (const auto distance : distances)
  {
    for (const auto& uv : ring)
    {
      points.push_back(toWorld(
        frame, distance * emitterSpotDirection(cone.tanHalfH, cone.tanHalfV, uv, scale)));
    }
  }
  return points;
}

/**
 * Appends the lateral surface of an emitter spot's cone between the given rings, wound
 * like addConeSurface. Its first ring is the bulb, where the first triangle of each quad
 * has no area, so the first band is a fan.
 */
void addEmitterConeSurface(
  LightVolumeMesh& mesh,
  const std::vector<vm::vec3d>& points,
  const std::vector<vm::vec4f>& ringColors,
  const bool flip)
{
  const auto rings = ringColors.size();
  const auto at = [&](const std::size_t i, const std::size_t j) -> const vm::vec3d& {
    return points[j * SpotSegments + (i % SpotSegments)];
  };

  for (std::size_t j = 0; j + 1 < rings; ++j)
  {
    for (std::size_t i = 0; i < SpotSegments; ++i)
    {
      const auto& a = at(i, j);
      const auto& b = at(i + 1, j);
      const auto& c = at(i + 1, j + 1);
      const auto& d = at(i, j + 1);
      const auto& cj = ringColors[j];
      const auto& cj1 = ringColors[j + 1];
      if (j != 0)
      {
        addTriangle(mesh, flip, a, b, c, cj, cj, cj1);
      }
      addTriangle(mesh, flip, a, c, d, cj, cj1, cj1);
    }
  }
}

/**
 * Appends the round cap of an emitter spot at the given distance from its bulb, the part
 * of that sphere inside the cone: a fan around the axis and EmitterCapRings - 1 bands of
 * quads out to the cone's edge (one band at two rings), all wound like a fan of addCap.
 * Returns its outermost ring, the cone's edge on that sphere.
 */
EmitterRing addEmitterCap(
  LightVolumeMesh& mesh,
  const EmitterCone& cone,
  const mdl::LightFrame& frame,
  const std::array<vm::vec2d, SpotSegments>& ring,
  const double distance,
  const vm::vec4f& color,
  const bool flip)
{
  // ring k at (k + 1) / EmitterCapRings of the cone's edge, on the sphere
  auto rings = std::array<EmitterRing, EmitterCapRings>{};
  for (std::size_t k = 0; k < EmitterCapRings; ++k)
  {
    rings[k] = emitterConeRing(
      cone, frame, ring, double(k + 1) / double(EmitterCapRings), distance);
  }

  addCap(mesh, toWorld(frame, vm::vec3d{distance, 0, 0}), rings.front(), 0, color, flip);
  for (std::size_t k = 0; k + 1 < EmitterCapRings; ++k)
  {
    const auto& inner = rings[k];
    const auto& outer = rings[k + 1];
    for (std::size_t i = 0; i < SpotSegments; ++i)
    {
      const auto j = (i + 1) % SpotSegments;
      addTriangle(mesh, flip, inner[i], outer[i], outer[j], color, color, color);
      addTriangle(mesh, flip, inner[i], outer[j], inner[j], color, color, color);
    }
  }
  return rings.back();
}

/**
 * Whether an emitter spot's round cap is outlined by its two arcs: where it is not filled
 * (an unselected light) and in a 2D view, where its dome is seen from the side.
 */
bool hasEmitterCapArcs(const LightVolumeStyle& style)
{
  return style.outerShellOnly || style.twoD;
}

/**
 * Appends the two arcs of an emitter spot's round cap through its axis, in the planes of
 * cone h (forward and axis[1]) and cone v (forward and axis[2]), from rim to rim on the
 * sphere of the given range, so that the drawn light reaches its range on the axis.
 */
void addEmitterCapArcs(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const double range,
  const vm::vec4f& color)
{
  for (const auto& [axis, cone] :
       {std::pair{std::size_t{1}, light.emitter.coneH},
        std::pair{std::size_t{2}, light.emitter.coneV}})
  {
    const auto halfAngle = std::atan(mdl::emitterTanHalf(cone));
    const auto at = [&](const std::size_t i) {
      const auto angle =
        halfAngle * (2.0 * double(i) / double(EmitterCapArcSegments) - 1.0);
      auto local = vm::vec3d{range * std::cos(angle), 0, 0};
      local[axis] = range * std::sin(angle);
      return toWorld(frame, local);
    };
    // each point of the arc once (RV2-TP-03)
    auto previous = at(0);
    for (std::size_t i = 1; i <= EmitterCapArcSegments; ++i)
    {
      const auto next = at(i);
      addLine(mesh, previous, next, color);
      previous = next;
    }
  }
}

void appendEmitterSpot(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const vm::vec3d& hue)
{
  const auto range = mdl::emitterRange(light.radius);
  if (range < DegenerateRadius)
  {
    return;
  }

  // the light's own axes (forward, cone h, cone v) keep the orientation of the (u, v, d)
  // space that the cone is wound for; only a mirrored rotation reverses it
  const auto flip = vm::compute_determinant(frame.R) > 0.0;

  // the rings at t = j / bands on the spheres about the bulb, the first the bulb itself
  const auto bands = spotBands(style);
  auto distances = std::vector<double>{};
  auto ringColors = std::vector<vm::vec4f>{};
  // one allocation each (RV2-TP-06)
  distances.reserve(std::size_t(bands) + 1);
  ringColors.reserve(std::size_t(bands) + 1);
  const auto alphaScale = spotAlphaScale(light, curve, style);
  for (int j = 0; j <= bands; ++j)
  {
    const auto t = double(j) / double(bands);
    distances.push_back(range * t);
    ringColors.push_back(vertexColor(hue, alphaScale * lightVolumeFalloff(t, curve)));
  }

  // the two tangents once for every ring of the light
  const auto cone = emitterCone(light);
  const auto ring = spotRing(light);
  const auto outer = emitterConeRings(cone, frame, ring, 1.0, distances);
  const auto lastRing = distances.size() - 1;

  const auto firstFunnel = mesh.triangles.size();
  addEmitterConeSurface(mesh, outer, ringColors, flip);
  addRange(mesh, mdl::LightElement::Funnel, firstFunnel);

  const auto hasPenumbra = hasPenumbraSurface(light);
  if (!style.outerShellOnly)
  {
    if (hasPenumbra)
    {
      const auto first = mesh.triangles.size();
      const auto inner =
        emitterConeRings(cone, frame, ring, 1.0 - penumbraOf(light), distances);
      addEmitterConeSurface(mesh, inner, ringColors, flip);
      addRange(mesh, mdl::LightElement::Penumbra, first);
    }

    // the round cap is drawn even at alpha 0, so that it can be picked and highlighted
    auto capColor = ringColors[lastRing];
    capColor[3] = spotCapAlpha(light, curve, style, 1.0);
    const auto first = mesh.triangles.size();
    addEmitterCap(mesh, cone, frame, ring, range, capColor, flip);
    addRange(mesh, mdl::LightElement::Base, first);
  }

  // the end of the core at the start, wound like the base so that it faces away from
  // the bulb, drawn even at alpha 0 so that it can be picked and highlighted; its
  // outermost ring is the ring of its outline
  auto innerCapRing = std::optional<EmitterRing>{};
  if (hasInnerCap(light, curve, style))
  {
    const auto t = curve->innerRadius;
    const auto first = mesh.triangles.size();
    innerCapRing = addEmitterCap(
      mesh,
      cone,
      frame,
      ring,
      range * t,
      vertexColor(hue, spotCapAlpha(light, curve, style, t)),
      flip);
    addRange(mesh, mdl::LightElement::InnerCap, first);
  }

  const auto lineColor = vertexColor(hue, LineAlpha);
  // four edges from the bulb to the rim, a quarter turn apart
  for (std::size_t i = 0; i < SpotSegments; i += SpotSegments / 4)
  {
    addLine(mesh, frame.origin, outer[lastRing * SpotSegments + i], lineColor);
  }
  addRingOutline(mesh, outer, lastRing, lineColor);
  if (hasPenumbra)
  {
    const auto penumbra =
      emitterConeRing(cone, frame, ring, 1.0 - penumbraOf(light), range);
    addRingOutline(mesh, penumbra, 0, lineColor);
  }
  if (innerCapRing)
  {
    addRingOutline(mesh, *innerCapRing, 0, lineColor);
  }
  if (hasEmitterCapArcs(style))
  {
    addEmitterCapArcs(mesh, light, frame, range, lineColor);
  }
}

/**
 * lightVolumeMeshSize with the light's curve.
 */
LightVolumeMeshSize meshSize(
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin)
{
  // two vertices per line
  constexpr auto BoxLineVertices = std::size_t{12 * 2};
  constexpr auto CrossLineVertices = std::size_t{3 * 2};
  constexpr auto ArrowLineVertices = std::size_t{5 * 2};
  constexpr auto FrustumLineVertices = std::size_t{3 * 4 * 2};
  constexpr auto OutlineVertices = SpotSegments * 2;
  constexpr auto CapVertices = SpotSegments * 3;

  auto size = LightVolumeMeshSize{};
  switch (light.type)
  {
  case mdl::LightType::Point:
    if (hasVolume(vm::abs(light.radius)))
    {
      const auto alphas = shellAlphas(light, curve, style);
      size.ranges = std::size_t(
        std::ranges::count_if(alphas, [](const auto alpha) { return alpha > 0.0; }));
      size.ranges += hasInnerShell(curve, style) ? 1 : 0;
      size.triangles = size.ranges * unitSphere().triangles.size();
    }
    size.lines = BoxLineVertices + (light.hasCenter ? CrossLineVertices : 0);
    break;
  case mdl::LightType::Spot:
    if (mdl::lightUsesEmitter(light))
    {
      if (mdl::emitterRange(light.radius) >= DegenerateRadius)
      {
        // the cone's first band is a fan from the bulb; a round cap is a fan and quads
        constexpr auto EmitterCapVertices =
          (SpotSegments + (EmitterCapRings - 1) * SpotSegments * 2) * 3;
        const auto surfaceVertices =
          SpotSegments * (std::size_t(spotBands(style)) * 2 - 1) * 3;
        const auto penumbra = hasPenumbraSurface(light);
        const auto innerCap = hasInnerCap(light, curve, style);
        size.triangles = surfaceVertices;
        size.ranges = 1;
        if (!style.outerShellOnly)
        {
          size.triangles += (penumbra ? surfaceVertices : 0) + EmitterCapVertices;
          size.ranges += (penumbra ? 1 : 0) + 1;
        }
        if (innerCap)
        {
          size.triangles += EmitterCapVertices;
          size.ranges += 1;
        }
        // two vertices per arc segment, two arcs
        constexpr auto CapArcVertices = EmitterCapArcSegments * 2 * 2;
        size.lines = 4 * 2 + OutlineVertices + (penumbra ? OutlineVertices : 0)
                     + (innerCap ? OutlineVertices : 0)
                     + (hasEmitterCapArcs(style) ? CapArcVertices : 0);
      }
      break;
    }
    if (mdl::spotLightFrustum(light))
    {
      const auto surfaceVertices = SpotSegments * std::size_t(spotBands(style)) * 6;
      size.triangles = surfaceVertices;
      size.ranges = 1;
      if (!style.outerShellOnly)
      {
        const auto penumbra = hasPenumbraSurface(light);
        const auto nearCap = spotCapAlpha(light, curve, style, 0.0) > 0.0f;
        size.triangles +=
          (penumbra ? surfaceVertices : 0) + (nearCap ? CapVertices : 0) + CapVertices;
        size.ranges += (penumbra ? 1 : 0) + (nearCap ? 1 : 0) + 1;
      }
      size.lines = FrustumLineVertices + 2 * OutlineVertices
                   + (hasPenumbraSurface(light) ? OutlineVertices : 0);
    }
    break;
  case mdl::LightType::Sun:
    size.triangles = 6 * 2 * 3;
    size.lines = BoxLineVertices + ArrowLineVertices;
    size.ranges = 1;
    break;
    switchDefault();
  }

  if (light.hasLightOrigin && entityOrigin)
  {
    size.lines += 2;
  }
  return size;
}

/**
 * appendLightVolumeMesh with the light's curve.
 */
void appendMesh(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const std::optional<mdl::LightCurve>& curve,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin)
{
  const auto hue = lightVolumeHue(light.color);

  switch (light.type)
  {
  case mdl::LightType::Point:
    appendPointLight(mesh, light, curve, frame, style, hue);
    break;
  case mdl::LightType::Spot:
    if (mdl::lightUsesEmitter(light))
    {
      appendEmitterSpot(mesh, light, curve, frame, style, hue);
    }
    else
    {
      appendSpotLight(mesh, light, curve, frame, style, hue);
    }
    break;
  case mdl::LightType::Sun:
    appendSun(mesh, light, curve, frame, style, hue);
    break;
    switchDefault();
  }

  if (light.hasLightOrigin && entityOrigin)
  {
    addLine(mesh, *entityOrigin, frame.origin, vertexColor(hue, LineAlpha));
  }
}

} // namespace

bool lightVolumeHasInnerShell(const mdl::LightProperties& light)
{
  return hasCore(lightVolumeCurve(light));
}

bool lightVolumeHasInnerCap(const mdl::LightProperties& light)
{
  return light.type == mdl::LightType::Spot && lightVolumeHasInnerShell(light);
}

double lightVolumeFalloff(const double t, const std::optional<mdl::LightCurve>& curve)
{
  if (!curve)
  {
    return std::clamp(mdl::legacyFalloff(t), 0.0, 1.0);
  }
  if (curve->innerRadius >= InnerRadiusSharp)
  {
    return 1.0;
  }
  return std::clamp(mdl::lightFalloff(t, *curve), 0.0, 1.0);
}

std::optional<mdl::LightCurve> lightVolumeCurve(const mdl::LightProperties& light)
{
  return mdl::lightUsesEmitter(light) ? std::optional{mdl::emitterCurve(light)}
                                      : std::nullopt;
}

double lightVolumeAlphaCap(const mdl::LightProperties& light, const double alphaCap)
{
  return alphaCapOf(light, lightVolumeCurve(light), alphaCap);
}

vm::vec3d lightVolumeHue(const vm::vec3d& color)
{
  const auto divisor = std::max(vm::get_max_component(color), MinHueDivisor);
  return vm::clamp(color / divisor, vm::vec3d{0, 0, 0}, vm::vec3d{1, 1, 1});
}

double lightVolumeBrightness(const vm::vec3d& color)
{
  return std::clamp(vm::get_max_component(color), MinBrightness, 1.0);
}

std::vector<double> lightVolumeBandMeans(
  const mdl::LightProperties& light, const LightVolumeStyle& style)
{
  return bandMeans(light, lightVolumeCurve(light), style);
}

std::vector<double> lightVolumeShellAlphas(
  const mdl::LightProperties& light, const LightVolumeStyle& style)
{
  return shellAlphas(light, lightVolumeCurve(light), style);
}

LightVolumeMeshSize lightVolumeMeshSize(
  const mdl::LightProperties& light,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin)
{
  // a sun's size needs no curve
  const auto curve =
    light.type != mdl::LightType::Sun ? lightVolumeCurve(light) : std::nullopt;
  return meshSize(light, curve, style, entityOrigin);
}

void appendLightVolumeMesh(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin)
{
  appendMesh(mesh, light, lightVolumeCurve(light), frame, style, entityOrigin);
}

LightVolumeMesh buildLightVolumeMesh(
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin)
{
  // the curve once for the size and the mesh
  const auto curve = lightVolumeCurve(light);
  const auto size = meshSize(light, curve, style, entityOrigin);

  auto mesh = LightVolumeMesh{};
  mesh.triangles.reserve(size.triangles);
  mesh.lines.reserve(size.lines);
  mesh.ranges.reserve(size.ranges);
  appendMesh(mesh, light, curve, frame, style, entityOrigin);
  return mesh;
}

std::vector<vm::vec3d> emitterSpotRing(
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const double distance,
  const double scale)
{
  if (light.type != mdl::LightType::Spot || !mdl::lightUsesEmitter(light))
  {
    return {};
  }
  const auto points =
    emitterConeRing(emitterCone(light), frame, spotRing(light), scale, distance);
  return {points.begin(), points.end()};
}

} // namespace tb::render
