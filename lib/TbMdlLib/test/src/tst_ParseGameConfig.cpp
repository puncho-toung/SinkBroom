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
#include "el/Expression.h"
#include "fs/DiskIO.h"
#include "fs/Reader.h"
#include "fs/TraversalMode.h"
#include "mdl/CatchConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/Tag.h"
#include "mdl/TagMatcher.h"

#include "kd/ranges/to.h"

#include <filesystem>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("GameConfigParser")
{
  SECTION("includes")
  {
    SECTION("parseIncludedGameConfigs")
    {
      // only GameConfig.cfg files are game configs; the folders also hold compilation
      // and game engine profiles
      const auto basePath = getFixtureRoot() / "games/";
      const auto cfgFiles = fs::Disk::find(
                              basePath,
                              fs::TraversalMode::Recursive,
                              fs::makeFilenamePathMatcher("GameConfig.cfg"))
                            | kdl::value();
      CHECK_FALSE(cfgFiles.empty());

      for (const auto& path : cfgFiles)
      {
        CAPTURE(path);

        auto file = fs::Disk::openFile(path) | kdl::value();
        auto reader = file->reader().buffer();

        CHECK(parseGameConfig(reader.stringView(), path).is_success());
      }
    }

    SECTION("parseDoom3BfgConfig")
    {
      const auto path = getFixtureRoot() / "games/Doom3BFG/GameConfig.cfg";
      auto file = fs::Disk::openFile(path) | kdl::value();
      auto reader = file->reader().buffer();

      const auto result = parseGameConfig(reader.stringView(), path);
      REQUIRE(result.is_success());

      const auto& config = result.value();

      // RBDOOM-3-BFG writes "// Game: Doom 3 BFG" and "// Format: Doom3 (Valve)"
      CHECK(config.name == "Doom 3 BFG");
      CHECK(config.icon == "Icon.png");
      CHECK_FALSE(config.experimental);
      CHECK(
        config.fileFormats
        == std::vector<MapFormatConfig>{{.format = "Doom3 (Valve)", .initialMap = {}}});
      CHECK(
        config.fileSystemConfig
        == FileSystemConfig{
          .searchPath = "base",
          .packageFormat = {.extensions = {".pk4"}, .format = "zip"},
        });

      CHECK(
        config.materialConfig
        == MaterialConfig{
          .root = "textures",
          .extensions = {".tga", ".png", ".jpg", ".jpeg", ".dds"},
          .palette = {},
          .property = std::nullopt,
          .shaderSearchPath = "materials",
          .excludes =
            {"*_d",
             "*_s",
             "*_local",
             "*_h",
             "*_bmp",
             "*_rmao",
             "*_rmaod",
             "*_add",
             "*_n"},
          .shaderExtensions = {".mtr"},
          .shaderImageSearchPaths = {"_tb"},
        });
      // Doom 3 has no WAD key; this would turn on the WAD editor and drag and drop
      CHECK(config.materialConfig.property == std::nullopt);

      CHECK(
        config.entityConfig
        == EntityConfig{
          // the first file is the default for new maps
          .defFilePaths =
            {"DOOM-3-slim.fgd",
             "DOOM-3-all.fgd",
             "DOOM-3-slim-and-models.fgd",
             "DOOM-3-all-and-models.fgd",
             "DOOM-3-models.fgd",
             "DOOM-3-multiplayer.fgd"},
          .defaultColor = RgbaF{0.6f, 0.6f, 0.6f, 1.0f},
          .scaleExpression = std::nullopt,
          .setDefaultProperties = false,
        });

      CHECK(config.faceAttribsConfig.surfaceFlags.flags.empty());
      CHECK(config.faceAttribsConfig.contentFlags.flags.empty());
      CHECK(config.faceAttribsConfig.defaultUvAttributes.scale == vm::vec2f{0.5f, 0.5f});

      CHECK(
        config.softMapBounds
        == vm::bbox3d{{-131072.0, -131072.0, -131072.0}, {131072.0, 131072.0, 131072.0}});

      CHECK(
        (config.compilationTools | std::views::transform(&CompilationTool::name)
         | kdl::ranges::to<std::vector>())
        == std::vector<std::string>{"rbdmap", "rbdoom3bfg"});

      const auto tagNames =
        config.smartTags
        | std::views::transform([](const auto& tag) { return std::string{tag.name()}; })
        | kdl::ranges::to<std::vector>();
      CHECK(
        tagNames
        == std::vector<std::string>{
          "Trigger",     "Caulk",          "Nodraw",           "Clip",      "Collision",
          "Shadow",      "Origin",         "Visportal",        "AAS",       "Decal",
          "ClipSurface", "TriggerSurface", "Areaportal",       "Invisible", "Liquid",
          "Translucent", "AASSurface",     "CollisionSurface", "Broken",
        });
      CHECK(
        std::set<std::string>{tagNames.begin(), tagNames.end()}.size()
        == tagNames.size());
    }
  }

  SECTION("invalid input")
  {
    SECTION("parseBlankConfig")
    {
      CHECK(parseGameConfig("   ").is_error());
    }

    SECTION("parseEmptyConfig")
    {
      CHECK(parseGameConfig("  {  } ").is_error());
    }
  }

  SECTION("full config parsing")
  {
    SECTION("parseQuakeConfig")
    {
      const auto config = R"(
{
    "version": 9,
    "unexpectedKey": [],
    "name": "Quake",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" },
        { "format": "Valve" }
    ],
    "filesystem": {
        "searchpath": "id1",
        "packageformat": { "extension": "pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": ["D"],
        "palette": "gfx/palette.lmp",
        "attribute": "wad"
    },
    "entities": {
        "definitions": [ "Quake.fgd", "Quoth2.fgd", "Rubicon2.def", "Teamfortress.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "mdl", "bsp" ]
    },
    "tags": {
        "brush": [
            {
                "name": "Trigger",
                "attribs": [ "transparent" ],
                "match": "classname",
                "pattern": "trigger*"
            }
        ],
        "brushface": [
            {
                "name": "Clip",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "clip"
            },
            {
                "name": "Skip",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "skip"
            },
            {
                "name": "Hint",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "hint*"
            },
            {
                "name": "Liquid",
                "match": "material",
                "pattern": "\**"
            }
        ]
    }
}
)";

      CHECK(
        parseGameConfig(config)
        == mdl::GameConfig{
          .name = "Quake",
          .path = {},
          .icon = {"Icon.png"},
          .experimental = false,
          .fileFormats =
            {
              {.format = "Standard", .initialMap = {}},
              {.format = "Valve", .initialMap = {}},
            },
          .fileSystemConfig =
            {
              .searchPath = {"id1"},
              .packageFormat = {.extensions = {".pak"}, .format = "idpak"},
            },
          .materialConfig =
            {
              .root = {"textures"},
              .extensions = {".D"},
              .palette = {"gfx/palette.lmp"},
              .property = "wad",
              .shaderSearchPath = {},
              .excludes = {},
            },
          .entityConfig =
            {
              .defFilePaths =
                {{"Quake.fgd"}, {"Quoth2.fgd"}, {"Rubicon2.def"}, {"Teamfortress.fgd"}},
              .defaultColor = RgbaF{0.6f, 0.6f, 0.6f, 1.0f},
              .scaleExpression = {},
              .setDefaultProperties = false,
            },
          .faceAttribsConfig = {},
          .smartTags =
            {
              mdl::SmartTag{
                "Trigger",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::EntityClassNameTagMatcher>("trigger*", "")},
              mdl::SmartTag{
                "Clip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("clip")},
              mdl::SmartTag{
                "Skip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("skip")},
              mdl::SmartTag{
                "Hint",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("hint*")},
              mdl::SmartTag{
                "Liquid", {}, std::make_unique<mdl::MaterialNameTagMatcher>("\\**")},
            },
          .softMapBounds = std::nullopt,
          .compilationTools = {},
        });
    }

    SECTION("parseQuake2Config")
    {
      const auto config = R"%(
{
    "version": 9,
    "name": "Quake 2",
    "icon": "Icon.png",
    "fileformats": [ { "format": "Quake2" } ],
    "filesystem": {
        "searchpath": "baseq2",
        "packageformat": { "extension": "pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": ["wal"],
        "palette": "pics/colormap.pcx"
    },
    "entities": {
        "definitions": [ "Quake2.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "md2" ]
    },
    "tags": {
        "brush": [
            {
                "name": "Trigger",
                "attribs": [ "transparent" ],
                "match": "classname",
                "pattern": "trigger*",
                "material": "trigger"
            }
        ],
        "brushface": [
            {
                "name": "Clip",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "clip"
            },
            {
                "name": "Skip",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "skip"
            },
            {
                "name": "Hint",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "hint*"
            },
            {
                "name": "Detail",
                "match": "contentflag",
                "flags": [ "detail" ]
            },
            {
                "name": "Liquid",
                "match": "contentflag",
                "flags": [ "lava", "slime", "water" ]
            },
            {
                "name": "trans",
                "attribs": [ "transparent" ],
                "match": "surfaceflag",
                "flags": [ "trans33", "trans66" ]
            }
        ]
    },
    "faceattribs": {
        "surfaceflags": [
            {
                "name": "light",
                "description": "Emit light from the surface, brightness is specified in the 'value' field"
            },
            {
                "name": "slick",
                "description": "The surface is slippery"
            },
            {
                "name": "sky",
                "description": "The surface is sky, the texture will not be drawn, but the background sky box is used instead"
            },
            {
                "name": "warp",
                "description": "The surface warps (like water textures do)"
            },
            {
                "name": "trans33",
                "description": "The surface is 33% transparent"
            },
            {
                "name": "trans66",
                "description": "The surface is 66% transparent"
            },
            {
                "name": "flowing",
                "description": "The texture wraps in a downward 'flowing' pattern (warp must also be set)"
            },
            {
                "name": "nodraw",
                "description": "Used for non-fixed-size brush triggers and clip brushes"
            },
            {
                "name": "hint",
                "description": "Make a primary bsp splitter"
            },
            {
                "name": "skip",
                "description": "Completely ignore, allowing non-closed brushes"
            }
        ],
        "contentflags": [
            {
                "name": "solid",
                "description": "Default for all brushes"
            }, // 1 << 0
            {
                "name": "window",
                "description": "Brush is a window (not really used)"
            }, // 1 << 1
            {
                "name": "aux",
                "description": "Unused by the engine"
            }, // 1 << 2
            {
                "name": "lava",
                "description": "The brush is lava"
            }, // 1 << 3
            {
                "name": "slime",
                "description": "The brush is slime"
            }, // 1 << 4
            {
                "name": "water",
                "description": "The brush is water"
            }, // 1 << 5
            {
                "name": "mist",
                "description": "The brush is non-solid"
            }, // 1 << 6
            { "unused": true }, // 1 << 7
            { "unused": true }, // 1 << 8
            { "unused": true }, // 1 << 9
            { "unused": true }, // 1 << 10
            { "unused": true }, // 1 << 11
            { "unused": true }, // 1 << 12
            { "unused": true }, // 1 << 13
            { "unused": true }, // 1 << 14
            { "unused": true }, // 1 << 15
            {
                "name": "playerclip",
                "description": "Player cannot pass through the brush (other things can)"
            }, // 1 << 16
            {
                "name": "monsterclip",
                "description": "Monster cannot pass through the brush (player and other things can)"
            }, // 1 << 17
            {
                "name": "current_0",
                "description": "Brush has a current in direction of 0 degrees"
            }, // 1 << 18
            {
                "name": "current_90",
                "description": "Brush has a current in direction of 90 degrees"
            }, // 1 << 19
            {
                "name": "current_180",
                "description": "Brush has a current in direction of 180 degrees"
            }, // 1 << 20
            {
                "name": "current_270",
                "description": "Brush has a current in direction of 270 degrees"
            }, // 1 << 21
            {
                "name": "current_up",
                "description": "Brush has a current in the up direction"
            }, // 1 << 22
            {
                "name": "current_dn",
                "description": "Brush has a current in the down direction"
            }, // 1 << 23
            {
                "name": "origin",
                "description": "Special brush used for specifying origin of rotation for rotating brushes"
            }, // 1 << 24
            {
                "name": "monster",
                "description": "Purpose unknown"
            }, // 1 << 25
            {
                "name": "corpse",
                "description": "Purpose unknown"
            }, // 1 << 26
            {
                "name": "detail",
                "description": "Detail brush"
            }, // 1 << 27
            {
                "name": "translucent",
                "description": "Use for opaque water that does not block vis"
            }, // 1 << 28
            {
                "name": "ladder",
                "description": "Brushes with this flag allow a player to move up and down a vertical surface"
            } // 1 << 29
        ]
    }
}
)%";

      CHECK(
        parseGameConfig(config)
        == mdl::GameConfig{
          .name = "Quake 2",
          .path = {},
          .icon = {"Icon.png"},
          .experimental = false,
          .fileFormats = {{.format = "Quake2", .initialMap = {}}},
          .fileSystemConfig =
            {
              .searchPath = {"baseq2"},
              .packageFormat = {.extensions = {".pak"}, .format = "idpak"},
            },
          .materialConfig =
            {
              .root = {"textures"},
              .extensions = {".wal"},
              .palette = {"pics/colormap.pcx"},
              .property = std::nullopt,
              .shaderSearchPath = {},
              .excludes = {},
            },
          .entityConfig =
            {
              .defFilePaths = {{"Quake2.fgd"}},
              .defaultColor = RgbaF{0.6f, 0.6f, 0.6f, 1.0f},
              .scaleExpression = {},
              .setDefaultProperties = false,
            },
          .faceAttribsConfig =
            {
              .surfaceFlags =
                {.flags =
                   {{"light",
                     "Emit light from the surface, brightness is specified in the "
                     "'value' field",
                     1 << 0},
                    {"slick", "The surface is slippery", 1 << 1},
                    {"sky",
                     "The surface is sky, the texture will not be drawn, but the "
                     "background sky box is used instead",
                     1 << 2},
                    {"warp", "The surface warps (like water textures do)", 1 << 3},
                    {"trans33", "The surface is 33% transparent", 1 << 4},
                    {"trans66", "The surface is 66% transparent", 1 << 5},
                    {"flowing",
                     "The texture wraps in a downward 'flowing' pattern (warp must also "
                     "be set)",
                     1 << 6},
                    {"nodraw",
                     "Used for non-fixed-size brush triggers and clip brushes",
                     1 << 7},
                    {"hint", "Make a primary bsp splitter", 1 << 8},
                    {"skip", "Completely ignore, allowing non-closed brushes", 1 << 9}}},
              .contentFlags =
                {.flags =
                   {{"solid", "Default for all brushes", 1 << 0},
                    {"window", "Brush is a window (not really used)", 1 << 1},
                    {"aux", "Unused by the engine", 1 << 2},
                    {"lava", "The brush is lava", 1 << 3},
                    {"slime", "The brush is slime", 1 << 4},
                    {"water", "The brush is water", 1 << 5},
                    {"mist", "The brush is non-solid", 1 << 6},
                    {"playerclip",
                     "Player cannot pass through the brush (other things can)",
                     1 << 16},
                    {"monsterclip",
                     "Monster cannot pass through the brush (player and other things "
                     "can)",
                     1 << 17},
                    {"current_0",
                     "Brush has a current in direction of 0 degrees",
                     1 << 18},
                    {"current_90",
                     "Brush has a current in direction of 90 degrees",
                     1 << 19},
                    {"current_180",
                     "Brush has a current in direction of 180 degrees",
                     1 << 20},
                    {"current_270",
                     "Brush has a current in direction of 270 degrees",
                     1 << 21},
                    {"current_up", "Brush has a current in the up direction", 1 << 22},
                    {"current_dn", "Brush has a current in the down direction", 1 << 23},
                    {"origin",
                     "Special brush used for specifying origin of rotation for rotating "
                     "brushes",
                     1 << 24},
                    {"monster", "Purpose unknown", 1 << 25},
                    {"corpse", "Purpose unknown", 1 << 26},
                    {"detail", "Detail brush", 1 << 27},
                    {"translucent",
                     "Use for opaque water that does not block vis",
                     1 << 28},
                    {"ladder",
                     "Brushes with this flag allow a player to move up and down a "
                     "vertical surface",
                     1 << 29}}},
              .defaultUvAttributes = mdl::UvAttributes{},
              .defaultSurfaceAttributes = mdl::SurfaceAttributes{},
            },
          .smartTags =
            {
              mdl::SmartTag{
                "Trigger",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::EntityClassNameTagMatcher>("trigger*", "trigger")},
              mdl::SmartTag{
                "Clip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("clip")},
              mdl::SmartTag{
                "Skip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("skip")},
              mdl::SmartTag{
                "Hint",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("hint*")},
              mdl::SmartTag{
                "Detail", {}, std::make_unique<mdl::ContentFlagsTagMatcher>(1 << 27)},
              mdl::SmartTag{
                "Liquid",
                {},
                std::make_unique<mdl::ContentFlagsTagMatcher>(
                  (1 << 3) | (1 << 4) | (1 << 5))},
              mdl::SmartTag{
                "trans",
                {},
                std::make_unique<mdl::SurfaceFlagsTagMatcher>((1 << 4) | (1 << 5))},
            },
          .softMapBounds = std::nullopt,
          .compilationTools = {},
        });
    }

    SECTION("parseExtrasConfig")
    {
      const auto config = R"%(
{
    "version": 9,
    "name": "Extras",
    "fileformats": [ { "format": "Quake3" } ],
    "filesystem": {
        "searchpath": "baseq3",
        "packageformat": { "extension": "pk3", "format": "zip" }
    },
    "materials": {
        "root": "textures",
        "extensions": [ "" ],
        "shaderSearchPath": "scripts", // this will likely change when we get a material system
        "excludes": [
            "*_norm",
            "*_gloss"
        ]
    },
    "entities": {
        "definitions": [ "Extras.ent" ],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "md3" ],
        "scale": [ modelscale, modelscale_vec ]
    },
    "tags": {
        "brush": [
            {
                "name": "Trigger",
                "attribs": [ "transparent" ],
                "match": "classname",
                "pattern": "trigger*",
                "material": "trigger"
            }
        ],
        "brushface": [
            {
                "name": "Clip",
                "attribs": [ "transparent" ],
                "match": "surfaceparm",
                "pattern": "playerclip"
            },
            {
                "name": "Skip",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "skip"
            },
            {
                "name": "Hint",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "hint*"
            },
            {
                "name": "Detail",
                "match": "contentflag",
                "flags": [ "detail" ]
            },
            {
                "name": "Liquid",
                "match": "contentflag",
                "flags": [ "lava", "slime", "water" ]
            }
        ]
    },
    "faceattribs": {
        "defaults": {
            "materialName": "defaultMaterial", // ignored for backward compatibility
            "offset": [0, 0],
            "scale": [0.5, 0.5],
            "rotation": 0,
            "surfaceFlags": [ "slick" ],
            "surfaceContents": [ "solid" ],
            "surfaceValue": 0,
            "color": "0 128 255"
        },
        "surfaceflags": [
            {
                "name": "light",
                "description": "Emit light from the surface, brightness is specified in the 'value' field"
            },
            {
                "name": "slick",
                "description": "The surface is slippery"
            },
            {
                "name": "sky",
                "description": "The surface is sky, the texture will not be drawn, but the background sky box is used instead"
            },
            {
                "name": "warp",
                "description": "The surface warps (like water textures do)"
            },
            {
                "name": "trans33",
                "description": "The surface is 33% transparent"
            },
            {
                "name": "trans66",
                "description": "The surface is 66% transparent"
            },
            {
                "name": "flowing",
                "description": "The texture wraps in a downward 'flowing' pattern (warp must also be set)"
            },
            {
                "name": "nodraw",
                "description": "Used for non-fixed-size brush triggers and clip brushes"
            },
            {
                "name": "hint",
                "description": "Make a primary bsp splitter"
            },
            {
                "name": "skip",
                "description": "Completely ignore, allowing non-closed brushes"
            }
        ],
        "contentflags": [
            {
                "name": "solid",
                "description": "Default for all brushes"
            }, // 1
            {
                "name": "window",
                "description": "Brush is a window (not really used)"
            }, // 2
            {
                "name": "aux",
                "description": "Unused by the engine"
            }, // 4
            {
                "name": "lava",
                "description": "The brush is lava"
            }, // 8
            {
                "name": "slime",
                "description": "The brush is slime"
            }, // 16
            {
                "name": "water",
                "description": "The brush is water"
            }, // 32
            {
                "name": "mist",
                "description": "The brush is non-solid"
            }, // 64
            { "unused": true }, // 128
            { "unused": true }, // 256
            { "unused": true }, // 512
            { "unused": true }, // 1024
            { "unused": true }, // 2048
            { "unused": true }, // 4096
            { "unused": true }, // 8192
            { "unused": true }, // 16384
            { "unused": true }, // 32768
            {
                "name": "playerclip",
                "description": "Player cannot pass through the brush (other things can)"
            }, // 65536
            {
                "name": "monsterclip",
                "description": "Monster cannot pass through the brush (player and other things can)"
            }, // 131072
            {
                "name": "current_0",
                "description": "Brush has a current in direction of 0 degrees"
            },
            {
                "name": "current_90",
                "description": "Brush has a current in direction of 90 degrees"
            },
            {
                "name": "current_180",
                "description": "Brush has a current in direction of 180 degrees"
            },
            {
                "name": "current_270",
                "description": "Brush has a current in direction of 270 degrees"
            },
            {
                "name": "current_up",
                "description": "Brush has a current in the up direction"
            },
            {
                "name": "current_dn",
                "description": "Brush has a current in the down direction"
            },
            {
                "name": "origin",
                "description": "Special brush used for specifying origin of rotation for rotating brushes"
            },
            {
                "name": "monster",
                "description": "Purpose unknown"
            },
            {
                "name": "corpse",
                "description": "Purpose unknown"
            },
            {
                "name": "detail",
                "description": "Detail brush"
            },
            {
                "name": "translucent",
                "description": "Use for opaque water that does not block vis"
            },
            {
                "name": "ladder",
                "description": "Brushes with this flag allow a player to move up and down a vertical surface"
            }
        ]
    }
}
)%";

      const auto expectedUvAttributes = mdl::UvAttributes{
        .offset = {0.0f, 0.0f},
        .scale = {0.5f, 0.5f},
        .rotation = 0.0f,
      };
      const auto expectedSurfaceAttributes = mdl::SurfaceAttributes{
        .contents = 1 << 0,
        .flags = 1 << 1,
        .value = 0.0f,
        .color = RgbB(0, 128, 255),
      };

      CHECK(
        parseGameConfig(config)
        == mdl::GameConfig{
          .name = "Extras",
          .path = {},
          .icon = {},
          .experimental = false,
          .fileFormats = {{.format = "Quake3", .initialMap = {}}},
          .fileSystemConfig =
            {
              .searchPath = {"baseq3"},
              .packageFormat = {.extensions = {".pk3"}, .format = "zip"},
            },
          .materialConfig =
            {
              .root = {"textures"},
              .extensions = {""},
              .palette = {},
              .property = std::nullopt,
              .shaderSearchPath = {"scripts"},
              .excludes = {"*_norm", "*_gloss"},
            },
          .entityConfig =
            {
              .defFilePaths = {{"Extras.ent"}},
              .defaultColor = RgbaF{0.6f, 0.6f, 0.6f, 1.0f},
              .scaleExpression = el::ExpressionNode{el::ArrayExpression{{
                // the line numbers are not checked
                el::ExpressionNode{el::VariableExpression{"modelscale"}},
                el::ExpressionNode{el::VariableExpression{"modelscale_vec"}},
              }}},
              .setDefaultProperties = false,
            },
          .faceAttribsConfig =
            {
              .surfaceFlags =
                {.flags =
                   {{"light",
                     "Emit light from the surface, brightness is specified in the "
                     "'value' field",
                     1 << 0},
                    {"slick", "The surface is slippery", 1 << 1},
                    {"sky",
                     "The surface is sky, the texture will not be drawn, but the "
                     "background sky box is used instead",
                     1 << 2},
                    {"warp", "The surface warps (like water textures do)", 1 << 3},
                    {"trans33", "The surface is 33% transparent", 1 << 4},
                    {"trans66", "The surface is 66% transparent", 1 << 5},
                    {"flowing",
                     "The texture wraps in a downward 'flowing' pattern (warp must also "
                     "be set)",
                     1 << 6},
                    {"nodraw",
                     "Used for non-fixed-size brush triggers and clip brushes",
                     1 << 7},
                    {"hint", "Make a primary bsp splitter", 1 << 8},
                    {"skip", "Completely ignore, allowing non-closed brushes", 1 << 9}}},
              .contentFlags =
                {.flags =
                   {{"solid", "Default for all brushes", 1 << 0},
                    {"window", "Brush is a window (not really used)", 1 << 1},
                    {"aux", "Unused by the engine", 1 << 2},
                    {"lava", "The brush is lava", 1 << 3},
                    {"slime", "The brush is slime", 1 << 4},
                    {"water", "The brush is water", 1 << 5},
                    {"mist", "The brush is non-solid", 1 << 6},
                    {"playerclip",
                     "Player cannot pass through the brush (other things can)",
                     1 << 16},
                    {"monsterclip",
                     "Monster cannot pass through the brush (player and other things "
                     "can)",
                     1 << 17},
                    {"current_0",
                     "Brush has a current in direction of 0 degrees",
                     1 << 18},
                    {"current_90",
                     "Brush has a current in direction of 90 degrees",
                     1 << 19},
                    {"current_180",
                     "Brush has a current in direction of 180 degrees",
                     1 << 20},
                    {"current_270",
                     "Brush has a current in direction of 270 degrees",
                     1 << 21},
                    {"current_up", "Brush has a current in the up direction", 1 << 22},
                    {"current_dn", "Brush has a current in the down direction", 1 << 23},
                    {"origin",
                     "Special brush used for specifying origin of rotation for rotating "
                     "brushes",
                     1 << 24},
                    {"monster", "Purpose unknown", 1 << 25},
                    {"corpse", "Purpose unknown", 1 << 26},
                    {"detail", "Detail brush", 1 << 27},
                    {"translucent",
                     "Use for opaque water that does not block vis",
                     1 << 28},
                    {"ladder",
                     "Brushes with this flag allow a player to move up and down a "
                     "vertical surface",
                     1 << 29}}},
              .defaultUvAttributes = expectedUvAttributes,
              .defaultSurfaceAttributes = expectedSurfaceAttributes,
            },
          .smartTags =
            {
              mdl::SmartTag{
                "Trigger",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::EntityClassNameTagMatcher>("trigger*", "trigger")},
              mdl::SmartTag{
                "Clip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("clip")},
              mdl::SmartTag{
                "Skip",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("skip")},
              mdl::SmartTag{
                "Hint",
                {mdl::TagAttribute{1u, "transparent"}},
                std::make_unique<mdl::MaterialNameTagMatcher>("hint*")},
              mdl::SmartTag{
                "Detail", {}, std::make_unique<mdl::ContentFlagsTagMatcher>(1 << 27)},
              mdl::SmartTag{
                "Liquid",
                {},
                std::make_unique<mdl::ContentFlagsTagMatcher>(
                  (1 << 3) | (1 << 4) | (1 << 5))},
            },
          .softMapBounds = std::nullopt,
          .compilationTools = {},
        });
    }
  }

  SECTION("tags")
  {
    SECTION("parseDuplicateTags")
    {
      const auto config = R"(
{
    "version": 9,
    "name": "Quake",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" }
    ],
    "filesystem": {
        "searchpath": "id1",
        "packageformat": { "extension": "pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": ["D"],
        "palette": "gfx/palette.lmp",
        "attribute": "wad"
    },
    "entities": {
        "definitions": [ "Quake.fgd", "Quoth2.fgd", "Rubicon2.def", "Teamfortress.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "mdl", "bsp" ]
    },
    "tags": {
        "brush": [
            {
                "name": "Trigger",
                "attribs": [ "transparent" ],
                "match": "classname",
                "pattern": "trigger*"
            }
        ],
        "brushface": [
            {
                "name": "Trigger",
                "attribs": [ "transparent" ],
                "match": "material",
                "pattern": "clip"
            }
        ]
    }
}
)";

      CHECK(parseGameConfig(config).is_error());
    }
  }

  SECTION("properties")
  {
    SECTION("parseSetDefaultProperties")
    {
      const auto config = R"(
{
    "version": 9,
    "name": "Quake",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" }
    ],
    "filesystem": {
        "searchpath": "id1",
        "packageformat": { "extension": "pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": ["D"],
        "palette": "gfx/palette.lmp",
        "attribute": "wad"
    },
    "entities": {
        "definitions": [ "Quake.fgd", "Quoth2.fgd", "Rubicon2.def", "Teamfortress.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "mdl", "bsp" ],
        "setDefaultProperties": true
    }
}
)";

      CHECK(
        parseGameConfig(config)
        == mdl::GameConfig{
          .name = "Quake",
          .path = {},
          .icon = {"Icon.png"},
          .experimental = false,
          .fileFormats = {{.format = "Standard", .initialMap = {}}},
          .fileSystemConfig =
            {
              .searchPath = {"id1"},
              .packageFormat = {.extensions = {".pak"}, .format = "idpak"},
            },
          .materialConfig =
            {
              .root = {"textures"},
              .extensions = {".D"},
              .palette = {"gfx/palette.lmp"},
              .property = "wad",
              .shaderSearchPath = {},
              .excludes = {},
            },
          .entityConfig =
            {
              .defFilePaths =
                {{"Quake.fgd"}, {"Quoth2.fgd"}, {"Rubicon2.def"}, {"Teamfortress.fgd"}},
              .defaultColor = RgbaF{0.6f, 0.6f, 0.6f, 1.0f},
              .scaleExpression = {},
              .setDefaultProperties = true,
            },
          .faceAttribsConfig = {},
          .smartTags = {},
          .softMapBounds = std::nullopt,
          .compilationTools = {},
        });
    }
  }

  SECTION("material scripts")
  {
    const auto makeConfig = [](const std::string& materials) {
      return R"%(
{
    "version": 9,
    "name": "Doom 3 BFG",
    "fileformats": [ { "format": "Quake3 (Valve)" } ],
    "filesystem": {
        "searchpath": "base",
        "packageformat": { "extension": "pk4", "format": "zip" }
    },
    "materials": )%"
             + materials + R"%(,
    "entities": {
        "definitions": [],
        "defaultcolor": "0.6 0.6 0.6 1.0"
    }
}
)%";
    };

    SECTION("shaderExtensions and shaderImageSearchPaths default")
    {
      const auto config = makeConfig(R"%(
    {
        "root": "textures",
        "extensions": [ ".tga" ],
        "shaderSearchPath": "scripts"
    })%");

      const auto materialConfig = (parseGameConfig(config) | kdl::value()).materialConfig;
      CHECK(
        materialConfig.shaderExtensions == std::vector<std::filesystem::path>{".shader"});
      CHECK(materialConfig.shaderImageSearchPaths.empty());
      CHECK(
        materialConfig
        == MaterialConfig{
          .root = "textures",
          .extensions = {".tga"},
          .palette = {},
          .property = std::nullopt,
          .shaderSearchPath = "scripts",
          .excludes = {},
        });
    }

    SECTION("shaderExtensions and shaderImageSearchPaths are read")
    {
      const auto config = makeConfig(R"%(
    {
        "root": "textures",
        "extensions": [ ".tga", ".png", ".jpg", ".dds" ],
        "shaderSearchPath": "materials",
        "shaderExtensions": [ ".mtr", "shader" ],
        "shaderImageSearchPaths": [ "_tb", "extra/images" ]
    })%");

      const auto materialConfig = (parseGameConfig(config) | kdl::value()).materialConfig;
      CHECK(
        materialConfig
        == MaterialConfig{
          .root = "textures",
          .extensions = {".tga", ".png", ".jpg", ".dds"},
          .palette = {},
          .property = std::nullopt,
          .shaderSearchPath = "materials",
          .excludes = {},
          // like the material extensions, a missing leading dot is added
          .shaderExtensions = {".mtr", ".shader"},
          .shaderImageSearchPaths = {"_tb", "extra/images"},
        });
    }

    SECTION("the new keys take part in comparisons")
    {
      const auto config = MaterialConfig{.root = "textures"};

      auto otherExtensions = config;
      otherExtensions.shaderExtensions = {".mtr"};
      CHECK(otherExtensions != config);

      auto otherImageSearchPaths = config;
      otherImageSearchPaths.shaderImageSearchPaths = {"_tb"};
      CHECK(otherImageSearchPaths != config);
    }
  }
}

} // namespace tb::mdl
