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
#include "base/ParserStatus.h"
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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{

namespace
{

/**
 * Logs like SimpleParserStatus, and counts the material decls that Doom3MaterialParser
 * reports as running to the end of their file.
 */
class Doom3MaterialFileStatus : public ParserStatus
{
private:
  Logger& m_logger;
  size_t m_unterminatedCount = 0;

public:
  Doom3MaterialFileStatus(Logger& logger, std::string prefix)
    : ParserStatus{logger, std::move(prefix)}
    , m_logger{logger}
  {
  }

  size_t unterminatedCount() const { return m_unterminatedCount; }

private:
  void doProgress(const double /* progress */) override {}

  void doLog(const LogLevel level, const std::string& str) override
  {
    // the warning that Doom3MaterialParser issues for a decl without its closing brace
    if (level == LogLevel::Warn && str.ends_with("runs to the end of the file"))
    {
      ++m_unterminatedCount;
    }
    m_logger.log(level, str);
  }
};

struct ShaderFile
{
  std::filesystem::path path;
  std::vector<Quake3Shader> shaders;
  std::vector<std::string> declaredTables;
  size_t unterminatedCount = 0;
};

bool isDoom3MaterialFile(const std::filesystem::path& path)
{
  return kdl::path_to_lower(path.extension()) == ".mtr";
}

Result<ShaderFile> loadShaderFile(
  const fs::FileSystem& fs, const std::filesystem::path& path, Logger& logger)
{
  return fs.openFile(path) | kdl::and_then([&](auto file) -> Result<ShaderFile> {
           auto bufferedReader = file->reader().buffer();
           if (isDoom3MaterialFile(path))
           {
             // the Doom 3 parser reports every problem as a warning and never fails
             auto parser = Doom3MaterialParser{bufferedReader.stringView()};
             auto status = Doom3MaterialFileStatus{logger, path.string()};
             auto shaders = parser.parse(status);
             return ShaderFile{
               path,
               std::move(shaders),
               parser.declaredTables(),
               status.unterminatedCount(),
             };
           }

           auto parser = Quake3ShaderParser{bufferedReader.stringView()};
           auto status = SimpleParserStatus{logger, path.string()};
           return parser.parse(status) | kdl::transform([&](auto shaders) {
                    return ShaderFile{path, std::move(shaders), {}, 0};
                  });
         })
         | kdl::transform_error([&](const auto& e) {
             logger.warn() << "Skipping malformed shader file " << path << ": " << e.msg;
             return ShaderFile{path, {}, {}, 0};
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

  // Like idDeclFile::LoadAndParse, keep the first definition of a name. The files are in
  // sorted order, and each file's records are in file order.
  auto definingFiles =
    std::unordered_map<std::filesystem::path, size_t, kdl::path_hash>{};
  auto declaredTables = std::unordered_set<std::string>{};
  for (size_t i = 0; i < files.size(); ++i)
  {
    auto& file = files[i];
    result.unterminatedCount += file.unterminatedCount;
    declaredTables.insert(file.declaredTables.begin(), file.declaredTables.end());

    for (auto& shader : file.shaders)
    {
      if (const auto [definingFile, inserted] =
            definingFiles.try_emplace(shader.shaderPath, i);
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
  // that no file declares.
  for (auto& shader : result.shaders)
  {
    if (const auto undeclaredTable = std::ranges::find_if(
          shader.referencedTables,
          [&](const auto& table) { return !declaredTables.contains(table); });
        undeclaredTable != shader.referencedTables.end())
    {
      logger.warn() << fmt::format(
        "Material '{:g}' looks up the table '{}', which no material file declares; the "
        "engine replaces this material with its default material",
        shader.shaderPath,
        *undeclaredTable);
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
             auto tasks =
               sortedPaths | std::views::transform([&](const auto& path) {
                 return std::function{[&]() { return loadShaderFile(fs, path, logger); }};
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
