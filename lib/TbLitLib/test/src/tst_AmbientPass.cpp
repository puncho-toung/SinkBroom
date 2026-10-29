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

// LP4.2a step 5 (C34, C35, P4.19): the ambient pass's decisions without a GPU:
// AmbientPass' stage loop (RenderBackend.cpp:2538-2703), its blend per vertex colour,
// AMBIENT_LIGHTING_IBL's units and DX.5's start list.

#include "LitPasses.h"
#include "LitStages.h"
#include "lit/EngineBuiltins.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

namespace em = engine_material;
namespace gls = engine_gls;

MaterialImageKey imageKey(const std::string& name, const int usage)
{
  auto key = MaterialImageKey{};
  key.name = name;
  key.usage = usage;
  key.filter = engine_image::FilterDefault;
  return key;
}

MaterialStage stageOf(
  const int lighting, const std::optional<MaterialImageKey>& image, const int color = 1)
{
  auto stage = MaterialStage{};
  stage.lighting = lighting;
  stage.conditionRegister = 1;
  stage.colorRegisters = {color, color, color, 1};
  stage.image = image;
  return stage;
}

// registers: 0 holds 0, 1 holds 1, 2 holds 0.5, 3 holds 2
std::vector<float> testRegisters()
{
  return {0.0f, 1.0f, 0.5f, 2.0f};
}

const auto LightWhite = Vec4{3.0f, 3.0f, 3.0f, 1.0f};

// the interactions' loop, for the difference of the first colours
std::vector<InteractionDraw> drawsOf(
  const MaterialEntry& material, const std::vector<float>& registers)
{
  auto draws = std::vector<InteractionDraw>{};
  auto state = InteractionState{};
  engineSurfaceInteractions(
    material,
    registers,
    LightWhite,
    LightWhite,
    false,
    state,
    {},
    [&](const InteractionDraw& draw) { draws.push_back(draw); });
  return draws;
}

} // namespace

TEST_CASE("AmbientPass: the stage loop, the state and the units (LP4.2a, C34)")
{
  const auto registers = testRegisters();
  const auto bump = imageKey("bump", engine_image::UsageBump);
  const auto diffuse = imageKey("diffuse", engine_image::UsageDiffuse);
  const auto specular = imageKey("specular", engine_image::UsageSpecular);
  const auto ambientDraws = [&](const MaterialEntry& material) {
    auto draws = std::vector<InteractionDraw>{};
    engineAmbientSurfaceInteractions(
      material, registers, LightWhite, LightWhite, {}, [&](const InteractionDraw& draw) {
        draws.push_back(draw);
      });
    return draws;
  };

  SECTION("one group: the colour clamp01( register ) × 3, specular 0 without a stage")
  {
    auto material = MaterialEntry{};
    material.stages = {
      stageOf(em::LightingBump, bump), stageOf(em::LightingDiffuse, diffuse, 2)};
    const auto draws = ambientDraws(material);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].diffuseColor == Vec4{1.5f, 1.5f, 1.5f, 1.0f});
    // RenderBackend.cpp:2542: inter.specularColor starts at 0, and no stage sets it
    CHECK(draws[0].specularColor == Vec4{0, 0, 0, 0});
    CHECK(draws[0].specularImage == engineBlackImageKey());
    CHECK(draws[0].vertexColor == em::VertexColorIgnore);
    CHECK(
      ambientStateBits(draws[0].vertexColor) == ambientStateBits(em::VertexColorIgnore));
  }

  SECTION("a bump stage alone draws with diffuse 1 and _blackDiffuse (:2541)")
  {
    // unlike the interactions, whose diffuse colour starts at 0 (:2150)
    auto material = MaterialEntry{};
    material.stages = {stageOf(em::LightingBump, bump)};
    const auto draws = ambientDraws(material);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].diffuseImage == engineBlackDiffuseImageKey());
    CHECK(draws[0].diffuseColor == Vec4{1, 1, 1, 1});
    CHECK(drawsOf(material, registers).empty());
  }

  SECTION("no lit stage: no bump, no draw (no implicit _flat, Material.cpp:2491-2503)")
  {
    auto blendOnly = MaterialEntry{};
    blendOnly.stages = {stageOf(em::LightingAmbient, imageKey("add", 2))};
    CHECK(ambientDraws(blendOnly).empty());
  }

  SECTION("two groups, the second with vertexColor: overwrite, then add (A04, P4.19)")
  {
    auto material = MaterialEntry{};
    auto second = stageOf(em::LightingDiffuse, imageKey("d2", 1));
    second.vertexColor = em::VertexColorModulate;
    material.stages = {
      stageOf(em::LightingBump, bump),
      stageOf(em::LightingDiffuse, diffuse),
      stageOf(em::LightingBump, imageKey("bump2", 3)),
      second};
    const auto draws = ambientDraws(material);
    REQUIRE(draws.size() == 2u);
    CHECK(draws[0].vertexColor == em::VertexColorIgnore);
    CHECK(draws[1].vertexColor == em::VertexColorModulate);
    CHECK(
      ambientStateBits(draws[0].vertexColor)
      == (gls::SrcBlendOne | gls::DstBlendZero | gls::DepthMask | gls::DepthFuncEqual | gls::StencilFuncAlways));
    CHECK(
      ambientStateBits(draws[1].vertexColor)
      == (gls::SrcBlendOne | gls::DstBlendOne | gls::DepthMask | gls::DepthFuncEqual | gls::StencilFuncAlways));
    // a third group of a bump stage alone blends by the second's flag, kept across it
    // (RenderBackend.cpp:2592-2621: a bump stage resets the images, not vertexColor nor
    // the colours), with _blackDiffuse and the second group's colour
    auto third = material;
    third.stages.push_back(stageOf(em::LightingBump, imageKey("bump3", 3)));
    const auto three = ambientDraws(third);
    REQUIRE(three.size() == 3u);
    CHECK(three[2].vertexColor == em::VertexColorModulate);
    CHECK(three[2].diffuseImage == engineBlackDiffuseImageKey());
    CHECK(three[2].diffuseColor == Vec4{3, 3, 3, 1});
    // a specular stage sets the flag of its own (:2682)
    auto fourth = third;
    fourth.stages.push_back(stageOf(em::LightingSpecular, specular));
    const auto four = ambientDraws(fourth);
    REQUIRE(four.size() == 3u);
    CHECK(four[2].vertexColor == em::VertexColorIgnore);
    CHECK(four[2].specularColor == Vec4{3, 3, 3, 1});
    // the state bits through the copied GetRenderState: ONE / ONE adds the colour, the
    // alpha stays One / Zero (SC210), depth EQUAL without a write
    const auto renderState = [](const std::uint64_t bits) {
      // GetOrCreatePipeline's defaults (PipelineCache.cpp:78-91)
      auto state = nvrhi::RenderState{};
      state.rasterState.enableScissor();
      state.depthStencilState.enableDepthTest().enableDepthWrite();
      for (auto& target : state.blendState.targets)
      {
        target.enableBlend();
      }
      engineRenderState(bits, 0, 0.0f, state);
      return state;
    };
    const auto add = renderState(ambientStateBits(em::VertexColorModulate));
    const auto& target = add.blendState.targets[0];
    CHECK(target.blendEnable);
    CHECK(target.srcBlend == nvrhi::BlendFactor::One);
    CHECK(target.destBlend == nvrhi::BlendFactor::One);
    CHECK(target.srcBlendAlpha == nvrhi::BlendFactor::One);
    CHECK(target.destBlendAlpha == nvrhi::BlendFactor::Zero);
    CHECK_FALSE(add.depthStencilState.depthWriteEnable);
    CHECK(add.depthStencilState.depthFunc == nvrhi::ComparisonFunc::Equal);
    const auto overwrite = renderState(ambientStateBits(em::VertexColorIgnore));
    CHECK(overwrite.blendState.targets[0].destBlend == nvrhi::BlendFactor::Zero);
    CHECK(overwrite.depthStencilState.depthFunc == nvrhi::ComparisonFunc::Equal);
  }

  SECTION("the units and the binding layout agree item for item (nvrhi rule 4)")
  {
    const auto units = ambientUnitBindings();
    REQUIRE(units.size() == 12u);
    const auto layout =
      *engineBindingLayoutDescs(EngineBindingLayout::AmbientLightingIbl, false);
    auto bySet = std::map<std::uint32_t, std::vector<std::uint32_t>>{};
    for (const auto& unit : units)
    {
      CHECK(unit.unit == std::size_t(unit.slot));
      CHECK(unit.unit < LitTextureUnits);
      bySet[unit.set].push_back(unit.slot);
    }
    for (const auto set : {1u, 2u})
    {
      CAPTURE(set);
      auto slots = std::vector<std::uint32_t>{};
      for (const auto& item : layout[set].bindings)
      {
        slots.push_back(item.slot);
      }
      CHECK(bySet[set] == slots);
    }
    CHECK(bySet[1] == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(bySet[2] == std::vector<std::uint32_t>{3, 4, 7, 8, 9, 10, 12, 13, 14});
  }

  SECTION("DX.5's start list gains the ambient pass's four pipelines")
  {
    const auto keys = litStartPipelineKeys(nvrhi::Format::D24S8);
    // LP6.3 (C67 (c)): 11 until LP6.2, then the grid's 4 and the shadow atlas's 12
    REQUIRE(keys.size() == 27u);
    auto ambient = std::vector<std::pair<int, std::uint64_t>>{};
    for (const auto& key : keys)
    {
      if (
        key.program == EngineBuiltinAmbientLightingIbl
        || key.program == EngineBuiltinAmbientLightingIblPbr)
      {
        CHECK(key.framebuffer.color == nvrhi::Format::RGBA16_FLOAT);
        CHECK(key.depthBias == 0);
        CHECK(key.slopeBias == 0.0f);
        ambient.emplace_back(key.program, key.state);
      }
    }
    CHECK(
      ambient
      == std::vector<std::pair<int, std::uint64_t>>{
        {4, ambientStateBits(em::VertexColorIgnore)},
        {4, ambientStateBits(em::VertexColorModulate)},
        {6, ambientStateBits(em::VertexColorIgnore)},
        {6, ambientStateBits(em::VertexColorModulate)}});
  }

  SECTION("LP6.3: the start list holds the grid path's and the shadow atlas's pipelines")
  {
    // C67 (c), P8.12: the (program, state) pairs a start view's first frames draw with
    // beside DX.5's: the light-grid programs in the probe path's two states, and the
    // shadow-atlas interactions of the three light types, legacy and PBR, EQUAL for the
    // local and global lists and LESS for the translucent one; no skinned row
    const auto keys = litStartPipelineKeys(nvrhi::Format::D24S8);
    auto added = std::vector<std::pair<int, std::uint64_t>>{};
    for (const auto& key : keys)
    {
      CHECK(key.recipe == PipelineRecipe::Draw);
      CHECK(key.framebuffer.color == nvrhi::Format::RGBA16_FLOAT);
      CHECK(key.framebuffer.depth == nvrhi::Format::D24S8);
      CHECK(key.depthBias == 0);
      CHECK(key.slopeBias == 0.0f);
      if (
        key.program == EngineBuiltinAmbientLightgridIbl
        || key.program == EngineBuiltinAmbientLightgridIblPbr
        || engineShadowAtlasProgram(key.program))
      {
        added.emplace_back(key.program, key.state);
      }
    }
    const auto equal = interactionStateBits(gls::DepthFuncEqual);
    const auto less = interactionStateBits(gls::DepthFuncLess);
    CHECK(
      added
      == std::vector<std::pair<int, std::uint64_t>>{
        {8, ambientStateBits(em::VertexColorIgnore)},
        {8, ambientStateBits(em::VertexColorModulate)},
        {10, ambientStateBits(em::VertexColorIgnore)},
        {10, ambientStateBits(em::VertexColorModulate)},
        {39, equal},
        {39, less},
        {41, equal},
        {41, less},
        {43, equal},
        {43, less},
        {45, equal},
        {45, less},
        {47, equal},
        {47, less},
        {49, equal},
        {49, less}});
    // every key once; the builtins' ids are the rows named (their skinned rows follow)
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
      for (auto k = i + 1; k < keys.size(); ++k)
      {
        CHECK_FALSE(keys[i] == keys[k]);
      }
    }
    CHECK(EngineBuiltinAmbientLightgridIbl == 8);
    CHECK(EngineBuiltinAmbientLightgridIblPbr == 10);
    CHECK(EngineBuiltinInteractionShadowAtlasSpot == 39);
    CHECK(EngineBuiltinPbrInteractionShadowAtlasParallel == 49);
  }
}

/**
 * LP6.6 (C73, SC818, SC821, P8.20, K57): the progressive TAA's pure parts: the MSAA
 * jitter table by history index, the resolve's constants and dispatch, and the jitter as
 * a clip-space offset, which equals the jittered projection's MVP.
 */
TEST_CASE(
  "AmbientPass: the progressive TAA's jitter, constants and dispatch (LP6.6, C73)")
{
  using Offset = std::array<float, 2>;
  // TemporalAntiAliasingPass.cpp:281-287: offsets[( index - 1 ) % 8]; 0 plain
  CHECK(engineTaaPixelOffset(0) == Offset{0.0f, 0.0f});
  CHECK(engineTaaPixelOffset(1) == Offset{0.0625f, -0.1875f});
  CHECK(engineTaaPixelOffset(2) == Offset{-0.0625f, 0.1875f});
  CHECK(engineTaaPixelOffset(4) == Offset{-0.1875f, -0.3125f});
  CHECK(engineTaaPixelOffset(8) == Offset{0.4375f, -0.4375f});
  CHECK(engineTaaPixelOffset(9) == engineTaaPixelOffset(1));
  CHECK(engineTaaPixelOffset(16) == engineTaaPixelOffset(8));
  CHECK(EngineTaa.historyFrames == 16u);
  CHECK(EngineTaa.newFrameWeight == 0.1f);

  // TemporalResolve's constants over a 160 x 120 target
  const auto first = engineTaaConstants(160, 120, engineTaaPixelOffset(1), true);
  CHECK(first.reprojectionMatrix == std::array<float, 16>{});
  CHECK(first.inputViewOrigin == Offset{0.0f, 0.0f});
  CHECK(first.inputViewSize == Offset{160.0f, 120.0f});
  CHECK(first.outputViewOrigin == Offset{0.0f, 0.0f});
  CHECK(first.outputViewSize == Offset{160.0f, 120.0f});
  CHECK(first.inputPixelOffset == engineTaaPixelOffset(1));
  CHECK(first.outputTextureSizeInv == Offset{1.0f / 160.0f, 1.0f / 120.0f});
  CHECK(first.inputOverOutputViewSize == Offset{1.0f, 1.0f});
  CHECK(first.outputOverInputViewSize == Offset{1.0f, 1.0f});
  CHECK(first.clampingFactor == 1.0f);
  CHECK(first.newFrameWeight == 1.0f);
  CHECK(first.pqC == 10000.0f);
  CHECK(first.invPqC == 1.0f / 10000.0f);
  CHECK(first.stencilMask == 0u);
  const auto later = engineTaaConstants(160, 120, engineTaaPixelOffset(5), false);
  CHECK(later.newFrameWeight == 0.1f);
  CHECK(later.inputPixelOffset == engineTaaPixelOffset(5));
  // ( w + 15 ) / 16 x ( h + 15 ) / 16 groups
  CHECK(engineTaaDispatch(160, 120) == std::array<std::uint32_t, 2>{10, 8});
  CHECK(engineTaaDispatch(1920, 1080) == std::array<std::uint32_t, 2>{120, 68});
  CHECK(engineTaaDispatch(200, 150) == std::array<std::uint32_t, 2>{13, 10});

  // the jittered view: R_SetupProjectionMatrix's P[2·4+0] and P[2·4+1], the rest the
  // view's; with no offset the view itself
  auto camera = CameraState{};
  camera.position = vm::vec3f{10, 20, 30};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  const auto view = litViewOf(camera, 160, 120);
  const auto offset = engineTaaPixelOffset(3);
  const auto jittered = engineJitteredView(view, offset);
  CHECK(jittered.projectionMatrix[2 * 4 + 0] == -2.0f * offset[0] / 160.0f);
  CHECK(jittered.projectionMatrix[2 * 4 + 1] == -2.0f * offset[1] / 120.0f);
  for (std::size_t i = 0; i < 16; ++i)
  {
    if (i != 2 * 4 + 0 && i != 2 * 4 + 1)
    {
      CHECK(jittered.projectionMatrix[i] == view.projectionMatrix[i]);
    }
  }
  CHECK(jittered.worldModelViewMatrix == view.worldModelViewMatrix);
  CHECK(jittered.viewOrigin == view.viewOrigin);
  CHECK(engineJitteredView(view, {0.0f, 0.0f}).worldMvp == view.worldMvp);

  // the clip-space form at parm time equals the jittered projection's MVP, and moves a
  // projected point by the offset in pixels (NDC by 2 j / size)
  const auto parmTime = engineJitteredClipMatrix(view.worldMvp, offset, 160, 120);
  auto worst = 0.0f;
  for (std::size_t i = 0; i < 16; ++i)
  {
    worst = std::max(worst, std::abs(parmTime[i] - jittered.worldMvp[i]));
  }
  CHECK(worst <= 1e-6f);
  CHECK(engineJitteredClipMatrix(view.worldMvp, {0.0f, 0.0f}, 160, 120) == view.worldMvp);
  const auto clipOf = [](const EngineMat4& m, const std::array<float, 4>& p) {
    auto out = std::array<float, 4>{};
    for (std::size_t r = 0; r < 4; ++r)
    {
      out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2]
               + m[r * 4 + 3] * p[3];
    }
    return out;
  };
  const auto point = std::array<float, 4>{110.0f, 35.0f, 12.0f, 1.0f};
  const auto plain = clipOf(view.worldMvp, point);
  const auto shifted = clipOf(parmTime, point);
  REQUIRE(plain[3] > 0.0f);
  CHECK(shifted[3] == plain[3]);
  CHECK(
    (shifted[0] / shifted[3] - plain[0] / plain[3]) * 80.0f
    == Catch::Approx(offset[0]).margin(1e-4));
  CHECK(
    (shifted[1] / shifted[3] - plain[1] / plain[3]) * 60.0f
    == Catch::Approx(offset[1]).margin(1e-4));
  CHECK(shifted[2] == plain[2]);

  // SSAO's constants on an accumulated frame: the jitter and the jittered inverse
  const auto ssao = engineSsaoConstants(jittered, 160, 120, offset);
  CHECK(ssao.pixelOffset == offset);
  CHECK(engineSsaoConstants(view, 160, 120).pixelOffset == Offset{0.0f, 0.0f});
  CHECK(ssao.matClipToView != engineSsaoConstants(view, 160, 120).matClipToView);
}

} // namespace tb::lit
