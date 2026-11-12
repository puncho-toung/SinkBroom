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
#include "mdl/EntityRotation.h"
#include "mdl/Light.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <array>
#include <iosfwd>
#include <optional>
#include <utility>

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
  // an omni emitter's ellipsoid at its start (emit_start), the edge of its core
  InnerShell,
  // a spot light's cone surface from the near to the far cap
  Funnel,
  // a spot light's far cap
  Base,
  // an emitter spot's cap at its start (emit_start), the end of its core
  InnerCap,
  // a spot light's inner cone at 1 - emit_penumbra (a texture spot at the stand-in's
  // 0.15), never picked
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
 * shading curves of its emitters (emitter.inc.hlsl).
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
 * Returns the rotation of the given light entity: the 3x3 part of
 * entityRotation(entity, policy), or the identity if it is not finite, as for an angle
 * that is not read. The policy must be passed explicitly, the game's
 * entities.rotation.policy.
 */
vm::mat3x3d lightRotation(const Entity& entity, EntityRotationPolicy policy);

/**
 * Returns the frame of the given light entity: its light_origin if it has one, else its
 * origin, and lightRotation(entity, policy). The policy must be passed explicitly, the
 * game's entities.rotation.policy.
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
 * An enabled emitter's box is emitterEditBox, without a threshold.
 */
std::optional<vm::bbox3d> lightEditBox(const LightProperties& light);

/**
 * The largest falloff parameter t in [0, 1] at which the light is still visible: its
 * stand-in falloff curve (legacyFalloff), at most 1, is at least the given threshold
 * there. Found by bisection in 24 steps. The threshold is clamped to [0.01, 0.5].
 */
double visibleFalloffT(const LightProperties& light, double threshold);

/**
 * The handle box of the light volume tool (light editor plan, D61): for a point light the
 * box of +-radius * visibleFalloffT(light, threshold) in local space, so that its faces
 * are where the light fades below the threshold; for a spot light and a sun the box of
 * lightEditBox. Returns nothing for a degenerate light.
 * An enabled emitter's box is emitterEditBox, without a threshold.
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
 * An enabled emitter is emitterFromEditBox, without a threshold.
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
 * An enabled emitter is emitterFromEditBox, without a threshold.
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
 * An enabled emitter: its lit shape, see the emitter functions below.
 */
bool containsPoint(
  const LightProperties& light, const LightFrame& frame, const vm::vec3d& world);

/**
 * A sphere about the given light's volume in the given frame: every world point that
 * containsPoint accepts, and every point of the light's volume placed in the world by
 * the frame (origin + R * local), lies within the radius of the centre. The centre is an
 * emitter's bulb in the world, origin + R * emitterBulb, else the frame's origin. The
 * radius about it: a point light's largest radius, a sun's box diagonal, an emitter
 * spot's range, a texture spot's farthest frustum corner; infinite where no bound is
 * known (a degenerate spot).
 *
 * R need not be a rotation. A frame read from a matrix key that was written with two
 * decimals is orthonormal only to about 0.01, so the radius is widened by a bound on its
 * error: with e the largest entry of |R^T R - I|, Gershgorin gives the singular values of
 * R in [sqrt(1 - 3e), sqrt(1 + 3e)], so a local distance d is at most d / sqrt(1 - 3e) in
 * the world either way, and an emitter's bulb, placed by R where containsPoint inverts
 * with R^T, is off by at most 3e |bulb| / sqrt(1 - 3e). For e above 0.1 the radius is
 * infinite.
 */
std::pair<vm::vec3d, double> lightBoundingSphere(
  const LightProperties& light, const LightFrame& frame);

/**
 * The falloff curve of emitter.inc.hlsl in double precision: 1 at t = 0 (except
 * the unclamped inverse square, which is 1e6 there) and 0 at t = 1.
 */
double lightFalloff(double t, const LightCurve& curve);

/**
 * The stand-in falloff of a texture light: 1 - smoothstep(0, 1, t).
 */
double legacyFalloff(double t);

/**
 * The spot light's superellipse mask of emitter.inc.hlsl at the projected position (u,
 * v): 1 inside the penumbra ring, 0 outside the unit superellipse. A penumbra of 0 gives
 * a sharp edge.
 */
double spotMask(double u, double v, const LightCurve& curve);

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

/**
 * The bulb of an emitter in local space: light_center for an omni that has one, else the
 * origin.
 */
vm::vec3d emitterBulb(const LightProperties& light);

/**
 * The distance along the given world ray to a cap of the given emitter spot in the given
 * frame: the part of the sphere of the given radius about its bulb that lies in front of
 * the bulb inside the ring of its cone, the superellipse of its squareness (with a
 * tolerance of 1e-6). That is its round cap at its range and the cap of its core at its
 * start. The ray's first crossing of the sphere, where it enters, if it lies on the cap;
 * with fromInside also the second, where it leaves. Nothing if neither does.
 */
std::optional<double> intersectRayEmitterCap(
  const LightProperties& light,
  const LightFrame& frame,
  double radius,
  bool fromInside,
  const vm::ray3d& ray);

/**
 * The box that the light volume tool edits for an emitter, in the light's own axes (local
 * space: forward, left, up; not the beam space of a texture spot). An omni:
 * +-light_radius about the bulb, without visible-falloff scaling. A spot: [0, R] x [-R
 * sinHalfH, R sinHalfH] x [-R sinHalfV, R sinHalfV] with sinHalf = sin(cone / 2), whose
 * face x = 0 is the bulb: the bounding box of the lit cone, which ends on the sphere of
 * radius R, so that its side faces touch the cone where it is widest (G0 section 9). A
 * sun: +-radius. Nothing for a radius component below 1e-6.
 */
std::optional<vm::bbox3d> emitterEditBox(const LightProperties& light);

/**
 * Returns the given emitter changed so that its edit box is the given box, which is in
 * the space of the base's edit box; the rotation is the light's R. A box of the base
 * box's size moves the origin and changes nothing else. Otherwise an omni gets the half
 * extents as its radius and keeps its light_center; a spot gets the box's depth as its
 * range, keeps a cone angle whose side faces did not move (so the sides follow the far
 * face), and sets a cone angle whose side face moved to 2 asin(e / R), e that face's
 * distance to the axis (half the width when both faces moved) and R the new range,
 * clamped to 1 to 120 degrees; an edge or a corner
 * applies both rules. A changed range then sets the size by the auto range rule. A sun is
 * edited like today's. The range floor ends a drag on 24 (G0 section 5): a spot's depth
 * below 24 is 24, and an omni whose largest half extent would be below 24 has its dragged
 * half extents scaled, in proportion and each about its face that did not move, so that
 * the largest is 24. Returns nothing if the box is denied: any half extent below 1.
 */
std::optional<LightProperties> emitterFromEditBox(
  const LightProperties& base, const vm::mat3x3d& rotation, const vm::bbox3d& box);

/**
 * The rotation of a new emitter spot, EmitterSpotPitchYawRoll read as entityRotation
 * reads id Tech 4 angles: axis[0] points down, axis[1] is world +Y, axis[2] world +X.
 */
vm::mat3x3d emitterSpotRotation();

/**
 * Returns the given light as an emitter spot that fills the given world-aligned box (draw
 * mode): its bulb at the centre of the box's top face, its range the box's height, and
 * each cone angle 2 asin(half extent / height), clamped to 1 to 120 degrees, from the
 * box's half extent along the matching axis of emitterSpotRotation(), so that the drawn
 * box is the spot's edit box unless a side is wider than sin 60 of the height. The size
 * follows the range by auto range. The
 * caller writes the rotation EmitterSpotPitchYawRoll. The box must be at least 24 high;
 * returns the base if a half extent is below 1.
 */
LightProperties emitterSpotFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base);

/**
 * Returns the given light as an emitter omni (a sun stays a sun) that fills the given
 * world-aligned box (draw mode): its bulb at the box's centre and its radius the half
 * extents. The rotation properties must be removed, as today. The size follows the range
 * by auto range. Returns the base if a half extent is below 1, or for an omni if the
 * largest is below 24; a sun's box may be any size.
 */
LightProperties emitterOmniFromWorldBox(
  const vm::bbox3d& box, const LightProperties& base);

/**
 * A light with the rotation it must be written with.
 */
struct OrientedLight
{
  LightProperties light;
  vm::mat3x3d rotation;
};

/**
 * Converts a texture spot into an emitter spot: with n = target / |target| and the beam
 * frame's r (right made perpendicular to n), the emitter's axes in the old light's space
 * are x = n, y = -r, z = x cross y, turned by the given rotation; coneH = 2 atan(|right|
 * / |target|) and coneV = 2 atan(|up| / |target|), clamped; range = zNear + zFar; the
 * origin, the colour and the kept emitter values stay, the target vectors become 0, and
 * the size follows the range by auto range. With an explicit light_start the emitter's
 * start is zNear, max(start * n, 8); otherwise the block keeps its own start, 0 for a new
 * block. Nothing for a degenerate spot or a light that is not a texture spot. The inverse
 * of textureSpotFromEmitterSpot.
 */
std::optional<OrientedLight> emitterSpotFromTextureSpot(
  const LightProperties& light, const vm::mat3x3d& rotation);

/**
 * Converts an emitter spot into a texture spot with the same rotation: with T = max(R -
 * 8, 16), light_target (T, 0, 0), light_right (0, -T tanHalfH, 0), light_up (0, 0, T
 * tanHalfV), no start or end. The emitter block is disabled and keeps its values. The
 * light must be a spot. The inverse of emitterSpotFromTextureSpot for a range of 24 or
 * more.
 */
LightProperties textureSpotFromEmitterSpot(const LightProperties& light);

/**
 * The only way an emitter becomes a texture light: a spot through
 * textureSpotFromEmitterSpot, an omni and a sun by clearing the flag, since they share
 * their volume keys. The rotation is unchanged. A light that is not an emitter is
 * returned unchanged.
 */
LightProperties asTextureLight(const LightProperties& light);

/**
 * The inverse of asTextureLight, the Emitter tab's switch (G0 section 9, kind switch):
 * the given light of the given entity as an emitter of its own type, and the rotation to
 * write with it, if any. A kept emitter block (hasEmitterBlock) is enabled again; a light
 * without one gets the block of a new emitter. A light's first effect is its style's
 * (emitterEffectForStyle) when the entity has no emit_effect key. A spot's frustum
 * becomes its forward axis, cone and range (emitterSpotFromTextureSpot), the kept values
 * staying; a spot without a frustum becomes a new spot pointing down
 * (seededEmitterLight, emitterSpotRotation). A point light or a sun keeps its volume,
 * whose keys emitters share, and a new block's size follows its range. A kept block whose
 * range is out of step with its auto range is brought back with auto range off, as a read
 * of it would take it (G0 section 5, a hand-set range wins). The policy is the game's
 * entities.rotation.policy.
 */
std::pair<LightProperties, std::optional<vm::mat3x3d>> asEmitterLight(
  const Entity& entity, const LightProperties& light, EntityRotationPolicy policy);

/**
 * The given light with the given range by an emitter's rule (G0 section 5): a spot's
 * three radii are the range, an omni's box is scaled as a whole so that its largest
 * radius is the range. A sun has no range and is returned unchanged.
 */
LightProperties withEmitterRange(const LightProperties& light, double range);

/**
 * The rotation of an emitter spot whose beam runs along the given direction, with the
 * roll of the in-engine aim's world-up rule (G0 section 9): axis[0] the direction,
 * axis[1] = normalize(worldUp x axis[0]), or, when that cross product is shorter than
 * 0.01, the given previous axis[1] made perpendicular to axis[0]; axis[2] = axis[0] x
 * axis[1].
 */
vm::mat3x3d emitterAimRotation(const vm::vec3d& forward, const vm::vec3d& previousLeft);

/**
 * The property changes that write the given rotation (column i = axis[i]) to the given
 * light entity the way the editors write rotation today: idTech4AnglesUpdate with the
 * matrix's pitch, yaw and roll. Nothing for a degenerate rotation, and nothing when the
 * entity's rotation (lightRotation under the id Tech 4 policy) already is the given one,
 * each entry within 1e-6, so that its keys stay as they are.
 */
EntityRotationUpdate lightRotationUpdate(
  const Entity& entity, const vm::mat3x3d& rotation);

/**
 * The curve and mask inputs of an emitter for lightFalloff and spotMask:
 * with start' = clamp(start, 0, R - 1), innerRadius = start' / R and falloffExp = k = (R
 * - start') / max(size, 1); falloff, squareness (shapeExp), penumbra, pattern and energy
 * clamp as the emitter has them.
 */
LightCurve emitterCurve(const LightProperties& light);

/**
 * The falloff parameter of an emitter at the given local point: the length of the point's
 * offset from the bulb divided per axis by the radius (a spot's range on every axis). Not
 * saturated; a sun reads like an omni about its origin.
 */
double emitterFalloffT(const LightProperties& light, const vm::vec3d& local);

/**
 * The spot mask of an emitter at the given local point: with p the offset from the bulb
 * over the range, a = p.x and safeA = max(a, 1e-4), spotMask((p.y / tanHalfH / safeA,
 * p.z / tanHalfV / safeA)) in front of the bulb and 0 behind it. 1 for an omni and a sun.
 */
double emitterSpotMask(const LightProperties& light, const vm::vec3d& local);

/**
 * lightFalloff(emitterFalloffT, emitterCurve) times emitterSpotMask: the emitter's
 * attenuation at the given local point, before colour, pattern and energy clamp.
 */
double emitterFalloff(const LightProperties& light, const vm::vec3d& local);

} // namespace tb::mdl
