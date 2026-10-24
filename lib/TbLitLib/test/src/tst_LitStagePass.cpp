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
#include "LitStages.h"
#include "LitView.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineLight.h"
#include "lit/EnginePipelineState.h"
#include "lit/LitSession.h"
#include "lit/MaterialDecls.h"

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
  std::sort(indices.begin(), indices.end(), std::greater<std::uint64_t>{});
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

// the stage-pass materials of the S rooms and of the [litgpu2] cases, through the copied
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

  SECTION("the texgens: screen on 18, a cube texgen LP5.2b's, a cinematic not drawn")
  {
    const auto screen = drawsOf("textures/lp5t/screen", WhiteParms);
    REQUIRE(screen.draws.size() == 1u);
    CHECK(screen.draws[0].program == StageProgram::TextureColorTexgen);
    CHECK(screen.draws[0].screenTexgen);

    const auto reflect = drawsOf("textures/lp5t/reflect", WhiteParms);
    CHECK(reflect.draws.empty());
    CHECK(reflect.texgenDeferred == 1u);
    CHECK(reflect.skipped == 1u);

    const auto movie = drawsOf("textures/lp5t/movie", WhiteParms);
    CHECK(movie.draws.empty());
    CHECK(movie.cinematic == 1u);
    CHECK(movie.skipped == 1u);
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

  SECTION("a fragmentMap of _currentRender: SS_POST_PROCESS and translucent (K38)")
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
} // namespace tb::lit
