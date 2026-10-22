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

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The engine's compiled shaders (C1, C2, C26): the blob files of renderprogs2/spirv and
// the permutations ShaderMake stores in them. Mirrored from the engine at 285ea4d1,
// renderer/NVRHI/RenderProgs_NVRHI.cpp (LoadShader, GetBytecode,
// createShaderPermutation); the blob format is ShaderMake's own reader, vendored through
// CPM (mirror.json M-SHADER-*). Every function here is pure or a plain file read: any
// thread.

namespace tb::lit
{

// LoadShader's three stages and their file suffixes "vs", "ps" and "cs" (SC247)
enum class ShaderStage
{
  Vertex,
  Pixel,
  Compute,
};

std::string_view shaderStageSuffix(ShaderStage stage);
nvrhi::ShaderType nvrhiShaderType(ShaderStage stage);

// one define of a permutation, as a builtins row names it (shaderMacro_t)
struct ShaderDefine
{
  std::string name;
  std::string value;

  bool operator==(const ShaderDefine&) const = default;
};

/**
 * SC248: the key ShaderMake stores for a permutation, as FindPermutationInBlob builds it:
 * NAME=VALUE pairs in the given order, joined by single spaces; empty for none.
 */
std::string shaderPermutationKey(const std::vector<ShaderDefine>& defines);

/**
 * C1, SC247: a shader's blob below the game's base folder:
 * renderprogs2/spirv/<name without its extension>.<stage>.bin.
 */
std::string shaderBlobPath(std::string_view name, ShaderStage stage);

/**
 * C1, C26: a shader's blob file, read whole: the loose file <gamePath>/base/
 * <shaderBlobPath>, found as the engine's OpenOSFile finds a loose file (its case
 * ignored; lit/GameFile.h findLooseFile) and read as GameFile reads (sharing write and
 * delete access). Unlike the engine's GetBytecode, which reads through ReadFile, no .pk4
 * or .resources container is searched: the loose files are the engine's own build output
 * (D6, R-A18; mirror.json M-SHADER-DIFF-01). A missing or unreadable file is an error
 * that disables the passes that need the shader (D9), never a fatal one; so is an empty
 * or relative game path (T0.4), which would otherwise be read from the process's working
 * folder.
 */
Result<std::string> readShaderBlob(
  const std::filesystem::path& gamePath, std::string_view name, ShaderStage stage);

// whether a blob is a ShaderMake permutation container ("NVSP"), not raw SPIR-V
bool isPermutationBlob(std::string_view blob);

/**
 * The keys of a permutation container in file order, walked as FindPermutationInBlob
 * walks it, each entry's header read through an aligned copy (P2.22): the walk stops at
 * an entry with no code or one that runs past the end, as the reader's does. Empty for a
 * blob that is not a container.
 */
std::vector<std::string> shaderBlobKeys(std::string_view blob);

/**
 * C2: the SPIR-V of the permutation that the defines' key names, found by ShaderMake's
 * FindPermutationInBlob; a blob that is not a container is taken whole only when no
 * define is asked for (the raw rect.vs). The code is copied into word-aligned storage,
 * since an entry's code may start at any byte offset. A permutation that is not found is
 * an error that names the key asked for and the keys the blob has (D9).
 */
Result<std::vector<std::uint32_t>> findShaderPermutation(
  std::string_view blob, const std::vector<ShaderDefine>& defines);

/**
 * SC247: the ShaderDesc LoadShader gives a shader: its stage's type, entry point "main",
 * debug name = name + nameOutSuffix.
 */
nvrhi::ShaderDesc engineShaderDesc(
  std::string_view name, std::string_view nameOutSuffix, ShaderStage stage);

} // namespace tb::lit
