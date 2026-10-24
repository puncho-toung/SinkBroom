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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderProgs.h, RenderProgs.cpp,
// Passes/CommonPasses.h, Passes/CommonPasses.cpp, Passes/TonemapPass.cpp,
// Passes/TonemapPass_cb.h, NVRHI/Image_NVRHI.cpp, NVRHI/RenderBackend_NVRHI.cpp,
// sys/DeviceManager.cpp, sys/DeviceManager_VK.cpp and neo/shaders/blit.cb.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023 Stephen Saunders
// The NVIDIA Donut code the engine adapted there (TonemapPass, CommonPasses) is under
// the MIT licence. That code is also subject to the additional terms that follow the GNU
// General Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/EngineTables.h"

#include "lit/EngineGeometry.h"
#include "lit/ImageCache.h"

#include <algorithm>
#include <cstddef>

namespace tb::lit
{
namespace
{

using L = EngineBindingLayout;

// RenderProgs.h:73-217, in enum order
constexpr auto RenderParmNames =
  std::array<std::string_view, std::size_t(RenderParm::Total)>{
    "RENDERPARM_SCREENCORRECTIONFACTOR",
    "RENDERPARM_WINDOWCOORD",
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
    "RENDERPARM_COLOR",
    "RENDERPARM_VIEWORIGIN",
    "RENDERPARM_GLOBALEYEPOS",
    "RENDERPARM_MVPMATRIX_X",
    "RENDERPARM_MVPMATRIX_Y",
    "RENDERPARM_MVPMATRIX_Z",
    "RENDERPARM_MVPMATRIX_W",
    "RENDERPARM_MODELMATRIX_X",
    "RENDERPARM_MODELMATRIX_Y",
    "RENDERPARM_MODELMATRIX_Z",
    "RENDERPARM_MODELMATRIX_W",
    "RENDERPARM_PROJMATRIX_X",
    "RENDERPARM_PROJMATRIX_Y",
    "RENDERPARM_PROJMATRIX_Z",
    "RENDERPARM_PROJMATRIX_W",
    "RENDERPARM_MODELVIEWMATRIX_X",
    "RENDERPARM_MODELVIEWMATRIX_Y",
    "RENDERPARM_MODELVIEWMATRIX_Z",
    "RENDERPARM_MODELVIEWMATRIX_W",
    "RENDERPARM_TEXTUREMATRIX_S",
    "RENDERPARM_TEXTUREMATRIX_T",
    "RENDERPARM_TEXGEN_0_S",
    "RENDERPARM_TEXGEN_0_T",
    "RENDERPARM_TEXGEN_0_Q",
    "RENDERPARM_TEXGEN_0_ENABLED",
    "RENDERPARM_TEXGEN_1_S",
    "RENDERPARM_TEXGEN_1_T",
    "RENDERPARM_TEXGEN_1_Q",
    "RENDERPARM_TEXGEN_1_ENABLED",
    "RENDERPARM_WOBBLESKY_X",
    "RENDERPARM_WOBBLESKY_Y",
    "RENDERPARM_WOBBLESKY_Z",
    "RENDERPARM_OVERBRIGHT",
    "RENDERPARM_ENABLE_SKINNING",
    "RENDERPARM_ALPHA_TEST",
    "RENDERPARM_AMBIENT_COLOR",
    "RENDERPARM_GLOBALLIGHTORIGIN",
    "RENDERPARM_JITTERTEXSCALE",
    "RENDERPARM_JITTERTEXOFFSET",
    "RENDERPARM_PSX_DISTORTIONS",
    "RENDERPARM_CASCADEDISTANCES",
    "RENDERPARM_SHADOW_MATRIX_0_X",
    "RENDERPARM_SHADOW_MATRIX_0_Y",
    "RENDERPARM_SHADOW_MATRIX_0_Z",
    "RENDERPARM_SHADOW_MATRIX_0_W",
    "RENDERPARM_SHADOW_MATRIX_1_X",
    "RENDERPARM_SHADOW_MATRIX_1_Y",
    "RENDERPARM_SHADOW_MATRIX_1_Z",
    "RENDERPARM_SHADOW_MATRIX_1_W",
    "RENDERPARM_SHADOW_MATRIX_2_X",
    "RENDERPARM_SHADOW_MATRIX_2_Y",
    "RENDERPARM_SHADOW_MATRIX_2_Z",
    "RENDERPARM_SHADOW_MATRIX_2_W",
    "RENDERPARM_SHADOW_MATRIX_3_X",
    "RENDERPARM_SHADOW_MATRIX_3_Y",
    "RENDERPARM_SHADOW_MATRIX_3_Z",
    "RENDERPARM_SHADOW_MATRIX_3_W",
    "RENDERPARM_SHADOW_MATRIX_4_X",
    "RENDERPARM_SHADOW_MATRIX_4_Y",
    "RENDERPARM_SHADOW_MATRIX_4_Z",
    "RENDERPARM_SHADOW_MATRIX_4_W",
    "RENDERPARM_SHADOW_MATRIX_5_X",
    "RENDERPARM_SHADOW_MATRIX_5_Y",
    "RENDERPARM_SHADOW_MATRIX_5_Z",
    "RENDERPARM_SHADOW_MATRIX_5_W",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_0",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_1",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_2",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_3",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_4",
    "RENDERPARM_SHADOW_ATLAS_OFFSET_5",
    "RENDERPARM_SH_IRRADIANCE_0",
    "RENDERPARM_SH_IRRADIANCE_1",
    "RENDERPARM_SH_IRRADIANCE_2",
    "RENDERPARM_SH_IRRADIANCE_3",
    "RENDERPARM_SH_IRRADIANCE_4",
    "RENDERPARM_SH_IRRADIANCE_5",
    "RENDERPARM_SH_IRRADIANCE_6",
    "RENDERPARM_SH_IRRADIANCE_7",
    "RENDERPARM_SH_IRRADIANCE_8",
    "RENDERPARM_USER0",
    "RENDERPARM_USER1",
    "RENDERPARM_USER2",
    "RENDERPARM_USER3",
    "RENDERPARM_USER4",
    "RENDERPARM_USER5",
    "RENDERPARM_USER6",
    "RENDERPARM_USER7",
  };

// the passes' own constant structs (RenderProgs.cpp:397-401): MipmmapGenConstants
// (MipMapGenPass_cb.h:25-30), TemporalAntiAliasingConstants
// (TemporalAntiAliasingPass_cb.h:25-47), ToneMappingConstants (TonemapPass_cb.h:25-47)
constexpr auto MipmapGenConstantsSize = std::uint32_t{16};
constexpr auto TemporalAntiAliasingConstantsSize = std::uint32_t{132};
constexpr auto ToneMappingConstantsSize = std::uint32_t{80};
// BlitConstants (CommonPasses.h:63-72)
constexpr auto BlitConstantsSize = std::uint32_t{36};

// the HLSL structs of the sets (renderParmSet<N>.inc.hlsl)
constexpr auto ParmSetStructNames = std::array<std::string_view, EngineParmSetCount>{
  "renderParmSet0_t",
  "renderParmSet1_t",
  "renderParmSet2_t",
  "renderParmSet3_t",
  "renderParmSet4_t",
  "renderParmSet5_t",
  "renderParmSet6_t",
  "renderParmSet7_t",
  "renderParmSet8_t",
  "renderParmSet9_t",
  "renderParmSet10_t",
  "renderParmSet11_t",
  "renderParmSet12_t",
  "renderParmSet13_t",
  "renderParmSet14_t",
};

nvrhi::BindingLayoutDesc layoutDesc(
  const nvrhi::ShaderType visibility, std::vector<nvrhi::BindingLayoutItem> items)
{
  auto desc = nvrhi::BindingLayoutDesc{}.setVisibility(visibility);
  for (const auto& item : items)
  {
    desc.addItem(item);
  }
  return desc;
}

// RenderProgs.cpp:79-108 uniformsLayout's item (cbStatic is always false, :224-390)
nvrhi::BindingLayoutItem uniformsItem(const bool pushConstants, const std::uint32_t size)
{
  return pushConstants ? nvrhi::BindingLayoutItem::PushConstants(0, size)
                       : nvrhi::BindingLayoutItem::VolatileConstantBuffer(0);
}

std::uint32_t parmSetBytes(const int set)
{
  return set >= 0 && set < EngineParmSetCount
           ? std::uint32_t(EngineParmSetSizes[std::size_t(set)]) * 16u
           : 0u;
}

} // namespace

std::string_view renderParmName(const RenderParm parm)
{
  const auto index = std::size_t(parm);
  return index < RenderParmNames.size() ? RenderParmNames[index] : std::string_view{};
}

int engineParmSetOfLayout(const EngineBindingLayout layout)
{
  // RenderProgs.h:248-318, the rp*Set<N>LayoutTypes lists
  switch (layout)
  {
  case L::Default:
  case L::DefaultSkinned:
  case L::ConstantBufferOnly:
  case L::ConstantBufferOnlySkinned:
  case L::BinkVideo:
    return 0;
  case L::PostProcess:
  case L::TaaMotionVectors:
    return 1;
  case L::PostProcessFinal:
  case L::PostProcessCrt:
  case L::SmaaEdgeDetection:
  case L::SmaaWeightCalc:
    return 2;
  case L::Gbuffer:
  case L::GbufferSkinned:
    return 3;
  case L::Texture:
  case L::TextureSkinned:
    return 4;
  case L::Legacy:
  case L::NormalCube:
  case L::NormalCubeSkinned:
    return 5;
  case L::Debug:
  case L::DebugSkinned:
    return 6;
  case L::Fog:
  case L::FogSkinned:
  case L::Blendlight:
  case L::BlendlightSkinned:
    return 7;
  case L::PostProcessIngame:
    return 8;
  case L::AmbientLightingIbl:
  case L::AmbientLightingIblSkinned:
    return 9;
  case L::DrawInteraction:
  case L::DrawInteractionSkinned:
    return 10;
  case L::DrawInteractionSm:
  case L::DrawInteractionSmSkinned:
    return 11;
  case L::Texgen:
    return 12;
  case L::PostProcessFinal2:
  case L::DrawAo:
  case L::DrawAo1:
    return 13;
  case L::OctahedronCube:
  case L::OctahedronCubeSkinned:
    return 14;
  case L::Blit:
  case L::Mipmapgen:
  case L::TaaResolve:
  case L::Tonemap:
  case L::Histogram:
  case L::Exposure:
  case L::Num:
    return -1;
  }
  return -1;
}

std::span<const EngineParmSetMember> engineParmSetMembers(const int set)
{
  switch (set)
  {
  case 0:
    return EngineParmSet0;
  case 4:
    return EngineParmSet4;
  case 5:
    return EngineParmSet5;
  case 7:
    return EngineParmSet7;
  case 8:
    return EngineParmSet8;
  case 9:
    return EngineParmSet9;
  case 10:
    return EngineParmSet10;
  case 12:
    return EngineParmSet12;
  default:
    return {};
  }
}

EngineConstantBlock engineParmSetBlock(const int set)
{
  auto block = EngineConstantBlock{};
  block.size = parmSetBytes(set);
  const auto members = engineParmSetMembers(set);
  for (std::size_t i = 0; i < members.size(); ++i)
  {
    block.members.push_back({members[i].shaderName, std::uint32_t(i * 16)});
  }
  return block;
}

EngineConstantBlock engineToneMappingBlock()
{
  // TonemapPass_cb.h:25-47 and shaders/builtin/post/tonemapping_cb.h
  return EngineConstantBlock{
    "ToneMappingConstants",
    {
      {"viewOrigin", 0},
      {"viewSize", 8},
      {"logLuminanceScale", 16},
      {"logLuminanceBias", 20},
      {"histogramLowPercentile", 24},
      {"histogramHighPercentile", 28},
      {"eyeAdaptationSpeedUp", 32},
      {"eyeAdaptationSpeedDown", 36},
      {"minAdaptedLuminance", 40},
      {"maxAdaptedLuminance", 44},
      {"frameTime", 48},
      {"exposureScale", 52},
      {"whitePointInvSquared", 56},
      {"sourceSlice", 60},
      {"colorLUTTextureSize", 64},
      {"colorLUTTextureSizeInv", 72},
    },
    ToneMappingConstantsSize};
}

EngineConstantBlock engineBlitBlock()
{
  // CommonPasses.h:63-72 and shaders/blit.cb.h
  return EngineConstantBlock{
    "BlitConstants",
    {
      {"sourceOrigin", 0},
      {"sourceSize", 8},
      {"targetOrigin", 16},
      {"targetSize", 24},
      {"sharpenFactor", 32},
    },
    BlitConstantsSize};
}

std::optional<EngineConstantBlock> engineLayoutConstantBlock(
  const EngineBindingLayout layout)
{
  switch (layout)
  {
  case L::Tonemap:
  case L::Histogram:
  case L::Exposure:
    return engineToneMappingBlock();
  case L::Blit:
    return engineBlitBlock();
  default:
    break;
  }
  const auto set = engineParmSetOfLayout(layout);
  if (set < 0)
  {
    return std::nullopt;
  }
  auto block = engineParmSetBlock(set);
  if (block.members.empty())
  {
    return std::nullopt;
  }
  block.name = ParmSetStructNames[std::size_t(set)];
  return block;
}

std::uint32_t enginePushConstantLimit(
  const std::uint32_t maxPushConstantsSize, const std::uint32_t nvrhiLimit)
{
  return std::min(maxPushConstantsSize, nvrhiLimit);
}

bool enginePushConstantsEnabled(
  const EngineBindingLayout layout,
  const std::uint32_t pushConstantLimit,
  const std::uint32_t vendorID)
{
  // RenderProgs.cpp:393
  const auto set = engineParmSetOfLayout(layout);
  auto enabled = set >= 0 && parmSetBytes(set) <= pushConstantLimit;

  // :396-401, the passes' own structs
  switch (layout)
  {
  case L::Mipmapgen:
    enabled = MipmapGenConstantsSize <= pushConstantLimit;
    break;
  case L::TaaResolve:
    enabled = TemporalAntiAliasingConstantsSize <= pushConstantLimit;
    break;
  case L::Tonemap:
  case L::Histogram:
  case L::Exposure:
    enabled = ToneMappingConstantsSize <= pushConstantLimit;
    break;
  default:
    break;
  }

  // :403-413, Vulkan on AMD
  if (vendorID == EngineVendorAmd)
  {
    switch (layout)
    {
    case L::Gbuffer:
    case L::GbufferSkinned:
    case L::Texture:
    case L::TextureSkinned:
      enabled = false;
      break;
    default:
      break;
    }
  }
  return enabled;
}

std::vector<ShaderDefine> engineBuiltinDefines(
  const EngineBuiltin& builtin, const bool pushConstantsEnabled)
{
  auto defines = std::vector<ShaderDefine>{};
  for (std::size_t i = 0; i < builtin.defineCount && i < builtin.defines.size(); ++i)
  {
    const auto& define = builtin.defines[i];
    defines.push_back(ShaderDefine{
      define.name,
      define.value ? std::string{define.value}
                   : std::string{pushConstantsEnabled ? "1" : "0"}});
  }
  return defines;
}

const EngineBuiltin* engineBuiltin(const int index)
{
  return index >= 0 && std::size_t(index) < EngineBuiltins.size()
           ? &EngineBuiltins[std::size_t(index)]
           : nullptr;
}

std::optional<std::vector<nvrhi::BindingLayoutDesc>> engineBindingLayoutDescs(
  const EngineBindingLayout layout, const bool pushConstants)
{
  using Item = nvrhi::BindingLayoutItem;
  using S = nvrhi::ShaderType;
  // uniformsLayout( type, skinning ) (RenderProgs.cpp:79-108): the uniforms item of the
  // layout's set, with the joint buffer at t11 for a skinned type
  const auto uniforms = [&](const bool skinned) {
    auto items = std::vector<Item>{
      uniformsItem(pushConstants, parmSetBytes(engineParmSetOfLayout(layout)))};
    if (skinned)
    {
      items.push_back(Item::StructuredBuffer_SRV(11));
    }
    return layoutDesc(S::All, std::move(items));
  };
  switch (layout)
  {
  case L::ConstantBufferOnly:
    // RenderProgs.cpp:443
    return std::vector{
      layoutDesc(S::All, {uniformsItem(pushConstants, parmSetBytes(0))})};
  case L::Default:
  case L::DefaultSkinned:
  case L::Texture:
  case L::TextureSkinned:
  case L::Texgen:
  case L::Legacy:
    // :415-436: uniforms, the image (defaultLayout), its own sampler
    // (samplerOneBindingLayout); LP5.0: DEFAULT and its skinned form :426-427, TEXTURE's
    // skinned form :433, TEXGEN :434, LEGACY :436
    return std::vector{
      uniforms(layout == L::DefaultSkinned || layout == L::TextureSkinned),
      layoutDesc(S::Pixel, {Item::Texture_SRV(0)}),
      layoutDesc(S::Pixel, {Item::Sampler(0)}),
    };
  case L::DrawInteraction:
  case L::DrawInteractionSkinned:
    // :446-452, 470-474, 514-524: uniforms; normal, specular, base colour; light
    // falloff and projection; the material and lighting samplers. LP5.0: the skinned
    // form :525-528 (interactionAmbient_skinned, row 22)
    return std::vector{
      uniforms(layout == L::DrawInteractionSkinned),
      layoutDesc(
        S::Pixel, {Item::Texture_SRV(0), Item::Texture_SRV(1), Item::Texture_SRV(2)}),
      layoutDesc(S::Pixel, {Item::Texture_SRV(3), Item::Texture_SRV(4)}),
      layoutDesc(S::Pixel, {Item::Sampler(0), Item::Sampler(1)}),
    };
  case L::Fog:
    // LP5.0 (C49): :556-570: uniforms; t0 t1 (fogBindingLayout); s0 s1
    // (samplerTwoBindingLayout)
    return std::vector{
      uniforms(false),
      layoutDesc(S::Pixel, {Item::Texture_SRV(0), Item::Texture_SRV(1)}),
      layoutDesc(S::Pixel, {Item::Sampler(0), Item::Sampler(1)}),
    };
  case L::Blendlight:
    // LP5.0 (C49): :572-586: uniforms; t0 t1 (blendLightBindingLayout); s0
    return std::vector{
      uniforms(false),
      layoutDesc(S::Pixel, {Item::Texture_SRV(0), Item::Texture_SRV(1)}),
      layoutDesc(S::Pixel, {Item::Sampler(0)}),
    };
  case L::NormalCube:
    // LP5.0 (C48): :651-665: uniforms; the cube at t0 and the normal map at t1
    // (normalCubeBindingLayout); s0
    return std::vector{
      uniforms(false),
      layoutDesc(S::Pixel, {Item::Texture_SRV(0), Item::Texture_SRV(1)}),
      layoutDesc(S::Pixel, {Item::Sampler(0)}),
    };
  case L::PostProcessIngame:
    // LP5.0 (C47): :588-602: one set of the uniforms and t0 t1 t2 (_currentRender, the
    // normal map, the mask), visibility All; s0
    return std::vector{
      layoutDesc(
        S::All,
        {uniformsItem(pushConstants, parmSetBytes(8)),
         Item::Texture_SRV(0),
         Item::Texture_SRV(1),
         Item::Texture_SRV(2)}),
      layoutDesc(S::Pixel, {Item::Sampler(0)}),
    };
  case L::AmbientLightingIbl:
  case L::AmbientLightingIblSkinned: {
    // LP4.2a (C35, SC329): RenderProgs.cpp:446-483: uniforms (with the joint buffer at
    // t11 for the skinned form, uniformsLayout's skinning item :91-98, tabled and never
    // requested); normal, specular, base colour (defaultMaterialLayout :446-452); the
    // BRDF LUT, the SSAO image, the irradiance image, three radiance cubes and three
    // depth cubes (ambientIblLayout :454-468); the material and the linear clamp
    // samplers (samplerTwoBindingLayout :470-474)
    return std::vector{
      uniforms(layout == L::AmbientLightingIblSkinned),
      layoutDesc(
        S::Pixel, {Item::Texture_SRV(0), Item::Texture_SRV(1), Item::Texture_SRV(2)}),
      layoutDesc(
        S::Pixel,
        {Item::Texture_SRV(3),
         Item::Texture_SRV(4),
         Item::Texture_SRV(7),
         Item::Texture_SRV(8),
         Item::Texture_SRV(9),
         Item::Texture_SRV(10),
         Item::Texture_SRV(12),
         Item::Texture_SRV(13),
         Item::Texture_SRV(14)}),
      layoutDesc(S::Pixel, {Item::Sampler(0), Item::Sampler(1)}),
    };
  }
  case L::Histogram:
    // TonemapPass.cpp:119-142
    return std::vector{layoutDesc(
      S::Compute,
      {uniformsItem(pushConstants, ToneMappingConstantsSize),
       Item::Texture_SRV(0),
       Item::TypedBuffer_UAV(0)})};
  case L::Exposure:
    // :150-172
    return std::vector{layoutDesc(
      S::Compute,
      {uniformsItem(pushConstants, ToneMappingConstantsSize),
       Item::TypedBuffer_SRV(0),
       Item::TypedBuffer_UAV(0)})};
  case L::Tonemap:
    // :202-227: the source, the exposure, the colour LUT, its sampler
    return std::vector{layoutDesc(
      S::Pixel,
      {uniformsItem(pushConstants, ToneMappingConstantsSize),
       Item::Texture_SRV(0),
       Item::TypedBuffer_SRV(1),
       Item::Texture_SRV(2),
       Item::Sampler(0)})};
  case L::Blit:
    // CommonPasses.cpp:166-177: always pushed
    return std::vector{layoutDesc(
      S::All,
      {Item::PushConstants(0, BlitConstantsSize),
       Item::Texture_SRV(0),
       Item::Sampler(0)})};
  default:
    return std::nullopt;
  }
}

nvrhi::SamplerDesc engineCommonSamplerDesc(const EngineCommonSampler sampler)
{
  const auto samplers = engineCommonSamplers();
  using C = EngineCommonSampler;
  switch (sampler)
  {
  case C::PointClamp:
    return samplers.pointClamp;
  case C::PointWrap:
    return samplers.pointWrap;
  case C::LinearClamp:
    return samplers.linearClamp;
  case C::LinearBorder:
    return samplers.linearBorder;
  case C::LinearClampCompare:
    return samplers.linearClampCompare;
  case C::LinearWrap:
    return samplers.linearWrap;
  case C::AnisotropicWrap:
    return samplers.anisotropicWrap;
  case C::AnisotropicClampEdge:
    return samplers.anisotropicClampEdge;
  }
  return samplers.pointClamp;
}

std::optional<std::vector<std::vector<EngineBindingSetItem>>> engineBindingSetShape(
  const EngineBindingLayout layout)
{
  // GetCurrentBindingLayout (RenderBackend_NVRHI.cpp:566-1990), R_UsePixelatedLook false
  using Item = EngineBindingSetItem;
  using Source = EngineBindingSetItem::Source;
  using C = EngineCommonSampler;
  const auto uniforms = Item{Source::Uniforms, 0, 0, C::PointClamp};
  const auto joints = Item{Source::JointBuffer, 11, 0, C::PointClamp};
  const auto unit = [](const std::uint32_t slot) {
    return Item{Source::ImageUnit, slot, slot, C::PointClamp};
  };
  const auto common = [](const std::uint32_t slot, const C sampler) {
    return Item{Source::CommonSampler, slot, 0, sampler};
  };
  using Sets = std::vector<std::vector<Item>>;
  switch (layout)
  {
  case L::ConstantBufferOnly:
    // :666-679
    return Sets{{uniforms}};
  case L::Default:
  case L::Texture:
  case L::Texgen:
  case L::Legacy:
    // :586-623: unit 0 and its own sampler
    return Sets{{uniforms}, {unit(0)}, {Item{Source::ImageSampler, 0, 0, C::PointClamp}}};
  case L::DefaultSkinned:
  case L::TextureSkinned:
    // :624-665
    return Sets{
      {uniforms, joints}, {unit(0)}, {Item{Source::ImageSampler, 0, 0, C::PointClamp}}};
  case L::DrawInteraction:
  case L::DrawInteractionSkinned: {
    // :916-996 and :997-1081
    auto first = std::vector<Item>{uniforms};
    if (layout == L::DrawInteractionSkinned)
    {
      first.push_back(joints);
    }
    return Sets{
      std::move(first),
      {unit(0), unit(1), unit(2)},
      {unit(3), unit(4)},
      {common(0, C::AnisotropicWrap), common(1, C::LinearBorder)}};
  }
  case L::AmbientLightingIbl:
  case L::AmbientLightingIblSkinned: {
    // :698-788 and :789-883
    auto first = std::vector<Item>{uniforms};
    if (layout == L::AmbientLightingIblSkinned)
    {
      first.push_back(joints);
    }
    return Sets{
      std::move(first),
      {unit(0), unit(1), unit(2)},
      {unit(3),
       unit(4),
       unit(7),
       unit(8),
       unit(9),
       unit(10),
       unit(12),
       unit(13),
       unit(14)},
      {common(0, C::AnisotropicWrap), common(1, C::LinearClamp)}};
  }
  case L::Fog:
    // :1286-1329: LinearClamp at both samplers
    return Sets{
      {uniforms},
      {unit(0), unit(1)},
      {common(0, C::LinearClamp), common(1, C::LinearClamp)}};
  case L::Blendlight:
    // :1378-1418
    return Sets{{uniforms}, {unit(0), unit(1)}, {common(0, C::LinearBorder)}};
  case L::PostProcessIngame:
    // :1464-1512 (PointClamp only under R_UsePixelatedLook)
    return Sets{{uniforms, unit(0), unit(1), unit(2)}, {common(0, C::LinearClamp)}};
  case L::NormalCube:
    // :1677-1734 (PointWrap only under R_UsePixelatedLook): m_LinearWrapSampler, made
    // after the leaked Comparison reduction (CommonPasses.cpp:87-91, D68)
    return Sets{{uniforms}, {unit(0), unit(1)}, {common(0, C::LinearWrap)}};
  default:
    return std::nullopt;
  }
}

std::uint32_t engineVulkanBinding(const nvrhi::BindingLayoutItem& item)
{
  using T = nvrhi::ResourceType;
  const auto& offsets = EngineVulkanBindingOffsets;
  switch (T(item.type))
  {
  case T::Texture_SRV:
  case T::TypedBuffer_SRV:
  case T::StructuredBuffer_SRV:
  case T::RawBuffer_SRV:
  case T::RayTracingAccelStruct:
    return item.slot + offsets.shaderResource;
  case T::Sampler:
    return item.slot + offsets.sampler;
  case T::ConstantBuffer:
  case T::VolatileConstantBuffer:
    return item.slot + offsets.constantBuffer;
  case T::Texture_UAV:
  case T::TypedBuffer_UAV:
  case T::StructuredBuffer_UAV:
  case T::RawBuffer_UAV:
    return item.slot + offsets.unorderedAccess;
  default:
    return item.slot;
  }
}

std::vector<nvrhi::VertexAttributeDesc> engineDrawVertAttributes()
{
  // RenderProgs.cpp:131-165
  constexpr auto stride = std::uint32_t(sizeof(EngineDrawVert));
  const auto attribute =
    [&](const char* name, const nvrhi::Format format, const std::size_t offset) {
      return nvrhi::VertexAttributeDesc{}
        .setName(name)
        .setFormat(format)
        .setOffset(std::uint32_t(offset))
        .setElementStride(stride);
    };
  return {
    attribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(EngineDrawVert, xyz)),
    attribute("TEXCOORD", nvrhi::Format::RG16_FLOAT, offsetof(EngineDrawVert, st)),
    attribute("NORMAL", nvrhi::Format::RGBA8_UNORM, offsetof(EngineDrawVert, normal)),
    attribute("TANGENT", nvrhi::Format::RGBA8_UNORM, offsetof(EngineDrawVert, tangent)),
    attribute("COLOR", nvrhi::Format::RGBA8_UNORM, offsetof(EngineDrawVert, color))
      .setArraySize(2),
  };
}

std::span<const EngineVertexInput> engineDrawVertInputs()
{
  // the VS_IN of builtin/color, depth, texture_color and lighting/interaction
  static constexpr auto Inputs = std::array<EngineVertexInput, 6>{{
    {0, "POSITION", 4},
    {1, "TEXCOORD0", 2},
    {2, "NORMAL", 4},
    {3, "TANGENT", 4},
    {4, "COLOR0", 4},
    {5, "COLOR1", 4},
  }};
  return Inputs;
}

EngineCommonSamplers engineCommonSamplers()
{
  // CommonPasses.cpp:71-97: one desc, changed in place
  using A = nvrhi::SamplerAddressMode;
  auto samplers = EngineCommonSamplers{};
  auto desc = nvrhi::SamplerDesc{}.setAllFilters(false).setAllAddressModes(A::Clamp);
  samplers.pointClamp = desc;

  desc.setAllAddressModes(A::Wrap);
  samplers.pointWrap = desc;

  desc.setAllAddressModes(A::Clamp);
  desc.setAllFilters(true);
  samplers.linearClamp = desc;

  desc.setAllAddressModes(A::Border);
  desc.setBorderColor(nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
  samplers.linearBorder = desc;

  desc.setReductionType(nvrhi::SamplerReductionType::Comparison);
  samplers.linearClampCompare = desc;

  desc.setAllAddressModes(A::Wrap);
  samplers.linearWrap = desc;

  desc.setMaxAnisotropy(16);
  samplers.anisotropicWrap = desc;

  desc.setAllAddressModes(A::ClampToEdge);
  samplers.anisotropicClampEdge = desc;
  return samplers;
}

std::optional<nvrhi::SamplerDesc> engineImageSamplerDesc(
  const int format, const int filter, const int repeat, const int maxAnisotropy)
{
  // Image_NVRHI.cpp:173-243
  using A = nvrhi::SamplerAddressMode;
  namespace ei = engine_image;
  auto desc = nvrhi::SamplerDesc{}
                .setAllFilters(false)
                .setAllAddressModes(A::Clamp)
                .setMaxAnisotropy(1.0f);
  if (format == ei::FormatDepth || format == ei::FormatDepthStencil)
  {
    desc.setReductionType(nvrhi::SamplerReductionType::Comparison);
  }

  switch (filter)
  {
  case ei::FilterDefault:
    desc.setAllFilters(true).setMaxAnisotropy(float(maxAnisotropy));
    break;
  case ei::FilterLinear:
    desc.setAllFilters(true);
    break;
  case ei::FilterNearest:
    desc.setAllFilters(false);
    break;
  case ei::FilterNearestMipmap:
    desc.setMipFilter(true);
    break;
  default:
    return std::nullopt;
  }

  switch (repeat)
  {
  case ei::RepeatRepeat:
    desc.setAddressU(A::Repeat).setAddressV(A::Repeat).setAddressW(A::Repeat);
    break;
  case ei::RepeatClamp:
    desc.setAddressU(A::ClampToEdge)
      .setAddressV(A::ClampToEdge)
      .setAddressW(A::ClampToEdge);
    break;
  case ei::RepeatClampToZeroAlpha:
    desc.setBorderColor(nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f})
      .setAddressU(A::ClampToBorder)
      .setAddressV(A::ClampToBorder)
      .setAddressW(A::ClampToBorder);
    break;
  case ei::RepeatClampToZero:
    desc.setBorderColor(nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f})
      .setAddressU(A::ClampToBorder)
      .setAddressV(A::ClampToBorder)
      .setAddressW(A::ClampToBorder);
    break;
  default:
    return std::nullopt;
  }
  return desc;
}

std::optional<nvrhi::Format> engineTextureFormat(
  const int format, const bool d24s8Supported)
{
  // Image_NVRHI.cpp:349-459
  namespace ei = engine_image;
  using F = nvrhi::Format;
  switch (format)
  {
  case ei::FormatRgba8:
    return F::RGBA8_UNORM;
  case ei::FormatXrgb8:
    return F::X32G8_UINT;
  case ei::FormatRgb565:
    return F::B5G6R5_UNORM;
  case ei::FormatAlpha:
  case ei::FormatLum8:
  case ei::FormatInt8:
  case ei::FormatR8f:
    return F::R8_UNORM;
  case ei::FormatL8a8:
    return F::RG8_UNORM;
  case ei::FormatDxt1:
    return F::BC1_UNORM;
  case ei::FormatDxt5:
    return F::BC3_UNORM;
  case ei::FormatBc6h:
    return F::BC6H_UFLOAT;
  case ei::FormatDepth:
  case ei::FormatShadowArray:
    return F::D32;
  case ei::FormatDepthStencil:
    // SRS: D24S8 when the device has it, else D32S8 (:404-414)
    return d24s8Supported ? F::D24S8 : F::D32S8;
  case ei::FormatRg16f:
    return F::RG16_FLOAT;
  case ei::FormatRgba16f:
    return F::RGBA16_FLOAT;
  case ei::FormatRgba16s:
    return F::RGBA16_SNORM;
  case ei::FormatRgba32f:
    return F::RGBA32_FLOAT;
  case ei::FormatR32f:
    return F::R32_FLOAT;
  case ei::FormatX16:
  case ei::FormatY16X16:
    // no case sets a format: the default of :349 stands
    return F::RGBA8_UINT;
  case ei::FormatR11g11b10f:
    return F::R11G11B10_FLOAT;
  default:
    // idLib::Error (:457-458): BC7, ETC1, none
    return std::nullopt;
  }
}

nvrhi::ComponentMapping engineComponentMapping(const int format, const int colorFormat)
{
  // Image_NVRHI.cpp:513-554
  namespace ei = engine_image;
  using C = nvrhi::ComponentSwizzle;
  const auto mapping = [](const C r, const C g, const C b, const C a) {
    auto result = nvrhi::ComponentMapping{};
    result.r = r;
    result.g = g;
    result.b = b;
    result.a = a;
    return result;
  };
  if (colorFormat == ei::ColorGreenAlpha)
  {
    return mapping(C::One, C::One, C::One, C::Green);
  }
  switch (format)
  {
  case ei::FormatLum8:
    return mapping(C::Red, C::Red, C::Red, C::One);
  case ei::FormatL8a8:
    return mapping(C::Red, C::Red, C::Red, C::Green);
  case ei::FormatAlpha:
    return mapping(C::One, C::One, C::One, C::Red);
  case ei::FormatInt8:
    return mapping(C::Red, C::Red, C::Red, C::Red);
  case ei::FormatR11g11b10f:
    return mapping(C::Red, C::Green, C::Blue, C::One);
  default:
    return nvrhi::ComponentMapping{};
  }
}

} // namespace tb::lit
