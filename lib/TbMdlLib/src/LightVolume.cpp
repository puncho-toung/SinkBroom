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

#include "mdl/LightVolume.h"

#include "base/Macros.h"
#include "mdl/Entity.h"
#include "mdl/EntityRotation.h"

#include "kd/contracts.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tb::mdl
{
namespace
{

// every guard of the light geometry, checked before anything is normalized
constexpr auto DegenerateEpsilon = 1e-6;

// R_ComputeSpotLightProjectionMatrix
constexpr auto SpotLightMinZNear = 8.0;
constexpr auto SpotLightMinZFar = 16.0;

// the tool denies a spot box shallower than this, and a half extent below 1
constexpr auto MinSpotBoxDepth = 24.0;
constexpr auto MinHalfExtent = 1.0;

// the tolerance of a centred spot light
constexpr auto CentredRelativeEpsilon = 1e-4;
constexpr auto CentredAbsoluteEpsilon = 0.01;

/**
 * HLSL's saturate, which unlike std::clamp maps NaN to 0.
 */
double saturate(const double x)
{
  return std::isnan(x) ? 0.0 : std::clamp(x, 0.0, 1.0);
}

/**
 * HLSL's smoothstep for edge0 < edge1.
 */
double smoothstep(const double edge0, const double edge1, const double x)
{
  const auto t = saturate((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

/**
 * The spot light quantities that every function derives, after the guards.
 */
struct Spot
{
  vm::vec3d n;
  double targetLength;
  double det;
  double zNear;
  double zFar;
};

std::optional<Spot> spot(const LightProperties& light)
{
  const auto targetLength = vm::length(light.target);
  if (
    targetLength < DegenerateEpsilon || vm::length(light.right) < DegenerateEpsilon
    || vm::length(light.up) < DegenerateEpsilon)
  {
    return std::nullopt;
  }

  const auto n = light.target / targetLength;

  // the determinant of the rows right, up, n
  const auto det = vm::dot(light.right, vm::cross(light.up, n));
  if (std::abs(det) < DegenerateEpsilon)
  {
    return std::nullopt;
  }

  // without light_start, the engine's start is 0 0 0; without light_end, its end is the
  // target
  const auto start = light.explicitStartEnd ? light.start : vm::vec3d{0, 0, 0};
  const auto end = light.explicitStartEnd ? light.end : light.target;

  return Spot{
    n,
    targetLength,
    det,
    std::max(vm::dot(start, n), SpotLightMinZNear),
    std::max(vm::dot(end, n), SpotLightMinZFar),
  };
}

vm::vec3d spotPoint(
  const LightProperties& light,
  const Spot& s,
  const double u,
  const double v,
  const double d)
{
  // solve [right; up; n] p = b by Cramer's rule: the inverse's columns are the cross
  // products of the rows divided by the determinant
  const auto b0 =
    d * (vm::dot(light.right, light.target) + u * vm::squared_length(light.right))
    / s.targetLength;
  const auto b1 = d * (vm::dot(light.up, light.target) + v * vm::squared_length(light.up))
                  / s.targetLength;
  const auto b2 = d;

  return (b0 * vm::cross(light.up, s.n) + b1 * vm::cross(s.n, light.right)
          + b2 * vm::cross(light.right, light.up))
         / s.det;
}

constexpr auto CornerUVs = std::array<std::array<double, 2>, 4>{{
  {-1.0, -1.0},
  {1.0, -1.0},
  {1.0, 1.0},
  {-1.0, 1.0},
}};

vm::mat3x3d rotationPart(const vm::mat4x4d& m)
{
  auto result = vm::mat3x3d{};
  for (std::size_t c = 0; c < 3; ++c)
  {
    for (std::size_t r = 0; r < 3; ++r)
    {
      result[c][r] = m[c][r];
    }
  }
  return result;
}

/**
 * The index of the component with the smallest absolute value, the first of equal ones.
 */
std::size_t absMinComponent(const vm::vec3d& v)
{
  auto result = std::size_t{0};
  for (std::size_t i = 1; i < 3; ++i)
  {
    if (std::abs(v[i]) < std::abs(v[result]))
    {
      result = i;
    }
  }
  return result;
}

bool hasDegenerateRadius(const vm::vec3d& radius)
{
  return radius.x() < DegenerateEpsilon || radius.y() < DegenerateEpsilon
         || radius.z() < DegenerateEpsilon;
}

bool equalComponents(const vm::vec3d& v)
{
  return std::abs(v.x() - v.y()) <= DegenerateEpsilon
         && std::abs(v.x() - v.z()) <= DegenerateEpsilon;
}

/**
 * The zNear of the given light if it is an explicit spot light, else the engine's
 * minimum.
 */
double explicitZNear(const LightProperties& light)
{
  if (light.type == LightType::Spot && light.explicitStartEnd)
  {
    if (const auto s = spot(light))
    {
      return s->zNear;
    }
  }
  return SpotLightMinZNear;
}

LightProperties pointFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box)
{
  const auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinHalfExtent || halfExtents.y() < MinHalfExtent
    || halfExtents.z() < MinHalfExtent)
  {
    return base;
  }

  auto light = base;
  light.origin = base.origin + rotation * box.center();
  light.radius = halfExtents;
  light.equalRadius = equalComponents(halfExtents);
  return light;
}

LightProperties spotFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box)
{
  // step 1
  const auto cx = (box.min.x() + box.max.x()) / 2.0;
  const auto cy = (box.min.y() + box.max.y()) / 2.0;
  const auto hx = (box.max.x() - box.min.x()) / 2.0;
  const auto hy = (box.max.y() - box.min.y()) / 2.0;
  const auto depth = box.max.z() - box.min.z();

  // step 2, every deny before anything is normalized
  if (depth < MinSpotBoxDepth || hx < MinHalfExtent || hy < MinHalfExtent)
  {
    return base;
  }

  const auto s = spot(base);
  const auto frame = beamFrame(base);
  if (!s || !frame)
  {
    return base;
  }

  const auto centre = vm::vec3d{cx, cy, depth};
  const auto distance = vm::length(centre);

  auto light = base;

  // step 3: the apex moves only along the beam
  light.origin = base.origin + box.min.z() * (rotation * frame->n);

  // step 4
  const auto zNear =
    base.explicitStartEnd ? std::min(s->zNear, distance - 16.0) : SpotLightMinZNear;
  const auto targetLength = distance - zNear;
  const auto n = (cx * frame->r + cy * frame->u + depth * frame->n) / distance;
  light.target = targetLength * n;

  // step 5: never degenerate, |r * n| = |cx| / distance < 1
  const auto r = vm::normalize(frame->r - vm::dot(frame->r, n) * n);
  const auto u = vm::cross(n, r);

  // step 6: the far cap is at distance zNear + |target| = distance
  const auto scale = targetLength / distance;
  light.right = hx * scale * r;
  light.up = frame->upSign * hy * scale * u;

  // step 7
  if (base.explicitStartEnd)
  {
    light.start = zNear * n;
    light.end = light.target;
  }
  else
  {
    light.start = vm::vec3d{0, 0, 0};
    light.end = light.target;
  }

  return light;
}

} // namespace

LightFrame lightFrame(const Entity& entity, const EntityRotationPolicy policy)
{
  return {
    parseLightProperties(entity).origin,
    rotationPart(entityRotation(entity, policy)),
  };
}

vm::bbox3d pointLightLocalBox(const LightProperties& light)
{
  const auto radius = vm::abs(light.radius);
  return vm::bbox3d{-radius, radius};
}

std::optional<SpotFrustum> spotLightFrustum(const LightProperties& light)
{
  const auto s = spot(light);
  if (!s)
  {
    return std::nullopt;
  }

  auto frustum = SpotFrustum{{}, {}, s->n, s->zNear, s->zFar};
  for (std::size_t i = 0; i < CornerUVs.size(); ++i)
  {
    const auto [u, v] = CornerUVs[i];
    frustum.nearCorners[i] = spotPoint(light, *s, u, v, s->zNear);
    frustum.farCorners[i] = spotPoint(light, *s, u, v, s->zNear + s->zFar);

    if (
      vm::dot(frustum.nearCorners[i], s->n) <= DegenerateEpsilon
      || vm::dot(frustum.farCorners[i], s->n) <= DegenerateEpsilon)
    {
      return std::nullopt;
    }
  }

  return frustum;
}

std::optional<BeamFrame> beamFrame(const LightProperties& light)
{
  const auto s = spot(light);
  if (!s)
  {
    return std::nullopt;
  }

  const auto& n = s->n;
  const auto rightPerpendicular = light.right - vm::dot(light.right, n) * n;
  const auto rightPerpendicularLength = vm::length(rightPerpendicular);

  auto r = vm::vec3d{};
  if (rightPerpendicularLength < DegenerateEpsilon)
  {
    // right is parallel to the beam: any direction orthogonal to it will do
    const auto k = absMinComponent(n);
    r = vm::normalize(vm::cross(n, vm::vec3d::axis(k)));
  }
  else
  {
    r = rightPerpendicular / rightPerpendicularLength;
  }

  // never flipped to match up: [r u n] must be a proper rotation
  const auto u = vm::cross(n, r);
  const auto upSign = vm::dot(u, light.up) < 0.0 ? -1.0 : 1.0;

  return BeamFrame{r, u, n, upSign};
}

bool isCentredSpot(const LightProperties& light)
{
  const auto targetLength = vm::length(light.target);
  if (targetLength < DegenerateEpsilon)
  {
    return false;
  }

  const auto n = light.target / targetLength;
  const auto rightLength = vm::length(light.right);
  const auto upLength = vm::length(light.up);

  return std::abs(vm::dot(light.right, n))
           <= CentredRelativeEpsilon * rightLength + CentredAbsoluteEpsilon
         && std::abs(vm::dot(light.up, n))
              <= CentredRelativeEpsilon * upLength + CentredAbsoluteEpsilon
         && std::abs(vm::dot(light.right, light.up))
              <= CentredRelativeEpsilon * rightLength * upLength + CentredAbsoluteEpsilon;
}

std::optional<vm::bbox3d> lightEditBox(const LightProperties& light)
{
  switch (light.type)
  {
  case LightType::Point:
  case LightType::Sun:
    if (hasDegenerateRadius(light.radius))
    {
      return std::nullopt;
    }
    return vm::bbox3d{-light.radius, light.radius};
  case LightType::Spot: {
    if (!spotLightFrustum(light))
    {
      return std::nullopt;
    }

    const auto s = *spot(light);
    const auto farDistance = s.zNear + s.zFar;
    const auto hx = vm::length(light.right) * farDistance / s.targetLength;
    const auto hy = vm::length(light.up) * farDistance / s.targetLength;
    return vm::bbox3d{vm::vec3d{-hx, -hy, 0.0}, vm::vec3d{hx, hy, farDistance}};
  }
    switchDefault();
  }
}

LightProperties lightFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box)
{
  switch (base.type)
  {
  case LightType::Point:
  case LightType::Sun:
    return pointFromEditBox(base, rotation, box);
  case LightType::Spot:
    return spotFromEditBox(base, rotation, box);
    switchDefault();
  }
}

vm::vec3d spotPoint(
  const LightProperties& light, const double u, const double v, const double d)
{
  const auto s = spot(light);
  return s ? spotPoint(light, *s, u, v, d) : vm::vec3d{0, 0, 0};
}

bool containsPoint(
  const LightProperties& light, const LightFrame& frame, const vm::vec3d& world)
{
  const auto local = vm::transpose(frame.R) * (world - frame.origin);

  switch (light.type)
  {
  case LightType::Point:
    return pointFalloffT(local, vm::abs(light.radius)) <= 1.0;
  case LightType::Sun:
    return pointLightLocalBox(light).contains(local);
  case LightType::Spot: {
    const auto s = spot(light);
    if (!s)
    {
      return false;
    }

    // the inverse of spotPoint
    const auto d = vm::dot(local, s->n);
    if (d < s->zNear || d > s->zNear + s->zFar)
    {
      return false;
    }

    const auto u = (s->targetLength * vm::dot(light.right, local) / d
                    - vm::dot(light.right, light.target))
                   / vm::squared_length(light.right);
    const auto v =
      (s->targetLength * vm::dot(light.up, local) / d - vm::dot(light.up, light.target))
      / vm::squared_length(light.up);
    return std::abs(u) <= 1.0 && std::abs(v) <= 1.0;
  }
    switchDefault();
  }
}

double lightFalloff(const double t, const MathLightProperties& math)
{
  const auto k = math.falloffExp;
  const auto inner = math.innerRadius;
  const auto u = saturate((t - inner) / std::max(1.0 - inner, 1e-3));

  switch (math.falloff)
  {
  case LightFalloff::Linear:
    return 1.0 - u;
  case LightFalloff::Smooth:
    return 1.0 - smoothstep(0.0, 1.0, u);
  case LightFalloff::Exponential: {
    const auto w = saturate(1.0 - u * u * u * u);
    return (w * w) * std::exp(-k * u);
  }
  case LightFalloff::InvSquareClamped: {
    // Karis-style windowed inverse square: bounded at u = 0, zero at u = 1
    const auto w = saturate(1.0 - u * u * u * u);
    const auto d = u * k + 1e-3;
    return (w * w) / (d * d + 1.0);
  }
  case LightFalloff::InvSquare: {
    // unbounded near the source, only the energy clamp bounds it
    const auto w = saturate(1.0 - u * u * u * u);
    const auto d = u * k + 1e-3;
    return (w * w) / (d * d);
  }
    switchDefault();
  }
}

double legacyFalloff(const double t)
{
  return 1.0 - smoothstep(0.0, 1.0, t);
}

double spotMask(const double u, const double v, const MathLightProperties& math)
{
  const auto k = math.shapeExp;
  const auto r = std::pow(std::pow(std::abs(u), k) + std::pow(std::abs(v), k), 1.0 / k);

  // smoothstep(1, 1, r) divides by zero
  if (math.penumbra <= 0.0)
  {
    return r >= 1.0 ? 0.0 : 1.0;
  }
  return 1.0 - smoothstep(1.0 - math.penumbra, 1.0, r);
}

double spotFalloffT(const double d, const double zNear, const double zFar)
{
  return (d - zNear) / zFar;
}

double pointFalloffT(const vm::vec3d& local, const vm::vec3d& radius)
{
  if (hasDegenerateRadius(radius))
  {
    return std::numeric_limits<double>::infinity();
  }
  return vm::length(
    vm::vec3d{local.x() / radius.x(), local.y() / radius.y(), local.z() / radius.z()});
}

LightProperties spotFromWorldBox(const vm::bbox3d& box, const LightProperties& base)
{
  const auto size = box.size();
  const auto height = size.z();
  contract_pre(height >= MinSpotBoxDepth);

  const auto hx = size.x() / 2.0;
  const auto hy = size.y() / 2.0;
  if (hx < MinHalfExtent || hy < MinHalfExtent)
  {
    return base;
  }

  const auto explicitStartEnd = base.type == LightType::Spot && base.explicitStartEnd;
  const auto zNear = explicitStartEnd
                       ? std::min(explicitZNear(base), height - SpotLightMinZFar)
                       : SpotLightMinZNear;
  const auto targetLength = height - zNear;
  const auto scale = targetLength / height;

  auto light = base;
  light.type = LightType::Spot;
  light.badTargetInfo = false;
  light.origin = vm::vec3d{box.center().x(), box.center().y(), box.max.z()};

  // the engine's DefaultProjected: the beam points down, right along -X, up along -Y
  light.target = vm::vec3d{0, 0, -targetLength};
  light.right = vm::vec3d{-hx * scale, 0, 0};
  light.up = vm::vec3d{0, -hy * scale, 0};
  light.explicitStartEnd = explicitStartEnd;
  light.start = explicitStartEnd ? vm::vec3d{0, 0, -zNear} : vm::vec3d{0, 0, 0};
  light.end = light.target;

  return light;
}

LightProperties pointFromWorldBox(const vm::bbox3d& box, const LightProperties& base)
{
  const auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinHalfExtent || halfExtents.y() < MinHalfExtent
    || halfExtents.z() < MinHalfExtent)
  {
    return base;
  }

  auto light = base;
  light.type = base.type == LightType::Sun ? LightType::Sun : LightType::Point;
  light.badTargetInfo = false;
  light.origin = box.center();
  light.radius = halfExtents;
  light.equalRadius = equalComponents(halfExtents);

  return light;
}

} // namespace tb::mdl
