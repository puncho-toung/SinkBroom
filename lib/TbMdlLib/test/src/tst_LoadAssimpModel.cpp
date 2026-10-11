/*
 Copyright (C) 2023 Daniel Walder
 Copyright (C) 2022 Kristian Duske

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

#include "TestEnvironment.h"
#include "base/Logger.h"
#include "fs/DiskFileSystem.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "gl/Vertex.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityModel.h"
#include "mdl/LoadAssimpModel.h"
#include "mdl/ModelConvention.h"

#include "kd/ranges/contains.h"
#include "kd/ranges/stride_view.h"
#include "kd/vector_utils.h"

#include "vm/approx.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{

TEST_CASE("loadAssimpModel")
{
  auto logger = NullLogger{};

  SECTION("cube")
  {
    const auto basePath = getFixtureRoot() / "test/mdl/LoadAssimpModel/cube";
    auto fs = fs::DiskFileSystem{basePath};

    SECTION("dae")
    {
      auto modelData = loadAssimpModel("cube.dae", fs, logger);
      REQUIRE(modelData);

      CHECK(modelData.value().frameCount() == 1);
      CHECK(modelData.value().surfaceCount() == 1);
      CHECK(modelData.value().surface(0u).skinCount() == 1);
    }

    SECTION("mdl")
    {
      auto modelData = loadAssimpModel("cube.mdl", fs, logger);
      REQUIRE(modelData);

      CHECK(modelData.value().surfaceCount() == 4);
      CHECK(modelData.value().surface(0).skinCount() == 1);
      CHECK(modelData.value().surface(1).skinCount() == 3);
      CHECK(modelData.value().surface(2).skinCount() == 1);
      CHECK(modelData.value().surface(3).skinCount() == 1);
      CHECK(modelData.value().frameCount() == 3);
    }
  }

  SECTION("pitch type")
  {
    const auto [modelPath, expectedPitchType] =
      GENERATE(table<std::filesystem::path, PitchType>({
        {"cube/cube.mdl", PitchType::MdlInverted},
        {"alignment/ase/cuboid.ase", PitchType::Normal},
        {"alignment/obj/cuboid.obj", PitchType::Normal},
        {"alignment/fbx/cuboid.fbx", PitchType::Normal},
        {"alignment/gltf/cuboid.gltf", PitchType::Normal},
        {"alignment/glb/cuboid.glb", PitchType::Normal},
      }));

    CAPTURE(modelPath);

    const auto basePath = getFixtureRoot() / "test/mdl/LoadAssimpModel";
    auto fs = fs::DiskFileSystem{basePath};

    auto modelData = loadAssimpModel(modelPath, fs, logger);
    REQUIRE(modelData);

    CHECK(modelData.value().pitchType() == expectedPitchType);
  }

  SECTION("alignment")
  {
    const auto modelPath = GENERATE(values<std::filesystem::path>({
      "ase/cuboid.ase", // exported with -X forward and +Z up
      "obj/cuboid.obj",
      "fbx/cuboid.fbx", // exported with scale 0.01
      "gltf/cuboid.gltf",
      "glb/cuboid.glb",
    }));

    CAPTURE(modelPath);

    const auto basePath = getFixtureRoot() / "test/mdl/LoadAssimpModel/alignment";
    auto fs = fs::DiskFileSystem{basePath};

    auto modelData = loadAssimpModel(modelPath, fs, logger);
    REQUIRE(modelData);

    REQUIRE(modelData.value().frameCount() == 1);
    REQUIRE(modelData.value().surfaceCount() == 1);
    REQUIRE(modelData.value().surface(0).skinCount() == 1);

    CHECK(vm::approx(modelData.value().bounds(0)) == vm::bbox3f{{0, 0, 0}, {2, 1, 3}});
  }

  SECTION("model with mask texture")
  {
    const auto basePath =
      getFixtureRoot() / "test/mdl/LoadAssimpModel/uncompressedEmbeddedMaskedTexture";
    auto fs = std::make_shared<fs::DiskFileSystem>(basePath);

    auto modelData = loadAssimpModel("masked.mdl", *fs, logger);
    REQUIRE(modelData);

    const auto* texture = modelData.value().surface(0).skin(0)->texture();
    REQUIRE(texture != nullptr);

    const auto& buffers = texture->buffersIfLoaded();
    REQUIRE(!buffers.empty());

    const auto& image = buffers.front();
    REQUIRE(image.size() == 262144);

    const auto pixels = std::ranges::subrange{image.data(), image.data() + image.size()};
    const auto alphas = pixels | std::views::drop(3) | kdl::views::stride(4);

    const auto hasTransparentPixel = kdl::ranges::contains(alphas, 0);
    const auto hasOpaquePixel = kdl::ranges::contains(alphas, 255);

    CHECK(hasTransparentPixel);
    CHECK(hasOpaquePixel);

    // aiTexel carries real per-texel alpha alongside the chroma-key mask, so classify the
    // actual resulting buffer rather than assuming a binary cutout
    const auto hasIntermediateAlpha =
      std::ranges::any_of(alphas, [](const auto a) { return a != 0 && a != 255; });

    const auto* skin = modelData.value().surface(0).skin(0);
    if (hasIntermediateAlpha)
    {
      CHECK(texture->alphaDomain() == img::ImageAlphaDomain::Graduated);
      CHECK(!skin->effectiveAlphaFunc());
      CHECK(
        skin->effectiveBlendFunc().enable == gl::MaterialBlendFunc::Enable::UseFactors);
    }
    else
    {
      CHECK(texture->alphaDomain() == img::ImageAlphaDomain::Binary);
      CHECK(skin->effectiveAlphaFunc());
    }
  }

  SECTION("non ascii diffuse texture path")
  {
    const auto basePath =
      getFixtureRoot() / "test/mdl/LoadAssimpModel/nonAsciiTexturePath";
    auto fs = fs::DiskFileSystem{basePath};

    auto modelData = loadAssimpModel("non_ascii_texture_path.obj", fs, logger);
    REQUIRE(modelData);

    CHECK(modelData.value().frameCount() == 1);
    CHECK(modelData.value().surfaceCount() == 1);
    CHECK(modelData.value().surface(0).skinCount() == 1);
    CHECK(vm::approx(modelData.value().bounds(0)) == vm::bbox3f{{0, 0, 0}, {0, 1, 1}});
  }

  SECTION("malformed diffuse texture path")
  {
    const auto basePath =
      getFixtureRoot() / "test/mdl/LoadAssimpModel/malformedTexturePath";
    auto fs = fs::DiskFileSystem{basePath};

    auto modelData = loadAssimpModel("malformed_texture_path.obj", fs, logger);
    REQUIRE(modelData);

    CHECK(modelData.value().frameCount() == 1);
    CHECK(modelData.value().surfaceCount() == 1);
    CHECK(modelData.value().surface(0).skinCount() == 1);
    CHECK(vm::approx(modelData.value().bounds(0)) == vm::bbox3f{{0, 0, 0}, {0, 1, 1}});
  }

  SECTION("id Tech 4 convention")
  {
    const auto basePath = getFixtureRoot() / "test/mdl/LoadAssimpModel";
    auto fs = fs::DiskFileSystem{basePath};

    auto requestedNames = std::vector<std::filesystem::path>{};
    const auto findDecl =
      [&](const std::filesystem::path& materialName) -> std::optional<gl::Material> {
      requestedNames.push_back(materialName);
      return gl::Material{"resolved decl", gl::createTextureResource(gl::Texture{16, 8})};
    };
    const auto findNothing =
      [&](const std::filesystem::path& materialName) -> std::optional<gl::Material> {
      requestedNames.push_back(materialName);
      return std::nullopt;
    };

    // named.glb is a box from (0, 0, 0) to (4, 2, 1) in glTF coordinates, whose one
    // material, "models/test/wedge", has an embedded 2x2 base colour texture

    SECTION("a material decl named like the glTF material replaces its texture")
    {
      const auto modelData =
        loadAssimpModel(
          "idtech4/named.glb",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findDecl})
        | kdl::value();

      REQUIRE(modelData.surfaceCount() == 1u);
      REQUIRE(modelData.surface(0).skinCount() == 1u);

      const auto* skin = modelData.surface(0).skin(0);
      CHECK(skin->name() == "resolved decl");
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 16u);
      CHECK(requestedNames == std::vector<std::filesystem::path>{"models/test/wedge"});
    }

    SECTION("without such a decl, the embedded texture is shown")
    {
      const auto modelData =
        loadAssimpModel(
          "idtech4/named.glb",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findNothing})
        | kdl::value();

      REQUIRE(modelData.surface(0).skinCount() == 1u);

      // the material is named after the glTF material
      const auto* skin = modelData.surface(0).skin(0);
      CHECK(skin->name() == "models/test/wedge");
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 2u);
      CHECK(skin->texture()->height() == 2u);
      CHECK(requestedNames == std::vector<std::filesystem::path>{"models/test/wedge"});
    }

    SECTION("the default convention shows the embedded texture with assimp's axes")
    {
      const auto modelData =
        loadAssimpModel(
          "idtech4/named.glb",
          fs,
          logger,
          {.convention = ModelConvention::Default, .findMaterial = findDecl})
        | kdl::value();

      REQUIRE(modelData.surface(0).skinCount() == 1u);

      const auto* skin = modelData.surface(0).skin(0);
      CHECK(skin->name() == "models/test/wedge");
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 2u);
      CHECK(requestedNames.empty());

      // (x, y, z) becomes (z, x, y)
      CHECK(vm::approx(modelData.bounds(0)) == vm::bbox3f{{0, 0, 0}, {1, 4, 2}});
    }

    SECTION("glTF models are turned the way the engine turns them")
    {
      // (x, y, z) becomes (x, -z, y), from the accessors' bounds and the node offsets
      const auto [modelPath, expectedBounds] =
        GENERATE(table<std::filesystem::path, vm::bbox3f>({
          {"idtech4/named.glb", vm::bbox3f{{0, -1, 0}, {4, 0, 2}}},
          {"alignment/glb/cuboid.glb", vm::bbox3f{{0, -2, 0}, {1, 0, 3}}},
          {"alignment/gltf/cuboid.gltf", vm::bbox3f{{0, -2, 0}, {1, 0, 3}}},
        }));

      CAPTURE(modelPath);

      const auto modelData =
        loadAssimpModel(modelPath, fs, logger, {.convention = ModelConvention::IdTech4})
        | kdl::value();

      CHECK(modelData.pitchType() == PitchType::Normal);
      CHECK(vm::approx(modelData.bounds(0)) == expectedBounds);
    }

    SECTION("other formats keep their axes")
    {
      const auto modelPath = GENERATE(values<std::filesystem::path>({
        "alignment/obj/cuboid.obj",
        "alignment/fbx/cuboid.fbx",
        "alignment/ase/cuboid.ase",
      }));

      CAPTURE(modelPath);

      const auto modelData =
        loadAssimpModel(modelPath, fs, logger, {.convention = ModelConvention::IdTech4})
        | kdl::value();

      CHECK(vm::approx(modelData.bounds(0)) == vm::bbox3f{{0, 0, 0}, {2, 1, 3}});
    }

    SECTION("assimp's default material is not taken for a decl name")
    {
      // cuboid.obj has no usemtl, so its faces get assimp's DefaultMaterial
      const auto modelData =
        loadAssimpModel(
          "alignment/obj/cuboid.obj",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findDecl})
        | kdl::value();

      CHECK(requestedNames.empty());
      REQUIRE(modelData.surface(0).skinCount() == 1u);
      CHECK(modelData.surface(0).skin(0)->name() == "DefaultMaterial");
    }

    SECTION("glTF texture coordinates are the file's, as the engine reads them")
    {
      const auto modelData =
        loadAssimpModel(
          "idtech4/named.glb", fs, logger, {.convention = ModelConvention::IdTech4})
        | kdl::value();

      // The box corner (0, 2, 0) in glTF coordinates, here (0, 0, 2), has the texture
      // coordinates (0, 1) on the -X face, (0, 0) on the +Y face and (1, 0) on the -Z
      // face of the file. Assimp's glTF importer flips V and FlipUVs flips it back; one
      // flip too many would give (0, 0), (0, 1) and (1, 1).
      auto uvs = std::vector<vm::vec2f>{};
      modelData.surface(0).forEachPrimitive(
        0, [&](const auto*, const auto, const auto vertices) {
          for (const auto& vertex : vertices)
          {
            if (vm::is_equal(
                  gl::getVertexComponent<0>(vertex), vm::vec3f{0, 0, 2}, 0.0001f))
            {
              uvs.push_back(gl::getVertexComponent<1>(vertex));
            }
          }
        });

      CHECK(
        kdl::vec_sort_and_remove_duplicates(std::move(uvs))
        == std::vector<vm::vec2f>{{0, 0}, {0, 1}, {1, 0}});
    }

    SECTION("a glTF material name is canonicalized like a decl name")
    {
      // noncanonical.glb has one material, "Models\MapObjects\Test\Wedge.TGA", on two
      // meshes; each surface owns its skin, so the name is looked up for each mesh
      const auto modelData =
        loadAssimpModel(
          "idtech4/noncanonical.glb",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findDecl})
        | kdl::value();

      CHECK(
        requestedNames
        == std::vector<std::filesystem::path>{
          "models/mapobjects/test/wedge", "models/mapobjects/test/wedge"});
      REQUIRE(modelData.surfaceCount() == 2u);
      for (const auto& surface : modelData.surfaces())
      {
        REQUIRE(surface.skinCount() == 1u);
        CHECK(surface.skin(0)->name() == "resolved decl");
      }
    }

    SECTION("a glTF material that names no decl is looked up once")
    {
      const auto modelData =
        loadAssimpModel(
          "idtech4/noncanonical.glb",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findNothing})
        | kdl::value();

      CHECK(
        requestedNames
        == std::vector<std::filesystem::path>{"models/mapobjects/test/wedge"});
      REQUIRE(modelData.surfaceCount() == 2u);
      for (const auto& surface : modelData.surfaces())
      {
        REQUIRE(surface.skinCount() == 1u);
        CHECK(surface.skin(0)->name() == "Models\\MapObjects\\Test\\Wedge.TGA");
        REQUIRE(surface.skin(0)->texture() != nullptr);
        CHECK(surface.skin(0)->texture()->width() == 2u);
      }
    }

    SECTION("an unnamed glTF material shows its own texture")
    {
      // assimp gives a glTF material without a name no name, so there is nothing to
      // look up
      const auto modelData =
        loadAssimpModel(
          "idtech4/unnamed.glb",
          fs,
          logger,
          {.convention = ModelConvention::IdTech4, .findMaterial = findDecl})
        | kdl::value();

      CHECK(requestedNames.empty());
      REQUIRE(modelData.surface(0).skinCount() == 1u);

      const auto* skin = modelData.surface(0).skin(0);
      CHECK(skin->name().empty());
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 2u);
      CHECK(skin->texture()->height() == 2u);
    }
  }

  SECTION("diffuse texture with wrong extension")
  {
    // Regression test for https://github.com/TrenchBroom/TrenchBroom/issues/4725: the
    // model refers to "texture.tga", but the actual file on the file system is a 5x5
    // "texture.png". The texture must still be found by matching the base name.
    const auto basePath =
      getFixtureRoot() / "test/mdl/LoadAssimpModel/wrongTextureExtension";
    auto fs = fs::DiskFileSystem{basePath};

    auto modelData = loadAssimpModel("model.obj", fs, logger);
    REQUIRE(modelData);

    REQUIRE(modelData.value().surfaceCount() == 1);
    REQUIRE(modelData.value().surface(0).skinCount() == 1);

    const auto* texture = modelData.value().surface(0).skin(0)->texture();
    REQUIRE(texture != nullptr);

    const auto& buffers = texture->buffersIfLoaded();
    REQUIRE(!buffers.empty());

    // A 5x5 RGBA texture has 5 * 5 * 4 bytes. This confirms that "texture.png" was
    // found and loaded instead of the 32x32 default fallback texture.
    CHECK(buffers.front().size() == 5 * 5 * 4);
  }
}

} // namespace tb::mdl
