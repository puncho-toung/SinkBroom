/*
 Copyright (C) 2024 Kristian Duske

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
#include "mdl/EntityModelDataResource.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace tb
{
class Logger;

namespace gl
{
class Material;
}

namespace fs
{
class FileSystem;
} // namespace fs

namespace mdl
{
class EntityModel;
struct MaterialConfig;
enum class ModelConvention;

using LoadMaterialFunc = std::function<gl::Material(const std::filesystem::path&)>;
using FindMaterialFunc =
  std::function<std::optional<gl::Material>(const std::filesystem::path&)>;

/**
 * Loads the model at the given path with the loader that its format needs. Under
 * ModelConvention::IdTech4, a .obj file is read by loadIdTech4ObjModel instead of assimp,
 * and findMaterial resolves the material decl names of such models.
 */
Result<EntityModel> loadEntityModelSync(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  ModelConvention modelConvention,
  const std::filesystem::path& path,
  const LoadMaterialFunc& loadMaterial,
  const FindMaterialFunc& findMaterial,
  Logger& logger);

/**
 * Like loadEntityModelSync, but the model is loaded by the resource that createResource
 * makes, which may run on another thread. The loader keeps copies of the given material
 * config, model convention, path and functions.
 */
EntityModel loadEntityModelAsync(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  ModelConvention modelConvention,
  const std::filesystem::path& path,
  const LoadMaterialFunc& loadMaterial,
  const FindMaterialFunc& findMaterial,
  const CreateEntityModelDataResource& createResource,
  Logger& logger);

} // namespace mdl
} // namespace tb
