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

#include "LitFrontend.h"
#include "LitMapFixture.h"
#include "LitStages.h"
#include "LitTestUtils.h"
#include "LitView.h"
#include "RenderParms.h"
#include "ViewFlow.h"
#include "lit/AreaData.h"
#include "lit/BprocModel.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineBuiltins.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/GridTable.h"
#include "lit/ImageCache.h"
#include "lit/ImageOverride.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/LightRecord.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

namespace em = engine_material;
namespace gls = engine_gls;

// ----- helpers -----

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

// registers: 0 holds 0, 1 holds 1, 2 holds 0.5, 3 holds 2, 4 holds 45.5, 5 holds -41.25
std::vector<float> testRegisters()
{
  return {0.0f, 1.0f, 0.5f, 2.0f, 45.5f, -41.25f};
}

const auto LightWhite = Vec4{3.0f, 3.0f, 3.0f, 1.0f};

std::vector<InteractionDraw> drawsOf(
  const MaterialEntry& material,
  const std::vector<float>& registers,
  const bool noSpecular = false,
  const ImageUsage& usage = {},
  InteractionState* carried = nullptr)
{
  auto draws = std::vector<InteractionDraw>{};
  auto own = InteractionState{};
  engineSurfaceInteractions(
    material,
    registers,
    LightWhite,
    noSpecular ? Vec4{0, 0, 0, 0} : LightWhite,
    noSpecular,
    carried ? *carried : own,
    usage,
    [&](const InteractionDraw& draw) { draws.push_back(draw); });
  return draws;
}

} // namespace

TEST_CASE("LitPasses: RenderParms mirror SetUniformValue and SelectUniforms (SC241)")
{
  auto parms = RenderParms{};

  SECTION("ZeroUniforms clears every value and marks every layout")
  {
    parms.set(RenderParm::Color, {1, 2, 3, 4});
    parms.clearChanged(EngineBindingLayout::ConstantBufferOnly);
    parms.zero();
    CHECK(parms.value(RenderParm::Color) == RenderParms::Vec4{0, 0, 0, 0});
    for (auto layout = 0; layout < int(EngineBindingLayout::Num); ++layout)
    {
      CHECK(parms.changed(EngineBindingLayout(layout)));
    }
  }

  SECTION("a changed parm marks the layouts whose set holds it, an equal one none")
  {
    for (const auto layout :
         {EngineBindingLayout::ConstantBufferOnly,
          EngineBindingLayout::Texture,
          EngineBindingLayout::DrawInteraction})
    {
      parms.clearChanged(layout);
    }
    // USER0 is in set 10 only
    parms.set(RenderParm::User0, {1, 0, 0, 0});
    CHECK(parms.changed(EngineBindingLayout::DrawInteraction));
    CHECK_FALSE(parms.changed(EngineBindingLayout::ConstantBufferOnly));
    CHECK_FALSE(parms.changed(EngineBindingLayout::Texture));
    parms.clearChanged(EngineBindingLayout::DrawInteraction);
    parms.set(RenderParm::User0, {1, 0, 0, 0});
    CHECK_FALSE(parms.changed(EngineBindingLayout::DrawInteraction));
    // the MVP rows are in sets 0, 4 and 10
    parms.set(RenderParm::MvpMatrixY, {0, 1, 0, 0});
    CHECK(parms.changed(EngineBindingLayout::DrawInteraction));
    CHECK(parms.changed(EngineBindingLayout::ConstantBufferOnly));
    CHECK(parms.changed(EngineBindingLayout::Texture));
    // LP4.2a (C35): GLOBALEYEPOS is in set 9 alone (AMBIENT_LIGHTING_IBL and its skinned
    // form); OVERBRIGHT is in none of the mirrored sets
    for (const auto layout :
         {EngineBindingLayout::ConstantBufferOnly,
          EngineBindingLayout::Texture,
          EngineBindingLayout::DrawInteraction,
          EngineBindingLayout::AmbientLightingIbl})
    {
      parms.clearChanged(layout);
    }
    parms.set(RenderParm::GlobalEyePos, {1, 2, 3, 1});
    CHECK_FALSE(parms.changed(EngineBindingLayout::DrawInteraction));
    CHECK(parms.changed(EngineBindingLayout::AmbientLightingIbl));
    CHECK(
      engineLayoutsOfParm(RenderParm::GlobalEyePos)
      == ((std::uint64_t{1} << int(EngineBindingLayout::AmbientLightingIbl)) | (std::uint64_t{1} << int(EngineBindingLayout::AmbientLightingIblSkinned))));
    parms.clearChanged(EngineBindingLayout::AmbientLightingIbl);
    parms.set(RenderParm::Overbright, {1, 1, 1, 1});
    CHECK_FALSE(parms.changed(EngineBindingLayout::AmbientLightingIbl));
    CHECK_FALSE(parms.changed(EngineBindingLayout::DrawInteraction));
    CHECK(engineLayoutsOfParm(RenderParm::Overbright) == 0u);
  }

  SECTION("the subset order is the set's table order")
  {
    // every parm a value of its own index
    for (auto parm = 0; parm < int(RenderParm::Total); ++parm)
    {
      const auto v = float(parm);
      parms.set(RenderParm(parm), {v, v + 0.25f, v + 0.5f, v + 0.75f});
    }
    for (const auto& [layout, set] : std::vector<std::pair<EngineBindingLayout, int>>{
           {EngineBindingLayout::ConstantBufferOnly, 0},
           {EngineBindingLayout::Texture, 4},
           {EngineBindingLayout::AmbientLightingIbl, 9},
           {EngineBindingLayout::DrawInteraction, 10}})
    {
      CAPTURE(set);
      auto out = std::array<float, MaxParmSetFloat4s * 4>{};
      const auto members = engineParmSetMembers(set);
      REQUIRE(parms.select(layout, out) == members.size() * 4);
      CHECK(members.size() * 16 == engineParmSetBlock(set).size);
      for (std::size_t i = 0; i < members.size(); ++i)
      {
        const auto v = float(int(members[i].parm));
        CHECK(out[i * 4 + 0] == v);
        CHECK(out[i * 4 + 3] == v + 0.75f);
      }
    }
    // set 10: DiffuseModifier first, User2 last, 400 bytes
    auto out = std::array<float, MaxParmSetFloat4s * 4>{};
    CHECK(parms.select(EngineBindingLayout::DrawInteraction, out) == 100u);
    CHECK(out[0] == float(int(RenderParm::DiffuseModifier)));
    CHECK(out[24 * 4] == float(int(RenderParm::User2)));
    // LP4.2a (C35): set 9: ScreenCorrectionFactor first, the nine SH parms last, 720
    // bytes; SetVertexParms( SH_IRRADIANCE_0, sh, 9 ) writes them in one call and they
    // come back in order
    CHECK(parms.select(EngineBindingLayout::AmbientLightingIbl, out) == 180u);
    CHECK(out[0] == float(int(RenderParm::ScreenCorrectionFactor)));
    CHECK(out[24 * 4] == float(int(RenderParm::AmbientColor)));
    CHECK(out[36 * 4] == float(int(RenderParm::ShIrradiance0)));
    CHECK(out[44 * 4] == float(int(RenderParm::ShIrradiance8)));
    auto sh = std::array<float, 36>{};
    for (std::size_t i = 0; i < sh.size(); ++i)
    {
      sh[i] = 1000.0f + float(i);
    }
    parms.set(RenderParm::ShIrradiance0, sh);
    CHECK(parms.select(EngineBindingLayout::AmbientLightingIbl, out) == 180u);
    for (std::size_t i = 0; i < sh.size(); ++i)
    {
      CHECK(out[36 * 4 + i] == sh[i]);
    }
    CHECK(parms.select(EngineBindingLayout::AmbientLightingIblSkinned, out) == 180u);
    // a layout whose set is not mirrored selects nothing
    CHECK(parms.select(EngineBindingLayout::Gbuffer, out) == 0u);
  }
}

TEST_CASE("LitPasses: the view's matrices and parms (SC201, SC227)")
{
  auto camera = CameraState{};
  camera.position = vm::vec3f{10, 20, 30};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  const auto view = litViewOf(camera, 64, 48);

  CHECK(view.width == 64);
  CHECK(view.height == 48);
  // projectionRenderMatrix is the float[16]'s transpose: element (row, column)
  for (auto row = 0; row < 4; ++row)
  {
    for (auto column = 0; column < 4; ++column)
    {
      CHECK(
        view.projectionRenderMatrix[std::size_t(row * 4 + column)]
        == view.projectionMatrix[std::size_t(column * 4 + row)]);
    }
  }
  CHECK(
    view.worldMvp
    == engineMatrixMultiply(
      view.projectionRenderMatrix, engineMatrixTranspose(view.worldModelViewMatrix)));

  // worldspawn's space: the identity's model view is the world's
  const auto identity = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
  const auto world = litSpaceOf(view, 1, {0, 0, 0}, identity, engineWorldShaderParms());
  CHECK(world.modelViewMatrix == view.worldModelViewMatrix);
  CHECK(world.mvp == view.worldMvp);
  CHECK(world.shaderParms[3] == 1.0f);
  CHECK(world.shaderParms[4] == 0.0f);

  // R_AxisToModelMatrix: the axis rows are the columns, the origin the last column
  const auto turned =
    litSpaceOf(view, 2, {128, 64, 32}, {0, 1, 0, -1, 0, 0, 0, 0, 1}, ShaderParms{});
  CHECK(turned.modelMatrix[0 * 4 + 0] == 0.0f);
  CHECK(turned.modelMatrix[0 * 4 + 1] == 1.0f);
  CHECK(turned.modelMatrix[1 * 4 + 0] == -1.0f);
  CHECK(turned.modelMatrix[3 * 4 + 0] == 128.0f);
  CHECK(turned.modelMatrix[3 * 4 + 2] == 32.0f);
  CHECK(turned.modelMatrix[3 * 4 + 3] == 1.0f);
  // R_GlobalPointToLocal of the origin is 0, of origin + x-axis (0 1 0) is (1 0 0)
  CHECK(
    engineGlobalPointToLocal(turned.modelMatrix, {128, 64, 32}) == EngineVec3{0, 0, 0});
  CHECK(
    engineGlobalPointToLocal(turned.modelMatrix, {128, 65, 32}) == EngineVec3{1, 0, 0});
  // R_GlobalPlaneToLocal: the plane x = 128 is local -y = 0... the plane's d moves
  const auto plane =
    engineGlobalPlaneToLocal(turned.modelMatrix, EnginePlane{1, 0, 0, -128});
  CHECK(plane.a == 0.0f);
  CHECK(plane.b == -1.0f);
  CHECK(plane.d == 0.0f);

  auto parms = RenderParms{};
  setViewParms(parms, view);
  CHECK(parms.value(RenderParm::GlobalEyePos) == RenderParms::Vec4{10, 20, 30, 1});
  CHECK(parms.value(RenderParm::Overbright) == RenderParms::Vec4{1.5f, 1.5f, 1.5f, 1.5f});
  CHECK(
    parms.value(RenderParm::WindowCoord)
    == RenderParms::Vec4{1.0f / 64.0f, 1.0f / 48.0f, 64.0f, 48.0f});
  CHECK(parms.value(RenderParm::PsxDistortions) == RenderParms::Vec4{0, 0, 0, 0});
  for (auto row = 0; row < 4; ++row)
  {
    const auto& value = parms.value(RenderParm(int(RenderParm::ProjMatrixX) + row));
    for (auto column = 0; column < 4; ++column)
    {
      CHECK(
        value[std::size_t(column)]
        == view.projectionRenderMatrix[std::size_t(row * 4 + column)]);
    }
  }
  setMvp(parms, world.mvp);
  CHECK(parms.value(RenderParm::MvpMatrixW)[2] == world.mvp[14]);
  CHECK(EngineFrame.lightScale == 3.0f);
}

TEST_CASE("LitPasses: GL_Scissor's Y flip and the view scissor (SC211)")
{
  auto view = LitView{};
  view.width = 64;
  view.height = 48;

  // the view's scissor is the whole target
  const auto full = viewScissor(view);
  CHECK(full.x1 == 0);
  CHECK(full.y2 == 47);
  CHECK(engineScissorRect(view, full) == nvrhi::Rect{0, 64, 0, 48});

  // a light's rect in GL's convention (y up): x = 10, y = 47 - 15, w = h = 11
  auto rect = ScreenRect{};
  rect.x1 = 10;
  rect.y1 = 5;
  rect.x2 = 20;
  rect.y2 = 15;
  CHECK(engineScissorRect(view, rect) == nvrhi::Rect{10, 21, 32, 43});

  // the bottom row of pixels in GL is the last row of the target
  rect.y1 = 0;
  rect.y2 = 0;
  CHECK(engineScissorRect(view, rect) == nvrhi::Rect{10, 21, 47, 48});

  // an empty rect (a guard) is the whole viewport
  auto empty = ScreenRect{};
  CHECK(engineScissorRect(view, empty) == nvrhi::Rect{0, 64, 0, 48});

  // idScreenRect
  auto a = ScreenRect{};
  a.clear();
  CHECK(a.isEmpty());
  a.x1 = 0;
  a.y1 = 0;
  a.x2 = 9;
  a.y2 = 4;
  CHECK(a.area() == 50);
  a.expand();
  CHECK(a.x1 == -1);
  CHECK(a.y2 == 5);
  a.intersect(full);
  CHECK(a.x1 == 0);
  CHECK(a.x2 == 10);
}

TEST_CASE("LitPasses: the state bits of the prepass and the interaction lists (D75)")
{
  auto opaque = MaterialEntry{};
  opaque.coverage = em::CoverageOpaque;
  opaque.stages = {stageOf(em::LightingDiffuse, imageKey("d", 1))};
  const auto registers = testRegisters();

  // the opaque fast path's solid draw through the generic path: depth, GLS_ALPHAMASK,
  // no colour
  auto draws = engineGenericDepthDraws(opaque, registers);
  REQUIRE(draws.size() == 1u);
  CHECK(draws[0].program == PrepassProgram::Depth);
  CHECK(draws[0].state == gls::AlphaMask);
  CHECK_FALSE(draws[0].color);

  // MF_POLYGONOFFSET: GLS_POLYGON_OFFSET with r_offsetunits -600 × polygonOffset
  auto offset = opaque;
  offset.materialFlags = em::FlagPolygonOffset;
  offset.polygonOffset = 1.0f;
  draws = engineGenericDepthDraws(offset, registers);
  REQUIRE(draws.size() == 1u);
  CHECK(draws[0].state == (gls::AlphaMask | gls::PolygonOffset));
  CHECK(draws[0].depthBias == -600.0f);
  CHECK(draws[0].slopeBias == 0.0f);

  // SS_SUBVIEW: the colour program, DST_COLOR / ZERO, LESS, colour 1 (D74)
  auto subview = opaque;
  subview.sort = em::SortSubview;
  draws = engineGenericDepthDraws(subview, registers);
  REQUIRE(draws.size() == 1u);
  CHECK(draws[0].program == PrepassProgram::Color);
  CHECK(
    draws[0].state == (gls::SrcBlendDstColor | gls::DstBlendZero | gls::DepthFuncLess));
  CHECK(draws[0].color == Vec4{1, 1, 1, 1});

  // translucent and conditioned-off materials put nothing in the depth buffer
  auto translucent = opaque;
  translucent.coverage = em::CoverageTranslucent;
  CHECK(engineGenericDepthDraws(translucent, registers).empty());
  auto off = opaque;
  off.stages[0].conditionRegister = 0;
  CHECK(engineGenericDepthDraws(off, registers).empty());

  // a perforated material: one texture_color draw per alpha-tested stage that is on
  auto perforated = MaterialEntry{};
  perforated.coverage = em::CoveragePerforated;
  auto alphaStage = stageOf(em::LightingDiffuse, imageKey("grate", 1));
  alphaStage.hasAlphaTest = true;
  alphaStage.alphaTestRegister = 2;
  alphaStage.colorRegisters = {1, 1, 1, 3};
  alphaStage.privatePolygonOffset = 2.0f;
  alphaStage.hasTextureMatrix = true;
  alphaStage.textureMatrixRegisters = {{{1, 0, 4}, {0, 1, 5}}};
  perforated.stages = {alphaStage};
  draws = engineGenericDepthDraws(perforated, registers);
  REQUIRE(draws.size() == 1u);
  CHECK(draws[0].program == PrepassProgram::TextureColor);
  CHECK(draws[0].stage == 0);
  // GL_Color: (0 0 0 alpha) clamped, alpha register 3 holds 2
  CHECK(draws[0].color == Vec4{0, 0, 0, 1});
  CHECK(draws[0].alphaTest == Vec4{0.5f, 0.5f, 0.5f, 0.5f});
  CHECK(draws[0].state == gls::PolygonOffset);
  CHECK(draws[0].depthBias == -1200.0f);
  // RB_GetShaderTextureMatrix: the offsets 45.5 and -41.25 wrapped to 0.5 and -0.25
  CHECK(draws[0].textureMatrix[0] == Vec4{1, 0, 0, 0.5f});
  CHECK(draws[0].textureMatrix[1] == Vec4{0, 1, 0, -0.25f});
  CHECK(draws[0].texGenEnabled == Vec4{0, 0, 0, 0});

  // an alpha of 0 skips the stage, and the surface is not drawn solid either
  auto zeroAlpha = perforated;
  zeroAlpha.stages[0].colorRegisters[3] = 0;
  CHECK(engineGenericDepthDraws(zeroAlpha, registers).empty());
  // no alpha-tested stage on: the GLS_ALPHAMASK solid fallback
  auto noAlpha = perforated;
  noAlpha.stages[0].hasAlphaTest = false;
  draws = engineGenericDepthDraws(noAlpha, registers);
  REQUIRE(draws.size() == 1u);
  CHECK(draws[0].program == PrepassProgram::Depth);
  CHECK(draws[0].state == gls::AlphaMask);

  // the interaction lists (tr_frontend_addmodels.cpp:991-1006)
  auto noSelf = opaque;
  noSelf.materialFlags = em::FlagNoSelfShadow;
  CHECK(engineInteractionList(opaque, false) == InteractionList::Global);
  CHECK(engineInteractionList(noSelf, false) == InteractionList::Local);
  CHECK(engineInteractionList(noSelf, true) == InteractionList::Global);
  CHECK(engineInteractionList(translucent, false) == InteractionList::Translucent);
  CHECK(engineInteractionList(translucent, true) == InteractionList::Translucent);
}

TEST_CASE(
  "LitPasses: the prepass's alpha-tested cube and sky stages (LP5.2b, C48, SC528, "
  "M-PASS-11)")
{
  const auto registers = testRegisters();
  auto cube = imageKey("env/sky", engine_image::UsageDefault);
  cube.cube = 1;
  // a perforated material whose one alpha-tested SL_AMBIENT stage has the texgen
  const auto materialOf = [&](const int texgen, const bool bump) {
    auto material = MaterialEntry{};
    material.coverage = em::CoveragePerforated;
    auto stage = stageOf(em::LightingAmbient, cube);
    stage.hasAlphaTest = true;
    stage.alphaTestRegister = 2;
    stage.texgen = texgen;
    if (bump)
    {
      material.stages.push_back(
        stageOf(em::LightingBump, imageKey("local", engine_image::UsageBump)));
    }
    material.stages.push_back(stage);
    // wobbleSky 0.5 2 45.5 (registers 2, 3, 4)
    material.texGenRegisters = {2, 3, 4};
    return material;
  };

  SECTION("an alpha-tested skybox stage: skybox (69), ALPHA_TEST written, no solid draw")
  {
    const auto draws =
      engineGenericDepthDraws(materialOf(em::TexgenSkyboxCube, false), registers);
    REQUIRE(draws.size() == 1u);
    // the program replaced after ALPHA_TEST was set (:832-850): the draw is skybox's,
    // whose shader never clips, so the whole surface's depth is written
    CHECK(draws[0].program == PrepassProgram::SkyBox);
    CHECK(draws[0].alphaTest == Vec4{0.5f, 0.5f, 0.5f, 0.5f});
    // GL_Color( 0, 0, 0, a ): rpColor, the skybox colour, is black
    CHECK(draws[0].color == Vec4{0, 0, 0, 1});
    CHECK(draws[0].stage == 0);
    // GLS_DEFAULT's depth (LESS, write) and blend (ONE / ZERO): the colour is written
    CHECK(draws[0].state == 0u);
    CHECK(draws[0].texGenEnabled == Vec4{0, 0, 0, 0});
    CHECK_FALSE(draws[0].screenTexgen);
    // "ALPHA_TEST set, then ignored" is not observable here: no renderparm set of the
    // replacing programs' layouts (DEFAULT set 0, NORMAL_CUBE and LEGACY set 5) holds
    // ALPHA_TEST, and the pipeline key names the replacing program alone
    const auto alphaLayouts = engineLayoutsOfParm(RenderParm::AlphaTest);
    for (const auto layout :
         {EngineBindingLayout::Default,
          EngineBindingLayout::NormalCube,
          EngineBindingLayout::Legacy})
    {
      CHECK((alphaLayouts & (std::uint64_t{1} << std::size_t(layout))) == 0u);
    }
    CHECK(
      (alphaLayouts & (std::uint64_t{1} << std::size_t(EngineBindingLayout::Texture)))
      != 0u);
  }

  SECTION("reflect: environment (55), or bumpyenvironment (57) with the bump stage")
  {
    const auto plain =
      engineGenericDepthDraws(materialOf(em::TexgenReflectCube, false), registers);
    REQUIRE(plain.size() == 1u);
    CHECK(plain[0].program == PrepassProgram::Environment);
    CHECK(plain[0].bumpStage == -1);
    const auto bumpy =
      engineGenericDepthDraws(materialOf(em::TexgenReflectCube, true), registers);
    REQUIRE(bumpy.size() == 1u);
    CHECK(bumpy[0].program == PrepassProgram::BumpyEnvironment);
    CHECK(bumpy[0].bumpStage == 0);
    CHECK(bumpy[0].stage == 1);
  }

  SECTION("wobbleSky: wobblesky (70) with its transform at time 0")
  {
    const auto material = materialOf(em::TexgenWobbleskyCube, false);
    const auto draws = engineGenericDepthDraws(material, registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].program == PrepassProgram::WobbleSky);
    CHECK(draws[0].wobbleSky == engineStageWobbleSky(material, registers, 0.0f));
    // the tilt of 0.5 degrees at time 0: the third row ( sin, 0, cos )
    CHECK(
      std::abs(draws[0].wobbleSky[2][0] - std::sin(0.5f * 3.14159265f / 180.0f)) < 1e-6f);
  }

  SECTION("reflect2: not drawn (LP6's inputs, M-STAGE-DIFF-05), and no solid draw")
  {
    CHECK(engineGenericDepthDraws(materialOf(em::TexgenReflectCube2, false), registers)
            .empty());
    CHECK(engineGenericDepthDraws(materialOf(em::TexgenReflectCube2, true), registers)
            .empty());
  }

  SECTION("an explicit alpha-tested stage stays on texture_color")
  {
    const auto draws =
      engineGenericDepthDraws(materialOf(em::TexgenExplicit, false), registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].program == PrepassProgram::TextureColor);
  }
}

TEST_CASE("LitPasses: the surface stage loop's decisions (SC229, SC230)")
{
  const auto registers = testRegisters();
  const auto bump = imageKey("bump", engine_image::UsageBump);
  const auto diffuse = imageKey("diffuse", engine_image::UsageDiffuse);
  const auto specular = imageKey("specular", engine_image::UsageSpecular);

  auto material = MaterialEntry{};
  material.stages = {
    stageOf(em::LightingBump, bump), stageOf(em::LightingDiffuse, diffuse, 2)};

  SECTION("no specular stage: _black, and the colour clamp01( register ) × light")
  {
    const auto draws = drawsOf(material, registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].bumpImage == bump);
    CHECK(draws[0].diffuseImage == diffuse);
    CHECK(draws[0].specularImage == engineBlackImageKey());
    CHECK(draws[0].diffuseColor == Vec4{1.5f, 1.5f, 1.5f, 1.0f});
    CHECK(draws[0].program == InteractionProgram::Interaction);
    // identity matrices without a texture matrix
    CHECK(draws[0].diffuseMatrix[0] == Vec4{1, 0, 0, 0});
    CHECK(draws[0].bumpMatrix[1] == Vec4{0, 1, 0, 0});
  }

  SECTION("no diffuse stage: _blackDiffuse; both black: no draw")
  {
    auto bumpOnly = material;
    bumpOnly.stages.resize(1);
    CHECK(drawsOf(bumpOnly, registers).empty());
    auto withSpecular = bumpOnly;
    withSpecular.stages.push_back(stageOf(em::LightingSpecular, specular));
    const auto draws = drawsOf(withSpecular, registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].diffuseImage == engineBlackDiffuseImageKey());
    CHECK(draws[0].specularImage == specular);
    CHECK(draws[0].specularColor == Vec4{3, 3, 3, 1});
  }

  SECTION("a colour of 0 or the _black image is black")
  {
    auto zero = material;
    zero.stages[1].colorRegisters = {0, 0, 0, 1};
    CHECK(drawsOf(zero, registers).empty());
    auto black = material;
    black.stages[1].image = engineBlackImageKey();
    CHECK(drawsOf(black, registers).empty());
  }

  SECTION("no bump stage: no interaction at all")
  {
    auto noBump = material;
    noBump.stages.erase(noBump.stages.begin());
    CHECK(drawsOf(noBump, registers).empty());
  }

  SECTION("the condition register, and noSpecular")
  {
    auto off = material;
    off.stages[1].conditionRegister = 0;
    off.stages.push_back(stageOf(em::LightingSpecular, specular));
    // the diffuse is off: _blackDiffuse with colour 0, the specular draws
    auto draws = drawsOf(off, registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].diffuseImage == engineBlackDiffuseImageKey());
    // a noSpecular light skips the specular stage: both black
    CHECK(drawsOf(off, registers, true).empty());
  }

  SECTION("the program by the specular image's usage after the PBR rename")
  {
    auto pbr = material;
    pbr.stages.push_back(stageOf(em::LightingSpecular, specular));
    const auto usage = ImageUsage{[&](const MaterialImageKey& key) {
      return key == specular ? engine_image::UsageSpecularPbrRmao : key.usage;
    }};
    const auto draws = drawsOf(pbr, registers, false, usage);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].program == InteractionProgram::PbrInteraction);
  }

  SECTION("a second diffuse stage draws the first; a second bump stage draws both")
  {
    auto twice = material;
    twice.stages.push_back(stageOf(em::LightingDiffuse, imageKey("second", 1)));
    auto draws = drawsOf(twice, registers);
    REQUIRE(draws.size() == 2u);
    CHECK(draws[0].diffuseImage == diffuse);
    CHECK(draws[1].diffuseImage.name == "second");

    auto twoBumps = material;
    twoBumps.stages.push_back(stageOf(em::LightingBump, imageKey("bump2", 3)));
    twoBumps.stages.push_back(stageOf(em::LightingDiffuse, imageKey("d2", 1)));
    draws = drawsOf(twoBumps, registers);
    REQUIRE(draws.size() == 2u);
    CHECK(draws[0].bumpImage == bump);
    CHECK(draws[1].bumpImage.name == "bump2");
  }

  SECTION("the ±40 wrap of SetupInteractionStage, through idMath::Ftoi")
  {
    auto scrolled = material;
    scrolled.stages[1].hasTextureMatrix = true;
    scrolled.stages[1].textureMatrixRegisters = {{{1, 0, 4}, {0, 1, 5}}};
    const auto draws = drawsOf(scrolled, registers);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].diffuseMatrix[0] == Vec4{1, 0, 0, 0.5f});
    CHECK(draws[0].diffuseMatrix[1] == Vec4{0, 1, 0, -0.25f});
  }

  SECTION("the vertex colour and the colours carry (drawInteraction_t)")
  {
    auto modulated = material;
    modulated.stages[1].vertexColor = em::VertexColorModulate;
    auto state = InteractionState{};
    drawsOf(modulated, registers, false, {}, &state);
    CHECK(state.vertexColor == em::VertexColorModulate);
    // a surface whose stages set none keeps it in the state (it draws nothing: its
    // colours start at 0)
    auto bumpOnly = material;
    bumpOnly.stages.resize(1);
    CHECK(drawsOf(bumpOnly, registers, false, {}, &state).empty());
    CHECK(state.vertexColor == em::VertexColorModulate);
    // a second bump stage draws the first interaction, then a last one with
    // _blackDiffuse and the first diffuse stage's colour and vertex colour
    auto twoBumps = modulated;
    twoBumps.stages.push_back(stageOf(em::LightingBump, imageKey("bump2", 3)));
    const auto draws = drawsOf(twoBumps, registers, false, {}, &state);
    REQUIRE(draws.size() == 2u);
    CHECK(draws[1].diffuseImage == engineBlackDiffuseImageKey());
    CHECK(draws[1].diffuseColor == Vec4{1.5f, 1.5f, 1.5f, 1.0f});
    CHECK(draws[1].vertexColor == em::VertexColorModulate);
    CHECK(engineVertexColorParms(em::VertexColorModulate)[0] == Vec4{1, 1, 1, 1});
    CHECK(engineVertexColorParms(em::VertexColorIgnore)[1] == Vec4{1, 1, 1, 1});
  }

  SECTION("a pending surface: _flat and _default with the light's colour")
  {
    const auto pending = pendingSurfaceInteraction(LightWhite, {});
    REQUIRE(pending);
    CHECK(pending->bumpImage == engineFlatImageKey());
    CHECK(pending->diffuseImage == engineDefaultImageKey());
    CHECK(pending->specularImage == engineBlackImageKey());
    CHECK(pending->diffuseColor == LightWhite);
  }

  SECTION(
    "LP5.1: an ambientLight light's specular is _black, its program interactionAmbient")
  {
    // RenderBackend.cpp:1156-1159: the specular image _black whatever the stage, so
    // specularIsBlack holds and only the diffuse draws (:1166-1173); the program is the
    // legacy branch's interactionAmbient (:1528-1537) even over a PBR specular, whose
    // usage the _black image does not have (rows 25/26, :1404-1413, never reached)
    auto withSpecular = material;
    withSpecular.stages.push_back(stageOf(em::LightingSpecular, specular));
    const auto pbrUsage = ImageUsage{[&](const MaterialImageKey& key) {
      return key == specular ? engine_image::UsageSpecularPbrRmao : key.usage;
    }};
    auto state = InteractionState{};
    state.ambientLight = true;
    const auto draws = drawsOf(withSpecular, registers, false, pbrUsage, &state);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].specularImage == engineBlackImageKey());
    CHECK(draws[0].diffuseImage == diffuse);
    CHECK(draws[0].program == InteractionProgram::InteractionAmbient);
    CHECK(state.specularImage == engineBlackImageKey());
    // the stage loop never changes it
    CHECK(state.ambientLight);
    // a black diffuse draws nothing, though its specular stage has colour
    auto blackDiffuse = withSpecular;
    blackDiffuse.stages[1].colorRegisters = {0, 0, 0, 1};
    CHECK(drawsOf(blackDiffuse, registers, false, pbrUsage, &state).empty());
    // the same surface under a texture light keeps its specular and takes PBR
    const auto texture = drawsOf(withSpecular, registers, false, pbrUsage);
    REQUIRE(texture.size() == 1u);
    CHECK(texture[0].specularImage == specular);
    CHECK(texture[0].program == InteractionProgram::PbrInteraction);
    // a pending surface under an ambient light
    const auto pending = pendingSurfaceInteraction(LightWhite, {}, true);
    REQUIRE(pending);
    CHECK(pending->specularImage == engineBlackImageKey());
    CHECK(pending->program == InteractionProgram::InteractionAmbient);
    CHECK(
      pendingSurfaceInteraction(LightWhite, {})->program
      == InteractionProgram::Interaction);
  }
}

TEST_CASE("LitPasses: the light scissor, its sort and the caps (D67, SC206, SC222)")
{
  // a square 101 × 101 view at the origin looking down +x: g_fov 106.26 on both axes,
  // whose tangent at the half is 4 / 3
  auto camera = CameraState{};
  camera.fovDegrees = 90.0f;
  const auto view = litViewOf(camera, 101, 101);
  const auto lightAt = [&](const std::string& origin, const std::string& radius) {
    const auto light =
      parseLight(EntityKeys{{"origin", origin}, {"light_radius", radius}});
    auto geometry = EngineLightGeometry{};
    geometry.origin = light.parms.origin;
    geometry.axis = light.parms.axis;
    geometry.lightRadius = light.parms.lightRadius;
    geometry.lightCenter = light.parms.lightCenter;
    geometry.pointLight = light.parms.pointLight;
    return engineDeriveLightProjection(geometry);
  };

  // the box x 300..500, y and z -50..50: its near face at ±50 / 300 / (4 / 3) = ±0.125
  // in NDC, 0.4375..0.5625 in window space, ×100 is 43.75..56.25, Ftoi 43 and 56, and
  // the pixel each way: 42..57
  const auto ahead = lightAt("400 0 0", "100 50 50");
  const auto scissor =
    engineLightScissor(view, ahead.inverseBaseLightProject, viewScissor(view));
  REQUIRE(scissor);
  CHECK(scissor->x1 == 42);
  CHECK(scissor->x2 == 57);
  CHECK(scissor->y1 == 42);
  CHECK(scissor->y2 == 57);
  CHECK(scissor->zmin < scissor->zmax);

  // behind the camera: culled to the view frustum
  const auto behind = lightAt("-400 0 0", "100 50 50");
  CHECK_FALSE(
    engineLightScissor(view, behind.inverseBaseLightProject, viewScissor(view)));

  // enclosing the camera: the whole viewport (the expanded rect clipped to it)
  const auto around = lightAt("0 0 0", "100 100 100");
  const auto whole =
    engineLightScissor(view, around.inverseBaseLightProject, viewScissor(view));
  REQUIRE(whole);
  CHECK(*whole == [&] {
    auto rect = viewScissor(view);
    rect.zmin = whole->zmin;
    rect.zmax = whole->zmax;
    return rect;
  }());

  // largest first; equal areas in the reverse of their order
  auto lights = std::vector<ViewLight>{};
  for (const auto& [id, size] :
       std::vector<std::pair<PreviewId, int>>{{1, 9}, {2, 29}, {3, 19}, {4, 29}})
  {
    auto light = ViewLight{};
    auto record = LightRecord{};
    record.id = id;
    light.record = std::make_shared<const LightRecord>(record);
    light.scissor.x1 = 0;
    light.scissor.y1 = 0;
    light.scissor.x2 = short(size);
    light.scissor.y2 = 0;
    lights.push_back(light);
  }
  engineSortViewLights(lights);
  auto order = std::vector<PreviewId>{};
  for (const auto& light : lights)
  {
    order.push_back(light.record->id);
  }
  CHECK(order == std::vector<PreviewId>{4, 2, 3, 1});

  // the caps: 256 complex surfaces, 2048 in all, the fast-path ones first
  auto fast = std::make_shared<MaterialEntry>();
  fast->fastPathBumpImage = imageKey("fast", 3);
  auto complex = std::make_shared<MaterialEntry>();
  auto surfaces = std::vector<LitSurface>(3000);
  for (auto& surface : surfaces)
  {
    surface.material = complex;
  }
  CHECK(engineOrderInteractionSurfaces(surfaces).size() == 256u);
  for (std::size_t i = 0; i < 2100; ++i)
  {
    surfaces[i].material = fast;
  }
  const auto ordered = engineOrderInteractionSurfaces(surfaces);
  CHECK(ordered.size() == 2048u);
  CHECK(std::ranges::all_of(
    ordered, [&](const LitSurface* surface) { return surface->material == fast; }));
  surfaces.resize(20);
  const auto mixed = engineOrderInteractionSurfaces(surfaces);
  REQUIRE(mixed.size() == 20u);
  CHECK(mixed.front() == &surfaces.front());
}

namespace
{

// the fixture map's chunks and light records over one registry of its materials and
// the light stand-ins, whose registers the test evaluates itself
struct FrontendFixture
{
  Snapshot snapshot;
  std::unique_ptr<MemoryDeclSource> source;
  std::unique_ptr<MaterialDecls> decls;
  std::shared_ptr<MaterialTable> table;
  ChunkJobOutput chunks;
  std::map<ChunkKey, Chunk> chunkMap;
  std::unordered_map<PreviewId, ChunkEntityState> entities;
  LightTable lights;
  std::map<std::string, PreviewId> lightIds;
  // LP5.1: what the light records were built from, for a test's own records
  LightJobInput lightInput;
};

const auto LightStandIns = std::string{
  "lights/squarelight1\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultPointLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  // LP5.1 (C45): an ambientLight light's shader, and a fog and a blend light's (LP5.3's)
  "lights/lp51/ambient\n{\n\tambientLight\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/lp51/fog\n{\n\tfogLight\n\tnoshadows\n"
  "\t{\n\t\tmap _fog\n\t\tcolored\n\t}\n}\n"
  "lights/lp51/blend\n{\n\tblendLight\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tblend filter\n\t\tmap _white\n\t}\n}\n"};

// a lit translucent material (the fixture's glass has only a blend stage, which receives
// no light), and a box of it in front of the pillars, inside lp1b_light
const auto LitGlass = std::string{
  "textures/lp2/litglass\n{\n\ttranslucent\n\tqer_editorimage "
  "textures/lp1b/glass.tga\n\tdiffusemap textures/lp1b/glass.tga\n}\n"};

std::string withLitGlass(std::string map)
{
  const auto entity1 = map.find("// entity 1");
  const auto close = map.rfind('}', entity1);
  map.insert(close, boxBrush(-40, -16, 16, -36, 16, 48, "lp2/litglass"));
  return map;
}

std::unique_ptr<FrontendFixture> frontendFixture()
{
  auto f = std::make_unique<FrontendFixture>();
  const auto root = litFixtureFolder();
  f->snapshot =
    snapshotOfMapText(withLitGlass(readTextFile(root / "maps" / "lp1b_geometry.map")));
  const auto sizes =
    listImagesSizes(readTextFile(root / "goldens" / "lp1b_geometry" / "listImages.txt"));
  f->source = std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{
    DeclFile{"materials/lp1bc.mtr", readTextFile(root / "materials" / "lp1bc.mtr"), 0},
    DeclFile{
      "materials/zz_stand_ins.mtr",
      "textures/editor/visportal\n{\n\tqer_editorimage textures/editor/visportal\n"
      "\tareaportal\n\tnoshadows\n}\n"
      "textures/common/origin\n{\n\tqer_editorimage textures/common/origin\n"
      "\torigin\n\tnoshadows\n}\n"
        + LitGlass + LightStandIns,
      0}});
  f->decls = std::make_unique<MaterialDecls>(*f->source);
  REQUIRE(f->decls->constructionError().empty());
  f->table = std::make_shared<MaterialTable>();
  f->table->registry = f->decls->id();
  for (const auto* name :
       {"textures/lp1b/floor",
        "textures/lp1b/wall",
        "textures/lp1b/crate",
        "textures/lp1b/grate",
        "textures/lp1b/glass",
        "textures/lp1b/patch",
        "textures/lp2/litglass",
        "textures/editor/visportal",
        "textures/common/origin",
        "lights/squarelight1",
        "lights/defaultPointLight",
        "lights/defaultProjectedLight",
        "lights/lp1b_missing",
        "lights/lp51/ambient",
        "lights/lp51/fog",
        "lights/lp51/blend"})
  {
    auto found = f->decls->lookup(name);
    REQUIRE(found);
    auto entry = std::make_shared<MaterialEntry>(**found.value);
    if (const auto size = sizes.find(name); size != sizes.end())
    {
      entry->editorImageSize = EditorImageSize{
        size->second.first, size->second.second, EditorImageSize::State::Ready, {}};
    }
    f->table->materials = f->table->materials.set(
      name, std::make_shared<const MaterialSlot>(MaterialSlot{std::move(entry), {}}));
  }

  const auto proc = std::make_shared<const ProcTree>(
    readProcFile(
      readTextFile(root / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"), true)
    | kdl::value());

  // the chunks of every cell
  auto input = ChunkJobInput{};
  for (const auto& entity : sceneEntitiesOf(f->snapshot))
  {
    input.entities[entity->id] = entity;
    if (entity->classname == "worldspawn")
    {
      input.worldspawn = entity;
    }
  }
  auto cells = std::set<CellRef>{};
  for (const auto& node : sceneNodesOf(f->snapshot))
  {
    input.nodes.push_back(node);
    if (node->kind == SnapshotNodeKind::Brush || node->kind == SnapshotNodeKind::Patch)
    {
      cells.insert(CellRef{node->entity, cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.proc = proc;
  input.materials = f->table;
  f->chunks = buildChunks(input);
  for (auto& chunk : f->chunks.chunks)
  {
    f->chunkMap.emplace(chunk.key, chunk);
  }
  for (const auto& state : f->chunks.entities)
  {
    f->entities.emplace(state.id, state);
  }

  // the light records
  auto lightInput = LightJobInput{};
  lightInput.materials = f->table;
  lightInput.proc = proc;
  for (const auto& entity : f->snapshot.entities)
  {
    auto keys = EntityKeys{};
    for (auto k = entity.keys.first; k < entity.keys.first + entity.keys.count; ++k)
    {
      keys.emplace_back(
        f->snapshot.string(f->snapshot.keyValues[k].key),
        f->snapshot.string(f->snapshot.keyValues[k].value));
    }
    const auto classname = std::string{f->snapshot.string(entity.classname)};
    if (classname == "worldspawn")
    {
      lightInput.worldspawn = keys;
    }
    if (classname == "light")
    {
      if (const auto* name = findKey(keys, "name"))
      {
        f->lightIds[*name] = entity.id;
      }
      lightInput.lights.push_back(
        LightJobInput::Light{entity.id, 1, classname, keys, true});
    }
  }
  for (auto& record : buildLightRecords(lightInput).records)
  {
    const auto id = record.id;
    f->lights.records =
      f->lights.records.set(id, std::make_shared<const LightRecord>(std::move(record)));
  }
  f->lights.version = 1;
  f->lightInput = std::move(lightInput);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return f;
}

// the registers of a material for shader parms, evaluated at once (current)
RegisterAnswer evaluatedRegisters(
  const FrontendFixture& f, const MaterialEntry& material, const ShaderParms& parms)
{
  auto evaluated = f.decls->evaluate(material, parms, 0.0f);
  REQUIRE(evaluated);
  return RegisterAnswer{
    std::make_shared<const EvaluatedRegisters>(std::move(*evaluated.value)), true};
}

// the fixture's lists over a light table, with the registers of registers (evaluated at
// once when none is given)
FrontendLists listsOfLights(
  FrontendFixture& f,
  const LightTable& lights,
  const CameraState& camera,
  const int width,
  const int height,
  std::function<RegisterAnswer(PreviewId, const MaterialEntry&, const ShaderParms&)>
    registers = {})
{
  auto inputs = FrontendInputs{};
  inputs.view = litViewOf(camera, std::uint32_t(width), std::uint32_t(height));
  inputs.chunks = &f.chunkMap;
  inputs.entities = &f.entities;
  inputs.lights = &lights;
  inputs.material = [&](const std::string& name) {
    const auto slot = f.table->find(name);
    return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
  };
  inputs.registers =
    registers
      ? std::move(registers)
      : [&](const PreviewId, const MaterialEntry& material, const ShaderParms& parms) {
          return evaluatedRegisters(f, material, parms);
        };
  return buildFrontendLists(inputs);
}

// LP5.3: listsOfLights with the view's scissor of a test (FrontendInputs::
// scissorForTesting)
FrontendLists listsWithScissor(
  FrontendFixture& f,
  const LightTable& lights,
  const CameraState& camera,
  const int width,
  const int height,
  const ScreenRect& scissor)
{
  auto inputs = FrontendInputs{};
  inputs.view = litViewOf(camera, std::uint32_t(width), std::uint32_t(height));
  inputs.chunks = &f.chunkMap;
  inputs.entities = &f.entities;
  inputs.lights = &lights;
  inputs.material = [&](const std::string& name) {
    const auto slot = f.table->find(name);
    return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
  };
  inputs.registers =
    [&](const PreviewId, const MaterialEntry& material, const ShaderParms& parms) {
      return evaluatedRegisters(f, material, parms);
    };
  inputs.scissorForTesting = scissor;
  return buildFrontendLists(inputs);
}

FrontendLists listsOf(
  FrontendFixture& f, const CameraState& camera, int width, int height)
{
  return listsOfLights(f, f.lights, camera, width, height);
}

// LP5.1: the record of one of the fixture's lights with its keys changed (a key set to a
// value, or added), built as the fixture's records are
std::shared_ptr<const LightRecord> lightRecordWith(
  const FrontendFixture& f,
  const std::string& name,
  const std::vector<std::pair<std::string, std::string>>& changes)
{
  auto input = f.lightInput;
  const auto id = f.lightIds.at(name);
  std::erase_if(input.lights, [&](const auto& light) { return light.id != id; });
  REQUIRE(input.lights.size() == 1u);
  auto& keys = input.lights.front().mapKeys;
  for (const auto& [key, value] : changes)
  {
    const auto it =
      std::ranges::find_if(keys, [&](const auto& pair) { return pair.first == key; });
    if (it != keys.end())
    {
      it->second = value;
    }
    else
    {
      keys.emplace_back(key, value);
    }
  }
  auto records = buildLightRecords(input).records;
  REQUIRE(records.size() == 1u);
  return std::make_shared<const LightRecord>(std::move(records.front()));
}

// a light table of records (LP5.1)
LightTable lightTableOf(
  const std::vector<std::shared_ptr<const LightRecord>>& records,
  const std::uint64_t version)
{
  auto table = LightTable{};
  for (const auto& record : records)
  {
    table.records = table.records.set(record->id, record);
  }
  table.version = version;
  return table;
}

CameraState fixtureCamera(const float x, const float y, const float z)
{
  auto camera = CameraState{};
  camera.position = vm::vec3f{x, y, z};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  return camera;
}

const ViewLight* lightOf(
  const FrontendLists& lists, const FrontendFixture& f, const std::string& name)
{
  const auto id = f.lightIds.at(name);
  for (const auto& light : lists.lights)
  {
    if (light.record->id == id)
    {
      return &light;
    }
  }
  return nullptr;
}

std::set<std::string> materialsOf(const std::vector<LitSurface>& surfaces)
{
  auto names = std::set<std::string>{};
  for (const auto& surface : surfaces)
  {
    names.insert(surface.chunk->key.material);
  }
  return names;
}

// FNV-1a over a text, the same on every platform
std::uint64_t textHash(const std::string& text)
{
  auto hash = std::uint64_t{14695981039346656037ull};
  for (const auto c : text)
  {
    hash = (hash ^ std::uint64_t(std::uint8_t(c))) * 1099511628211ull;
  }
  return hash;
}

// the lists as text: the drawn surfaces' chunk keys in order, then per light its id, its
// scissor and its three lists' chunk keys in order
std::string listsText(const FrontendLists& lists)
{
  const auto keyText = [](const ChunkKey& key) {
    return fmt::format(
      "({} {} {} {} {} {} {})",
      key.area,
      key.entity,
      key.material,
      key.cell[0],
      key.cell[1],
      key.cell[2],
      key.part);
  };
  auto text = std::string{"drawn"};
  for (const auto& surface : lists.drawSurfs)
  {
    text += keyText(surface.chunk->key);
  }
  for (const auto& light : lists.lights)
  {
    text += fmt::format(
      "\nlight {} [{} {} {} {}]",
      light.record->id,
      light.scissor.x1,
      light.scissor.y1,
      light.scissor.x2,
      light.scissor.y2);
    for (const auto* list :
         {&light.localInteractions,
          &light.globalInteractions,
          &light.translucentInteractions})
    {
      text += " |";
      for (const auto& surface : *list)
      {
        text += keyText(surface.chunk->key);
      }
    }
  }
  return text;
}

CameraState cameraAt(const EngineVec3& eye, const EngineVec3& direction)
{
  auto camera = fixtureCamera(eye[0], eye[1], eye[2]);
  camera.direction = vm::vec3f{direction[0], direction[1], direction[2]};
  return camera;
}

// LP4.1: a scene of synthetic chunks, entity states and light records over a tree, with
// the fixture's materials and the light stand-ins
struct FlowScene
{
  std::map<ChunkKey, Chunk> chunks;
  std::unordered_map<PreviewId, ChunkEntityState> entities;
  LightTable lights;
  const ProcTree* tree = nullptr;
  const AreaData* areas = nullptr;
  // LP4.3 (C36): the light grid and the areas whose grid is valid
  const GridTable* grid = nullptr;
  std::set<int> validGrids;
};

// a chunk of one triangle whose bounds are mins..maxs (the frontend reads its key, its
// indexes and its bounds); in its entity's model space
void addChunk(
  FlowScene& scene,
  const int area,
  const PreviewId entity,
  const EngineVec3& mins,
  const EngineVec3& maxs,
  const std::string& material = "textures/lp1b/wall")
{
  auto chunk = Chunk{};
  chunk.key = ChunkKey{area, entity, material, {0, 0, 0}, 0};
  chunk.verts.resize(3);
  chunk.verts[0].xyz = mins;
  chunk.verts[1].xyz = maxs;
  chunk.verts[2].xyz = EngineVec3{mins[0], maxs[1], mins[2]};
  chunk.indexes = {0, 1, 2};
  chunk.bounds = {mins, maxs};
  scene.chunks.emplace(chunk.key, chunk);
}

// a brush entity's state at origin (identity axis) whose chunks span local bounds and
// reference areas (in chain order), as SceneGeometry::updateEntityReferences leaves it
void addEntity(
  FlowScene& scene,
  const PreviewId id,
  const EngineVec3& origin,
  const EngineBounds& local,
  const std::vector<int>& areas)
{
  auto state = ChunkEntityState{};
  state.id = id;
  state.parms.axis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  state.parms.shaderParms = engineWorldShaderParms();
  state.modelOrigin = origin;
  state.localReferenceBounds = local;
  state.inverseBaseModelProject = engineOffsetScaleForBounds(
    engineMatrixFromOriginAxis(origin, state.parms.axis), local);
  state.globalReferenceBounds =
    engineProjectedBounds(state.inverseBaseModelProject, engineUnitCube(), false);
  state.areas = areas;
  scene.entities.emplace(id, std::move(state));
}

// a light record of the map keys, derived over the scene's tree (its areaNum and area
// references) with the fixture's table
void addLight(
  FlowScene& scene, const FrontendFixture& f, const PreviewId id, EntityKeys keys)
{
  auto record = LightRecord{};
  record.id = id;
  record.version = 1;
  record.classname = "light";
  record.spawnArgs = keys;
  record.light = parseLight(mergeSpawnArgs(keys, nullptr));
  record.derived = deriveLight(record.light, f.table.get(), scene.tree);
  REQUIRE_FALSE(record.derived.shader.pending);
  scene.lights.records =
    scene.lights.records.set(id, std::make_shared<const LightRecord>(std::move(record)));
  scene.lights.version += 1;
}

FrontendLists flowListsOf(
  const FrontendFixture& f, const FlowScene& scene, const CameraState& camera)
{
  auto inputs = FrontendInputs{};
  inputs.view = litViewOf(camera, 128, 96);
  inputs.chunks = &scene.chunks;
  inputs.entities = &scene.entities;
  inputs.lights = &scene.lights;
  inputs.tree = scene.tree;
  inputs.areas = scene.areas;
  if (scene.grid)
  {
    inputs.grid = scene.grid;
    inputs.gridValid = [&](const int area) { return scene.validGrids.contains(area); };
  }
  inputs.material = [&](const std::string& name) {
    const auto slot = f.table->find(name);
    return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
  };
  inputs.registers =
    [&](const PreviewId, const MaterialEntry& material, const ShaderParms& parms) {
      auto evaluated = f.decls->evaluate(material, parms, 0.0f);
      REQUIRE(evaluated);
      return RegisterAnswer{
        std::make_shared<const EvaluatedRegisters>(std::move(*evaluated.value)), true};
    };
  return buildFrontendLists(inputs);
}

const ViewLight* viewLightOf(const FrontendLists& lists, const PreviewId id)
{
  const auto it = std::ranges::find_if(
    lists.lights, [&](const ViewLight& light) { return light.record->id == id; });
  return it != lists.lights.end() ? &*it : nullptr;
}

// the chunks' areas and entities a light lights, as (area, entity) pairs
std::set<std::pair<int, PreviewId>> litOf(const ViewLight* light)
{
  auto lit = std::set<std::pair<int, PreviewId>>{};
  if (light)
  {
    for (const auto* list :
         {&light->localInteractions,
          &light->globalInteractions,
          &light->translucentInteractions})
    {
      for (const auto& surface : *list)
      {
        lit.emplace(surface.chunk->key.area, surface.chunk->key.entity);
      }
    }
  }
  return lit;
}

std::set<std::pair<int, PreviewId>> drawnOf(const FrontendLists& lists)
{
  auto drawn = std::set<std::pair<int, PreviewId>>{};
  for (const auto& surface : lists.drawSurfs)
  {
    drawn.emplace(surface.chunk->key.area, surface.chunk->key.entity);
  }
  return drawn;
}

ScreenRect screenRect(const short x1, const short y1, const short x2, const short y2)
{
  auto rect = ScreenRect{};
  rect.x1 = x1;
  rect.y1 = y1;
  rect.x2 = x2;
  rect.y2 = y2;
  return rect;
}

} // namespace

TEST_CASE("LitPasses: the LP2.2a fixture's lists without a tree (LP4.1, D94)")
{
  // D94: without a tree the lists are LP2's; the drawn surfaces and the lights (their
  // ids, scissors and three lists' chunks) of three cameras equal those before LP4.1
  // (the counts and the FNV-1a hash of listsText, taken from ee09c988c's frontend)
  auto f = frontendFixture();
  struct Expected
  {
    CameraState camera;
    std::size_t drawn = 0;
    std::size_t lights = 0;
    std::uint64_t hash = 0;
  };
  for (const auto& expected : {
         Expected{fixtureCamera(-240, 0, 64), 17, 8, 0x80fa1976e119e7a7ull},
         Expected{fixtureCamera(0, 0, 64), 11, 5, 0xe86f28e4f3cc3800ull},
         Expected{cameraAt({200, 0, 64}, {-1, 0, 0}), 19, 6, 0xf2e82e63883146bbull},
       })
  {
    const auto lists = listsOf(*f, expected.camera, 128, 96);
    CHECK_FALSE(lists.flowed);
    CHECK(lists.viewArea == -1);
    CHECK(lists.areasFlowed == 0u);
    CHECK(lists.portalChains == 0u);
    CHECK(lists.entitiesCulled == 0u);
    CHECK(lists.lightsInView >= lists.lights.size());
    CHECK(lists.drawSurfs.size() == expected.drawn);
    CHECK(lists.lights.size() == expected.lights);
    CHECK(textHash(listsText(lists)) == expected.hash);
  }
}

TEST_CASE("LitPasses: the frontend on the view's portal flow (LP4.1, C33, D81)")
{
  auto f = frontendFixture();
  // the LP1b fixture's tree: area 0 the east room (x 8..256), area 1 the west room (x
  // -256..8), one portal on x = 8 (y -32..32, z 0..96)
  const auto tree =
    readProcFile(readTextFile(
      litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"))
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  const auto areas = buildAreaData(tree);
  REQUIRE(areas.areas.size() == 2u);
  const auto point = [](const std::string& origin, const std::string& radius) {
    return EntityKeys{{"origin", origin}, {"light_radius", radius}, {"noshadows", "1"}};
  };
  constexpr auto World = PreviewId{1};

  SECTION("an area-1 light pairs with area 1's chunks, never area 0's")
  {
    auto scene = FlowScene{};
    scene.tree = &tree;
    scene.areas = &areas;
    // area 1's chunk and one keyed area 0 at the same place, both inside the light's
    // volume (x -210..-90) and ahead of the eye
    addChunk(scene, 1, World, {-215, -50, 0}, {-205, 50, 96});
    addChunk(scene, 0, World, {-215, -50, 0}, {-205, 50, 96}, "textures/lp1b/floor");
    addLight(scene, *f, 10, point("-150 0 48", "60 60 60"));
    REQUIRE(scene.lights.find(10)->derived.areas == std::vector<int>{1});

    // in area 1 facing away from the portal: area 0 is not flowed, so its chunk is not
    // drawn; the light lights area 1's chunk
    const auto away = flowListsOf(*f, scene, cameraAt({-200, 0, 48}, {-1, 0, 0}));
    CHECK(away.flowed);
    CHECK(away.viewArea == 1);
    CHECK(away.areasFlowed == 1u);
    CHECK(away.portalChains == 1u);
    CHECK(drawnOf(away) == std::set<std::pair<int, PreviewId>>{{1, World}});
    CHECK(away.lightsInView == 1u);
    CHECK(
      litOf(viewLightOf(away, 10)) == std::set<std::pair<int, PreviewId>>{{1, World}});

    // facing the portal from area 1, area 0 is flowed and its chunk drawn, but the light
    // does not reference area 0, so it does not light it although its volume does (C33 a)
    auto through = scene;
    through.chunks.clear();
    addChunk(through, 1, World, {-60, -10, 30}, {-50, 10, 60});
    addChunk(through, 0, World, {-60, -10, 30}, {-50, 10, 60}, "textures/lp1b/floor");
    through.lights = LightTable{};
    addLight(through, *f, 11, point("-90 0 48", "60 60 60"));
    REQUIRE(through.lights.find(11)->derived.areas == std::vector<int>{1});
    const auto facing = flowListsOf(*f, through, cameraAt({-200, 0, 48}, {1, 0, 0}));
    CHECK(facing.viewArea == 1);
    CHECK(facing.areasFlowed == 2u);
    CHECK(drawnOf(facing) == std::set<std::pair<int, PreviewId>>{{0, World}, {1, World}});
    CHECK(
      litOf(viewLightOf(facing, 11)) == std::set<std::pair<int, PreviewId>>{{1, World}});
  }

  SECTION("an area-0 light referencing both areas pairs with both")
  {
    auto scene = FlowScene{};
    scene.tree = &tree;
    scene.areas = &areas;
    // the light's box x -40..80 crosses the portal's plane
    addLight(scene, *f, 10, point("20 0 48", "60 60 60"));
    const auto& record = *scene.lights.find(10);
    REQUIRE(record.derived.areaNum == 0);
    REQUIRE(
      std::set<int>{record.derived.areas.begin(), record.derived.areas.end()}
      == std::set<int>{0, 1});
    // area 0's chunk, and area 1's seen through the portal
    addChunk(scene, 0, World, {40, -20, 20}, {50, 20, 80});
    addChunk(scene, 1, World, {-30, -20, 20}, {-20, 20, 60}, "textures/lp1b/floor");
    const auto lists = flowListsOf(*f, scene, cameraAt({200, 0, 48}, {-1, 0, 0}));
    CHECK(lists.viewArea == 0);
    CHECK(lists.areasFlowed == 2u);
    CHECK(lists.portalChains == 2u);
    CHECK(
      litOf(viewLightOf(lists, 10))
      == std::set<std::pair<int, PreviewId>>{{0, World}, {1, World}});
  }

  SECTION("a brush entity referenced in both areas pairs through the shared one")
  {
    auto scene = FlowScene{};
    scene.tree = &tree;
    scene.areas = &areas;
    // the entity straddles the doorway (world x -8..24); the light's box x -115..-5
    // references area 1 alone and reaches the entity's west end
    constexpr auto Door = PreviewId{20};
    addEntity(
      scene,
      Door,
      {8, 0, 48},
      EngineBounds{EngineVec3{-16, -16, -16}, EngineVec3{16, 16, 16}},
      {0, 1});
    addChunk(scene, 0, Door, {-16, -16, -16}, {16, 16, 16});
    addLight(scene, *f, 10, point("-60 0 48", "55 55 55"));
    REQUIRE(scene.lights.find(10)->derived.areas == std::vector<int>{1});
    // seen directly from area 0 (its chain), the light through the portal
    const auto lists = flowListsOf(*f, scene, cameraAt({200, 0, 48}, {-1, 0, 0}));
    CHECK(drawnOf(lists) == std::set<std::pair<int, PreviewId>>{{0, Door}});
    CHECK(
      litOf(viewLightOf(lists, 10)) == std::set<std::pair<int, PreviewId>>{{0, Door}});
    CHECK(lists.entitiesCulled == 0u);

    // from area 0 facing away the entity is behind the eye: culled by the chain's
    // corners, and counted
    const auto away = flowListsOf(*f, scene, cameraAt({60, 0, 48}, {1, 0, 0}));
    CHECK(away.drawSurfs.empty());
    CHECK(away.entitiesCulled == 1u);
    CHECK(away.lights.empty());
  }

  SECTION("the outside set: drawn, lit by volume (D81)")
  {
    auto scene = FlowScene{};
    scene.tree = &tree;
    scene.areas = &areas;
    // a world piece in solid (area -1) ahead of the eye, inside an area-0 light's volume
    addChunk(scene, -1, World, {100, -10, 30}, {110, 10, 60});
    addLight(scene, *f, 10, point("120 0 48", "40 40 40"));
    REQUIRE(scene.lights.find(10)->derived.areas == std::vector<int>{0});
    // a light in solid, east of the room (x > 256): no references; a piece in solid and
    // a piece keyed area 0 in its volume
    addLight(scene, *f, 11, point("400 0 48", "20 20 20"));
    REQUIRE(scene.lights.find(11)->derived.areas.empty());
    addChunk(scene, -1, World, {390, -10, 40}, {395, 10, 56}, "textures/lp1b/floor");
    addChunk(scene, 0, World, {396, -10, 40}, {398, 10, 56}, "textures/lp1b/crate");
    // an entity without references (cleared bounds would have none; here its state says
    // so) ahead of the eye in the first light's volume
    constexpr auto Loose = PreviewId{30};
    addEntity(
      scene,
      Loose,
      {130, 0, 48},
      EngineBounds{EngineVec3{-4, -4, -4}, EngineVec3{4, 4, 4}},
      {});
    addChunk(scene, 0, Loose, {-4, -4, -4}, {4, 4, 4});

    const auto west = flowListsOf(*f, scene, cameraAt({200, 0, 48}, {-1, 0, 0}));
    CHECK(drawnOf(west).contains({-1, World}));
    CHECK(drawnOf(west).contains({0, Loose}));
    CHECK(
      litOf(viewLightOf(west, 10))
      == std::set<std::pair<int, PreviewId>>{{-1, World}, {0, Loose}});

    // facing east: the light in solid has the view's scissor and lights the piece in
    // solid alone, not the area-0 piece in its volume (C33 c)
    const auto east = flowListsOf(*f, scene, cameraAt({200, 0, 48}, {1, 0, 0}));
    CHECK(drawnOf(east).contains({0, World}));
    const auto* outside = viewLightOf(east, 11);
    REQUIRE(outside);
    CHECK(litOf(outside) == std::set<std::pair<int, PreviewId>>{{-1, World}});
  }

  SECTION("a light through the portal: its scissor is the chain's rect ∩ its projection")
  {
    auto scene = FlowScene{};
    scene.tree = &tree;
    scene.areas = &areas;
    // the box x -140..-60, y -120..120, z 8..88 lies in area 1
    addLight(
      scene,
      *f,
      10,
      EntityKeys{
        {"origin", "-100 0 48"}, {"light_radius", "40 120 40"}, {"noshadows", "1"}});
    REQUIRE(scene.lights.find(10)->derived.areas == std::vector<int>{1});
    addChunk(scene, 1, World, {-130, -30, 20}, {-120, 30, 60});
    const auto lists = flowListsOf(*f, scene, cameraAt({200, 0, 64}, {-1, 0, 0}));
    const auto* light = viewLightOf(lists, 10);
    REQUIRE(light);
    // the chain into area 1 is ( 51, 25, 75, 59 ) (tst_ViewFlow.cpp's hand rect). The
    // light's near face x -60 is 260 ahead: y -+120 is ndc x -+0.46154, window ( ndc ·
    // 0.5 + 0.5 ) · 127 = 34.19 .. 92.81; z 8 and 88 are -56 and +24 from the eye, ndc y
    // -56 / 195 and 24 / 195 (tan 0.75), window · 95 = 33.86 .. 53.35; at the far face
    // (340 ahead) both ranges are inside these. Ftoi and Expand: ( 33, 32, 93, 54 ),
    // within the chain's x and outside its y: ( 51, 32, 75, 54 )
    CHECK(light->scissor.x1 == 51);
    CHECK(light->scissor.y1 == 32);
    CHECK(light->scissor.x2 == 75);
    CHECK(light->scissor.y2 == 54);
    const auto view = viewScissor(litViewOf(cameraAt({200, 0, 64}, {-1, 0, 0}), 128, 96));
    CHECK(light->scissor.x1 >= view.x1);
    CHECK(light->scissor.y1 >= view.y1);
    CHECK(light->scissor.x2 <= view.x2);
    CHECK(light->scissor.y2 <= view.y2);
    CHECK(light->scissor.area() < view.area());
    // the projected rect alone (the view's scissor for the flow's) is wider
    const auto projected = engineLightScissor(
      litViewOf(cameraAt({200, 0, 64}, {-1, 0, 0}), 128, 96),
      scene.lights.find(10)->derived.projection.inverseBaseLightProject,
      view);
    REQUIRE(projected);
    CHECK(*projected == [&] {
      auto rect = screenRect(33, 32, 93, 54);
      rect.zmin = projected->zmin;
      rect.zmax = projected->zmax;
      return rect;
    }());
  }
}

// LP4.3 (C36, P4.11, D81): the grid area of each drawn surface, on the LP1b fixture's
// tree and a synthetic grid of both areas, the valid ones the test's
TEST_CASE("LitPasses: the grid area of the drawn surfaces (LP4.3, C36, P4.11)")
{
  auto f = frontendFixture();
  const auto tree =
    readProcFile(readTextFile(
      litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"))
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  const auto areas = buildAreaData(tree);
  REQUIRE(areas.areas.size() == 2u);
  auto grid = GridTable{};
  grid.version = 1;
  for (auto i = 0; i < 2; ++i)
  {
    auto area = GridArea{};
    area.area = i;
    area.hasPoints = true;
    area.atlas = lightGridImageKey("maps/lp1b_geometry", i);
    grid.areas.push_back(area);
  }
  constexpr auto World = PreviewId{1};
  constexpr auto Crate = PreviewId{20};
  constexpr auto Long = PreviewId{21};
  constexpr auto Stray = PreviewId{22};
  // from area 0 looking west through the portal: area 0 and area 1 flowed
  const auto camera = cameraAt({200, 0, 48}, {-1, 0, 0});

  auto scene = FlowScene{};
  scene.tree = &tree;
  scene.areas = &areas;
  scene.grid = &grid;
  // a world chunk in each room, in front of the eye, and one in solid (the outside set)
  addChunk(scene, 0, World, {40, -20, 20}, {50, 20, 80});
  addChunk(scene, 1, World, {-30, -20, 20}, {-20, 20, 60}, "textures/lp1b/floor");
  addChunk(scene, -1, World, {100, -10, 30}, {110, 10, 60}, "textures/lp1b/crate");
  // a crate whose centre (120 0 48) lies in its own area 0
  addEntity(
    scene,
    Crate,
    {120, 0, 48},
    EngineBounds{EngineVec3{-8, -8, -8}, EngineVec3{8, 8, 8}},
    {0});
  scene.entities.at(Crate).materialBounds.emplace(
    "textures/lp1b/crate", EngineBounds{EngineVec3{-8, -8, -8}, EngineVec3{8, 8, 8}});
  addChunk(scene, 0, Crate, {-8, -8, -8}, {8, 8, 8}, "textures/lp1b/crate");
  // a long entity spread over two cells, one material: its union's centre (world x 0)
  // lies in area 1 though one of its chunks is in area 0's room
  addEntity(
    scene,
    Long,
    {-200, 0, 48},
    EngineBounds{EngineVec3{0, -4, -4}, EngineVec3{400, 4, 4}},
    {0, 1});
  scene.entities.at(Long).materialBounds.emplace(
    "textures/lp1b/wall", EngineBounds{EngineVec3{0, -4, -4}, EngineVec3{400, 4, 4}});
  {
    auto chunk = Chunk{};
    chunk.key = ChunkKey{0, Long, "textures/lp1b/wall", {0, 0, 0}, 0};
    chunk.verts.resize(3);
    chunk.verts[0].xyz = {0, -4, -4};
    chunk.verts[1].xyz = {150, 4, 4};
    chunk.verts[2].xyz = {0, 4, -4};
    chunk.indexes = {0, 1, 2};
    chunk.bounds = {EngineVec3{0, -4, -4}, EngineVec3{150, 4, 4}};
    scene.chunks.emplace(chunk.key, chunk);
    chunk.key.cell = {1, 0, 0};
    chunk.verts[0].xyz = {250, -4, -4};
    chunk.verts[1].xyz = {360, 4, 4};
    chunk.verts[2].xyz = {250, 4, -4};
    chunk.bounds = {EngineVec3{250, -4, -4}, EngineVec3{360, 4, 4}};
    scene.chunks.emplace(chunk.key, chunk);
  }
  // an entity whose material's centre (300 0 48, east of the room) is in solid,
  // referenced in both areas in the chain order { 1, 0 }: the first valid reference
  addEntity(
    scene,
    Stray,
    {100, 0, 48},
    EngineBounds{EngineVec3{-4, -4, -4}, EngineVec3{4, 4, 4}},
    {1, 0});
  scene.entities.at(Stray).materialBounds.emplace(
    "textures/lp1b/crate", EngineBounds{EngineVec3{190, -4, -4}, EngineVec3{210, 4, 4}});
  addChunk(scene, 0, Stray, {-4, -4, -4}, {4, 4, 4}, "textures/lp1b/crate");

  const auto gridAreas = [](const FrontendLists& lists) {
    auto out = std::map<std::pair<PreviewId, int>, std::optional<int>>{};
    for (const auto& surface : lists.drawSurfs)
    {
      out[{
        surface.chunk->key.entity,
        surface.chunk->key.cell[0] * 10 + surface.chunk->key.area}] = surface.gridArea;
    }
    return out;
  };

  SECTION("both grids valid")
  {
    scene.validGrids = {0, 1};
    const auto lists = flowListsOf(*f, scene, camera);
    REQUIRE(lists.flowed);
    REQUIRE(lists.areasFlowed == 2u);
    const auto got = gridAreas(lists);
    using Key = std::pair<PreviewId, int>;
    // the world: refs [N], its own area
    CHECK(got.at(Key{World, 0}) == 0);
    CHECK(got.at(Key{World, 1}) == 1);
    // the outside set: none (the probe path, D81)
    CHECK(got.at(Key{World, -1}) == std::nullopt);
    // the centre's own area
    CHECK(got.at(Key{Crate, 0}) == 0);
    // one centre for both cells: area 1, the first loop
    CHECK(got.at(Key{Long, 0}) == 1);
    CHECK(got.at(Key{Long, 10}) == 1);
    // the centre in solid: the first reference with a valid grid in chain order
    CHECK(got.at(Key{Stray, 0}) == 1);
    CHECK(lists.gridSurfaces == 6u);
  }

  SECTION("area 1's grid not valid: the first valid reference")
  {
    scene.validGrids = {0};
    const auto got = gridAreas(flowListsOf(*f, scene, camera));
    using Key = std::pair<PreviewId, int>;
    CHECK(got.at(Key{World, 0}) == 0);
    CHECK(got.at(Key{World, 1}) == std::nullopt);
    CHECK(got.at(Key{Crate, 0}) == 0);
    // the centre's area 1 has no valid grid: the first loop finds none, the second area 0
    CHECK(got.at(Key{Long, 0}) == 0);
    CHECK(got.at(Key{Long, 10}) == 0);
    CHECK(got.at(Key{Stray, 0}) == 0);
  }

  SECTION("no valid grid anywhere: every surface on the probe path")
  {
    scene.validGrids = {};
    const auto lists = flowListsOf(*f, scene, camera);
    for (const auto& surface : lists.drawSurfs)
    {
      CHECK_FALSE(surface.gridArea);
    }
    CHECK(lists.gridSurfaces == 0u);
  }

  SECTION("no grid table, or no tree: none")
  {
    scene.validGrids = {0, 1};
    auto noGrid = scene;
    noGrid.grid = nullptr;
    CHECK(flowListsOf(*f, noGrid, camera).gridSurfaces == 0u);
    auto noTree = scene;
    noTree.tree = nullptr;
    noTree.areas = nullptr;
    const auto lists = flowListsOf(*f, noTree, camera);
    CHECK_FALSE(lists.flowed);
    CHECK(lists.gridSurfaces == 0u);
  }
}

TEST_CASE("LitPasses: the areas parity map's portal brushes make no chunk (LP4.1)")
{
  // TOOLS/lit-preview/parity.py's areas map: its doorways hold areaportal brushes of
  // lp4/portal (noshadows, areaportal, no stage), whose sides dmap makes no surface of
  // (usurface.cpp:191-195: neither drawn nor casting shadows), and neither does the chunk
  // builder (addBrush's same rule); every other face is built
  const auto root = litFixtureFolder();
  const auto snapshot =
    snapshotOfMapText(readTextFile(root / "maps" / "test" / "litpreview_areas.map"));
  auto source = MemoryDeclSource{std::vector<DeclFile>{
    DeclFile{
      "materials/lp2_parity.mtr", readTextFile(root / "materials" / "lp2_parity.mtr"), 0},
    DeclFile{
      "materials/lp4_areas.mtr", readTextFile(root / "materials" / "lp4_areas.mtr"), 0}}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  auto table = std::make_shared<MaterialTable>();
  table->registry = decls.id();
  for (const auto* name :
       {"textures/lp2/floor",
        "textures/lp2/wall",
        "textures/lp2/crate",
        "textures/lp2/near",
        "textures/lp4/portal"})
  {
    auto found = decls.lookup(name);
    REQUIRE(found);
    auto entry = std::make_shared<MaterialEntry>(**found.value);
    entry->editorImageSize = EditorImageSize{64, 64, EditorImageSize::State::Ready, {}};
    table->materials = table->materials.set(
      name, std::make_shared<const MaterialSlot>(MaterialSlot{std::move(entry), {}}));
  }
  const auto portal = table->find("textures/lp4/portal");
  REQUIRE(portal);
  // CONTENTS_AREAPORTAL (renderer/Material.h:392)
  CHECK((portal->entry->contentFlags & (1 << 20)) != 0);
  CHECK_FALSE(engineMaterialIsDrawn(*portal->entry));

  auto input = ChunkJobInput{};
  for (const auto& entity : sceneEntitiesOf(snapshot))
  {
    input.entities[entity->id] = entity;
    if (entity->classname == "worldspawn")
    {
      input.worldspawn = entity;
    }
  }
  auto cells = std::set<CellRef>{};
  auto portalFaces = std::size_t{0};
  for (const auto& node : sceneNodesOf(snapshot))
  {
    input.nodes.push_back(node);
    if (node->kind == SnapshotNodeKind::Brush)
    {
      cells.insert(CellRef{node->entity, cellOfNode(*node)});
      portalFaces += std::size_t(std::ranges::count_if(node->faces, [](const auto& face) {
        return face.material.ends_with("lp4/portal");
      }));
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.materials = table;
  const auto output = buildChunks(input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  // four doorways, six sides each
  CHECK(portalFaces == 24u);
  CHECK_FALSE(output.chunks.empty());
  auto materials = std::set<std::string>{};
  for (const auto& chunk : output.chunks)
  {
    materials.insert(chunk.key.material);
  }
  CHECK_FALSE(materials.contains("textures/lp4/portal"));
  CHECK(
    materials
    == std::set<std::string>{
      "textures/lp2/crate",
      "textures/lp2/floor",
      "textures/lp2/near",
      "textures/lp2/wall"});
}

TEST_CASE("LitPasses: the frontend on a tree of two components (LP4.1, C33 a)")
{
  auto f = frontendFixture();
  // two areas split by x = 0 and no portal: area 0 east (x > 0), area 1 west, each its
  // own component; the area models' bounds as a .bproc would store them
  auto tree = ProcTree{};
  tree.numAreas = 2;
  tree.nodes.push_back(ProcNode{{1, 0, 0, 0}, {-1, -2}});
  for (const auto area : {0, 1})
  {
    auto model = ProcModel{};
    model.name = fmt::format("_area{}", area);
    model.area = area;
    model.bounds = area == 0
                     ? std::array<EngineVec3, 2>{{{0, -128, 0}, {256, 128, 128}}}
                     : std::array<EngineVec3, 2>{{{-256, -128, 0}, {0, 128, 128}}};
    model.boundsKnown = true;
    tree.models.push_back(model);
  }
  const auto areas = buildAreaData(tree);
  REQUIRE(areas.components == std::vector<int>{0, 1});
  constexpr auto World = PreviewId{1};

  auto scene = FlowScene{};
  scene.tree = &tree;
  scene.areas = &areas;
  addChunk(scene, 0, World, {20, -20, 20}, {30, 20, 60});
  addChunk(scene, 1, World, {-30, -20, 20}, {-20, 20, 60}, "textures/lp1b/floor");
  // both lights stand in area 1 and reach area 0 (x -250..150); one casts shadows (the
  // stand-in shader has no noshadows, nor has the light)
  addLight(
    scene,
    *f,
    10,
    {{"origin", "-50 0 48"}, {"light_radius", "200 200 200"}, {"noshadows", "1"}});
  addLight(scene, *f, 11, {{"origin", "-50 0 48"}, {"light_radius", "200 200 200"}});
  for (const auto id : {10, 11})
  {
    const auto& record = *scene.lights.find(PreviewId(id));
    REQUIRE(record.derived.areaNum == 1);
    REQUIRE(
      std::set<int>{record.derived.areas.begin(), record.derived.areas.end()}
      == std::set<int>{0, 1});
  }
  REQUIRE(lightCastsShadows(*scene.lights.find(11)).value_or(false));

  // from area 0 looking west: the non-shadowing light is dropped (r_useLightAreaCulling),
  // the shadowing one is in view but lights nothing of area 0 (not connected to its own
  // area), so it has no surface and is dropped from the lists
  const auto camera = cameraAt({100, 0, 48}, {-1, 0, 0});
  const auto lists = flowListsOf(*f, scene, camera);
  CHECK(lists.viewArea == 0);
  CHECK(lists.areasFlowed == 1u);
  CHECK(drawnOf(lists) == std::set<std::pair<int, PreviewId>>{{0, World}});
  CHECK(lists.lightsInView == 1u);
  CHECK(lists.lights.empty());

  // a brush entity through the dividing plane, referenced in both areas, seen from area
  // 0: the shadowing light lights it through area 1, its own (C33 b)
  constexpr auto Panel = PreviewId{20};
  addEntity(
    scene,
    Panel,
    {0, 0, 48},
    EngineBounds{EngineVec3{-16, -16, -16}, EngineVec3{16, 16, 16}},
    {0, 1});
  addChunk(scene, 0, Panel, {-16, -16, -16}, {16, 16, 16});
  const auto withPanel = flowListsOf(*f, scene, camera);
  CHECK(withPanel.lightsInView == 1u);
  CHECK_FALSE(viewLightOf(withPanel, 10));
  CHECK(
    litOf(viewLightOf(withPanel, 11)) == std::set<std::pair<int, PreviewId>>{{0, Panel}});
}

TEST_CASE("LitPasses: the frontend lists of the LP1b fixture (C27, A1, D67)")
{
  auto f = frontendFixture();
  REQUIRE(f->lightIds.size() == 8u);
  REQUIRE_FALSE(f->chunkMap.empty());

  // from the room's west end looking east, 128 × 96: fov_x 90, fov_y 73.74
  const auto lists = listsOf(*f, fixtureCamera(-240, 0, 64), 128, 96);
  CHECK_FALSE(lists.pending);

  // lp1b_light: the point light of radius 200 at (64 0 96), box x -136..264
  const auto* light = lightOf(lists, *f, "lp1b_light");
  REQUIRE(light);
  // its near face (x -136, 104 ahead) reaches past every side: the whole viewport
  CHECK(light->scissor.x1 == 0);
  CHECK(light->scissor.y1 == 0);
  CHECK(light->scissor.x2 == 127);
  CHECK(light->scissor.y2 == 95);
  // the largest light comes first
  CHECK(lists.lights.front().record->id == light->record->id);
  // the grate (perforated) is global, the lit glass translucent, nothing local (no
  // material has noSelfShadow); the fixture's glass receives no light (one blend stage)
  CHECK(light->localInteractions.empty());
  CHECK(
    materialsOf(light->translucentInteractions)
    == std::set<std::string>{"textures/lp2/litglass"});
  const auto global = materialsOf(light->globalInteractions);
  CHECK(global.contains("textures/lp1b/floor"));
  CHECK(global.contains("textures/lp1b/wall"));
  CHECK(global.contains("textures/lp1b/grate"));
  CHECK(global.contains("textures/lp1b/crate"));
  CHECK(global.contains("textures/lp1b/patch"));
  CHECK_FALSE(global.contains("textures/lp1b/glass"));
  // culled to the light volume: the west wall (x -272..-256), the box at x 400..432 and
  // the patchDef3 (x -224..-160) are outside x -136..264
  for (const auto& surface : light->globalInteractions)
  {
    const auto& bounds = surface.chunk->bounds;
    const auto entity = f->entities.find(surface.chunk->key.entity);
    if (entity == f->entities.end())
    {
      // worldspawn: the bounds are world coordinates
      CHECK(bounds[1][0] >= -136.0f);
      CHECK(bounds[0][0] <= 264.0f);
    }
  }

  // lp1b_small: the box x 168..216, y 40..88, z 40..88. Seen from x -240, its near face
  // 408 ahead: NDC x -88 / 408 .. -40 / 456 (screen right is -y), window
  // 0.39216..0.45614, × 127: 49.80..57.93; NDC y ±24 / 408 / 0.75, window
  // 0.46078..0.53922, × 95: 43.77..51.23; Ftoi and a pixel each way
  const auto* small = lightOf(lists, *f, "lp1b_small");
  REQUIRE(small);
  CHECK(small->scissor.x1 == 48);
  CHECK(small->scissor.x2 == 58);
  CHECK(small->scissor.y1 == 42);
  CHECK(small->scissor.y2 == 52);

  // the eight lights are texture lights; those with surfaces in reach are listed
  CHECK(lists.mathLights == 0u);
  CHECK(lists.fogAndBlendLights == 0u);
  for (const auto& viewLight : lists.lights)
  {
    CHECK_FALSE(viewLight.scissor.isEmpty());
    CHECK(
      (!viewLight.localInteractions.empty() || !viewLight.globalInteractions.empty()
       || !viewLight.translucentInteractions.empty()));
  }
  // sorted largest first
  for (std::size_t i = 1; i < lists.lights.size(); ++i)
  {
    CHECK(lists.lights[i - 1].scissor.area() >= lists.lights[i].scissor.area());
  }

  // the prepass: every visible drawn surface, the translucent glass among them (the
  // pass skips it), no visportal (not drawn)
  const auto drawn = materialsOf(lists.drawSurfs);
  CHECK(drawn.contains("textures/lp1b/glass"));
  CHECK_FALSE(drawn.contains("textures/editor/visportal"));

  // a light behind the camera: from x 230 looking east, lp1b_small (x 168..216) is
  // behind
  const auto turned = listsOf(*f, fixtureCamera(230, 0, 64), 128, 96);
  CHECK_FALSE(lightOf(turned, *f, "lp1b_small"));

  // a light enclosing the camera: inside lp1b_light its scissor is the whole viewport
  const auto inside = listsOf(*f, fixtureCamera(0, 0, 64), 128, 96);
  const auto* around = lightOf(inside, *f, "lp1b_light");
  REQUIRE(around);
  CHECK(around->scissor.x1 == 0);
  CHECK(around->scissor.x2 == 127);
  CHECK(around->scissor.y1 == 0);
  CHECK(around->scissor.y2 == 95);
}

TEST_CASE("LitPasses: the fog portals in the frontend's flow (LP5.3, C49, D123, P6.6)")
{
  // the LP1b fixture's tree: area 0 the east room (x 8..256), area 1 the west room, one
  // portal on x = 8 (y -32..32, z 0..96); lp1b_light (64 0 96, radius 200) holds it
  auto f = frontendFixture();
  const auto proc = f->lightInput.proc;
  REQUIRE(proc);
  const auto areas = buildAreaData(*proc);
  REQUIRE(areas.areas.size() == 2u);
  const auto westFrom = [](const float x) {
    auto camera = CameraState{};
    camera.position = vm::vec3f{x, 0, 64};
    camera.direction = vm::vec3f{-1, 0, 0};
    camera.up = vm::vec3f{0, 0, 1};
    camera.fovDegrees = 90.0f;
    return camera;
  };
  const auto withRecord = [&](const std::shared_ptr<const LightRecord>& record) {
    auto table = LightTable{};
    table.version = 2;
    f->lights.records.forEach([&](const PreviewId& id, const auto& other) {
      if (other)
      {
        table.records = table.records.set(id, id == record->id ? record : other);
      }
    });
    return table;
  };
  const auto listsWith = [&](
                           const LightTable& lights,
                           const PortalFogTable& portalFog,
                           const CameraState& camera,
                           const PreviewId withheld = NoPreviewId) {
    auto inputs = FrontendInputs{};
    inputs.view = litViewOf(camera, 128, 96);
    inputs.chunks = &f->chunkMap;
    inputs.entities = &f->entities;
    inputs.lights = &lights;
    inputs.tree = proc.get();
    inputs.areas = &areas;
    inputs.portalFog = &portalFog;
    inputs.material = [&](const std::string& name) {
      const auto slot = f->table->find(name);
      return MaterialAnswer{slot ? slot->entry : nullptr, !slot};
    };
    inputs.registers = [&](
                         const PreviewId consumer,
                         const MaterialEntry& material,
                         const ShaderParms& parms) {
      return consumer == withheld ? RegisterAnswer{}
                                  : evaluatedRegisters(*f, material, parms);
    };
    return buildFrontendLists(inputs);
  };

  // a fog distance of 150 (shaderParm3 through the fog material's colored parm3)
  const auto fog = lightRecordWith(
    *f, "lp1b_light", {{"texture", "lights/lp51/fog"}, {"shaderParm3", "150"}});
  REQUIRE(fog->derived.shader.material);
  REQUIRE(fog->derived.shader.material->fogLight);
  const auto lights = withRecord(fog);
  const auto portalFog = buildPortalFogTable(lights, areas, lights.version);
  REQUIRE(portalFog.fogLights.size() == 1u);
  CHECK(portalFog.fogLights[0] == fog->id);

  SECTION("from 192 ahead the doorway is beyond the fog distance: area 1 is not flowed")
  {
    const auto lists = listsWith(lights, portalFog, westFrom(200));
    CHECK(lists.viewArea == 0);
    CHECK(lists.areasFlowed == 1u);
    CHECK(lists.portalsFogged >= 1u);
    CHECK_FALSE(lists.pending);
    // without the table the doorway is flowed
    const auto none = PortalFogTable{};
    CHECK(listsWith(lights, none, westFrom(200)).areasFlowed == 2u);
  }

  SECTION("a closer camera: flowed")
  {
    const auto lists = listsWith(lights, portalFog, westFrom(100));
    CHECK(lists.areasFlowed == 2u);
    CHECK(lists.portalsFogged == 0u);
  }

  SECTION("noPortalFog: the light assigns no portal, flowed")
  {
    auto noPortal = *fog;
    auto material = std::make_shared<MaterialEntry>(*fog->derived.shader.material);
    material->materialFlags |= engine_material::FlagNoPortalFog;
    noPortal.derived.shader.material = material;
    const auto noPortalLights =
      withRecord(std::make_shared<const LightRecord>(std::move(noPortal)));
    const auto table = buildPortalFogTable(noPortalLights, areas, noPortalLights.version);
    CHECK(table.fogLights[0] == NoPreviewId);
    CHECK(listsWith(noPortalLights, table, westFrom(200)).areasFlowed == 2u);
  }

  SECTION("the fog light's registers first-seen: the portal open, the frame unconverged")
  {
    const auto lists = listsWith(lights, portalFog, westFrom(200), fog->id);
    CHECK(lists.areasFlowed == 2u);
    CHECK(lists.portalFogPending == 1u);
    CHECK(lists.pending);
  }
}

TEST_CASE("LitPasses: math, ambientLight, fog and blend lights in the lists (LP5.1, C45)")
{
  auto f = frontendFixture();
  const auto camera = fixtureCamera(-240, 0, 64);
  const auto lightId = f->lightIds.at("lp1b_light");
  const auto otherLights = [&](const std::shared_ptr<const LightRecord>& record) {
    auto records = std::vector<std::shared_ptr<const LightRecord>>{};
    f->lights.records.forEach([&](const PreviewId& id, const auto& other) {
      if (other)
      {
        records.push_back(id == record->id ? record : other);
      }
    });
    return records;
  };
  const auto texture = listsOf(*f, camera, 128, 96);
  const auto* textureLight = lightOf(texture, *f, "lp1b_light");
  REQUIRE(textureLight);
  CHECK(texture.mathLights == 0u);
  CHECK(texture.ambientLights == 0u);

  SECTION("a math light takes the normal path: the snap, the scissor, the lists")
  {
    // SC510, SC512: R_AddSingleLight treats it as any light; its lists are the texture
    // light's, its scissor too; c_mathViewLights counts it (tr_frontend_addlights.cpp:
    // 740-746)
    const auto record = lightRecordWith(*f, "lp1b_light", {{"math_enabled", "1"}});
    REQUIRE(engineLightUsesMathPath(record->light.parms));
    const auto lights = lightTableOf(otherLights(record), 2);
    const auto lists = listsOfLights(*f, lights, camera, 128, 96);
    const auto* math = lightOf(lists, *f, "lp1b_light");
    REQUIRE(math);
    CHECK(math->scissor.equals(textureLight->scissor));
    CHECK(
      materialsOf(math->globalInteractions)
      == materialsOf(textureLight->globalInteractions));
    CHECK(
      materialsOf(math->translucentInteractions)
      == materialsOf(textureLight->translucentInteractions));
    CHECK(lists.mathLights == 1u);
    CHECK(lists.ambientLights == 0u);
    CHECK(lists.lightsInView == texture.lightsInView);
    // a math light whose colour is black is snapped out as any light
    const auto black = lightRecordWith(
      *f, "lp1b_light", {{"math_enabled", "1"}, {"_color", "0.0005 0 0"}});
    const auto blackLists =
      listsOfLights(*f, lightTableOf(otherLights(black), 3), camera, 128, 96);
    CHECK_FALSE(lightOf(blackLists, *f, "lp1b_light"));
    CHECK(blackLists.mathLights == 0u);
  }

  SECTION("an ambientLight light takes the normal path; it casts no shadows")
  {
    const auto record =
      lightRecordWith(*f, "lp1b_light", {{"texture", "lights/lp51/ambient"}});
    REQUIRE(record->derived.shader.material);
    REQUIRE(record->derived.shader.material->ambientLight);
    CHECK(lightCastsShadows(*record) == std::optional<bool>{false});
    const auto lists =
      listsOfLights(*f, lightTableOf(otherLights(record), 2), camera, 128, 96);
    const auto* ambient = lightOf(lists, *f, "lp1b_light");
    REQUIRE(ambient);
    CHECK(ambient->scissor.equals(textureLight->scissor));
    CHECK(
      materialsOf(ambient->globalInteractions)
      == materialsOf(textureLight->globalInteractions));
    CHECK(lists.ambientLights == 1u);
    CHECK(lists.mathLights == 0u);
    // and as a math light: both counts
    const auto both = lightRecordWith(
      *f, "lp1b_light", {{"texture", "lights/lp51/ambient"}, {"math_enabled", "1"}});
    const auto bothLists =
      listsOfLights(*f, lightTableOf(otherLights(both), 3), camera, 128, 96);
    CHECK(lightOf(bothLists, *f, "lp1b_light"));
    CHECK(bothLists.ambientLights == 1u);
    CHECK(bothLists.mathLights == 1u);
  }

  SECTION("fog and blend lights take the lists as any light, unsnapped (LP5.3, C49)")
  {
    for (const auto& [shader, kind] :
         {std::pair{"lights/lp51/fog", ViewLightKind::Fog},
          std::pair{"lights/lp51/blend", ViewLightKind::Blend}})
    {
      CAPTURE(shader);
      const auto record = lightRecordWith(*f, "lp1b_light", {{"texture", shader}});
      REQUIRE(record->derived.shader.material);
      const auto lists =
        listsOfLights(*f, lightTableOf(otherLights(record), 2), camera, 128, 96);
      const auto* light = lightOf(lists, *f, "lp1b_light");
      REQUIRE(light);
      CHECK(light->kind == kind);
      // FAM:949-1008: the surfaces that receive lighting, as the texture light's (the
      // dead noFog / ReceivesFog not asked), the same scissor
      CHECK(light->scissor.equals(textureLight->scissor));
      CHECK(
        materialsOf(light->globalInteractions)
        == materialsOf(textureLight->globalInteractions));
      CHECK(
        materialsOf(light->translucentInteractions)
        == materialsOf(textureLight->translucentInteractions));
      CHECK(lists.fogAndBlendLights == 1u);
      CHECK(lists.lightsInView == texture.lightsInView);
      // the fog plane (FAL:212-221): the point light's far plane z = 96 + 200, from
      // baseLightProject row 2 ( 0, 0, 0.5 / 200, 0.5 - 0.5 · 96 / 200 ) less row 3 ( 0,
      // 0, 0, 1 ), normalised: ( 0, 0, 1, -296 )
      CHECK(light->fogPlane.a == Catch::Approx(0.0f).margin(1e-6));
      CHECK(light->fogPlane.b == Catch::Approx(0.0f).margin(1e-6));
      CHECK(light->fogPlane.c == Catch::Approx(1.0f).margin(1e-6));
      CHECK(light->fogPlane.d == Catch::Approx(-296.0f).epsilon(1e-6));
      // a texture light's kind and plane (every light gets the plane)
      CHECK(textureLight->kind == ViewLightKind::Texture);
      CHECK(textureLight->fogPlane.d == Catch::Approx(-296.0f).epsilon(1e-6));

      // no snap (FAL:155): a black fog or blend light stays, where a texture light goes
      const auto black =
        lightRecordWith(*f, "lp1b_light", {{"texture", shader}, {"_color", "0 0 0"}});
      const auto blackLists =
        listsOfLights(*f, lightTableOf(otherLights(black), 3), camera, 128, 96);
      const auto* blackLight = lightOf(blackLists, *f, "lp1b_light");
      REQUIRE(blackLight);
      CHECK(blackLists.fogAndBlendLights == 1u);

      // the view's scissor away from the light's rect: an empty scissor, no surface, and
      // the light kept (FAL:727-735, FAM:362-367: it contacts no entity); a texture
      // light is dropped so
      auto inputsCorner = ScreenRect{};
      inputsCorner.x1 = 120;
      inputsCorner.y1 = 0;
      inputsCorner.x2 = 127;
      inputsCorner.y2 = 3;
      // lp1b_small's rect is x 48..58, y 42..52 (the fixture's light test)
      const auto small = lightRecordWith(*f, "lp1b_small", {{"texture", shader}});
      const auto cornerLists = listsWithScissor(
        *f, lightTableOf(otherLights(small), 4), camera, 128, 96, inputsCorner);
      const auto* corner = lightOf(cornerLists, *f, "lp1b_small");
      REQUIRE(corner);
      CHECK(corner->scissor.isEmpty());
      CHECK(corner->localInteractions.empty());
      CHECK(corner->globalInteractions.empty());
      CHECK(corner->translucentInteractions.empty());
      CHECK(cornerLists.fogAndBlendLights == 1u);
      const auto textureCorner =
        listsWithScissor(*f, f->lights, camera, 128, 96, inputsCorner);
      CHECK_FALSE(lightOf(textureCorner, *f, "lp1b_small"));
    }
  }

  SECTION("the light sort's area is GetArea's arithmetic, an empty rect's too (P6.23)")
  {
    // idScreenRect::GetArea ( x2 - x1 + 1 ) · ( y2 - y1 + 1 ): an empty rect inverted on
    // both axes is positive, on one axis negative
    const auto rect = [](const short x1, const short y1, const short x2, const short y2) {
      auto r = ScreenRect{};
      r.x1 = x1;
      r.y1 = y1;
      r.x2 = x2;
      r.y2 = y2;
      return r;
    };
    auto lights = std::vector<ViewLight>(3);
    lights[0].scissor = rect(0, 0, 9, 9);
    lights[1].scissor = rect(50, 60, 20, 30);
    lights[2].scissor = rect(50, 0, 20, 9);
    CHECK(lights[0].scissor.area() == 100);
    CHECK(lights[1].scissor.isEmpty());
    CHECK(lights[1].scissor.area() == (20 - 50 + 1) * (30 - 60 + 1));
    CHECK(lights[1].scissor.area() == 841);
    CHECK(lights[2].scissor.area() == -290);
    lights[0].fogPlane.d = 0.0f;
    lights[1].fogPlane.d = 1.0f;
    lights[2].fogPlane.d = 2.0f;
    engineSortViewLights(lights);
    // largest first: the doubly inverted empty rect before the real one, the singly
    // inverted after every light
    CHECK(lights[0].fogPlane.d == 1.0f);
    CHECK(lights[1].fogPlane.d == 0.0f);
    CHECK(lights[2].fogPlane.d == 2.0f);
  }

  SECTION("the local list of a light without noShadows (FAM:996-1002, LP5.3)")
  {
    // MC_TRANSLUCENT first; then !noShadows && MF_NOSELFSHADOW: local; else global, a fog
    // or blend light's list as any light's
    auto material = MaterialEntry{};
    material.materialFlags = engine_material::FlagNoSelfShadow;
    CHECK(engineInteractionList(material, false) == InteractionList::Local);
    CHECK(engineInteractionList(material, true) == InteractionList::Global);
    material.coverage = engine_material::CoverageTranslucent;
    CHECK(engineInteractionList(material, false) == InteractionList::Translucent);
    material = MaterialEntry{};
    CHECK(engineInteractionList(material, false) == InteractionList::Global);
  }

  SECTION("a first-seen light without registers is skipped, a kept one drawn (D52)")
  {
    const auto record = lightRecordWith(*f, "lp1b_light", {{"math_enabled", "1"}});
    const auto lights = lightTableOf(otherLights(record), 2);
    const auto none = [&](
                        const PreviewId consumer,
                        const MaterialEntry& material,
                        const ShaderParms& parms) {
      return consumer == lightId ? RegisterAnswer{}
                                 : evaluatedRegisters(*f, material, parms);
    };
    const auto skipped = listsOfLights(*f, lights, camera, 128, 96, none);
    CHECK_FALSE(lightOf(skipped, *f, "lp1b_light"));
    CHECK(skipped.pending);
    const auto kept = [&](
                        const PreviewId consumer,
                        const MaterialEntry& material,
                        const ShaderParms& parms) {
      auto answer = evaluatedRegisters(*f, material, parms);
      answer.current = consumer != lightId;
      return answer;
    };
    const auto drawn = listsOfLights(*f, lights, camera, 128, 96, kept);
    const auto* light = lightOf(drawn, *f, "lp1b_light");
    REQUIRE(light);
    CHECK(light->provisional);
    CHECK(drawn.pending);
  }
}

TEST_CASE("LitPasses: R_AddSingleLight's register snap and the math light test")
{
  auto shader = MaterialEntry{};
  auto stage = MaterialStage{};
  stage.conditionRegister = 1;
  stage.colorRegisters = {2, 3, 4, 1};
  shader.stages = {stage};
  // below 0.001 snaps to 0: no stage adds light
  auto registers = std::vector<float>{0.0f, 1.0f, 0.0005f, 0.0f, -1.0f};
  CHECK_FALSE(engineSnapLightRegisters(shader, registers));
  CHECK(registers[2] == 0.0f);
  CHECK(registers[4] == 0.0f);
  registers = {0.0f, 1.0f, 0.0f, 0.25f, 0.0f};
  CHECK(engineSnapLightRegisters(shader, registers));
  // a stage conditioned off adds nothing
  registers = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  shader.stages[0].conditionRegister = 0;
  CHECK_FALSE(engineSnapLightRegisters(shader, registers));

  auto parms = RenderLightParms{};
  CHECK_FALSE(engineLightUsesMathPath(parms));
  parms.mathLight.enabled = true;
  CHECK(engineLightUsesMathPath(parms));

  // USER0-2 of a texture light: mlMode 0, the math data as parsed
  auto math = MathLightParms{};
  math.type = 1;
  math.falloffExp = 2.0f;
  math.energyClamp = true;
  const auto user = engineMathLightParms(math, false);
  CHECK(user[0] == Vec4{0, 1, 0, 0});
  CHECK(user[1] == Vec4{2, 0, 0, 0});
  CHECK(user[2] == Vec4{0, 0, 0, 1});
  // LP5.1 (C45): a math light's mlMode is 1 + r_showMathLights (0), its fields as
  // parsed (RenderBackend.cpp:1934-1965)
  const auto mathUser = engineMathLightParms(math, true);
  CHECK(mathUser[0] == Vec4{1, 1, 0, 0});
  CHECK(mathUser[1] == user[1]);
  CHECK(mathUser[2] == user[2]);

  // LP5.1 (C45, D112): units 3 and 4 of a math light and a texture light
  {
    const auto falloff = imageKey("falloff", engine_image::UsageLight);
    const auto projection = imageKey("projection", engine_image::UsageLight);
    auto lightStage = stageOf(em::LightingDiffuse, projection);
    // a texture light: its falloff and its stage's image
    const auto textureUnits = engineLightStageUnits(false, falloff, lightStage);
    REQUIRE(textureUnits);
    CHECK((*textureUnits)[0] == falloff);
    CHECK((*textureUnits)[1] == projection);
    // a math light: _white at both (RenderBackend.cpp:1914-1921)
    const auto mathUnits = engineLightStageUnits(true, falloff, lightStage);
    REQUIRE(mathUnits);
    CHECK((*mathUnits)[0] == engineWhiteImageKey());
    CHECK((*mathUnits)[1] == engineWhiteImageKey());
    // M-PASS-GUARD-01: a stage without an image (a cinematic's) is not drawn for a
    // texture light, and is for a math light, which never reads it
    lightStage.image.reset();
    CHECK_FALSE(engineLightStageUnits(false, falloff, lightStage));
    CHECK(engineLightStageUnits(true, falloff, lightStage));
  }
}

TEST_CASE("LitPasses: the lists' table key moves once per re-parse (LPF.0b, C43, D106)")
{
  using namespace std::chrono_literals;
  auto options = MaterialServiceOptions{};
  options.makeSource = [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(materialDeclsFixture())};
  };
  options.pollInterval = 0ms;
  auto service = MaterialService::create(std::move(options)) | kdl::value();
  const auto handedOver = [&] {
    const auto requested = service->feedRequested();
    service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.feedApplied >= requested; }, 10s);
    REQUIRE(service->waitForStatsForTesting([](const auto& s) { return s.idle; }, 10s));
    return service->table();
  };
  static_cast<void>(service->lookup("textures/lp1a/wall"));
  REQUIRE(service->waitForTableForTesting(
    [](const MaterialTable& t) {
      const auto slot = t.find("textures/lp1a/wall");
      return slot && slot->sizeMeasured;
    },
    10s));
  REQUIRE(service->waitForStatsForTesting([](const auto& s) { return s.idle; }, 10s));
  const auto before = ListsTableKey::of(service->table().get());

  // a re-parse moves it once: its publication and the measure after it share one key
  REQUIRE(service->reparseMaterial("textures/lp1a/wall", "{ diffusemap textures/lp1a/x }")
            .is_success());
  const auto after = ListsTableKey::of(handedOver().get());
  CHECK(after.registry == before.registry);
  CHECK(after.materialsRevision == before.materialsRevision + 1);

  // an image override moves it not
  auto image = ImageOverride{};
  auto pixels = RawImagePixels{};
  pixels.width = 2;
  pixels.height = 2;
  pixels.bytes.assign(16, 100);
  image.image = std::move(pixels);
  REQUIRE(service->setImageOverride("textures/lp1a/x", std::move(image)).is_success());
  const auto overridden = handedOver();
  CHECK(ListsTableKey::of(overridden.get()) == after);
  CHECK(ListsTableKey::of(nullptr) == ListsTableKey{});
  service->requestStop();
  service->join();
}

TEST_CASE("LitPasses: RegisterCache keeps registers across a re-parse that fits (D52)")
{
  // LPF.0b (C43, P5.27): the service erases a re-parsed material's register deltas; the
  // cache answers with the kept registers while they cover the material's count
  auto material = MaterialEntry{};
  material.name = "textures/reparsed";
  material.registry = 1;
  material.index = 0;
  material.numRegisters = 3;
  auto table = MaterialTable{};
  auto registers = std::make_shared<EvaluatedRegisters>();
  registers->registers = {1.0f, 0.5f, 0.25f};
  table.registers = table.registers.set(
    RegisterKey{MaterialId::of(material), ShaderParms{}, 0.0f}, registers);
  auto cache = RegisterCache{};
  cache.beginBuild();
  const auto first =
    cache.registers(PreviewId{1}, material, ShaderParms{}, table, nullptr);
  CHECK(first.current);
  CHECK(first.registers == registers);
  cache.endBuild();

  // re-parse 1: the same count, no evaluation yet: the kept registers, no blink
  const auto erased = MaterialTable{};
  material.revision = 1;
  cache.beginBuild();
  const auto kept =
    cache.registers(PreviewId{1}, material, ShaderParms{}, erased, nullptr);
  CHECK_FALSE(kept.current);
  CHECK(kept.registers == registers);
  cache.endBuild();

  // re-parse 2: more registers than the kept ones cover: none until the evaluation lands
  material.revision = 2;
  material.numRegisters = 8;
  cache.beginBuild();
  const auto dropped =
    cache.registers(PreviewId{1}, material, ShaderParms{}, erased, nullptr);
  CHECK_FALSE(dropped.current);
  CHECK_FALSE(dropped.registers);
  auto grown = MaterialTable{};
  auto eight = std::make_shared<EvaluatedRegisters>();
  eight->registers = std::vector<float>(8, 1.0f);
  grown.registers = grown.registers.set(
    RegisterKey{MaterialId::of(material), ShaderParms{}, 0.0f}, eight);
  const auto landed =
    cache.registers(PreviewId{1}, material, ShaderParms{}, grown, nullptr);
  CHECK(landed.current);
  CHECK(landed.registers == eight);
  cache.endBuild();
}

namespace
{

// LP5.1 (C50, P6.7, R5.16): the light kinds the sweep steps
enum class SweepKind
{
  Texture,
  Math,
  Ambient,
};

/**
 * The 60-step light-colour sweep of one light kind over the fixture's lp1b_light: per
 * step its _color is the step's (the record built again, as a drag rebuilds it), the
 * lists are built with a RegisterCache over a fake service's table, which publishes the
 * step's evaluation but on the withheld steps (every second one, and the run 40-49); a
 * mutant (TB_LIT_SWEEP_MUTANT=1 in the environment: a test-only fake, off by default)
 * answers a withheld step with the default registers (the light's colour 1 1 1) instead.
 * What fails, step by step: the light missing from the lists or without registers, its
 * provisional flag not the withheld one, its colour not a requested step or not the
 * last answered one on a withheld step, a unit of its stages _default, a surface of its
 * lists without its material or registers (pendingSurfaceInteraction's _default), the
 * light not of its kind.
 */
std::vector<std::string> sweepProblems(
  FrontendFixture& f, const SweepKind kind, const bool mutant)
{
  const auto name = std::string{"lp1b_light"};
  const auto lightId = f.lightIds.at(name);
  const auto camera = fixtureCamera(-240, 0, 64);
  // the fake service's published evaluations, and the render thread's cache over them
  auto published = MaterialTable{};
  auto cache = RegisterCache{};
  auto requested = std::vector<std::array<float, 3>>{};
  auto answered = std::optional<std::array<float, 3>>{};
  auto problems = std::vector<std::string>{};
  for (auto step = 0; step < 60; ++step)
  {
    const auto colour = std::array<float, 3>{
      0.20f + 0.01f * float(step),
      0.15f + 0.01f * float(step),
      0.10f + 0.01f * float(step)};
    const auto withheld = step % 2 == 1 || (step >= 40 && step < 50);
    auto changes = std::vector<std::pair<std::string, std::string>>{
      {"_color", fmt::format("{} {} {}", colour[0], colour[1], colour[2])}};
    if (kind == SweepKind::Math)
    {
      changes.emplace_back("math_enabled", "1");
    }
    if (kind == SweepKind::Ambient)
    {
      changes.emplace_back("texture", "lights/lp51/ambient");
    }
    const auto record = lightRecordWith(f, name, changes);
    const auto& shader = record->derived.shader.material;
    REQUIRE(shader);
    const auto& parms = record->light.parms.shaderParms;
    requested.push_back(colour);
    if (!withheld)
    {
      // the service answers this step's evaluation
      published.registers = published.registers.set(
        RegisterKey{MaterialId::of(*shader), parms, 0.0f},
        evaluatedRegisters(f, *shader, parms).registers);
    }
    const auto registers = [&](
                             const PreviewId consumer,
                             const MaterialEntry& material,
                             const ShaderParms& asked) {
      if (consumer != lightId)
      {
        return evaluatedRegisters(f, material, asked);
      }
      if (mutant && withheld)
      {
        // the mutant: the default registers where the kept ones belong
        auto defaults = ShaderParms{};
        defaults[0] = defaults[1] = defaults[2] = defaults[3] = 1.0f;
        return evaluatedRegisters(f, material, defaults);
      }
      return cache.registers(consumer, material, asked, published, nullptr);
    };
    cache.beginBuild();
    const auto lists = listsOfLights(
      f, lightTableOf({record}, std::uint64_t(step) + 2), camera, 128, 96, registers);
    cache.endBuild();

    const auto at = fmt::format("step {} ({})", step, withheld ? "withheld" : "answered");
    const auto* light = lightOf(lists, f, name);
    if (!light)
    {
      problems.push_back(at + ": the light is not in the lists");
      continue;
    }
    if (light->registers.empty())
    {
      problems.push_back(at + ": the light has no registers");
      continue;
    }
    if (light->provisional != withheld)
    {
      problems.push_back(at + fmt::format(": provisional {}", light->provisional));
    }
    // the colour its first stage draws with (the stand-ins' stages read parm0-parm2)
    const auto& stage = light->shader->stages.front();
    const auto drawn = std::array<float, 3>{
      engineRegister(light->registers, stage.colorRegisters[0]),
      engineRegister(light->registers, stage.colorRegisters[1]),
      engineRegister(light->registers, stage.colorRegisters[2])};
    if (std::ranges::find(requested, drawn) == requested.end())
    {
      problems.push_back(
        at
        + fmt::format(
          ": colour {} {} {} is no requested step", drawn[0], drawn[1], drawn[2]));
    }
    const auto expected = withheld ? answered : std::optional{colour};
    if (!expected || drawn != *expected)
    {
      problems.push_back(
        at
        + fmt::format(
          ": colour {} {} {}, not the last answered step's",
          drawn[0],
          drawn[1],
          drawn[2]));
    }
    // no draw of its stages binds _default: its units 3 and 4, and every surface it
    // lights with its material and registers
    const auto mathPath = engineLightUsesMathPath(light->record->light.parms);
    for (const auto& lightStage : light->shader->stages)
    {
      if (
        const auto units = engineLightStageUnits(
          mathPath, light->record->derived.shader.falloffImage, lightStage))
      {
        for (const auto& unit : *units)
        {
          if (unit == engineDefaultImageKey())
          {
            problems.push_back(at + ": a light stage binds _default");
          }
        }
      }
    }
    for (const auto* list :
         {&light->localInteractions,
          &light->globalInteractions,
          &light->translucentInteractions})
    {
      for (const auto& surface : *list)
      {
        if (!surface.material || !surface.registers)
        {
          problems.push_back(at + ": a surface drawn with _default");
        }
      }
    }
    if (mathPath != (kind == SweepKind::Math))
    {
      problems.push_back(at + ": the math path is not the kind's");
    }
    if (light->shader->ambientLight != (kind == SweepKind::Ambient))
    {
      problems.push_back(at + ": ambientLight is not the kind's");
    }
    if (!withheld)
    {
      answered = colour;
    }
  }
  return problems;
}

} // namespace

TEST_CASE(
  "LitPasses: the 60-step light-colour sweep, evaluations lagging (LP5.1, C50, P6.7)")
{
  // R5.16 (00-PLAN.md section 6): no frame of a colour drag shows a default; the mutant
  // (TB_LIT_SWEEP_MUTANT=1, a test-only fake answering withheld steps with the defaults)
  // must fail every kind's section
  const auto* mutantValue = std::getenv("TB_LIT_SWEEP_MUTANT");
  const auto mutant = mutantValue && std::string_view{mutantValue} == "1";
  auto f = frontendFixture();
  const auto check = [&](const SweepKind kind) {
    const auto problems = sweepProblems(*f, kind, mutant);
    // the first problems, and how many there are
    auto shown = std::vector<std::string>{
      problems.begin(),
      problems.begin() + std::ptrdiff_t(std::min(problems.size(), std::size_t{6}))};
    INFO(fmt::format("{} problems: {}", problems.size(), fmt::join(shown, "; ")));
    CHECK(problems.empty());
  };

  SECTION("a texture light")
  {
    check(SweepKind::Texture);
  }

  SECTION("a math light")
  {
    check(SweepKind::Math);
  }

  SECTION("an ambientLight light")
  {
    check(SweepKind::Ambient);
  }
}

// the sweep's fog and blend kinds (C50): LP5.3's, which draws them (FogAllLights)
TEST_CASE("LitPasses: the 60-step light-colour sweep of fog and blend lights (LP5.3)")
{
  SECTION("a fog light")
  {
    SKIP("LP5.3: fog lights are drawn by FogAllLights, which LP5.3 adds (C49)");
  }

  SECTION("a blend light")
  {
    SKIP("LP5.3: blend lights are drawn by FogAllLights, which LP5.3 adds (C49)");
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
// U2.4: RegisterCache over 100 list builds, each asking for consumers of which half
// change every build, leaves the CRT's debug heap as two warm-up builds left it
// (consumers not asked for in a build are forgotten at its end)
TEST_CASE("LitPasses: RegisterCache over 100 builds on the debug heap (U2.4)")
{
  auto table = MaterialTable{};
  auto materials = std::vector<MaterialEntry>{};
  for (auto i = 0; i < 6; ++i)
  {
    auto material = MaterialEntry{};
    material.name = "textures/heap" + std::to_string(i);
    material.registry = 1;
    material.index = i;
    material.numRegisters = 3;
    materials.push_back(material);
    auto registers = std::make_shared<EvaluatedRegisters>();
    registers->registers = {1.0f, 0.5f, float(i)};
    table.registers = table.registers.set(
      RegisterKey{MaterialId::of(material), ShaderParms{}, 0.0f}, registers);
  }
  auto cache = RegisterCache{};
  auto build = PreviewId{0};
  const auto cycle = [&] {
    ++build;
    cache.beginBuild();
    for (const auto& material : materials)
    {
      // a consumer kept for every build, and one that a build asks for once
      cache.registers(PreviewId{1}, material, ShaderParms{}, table, nullptr);
      cache.registers(PreviewId{1000 + build}, material, ShaderParms{}, table, nullptr);
    }
    cache.endBuild();
    cache.beginBuild();
    for (const auto& material : materials)
    {
      cache.registers(PreviewId{1}, material, ShaderParms{}, table, nullptr);
    }
    cache.endBuild();
  };
  cycle();
  cycle();
  CHECK_FALSE(heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      cycle();
    }
  }));
  CHECK(cache.size() == materials.size());
  CHECK(
    cache.registers(PreviewId{1}, materials[0], ShaderParms{}, table, nullptr).current);
}
#endif

} // namespace tb::lit
