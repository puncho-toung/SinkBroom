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

// ---- engine notice ----
// TonemapModel.cpp transcribes, for the tests only, the arithmetic of RBDOOM-3-BFG's
// shaders neo/shaders/builtin/post/histogram.cs.hlsl, exposure.cs.hlsl and
// tonemapping.ps.hlsl, which are NVIDIA Donut code under the MIT licence (its notice is
// in TonemapModel.cpp).
// ---- end of engine notice ----

#pragma once

#include "TonemapPasses.h"

#include <array>
#include <cstdint>
#include <vector>

namespace tb::lit
{

// HISTOGRAM_BINS of the builtins rows 111, 113 and 115
inline constexpr auto ModelHistogramBins = std::size_t{256};
using ModelHistogram = std::array<std::uint32_t, ModelHistogramBins>;

/**
 * histogram.cs.hlsl over a source whose every pixel is rgb: each of the viewSize.x ·
 * viewSize.y pixels adds its fixed-point weights (6 fraction bits) to the two bins
 * around saturate( log2( luminance ) · scale + bias ) · 255.
 */
ModelHistogram modelHistogram(
  const std::array<float, 3>& rgb, const EngineToneMappingConstants& constants);

/**
 * U3.4: histogram.cs.hlsl over an image: every pixel of rgba (width x height, four floats
 * a pixel, the RGBA16F target's values) inside viewOrigin + viewSize adds its weights to
 * the two bins around its own luminance.
 */
ModelHistogram modelHistogramOfImage(
  const std::vector<float>& rgba,
  std::uint32_t width,
  std::uint32_t height,
  const EngineToneMappingConstants& constants);

/**
 * exposure.cs.hlsl: the weighted mean of the bins' luminances exp2( i / 256 · scale +
 * bias ) between the low and high percentiles, clamped to the adapted bounds, adapted
 * from oldExposure by exp2( -frameTime · speed ).
 */
float modelExposure(
  const ModelHistogram& histogram,
  float oldExposure,
  const EngineToneMappingConstants& constants);

/**
 * tonemapping.ps.hlsl without a colour LUT: ConvertToLDR (exposureScale · L / adapted,
 * the extended Reinhard of whitePointInvSquared), ACESFilm, then pow( 1 / 2.2 ); 0..1.
 */
std::array<float, 3> modelTonemap(
  const std::array<float, 3>& rgb,
  float adaptedLuminance,
  const EngineToneMappingConstants& constants);

// an RGBA8_UNORM render target's conversion: round( saturate( v ) · 255 )
std::uint8_t modelUnorm8(float value);

// an RGBA16_FLOAT render target's storage of a float (round to nearest even), as a float
float modelHalf(float value);

} // namespace tb::lit
