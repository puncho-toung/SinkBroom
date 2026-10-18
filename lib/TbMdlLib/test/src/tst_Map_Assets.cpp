/*
 Copyright (C) 2025 Kristian Duske

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

#include "Observer.h"
#include "base/Logger.h"
#include "fs/TestEnvironment.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/BrushFace.h" // IWYU pragma: keep
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"

#include "kd/path_utils.h"
#include "kd/result.h"
#include "kd/task_manager.h"

#include "vm/bbox.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

namespace tb::mdl
{
using namespace Catch::Matchers;

namespace
{

// Keeps every message; shader files are parsed and log on task threads.
class CollectingLogger : public Logger
{
private:
  mutable std::mutex m_mutex;
  std::vector<std::string> m_messages;

public:
  size_t countMessagesContaining(const std::string_view text) const
  {
    const auto lock = std::lock_guard{m_mutex};
    return size_t(std::ranges::count_if(m_messages, [&](const auto& message) {
      return message.find(text) != std::string::npos;
    }));
  }

private:
  void doLog(const LogLevel, const std::string_view message) override
  {
    const auto lock = std::lock_guard{m_mutex};
    m_messages.emplace_back(message);
  }
};

} // namespace

TEST_CASE("Map_Assets")
{
  auto fixture = MapFixture{};

  SECTION("entityDefinitionFile")
  {
    using T = std::tuple<std::optional<std::string>, EntityDefinitionFileSpec>;

    const auto [entityProperty, expectedEntityDefinitionFileSpec] = GENERATE(values<T>({
      {std::nullopt, EntityDefinitionFileSpec::makeBuiltin("Quake.def")},
      {"", EntityDefinitionFileSpec::makeBuiltin("Quake.def")},
      {"asdf", EntityDefinitionFileSpec::makeBuiltin("Quake.def")},
      {"builtin:ad.fgd", EntityDefinitionFileSpec::makeBuiltin("ad.fgd")},
      {"builtin:Quake.def", EntityDefinitionFileSpec::makeBuiltin("Quake.def")},
      {"external:/Applications/Quake/Quake.fgd",
       EntityDefinitionFileSpec::makeExternal("/Applications/Quake/Quake.fgd")},
      {"external:\\Applications\\Quake\\Quake.fgd",
       EntityDefinitionFileSpec::makeExternal("/Applications/Quake/Quake.fgd")},
    }));

    CAPTURE(entityProperty);

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo.gameConfig.entityConfig.defFilePaths =
      std::vector<std::filesystem::path>{
        "Quake.def",
        "ad.fgd",
        "Quoth.fgd",
      };

    auto& map = fixture.create(fixtureConfig);

    if (entityProperty)
    {
      setEntityProperty(map, EntityPropertyKeys::TbEntityDefinitions, *entityProperty);
    }

    CHECK(entityDefinitionFile(map) == expectedEntityDefinitionFileSpec);
  }

  SECTION("setEntityDefinitionFile")
  {
    using T = std::tuple<EntityDefinitionFileSpec, std::string>;

    const auto [entityDefinitionFileSpec, expectedPropertyValue] = GENERATE(values<T>({
      {EntityDefinitionFileSpec::makeBuiltin("Quake.def"), "builtin:Quake.def"},
      {EntityDefinitionFileSpec::makeBuiltin("ad.fgd"), "builtin:ad.fgd"},
      {
        EntityDefinitionFileSpec::makeExternal("/Applications/Quake/Quake.fgd"),
        "external:/Applications/Quake/Quake.fgd",
      },
      {
        EntityDefinitionFileSpec::makeExternal(
          kdl::parse_path(std::string{R"(Applications\Quake\Quake.fgd)"})),
        "external:Applications/Quake/Quake.fgd",
      },
    }));

    CAPTURE(entityDefinitionFileSpec);

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo.gameConfig.entityConfig.defFilePaths =
      std::vector<std::filesystem::path>{
        "Quake.def",
        "ad.fgd",
        "Quoth.fgd",
      };

    auto& map = fixture.create(fixtureConfig);

    auto entityDefinitionsWillChange =
      Observer<>{map.entityDefinitionsWillChangeNotifier};
    auto entityDefinitionsDidChange = Observer<>{map.entityDefinitionsDidChangeNotifier};

    setEntityDefinitionFile(map, entityDefinitionFileSpec);

    CHECK(entityDefinitionsWillChange.notifications == std::vector<std::tuple<>>{{}});
    CHECK(entityDefinitionsDidChange.notifications == std::vector<std::tuple<>>{{}});

    const auto& worldNode = map.worldNode();
    const auto& entity = worldNode.entity();
    const auto* propertyValue = entity.property(EntityPropertyKeys::TbEntityDefinitions);

    REQUIRE(propertyValue);
    CHECK(*propertyValue == expectedPropertyValue);
  }

  SECTION("enabledMaterialCollections")
  {
    auto& map = fixture.create(Quake2FixtureConfig);

    REQUIRE(map.materialManager().collections().size() == 3);

    const auto& worldNode = map.worldNode();

    SECTION("When no material collections are explicitly enabled")
    {
      REQUIRE(
        worldNode.entity().property(EntityPropertyKeys::TbEnabledMaterialCollections)
        == nullptr);

      CHECK(
        enabledMaterialCollections(map)
        == std::vector<std::filesystem::path>{
          "textures",
          "textures/e1m1",
          "textures/e1m1/f1",
        });
    }

    SECTION("When a material collection is explicitly enabled")
    {
      setEntityProperty(
        map,
        EntityPropertyKeys::TbEnabledMaterialCollections,
        "textures/e1m1;textures/e1m1/f1");

      CHECK(
        enabledMaterialCollections(map)
        == std::vector<std::filesystem::path>{
          "textures/e1m1",
          "textures/e1m1/f1",
        });
    }

    SECTION("Enabled material collections are sorted and unique")
    {
      setEntityProperty(
        map,
        EntityPropertyKeys::TbEnabledMaterialCollections,
        "textures/e1m1/f1;textures/e1m1;textures/e1m1");

      CHECK(
        enabledMaterialCollections(map)
        == std::vector<std::filesystem::path>{
          "textures/e1m1",
          "textures/e1m1/f1",
        });
    }

    SECTION("Unknown material collections are returned")
    {
      setEntityProperty(
        map,
        EntityPropertyKeys::TbEnabledMaterialCollections,
        "textures/unknown;textures/e1m1");

      CHECK(
        enabledMaterialCollections(map)
        == std::vector<std::filesystem::path>{
          "textures/e1m1",
          "textures/unknown",
        });
    }
  }

  SECTION("disabledMaterialCollections")
  {
    auto& map = fixture.create(Quake2FixtureConfig);

    REQUIRE(map.materialManager().collections().size() == 3);

    const auto& worldNode = map.worldNode();

    SECTION("When no material collections are explicitly enabled")
    {
      REQUIRE(
        worldNode.entity().property(EntityPropertyKeys::TbEnabledMaterialCollections)
        == nullptr);

      CHECK(disabledMaterialCollections(map) == std::vector<std::filesystem::path>{});
    }

    SECTION("When a material collection is explicitly enabled")
    {
      setEntityProperty(
        map, EntityPropertyKeys::TbEnabledMaterialCollections, "textures/e1m1");

      CHECK(
        disabledMaterialCollections(map)
        == std::vector<std::filesystem::path>{"textures", "textures/e1m1/f1"});
    }
  }

  SECTION("Material collections are ordered naturally")
  {
    auto env = fs::TestEnvironment{};
    env.createDirectory("textures");
    for (const auto* name : {"tex10", "tex9", "tex2"})
    {
      env.createDirectory("textures/" + std::string{name});
      env.createFile("textures/" + std::string{name} + "/a.png", "a");
    }

    auto gameConfig = DefaultGameInfo.gameConfig;
    gameConfig.materialConfig.extensions = {".png"};
    gameConfig.materialConfig.palette = std::filesystem::path{};

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo = detail::makeGameInfoFixture(gameConfig, env.dir());

    auto& map = fixture.create(fixtureConfig);

    REQUIRE(map.materialManager().collections().size() == 3);

    // A plain lexicographic sort would order these as tex10, tex2, tex9.
    CHECK(
      enabledMaterialCollections(map)
      == std::vector<std::filesystem::path>{
        "textures/tex2",
        "textures/tex9",
        "textures/tex10",
      });

    setEntityProperty(
      map, EntityPropertyKeys::TbEnabledMaterialCollections, "textures/tex9");

    CHECK(
      disabledMaterialCollections(map)
      == std::vector<std::filesystem::path>{
        "textures/tex2",
        "textures/tex10",
      });
  }

  SECTION("Nested material collections are ordered naturally")
  {
    // A digit run can never straddle a path separator, so natural order still compares
    // "tex2" against "tex10" directly even though they are followed by differently
    // numbered subfolders - nesting depth does not throw off the comparison.
    auto env = fs::TestEnvironment{};
    env.createDirectory("textures");
    for (const auto* name : {"tex10", "tex9", "tex2"})
    {
      env.createDirectory("textures/" + std::string{name});
      env.createFile("textures/" + std::string{name} + "/a.png", "a");
    }
    env.createDirectory("textures/tex10/folder2");
    env.createFile("textures/tex10/folder2/a.png", "a");
    env.createDirectory("textures/tex2/folder10");
    env.createFile("textures/tex2/folder10/a.png", "a");

    auto gameConfig = DefaultGameInfo.gameConfig;
    gameConfig.materialConfig.extensions = {".png"};
    gameConfig.materialConfig.palette = std::filesystem::path{};

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo = detail::makeGameInfoFixture(gameConfig, env.dir());

    auto& map = fixture.create(fixtureConfig);

    REQUIRE(map.materialManager().collections().size() == 5);

    CHECK(
      enabledMaterialCollections(map)
      == std::vector<std::filesystem::path>{
        "textures/tex2",
        "textures/tex2/folder10",
        "textures/tex9",
        "textures/tex10",
        "textures/tex10/folder2",
      });
  }

  SECTION("setEnabledMaterialCollections")
  {
    auto& map = fixture.create(Quake2FixtureConfig);

    const auto collectionPaths =
      map.materialManager().collections()
      | std::views::transform([](const auto& collection) { return collection.path(); })
      | kdl::ranges::to<std::vector>();

    REQUIRE(collectionPaths.size() == 3);

    const auto& worldNode = map.worldNode();

    const auto getEnabledMaterialCollections = [&] {
      return worldNode.entity().property(
        EntityPropertyKeys::TbEnabledMaterialCollections);
    };

    REQUIRE(!getEnabledMaterialCollections());

    SECTION("Enabling all collections resets the property")
    {
      setEnabledMaterialCollections(map, collectionPaths);
      REQUIRE(getEnabledMaterialCollections() == nullptr);
    }

    SECTION("Disabling all collections sets the property to an empty string")
    {
      setEnabledMaterialCollections(map, {});
      REQUIRE(getEnabledMaterialCollections() != nullptr);
      CHECK(*getEnabledMaterialCollections() == "");
    }

    SECTION("Setting a non-empty vector sets the property")
    {
      setEnabledMaterialCollections(map, {collectionPaths.front()});
      REQUIRE(getEnabledMaterialCollections());
      CHECK(*getEnabledMaterialCollections() == collectionPaths.front());
    }

    SECTION("Enabled material collections are sorted and unique")
    {
      setEnabledMaterialCollections(
        map, {"textures/e1m1/f1", "textures/e1m1", "textures/e1m1"});
      REQUIRE(getEnabledMaterialCollections());
      CHECK(*getEnabledMaterialCollections() == "textures/e1m1;textures/e1m1/f1");
    }
  }

  SECTION("setting wad property reloads collections from wad paths")
  {
    using T = std::tuple<std::string, std::vector<std::filesystem::path>>;

    const auto [wadPropertyValue, expectedMaterialCollections] = GENERATE(values<T>({
      {"cr8_czg.wad", {"cr8_czg.wad"}},
      {
        "cr8_czg.wad;folder/cr8_a_excerpt.wad",
        {"cr8_a_excerpt.wad", "cr8_czg.wad"},
      },
    }));

    CAPTURE(wadPropertyValue);

    auto& map = fixture.load("test/mdl/Map/emptyValveMap.map", QuakeFixtureConfig);

    CHECK(setEntityProperty(map, EntityPropertyKeys::Wad, wadPropertyValue));

    CHECK_THAT(
      map.materialManager().collections()
        | std::views::transform([](const auto& collection) { return collection.path(); }),
      RangeEquals(expectedMaterialCollections));
  }

  SECTION("reloadMaterialCollections")
  {
    auto& map =
      fixture.load("test/mdl/Map/reloadMaterialCollectionsQ2.map", Quake2FixtureConfig);

    auto materialCollectionsWillChange =
      Observer<>{map.materialCollectionsWillChangeNotifier};
    auto materialCollectionsDidChange =
      Observer<>{map.materialCollectionsDidChangeNotifier};

    const auto faces = map.worldNode().defaultLayer()->children()
                       | std::views::transform([&](const auto* node) {
                           const auto* brushNode =
                             dynamic_cast<const mdl::BrushNode*>(node);
                           REQUIRE(brushNode);
                           return &brushNode->brush().faces().front();
                         })
                       | kdl::ranges::to<std::vector>();

    REQUIRE(faces.size() == 4);
    REQUIRE_THAT(
      faces
        | std::views::transform([](const auto* face) { return face->materialName(); }),
      RangeEquals(std::vector<std::string>{
        "b_pv_v1a1", "e1m1/b_pv_v1a2", "e1m1/f1/b_rc_v4", "lavatest"}));

    REQUIRE(std::ranges::none_of(
      faces, [](const auto* face) { return face->material() == nullptr; }));

    reloadMaterialCollections(map);
    CHECK(materialCollectionsWillChange.notifications == std::vector<std::tuple<>>{{}});
    CHECK(materialCollectionsDidChange.notifications == std::vector<std::tuple<>>{{}});

    CHECK(std::ranges::none_of(
      faces, [](const auto* face) { return face->material() == nullptr; }));
  }

  SECTION("reloadMaterialCollections picks up materials added to disk")
  {
    auto env = fs::TestEnvironment{};
    env.createDirectory("textures");
    env.createFile("textures/tex1.png", "tex1");

    auto gameConfig = DefaultGameInfo.gameConfig;
    gameConfig.materialConfig.extensions = {".png"};
    gameConfig.materialConfig.palette = std::filesystem::path{};

    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo = detail::makeGameInfoFixture(gameConfig, env.dir());

    auto& map = fixture.create(fixtureConfig);

    REQUIRE(map.materialManager().collections().size() == 1);
    REQUIRE(map.materialManager().collections().front().materials().size() == 1);

    env.createFile("textures/tex2.png", "tex2");
    reloadMaterialCollections(map);

    REQUIRE(map.materialManager().collections().size() == 1);
    CHECK(map.materialManager().collections().front().materials().size() == 2);
  }

  SECTION("the material scripts are parsed once per load of the material collections")
  {
    // the material parser warns about the second definition of a name once per parse
    auto env = fs::TestEnvironment{};
    env.createDirectory("base/materials");
    env.createFile(
      "base/materials/twice.mtr", "textures/twice\n{\n}\ntextures/twice\n{\n}\n");

    const auto environmentConfig = EnvironmentConfig{};
    const auto gameInfo =
      detail::makeGameInfoFixture(Doom3BFGGameInfo.gameConfig, env.dir());
    auto taskManager = kdl::task_manager{};
    auto resourceManager = gl::ResourceManager{};
    auto logger = CollectingLogger{};
    const auto parseCount = [&]() {
      return logger.countMessagesContaining(
        "Material 'textures/twice' was already defined");
    };

    auto map = Map::createMap(
                 environmentConfig,
                 gameInfo,
                 env.dir(),
                 MapFormat::Doom3_Valve,
                 vm::bbox3d{8192.0},
                 taskManager,
                 resourceManager,
                 logger)
               | kdl::value();

    // the entity models and the material collections share one parse
    CHECK(parseCount() == 1);
    const auto shaders = map->entityModelManager().shaders();
    CHECK(shaders->findShader("textures/twice") != nullptr);
    CHECK(map->materialManager().material("twice") != nullptr);

    // a reload parses them again and replaces the shaders of the entity models
    reloadMaterialCollections(*map);
    CHECK(parseCount() == 2);
    CHECK(map->entityModelManager().shaders() != shaders);
    CHECK(map->materialManager().material("twice") != nullptr);
  }

  SECTION("reloadMaterialCollections loads the entity models again")
  {
    auto& map = fixture.create(Doom3BFGFixtureConfig);

    const auto* definition =
      map.entityDefinitionManager().definition("genmodel_tb_test_arrow");
    REQUIRE(definition != nullptr);
    auto* entityNode = createPointEntity(map, *definition, {0, 0, 0});
    REQUIRE(entityNode != nullptr);

    auto gl = gl::TestGl{};
    const auto processResources = [&]() {
      gl::processResourcesSync(
        map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
    };

    processResources();
    const auto* model = entityNode->entity().model();
    REQUIRE(model != nullptr);
    REQUIRE(model->data() != nullptr);
    const auto resourceId = model->dataResource().id();

    // the models are shaded with the shaders that the reload parses
    reloadMaterialCollections(map);
    processResources();

    model = entityNode->entity().model();
    REQUIRE(model != nullptr);
    CHECK(model->data() != nullptr);
    CHECK(model->dataResource().id() != resourceId);

    // a wad change reloads the material collections inside a command, and so does its
    // undo
    deselectAll(map);
    REQUIRE(setEntityProperty(map, EntityPropertyKeys::Wad, "missing.wad"));
    processResources();
    REQUIRE(entityNode->entity().model() != nullptr);
    CHECK(entityNode->entity().model()->data() != nullptr);

    map.undoCommand();
    processResources();
    REQUIRE(entityNode->entity().model() != nullptr);
    CHECK(entityNode->entity().model()->data() != nullptr);
  }

  SECTION("reloadMaterialCollections keeps the entity models without material scripts")
  {
    // the Doom 3 BFG config without a shader search path, like the Quake configs
    auto config = Doom3BFGFixtureConfig;
    config.gameInfo.gameConfig.materialConfig.shaderSearchPath = "";
    auto& map = fixture.create(config);

    const auto* definition =
      map.entityDefinitionManager().definition("genmodel_tb_test_arrow");
    REQUIRE(definition != nullptr);
    auto* entityNode = createPointEntity(map, *definition, {0, 0, 0});
    REQUIRE(entityNode != nullptr);

    auto gl = gl::TestGl{};
    const auto processResources = [&]() {
      gl::processResourcesSync(
        map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
    };
    const auto modelResourceId = [&]() {
      const auto* model = entityNode->entity().model();
      REQUIRE(model != nullptr);
      REQUIRE(model->data() != nullptr);
      return model->dataResource().id();
    };

    processResources();
    const auto resourceId = modelResourceId();

    reloadMaterialCollections(map);
    processResources();
    CHECK(modelResourceId() == resourceId);

    deselectAll(map);
    REQUIRE(setEntityProperty(map, EntityPropertyKeys::Wad, "missing.wad"));
    processResources();
    CHECK(modelResourceId() == resourceId);
  }

  SECTION("reloadEntityDefinitions")
  {
    const auto fgdFilename = "Test.fgd";

    auto env = fs::TestEnvironment{};
    env.createFile(fgdFilename, R"x(
@SolidClass = worldspawn : "World entity"
[
  message(string) : "Text on entering the world"
]
    )x");

    auto& map = fixture.create();

    auto entityDefinitionsWillChange =
      Observer<>{map.entityDefinitionsWillChangeNotifier};
    auto entityDefinitionsDidChange = Observer<>{map.entityDefinitionsDidChangeNotifier};

    setEntityDefinitionFile(
      map, EntityDefinitionFileSpec::makeExternal(env.dir() / fgdFilename));

    REQUIRE(entityDefinitionsWillChange.notifications == std::vector<std::tuple<>>{{}});
    REQUIRE(entityDefinitionsDidChange.notifications == std::vector<std::tuple<>>{{}});

    entityDefinitionsWillChange.reset();
    entityDefinitionsDidChange.reset();

    REQUIRE(
      entityDefinitionFile(map)
      == EntityDefinitionFileSpec::makeExternal(env.dir() / fgdFilename));

    reloadEntityDefinitions(map);

    CHECK(entityDefinitionsWillChange.notifications == std::vector<std::tuple<>>{{}});
    CHECK(entityDefinitionsDidChange.notifications == std::vector<std::tuple<>>{{}});
  }
}

} // namespace tb::mdl
