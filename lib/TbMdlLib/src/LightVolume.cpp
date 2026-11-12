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

#include "vm/intersection.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

namespace tb::mdl
{
namespace
{

// every guard of the light geometry, checked before anything is normalized
constexpr auto DegenerateEpsilon = 1e-6;

// the handle box threshold (D53, D61): its range and the bisection's steps
constexpr auto MinVisibleThreshold = 0.01;
constexpr auto MaxVisibleThreshold = 0.5;
constexpr auto VisibleFalloffSteps = 24;

// R_ComputeSpotLightProjectionMatrix
constexpr auto SpotLightMinZNear = 8.0;
constexpr auto SpotLightMinZFar = 16.0;

// the tool denies a spot box shallower than this, and a half extent below 1
constexpr auto MinSpotBoxDepth = 24.0;
constexpr auto MinHalfExtent = 1.0;

// a face of an edited box nearer than this to the base box's face did not move
constexpr auto BoxFaceEpsilon = 1e-6;

// the tolerance of a centred spot light
constexpr auto CentredRelativeEpsilon = 1e-4;
constexpr auto CentredAbsoluteEpsilon = 0.01;

// the smallest shape exponent of a superellipse
constexpr auto MinShapeExp = 1e-3;

// the in-engine aim keeps axis[1] level while worldUp x axis[0] is at least this long
constexpr auto AimLevelEpsilon = 0.01;

// two rotations whose entries agree to this are the same rotation
constexpr auto SameRotationEpsilon = 1e-6;

// a point of an emitter cap this close outside the ring of its cone is still on the cap
// (D69)
constexpr auto RingEpsilon = 1e-6;

// lightBoundingSphere bounds a frame whose R^T R is off the identity by at most this
// in any entry; a frame further off, a scale, has no bound
constexpr auto MaxFrameError = 0.1;

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

/**
 * The inverse of spotPoint: the projected position of a local point at the depth d along
 * the beam, which must be in front of the apex.
 */
vm::vec2d projectedUV(
  const LightProperties& light, const Spot& s, const vm::vec3d& local, const double d)
{
  const auto u = (s.targetLength * vm::dot(light.right, local) / d
                  - vm::dot(light.right, light.target))
                 / vm::squared_length(light.right);
  const auto v =
    (s.targetLength * vm::dot(light.up, local) / d - vm::dot(light.up, light.target))
    / vm::squared_length(light.up);
  return vm::vec2d{u, v};
}

constexpr auto CornerUVs = std::array<std::array<double, 2>, 4>{{
  {-1.0, -1.0},
  {1.0, -1.0},
  {1.0, 1.0},
  {-1.0, 1.0},
}};

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

/**
 * The light of the given box, whose half extents are the radius scaled by the given
 * falloff parameter (1 for the full extent, visibleFalloffT for the handle box), or
 * nothing if the box is denied.
 */
std::optional<LightProperties> pointFromEditBox(
  const LightProperties& base,
  const vm::mat3x3d& rotation,
  const vm::bbox3d& box,
  const double t)
{
  const auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinHalfExtent || halfExtents.y() < MinHalfExtent
    || halfExtents.z() < MinHalfExtent || t < DegenerateEpsilon)
  {
    return std::nullopt;
  }

  const auto radius = halfExtents / t;

  auto light = base;
  light.origin = base.origin + rotation * box.center();
  light.radius = radius;
  light.equalRadius = equalComponents(radius);
  return light;
}

std::optional<LightProperties> spotFromEditBox(
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
    return std::nullopt;
  }

  const auto s = spot(base);
  const auto frame = beamFrame(base);
  if (!s || !frame)
  {
    return std::nullopt;
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

/**
 * The falloff of a light that visibleFalloffT compares with its threshold: the stand-in
 * curve, at most 1.
 */
double visibleFalloff(const double t)
{
  return std::min(legacyFalloff(t), 1.0);
}

vm::mat4x4d rotationMatrix4(const vm::mat3x3d& R)
{
  auto result = vm::mat4x4d::identity();
  for (std::size_t c = 0; c < 3; ++c)
  {
    for (std::size_t r = 0; r < 3; ++r)
    {
      result[c][r] = R[c][r];
    }
  }
  return result;
}

/**
 * An emitter ignores the target vectors; they are 0, as parseLightProperties leaves them.
 */
void clearTargetVectors(LightProperties& light)
{
  light.target = vm::vec3d{0, 0, 0};
  light.right = vm::vec3d{0, 0, 0};
  light.up = vm::vec3d{0, 0, 0};
  light.explicitStartEnd = false;
  light.start = vm::vec3d{0, 0, 0};
  light.end = vm::vec3d{0, 0, 0};
  light.badTargetInfo = false;
}

bool faceMoved(const double edited, const double base)
{
  return std::abs(edited - base) > BoxFaceEpsilon;
}

bool lateralFacesMoved(const vm::bbox3d& box, const vm::bbox3d& base, const std::size_t i)
{
  return faceMoved(box.min[i], base.min[i]) || faceMoved(box.max[i], base.max[i]);
}

/**
 * The half extent of an emitter spot's box along the lateral axis i after an edit: the
 * cone is symmetric, so a single moved face sets it by its distance to the axis; with
 * both faces or neither moved it is half the width.
 */
double lateralHalfExtent(
  const vm::bbox3d& box, const vm::bbox3d& base, const std::size_t i)
{
  const auto minMoved = faceMoved(box.min[i], base.min[i]);
  const auto maxMoved = faceMoved(box.max[i], base.max[i]);
  if (minMoved && !maxMoved)
  {
    return std::abs(box.min[i]);
  }
  if (maxMoved && !minMoved)
  {
    return std::abs(box.max[i]);
  }
  return (box.max[i] - box.min[i]) / 2.0;
}

std::optional<LightProperties> emitterOmniFromEditBox(
  const LightProperties& base,
  const vm::mat3x3d& rotation,
  const vm::bbox3d& box,
  const vm::bbox3d& baseBox)
{
  auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinHalfExtent || halfExtents.y() < MinHalfExtent
    || halfExtents.z() < MinHalfExtent)
  {
    return std::nullopt;
  }

  // the range floor (G0 section 5): a drag that would leave the largest radius below 24
  // ends on 24; the dragged radii are held, in proportion, so that the largest is 24,
  // each about its face that did not move (about the centre when both moved)
  auto heldBox = box;
  if (vm::get_max_component(halfExtents) < MinEmitterRange)
  {
    auto dragged = 0.0;
    for (std::size_t i = 0; i < 3; ++i)
    {
      if (lateralFacesMoved(box, baseBox, i))
      {
        dragged = std::max(dragged, halfExtents[i]);
      }
    }
    if (dragged <= 0.0)
    {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
      const auto minMoved = faceMoved(box.min[i], baseBox.min[i]);
      const auto maxMoved = faceMoved(box.max[i], baseBox.max[i]);
      if (minMoved || maxMoved)
      {
        halfExtents[i] *= MinEmitterRange / dragged;
        const auto centre = !maxMoved   ? box.max[i] - halfExtents[i]
                            : !minMoved ? box.min[i] + halfExtents[i]
                                        : (box.min[i] + box.max[i]) / 2.0;
        heldBox.min[i] = centre - halfExtents[i];
        heldBox.max[i] = centre + halfExtents[i];
      }
    }
  }

  auto light = base;
  // the box is centred on the bulb, which light_center keeps apart from the origin
  light.origin = base.origin + rotation * (heldBox.center() - emitterBulb(base));
  light.radius = halfExtents;
  light.equalRadius = equalComponents(halfExtents);

  return faceMoved(emitterRange(light.radius), emitterRange(base.radius))
           ? applyEmitterAutoRange(light, EmitterEdit::Range)
           : light;
}

std::optional<LightProperties> emitterSpotFromEditBox(
  const LightProperties& base,
  const vm::mat3x3d& rotation,
  const vm::bbox3d& box,
  const vm::bbox3d& baseBox)
{
  // the range floor (G0 section 5): a far face dragged nearer than 24 to the bulb ends
  // on 24
  const auto range = std::max(box.max.x() - box.min.x(), MinEmitterRange);
  const auto halfH = lateralHalfExtent(box, baseBox, 1);
  const auto halfV = lateralHalfExtent(box, baseBox, 2);
  if (halfH < MinHalfExtent || halfV < MinHalfExtent)
  {
    return std::nullopt;
  }

  auto light = base;
  // the near face is the bulb, which has no handle of its own
  light.origin = base.origin + rotation * vm::vec3d{box.min.x(), 0, 0};
  light.radius = vm::vec3d{range, range, range};
  light.equalRadius = true;

  // a cone angle whose side faces stayed is kept, so that the sides follow the far face;
  // a side face at distance e from the axis sets 2 asin(e / R), clamped
  if (lateralFacesMoved(box, baseBox, 1))
  {
    light.emitter.coneH = emitterConeFromSinHalf(halfH / range);
  }
  if (lateralFacesMoved(box, baseBox, 2))
  {
    light.emitter.coneV = emitterConeFromSinHalf(halfV / range);
  }

  return faceMoved(range, emitterRange(base.radius))
           ? applyEmitterAutoRange(light, EmitterEdit::Range)
           : light;
}

/**
 * Whether the given local point is inside an emitter's lit shape: an omni's ellipsoid
 * about its bulb, a spot's cone up to its range, a sun's box.
 */
bool emitterContainsPoint(const LightProperties& light, const vm::vec3d& local)
{
  switch (light.type)
  {
  case LightType::Point:
    return pointFalloffT(local - emitterBulb(light), vm::abs(light.radius)) <= 1.0;
  case LightType::Sun:
    return pointLightLocalBox(light).contains(local);
  case LightType::Spot: {
    const auto a = local.x();
    if (a <= 0.0 || vm::length(local) > emitterRange(light.radius))
    {
      return false;
    }
    const auto u = local.y() / (a * emitterTanHalf(light.emitter.coneH));
    const auto v = local.z() / (a * emitterTanHalf(light.emitter.coneV));
    return superellipseRadius(u, v, light.emitter.squareness) <= 1.0;
  }
    switchDefault();
  }
}

/**
 * Whether the given local point in front of an emitter spot's bulb (x above 0) lies
 * inside the ring of its cone, the superellipse of its squareness.
 */
bool insideEmitterRing(const LightProperties& light, const vm::vec3d& local)
{
  const auto u = local.y() / (local.x() * emitterTanHalf(light.emitter.coneH));
  const auto v = local.z() / (local.x() * emitterTanHalf(light.emitter.coneV));
  return superellipseRadius(u, v, light.emitter.squareness) <= 1.0 + RingEpsilon;
}

} // namespace

std::ostream& operator<<(std::ostream& lhs, const LightElement rhs)
{
  switch (rhs)
  {
  case LightElement::OuterShell:
    lhs << "OuterShell";
    break;
  case LightElement::InnerShell:
    lhs << "InnerShell";
    break;
  case LightElement::Funnel:
    lhs << "Funnel";
    break;
  case LightElement::Base:
    lhs << "Base";
    break;
  case LightElement::InnerCap:
    lhs << "InnerCap";
    break;
  case LightElement::Penumbra:
    lhs << "Penumbra";
    break;
  case LightElement::SunBox:
    lhs << "SunBox";
    break;
  case LightElement::Band:
    lhs << "Band";
    break;
  case LightElement::NearCap:
    lhs << "NearCap";
    break;
    switchDefault();
  }
  return lhs;
}

vm::mat3x3d lightRotation(const Entity& entity, const EntityRotationPolicy policy)
{
  const auto R = vm::slice<3, 3>(entityRotation(entity, policy), 0, 0);

  // upstream's angle and Euler parses read nan and inf: no rotation, like an unread angle
  for (std::size_t c = 0; c < 3; ++c)
  {
    if (!vm::is_finite(R[c]))
    {
      return vm::mat3x3d::identity();
    }
  }
  return R;
}

LightFrame lightFrame(const Entity& entity, const EntityRotationPolicy policy)
{
  return {
    parseLightProperties(entity).origin,
    lightRotation(entity, policy),
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
  if (light.emitter.enabled)
  {
    return emitterEditBox(light);
  }

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

double visibleFalloffT(const LightProperties&, const double threshold)
{
  const auto level = std::clamp(threshold, MinVisibleThreshold, MaxVisibleThreshold);
  if (visibleFalloff(1.0) >= level)
  {
    return 1.0;
  }

  // the curve decreases from t = 0 to t = 1
  auto visible = 0.0;
  auto invisible = 1.0;
  for (auto step = 0; step < VisibleFalloffSteps; ++step)
  {
    const auto t = (visible + invisible) / 2.0;
    if (visibleFalloff(t) >= level)
    {
      visible = t;
    }
    else
    {
      invisible = t;
    }
  }
  return visible;
}

std::optional<vm::bbox3d> lightEditBox(
  const LightProperties& light, const double threshold)
{
  if (light.type != LightType::Point || light.emitter.enabled)
  {
    return lightEditBox(light);
  }

  const auto halfExtents = light.radius * visibleFalloffT(light, threshold);
  if (hasDegenerateRadius(light.radius) || hasDegenerateRadius(halfExtents))
  {
    return std::nullopt;
  }
  return vm::bbox3d{-halfExtents, halfExtents};
}

std::optional<LightProperties> lightFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box)
{
  if (base.emitter.enabled)
  {
    return emitterFromEditBox(base, rotation, box);
  }

  switch (base.type)
  {
  case LightType::Point:
  case LightType::Sun:
    return pointFromEditBox(base, rotation, box, 1.0);
  case LightType::Spot:
    return spotFromEditBox(base, rotation, box);
    switchDefault();
  }
}

std::optional<LightProperties> lightFromEditBox(
  const LightProperties& base,
  const vm::mat3x3d& rotation,
  const vm::bbox3d& box,
  const double threshold)
{
  return base.type == LightType::Point && !base.emitter.enabled
           ? pointFromEditBox(base, rotation, box, visibleFalloffT(base, threshold))
           : lightFromEditBox(base, rotation, box);
}

vm::vec3d spotPoint(
  const LightProperties& light, const double u, const double v, const double d)
{
  const auto s = spot(light);
  return s ? spotPoint(light, *s, u, v, d) : vm::vec3d{0, 0, 0};
}

std::optional<vm::vec2d> spotProjectedUV(
  const LightProperties& light, const vm::vec3d& local)
{
  const auto s = spot(light);
  if (!s)
  {
    return std::nullopt;
  }

  const auto d = vm::dot(local, s->n);
  if (d < DegenerateEpsilon)
  {
    return std::nullopt;
  }
  return projectedUV(light, *s, local, d);
}

double superellipseRadius(const double u, const double v, const double shapeExp)
{
  const auto k = std::max(shapeExp, MinShapeExp);
  return std::pow(std::pow(std::abs(u), k) + std::pow(std::abs(v), k), 1.0 / k);
}

bool containsPoint(
  const LightProperties& light, const LightFrame& frame, const vm::vec3d& world)
{
  const auto local = vm::transpose(frame.R) * (world - frame.origin);

  if (light.emitter.enabled)
  {
    return emitterContainsPoint(light, local);
  }

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

    const auto d = vm::dot(local, s->n);
    if (d < s->zNear || d > s->zNear + s->zFar)
    {
      return false;
    }

    const auto uv = projectedUV(light, *s, local, d);
    return std::abs(uv.x()) <= 1.0 && std::abs(uv.y()) <= 1.0;
  }
    switchDefault();
  }
}

std::pair<vm::vec3d, double> lightBoundingSphere(
  const LightProperties& light, const LightFrame& frame)
{
  constexpr auto Unbounded = std::numeric_limits<double>::infinity();

  // an emitter omni's volume is centred on its bulb, every other volume on the origin
  const auto bulb = light.emitter.enabled ? emitterBulb(light) : vm::vec3d{0, 0, 0};
  const auto centre =
    light.emitter.enabled ? frame.origin + frame.R * bulb : frame.origin;

  // how far R is from a rotation (RV2-TP-01): a rounded matrix key is off by about 0.01
  const auto gram = vm::transpose(frame.R) * frame.R - vm::mat3x3d::identity();
  auto e = 0.0;
  for (std::size_t c = 0; c < 3; ++c)
  {
    e = std::max(e, vm::get_max_component(vm::abs(gram[c])));
  }
  if (!(e <= MaxFrameError))
  {
    return {centre, Unbounded};
  }
  const auto stretch = 1.0 / std::sqrt(1.0 - 3.0 * e);

  // the bounds below hold in the light's local space, about the bulb
  const auto radius = vm::abs(light.radius);
  auto reach = Unbounded;
  switch (light.type)
  {
  case LightType::Point:
    // an ellipsoid of the radii about the centre
    reach = vm::get_max_component(radius);
    break;
  case LightType::Sun:
    // the box of the radii about the origin
    reach = vm::length(radius);
    break;
  case LightType::Spot:
    if (light.emitter.enabled)
    {
      // the cone up to the sphere of its range about the bulb
      reach = emitterRange(light.radius);
    }
    else if (const auto frustum = spotLightFrustum(light))
    {
      // the frustum is the convex hull of its corners
      reach = 0.0;
      for (const auto& corners : {frustum->nearCorners, frustum->farCorners})
      {
        for (const auto& corner : corners)
        {
          reach = std::max(reach, vm::length(corner));
        }
      }
    }
    break;
    switchDefault();
  }

  // the tests behind it compare with <= 1 in their own arithmetic
  const auto world = stretch * (reach + 3.0 * e * vm::length(bulb));
  return {centre, std::isfinite(world) ? world * (1.0 + 1e-6) + 1e-6 : Unbounded};
}

double lightFalloff(const double t, const LightCurve& curve)
{
  const auto k = curve.falloffExp;
  const auto inner = curve.innerRadius;
  const auto u = saturate((t - inner) / std::max(1.0 - inner, 1e-3));

  switch (curve.falloff)
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

double spotMask(const double u, const double v, const LightCurve& curve)
{
  const auto k = curve.shapeExp;
  const auto r = std::pow(std::pow(std::abs(u), k) + std::pow(std::abs(v), k), 1.0 / k);

  // smoothstep(1, 1, r) divides by zero
  if (curve.penumbra <= 0.0)
  {
    return r >= 1.0 ? 0.0 : 1.0;
  }
  return 1.0 - smoothstep(1.0 - curve.penumbra, 1.0, r);
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

LightProperties pointFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base, const double threshold)
{
  const auto t = visibleFalloffT(base, threshold);
  const auto halfExtents = box.size() / 2.0;
  if (
    base.type == LightType::Sun || halfExtents.x() < MinHalfExtent
    || halfExtents.y() < MinHalfExtent || halfExtents.z() < MinHalfExtent
    || t < DegenerateEpsilon)
  {
    return pointFromWorldBox(box, base);
  }

  auto light = pointFromWorldBox(box, base);
  light.radius = halfExtents / t;
  light.equalRadius = equalComponents(light.radius);
  return light;
}

std::optional<double> intersectRayEllipsoid(
  const LightFrame& frame, const vm::vec3d& radius, const double t, const vm::ray3d& ray)
{
  const auto scale = vm::abs(radius) * t;
  if (hasDegenerateRadius(scale))
  {
    return std::nullopt;
  }

  // in the space where the ellipsoid is the unit sphere, the ray keeps its parameter
  const auto toLocal = vm::transpose(frame.R);
  const auto origin = toLocal * (ray.origin - frame.origin) / scale;
  const auto direction = toLocal * ray.direction / scale;

  const auto a = vm::dot(direction, direction);
  const auto b = 2.0 * vm::dot(origin, direction);
  const auto c = vm::dot(origin, origin) - 1.0;
  const auto discriminant = b * b - 4.0 * a * c;

  // a is about 1 / radius^2 for a unit ray: only a ray without a direction has none
  if (a <= 0.0 || discriminant < 0.0)
  {
    return std::nullopt;
  }

  const auto root = std::sqrt(discriminant);
  const auto nearer = (-b - root) / (2.0 * a);
  const auto farther = (-b + root) / (2.0 * a);
  if (nearer >= 0.0)
  {
    return nearer;
  }
  if (farther >= 0.0)
  {
    return farther;
  }
  return std::nullopt;
}

std::optional<double> intersectRaySpotCap(
  const LightProperties& light,
  const LightFrame& frame,
  const double depth,
  const vm::ray3d& ray)
{
  // a cap at the apex is a point, and there is none behind it
  const auto s = spot(light);
  if (!s || depth < DegenerateEpsilon || !spotLightFrustum(light))
  {
    return std::nullopt;
  }

  auto corners = std::array<vm::vec3d, 4>{};
  std::ranges::transform(CornerUVs, corners.begin(), [&](const auto& uv) {
    return frame.origin + frame.R * spotPoint(light, *s, uv[0], uv[1], depth);
  });
  return vm::intersect_ray_polygon(ray, corners.begin(), corners.end());
}

std::optional<double> intersectRaySpotBase(
  const LightProperties& light, const LightFrame& frame, const vm::ray3d& ray)
{
  const auto s = spot(light);
  return s ? intersectRaySpotCap(light, frame, s->zNear + s->zFar, ray) : std::nullopt;
}

vm::vec3d emitterBulb(const LightProperties& light)
{
  return light.type == LightType::Point && light.hasCenter ? light.center
                                                           : vm::vec3d{0, 0, 0};
}

std::optional<double> intersectRayEmitterCap(
  const LightProperties& light,
  const LightFrame& frame,
  const double radius,
  const bool fromInside,
  const vm::ray3d& ray)
{
  const auto origin = vm::transpose(frame.R) * (ray.origin - frame.origin);
  const auto direction = vm::transpose(frame.R) * ray.direction;

  // |origin + s direction| = radius
  const auto a = vm::dot(direction, direction);
  const auto b = vm::dot(origin, direction);
  const auto c = vm::dot(origin, origin) - radius * radius;
  const auto discriminant = b * b - a * c;
  if (a <= 0.0 || discriminant < 0.0)
  {
    return std::nullopt;
  }

  const auto root = std::sqrt(discriminant);
  const auto crossings = std::array{(-b - root) / a, (-b + root) / a};
  // the first crossing is where the ray enters the sphere, the second where it leaves
  for (std::size_t i = 0; i < (fromInside ? 2u : 1u); ++i)
  {
    const auto distance = crossings[i];
    const auto point = origin + distance * direction;
    if (distance >= 0.0 && point.x() > 0.0 && insideEmitterRing(light, point))
    {
      return distance;
    }
  }
  return std::nullopt;
}

std::optional<vm::bbox3d> emitterEditBox(const LightProperties& light)
{
  if (hasDegenerateRadius(light.radius))
  {
    return std::nullopt;
  }

  switch (light.type)
  {
  case LightType::Point: {
    const auto bulb = emitterBulb(light);
    return vm::bbox3d{bulb - light.radius, bulb + light.radius};
  }
  case LightType::Spot: {
    // the bounding box of the lit cone, which ends on the sphere of its range: its side
    // faces touch the cone where it is widest (G0 section 9)
    const auto range = emitterRange(light.radius);
    const auto halfH = range * emitterSinHalf(light.emitter.coneH);
    const auto halfV = range * emitterSinHalf(light.emitter.coneV);
    return vm::bbox3d{vm::vec3d{0.0, -halfH, -halfV}, vm::vec3d{range, halfH, halfV}};
  }
  case LightType::Sun:
    return vm::bbox3d{-light.radius, light.radius};
    switchDefault();
  }
}

std::optional<LightProperties> emitterFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box)
{
  const auto baseBox = emitterEditBox(base);
  if (!baseBox)
  {
    return std::nullopt;
  }

  if (base.type == LightType::Sun)
  {
    return pointFromEditBox(base, rotation, box, 1.0);
  }

  // a box of the same size was moved
  if (vm::is_equal(box.size(), baseBox->size(), BoxFaceEpsilon))
  {
    auto light = base;
    light.origin = base.origin + rotation * (box.min - baseBox->min);
    return light;
  }

  return base.type == LightType::Spot
           ? emitterSpotFromEditBox(base, rotation, box, *baseBox)
           : emitterOmniFromEditBox(base, rotation, box, *baseBox);
}

vm::mat3x3d emitterSpotRotation()
{
  // as entityRotation reads id Tech 4 angles: roll, then pitch, then yaw
  const auto& angles = EmitterSpotPitchYawRoll;
  return vm::slice<3, 3>(
    vm::rotation_matrix(
      vm::to_radians(angles.z()), vm::to_radians(angles.x()), vm::to_radians(angles.y())),
    0,
    0);
}

LightProperties emitterSpotFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base)
{
  const auto size = box.size();
  const auto height = size.z();
  contract_pre(height >= MinSpotBoxDepth);

  // the box's half extent along each lateral axis of the new spot, from its matrix
  const auto R = emitterSpotRotation();
  const auto halfSize = size / 2.0;
  const auto halfH = vm::dot(vm::abs(R[1]), halfSize);
  const auto halfV = vm::dot(vm::abs(R[2]), halfSize);
  if (halfH < MinHalfExtent || halfV < MinHalfExtent)
  {
    return base;
  }

  auto light = base;
  clearTargetVectors(light);
  light.type = LightType::Spot;
  light.emitter.enabled = true;
  light.emitter.hasBlock = true;
  light.origin = vm::vec3d{box.center().x(), box.center().y(), box.max.z()};
  light.radius = vm::vec3d{height, height, height};
  light.equalRadius = true;
  light.hasCenter = false;
  light.center = vm::vec3d{0, 0, 0};
  // so that the drawn box is the spot's edit box, unless a side is wider than sin 60 of
  // the height
  light.emitter.coneH = emitterConeFromSinHalf(halfH / height);
  light.emitter.coneV = emitterConeFromSinHalf(halfV / height);

  return applyEmitterAutoRange(light, EmitterEdit::Range);
}

LightProperties emitterOmniFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base)
{
  // a point-light box smaller than 24 makes no light; a sun's box may be any size
  const auto halfExtents = box.size() / 2.0;
  if (
    halfExtents.x() < MinHalfExtent || halfExtents.y() < MinHalfExtent
    || halfExtents.z() < MinHalfExtent
    || (base.type != LightType::Sun && vm::get_max_component(halfExtents) < MinEmitterRange))
  {
    return base;
  }

  auto light = base;
  clearTargetVectors(light);
  light.type = base.type == LightType::Sun ? LightType::Sun : LightType::Point;
  light.emitter.enabled = true;
  light.emitter.hasBlock = true;
  light.radius = halfExtents;
  light.equalRadius = equalComponents(halfExtents);
  // the rotation keys are removed, so the bulb is origin + light_center
  light.origin = box.center() - emitterBulb(light);

  return applyEmitterAutoRange(light, EmitterEdit::Range);
}

std::optional<OrientedLight> emitterSpotFromTextureSpot(
  const LightProperties& light, const vm::mat3x3d& rotation)
{
  if (light.type != LightType::Spot || light.emitter.enabled)
  {
    return std::nullopt;
  }

  const auto s = spot(light);
  const auto beam = beamFrame(light);
  if (!s || !beam)
  {
    return std::nullopt;
  }

  // forward along the beam; y is minus right, so that the inverse writes right as the
  // engine's right, which is minus left
  auto axes = vm::mat3x3d{};
  axes[0] = s->n;
  axes[1] = -beam->r;
  axes[2] = vm::cross(axes[0], axes[1]);

  const auto range = s->zNear + s->zFar;

  auto result = light;
  result.emitter.enabled = true;
  result.emitter.hasBlock = true;
  result.emitter.coneH =
    emitterConeFromTanHalf(vm::length(light.right) / s->targetLength);
  result.emitter.coneV = emitterConeFromTanHalf(vm::length(light.up) / s->targetLength);
  result.radius = vm::vec3d{range, range, range};
  result.equalRadius = true;
  result.hasCenter = false;
  result.center = vm::vec3d{0, 0, 0};
  // an explicit light_start is where the old light began: the emitter starts there too;
  // otherwise the block keeps its own start, 0 for a new block
  if (light.explicitStartEnd)
  {
    result.emitter.start = s->zNear;
  }
  clearTargetVectors(result);

  // the kind switch is no range edit (G0 section 5): a kept linear or smooth block keeps
  // its auto range flag, which a Range edit would turn off
  auto converted = applyEmitterAutoRange(result, EmitterEdit::Range);
  converted.emitter.autoRange = result.emitter.autoRange;

  return OrientedLight{
    std::move(converted),
    rotation * axes,
  };
}

LightProperties textureSpotFromEmitterSpot(const LightProperties& light)
{
  contract_pre(light.type == LightType::Spot);

  // the engine's near plane is at 8 and its far plane at 8 + T, so the beam ends at R
  const auto T =
    std::max(emitterRange(light.radius) - SpotLightMinZNear, SpotLightMinZFar);

  auto result = light;
  result.emitter.enabled = false;
  result.badTargetInfo = false;
  result.target = vm::vec3d{T, 0, 0};
  result.right = vm::vec3d{0, -T * emitterTanHalf(light.emitter.coneH), 0};
  result.up = vm::vec3d{0, 0, T * emitterTanHalf(light.emitter.coneV)};
  result.explicitStartEnd = false;
  result.start = vm::vec3d{0, 0, 0};
  result.end = result.target;

  // a texture spot reads no radius and no centre: the values parseLightProperties leaves
  result.radius = vm::vec3d{300, 300, 300};
  result.equalRadius = true;
  result.hasCenter = false;
  result.center = vm::vec3d{0, 0, 0};
  return result;
}

LightProperties asTextureLight(const LightProperties& light)
{
  if (!light.emitter.enabled)
  {
    return light;
  }
  if (light.type == LightType::Spot)
  {
    return textureSpotFromEmitterSpot(light);
  }

  // an omni and a sun share their volume keys with a texture light
  auto result = light;
  result.emitter.enabled = false;
  return result;
}

std::pair<LightProperties, std::optional<vm::mat3x3d>> asEmitterLight(
  const Entity& entity, const LightProperties& light, const EntityRotationPolicy policy)
{
  const auto keptBlock = hasEmitterBlock(entity);

  auto block = light.emitter;
  if (keptBlock)
  {
    block.enabled = true;
    block.hasBlock = true;
  }
  else
  {
    block = seededEmitterProperties();
  }
  // a light's first effect is its style's: style and emit_effect select the same
  // predefined styles (G0 section 9, kind switch)
  if (!entity.hasProperty(LightPropertyKeys::EmitEffect))
  {
    block.effect = emitterEffectForStyle(light.style);
  }

  // the test of a hand-set range, made where the kept block comes back (DP-T5-02), so
  // that the light does not keep a stored "1" that it does not honour
  const auto withHandSetTest = [&](LightProperties result) {
    if (keptBlock && emitterRangeSetByHand(result))
    {
      result.emitter.autoRange = false;
    }
    return result;
  };

  if (light.type != LightType::Spot)
  {
    auto result = light;
    result.emitter = block;
    return {
      keptBlock ? withHandSetTest(result)
                : applyEmitterAutoRange(result, EmitterEdit::Range),
      std::nullopt};
  }

  // the conversion reads the texture spot with the block it will enable
  auto texture = light;
  texture.emitter = block;
  texture.emitter.enabled = false;
  if (
    const auto oriented =
      emitterSpotFromTextureSpot(texture, lightRotation(entity, policy)))
  {
    return {withHandSetTest(oriented->light), oriented->rotation};
  }
  return {seededEmitterLight(LightType::Spot, light), emitterSpotRotation()};
}

LightProperties withEmitterRange(const LightProperties& light, const double range)
{
  auto result = light;
  if (light.type == LightType::Spot)
  {
    result.radius = vm::vec3d{range, range, range};
    result.equalRadius = true;
  }
  else if (light.type == LightType::Point)
  {
    const auto largest = emitterRange(light.radius);
    result.radius =
      largest > 0.0 ? light.radius * (range / largest) : vm::vec3d{range, range, range};
  }
  return result;
}

vm::mat3x3d emitterAimRotation(const vm::vec3d& forward, const vm::vec3d& previousLeft)
{
  auto axes = vm::mat3x3d{};
  axes[0] = vm::normalize(forward);

  // the in-engine aim's rule: the roll keeps axis[1] level, unless the beam is within
  // 0.01 of vertical, where the previous axis[1] is kept
  const auto level = vm::cross(vm::vec3d{0, 0, 1}, axes[0]);
  const auto left = vm::length(level) >= AimLevelEpsilon
                      ? level
                      : previousLeft - vm::dot(previousLeft, axes[0]) * axes[0];
  axes[1] = vm::normalize(left);
  axes[2] = vm::cross(axes[0], axes[1]);
  return axes;
}

EntityRotationUpdate lightRotationUpdate(
  const Entity& entity, const vm::mat3x3d& rotation)
{
  // the same rotation is no edit, even where its angles would be written differently
  if (vm::is_equal(
        lightRotation(entity, EntityRotationPolicy::IdTech4),
        rotation,
        SameRotationEpsilon))
  {
    return {};
  }

  const auto yawPitchRoll =
    entityYawPitchRoll(vm::mat4x4d::identity(), rotationMatrix4(rotation));
  if (vm::is_nan(yawPitchRoll))
  {
    return {};
  }
  return idTech4AnglesUpdate(
    entity, vm::vec3d{yawPitchRoll.y(), yawPitchRoll.x(), yawPitchRoll.z()});
}

LightCurve emitterCurve(const LightProperties& light)
{
  const auto& emitter = light.emitter;
  const auto range = emitterRange(light.radius);
  const auto start = clampEmitterStart(emitter.start, range);

  return LightCurve{
    .falloff = emitter.falloff,
    .falloffExp = (range - start) / std::max(emitter.size, 1.0),
    .innerRadius = range > 0.0 ? start / range : 0.0,
    .shapeExp = emitter.squareness,
    .penumbra = emitter.penumbra,
    .pattern = emitter.pattern,
    .patternStrength = emitter.patternStrength,
    .patternScale = emitter.patternScale,
    .patternPhase = emitter.patternPhase,
    .energyClamp = emitter.energyClamp,
  };
}

double emitterFalloffT(const LightProperties& light, const vm::vec3d& local)
{
  const auto range = emitterRange(light.radius);
  const auto radius = light.type == LightType::Spot ? vm::vec3d{range, range, range}
                                                    : vm::abs(light.radius);
  return pointFalloffT(local - emitterBulb(light), radius);
}

double emitterSpotMask(const LightProperties& light, const vm::vec3d& local)
{
  if (light.type != LightType::Spot)
  {
    return 1.0;
  }

  // the shader's p: the offset from the bulb in units of the range
  const auto p = local / emitterRange(light.radius);
  const auto safeA = std::max(p.x(), 1e-4);
  const auto u = p.y() / (emitterTanHalf(light.emitter.coneH) * safeA);
  const auto v = p.z() / (emitterTanHalf(light.emitter.coneV) * safeA);
  return p.x() > 0.0 ? spotMask(u, v, emitterCurve(light)) : 0.0;
}

double emitterFalloff(const LightProperties& light, const vm::vec3d& local)
{
  return lightFalloff(emitterFalloffT(light, local), emitterCurve(light))
         * emitterSpotMask(light, local);
}

} // namespace tb::mdl
