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

#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"

#include <cctype>
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

  SECTION("renderparm sets")
  {
    // the mirrored sets have the lengths the engine's lists have
    CHECK(engineParmSetMembers(0).size() == std::size_t(EngineParmSetSizes[0]));
    CHECK(engineParmSetMembers(4).size() == std::size_t(EngineParmSetSizes[4]));
    CHECK(engineParmSetMembers(10).size() == std::size_t(EngineParmSetSizes[10]));
    CHECK(engineParmSetMembers(3).empty());

    CHECK(engineParmSetBlock(0).size == 128u);
    CHECK(engineParmSetBlock(4).size == 256u);
    CHECK(engineParmSetBlock(10).size == 400u);
    CHECK(engineParmSetBlock(11).size == 68u * 16u);

    for (const auto set : {0, 4, 10})
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
        if (c != '_')
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

    CHECK(
      engineParmSetBlock(10).members[3] == EngineConstantMember{"rpLocalViewOrigin", 48});
    CHECK(engineParmSetBlock(10).members[24] == EngineConstantMember{"rpUser2", 384});

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
    CHECK_FALSE(engineLayoutConstantBlock(EngineBindingLayout::Fog));
    CHECK_FALSE(engineLayoutConstantBlock(EngineBindingLayout::Mipmapgen));

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
    CHECK_FALSE(engineBindingLayoutDescs(L::Fog, false));

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

} // namespace tb::lit
