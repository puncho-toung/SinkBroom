/*
 Copyright (C) 2010 Kristian Duske

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
#include "fs/DiskIO.h"
#include "fs/Reader.h"
#include "mdl/CatchConfig.h"
#include "mdl/CompilationConfig.h"
#include "mdl/CompilationProfile.h"
#include "mdl/CompilationTask.h"
#include "mdl/GameEngineConfig.h"
#include "mdl/ParseCompilationConfig.h"
#include "mdl/ParseGameEngineConfig.h"

#include "kd/k.h"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("CompilationConfigParser")
{
  SECTION("invalid input")
  {
    SECTION("parseBlankConfig")
    {
      const auto config = "   ";
      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseEmptyConfig")
    {
      const auto config = "  {  } ";
      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseEmptyConfigWithTrailingGarbage")
    {
      const auto config = "  {  } asdf";
      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseMissingProfiles")
    {
      const auto config = "  { 'version' : 1 } ";
      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseMissingVersion")
    {
      const auto config = "  { 'profiles': {} } ";
      CHECK(parseCompilationConfig(config).is_error());
    }
  }

  SECTION("profiles")
  {
    SECTION("parseEmptyProfiles")
    {
      const auto config = "  { 'version': 1, 'profiles': [] } ";
      CHECK(parseCompilationConfig(config) == mdl::CompilationConfig{{}});
    }

    SECTION("parseOneProfileWithMissingNameAndMissingTasks")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {}
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndMissingTasks")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name': 'A profile'
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithMissingNameAndEmptyTasks")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'tasks': []
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndEmptyTasks")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile', 
      'workdir' : '', 
      'tasks' : []
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile", "", {}},
        }});
    }

    SECTION("parseOneProfileWithNameAndOneInvalidTask")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ { 'asdf' : 'asdf' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneTaskWithUnknownType")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ { 'type' : 'unknown' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }
  }

  SECTION("export task")
  {
    SECTION("parseOneProfileWithNameAndOneExportTask")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'export', 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationExportMap{
               K(enabled),
               !K(stripTbProperties),
               std::nullopt,
               std::nullopt,
               "the target"},
           }},
        }});
    }

    SECTION("parseOneProfileWithNameAndOneExportTaskWithStripProperties")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'export', 'stripTbProperties': true, 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationExportMap{
               K(enabled),
               K(stripTbProperties),
               std::nullopt,
               std::nullopt,
               "the target"},
           }},
        }});
    }

    SECTION("parseOneProfileWithNameAndOneExportTaskWithStripEntityPattern")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'export', 'stripTbProperties': true, 'stripEntityPattern': 'info_player_*', 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationExportMap{
               K(enabled),
               K(stripTbProperties),
               "info_player_*",
               std::nullopt,
               "the target"},
           }},
        }});
    }

    SECTION("parseOneProfileWithNameAndOneExportTaskWithEntityToAdd")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'export', 'stripTbProperties': true, 'entityToAdd': [ { 'key': 'classname', 'value': 'info_player_start' } ], 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationExportMap{
               K(enabled),
               K(stripTbProperties),
               std::nullopt,
               mdl::Entity{{{"classname", "info_player_start"}}},
               "the target"},
           }},
        }});
    }
  }

  SECTION("copy task")
  {
    SECTION("parseOneProfileWithNameAndOneCopyTaskWithMissingSource")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'copy', 'target' : 'somewhere' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneCopyTaskWithMissingTarget")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'copy', 'source' : 'somewhere' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneCopyTask")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'copy', 'source' : 'the source', 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationCopyFiles{K(enabled), "the source", "the target"},
           }},
        }});
    }
  }

  SECTION("rename task")
  {
    SECTION("parseOneProfileWithNameAndOneRenameTask")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [ { 'type' : 'rename', 'source' : 'the source', 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationRenameFile{K(enabled), "the source", "the target"},
           }},
        }});
    }
  }

  SECTION("delete task")
  {
    SECTION("parseOneProfileWithNameAndOneDeleteTaskWithMissingTarget")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'delete', } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneDeleteTask")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'delete', 'target' : 'the target' } ]
    }
  ]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationDeleteFiles{K(enabled), "the target"},
           }},
        }});
    }
  }

  SECTION("tool task")
  {
    SECTION("parseOneProfileWithNameAndOneToolTaskWithMissingTool")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'tool', 'parameters' : 'this and that' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneToolTaskWithMissingParameters")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'tool', 'tool' : 'tyrbsp.exe' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneToolTask")
    {
      const auto config = R"(
{
  'version': 1,
  'unexpectedKey': '',
  'profiles': [{
      'name' : 'A profile',
      'unexpectedKey' : '',
      'workdir' : '',
      'tasks' : [{ 
        'type' : 'tool',
        'unexpectedKey' : '',
        'tool' : 'tyrbsp.exe',
        'parameters': 'this and that'
      }]
    }]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationRunTool{
               K(enabled),
               "tyrbsp.exe",
               "this and that",
               !K(treatNonZeroResultCodeAsError)},
           }},
        }});
    }
  }

  SECTION("launch engine task")
  {
    SECTION("parseOneProfileWithNameAndOneLaunchEngineTaskWithMissingEngineProfileId")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [
    {
      'name' : 'A profile',
      'workdir' : '',
      'tasks': [ {  'type' : 'launchEngine' } ]
    }
  ]
})";

      CHECK(parseCompilationConfig(config).is_error());
    }

    SECTION("parseOneProfileWithNameAndOneLaunchEngineTask")
    {
      const auto config = R"(
{
  'version': 1,
  'unexpectedKey': '',
  'profiles': [{
      'name' : 'A profile',
      'unexpectedKey' : '',
      'workdir' : '',
      'tasks' : [{
        'type' : 'launchEngine',
        'unexpectedKey' : '',
        'engineProfileId' : 'Quakespasm'
      }]
    }]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationLaunchEngine{
               K(enabled), "Quakespasm", !K(treatLaunchFailureAsError)},
           }},
        }});
    }

    SECTION("parseOneProfileWithNameAndOneLaunchEngineTaskWithLaunchFailureAsError")
    {
      const auto config = R"(
{
  'version': 1,
  'profiles': [{
      'name' : 'A profile',
      'workdir' : '',
      'tasks' : [{
        'type' : 'launchEngine',
        'engineProfileId' : 'Quakespasm',
        'treatLaunchFailureAsError': true
      }]
    }]
})";

      CHECK(
        parseCompilationConfig(config)
        == mdl::CompilationConfig{{
          {"A profile",
           "",
           {
             mdl::CompilationLaunchEngine{
               K(enabled), "Quakespasm", K(treatLaunchFailureAsError)},
           }},
        }});
    }
  }

  SECTION("parseOneProfileWithNameAndFiveTasks")
  {
    const auto config = R"(
{
  'version': 1,
  'profiles': [{
    'name': 'A profile',
    'workdir': '',
    'tasks': [{
      'type':'tool',
      'tool': 'tyrbsp.exe',
      'parameters': 'this and that',
      'treatNonZeroResultCodeAsError': true
    },
    {
      'type':'copy',
      'source': 'the source',
      'target': 'the target',
      'enabled': false
    },
    {
      'type':'rename',
      'source': 'the source',
      'target': 'the target',
      'enabled': true
    },
    {
      'type':'delete',
      'target': 'some other target',
      'enabled': false
    },
    {
      'type':'launchEngine',
      'engineProfileId': 'Quakespasm',
      'treatLaunchFailureAsError': true,
      'enabled': true
    }]
  }]
})";

    CHECK(
      parseCompilationConfig(config)
      == mdl::CompilationConfig{{
        {"A profile",
         "",
         {
           mdl::CompilationRunTool{
             K(enabled), "tyrbsp.exe", "this and that", K(treatNonZeroResultCodeAsError)},
           mdl::CompilationCopyFiles{!K(enabled), "the source", "the target"},
           mdl::CompilationRenameFile{K(enabled), "the source", "the target"},
           mdl::CompilationDeleteFiles{!K(enabled), "some other target"},
           mdl::CompilationLaunchEngine{
             K(enabled), "Quakespasm", K(treatLaunchFailureAsError)},
         }},
      }});
  }

  SECTION("parseUnescapedBackslashes")
  {
    // https://github.com/TrenchBroom/TrenchBroom/issues/1437
    const auto config = R"(
{
  "profiles": [{
    "name": "Full Compile",
    "tasks": [{
      "source": "${WORK_DIR_PATH}/${MAP_BASE_NAME}.bsp",
      "target": "C:\\quake2\\chaos\\maps\\",
      "type": "copy"
    }],
    "workdir": "${MAP_DIR_PATH}"
  }],
  "version": 1
})";

    CHECK(
      parseCompilationConfig(config)
      == mdl::CompilationConfig{{
        {"Full Compile",
         "${MAP_DIR_PATH}",
         {
           mdl::CompilationCopyFiles{
             K(enabled),
             "${WORK_DIR_PATH}/${MAP_BASE_NAME}.bsp",
             R"(C:\quake2\chaos\maps\)"},
         }},
      }});
  }

  SECTION("parseDoom3BfgProfiles")
  {
    const auto readFile = [](const std::filesystem::path& path) {
      auto file = fs::Disk::openFile(path) | kdl::value();
      auto reader = file->reader().buffer();
      return std::string{reader.stringView()};
    };

    const auto gamePath = getFixtureRoot() / "games/Doom3BFG";
    const auto compilationConfig =
      parseCompilationConfig(readFile(gamePath / "CompilationProfiles.cfg"));
    REQUIRE(compilationConfig.is_success());
    const auto gameEngineConfig =
      parseGameEngineConfig(readFile(gamePath / "GameEngineProfiles.cfg"));
    REQUIRE(gameEngineConfig.is_success());
    REQUIRE(gameEngineConfig.value().profiles.size() == 1u);

    // dmap reads the map from the game file system, so it is exported into the maps
    // folder of the last enabled mod and that mod is passed as fs_game (the engine
    // ignores "base" there, so a map without a mod still compiles into base); the
    // engine's Valve 220 loader uses _tb_group, so TrenchBroom's properties stay;
    // without -nogui, rbdmap keeps its text UI open and the task never ends
    const auto exportMap = mdl::CompilationExportMap{
      K(enabled),
      !K(stripTbProperties),
      std::nullopt,
      std::nullopt,
      "${GAME_DIR_PATH}/${MODS[-1]}/maps/${MAP_BASE_NAME}.map"};
    const auto runRbdmap = mdl::CompilationRunTool{
      K(enabled),
      "${rbdmap}",
      R"(-nogui +set fs_basepath "${GAME_DIR_PATH}" +set fs_game ${MODS[-1]} ${MAP_BASE_NAME})",
      K(treatNonZeroResultCodeAsError)};

    CHECK(
      compilationConfig.value()
      == mdl::CompilationConfig{{
        {"dmap (rbdmap)", "${GAME_DIR_PATH}", {exportMap, runRbdmap}},
        {"dmap (in engine)",
         "${GAME_DIR_PATH}",
         {
           exportMap,
           mdl::CompilationRunTool{
             K(enabled),
             "${rbdoom3bfg}",
             R"(+set fs_basepath "${GAME_DIR_PATH}" +set fs_game ${MODS[-1]} +dmap ${MAP_BASE_NAME} +quit)",
             K(treatNonZeroResultCodeAsError)},
         }},
        {"dmap (rbdmap) and run",
         "${GAME_DIR_PATH}",
         {
           exportMap,
           runRbdmap,
           mdl::CompilationLaunchEngine{
             K(enabled), "doom3bfg-rbdoom3bfg", K(treatLaunchFailureAsError)},
         }},
      }});

    // the launch task refers to the shipped engine profile by its id
    const auto& profiles = compilationConfig.value().profiles;
    REQUIRE(profiles.size() == 3u);
    const auto* launchTask =
      std::get_if<CompilationLaunchEngine>(&profiles.back().tasks.back());
    REQUIRE(launchTask != nullptr);
    CHECK(launchTask->engineProfileId == gameEngineConfig.value().profiles.front().id);
  }
}

} // namespace tb::mdl
