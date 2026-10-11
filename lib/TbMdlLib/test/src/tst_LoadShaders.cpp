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
#include "fs/VirtualFileSystem.h"
#include "mdl/CatchConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/LoadShaders.h"
#include "mdl/Quake3Shader.h"

#include "kd/result.h"
#include "kd/task_manager.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
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

const Quake3Shader* findShader(
  const std::vector<Quake3Shader>& shaders, const std::filesystem::path& shaderPath)
{
  const auto iShader = std::ranges::find(shaders, shaderPath, &Quake3Shader::shaderPath);
  return iShader != shaders.end() ? &*iShader : nullptr;
}

const auto Doom3FixtureMaterialConfig = MaterialConfig{
  .root = "textures",
  .extensions = {".tga", ".png", ".jpg", ".dds"},
  .palette = "",
  .property = std::nullopt,
  .shaderSearchPath = "materials",
  .excludes = {"*_d", "*_ed", "*_add"},
  .shaderExtensions = {".mtr"},
  .shaderImageSearchPaths = {"_tb"},
};

const auto Quake3FixtureMaterialConfig = MaterialConfig{
  .root = "textures",
  .extensions = {".tga", ".png", ".jpg", ".jpeg"},
  .palette = "",
  .property = std::nullopt,
  .shaderSearchPath = "scripts",
  .excludes = {},
};

} // namespace

TEST_CASE("LoadShaders")
{
  auto fs = fs::VirtualFileSystem{};
  auto logger = CollectingLogger{};

  const auto fixtureRoot = getFixtureRoot();

  auto taskManager = kdl::task_manager{};

  SECTION("loadShaderFiles")
  {
    SECTION("without a shader search path")
    {
      fs.mount(
        "",
        std::make_unique<fs::DiskFileSystem>(
          fixtureRoot / "test/mdl/LoadMaterialCollections/doom3"));

      auto materialConfig = Doom3FixtureMaterialConfig;
      materialConfig.shaderSearchPath = "no_such_directory";

      const auto loadedShaders =
        loadShaderFiles(fs, materialConfig, taskManager, logger) | kdl::value();
      CHECK(loadedShaders.shaders.empty());
      CHECK(loadedShaders.fileCount == 0);
      CHECK(loadedShaders.unterminatedCount == 0);
    }

    SECTION("Quake 3 shaders defined in two files")
    {
      const auto testDir =
        fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/duplicate_shader";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      // shaderExtensions keeps its default
      const auto loadedShaders =
        loadShaderFiles(fs, Quake3FixtureMaterialConfig, taskManager, logger)
        | kdl::value();
      CHECK(loadedShaders.fileCount == 3);

      // the files are read in sorted order, ignoring case, and the first definition wins
      REQUIRE(loadedShaders.shaders.size() == 2);
      const auto* duplicate =
        findShader(loadedShaders.shaders, "textures/test/duplicate");
      REQUIRE(duplicate != nullptr);
      CHECK(duplicate->editorImage == "textures/test/from_a.tga");

      // names that differ only in case are the same name
      const auto* differentCase = findShader(loadedShaders.shaders, "textures/test/Case");
      REQUIRE(differentCase != nullptr);
      CHECK(differentCase->editorImage == "textures/test/case_upper.tga");
      CHECK(findShader(loadedShaders.shaders, "textures/test/case") == nullptr);

      CHECK(logger.countMessagesContaining("it is already defined in") == 2);
    }

    SECTION("Doom 3 materials")
    {
      const auto testDir = fixtureRoot / "test/mdl/LoadMaterialCollections/doom3";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      const auto loadedShaders =
        loadShaderFiles(fs, Doom3FixtureMaterialConfig, taskManager, logger)
        | kdl::value();
      const auto& shaders = loadedShaders.shaders;

      // only files with a configured extension are read, whatever its case
      CHECK(loadedShaders.fileCount == 3);
      CHECK(findShader(shaders, "textures/test/ignored") == nullptr);

      CHECK(shaders.size() == 26);
      CHECK(std::ranges::is_sorted(shaders, std::less{}, &Quake3Shader::shaderPath));
      CHECK(std::ranges::all_of(shaders, [](const auto& shader) {
        return shader.format == Quake3Shader::Format::Doom3;
      }));

      // records outside of the material root are kept for entity models
      CHECK(findShader(shaders, "models/test/crate") != nullptr);

      // the files are read in sorted order, ignoring case, and the first definition wins
      const auto* duplicate = findShader(shaders, "textures/test/duplicate");
      REQUIRE(duplicate != nullptr);
      CHECK(
        duplicate->previewImages
        == std::vector<std::filesystem::path>{"textures/test/editor_ed.tga"});
      CHECK(logger.countMessagesContaining("it is already defined in") == 2);

      // a decl without its closing brace is counted, and the engine defaults it
      CHECK(loadedShaders.unterminatedCount == 1);
      const auto* unterminated = findShader(shaders, "textures/test/unterminated");
      REQUIRE(unterminated != nullptr);
      CHECK(unterminated->engineDefaulted);

      // a table declared in another file is found
      const auto* tableDeclared = findShader(shaders, "textures/test/table_declared");
      REQUIRE(tableDeclared != nullptr);
      CHECK_FALSE(tableDeclared->engineDefaulted);
      CHECK(tableDeclared->referencedTables == std::vector<std::string>{"fixturetable"});

      // the engine defaults a material that looks up a table no file declares; the
      // material keeps its editor image
      const auto* tableUndeclared = findShader(shaders, "textures/test/table_undeclared");
      REQUIRE(tableUndeclared != nullptr);
      CHECK(tableUndeclared->engineDefaulted);
      CHECK(tableUndeclared->surfaceParms.contains("engine_defaulted"));
      CHECK(
        tableUndeclared->previewImages
        == std::vector<std::filesystem::path>{"textures/test/editor_ed.tga"});
      CHECK(
        logger.countMessagesContaining(
          "No material file declares table 'undeclared_table'")
        == 1);
    }

    SECTION("Doom 3 decls without their closing brace")
    {
      const auto testDir = fixtureRoot / "test/mdl/LoadShaders/unterminated";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      const auto loadedShaders =
        loadShaderFiles(fs, Doom3FixtureMaterialConfig, taskManager, logger)
        | kdl::value();

      // a comment left open after the last decl doesn't count, but a decl whose closing
      // brace is inside a comment left open does
      CHECK(loadedShaders.unterminatedCount == 1);

      const auto* closed = findShader(loadedShaders.shaders, "textures/test/closed");
      REQUIRE(closed != nullptr);
      CHECK_FALSE(closed->unterminated);

      const auto* swallowed =
        findShader(loadedShaders.shaders, "textures/test/swallowed");
      REQUIRE(swallowed != nullptr);
      CHECK(swallowed->unterminated);
      CHECK(swallowed->engineDefaulted);
    }
  }

  SECTION("loadShaders")
  {
    fs.mount(
      "",
      std::make_unique<fs::DiskFileSystem>(
        fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/duplicate_shader"));

    const auto shaders =
      loadShaders(fs, Quake3FixtureMaterialConfig, taskManager, logger) | kdl::value();
    CHECK(shaders.size() == 2);
    CHECK(logger.countMessagesContaining("Found 2 shaders") == 1);
  }
}

} // namespace tb::mdl
