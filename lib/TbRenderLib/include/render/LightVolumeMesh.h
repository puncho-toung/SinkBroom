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

#include "gl/VertexType.h"

#include "vm/vec.h"

#include <optional>
#include <vector>

namespace tb
{
namespace mdl
{
struct LightFrame;
struct LightProperties;
struct MathLightProperties;
} // namespace mdl

namespace render
{

/**
 * How a light volume is drawn; read once per frame from the preferences.
 */
struct LightVolumeStyle
{
  // the alpha of a clamped or legacy light at full brightness; an unclamped math light
  // reaches 1
  double alphaCap = 0.45;
  // the number of nested shells of a point light, and the number of rings minus one of
  // a spot light's cone
  int shells = 4;
  // a 2D view: every fill alpha is scaled by 0.6, the lines are unchanged
  bool twoD = false;
  // only the outermost surface: one shell of a point light whose alpha is the composite
  // of all shells through the centre, the outer cone of a spot light without its
  // penumbra surface and caps (unselected lights)
  bool outerShellOnly = false;
};

/**
 * The triangles (CW seen from outside, like the map's faces) and lines of a light
 * volume in world space, each vertex with the light's hue and its alpha.
 */
struct LightVolumeMesh
{
  std::vector<gl::VertexTypes::P3C4::Vertex> triangles;
  std::vector<gl::VertexTypes::P3C4::Vertex> lines;
};

/**
 * The visible falloff Fvis(t) = min(F(t), 1) of a light at the falloff parameter t: its
 * math falloff curve, or the legacy stand-in for a light without math. A math light with
 * an inner radius of at least 0.999 is 1 everywhere.
 */
double lightVolumeFalloff(double t, const mdl::MathLightProperties& math);

/**
 * The alpha cap of a light: the given cap for a clamped or legacy light and for every
 * sun, 1 for an unclamped math point or spot light.
 */
double lightVolumeAlphaCap(const mdl::LightProperties& light, double alphaCap);

/**
 * The hue of a light colour: the colour divided by its largest component.
 */
vm::vec3d lightVolumeHue(const vm::vec3d& color);

/**
 * The brightness of a light colour: its largest component, clamped to [0.25, 1].
 */
double lightVolumeBrightness(const vm::vec3d& color);

/**
 * The band means C_j = cap b (Fvis(t_{j-1}) + Fvis(t_j)) / 2 of a light's K = shells
 * nested shells at t_j = j / K, innermost first.
 */
std::vector<double> lightVolumeBandMeans(
  const mdl::LightProperties& light, const LightVolumeStyle& style);

/**
 * The alphas of a point light's K = shells nested shells, innermost first: the peel
 * a_j = (C_j - C_{j+1}) / (1 - C_{j+1}) of the band means with C_{K+1} = 0, clamped to
 * [0, 1], so that a ray that crosses the shells j..K once each composites to C_j. With
 * outerShellOnly, the one alpha C_1 of the outer shell. Scaled by 0.6 in 2D.
 */
std::vector<double> lightVolumeShellAlphas(
  const mdl::LightProperties& light, const LightVolumeStyle& style);

/**
 * Appends the volume of the given light in the given frame to the given mesh: the
 * nested ellipsoid shells, the clip box and the centre cross of a point light; the cone,
 * penumbra surface, caps, frustum edges and outlines of a spot light; the box and the
 * direction arrow of a sun. If the light has light_origin and the entity's origin is
 * given, a line links the entity's origin to the volume's origin.
 */
void appendLightVolumeMesh(
  LightVolumeMesh& mesh,
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin = std::nullopt);

/**
 * Returns the volume of the given light in the given frame, see appendLightVolumeMesh.
 */
LightVolumeMesh buildLightVolumeMesh(
  const mdl::LightProperties& light,
  const mdl::LightFrame& frame,
  const LightVolumeStyle& style,
  const std::optional<vm::vec3d>& entityOrigin = std::nullopt);

} // namespace render
} // namespace tb
