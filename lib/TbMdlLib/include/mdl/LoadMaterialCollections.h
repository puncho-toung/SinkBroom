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

#include "kd/path_hash.h"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
 * Shaders with what finding materials among them needs from all of them: an index of
 * their paths that ignores case, and the engine names of the images that they use in
 * diffuse stages, which decide whether an editor image found only under a search path
 * previews a Doom 3 material. Both take a pass over every shader, so they are built once
 * per load of the shaders rather than once per material. Nothing changes after
 * construction, so the model loaders on several threads can share one.
 */
class IndexedShaders
{
private:
  std::vector<Quake3Shader> m_shaders;
  // lowercase shader path -> its shader in m_shaders
  std::unordered_map<std::filesystem::path, const Quake3Shader*, kdl::path_hash> m_index;
  // lowercase image paths without extension, as the engine's image manager names them
  std::unordered_set<std::filesystem::path, kdl::path_hash> m_diffuseImages;

public:
  IndexedShaders();
  explicit IndexedShaders(std::vector<Quake3Shader> shaders);

  // the index points into the shaders, which a copy would not own
  IndexedShaders(const IndexedShaders&) = delete;
  IndexedShaders& operator=(const IndexedShaders&) = delete;
  IndexedShaders(IndexedShaders&&) = default;
  IndexedShaders& operator=(IndexedShaders&&) = default;

  const std::vector<Quake3Shader>& shaders() const;

  /**
   * Returns the shader with the given path, ignoring case. Of shaders whose paths differ
   * only in case, the first one is found.
   */
  const Quake3Shader* findShader(const std::filesystem::path& shaderPath) const;

  /**
   * Returns the engine names of the images that the shaders use in diffuse stages:
   * lowercase and without extension.
   */
  const std::unordered_set<std::filesystem::path, kdl::path_hash>& diffuseImages() const;
};

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
 * Finds the material that an id Tech 4 model names by its canonical decl name, such as
 * the name after usemtl in an OBJ file. If one of the given shaders has that name,
 * ignoring case, the material is made from it, whatever its root. Otherwise, the engine
 * would make an implicit material that shows the image of that name, blended by its
 * alpha; if that image is found, as a loose file or under one of
 * materialConfig.shaderImageSearchPaths, the material is made like that. Otherwise,
 * returns nothing, where loadMaterial would return a material whose texture fails to
 * load.
 *
 * The shaders are passed indexed, because a model with many material names calls this
 * once per name, and the model loaders call it on several threads.
 */
Result<std::optional<gl::Material>> findIdTech4Material(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialName,
  const gl::CreateTextureResource& createResource,
  const IndexedShaders& shaders);

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
