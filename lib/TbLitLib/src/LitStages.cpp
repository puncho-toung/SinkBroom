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

#include "LitStages.h"

#include "ImageSlots.h"
#include "lit/EngineGeometry.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/ImageCache.h"

namespace tb::lit
{
namespace
{

namespace em = engine_material;

// idLib's Min and Max (Lib.h), and idMath::ClampFloat( min, max, value ) =
// Max( min, Min( max, value ) ) (Math.h:1518-1521)
float clampFloat(const float min, const float max, const float value)
{
  const auto lower = (max < value) ? max : value;
  return (min > lower) ? min : lower;
}

bool isBlackImage(const MaterialImageKey& key)
{
  return key == engineBlackImageKey();
}

} // namespace

float engineRegister(const std::span<const float> registers, const int index)
{
  return index >= 0 && std::size_t(index) < registers.size()
           ? registers[std::size_t(index)]
           : 0.0f;
}

bool engineMaterialIsDrawn(const MaterialEntry& material)
{
  // numStages > 0 || entityGui != 0 || gui != NULL
  return !material.stages.empty() || material.entityGui != 0 || !material.gui.empty();
}

bool engineLightCastsShadows(const MaterialEntry& material)
{
  return (material.materialFlags & em::FlagForceShadows) != 0
         || (!material.fogLight && !material.ambientLight && !material.blendLight
             && (material.materialFlags & em::FlagNoShadows) == 0);
}

EngineGlMatrix engineShaderTextureMatrix(
  const std::span<const float> registers, const MaterialStage& stage)
{
  const auto& m = stage.textureMatrixRegisters;
  auto matrix = EngineGlMatrix{};
  matrix[0 * 4 + 0] = engineRegister(registers, m[0][0]);
  matrix[1 * 4 + 0] = engineRegister(registers, m[0][1]);
  matrix[2 * 4 + 0] = 0.0f;
  matrix[3 * 4 + 0] = engineRegister(registers, m[0][2]);

  matrix[0 * 4 + 1] = engineRegister(registers, m[1][0]);
  matrix[1 * 4 + 1] = engineRegister(registers, m[1][1]);
  matrix[2 * 4 + 1] = 0.0f;
  matrix[3 * 4 + 1] = engineRegister(registers, m[1][2]);

  // we attempt to keep scrolls from generating incredibly large texture values, but
  // center rotations and center scales can still generate offsets that need to be > 1
  if (matrix[3 * 4 + 0] < -40.0f || matrix[12] > 40.0f)
  {
    matrix[3 * 4 + 0] -= float(int(matrix[3 * 4 + 0]));
  }
  if (matrix[13] < -40.0f || matrix[13] > 40.0f)
  {
    matrix[13] -= float(int(matrix[13]));
  }

  matrix[0 * 4 + 2] = 0.0f;
  matrix[1 * 4 + 2] = 0.0f;
  matrix[2 * 4 + 2] = 1.0f;
  matrix[3 * 4 + 2] = 0.0f;

  matrix[0 * 4 + 3] = 0.0f;
  matrix[1 * 4 + 3] = 0.0f;
  matrix[2 * 4 + 3] = 0.0f;
  matrix[3 * 4 + 3] = 1.0f;
  return matrix;
}

std::array<Vec4, 2> engineLoadShaderTextureMatrix(
  const std::span<const float> registers, const MaterialStage& stage)
{
  auto texS = Vec4{1.0f, 0.0f, 0.0f, 0.0f};
  auto texT = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  if (stage.hasTextureMatrix)
  {
    const auto matrix = engineShaderTextureMatrix(registers, stage);
    texS =
      Vec4{matrix[0 * 4 + 0], matrix[1 * 4 + 0], matrix[2 * 4 + 0], matrix[3 * 4 + 0]};
    texT =
      Vec4{matrix[0 * 4 + 1], matrix[1 * 4 + 1], matrix[2 * 4 + 1], matrix[3 * 4 + 1]};
  }
  return {texS, texT};
}

void engineSetupInteractionStage(
  const MaterialStage& stage,
  const std::span<const float> registers,
  const Vec4* lightColor,
  std::array<Vec4, 2>& matrix,
  Vec4* color)
{
  if (stage.hasTextureMatrix)
  {
    const auto& m = stage.textureMatrixRegisters;
    matrix[0][0] = engineRegister(registers, m[0][0]);
    matrix[0][1] = engineRegister(registers, m[0][1]);
    matrix[0][2] = 0.0f;
    matrix[0][3] = engineRegister(registers, m[0][2]);

    matrix[1][0] = engineRegister(registers, m[1][0]);
    matrix[1][1] = engineRegister(registers, m[1][1]);
    matrix[1][2] = 0.0f;
    matrix[1][3] = engineRegister(registers, m[1][2]);

    // we attempt to keep scrolls from generating incredibly large texture values, but
    // center rotations and center scales can still generate offsets that need to be > 1
    if (matrix[0][3] < -40.0f || matrix[0][3] > 40.0f)
    {
      matrix[0][3] -= float(engineFtoi(matrix[0][3]));
    }
    if (matrix[1][3] < -40.0f || matrix[1][3] > 40.0f)
    {
      matrix[1][3] -= float(engineFtoi(matrix[1][3]));
    }
  }
  else
  {
    matrix[0] = Vec4{1.0f, 0.0f, 0.0f, 0.0f};
    matrix[1] = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  }

  if (color && lightColor)
  {
    for (std::size_t i = 0; i < 4; ++i)
    {
      // clamp here, so cards with a greater range don't look different
      (*color)[i] =
        clampFloat(0.0f, 1.0f, engineRegister(registers, stage.colorRegisters[i]))
        * (*lightColor)[i];
    }
  }
}

Vec4 engineClampedColor(const Vec4& color)
{
  return Vec4{
    clampFloat(0.0f, 1.0f, color[0]),
    clampFloat(0.0f, 1.0f, color[1]),
    clampFloat(0.0f, 1.0f, color[2]),
    clampFloat(0.0f, 1.0f, color[3])};
}

std::array<Vec4, 2> engineVertexColorParms(const int vertexColor)
{
  const auto zero = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  const auto one = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
  const auto negOne = Vec4{-1.0f, -1.0f, -1.0f, -1.0f};
  switch (vertexColor)
  {
  case em::VertexColorModulate:
    return {one, zero};
  case em::VertexColorInverseModulate:
    return {negOne, one};
  case em::VertexColorIgnore:
  default:
    // an unknown value sets nothing in the engine; SVC_IGNORE is the parser's only
    // other value
    return {zero, one};
  }
}

int engineBumpStage(const MaterialEntry& material)
{
  for (std::size_t i = 0; i < material.stages.size(); ++i)
  {
    if (material.stages[i].lighting == em::LightingBump)
    {
      return int(i);
    }
  }
  return -1;
}

std::array<Vec4, 3> engineStageWobbleSky(
  const MaterialEntry& material,
  const std::span<const float> registers,
  const float timeMs)
{
  // const int* parms = surf->material->GetTexGenRegisters(); shaderRegisters[ parms[i] ]
  const auto& parms = material.texGenRegisters;
  const auto rows = engineWobbleSkyTransform(
    engineRegister(registers, parms[0]),
    engineRegister(registers, parms[1]),
    engineRegister(registers, parms[2]),
    timeMs);
  return {
    Vec4{rows[0][0], rows[0][1], rows[0][2], rows[0][3]},
    Vec4{rows[1][0], rows[1][1], rows[1][2], rows[1][3]},
    Vec4{rows[2][0], rows[2][1], rows[2][2], rows[2][3]}};
}

bool engineStageProgramSamplesCube(const StageProgram program)
{
  switch (program)
  {
  case StageProgram::Environment:
  case StageProgram::BumpyEnvironment:
  case StageProgram::SkyBox:
  case StageProgram::WobbleSky:
    return true;
  case StageProgram::TextureColor:
  case StageProgram::TextureColorTexgen:
  case StageProgram::Material:
    return false;
  }
  return false;
}

std::vector<PrepassDraw> engineGenericDepthDraws(
  const MaterialEntry& material, const std::span<const float> registers)
{
  auto draws = std::vector<PrepassDraw>{};

  // translucent surfaces don't put anything in the depth buffer and don't test against
  // it
  if (material.coverage == em::CoverageTranslucent)
  {
    return draws;
  }

  // if all stages of a material have been conditioned off, don't do anything
  auto anyOn = false;
  for (const auto& stage : material.stages)
  {
    if (engineRegister(registers, stage.conditionRegister) != 0.0f)
    {
      anyOn = true;
      break;
    }
  }
  if (!anyOn)
  {
    return draws;
  }

  auto surfState = std::uint64_t{0};
  auto slope = 0.0f;
  auto bias = 0.0f;

  // set polygon offset if necessary
  if (material.materialFlags & em::FlagPolygonOffset)
  {
    surfState |= engine_gls::PolygonOffset;
    slope = EngineFrame.offsetFactor;
    bias = EngineFrame.offsetUnits * material.polygonOffset;
  }

  // subviews will just down-modulate the color buffer
  auto color = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
  const auto subview = material.sort == em::SortSubview;
  if (subview)
  {
    surfState |=
      engine_gls::SrcBlendDstColor | engine_gls::DstBlendZero | engine_gls::DepthFuncLess;
    color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
  }

  auto drawSolid = false;
  if (material.coverage == em::CoverageOpaque)
  {
    drawSolid = true;
  }
  else if (material.coverage == em::CoveragePerforated)
  {
    // perforated surfaces may have multiple alpha tested stages
    auto didDraw = false;
    for (std::size_t s = 0; s < material.stages.size(); ++s)
    {
      const auto& stage = material.stages[s];
      if (!stage.hasAlphaTest)
      {
        continue;
      }
      // check the stage enable condition
      if (engineRegister(registers, stage.conditionRegister) == 0.0f)
      {
        continue;
      }

      // if we at least tried to draw an alpha tested stage, we won't draw the opaque
      // surface
      didDraw = true;

      // set the alpha modulate
      color[3] = engineRegister(registers, stage.colorRegisters[3]);

      // skip the entire stage if alpha would be black
      if (color[3] <= 0.0f)
      {
        continue;
      }

      auto draw = PrepassDraw{};
      draw.program = PrepassProgram::TextureColor;
      draw.state = surfState;
      draw.slopeBias = slope;
      draw.depthBias = bias;

      // set privatePolygonOffset if necessary
      if (stage.privatePolygonOffset != 0.0f)
      {
        draw.slopeBias = EngineFrame.offsetFactor;
        draw.depthBias = EngineFrame.offsetUnits * stage.privatePolygonOffset;
        draw.state |= engine_gls::PolygonOffset;
      }

      draw.color = engineClampedColor(color);
      const auto alpha = engineRegister(registers, stage.alphaTestRegister);
      draw.alphaTest = Vec4{alpha, alpha, alpha, alpha};
      draw.stage = int(s);

      // PrepareStageTexturing (RenderBackend.cpp:352-638): the texture matrix and the
      // texgens
      draw.textureMatrix = engineLoadShaderTextureMatrix(registers, stage);
      switch (stage.texgen)
      {
      case em::TexgenScreen:
      case em::TexgenScreen2:
        draw.texGenEnabled = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        draw.screenTexgen = true;
        break;
      case em::TexgenReflectCube:
        // LP5.2b (C48, SC528): the program replaced after ALPHA_TEST was set (:360-392):
        // environment, or bumpyenvironment with the bump stage's image at unit 1
        draw.bumpStage = engineBumpStage(material);
        draw.program = draw.bumpStage >= 0 ? PrepassProgram::BumpyEnvironment
                                           : PrepassProgram::Environment;
        break;
      case em::TexgenReflectCube2:
        // :393-525: BUMPY_ENVIRONMENT2 (LP6's inputs) with a bump stage, texture_color
        // sampling the cube as a Texture2D without one: not drawn (M-STAGE-DIFF-05)
        continue;
      case em::TexgenSkyboxCube:
        // :526-529
        draw.program = PrepassProgram::SkyBox;
        break;
      case em::TexgenWobbleskyCube:
        // :530-586, at the preview's material time 0 (K5)
        draw.program = PrepassProgram::WobbleSky;
        draw.wobbleSky = engineStageWobbleSky(material, registers);
        break;
      default:
        break;
      }
      draws.push_back(draw);
    }

    if (!didDraw)
    {
      drawSolid = true;
    }
  }

  // draw the entire surface solid
  if (drawSolid)
  {
    auto draw = PrepassDraw{};
    draw.slopeBias = slope;
    draw.depthBias = bias;
    if (subview)
    {
      draw.program = PrepassProgram::Color;
      draw.color = engineClampedColor(color);
      draw.state = surfState;
    }
    else
    {
      // no GL_Color: COLOR keeps its last value
      draw.program = PrepassProgram::Depth;
      draw.state = surfState | engine_gls::AlphaMask;
    }
    draws.push_back(draw);
  }
  return draws;
}

MaterialImageKey engineBlackImageKey()
{
  return intrinsicImageKey("_black");
}

MaterialImageKey engineBlackDiffuseImageKey()
{
  return intrinsicImageKey("_blackDiffuse");
}

MaterialImageKey engineFlatImageKey()
{
  return intrinsicImageKey("_flat");
}

MaterialImageKey engineDefaultImageKey()
{
  return intrinsicImageKey("_default");
}

MaterialImageKey engineWhiteImageKey()
{
  return intrinsicImageKey("_white");
}

std::optional<InteractionDraw> engineSingleInteraction(
  InteractionState& state, const ImageUsage& usage)
{
  if (!state.bumpImage)
  {
    // stage wasn't actually an interaction
    return std::nullopt;
  }

  if (!state.diffuseImage)
  {
    state.diffuseImage = engineBlackDiffuseImageKey();
  }
  // LP5.1 (C45, SC513): an ambientLight light's specular is _black whatever its stage
  if (!state.specularImage || state.ambientLight)
  {
    state.specularImage = engineBlackImageKey();
  }

  // if we wouldn't draw anything, don't call the Draw function
  const auto diffuseIsBlack =
    isBlackImage(*state.diffuseImage)
    || (state.diffuseColor[0] <= 0 && state.diffuseColor[1] <= 0 && state.diffuseColor[2] <= 0);
  const auto specularIsBlack =
    isBlackImage(*state.specularImage)
    || (state.specularColor[0] <= 0 && state.specularColor[1] <= 0 && state.specularColor[2] <= 0);
  if (diffuseIsBlack && specularIsBlack)
  {
    return std::nullopt;
  }

  auto draw = InteractionDraw{};
  draw.bumpImage = *state.bumpImage;
  draw.diffuseImage = *state.diffuseImage;
  draw.specularImage = *state.specularImage;
  draw.bumpMatrix = state.bumpMatrix;
  draw.diffuseMatrix = state.diffuseMatrix;
  draw.specularMatrix = state.specularMatrix;
  draw.diffuseColor = state.diffuseColor;
  draw.specularColor = state.specularColor;
  draw.vertexColor = state.vertexColor;

  // the program by the specular image's usage; K26: the unshadowed one. LP5.1 (D114):
  // an ambient light's specular is _black, TD_DEFAULT, so it takes the legacy branch's
  // BindShader_InteractionAmbient (RenderBackend.cpp:1528-1537); the PBR branch's
  // BindShader_PBR_InteractionAmbient (:1404-1413) is never reached
  const auto specUsage = usage ? usage(draw.specularImage) : draw.specularImage.usage;
  const auto pbr = specUsage == engine_image::UsageSpecularPbrRmao
                   || specUsage == engine_image::UsageSpecularPbrRmao + 1;
  draw.program = state.ambientLight ? InteractionProgram::InteractionAmbient
                 : pbr              ? InteractionProgram::PbrInteraction
                                    : InteractionProgram::Interaction;
  return draw;
}

namespace
{

// the stage loop RenderInteractions (RenderBackend.cpp:2160-2235) and AmbientPass
// (:2573-2703) share: their states start apart, their loops are alike
void surfaceStageLoop(
  const MaterialEntry& material,
  const std::span<const float> registers,
  const Vec4& diffuseColor,
  const Vec4& specularColor,
  const bool noSpecular,
  InteractionState& state,
  const ImageUsage& usage,
  const std::function<void(const InteractionDraw&)>& draw)
{
  const auto drawPending = [&] {
    if (const auto made = engineSingleInteraction(state, usage))
    {
      draw(*made);
    }
  };

  // go through the individual surface stages
  for (const auto& stage : material.stages)
  {
    switch (stage.lighting)
    {
    case em::LightingCoverage:
      // ignore any coverage stages since they should only be used for the depth fill
      // pass for diffuse stages that use alpha test
      break;
    case em::LightingAmbient:
      // ignore ambient stages while drawing interactions
      break;
    case em::LightingBump:
      // ignore stage that fails the condition
      if (engineRegister(registers, stage.conditionRegister) == 0.0f)
      {
        break;
      }
      // draw any previous interaction
      if (state.bumpImage)
      {
        drawPending();
      }
      state.bumpImage = stage.image;
      state.diffuseImage.reset();
      state.specularImage.reset();
      engineSetupInteractionStage(stage, registers, nullptr, state.bumpMatrix, nullptr);
      break;
    case em::LightingDiffuse:
      if (engineRegister(registers, stage.conditionRegister) == 0.0f)
      {
        break;
      }
      if (state.diffuseImage)
      {
        drawPending();
      }
      state.diffuseImage = stage.image;
      state.vertexColor = stage.vertexColor;
      engineSetupInteractionStage(
        stage, registers, &diffuseColor, state.diffuseMatrix, &state.diffuseColor);
      break;
    case em::LightingSpecular:
      if (engineRegister(registers, stage.conditionRegister) == 0.0f || noSpecular)
      {
        break;
      }
      if (state.specularImage)
      {
        drawPending();
      }
      state.specularImage = stage.image;
      state.vertexColor = stage.vertexColor;
      engineSetupInteractionStage(
        stage, registers, &specularColor, state.specularMatrix, &state.specularColor);
      break;
    default:
      break;
    }
  }

  // draw the final interaction
  drawPending();
}

} // namespace

void engineSurfaceInteractions(
  const MaterialEntry& material,
  const std::span<const float> registers,
  const Vec4& diffuseColor,
  const Vec4& specularColor,
  const bool noSpecular,
  InteractionState& state,
  const ImageUsage& usage,
  const std::function<void(const InteractionDraw&)>& draw)
{
  // RenderBackend.cpp:2147-2151
  state.bumpImage.reset();
  state.specularImage.reset();
  state.diffuseImage.reset();
  state.diffuseColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  state.specularColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  surfaceStageLoop(
    material, registers, diffuseColor, specularColor, noSpecular, state, usage, draw);
}

void engineAmbientSurfaceInteractions(
  const MaterialEntry& material,
  const std::span<const float> registers,
  const Vec4& diffuseColor,
  const Vec4& specularColor,
  const ImageUsage& usage,
  const std::function<void(const InteractionDraw&)>& draw)
{
  // RenderBackend.cpp:2538-2542, 2563-2565: drawInteraction_t inter = {} per surface,
  // diffuseColor 1 and specularColor 0, the images null, vertexColor SVC_IGNORE
  auto state = InteractionState{};
  state.diffuseColor = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
  state.specularColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  // no light: noSpecular is false (the stage loop's specular test, :2660)
  surfaceStageLoop(
    material, registers, diffuseColor, specularColor, false, state, usage, draw);
}

std::optional<InteractionDraw> pendingSurfaceInteraction(
  const Vec4& diffuseColor, const ImageUsage& usage, const bool ambientLight)
{
  auto state = InteractionState{};
  state.ambientLight = ambientLight;
  state.bumpImage = engineFlatImageKey();
  state.diffuseImage = engineDefaultImageKey();
  state.bumpMatrix = {Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f}};
  state.diffuseMatrix = state.bumpMatrix;
  state.specularMatrix = state.bumpMatrix;
  state.diffuseColor = diffuseColor;
  return engineSingleInteraction(state, usage);
}

std::array<Vec4, 3> engineMathLightParms(const MathLightParms& m, const bool mathPath)
{
  // flags.x: 0 = legacy path, 1 = math on, 2-4 = debug views
  const auto mlMode = mathPath ? 1.0f + float(EngineFrame.showMathLights) : 0.0f;
  return {
    Vec4{mlMode, float(m.type), float(m.falloff), float(m.pattern)},
    Vec4{m.falloffExp, m.innerRadius, m.shapeExp, m.penumbra},
    Vec4{m.patternStrength, m.patternScale, m.patternPhase, m.energyClamp ? 1.0f : 0.0f}};
}

std::optional<std::array<MaterialImageKey, 2>> engineLightStageUnits(
  const bool mathPath,
  const std::optional<MaterialImageKey>& falloffImage,
  const MaterialStage& lightStage)
{
  // a math light samples neither of these: the shared neutral image instead of the
  // light's own (RenderBackend.cpp:1906-1921)
  if (mathPath)
  {
    return std::array{engineWhiteImageKey(), engineWhiteImageKey()};
  }
  // texture 1 will be the light falloff texture, texture 2 the light projection texture
  if (!falloffImage || !lightStage.image)
  {
    return std::nullopt;
  }
  return std::array{*falloffImage, *lightStage.image};
}

StagePassSurface engineShaderPassDraws(
  const MaterialEntry& material, const std::span<const float> registers)
{
  namespace gls = engine_gls;
  auto result = StagePassSurface{};

  // set face culling appropriately (RenderBackend.cpp:4396-4422): not a GUI surface
  auto cullMode = gls::CullFrontSided;
  switch (material.cullType)
  {
  case engine_cull::TwoSided:
    cullMode = gls::CullTwoSided;
    break;
  case engine_cull::BackSided:
    // M-STAGE-DEFECT-01 (D117): GLS_CULL_BITS (2 << 15) masks this bit away, so the
    // copied GetRenderState draws it as the front-sided case
    cullMode = gls::CullBackSided;
    break;
  case engine_cull::FrontSided:
  default:
    cullMode = gls::CullFrontSided;
    break;
  }
  // surf->extraGLState is 0 for every preview surface
  auto surfState = cullMode;

  // set polygon offset if necessary (:4426-4432)
  auto surfSlope = 0.0f;
  auto surfBias = 0.0f;
  if (material.materialFlags & em::FlagPolygonOffset)
  {
    surfSlope = EngineFrame.offsetFactor;
    surfBias = EngineFrame.offsetUnits * material.polygonOffset;
    surfState |= gls::PolygonOffset;
  }

  for (std::size_t s = 0; s < material.stages.size(); ++s)
  {
    const auto& stage = material.stages[s];
    const auto ambient = stage.lighting == em::LightingAmbient;

    // check the enable condition
    if (engineRegister(registers, stage.conditionRegister) == 0.0f)
    {
      result.skipped += ambient ? 1 : 0;
      continue;
    }
    // skip the stages involved in lighting
    if (!ambient)
    {
      continue;
    }

    auto stageState = surfState;
    if ((surfState & gls::Override) == 0)
    {
      stageState |= stage.drawStateBits;
    }

    // skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
    if (
      (stageState & (gls::SrcBlendBits | gls::DstBlendBits))
      == (gls::SrcBlendZero | gls::DstBlendOne))
    {
      ++result.skipped;
      continue;
    }

    // the stencil stage (:4462-4506): only these values add bits
    if (stage.hasStencil)
    {
      const auto& stencil = stage.stencil;
      // stencilComp_t 4 EQUAL, 6 ALWAYS; stencilOperation_t 0 KEEP, 2 REPLACE, 7
      // DECRWRAP
      switch (stencil.comp)
      {
      case 6:
        stageState |= gls::StencilFuncAlways;
        break;
      case 4:
        stageState |= gls::StencilFuncEqual;
        break;
      default:
        break;
      }
      switch (stencil.pass)
      {
      case 2:
        stageState |= gls::StencilOpPassReplace;
        break;
      case 0:
        stageState |= gls::StencilOpPassKeep;
        break;
      case 7:
        stageState |= gls::StencilOpPassDecrWrap;
        break;
      default:
        break;
      }
      switch (stencil.zFail)
      {
      case 7:
        stageState |= gls::StencilOpZFailDecrWrap;
        break;
      case 0:
        stageState |= gls::StencilOpZFailKeep;
        break;
      default:
        break;
      }
      if (stencil.fail == 0)
      {
        stageState |= gls::StencilOpFailKeep;
      }
      stageState |=
        ((std::uint64_t(stencil.ref) << gls::StencilFuncRefShift)
         & gls::StencilFuncRefBits)
        | ((std::uint64_t(stencil.writeMask) << gls::StencilFuncMaskShift) & gls::StencilFuncMaskBits);
    }

    auto draw = StagePassDraw{};
    draw.stage = int(s);
    draw.state = stageState;
    draw.slopeBias = surfSlope;
    draw.depthBias = surfBias;

    // see if we are a new-style stage (:4509-4570)
    if (stage.newStage)
    {
      if (EngineFrame.skipNewAmbient)
      {
        ++result.skipped;
        continue;
      }
      draw.program = StageProgram::Material;
      for (const auto& parm : stage.vertexParms)
      {
        draw.userParms.push_back(Vec4{
          engineRegister(registers, parm[0]),
          engineRegister(registers, parm[1]),
          engineRegister(registers, parm[2]),
          engineRegister(registers, parm[3])});
      }
      result.draws.push_back(std::move(draw));
      continue;
    }

    // old style stages: set the color (:4577-4581)
    const auto& c = stage.colorRegisters;
    const auto color = Vec4{
      engineRegister(registers, c[0]),
      engineRegister(registers, c[1]),
      engineRegister(registers, c[2]),
      engineRegister(registers, c[3])};

    const auto blend = stageState & (gls::SrcBlendBits | gls::DstBlendBits);
    // skip the entire stage if an add would be black
    if (
      blend == (gls::SrcBlendOne | gls::DstBlendOne) && color[0] <= 0.0f
      && color[1] <= 0.0f && color[2] <= 0.0f)
    {
      ++result.skipped;
      continue;
    }
    // skip the entire stage if a blend would be completely transparent
    if (
      blend == (gls::SrcBlendSrcAlpha | gls::DstBlendOneMinusSrcAlpha)
      && color[3] <= 0.0f)
    {
      ++result.skipped;
      continue;
    }

    // D120: a cinematic's bink or RoQ frame is not drawn (mirror.json M-STAGE-DIFF-02)
    if (!stage.cinematic.empty() || stage.soundMap)
    {
      ++result.skipped;
      ++result.cinematic;
      continue;
    }

    // the program by the texgen (:4604-4671), then PrepareStageTexturing's (:352-638),
    // which binds another program over texture_color for the cube and sky texgens
    switch (stage.texgen)
    {
    case em::TexgenScreen:
    case em::TexgenScreen2:
      draw.program = StageProgram::TextureColorTexgen;
      draw.screenTexgen = true;
      break;
    case em::TexgenReflectCube:
      // see if there is also a bump map specified (:360-392): its image raw at unit 1
      draw.bumpStage = engineBumpStage(material);
      draw.program =
        draw.bumpStage >= 0 ? StageProgram::BumpyEnvironment : StageProgram::Environment;
      break;
    case em::TexgenReflectCube2:
      // :393-525: with a bump stage BUMPY_ENVIRONMENT2 (59-62) on _currentRender, the
      // G-buffer normals, Hi-Z and the view's radiance probes (LP6's inputs); without one
      // texture_color stays and samples the cube as a Texture2D: neither is drawn
      // (mirror.json M-STAGE-DIFF-05)
      ++result.skipped;
      ++result.texgenLp6;
      continue;
    case em::TexgenSkyboxCube:
      // :526-529
      draw.program = StageProgram::SkyBox;
      break;
    case em::TexgenWobbleskyCube:
      // :530-586: the transform at the preview's material time 0 (K5, D119)
      draw.program = StageProgram::WobbleSky;
      draw.wobbleSky = engineStageWobbleSky(material, registers);
      break;
    case em::TexgenDiffuseCube:
    case em::TexgenGlasswarp:
      // :625-635: "Please contact Brian!" and nothing else: texture_color stays
      draw.program = StageProgram::TextureColor;
      draw.texgenWarning = true;
      break;
    case em::TexgenExplicit:
    default:
      draw.program = StageProgram::TextureColor;
      break;
    }

    // GL_Color clamps (RenderBackend_NVRHI.cpp:2238-2246)
    draw.color = engineClampedColor(color);
    draw.vertexColor = engineVertexColorParms(stage.vertexColor);

    // set privatePolygonOffset if necessary (:4679-4683)
    if (stage.privatePolygonOffset != 0.0f)
    {
      draw.slopeBias = EngineFrame.offsetFactor;
      draw.depthBias = EngineFrame.offsetUnits * stage.privatePolygonOffset;
      draw.state |= gls::PolygonOffset;
    }

    // PrepareStageTexturing (:352-638): the texture matrix always; TG_SCREEN's planes
    // are the pass's (the space's modelView × projection)
    draw.textureMatrix = engineLoadShaderTextureMatrix(registers, stage);
    result.draws.push_back(std::move(draw));
  }
  return result;
}

} // namespace tb::lit
