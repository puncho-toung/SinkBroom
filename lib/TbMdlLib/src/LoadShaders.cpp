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

#include "mdl/LoadShaders.h"

#include "base/Error.h" // IWYU pragma: keep
#include "base/Logger.h"
#include "base/SimpleParserStatus.h"
#include "fs/FileSystem.h"
#include "fs/PathInfo.h"
#include "fs/TraversalMode.h"
#include "mdl/Doom3MaterialParser.h"
#include "mdl/GameConfig.h"
#include "mdl/Quake3Shader.h"
#include "mdl/Quake3ShaderParser.h"

#include "kd/path_hash.h"
#include "kd/path_utils.h"
#include "kd/result.h"
#include "kd/result_fold.h"
#include "kd/task_manager.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <functional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{

namespace
{

struct ShaderFile
{
  std::filesystem::path path;
  std::vector<Quake3Shader> shaders;
  std::vector<std::string> declaredTables;
};

bool isDoom3MaterialFile(const std::filesystem::path& path)
{
  return kdl::path_to_lower(path.extension()) == ".mtr";
}

Result<ShaderFile> parseShaderFile(
  const fs::FileSystem& fs, const std::filesystem::path& path, Logger& logger)
{
  return fs.openFile(path) | kdl::and_then([&](auto file) -> Result<ShaderFile> {
           auto bufferedReader = file->reader().buffer();
           auto status = SimpleParserStatus{logger, path.string()};
           if (isDoom3MaterialFile(path))
           {
             // the Doom 3 parser reports every problem as a warning and never fails
             auto parser = Doom3MaterialParser{bufferedReader.stringView()};
             auto shaders = parser.parse(status);
             return ShaderFile{path, std::move(shaders), parser.declaredTables()};
           }

           auto parser = Quake3ShaderParser{bufferedReader.stringView()};
           return parser.parse(status) | kdl::transform([&](auto shaders) {
                    return ShaderFile{path, std::move(shaders), {}};
                  });
         })
         | kdl::transform_error([&](const auto& e) {
             logger.warn() << "Skipping malformed shader file " << path << ": " << e.msg;
             return ShaderFile{path, {}, {}};
           });
}

std::vector<std::filesystem::path> sortShaderFilePaths(
  std::vector<std::filesystem::path> paths)
{
  // the engine sorts decl files without case (idFileSystemLocal::ListFiles), and file
  // system listings are not sorted at all
  std::ranges::sort(paths, [](const auto& lhs, const auto& rhs) {
    return kdl::path_to_lower(lhs) < kdl::path_to_lower(rhs);
  });
  return paths;
}

LoadedShaders mergeShaderFiles(std::vector<ShaderFile> files, Logger& logger)
{
  auto result = LoadedShaders{.shaders = {}, .fileCount = files.size()};

  // Like idDeclFile::LoadAndParse, keep the first definition of a name, ignoring case
  // like the file system and the material manager. The files are in sorted order, and
  // each file's records are in file order.
  auto definingFiles =
    std::unordered_map<std::filesystem::path, size_t, kdl::path_hash>{};
  auto declaredTables = std::unordered_set<std::string>{};
  for (size_t i = 0; i < files.size(); ++i)
  {
    auto& file = files[i];
    result.unterminatedCount += size_t(std::ranges::count_if(
      file.shaders, [](const auto& shader) { return shader.unterminated; }));
    declaredTables.insert(file.declaredTables.begin(), file.declaredTables.end());

    for (auto& shader : file.shaders)
    {
      if (const auto [definingFile, inserted] =
            definingFiles.try_emplace(kdl::path_to_lower(shader.shaderPath), i);
          !inserted)
      {
        logger.warn() << fmt::format(
          "Skipping shader '{:g}' in {:g}, it is already defined in {:g}",
          shader.shaderPath,
          file.path,
          files[definingFile->second].path);
        continue;
      }
      result.shaders.push_back(std::move(shader));
    }
  }

  // Tables can be declared in any file, so only now is it known whether the tables a
  // Doom 3 material looks up exist. The engine defaults a material that looks up a table
  // that no decl file declares. It accepts table decls in the files of every registered
  // decl folder (idDeclFile::LoadAndParse at framework/DeclManager.cpp:744-752; folders
  // at :936, :995-996 and d3xp/Game_local.cpp:327-331), but only the material files are
  // read here, so a table declared in, say, a .def file is missed and the material is
  // defaulted wrongly.
  for (auto& shader : result.shaders)
  {
    if (const auto undeclaredTable = std::ranges::find_if(
          shader.referencedTables,
          [&](const auto& table) { return !declaredTables.contains(table); });
        undeclaredTable != shader.referencedTables.end())
    {
      logger.warn() << fmt::format(
        "No material file declares table '{}', which material '{:g}' looks up; unless "
        "another decl file declares it, the engine replaces this material with its "
        "default material",
        *undeclaredTable,
        shader.shaderPath);
      makeEngineDefaulted(shader);
    }
  }

  std::ranges::sort(result.shaders, std::less{}, &Quake3Shader::shaderPath);
  return result;
}

} // namespace

Result<LoadedShaders> loadShaderFiles(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  kdl::task_manager& taskManager,
  Logger& logger)
{
  if (fs.pathInfo(materialConfig.shaderSearchPath) != fs::PathInfo::Directory)
  {
    return LoadedShaders{};
  }

  return fs.find(
           materialConfig.shaderSearchPath,
           fs::TraversalMode::Flat,
           fs::makeExtensionPathMatcher(materialConfig.shaderExtensions))
         | kdl::and_then([&](auto paths) {
             const auto sortedPaths = sortShaderFilePaths(std::move(paths));
             auto tasks = sortedPaths | std::views::transform([&](const auto& path) {
                            return std::function{
                              [&]() { return parseShaderFile(fs, path, logger); }};
                          });
             return taskManager.run_tasks_and_wait(tasks) | kdl::fold;
           })
         | kdl::transform(
           [&](auto files) { return mergeShaderFiles(std::move(files), logger); });
}

Result<std::vector<Quake3Shader>> loadShaders(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  kdl::task_manager& taskManager,
  Logger& logger)
{
  return loadShaderFiles(fs, materialConfig, taskManager, logger)
         | kdl::transform([&](auto loadedShaders) {
             logger.info() << "Found " << loadedShaders.shaders.size() << " shaders";
             return std::move(loadedShaders.shaders);
           });
}

} // namespace tb::mdl
