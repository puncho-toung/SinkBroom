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

#pragma once

#include "base/Result.h"
#include "lit/EngineTables.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// D9, C3, C26 (P2.19): every engine shader the preview loads is reflected with
// SPIRV-Reflect (the pin SPIRV-Reflect) and compared with the mirrored tables before a
// pipeline is made from it: descriptor sets and bindings (NVRHI's offsets, which equal
// the engine's register shifts), the push-constant block's size, the constant block's
// members by name and offset, the vertex inputs' locations and component counts. A
// mismatch disables the passes that need the program, with a message naming the field.
// All pure; any thread (the compile thread runs it).

namespace tb::lit
{

// a descriptor's Vulkan type, as reflection reports it and as an NVRHI layout item makes
// it
enum class DescriptorKind
{
  Sampler,
  SampledImage,
  StorageImage,
  UniformTexelBuffer,
  StorageTexelBuffer,
  UniformBuffer,
  StorageBuffer,
  Other,
};

std::string_view describeDescriptorKind(DescriptorKind kind);

// the descriptor an NVRHI layout item makes on Vulkan; nullopt for push constants, which
// are no descriptor
std::optional<DescriptorKind> descriptorKindOf(const nvrhi::BindingLayoutItem& item);

struct ReflectedMember
{
  std::string name;
  std::uint32_t offset = 0;

  bool operator==(const ReflectedMember&) const = default;
};

// a constant block; one whose only member is a struct (cbuffer pc { renderParmSet10_t
// pc; }) is given as that struct, as a push-constant ConstantBuffer<T> is
struct ReflectedBlock
{
  std::string name;
  std::uint32_t size = 0;
  std::vector<ReflectedMember> members;

  bool operator==(const ReflectedBlock&) const = default;
};

struct ReflectedBinding
{
  std::uint32_t set = 0;
  std::uint32_t binding = 0;
  DescriptorKind kind = DescriptorKind::Other;
  std::string name;
  // a uniform buffer's block
  std::optional<ReflectedBlock> block;

  bool operator==(const ReflectedBinding&) const = default;
};

// a stage input that is not a built-in (SV_VertexID is one)
struct ReflectedInput
{
  std::uint32_t location = 0;
  std::uint32_t components = 0;
  std::string name;

  bool operator==(const ReflectedInput&) const = default;
};

struct ShaderReflection
{
  bool vertexStage = false;
  std::vector<ReflectedBinding> bindings;
  std::optional<ReflectedBlock> pushConstants;
  // a vertex shader's inputs; empty for the other stages
  std::vector<ReflectedInput> inputs;

  bool operator==(const ShaderReflection&) const = default;
};

/**
 * SPIRV-Reflect over one shader's code: its descriptor bindings with their sets, binding
 * numbers, types and uniform blocks, its push-constant block, and a vertex shader's
 * inputs without the built-ins. The engine's blobs keep their names (SC251: reflection is
 * not stripped). Code SPIRV-Reflect refuses is an error.
 */
Result<ShaderReflection> reflectSpirv(std::span<const std::uint32_t> code);

// what a program's shaders must agree with
struct ExpectedShaderLayout
{
  // the binding layouts in set order
  std::vector<nvrhi::BindingLayoutDesc> sets;
  // the constants the uniforms item holds
  std::optional<EngineConstantBlock> constants;
  // whether the uniforms item is push constants (USE_PUSH_CONSTANTS=1), else a constant
  // buffer at b0 of set 0
  bool pushConstants = false;
  // the vertex layout's locations; empty for a program without a vertex buffer
  std::vector<EngineVertexInput> vertexInputs;
};

/**
 * The layout the tables give a builtin when its binding layout does (pushConstants) or
 * does not push: engineBindingLayoutDescs, engineLayoutConstantBlock and, for
 * LAYOUT_DRAW_ VERT, engineDrawVertInputs. The blit rows (builtin/blit, builtin/rect)
 * take CommonPasses' layout, which always pushes. nullopt for a layout the tables do not
 * mirror.
 */
std::optional<ExpectedShaderLayout> engineExpectedLayout(
  const EngineBuiltin& builtin, bool pushConstants);

/**
 * D9: nullopt when the shader agrees with the layout; else the first field that differs,
 * named: a descriptor whose set, binding or type the layout lacks; a push-constant block
 * where the layout has a constant buffer, or one of another size; a constant block whose
 * members differ by name, offset or count from the table's; a vertex input at a location
 * the vertex layout lacks, or with other components. A shader may leave out what the
 * layout has (the compiler drops what a shader never reads).
 */
std::optional<std::string> compareReflection(
  const ShaderReflection& reflection, const ExpectedShaderLayout& expected);

} // namespace tb::lit
