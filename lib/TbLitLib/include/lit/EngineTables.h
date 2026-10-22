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
// Passes/CommonPasses.cpp, Passes/TonemapPass.cpp, Passes/TonemapPass_cb.h,
// NVRHI/Image_NVRHI.cpp, RenderSystem_init.cpp, sys/DeviceManager.cpp,
// sys/DeviceManager_VK.cpp and shaders/renderParmSet*.inc.hlsl):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023 Stephen Saunders
// The NVIDIA Donut code the engine adapted there (TonemapPass, CommonPasses) is under
// the MIT licence. That code is also subject to the additional terms that follow the GNU
// General Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "lit/EngineBuiltins.h"
#include "lit/ShaderBlobs.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The engine's render-program tables that the preview's pipelines are built from (C3,
// C27), mirrored row for row from the engine at 285ea4d1 and listed in mirror.json
// (M-TABLE-*). The builtins rows themselves are lit/EngineBuiltins.h, the one table the
// parser's shim also reads (P2.24); DeriveOpts' mip rule is lit/ImageCache.h's
// engineMipLevels (LP1c.1). Everything here is constant data or a pure function: any
// thread.

namespace tb::lit
{

// ----- renderParm_t (RenderProgs.h:73-217, SC234) -----

enum class RenderParm : int
{
  ScreenCorrectionFactor,
  WindowCoord,
  DiffuseModifier,
  SpecularModifier,
  LocalLightOrigin,
  LocalViewOrigin,
  LightProjectionS,
  LightProjectionT,
  LightProjectionQ,
  LightFalloffS,
  BumpMatrixS,
  BumpMatrixT,
  DiffuseMatrixS,
  DiffuseMatrixT,
  SpecularMatrixS,
  SpecularMatrixT,
  VertexColorModulate,
  VertexColorAdd,
  Color,
  ViewOrigin,
  GlobalEyePos,
  MvpMatrixX,
  MvpMatrixY,
  MvpMatrixZ,
  MvpMatrixW,
  ModelMatrixX,
  ModelMatrixY,
  ModelMatrixZ,
  ModelMatrixW,
  ProjMatrixX,
  ProjMatrixY,
  ProjMatrixZ,
  ProjMatrixW,
  ModelViewMatrixX,
  ModelViewMatrixY,
  ModelViewMatrixZ,
  ModelViewMatrixW,
  TextureMatrixS,
  TextureMatrixT,
  TexGen0S,
  TexGen0T,
  TexGen0Q,
  TexGen0Enabled,
  TexGen1S,
  TexGen1T,
  TexGen1Q,
  TexGen1Enabled,
  WobbleSkyX,
  WobbleSkyY,
  WobbleSkyZ,
  Overbright,
  EnableSkinning,
  AlphaTest,
  AmbientColor,
  GlobalLightOrigin,
  JitterTexScale,
  JitterTexOffset,
  PsxDistortions,
  CascadeDistances,
  ShadowMatrix0X,
  ShadowMatrix0Y,
  ShadowMatrix0Z,
  ShadowMatrix0W,
  ShadowMatrix1X,
  ShadowMatrix1Y,
  ShadowMatrix1Z,
  ShadowMatrix1W,
  ShadowMatrix2X,
  ShadowMatrix2Y,
  ShadowMatrix2Z,
  ShadowMatrix2W,
  ShadowMatrix3X,
  ShadowMatrix3Y,
  ShadowMatrix3Z,
  ShadowMatrix3W,
  ShadowMatrix4X,
  ShadowMatrix4Y,
  ShadowMatrix4Z,
  ShadowMatrix4W,
  ShadowMatrix5X,
  ShadowMatrix5Y,
  ShadowMatrix5Z,
  ShadowMatrix5W,
  ShadowAtlasOffset0,
  ShadowAtlasOffset1,
  ShadowAtlasOffset2,
  ShadowAtlasOffset3,
  ShadowAtlasOffset4,
  ShadowAtlasOffset5,
  ShIrradiance0,
  ShIrradiance1,
  ShIrradiance2,
  ShIrradiance3,
  ShIrradiance4,
  ShIrradiance5,
  ShIrradiance6,
  ShIrradiance7,
  ShIrradiance8,
  User0,
  User1,
  User2,
  User3,
  User4,
  User5,
  User6,
  User7,
  Total,
};

// the engine's names, RENDERPARM_<...>, in enum order
std::string_view renderParmName(RenderParm parm);

// ----- renderparm sets (RenderProgs.h:219-318, renderParmSet<N>.inc.hlsl) -----

// renderParmSubSet_t: 15 sets; renderParmNullSet is -1 here
inline constexpr int EngineParmSetCount = 15;

// a set's member: the renderparm and the float4's name in the set's HLSL struct
struct EngineParmSetMember
{
  RenderParm parm = RenderParm::Total;
  std::string_view shaderName;
};

// rpMinimalSet0 and renderParmSet0_t (SC217): CONSTANT_BUFFER_ONLY (color, depth)
inline constexpr auto EngineParmSet0 = std::array<EngineParmSetMember, 8>{{
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::Color, "rpColor"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
}};

// rpNominalSet4 and renderParmSet4_t (SC219): TEXTURE (texture_color)
inline constexpr auto EngineParmSet4 = std::array<EngineParmSetMember, 16>{{
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::VertexColorModulate, "rpVertexColorModulate"},
  {RenderParm::VertexColorAdd, "rpVertexColorAdd"},
  {RenderParm::Color, "rpColor"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::TextureMatrixS, "rpTextureMatrixS"},
  {RenderParm::TextureMatrixT, "rpTextureMatrixT"},
  {RenderParm::TexGen0S, "rpTexGen0S"},
  {RenderParm::TexGen0T, "rpTexGen0T"},
  {RenderParm::TexGen0Enabled, "rpTexGen0Enabled"},
  {RenderParm::AlphaTest, "rpAlphaTest"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
}};

// rpMaximalSet10 and renderParmSet10_t (SC232): DRAW_INTERACTION, 25 float4 = 400 bytes
inline constexpr auto EngineParmSet10 = std::array<EngineParmSetMember, 25>{{
  {RenderParm::DiffuseModifier, "rpDiffuseModifier"},
  {RenderParm::SpecularModifier, "rpSpecularModifier"},
  {RenderParm::LocalLightOrigin, "rpLocalLightOrigin"},
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::LightProjectionS, "rpLightProjectionS"},
  {RenderParm::LightProjectionT, "rpLightProjectionT"},
  {RenderParm::LightProjectionQ, "rpLightProjectionQ"},
  {RenderParm::LightFalloffS, "rpLightFalloffS"},
  {RenderParm::BumpMatrixS, "rpBumpMatrixS"},
  {RenderParm::BumpMatrixT, "rpBumpMatrixT"},
  {RenderParm::DiffuseMatrixS, "rpDiffuseMatrixS"},
  {RenderParm::DiffuseMatrixT, "rpDiffuseMatrixT"},
  {RenderParm::SpecularMatrixS, "rpSpecularMatrixS"},
  {RenderParm::SpecularMatrixT, "rpSpecularMatrixT"},
  {RenderParm::VertexColorModulate, "rpVertexColorModulate"},
  {RenderParm::VertexColorAdd, "rpVertexColorAdd"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
  {RenderParm::User0, "rpUser0"},
  {RenderParm::User1, "rpUser1"},
  {RenderParm::User2, "rpUser2"},
}};

// the number of float4s of each set (the rp*Set<N> lists' lengths), for rpBufSize
inline constexpr auto EngineParmSetSizes = std::array<int, EngineParmSetCount>{
  8, 7, 5, 16, 16, 16, 14, 12, 13, 45, 25, 68, 16, 12, 32};

// the set a binding layout's renderparms come from (the rp*Set<N>LayoutTypes lists), -1
// for renderParmNullSet
int engineParmSetOfLayout(EngineBindingLayout layout);

// the members of a mirrored set (0, 4, 10); empty for the others
std::span<const EngineParmSetMember> engineParmSetMembers(int set);

// ----- the constant blocks of the layouts LP2 uses (C26, P2.19) -----

// a member of a constant block, by its name in the shader and its byte offset
struct EngineConstantMember
{
  std::string_view name;
  std::uint32_t offset = 0;

  bool operator==(const EngineConstantMember&) const = default;
};

// what a layout's uniforms item holds: the members in order and the block's size
struct EngineConstantBlock
{
  std::string_view name;
  std::vector<EngineConstantMember> members;
  std::uint32_t size = 0;

  bool operator==(const EngineConstantBlock&) const = default;
};

// a renderparm set as a block: member i at 16 · i, size 16 · count; empty members for a
// set that is not mirrored, with its size still known
EngineConstantBlock engineParmSetBlock(int set);
// ToneMappingConstants (TonemapPass_cb.h, tonemapping_cb.h), 80 bytes (SC261)
EngineConstantBlock engineToneMappingBlock();
// BlitConstants (CommonPasses.h:63-72, blit.cb.h), 36 bytes
EngineConstantBlock engineBlitBlock();

/**
 * The block a binding layout's uniforms item holds: the layout's renderparm set
 * (RenderProgs.cpp:217-391), ToneMappingConstants for TONEMAP, HISTOGRAM and EXPOSURE
 * (TonemapPass.cpp:61), CommonPasses' BlitConstants for BLIT; nullopt for a layout with
 * none or one LP2 does not mirror.
 */
std::optional<EngineConstantBlock> engineLayoutConstantBlock(EngineBindingLayout layout);

// ----- the push-constant rule (C30, SC242) -----

/**
 * DeviceManager_VK.cpp:1377-1382 with r_vkUsePushConstants 1: the push-constant budget
 * is min(the device's maxPushConstantsSize, NVRHI's c_MaxPushConstantSize), which is 256
 * outside Apple and 4096 under __APPLE__ (nvrhi.h:81-85).
 */
std::uint32_t enginePushConstantLimit(
  std::uint32_t maxPushConstantsSize,
  std::uint32_t nvrhiLimit = nvrhi::c_MaxPushConstantSize);

// DeviceManager.cpp:62-77 getGPUVendor's AMD id
inline constexpr std::uint32_t EngineVendorAmd = 0x1002;

/**
 * RenderProgs.cpp:393-413 pcEnabled: a layout's renderparm set fits the budget
 * (rpBufSize ≤ limit) and is not the null set; MIPMAPGEN, TAA_RESOLVE, TONEMAP,
 * HISTOGRAM and EXPOSURE by their passes' constant structs (16, 132, 80, 80, 80 bytes);
 * then, on Vulkan with an AMD GPU, GBUFFER, TEXTURE and their skinned forms are forced to
 * a constant buffer. BLIT's builtin layout has none (CommonPasses' blit layout always
 * pushes its 36 bytes).
 */
bool enginePushConstantsEnabled(
  EngineBindingLayout layout, std::uint32_t pushConstantLimit, std::uint32_t vendorID);

/**
 * A builtin's defines in its row's order, USE_PUSH_CONSTANTS made "1" or "0" by the
 * row's binding layout (usePushConstants, RenderProgs.cpp:777): the key of the
 * permutation the engine loads (C2).
 */
std::vector<ShaderDefine> engineBuiltinDefines(
  const EngineBuiltin& builtin, bool pushConstantsEnabled);

// the row of a BUILTIN_ index, or null outside the table
const EngineBuiltin* engineBuiltin(int index);

// ----- binding layouts (C3, SC236-SC237, SC262) -----

/**
 * The binding layouts of a builtin's layout type, one per descriptor set in set order,
 * as the engine creates them: bindingLayouts[type] (RenderProgs.cpp:79-108, 415-750) for
 * CONSTANT_BUFFER_ONLY, TEXTURE and DRAW_INTERACTION; TonemapPass' one layout
 * (TonemapPass.cpp:119-227) for HISTOGRAM, EXPOSURE and TONEMAP; CommonPasses' blit
 * layout (CommonPasses.cpp:166-177) for BLIT, which the engine draws its blits with (the
 * builtin BLIT layout, one volatile constant buffer, is never used). The uniforms item is
 * PushConstants(0, size) when pushConstants, else VolatileConstantBuffer(0) (cbStatic is
 * always false). nullopt for a type LP2 does not mirror.
 */
std::optional<std::vector<nvrhi::BindingLayoutDesc>> engineBindingLayoutDescs(
  EngineBindingLayout layout, bool pushConstants);

// NVRHI's default Vulkan binding offsets, equal to the engine's -fvk-*-shift options
// (cmake/compileshaders.cmake:120-122, SC251); never changed (C2)
inline constexpr auto EngineVulkanBindingOffsets = nvrhi::VulkanBindingOffsets{};

// the Vulkan binding number a layout item lands on: its slot plus its type's offset
std::uint32_t engineVulkanBinding(const nvrhi::BindingLayoutItem& item);

// ----- the vertex input (SC243) -----

/**
 * LAYOUT_DRAW_VERT (RenderProgs.cpp:131-165): POSITION RGB32_FLOAT, TEXCOORD RG16_FLOAT,
 * NORMAL and TANGENT RGBA8_UNORM, COLOR RGBA8_UNORM with arraySize 2, at idDrawVert's
 * offsets, stride 32. NVRHI's Vulkan input layout gives them locations 0 to 5.
 */
std::vector<nvrhi::VertexAttributeDesc> engineDrawVertAttributes();

// a vertex shader input location, with the components the engine's VS_IN declares for it
// (float4 position, float2 texcoord, float4 normal, tangent, color, color2)
struct EngineVertexInput
{
  std::uint32_t location = 0;
  std::string_view semantic;
  std::uint32_t components = 0;

  bool operator==(const EngineVertexInput&) const = default;
};
std::span<const EngineVertexInput> engineDrawVertInputs();

// ----- samplers (D68, SC240, SC269) -----

/**
 * CommonPasses::Init's samplers (CommonPasses.cpp:71-97), built as the engine builds
 * them: one SamplerDesc changed in place, so each inherits the one before; from
 * LinearClampCompare on every one keeps the Comparison reduction (the leaked desc, D68).
 */
struct EngineCommonSamplers
{
  nvrhi::SamplerDesc pointClamp;
  nvrhi::SamplerDesc pointWrap;
  nvrhi::SamplerDesc linearClamp;
  nvrhi::SamplerDesc linearBorder;
  nvrhi::SamplerDesc linearClampCompare;
  nvrhi::SamplerDesc linearWrap;
  nvrhi::SamplerDesc anisotropicWrap;
  nvrhi::SamplerDesc anisotropicClampEdge;
};
EngineCommonSamplers engineCommonSamplers();

// r_maxAnisotropicFiltering's default (RenderSystem_init.cpp:110)
inline constexpr int EngineMaxAnisotropicFiltering = 8;

/**
 * idImage::CreateSampler (Image_NVRHI.cpp:173-243): the sampler of an image of this
 * format (textureFormat_t), filter (textureFilter_t) and repeat (textureRepeat_t), the
 * TEXTURE layout's per-image sampler (SC269); a depth format gets Comparison. nullopt for
 * a filter or repeat the engine refuses with FatalError.
 */
std::optional<nvrhi::SamplerDesc> engineImageSamplerDesc(
  int format, int filter, int repeat, int maxAnisotropy = EngineMaxAnisotropicFiltering);

// ----- image formats and swizzles (SC265, SC266) -----

/**
 * idImage::AllocImage's format map (Image_NVRHI.cpp:349-459): an engine textureFormat_t
 * as an NVRHI format; DEPTH_STENCIL is D24S8 when the device has it, else D32S8; X16 and
 * Y16_X16 stay at the default RGBA8_UINT; nullopt where the engine raises idLib::Error
 * (BC7, ETC1, an unknown format).
 */
std::optional<nvrhi::Format> engineTextureFormat(int format, bool d24s8Supported);

// AllocImage's component mappings (Image_NVRHI.cpp:513-554) by colour format and format;
// the identity for every other
nvrhi::ComponentMapping engineComponentMapping(int format, int colorFormat);

// ----- the tonemap pass (SC259-SC262, D70) -----

// The constants TonemapPass feeds its three shaders with, at this tree's defaults.
struct EngineTonemapDefaults
{
  // ToneMappingParameters (TonemapPass.h:30-41), as the tonemap block passes it
  float histogramLowPercentile = 0.8f;
  float histogramHighPercentile = 0.95f;
  float whitePoint = 3.0f;
  bool enableColorLut = true;
  // ComputeExposure's log-luminance range: scale max - min, bias min
  // (TonemapPass.cpp:439-446)
  float exposureLogMin = -10.0f;
  float exposureLogMax = 4.0f;
  // r_hdrAdaptionRate (RenderSystem_init.cpp:273): the speed up; down is half of it
  float adaptationRate = 1.0f;
  // r_hdrMinLuminance and r_hdrMaxLuminance (:274-275): the histogram's luminance range
  // and the adapted-luminance bounds of the exposure and tonemap passes
  float hdrMinLuminance = 0.02f;
  float hdrMaxLuminance = 0.5f;
  // r_exposure (:301): exposureScale = exp2(r_exposure)
  float exposure = 0.5f;
  // TonemapPass::CreateParameters (TonemapPass.h:47)
  std::uint32_t histogramBins = 256;
};
inline constexpr auto EngineTonemap = EngineTonemapDefaults{};

} // namespace tb::lit
