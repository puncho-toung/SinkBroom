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
      case em::TexgenReflectCube2:
      case em::TexgenSkyboxCube:
      case em::TexgenWobbleskyCube:
        // these bind another program over texture_color, which LP2 does not load
        continue;
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
  // the light is not an ambient light (LP2 draws none)
  if (!state.specularImage)
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

  // the program by the specular image's usage; K26: the unshadowed one
  const auto specUsage = usage ? usage(draw.specularImage) : draw.specularImage.usage;
  draw.program = specUsage == engine_image::UsageSpecularPbrRmao
                     || specUsage == engine_image::UsageSpecularPbrRmao + 1
                   ? InteractionProgram::PbrInteraction
                   : InteractionProgram::Interaction;
  return draw;
}

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
  const auto drawPending = [&] {
    if (const auto made = engineSingleInteraction(state, usage))
    {
      draw(*made);
    }
  };

  state.bumpImage.reset();
  state.specularImage.reset();
  state.diffuseImage.reset();
  state.diffuseColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  state.specularColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f};

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

std::optional<InteractionDraw> pendingSurfaceInteraction(
  const Vec4& diffuseColor, const ImageUsage& usage)
{
  auto state = InteractionState{};
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
  // flags.x: 0 = legacy path, 1 = math on (r_showMathLights 0)
  const auto mlMode = mathPath ? 1.0f : 0.0f;
  return {
    Vec4{mlMode, float(m.type), float(m.falloff), float(m.pattern)},
    Vec4{m.falloffExp, m.innerRadius, m.shapeExp, m.penumbra},
    Vec4{m.patternStrength, m.patternScale, m.patternPhase, m.energyClamp ? 1.0f : 0.0f}};
}

} // namespace tb::lit
