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
// NVRHI/Image_NVRHI.cpp, NVRHI/RenderBackend_NVRHI.cpp, RenderSystem_init.cpp,
// RenderBackend.cpp, GLState.h,
// sys/DeviceManager.cpp, sys/DeviceManager_VK.cpp and shaders/renderParmSet*.inc.hlsl):
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

/**
 * LP6.5a (C69, SC807): rpNominalSet3 (RenderProgs.h:261, its layouts :263 GBUFFER and its
 * skinned form) and renderParmSet3_t (renderParmSet3.inc.hlsl:32-57), 16 float4 = 256
 * bytes: the G-buffer program (12)
 */
inline constexpr auto EngineParmSet3 = std::array<EngineParmSetMember, 16>{{
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::BumpMatrixS, "rpBumpMatrixS"},
  {RenderParm::BumpMatrixT, "rpBumpMatrixT"},
  {RenderParm::SpecularMatrixS, "rpSpecularMatrixS"},
  {RenderParm::SpecularMatrixT, "rpSpecularMatrixT"},
  {RenderParm::VertexColorModulate, "rpVertexColorModulate"},
  {RenderParm::VertexColorAdd, "rpVertexColorAdd"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ModelMatrixX, "rpModelMatrixX"},
  {RenderParm::ModelMatrixY, "rpModelMatrixY"},
  {RenderParm::ModelMatrixZ, "rpModelMatrixZ"},
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

/**
 * LP4.2a (C35, SC330): rpMaximalSet9 (RenderProgs.h:291, its layouts :293
 * AMBIENT_LIGHTING_IBL and its skinned form) and renderParmSet9_t
 * (renderParmSet9.inc.hlsl:33-85), 45 float4 = 720 bytes, the two lists in the same
 * order. The struct's last member is the array float4 rpSHIrradiance[9]; its elements are
 * named here as reflection lists them (an array member expanded, lit/ShaderReflection.h),
 * each 16 bytes on from the one before, so that member i sits at 16 · i as in every set.
 */
inline constexpr auto EngineParmSet9 = std::array<EngineParmSetMember, 45>{{
  {RenderParm::ScreenCorrectionFactor, "rpScreenCorrectionFactor"},
  {RenderParm::WindowCoord, "rpWindowCoord"},
  {RenderParm::DiffuseModifier, "rpDiffuseModifier"},
  {RenderParm::SpecularModifier, "rpSpecularModifier"},
  {RenderParm::LocalLightOrigin, "rpLocalLightOrigin"},
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::BumpMatrixS, "rpBumpMatrixS"},
  {RenderParm::BumpMatrixT, "rpBumpMatrixT"},
  {RenderParm::DiffuseMatrixS, "rpDiffuseMatrixS"},
  {RenderParm::DiffuseMatrixT, "rpDiffuseMatrixT"},
  {RenderParm::SpecularMatrixS, "rpSpecularMatrixS"},
  {RenderParm::SpecularMatrixT, "rpSpecularMatrixT"},
  {RenderParm::VertexColorModulate, "rpVertexColorModulate"},
  {RenderParm::VertexColorAdd, "rpVertexColorAdd"},
  {RenderParm::GlobalEyePos, "rpGlobalEyePos"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ModelMatrixX, "rpModelMatrixX"},
  {RenderParm::ModelMatrixY, "rpModelMatrixY"},
  {RenderParm::ModelMatrixZ, "rpModelMatrixZ"},
  {RenderParm::ModelMatrixW, "rpModelMatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::AmbientColor, "rpAmbientColor"},
  {RenderParm::GlobalLightOrigin, "rpGlobalLightOrigin"},
  {RenderParm::JitterTexScale, "rpJitterTexScale"},
  {RenderParm::JitterTexOffset, "rpJitterTexOffset"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
  {RenderParm::CascadeDistances, "rpCascadeDistances"},
  {RenderParm::WobbleSkyX, "rpWobbleSkyX"},
  {RenderParm::WobbleSkyY, "rpWobbleSkyY"},
  {RenderParm::WobbleSkyZ, "rpWobbleSkyZ"},
  {RenderParm::TexGen0S, "rpTexGen0S"},
  {RenderParm::TexGen0T, "rpTexGen0T"},
  {RenderParm::TexGen0Q, "rpTexGen0Q"},
  {RenderParm::ShIrradiance0, "rpSHIrradiance[0]"},
  {RenderParm::ShIrradiance1, "rpSHIrradiance[1]"},
  {RenderParm::ShIrradiance2, "rpSHIrradiance[2]"},
  {RenderParm::ShIrradiance3, "rpSHIrradiance[3]"},
  {RenderParm::ShIrradiance4, "rpSHIrradiance[4]"},
  {RenderParm::ShIrradiance5, "rpSHIrradiance[5]"},
  {RenderParm::ShIrradiance6, "rpSHIrradiance[6]"},
  {RenderParm::ShIrradiance7, "rpSHIrradiance[7]"},
  {RenderParm::ShIrradiance8, "rpSHIrradiance[8]"},
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

/**
 * LP3.0 (C60, SC730, P3.29): rpMaximalSet11 (RenderProgs.h:301, its layouts :303
 * DRAW_INTERACTION_SM and its skinned form) and renderParmSet11_t
 * (renderParmSet11.inc.hlsl:32-87), 68 float4 = 1088 bytes, the two lists in the same
 * order: the interactionSM programs (builtins 27-50). The struct's arrays float4
 * rpShadowMatrices[6 * 4] (bytes 560-943) and rpShadowAtlasOffsets[6] (944-1039) are
 * named element by element as reflection lists them (lit/ShaderReflection.h), each 16
 * bytes on from the one before; rpUser0-2 follow at 1040, 1056 and 1072. Never pushed:
 * 1088 bytes pass every push-constant budget but Apple's (pcEnabled false, C30)
 */
inline constexpr auto EngineParmSet11 = std::array<EngineParmSetMember, 68>{{
  {RenderParm::ScreenCorrectionFactor, "rpScreenCorrectionFactor"},
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
  {RenderParm::ModelMatrixX, "rpModelMatrixX"},
  {RenderParm::ModelMatrixY, "rpModelMatrixY"},
  {RenderParm::ModelMatrixZ, "rpModelMatrixZ"},
  {RenderParm::ModelMatrixW, "rpModelMatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::ModelViewMatrixX, "rpModelViewMatrixX"},
  {RenderParm::ModelViewMatrixY, "rpModelViewMatrixY"},
  {RenderParm::ModelViewMatrixZ, "rpModelViewMatrixZ"},
  {RenderParm::ModelViewMatrixW, "rpModelViewMatrixW"},
  {RenderParm::GlobalLightOrigin, "rpGlobalLightOrigin"},
  {RenderParm::JitterTexScale, "rpJitterTexScale"},
  {RenderParm::JitterTexOffset, "rpJitterTexOffset"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
  {RenderParm::CascadeDistances, "rpCascadeDistances"},
  {RenderParm::ShadowMatrix0X, "rpShadowMatrices[0]"},
  {RenderParm::ShadowMatrix0Y, "rpShadowMatrices[1]"},
  {RenderParm::ShadowMatrix0Z, "rpShadowMatrices[2]"},
  {RenderParm::ShadowMatrix0W, "rpShadowMatrices[3]"},
  {RenderParm::ShadowMatrix1X, "rpShadowMatrices[4]"},
  {RenderParm::ShadowMatrix1Y, "rpShadowMatrices[5]"},
  {RenderParm::ShadowMatrix1Z, "rpShadowMatrices[6]"},
  {RenderParm::ShadowMatrix1W, "rpShadowMatrices[7]"},
  {RenderParm::ShadowMatrix2X, "rpShadowMatrices[8]"},
  {RenderParm::ShadowMatrix2Y, "rpShadowMatrices[9]"},
  {RenderParm::ShadowMatrix2Z, "rpShadowMatrices[10]"},
  {RenderParm::ShadowMatrix2W, "rpShadowMatrices[11]"},
  {RenderParm::ShadowMatrix3X, "rpShadowMatrices[12]"},
  {RenderParm::ShadowMatrix3Y, "rpShadowMatrices[13]"},
  {RenderParm::ShadowMatrix3Z, "rpShadowMatrices[14]"},
  {RenderParm::ShadowMatrix3W, "rpShadowMatrices[15]"},
  {RenderParm::ShadowMatrix4X, "rpShadowMatrices[16]"},
  {RenderParm::ShadowMatrix4Y, "rpShadowMatrices[17]"},
  {RenderParm::ShadowMatrix4Z, "rpShadowMatrices[18]"},
  {RenderParm::ShadowMatrix4W, "rpShadowMatrices[19]"},
  {RenderParm::ShadowMatrix5X, "rpShadowMatrices[20]"},
  {RenderParm::ShadowMatrix5Y, "rpShadowMatrices[21]"},
  {RenderParm::ShadowMatrix5Z, "rpShadowMatrices[22]"},
  {RenderParm::ShadowMatrix5W, "rpShadowMatrices[23]"},
  {RenderParm::ShadowAtlasOffset0, "rpShadowAtlasOffsets[0]"},
  {RenderParm::ShadowAtlasOffset1, "rpShadowAtlasOffsets[1]"},
  {RenderParm::ShadowAtlasOffset2, "rpShadowAtlasOffsets[2]"},
  {RenderParm::ShadowAtlasOffset3, "rpShadowAtlasOffsets[3]"},
  {RenderParm::ShadowAtlasOffset4, "rpShadowAtlasOffsets[4]"},
  {RenderParm::ShadowAtlasOffset5, "rpShadowAtlasOffsets[5]"},
  {RenderParm::User0, "rpUser0"},
  {RenderParm::User1, "rpUser1"},
  {RenderParm::User2, "rpUser2"},
}};

/**
 * LP5.0 (C48, SC524-SC526): rpNominalSet5 (RenderProgs.h:271, its layouts :273 LEGACY,
 * NORMAL_CUBE and its skinned form) and renderParmSet5_t (renderParmSet5.inc.hlsl:32-57),
 * 16 float4 = 256 bytes, the two lists in the same order: the skybox, wobblesky and
 * bumpyenvironment programs (69, 70, 57)
 */
inline constexpr auto EngineParmSet5 = std::array<EngineParmSetMember, 16>{{
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::VertexColorModulate, "rpVertexColorModulate"},
  {RenderParm::VertexColorAdd, "rpVertexColorAdd"},
  {RenderParm::Color, "rpColor"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ModelMatrixX, "rpModelMatrixX"},
  {RenderParm::ModelMatrixY, "rpModelMatrixY"},
  {RenderParm::ModelMatrixZ, "rpModelMatrixZ"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::WobbleSkyX, "rpWobbleSkyX"},
  {RenderParm::WobbleSkyY, "rpWobbleSkyY"},
  {RenderParm::WobbleSkyZ, "rpWobbleSkyZ"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
}};

/**
 * LP5.0 (C49, SC533-SC537): rpNominalSet7 (RenderProgs.h:281, its layouts :283 FOG,
 * BLENDLIGHT and their skinned forms) and renderParmSet7_t
 * (renderParmSet7.inc.hlsl:32-51), 12 float4 = 192 bytes: the fog and blendlight programs
 * (67, 65). The set has no TEXTUREMATRIX: BlendLight's texture matrix is loaded and never
 * read (a mirrored fact)
 */
inline constexpr auto EngineParmSet7 = std::array<EngineParmSetMember, 12>{{
  {RenderParm::Color, "rpColor"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::TexGen0S, "rpTexGen0S"},
  {RenderParm::TexGen0T, "rpTexGen0T"},
  {RenderParm::TexGen0Q, "rpTexGen0Q"},
  {RenderParm::TexGen1S, "rpTexGen1S"},
  {RenderParm::TexGen1T, "rpTexGen1T"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
}};

/**
 * LP5.0 (C47, SC527): rpNominalSet8 (RenderProgs.h:286, its layout :288
 * POST_PROCESS_INGAME) and renderParmSet8_t (renderParmSet8.inc.hlsl:32-52), 13 float4 =
 * 208 bytes: the material programs of newStage blocks (MAT:2180-2217)
 */
inline constexpr auto EngineParmSet8 = std::array<EngineParmSetMember, 13>{{
  {RenderParm::WindowCoord, "rpWindowCoord"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ProjMatrixY, "rpProjectionMatrixY"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::ModelViewMatrixZ, "rpModelViewMatrixZ"},
  {RenderParm::EnableSkinning, "rpEnableSkinning"},
  {RenderParm::AlphaTest, "rpAlphaTest"},
  {RenderParm::User0, "rpUser0"},
  {RenderParm::User1, "rpUser1"},
  {RenderParm::User2, "rpUser2"},
}};

/**
 * LP5.0 (C48): rpNominalSet12 (RenderProgs.h:306, its layout :308 TEXGEN) and
 * renderParmSet12_t (renderParmSet12.inc.hlsl:32-57), 16 float4 = 256 bytes: the
 * texture_color_texgen program (18) of TG_SCREEN and TG_SCREEN2 stages
 */
inline constexpr auto EngineParmSet12 = std::array<EngineParmSetMember, 16>{{
  {RenderParm::ScreenCorrectionFactor, "rpScreenCorrectionFactor"},
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
  {RenderParm::TexGen0Q, "rpTexGen0Q"},
  {RenderParm::TexGen0Enabled, "rpTexGen0Enabled"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
}};

/**
 * LP6.5b (C72, SC829): rpMaximalSet14 (RenderProgs.h:316, its layouts :318
 * OCTAHEDRON_CUBE and its skinned form) and renderParmSet14_t
 * (renderParmSet14.inc.hlsl:32-70), 32 float4 = 512 bytes (never pushed: over the
 * 256-byte budget), the two lists in the same order: bumpyenvironment2 (59). The struct's
 * last member is the array float4 rpShadowMatrices[2 * 4], named element by element as
 * reflection lists it.
 */
inline constexpr auto EngineParmSet14 = std::array<EngineParmSetMember, 32>{{
  {RenderParm::WindowCoord, "rpWindowCoord"},
  {RenderParm::LocalLightOrigin, "rpLocalLightOrigin"},
  {RenderParm::LocalViewOrigin, "rpLocalViewOrigin"},
  {RenderParm::Color, "rpColor"},
  {RenderParm::GlobalEyePos, "rpGlobalEyePos"},
  {RenderParm::MvpMatrixX, "rpMVPmatrixX"},
  {RenderParm::MvpMatrixY, "rpMVPmatrixY"},
  {RenderParm::MvpMatrixZ, "rpMVPmatrixZ"},
  {RenderParm::MvpMatrixW, "rpMVPmatrixW"},
  {RenderParm::ModelMatrixX, "rpModelMatrixX"},
  {RenderParm::ModelMatrixY, "rpModelMatrixY"},
  {RenderParm::ModelMatrixZ, "rpModelMatrixZ"},
  {RenderParm::ModelMatrixW, "rpModelMatrixW"},
  {RenderParm::ProjMatrixZ, "rpProjectionMatrixZ"},
  {RenderParm::ProjMatrixW, "rpProjectionMatrixW"},
  {RenderParm::ModelViewMatrixX, "rpModelViewMatrixX"},
  {RenderParm::ModelViewMatrixY, "rpModelViewMatrixY"},
  {RenderParm::ModelViewMatrixZ, "rpModelViewMatrixZ"},
  {RenderParm::WobbleSkyX, "rpWobbleSkyX"},
  {RenderParm::WobbleSkyY, "rpWobbleSkyY"},
  {RenderParm::WobbleSkyZ, "rpWobbleSkyZ"},
  {RenderParm::GlobalLightOrigin, "rpGlobalLightOrigin"},
  {RenderParm::JitterTexScale, "rpJitterTexScale"},
  {RenderParm::PsxDistortions, "rpPSXDistortions"},
  {RenderParm::ShadowMatrix0X, "rpShadowMatrices[0]"},
  {RenderParm::ShadowMatrix0Y, "rpShadowMatrices[1]"},
  {RenderParm::ShadowMatrix0Z, "rpShadowMatrices[2]"},
  {RenderParm::ShadowMatrix0W, "rpShadowMatrices[3]"},
  {RenderParm::ShadowMatrix1X, "rpShadowMatrices[4]"},
  {RenderParm::ShadowMatrix1Y, "rpShadowMatrices[5]"},
  {RenderParm::ShadowMatrix1Z, "rpShadowMatrices[6]"},
  {RenderParm::ShadowMatrix1W, "rpShadowMatrices[7]"},
}};

// the number of float4s of each set (the rp*Set<N> lists' lengths), for rpBufSize
inline constexpr auto EngineParmSetSizes = std::array<int, EngineParmSetCount>{
  8, 7, 5, 16, 16, 16, 14, 12, 13, 45, 25, 68, 16, 12, 32};

// the set a binding layout's renderparms come from (the rp*Set<N>LayoutTypes lists), -1
// for renderParmNullSet
int engineParmSetOfLayout(EngineBindingLayout layout);

// the members of a mirrored set (0, 4, 9, 10; LP5.0: 5, 7, 8, 12; LP3.0: 11; LP6.5a: 3;
// LP6.5b: 14); empty for the others
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
// LP6.5a (C70, SC812): SsaoConstants (SsaoPass_cb.h, the struct of the three SSAO
// shaders' c_Ssao at b1), 272 bytes
EngineConstantBlock engineSsaoBlock();

/**
 * The block a binding layout's uniforms item holds: the layout's renderparm set
 * (RenderProgs.cpp:217-391), ToneMappingConstants for TONEMAP, HISTOGRAM and EXPOSURE
 * (TonemapPass.cpp:61), CommonPasses' BlitConstants for BLIT, LP6.5a SsaoConstants for
 * the three SSAO layouts (their b1); nullopt for a layout with none or one LP2 does not
 * mirror.
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

// ----- the material programs (LP5.0, C47, D121, SC527) -----

/**
 * A program a material's newStage block names (program, vertexProgram, fragmentProgram;
 * Material.cpp:2180-2217): the engine finds its shaders by name with FindShader( name,
 * stage, "", { USE_GPU_SKINNING "0", USE_PUSH_CONSTANTS pcEnabled( POST_PROCESS_INGAME )
 * } ) (RenderProgs.cpp:1157-1185) and makes the program with FindProgram( ...,
 * BINDING_LAYOUT_POST_PROCESS_INGAME ) (Material.cpp:2258). The preview keeps them in a
 * table of its own, not in the fixed builtins table (P6.21): each row a builtins-shaped
 * row (index EngineMaterialProgramBase + its place, the shaders.cfg name, no suffix, the
 * two defines, vertex and pixel, LAYOUT_DRAW_VERT, POST_PROCESS_INGAME) that the
 * reflection check and the compile thread take as they take a builtin, and whether its
 * pixel shader declares t_CurrentRender: such a program draws over _currentRender, so its
 * material is SS_POST_PROCESS: the first stage call stops before it and (LP6.5b, C71) the
 * second draws it over the frame's copy.
 */
struct EngineMaterialProgram
{
  EngineBuiltin row;
  bool readsCurrentRender = false;
};

// the first index of the material programs, above the 116 builtins
inline constexpr int EngineMaterialProgramBase = 1000;

namespace detail
{
constexpr EngineMaterialProgram materialProgram(
  const int place, const char* name, const bool readsCurrentRender)
{
  return EngineMaterialProgram{
    EngineBuiltin{
      EngineMaterialProgramBase + place,
      name,
      "",
      {{{"USE_GPU_SKINNING", "0"}, {"USE_PUSH_CONSTANTS", nullptr}}},
      2,
      false,
      EngineStageDefault,
      EngineVertexLayout::DrawVert,
      EngineBindingLayout::PostProcessIngame},
    readsCurrentRender};
}
} // namespace detail

/**
 * The "User shaders" of shaders.cfg:130-154 at 285ea4d1 (the engine's shader build lists
 * each name's .vs and .ps with USE_GPU_SKINNING={0,1} USE_PUSH_CONSTANTS={0,1}), in its
 * order; all twelve have both blobs in the game folder of this machine
 * (NecroGameBuild/base/renderprogs2/spirv). Ten read _currentRender (C47, P6.3); the
 * fork's vornoipattern declares no texture, and fogwithlights declares t_CurrentRender
 * (fogwithlights.ps.hlsl:35) but never samples it, so its compiled blobs bind no
 * t_CurrentRender (the [retail] reflection decides the flags, as the blobs are the
 * truth).
 */
inline constexpr auto EngineMaterialPrograms = std::array<EngineMaterialProgram, 12>{{
  detail::materialProgram(0, "bloodorb_draw", true),
  detail::materialProgram(1, "bloodorb1_capture", true),
  detail::materialProgram(2, "bloodorb2_capture", true),
  detail::materialProgram(3, "bloodorb3_capture", true),
  detail::materialProgram(4, "colorProcess", true),
  detail::materialProgram(5, "enviroSuit", true),
  detail::materialProgram(6, "fogwithlights", false),
  detail::materialProgram(7, "heathaze", true),
  detail::materialProgram(8, "heatHazeWithMask", true),
  detail::materialProgram(9, "heatHazeWithMaskAndVertex", true),
  detail::materialProgram(10, "mattiascrt", true),
  detail::materialProgram(11, "vornoipattern", false),
}};

/**
 * The table's row of a name as a stage gives it, as FindShader matches it
 * (RenderProgs.cpp:1159-1166): the file extension stripped (idStr::StripFileExtension,
 * as the blob path strips it, lit/ShaderBlobs.h), the case ignored (Icmp, C1's rule);
 * null for a name not in the table.
 */
const EngineMaterialProgram* findEngineMaterialProgram(std::string_view name);

/**
 * C47, D9: the row of a name whose two blobs (renderprogs2/spirv/<name>.vs.bin and
 * .ps.bin) are loose files of the game folder; nullopt for a name not in the table or a
 * program with a blob missing (the stage pass then disables the stage with one warning,
 * LP5.2a). A plain file lookup; any thread.
 */
std::optional<EngineMaterialProgram> engineMaterialProgram(
  const std::filesystem::path& gamePath, std::string_view name);

// ----- the named programs (LP6.5a, C70, P8.13, the director's reading (3)) -----

/**
 * A program a pass of the engine finds by name with FindShader( name, stage, "", defines,
 * true, layout ) and binds with a binding layout of its own, not through the builtins
 * table: SsaoPass' three compute shaders (SsaoPass.cpp:74-76, 98-104, 127-132). The
 * preview keeps them in a table of their own (as the material programs): each row a
 * builtins-shaped row (index EngineNamedProgramBase + its place, the name, no suffix, the
 * defines in FindShader's order with their values, no USE_PUSH_CONSTANTS: CFG:126-128,
 * the compute stage, no vertex layout, the pass's own EngineBindingLayout), so that the
 * compile thread makes its pipeline as it makes a builtin's (makeCompute: the blob by
 * name, the permutation by the defines, D9's reflection against
 * engineExpectedLayout( row ), which engineBindingLayoutDescs and
 * engineLayoutConstantBlock give for the row's layout).
 */
struct EngineNamedProgram
{
  EngineBuiltin row;
};

// the first index of the named programs, above the material programs
inline constexpr int EngineNamedProgramBase = 2000;

// SsaoPass::CreateParameters' defaults (SsaoPass.h:47-54, SsaoPass.cpp:163): no linear
// depth, no octahedral normals, no directional occlusion
inline constexpr auto EngineNamedPrograms = std::array<EngineNamedProgram, 3>{{
  {EngineBuiltin{
    EngineNamedProgramBase + 0,
    "builtin/SSAO/ssao_deinterleave",
    "",
    {{{"LINEAR_DEPTH", "0"}}},
    1,
    false,
    EngineStageCompute,
    EngineVertexLayout::Unknown,
    EngineBindingLayout::SsaoDeinterleave}},
  {EngineBuiltin{
    EngineNamedProgramBase + 1,
    "builtin/SSAO/ssao_compute",
    "",
    {{{"OCT_ENCODED_NORMALS", "0"}, {"DIRECTIONAL_OCCLUSION", "0"}}},
    2,
    false,
    EngineStageCompute,
    EngineVertexLayout::Unknown,
    EngineBindingLayout::SsaoCompute}},
  {EngineBuiltin{
    EngineNamedProgramBase + 2,
    "builtin/SSAO/ssao_blur",
    "",
    {{{"DIRECTIONAL_OCCLUSION", "0"}}},
    1,
    false,
    EngineStageCompute,
    EngineVertexLayout::Unknown,
    EngineBindingLayout::SsaoBlur}},
}};

// the named programs' indices, in SsaoPass::Render's dispatch order
inline constexpr int EngineNamedSsaoDeinterleave = EngineNamedProgramBase + 0;
inline constexpr int EngineNamedSsaoCompute = EngineNamedProgramBase + 1;
inline constexpr int EngineNamedSsaoBlur = EngineNamedProgramBase + 2;

// the row of a named program's index; null for any other index
const EngineNamedProgram* engineNamedProgram(int index);

// the row of a program index: a builtin's (engineBuiltin), a material program's
// (EngineMaterialProgramBase + its place) or (LP6.5a) a named program's
// (EngineNamedProgramBase + its place); null for any other index
const EngineBuiltin* engineProgram(int index);

// ----- binding layouts (C3, SC236-SC237, SC262) -----

/**
 * The binding layouts of a builtin's layout type, one per descriptor set in set order,
 * as the engine creates them: bindingLayouts[type] (RenderProgs.cpp:79-108, 415-750) for
 * CONSTANT_BUFFER_ONLY, TEXTURE, DRAW_INTERACTION and (LP4.2a, C35) AMBIENT_LIGHTING_IBL
 * with its skinned form (four sets: the uniforms, t0-t2, t3 t4 t7 t8 t9 t10 t12 t13 t14,
 * s0 s1; the skinned uniforms set adds the joint buffer at t11); TonemapPass' one layout
 * (TonemapPass.cpp:119-227) for HISTOGRAM, EXPOSURE and TONEMAP; CommonPasses' blit
 * layout (CommonPasses.cpp:166-177) for BLIT, which the engine draws its blits with (the
 * builtin BLIT layout, one volatile constant buffer, is never used). The uniforms item is
 * PushConstants(0, size) when pushConstants, else VolatileConstantBuffer(0) (cbStatic is
 * always false). LP5.0 (C45-C49, RenderProgs.cpp:415-750): DEFAULT, TEXTURE, TEXGEN and
 * LEGACY are { uniforms }, { t0 }, { s0 } of their sets 0, 4, 12 and 5, the skinned
 * DEFAULT and TEXTURE forms adding the joint buffer at t11 to the uniforms' set;
 * NORMAL_CUBE { uniforms }, { t0 t1 }, { s0 }; FOG { uniforms }, { t0 t1 }, { s0 s1 };
 * BLENDLIGHT { uniforms }, { t0 t1 }, { s0 }; POST_PROCESS_INGAME { uniforms t0 t1 t2 },
 * { s0 } (the uniforms set's visibility All); DRAW_INTERACTION_SKINNED is
 * DRAW_INTERACTION with the joint buffer. LP3.0 (C60): DRAW_INTERACTION_SM (:530-550)
 * { uniforms }, { t0 t1 t2 }, { t3 t4 t5 t6 }, { s0 s1 s2 s3 }; its skinned form is not
 * mirrored (P3.24e). nullopt for a type the tables do not mirror.
 */
std::optional<std::vector<nvrhi::BindingLayoutDesc>> engineBindingLayoutDescs(
  EngineBindingLayout layout, bool pushConstants);

// ----- binding sets (LP5.0, C45-C49) -----

// CommonPasses' samplers by name (CommonPasses.cpp:71-97, engineCommonSamplers)
enum class EngineCommonSampler
{
  PointClamp,
  PointWrap,
  LinearClamp,
  LinearBorder,
  LinearClampCompare,
  LinearWrap,
  AnisotropicWrap,
  AnisotropicClampEdge,
};

// the desc of a common sampler, as engineCommonSamplers builds it
nvrhi::SamplerDesc engineCommonSamplerDesc(EngineCommonSampler sampler);

/**
 * What one item of a binding set holds, as idRenderBackend::GetCurrentBindingLayout
 * fills it (RenderBackend_NVRHI.cpp:566-1990): the uniforms (the constant buffer or the
 * push constants of the layout's set), the joint buffer, the texture bound at a unit
 * (GetImageAt( unit )), that texture's own sampler (GetImageAt( unit )->GetSampler, the
 * per-image sampler of engineImageSamplerDesc) or one of CommonPasses' samplers.
 */
struct EngineBindingSetItem
{
  enum class Source
  {
    Uniforms,
    JointBuffer,
    ImageUnit,
    ImageSampler,
    CommonSampler,
  };
  Source source = Source::Uniforms;
  // the register the item binds (t, s or b)
  std::uint32_t slot = 0;
  // ImageUnit, ImageSampler: the texture unit
  std::uint32_t unit = 0;
  // CommonSampler
  EngineCommonSampler sampler = EngineCommonSampler::PointClamp;

  bool operator==(const EngineBindingSetItem&) const = default;
};

/**
 * The binding sets of a layout type in set order, item for item as the layout's
 * engineBindingLayoutDescs, with r_renderMode "0" (R_UsePixelatedLook false: never the
 * point samplers the pixelated look picks): CONSTANT_BUFFER_ONLY (:666-679); DEFAULT,
 * TEXTURE, TEXGEN, LEGACY (:586-623) and the skinned DEFAULT and TEXTURE (:624-665): unit
 * 0 at t0, its own sampler at s0; DRAW_INTERACTION and its skinned form (:916-1081):
 * units 0-2 at t0-t2, 3-4 at t3-t4, AnisotropicWrap and LinearBorder;
 * AMBIENT_LIGHTING_IBL and its skinned form (:698-883): AnisotropicWrap and LinearClamp;
 * FOG (:1286-1329): units 0-1, LinearClamp at s0 and s1; BLENDLIGHT (:1378-1418): units
 * 0-1, LinearBorder; POST_PROCESS_INGAME (:1464-1512): units 0-2 beside the uniforms,
 * LinearClamp; NORMAL_CUBE (:1677-1734): units 0-1, LinearWrap, which carries the
 * Comparison reduction of the leaked desc (D68) as the engine creates it. LP3.0 (C60):
 * DRAW_INTERACTION_SM (:1082-1181): units 0-6 at t0-t6 (unit 5 the shadow atlas, unit 6
 * _blueNoise256), AnisotropicWrap, LinearBorder, LinearClampCompare, PointWrap; no
 * SM_SKINNED set. nullopt for the passes' own layouts and the types the tables do not
 * mirror.
 */
std::optional<std::vector<std::vector<EngineBindingSetItem>>> engineBindingSetShape(
  EngineBindingLayout layout);

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

// ----- the frame's cvars and limits (LP2.2a, D21: the engine's defaults) -----

/**
 * The values the engine's frame and interaction code reads, at this tree's defaults
 * (RenderSystem_init.cpp, RenderBackend.cpp, RenderProgs.cpp; T0.15 of
 * 29-LP2-0-REVIEW.md). OVERBRIGHT is lightScale · 0.5, and every light stage's colour is
 * multiplied by lightScale.
 */
struct EngineFrameDefaults
{
  // r_lightScale "3" (RenderSystem_init.cpp:171)
  float lightScale = 3.0f;
  // r_offsetfactor "0" and r_offsetunits "-600" outside __ANDROID__ (:159-164)
  float offsetFactor = 0.0f;
  float offsetUnits = -600.0f;
  // r_clear "2": every frame clears hdrFBO and ldrFBO to (0, 0, 0, 1) (SC202)
  int clear = 2;
  // STENCIL_SHADOW_TEST_VALUE (GLState.h:191): the stencil clear of every 3D view
  std::uint8_t stencilClear = 128;
  // r_useLightScissors "3": the fully clipped projected bounds (:155)
  int useLightScissors = 3;
  // r_znear "3" (:118): the projection's zNear, and what R_RenderView takes off the
  // portal flow's near plane (tr_frontend_main.cpp:921)
  float znear = 3.0f;
  // r_skipInteractionFastPath "1" (RenderBackend.cpp:56): every surface runs the stage
  // loop
  bool skipInteractionFastPath = true;
  // r_mathLightsEnabled "1" (:245): a light with math_enabled shades through the math
  // path (LP5)
  bool mathLightsEnabled = true;
  // RenderInteractions' caps (RenderBackend.cpp:1760-1761): surfaces beyond them are
  // dropped silently (idStaticList::Append)
  std::size_t maxInteractionsPerLight = 2048;
  std::size_t maxComplexInteractionsPerLight = 256;
  // the volatile constant buffers' versions (RenderProgs.cpp:769)
  std::uint32_t constantBufferVersions = 8192;
  // LP4.0 (C32, lit/ViewFlow.h): the view's portal flow at its defaults
  // r_usePortals "1" (:179): the view's area is PointInArea( vieworg )
  bool usePortals = true;
  // r_singleArea "0" (:184): the flow goes through the portals
  bool singleArea = false;
  // r_useLightPortalCulling "1" (:153) and r_useEntityPortalCulling "1" (:156): the
  // frustum corners against the chain's planes
  int useLightPortalCulling = 1;
  int useEntityPortalCulling = 1;
  // r_useLightAreaCulling "1" (:154): a light that casts no shadows from an area the view
  // is not connected to is dropped
  bool useLightAreaCulling = true;
  // r_useAreasConnectedForShadowCulling "2" (tr_frontend_addlights.cpp:48): a light
  // reaches only the referenced areas connected to its own
  int useAreasConnectedForShadowCulling = 2;
  // LP4.2a (C34, D84): the ambient pass's
  // r_forceAmbient "0.5", ARCHIVE, 0..1 (RenderSystem_init.cpp:288): AMBIENT_COLOR's rgb,
  // which scales the probe path's diffuse alone (Q25: mirrored until C11's overrides)
  float forceAmbient = 0.5f;
  // r_useLightGrid "1" (:299): a surface with a grid area takes the light-grid path
  // (LP4.3; LP4.2a draws the probe path only)
  bool useLightGrid = true;
  // r_useSSAO "1", ARCHIVE (:290), with r_useNewSSAOPass "1": the ambient pass binds
  // _ao0 at t4 (SC810, SC815). LP6.5a (C70, D177): the engine's default, which the
  // preview takes per session (SessionOptions::ssaoForTesting: on in sessions, off in
  // TbLitLib tests); r_taaJitter's sample-window offset is not mirrored (K54)
  bool useSsao = true;
  // SsaoPass.cpp:33-38, the new pass's cvars: r_ssaoBackgroundViewDepth "100",
  // r_ssaoRadiusWorld "1.0", r_ssaoSurfaceBias "0.05" (metres), r_ssaoPowerExponent "2",
  // r_ssaoAmount "4"; r_ssaoBlurSharpness "16" is never read
  float ssaoBackgroundViewDepth = 100.0f;
  float ssaoRadiusWorld = 1.0f;
  float ssaoSurfaceBias = 0.05f;
  float ssaoPowerExponent = 2.0f;
  float ssaoAmount = 4.0f;
  // LP6.5b (C72, SC829): r_useSSR is "1" in the engine (RenderSystem_init.cpp:302); the
  // preview's is 0 (D19: no SSR, K2; parity pins it, A55), as useSsao's is a choice: a
  // TG_REFLECT_CUBE2 stage with a bump stage binds bumpyenvironment2 59, never 61. Its
  // SSR parms are set anyway (RenderBackend.cpp:421-435) from r_ssrStride "12",
  // r_ssrMaxDistance "100" (metres), r_ssrZThickness "2" and r_ssrJitter "0" (:303-307),
  // read only under USE_SSR
  bool useSsr = false;
  float ssrStride = 12.0f;
  float ssrMaxDistance = 100.0f;
  float ssrZThickness = 2.0f;
  float ssrJitter = 0.0f;
  // LP5.0 (C46, C47, C49, D118): the cvars of passes 7-9, each at the engine's default
  // (RenderSystem_init.cpp and RenderBackend.cpp at 285ea4d1)
  // r_skipFogLights "0" (RenderSystem_init.cpp:139): FogAllLights draws the fog lights
  bool skipFogLights = false;
  // r_skipBlendLights "0" (:138): FogAllLights draws the blend lights
  bool skipBlendLights = false;
  // r_skipTranslucent "0" (:135): the translucent interactions draw with DEPTHFUNC_LESS
  // (RenderBackend.cpp:4145-4167)
  bool skipTranslucent = false;
  // r_skipAmbient: the stage pass drawn (both DrawShaderPasses calls,
  // RenderBackend.cpp:4275); this is the preview's behaviour, as useSsao's is, not taken
  // from the engine's default, which is also "0" (RenderSystem_init.cpp:136); the parity
  // maps set 1 or 0 per row (D118: rows.json's skip_ambient and
  // LitSession::skipShaderPassesForTesting)
  bool skipAmbient = false;
  // r_skipShaderPasses "0" (RenderBackend.cpp:55): the first DrawShaderPasses call runs
  // (:6108)
  bool skipShaderPasses = false;
  // r_skipPostProcess "0" (RenderSystem_init.cpp:128): the post-process stages would draw
  // after the copy (RenderBackend.cpp:6141); the preview stops before them (K38)
  bool skipPostProcess = false;
  // r_skipNewAmbient "0", ARCHIVE (:137): newStage stages draw (RenderBackend.cpp:4517)
  bool skipNewAmbient = false;
  // r_showMathLights "0", 0..3 (:246): USER0.x of a math light is 1 + this
  // (RenderBackend.cpp:1941)
  int showMathLights = 0;
  // LP7.1a (C51, SC607): the model conversion's merge and slop cvars (Model.cpp:42-45),
  // which the copied conversions read through their own idCVars at these defaults
  // (lit/EngineModels.h engineModelCvars; equal by a test)
  // r_mergeModelSurfaces "1": the surfaces of one material merge unless IsDiscrete
  bool mergeModelSurfaces = true;
  // r_slopVertex "0.01": positions this far apart merge
  float slopVertex = 0.01f;
  // r_slopTexCoord "0.001": texture coordinates this far apart merge
  float slopTexCoord = 0.001f;
  // r_slopNormal "0.02": normals merge when their dot product is above 1 - this
  float slopNormal = 0.02f;
  // LP3.0 (C58, C60, D159, SC741): the shadow cvars, each at the engine's default
  // (RenderSystem_init.cpp at 285ea4d1 unless named); LP3.1 on read them
  // r_skipShadows "0", ARCHIVE (:151): ShadowAtlasPass and the SM programs run
  bool skipShadows = false;
  // r_useShadowAtlas "1", INTEGER, not archived (:251): the atlas path, never the arrays
  // (K50)
  int useShadowAtlas = 1;
  // r_shadowMapAtlasSize "8192", ROM (:252): the atlas's side
  int shadowMapAtlasSize = 8192;
  // r_shadowMapFrustumFOV "92" (:253): a point light side's field of view, both axes
  float shadowMapFrustumFov = 92.0f;
  // r_shadowMapSingleSide "-1" (:254): every side of a point light
  int shadowMapSingleSide = -1;
  // r_shadowMapJitterScale "2.5" (:256): the PCF disk's radius in atlas texels
  float shadowMapJitterScale = 2.5f;
  // r_shadowMapRandomizeJitter: the engine's default is "1" (:258), which feeds
  // Sys_Milliseconds and frameCount into jitterTexOffset; the preview draws the pinned
  // jitter, as parity.cfg's SWITCHES pin it to 0 (SC727, SC741), so this is the
  // preview's behaviour, not the engine's default
  bool shadowMapRandomizeJitter = false;
  // r_shadowMapSamples "16", 1..64 (:259): the PCF taps
  int shadowMapSamples = 16;
  // r_shadowMapSplits "3", 0..4 (:260): a parallel light's cascades are this + 1
  int shadowMapSplits = 3;
  // r_shadowMapSplitWeight "0.9" (:261): R_SetupSplitFrustums' lambda
  float shadowMapSplitWeight = 0.9f;
  // r_shadowMapLodScale "1.4" (:262) and r_shadowMapLodBias "0" (:263): the LOD rule
  // (engineShadowLod's defaults)
  float shadowMapLodScale = 1.4f;
  int shadowMapLodBias = 0;
  // r_shadowMapPolygonFactor "2" (:264): the tile draw's slope-scaled bias
  float shadowMapPolygonFactor = 2.0f;
  // r_vkShadowMapPolygonOffset "1" (:266): the tile draw's constant bias on Vulkan, which
  // the pipeline key makes an int (RenderBackend_NVRHI.cpp:460, SC722;
  // r_dxShadowMapPolygonOffset's 0.1 is DX12's, never the preview's)
  float vkShadowMapPolygonOffset = 1.0f;
  // r_shadowMapOccluderFacing "2" (:267): casters drawn two-sided
  int shadowMapOccluderFacing = 2;
  // r_shadowMapRegularDepthBiasScale "0.999" (:268) and r_shadowMapSunDepthBiasScale
  // "0.999" (:269): the receiver's depth scale, point and spot, and parallel
  float shadowMapRegularDepthBiasScale = 0.999f;
  float shadowMapSunDepthBiasScale = 0.999f;
  // r_forceShadowMapsOnAlphaTestedSurfaces "1" (tr_frontend_addmodels.cpp:50): a
  // perforated material casts even with noShadows (C57)
  bool forceShadowMapsOnAlphaTestedSurfaces = true;
  // r_useShadowSurfaceScissor "1" (:108): R_OptimizeViewLightsList shrinks a casting
  // shader's light rect to its surfaces' (D148)
  bool useShadowSurfaceScissor = true;
  // r_useMaskedOcclusionCulling "1", NOCHEAT (:329): the engine's default; the preview
  // has no MOC before LP6 (K51, M-FRONT-DIFF-02), and parity pins it to 0 (LP3.2's
  // parity.cfg)
  bool useMaskedOcclusionCulling = true;
};
inline constexpr auto EngineFrame = EngineFrameDefaults{};

} // namespace tb::lit
