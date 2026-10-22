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
// This file transcribes, for the tests only, the arithmetic of RBDOOM-3-BFG's shaders
// neo/shaders/builtin/post/histogram.cs.hlsl, exposure.cs.hlsl and tonemapping.ps.hlsl
// (at 285ea4d1), which carry this notice:
//
// Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
// ---- end of engine notice ----

#include "TonemapModel.h"

#include <algorithm>
#include <cmath>

namespace tb::lit
{
namespace
{

// histogram.cs.hlsl:53-54, exposure.cs.hlsl:42-43
constexpr auto FixedPointFracBits = 6u;
constexpr auto FixedPointFracMultiplier = 1u << FixedPointFracBits;

float saturate(const float value)
{
  return std::clamp(value, 0.0f, 1.0f);
}

// histogram.cs.hlsl:46-49, tonemapping.ps.hlsl:59-62
float luminance(const std::array<float, 3>& color)
{
  return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
}

// tonemapping.ps.hlsl:49-57
float acesFilm(const float x)
{
  const auto a = 2.51f;
  const auto b = 0.03f;
  const auto c = 2.43f;
  const auto d = 0.59f;
  const auto e = 0.14f;
  return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

} // namespace

ModelHistogram modelHistogram(
  const std::array<float, 3>& rgb, const EngineToneMappingConstants& constants)
{
  // histogram.cs.hlsl:58-108 for viewSize.x · viewSize.y pixels of one colour
  auto histogram = ModelHistogram{};
  const auto pixels =
    std::uint64_t(constants.viewSize[0]) * std::uint64_t(constants.viewSize[1]);
  const auto biasedLogLuminance =
    std::log2(luminance(rgb)) * constants.logLuminanceScale + constants.logLuminanceBias;
  const auto histogramBin = saturate(biasedLogLuminance) * float(ModelHistogramBins - 1);

  const auto leftBin = std::uint32_t(std::floor(histogramBin));
  const auto rightBin = leftBin + 1;

  const auto rightWeight = std::uint32_t(
    (histogramBin - std::floor(histogramBin)) * float(FixedPointFracMultiplier));
  const auto leftWeight = FixedPointFracMultiplier - rightWeight;

  if (leftWeight != 0 && leftBin < ModelHistogramBins)
  {
    histogram[leftBin] += std::uint32_t(pixels * leftWeight);
  }
  if (rightWeight != 0 && rightBin < ModelHistogramBins)
  {
    histogram[rightBin] += std::uint32_t(pixels * rightWeight);
  }
  return histogram;
}

float modelExposure(
  const ModelHistogram& histogram,
  const float oldExposure,
  const EngineToneMappingConstants& constants)
{
  // exposure.cs.hlsl:45-101
  auto cdf = 0.0f;
  for (std::size_t i = 0; i < ModelHistogramBins; ++i)
  {
    cdf += float(histogram[i]) / float(FixedPointFracMultiplier);
  }

  const auto lowCdf = cdf * constants.histogramLowPercentile;
  const auto highCdf = cdf * constants.histogramHighPercentile;

  auto weightSum = 0.0f;
  auto binSum = 0.0f;
  cdf = 0.0f;

  for (std::size_t i = 0; i < ModelHistogramBins; ++i)
  {
    const auto binValue = float(histogram[i]) / float(FixedPointFracMultiplier);

    if (lowCdf <= cdf + binValue && cdf <= highCdf)
    {
      const auto histogramBinLuminance = std::exp2(
        (float(i) / float(ModelHistogramBins)) * constants.logLuminanceScale
        + constants.logLuminanceBias);

      weightSum += histogramBinLuminance * binValue;
      binSum += binValue;
    }

    cdf += binValue;
  }

  auto targetExposure = (binSum > 0.0f) ? (weightSum / binSum) : 0.0f;

  targetExposure = std::clamp(
    targetExposure, constants.minAdaptedLuminance, constants.maxAdaptedLuminance);

  const auto diff = oldExposure - targetExposure;

  const auto adaptationSpeed =
    (diff < 0.0f) ? constants.eyeAdaptationSpeedUp : constants.eyeAdaptationSpeedDown;

  if (adaptationSpeed > 0.0f)
  {
    targetExposure += diff * std::exp2(-constants.frameTime * adaptationSpeed);
  }

  return targetExposure;
}

std::array<float, 3> modelTonemap(
  const std::array<float, 3>& rgb,
  float adaptedLuminance,
  const EngineToneMappingConstants& constants)
{
  // tonemapping.ps.hlsl:64-83 ConvertToLDR
  auto color = std::array<float, 3>{0.0f, 0.0f, 0.0f};
  const auto srcLuminance = luminance(rgb);
  if (srcLuminance > 0.0f)
  {
    if (adaptedLuminance <= 0.0f)
    {
      adaptedLuminance = constants.minAdaptedLuminance;
    }

    const auto scaledLuminance =
      constants.exposureScale * srcLuminance / adaptedLuminance;
    const auto mappedLuminance =
      (scaledLuminance * (1.0f + scaledLuminance * constants.whitePointInvSquared))
      / (1.0f + scaledLuminance);

    for (std::size_t i = 0; i < 3; ++i)
    {
      color[i] = rgb[i] * (mappedLuminance / srcLuminance);
    }
  }

  // :119-134: no LUT (colorLUTTextureSize.x is 0), so ACESFilm, then the gamma
  const auto hdrGamma = 2.2f;
  const auto gamma = 1.0f / hdrGamma;
  for (auto& channel : color)
  {
    channel = std::pow(acesFilm(channel), gamma);
  }
  return color;
}

std::uint8_t modelUnorm8(const float value)
{
  return std::uint8_t(std::lround(saturate(value) * 255.0f));
}

float modelHalf(const float value)
{
  // a normal half has 11 significant bits; the tests' values are normal halves
  if (value == 0.0f)
  {
    return 0.0f;
  }
  auto exponent = 0;
  const auto mantissa = std::frexp(value, &exponent);
  return std::ldexp(std::nearbyint(std::ldexp(mantissa, 11)), exponent - 11);
}

} // namespace tb::lit
