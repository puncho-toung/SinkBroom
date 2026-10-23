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
#include "lit/ImageCache.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/LightRecord.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
    // GLOBALEYEPOS is in none of the mirrored sets
    for (const auto layout :
         {EngineBindingLayout::ConstantBufferOnly,
          EngineBindingLayout::Texture,
          EngineBindingLayout::DrawInteraction})
    {
      parms.clearChanged(layout);
    }
    parms.set(RenderParm::GlobalEyePos, {1, 2, 3, 1});
    CHECK_FALSE(parms.changed(EngineBindingLayout::DrawInteraction));
    CHECK(engineLayoutsOfParm(RenderParm::GlobalEyePos) == 0u);
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
};

const auto LightStandIns = std::string{
  "lights/squarelight1\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultPointLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"};

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
        "lights/lp1b_missing"})
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
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return f;
}

FrontendLists listsOf(
  FrontendFixture& f, const CameraState& camera, int width, int height)
{
  auto inputs = FrontendInputs{};
  inputs.view = litViewOf(camera, std::uint32_t(width), std::uint32_t(height));
  inputs.chunks = &f.chunkMap;
  inputs.entities = &f.entities;
  inputs.lights = &f.lights;
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
