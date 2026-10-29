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

// The hidden retail test of LP2's shaders ([.][retail]): it reads the blobs of the game
// folder that TB_LIT_GAME_PATH names (read-only), and is skipped without it. Every blob
// LP2 draws with is found, its permutation keys are the ones the engine's shader build
// wrote (SC249), and every permutation the preview may load, pushed or not, reflects
// equal to the mirrored tables (D9, C26). LP4.2a (C35) adds the ambient pass's four
// programs (4 and 6 the probe path, 8 and 10 the light grid's) and set 9; LP5.0 the
// programs of passes 7-9 and the cube and sky texgens, with sets 5, 7, 8 and 12; LP3.0
// (C60) the six non-skinned shadow-atlas interactionSM programs and set 11.

#include "lit/EngineTables.h"
#include "lit/ShaderBlobs.h"
#include "lit/ShaderReflection.h"

#include "kd/result.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::optional<std::filesystem::path> retailGamePath()
{
  const auto* value = std::getenv("TB_LIT_GAME_PATH");
  if (!value || !*value)
  {
    return std::nullopt;
  }
  return std::filesystem::path{value};
}

std::vector<ShaderStage> stagesOf(const EngineBuiltin& builtin)
{
  auto stages = std::vector<ShaderStage>{};
  if (builtin.stages & EngineStageVertex)
  {
    stages.push_back(ShaderStage::Vertex);
  }
  if (builtin.stages & EngineStageFragment)
  {
    stages.push_back(ShaderStage::Pixel);
  }
  if (builtin.stages & EngineStageCompute)
  {
    stages.push_back(ShaderStage::Compute);
  }
  return stages;
}

bool hasPushConstantsDefine(const EngineBuiltin& builtin)
{
  for (std::size_t i = 0; i < builtin.defineCount; ++i)
  {
    if (!builtin.defines[i].value)
    {
      return true;
    }
  }
  return false;
}

// a key of the form "A=a B=b": its names in order
std::vector<std::string> keyNames(const std::string& key)
{
  auto names = std::vector<std::string>{};
  auto start = std::size_t{0};
  while (start < key.size())
  {
    const auto end = std::min(key.find(' ', start), key.size());
    const auto pair = key.substr(start, end - start);
    names.push_back(pair.substr(0, pair.find('=')));
    start = end + 1;
  }
  return names;
}

} // namespace

TEST_CASE("Retail shaders: LP2's blobs, keys and reflection", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // the programs LP2 draws with (27-LP2-PACKAGES.md §4)
  const auto programs = std::vector<int>{
    EngineBuiltinColor,
    EngineBuiltinTextureVertexColor,
    EngineBuiltinInteraction,
    EngineBuiltinPbrInteraction,
    EngineBuiltinDepth,
    EngineBuiltinBlit,
    EngineBuiltinRect,
    EngineBuiltinTonemapping,
    EngineBuiltinHistogram,
    EngineBuiltinExposure,
    // LP4.2a (C35): the ambient pass's (shaders.cfg:44-47)
    EngineBuiltinAmbientLightingIbl,
    EngineBuiltinAmbientLightingIblPbr,
    EngineBuiltinAmbientLightgridIbl,
    EngineBuiltinAmbientLightgridIblPbr,
  };

  // SC249: the keys each blob holds
  struct Keys
  {
    std::string_view file;
    std::size_t count;
    std::string_view first;
  };
  const auto expectedKeys = std::vector<Keys>{
    {"builtin/lighting/interaction.ps",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/lighting/interaction.vs",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/depth.vs", 4, "USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/depth.ps", 4, "USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/color.vs", 4, "USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/texture_color.ps", 8, "USE_GPU_SKINNING=0 USE_SRGB=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/post/tonemapping.ps",
     8,
     "HISTOGRAM_BINS=256 SOURCE_ARRAY=0 QUAD_Z=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/post/tonemapping.vs",
     8,
     "HISTOGRAM_BINS=256 SOURCE_ARRAY=0 QUAD_Z=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/post/histogram.cs",
     4,
     "HISTOGRAM_BINS=256 SOURCE_ARRAY=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/post/exposure.cs", 2, "HISTOGRAM_BINS=256 USE_PUSH_CONSTANTS=0"},
    {"builtin/blit.ps", 2, "TEXTURE_ARRAY=0"},
    // LP4.2a: shaders.cfg:44-47, three defines of two values each
    {"builtin/lighting/ambient_lighting_IBL.vs",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/lighting/ambient_lighting_IBL.ps",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/lighting/ambient_lightgrid_IBL.vs",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
    {"builtin/lighting/ambient_lightgrid_IBL.ps",
     8,
     "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0"},
  };
  for (const auto& keys : expectedKeys)
  {
    CAPTURE(keys.file);
    const auto dot = keys.file.rfind('.');
    const auto suffix = keys.file.substr(dot + 1);
    const auto stage = suffix == "vs"   ? ShaderStage::Vertex
                       : suffix == "ps" ? ShaderStage::Pixel
                                        : ShaderStage::Compute;
    const auto blob = readShaderBlob(*gamePath, keys.file.substr(0, dot), stage);
    REQUIRE(blob.is_success());
    const auto found = shaderBlobKeys(blob.value());
    CHECK(found.size() == keys.count);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front() == keys.first);
  }
  // the raw rect.vs
  const auto rect = readShaderBlob(*gamePath, "builtin/rect", ShaderStage::Vertex);
  REQUIRE(rect.is_success());
  CHECK_FALSE(isPermutationBlob(rect.value()));

  auto checked = 0;
  // T0.5: each program's stages by push-constant variant, for the reverse check (the
  // blit draws rect.vs with blit.ps, CommonPasses.cpp:59-65)
  auto programStages = std::map<std::pair<int, bool>, std::vector<ShaderReflection>>{};
  for (const auto index : programs)
  {
    const auto& builtin = *engineBuiltin(index);
    for (const auto stage : stagesOf(builtin))
    {
      CAPTURE(builtin.name, builtin.nameOutSuffix, shaderStageSuffix(stage));
      const auto blob = readShaderBlob(*gamePath, builtin.name, stage);
      REQUIRE(blob.is_success());

      // C2: the key the builtins row makes is on disk, in the table's define order,
      // pushed and not where the row has the define
      const auto keys = shaderBlobKeys(blob.value());
      for (const auto& key : keys)
      {
        auto names = std::vector<std::string>{};
        for (const auto& define : engineBuiltinDefines(builtin, false))
        {
          names.push_back(define.name);
        }
        CHECK(keyNames(key) == names);
      }

      for (const auto pushed : hasPushConstantsDefine(builtin)
                                 ? std::vector<bool>{false, true}
                                 : std::vector<bool>{true})
      {
        CAPTURE(pushed);
        const auto defines = engineBuiltinDefines(builtin, pushed);
        const auto code = findShaderPermutation(blob.value(), defines);
        REQUIRE(code.is_success());
        const auto reflection = reflectSpirv(code.value());
        REQUIRE(reflection.is_success());
        const auto expected = engineExpectedLayout(builtin, pushed);
        REQUIRE(expected);
        const auto difference = compareReflection(reflection.value(), *expected);
        CHECK_FALSE(difference);
        programStages[{index, pushed}].push_back(reflection.value());
        if (index == EngineBuiltinRect)
        {
          programStages[{EngineBuiltinBlit, pushed}].push_back(reflection.value());
        }
        if (difference)
        {
          std::cout << builtin.name << builtin.nameOutSuffix << "."
                    << shaderStageSuffix(stage) << " pushed " << pushed << ": "
                    << *difference << "\n";
        }

        const auto& r = reflection.value();
        std::cout << builtin.name << builtin.nameOutSuffix << "."
                  << shaderStageSuffix(stage) << " [" << shaderPermutationKey(defines)
                  << "]: " << r.bindings.size() << " descriptors";
        for (const auto& binding : r.bindings)
        {
          std::cout << " " << binding.set << ":" << binding.binding;
          if (binding.block)
          {
            std::cout << " (cb " << binding.block->members.size() << " members, "
                      << binding.block->size << " B)";
          }
        }
        if (r.pushConstants)
        {
          std::cout << "; push " << r.pushConstants->members.size() << " members, "
                    << r.pushConstants->size << " B";
        }
        if (!r.inputs.empty())
        {
          std::cout << "; inputs at";
          for (const auto& input : r.inputs)
          {
            std::cout << " " << input.location << "(" << input.components << ")";
          }
        }
        std::cout << "\n";
        ++checked;
      }
    }
  }

  // T0.5: the table's every item declared by a stage of its program (rect.vs alone is no
  // program); the reflection and the tables are then equal as sets
  auto covered = 0;
  for (const auto& [program, stages] : programStages)
  {
    const auto& builtin = *engineBuiltin(program.first);
    if (program.first == EngineBuiltinRect)
    {
      continue;
    }
    CAPTURE(builtin.name, builtin.nameOutSuffix, program.second);
    auto expected = engineExpectedLayout(builtin, program.second);
    REQUIRE(expected);
    if (
      program.first == EngineBuiltinAmbientLightingIbl
      || program.first == EngineBuiltinAmbientLightingIblPbr)
    {
      // LP4.2a (D85): the probe path's shader declares no t7
      // (ambient_lighting_IBL.ps.hlsl :44, "no longer used"); the shared layout has it
      // (RenderProgs.cpp:458), which the light-grid path reads. The comparison accepts a
      // layout item a shader leaves out (compareReflection); the coverage names exactly
      // that item and covers the rest
      const auto withT7 = compareLayoutCoverage(stages, *expected);
      REQUIRE(withT7);
      CHECK(
        *withT7
        == "the table's set 2 binding 7 (a sampled image) is declared by no stage");
      auto withoutT7 = expected->sets[2];
      withoutT7.bindings.resize(0);
      for (const auto& item : expected->sets[2].bindings)
      {
        if (
          nvrhi::ResourceType(item.type) != nvrhi::ResourceType::Texture_SRV
          || item.slot != 7u)
        {
          withoutT7.bindings.push_back(item);
        }
      }
      REQUIRE(withoutT7.bindings.size() + 1 == expected->sets[2].bindings.size());
      expected->sets[2] = withoutT7;
    }
    const auto uncovered = compareLayoutCoverage(stages, *expected);
    CHECK_FALSE(uncovered);
    if (uncovered)
    {
      std::cout << builtin.name << builtin.nameOutSuffix << " pushed " << program.second
                << ": " << *uncovered << "\n";
    }
    ++covered;
  }
  CHECK(covered > 0);

  // set 10's 25 members by name and offset, in the interaction's constant buffer
  const auto& interaction = *engineBuiltin(EngineBuiltinInteraction);
  const auto blob = readShaderBlob(*gamePath, interaction.name, ShaderStage::Pixel);
  REQUIRE(blob.is_success());
  const auto code =
    findShaderPermutation(blob.value(), engineBuiltinDefines(interaction, false));
  REQUIRE(code.is_success());
  const auto reflection = reflectSpirv(code.value());
  REQUIRE(reflection.is_success());
  auto set10 = std::optional<ReflectedBlock>{};
  for (const auto& binding : reflection.value().bindings)
  {
    if (binding.set == 0 && binding.binding == 256)
    {
      set10 = binding.block;
    }
  }
  REQUIRE(set10);
  const auto table = engineParmSetBlock(10);
  REQUIRE(set10->members.size() == 25u);
  for (std::size_t i = 0; i < 25; ++i)
  {
    CHECK(set10->members[i].name == table.members[i].name);
    CHECK(set10->members[i].offset == table.members[i].offset);
  }
  CHECK(set10->size == 400u);

  // LP4.2a (C35): set 9's 45 members by name and offset in each ambient program's
  // constant buffer (the probe and the grid shaders, both stages), the SH array element
  // by element, 720 bytes
  const auto table9 = engineParmSetBlock(9);
  REQUIRE(table9.members.size() == 45u);
  CHECK(table9.size == 720u);
  for (const auto program :
       {EngineBuiltinAmbientLightingIbl,
        EngineBuiltinAmbientLightingIblPbr,
        EngineBuiltinAmbientLightgridIbl,
        EngineBuiltinAmbientLightgridIblPbr})
  {
    const auto& ambient = *engineBuiltin(program);
    for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
      CAPTURE(ambient.name, ambient.nameOutSuffix, shaderStageSuffix(stage));
      const auto ambientBlob = readShaderBlob(*gamePath, ambient.name, stage);
      REQUIRE(ambientBlob.is_success());
      const auto ambientCode =
        findShaderPermutation(ambientBlob.value(), engineBuiltinDefines(ambient, false));
      REQUIRE(ambientCode.is_success());
      const auto ambientReflection = reflectSpirv(ambientCode.value());
      REQUIRE(ambientReflection.is_success());
      auto set9 = std::optional<ReflectedBlock>{};
      for (const auto& binding : ambientReflection.value().bindings)
      {
        if (binding.set == 0 && binding.binding == 256)
        {
          set9 = binding.block;
        }
      }
      REQUIRE(set9);
      REQUIRE(set9->members.size() == 45u);
      for (std::size_t i = 0; i < 45; ++i)
      {
        CHECK(set9->members[i].name == table9.members[i].name);
        CHECK(set9->members[i].offset == table9.members[i].offset);
      }
      CHECK(set9->size == 720u);
      CHECK(set9->members[0] == ReflectedMember{"rpScreenCorrectionFactor", 0});
      CHECK(set9->members[36] == ReflectedMember{"rpSHIrradiance[0]", 576});
      CHECK(set9->members[44] == ReflectedMember{"rpSHIrradiance[8]", 704});
    }
  }

  std::cout << "reflected and compared " << checked << " permutations, " << covered
            << " programs covered\n";
  // T0.5: the fourteen programs' stages, each of their permutations (LP4.2a: the four
  // ambient programs' two stages, pushed and not)
  CHECK(checked == 46);
}

TEST_CASE("Retail shaders: LP5's blobs, sets 5, 7, 8 and 12", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // LP5.0 (C45-C49): every builtin LP5 binds (rows 15, 18, 21, 55, 57, 65, 67, 69, 70)
  // and the skinned rows 17 and 22, reflected only (LP7 binds them), with the layouts and
  // sets the tables give them
  struct Program
  {
    int index;
    EngineBindingLayout layout;
    int set;
  };
  using L = EngineBindingLayout;
  const auto programs = std::vector<Program>{
    {15, L::Texture, 4},
    {17, L::TextureSkinned, 4},
    {18, L::Texgen, 12},
    {21, L::DrawInteraction, 10},
    {22, L::DrawInteractionSkinned, 10},
    {55, L::Default, 0},
    {57, L::NormalCube, 5},
    {65, L::Blendlight, 7},
    {67, L::Fog, 7},
    {69, L::Legacy, 5},
    {70, L::Legacy, 5},
  };

  auto checked = 0;
  auto setsChecked = std::map<int, int>{};
  for (const auto& program : programs)
  {
    const auto& builtin = *engineBuiltin(program.index);
    CAPTURE(builtin.name, builtin.nameOutSuffix);
    REQUIRE(builtin.bindingLayout == program.layout);
    REQUIRE(engineParmSetOfLayout(program.layout) == program.set);
    const auto table = engineParmSetBlock(program.set);
    REQUIRE(
      table.members.size() == std::size_t(EngineParmSetSizes[std::size_t(program.set)]));

    for (const auto pushed : {false, true})
    {
      CAPTURE(pushed);
      auto stages = std::vector<ShaderReflection>{};
      for (const auto stage : stagesOf(builtin))
      {
        CAPTURE(shaderStageSuffix(stage));
        const auto blob = readShaderBlob(*gamePath, builtin.name, stage);
        REQUIRE(blob.is_success());

        // C2: the keys on disk name the row's defines in its order
        auto names = std::vector<std::string>{};
        for (const auto& define : engineBuiltinDefines(builtin, false))
        {
          names.push_back(define.name);
        }
        for (const auto& key : shaderBlobKeys(blob.value()))
        {
          CHECK(keyNames(key) == names);
        }

        const auto code =
          findShaderPermutation(blob.value(), engineBuiltinDefines(builtin, pushed));
        REQUIRE(code.is_success());
        const auto reflection = reflectSpirv(code.value());
        REQUIRE(reflection.is_success());
        const auto expected = engineExpectedLayout(builtin, pushed);
        REQUIRE(expected);
        const auto difference = compareReflection(reflection.value(), *expected);
        CHECK_FALSE(difference);
        if (difference)
        {
          std::cout << builtin.name << builtin.nameOutSuffix << "."
                    << shaderStageSuffix(stage) << " pushed " << pushed << ": "
                    << *difference << "\n";
        }

        // every member of the set by name and offset, and the set's size, in the
        // stage's uniforms (the push-constant block, or the constant buffer at b0 of
        // set 0 = binding 256), where the stage declares them
        auto uniforms = std::optional<ReflectedBlock>{};
        if (pushed)
        {
          uniforms = reflection.value().pushConstants;
        }
        else
        {
          for (const auto& binding : reflection.value().bindings)
          {
            if (binding.set == 0 && binding.binding == 256 && binding.block)
            {
              uniforms = binding.block;
            }
          }
        }
        if (uniforms)
        {
          REQUIRE(uniforms->members.size() == table.members.size());
          for (std::size_t i = 0; i < table.members.size(); ++i)
          {
            CHECK(uniforms->members[i].name == table.members[i].name);
            CHECK(uniforms->members[i].offset == table.members[i].offset);
          }
          CHECK(uniforms->size == table.size);
          ++setsChecked[program.set];
        }
        stages.push_back(reflection.value());
        ++checked;
      }
      // T0.5: every item of the table's layout declared by a stage
      const auto expected = engineExpectedLayout(builtin, pushed);
      REQUIRE(expected);
      const auto uncovered = compareLayoutCoverage(stages, *expected);
      CHECK_FALSE(uncovered);
      if (uncovered)
      {
        std::cout << builtin.name << builtin.nameOutSuffix << " pushed " << pushed << ": "
                  << *uncovered << "\n";
      }
    }
  }

  // the four new sets were each met in some stage of every variant
  CHECK(setsChecked[5] > 0);
  CHECK(setsChecked[7] > 0);
  CHECK(setsChecked[12] > 0);
  std::cout << "LP5: reflected and compared " << checked << " permutations; set 0 "
            << setsChecked[0] << ", 4 " << setsChecked[4] << ", 5 " << setsChecked[5]
            << ", 7 " << setsChecked[7] << ", 10 " << setsChecked[10] << ", 12 "
            << setsChecked[12] << " blocks\n";
  // eleven programs of two stages, pushed and not
  CHECK(checked == 44);
}

TEST_CASE("Retail shaders: the material programs and set 8 (LP5.0)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // C47, D121: every program of shaders.cfg:130-154 has both blobs here, reflects equal
  // to POST_PROCESS_INGAME (set 8's 13 members by name and offset where a stage declares
  // the uniforms), and binds t_CurrentRender exactly where the table says it reads
  // _currentRender
  const auto table8 = engineParmSetBlock(8);
  REQUIRE(table8.members.size() == 13u);
  CHECK(table8.size == 208u);
  auto checked = 0;
  auto blocks = 0;
  auto readers = std::vector<std::string>{};
  for (const auto& entry : EngineMaterialPrograms)
  {
    const auto& row = entry.row;
    CAPTURE(row.name);
    const auto program = engineMaterialProgram(*gamePath, row.name);
    REQUIRE(program);
    CHECK(program->row.index == row.index);

    auto readsCurrentRender = false;
    for (const auto pushed : {false, true})
    {
      CAPTURE(pushed);
      for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
      {
        CAPTURE(shaderStageSuffix(stage));
        const auto blob = readShaderBlob(*gamePath, row.name, stage);
        REQUIRE(blob.is_success());
        for (const auto& key : shaderBlobKeys(blob.value()))
        {
          CHECK(
            keyNames(key)
            == std::vector<std::string>{"USE_GPU_SKINNING", "USE_PUSH_CONSTANTS"});
        }
        const auto code =
          findShaderPermutation(blob.value(), engineBuiltinDefines(row, pushed));
        REQUIRE(code.is_success());
        const auto reflection = reflectSpirv(code.value());
        REQUIRE(reflection.is_success());
        const auto expected = engineExpectedLayout(row, pushed);
        REQUIRE(expected);
        const auto difference = compareReflection(reflection.value(), *expected);
        CHECK_FALSE(difference);
        if (difference)
        {
          std::cout << row.name << "." << shaderStageSuffix(stage) << " pushed " << pushed
                    << ": " << *difference << "\n";
        }

        auto uniforms = std::optional<ReflectedBlock>{};
        if (pushed)
        {
          uniforms = reflection.value().pushConstants;
        }
        for (const auto& binding : reflection.value().bindings)
        {
          if (!pushed && binding.set == 0 && binding.binding == 256 && binding.block)
          {
            uniforms = binding.block;
          }
          if (binding.name == "t_CurrentRender")
          {
            readsCurrentRender = true;
            CHECK(stage == ShaderStage::Pixel);
          }
        }
        if (uniforms)
        {
          REQUIRE(uniforms->members.size() == table8.members.size());
          for (std::size_t i = 0; i < table8.members.size(); ++i)
          {
            CHECK(uniforms->members[i].name == table8.members[i].name);
            CHECK(uniforms->members[i].offset == table8.members[i].offset);
          }
          CHECK(uniforms->size == 208u);
          ++blocks;
        }
        ++checked;
      }
    }
    CHECK(readsCurrentRender == entry.readsCurrentRender);
    if (readsCurrentRender)
    {
      readers.push_back(row.name);
    }
  }
  std::cout << "material programs: " << checked << " permutations, " << blocks
            << " set 8 blocks; reading _currentRender:";
  for (const auto& name : readers)
  {
    std::cout << " " << name;
  }
  std::cout << "\n";
  // twelve programs, two stages, pushed and not
  CHECK(checked == 48);
  CHECK(blocks > 0);
  // C47 said every one but vornoipattern; fogwithlights' HLSL declares t_CurrentRender
  // (fogwithlights.ps.hlsl:35) and never samples it, so its blobs do not bind it
  CHECK(readers.size() == 10u);
}

TEST_CASE(
  "Retail shaders: the shadow-atlas interactionSM programs and set 11 (LP3.0)",
  "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // C60, D9 (P3.24e: no skinned set): the rows LP3.1 selects, _atlas_{spot|point|
  // parallel}[_PBR] (RenderProgs.cpp:851-867), each with USE_PUSH_CONSTANTS 0 only: set
  // 11's 1088 bytes pass no desktop budget (pcEnabled false, C30), so the engine never
  // loads the pushed permutation; both stages reflect equal to DRAW_INTERACTION_SM's
  // table (descriptor names' sets and bindings, the constant buffer's members by name and
  // offset and its size), and every item of the table is declared by a stage
  using L = EngineBindingLayout;
  const auto table = engineParmSetBlock(11);
  REQUIRE(table.members.size() == 68u);
  REQUIRE(table.size == 1088u);
  REQUIRE_FALSE(enginePushConstantsEnabled(
    L::DrawInteractionSm, enginePushConstantLimit(4096), 0x10DE));

  auto checked = 0;
  auto blocks = 0;
  for (const auto index : {39, 41, 43, 45, 47, 49})
  {
    const auto& builtin = *engineBuiltin(index);
    CAPTURE(builtin.name, builtin.nameOutSuffix);
    REQUIRE(builtin.bindingLayout == L::DrawInteractionSm);
    REQUIRE_FALSE(builtin.requireGPUSkinningSupport);
    REQUIRE(engineParmSetOfLayout(builtin.bindingLayout) == 11);

    auto stages = std::vector<ShaderReflection>{};
    for (const auto stage : stagesOf(builtin))
    {
      CAPTURE(shaderStageSuffix(stage));
      const auto blob = readShaderBlob(*gamePath, builtin.name, stage);
      REQUIRE(blob.is_success());
      // C2: the keys on disk name the row's defines in its order
      auto names = std::vector<std::string>{};
      for (const auto& define : engineBuiltinDefines(builtin, false))
      {
        names.push_back(define.name);
      }
      for (const auto& key : shaderBlobKeys(blob.value()))
      {
        CHECK(keyNames(key) == names);
      }
      const auto code =
        findShaderPermutation(blob.value(), engineBuiltinDefines(builtin, false));
      REQUIRE(code.is_success());
      const auto reflection = reflectSpirv(code.value());
      REQUIRE(reflection.is_success());
      const auto expected = engineExpectedLayout(builtin, false);
      REQUIRE(expected);
      const auto difference = compareReflection(reflection.value(), *expected);
      CHECK_FALSE(difference);
      if (difference)
      {
        std::cout << builtin.name << builtin.nameOutSuffix << "."
                  << shaderStageSuffix(stage) << ": " << *difference << "\n";
      }
      CHECK_FALSE(reflection.value().pushConstants);
      for (const auto& binding : reflection.value().bindings)
      {
        if (binding.set == 0 && binding.binding == 256 && binding.block)
        {
          const auto& uniforms = *binding.block;
          REQUIRE(uniforms.members.size() == table.members.size());
          for (std::size_t i = 0; i < table.members.size(); ++i)
          {
            CHECK(uniforms.members[i].name == table.members[i].name);
            CHECK(uniforms.members[i].offset == table.members[i].offset);
          }
          CHECK(uniforms.size == 1088u);
          ++blocks;
        }
      }
      stages.push_back(reflection.value());
      ++checked;
    }
    const auto expected = engineExpectedLayout(builtin, false);
    REQUIRE(expected);
    const auto uncovered = compareLayoutCoverage(stages, *expected);
    CHECK_FALSE(uncovered);
    if (uncovered)
    {
      std::cout << builtin.name << builtin.nameOutSuffix << ": " << *uncovered << "\n";
    }
  }
  std::cout << "LP3.0: reflected and compared " << checked
            << " interactionSM permutations, " << blocks << " set 11 blocks\n";
  // six programs of two stages, not pushed: the twelve blobs' permutations
  CHECK(checked == 12);
  CHECK(blocks > 0);
}

TEST_CASE(
  "Retail shaders: the G-buffer program, set 3 and the SSAO named programs (LP6.5a)",
  "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // C69 (SC807): builtin/gbuffer (12), GBUFFER's {uniforms, t0, s0} with set 3 pushed or
  // not (pcEnabled: off only on Vulkan with AMD), both stages; C70 (P8.13, D9): the three
  // named programs, the permutation their FindShader defines name (no
  // USE_PUSH_CONSTANTS: CFG:126-128), reflected against engineExpectedLayout of their
  // rows: one set, b1 the SsaoConstants block by name and offset, the SRVs, the UAV, the
  // blur's sampler; and every item of each table declared by the shader
  using L = EngineBindingLayout;
  auto checked = 0;
  auto ssaoBlocks = 0;
  auto set3Blocks = 0;
  const auto set3 = engineParmSetBlock(3);
  REQUIRE(set3.members.size() == 16u);
  REQUIRE(set3.size == 256u);
  const auto ssao = engineSsaoBlock();
  REQUIRE(ssao.size == 272u);

  const auto check = [&](const EngineBuiltin& builtin, const bool pushed) {
    CAPTURE(builtin.name, builtin.nameOutSuffix, pushed);
    const auto expected = engineExpectedLayout(builtin, pushed);
    REQUIRE(expected);
    auto stages = std::vector<ShaderReflection>{};
    for (const auto stage : stagesOf(builtin))
    {
      CAPTURE(shaderStageSuffix(stage));
      const auto blob = readShaderBlob(*gamePath, builtin.name, stage);
      REQUIRE(blob.is_success());
      const auto defines = engineBuiltinDefines(builtin, pushed);
      const auto key = shaderPermutationKey(defines);
      const auto keys = shaderBlobKeys(blob.value());
      // C2: the key the defines name is one the blob holds
      CHECK(std::ranges::find(keys, key) != keys.end());
      std::cout << "LP6.5a: " << builtin.name << "." << shaderStageSuffix(stage)
                << " key '" << key << "' of " << keys.size() << " keys\n";
      const auto code = findShaderPermutation(blob.value(), defines);
      REQUIRE(code.is_success());
      const auto reflection = reflectSpirv(code.value());
      REQUIRE(reflection.is_success());
      const auto difference = compareReflection(reflection.value(), *expected);
      CHECK_FALSE(difference);
      if (difference)
      {
        std::cout << builtin.name << "." << shaderStageSuffix(stage) << ": "
                  << *difference << "\n";
      }
      for (const auto& binding : reflection.value().bindings)
      {
        std::cout << "  set " << binding.set << " binding " << binding.binding << " "
                  << describeDescriptorKind(binding.kind) << " " << binding.name << "\n";
        if (binding.block && binding.block->size == 272u)
        {
          REQUIRE(binding.block->members.size() == ssao.members.size());
          for (std::size_t i = 0; i < ssao.members.size(); ++i)
          {
            CHECK(binding.block->members[i].name == ssao.members[i].name);
            CHECK(binding.block->members[i].offset == ssao.members[i].offset);
          }
          ++ssaoBlocks;
        }
        if (binding.block && binding.set == 0 && binding.binding == 256)
        {
          CHECK(binding.block->size == 256u);
          ++set3Blocks;
        }
      }
      if (reflection.value().pushConstants)
      {
        CHECK(pushed);
        CHECK(reflection.value().pushConstants->size == 256u);
        ++set3Blocks;
      }
      stages.push_back(reflection.value());
      ++checked;
    }
    const auto uncovered = compareLayoutCoverage(stages, *expected);
    CHECK_FALSE(uncovered);
    if (uncovered)
    {
      std::cout << builtin.name << ": " << *uncovered << "\n";
    }
  };

  const auto& gbuffer = *engineBuiltin(EngineBuiltinSmallGeometryBuffer);
  REQUIRE(gbuffer.bindingLayout == L::Gbuffer);
  REQUIRE(engineParmSetOfLayout(gbuffer.bindingLayout) == 3);
  check(gbuffer, true);
  check(gbuffer, false);
  for (const auto& named : EngineNamedPrograms)
  {
    REQUIRE(engineProgram(named.row.index) == &named.row);
    REQUIRE(engineParmSetOfLayout(named.row.bindingLayout) == -1);
    REQUIRE_FALSE(enginePushConstantsEnabled(
      named.row.bindingLayout, enginePushConstantLimit(256), 0x10DE));
    check(named.row, false);
  }
  std::cout << "LP6.5a: reflected and compared " << checked << " permutations, "
            << ssaoBlocks << " SsaoConstants blocks, " << set3Blocks << " set 3 blocks\n";
  // gbuffer's two stages pushed and not, the three compute shaders
  CHECK(checked == 7);
  CHECK(ssaoBlocks == 3);
  CHECK(set3Blocks > 0);
}

TEST_CASE("Retail shaders: bumpyenvironment2 and set 14 (LP6.5b)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the shader test needs the game folder");
  }

  // C72 (SC829, D178): builtin/legacy/bumpyenvironment2 (59), USE_SSR 0, set 14's 512
  // bytes never pushed (pcEnabled false); both stages reflect equal to OCTAHEDRON_CUBE's
  // table (set 0 the block by name and offset, set 1 t0-t6, set 2 s0 s1); at USE_SSR 0
  // the pixel shader declares no t1 (t_ScreenColor) and no t3 (t_Depth): read only
  // under USE_SSR (BE2:394-483), so the layout's t1 and t3 are the items no stage
  // declares, and every other item is declared
  using L = EngineBindingLayout;
  const auto table = engineParmSetBlock(14);
  REQUIRE(table.members.size() == 32u);
  REQUIRE(table.size == 512u);
  const auto& builtin = *engineBuiltin(EngineBuiltinBumpyEnvironment2);
  REQUIRE(builtin.bindingLayout == L::OctahedronCube);
  REQUIRE_FALSE(builtin.requireGPUSkinningSupport);
  REQUIRE_FALSE(
    enginePushConstantsEnabled(L::OctahedronCube, enginePushConstantLimit(4096), 0x10DE));
  const auto expected = engineExpectedLayout(builtin, false);
  REQUIRE(expected);

  auto stages = std::vector<ShaderReflection>{};
  auto blocks = 0;
  for (const auto stage : stagesOf(builtin))
  {
    CAPTURE(shaderStageSuffix(stage));
    const auto blob = readShaderBlob(*gamePath, builtin.name, stage);
    REQUIRE(blob.is_success());
    const auto defines = engineBuiltinDefines(builtin, false);
    const auto keys = shaderBlobKeys(blob.value());
    CHECK(std::ranges::find(keys, shaderPermutationKey(defines)) != keys.end());
    const auto code = findShaderPermutation(blob.value(), defines);
    REQUIRE(code.is_success());
    const auto reflection = reflectSpirv(code.value());
    REQUIRE(reflection.is_success());
    const auto difference = compareReflection(reflection.value(), *expected);
    CHECK_FALSE(difference);
    if (difference)
    {
      std::cout << builtin.name << "." << shaderStageSuffix(stage) << ": " << *difference
                << "\n";
    }
    CHECK_FALSE(reflection.value().pushConstants);
    for (const auto& binding : reflection.value().bindings)
    {
      std::cout << "LP6.5b: " << shaderStageSuffix(stage) << " set " << binding.set
                << " binding " << binding.binding << " "
                << describeDescriptorKind(binding.kind) << " " << binding.name << "\n";
      if (binding.set == 0 && binding.binding == 256 && binding.block)
      {
        const auto& uniforms = *binding.block;
        REQUIRE(uniforms.members.size() == table.members.size());
        for (std::size_t i = 0; i < table.members.size(); ++i)
        {
          CHECK(uniforms.members[i].name == table.members[i].name);
          CHECK(uniforms.members[i].offset == table.members[i].offset);
        }
        CHECK(uniforms.size == 512u);
        ++blocks;
      }
    }
    stages.push_back(reflection.value());
  }
  CHECK(blocks == 2);

  // the coverage: t1 first, then (without it) t3, then (without both) nothing
  const auto uncovered = compareLayoutCoverage(stages, *expected);
  REQUIRE(uncovered);
  CHECK(
    *uncovered
    == "the table's set 1 binding 1 (a sampled image) is declared by no stage");
  auto withoutSsr = *expected;
  withoutSsr.sets[1].bindings.resize(0);
  for (const auto& item : expected->sets[1].bindings)
  {
    if (
      nvrhi::ResourceType(item.type) != nvrhi::ResourceType::Texture_SRV
      || (item.slot != 1u && item.slot != 3u))
    {
      withoutSsr.sets[1].bindings.push_back(item);
    }
  }
  const auto rest = compareLayoutCoverage(stages, withoutSsr);
  CHECK_FALSE(rest);
  if (rest)
  {
    std::cout << builtin.name << ": " << *rest << "\n";
  }
  std::cout << "LP6.5b: bumpyenvironment2 reflected (two stages, " << blocks
            << " set 14 blocks); the layout's items no stage declares: t1, t3\n";
}

} // namespace tb::lit
