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

#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/EngineTables.h"
#include "lit/ShaderBlobs.h"

#include "kd/result.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

// a SPIR-V stand-in: the magic number, then words that tell the permutations apart
std::vector<std::uint32_t> fakeCode(const std::uint32_t tag, const std::size_t words)
{
  auto code = std::vector<std::uint32_t>(words, tag);
  code[0] = 0x07230203u;
  return code;
}

std::string bytesOf(const std::vector<std::uint32_t>& code)
{
  auto bytes = std::string(code.size() * 4, '\0');
  std::memcpy(bytes.data(), code.data(), bytes.size());
  return bytes;
}

void appendUint32(std::string& bytes, const std::uint32_t value)
{
  for (auto shift = 0; shift < 32; shift += 8)
  {
    bytes += char((value >> shift) & 0xFFu);
  }
}

// a ShaderMake permutation blob ("NVSP", then per entry the key's size, the code's size,
// the key and the code), written byte by byte, so that the reader is tested against the
// format, not against the writer
std::string nvspBlob(
  const std::vector<std::pair<std::string, std::vector<std::uint32_t>>>& permutations)
{
  auto bytes = std::string{"NVSP"};
  for (const auto& [key, code] : permutations)
  {
    appendUint32(bytes, std::uint32_t(key.size()));
    appendUint32(bytes, std::uint32_t(code.size() * 4));
    bytes += key;
    bytes += bytesOf(code);
  }
  return bytes;
}

void writeBinaryFile(const std::filesystem::path& path, const std::string& bytes)
{
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream{path, std::ios::binary};
  stream.write(bytes.data(), std::streamsize(bytes.size()));
}

// the message of an error, empty for a value
template <typename T>
std::string errorMessage(const Result<T>& result)
{
  return result.is_error() ? std::get<Error>(result.error()).msg : std::string{};
}

const auto Interaction = std::vector<ShaderDefine>{
  {"USE_GPU_SKINNING", "0"},
  {"USE_PBR", "1"},
  {"USE_PUSH_CONSTANTS", "0"},
};

} // namespace

TEST_CASE("ShaderBlobs")
{
  SECTION("shaderPermutationKey")
  {
    // SC248: the defines in the given order, joined by single spaces
    CHECK(
      shaderPermutationKey(Interaction)
      == "USE_GPU_SKINNING=0 USE_PBR=1 USE_PUSH_CONSTANTS=0");
    CHECK(shaderPermutationKey({{"TEXTURE_ARRAY", "0"}}) == "TEXTURE_ARRAY=0");
    CHECK(shaderPermutationKey({}).empty());
  }

  SECTION("shaderBlobPath")
  {
    CHECK(
      shaderBlobPath("builtin/lighting/interaction", ShaderStage::Pixel)
      == "renderprogs2/spirv/builtin/lighting/interaction.ps.bin");
    CHECK(
      shaderBlobPath("builtin/depth", ShaderStage::Vertex)
      == "renderprogs2/spirv/builtin/depth.vs.bin");
    CHECK(
      shaderBlobPath("builtin/post/exposure", ShaderStage::Compute)
      == "renderprogs2/spirv/builtin/post/exposure.cs.bin");
    // StripFileExtension: the last extension of the last component only
    CHECK(
      shaderBlobPath("builtin/depth.hlsl", ShaderStage::Vertex)
      == "renderprogs2/spirv/builtin/depth.vs.bin");
    CHECK(
      shaderBlobPath("a.b/depth", ShaderStage::Vertex)
      == "renderprogs2/spirv/a.b/depth.vs.bin");
  }

  SECTION("findShaderPermutation and shaderBlobKeys")
  {
    // keys of odd lengths, so that the second entry's header and both codes lie off a
    // four-byte boundary
    const auto pbr = fakeCode(1, 7);
    const auto legacy = fakeCode(2, 5);
    const auto blob = nvspBlob({
      {"USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0", legacy},
      {"USE_GPU_SKINNING=0 USE_PBR=1 USE_PUSH_CONSTANTS=0", pbr},
    });
    REQUIRE(isPermutationBlob(blob));

    CHECK(
      shaderBlobKeys(blob)
      == std::vector<std::string>{
        "USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0",
        "USE_GPU_SKINNING=0 USE_PBR=1 USE_PUSH_CONSTANTS=0"});

    CHECK(
      findShaderPermutation(blob, Interaction)
      == Result<std::vector<std::uint32_t>>{pbr});
    auto legacyDefines = Interaction;
    legacyDefines[1].value = "0";
    CHECK(
      findShaderPermutation(blob, legacyDefines)
      == Result<std::vector<std::uint32_t>>{legacy});

    // the key follows the table's order: the same defines in another order miss
    const auto reordered = std::vector<ShaderDefine>{
      {"USE_PBR", "1"},
      {"USE_GPU_SKINNING", "0"},
      {"USE_PUSH_CONSTANTS", "0"},
    };
    const auto missed = findShaderPermutation(blob, reordered);
    REQUIRE(missed.is_error());
    CHECK_THAT(
      errorMessage(missed),
      Catch::Matchers::ContainsSubstring(
        "no permutation 'USE_PBR=1 USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=0'"));
    CHECK_THAT(
      errorMessage(missed),
      Catch::Matchers::ContainsSubstring(
        "'USE_GPU_SKINNING=0 USE_PBR=0 USE_PUSH_CONSTANTS=0', "
        "'USE_GPU_SKINNING=0 USE_PBR=1 USE_PUSH_CONSTANTS=0'"));

    // a missing permutation
    auto pushConstants = Interaction;
    pushConstants[2].value = "1";
    CHECK(findShaderPermutation(blob, pushConstants).is_error());
    // no define at all asks for the empty key, which a container does not hold
    CHECK(findShaderPermutation(blob, {}).is_error());
  }

  SECTION("a cut blob ends the walk as the reader's does")
  {
    const auto blob = nvspBlob({{"A=0", fakeCode(1, 4)}, {"A=1", fakeCode(2, 4)}});
    const auto cut = blob.substr(0, blob.size() - 3);
    CHECK(shaderBlobKeys(cut) == std::vector<std::string>{"A=0"});
    CHECK(findShaderPermutation(cut, {{"A", "1"}}).is_error());
    CHECK(findShaderPermutation(cut, {{"A", "0"}}).is_success());
  }

  SECTION("a raw SPIR-V file is taken only without defines")
  {
    const auto code = fakeCode(3, 9);
    const auto raw = bytesOf(code);
    CHECK_FALSE(isPermutationBlob(raw));
    CHECK(shaderBlobKeys(raw).empty());
    CHECK(findShaderPermutation(raw, {}) == Result<std::vector<std::uint32_t>>{code});
    const auto refused = findShaderPermutation(raw, {{"TEXTURE_ARRAY", "0"}});
    REQUIRE(refused.is_error());
    CHECK_THAT(
      errorMessage(refused),
      Catch::Matchers::ContainsSubstring("not a permutation blob"));
  }

  SECTION("engineShaderDesc")
  {
    const auto desc =
      engineShaderDesc("builtin/lighting/interaction", "_PBR", ShaderStage::Pixel);
    CHECK(desc.shaderType == nvrhi::ShaderType::Pixel);
    CHECK(desc.entryName == "main");
    CHECK(desc.debugName == "builtin/lighting/interaction_PBR");
    CHECK(
      engineShaderDesc("builtin/post/exposure", "", ShaderStage::Compute).shaderType
      == nvrhi::ShaderType::Compute);
    CHECK(
      engineShaderDesc("builtin/rect", "", ShaderStage::Vertex).shaderType
      == nvrhi::ShaderType::Vertex);
  }
}

TEST_CASE("readShaderBlob")
{
  const auto env = fs::TestEnvironment{};
  const auto spirv = env.dir() / "base" / "renderprogs2" / "spirv" / "builtin";
  const auto blob =
    nvspBlob({{"USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=0", fakeCode(4, 4)}});

  SECTION("a loose file, whatever the case of its name")
  {
    writeBinaryFile(spirv / "Depth.VS.bin", blob);
#if defined(_WIN32)
    CHECK(
      readShaderBlob(env.dir(), "builtin/depth", ShaderStage::Vertex)
      == Result<std::string>{blob});
#endif
    CHECK(
      readShaderBlob(env.dir(), "builtin/Depth", ShaderStage::Vertex)
      == Result<std::string>{blob});
  }

  SECTION("a missing file names its path")
  {
    const auto missing = readShaderBlob(env.dir(), "builtin/depth", ShaderStage::Pixel);
    REQUIRE(missing.is_error());
    CHECK_THAT(
      errorMessage(missing),
      Catch::Matchers::ContainsSubstring("base/renderprogs2/spirv/builtin/depth.ps.bin"));
  }

  SECTION("containers are not searched (C1)")
  {
    // the engine's ReadFile would find it in the .pk4 (mirror.json M-SHADER-DIFF-01)
    writePk4(
      env.dir() / "base" / "shaders.pk4",
      {{"renderprogs2/spirv/builtin/depth.vs.bin", blob}});
    CHECK(readShaderBlob(env.dir(), "builtin/depth", ShaderStage::Vertex).is_error());
  }

  SECTION("only base is searched")
  {
    writeBinaryFile(
      env.dir() / "mymod" / "renderprogs2" / "spirv" / "builtin" / "depth.vs.bin", blob);
    CHECK(readShaderBlob(env.dir(), "builtin/depth", ShaderStage::Vertex).is_error());
  }

  SECTION("T0.4: an empty or relative game path reads nothing")
  {
    // a blob that a working-folder-relative read would find
    writeBinaryFile(spirv / "depth.vs.bin", blob);
    const auto empty = readShaderBlob({}, "builtin/depth", ShaderStage::Vertex);
    REQUIRE(empty.is_error());
    CHECK(
      errorMessage(empty) == "the game path is empty, so no shader blob is read (C1)");
    const auto relative =
      readShaderBlob(env.dir().filename(), "builtin/depth", ShaderStage::Vertex);
    REQUIRE(relative.is_error());
    CHECK_THAT(
      errorMessage(relative),
      Catch::Matchers::EndsWith("is relative, so no shader blob is read (C1)"));
  }
}

TEST_CASE("shaderBlobExists and the material programs' lookup (LP5.0, C47)")
{
  const auto env = fs::TestEnvironment{};
  const auto spirv = env.dir() / "base" / "renderprogs2" / "spirv";
  const auto blob =
    nvspBlob({{"USE_GPU_SKINNING=0 USE_PUSH_CONSTANTS=1", fakeCode(4, 4)}});
  // vornoipattern has both blobs, heathaze its vertex blob alone, colorProcess none
  writeBinaryFile(spirv / "vornoipattern.vs.bin", blob);
  writeBinaryFile(spirv / "VornoiPattern.PS.bin", blob);
  writeBinaryFile(spirv / "heathaze.vs.bin", blob);

  SECTION("shaderBlobExists finds what readShaderBlob reads, without reading it")
  {
    CHECK(shaderBlobExists(env.dir(), "vornoipattern", ShaderStage::Vertex));
    CHECK(shaderBlobExists(env.dir(), "vornoipattern.vfp", ShaderStage::Vertex));
#if defined(_WIN32)
    CHECK(shaderBlobExists(env.dir(), "vornoipattern", ShaderStage::Pixel));
#endif
    CHECK(shaderBlobExists(env.dir(), "VornoiPattern", ShaderStage::Pixel));
    CHECK_FALSE(shaderBlobExists(env.dir(), "heathaze", ShaderStage::Pixel));
    CHECK_FALSE(shaderBlobExists(env.dir(), "vornoipattern", ShaderStage::Compute));
    // T0.4's rule: no empty or relative game path
    CHECK_FALSE(shaderBlobExists({}, "vornoipattern", ShaderStage::Vertex));
    CHECK_FALSE(
      shaderBlobExists(env.dir().filename(), "vornoipattern", ShaderStage::Vertex));
    CHECK(shaderNameWithoutExtension("heatHaze.vfp") == "heatHaze");
    CHECK(shaderNameWithoutExtension("a.b/c") == "a.b/c");
  }

  SECTION("a program named, in the table, with both blobs (D9)")
  {
    const auto voronoi = engineMaterialProgram(env.dir(), "VORNOIPATTERN.vfp");
    REQUIRE(voronoi);
    CHECK(std::string_view{voronoi->row.name} == "vornoipattern");
    CHECK(voronoi->row.index == EngineMaterialProgramBase + 11);
    CHECK_FALSE(voronoi->readsCurrentRender);
    // a blob missing, no blob, not in the table
    CHECK_FALSE(engineMaterialProgram(env.dir(), "heathaze"));
    CHECK_FALSE(engineMaterialProgram(env.dir(), "colorProcess"));
    CHECK_FALSE(engineMaterialProgram(env.dir(), "myOwnProgram"));
    writeBinaryFile(spirv / "myOwnProgram.vs.bin", blob);
    writeBinaryFile(spirv / "myOwnProgram.ps.bin", blob);
    CHECK_FALSE(engineMaterialProgram(env.dir(), "myOwnProgram"));
    CHECK_FALSE(engineMaterialProgram({}, "vornoipattern"));
  }
}

} // namespace tb::lit
