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
#include "gl/ResourceId.h"
#include "mdl/EntityModel.h"
#include "mdl/ModelSpecification.h"

#include "kd/path_hash.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb
{
class Logger;

namespace fs
{
class FileSystem;
}

namespace gl
{
class Gl;
class MaterialRenderer;
class VboManager;
} // namespace gl

namespace mdl
{
class EntityModelFrame;
class EntityNode;
class IndexedShaders;
class ModelMaterialCache;

enum class Orientation;

struct GameInfo;

class EntityModelManager
{
private:
  const GameInfo& m_gameInfo;
  const fs::FileSystem& m_gameFileSystem;

  CreateEntityModelDataResource m_createResource;
  Logger& m_logger;

  // The shaders that models are shaded with, which the map parses once per load of the
  // material scripts and shares with its material collections. Models load on worker
  // threads, which keep the shaders they started with when setShaders replaces them.
  std::shared_ptr<const IndexedShaders> m_shaders;

  // The materials of the models that load from now on, which share one texture resource
  // per image. A new cache replaces it in setShaders and in clear, so that the models
  // loaded after either get new textures; a load in flight keeps the cache it started
  // with.
  std::shared_ptr<ModelMaterialCache> m_materialCache;

  // Models share the textures of their materials through the cache, and a dropped
  // texture never uploads again (EntityModelSurface::drop drops them). So the models are
  // only ever dropped all together, in clear, which also replaces the cache; evicting or
  // reloading one model would leave every model that shares its images untextured.
  mutable std::unordered_map<std::filesystem::path, EntityModel, kdl::path_hash> m_models;
  mutable std::unordered_map<ModelSpecification, std::unique_ptr<gl::MaterialRenderer>>
    m_renderers;
  mutable std::unordered_set<ModelSpecification> m_rendererMismatches;

  mutable std::vector<gl::MaterialRenderer*> m_unpreparedRenderers;

public:
  EntityModelManager(
    const GameInfo& gameInfo,
    const fs::FileSystem& gameFilesystem,
    CreateEntityModelDataResource createResource,
    Logger& logger);
  ~EntityModelManager();

  void clear();

  /**
   * Replaces the shaders that the models loaded from now on are shaded with. The given
   * pointer must not be null. The models already loaded keep their materials until
   * clear is called.
   */
  void setShaders(std::shared_ptr<const IndexedShaders> shaders);

  /**
   * The shaders that the last setShaders set, empty before the first. Every setShaders
   * replaces the pointer, so a caller copies what it needs at once and keeps neither the
   * pointer nor a reference into the shaders.
   */
  std::shared_ptr<const IndexedShaders> shaders() const;

  gl::MaterialRenderer* renderer(const ModelSpecification& spec) const;

  const EntityModelFrame* frame(const ModelSpecification& spec) const;
  const EntityModel* model(const std::filesystem::path& path) const;

  const std::vector<const EntityModel*> findEntityModelsByTextureResourceId(
    const std::vector<gl::ResourceId>& resourceIds) const;

private:
  std::shared_ptr<ModelMaterialCache> makeMaterialCache() const;
  Result<EntityModel> loadModel(const std::filesystem::path& path) const;

public:
  void prepare(gl::Gl& gl, gl::VboManager& vboManager);

private:
  void prepareRenderers(gl::Gl& gl, gl::VboManager& vboManager);
};

} // namespace mdl
} // namespace tb
