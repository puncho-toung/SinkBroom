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
#include "mdl/LoadShaders.h"
#include "mdl/Palette.h"
#include "mdl/Quake3Shader.h"

#include "kd/path_hash.h"

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
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
  // what the summary of a load of the material collections reports about the files
  size_t m_fileCount = 0;
  size_t m_unterminatedCount = 0;

public:
  IndexedShaders();
  explicit IndexedShaders(std::vector<Quake3Shader> shaders);

  /**
   * Keeps the shaders that loadShaderFiles found, and what it counted in their files, so
   * that one parse serves the material collections and the entity models.
   */
  explicit IndexedShaders(LoadedShaders loadedShaders);

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

  /**
   * Returns the number of material script files that the shaders were read from, or 0
   * unless they were made from what loadShaderFiles returned.
   */
  size_t fileCount() const;

  /**
   * Returns the number of Doom 3 material decls that lack their closing brace, or 0
   * unless the shaders were made from what loadShaderFiles returned.
   */
  size_t unterminatedCount() const;
};

/**
 * Returns the paths of the given shaders that start with one of the given prefixes,
 * ignoring case, as copied strings, e.g. the materials under "lights/" and "fogs/" that
 * a Doom 3 light can project. The paths are sorted ignoring case; of paths that differ
 * only in case, the first one is kept, the one that findShader finds.
 */
std::vector<std::string> shaderNamesWithPrefix(
  const IndexedShaders& shaders, std::initializer_list<std::string_view> prefixes);

/**
 * Loads the material at the given path. If one of the given shaders has the path without
 * its extension as its shader path, the material is made from that shader, whatever its
 * root; a Doom 3 material is found by its full canonical decl name, such as
 * "models/mapobjects/crate". An exact match is looked for first, then one that ignores
 * case. Otherwise, the material is made from the image file at the given path.
 *
 * Every call scans the shaders, so a caller that loads many materials from the same
 * shaders passes them indexed to the overload below.
 */
Result<gl::Material> loadMaterial(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath,
  const gl::CreateTextureResource& createResource,
  const std::vector<Quake3Shader>& shaders,
  const std::optional<Palette>& palette);

/**
 * Loads the material at the given path like the overload above, but finds its shader in
 * the index and takes the diffuse images from it instead of a pass over every shader.
 * Of shaders whose paths differ only in case, the index finds the first one; the
 * shaders that loadShaderFiles returns have no such paths, so for them both overloads
 * make the same material.
 *
 * The model loaders call this once per material name, on several threads.
 */
Result<gl::Material> loadMaterial(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath,
  const gl::CreateTextureResource& createResource,
  const IndexedShaders& shaders,
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
 * Loads the materials under materialConfig.root from the given shaders, which are
 * usually what loadShaderFiles read from the material scripts, grouped into collections
 * by directory. The shaders are not parsed again, so the caller can share one parse with
 * the entity models.
 *
 * Every shader under the root becomes a material. If
 * materialConfig.implicitMaterialsFromImages is set, so does every image file under the
 * root that no shader of the same name (ignoring case) replaces and that
 * materialConfig.excludes doesn't hide; otherwise, image files are only used as the
 * shaders' previews and materialConfig.excludes has no effect. Shader previews are
 * resolved here, on the calling thread, so that previewImageMissing is known at once; the
 * images are decoded by the texture resources that createResource makes, and materials
 * that show the same image share one resource. One summary line is logged per load; it
 * counts the files that the shaders carry.
 */
Result<std::vector<gl::MaterialCollection>> loadMaterialCollections(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const IndexedShaders& shaders,
  const gl::CreateTextureResource& createResource,
  Logger& logger);

/**
 * Reads the material scripts with loadShaderFiles and loads the materials from them as
 * the overload above does. A caller that needs the shaders for anything else parses them
 * once and calls the overload above.
 */
Result<std::vector<gl::MaterialCollection>> loadMaterialCollections(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const gl::CreateTextureResource& createResource,
  kdl::task_manager& taskManager,
  Logger& logger);

} // namespace mdl
} // namespace tb
