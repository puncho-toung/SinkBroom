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

#pragma once

#include "mdl/EntityProperties.h"
#include "mdl/Light.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <array>
#include <iosfwd>
#include <optional>

namespace tb::mdl
{
class Entity;

/**
 * The parts of a drawn light volume (light editor plan, D55, D69). The first seven are
 * the elements that the editor names; the last two are the translucency layers that are
 * drawn but neither picked nor highlighted, so that the parts tile the volume.
 */
enum class LightElement
{
  // a point light's ellipsoid at t = 1, where its falloff reaches 0
  OuterShell,
  // a math point light's ellipsoid at t = math_inner_radius, the edge of its core
  InnerShell,
  // a spot light's cone surface from the near to the far cap
  Funnel,
  // a spot light's far cap
  Base,
  // a math spot light's cap at t = math_inner_radius, the end of its core
  InnerCap,
  // a spot light's inner cone at 1 - math_penumbra (a light without math at the
  // stand-in's 0.15), never picked
  Penumbra,
  // a sun's box
  SunBox,
  // an intermediate translucency shell of a point light, never picked
  Band,
  // a spot light's near cap, never picked
  NearCap,
};

std::ostream& operator<<(std::ostream& lhs, LightElement rhs);

/**
 * The geometry of an id Tech 4 light as RBDOOM-3-BFG's renderer derives it
 * (R_ComputePointLightProjectionMatrix, R_ComputeSpotLightProjectionMatrix) and the
 * shading curves of its math lights (mathlight.inc.hlsl).
 *
 * "Local" is the engine's light space: a point p_l is at origin + R * p_l in the world,
 * where R is the light's rotation, whose column i is the engine's axis[i]. A spot light's
 * apex is the local origin.
 */

/**
 * The light's position and rotation: world = origin + R * local.
 */
struct LightFrame
{
  vm::vec3d origin;
  vm::mat3x3d R;
};

/**
 * Returns the frame of the given light entity: its light_origin if it has one, else its
 * origin, and the 3x3 part of entityRotation(entity, policy). The policy must be passed
 * explicitly, the game's entities.rotation.policy.
 */
LightFrame lightFrame(const Entity& entity, EntityRotationPolicy policy);

/**
 * The volume of a point light or a sun in local space: the box of +-radius.
 */
vm::bbox3d pointLightLocalBox(const LightProperties& light);

/**
 * The lit volume of a spot light in local space. The engine lights d = p * n from zNear
 * to zNear + zFar, so the far corners are at depth zNear + zFar. The corners are at the
 * projected positions (u, v) = (-1, -1), (1, -1), (1, 1), (-1, 1), in this order.
 */
struct SpotFrustum
{
  std::array<vm::vec3d, 4> nearCorners;
  std::array<vm::vec3d, 4> farCorners;
  // the unit beam direction, target / |target|
  vm::vec3d n;
  double zNear;
  double zFar;
};

/**
 * Returns the lit volume of the given spot light, or nothing if it is degenerate: if
 * |target|, |right| or |up| is below 1e-6, the rows right, up, n are linearly dependent
 * (|det| below 1e-6), or a corner is not in front of the apex. zNear > zFar is valid.
 */
std::optional<SpotFrustum> spotLightFrustum(const LightProperties& light);

/**
 * The beam frame of a spot light in local space: n along the target, r the part of right
 * orthogonal to n, u = n x r, so that [r u n] is a proper rotation. upSign is the sign of
 * u * up (+1 when it is 0): the base's up points along upSign * u.
 */
struct BeamFrame
{
  vm::vec3d r;
  vm::vec3d u;
  vm::vec3d n;
  double upSign;
};

/**
 * Returns the beam frame of the given spot light, or nothing if it is degenerate (see
 * spotLightFrustum).
 */
std::optional<BeamFrame> beamFrame(const LightProperties& light);

/**
 * Whether right, up and the target of the given spot light are orthogonal within the
 * tolerance that absorbs the two decimals of the written values: |right * n| <=
 * 1e-4 |right| + 0.01, |up * n| <= 1e-4 |up| + 0.01 and |right * up| <=
 * 1e-4 |right| |up| + 0.01. A spot light that is not centred is drawn exactly, and a box
 * edit recentres it.
 */
bool isCentredSpot(const LightProperties& light);

/**
 * The box that the light volume tool edits: for a point light or a sun the box of
 * +-radius in local space, for a spot light the box [-hx, hx] x [-hy, hy] x
 * [0, zNear + zFar] in beam space (the coordinates along r, u and n of the beam frame),
 * where hx and hy are the half extents of the far cap, |right| (zNear + zFar) / |target|
 * and |up| (zNear + zFar) / |target|. The apex is the centre of the face z = 0. Returns
 * nothing for a degenerate light (a radius component below 1e-6, or see
 * spotLightFrustum).
 */
std::optional<vm::bbox3d> lightEditBox(const LightProperties& light);

/**
 * The largest falloff parameter t in [0, 1] at which the light is still visible: its
 * falloff curve (lightFalloff for a math light, legacyFalloff for a light without math),
 * at most 1, is at least the given threshold there. Found by bisection in 24 steps; a
 * math light with an inner radius of at least 0.999 is visible to t = 1. The threshold is
 * clamped to [0.01, 0.5].
 */
double visibleFalloffT(const LightProperties& light, double threshold);

/**
 * The handle box of the light volume tool (light editor plan, D61): for a point light the
 * box of +-radius * visibleFalloffT(light, threshold) in local space, so that its faces
 * are where the light fades below the threshold; for a spot light and a sun the box of
 * lightEditBox. Returns nothing for a degenerate light.
 */
std::optional<vm::bbox3d> lightEditBox(const LightProperties& light, double threshold);

/**
 * Returns the given light changed so that its edit box is the given box, which is in the
 * space of the base's edit box. The given rotation is the light's R, which places a
 * moved centre or apex in the world.
 *
 * A point light or a sun gets the centre of the box as its origin and the half extents as
 * its radius. A spot light keeps its apex laterally and follows the box with the seven
 * steps of the handle contract (light editor plan, section 3.3): with (cx, cy) the box's
 * lateral centre, L its depth and D' = |(cx, cy, L)|, the apex moves to the box's near
 * face, zN* = min(zNear, D' - 16), |target'| = D' - zN* along the box's centre direction,
 * and right' and up' are scaled so that the far cap is at distance D'.
 *
 * Returns nothing if the box is denied: a point half extent below 1, a spot box
 * shallower than 24 or with a half extent below 1, or a degenerate base.
 */
std::optional<LightProperties> lightFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box);

/**
 * Returns the given light changed so that its handle box (lightEditBox with the given
 * threshold) is the given box: a point light's radius is the box's half extents divided
 * by visibleFalloffT of the base, so that dragging a face of the handle box by d moves
 * the radius on that side by d / visibleFalloffT; spot lights and suns as
 * lightFromEditBox without a threshold.
 *
 * Returns nothing if the box is denied: as without a threshold, or a point light whose
 * visibleFalloffT is below 1e-6.
 */
std::optional<LightProperties> lightFromEditBox(
  const LightProperties& base,
  const vm::mat3x3d& rotation,
  const vm::bbox3d& box,
  double threshold);

/**
 * Returns the local point of the given spot light at the projected position (u, v) in
 * [-1, 1]^2 and depth d along the beam, the solution p of
 * [right; up; n] p = d ((right * target + u |right|^2) / |target|,
 *                       (up * target + v |up|^2) / |target|, 1),
 * which is the engine's projection after it recentres the target. For a centred spot this
 * is P (d / (P * n)) with P = target + u right + v up. Returns the apex for a degenerate
 * spot light.
 */
vm::vec3d spotPoint(const LightProperties& light, double u, double v, double d);

/**
 * Returns the projected position (u, v) of the given local point of the given spot
 * light, the inverse of spotPoint at the point's depth d = p * n: (u, v) = (+-1, +-1) at
 * the corners of every cap. Nothing for a degenerate spot light or a point that is not
 * in front of the apex (d below 1e-6).
 */
std::optional<vm::vec2d> spotProjectedUV(
  const LightProperties& light, const vm::vec3d& local);

/**
 * The superellipse radius (|u|^k + |v|^k)^(1/k) of the projected position (u, v) for the
 * shape exponent k, at least 1e-3: 1 on the ring of a spot light's cone.
 */
double superellipseRadius(double u, double v, double shapeExp);

/**
 * Whether the given world point is inside the light's volume: the ellipsoid inscribed in
 * the box of a point light, the lit frustum of a spot light, the box of a sun.
 */
bool containsPoint(
  const LightProperties& light, const LightFrame& frame, const vm::vec3d& world);

/**
 * The math light falloff curve MLFalloffCurve in double precision: 1 at t = 0 (except
 * the unclamped inverse square, which is 1e6 there) and 0 at t = 1.
 */
double lightFalloff(double t, const MathLightProperties& math);

/**
 * The stand-in falloff of a light without math: 1 - smoothstep(0, 1, t).
 */
double legacyFalloff(double t);

/**
 * The spot light's superellipse mask MLSpotMask at the projected position (u, v): 1
 * inside the penumbra ring, 0 outside the unit superellipse. A penumbra of 0 gives a
 * sharp edge.
 */
double spotMask(double u, double v, const MathLightProperties& math);

/**
 * The falloff parameter of a spot light at depth d: (d - zNear) / zFar, 0 at the near cap
 * and 1 at the far cap.
 */
double spotFalloffT(double d, double zNear, double zFar);

/**
 * The falloff parameter of a point light at the given local point: the length of the
 * point divided by the radius per component, 1 on the ellipsoid inscribed in the box.
 * Infinite for a radius component below 1e-6.
 */
double pointFalloffT(const vm::vec3d& local, const vm::vec3d& radius);

/**
 * Returns the given light as a spot light that fills the given world-aligned box (draw
 * mode): its beam points down from the centre of the box's top face, and its far cap is
 * the bottom face. Without explicit start and end points, |target| = H - 8 for a box of
 * height H; with them (if the base is an explicit spot light), zN* = min(zNear of the
 * base, H - 16), start = zN* down and end = target, |target| = H - zN*. The rotation
 * properties must be removed. The box must be at least 24 high; returns the base if a
 * half extent is below 1.
 */
LightProperties spotFromWorldBox(const vm::bbox3d& box, const LightProperties& base);

/**
 * Returns the given light as a point light (a sun stays a sun) that fills the given
 * world-aligned box (draw mode): the box's centre is its origin and its half extents are
 * its radius. The rotation properties must be removed. Returns the base if a half extent
 * is below 1.
 */
LightProperties pointFromWorldBox(const vm::bbox3d& box, const LightProperties& base);

/**
 * Returns the given light as a point light (a sun stays a sun) whose handle box
 * (lightEditBox with the given threshold) is the given world-aligned box: the box's
 * centre is its origin, and a point light's radius is the box's half extents divided by
 * visibleFalloffT of the base, so that the drawn box is where the light will be visible.
 * Returns the base if a half extent is below 1.
 */
LightProperties pointFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base, double threshold);

/**
 * The distance along the given world ray to the ellipsoid of the given radius scaled by
 * t in the given frame (a point light's shell at the falloff parameter t): the nearer of
 * the two intersections that is not behind the ray's origin, so the far one if the
 * origin is inside. Nothing if the ray misses it or the scaled radius is degenerate.
 */
std::optional<double> intersectRayEllipsoid(
  const LightFrame& frame, const vm::vec3d& radius, double t, const vm::ray3d& ray);

/**
 * The distance along the given world ray to the cap of the given spot light in the given
 * frame at the given depth along the beam: the parallelogram of the points spotPoint(+-1,
 * +-1, depth), from either side. Nothing if the ray misses it, the depth is not in front
 * of the apex or the light is degenerate.
 */
std::optional<double> intersectRaySpotCap(
  const LightProperties& light,
  const LightFrame& frame,
  double depth,
  const vm::ray3d& ray);

/**
 * The distance along the given world ray to the far cap of the given spot light in the
 * given frame, the parallelogram of its far corners at depth zNear + zFar, from either
 * side: intersectRaySpotCap at that depth. Nothing if the ray misses it or the light is
 * degenerate.
 */
std::optional<double> intersectRaySpotBase(
  const LightProperties& light, const LightFrame& frame, const vm::ray3d& ray);

} // namespace tb::mdl
