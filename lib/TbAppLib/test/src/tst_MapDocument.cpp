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
#include "TestEnvironment.h"
#include "base/Logger.h"
#include "gl/ResourceManager.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityNode.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <limits>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MapDocument")
{
  auto logger = NullLogger{};
  const auto environmentConfig = mdl::EnvironmentConfig{};
  auto taskManager = createTestTaskManager();
  auto resourceManager = gl::ResourceManager{};

  SECTION("worldBoundsForGame")
  {
    SECTION("returns the default bounds if the game has no soft map bounds")
    {
      CHECK(!mdl::Doom3BFGGameInfo.gameConfig.softMapBounds);
      CHECK(
        MapDocument::worldBoundsForGame(mdl::Doom3BFGGameInfo.gameConfig)
        == MapDocument::DefaultWorldBounds);
    }

    SECTION("returns the default bounds if the soft map bounds are smaller")
    {
      CHECK(mdl::QuakeGameInfo.gameConfig.softMapBounds == vm::bbox3d{4096.0});
      CHECK(
        MapDocument::worldBoundsForGame(mdl::QuakeGameInfo.gameConfig)
        == MapDocument::DefaultWorldBounds);
    }

    SECTION("returns the soft map bounds if they are larger")
    {
      auto gameInfo = mdl::Doom3BFGGameInfo;
      gameInfo.gameConfig.softMapBounds = vm::bbox3d{131072.0};

      CHECK(MapDocument::worldBoundsForGame(gameInfo.gameConfig) == vm::bbox3d{131072.0});
    }

    SECTION("returns the hull of the default and the soft map bounds")
    {
      auto gameInfo = mdl::Doom3BFGGameInfo;
      gameInfo.gameConfig.softMapBounds =
        vm::bbox3d{{-65536.0, -4096.0, -4096.0}, {4096.0, 131072.0, 8192.0}};

      CHECK(
        MapDocument::worldBoundsForGame(gameInfo.gameConfig)
        == vm::bbox3d{{-65536.0, -32768.0, -32768.0}, {32768.0, 131072.0, 32768.0}});
    }

    SECTION("returns the default bounds if the soft map bounds are not finite")
    {
      auto gameInfo = mdl::Doom3BFGGameInfo;

      auto bounds = vm::bbox3d{131072.0};
      bounds.max[2] = std::numeric_limits<double>::infinity();
      gameInfo.gameConfig.softMapBounds = bounds;
      CHECK(
        MapDocument::worldBoundsForGame(gameInfo.gameConfig)
        == MapDocument::DefaultWorldBounds);

      bounds = vm::bbox3d{131072.0};
      bounds.min[0] = std::numeric_limits<double>::quiet_NaN();
      gameInfo.gameConfig.softMapBounds = bounds;
      CHECK(
        MapDocument::worldBoundsForGame(gameInfo.gameConfig)
        == MapDocument::DefaultWorldBounds);
    }

    SECTION("returns the default bounds if the soft map bounds are not a valid box")
    {
      auto gameInfo = mdl::Doom3BFGGameInfo;

      // the bbox constructors assert on this, so set the members
      auto bounds = vm::bbox3d{131072.0};
      bounds.min[1] = 131073.0;
      REQUIRE(!bounds.is_valid());

      gameInfo.gameConfig.softMapBounds = bounds;
      CHECK(
        MapDocument::worldBoundsForGame(gameInfo.gameConfig)
        == MapDocument::DefaultWorldBounds);
    }
  }

  SECTION("createDocument")
  {
    MapDocument::createDocument(
      environmentConfig,
      mdl::Quake2GameInfo,
      mdl::MapFormat::Valve,
      vm::bbox3d{8192.0},
      *taskManager,
      resourceManager)
      | kdl::transform([&](auto document) {
          SECTION("creates a new map with the given game")
          {
            CHECK(&document->map().gameInfo() == &mdl::Quake2GameInfo);
          }
        })
      | kdl::transform_error([](const auto& e) { FAIL(e); });
  }

  SECTION("create")
  {
    auto document = MapDocument::createDocument(
                      environmentConfig,
                      mdl::Quake2GameInfo,
                      mdl::MapFormat::Valve,
                      vm::bbox3d{8192.0},
                      *taskManager,
                      resourceManager)
                    | kdl::value();

    auto documentWasLoaded = Observer<>{document->documentWasLoadedNotifier};

    const auto* previousMap = &document->map();

    document->create(
      environmentConfig,
      mdl::QuakeGameInfo,
      mdl::MapFormat::Daikatana,
      vm::bbox3d{8192.0})
      | kdl::transform([&]() {
          SECTION("creates a new map with the given game")
          {
            CHECK(&document->map() != previousMap);
            CHECK(&document->map().gameInfo() == &mdl::QuakeGameInfo);
          }

          SECTION("calls notifiers")
          {
            CHECK(documentWasLoaded.notifications == std::vector<std::tuple<>>{{}});
          }
        })
      | kdl::transform_error([](auto e) { FAIL(e.msg); });
  }


  SECTION("loadDocument")
  {
    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    MapDocument::loadDocument(
      environmentConfig,
      mdl::Quake2GameInfo,
      mdl::MapFormat::Valve,
      vm::bbox3d{8192.0},
      path,
      *taskManager,
      resourceManager)
      | kdl::transform([&](auto document) {
          SECTION("loads map at given path")
          {
            CHECK(document->map().path() == path);
            CHECK(&document->map().gameInfo() == &mdl::Quake2GameInfo);
          }
        })
      | kdl::transform_error([](const auto& e) { FAIL(e); });
  }

  SECTION("load")
  {
    auto document = MapDocument::createDocument(
                      environmentConfig,
                      mdl::Quake2GameInfo,
                      mdl::MapFormat::Valve,
                      vm::bbox3d{8192.0},
                      *taskManager,
                      resourceManager)
                    | kdl::value();

    auto documentWasLoaded = Observer<>{document->documentWasLoadedNotifier};

    const auto* previousMap = &document->map();

    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    document->load(
      environmentConfig,
      mdl::QuakeGameInfo,
      mdl::MapFormat::Unknown,
      vm::bbox3d{8192.0},
      path)
      | kdl::transform([&]() {
          SECTION("loads map at given path")
          {
            CHECK(&document->map() != previousMap);
            CHECK(document->map().path() == path);
            CHECK(&document->map().gameInfo() == &mdl::QuakeGameInfo);
          }

          SECTION("calls notifiers")
          {
            CHECK(documentWasLoaded.notifications == std::vector<std::tuple<>>{{}});
          }
        })
      | kdl::transform_error([](auto e) { FAIL(e.msg); });
  }

  SECTION("reload")
  {
    auto document = MapDocument::createDocument(
                      environmentConfig,
                      mdl::Quake2GameInfo,
                      mdl::MapFormat::Valve,
                      vm::bbox3d{8192.0},
                      *taskManager,
                      resourceManager)
                    | kdl::value();

    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    REQUIRE(document->load(
      environmentConfig,
      mdl::QuakeGameInfo,
      mdl::MapFormat::Unknown,
      vm::bbox3d{8192.0},
      path));

    REQUIRE(document->map().path() == path);

    auto* transientEntityNode = new mdl::EntityNode{{}};
    addNodes(
      document->map(), {{&mdl::parentForNodes(document->map()), {transientEntityNode}}});
    REQUIRE(
      document->map().worldNode().defaultLayer()->children()
      == std::vector<mdl::Node*>{transientEntityNode});
    REQUIRE(document->map().modified());

    auto documentWasLoaded = Observer<>{document->documentWasLoadedNotifier};

    const auto* previousMap = &document->map();

    document->reload() | kdl::transform([&]() {
      SECTION("reloads map")
      {
        CHECK(&document->map() != previousMap);
        CHECK(document->map().path() == path);
        CHECK(&document->map().gameInfo() == &mdl::QuakeGameInfo);
        CHECK(!document->map().modified());
      }

      SECTION("calls notifiers")
      {
        CHECK(documentWasLoaded.notifications == std::vector<std::tuple<>>{{}});
      }
    }) | kdl::transform_error([](auto e) { FAIL(e.msg); });
  }
}

} // namespace tb::ui
