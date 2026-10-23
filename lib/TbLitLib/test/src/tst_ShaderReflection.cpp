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

#include "lit/ShaderReflection.h"

#include "kd/result.h"

#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

ReflectedBlock blockFrom(const EngineConstantBlock& block)
{
  auto result = ReflectedBlock{std::string{block.name}, block.size, {}};
  for (const auto& member : block.members)
  {
    result.members.push_back({std::string{member.name}, member.offset});
  }
  return result;
}

// interaction.ps as the engine compiles it with USE_PUSH_CONSTANTS=0 (SC233): the
// renderparms at b0 of set 0, t0-t2 in set 1, t3-t4 in set 2, s0-s1 in set 3
ShaderReflection interactionPixelShader()
{
  auto reflection = ShaderReflection{};
  reflection.bindings = {
    {0, 256, DescriptorKind::UniformBuffer, "pc", blockFrom(engineParmSetBlock(10))},
    {1, 0, DescriptorKind::SampledImage, "t_Normal", std::nullopt},
    {1, 1, DescriptorKind::SampledImage, "t_Specular", std::nullopt},
    {1, 2, DescriptorKind::SampledImage, "t_BaseColor", std::nullopt},
    {2, 3, DescriptorKind::SampledImage, "t_LightFalloff", std::nullopt},
    {2, 4, DescriptorKind::SampledImage, "t_LightProjection", std::nullopt},
    {3, 128, DescriptorKind::Sampler, "s_Material", std::nullopt},
    {3, 129, DescriptorKind::Sampler, "s_Lighting", std::nullopt},
  };
  return reflection;
}

// depth.vs with USE_PUSH_CONSTANTS=1: set 0's renderparms pushed, the six VS_IN inputs
ShaderReflection depthVertexShader()
{
  auto reflection = ShaderReflection{};
  reflection.vertexStage = true;
  reflection.pushConstants = blockFrom(engineParmSetBlock(0));
  reflection.inputs = {
    {0, 4, "in.var.POSITION"},
    {1, 2, "in.var.TEXCOORD0"},
    {2, 4, "in.var.NORMAL"},
    {3, 4, "in.var.TANGENT"},
    {4, 4, "in.var.COLOR0"},
    {5, 4, "in.var.COLOR1"},
  };
  return reflection;
}

const EngineBuiltin& row(const int index)
{
  return *engineBuiltin(index);
}

} // namespace

TEST_CASE("ShaderReflection")
{
  SECTION("engineExpectedLayout")
  {
    const auto interaction = engineExpectedLayout(row(EngineBuiltinInteraction), false);
    REQUIRE(interaction);
    CHECK(interaction->sets.size() == 4u);
    CHECK_FALSE(interaction->pushConstants);
    CHECK(interaction->constants->size == 400u);
    CHECK(interaction->vertexInputs.size() == 6u);

    // CommonPasses' blit layout pushes whatever the device says
    const auto blit = engineExpectedLayout(row(EngineBuiltinBlit), false);
    REQUIRE(blit);
    CHECK(blit->pushConstants);
    CHECK(blit->constants->name == "BlitConstants");
    CHECK(blit->vertexInputs.empty());

    const auto tonemapping = engineExpectedLayout(row(EngineBuiltinTonemapping), true);
    REQUIRE(tonemapping);
    CHECK(tonemapping->vertexInputs.empty());

    // LP4.2a (C35): the ambient pass's layout, both paths' programs
    for (const auto program :
         {EngineBuiltinAmbientLightingIbl,
          EngineBuiltinAmbientLightingIblPbr,
          EngineBuiltinAmbientLightgridIbl,
          EngineBuiltinAmbientLightgridIblPbr})
    {
      CAPTURE(program);
      const auto ibl = engineExpectedLayout(row(program), false);
      REQUIRE(ibl);
      CHECK(ibl->sets.size() == 4u);
      CHECK_FALSE(ibl->pushConstants);
      CHECK(ibl->constants->name == "renderParmSet9_t");
      CHECK(ibl->constants->size == 720u);
      CHECK(ibl->constants->members.size() == 45u);
      CHECK(ibl->vertexInputs.size() == 6u);
    }

    // a layout the tables do not mirror
    CHECK_FALSE(engineExpectedLayout(row(2), false));
  }

  SECTION("a shader that agrees with the tables")
  {
    CHECK_FALSE(compareReflection(
      interactionPixelShader(),
      *engineExpectedLayout(row(EngineBuiltinInteraction), false)));
    CHECK_FALSE(compareReflection(
      depthVertexShader(), *engineExpectedLayout(row(EngineBuiltinDepth), true)));
    // a shader may leave out what the layout has
    auto fewer = interactionPixelShader();
    fewer.bindings.erase(fewer.bindings.begin() + 2);
    CHECK_FALSE(compareReflection(
      fewer, *engineExpectedLayout(row(EngineBuiltinInteraction), false)));
    auto noPush = depthVertexShader();
    noPush.pushConstants = std::nullopt;
    CHECK_FALSE(
      compareReflection(noPush, *engineExpectedLayout(row(EngineBuiltinDepth), true)));
  }

  SECTION("T0.5: every item of the table declared by some stage")
  {
    // interaction.ps declares all of the table's items; depth.vs pushes its constants
    const auto interaction = *engineExpectedLayout(row(EngineBuiltinInteraction), false);
    CHECK_FALSE(compareLayoutCoverage({interactionPixelShader()}, interaction));
    CHECK_FALSE(compareLayoutCoverage(
      {depthVertexShader()}, *engineExpectedLayout(row(EngineBuiltinDepth), true)));
    // a table item no stage declares: one stage leaves out a texture, then both
    auto fewer = interactionPixelShader();
    fewer.bindings.erase(fewer.bindings.begin() + 2);
    CHECK_FALSE(compareLayoutCoverage({fewer, interactionPixelShader()}, interaction));
    const auto missing = compareLayoutCoverage({fewer}, interaction);
    REQUIRE(missing);
    CHECK_THAT(
      *missing, Catch::Matchers::ContainsSubstring("set 1 binding 1 (a sampled image)"));
    // pushed constants no stage declares
    auto noPush = depthVertexShader();
    noPush.pushConstants = std::nullopt;
    const auto unpushed = compareLayoutCoverage(
      {noPush}, *engineExpectedLayout(row(EngineBuiltinDepth), true));
    REQUIRE(unpushed);
    CHECK_THAT(*unpushed, Catch::Matchers::ContainsSubstring("no stage declares"));
  }

  SECTION("a swapped member in the table disables the pass")
  {
    auto expected = *engineExpectedLayout(row(EngineBuiltinInteraction), false);
    std::swap(expected.constants->members[2].name, expected.constants->members[3].name);
    CHECK(
      compareReflection(interactionPixelShader(), expected)
      == "the constant buffer at set 0 binding 256 member 2 is rpLocalLightOrigin at 32, the "
         "table's renderParmSet10_t has rpLocalViewOrigin at 32");

    // a member left out of the table
    auto shorter = *engineExpectedLayout(row(EngineBuiltinInteraction), false);
    shorter.constants->members.pop_back();
    CHECK(
      compareReflection(interactionPixelShader(), shorter)
      == "the constant buffer at set 0 binding 256 has 25 members, the table's "
         "renderParmSet10_t has 24");

    // the pushed block of set 0 against a table with a wrong offset
    auto pushed = *engineExpectedLayout(row(EngineBuiltinDepth), true);
    pushed.constants->members[1].offset = 20;
    CHECK(
      compareReflection(depthVertexShader(), pushed)
      == "the push-constant block member 1 is rpColor at 16, the table's renderParmSet0_t has "
         "rpColor at 20");
  }

  SECTION("a wrong set in the table disables the pass")
  {
    // t3 moved to set 1 in the table: the shader's set 2 binding 3 is not there
    auto expected = *engineExpectedLayout(row(EngineBuiltinInteraction), false);
    expected.sets[1].addItem(nvrhi::BindingLayoutItem::Texture_SRV(3));
    expected.sets[2] = nvrhi::BindingLayoutDesc{}
                         .setVisibility(nvrhi::ShaderType::Pixel)
                         .addItem(nvrhi::BindingLayoutItem::Texture_SRV(4));
    CHECK(
      compareReflection(interactionPixelShader(), expected)
      == "descriptor set 2 binding 3 (t_LightFalloff): the table's set 2 has no binding 3");

    // a table with fewer sets
    auto fewerSets = *engineExpectedLayout(row(EngineBuiltinInteraction), false);
    fewerSets.sets.pop_back();
    CHECK(
      compareReflection(interactionPixelShader(), fewerSets)
      == "descriptor set 3 binding 128 (s_Material): the table has 3 descriptor sets");
  }

  SECTION("a descriptor of another type")
  {
    auto reflection = interactionPixelShader();
    reflection.bindings[6].kind = DescriptorKind::SampledImage;
    CHECK(
      compareReflection(reflection, *engineExpectedLayout(row(EngineBuiltinInteraction), false))
      == "descriptor set 3 binding 128 (s_Material) is a sampled image, the table's is a "
         "sampler");
  }

  SECTION("push constants against a constant buffer, and the other way round")
  {
    // the pushed permutation against a table that does not push
    CHECK(
      compareReflection(depthVertexShader(), *engineExpectedLayout(row(EngineBuiltinDepth), false))
      == "a push-constant block of 128 bytes, the table puts the constants in a constant "
         "buffer");
    // the constant-buffer permutation against a table that pushes
    CHECK(
      compareReflection(
        interactionPixelShader(), *engineExpectedLayout(row(EngineBuiltinInteraction), true))
      == "descriptor set 0 binding 256 (pc) is a constant buffer, the table pushes its "
         "constants");
    // a pushed block of another size
    auto bigger = depthVertexShader();
    bigger.pushConstants->size = 144;
    CHECK(
      compareReflection(bigger, *engineExpectedLayout(row(EngineBuiltinDepth), true))
      == "the push-constant block is 144 bytes, the table's renderParmSet0_t is 128");
  }

  SECTION("vertex inputs")
  {
    auto extra = depthVertexShader();
    extra.inputs.push_back({6, 4, "in.var.EXTRA"});
    CHECK(
      compareReflection(extra, *engineExpectedLayout(row(EngineBuiltinDepth), true))
      == "vertex input location 6 (in.var.EXTRA): the table's vertex layout has no location 6");
    auto narrow = depthVertexShader();
    narrow.inputs[0].components = 3;
    CHECK(
      compareReflection(narrow, *engineExpectedLayout(row(EngineBuiltinDepth), true))
      == "vertex input location 0 (in.var.POSITION) reads 3 components, the table's POSITION "
         "has 4");
    // a program without a vertex buffer (tonemapping) reads none
    CHECK(
      compareReflection(
        depthVertexShader(), *engineExpectedLayout(row(EngineBuiltinTonemapping), true))
        .has_value());
  }

  SECTION("descriptorKindOf")
  {
    using Item = nvrhi::BindingLayoutItem;
    CHECK(descriptorKindOf(Item::Texture_SRV(0)) == DescriptorKind::SampledImage);
    CHECK(descriptorKindOf(Item::Sampler(0)) == DescriptorKind::Sampler);
    CHECK(
      descriptorKindOf(Item::VolatileConstantBuffer(0)) == DescriptorKind::UniformBuffer);
    CHECK(
      descriptorKindOf(Item::TypedBuffer_SRV(0)) == DescriptorKind::UniformTexelBuffer);
    CHECK(
      descriptorKindOf(Item::TypedBuffer_UAV(0)) == DescriptorKind::StorageTexelBuffer);
    CHECK_FALSE(descriptorKindOf(Item::PushConstants(0, 16)));
  }

  SECTION("code SPIRV-Reflect refuses")
  {
    // not SPIR-V's magic number
    const auto garbage = std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6};
    CHECK(reflectSpirv(garbage).is_error());
    // shorter than SPIR-V's header
    CHECK(reflectSpirv({}).is_error());
    CHECK(reflectSpirv(std::vector<std::uint32_t>{0x07230203u, 0, 0}).is_error());
    // a header and nothing else: no entry point to reflect
    const auto headerOnly = std::vector<std::uint32_t>{0x07230203u, 0x00010500u, 0, 1, 0};
    CHECK(reflectSpirv(headerOnly).is_error());
  }
}

} // namespace tb::lit
