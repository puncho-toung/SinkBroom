/*
 Copyright (C) 2023 Daniel Walder
 Copyright (C) 2022 Amara M. Kilic
 Copyright (C) 2022 Kristian Duske

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
#include "mdl/EntityModel.h"
#include "mdl/FindMaterialFunc.h"
#include "mdl/ModelConvention.h"

#include <filesystem>

namespace tb
{
class Logger;

namespace fs
{
class FileSystem;
}

namespace mdl
{

struct AssimpModelOptions
{
  /**
   * Under ModelConvention::IdTech4, a mesh whose material name resolves with findMaterial
   * as a canonical id Tech 4 decl name shows that material instead of the textures of its
   * assimp material, and a glTF model is turned from Y up to Z up the way RBDOOM-3-BFG
   * turns it, (x, y, z) to (x, -z, y).
   */
  ModelConvention convention = ModelConvention::Default;
  FindMaterialFunc findMaterial = {};
};

bool canLoadAssimpModel(const std::filesystem::path& path);

Result<EntityModelData> loadAssimpModel(
  const std::filesystem::path& path,
  const fs::FileSystem& fs,
  Logger& logger,
  const AssimpModelOptions& options = {});

} // namespace mdl
} // namespace tb
