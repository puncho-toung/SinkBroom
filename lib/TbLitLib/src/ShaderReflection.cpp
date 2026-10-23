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

#include <fmt/format.h>
#include <spirv_reflect.h>

#include <algorithm>

namespace tb::lit
{
namespace
{

// SPIRV-Reflect's module, destroyed with the scope
class ReflectModule
{
private:
  SpvReflectShaderModule m_module{};
  bool m_valid = false;

public:
  explicit ReflectModule(const std::span<const std::uint32_t> code)
  {
    m_valid =
      spvReflectCreateShaderModule2(
        SPV_REFLECT_MODULE_FLAG_NO_COPY, code.size_bytes(), code.data(), &m_module)
      == SPV_REFLECT_RESULT_SUCCESS;
  }

  ~ReflectModule()
  {
    if (m_valid)
    {
      spvReflectDestroyShaderModule(&m_module);
    }
  }

  ReflectModule(const ReflectModule&) = delete;
  ReflectModule& operator=(const ReflectModule&) = delete;

  bool valid() const { return m_valid; }
  SpvReflectShaderModule& get() { return m_module; }
};

DescriptorKind kindOf(const SpvReflectDescriptorType type)
{
  switch (type)
  {
  case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
    return DescriptorKind::Sampler;
  case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    return DescriptorKind::SampledImage;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    return DescriptorKind::StorageImage;
  case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    return DescriptorKind::UniformTexelBuffer;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
    return DescriptorKind::StorageTexelBuffer;
  case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
  case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    return DescriptorKind::UniformBuffer;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
    return DescriptorKind::StorageBuffer;
  default:
    return DescriptorKind::Other;
  }
}

std::string nameOf(const char* name)
{
  return name ? std::string{name} : std::string{};
}

// a block as its members; one struct member stands for the block (cbuffer pc { T pc; })
ReflectedBlock blockOf(const SpvReflectBlockVariable& block)
{
  const auto* source = &block;
  if (block.member_count == 1 && block.members && block.members[0].member_count > 0)
  {
    source = &block.members[0];
  }
  auto result = ReflectedBlock{nameOf(source->name), source->size, {}};
  for (std::uint32_t i = 0; i < source->member_count; ++i)
  {
    const auto& member = source->members[i];
    result.members.push_back({nameOf(member.name), member.offset});
  }
  return result;
}

std::uint32_t componentsOf(const SpvReflectInterfaceVariable& variable)
{
  const auto count = variable.numeric.vector.component_count;
  return count == 0 ? 1u : count;
}

std::string bindingName(const ReflectedBinding& binding)
{
  return binding.name.empty() ? std::string{} : fmt::format(" ({})", binding.name);
}

// the binding layout item at this set and Vulkan binding, or null
const nvrhi::BindingLayoutItem* itemAt(
  const ExpectedShaderLayout& expected,
  const std::uint32_t set,
  const std::uint32_t binding)
{
  if (set >= expected.sets.size())
  {
    return nullptr;
  }
  for (const auto& item : expected.sets[set].bindings)
  {
    if (descriptorKindOf(item) && engineVulkanBinding(item) == binding)
    {
      return &item;
    }
  }
  return nullptr;
}

std::optional<std::string> compareBlock(
  const std::string_view what,
  const ReflectedBlock& block,
  const EngineConstantBlock& expected)
{
  const auto count = std::min(block.members.size(), expected.members.size());
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto& member = block.members[i];
    const auto& want = expected.members[i];
    if (member.name != want.name || member.offset != want.offset)
    {
      return fmt::format(
        "{} member {} is {} at {}, the table's {} has {} at {}",
        what,
        i,
        member.name,
        member.offset,
        expected.name,
        want.name,
        want.offset);
    }
  }
  if (block.members.size() != expected.members.size())
  {
    return fmt::format(
      "{} has {} members, the table's {} has {}",
      what,
      block.members.size(),
      expected.name,
      expected.members.size());
  }
  if (block.size != expected.size)
  {
    return fmt::format(
      "{} is {} bytes, the table's {} is {}",
      what,
      block.size,
      expected.name,
      expected.size);
  }
  return std::nullopt;
}

} // namespace

std::string_view describeDescriptorKind(const DescriptorKind kind)
{
  switch (kind)
  {
  case DescriptorKind::Sampler:
    return "a sampler";
  case DescriptorKind::SampledImage:
    return "a sampled image";
  case DescriptorKind::StorageImage:
    return "a storage image";
  case DescriptorKind::UniformTexelBuffer:
    return "a uniform texel buffer";
  case DescriptorKind::StorageTexelBuffer:
    return "a storage texel buffer";
  case DescriptorKind::UniformBuffer:
    return "a constant buffer";
  case DescriptorKind::StorageBuffer:
    return "a storage buffer";
  case DescriptorKind::Other:
    break;
  }
  return "another descriptor";
}

std::optional<DescriptorKind> descriptorKindOf(const nvrhi::BindingLayoutItem& item)
{
  // nvrhi src/vulkan/vulkan-resource-bindings.cpp's descriptor types
  using T = nvrhi::ResourceType;
  switch (T(item.type))
  {
  case T::Texture_SRV:
    return DescriptorKind::SampledImage;
  case T::Texture_UAV:
    return DescriptorKind::StorageImage;
  case T::TypedBuffer_SRV:
    return DescriptorKind::UniformTexelBuffer;
  case T::TypedBuffer_UAV:
    return DescriptorKind::StorageTexelBuffer;
  case T::StructuredBuffer_SRV:
  case T::StructuredBuffer_UAV:
  case T::RawBuffer_SRV:
  case T::RawBuffer_UAV:
    return DescriptorKind::StorageBuffer;
  case T::ConstantBuffer:
  case T::VolatileConstantBuffer:
    return DescriptorKind::UniformBuffer;
  case T::Sampler:
    return DescriptorKind::Sampler;
  case T::PushConstants:
  case T::None:
    return std::nullopt;
  default:
    return DescriptorKind::Other;
  }
}

Result<ShaderReflection> reflectSpirv(const std::span<const std::uint32_t> code)
{
  // SPIR-V's five-word header; SPIRV-Reflect reads the generator word before it checks
  // the size (spirv_reflect.c CreateShaderModule)
  if (code.size() < 5)
  {
    return Error{fmt::format("{} words are too few to be SPIR-V", code.size())};
  }
  auto module = ReflectModule{code};
  if (!module.valid())
  {
    return Error{"SPIRV-Reflect cannot read the shader's code"};
  }
  auto& reflected = module.get();
  if (reflected.entry_point_count == 0)
  {
    return Error{"The shader's code has no entry point"};
  }
  auto result = ShaderReflection{};
  result.vertexStage = reflected.shader_stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT;

  auto count = std::uint32_t{0};
  if (
    spvReflectEnumerateDescriptorBindings(&reflected, &count, nullptr)
    != SPV_REFLECT_RESULT_SUCCESS)
  {
    return Error{"SPIRV-Reflect cannot list the shader's descriptors"};
  }
  auto bindings = std::vector<SpvReflectDescriptorBinding*>(count);
  spvReflectEnumerateDescriptorBindings(&reflected, &count, bindings.data());
  for (const auto* binding : bindings)
  {
    auto entry = ReflectedBinding{
      binding->set,
      binding->binding,
      kindOf(binding->descriptor_type),
      nameOf(binding->name),
      {}};
    if (entry.kind == DescriptorKind::UniformBuffer)
    {
      entry.block = blockOf(binding->block);
    }
    result.bindings.push_back(std::move(entry));
  }
  std::sort(
    result.bindings.begin(), result.bindings.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.set != rhs.set ? lhs.set < rhs.set : lhs.binding < rhs.binding;
    });

  count = 0;
  if (
    spvReflectEnumeratePushConstantBlocks(&reflected, &count, nullptr)
    != SPV_REFLECT_RESULT_SUCCESS)
  {
    return Error{"SPIRV-Reflect cannot list the shader's push constants"};
  }
  if (count > 1)
  {
    return Error{fmt::format("The shader has {} push-constant blocks", count)};
  }
  if (count == 1)
  {
    auto* block = static_cast<SpvReflectBlockVariable*>(nullptr);
    spvReflectEnumeratePushConstantBlocks(&reflected, &count, &block);
    result.pushConstants = blockOf(*block);
  }

  if (result.vertexStage)
  {
    count = 0;
    if (
      spvReflectEnumerateInputVariables(&reflected, &count, nullptr)
      != SPV_REFLECT_RESULT_SUCCESS)
    {
      return Error{"SPIRV-Reflect cannot list the shader's inputs"};
    }
    auto inputs = std::vector<SpvReflectInterfaceVariable*>(count);
    spvReflectEnumerateInputVariables(&reflected, &count, inputs.data());
    for (const auto* input : inputs)
    {
      if (input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN)
      {
        continue;
      }
      result.inputs.push_back(
        {input->location, componentsOf(*input), nameOf(input->name)});
    }
    std::sort(
      result.inputs.begin(), result.inputs.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.location < rhs.location;
      });
  }
  return result;
}

std::optional<ExpectedShaderLayout> engineExpectedLayout(
  const EngineBuiltin& builtin, const bool pushConstants)
{
  const auto layout = builtin.bindingLayout;
  // CommonPasses' blit layout always pushes (CommonPasses.cpp:166-177)
  const auto pushed = layout == EngineBindingLayout::Blit ? true : pushConstants;
  auto sets = engineBindingLayoutDescs(layout, pushed);
  if (!sets)
  {
    return std::nullopt;
  }
  auto expected = ExpectedShaderLayout{};
  expected.sets = std::move(*sets);
  expected.constants = engineLayoutConstantBlock(layout);
  expected.pushConstants = pushed;
  if (builtin.vertexLayout == EngineVertexLayout::DrawVert)
  {
    const auto inputs = engineDrawVertInputs();
    expected.vertexInputs.assign(inputs.begin(), inputs.end());
  }
  return expected;
}

std::optional<std::string> compareReflection(
  const ShaderReflection& reflection, const ExpectedShaderLayout& expected)
{
  // descriptors: every one the shader declares is in the table, of the table's type
  for (const auto& binding : reflection.bindings)
  {
    if (binding.set >= expected.sets.size())
    {
      return fmt::format(
        "descriptor set {} binding {}{}: the table has {} descriptor sets",
        binding.set,
        binding.binding,
        bindingName(binding),
        expected.sets.size());
    }
    const auto* item = itemAt(expected, binding.set, binding.binding);
    if (!item)
    {
      if (
        binding.kind == DescriptorKind::UniformBuffer && binding.set == 0
        && binding.binding == EngineVulkanBindingOffsets.constantBuffer
        && expected.pushConstants)
      {
        return fmt::format(
          "descriptor set 0 binding {}{} is a constant buffer, the table pushes its "
          "constants",
          binding.binding,
          bindingName(binding));
      }
      return fmt::format(
        "descriptor set {} binding {}{}: the table's set {} has no binding {}",
        binding.set,
        binding.binding,
        bindingName(binding),
        binding.set,
        binding.binding);
    }
    const auto kind = descriptorKindOf(*item);
    if (kind != binding.kind)
    {
      return fmt::format(
        "descriptor set {} binding {}{} is {}, the table's is {}",
        binding.set,
        binding.binding,
        bindingName(binding),
        describeDescriptorKind(binding.kind),
        describeDescriptorKind(*kind));
    }
    if (
      binding.kind == DescriptorKind::UniformBuffer && binding.block
      && expected.constants)
    {
      if (
        const auto difference = compareBlock(
          fmt::format(
            "the constant buffer at set {} binding {}", binding.set, binding.binding),
          *binding.block,
          *expected.constants))
      {
        return difference;
      }
    }
  }

  // push constants
  if (reflection.pushConstants)
  {
    if (!expected.pushConstants)
    {
      return fmt::format(
        "a push-constant block of {} bytes, the table puts the constants in a constant "
        "buffer",
        reflection.pushConstants->size);
    }
    if (expected.constants)
    {
      if (
        const auto difference = compareBlock(
          "the push-constant block", *reflection.pushConstants, *expected.constants))
      {
        return difference;
      }
    }
  }

  // vertex inputs, by location
  for (const auto& input : reflection.inputs)
  {
    const auto it = std::find_if(
      expected.vertexInputs.begin(), expected.vertexInputs.end(), [&](const auto& want) {
        return want.location == input.location;
      });
    if (it == expected.vertexInputs.end())
    {
      return fmt::format(
        "vertex input location {} ({}): the table's vertex layout has no location {}",
        input.location,
        input.name,
        input.location);
    }
    if (it->components != input.components)
    {
      return fmt::format(
        "vertex input location {} ({}) reads {} components, the table's {} has {}",
        input.location,
        input.name,
        input.components,
        it->semantic,
        it->components);
    }
  }
  return std::nullopt;
}

std::optional<std::string> compareLayoutCoverage(
  const std::vector<ShaderReflection>& stages, const ExpectedShaderLayout& expected)
{
  for (std::uint32_t set = 0; set < expected.sets.size(); ++set)
  {
    for (const auto& item : expected.sets[set].bindings)
    {
      if (nvrhi::ResourceType(item.type) == nvrhi::ResourceType::PushConstants)
      {
        if (!std::ranges::any_of(
              stages, [](const auto& stage) { return stage.pushConstants.has_value(); }))
        {
          return fmt::format(
            "the table's set {} pushes {} bytes of constants, no stage declares a "
            "push-constant block",
            set,
            std::uint32_t(item.size));
        }
        continue;
      }
      const auto kind = descriptorKindOf(item);
      if (!kind)
      {
        continue;
      }
      const auto binding = engineVulkanBinding(item);
      const auto declared = std::ranges::any_of(stages, [&](const auto& stage) {
        return std::ranges::any_of(stage.bindings, [&](const auto& b) {
          return b.set == set && b.binding == binding && b.kind == *kind;
        });
      });
      if (!declared)
      {
        return fmt::format(
          "the table's set {} binding {} ({}) is declared by no stage",
          set,
          binding,
          describeDescriptorKind(*kind));
      }
    }
  }
  return std::nullopt;
}

} // namespace tb::lit
