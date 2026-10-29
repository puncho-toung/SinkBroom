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
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_main.cpp and
// renderer/RenderBackend.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "LitFrontend.h"
#include "LitMapFixture.h"
#include "LitStages.h"
#include "LitTestUtils.h"
#include "LitView.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineLight.h"
#include "lit/EnginePipelineState.h"
#include "lit/LitSession.h"
#include "lit/MaterialDecls.h"

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// a chunk of one axis-aligned box of bounds (min, max), keyed by material and part
Chunk boxChunk(
  const std::string& material,
  const EngineVec3& min,
  const EngineVec3& max,
  const int part = 0,
  const int area = 0)
{
  auto chunk = Chunk{};
  chunk.key.area = area;
  chunk.key.material = material;
  chunk.key.part = part;
  chunk.bounds = {min, max};
  return chunk;
}

std::shared_ptr<const MaterialEntry> sortedMaterial(const std::string& name, float sort)
{
  auto material = std::make_shared<MaterialEntry>();
  material->name = name;
  material->sort = sort;
  return material;
}

// the engine's R_SortDrawSurfs (tr_frontend_main.cpp:255-359) transcribed on its own: the
// 64-bit key of each surface in creation order, sorted descending (the engine's quicksort
// and selection sort leave the unique keys descending), and the surfaces read back from
// the low 16 bits
std::vector<const LitSurface*> engineOrderModel(
  const std::vector<const LitSurface*>& created, const std::vector<LitSpace>& spaces)
{
  const auto n = created.size();
  auto indices = std::vector<std::uint64_t>(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    const auto* surface = created[i];
    float sort = 100.0f - (surface->material ? surface->material->sort : 0.0f);
    auto dist = std::uint64_t{0};
    const auto depth =
      engineDepthBoundsForBounds(spaces[surface->space].mvp, surface->chunk->bounds);
    // idMath::Ftoui16: the truncation, clamped to 0..65535
    const auto i32 = int(depth[0] * 0xFFFF);
    dist = std::uint64_t(i32 < 0 ? 0 : i32 > 65535 ? 65535 : i32);
    auto bits = std::uint32_t{0};
    std::memcpy(&bits, &sort, sizeof(bits));
    indices[i] =
      ((std::uint64_t(n - i)) & 0xFFFF) | (dist << 16) | (std::uint64_t(bits) << 32);
  }
  std::ranges::sort(indices, std::greater<std::uint64_t>{});
  auto order = std::vector<const LitSurface*>{};
  for (const auto key : indices)
  {
    order.push_back(created[n - std::size_t(key & 0xFFFF)]);
  }
  return order;
}

} // namespace

TEST_CASE("LitStagePass: the stage pass's sorted list (LP5.2a, C46, D115, K37)")
{
  // a view from the origin looking down +x
  auto camera = CameraState{};
  camera.position = vm::vec3f{0, 0, 0};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.viewportWidth = 640;
  camera.viewportHeight = 480;
  const auto view = litViewOf(camera, 640, 480);
  const auto spaces = std::vector<LitSpace>{litSpaceOf(
    view,
    NoPreviewId,
    EngineVec3{0, 0, 0},
    EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1},
    engineWorldShaderParms())};

  // the chunks in ChunkKey order (the order buildFrontendLists makes surfaces in)
  auto chunks = std::map<ChunkKey, Chunk>{};
  const auto add = [&](Chunk chunk) {
    const auto key = chunk.key;
    chunks.emplace(key, std::move(chunk));
  };
  // a slab in front of the camera at x .. x + 4
  const auto slab = [](const float x) {
    return std::pair{EngineVec3{x, -32, -32}, EngineVec3{x + 4, 32, 32}};
  };
  const auto [nearMin, nearMax] = slab(50);
  const auto [midMin, midMax] = slab(120);
  const auto [farMin, farMax] = slab(300);
  add(boxChunk("a_opaque", midMin, midMax));
  add(boxChunk("b_medium_near", nearMin, nearMax));
  add(boxChunk("c_medium_far", farMin, farMax));
  // equal bounds and sort: creation (ChunkKey) order, the material name first, then the
  // part
  add(boxChunk("d_medium_tie", midMin, midMax, 1));
  add(boxChunk("d_medium_tie", midMin, midMax, 0));
  add(boxChunk("e_decal", nearMin, nearMax));
  add(boxChunk("f_post", nearMin, nearMax));
  add(boxChunk("g_subview", farMin, farMax));
  // a sort above SS_POST_PROCESS: its key is a negative float, whose bits sort first
  add(boxChunk("h_beyond", farMin, farMax));
  // a pending material sorts as SS_OPAQUE
  add(boxChunk("i_pending", farMin, farMax));
  // across the eye: a corner behind it gives the near depth 0, the nearest key
  add(boxChunk("j_medium_across", EngineVec3{-16, -8, -8}, EngineVec3{200, 8, 8}));

  const auto sorts = std::map<std::string, float>{
    {"a_opaque", 0.0f},
    {"b_medium_near", 4.0f},
    {"c_medium_far", 4.0f},
    {"d_medium_tie", 4.0f},
    {"e_decal", 2.0f},
    {"f_post", 100.0f},
    {"g_subview", -3.0f},
    {"h_beyond", 150.0f},
    {"j_medium_across", 4.0f},
  };
  auto drawSurfs = std::vector<LitSurface>{};
  for (const auto& [key, chunk] : chunks)
  {
    auto surface = LitSurface{};
    surface.chunk = &chunk;
    surface.space = 0;
    if (const auto it = sorts.find(key.material); it != sorts.end())
    {
      surface.material = sortedMaterial(key.material, it->second);
    }
    drawSurfs.push_back(surface);
  }

  const auto order = stagePassOrder(drawSurfs, spaces);
  REQUIRE(order.size() == drawSurfs.size());

  SECTION("the levels: the sort value, then the farthest first, then ChunkKey's order")
  {
    auto names = std::vector<std::string>{};
    for (const auto* surface : order)
    {
      names.push_back(
        surface->chunk->key.material + "#" + std::to_string(surface->chunk->key.part));
    }
    CHECK(
      names
      == std::vector<std::string>{
        "h_beyond#0",
        "g_subview#0",
        "i_pending#0",
        "a_opaque#0",
        "e_decal#0",
        "c_medium_far#0",
        "d_medium_tie#0",
        "d_medium_tie#1",
        "b_medium_near#0",
        "j_medium_across#0",
        "f_post#0",
      });
  }

  SECTION("the engine's key arithmetic gives the same order")
  {
    auto created = std::vector<const LitSurface*>{};
    for (const auto& surface : drawSurfs)
    {
      created.push_back(&surface);
    }
    CHECK(engineOrderModel(created, spaces) == order);
  }

  SECTION("the near depth: farther slabs have larger keys, one across the eye 0")
  {
    const auto depthOf = [&](const EngineVec3& min, const EngineVec3& max) {
      return engineFtoui16(
        engineDepthBoundsForBounds(spaces[0].mvp, EngineBounds{min, max})[0]
        * float(0xFFFF));
    };
    CHECK(depthOf(nearMin, nearMax) < depthOf(midMin, midMax));
    CHECK(depthOf(midMin, midMax) < depthOf(farMin, farMax));
    CHECK(depthOf(EngineVec3{-16, -8, -8}, EngineVec3{200, 8, 8}) == 0u);
    CHECK(engineFtoui16(-3.0f) == 0u);
    CHECK(engineFtoui16(65535.9f) == 65535u);
    CHECK(engineFtoui16(70000.0f) == 65535u);
    CHECK(engineFtoui16(12.75f) == 12u);
  }
}

TEST_CASE("LitStagePass: the sort's cost over a few thousand chunks", "[.][stagesort]")
{
  auto camera = CameraState{};
  camera.viewportWidth = 1280;
  camera.viewportHeight = 720;
  const auto view = litViewOf(camera, 1280, 720);
  const auto spaces = std::vector<LitSpace>{litSpaceOf(
    view,
    NoPreviewId,
    EngineVec3{0, 0, 0},
    EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1},
    engineWorldShaderParms())};
  auto chunks = std::map<ChunkKey, Chunk>{};
  const auto material = sortedMaterial("textures/x", 4.0f);
  for (int i = 0; i < 4096; ++i)
  {
    const auto x = float(16 + (i * 37) % 2000);
    const auto y = float((i * 53) % 800 - 400);
    auto chunk = boxChunk(
      "textures/m" + std::to_string(i % 64),
      EngineVec3{x, y, -16},
      EngineVec3{x + 32, y + 32, 16},
      i);
    const auto key = chunk.key;
    chunks.emplace(key, std::move(chunk));
  }
  auto drawSurfs = std::vector<LitSurface>{};
  for (const auto& [key, chunk] : chunks)
  {
    auto surface = LitSurface{};
    surface.chunk = &chunk;
    surface.material = material;
    drawSurfs.push_back(surface);
  }
  const auto start = std::chrono::steady_clock::now();
  constexpr auto Runs = 100;
  auto total = std::size_t{0};
  for (int run = 0; run < Runs; ++run)
  {
    total += stagePassOrder(drawSurfs, spaces).size();
  }
  const auto ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
  CHECK(total == drawSurfs.size() * Runs);
  std::cout << "stagePassOrder over " << drawSurfs.size() << " surfaces: " << ms / Runs
            << " ms\n";
}


namespace
{

namespace em = engine_material;
namespace gls = engine_gls;

// the stage-pass materials of the S rooms and of the [litgpu3] cases, through the copied
// parser
const char* const StageMaterials =
  "textures/lp5t/add { { blend add map textures/lp5t/glow rgb 2 } }\n"
  "textures/lp5t/blend { { blend blend map textures/lp5t/glow colored } }\n"
  "textures/lp5t/filter { { blend filter map textures/lp5t/glow } }\n"
  "textures/lp5t/black_add { { blend add map textures/lp5t/glow rgb 0 } }\n"
  "textures/lp5t/clear_blend { { blend blend map textures/lp5t/glow alpha 0 } }\n"
  "textures/lp5t/mask { { blend gl_zero, gl_one map textures/lp5t/glow } }\n"
  "textures/lp5t/inverse { { blend add map textures/lp5t/glow inverseVertexColor } }\n"
  "textures/lp5t/lit_and_add\n"
  "{\n"
  "  diffusemap textures/lp5t/wall\n"
  "  { blend add map textures/lp5t/glow }\n"
  "}\n"
  "textures/lp5t/alpha_ambient\n"
  "{\n"
  "  { map textures/lp5t/grate alphaTest 0.5 }\n"
  "}\n"
  "textures/lp5t/decal { polygonOffset 1 { blend blend map textures/lp5t/glow } }\n"
  "textures/lp5t/far { sort far { blend blend map textures/lp5t/glow "
  "privatePolygonOffset "
  "2 } }\n"
  "textures/lp5t/screen { { blend add map textures/lp5t/glow screen } }\n"
  "textures/lp5t/reflect { { blend add cubeMap env/lp5t texgen reflect } }\n"
  "textures/lp5t/bumpy_reflect\n"
  "{\n"
  "  translucent\n"
  "  bumpmap textures/lp5t/local\n"
  "  { blend add cubeMap env/lp5t texgen reflect }\n"
  "}\n"
  "textures/lp5t/reflect2 { { blend add cubeMap env/lp5t texgen reflect2 } }\n"
  "textures/lp5t/bumpy_reflect2\n"
  "{\n"
  "  translucent\n"
  "  bumpmap textures/lp5t/local\n"
  "  { blend add cubeMap env/lp5t texgen reflect2 }\n"
  "}\n"
  "textures/lp5t/skybox { { cubeMap env/lp5t texgen skybox } }\n"
  "textures/lp5t/wobble { { cubeMap env/lp5t texgen wobbleSky 0 0 0 } }\n"
  "textures/lp5t/wobble_tilted { { cubeMap env/lp5t texgen wobbleSky 30 2 + 1 6 } }\n"
  "textures/lp5t/normal_cube { { blend add map textures/lp5t/glow texgen normal } }\n"
  "textures/lp5t/glasswarp { { blend add map textures/lp5t/glow glassWarp } }\n"
  "textures/lp5t/movie { { blend add videomap video/lp5t.roq } }\n"
  "textures/lp5t/backbox { translucent backSided { blend blend map textures/lp5t/glow } "
  "}\n"
  "textures/lp5t/twobox { twoSided { blend blend map textures/lp5t/glow } }\n"
  "textures/lp5t/voronoi\n"
  "{\n"
  "  {\n"
  "    blend add\n"
  "    program vornoipattern\n"
  "    vertexParm 0 0.5, 400, 400\n"
  "  }\n"
  "}\n"
  "textures/lp5t/haze\n"
  "{\n"
  "  {\n"
  "    program heathaze\n"
  "    vertexParm 0 0.1, 0.2\n"
  "    fragmentMap 0 _currentRender\n"
  "    fragmentMap 1 textures/lp5t/haze_local\n"
  "  }\n"
  "}\n"
  "textures/lp5t/stencil\n"
  "{\n"
  "  {\n"
  "    blend blend\n"
  "    map textures/lp5t/glow\n"
  "    stencil { Ref 7 Comp Equal Pass Replace Fail Keep ZFail DecrWrap }\n"
  "  }\n"
  "}\n"
  "textures/lp5t/cond { { if parm4 blend add map textures/lp5t/glow } }\n";

std::shared_ptr<const MaterialEntry> parsed(MaterialDecls& decls, const std::string& name)
{
  auto result = decls.lookup(name);
  INFO(result.error);
  REQUIRE(result);
  return *result.value;
}

std::vector<float> registersOf(
  MaterialDecls& decls,
  const MaterialEntry& material,
  const std::array<float, MaterialDecls::ShaderParmCount>& parms)
{
  auto result = decls.evaluate(material, parms, 0.0f);
  INFO(result.error);
  REQUIRE(result);
  return result.value->registers;
}

constexpr auto WhiteParms = std::array<float, MaterialDecls::ShaderParmCount>{
  1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

std::uint64_t blendOf(const std::uint64_t state)
{
  return state & (gls::SrcBlendBits | gls::DstBlendBits);
}

} // namespace

TEST_CASE(
  "LitStagePass: DrawShaderPasses' stage decisions (LP5.2a, C46, C47, SC519-SC523)")
{
  auto source = MemoryDeclSource{{DeclFile{"materials/lp5t.mtr", StageMaterials, 0}}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto drawsOf = [&](const std::string& name, const auto& parms) {
    const auto material = parsed(decls, name);
    return engineShaderPassDraws(*material, registersOf(decls, *material, parms));
  };

  SECTION("blend add, blend and filter: one draw on texture_color, LESS | DEPTHMASK")
  {
    for (const auto* name :
         {"textures/lp5t/add", "textures/lp5t/blend", "textures/lp5t/filter"})
    {
      CAPTURE(name);
      const auto material = parsed(decls, name);
      // MAT:3129-3159: the first stage blended with the destination: translucent,
      // SS_MEDIUM (MAT:3183-3197)
      CHECK(material->coverage == em::CoverageTranslucent);
      CHECK(material->sort == 4.0f);
      const auto result = drawsOf(name, WhiteParms);
      REQUIRE(result.draws.size() == 1u);
      CHECK(result.skipped == 0u);
      const auto& draw = result.draws[0];
      CHECK(draw.program == StageProgram::TextureColor);
      // MAT:3232-3253: translucent: LESS | DEPTHMASK (no depth write)
      CHECK((draw.state & gls::DepthFuncBits) == gls::DepthFuncLess);
      CHECK((draw.state & gls::DepthMask) != 0);
      CHECK((draw.state & gls::CullBits) == gls::CullFrontSided);
      CHECK_FALSE(draw.screenTexgen);
    }
    CHECK(
      blendOf(drawsOf("textures/lp5t/add", WhiteParms).draws[0].state)
      == (gls::SrcBlendOne | gls::DstBlendOne));
    CHECK(
      blendOf(drawsOf("textures/lp5t/blend", WhiteParms).draws[0].state)
      == (gls::SrcBlendSrcAlpha | gls::DstBlendOneMinusSrcAlpha));
    // blend filter: DST_COLOR, ZERO
    CHECK(
      blendOf(drawsOf("textures/lp5t/filter", WhiteParms).draws[0].state)
      == (gls::SrcBlendDstColor | gls::DstBlendZero));
  }

  SECTION("GL_Color clamps the colour; colored reads the entity's parms 0-3")
  {
    // rgb 2: clamped to 1
    CHECK(
      drawsOf("textures/lp5t/add", WhiteParms).draws[0].color
      == Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    auto parms = WhiteParms;
    parms[0] = 0.5f;
    parms[1] = 0.25f;
    parms[2] = 1.0f;
    parms[3] = 0.75f;
    CHECK(
      drawsOf("textures/lp5t/blend", parms).draws[0].color
      == Vec4{0.5f, 0.25f, 1.0f, 0.75f});
  }

  SECTION("RB_SetVertexColorParms: inverseVertexColor is ( -1, 1 ), the default ( 0, 1 )")
  {
    const auto inverse = drawsOf("textures/lp5t/inverse", WhiteParms).draws[0];
    CHECK(inverse.vertexColor[0] == Vec4{-1.0f, -1.0f, -1.0f, -1.0f});
    CHECK(inverse.vertexColor[1] == Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    const auto plain = drawsOf("textures/lp5t/add", WhiteParms).draws[0];
    CHECK(plain.vertexColor[0] == Vec4{0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(plain.vertexColor[1] == Vec4{1.0f, 1.0f, 1.0f, 1.0f});
  }

  SECTION("the skips: black add, transparent blend, ZERO / ONE, the condition")
  {
    for (const auto* name :
         {"textures/lp5t/black_add",
          "textures/lp5t/clear_blend",
          "textures/lp5t/mask",
          "textures/lp5t/cond"})
    {
      CAPTURE(name);
      const auto result = drawsOf(name, WhiteParms);
      CHECK(result.draws.empty());
      CHECK(result.skipped == 1u);
    }
    // the condition register on: drawn
    auto parms = WhiteParms;
    parms[4] = 1.0f;
    CHECK(drawsOf("textures/lp5t/cond", parms).draws.size() == 1u);
  }

  SECTION(
    "an opaque material's ambient stage: EQUAL | DEPTHMASK; its lit stages are not "
    "the pass's")
  {
    const auto material = parsed(decls, "textures/lp5t/lit_and_add");
    CHECK(material->coverage == em::CoverageOpaque);
    CHECK(material->hasAmbient);
    const auto result = drawsOf("textures/lp5t/lit_and_add", WhiteParms);
    REQUIRE(result.draws.size() == 1u);
    CHECK(result.skipped == 0u);
    CHECK((result.draws[0].state & gls::DepthFuncBits) == gls::DepthFuncEqual);
    CHECK(
      material->stages[std::size_t(result.draws[0].stage)].lighting
      == em::LightingAmbient);
  }

  SECTION("an alpha-tested SL_AMBIENT stage: perforated, EQUAL, no ALPHA_TEST of its own")
  {
    const auto material = parsed(decls, "textures/lp5t/alpha_ambient");
    // one stage, not blended with the destination: opaque by MAT:3129-3159, yet
    // perforated by its alpha test (the prepass draws it alpha-tested)
    CHECK(material->coverage == em::CoveragePerforated);
    const auto result = drawsOf("textures/lp5t/alpha_ambient", WhiteParms);
    REQUIRE(result.draws.size() == 1u);
    CHECK((result.draws[0].state & gls::DepthFuncBits) == gls::DepthFuncEqual);
  }

  SECTION("polygonOffset and privatePolygonOffset")
  {
    const auto decal = drawsOf("textures/lp5t/decal", WhiteParms).draws[0];
    CHECK(parsed(decls, "textures/lp5t/decal")->sort == 2.0f);
    CHECK((decal.state & gls::PolygonOffset) != 0);
    CHECK(decal.slopeBias == 0.0f);
    CHECK(decal.depthBias == -600.0f);
    const auto far = drawsOf("textures/lp5t/far", WhiteParms).draws[0];
    CHECK(parsed(decls, "textures/lp5t/far")->sort == 3.0f);
    CHECK((far.state & gls::PolygonOffset) != 0);
    CHECK(far.depthBias == -1200.0f);
  }

  SECTION("the texgens: screen on 18, a cinematic not drawn")
  {
    const auto screen = drawsOf("textures/lp5t/screen", WhiteParms);
    REQUIRE(screen.draws.size() == 1u);
    CHECK(screen.draws[0].program == StageProgram::TextureColorTexgen);
    CHECK(screen.draws[0].screenTexgen);
    CHECK_FALSE(engineStageProgramSamplesCube(screen.draws[0].program));

    const auto movie = drawsOf("textures/lp5t/movie", WhiteParms);
    CHECK(movie.draws.empty());
    CHECK(movie.cinematic == 1u);
    CHECK(movie.skipped == 1u);
  }

  SECTION(
    "LP5.2b (C48): the cube and sky texgens on 55, 57, 69 and 70; reflect2 skipped "
    "without a bump stage, on 59 with one (LP6.5b); normal and glassWarp on 15 with a "
    "warning")
  {
    // TG_REFLECT_CUBE without a bump stage: environment (55)
    const auto reflect = drawsOf("textures/lp5t/reflect", WhiteParms);
    REQUIRE(reflect.draws.size() == 1u);
    CHECK(reflect.skipped == 0u);
    CHECK(reflect.draws[0].program == StageProgram::Environment);
    CHECK(reflect.draws[0].bumpStage == -1);
    CHECK(engineStageProgramSamplesCube(reflect.draws[0].program));
    CHECK(reflect.draws[0].color == Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(blendOf(reflect.draws[0].state) == (gls::SrcBlendOne | gls::DstBlendOne));
    {
      const auto material = parsed(decls, "textures/lp5t/reflect");
      REQUIRE(material->stages[0].image);
      // cubeMap: CF_NATIVE (Material.cpp:1270-1273)
      CHECK(material->stages[0].image->cube == 1);
      CHECK(material->stages[0].texgen == em::TexgenReflectCube);
      CHECK(engineBumpStage(*material) == -1);
    }

    // with a bump stage (GetBumpStage, Material.cpp:3796-3806): bumpyenvironment (57),
    // the bump stage's index carried for unit 1
    {
      const auto material = parsed(decls, "textures/lp5t/bumpy_reflect");
      const auto bump = engineBumpStage(*material);
      REQUIRE(bump >= 0);
      CHECK(material->stages[std::size_t(bump)].lighting == em::LightingBump);
      const auto bumpy = drawsOf("textures/lp5t/bumpy_reflect", WhiteParms);
      REQUIRE(bumpy.draws.size() == 1u);
      CHECK(bumpy.draws[0].program == StageProgram::BumpyEnvironment);
      CHECK(bumpy.draws[0].bumpStage == bump);
      CHECK(
        material->stages[std::size_t(bumpy.draws[0].stage)].lighting
        == em::LightingAmbient);
    }

    // TG_REFLECT_CUBE2 without a bump stage: not drawn (a cube in texture_color's
    // Texture2D, M-STAGE-DIFF-05), counted
    {
      const auto result = drawsOf("textures/lp5t/reflect2", WhiteParms);
      CHECK(result.draws.empty());
      CHECK(result.skipped == 1u);
      CHECK(result.texgenLp6 == 1u);
      CHECK_FALSE(
        engineHasBumpyEnvironment2Stage(*parsed(decls, "textures/lp5t/reflect2")));
    }
    // LP6.5b (C72, D180): with a bump stage, bumpyenvironment2 (59), the bump stage's
    // index carried for unit 0; it samples no cube at t0 (P8.24)
    {
      const auto material = parsed(decls, "textures/lp5t/bumpy_reflect2");
      const auto bump = engineBumpStage(*material);
      REQUIRE(bump >= 0);
      const auto result = drawsOf("textures/lp5t/bumpy_reflect2", WhiteParms);
      REQUIRE(result.draws.size() == 1u);
      CHECK(result.skipped == 0u);
      CHECK(result.texgenLp6 == 0u);
      CHECK(result.draws[0].program == StageProgram::BumpyEnvironment2);
      CHECK(result.draws[0].bumpStage == bump);
      CHECK_FALSE(engineStageProgramSamplesCube(result.draws[0].program));
      CHECK(blendOf(result.draws[0].state) == (gls::SrcBlendOne | gls::DstBlendOne));
      CHECK(engineHasBumpyEnvironment2Stage(*material));
      // TG_REFLECT_CUBE with a bump stage is 57's, not a consumer of the normals
      CHECK_FALSE(
        engineHasBumpyEnvironment2Stage(*parsed(decls, "textures/lp5t/bumpy_reflect")));
    }

    // TG_SKYBOX_CUBE: skybox (69); one stage, not blended: opaque, EQUAL
    const auto skybox = drawsOf("textures/lp5t/skybox", WhiteParms);
    REQUIRE(skybox.draws.size() == 1u);
    CHECK(skybox.draws[0].program == StageProgram::SkyBox);
    CHECK((skybox.draws[0].state & gls::DepthFuncBits) == gls::DepthFuncEqual);
    // the texture matrix is loaded for every stage (RB_LoadShaderTextureMatrix): identity
    CHECK(skybox.draws[0].textureMatrix[0] == Vec4{1.0f, 0.0f, 0.0f, 0.0f});
    CHECK(skybox.draws[0].textureMatrix[1] == Vec4{0.0f, 1.0f, 0.0f, 0.0f});

    // TG_WOBBLESKY_CUBE: wobblesky (70), its transform at time 0; rates 0: the identity
    const auto wobble = drawsOf("textures/lp5t/wobble", WhiteParms);
    REQUIRE(wobble.draws.size() == 1u);
    CHECK(wobble.draws[0].program == StageProgram::WobbleSky);
    CHECK(wobble.draws[0].wobbleSky[0] == Vec4{1.0f, 0.0f, 0.0f, 0.0f});
    CHECK(wobble.draws[0].wobbleSky[1] == Vec4{0.0f, 1.0f, 0.0f, 0.0f});
    CHECK(wobble.draws[0].wobbleSky[2] == Vec4{0.0f, 0.0f, 1.0f, 0.0f});
    // SVC_IGNORE: modulate 0, add 1 (the colour the shader takes, not rpColor)
    CHECK(wobble.draws[0].vertexColor[0] == Vec4{0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(wobble.draws[0].vertexColor[1] == Vec4{1.0f, 1.0f, 1.0f, 1.0f});

    // the texGenRegisters through the registers (a tilt of 30 degrees, rates 2 + 1 and 6
    // per minute): at time 0 by hand (RenderBackend.cpp:530-565): s 0, c 1, rs 0, rc 1,
    // ws sin 30° 0.5, wc cos 30°; axis[2] = ( ws, 0, wc ), axis[1] = ( 0, 1, 0 ),
    // axis[0] = axis[1] × axis[2] = ( wc, 0, -ws ); the rows axis[0], axis[1], axis[2]
    {
      const auto material = parsed(decls, "textures/lp5t/wobble_tilted");
      const auto registers = registersOf(decls, *material, WhiteParms);
      CHECK(engineRegister(registers, material->texGenRegisters[0]) == 30.0f);
      CHECK(engineRegister(registers, material->texGenRegisters[1]) == 3.0f);
      CHECK(engineRegister(registers, material->texGenRegisters[2]) == 6.0f);
      const auto result = engineShaderPassDraws(*material, registers);
      REQUIRE(result.draws.size() == 1u);
      const auto& rows = result.draws[0].wobbleSky;
      const auto ws = 0.5f;
      const auto wc = std::sqrt(3.0f) * 0.5f;
      const auto want = std::array<Vec4, 3>{
        Vec4{wc, 0.0f, -ws, 0.0f},
        Vec4{0.0f, 1.0f, 0.0f, 0.0f},
        Vec4{ws, 0.0f, wc, 0.0f}};
      for (std::size_t r = 0; r < 3; ++r)
      {
        for (std::size_t i = 0; i < 4; ++i)
        {
          CAPTURE(r, i, rows[r][i], want[r][i]);
          CHECK(std::abs(rows[r][i] - want[r][i]) < 1e-6f);
        }
      }
      CHECK(engineStageWobbleSky(*material, registers) == rows);
      // the time moves it (the engine's renderView.time[0]; the preview's is 0, K5): at
      // 5 s the wobble has turned a quarter (3 per minute: 2 PI · 3 / 60 · 5 = PI / 2)
      const auto later = engineStageWobbleSky(*material, registers, 5000.0f);
      CHECK(std::abs(later[2][0]) < 1e-5f);
      CHECK(std::abs(later[2][1] - ws) < 1e-5f);
    }

    // TG_DIFFUSE_CUBE and TG_GLASSWARP: the engine warns and changes nothing (:625-635)
    for (const auto* name : {"textures/lp5t/normal_cube", "textures/lp5t/glasswarp"})
    {
      CAPTURE(name);
      const auto result = drawsOf(name, WhiteParms);
      REQUIRE(result.draws.size() == 1u);
      CHECK(result.draws[0].program == StageProgram::TextureColor);
      CHECK(result.draws[0].texgenWarning);
      CHECK_FALSE(result.draws[0].screenTexgen);
    }
    CHECK_FALSE(drawsOf("textures/lp5t/add", WhiteParms).draws[0].texgenWarning);
  }

  SECTION("the cull: two-sided none, back-sided the engine's front-sided decode (D117)")
  {
    const auto back = drawsOf("textures/lp5t/backbox", WhiteParms).draws[0];
    CHECK((back.state & (3ull << 15)) == gls::CullBackSided);
    const auto two = drawsOf("textures/lp5t/twobox", WhiteParms).draws[0];
    CHECK((two.state & gls::CullBits) == gls::CullTwoSided);
    const auto front = drawsOf("textures/lp5t/blend", WhiteParms).draws[0];

    // M-STAGE-DEFECT-01: GetRenderState masks GLS_CULL_BACKSIDED away with GLS_CULL_BITS,
    // so the back-sided draw culls as the front-sided one
    const auto rasterOf = [](const std::uint64_t state) {
      auto renderState = nvrhi::RenderState{};
      engineRenderState(state, 0, 0.0f, renderState);
      return renderState.rasterState.cullMode;
    };
    CHECK(rasterOf(back.state) == rasterOf(front.state));
    CHECK(rasterOf(two.state) == nvrhi::RasterCullMode::None);
    CHECK(rasterOf(front.state) != nvrhi::RasterCullMode::None);
  }

  SECTION("a stencil block's bits (:4462-4506)")
  {
    const auto draw = drawsOf("textures/lp5t/stencil", WhiteParms).draws[0];
    CHECK((draw.state & (7ull << 36)) == gls::StencilFuncEqual);
    CHECK((draw.state & (7ull << 45)) == gls::StencilOpPassReplace);
    CHECK((draw.state & (7ull << 42)) == gls::StencilOpZFailDecrWrap);
    CHECK((draw.state & (7ull << 39)) == gls::StencilOpFailKeep);
    CHECK(((draw.state & gls::StencilFuncRefBits) >> gls::StencilFuncRefShift) == 7u);
    // GLS_STENCIL_MAKE_MASK of the write mask (255: no keyword sets it)
    CHECK(((draw.state & gls::StencilFuncMaskBits) >> gls::StencilFuncMaskShift) == 255u);
  }

  SECTION("a newStage block: its table program, USER0 from vertexParm 0 (C47)")
  {
    const auto material = parsed(decls, "textures/lp5t/voronoi");
    REQUIRE(material->stages.size() == 1u);
    CHECK(material->stages[0].newStage);
    CHECK(material->stages[0].fragmentProgram == "vornoipattern");
    CHECK(material->stages[0].vertexProgram == "vornoipattern");
    // no _currentRender: blended with the destination, SS_MEDIUM
    CHECK(material->sort == 4.0f);
    const auto result = drawsOf("textures/lp5t/voronoi", WhiteParms);
    REQUIRE(result.draws.size() == 1u);
    CHECK(result.draws[0].program == StageProgram::Material);
    REQUIRE(result.draws[0].userParms.size() == 1u);
    CHECK(result.draws[0].userParms[0] == Vec4{0.5f, 400.0f, 400.0f, 1.0f});
    CHECK(blendOf(result.draws[0].state) == (gls::SrcBlendOne | gls::DstBlendOne));
  }

  SECTION(
    "a fragmentMap of _currentRender: SS_POST_PROCESS and translucent (the second stage "
    "call draws it, LP6.5b)")
  {
    const auto haze = parsed(decls, "textures/lp5t/haze");
    // MAT:3199-3230
    CHECK(haze->sort == EngineSortPostProcess);
    CHECK(haze->coverage == em::CoverageTranslucent);
    // post-process effects fill the depth buffer as they draw: LESS with depth writes
    CHECK((haze->stages[0].drawStateBits & gls::DepthFuncBits) == gls::DepthFuncLess);
    CHECK((haze->stages[0].drawStateBits & gls::DepthMask) == 0);
  }

  SECTION("an implicit material: blend blend, colored, its image clamped (SC523)")
  {
    // Material.cpp SetDefaultText: a name with no decl
    const auto implicit = parsed(decls, "textures/lp5t/no_decl");
    CHECK(implicit->implicit);
    REQUIRE(implicit->stages.size() == 1u);
    CHECK(implicit->stages[0].lighting == em::LightingAmbient);
    REQUIRE(implicit->stages[0].image);
    CHECK(implicit->stages[0].image->name == "textures/lp5t/no_decl");
    CHECK(implicit->coverage == em::CoverageTranslucent);
    CHECK(implicit->sort == 4.0f);
    CHECK_FALSE(implicit->receivesLighting);
    auto parms = WhiteParms;
    parms[0] = 0.5f;
    const auto result = drawsOf("textures/lp5t/no_decl", parms);
    REQUIRE(result.draws.size() == 1u);
    CHECK(result.draws[0].program == StageProgram::TextureColor);
    CHECK(
      blendOf(result.draws[0].state)
      == (gls::SrcBlendSrcAlpha | gls::DstBlendOneMinusSrcAlpha));
    CHECK(result.draws[0].color == Vec4{0.5f, 1.0f, 1.0f, 1.0f});
  }

  // the parse warned of nothing (a bad keyword would default the material)
  for (const auto& message : decls.messages())
  {
    CAPTURE(message.text);
    CHECK(message.kind != DeclMessageKind::Warning);
  }
}

TEST_CASE("LitStagePass: the stages map's S materials (LP5.2a, C50)")
{
  // parity.py's lp5_stages.mtr, as fixtures wrote it: the S rooms' stage decisions are
  // the rows' expect counts (one chunk per material in each room)
  auto source = MemoryDeclSource{{DeclFile{
    "materials/lp5_stages.mtr",
    readTextFile(litFixtureFolder() / "materials" / "lp5_stages.mtr"),
    0}}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  auto colored = WhiteParms;
  colored[0] = 0.5f;
  colored[1] = 0.25f;
  colored[2] = 1.0f;
  struct Expected
  {
    std::string name;
    std::size_t draws = 0;
    std::size_t skipped = 0;
    StageProgram program = StageProgram::TextureColor;
    float sort = 4.0f;
    int coverage = em::CoverageTranslucent;
  };
  const auto expected = std::vector<Expected>{
    {"textures/lp5/s01_add", 1, 1},
    {"textures/lp5/s01_blend", 1, 0},
    {"textures/lp5/s01_filter", 1, 0},
    {"textures/lp5/s02_inverse", 1, 0},
    {"textures/lp5/s02_plain", 1, 0},
    {"textures/lp5/s02_colored", 1, 0},
    {"textures/lp5/s03_grate",
     1,
     0,
     StageProgram::TextureColor,
     0.0f,
     em::CoveragePerforated},
    {"textures/lp5/s04_red", 1, 0},
    {"textures/lp5/s04_blue", 1, 0},
    {"textures/lp5/s05_decal", 1, 0, StageProgram::TextureColor, 2.0f},
    {"textures/lp5/s05_far", 1, 0, StageProgram::TextureColor, 3.0f},
    {"textures/lp5/s05_close", 1, 0, StageProgram::TextureColor, 5.0f},
    {"textures/lp5/s06_implicit", 1, 0},
    {"textures/lp5/s07_screen", 1, 0, StageProgram::TextureColorTexgen},
    {"textures/lp5/s12_backbox", 1, 0},
    {"textures/lp5/s13_voronoi", 1, 0, StageProgram::Material},
    {"textures/lp5/s14_fogwithlights", 1, 0, StageProgram::Material},
    // LP5.2b: the texgen rooms
    {"textures/lp5/s08_sky", 1, 0, StageProgram::SkyBox, 0.0f, em::CoverageOpaque},
    {"textures/lp5/s09_env", 1, 0, StageProgram::Environment, 0.0f, em::CoverageOpaque},
    {"textures/lp5/s09_bumpy_env",
     1,
     0,
     StageProgram::BumpyEnvironment,
     0.0f,
     em::CoverageOpaque},
    {"textures/lp5/s10_sky_tested",
     1,
     0,
     StageProgram::SkyBox,
     0.0f,
     em::CoveragePerforated},
    {"textures/lp5/s11_wobble_add",
     1,
     0,
     StageProgram::WobbleSky,
     0.0f,
     em::CoveragePerforated},
  };
  for (const auto& want : expected)
  {
    CAPTURE(want.name);
    const auto material = parsed(decls, want.name);
    CHECK(material->sort == want.sort);
    CHECK(material->coverage == want.coverage);
    CHECK(material->hasAmbient);
    const auto parms = want.name == "textures/lp5/s02_colored" ? colored : WhiteParms;
    const auto result =
      engineShaderPassDraws(*material, registersOf(decls, *material, parms));
    REQUIRE(result.draws.size() == want.draws);
    CHECK(result.skipped == want.skipped);
    CHECK(result.draws[0].program == want.program);
  }
  // LP5.2b: the gl_zero, gl_one twins: perforated, skipped by pass 8, drawn by the
  // prepass on their texgen's program (the whole surface's depth)
  for (const auto& [name, program] :
       {std::pair{"textures/lp5/s10_sky_mask", PrepassProgram::SkyBox},
        std::pair{"textures/lp5/s11_wobble_mask", PrepassProgram::WobbleSky},
        std::pair{"textures/lp5/s10_sky_tested", PrepassProgram::SkyBox},
        std::pair{"textures/lp5/s11_wobble_add", PrepassProgram::WobbleSky}})
  {
    CAPTURE(name);
    const auto material = parsed(decls, name);
    CHECK(material->coverage == em::CoveragePerforated);
    const auto registers = registersOf(decls, *material, WhiteParms);
    const auto result = engineShaderPassDraws(*material, registers);
    const auto mask = std::string{name}.find("mask") != std::string::npos;
    CHECK(result.draws.size() == (mask ? 0u : 1u));
    CHECK(result.skipped == (mask ? 1u : 0u));
    const auto prepass = engineGenericDepthDraws(*material, registers);
    REQUIRE(prepass.size() == 1u);
    CHECK(prepass[0].program == program);
  }
  // the cube images: CF_NATIVE
  REQUIRE(parsed(decls, "textures/lp5/s08_sky")->stages[0].image);
  CHECK(parsed(decls, "textures/lp5/s08_sky")->stages[0].image->cube == 1);
  CHECK(
    parsed(decls, "textures/lp5/s08_sky")->stages[0].image->name == "textures/lp5/sky");
  CHECK(parsed(decls, "textures/lp5/s06_implicit")->implicit);
  CHECK(parsed(decls, "textures/lp5/s12_backbox")->cullType == engine_cull::BackSided);
  // the M and B rooms' materials have no SL_AMBIENT stage
  for (const auto* name :
       {"textures/lp5/bump",
        "textures/lp5/flat",
        "textures/lp5/pbr",
        "textures/lp5/white_wall"})
  {
    CAPTURE(name);
    CHECK_FALSE(parsed(decls, name)->hasAmbient);
  }
  for (const auto& message : decls.messages())
  {
    CAPTURE(message.text);
    CHECK(message.kind != DeclMessageKind::Warning);
  }
}

TEST_CASE("LitStagePass: FogPass' colour, density and planes (LP5.3, C49, SC533)")
{
  // a camera at ( 100, 50, 80 ) looking down +x; the light's far plane z = 296
  auto camera = CameraState{};
  camera.position = vm::vec3f{100, 50, 80};
  camera.direction = vm::vec3f{1, 0, 0};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  const auto view = litViewOf(camera, 64, 48);
  const auto fogPlane = EnginePlane{0.0f, 0.0f, 1.0f, -296.0f};
  auto shader = MaterialEntry{};
  shader.fogLight = true;
  shader.stages.resize(1);
  shader.stages[0].colorRegisters = {4, 5, 6, 7};
  const auto dot = [](const EnginePlane& p, const EngineVec3& v) {
    return p.a * v[0] + p.b * v[1] + p.c * v[2] + p.d;
  };
  // a point 300 ahead of the eye, and one 300 ahead and 40 up
  const auto ahead = EngineVec3{400, 50, 80};
  const auto up = EngineVec3{400, 50, 120};

  SECTION("a distance of 2000: rpColor.a clamped to 1, a = -0.5 / 2000")
  {
    const auto registers = std::vector<float>{1, 1, 0, 0, 0.5f, 0.6f, 0.7f, 2000.0f};
    const auto parms = engineFogParms(
      shader, registers, fogPlane, view.worldModelViewMatrix, view.viewOrigin);
    REQUIRE(parms);
    CHECK(parms->lightColor[3] == 2000.0f);
    CHECK(parms->color == Vec4{0.5f, 0.6f, 0.7f, 1.0f});
    CHECK(parms->a == -0.5f / 2000.0f);
    // p0 · position = a · eye z + 0.5 = 0.5 + 0.5 · depth / 2000 (eye z = -depth)
    CHECK(dot(parms->planes[0], ahead) == Catch::Approx(0.5 + 0.5 * 300.0 / 2000.0));
    CHECK(dot(parms->planes[0], up) == Catch::Approx(0.5 + 0.5 * 300.0 / 2000.0));
    CHECK(parms->planes[1].a == 0.0f);
    CHECK(parms->planes[1].b == 0.0f);
    CHECK(parms->planes[1].c == 0.0f);
    CHECK(parms->planes[1].d == 0.5f);
    // p2 = 0.001 · fogPlane + FOG_ENTER, p3 = 0.001 · s + FOG_ENTER, s = 80 - 296
    CHECK(EngineFogEnter == 65.0f / 128.0f);
    CHECK(parms->planes[2].c == 0.001f);
    CHECK(parms->planes[2].d == 0.001f * -296.0f + 65.0f / 128.0f);
    CHECK(
      dot(parms->planes[2], up) == Catch::Approx(0.001 * (120.0 - 296.0) + 65.0 / 128.0));
    CHECK(parms->planes[3].a == 0.0f);
    CHECK(parms->planes[3].b == 0.0f);
    CHECK(parms->planes[3].c == 0.0f);
    CHECK(parms->planes[3].d == 0.001f * (80.0f - 296.0f) + 65.0f / 128.0f);
  }

  SECTION("a distance of 0.5: rpColor.a 0.5, a = -0.5 / DEFAULT_FOG_DISTANCE")
  {
    const auto registers = std::vector<float>{1, 1, 0, 0, 0.5f, 0.6f, 0.7f, 0.5f};
    const auto parms = engineFogParms(
      shader, registers, fogPlane, view.worldModelViewMatrix, view.viewOrigin);
    REQUIRE(parms);
    CHECK(parms->color[3] == 0.5f);
    CHECK(parms->a == -0.5f / 500.0f);
    CHECK(dot(parms->planes[0], ahead) == Catch::Approx(0.5 + 0.5 * 300.0 / 500.0));
    // an alpha of 1 is the default's too
    CHECK(engineFogDensity(1.0f) == -0.5f / 500.0f);
    CHECK(engineFogDensity(1.5f) == -0.5f / 1.5f);
  }

  SECTION("no stage: nothing")
  {
    shader.stages.clear();
    CHECK_FALSE(engineFogParms(
      shader,
      std::vector<float>(8, 1.0f),
      fogPlane,
      view.worldModelViewMatrix,
      view.viewOrigin));
  }
}

TEST_CASE("LitStagePass: the fog cube is R_MakeZeroOneCubeTris (LP5.3, C49, D122)")
{
  const auto vertices = engineZeroOneCubeVertices();
  const auto indexes = engineZeroOneCubeIndexes();
  // vertex i: x of bits 1 and 2 of i % 4, y of 2 and 3, z of i >= 4
  for (std::size_t i = 0; i < vertices.size(); ++i)
  {
    CAPTURE(i);
    const auto corner = i % 4;
    CHECK(vertices[i][0] == (corner == 1 || corner == 2 ? 1.0f : 0.0f));
    CHECK(vertices[i][1] == (corner >= 2 ? 1.0f : 0.0f));
    CHECK(vertices[i][2] == (i >= 4 ? 1.0f : 0.0f));
  }
  // the engine's 36 indexes; every face's id normal, ( c - a ) × ( b - a ) as
  // R_DeriveFacePlanes takes it, points away from the centre (so ( b - a ) × ( c - a )
  // towards it): the faces a FRONTSIDED draw keeps are the ones seen from outside, so
  // the cube's near faces draw from outside and nothing draws from inside (SC535)
  CHECK(indexes == std::array<std::uint16_t, 36>{2, 3, 0, 1, 2, 0, 5, 1, 0, 4, 5, 0,
                                                 7, 4, 0, 3, 7, 0, 1, 5, 6, 2, 1, 6,
                                                 3, 2, 6, 7, 3, 6, 4, 7, 6, 5, 4, 6});
  for (std::size_t t = 0; t < 12; ++t)
  {
    CAPTURE(t);
    const auto& a = vertices[indexes[t * 3 + 0]];
    const auto& b = vertices[indexes[t * 3 + 1]];
    const auto& c = vertices[indexes[t * 3 + 2]];
    const auto u = EngineVec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const auto v = EngineVec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const auto n = EngineVec3{
      u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const auto centre = EngineVec3{
      (a[0] + b[0] + c[0]) / 3.0f - 0.5f,
      (a[1] + b[1] + c[1]) / 3.0f - 0.5f,
      (a[2] + b[2] + c[2]) / 3.0f - 0.5f};
    CHECK(n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] < 0.0f);
  }
}

namespace
{

// LP3.4: the shadows map's two grates (materials/lp3_parity.mtr, H04 and H08) and the
// perforated shadow pass's cases, through the copied parser
const char* const PerforatedMaterials =
  "textures/lp3p/grate\n"
  "{\n"
  "  qer_editorimage textures/lp3p/grate.tga\n"
  "  bumpmap textures/lp2/floor_local.tga\n"
  "  { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 nopicmip }\n"
  "}\n"
  "textures/lp3p/grate_noshadows\n"
  "{\n"
  "  qer_editorimage textures/lp3p/grate.tga\n"
  "  noShadows\n"
  "  bumpmap textures/lp2/floor_local.tga\n"
  "  { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 nopicmip }\n"
  "}\n"
  "textures/lp34/cond\n"
  "{ { if parm4 blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 } }\n"
  "textures/lp34/clear\n"
  "{ { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 alpha 0 } }\n"
  "textures/lp34/parms\n"
  "{ { blend diffusemap map textures/lp3p/grate.tga alphaTest parm5 alpha parm3 } }\n"
  "textures/lp34/offset\n"
  "{ polygonOffset 1 { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 } }\n"
  "textures/lp34/offset_cond\n"
  "{\n"
  "  polygonOffset 1\n"
  "  { if parm4 blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 }\n"
  "}\n"
  "textures/lp34/private\n"
  "{\n"
  "  { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 privatePolygonOffset 2 "
  "}\n"
  "}\n"
  "textures/lp34/two\n"
  "{\n"
  "  { blend diffusemap map textures/lp3p/grate.tga alphaTest 0.5 }\n"
  "  { map textures/lp34/bars.tga alphaTest 0.25 }\n"
  "}\n"
  "textures/lp34/mixed\n"
  "{\n"
  "  { map textures/lp34/glow.tga }\n"
  "  { map textures/lp3p/grate.tga alphaTest 0.5 }\n"
  "}\n";

} // namespace

// LP3.4 (D155, K52, E-LIT-12; SC723): ShadowMapPassPerforated's draws of one surface
// (RenderBackend.cpp:3272-3385), with the state a side's call carries from surface to
// surface
TEST_CASE("LitStagePass: the perforated shadow pass's stage draws (LP3.4, D155, K52)")
{
  auto source =
    MemoryDeclSource{{DeclFile{"materials/lp34.mtr", PerforatedMaterials, 0}}};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto opening = enginePerforatedShadowOpening();
  // the call opens with the fast pass's state (:3191-3222): GLS_POLYGON_OFFSET, the
  // occluder facing 2's TWOSIDED, GL_PolygonOffset( 2, 1 ) (Vulkan)
  CHECK(opening.state == (gls::PolygonOffset | gls::CullTwoSided));
  CHECK(opening.slopeBias == 2.0f);
  CHECK(opening.depthBias == 1.0f);
  const auto drawsOf = [&](
                         const std::string& name,
                         const std::array<float, MaterialDecls::ShaderParmCount>& parms,
                         PerforatedShadowState& carried) {
    const auto material = parsed(decls, name);
    CHECK(material->coverage == em::CoveragePerforated);
    return enginePerforatedShadowDraws(
      *material, registersOf(decls, *material, parms), carried);
  };

  SECTION("H04's grate: one texture_color draw under state 0, ALPHA_TEST 0.5")
  {
    auto carried = opening;
    const auto draws = drawsOf("textures/lp3p/grate", WhiteParms, carried);
    // the bump and the diffuse stage have no alpha test: ignored; the coverage stage the
    // parse made of the alpha-tested diffuse map drawn (Material.cpp:2297-2330: its alpha
    // test moved there, its image TD_COVERAGE), in the stage order the engine walks
    REQUIRE(draws.size() == 1u);
    const auto& draw = draws[0];
    CHECK(draw.program == PrepassProgram::TextureColor);
    const auto grate = parsed(decls, "textures/lp3p/grate");
    REQUIRE(draw.stage >= 0);
    REQUIRE(std::size_t(draw.stage) < grate->stages.size());
    CHECK(grate->stages[std::size_t(draw.stage)].hasAlphaTest);
    CHECK(grate->stages[std::size_t(draw.stage)].lighting == em::LightingCoverage);
    CHECK(
      std::ranges::count_if(grate->stages, &MaterialStage::hasAlphaTest)
      == std::ptrdiff_t{1});
    // GL_State( stageGLState ) with no MF_POLYGONOFFSET: 0 (E-LIT-12: FRONTSIDED, no
    // shadow offset), the key's offsets 0
    CHECK(draw.state == 0u);
    CHECK(draw.slopeBias == 0.0f);
    CHECK(draw.depthBias == 0.0f);
    // GL_Color( 0, 0, 0, the alpha register: 1 )
    CHECK(draw.color == Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(draw.alphaTest == Vec4{0.5f, 0.5f, 0.5f, 0.5f});
    // PrepareStageTexturing: the identity matrix, no texgen
    CHECK(draw.textureMatrix[0] == Vec4{1.0f, 0.0f, 0.0f, 0.0f});
    CHECK(draw.textureMatrix[1] == Vec4{0.0f, 1.0f, 0.0f, 0.0f});
    CHECK(draw.texGenEnabled == Vec4{0.0f, 0.0f, 0.0f, 0.0f});
    CHECK_FALSE(draw.screenTexgen);
    // the call's state is the stage's now; its offset untouched
    CHECK(carried.state == 0u);
    CHECK(carried.slopeBias == 2.0f);
    CHECK(carried.depthBias == 1.0f);

    // H08's: noShadows changes nothing of the draws (C57: it casts)
    auto carriedB = opening;
    const auto noShadows = drawsOf("textures/lp3p/grate_noshadows", WhiteParms, carriedB);
    REQUIRE(noShadows.size() == 1u);
    CHECK(noShadows[0].program == draw.program);
    CHECK(noShadows[0].stage == draw.stage);
    CHECK(noShadows[0].state == draw.state);
    CHECK(noShadows[0].color == draw.color);
    CHECK(noShadows[0].alphaTest == draw.alphaTest);
    CHECK(noShadows[0].textureMatrix == draw.textureMatrix);
    CHECK(carriedB == carried);
  }

  SECTION("the colour alpha and ALPHA_TEST from their registers")
  {
    auto parms = WhiteParms;
    parms[3] = 0.75f;
    parms[5] = 0.25f;
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/parms", parms, carried);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].color == Vec4{0.0f, 0.0f, 0.0f, 0.75f});
    CHECK(draws[0].alphaTest == Vec4{0.25f, 0.25f, 0.25f, 0.25f});
  }

  SECTION("a stage whose condition register is 0: the fallback depth draw")
  {
    // first in the call: under the opening state (the opaque casters')
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/cond", WhiteParms, carried);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].program == PrepassProgram::Depth);
    CHECK(draws[0].state == (gls::PolygonOffset | gls::CullTwoSided));
    CHECK(draws[0].slopeBias == 2.0f);
    CHECK(draws[0].depthBias == 1.0f);
    // no GL_Color, no ALPHA_TEST
    CHECK_FALSE(draws[0].color);
    CHECK_FALSE(draws[0].alphaTest);
    CHECK(carried == opening);

    // K52, E-LIT-12: after a grate's stage the fallback inherits its state 0
    auto chain = opening;
    drawsOf("textures/lp3p/grate", WhiteParms, chain);
    const auto after = drawsOf("textures/lp34/cond", WhiteParms, chain);
    REQUIRE(after.size() == 1u);
    CHECK(after[0].program == PrepassProgram::Depth);
    CHECK(after[0].state == 0u);
    CHECK(after[0].slopeBias == 0.0f);
    CHECK(after[0].depthBias == 0.0f);

    // the condition on: the stage's draw
    auto parms = WhiteParms;
    parms[4] = 1.0f;
    auto on = opening;
    const auto drawn = drawsOf("textures/lp34/cond", parms, on);
    REQUIRE(drawn.size() == 1u);
    CHECK(drawn[0].program == PrepassProgram::TextureColor);
  }

  SECTION("an alpha of 0: the stage tried (didDraw), no draw at all")
  {
    auto carried = opening;
    CHECK(drawsOf("textures/lp34/clear", WhiteParms, carried).empty());
    // GL_State was not called
    CHECK(carried == opening);
  }

  SECTION("MF_POLYGONOFFSET: GLS_POLYGON_OFFSET with r_offsetUnits -600 x polygonOffset")
  {
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/offset", WhiteParms, carried);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].program == PrepassProgram::TextureColor);
    CHECK(draws[0].state == gls::PolygonOffset);
    CHECK(draws[0].slopeBias == 0.0f);
    CHECK(draws[0].depthBias == -600.0f);
    CHECK(carried.state == gls::PolygonOffset);
    CHECK(carried.depthBias == -600.0f);

    // its fallback: the call's state bits with the surface's own offset (set at the
    // surface's start, :3285-3289)
    auto fallback = opening;
    const auto solid = drawsOf("textures/lp34/offset_cond", WhiteParms, fallback);
    REQUIRE(solid.size() == 1u);
    CHECK(solid[0].program == PrepassProgram::Depth);
    CHECK(solid[0].state == (gls::PolygonOffset | gls::CullTwoSided));
    CHECK(solid[0].slopeBias == 0.0f);
    CHECK(solid[0].depthBias == -600.0f);
  }

  SECTION("privatePolygonOffset: the stage's offset, then the surface's again")
  {
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/private", WhiteParms, carried);
    REQUIRE(draws.size() == 1u);
    CHECK(draws[0].state == gls::PolygonOffset);
    CHECK(draws[0].slopeBias == 0.0f);
    CHECK(draws[0].depthBias == -1200.0f);
    // GL_State( stageGLState ) kept; GL_PolygonOffset( 0, -600 x 0 ) after the stage
    CHECK(carried.state == gls::PolygonOffset);
    CHECK(carried.slopeBias == 0.0f);
    CHECK(carried.depthBias == 0.0f);
  }

  SECTION("two alpha-tested stages: two draws in stage order")
  {
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/two", WhiteParms, carried);
    REQUIRE(draws.size() == 2u);
    // the parsed material's order (the diffuse map's coverage stage the parse makes, and
    // the ambient one): each draw its stage's, the first stage first
    const auto two = parsed(decls, "textures/lp34/two");
    auto tested = std::vector<int>{};
    for (std::size_t s = 0; s < two->stages.size(); ++s)
    {
      if (two->stages[s].hasAlphaTest)
      {
        tested.push_back(int(s));
      }
    }
    REQUIRE(tested.size() == 2u);
    CHECK(draws[0].stage == tested[0]);
    CHECK(draws[1].stage == tested[1]);
    CHECK(draws[0].stage < draws[1].stage);
    const auto registers = registersOf(decls, *two, WhiteParms);
    for (const auto& draw : draws)
    {
      CAPTURE(draw.stage);
      const auto alpha =
        engineRegister(registers, two->stages[std::size_t(draw.stage)].alphaTestRegister);
      CHECK(draw.alphaTest == Vec4{alpha, alpha, alpha, alpha});
      CHECK(draw.program == PrepassProgram::TextureColor);
    }
    CHECK(draws[0].alphaTest != draws[1].alphaTest);
  }

  SECTION("a stage without an alpha test is ignored")
  {
    auto carried = opening;
    const auto draws = drawsOf("textures/lp34/mixed", WhiteParms, carried);
    REQUIRE(draws.size() == 1u);
    const auto mixed = parsed(decls, "textures/lp34/mixed");
    REQUIRE(mixed->stages.size() == 2u);
    CHECK_FALSE(mixed->stages[0].hasAlphaTest);
    CHECK(draws[0].stage == 1);
    CHECK(draws[0].program == PrepassProgram::TextureColor);
  }
}
} // namespace tb::lit
