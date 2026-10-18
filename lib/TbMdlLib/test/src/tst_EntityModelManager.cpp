/*
 Copyright (C) 2026 Kristian Duske

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
#include "fs/VirtualFileSystem.h"
#include "gl/CreateResource.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityModelManager.h"
#include "mdl/GameConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameInfo.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/ModelSpecification.h"
#include "mdl/Quake3Shader.h"
#include "mdl/TestUtils.h"

#include "vm/approx.h"
#include "vm/bbox_io.h" // IWYU pragma: keep

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

GameInfo makeTestGameInfo()
{
  return makeGameInfo(GameConfig{
    .name = "Test",
    .path = {},
    .icon = {},
    .fileFormats = {},
    .fileSystemConfig = {},
    .materialConfig =
      MaterialConfig{
        .root = "",
        .extensions = {},
        .palette = "palette.lmp",
        .property = {},
        .shaderSearchPath = "",
        .excludes = {},
      },
    .entityConfig = {},
    .faceAttribsConfig = {},
    .smartTags = {},
    .softMapBounds = {},
    .compilationTools = {},
  });
}

} // namespace

TEST_CASE("EntityModelManager")
{
  auto logger = NullLogger{};
  auto fileSystem = fs::DiskFileSystem{getFixtureRoot() / "test/mdl/LoadMdlModel"};
  const auto gameInfo = makeTestGameInfo();

  auto manager = EntityModelManager{
    gameInfo,
    fileSystem,
    [](auto resourceLoader) { return gl::createResourceSync(std::move(resourceLoader)); },
    logger};

  SECTION("model")
  {
    SECTION("returns nullptr for an empty path")
    {
      CHECK(manager.model({}) == nullptr);
    }

    SECTION("loads a valid model")
    {
      const auto* model = manager.model("armor.mdl");
      REQUIRE(model != nullptr);
      CHECK(model->data() != nullptr);
    }

    SECTION("returns the same model instance on subsequent calls")
    {
      const auto* model1 = manager.model("armor.mdl");
      const auto* model2 = manager.model("armor.mdl");
      CHECK(model1 == model2);
    }

    SECTION("loads a model whose data is null if the underlying file does not exist")
    {
      // model() always returns a non-null EntityModel; the load failure only surfaces
      // once the (synchronously loaded) data resource is inspected.
      const auto* model = manager.model("does_not_exist.mdl");
      REQUIRE(model != nullptr);
      CHECK(model->data() == nullptr);
    }
  }

  SECTION("frame")
  {
    SECTION("returns nullptr for a model that does not exist")
    {
      CHECK(manager.frame({"does_not_exist.mdl", 0, 0}) == nullptr);
    }

    SECTION("returns the frame for a valid model")
    {
      CHECK(manager.frame({"armor.mdl", 0, 0}) != nullptr);
    }

    SECTION("returns nullptr for an invalid frame index")
    {
      CHECK(manager.frame({"armor.mdl", 0, 99}) == nullptr);
    }
  }

  SECTION("renderer")
  {
    const auto spec = ModelSpecification{"armor.mdl", 0, 0};

    SECTION("returns nullptr for a model that does not exist")
    {
      CHECK(manager.renderer(ModelSpecification{"does_not_exist.mdl", 0, 0}) == nullptr);
    }

    SECTION("builds a renderer for a valid model")
    {
      CHECK(manager.renderer(spec) != nullptr);
    }

    SECTION("returns the same renderer instance on subsequent calls")
    {
      auto* renderer1 = manager.renderer(spec);
      auto* renderer2 = manager.renderer(spec);
      CHECK(renderer1 == renderer2);
    }

    SECTION("falls back to skin 0 for an out-of-range skin index")
    {
      // armor.mdl has 3 skins; an out-of-range index is not an error, it just falls
      // back to skin 0 (see EntityModelData::buildRenderer).
      CHECK(manager.renderer(ModelSpecification{"armor.mdl", 99, 0}) != nullptr);
    }

    SECTION("returns nullptr for an invalid frame index and caches the failure")
    {
      // armor.mdl has only 1 frame.
      const auto badSpec = ModelSpecification{"armor.mdl", 0, 99};
      CHECK(manager.renderer(badSpec) == nullptr);

      // second call takes the cached-mismatch path instead of trying to build again
      CHECK(manager.renderer(badSpec) == nullptr);
    }
  }

  SECTION("clear allows a model to be reloaded")
  {
    const auto* modelBeforeClear = manager.model("armor.mdl");
    REQUIRE(modelBeforeClear != nullptr);
    REQUIRE(modelBeforeClear->data() != nullptr);

    manager.clear();

    const auto* modelAfterClear = manager.model("armor.mdl");
    CHECK(modelAfterClear != nullptr);
    CHECK(modelAfterClear->data() != nullptr);
  }

  SECTION("shaders")
  {
    // empty before the first setShaders
    const auto initialShaders = manager.shaders();
    REQUIRE(initialShaders != nullptr);
    CHECK(initialShaders->shaders().empty());

    auto doom3FileSystem =
      fs::DiskFileSystem{getFixtureRoot() / "test/mdl/Game/Doom3BFG/base"};
    auto doom3Manager = EntityModelManager{
      Doom3BFGGameInfo,
      doom3FileSystem,
      [](auto resourceLoader) {
        return gl::createResourceSync(std::move(resourceLoader));
      },
      logger};
    const auto parseShaders = [&]() {
      return loadIndexedShaders(
        doom3FileSystem, Doom3BFGGameInfo.gameConfig.materialConfig);
    };
    doom3Manager.setShaders(parseShaders());

    const auto loadedShaders = doom3Manager.shaders();
    REQUIRE(loadedShaders != nullptr);
    CHECK(loadedShaders->findShader("textures/tb_test/wall") != nullptr);
    // tb_test_lights.mtr: the light materials that the material browser leaves out
    CHECK(
      shaderNamesWithPrefix(*loadedShaders, {"lights/", "fogs/"})
      == std::vector<std::string>{"fogs/tb_test_fog", "lights/tb_test_point"});

    // setShaders replaces the shaders, and a caller's pointer keeps the old ones alive
    const auto newShaders = parseShaders();
    doom3Manager.setShaders(newShaders);
    CHECK(doom3Manager.shaders() == newShaders);
    CHECK(doom3Manager.shaders() != loadedShaders);
    CHECK(loadedShaders->findShader("textures/tb_test/wall") != nullptr);
  }

  SECTION("id Tech 4 OBJ proxies")
  {
    auto doom3FileSystem =
      fs::DiskFileSystem{getFixtureRoot() / "test/mdl/Game/Doom3BFG/base"};

    const auto makeManager = [&](const GameInfo& doom3GameInfo) {
      auto doom3Manager = std::make_unique<EntityModelManager>(
        doom3GameInfo,
        doom3FileSystem,
        [](auto resourceLoader) {
          return gl::createResourceSync(std::move(resourceLoader));
        },
        logger);
      doom3Manager->setShaders(
        loadIndexedShaders(doom3FileSystem, doom3GameInfo.gameConfig.materialConfig));
      return doom3Manager;
    };

    const auto modelPath = std::filesystem::path{"_tb/models/tb_test/arrow.obj"};

    SECTION("the id Tech 4 convention shows the proxy upright with its materials")
    {
      REQUIRE(
        Doom3BFGGameInfo.gameConfig.entityConfig.modelConvention
        == ModelConvention::IdTech4);
      const auto doom3Manager = makeManager(Doom3BFGGameInfo);

      const auto* model = doom3Manager->model(modelPath);
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);

      // the engine's coordinates: the arrow points along +X, its fin up along +Z
      CHECK(model->data()->bounds(0) == vm::bbox3f{{-32, -8, -8}, {32, 12, 20}});

      const auto& surface = model->data()->surface(0);
      REQUIRE(surface.skinCount() == 3u);

      // usemtl textures/tb_test/wall: the decl in tb_test.mtr, previewed with its
      // 8x8 editor image
      const auto* wall = surface.skin(0);
      CHECK(wall->relativePath() == "textures/tb_test/wall");
      CHECK(wall->name() == "tb_test/wall");
      CHECK_FALSE(wall->previewImageMissing());
      REQUIRE(wall->texture() != nullptr);
      CHECK(wall->texture()->width() == 8u);

      // usemtl textures/tb_test/grate: a twoSided decl
      const auto* grate = surface.skin(1);
      CHECK(grate->relativePath() == "textures/tb_test/grate");
      CHECK(grate->culling() == gl::MaterialCulling::None);

      CHECK(surface.skin(2)->name() == "__TB_empty");
    }

    SECTION("without the convention, assimp lays the proxy on its side")
    {
      auto defaultGameInfo = Doom3BFGGameInfo;
      defaultGameInfo.gameConfig.entityConfig.modelConvention = ModelConvention::Default;
      const auto doom3Manager = makeManager(defaultGameInfo);

      const auto* model = doom3Manager->model(modelPath);
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);

      // (x, y, z) becomes (z, x, y)
      CHECK(
        vm::approx(model->data()->bounds(0)) == vm::bbox3f{{-8, -32, -8}, {20, 32, 12}});
    }
  }

  SECTION("id Tech 4 glTF models")
  {
    // named.glb is a box from (0, 0, 0) to (4, 2, 1) in glTF coordinates, whose material
    // is named "models/test/wedge" and has an embedded 2x2 texture; no decl has that
    // name, but _tb/models/test/wedge.tga, 4x2, is the engine's export of the image of
    // the implicit material
    auto assimpFileSystem =
      fs::DiskFileSystem{getFixtureRoot() / "test/mdl/LoadAssimpModel"};

    const auto makeManager = [&](const GameInfo& gameInfo) {
      auto glbManager = std::make_unique<EntityModelManager>(
        gameInfo,
        assimpFileSystem,
        [](auto resourceLoader) {
          return gl::createResourceSync(std::move(resourceLoader));
        },
        logger);
      glbManager->setShaders(
        loadIndexedShaders(assimpFileSystem, gameInfo.gameConfig.materialConfig));
      return glbManager;
    };

    const auto modelPath = std::filesystem::path{"idtech4/named.glb"};

    SECTION("the id Tech 4 convention turns the model and resolves its material name")
    {
      const auto glbManager = makeManager(Doom3BFGGameInfo);

      const auto* model = glbManager->model(modelPath);
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);

      // (x, y, z) becomes (x, -z, y)
      CHECK(vm::approx(model->data()->bounds(0)) == vm::bbox3f{{0, -1, 0}, {4, 0, 2}});

      REQUIRE(model->data()->surface(0).skinCount() == 1u);
      const auto* skin = model->data()->surface(0).skin(0);
      CHECK(skin->relativePath() == "models/test/wedge");
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 4u);
      CHECK(skin->texture()->height() == 2u);
    }

    SECTION("without the convention, the model shows its own texture")
    {
      auto defaultGameInfo = Doom3BFGGameInfo;
      defaultGameInfo.gameConfig.entityConfig.modelConvention = ModelConvention::Default;
      const auto glbManager = makeManager(defaultGameInfo);

      const auto* model = glbManager->model(modelPath);
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);

      // (x, y, z) becomes (z, x, y)
      CHECK(vm::approx(model->data()->bounds(0)) == vm::bbox3f{{0, 0, 0}, {1, 4, 2}});

      REQUIRE(model->data()->surface(0).skinCount() == 1u);
      const auto* skin = model->data()->surface(0).skin(0);
      CHECK(skin->name() == "models/test/wedge");
      REQUIRE(skin->texture() != nullptr);
      CHECK(skin->texture()->width() == 2u);
    }
  }

  SECTION("id Tech 4 model materials")
  {
    // wedge.obj and noncanonical.obj name the decl models/mapobjects/test/wedge, and
    // noncanonical.glb has one material of that name on two meshes; the decl is
    // previewed with models/mapobjects/test/wedge_ed.tga, 4x2
    auto modelFileSystem = fs::VirtualFileSystem{};
    modelFileSystem.mount(
      "",
      std::make_unique<fs::DiskFileSystem>(
        getFixtureRoot() / "test/mdl/LoadIdTech4ObjModel"));
    modelFileSystem.mount(
      "",
      std::make_unique<fs::DiskFileSystem>(
        getFixtureRoot() / "test/mdl/LoadAssimpModel"));

    const auto makeShaders = []() {
      return std::make_shared<const IndexedShaders>(std::vector<Quake3Shader>{
        Quake3Shader{
          .shaderPath = "models/mapobjects/test/wedge",
          .editorImage = "models/mapobjects/test/wedge_ed.tga",
          .format = Quake3Shader::Format::Doom3,
          .previewImages = {"models/mapobjects/test/wedge_ed.tga"},
        },
      });
    };

    auto doom3Manager = EntityModelManager{
      Doom3BFGGameInfo,
      modelFileSystem,
      [](auto resourceLoader) {
        return gl::createResourceSync(std::move(resourceLoader));
      },
      logger};
    doom3Manager.setShaders(makeShaders());

    const auto skin = [&](
                        const std::filesystem::path& modelPath,
                        const size_t surfaceIndex,
                        const size_t skinIndex) {
      const auto* model = doom3Manager.model(modelPath);
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);
      REQUIRE(surfaceIndex < model->data()->surfaceCount());
      const auto& surface = model->data()->surface(surfaceIndex);
      REQUIRE(skinIndex < surface.skinCount());
      return surface.skin(skinIndex);
    };

    const auto wedgeObj = std::filesystem::path{"_tb/models/mapobjects/test/wedge.obj"};
    const auto noncanonicalObj =
      std::filesystem::path{"_tb/models/mapobjects/test/noncanonical.obj"};
    const auto noncanonicalGlb = std::filesystem::path{"idtech4/noncanonical.glb"};

    SECTION("models that name the same decl share its texture")
    {
      const auto* wedge = skin(wedgeObj, 0, 0);
      CHECK(wedge->relativePath() == "models/mapobjects/test/wedge");
      REQUIRE(wedge->texture() != nullptr);
      CHECK(wedge->texture()->width() == 4u);

      CHECK(&skin(noncanonicalObj, 0, 0)->textureResource() == &wedge->textureResource());
      // the material of both meshes is looked up for each, but its texture is shared
      CHECK(&skin(noncanonicalGlb, 0, 0)->textureResource() == &wedge->textureResource());
      CHECK(&skin(noncanonicalGlb, 1, 0)->textureResource() == &wedge->textureResource());
    }

    SECTION("the default skins of OBJ models share one texture")
    {
      // the last skin, for the faces before the first usemtl
      const auto* wedgeDefault = skin(wedgeObj, 0, 2);
      CHECK(wedgeDefault->name() == "__TB_empty");
      REQUIRE(wedgeDefault->texture() != nullptr);
      CHECK(wedgeDefault->texture()->width() == 32u);
      CHECK(wedgeDefault->texture()->buffersIfLoaded().size() == 1u);

      CHECK(
        &skin(noncanonicalObj, 0, 1)->textureResource()
        == &wedgeDefault->textureResource());
    }

    SECTION("models loaded after clear get new textures")
    {
      // the old model and its texture are gone, so compare ids, not addresses
      const auto textureId = skin(wedgeObj, 0, 0)->textureResource().id();
      doom3Manager.clear();
      CHECK(skin(wedgeObj, 0, 0)->textureResource().id() != textureId);
    }

    SECTION("models loaded after setShaders get new textures")
    {
      const auto& texture = skin(wedgeObj, 0, 0)->textureResource();
      doom3Manager.setShaders(makeShaders());
      CHECK(&skin(noncanonicalObj, 0, 0)->textureResource() != &texture);
    }
  }

  SECTION("findEntityModelsByTextureResourceId")
  {
    const auto* model = manager.model("armor.mdl");
    REQUIRE(model != nullptr);
    const auto id = model->dataResource().id();

    CHECK_THAT(
      manager.findEntityModelsByTextureResourceId({id}),
      Catch::Matchers::Equals(std::vector<const EntityModel*>{model}));
    CHECK(manager.findEntityModelsByTextureResourceId({}).empty());
  }
}

} // namespace tb::mdl
