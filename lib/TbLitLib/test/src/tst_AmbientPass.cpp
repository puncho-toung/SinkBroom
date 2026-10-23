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

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    REQUIRE(keys.size() == 11u);
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
}

} // namespace tb::lit
