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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/NVRHI/RenderProgs_NVRHI.cpp):
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ShaderBlobs.h"

#include "lit/GameFile.h"

#include "kd/result.h"

#include <ShaderMake/ShaderBlob.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <cstring>

namespace tb::lit
{
namespace
{

// ShaderBlob.cpp's g_BlobSignature
constexpr auto BlobSignature = std::string_view{"NVSP"};

// idStr::StripFileExtension: the text from the last '.' after the last separator
std::string_view stripExtension(const std::string_view name)
{
  for (auto i = name.size(); i > 0; --i)
  {
    const auto c = name[i - 1];
    if (c == '/' || c == '\\')
    {
      break;
    }
    if (c == '.')
    {
      return name.substr(0, i - 1);
    }
  }
  return name;
}

std::vector<std::uint32_t> alignedWords(const void* code, const std::size_t size)
{
  auto words = std::vector<std::uint32_t>((size + 3) / 4, 0u);
  if (size > 0)
  {
    std::memcpy(words.data(), code, size);
  }
  return words;
}

} // namespace

std::string_view shaderStageSuffix(const ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return "vs";
  case ShaderStage::Pixel:
    return "ps";
  case ShaderStage::Compute:
    return "cs";
  }
  return "";
}

nvrhi::ShaderType nvrhiShaderType(const ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return nvrhi::ShaderType::Vertex;
  case ShaderStage::Pixel:
    return nvrhi::ShaderType::Pixel;
  case ShaderStage::Compute:
    return nvrhi::ShaderType::Compute;
  }
  return nvrhi::ShaderType::None;
}

std::string shaderPermutationKey(const std::vector<ShaderDefine>& defines)
{
  // FindPermutationInBlob (ShaderBlob.cpp:58-66)
  auto key = std::string{};
  for (std::size_t i = 0; i < defines.size(); ++i)
  {
    key += defines[i].name;
    key += '=';
    key += defines[i].value;
    if (i + 1 < defines.size())
    {
      key += ' ';
    }
  }
  return key;
}

std::string shaderBlobPath(const std::string_view name, const ShaderStage stage)
{
  // RenderProgs_NVRHI.cpp:163-172, Vulkan
  return fmt::format(
    "renderprogs2/spirv/{}.{}.bin", stripExtension(name), shaderStageSuffix(stage));
}

Result<std::string> readShaderBlob(
  const std::filesystem::path& gamePath,
  const std::string_view name,
  const ShaderStage stage)
{
  const auto relative = shaderBlobPath(name, stage);
  const auto relativePath = pathFromUtf8(relative);
  if (!relativePath)
  {
    return Error{fmt::format("The shader name '{}' is not a valid path", name)};
  }
  const auto file = findLooseFile(gamePath / "base" / *relativePath);
  if (!file)
  {
    return Error{fmt::format(
      "The shader blob base/{} is not in the game folder (the engine's shader build "
      "writes "
      "it)",
      relative)};
  }
  return readGameFile(*file) | kdl::or_else([&](const auto& e) -> Result<std::string> {
           return Error{
             fmt::format("Cannot read the shader blob base/{}: {}", relative, e.msg)};
         });
}

bool isPermutationBlob(const std::string_view blob)
{
  return blob.size() >= BlobSignature.size()
         && blob.substr(0, BlobSignature.size()) == BlobSignature;
}

std::vector<std::string> shaderBlobKeys(const std::string_view blob)
{
  auto keys = std::vector<std::string>{};
  if (!isPermutationBlob(blob))
  {
    return keys;
  }

  // EnumeratePermutationsInBlob's walk (ShaderBlob.cpp:101-131), each header copied out
  // of the bytes, which do not align it
  auto rest = blob.substr(BlobSignature.size());
  while (rest.size() > sizeof(ShaderMake::ShaderBlobEntry))
  {
    auto header = ShaderMake::ShaderBlobEntry{};
    std::memcpy(&header, rest.data(), sizeof(header));
    if (header.dataSize == 0)
    {
      break;
    }
    const auto entrySize = std::uint64_t{sizeof(header)} + header.dataSize
                           + std::uint64_t{header.permutationSize};
    if (rest.size() < entrySize)
    {
      break;
    }
    keys.emplace_back(rest.substr(sizeof(header), header.permutationSize));
    rest = rest.substr(std::size_t(entrySize));
  }
  return keys;
}

Result<std::vector<std::uint32_t>> findShaderPermutation(
  const std::string_view blob, const std::vector<ShaderDefine>& defines)
{
  // createShaderPermutation (RenderProgs_NVRHI.cpp:117-135) with LoadShader's constants
  // in the macro list's order (:185-201)
  auto constants = std::vector<ShaderMake::ShaderConstant>{};
  constants.reserve(defines.size());
  for (const auto& define : defines)
  {
    constants.push_back({define.name.c_str(), define.value.c_str()});
  }

  const void* code = nullptr;
  auto size = std::size_t{0};
  if (ShaderMake::FindPermutationInBlob(
        blob.data(),
        blob.size(),
        constants.empty() ? nullptr : constants.data(),
        std::uint32_t(constants.size()),
        &code,
        &size))
  {
    if (size == 0 || size % 4 != 0)
    {
      return Error{
        fmt::format("The SPIR-V code is {} bytes, not a whole number of words", size)};
    }
    return alignedWords(code, size);
  }

  const auto key = shaderPermutationKey(defines);
  if (!isPermutationBlob(blob))
  {
    return Error{fmt::format(
      "The blob is not a permutation blob, but the permutation '{}' is asked for", key)};
  }
  const auto keys = shaderBlobKeys(blob);
  return Error{fmt::format(
    "The blob has no permutation '{}'; it has {}",
    key,
    keys.empty() ? std::string{"none"} : fmt::format("'{}'", fmt::join(keys, "', '")))};
}

nvrhi::ShaderDesc engineShaderDesc(
  const std::string_view name,
  const std::string_view nameOutSuffix,
  const ShaderStage stage)
{
  // RenderProgs_NVRHI.cpp:203-208
  auto desc = nvrhi::ShaderDesc{nvrhiShaderType(stage)};
  desc.debugName = std::string{name} + std::string{nameOutSuffix};
  desc.entryName = "main";
  return desc;
}

} // namespace tb::lit
