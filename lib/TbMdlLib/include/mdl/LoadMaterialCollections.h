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
#include "gl/TextureResource.h"
#include "mdl/Palette.h"
#include "mdl/Quake3Shader.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
class Logger;

namespace gl
{
class Material;
class MaterialCollection;
} // namespace gl

namespace fs
{
class FileSystem;
} // namespace fs

namespace mdl
{
struct MaterialConfig;

/**
 * Loads the material at the given path. If one of the given shaders has the path without
 * its extension as its shader path, the material is made from that shader, whatever its
 * root; a Doom 3 material is found by its full canonical decl name, such as
 * "models/mapobjects/crate". An exact match is looked for first, then one that ignores
 * case. Otherwise, the material is made from the image file at the given path.
 */
Result<gl::Material> loadMaterial(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath,
  const gl::CreateTextureResource& createResource,
  const std::vector<Quake3Shader>& shaders,
  const std::optional<Palette>& palette);

/**
 * Loads the materials under materialConfig.root, grouped into collections by directory.
 *
 * The material scripts are read with loadShaderFiles. Every shader under the root becomes
 * a material, and so does every image file under the root that no shader of the same
 * name (ignoring case) replaces and that materialConfig.excludes doesn't hide. Shader
 * previews are resolved here, on the calling thread, so that previewImageMissing is
 * known at once; the images are decoded by the texture resources that createResource
 * makes, and materials that show the same image share one resource. One summary line is
 * logged per load.
 */
Result<std::vector<gl::MaterialCollection>> loadMaterialCollections(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const gl::CreateTextureResource& createResource,
  kdl::task_manager& taskManager,
  Logger& logger);

} // namespace mdl
} // namespace tb
