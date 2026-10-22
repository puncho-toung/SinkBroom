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

#include "TonemapModel.h"
#include "TonemapPasses.h"
#include "lit/EngineTables.h"

#include <cmath>
#include <cstddef>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::uint32_t offsetOfMember(
  const EngineConstantBlock& block, const std::string_view name)
{
  for (const auto& member : block.members)
  {
    if (member.name == name)
    {
      return member.offset;
    }
  }
  return ~0u;
}

} // namespace

TEST_CASE("TonemapPasses: the constant blocks' layouts (SC261, C27)")
{
  // the C++ structs the passes write are the tables' blocks, which the reflection test
  // holds equal to the blobs' (P2.19)
  const auto tonemap = engineToneMappingBlock();
  CHECK(sizeof(EngineToneMappingConstants) == tonemap.size);
  using T = EngineToneMappingConstants;
  CHECK(offsetof(T, viewOrigin) == offsetOfMember(tonemap, "viewOrigin"));
  CHECK(offsetof(T, viewSize) == offsetOfMember(tonemap, "viewSize"));
  CHECK(offsetof(T, logLuminanceScale) == offsetOfMember(tonemap, "logLuminanceScale"));
  CHECK(offsetof(T, logLuminanceBias) == offsetOfMember(tonemap, "logLuminanceBias"));
  CHECK(
    offsetof(T, histogramLowPercentile)
    == offsetOfMember(tonemap, "histogramLowPercentile"));
  CHECK(
    offsetof(T, histogramHighPercentile)
    == offsetOfMember(tonemap, "histogramHighPercentile"));
  CHECK(
    offsetof(T, eyeAdaptationSpeedUp) == offsetOfMember(tonemap, "eyeAdaptationSpeedUp"));
  CHECK(
    offsetof(T, eyeAdaptationSpeedDown)
    == offsetOfMember(tonemap, "eyeAdaptationSpeedDown"));
  CHECK(
    offsetof(T, minAdaptedLuminance) == offsetOfMember(tonemap, "minAdaptedLuminance"));
  CHECK(
    offsetof(T, maxAdaptedLuminance) == offsetOfMember(tonemap, "maxAdaptedLuminance"));
  CHECK(offsetof(T, frameTime) == offsetOfMember(tonemap, "frameTime"));
  CHECK(offsetof(T, exposureScale) == offsetOfMember(tonemap, "exposureScale"));
  CHECK(
    offsetof(T, whitePointInvSquared) == offsetOfMember(tonemap, "whitePointInvSquared"));
  CHECK(offsetof(T, sourceSlice) == offsetOfMember(tonemap, "sourceSlice"));
  CHECK(
    offsetof(T, colorLutTextureSize) == offsetOfMember(tonemap, "colorLUTTextureSize"));
  CHECK(
    offsetof(T, colorLutTextureSizeInv)
    == offsetOfMember(tonemap, "colorLUTTextureSizeInv"));

  const auto blit = engineBlitBlock();
  CHECK(sizeof(EngineBlitConstants) == blit.size);
  using B = EngineBlitConstants;
  CHECK(offsetof(B, sourceOrigin) == offsetOfMember(blit, "sourceOrigin"));
  CHECK(offsetof(B, sourceSize) == offsetOfMember(blit, "sourceSize"));
  CHECK(offsetof(B, targetOrigin) == offsetOfMember(blit, "targetOrigin"));
  CHECK(offsetof(B, targetSize) == offsetOfMember(blit, "targetSize"));
  CHECK(offsetof(B, sharpenFactor) == offsetOfMember(blit, "sharpenFactor"));
}

TEST_CASE("TonemapPasses: the histogram's x2 viewport and its constants (SC260, D20)")
{
  // AddFrameToHistogram's viewport leaves out the last column and row
  const auto viewport = engineHistogramViewport(160, 120);
  CHECK(viewport.minX == 0.0f);
  CHECK(viewport.maxX == 159.0f);
  CHECK(viewport.minY == 0.0f);
  CHECK(viewport.maxY == 119.0f);
  CHECK(viewport.minZ == 0.0f);
  CHECK(viewport.maxZ == 1.0f);

  const auto constants = engineHistogramConstants(160, 120);
  CHECK(constants.viewOrigin == std::array<std::int32_t, 2>{0, 0});
  CHECK(constants.viewSize == std::array<std::int32_t, 2>{159, 119});
  CHECK(engineHistogramGroups(constants) == std::array<std::uint32_t, 2>{10, 8});
  // D20: 1 / (r_hdrMinLuminance - r_hdrMaxLuminance), negative, and -min · scale, in
  // float as the engine computes them; the encode range is not the exposure's decode
  // range (SC286): mirrored, not fixed
  const auto scale = 1.0f / (0.02f - 0.5f);
  CHECK(constants.logLuminanceScale == scale);
  CHECK(constants.logLuminanceBias == -0.02f * scale);
  CHECK(constants.logLuminanceScale < 0.0f);
  CHECK(constants.sourceSlice == 0u);
  // the other members are zero, as ToneMappingConstants toneMappingConstants = {}
  CHECK(constants.minAdaptedLuminance == 0.0f);
  CHECK(constants.frameTime == 0.0f);

  // a 1920 × 1080 view: 1919 × 1079 pixels in 120 × 68 groups
  const auto hd = engineHistogramConstants(1920, 1080);
  CHECK(hd.viewSize == std::array<std::int32_t, 2>{1919, 1079});
  CHECK(engineHistogramGroups(hd) == std::array<std::uint32_t, 2>{120, 68});
}

TEST_CASE("TonemapPasses: the exposure's constants and the steady state (D70, K24)")
{
  const auto constants = engineExposureConstants();
  CHECK(constants.logLuminanceScale == 14.0f);
  CHECK(constants.logLuminanceBias == -10.0f);
  CHECK(constants.histogramLowPercentile == 0.8f);
  CHECK(constants.histogramHighPercentile == 0.95f);
  CHECK(constants.eyeAdaptationSpeedUp == 1.0f);
  CHECK(constants.eyeAdaptationSpeedDown == 0.5f);
  CHECK(constants.minAdaptedLuminance == 0.02f);
  CHECK(constants.maxAdaptedLuminance == 0.5f);
  CHECK(constants.frameTime == SteadyStateFrameTime);
  CHECK(constants.viewSize == std::array<std::int32_t, 2>{0, 0});

  // D70: both adaptation terms underflow, so a frame's exposure is its clamped target
  // whatever the last frame's was
  CHECK(std::exp2(-constants.frameTime * constants.eyeAdaptationSpeedUp) == 0.0f);
  CHECK(std::exp2(-constants.frameTime * constants.eyeAdaptationSpeedDown) == 0.0f);

  // the percentiles are clamped as ComputeExposure clamps them
  auto odd = EngineTonemap;
  odd.histogramLowPercentile = 1.5f;
  odd.histogramHighPercentile = 0.5f;
  const auto clamped = engineExposureConstants(odd);
  CHECK(clamped.histogramLowPercentile == 0.99f);
  CHECK(clamped.histogramHighPercentile == 0.99f);
}

TEST_CASE("TonemapPasses: the tonemap's constants and viewport (SC260)")
{
  const auto constants = engineTonemapConstants();
  CHECK(constants.exposureScale == std::exp2(0.5f));
  CHECK(constants.whitePointInvSquared == 1.0f / 9.0f);
  CHECK(constants.minAdaptedLuminance == 0.02f);
  CHECK(constants.maxAdaptedLuminance == 0.5f);
  CHECK(constants.sourceSlice == 0u);
  // no colour LUT: ACESFilm
  CHECK(constants.colorLutTextureSize == std::array<float, 2>{0.0f, 0.0f});
  CHECK(constants.colorLutTextureSizeInv == std::array<float, 2>{0.0f, 0.0f});
  CHECK(constants.logLuminanceScale == 0.0f);
  CHECK(constants.frameTime == 0.0f);

  const auto viewport = engineTonemapViewport(160, 120);
  CHECK(viewport.minX == 0.0f);
  CHECK(viewport.maxX == 160.0f);
  CHECK(viewport.minY == 0.0f);
  CHECK(viewport.maxY == 120.0f);
}

TEST_CASE("TonemapPasses: the blit's constants and viewport (SC201, SC203)")
{
  const auto constants = engineBlitConstants();
  CHECK(constants.sourceOrigin == std::array<float, 2>{0.0f, 0.0f});
  CHECK(constants.sourceSize == std::array<float, 2>{1.0f, 1.0f});
  CHECK(constants.targetOrigin == std::array<float, 2>{0.0f, 0.0f});
  CHECK(constants.targetSize == std::array<float, 2>{1.0f, 1.0f});
  CHECK(constants.sharpenFactor == 0.0f);

  const auto viewport = engineBlitViewport(800, 600);
  CHECK(viewport.minX == 0.0f);
  CHECK(viewport.maxX == 800.0f);
  CHECK(viewport.maxY == 600.0f);
  CHECK(viewport.maxZ == 1.0f);
  CHECK(nvrhi::Rect{viewport} == nvrhi::Rect{0, 800, 0, 600});
}

TEST_CASE("TonemapPasses: the shaders' arithmetic on one colour (the GPU test's model)")
{
  const auto histogram = engineHistogramConstants(160, 120);
  const auto exposure = engineExposureConstants();
  const auto tonemap = engineTonemapConstants();
  const auto pixels = std::uint32_t(159 * 119);

  // bright: log2( L ) · scale + bias is below 0, so every pixel weighs 64 in bin 0,
  // whose luminance exp2( -10 ) is clamped up to minAdaptedLuminance
  const auto bright = modelHistogram({2.0f, 2.0f, 2.0f}, histogram);
  CHECK(bright[0] == pixels * 64);
  CHECK(modelExposure(bright, 0.0f, exposure) == 0.02f);

  // dark: above 1, saturated to bin 255, exp2( 255 / 256 · 14 - 10 ) clamped down to
  // maxAdaptedLuminance
  const auto dark = modelHistogram({0.25f, 0.25f, 0.25f}, histogram);
  CHECK(dark[255] == pixels * 64);
  CHECK(modelExposure(dark, 0.0f, exposure) == 0.5f);

  // between: L 0.875 lands at bin 112.97 (weights 3 and 61); bin 112 holds less than
  // the low percentile, so the mean is bin 113's luminance alone
  const auto mid = modelHistogram({0.875f, 0.875f, 0.875f}, histogram);
  CHECK(mid[112] == pixels * 3);
  CHECK(mid[113] == pixels * 61);
  const auto adapted = modelExposure(mid, 0.0f, exposure);
  CHECK(adapted == std::exp2((113.0f / 256.0f) * 14.0f - 10.0f));
  CHECK(adapted > 0.02f);
  CHECK(adapted < 0.5f);

  // D70: the last frame's exposure makes no difference
  CHECK(modelExposure(mid, 0.4f, exposure) == adapted);
  CHECK(modelExposure(mid, adapted, exposure) == adapted);

  // a black pixel tonemaps to 0, and so does every channel at 0
  CHECK(modelTonemap({0.0f, 0.0f, 0.0f}, 0.5f, tonemap)[0] == 0.0f);
  const auto dim = modelTonemap({0.25f, 0.25f, 0.25f}, 0.5f, tonemap);
  CHECK(modelUnorm8(dim[0]) == 199);
  CHECK(dim[0] == dim[1]);
  CHECK(modelHalf(0.875f) == 0.875f);
  CHECK(modelHalf(0.1f) == 0.0999755859375f);
}

} // namespace tb::lit
