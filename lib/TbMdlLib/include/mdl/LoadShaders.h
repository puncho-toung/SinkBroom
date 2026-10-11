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

#pragma once

#include "base/Result.h"
#include "mdl/Quake3Shader.h"

#include <cstddef>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
class Logger;

namespace fs
{
class FileSystem;
} // namespace fs

namespace mdl
{
struct MaterialConfig;

/**
 * The records that loadShaderFiles found, and what a summary of the load needs to know
 * about the files they came from.
 */
struct LoadedShaders
{
  std::vector<Quake3Shader> shaders;
  // the number of material script files that were read
  std::size_t fileCount = 0;
  // the number of Doom 3 material decls that lack their closing brace and run to the end
  // of their file
  std::size_t unterminatedCount = 0;
};

/**
 * Reads the material script files in materialConfig.shaderSearchPath (not in its
 * subdirectories) whose extensions are listed in materialConfig.shaderExtensions.
 *
 * The extension selects the parser: .mtr files are read by Doom3MaterialParser, all
 * others by Quake3ShaderParser. The files are read in the order of their paths compared
 * without case, as the engine lists decl files, and the first definition of a shader
 * name, compared without case, wins; later ones are dropped with a warning that names
 * both files. Every record
 * is kept whatever its root, so that entity models can use materials outside of
 * materialConfig.root.
 *
 * A Doom 3 material that looks up a table which no material file declares is replaced
 * with the engine's default material (see makeEngineDefaulted), as the engine does if no
 * other decl file declares the table.
 *
 * The records are sorted by shader path.
 */
Result<LoadedShaders> loadShaderFiles(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  kdl::task_manager& taskManager,
  Logger& logger);

/**
 * Returns the records that loadShaderFiles finds, and logs their number.
 */
Result<std::vector<Quake3Shader>> loadShaders(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  kdl::task_manager& taskManager,
  Logger& logger);

} // namespace mdl
} // namespace tb
