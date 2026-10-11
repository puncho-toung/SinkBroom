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
