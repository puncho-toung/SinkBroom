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

#include "TestEnvironment.h"
#include "base/Logger.h"
#include "fs/DiskFileSystem.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityModel.h"
#include "mdl/GameConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LoadEntityModel.h"
#include "mdl/MaterialUtils.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox_io.h" // IWYU pragma: keep

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("LoadEntityModel")
{
  auto logger = NullLogger{};
  auto fs = fs::DiskFileSystem{getFixtureRoot() / "test/mdl/LoadIdTech4ObjModel"};
  const auto& materialConfig = Doom3BFGGameInfo.gameConfig.materialConfig;

  // written like RBDOOM-3-BFG's ExportOBJ, with raw Z-up positions
  const auto path = std::filesystem::path{"_tb/models/mapobjects/test/wedge.obj"};

  const auto loadMaterial = [&](const std::filesystem::path& materialPath) {
    return loadDefaultMaterial(fs, materialPath.generic_string(), logger);
  };

  auto requestedNames = std::vector<std::filesystem::path>{};
  const auto findMaterial =
    [&](const std::filesystem::path& materialName) -> std::optional<gl::Material> {
    requestedNames.push_back(materialName);
    return std::nullopt;
  };

  SECTION("loadEntityModelSync")
  {
    SECTION("the id Tech 4 convention reads a .obj file as the engine does")
    {
      const auto model = loadEntityModelSync(
                           fs,
                           materialConfig,
                           ModelConvention::IdTech4,
                           path,
                           loadMaterial,
                           findMaterial,
                           logger)
                         | kdl::value();

      REQUIRE(model.data() != nullptr);
      CHECK(model.data()->bounds(0) == vm::bbox3f{{0, 0, -8}, {64, 32, 16}});
      // two usemtl names and the default skin
      CHECK(model.data()->surface(0).skinCount() == 3u);
      CHECK(
        requestedNames
        == std::vector<std::filesystem::path>{
          "models/mapobjects/test/wedge", "textures/base_wall/lfwall27d"});
    }

    SECTION("the default convention leaves a .obj file to assimp")
    {
      const auto model = loadEntityModelSync(
                           fs,
                           materialConfig,
                           ModelConvention::Default,
                           path,
                           loadMaterial,
                           findMaterial,
                           logger)
                         | kdl::value();

      // assimp takes the file for Y-up and turns (x, y, z) into (z, x, y), which lays the
      // engine's Z-up proxies on their side; that is why the native loader is gated
      REQUIRE(model.data() != nullptr);
      CHECK(vm::approx(model.data()->bounds(0)) == vm::bbox3f{{-8, 0, 0}, {16, 64, 32}});
      CHECK(requestedNames.empty());
    }

    SECTION("a glTF model with and without the id Tech 4 convention")
    {
      // a box from (0, 0, 0) to (4, 2, 1) in glTF coordinates, whose material is named
      // "models/test/wedge" and has an embedded 2x2 texture
      auto assimpFs = fs::DiskFileSystem{getFixtureRoot() / "test/mdl/LoadAssimpModel"};
      const auto glbPath = std::filesystem::path{"idtech4/named.glb"};

      const auto findDecl =
        [&](const std::filesystem::path& materialName) -> std::optional<gl::Material> {
        requestedNames.push_back(materialName);
        return gl::Material{
          "resolved decl", gl::createTextureResource(gl::Texture{16, 8})};
      };

      SECTION("the id Tech 4 convention")
      {
        const auto model = loadEntityModelSync(
                             assimpFs,
                             materialConfig,
                             ModelConvention::IdTech4,
                             glbPath,
                             loadMaterial,
                             findDecl,
                             logger)
                           | kdl::value();

        // turned as the engine turns glTF models, (x, y, z) to (x, -z, y)
        REQUIRE(model.data() != nullptr);
        CHECK(vm::approx(model.data()->bounds(0)) == vm::bbox3f{{0, -1, 0}, {4, 0, 2}});

        // the material decl of the material's name
        CHECK(requestedNames == std::vector<std::filesystem::path>{"models/test/wedge"});
        REQUIRE(model.data()->surface(0).skinCount() == 1u);
        CHECK(model.data()->surface(0).skin(0)->name() == "resolved decl");
      }

      SECTION("the default convention")
      {
        const auto model = loadEntityModelSync(
                             assimpFs,
                             materialConfig,
                             ModelConvention::Default,
                             glbPath,
                             loadMaterial,
                             findDecl,
                             logger)
                           | kdl::value();

        // assimp's axes, (x, y, z) to (z, x, y), and the embedded texture
        REQUIRE(model.data() != nullptr);
        CHECK(vm::approx(model.data()->bounds(0)) == vm::bbox3f{{0, 0, 0}, {1, 4, 2}});

        CHECK(requestedNames.empty());
        REQUIRE(model.data()->surface(0).skinCount() == 1u);

        const auto* skin = model.data()->surface(0).skin(0);
        CHECK(skin->name() == "models/test/wedge");
        REQUIRE(skin->texture() != nullptr);
        CHECK(skin->texture()->width() == 2u);
      }
    }
  }

  SECTION("loadEntityModelAsync")
  {
    auto resource = std::shared_ptr<EntityModelDataResource>{};
    const auto createResource = [&](auto resourceLoader) {
      resource = std::make_shared<EntityModelDataResource>(std::move(resourceLoader));
      return resource;
    };

    auto model = std::optional<EntityModel>{};
    {
      // the loader keeps its own copies of what it is given
      const auto modelConvention = ModelConvention::IdTech4;
      const auto modelPath = path;
      const auto findMaterialCopy = FindMaterialFunc{findMaterial};
      model = loadEntityModelAsync(
        fs,
        materialConfig,
        modelConvention,
        modelPath,
        loadMaterial,
        findMaterialCopy,
        createResource,
        logger);
    }

    REQUIRE(resource != nullptr);
    CHECK(model->data() == nullptr);

    resource->loadSync();

    REQUIRE(model->data() != nullptr);
    CHECK(model->data()->bounds(0) == vm::bbox3f{{0, 0, -8}, {64, 32, 16}});
    CHECK(requestedNames.size() == 2u);
  }
}

} // namespace tb::mdl
