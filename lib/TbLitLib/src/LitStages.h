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
#include <cstddef>
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
// LP5.3 (C49, D123): MF_NOPORTALFOG (:352), the material keyword noPortalFog
constexpr int FlagNoPortalFog = 1 << 5;
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
  // LP5.2b (C48, SC528): the program PrepareStageTexturing binds over texture_color for
  // an alpha-tested stage of a cube or sky texgen (none of them clips):
  // BUILTIN_ENVIRONMENT (55), DEFAULT: TG_REFLECT_CUBE without a bump stage
  Environment,
  // BUILTIN_BUMPY_ENVIRONMENT (57), NORMAL_CUBE: TG_REFLECT_CUBE with one
  BumpyEnvironment,
  // BUILTIN_SKYBOX (69), LEGACY: TG_SKYBOX_CUBE
  SkyBox,
  // BUILTIN_WOBBLESKY (70), LEGACY: TG_WOBBLESKY_CUBE
  WobbleSky,
};

/**
 * One draw FillDepthBufferGeneric (RenderBackend.cpp:706-914) makes of a surface: the
 * program, the GL state bits, GL_PolygonOffset's (slope, bias) when POLYGON_OFFSET is
 * set, the colour after GL_Color's clamp, ALPHA_TEST, and for an alpha-tested stage's
 * draw the stage whose image unit 0 binds, its texture matrix and TEXGEN_0_ENABLED,
 * whether its texgen needs TG_SCREEN's planes, and (LP5.2b) BumpyEnvironment's bump stage
 * (unit 1) and WobbleSky's WOBBLESKY_X..Z.
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
  // an alpha-tested stage's draw: the stage index
  int stage = -1;
  std::array<Vec4, 2> textureMatrix = {};
  Vec4 texGenEnabled = {0.0f, 0.0f, 0.0f, 0.0f};
  bool screenTexgen = false;
  // LP5.2b: BumpyEnvironment's bump stage (GetBumpStage), -1 otherwise
  int bumpStage = -1;
  // LP5.2b: WobbleSky's WOBBLESKY_X..Z
  std::array<Vec4, 3> wobbleSky = {};
};

/**
 * idMaterial::GetBumpStage (Material.cpp:3796-3806): the first SL_BUMP stage, -1 when
 * there is none. Pure.
 */
int engineBumpStage(const MaterialEntry& material);

/**
 * LP5.2b (C48, K5, D119): a TG_WOBBLESKY_CUBE stage's WOBBLESKY_X..Z
 * (RenderBackend.cpp:530-565): engineWobbleSkyTransform of the registers at the
 * material's three texGenRegisters (the tilt in degrees, the wobble and rotate rates per
 * minute) at the time timeMs: the preview's material time is 0 (K5), and the engine's
 * renderView.time[0] has no renderparm of its own (it reaches the shader only through
 * this transform), so the stage pass and the prepass pass 0. Pure.
 */
std::array<Vec4, 3> engineStageWobbleSky(
  const MaterialEntry& material, std::span<const float> registers, float timeMs = 0.0f);

/**
 * FillDepthBufferGeneric for one surface of a material whose registers are given:
 * nothing for a translucent material or one whose every stage is conditioned off; for a
 * perforated material one TextureColor draw per alpha-tested stage whose condition is on
 * and whose alpha register is above 0 (privatePolygonOffset per stage), and when no
 * alpha-tested stage was tried the solid draw: Color for SS_SUBVIEW (DST_COLOR / ZERO,
 * LESS, colour 1), Depth with GLS_ALPHAMASK otherwise; MF_POLYGONOFFSET adds
 * GLS_POLYGON_OFFSET with r_offsetFactor and r_offsetUnits · polygonOffset. LP5.2b (C48,
 * SC528, :783-866): an alpha-tested stage's draw takes texture_color, GL_Color( 0, 0, 0,
 * a ) and ALPHA_TEST first, then PrepareStageTexturing replaces the program for the cube
 * and sky texgens: TG_REFLECT_CUBE Environment, or BumpyEnvironment with the material's
 * bump stage at unit 1; TG_SKYBOX_CUBE SkyBox; TG_WOBBLESKY_CUBE WobbleSky with its
 * transform at time 0 (K5). None of them clips (ALPHA_TEST is written and never read), so
 * the stage writes the whole surface's depth, and its colour: (0, 0, 0) through rpColor
 * for the first three, the sky's texel for WobbleSky (its colour the vertex colour, (1,
 * 1, 1) under SVC_IGNORE). A TG_REFLECT_CUBE2 stage is not drawn, its stage tried
 * (programs 59-62 with a bump stage, LP6's inputs; texture_color sampling a cube as a
 * Texture2D without one: mirror.json M-STAGE-DIFF-05).
 */
std::vector<PrepassDraw> engineGenericDepthDraws(
  const MaterialEntry& material, std::span<const float> registers);

// ----- LP3.4: the perforated shadow pass (SC723) -----

/**
 * LP3.4 (D155, K52, E-LIT-12): what one call of ShadowMapPassPerforated
 * (RenderBackend.cpp:3171-3388, one light's side) carries from one perforated surface to
 * the next: the bits of its last GL_State and its last GL_PolygonOffset's (slope, bias),
 * under which a surface's !didDraw fallback draws (:3372-3384).
 */
struct PerforatedShadowState
{
  std::uint64_t state = 0;
  float slopeBias = 0.0f;
  float depthBias = 0.0f;

  bool operator==(const PerforatedShadowState&) const = default;
};

/**
 * LP3.4 (D155): the state a call of ShadowMapPassPerforated opens with (:3191-3222), as
 * ShadowMapPassFast's does (:3418-3449) for the opaque casters: GL_State(
 * GLS_POLYGON_OFFSET ), then r_shadowMapOccluderFacing's cull (2 TWOSIDED, 0 FRONTSIDED,
 * 1 BACKSIDED) with GL_PolygonOffset( r_shadowMapPolygonFactor,
 * r_vkShadowMapPolygonOffset
 * ), negated for 1 (D159: the Vulkan offset). Pure.
 */
PerforatedShadowState enginePerforatedShadowOpening();

/**
 * LP3.4 (D155, K52, E-LIT-12; SC723, SC737): the draws ShadowMapPassPerforated makes of
 * one perforated surface (:3280-3384) of a material whose registers are given, carried
 * the state its side's call left, updated as the engine's GL_State and GL_PolygonOffset
 * calls leave it. The surface state is 0 (no shadow polygon offset, FRONTSIDED, LESS,
 * depth written: E-LIT-12), MF_POLYGONOFFSET adding GLS_POLYGON_OFFSET with
 * GL_PolygonOffset( r_offsetFactor, r_offsetUnits · polygonOffset ) set at the surface's
 * start; per stage, hasAlphaTest alone: a condition register of 0 skips it; once one is
 * tried didDraw is set; the colour is ( 0, 0, 0, the alpha register ), an alpha ≤ 0
 * skipping the stage's draw; privatePolygonOffset sets the offset ( r_offsetFactor,
 * r_offsetUnits · it ) and the bit for the stage, then the surface's offset again; one
 * TextureColor draw (BUILTIN_ TEXTURE_VERTEXCOLOR 15, never the skinned 17: P3.24e;
 * SVC_IGNORE; unit 0 the stage's image) with ALPHA_TEST and PrepareStageTexturing's
 * texture matrix and texgen, whose cube and sky texgens replace the program as in the
 * prepass (LP5.2b's rule: none of those programs clips, so the stage writes the whole
 * surface's depth) and whose TG_REFLECT_ CUBE2 stage is not drawn (M-STAGE-DIFF-05); when
 * no alpha-tested stage was tried, one Depth draw (BUILTIN_DEPTH) under carried: the
 * previous surface's state, in the order the caller draws its surfaces (K52;
 * enginePerforatedShadowOpening() for the first). A draw's slope and bias are what its
 * pipeline key reads: the offset in force under GLS_POLYGON_OFFSET, else 0. Pure.
 */
std::vector<PrepassDraw> enginePerforatedShadowDraws(
  const MaterialEntry& material,
  std::span<const float> registers,
  PerforatedShadowState& carried);

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

// LP6.1 (C64): the fields of InteractionState that carry from surface to surface
// (InteractionState::written, InteractionDraw::inherited)
inline constexpr std::uint8_t CarriedBumpMatrix = 1;
inline constexpr std::uint8_t CarriedDiffuseMatrix = 2;
inline constexpr std::uint8_t CarriedSpecularMatrix = 4;
inline constexpr std::uint8_t CarriedVertexColor = 8;
inline constexpr std::uint8_t CarriedFields = 15;

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
  // LP6.1 (C64): the carried fields (Carried*) the current surface's stage loop has set,
  // cleared with its images at the surface's start (engineSurfaceInteractions)
  std::uint8_t written = 0;
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
  // LP6.1 (C64): the carried fields (Carried*) this draw read from the state an earlier
  // surface (or the call's start) left: none the surface's own stage loop had set by then
  std::uint8_t inherited = 0;
};

/**
 * LP6.1 (C64): DrawSingleInteraction's program (RenderBackend.cpp:1395-1648) by the
 * specular image's usage once loaded: an ambientLight light's InteractionAmbient, else
 * PbrInteraction for TD_SPECULAR_PBR_RMAO and _RMAOD, else Interaction (K26: the
 * unshadowed ones; engineInteractionProgram picks the shadow-atlas forms). Pure.
 */
InteractionProgram engineInteractionDrawProgram(int specularUsage, bool ambientLight);

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

// ----- the stage pass (LP5.2a, C46, C47, SC519-SC523) -----

// materialSort_t SS_POST_PROCESS (Material.h:319): the first stage call stops at it, the
// second (LP6.5b, C71) draws from it over the frame's _currentRender copy
inline constexpr float EngineSortPostProcess = 100.0f;

// cullType_t (Material.h:329-334)
namespace engine_cull
{
constexpr int FrontSided = 0;
constexpr int BackSided = 1;
constexpr int TwoSided = 2;
} // namespace engine_cull

// what a stage-pass draw binds
enum class StageProgram
{
  // BUILTIN_TEXTURE_VERTEXCOLOR (15), TEXTURE
  TextureColor,
  // BUILTIN_TEXTURE_TEXGEN_VERTEXCOLOR (18), TEXGEN: TG_SCREEN and TG_SCREEN2
  TextureColorTexgen,
  // a newStage block's program (C47, D121), POST_PROCESS_INGAME: the material-program
  // table's row of its fragmentProgram
  Material,
  // LP5.2b (C48): PrepareStageTexturing's programs over texture_color (RenderBackend.cpp:
  // 360-392, 526-586): BUILTIN_ENVIRONMENT (55), DEFAULT: TG_REFLECT_CUBE without a bump
  // stage
  Environment,
  // BUILTIN_BUMPY_ENVIRONMENT (57), NORMAL_CUBE: TG_REFLECT_CUBE with one, its image raw
  // at unit 1
  BumpyEnvironment,
  // BUILTIN_SKYBOX (69), LEGACY: TG_SKYBOX_CUBE
  SkyBox,
  // BUILTIN_WOBBLESKY (70), LEGACY: TG_WOBBLESKY_CUBE, WOBBLESKY_X..Z
  WobbleSky,
  // LP6.5b (C72, D180): BUILTIN_BUMPY_ENVIRONMENT2 (59), OCTAHEDRON_CUBE:
  // TG_REFLECT_CUBE2 with a bump stage (RenderBackend.cpp:393-524 at r_useSSR 0): unit 0
  // the bump stage's image (the stage's own cube never sampled), the G-buffer normals and
  // the view's radiance images
  BumpyEnvironment2,
};

// whether a stage-pass or prepass program samples a TextureCube at t0 (55, 57, 69, 70;
// not 59, whose unit 0 is the bump image)
bool engineStageProgramSamplesCube(StageProgram program);

/**
 * LP6.5b (C72, D176): whether a material has a stage engineShaderPassDraws draws with
 * bumpyenvironment2: an SL_AMBIENT old-style stage of TG_REFLECT_CUBE2 and a bump stage
 * (GetBumpStage) in the material; the lists count such surfaces, pass 3's consumer (its
 * normals at t2). The stage's registers are not read (a stage conditioned off still
 * counts: pass 3 then runs for nothing, no picture differs, K55). Pure.
 */
bool engineHasBumpyEnvironment2Stage(const MaterialEntry& material);

/**
 * One draw DrawShaderPasses makes of a surface's stage (RenderBackend.cpp:4434-4702): the
 * stage, the program, the GL state bits (the surface's cull and polygon offset, the
 * stage's drawStateBits and its stencil bits), GL_PolygonOffset's (slope, bias) when
 * POLYGON_OFFSET is set (the material's, or the stage's privatePolygonOffset), and for an
 * old-style stage GL_Color's clamped colour, RB_SetVertexColorParms' MODULATE and ADD,
 * PrepareStageTexturing's texture matrix and whether TG_SCREEN's planes are loaded
 * (TEXGEN_0_ENABLED 1); for a newStage block USER0 + j of each vertexParm j through the
 * registers (no colour, vertex colour or texture matrix is set for it, :4509-4570).
 * LP5.2b (C48): for BumpyEnvironment the bump stage whose image unit 1 binds (LP6.5b:
 * for BumpyEnvironment2 the one unit 0 binds), for
 * WobbleSky its WOBBLESKY_X..Z at time 0 (K5), and whether PrepareStageTexturing warned
 * for the stage (TG_DIFFUSE_CUBE, TG_GLASSWARP, :625-635: drawn on texture_color).
 */
struct StagePassDraw
{
  int stage = -1;
  StageProgram program = StageProgram::TextureColor;
  std::uint64_t state = 0;
  float slopeBias = 0.0f;
  float depthBias = 0.0f;
  Vec4 color = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<Vec4, 2> vertexColor = {};
  std::array<Vec4, 2> textureMatrix = {};
  bool screenTexgen = false;
  std::vector<Vec4> userParms;
  int bumpStage = -1;
  std::array<Vec4, 3> wobbleSky = {};
  bool texgenWarning = false;
};

/**
 * What the stage loop of one surface leaves: its draws in stage order and the
 * SL_AMBIENT stages it did not draw (skipped: every reason summed; of them those of
 * TG_REFLECT_CUBE2 without a bump stage (texgenLp6), and those of a cinematic, D120).
 */
struct StagePassSurface
{
  std::vector<StagePassDraw> draws;
  std::size_t skipped = 0;
  std::size_t texgenLp6 = 0;
  std::size_t cinematic = 0;
};

/**
 * DrawShaderPasses for one surface (RenderBackend.cpp:4396-4702) of a material whose
 * registers are given, the surface not a GUI surface (the preview has none: D120) and
 * extraGLState 0: the cull of cullType (CT_TWO_SIDED none, CT_BACK_SIDED
 * GLS_CULL_BACKSIDED, which the copied GetRenderState decodes as the front-sided case,
 * GLState.h:74-77 and PipelineCache.cpp's switch: M-STAGE-DEFECT-01, mirrored, not fixed,
 * D117; else front-sided), MF_POLYGONOFFSET's GL_PolygonOffset( r_offsetFactor,
 * r_offsetUnits · polygonOffset ); per stage: skipped when its condition register is 0
 * (counted only for an SL_AMBIENT stage), not drawn at all when it is not SL_AMBIENT,
 * stageGLState = the surface's | drawStateBits (no GLS_OVERRIDE here), skipped for ZERO
 * / ONE, the stencil stage's comp ALWAYS or EQUAL, pass REPLACE, KEEP or DECRWRAP, zFail
 * DECRWRAP or KEEP, fail KEEP and GLS_STENCIL_MAKE_REF( ref ) | GLS_STENCIL_MAKE_MASK(
 * writeMask ) as the engine maps them (the other values add no bits); a newStage block
 * (unless r_skipNewAmbient) one Material draw with its vertexParms; an old-style stage
 * skipped for ONE / ONE with rgb at most 0 and SRC_ALPHA / ONE_MINUS_SRC_ALPHA with
 * alpha at most 0 and a cinematic (D120), else one draw with the stage's
 * privatePolygonOffset, its program by the texgen (LP5.2b, C48, PrepareStageTexturing
 * :352-638 after the program choice of :4604-4671): TG_SCREEN and TG_SCREEN2 on 18,
 * TG_REFLECT_CUBE on 55 or, with a bump stage (GetBumpStage), on 57, TG_SKYBOX_CUBE on
 * 69, TG_WOBBLESKY_CUBE on 70 with its transform at time 0 (K5); TG_REFLECT_CUBE2 with a
 * bump stage on 59 (LP6.5b, C72, D180: the bump stage's index carried for unit 0),
 * without one not drawn (texture_color would sample a cube as a Texture2D: counted in
 * texgenLp6, mirror.json M-STAGE-DIFF-05, narrowed);
 * TG_DIFFUSE_CUBE and TG_GLASSWARP, which only warn there, on 15 as TG_EXPLICIT is,
 * texgenWarning set. No ALPHA_TEST is set (SC520). Pure.
 */
StagePassSurface engineShaderPassDraws(
  const MaterialEntry& material, std::span<const float> registers);

// ----- pass 9, the fog and blend lights (LP5.3, C49, D122, SC530-SC538) -----

// DEFAULT_FOG_DISTANCE and FOG_ENTER (RenderCommon.h:50-55): 500, and ( 64 + 1 ) / 128
inline constexpr float EngineDefaultFogDistance = 500.0f;
inline constexpr float EngineFogEnter = (64.0f + 1.0f) / (64 * 2);

/**
 * FogPass' density (RenderBackend.cpp:4959-4969) as PortalIsFoggedOut computes it too
 * (RenderWorld_portals.cpp:476-486): -0.5 / DEFAULT_FOG_DISTANCE when the stage's alpha
 * (the light's shaderParm3 through the fog material's parm3, LT:177-181) is at most 1,
 * else -0.5 / alpha. Pure.
 */
float engineFogDensity(float alpha);

/**
 * What FogPass sets before its draws (RenderBackend.cpp:4940-5008): stage 0's colour
 * registers without r_lightScale (the alpha unclamped for the density, GL_Color's clamp
 * in color: an alpha of 2000 becomes 1, 0.5 stays 0.5), the density a, and the four
 * planes: p0 = a · the world's modelViewMatrix column 2 with w + 0.5 (the fade by view
 * depth, read at S of _fog), p1 = ( 0, 0, 0, 0.5 ) (T of _fog), p2 = 0.001 · fogPlane
 * with w + FOG_ENTER (T of _fogEnter: the light's far plane), p3 = ( 0, 0, 0, 0.001 · s +
 * FOG_ENTER ) with s = fogPlane.Distance( vieworg ) (S of _fogEnter); T_BasicFog sets
 * them as TEXGEN_0_S = p0, TEXGEN_0_T = p1, TEXGEN_1_T = p2, TEXGEN_1_S = p3 (the swap of
 * SC534). nullopt for a fog shader without a stage, whose stage 0 the engine reads
 * anyway (out of its list: a guard). Pure.
 */
struct EngineFogParms
{
  Vec4 lightColor = {};
  Vec4 color = {};
  float a = 0.0f;
  std::array<EnginePlane, 4> planes = {};
};
std::optional<EngineFogParms> engineFogParms(
  const MaterialEntry& shader,
  std::span<const float> registers,
  const EnginePlane& fogPlane,
  const EngineGlMatrix& worldModelViewMatrix,
  const EngineVec3& viewOrigin);

/**
 * R_MakeZeroOneCubeTris (RenderSystem_init.cpp:2035-2120, transcribed): the zero-to-one
 * cube FogPass draws through inverseBaseLightProject (zeroOneCubeSurface, :2443-2447):
 * eight corners (x, y, z each 0 or 1; vertex i has x of bit i % 4 in 1, 2 and y of 2, 3,
 * z of i >= 4) and 36 indexes, two triangles per face in the engine's winding (bottom,
 * back, left, right, front, top), the first four vertices' colour 0xffffffff, the
 * others 0 (R_DeriveTangents' normals and tangents, which the fog shaders never read,
 * are left 0). Pure.
 */
std::array<EngineVec3, 8> engineZeroOneCubeVertices();
std::array<std::uint16_t, 36> engineZeroOneCubeIndexes();

} // namespace tb::lit
