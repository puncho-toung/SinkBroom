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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp,
// renderer/NVRHI/RenderBackend_NVRHI.cpp and renderer/Material.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "RenderParms.h"
#include "lit/EngineLight.h"
#include "lit/LightParse.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// LP2.2a (C27): the backend's decisions per surface and stage, mirrored as plain
// functions of a material's entry and its evaluated registers, so that the GPU passes
// replay them and the tests check them without a GPU. Pure; any thread.

namespace tb::lit
{

using Vec4 = RenderParms::Vec4;

namespace engine_material
{
// stageLighting_t (Material.h:186-193)
constexpr int LightingAmbient = 0;
constexpr int LightingBump = 1;
constexpr int LightingDiffuse = 2;
constexpr int LightingSpecular = 3;
constexpr int LightingCoverage = 4;
// stageVertexColor_t (:197-202)
constexpr int VertexColorIgnore = 0;
constexpr int VertexColorModulate = 1;
constexpr int VertexColorInverseModulate = 2;
// materialCoverage_t (:292-298)
constexpr int CoverageBad = 0;
constexpr int CoverageOpaque = 1;
constexpr int CoveragePerforated = 2;
constexpr int CoverageTranslucent = 3;
// materialSort_t SS_SUBVIEW (:302)
constexpr float SortSubview = -3.0f;
// materialFlags_t (:346-351)
constexpr int FlagPolygonOffset = 1 << 1;
constexpr int FlagNoShadows = 1 << 2;
constexpr int FlagForceShadows = 1 << 3;
constexpr int FlagNoSelfShadow = 1 << 4;
// texgen_t (:158-169)
constexpr int TexgenExplicit = 0;
constexpr int TexgenDiffuseCube = 1;
constexpr int TexgenReflectCube = 2;
constexpr int TexgenReflectCube2 = 3;
constexpr int TexgenSkyboxCube = 4;
constexpr int TexgenWobbleskyCube = 5;
constexpr int TexgenScreen = 6;
constexpr int TexgenScreen2 = 7;
constexpr int TexgenGlasswarp = 8;
// deform_t (:72-84)
constexpr int DeformNone = 0;
constexpr int DeformParticle = 7;
constexpr int DeformParticle2 = 8;
} // namespace engine_material

// a register of an evaluation, 0 for an index the registers do not hold (a guard: the
// engine reads its frame-allocated array unchecked, mirror.json M-PASS-GUARD-01)
float engineRegister(std::span<const float> registers, int index);

// idMaterial::IsDrawn (Material.h:548-551)
bool engineMaterialIsDrawn(const MaterialEntry& material);

/**
 * idMaterial::LightCastsShadows (Material.h:719-723): MF_FORCESHADOWS, or neither a fog,
 * an ambient nor a blend light and not MF_NOSHADOWS.
 */
bool engineLightCastsShadows(const MaterialEntry& material);

/**
 * RB_GetShaderTextureMatrix (RenderBackend.cpp:155-187): a stage's 2 × 3 texture matrix
 * from the registers, in a float[16] with the offsets wrapped at ±40 (the engine's test
 * of [3 * 4 + 0] reads matrix[12], the same element).
 */
EngineGlMatrix engineShaderTextureMatrix(
  std::span<const float> registers, const MaterialStage& stage);

/**
 * RB_LoadShaderTextureMatrix (RenderBackend.cpp:194-216): TEXTUREMATRIX_S and _T, the
 * identity rows without a matrix.
 */
std::array<Vec4, 2> engineLoadShaderTextureMatrix(
  std::span<const float> registers, const MaterialStage& stage);

/**
 * SetupInteractionStage (RenderBackend.cpp:1074-1124): the stage's matrix rows from its
 * texture-matrix registers with the offsets wrapped at ±40 through idMath::Ftoi, the
 * identity without a matrix; with lightColor, the colour clamp01( registers[color] ) ·
 * lightColor per component.
 */
void engineSetupInteractionStage(
  const MaterialStage& stage,
  std::span<const float> registers,
  const Vec4* lightColor,
  std::array<Vec4, 2>& matrix,
  Vec4* color);

// GL_Color (RenderBackend_NVRHI.cpp:2238-2246): each component clamped to 0..1
Vec4 engineClampedColor(const Vec4& color);

// RB_SetVertexColorParms (RenderBackend.cpp:129-146): (MODULATE, ADD) of a
// stageVertexColor_t
std::array<Vec4, 2> engineVertexColorParms(int vertexColor);

// ----- the depth prepass (SC216-SC219) -----

// what a prepass draw binds
enum class PrepassProgram
{
  // BUILTIN_DEPTH, CONSTANT_BUFFER_ONLY
  Depth,
  // BUILTIN_COLOR, CONSTANT_BUFFER_ONLY
  Color,
  // BUILTIN_TEXTURE_VERTEXCOLOR, TEXTURE
  TextureColor,
};

/**
 * One draw FillDepthBufferGeneric (RenderBackend.cpp:706-914) makes of a surface: the
 * program, the GL state bits, GL_PolygonOffset's (slope, bias) when POLYGON_OFFSET is
 * set, the colour after GL_Color's clamp, ALPHA_TEST, and for TextureColor the stage
 * whose image unit 0 binds, its texture matrix and TEXGEN_0_ENABLED, and whether its
 * texgen needs TG_SCREEN's planes.
 */
struct PrepassDraw
{
  PrepassProgram program = PrepassProgram::Depth;
  std::uint64_t state = 0;
  float slopeBias = 0.0f;
  float depthBias = 0.0f;
  // GL_Color's value, for the draws that call it (the solid depth draw does not: COLOR
  // keeps its last value)
  std::optional<Vec4> color;
  // ALPHA_TEST, for the alpha-tested stages
  std::optional<Vec4> alphaTest;
  // TextureColor: the stage index
  int stage = -1;
  std::array<Vec4, 2> textureMatrix = {};
  Vec4 texGenEnabled = {0.0f, 0.0f, 0.0f, 0.0f};
  bool screenTexgen = false;
};

/**
 * FillDepthBufferGeneric for one surface of a material whose registers are given:
 * nothing for a translucent material or one whose every stage is conditioned off; for a
 * perforated material one TextureColor draw per alpha-tested stage whose condition is on
 * and whose alpha register is above 0 (privatePolygonOffset per stage), and when no
 * alpha-tested stage was tried the solid draw: Color for SS_SUBVIEW (DST_COLOR / ZERO,
 * LESS, colour 1), Depth with GLS_ALPHAMASK otherwise; MF_POLYGONOFFSET adds
 * GLS_POLYGON_OFFSET with r_offsetFactor and r_offsetUnits · polygonOffset. A stage of a
 * texgen that binds another program in PrepareStageTexturing (the cube and sky texgens)
 * is not drawn (mirror.json M-PASS-DIFF-03).
 */
std::vector<PrepassDraw> engineGenericDepthDraws(
  const MaterialEntry& material, std::span<const float> registers);

// ----- the interactions (SC221-SC233) -----

// the program DrawSingleInteraction picks (K26: the unshadowed one always)
enum class InteractionProgram
{
  // BUILTIN_INTERACTION
  Interaction,
  // BUILTIN_PBR_INTERACTION
  PbrInteraction,
  // LP5.1 (C45, D114): BUILTIN_INTERACTION_AMBIENT, an ambientLight light's (its PBR
  // form, rows 25 and 26, is never reached: the specular is _black, TD_DEFAULT)
  InteractionAmbient,
};

// the textureUsage_t an image has once loaded (the PBR rename of LP1c, SC230's program
// choice); the key's own usage when not known yet
using ImageUsage = std::function<int(const MaterialImageKey&)>;

/**
 * drawInteraction_t (RenderBackend.cpp:1749-1750 `inter = {}`): what RenderInteractions
 * carries from stage to stage and from surface to surface within one call. The images
 * are null until a stage sets them; DrawSingleInteraction writes its fallbacks into
 * them; vertexColor and the matrices of a stage kind stay from the last stage that set
 * them, also from an earlier surface.
 */
struct InteractionState
{
  std::optional<MaterialImageKey> bumpImage;
  std::optional<MaterialImageKey> diffuseImage;
  std::optional<MaterialImageKey> specularImage;
  std::array<Vec4, 2> bumpMatrix = {};
  std::array<Vec4, 2> diffuseMatrix = {};
  std::array<Vec4, 2> specularMatrix = {};
  Vec4 diffuseColor = {0.0f, 0.0f, 0.0f, 0.0f};
  Vec4 specularColor = {0.0f, 0.0f, 0.0f, 0.0f};
  int vertexColor = engine_material::VertexColorIgnore;
  // LP5.1 (C45, SC513): din->vLight->lightShader->IsAmbientLight(); false in the ambient
  // pass (no vLight); the caller sets it for the light, the stage loop never changes it
  bool ambientLight = false;
};

// one draw of DrawSingleInteraction: the units 0-2 images and the parms it sets
struct InteractionDraw
{
  MaterialImageKey bumpImage;
  MaterialImageKey diffuseImage;
  MaterialImageKey specularImage;
  std::array<Vec4, 2> bumpMatrix = {};
  std::array<Vec4, 2> diffuseMatrix = {};
  std::array<Vec4, 2> specularMatrix = {};
  Vec4 diffuseColor = {0.0f, 0.0f, 0.0f, 0.0f};
  Vec4 specularColor = {0.0f, 0.0f, 0.0f, 0.0f};
  int vertexColor = engine_material::VertexColorIgnore;
  InteractionProgram program = InteractionProgram::Interaction;
};

/**
 * DrawSingleInteraction (RenderBackend.cpp:1131-1191, the program at :1395-1648) with
 * r_skipDiffuse, r_skipSpecular and r_skipBump 0: no bump image, nothing (the state
 * untouched); a null diffuse becomes _blackDiffuse and a null specular _black, and LP5.1
 * (C45, SC513, RenderBackend.cpp:1156-1159) the specular of an ambientLight light
 * (state.ambientLight) _black whatever its stage, written into the state as the engine
 * writes them into din; no draw when both are black (the image _black, or every rgb
 * component of the colour at most 0: for an ambient light the draw happens only when the
 * diffuse is not black, :1166-1173); else the draw, its program by the specular image's
 * usage (TD_SPECULAR_PBR_RMAO or _RMAOD: PbrInteraction), an ambient light's
 * InteractionAmbient (:1528-1537: its _black is TD_DEFAULT, so the PBR branch's rows 25
 * and 26, :1404-1413, are never reached; D114). Pure.
 */
std::optional<InteractionDraw> engineSingleInteraction(
  InteractionState& state, const ImageUsage& usage);

/**
 * RenderInteractions' surface stage loop (RenderBackend.cpp:2141-2235) for one surface
 * under one light stage whose diffuse and specular colours are given, with
 * r_skipInteractionFastPath 1: the images and colours cleared, then per stage
 * SL_COVERAGE and SL_AMBIENT ignored, SL_BUMP (its condition on) drawing a pending
 * interaction and starting a new one, SL_DIFFUSE and SL_SPECULAR (the condition on; the
 * specular not for a noSpecular light) drawing a pending one of their kind, and the
 * final draw. Each draw that DrawSingleInteraction makes is passed to draw, in order.
 */
void engineSurfaceInteractions(
  const MaterialEntry& material,
  std::span<const float> registers,
  const Vec4& diffuseColor,
  const Vec4& specularColor,
  bool noSpecular,
  InteractionState& state,
  const ImageUsage& usage,
  const std::function<void(const InteractionDraw&)>& draw);

/**
 * LP4.2a (C34, P4.19): AmbientPass' surface stage loop (RenderBackend.cpp:2538-2703,
 * r_skipInteractionFastPath 1) for one surface, the light colours white ×
 * r_lightScale: a new drawInteraction_t per surface (inter = {}: the images null, the
 * matrices zero, vertexColor SVC_IGNORE) with diffuseColor 1 and specularColor 0
 * (:2538-2542), then the loop of engineSurfaceInteractions (no light, so no
 * noSpecular), its SetupInteractionStage colours clamp01( reg ) × the light colour and
 * its flush rule; each draw DrawSingleInteraction makes (the no-bump and both-black skips
 * of :1135-1173: a material without a lit stage has no implicit _flat, Material.cpp:
 * 2491-2503) is passed to draw, in order, with the state's vertexColor at that moment
 * (set by the diffuse and specular stages, kept across a bump stage), which chooses the
 * draw's blend (ambientStateBits, :2600-2700).
 */
void engineAmbientSurfaceInteractions(
  const MaterialEntry& material,
  std::span<const float> registers,
  const Vec4& diffuseColor,
  const Vec4& specularColor,
  const ImageUsage& usage,
  const std::function<void(const InteractionDraw&)>& draw);

// the image keys of DrawSingleInteraction's fallbacks and of a pending surface
MaterialImageKey engineBlackImageKey();
MaterialImageKey engineBlackDiffuseImageKey();
MaterialImageKey engineFlatImageKey();
MaterialImageKey engineDefaultImageKey();
MaterialImageKey engineWhiteImageKey();

/**
 * D77, D52: the interaction of a surface whose material or registers are pending: the
 * bump _flat and the diffuse _default with identity matrices and the light's colours,
 * through DrawSingleInteraction (no specular: _black; LP5.1: ambientLight for an
 * ambientLight light's, so its program is InteractionAmbient).
 */
std::optional<InteractionDraw> pendingSurfaceInteraction(
  const Vec4& diffuseColor, const ImageUsage& usage, bool ambientLight = false);

/**
 * USER0, USER1 and USER2 of a light stage (RenderBackend.cpp:1934-1965): (mlMode, type,
 * falloff, pattern), (falloffExp, innerRadius, shapeExp, penumbra), (patternStrength,
 * patternScale, patternPhase, energyClamp); mlMode 0 for a texture light, 1 +
 * r_showMathLights (EngineFrame.showMathLights, 0) for a math light (mathPath: LP5.1,
 * engineLightUsesMathPath, :1937). The blob evaluates the curve, mask and pattern from
 * them and clamps and linearises the result (mathlight.inc.hlsl MLEvaluate, then
 * sRGBToLinearRGB at interaction.ps.hlsl:179): nothing of it is the preview's. Pure.
 */
std::array<Vec4, 3> engineMathLightParms(const MathLightParms& light, bool mathPath);

/**
 * LP5.1 (C45, D112): what a light stage binds at the falloff and projection units 3 and 4
 * (RenderBackend.cpp:1914-1931): _white at both for a math light (it samples neither, and
 * its stage image is never read, so a stage without one is drawn), else the light's
 * falloff image and the stage's image (none for a stage without one: a cinematic's, which
 * the preview does not draw, mirror.json M-PASS-GUARD-01). Pure.
 */
std::optional<std::array<MaterialImageKey, 2>> engineLightStageUnits(
  bool mathPath,
  const std::optional<MaterialImageKey>& falloffImage,
  const MaterialStage& lightStage);

} // namespace tb::lit
