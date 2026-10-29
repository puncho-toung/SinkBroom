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

#include "lit/EngineLight.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"
#include "lit/ShaderBlobs.h"
#include "lit/ShaderReflection.h"

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::size_t setCount(const std::optional<std::vector<nvrhi::BindingLayoutDesc>>& layouts)
{
  return layouts ? layouts->size() : 0u;
}

} // namespace

TEST_CASE("EngineTables")
{
  SECTION("renderParm_t")
  {
    CHECK(int(RenderParm::Total) == 106);
    CHECK(int(RenderParm::MvpMatrixX) == 21);
    CHECK(int(RenderParm::Overbright) == 50);
    CHECK(int(RenderParm::User0) == 98);
    CHECK(
      renderParmName(RenderParm::ScreenCorrectionFactor)
      == "RENDERPARM_SCREENCORRECTIONFACTOR");
    CHECK(renderParmName(RenderParm::MvpMatrixX) == "RENDERPARM_MVPMATRIX_X");
    CHECK(renderParmName(RenderParm::TexGen0Enabled) == "RENDERPARM_TEXGEN_0_ENABLED");
    CHECK(renderParmName(RenderParm::ShIrradiance8) == "RENDERPARM_SH_IRRADIANCE_8");
    CHECK(renderParmName(RenderParm::User7) == "RENDERPARM_USER7");
    CHECK(renderParmName(RenderParm::Total).empty());

    auto names = std::set<std::string_view>{};
    for (auto i = 0; i < int(RenderParm::Total); ++i)
    {
      const auto name = renderParmName(RenderParm(i));
      CHECK(name.starts_with("RENDERPARM_"));
      names.insert(name);
    }
    CHECK(names.size() == 106u);
  }

  SECTION("bindingLayoutType_t")
  {
    CHECK(int(EngineBindingLayout::Num) == 43);
    CHECK(int(EngineBindingLayout::Texture) == 4);
    CHECK(int(EngineBindingLayout::ConstantBufferOnly) == 11);
    CHECK(int(EngineBindingLayout::DrawInteraction) == 15);
    CHECK(int(EngineBindingLayout::Blit) == 31);
    CHECK(int(EngineBindingLayout::Tonemap) == 40);
    CHECK(int(EngineBindingLayout::Histogram) == 41);
    CHECK(int(EngineBindingLayout::Exposure) == 42);
  }

  SECTION("the builtins table")
  {
    REQUIRE(EngineBuiltins.size() == 116u);
    for (std::size_t i = 0; i < EngineBuiltins.size(); ++i)
    {
      const auto& row = EngineBuiltins[i];
      CAPTURE(i, row.name, row.nameOutSuffix);
      CHECK(row.index == int(i));
      CHECK(engineBuiltin(int(i)) == &row);
      CHECK(row.defineCount <= EngineBuiltinMaxDefines);
      CHECK(row.stages != 0);
      CHECK(int(row.bindingLayout) < int(EngineBindingLayout::Num));
      // usePushConstants always names the row's own layout, so a null value is the last
      // define of a row that has one
      for (std::size_t d = 0; d < row.defineCount; ++d)
      {
        if (!row.defines[d].value)
        {
          CHECK(std::string_view{row.defines[d].name} == "USE_PUSH_CONSTANTS");
          CHECK(d + 1 == row.defineCount);
        }
      }
    }
    CHECK(engineBuiltin(-1) == nullptr);
    CHECK(engineBuiltin(116) == nullptr);

    // the rows LP2 draws with (SC245, RenderProgs.cpp:794-952)
    const auto& color = EngineBuiltins[EngineBuiltinColor];
    CHECK(std::string_view{color.name} == "builtin/color");
    CHECK(color.bindingLayout == EngineBindingLayout::ConstantBufferOnly);
    CHECK(color.stages == EngineStageDefault);

    const auto& textureColor = EngineBuiltins[EngineBuiltinTextureVertexColor];
    CHECK(std::string_view{textureColor.name} == "builtin/texture_color");
    CHECK(std::string_view{textureColor.nameOutSuffix}.empty());
    CHECK(textureColor.bindingLayout == EngineBindingLayout::Texture);
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(textureColor, true))
      == "USE_GPU_SKINNING=0 USE_SRGB=0 USE_PUSH_CONSTANTS=1");

    const auto& interaction = EngineBuiltins[EngineBuiltinInteraction];
    CHECK(std::string_view{interaction.name} == "builtin/lighting/interaction");
    CHECK(interaction.bindingLayout == EngineBindingLayout::DrawInteraction);
    CHECK(interaction.vertexLayout == EngineVertexLayout::DrawVert);
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(interaction, false))
      == "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0");

    const auto& pbr = EngineBuiltins[EngineBuiltinPbrInteraction];
    CHECK(std::string_view{pbr.name} == "builtin/lighting/interaction");
    CHECK(std::string_view{pbr.nameOutSuffix} == "_PBR");
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(pbr, false))
      == "USE_GPU_SKINNING=0 USE_PBR=1 USE_PUSH_CONSTANTS=0");

    const auto& depth = EngineBuiltins[EngineBuiltinDepth];
    CHECK(std::string_view{depth.name} == "builtin/depth");
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(depth, true))
      == "USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=1");

    const auto& blit = EngineBuiltins[EngineBuiltinBlit];
    CHECK(std::string_view{blit.name} == "builtin/blit");
    CHECK(blit.stages == EngineStageFragment);
    CHECK(shaderPermutationKey(engineBuiltinDefines(blit, true)) == "TEXTURE_ARRAY=0");

    const auto& rect = EngineBuiltins[EngineBuiltinRect];
    CHECK(std::string_view{rect.name} == "builtin/rect");
    CHECK(rect.stages == EngineStageVertex);
    CHECK(rect.defineCount == 0u);

    const auto& tonemapping = EngineBuiltins[EngineBuiltinTonemapping];
    CHECK(std::string_view{tonemapping.name} == "builtin/post/tonemapping");
    CHECK(tonemapping.vertexLayout == EngineVertexLayout::Unknown);
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(tonemapping, true))
      == "HISTOGRAM_BINS=256 SOURCE_ARRAY=0 QUAD_Z=0 USE_PUSH_CONSTANTS=1");

    const auto& histogram = EngineBuiltins[EngineBuiltinHistogram];
    CHECK(std::string_view{histogram.name} == "builtin/post/histogram");
    CHECK(histogram.stages == EngineStageCompute);
    CHECK(histogram.bindingLayout == EngineBindingLayout::Histogram);

    const auto& exposure = EngineBuiltins[EngineBuiltinExposure];
    CHECK(std::string_view{exposure.name} == "builtin/post/exposure");
    CHECK(exposure.bindingLayout == EngineBindingLayout::Exposure);
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(exposure, false))
      == "HISTOGRAM_BINS=256 USE_PUSH_CONSTANTS=0");
  }

  SECTION("the shadow cvars at the engine's defaults (LP3.0, C58, D159, SC741)")
  {
    // RenderSystem_init.cpp:108, 151, 251-269, 329 and tr_frontend_addmodels.cpp:50
    CHECK_FALSE(EngineFrame.skipShadows);
    CHECK(EngineFrame.useShadowAtlas == 1);
    CHECK(EngineFrame.shadowMapAtlasSize == 8192);
    CHECK(EngineFrame.shadowMapFrustumFov == 92.0f);
    CHECK(EngineFrame.shadowMapSingleSide == -1);
    CHECK(EngineFrame.shadowMapJitterScale == 2.5f);
    // the preview's pin (parity.cfg's SWITCHES), not the engine's default of 1
    CHECK_FALSE(EngineFrame.shadowMapRandomizeJitter);
    CHECK(EngineFrame.shadowMapSamples == 16);
    CHECK(EngineFrame.shadowMapSplits == 3);
    CHECK(EngineFrame.shadowMapSplitWeight == 0.9f);
    CHECK(EngineFrame.shadowMapLodScale == 1.4f);
    CHECK(EngineFrame.shadowMapLodBias == 0);
    CHECK(EngineFrame.shadowMapPolygonFactor == 2.0f);
    CHECK(EngineFrame.vkShadowMapPolygonOffset == 1.0f);
    CHECK(EngineFrame.shadowMapOccluderFacing == 2);
    CHECK(EngineFrame.shadowMapRegularDepthBiasScale == 0.999f);
    CHECK(EngineFrame.shadowMapSunDepthBiasScale == 0.999f);
    CHECK(EngineFrame.forceShadowMapsOnAlphaTestedSurfaces);
    CHECK(EngineFrame.useShadowSurfaceScissor);
    CHECK(EngineFrame.useMaskedOcclusionCulling);
    // engineShadowLod's defaults are EngineFrame's
    const auto box =
      EngineBounds{EngineVec3{0.0f, 0.5f, 0.5f}, EngineVec3{0.4f, 0.5f, 0.5f}};
    CHECK(
      engineShadowLod(box, false)
      == engineShadowLod(
        box, false, EngineFrame.shadowMapLodScale, EngineFrame.shadowMapLodBias));
  }

  SECTION("renderparm sets")
  {
    // the mirrored sets have the lengths the engine's lists have
    CHECK(engineParmSetMembers(0).size() == std::size_t(EngineParmSetSizes[0]));
    CHECK(engineParmSetMembers(4).size() == std::size_t(EngineParmSetSizes[4]));
    CHECK(engineParmSetMembers(10).size() == std::size_t(EngineParmSetSizes[10]));
    CHECK(engineParmSetMembers(9).size() == std::size_t(EngineParmSetSizes[9]));
    // LP6.5a (C69): set 3, GBUFFER's rpNominalSet3
    CHECK(engineParmSetMembers(3).size() == std::size_t(EngineParmSetSizes[3]));
    CHECK(engineParmSetBlock(3).size == 256u);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::Gbuffer) == 3);
    // LP5.0 (C45-C49): sets 5, 7, 8 and 12
    for (const auto set : {5, 7, 8, 12})
    {
      CAPTURE(set);
      CHECK(
        engineParmSetMembers(set).size()
        == std::size_t(EngineParmSetSizes[std::size_t(set)]));
    }
    CHECK(engineParmSetMembers(6).empty());
    // LP6.5b (C72): set 14, rpMaximalSet14's 32 (512 bytes: never pushed)
    CHECK(engineParmSetMembers(14).size() == std::size_t(EngineParmSetSizes[14]));
    CHECK(engineParmSetMembers(14).size() == 32u);
    CHECK(engineParmSetBlock(14).size == 512u);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::OctahedronCube) == 14);
    CHECK(engineParmSetMembers(14)[24].shaderName == "rpShadowMatrices[0]");
    CHECK(engineParmSetMembers(14)[31].parm == RenderParm::ShadowMatrix1W);
    // LP3.0 (C60): set 11, rpMaximalSet11's 68
    CHECK(engineParmSetMembers(11).size() == std::size_t(EngineParmSetSizes[11]));
    CHECK(engineParmSetMembers(11).size() == 68u);

    CHECK(engineParmSetBlock(0).size == 128u);
    CHECK(engineParmSetBlock(4).size == 256u);
    CHECK(engineParmSetBlock(10).size == 400u);
    CHECK(engineParmSetBlock(11).size == 68u * 16u);
    // LP4.2a (C35): 45 float4 = 720 bytes
    CHECK(engineParmSetBlock(9).size == 720u);
    // LP5.0: 16, 12, 13 and 16 float4
    CHECK(engineParmSetBlock(5).size == 256u);
    CHECK(engineParmSetBlock(7).size == 192u);
    CHECK(engineParmSetBlock(8).size == 208u);
    CHECK(engineParmSetBlock(12).size == 256u);

    for (const auto set : {0, 4, 5, 7, 8, 9, 10, 11, 12})
    {
      CAPTURE(set);
      const auto block = engineParmSetBlock(set);
      auto parms = std::set<int>{};
      auto names = std::set<std::string_view>{};
      for (std::size_t i = 0; i < block.members.size(); ++i)
      {
        CHECK(block.members[i].offset == i * 16);
        CHECK(block.members[i].name.starts_with("rp"));
        names.insert(block.members[i].name);
        const auto parm = engineParmSetMembers(set)[i].parm;
        CHECK(int(parm) < int(RenderParm::Total));
        parms.insert(int(parm));
      }
      CHECK(names.size() == block.members.size());
      CHECK(parms.size() == block.members.size());
    }

    // T0.1: each member's renderparm is the one its HLSL name names, by the engine's
    // naming (RENDERPARM_ and rp dropped, underscores removed, case folded; set 0, 4 and
    // 10's one exception: PROJMATRIX is ProjectionMatrix in the shaders), and the parms
    // are in the order of RenderProgs.h's lists at 285ea4d1 (:248 rpMinimalSet0, :266
    // rpNominalSet4, :296 rpMaximalSet10, copied below as the engine spells them)
    const auto folded = [](std::string_view text, const std::string_view prefix) {
      text.remove_prefix(prefix.size());
      auto result = std::string{};
      for (const auto c : text)
      {
        // set 9's array elements, rpSHIrradiance[k], name RENDERPARM_SH_IRRADIANCE_k
        if (c != '_' && c != '[' && c != ']')
        {
          result += char(std::tolower(static_cast<unsigned char>(c)));
        }
      }
      if (const auto at = result.find("projmatrix"); at != std::string::npos)
      {
        result.replace(at, 10, "projectionmatrix");
      }
      return result;
    };
    const auto engineLists = std::vector<std::pair<int, std::vector<std::string_view>>>{
      {0,
       {"RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_COLOR",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_PSX_DISTORTIONS"}},
      {4,
       {"RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_COLOR",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_TEXTUREMATRIX_S",
        "RENDERPARM_TEXTUREMATRIX_T",
        "RENDERPARM_TEXGEN_0_S",
        "RENDERPARM_TEXGEN_0_T",
        "RENDERPARM_TEXGEN_0_ENABLED",
        "RENDERPARM_ALPHA_TEST",
        "RENDERPARM_PSX_DISTORTIONS"}},
      {10,
       {"RENDERPARM_DIFFUSEMODIFIER",
        "RENDERPARM_SPECULARMODIFIER",
        "RENDERPARM_LOCALLIGHTORIGIN",
        "RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_LIGHTPROJECTION_S",
        "RENDERPARM_LIGHTPROJECTION_T",
        "RENDERPARM_LIGHTPROJECTION_Q",
        "RENDERPARM_LIGHTFALLOFF_S",
        "RENDERPARM_BUMPMATRIX_S",
        "RENDERPARM_BUMPMATRIX_T",
        "RENDERPARM_DIFFUSEMATRIX_S",
        "RENDERPARM_DIFFUSEMATRIX_T",
        "RENDERPARM_SPECULARMATRIX_S",
        "RENDERPARM_SPECULARMATRIX_T",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_PSX_DISTORTIONS",
        "RENDERPARM_USER0",
        "RENDERPARM_USER1",
        "RENDERPARM_USER2"}},
      // LP4.2a (C35): RenderProgs.h:291 rpMaximalSet9, in renderParmSet9.inc.hlsl:33-85's
      // order
      {9,
       {"RENDERPARM_SCREENCORRECTIONFACTOR",
        "RENDERPARM_WINDOWCOORD",
        "RENDERPARM_DIFFUSEMODIFIER",
        "RENDERPARM_SPECULARMODIFIER",
        "RENDERPARM_LOCALLIGHTORIGIN",
        "RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_BUMPMATRIX_S",
        "RENDERPARM_BUMPMATRIX_T",
        "RENDERPARM_DIFFUSEMATRIX_S",
        "RENDERPARM_DIFFUSEMATRIX_T",
        "RENDERPARM_SPECULARMATRIX_S",
        "RENDERPARM_SPECULARMATRIX_T",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_GLOBALEYEPOS",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_MODELMATRIX_X",
        "RENDERPARM_MODELMATRIX_Y",
        "RENDERPARM_MODELMATRIX_Z",
        "RENDERPARM_MODELMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_AMBIENT_COLOR",
        "RENDERPARM_GLOBALLIGHTORIGIN",
        "RENDERPARM_JITTERTEXSCALE",
        "RENDERPARM_JITTERTEXOFFSET",
        "RENDERPARM_PSX_DISTORTIONS",
        "RENDERPARM_CASCADEDISTANCES",
        "RENDERPARM_WOBBLESKY_X",
        "RENDERPARM_WOBBLESKY_Y",
        "RENDERPARM_WOBBLESKY_Z",
        "RENDERPARM_TEXGEN_0_S",
        "RENDERPARM_TEXGEN_0_T",
        "RENDERPARM_TEXGEN_0_Q",
        "RENDERPARM_SH_IRRADIANCE_0",
        "RENDERPARM_SH_IRRADIANCE_1",
        "RENDERPARM_SH_IRRADIANCE_2",
        "RENDERPARM_SH_IRRADIANCE_3",
        "RENDERPARM_SH_IRRADIANCE_4",
        "RENDERPARM_SH_IRRADIANCE_5",
        "RENDERPARM_SH_IRRADIANCE_6",
        "RENDERPARM_SH_IRRADIANCE_7",
        "RENDERPARM_SH_IRRADIANCE_8"}},
      // LP5.0: RenderProgs.h:271 rpNominalSet5, :281 rpNominalSet7, :286 rpNominalSet8,
      // :306 rpNominalSet12, as the engine spells them
      {5,
       {"RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_COLOR",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_MODELMATRIX_X",
        "RENDERPARM_MODELMATRIX_Y",
        "RENDERPARM_MODELMATRIX_Z",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_WOBBLESKY_X",
        "RENDERPARM_WOBBLESKY_Y",
        "RENDERPARM_WOBBLESKY_Z",
        "RENDERPARM_PSX_DISTORTIONS"}},
      {7,
       {"RENDERPARM_COLOR",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_TEXGEN_0_S",
        "RENDERPARM_TEXGEN_0_T",
        "RENDERPARM_TEXGEN_0_Q",
        "RENDERPARM_TEXGEN_1_S",
        "RENDERPARM_TEXGEN_1_T",
        "RENDERPARM_PSX_DISTORTIONS"}},
      {8,
       {"RENDERPARM_WINDOWCOORD",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_Y",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_MODELVIEWMATRIX_Z",
        "RENDERPARM_ENABLE_SKINNING",
        "RENDERPARM_ALPHA_TEST",
        "RENDERPARM_USER0",
        "RENDERPARM_USER1",
        "RENDERPARM_USER2"}},
      {12,
       {"RENDERPARM_SCREENCORRECTIONFACTOR",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_COLOR",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_TEXTUREMATRIX_S",
        "RENDERPARM_TEXTUREMATRIX_T",
        "RENDERPARM_TEXGEN_0_S",
        "RENDERPARM_TEXGEN_0_T",
        "RENDERPARM_TEXGEN_0_Q",
        "RENDERPARM_TEXGEN_0_ENABLED",
        "RENDERPARM_PSX_DISTORTIONS"}},
    };
    for (const auto& [set, list] : engineLists)
    {
      CAPTURE(set);
      const auto members = engineParmSetMembers(set);
      REQUIRE(members.size() == list.size());
      for (std::size_t i = 0; i < members.size(); ++i)
      {
        CAPTURE(i, members[i].shaderName);
        CHECK(renderParmName(members[i].parm) == list[i]);
        CHECK(
          folded(members[i].shaderName, "rp")
          == folded(renderParmName(members[i].parm), "RENDERPARM_"));
      }
    }
    // the rule tells a swapped row, as BumpMatrixT named rpBumpMatrixS
    CHECK(
      folded("rpBumpMatrixS", "rp") != folded("RENDERPARM_BUMPMATRIX_T", "RENDERPARM_"));

    // LP3.0 (C60, SC730, P3.29): set 11, RenderProgs.h:301 rpMaximalSet11 as the engine
    // spells it, in renderParmSet11.inc.hlsl:32-87's order; its two arrays' elements are
    // named rpShadowMatrices[4 · side + row] (RENDERPARM_SHADOW_MATRIX_<side>_<row>) and
    // rpShadowAtlasOffsets[side], the other members by the folding rule
    {
      auto list = std::vector<std::string>{
        "RENDERPARM_SCREENCORRECTIONFACTOR",
        "RENDERPARM_DIFFUSEMODIFIER",
        "RENDERPARM_SPECULARMODIFIER",
        "RENDERPARM_LOCALLIGHTORIGIN",
        "RENDERPARM_LOCALVIEWORIGIN",
        "RENDERPARM_LIGHTPROJECTION_S",
        "RENDERPARM_LIGHTPROJECTION_T",
        "RENDERPARM_LIGHTPROJECTION_Q",
        "RENDERPARM_LIGHTFALLOFF_S",
        "RENDERPARM_BUMPMATRIX_S",
        "RENDERPARM_BUMPMATRIX_T",
        "RENDERPARM_DIFFUSEMATRIX_S",
        "RENDERPARM_DIFFUSEMATRIX_T",
        "RENDERPARM_SPECULARMATRIX_S",
        "RENDERPARM_SPECULARMATRIX_T",
        "RENDERPARM_VERTEXCOLOR_MODULATE",
        "RENDERPARM_VERTEXCOLOR_ADD",
        "RENDERPARM_MVPMATRIX_X",
        "RENDERPARM_MVPMATRIX_Y",
        "RENDERPARM_MVPMATRIX_Z",
        "RENDERPARM_MVPMATRIX_W",
        "RENDERPARM_MODELMATRIX_X",
        "RENDERPARM_MODELMATRIX_Y",
        "RENDERPARM_MODELMATRIX_Z",
        "RENDERPARM_MODELMATRIX_W",
        "RENDERPARM_PROJMATRIX_W",
        "RENDERPARM_MODELVIEWMATRIX_X",
        "RENDERPARM_MODELVIEWMATRIX_Y",
        "RENDERPARM_MODELVIEWMATRIX_Z",
        "RENDERPARM_MODELVIEWMATRIX_W",
        "RENDERPARM_GLOBALLIGHTORIGIN",
        "RENDERPARM_JITTERTEXSCALE",
        "RENDERPARM_JITTERTEXOFFSET",
        "RENDERPARM_PSX_DISTORTIONS",
        "RENDERPARM_CASCADEDISTANCES",
      };
      for (auto side = 0; side < 6; ++side)
      {
        for (const auto* row : {"X", "Y", "Z", "W"})
        {
          list.push_back("RENDERPARM_SHADOW_MATRIX_" + std::to_string(side) + "_" + row);
        }
      }
      for (auto side = 0; side < 6; ++side)
      {
        list.push_back("RENDERPARM_SHADOW_ATLAS_OFFSET_" + std::to_string(side));
      }
      list.insert(
        list.end(), {"RENDERPARM_USER0", "RENDERPARM_USER1", "RENDERPARM_USER2"});
      const auto members = engineParmSetMembers(11);
      REQUIRE(members.size() == list.size());
      for (std::size_t i = 0; i < members.size(); ++i)
      {
        CAPTURE(i, members[i].shaderName);
        CHECK(renderParmName(members[i].parm) == list[i]);
        if (i >= 35 && i < 59)
        {
          CHECK(
            members[i].shaderName == "rpShadowMatrices[" + std::to_string(i - 35) + "]");
        }
        else if (i >= 59 && i < 65)
        {
          CHECK(
            members[i].shaderName
            == "rpShadowAtlasOffsets[" + std::to_string(i - 59) + "]");
        }
        else
        {
          CHECK(
            folded(members[i].shaderName, "rp")
            == folded(renderParmName(members[i].parm), "RENDERPARM_"));
        }
      }
      // the byte offsets of SC730 (P3.29: the matrices end at 943)
      const auto block = engineParmSetBlock(11);
      REQUIRE(block.members.size() == 68u);
      CHECK(block.size == 1088u);
      CHECK(block.members[0] == EngineConstantMember{"rpScreenCorrectionFactor", 0});
      CHECK(block.members[31] == EngineConstantMember{"rpJitterTexScale", 496});
      CHECK(block.members[32] == EngineConstantMember{"rpJitterTexOffset", 512});
      CHECK(block.members[34] == EngineConstantMember{"rpCascadeDistances", 544});
      CHECK(block.members[35] == EngineConstantMember{"rpShadowMatrices[0]", 560});
      CHECK(block.members[58] == EngineConstantMember{"rpShadowMatrices[23]", 928});
      CHECK(block.members[59] == EngineConstantMember{"rpShadowAtlasOffsets[0]", 944});
      CHECK(block.members[64] == EngineConstantMember{"rpShadowAtlasOffsets[5]", 1024});
      CHECK(block.members[65] == EngineConstantMember{"rpUser0", 1040});
      CHECK(block.members[67] == EngineConstantMember{"rpUser2", 1072});
      // the shadow rows are contiguous in the enum, as SetVertexParms writes them
      for (auto i = 0; i < 24; ++i)
      {
        CHECK(
          int(members[std::size_t(35 + i)].parm) == int(RenderParm::ShadowMatrix0X) + i);
      }
      for (auto i = 0; i < 6; ++i)
      {
        CHECK(
          int(members[std::size_t(59 + i)].parm)
          == int(RenderParm::ShadowAtlasOffset0) + i);
      }
    }

    CHECK(
      engineParmSetBlock(10).members[3] == EngineConstantMember{"rpLocalViewOrigin", 48});
    CHECK(engineParmSetBlock(10).members[24] == EngineConstantMember{"rpUser2", 384});

    // LP4.2a (C35): set 9's byte offsets (SC330), the nine SH parms contiguous in the
    // enum (SetVertexParms( RENDERPARM_SH_IRRADIANCE_0, ..., 9 ), RenderBackend.cpp:1335)
    {
      const auto block = engineParmSetBlock(9);
      REQUIRE(block.members.size() == 45u);
      const auto at = [&](const std::string_view name) {
        for (const auto& member : block.members)
        {
          if (member.name == name)
          {
            return std::optional{member.offset};
          }
        }
        return std::optional<std::uint32_t>{};
      };
      CHECK(at("rpScreenCorrectionFactor") == 0u);
      CHECK(at("rpWindowCoord") == 16u);
      CHECK(at("rpDiffuseModifier") == 32u);
      CHECK(at("rpSpecularModifier") == 48u);
      CHECK(at("rpLocalLightOrigin") == 64u);
      CHECK(at("rpLocalViewOrigin") == 80u);
      CHECK(at("rpBumpMatrixS") == 96u);
      CHECK(at("rpBumpMatrixT") == 112u);
      CHECK(at("rpDiffuseMatrixS") == 128u);
      CHECK(at("rpDiffuseMatrixT") == 144u);
      CHECK(at("rpSpecularMatrixS") == 160u);
      CHECK(at("rpSpecularMatrixT") == 176u);
      CHECK(at("rpVertexColorModulate") == 192u);
      CHECK(at("rpVertexColorAdd") == 208u);
      CHECK(at("rpGlobalEyePos") == 224u);
      CHECK(at("rpMVPmatrixX") == 240u);
      CHECK(at("rpMVPmatrixW") == 288u);
      CHECK(at("rpModelMatrixX") == 304u);
      CHECK(at("rpModelMatrixW") == 352u);
      CHECK(at("rpProjectionMatrixW") == 368u);
      CHECK(at("rpAmbientColor") == 384u);
      CHECK(at("rpGlobalLightOrigin") == 400u);
      CHECK(at("rpJitterTexScale") == 416u);
      CHECK(at("rpJitterTexOffset") == 432u);
      CHECK(at("rpPSXDistortions") == 448u);
      CHECK(at("rpCascadeDistances") == 464u);
      CHECK(at("rpWobbleSkyX") == 480u);
      CHECK(at("rpWobbleSkyY") == 496u);
      CHECK(at("rpWobbleSkyZ") == 512u);
      CHECK(at("rpTexGen0S") == 528u);
      CHECK(at("rpTexGen0T") == 544u);
      CHECK(at("rpTexGen0Q") == 560u);
      CHECK(at("rpSHIrradiance[0]") == 576u);
      CHECK(at("rpSHIrradiance[8]") == 704u);
      CHECK(block.members.back().offset + 16u == block.size);
      for (int i = 0; i < 9; ++i)
      {
        CHECK(
          engineParmSetMembers(9)[std::size_t(36 + i)].parm
          == RenderParm(int(RenderParm::ShIrradiance0) + i));
      }
      CHECK(
        engineLayoutConstantBlock(EngineBindingLayout::AmbientLightingIbl)->name
        == "renderParmSet9_t");
      CHECK(
        engineLayoutConstantBlock(EngineBindingLayout::AmbientLightingIblSkinned)->size
        == 720u);
    }

    // every layout is in at most one set, and every set has a layout
    auto sets = std::set<int>{};
    for (auto layout = 0; layout < int(EngineBindingLayout::Num); ++layout)
    {
      const auto set = engineParmSetOfLayout(EngineBindingLayout(layout));
      CHECK(set >= -1);
      CHECK(set < EngineParmSetCount);
      sets.insert(set);
    }
    CHECK(sets.size() == std::size_t(EngineParmSetCount) + 1);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::ConstantBufferOnly) == 0);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::Texture) == 4);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::DrawInteraction) == 10);
    CHECK(engineParmSetOfLayout(EngineBindingLayout::Tonemap) == -1);
  }

  SECTION("the constant blocks of LP2's layouts")
  {
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::ConstantBufferOnly)->size == 128u);
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::Texture)->size == 256u);
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::DrawInteraction)->size == 400u);
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::Tonemap)->size == 80u);
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::Histogram)->name
      == "ToneMappingConstants");
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::Blit)->size == 36u);
    CHECK_FALSE(engineLayoutConstantBlock(EngineBindingLayout::Mipmapgen));
    CHECK_FALSE(engineLayoutConstantBlock(EngineBindingLayout::Debug));
    // LP5.0: the four sets' blocks, named as their HLSL structs
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::Fog)->size == 192u);
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::Blendlight)->name
      == "renderParmSet7_t");
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::PostProcessIngame)->size == 208u);
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::PostProcessIngame)->name
      == "renderParmSet8_t");
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::Legacy)->name == "renderParmSet5_t");
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::NormalCube)->name
      == "renderParmSet5_t");
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::Texgen)->name
      == "renderParmSet12_t");
    CHECK(engineLayoutConstantBlock(EngineBindingLayout::Default)->size == 128u);
    CHECK(
      engineLayoutConstantBlock(EngineBindingLayout::TextureSkinned)->name
      == "renderParmSet4_t");

    for (const auto& block : {engineToneMappingBlock(), engineBlitBlock()})
    {
      CAPTURE(block.name);
      for (std::size_t i = 1; i < block.members.size(); ++i)
      {
        CHECK(block.members[i - 1].offset < block.members[i].offset);
      }
      CHECK(block.members.back().offset < block.size);
    }
  }

  SECTION("binding layouts")
  {
    using L = EngineBindingLayout;
    using T = nvrhi::ResourceType;
    CHECK(setCount(engineBindingLayoutDescs(L::ConstantBufferOnly, true)) == 1u);
    CHECK(setCount(engineBindingLayoutDescs(L::Texture, true)) == 3u);
    CHECK(setCount(engineBindingLayoutDescs(L::DrawInteraction, false)) == 4u);
    CHECK(setCount(engineBindingLayoutDescs(L::Tonemap, true)) == 1u);
    CHECK(setCount(engineBindingLayoutDescs(L::Histogram, true)) == 1u);
    CHECK(setCount(engineBindingLayoutDescs(L::Exposure, true)) == 1u);
    CHECK(setCount(engineBindingLayoutDescs(L::Blit, false)) == 1u);
    CHECK(setCount(engineBindingLayoutDescs(L::AmbientLightingIbl, false)) == 4u);
    CHECK(setCount(engineBindingLayoutDescs(L::AmbientLightingIblSkinned, true)) == 4u);
    CHECK_FALSE(engineBindingLayoutDescs(L::Debug, false));
    // LP6.5b (C72): OCTAHEDRON_CUBE's three sets (uniforms, t0-t6, s0 s1); its skinned
    // form is not mirrored
    CHECK(setCount(engineBindingLayoutDescs(L::OctahedronCube, false)) == 3u);
    CHECK_FALSE(engineBindingLayoutDescs(L::OctahedronCubeSkinned, false));
    CHECK_FALSE(enginePushConstantsEnabled(
      L::OctahedronCube, enginePushConstantLimit(4096), 0x10DE));
    // LP3.0 (C60): DRAW_INTERACTION_SM's four sets; no skinned form (P3.24e)
    CHECK(setCount(engineBindingLayoutDescs(L::DrawInteractionSm, false)) == 4u);
    CHECK_FALSE(engineBindingLayoutDescs(L::DrawInteractionSmSkinned, false));
    CHECK_FALSE(engineBindingSetShape(L::DrawInteractionSmSkinned));

    // the uniforms item: push constants of the set's size, or a volatile buffer at b0
    const auto pushed = *engineBindingLayoutDescs(L::Texture, true);
    REQUIRE(pushed[0].bindings.size() == 1u);
    CHECK(T(pushed[0].bindings[0].type) == T::PushConstants);
    CHECK(pushed[0].bindings[0].size == 256u);
    CHECK(pushed[0].visibility == nvrhi::ShaderType::All);
    CHECK(pushed[1].visibility == nvrhi::ShaderType::Pixel);
    CHECK(T(pushed[2].bindings[0].type) == T::Sampler);

    const auto interaction = *engineBindingLayoutDescs(L::DrawInteraction, false);
    CHECK(T(interaction[0].bindings[0].type) == T::VolatileConstantBuffer);
    CHECK(interaction[1].bindings.size() == 3u);
    CHECK(interaction[2].bindings.size() == 2u);
    CHECK(interaction[2].bindings[0].slot == 3u);
    CHECK(interaction[3].bindings.size() == 2u);
    CHECK(T(interaction[3].bindings[1].type) == T::Sampler);

    // LP4.2a (C35, SC329): RenderProgs.cpp:446-483, the uniforms a volatile buffer of
    // set 9's 720 bytes on desktop (or pushed where the limit allows), t0-t2, then t3 t4
    // t7 t8 t9 t10 t12 t13 t14, then s0 s1, every set but the uniforms' pixel-only
    for (const auto iblPushed : {false, true})
    {
      CAPTURE(iblPushed);
      for (const auto layout : {L::AmbientLightingIbl, L::AmbientLightingIblSkinned})
      {
        CAPTURE(int(layout));
        const auto ibl = *engineBindingLayoutDescs(layout, iblPushed);
        REQUIRE(ibl.size() == 4u);
        CHECK(ibl[0].visibility == nvrhi::ShaderType::All);
        const auto skinned = layout == L::AmbientLightingIblSkinned;
        REQUIRE(ibl[0].bindings.size() == (skinned ? 2u : 1u));
        CHECK(
          T(ibl[0].bindings[0].type)
          == (iblPushed ? T::PushConstants : T::VolatileConstantBuffer));
        CHECK(ibl[0].bindings[0].slot == 0u);
        if (iblPushed)
        {
          CHECK(ibl[0].bindings[0].size == 720u);
        }
        if (skinned)
        {
          CHECK(T(ibl[0].bindings[1].type) == T::StructuredBuffer_SRV);
          CHECK(ibl[0].bindings[1].slot == 11u);
        }
        const auto slots = [](const nvrhi::BindingLayoutDesc& desc) {
          auto result = std::vector<std::uint32_t>{};
          for (const auto& item : desc.bindings)
          {
            result.push_back(item.slot);
          }
          return result;
        };
        CHECK(ibl[1].visibility == nvrhi::ShaderType::Pixel);
        CHECK(slots(ibl[1]) == std::vector<std::uint32_t>{0, 1, 2});
        CHECK(ibl[2].visibility == nvrhi::ShaderType::Pixel);
        CHECK(slots(ibl[2]) == std::vector<std::uint32_t>{3, 4, 7, 8, 9, 10, 12, 13, 14});
        for (const auto& item : ibl[2].bindings)
        {
          CHECK(T(item.type) == T::Texture_SRV);
        }
        CHECK(ibl[3].visibility == nvrhi::ShaderType::Pixel);
        CHECK(slots(ibl[3]) == std::vector<std::uint32_t>{0, 1});
        CHECK(T(ibl[3].bindings[0].type) == T::Sampler);
        CHECK(T(ibl[3].bindings[1].type) == T::Sampler);
      }
    }

    const auto tonemap = *engineBindingLayoutDescs(L::Tonemap, true);
    REQUIRE(tonemap[0].bindings.size() == 5u);
    CHECK(tonemap[0].bindings[0].size == 80u);
    CHECK(T(tonemap[0].bindings[2].type) == T::TypedBuffer_SRV);
    CHECK(tonemap[0].bindings[2].slot == 1u);

    const auto blit = *engineBindingLayoutDescs(L::Blit, false);
    CHECK(T(blit[0].bindings[0].type) == T::PushConstants);
    CHECK(blit[0].bindings[0].size == 36u);

    // the shifts of SC251
    CHECK(engineVulkanBinding(nvrhi::BindingLayoutItem::Texture_SRV(2)) == 2u);
    CHECK(engineVulkanBinding(nvrhi::BindingLayoutItem::Sampler(1)) == 129u);
    CHECK(
      engineVulkanBinding(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)) == 256u);
    CHECK(engineVulkanBinding(nvrhi::BindingLayoutItem::TypedBuffer_UAV(0)) == 384u);
    CHECK(engineVulkanBinding(nvrhi::BindingLayoutItem::TypedBuffer_SRV(1)) == 1u);
  }

  SECTION("LP5.0's binding layouts and binding sets")
  {
    using L = EngineBindingLayout;
    using T = nvrhi::ResourceType;
    using V = nvrhi::ShaderType;
    using Source = EngineBindingSetItem::Source;
    using C = EngineCommonSampler;

    // A transcription of RenderProgs.cpp:415-665 (the layouts) and
    // RenderBackend_NVRHI.cpp:586-1734 (the binding sets) at 285ea4d1, written out by
    // hand: per descriptor set its visibility and items (type, register), and per item
    // what the backend binds there. 'U' is the uniforms item, pushed or a volatile
    // constant buffer; 'J' the joint buffer; 't' a texture; 's' a sampler.
    struct WantItem
    {
      char type = 'U';
      std::uint32_t slot = 0;
      Source source = Source::Uniforms;
      std::uint32_t unit = 0;
      C sampler = C::PointClamp;
    };
    struct WantSet
    {
      V visibility = V::All;
      std::vector<WantItem> items;
    };
    const auto U = WantItem{'U', 0, Source::Uniforms, 0, C::PointClamp};
    const auto J = WantItem{'J', 11, Source::JointBuffer, 0, C::PointClamp};
    const auto t = [](const std::uint32_t slot) {
      return WantItem{'t', slot, Source::ImageUnit, slot, C::PointClamp};
    };
    const auto own = WantItem{'s', 0, Source::ImageSampler, 0, C::PointClamp};
    const auto s = [](const std::uint32_t slot, const C sampler) {
      return WantItem{'s', slot, Source::CommonSampler, 0, sampler};
    };
    const auto transcription = std::vector<std::pair<L, std::vector<WantSet>>>{
      // :426, :586-623
      {L::Default, {{V::All, {U}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      // :427, :624-665
      {L::DefaultSkinned, {{V::All, {U, J}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      // :433, :624-665
      {L::TextureSkinned, {{V::All, {U, J}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      // :434, :586-623
      {L::Texgen, {{V::All, {U}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      // :436, :586-623
      {L::Legacy, {{V::All, {U}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      // :651-665, :1677-1734
      {L::NormalCube,
       {{V::All, {U}}, {V::Pixel, {t(0), t(1)}}, {V::Pixel, {s(0, C::LinearWrap)}}}},
      // :556-570, :1286-1329
      {L::Fog,
       {{V::All, {U}},
        {V::Pixel, {t(0), t(1)}},
        {V::Pixel, {s(0, C::LinearClamp), s(1, C::LinearClamp)}}}},
      // :572-586, :1378-1418
      {L::Blendlight,
       {{V::All, {U}}, {V::Pixel, {t(0), t(1)}}, {V::Pixel, {s(0, C::LinearBorder)}}}},
      // :588-602, :1464-1512
      {L::PostProcessIngame,
       {{V::All, {U, t(0), t(1), t(2)}}, {V::Pixel, {s(0, C::LinearClamp)}}}},
      // :525-528, :997-1081 (row 22, interactionAmbient_skinned)
      {L::DrawInteractionSkinned,
       {{V::All, {U, J}},
        {V::Pixel, {t(0), t(1), t(2)}},
        {V::Pixel, {t(3), t(4)}},
        {V::Pixel, {s(0, C::AnisotropicWrap), s(1, C::LinearBorder)}}}},
      // LP3.0 (C60, SC728): :530-550, :1082-1181 (rows 39, 41, 43, 45, 47, 49 and the
      // non-atlas 27-37); t5 the atlas, t6 _blueNoise256; s2 LinearClampCompare, s3 the
      // blue noise's PointWrap
      {L::DrawInteractionSm,
       {{V::All, {U}},
        {V::Pixel, {t(0), t(1), t(2)}},
        {V::Pixel, {t(3), t(4), t(5), t(6)}},
        {V::Pixel,
         {s(0, C::AnisotropicWrap),
          s(1, C::LinearBorder),
          s(2, C::LinearClampCompare),
          s(3, C::PointWrap)}}}},
      // the earlier ones, for their binding sets: :432 and :586-623, :521 and :916-996
      {L::Texture, {{V::All, {U}}, {V::Pixel, {t(0)}}, {V::Pixel, {own}}}},
      {L::DrawInteraction,
       {{V::All, {U}},
        {V::Pixel, {t(0), t(1), t(2)}},
        {V::Pixel, {t(3), t(4)}},
        {V::Pixel, {s(0, C::AnisotropicWrap), s(1, C::LinearBorder)}}}},
    };
    // the sets' sizes (RenderProgs.cpp:217-391: rpBufSize = the list's length · 16)
    const auto setBytes = std::map<L, std::uint32_t>{
      {L::Default, 128},
      {L::DefaultSkinned, 128},
      {L::TextureSkinned, 256},
      {L::Texgen, 256},
      {L::Legacy, 256},
      {L::NormalCube, 256},
      {L::Fog, 192},
      {L::Blendlight, 192},
      {L::PostProcessIngame, 208},
      {L::DrawInteractionSkinned, 400},
      {L::Texture, 256},
      {L::DrawInteraction, 400},
      {L::DrawInteractionSm, 1088},
    };

    for (const auto& [layout, wantSets] : transcription)
    {
      CAPTURE(int(layout));
      const auto shape = engineBindingSetShape(layout);
      REQUIRE(shape);
      REQUIRE(shape->size() == wantSets.size());
      for (const auto pushed : {false, true})
      {
        CAPTURE(pushed);
        const auto descs = engineBindingLayoutDescs(layout, pushed);
        REQUIRE(descs);
        REQUIRE(descs->size() == wantSets.size());
        for (std::size_t set = 0; set < wantSets.size(); ++set)
        {
          CAPTURE(set);
          const auto& want = wantSets[set];
          const auto& desc = (*descs)[set];
          CHECK(desc.visibility == want.visibility);
          REQUIRE(desc.bindings.size() == want.items.size());
          REQUIRE((*shape)[set].size() == want.items.size());
          for (std::size_t i = 0; i < want.items.size(); ++i)
          {
            CAPTURE(i);
            const auto& item = desc.bindings[i];
            const auto& wantItem = want.items[i];
            CHECK(item.slot == wantItem.slot);
            switch (wantItem.type)
            {
            case 'U':
              CHECK(
                T(item.type) == (pushed ? T::PushConstants : T::VolatileConstantBuffer));
              if (pushed)
              {
                CHECK(item.size == setBytes.at(layout));
              }
              break;
            case 'J':
              CHECK(T(item.type) == T::StructuredBuffer_SRV);
              break;
            case 't':
              CHECK(T(item.type) == T::Texture_SRV);
              break;
            default:
              CHECK(T(item.type) == T::Sampler);
              break;
            }
            const auto& made = (*shape)[set][i];
            CHECK(made.source == wantItem.source);
            CHECK(made.slot == wantItem.slot);
            if (made.source == Source::ImageUnit || made.source == Source::ImageSampler)
            {
              CHECK(made.unit == wantItem.unit);
            }
            if (made.source == Source::CommonSampler)
            {
              CHECK(made.sampler == wantItem.sampler);
            }
          }
        }
      }
      // the constant block of the layout's set, its size the uniforms' push size
      const auto block = engineLayoutConstantBlock(layout);
      REQUIRE(block);
      CHECK(block->size == setBytes.at(layout));
    }

    // every layout with a binding-set shape has layouts of the same shape, and the
    // passes' own layouts have none
    for (auto index = 0; index < int(L::Num); ++index)
    {
      const auto layout = L(index);
      CAPTURE(index);
      const auto shape = engineBindingSetShape(layout);
      if (!shape)
      {
        continue;
      }
      const auto descs = engineBindingLayoutDescs(layout, false);
      REQUIRE(descs);
      REQUIRE(descs->size() == shape->size());
      for (std::size_t set = 0; set < shape->size(); ++set)
      {
        REQUIRE((*descs)[set].bindings.size() == (*shape)[set].size());
        for (std::size_t i = 0; i < (*shape)[set].size(); ++i)
        {
          CHECK((*descs)[set].bindings[i].slot == (*shape)[set][i].slot);
        }
      }
    }
    CHECK_FALSE(engineBindingSetShape(L::Tonemap));
    CHECK_FALSE(engineBindingSetShape(L::Blit));
    // LP6.5b (C72): OCTAHEDRON_CUBE's units 0-6 and its two common samplers
    REQUIRE(engineBindingSetShape(L::OctahedronCube));
    CHECK((*engineBindingSetShape(L::OctahedronCube))[1].size() == 7u);
    CHECK((*engineBindingSetShape(L::OctahedronCube))[2].size() == 2u);

    // the samplers as the engine creates them (CommonPasses.cpp:71-97): FOG's LinearClamp
    // and BLENDLIGHT's LinearBorder before the leaked Comparison, NORMAL_CUBE's
    // LinearWrap after it (D68; P6.19: its first use on a colour cube is LP5.2b's to
    // measure)
    using A = nvrhi::SamplerAddressMode;
    using R = nvrhi::SamplerReductionType;
    const auto linearClamp = engineCommonSamplerDesc(C::LinearClamp);
    CHECK(linearClamp.minFilter);
    CHECK(linearClamp.magFilter);
    CHECK(linearClamp.mipFilter);
    CHECK(linearClamp.addressU == A::Clamp);
    CHECK(linearClamp.addressW == A::Clamp);
    CHECK(linearClamp.reductionType == R::Standard);
    const auto linearBorder = engineCommonSamplerDesc(C::LinearBorder);
    CHECK(linearBorder.addressV == A::Border);
    CHECK(linearBorder.borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(linearBorder.reductionType == R::Standard);
    // LP3.0 (C60, SC728, E-LIT-14): DRAW_INTERACTION_SM's s2, the Border desc (0, 0, 0,
    // 1) with the Comparison reduction: "Clamp" is the engine's misnomer
    const auto linearClampCompare = engineCommonSamplerDesc(C::LinearClampCompare);
    CHECK(linearClampCompare.minFilter);
    CHECK(linearClampCompare.addressU == A::Border);
    CHECK(linearClampCompare.addressV == A::Border);
    CHECK(linearClampCompare.addressW == A::Border);
    CHECK(linearClampCompare.borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(linearClampCompare.reductionType == R::Comparison);
    const auto linearWrap = engineCommonSamplerDesc(C::LinearWrap);
    CHECK(linearWrap.minFilter);
    CHECK(linearWrap.addressU == A::Wrap);
    CHECK(linearWrap.addressV == A::Wrap);
    CHECK(linearWrap.addressW == A::Wrap);
    CHECK(linearWrap.reductionType == R::Comparison);
    CHECK(linearWrap.borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(linearWrap.maxAnisotropy == 1.0f);
    CHECK(engineCommonSamplerDesc(C::AnisotropicWrap).maxAnisotropy == 16.0f);
    CHECK(engineCommonSamplerDesc(C::PointClamp).reductionType == R::Standard);
    CHECK_FALSE(engineCommonSamplerDesc(C::PointWrap).minFilter);

    // the push-constant limit (C47, C49): sets 7 (192 bytes) and 8 (208) fit this
    // machine's 256-byte maxPushConstantsSize, so FOG, BLENDLIGHT and POST_PROCESS_INGAME
    // push (pcEnabled, RenderProgs.cpp:393); sets 5 and 12 are exactly 256 and push too;
    // under a set's size a layout is a volatile constant buffer
    const auto limit = enginePushConstantLimit(256);
    CHECK(limit == 256u);
    for (const auto layout :
         {L::Fog,
          L::Blendlight,
          L::PostProcessIngame,
          L::Legacy,
          L::NormalCube,
          L::Texgen,
          L::Default})
    {
      CAPTURE(int(layout));
      CHECK(enginePushConstantsEnabled(layout, limit, 0x10DE));
      CHECK(enginePushConstantsEnabled(layout, limit, EngineVendorAmd));
      CHECK(enginePushConstantsEnabled(layout, setBytes.at(layout), 0x10DE));
      CHECK_FALSE(enginePushConstantsEnabled(layout, setBytes.at(layout) - 1, 0x10DE));
    }
    // TEXTURE_SKINNED is AMD's workaround (RenderProgs.cpp:403-413)
    CHECK_FALSE(enginePushConstantsEnabled(L::TextureSkinned, limit, EngineVendorAmd));
    CHECK(enginePushConstantsEnabled(L::TextureSkinned, limit, 0x10DE));
    // DRAW_INTERACTION_SKINNED's 400 bytes never push here
    CHECK_FALSE(enginePushConstantsEnabled(L::DrawInteractionSkinned, limit, 0x10DE));
  }

  SECTION("LP5.0: the material programs (C47, D121)")
  {
    // shaders.cfg:130-154 at 285ea4d1 (neo/shaders/shaders.cfg), the "User shaders",
    // transcribed line for line
    const auto cfg = std::vector<std::string>{
      "bloodorb_draw.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "bloodorb_draw.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "bloodorb1_capture.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "bloodorb1_capture.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "bloodorb2_capture.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "bloodorb2_capture.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "bloodorb3_capture.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "bloodorb3_capture.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "colorProcess.vs.hlsl  -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "colorProcess.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "enviroSuit.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "enviroSuit.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "fogwithlights.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "fogwithlights.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "heathaze.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "heathaze.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "heatHazeWithMask.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "heatHazeWithMask.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "heatHazeWithMaskAndVertex.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "heatHazeWithMaskAndVertex.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D "
      "USE_PUSH_CONSTANTS={0,1}",
      "mattiascrt.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "mattiascrt.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "vornoipattern.vs.hlsl -T vs -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
      "vornoipattern.ps.hlsl -T ps -D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}",
    };
    REQUIRE(cfg.size() == 24u);
    REQUIRE(EngineMaterialPrograms.size() == cfg.size() / 2);
    for (std::size_t i = 0; i < EngineMaterialPrograms.size(); ++i)
    {
      const auto& program = EngineMaterialPrograms[i];
      const auto& row = program.row;
      CAPTURE(i, row.name);
      const auto& vs = cfg[2 * i];
      const auto& ps = cfg[2 * i + 1];
      // the name, a vertex then a pixel line, the two defines in the cfg's order
      CHECK(vs.starts_with(std::string{row.name} + ".vs.hlsl "));
      CHECK(ps.starts_with(std::string{row.name} + ".ps.hlsl "));
      CHECK(vs.find("-T vs") != std::string::npos);
      CHECK(ps.find("-T ps") != std::string::npos);
      REQUIRE(row.defineCount == 2u);
      CHECK(std::string_view{row.defines[0].name} == "USE_GPU_SKINNING");
      CHECK(std::string_view{row.defines[0].value} == "0");
      CHECK(std::string_view{row.defines[1].name} == "USE_PUSH_CONSTANTS");
      CHECK(row.defines[1].value == nullptr);
      CHECK(
        vs.find("-D USE_GPU_SKINNING={0,1} -D USE_PUSH_CONSTANTS={0,1}")
        != std::string::npos);
      CHECK(row.index == EngineMaterialProgramBase + int(i));
      CHECK(std::string_view{row.nameOutSuffix}.empty());
      CHECK(row.stages == EngineStageDefault);
      CHECK(row.vertexLayout == EngineVertexLayout::DrawVert);
      CHECK(row.bindingLayout == EngineBindingLayout::PostProcessIngame);
      CHECK_FALSE(row.requireGPUSkinningSupport);
      // C47: every one but vornoipattern and fogwithlights reads _currentRender (the
      // blobs' reflection, [retail]: fogwithlights declares t_CurrentRender in its HLSL
      // and never samples it)
      CHECK(
        program.readsCurrentRender
        == (row.name != std::string_view{"vornoipattern"} && row.name != std::string_view{"fogwithlights"}));
      // engineProgram reaches it by its index, and the builtins as before
      CHECK(engineProgram(row.index) == &row);
      // MAT:2180-2189: USE_PUSH_CONSTANTS is POST_PROCESS_INGAME's pcEnabled, "1" here
      const auto defines = engineBuiltinDefines(row, true);
      REQUIRE(defines.size() == 2u);
      CHECK(defines[0] == ShaderDefine{"USE_GPU_SKINNING", "0"});
      CHECK(defines[1] == ShaderDefine{"USE_PUSH_CONSTANTS", "1"});
      CHECK(
        engineBuiltinDefines(row, false)[1] == ShaderDefine{"USE_PUSH_CONSTANTS", "0"});
    }
    CHECK(
      enginePushConstantsEnabled(EngineBindingLayout::PostProcessIngame, 256, 0x10DE));
    CHECK(engineProgram(0) == engineBuiltin(0));
    CHECK(engineProgram(115) == engineBuiltin(115));
    CHECK(engineProgram(116) == nullptr);
    CHECK(engineProgram(EngineMaterialProgramBase - 1) == nullptr);
    CHECK(engineProgram(EngineMaterialProgramBase + 12) == nullptr);
    CHECK(engineProgram(-1) == nullptr);
    CHECK(engineBuiltin(EngineMaterialProgramBase) == nullptr);

    // FindShader's name rule (RenderProgs.cpp:1159-1166): the extension stripped, the
    // case ignored (C1's)
    const auto* voronoi = findEngineMaterialProgram("vornoipattern");
    REQUIRE(voronoi);
    CHECK(voronoi == &EngineMaterialPrograms[11]);
    CHECK(findEngineMaterialProgram("VornoiPattern") == voronoi);
    CHECK(findEngineMaterialProgram("vornoipattern.vfp") == voronoi);
    CHECK(findEngineMaterialProgram("VORNOIPATTERN.VFP") == voronoi);
    CHECK(findEngineMaterialProgram("heatHaze.vfp") == &EngineMaterialPrograms[7]);
    CHECK(findEngineMaterialProgram("HEATHAZEWITHMASK") == &EngineMaterialPrograms[8]);
    CHECK(findEngineMaterialProgram("heatHazeWithMask.v") == &EngineMaterialPrograms[8]);
    CHECK(findEngineMaterialProgram("heatHazeWith") == nullptr);
    CHECK(findEngineMaterialProgram("progs/heathaze.vfp") == nullptr);
    CHECK(findEngineMaterialProgram("interaction") == nullptr);
    CHECK(findEngineMaterialProgram("") == nullptr);
  }

  SECTION("LP5.0: the frame cvars of passes 7-9 (C46, C49)")
  {
    // RenderSystem_init.cpp:128, 135-139, 246 and RenderBackend.cpp:55 at 285ea4d1: every
    // one "0"; r_skipAmbient is the preview's choice (the stage pass drawn), which is
    // also the engine's default
    CHECK_FALSE(EngineFrame.skipFogLights);
    CHECK_FALSE(EngineFrame.skipBlendLights);
    CHECK_FALSE(EngineFrame.skipTranslucent);
    CHECK_FALSE(EngineFrame.skipAmbient);
    CHECK_FALSE(EngineFrame.skipShaderPasses);
    CHECK_FALSE(EngineFrame.skipPostProcess);
    CHECK_FALSE(EngineFrame.skipNewAmbient);
    CHECK(EngineFrame.showMathLights == 0);
    CHECK(EngineFrame.mathLightsEnabled);
  }

  SECTION("the vertex input")
  {
    const auto attributes = engineDrawVertAttributes();
    REQUIRE(attributes.size() == 5u);
    CHECK(attributes[0].name == "POSITION");
    CHECK(attributes[0].format == nvrhi::Format::RGB32_FLOAT);
    CHECK(attributes[1].name == "TEXCOORD");
    CHECK(attributes[1].offset == 12u);
    CHECK(attributes[1].format == nvrhi::Format::RG16_FLOAT);
    // T0.5: RenderProgs.cpp:145-165's names and formats (a wrong one is silent)
    CHECK(attributes[2].name == "NORMAL");
    CHECK(attributes[2].offset == 16u);
    CHECK(attributes[2].format == nvrhi::Format::RGBA8_UNORM);
    CHECK(attributes[3].name == "TANGENT");
    CHECK(attributes[3].offset == 20u);
    CHECK(attributes[3].format == nvrhi::Format::RGBA8_UNORM);
    CHECK(attributes[4].name == "COLOR");
    CHECK(attributes[4].offset == 24u);
    CHECK(attributes[4].format == nvrhi::Format::RGBA8_UNORM);
    CHECK(attributes[4].arraySize == 2u);
    for (const auto& attribute : attributes)
    {
      CHECK(attribute.elementStride == 32u);
      CHECK(attribute.bufferIndex == 0u);
    }
    // one location per attribute element
    CHECK(engineDrawVertInputs().size() == 6u);
    CHECK(engineDrawVertInputs()[5].location == 5u);
  }

  SECTION("CommonPasses' samplers")
  {
    using A = nvrhi::SamplerAddressMode;
    using R = nvrhi::SamplerReductionType;
    const auto s = engineCommonSamplers();
    CHECK_FALSE(s.pointClamp.minFilter);
    CHECK(s.pointWrap.addressU == A::Wrap);
    CHECK(s.linearClamp.minFilter);
    CHECK(s.linearClamp.reductionType == R::Standard);
    CHECK(s.linearBorder.addressU == A::Border);
    CHECK(s.linearBorder.borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(s.linearBorder.reductionType == R::Standard);
    // the leaked desc (D68): every sampler from here on compares
    CHECK(s.linearClampCompare.reductionType == R::Comparison);
    CHECK(s.linearWrap.reductionType == R::Comparison);
    CHECK(s.linearWrap.addressU == A::Wrap);
    CHECK(s.anisotropicWrap.reductionType == R::Comparison);
    CHECK(s.anisotropicWrap.maxAnisotropy == 16.0f);
    CHECK(s.anisotropicClampEdge.reductionType == R::Comparison);
    CHECK(s.anisotropicClampEdge.addressU == A::ClampToEdge);
    CHECK(s.anisotropicClampEdge.maxAnisotropy == 16.0f);
  }

  SECTION("the per-image sampler")
  {
    namespace ei = engine_image;
    using A = nvrhi::SamplerAddressMode;
    const auto repeat =
      engineImageSamplerDesc(ei::FormatDxt5, ei::FilterDefault, ei::RepeatRepeat);
    REQUIRE(repeat);
    CHECK(repeat->maxAnisotropy == 8.0f);
    CHECK(repeat->minFilter);
    CHECK(repeat->addressV == A::Repeat);
    CHECK(repeat->reductionType == nvrhi::SamplerReductionType::Standard);

    const auto zero =
      engineImageSamplerDesc(ei::FormatRgb565, ei::FilterLinear, ei::RepeatClampToZero);
    REQUIRE(zero);
    CHECK(zero->addressU == A::ClampToBorder);
    CHECK(zero->borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(zero->maxAnisotropy == 1.0f);

    const auto zeroAlpha = engineImageSamplerDesc(
      ei::FormatRgba8, ei::FilterNearest, ei::RepeatClampToZeroAlpha);
    REQUIRE(zeroAlpha);
    CHECK(zeroAlpha->borderColor == nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f});
    CHECK_FALSE(zeroAlpha->magFilter);

    const auto mipmap = engineImageSamplerDesc(
      ei::FormatDepthStencil, ei::FilterNearestMipmap, ei::RepeatClamp);
    REQUIRE(mipmap);
    CHECK(mipmap->reductionType == nvrhi::SamplerReductionType::Comparison);
    CHECK(mipmap->mipFilter);
    CHECK_FALSE(mipmap->minFilter);

    CHECK_FALSE(engineImageSamplerDesc(ei::FormatRgba8, 7, ei::RepeatRepeat));
    CHECK_FALSE(engineImageSamplerDesc(ei::FormatRgba8, ei::FilterLinear, 9));
  }

  SECTION("image formats and swizzles")
  {
    namespace ei = engine_image;
    using F = nvrhi::Format;
    using C = nvrhi::ComponentSwizzle;
    CHECK(engineTextureFormat(ei::FormatDxt1, true) == F::BC1_UNORM);
    CHECK(engineTextureFormat(ei::FormatDxt5, true) == F::BC3_UNORM);
    CHECK(engineTextureFormat(ei::FormatBc6h, true) == F::BC6H_UFLOAT);
    CHECK(engineTextureFormat(ei::FormatRgb565, true) == F::B5G6R5_UNORM);
    CHECK(engineTextureFormat(ei::FormatLum8, true) == F::R8_UNORM);
    CHECK(engineTextureFormat(ei::FormatL8a8, true) == F::RG8_UNORM);
    CHECK(engineTextureFormat(ei::FormatDepthStencil, true) == F::D24S8);
    CHECK(engineTextureFormat(ei::FormatDepthStencil, false) == F::D32S8);
    CHECK(engineTextureFormat(ei::FormatRgba16f, true) == F::RGBA16_FLOAT);
    CHECK(engineTextureFormat(ei::FormatX16, true) == F::RGBA8_UINT);
    CHECK_FALSE(engineTextureFormat(ei::FormatBc7, true));
    CHECK_FALSE(engineTextureFormat(ei::FormatEtc1, true));
    CHECK_FALSE(engineTextureFormat(ei::FormatNone, true));

    const auto greenAlpha = engineComponentMapping(ei::FormatDxt1, ei::ColorGreenAlpha);
    CHECK(greenAlpha.r == C::One);
    CHECK(greenAlpha.a == C::Green);
    const auto lum = engineComponentMapping(ei::FormatLum8, ei::ColorDefault);
    CHECK(lum.g == C::Red);
    CHECK(lum.a == C::One);
    CHECK(engineComponentMapping(ei::FormatInt8, ei::ColorDefault).a == C::Red);
    CHECK(engineComponentMapping(ei::FormatAlpha, ei::ColorDefault).a == C::Red);
    CHECK(engineComponentMapping(ei::FormatL8a8, ei::ColorDefault).a == C::Green);
    CHECK(engineComponentMapping(ei::FormatR11g11b10f, ei::ColorDefault).a == C::One);
    const auto identity = engineComponentMapping(ei::FormatDxt5, ei::ColorYCoCgDxt5);
    CHECK(identity.r == C::Red);
    CHECK(identity.a == C::Alpha);
  }

  SECTION("the push-constant rule (C30, SC242)")
  {
    using L = EngineBindingLayout;
    constexpr auto nvidia = std::uint32_t{0x10DE};
    CHECK(enginePushConstantLimit(4096, 256) == 256u);
    CHECK(enginePushConstantLimit(128, 256) == 128u);
    CHECK(enginePushConstantLimit(4096, 4096) == 4096u);

    // Windows and Linux: NVRHI's 256 bytes
    const auto desktop = enginePushConstantLimit(4096, 256);
    CHECK(enginePushConstantsEnabled(L::ConstantBufferOnly, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::Texture, desktop, nvidia));
    CHECK_FALSE(enginePushConstantsEnabled(L::DrawInteraction, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::Tonemap, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::Histogram, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::Exposure, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::Mipmapgen, desktop, nvidia));
    CHECK(enginePushConstantsEnabled(L::TaaResolve, desktop, nvidia));
    CHECK_FALSE(enginePushConstantsEnabled(L::Blit, desktop, nvidia));
    CHECK_FALSE(enginePushConstantsEnabled(L::AmbientLightingIbl, desktop, nvidia));
    // LP3.0 (C60, C30): set 11's 1088 bytes are never pushed on the desktop, on AMD
    // either
    CHECK_FALSE(enginePushConstantsEnabled(L::DrawInteractionSm, desktop, nvidia));
    CHECK_FALSE(
      enginePushConstantsEnabled(L::DrawInteractionSm, desktop, EngineVendorAmd));

    // MoltenVK: 4096 bytes, so the interaction's 400 are pushed (P2.10)
    const auto apple = enginePushConstantLimit(4096, 4096);
    CHECK(enginePushConstantsEnabled(L::DrawInteraction, apple, nvidia));
    CHECK(enginePushConstantsEnabled(L::AmbientLightingIbl, apple, nvidia));
    CHECK(enginePushConstantsEnabled(L::Texture, apple, nvidia));

    // AMD: GBUFFER and TEXTURE (and their skinned forms) are forced to a buffer
    CHECK_FALSE(enginePushConstantsEnabled(L::Texture, desktop, EngineVendorAmd));
    CHECK_FALSE(enginePushConstantsEnabled(L::TextureSkinned, desktop, EngineVendorAmd));
    CHECK_FALSE(enginePushConstantsEnabled(L::Gbuffer, desktop, EngineVendorAmd));
    CHECK(enginePushConstantsEnabled(L::ConstantBufferOnly, desktop, EngineVendorAmd));
    CHECK(enginePushConstantsEnabled(L::Tonemap, desktop, EngineVendorAmd));
    CHECK_FALSE(enginePushConstantsEnabled(L::Texture, apple, EngineVendorAmd));

    // a device that offers only Vulkan's guaranteed 128 bytes
    const auto small = enginePushConstantLimit(128, 256);
    CHECK(enginePushConstantsEnabled(L::ConstantBufferOnly, small, nvidia));
    CHECK_FALSE(enginePushConstantsEnabled(L::Texture, small, nvidia));
    CHECK(enginePushConstantsEnabled(L::Tonemap, small, nvidia));
    CHECK_FALSE(enginePushConstantsEnabled(L::TaaResolve, small, nvidia));
  }

  SECTION("the tonemap constants")
  {
    CHECK(EngineTonemap.histogramBins == 256u);
    CHECK(EngineTonemap.exposureLogMax - EngineTonemap.exposureLogMin == 14.0f);
    CHECK(EngineTonemap.hdrMinLuminance == 0.02f);
    CHECK(EngineTonemap.hdrMaxLuminance == 0.5f);
  }
}

TEST_CASE("EnginePipelineState")
{
  SECTION("engineRenderState: the copied GetRenderState")
  {
    namespace gls = engine_gls;
    // GetOrCreatePipeline's defaults (PipelineCache.cpp:78-91)
    const auto defaults = [] {
      auto state = nvrhi::RenderState{};
      state.rasterState.enableScissor();
      state.depthStencilState.enableDepthTest().enableDepthWrite();
      for (auto& target : state.blendState.targets)
      {
        target.enableBlend();
      }
      return state;
    };

    // the prepass (GLS_DEFAULT)
    auto prepass = defaults();
    engineRenderState(gls::Default, 0, 0.0f, prepass);
    CHECK(prepass.rasterState.cullMode == nvrhi::RasterCullMode::Front);
    CHECK(prepass.rasterState.frontCounterClockwise);
    CHECK_FALSE(prepass.blendState.targets[0].blendEnable);
    CHECK(prepass.depthStencilState.depthFunc == nvrhi::ComparisonFunc::LessOrEqual);
    CHECK(prepass.depthStencilState.depthWriteEnable);
    CHECK_FALSE(prepass.depthStencilState.stencilEnable);
    CHECK(prepass.rasterState.depthBias == 0);

    // the interactions (RB:1736-1744): additive, no depth write, EQUAL, stencil on
    auto interaction = defaults();
    engineRenderState(
      gls::SrcBlendOne | gls::DstBlendOne | gls::DepthMask | gls::DepthFuncEqual
        | gls::StencilFuncAlways,
      0,
      0.0f,
      interaction);
    CHECK(interaction.blendState.targets[0].blendEnable);
    CHECK(interaction.blendState.targets[0].srcBlend == nvrhi::BlendFactor::One);
    CHECK(interaction.blendState.targets[0].destBlend == nvrhi::BlendFactor::One);
    // the alpha factors stay at NVRHI's defaults (SC210)
    CHECK(interaction.blendState.targets[0].srcBlendAlpha == nvrhi::BlendFactor::One);
    CHECK(interaction.blendState.targets[0].destBlendAlpha == nvrhi::BlendFactor::Zero);
    CHECK_FALSE(interaction.depthStencilState.depthWriteEnable);
    CHECK(interaction.depthStencilState.depthFunc == nvrhi::ComparisonFunc::Equal);
    CHECK(interaction.depthStencilState.stencilEnable);

    // a perforated surface's polygon offset: the int bias and the slope
    auto offset = defaults();
    engineRenderState(gls::PolygonOffset, -600, 0.5f, offset);
    CHECK(offset.rasterState.depthBias == -600);
    CHECK(offset.rasterState.slopeScaledDepthBias == 0.5f);

    // GLS_CULL_BITS is 2 << 15, so a back-sided cull falls to the default (SC209)
    auto backSided = defaults();
    engineRenderState(1ull << 15, 0, 0.0f, backSided);
    CHECK(backSided.rasterState.cullMode == nvrhi::RasterCullMode::Front);
    auto twoSided = defaults();
    engineRenderState(gls::CullTwoSided, 0, 0.0f, twoSided);
    CHECK(twoSided.rasterState.cullMode == nvrhi::RasterCullMode::None);
    auto mirrored = defaults();
    engineRenderState(gls::MirrorView, 0, 0.0f, mirrored);
    CHECK(mirrored.rasterState.cullMode == nvrhi::RasterCullMode::Back);

    // the alpha mask and DST_COLOR / ZERO of a subview surface in the prepass
    auto subview = defaults();
    engineRenderState(
      gls::SrcBlendDstColor | gls::DstBlendZero | gls::AlphaMask, 0, 0.0f, subview);
    CHECK(subview.blendState.targets[0].srcBlend == nvrhi::BlendFactor::DstColor);
    CHECK(
      subview.blendState.targets[0].colorWriteMask
      == (nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue));

    // DEPTHMASK with ALWAYS turns the depth test off
    auto always = defaults();
    engineRenderState(gls::DepthMask | gls::DepthFuncAlways, 0, 0.0f, always);
    CHECK_FALSE(always.depthStencilState.depthTestEnable);
    CHECK_FALSE(always.depthStencilState.stencilEnable);
  }

  SECTION("engineRowPitch: the copied GetRowPitch")
  {
    namespace ei = engine_image;
    CHECK(engineRowPitch(ei::FormatDxt1, 5) == 16);
    CHECK(engineRowPitch(ei::FormatDxt1, 1) == 8);
    CHECK(engineRowPitch(ei::FormatDxt5, 1) == 16);
    CHECK(engineRowPitch(ei::FormatDxt5, 16) == 64);
    CHECK(engineRowPitch(ei::FormatBc6h, 4) == 16);
    CHECK(engineRowPitch(ei::FormatRgba8, 10) == 40);
    CHECK(engineRowPitch(ei::FormatRgb565, 10) == 20);
    CHECK(engineRowPitch(ei::FormatRgba16f, 3) == 24);
    // T0.14: a format the copy does not know, FMT_NONE and an empty row have no pitch
    CHECK_FALSE(engineRowPitch(-1, 8));
    CHECK_FALSE(engineRowPitch(ei::FormatNone, 8));
    CHECK_FALSE(engineRowPitch(ei::FormatRgba16s + 1, 8));
    CHECK_FALSE(engineRowPitch(1000, 8));
    CHECK_FALSE(engineRowPitch(ei::FormatRgba8, 0));
    CHECK(engineRowPitch(ei::FormatRgba16s, 2) == 16);
    // the uncompressed formats agree with the mirrored BitsForFormat (LP1c.1)
    for (const auto format :
         {ei::FormatRgba8,
          ei::FormatAlpha,
          ei::FormatL8a8,
          ei::FormatLum8,
          ei::FormatInt8,
          ei::FormatRgb565,
          ei::FormatRg16f,
          ei::FormatRgba16f,
          ei::FormatRgba32f,
          ei::FormatR32f,
          ei::FormatR11g11b10f})
    {
      CAPTURE(format);
      CHECK(engineRowPitch(format, 8) == 8 * (engineBitsForFormat(format) / 8));
    }
  }
}

TEST_CASE("EngineTables: the G-buffer layout and the named programs (LP6.5a)")
{
  using L = EngineBindingLayout;

  SECTION("GBUFFER is DEFAULT's three sets, its uniforms set 3's 256 bytes")
  {
    // C69 (RenderProgs.cpp:429): { uniforms, defaultLayout, samplerOneBindingLayout }
    for (const auto pushed : {false, true})
    {
      CAPTURE(pushed);
      const auto gbuffer = engineBindingLayoutDescs(L::Gbuffer, pushed);
      const auto texture = engineBindingLayoutDescs(L::Texture, pushed);
      REQUIRE(gbuffer);
      REQUIRE(texture);
      REQUIRE(gbuffer->size() == 3u);
      REQUIRE((*gbuffer)[0].bindings.size() == 1u);
      CHECK(
        (*gbuffer)[0].bindings[0].type
        == (pushed ? nvrhi::ResourceType::PushConstants : nvrhi::ResourceType::VolatileConstantBuffer));
      CHECK((*gbuffer)[0].bindings[0].size == (pushed ? 256u : 0u));
      CHECK((*gbuffer)[1].bindings[0].type == (*texture)[1].bindings[0].type);
      CHECK((*gbuffer)[2].bindings[0].type == (*texture)[2].bindings[0].type);
    }
    CHECK(engineBindingSetShape(L::Gbuffer) == engineBindingSetShape(L::Default));
    const auto block = engineLayoutConstantBlock(L::Gbuffer);
    REQUIRE(block);
    CHECK(block->name == "renderParmSet3_t");
    CHECK(block->size == 256u);
    CHECK(block->members.front().name == "rpLocalViewOrigin");
    CHECK(block->members.back().name == "rpPSXDistortions");
    const auto* row = engineBuiltin(EngineBuiltinSmallGeometryBuffer);
    REQUIRE(row);
    CHECK(row->bindingLayout == L::Gbuffer);
    CHECK(
      shaderPermutationKey(engineBuiltinDefines(*row, true))
      == "USE_GPU_SKINNING=0 USE_NORMAL_FMT_RGB8=0 USE_PUSH_CONSTANTS=1");
  }

  SECTION("the table: three compute rows past the material programs")
  {
    REQUIRE(EngineNamedPrograms.size() == 3u);
    CHECK(EngineNamedProgramBase > EngineMaterialProgramBase);
    CHECK(
      EngineNamedProgramBase
      >= EngineMaterialProgramBase + int(EngineMaterialPrograms.size()));
    const auto names = std::vector<std::string_view>{
      "builtin/SSAO/ssao_deinterleave",
      "builtin/SSAO/ssao_compute",
      "builtin/SSAO/ssao_blur"};
    // FindShader's defines in their order (SsaoPass.cpp:74, 98-102, 127-130), no
    // USE_PUSH_CONSTANTS (CFG:126-128)
    const auto keys = std::vector<std::string>{
      "LINEAR_DEPTH=0",
      "OCT_ENCODED_NORMALS=0 DIRECTIONAL_OCCLUSION=0",
      "DIRECTIONAL_OCCLUSION=0"};
    const auto layouts = std::vector<L>{L::SsaoDeinterleave, L::SsaoCompute, L::SsaoBlur};
    const auto indices = std::vector<int>{
      EngineNamedSsaoDeinterleave, EngineNamedSsaoCompute, EngineNamedSsaoBlur};
    // the items of each set: b1, the SRVs, the UAV, the blur's sampler
    const auto items = std::vector<std::size_t>{3, 4, 5};
    for (std::size_t i = 0; i < EngineNamedPrograms.size(); ++i)
    {
      CAPTURE(i);
      const auto& row = EngineNamedPrograms[i].row;
      CHECK(row.index == EngineNamedProgramBase + int(i));
      CHECK(row.index == indices[i]);
      CHECK(std::string_view{row.name} == names[i]);
      CHECK(std::string_view{row.nameOutSuffix}.empty());
      CHECK(row.stages == EngineStageCompute);
      CHECK(row.vertexLayout == EngineVertexLayout::Unknown);
      CHECK(row.bindingLayout == layouts[i]);
      CHECK_FALSE(row.requireGPUSkinningSupport);
      // a define with a value is that value whatever the push-constant rule
      CHECK(shaderPermutationKey(engineBuiltinDefines(row, true)) == keys[i]);
      CHECK(shaderPermutationKey(engineBuiltinDefines(row, false)) == keys[i]);
      CHECK(engineProgram(row.index) == &row);
      CHECK(engineNamedProgram(row.index) == &EngineNamedPrograms[i]);
      CHECK(engineParmSetOfLayout(row.bindingLayout) == -1);
      CHECK_FALSE(enginePushConstantsEnabled(
        row.bindingLayout, enginePushConstantLimit(256), 0x10DE));
      CHECK_FALSE(engineBindingSetShape(row.bindingLayout));

      // the expected layout D9 compares the blob with
      const auto expected = engineExpectedLayout(row, false);
      REQUIRE(expected);
      REQUIRE(expected->sets.size() == 1u);
      CHECK(expected->sets[0].visibility == nvrhi::ShaderType::Compute);
      REQUIRE(expected->sets[0].bindings.size() == items[i]);
      CHECK(
        expected->sets[0].bindings[0].type
        == nvrhi::ResourceType::VolatileConstantBuffer);
      CHECK(expected->sets[0].bindings[0].slot == 1u);
      CHECK(engineVulkanBinding(expected->sets[0].bindings[0]) == 257u);
      CHECK_FALSE(expected->pushConstants);
      CHECK(expected->vertexInputs.empty());
      REQUIRE(expected->constants);
      CHECK(*expected->constants == engineSsaoBlock());
      auto uavs = 0;
      auto srvs = 0;
      auto samplers = 0;
      for (const auto& item : expected->sets[0].bindings)
      {
        uavs += item.type == nvrhi::ResourceType::Texture_UAV ? 1 : 0;
        srvs += item.type == nvrhi::ResourceType::Texture_SRV ? 1 : 0;
        samplers += item.type == nvrhi::ResourceType::Sampler ? 1 : 0;
      }
      CHECK(uavs == 1);
      CHECK(srvs == int(i == 0 ? 1 : 2));
      CHECK(samplers == int(i == 2 ? 1 : 0));
    }
    CHECK_FALSE(engineNamedProgram(EngineNamedProgramBase + 3));
    CHECK_FALSE(engineNamedProgram(EngineNamedProgramBase - 1));
    CHECK_FALSE(engineProgram(EngineNamedProgramBase + 3));
  }

  SECTION("SsaoConstants: 272 bytes, SsaoPass_cb.h's members")
  {
    const auto block = engineSsaoBlock();
    CHECK(block.name == "SsaoConstants");
    CHECK(block.size == 272u);
    REQUIRE(block.members.size() == 16u);
    CHECK(block.members[2] == EngineConstantMember{"pixelOffset", 16});
    CHECK(block.members[4] == EngineConstantMember{"matClipToView", 32});
    CHECK(block.members[9] == EngineConstantMember{"quantizedViewportOrigin", 240});
    CHECK(block.members[15] == EngineConstantMember{"powerExponent", 268});
    for (const auto layout : {L::SsaoDeinterleave, L::SsaoCompute, L::SsaoBlur})
    {
      CHECK(engineLayoutConstantBlock(layout) == block);
    }
  }
}

} // namespace tb::lit
