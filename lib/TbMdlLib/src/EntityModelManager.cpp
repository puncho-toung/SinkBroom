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

#include "mdl/EntityModelManager.h"

#include "base/Logger.h"
#include "gl/CreateResource.h"
#include "gl/MaterialIndexRangeRenderer.h"
#include "mdl/EntityModel.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/LoadEntityModel.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/MaterialUtils.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/result.h"

namespace tb::mdl
{
EntityModelManager::EntityModelManager(
  const GameInfo& gameInfo,
  const fs::FileSystem& gameFileSystem,
  CreateEntityModelDataResource createResource,
  Logger& logger)
  : m_gameInfo{gameInfo}
  , m_gameFileSystem{gameFileSystem}
  , m_createResource{std::move(createResource)}
  , m_logger{logger}
  , m_shaders{std::make_shared<const IndexedShaders>()}
  , m_materialCache{makeMaterialCache()}
{
}

EntityModelManager::~EntityModelManager()
{
  clear();
}

void EntityModelManager::clear()
{
  m_renderers.clear();
  m_models.clear();
  m_rendererMismatches.clear();

  m_unpreparedRenderers.clear();

  // the models loaded from now on must not share the textures of the dropped ones
  m_materialCache = makeMaterialCache();

  // Remove logging because it might fail when the document is already destroyed.
}

void EntityModelManager::setShaders(std::shared_ptr<const IndexedShaders> shaders)
{
  contract_pre(shaders != nullptr);

  m_shaders = std::move(shaders);
  m_materialCache = makeMaterialCache();
}

std::shared_ptr<const IndexedShaders> EntityModelManager::shaders() const
{
  return m_shaders;
}

gl::MaterialRenderer* EntityModelManager::renderer(const ModelSpecification& spec) const
{
  if (auto* entityModel = model(spec.path))
  {
    auto it = m_renderers.find(spec);
    if (it != std::end(m_renderers))
    {
      return it->second.get();
    }

    if (!m_rendererMismatches.contains(spec))
    {
      if (const auto* entityModelData = entityModel->data())
      {
        if (
          auto renderer = entityModelData->buildRenderer(spec.skinIndex, spec.frameIndex))
        {
          const auto [pos, success] = m_renderers.emplace(spec, std::move(renderer));
          contract_assert(success);

          auto* result = pos->second.get();
          m_unpreparedRenderers.push_back(result);
          m_logger.debug() << "Constructed entity model renderer for " << spec;
          return result;
        }

        m_rendererMismatches.insert(spec);
        m_logger.error() << "Failed to construct entity model renderer for " << spec
                         << ", check the skin and frame indices";
      }
    }
  }

  return nullptr;
}

const EntityModelFrame* EntityModelManager::frame(const ModelSpecification& spec) const
{
  if (auto* entityModel = model(spec.path))
  {
    if (const auto* entityModelData = entityModel->data())
    {
      return entityModelData->frame(spec.frameIndex);
    }
  }

  return nullptr;
}

const EntityModel* EntityModelManager::model(const std::filesystem::path& path) const
{
  if (!path.empty())
  {
    auto it = m_models.find(path);
    if (it != std::end(m_models))
    {
      return &it->second;
    }

    return loadModel(path) | kdl::transform([&](auto model) {
             const auto [pos, success] = m_models.emplace(path, std::move(model));
             contract_assert(success);

             m_logger.debug() << "Loading entity model " << path;
             return &(pos->second);
           })
           | kdl::transform_error([&](auto e) {
               m_logger.error() << e.msg;
               return nullptr;
             })
           | kdl::value();
  }

  return nullptr;
}

const std::vector<const EntityModel*> EntityModelManager::
  findEntityModelsByTextureResourceId(
    const std::vector<gl::ResourceId>& resourceIds) const
{
  using namespace std::ranges;

  const auto filterByResourceId =
    [resourceIdSet = std::unordered_set<gl::ResourceId>{
       resourceIds.begin(), resourceIds.end()}](const auto& model) {
      return resourceIdSet.contains(model.dataResource().id());
    };

  const auto toPointer = [](const auto& model) { return &model; };

  return m_models | views::values | views::filter(filterByResourceId)
         | views::transform(toPointer) | kdl::ranges::to<std::vector>();
}

std::shared_ptr<ModelMaterialCache> EntityModelManager::makeMaterialCache() const
{
  // The models load on worker threads, which decode the images of their materials at
  // once; the cache makes every image's texture resource once.
  return std::make_shared<ModelMaterialCache>(
    m_gameFileSystem,
    m_gameInfo.gameConfig.materialConfig,
    m_shaders,
    [](auto resourceLoader) {
      return gl::createResourceSync(std::move(resourceLoader));
    });
}

Result<EntityModel> EntityModelManager::loadModel(
  const std::filesystem::path& modelPath) const
{
  // The model may load on a worker thread after this function returns, so the material
  // functions own what they read. The file system and the logger outlive the loads.
  const auto& fs = m_gameFileSystem;
  auto& logger = m_logger;

  const auto loadMaterial =
    [&fs, &logger, materialCache = m_materialCache](const auto& materialPath) {
      return materialCache->loadMaterial(materialPath)
             | kdl::or_else(makeReadMaterialErrorHandler(fs, logger)) | kdl::value();
    };

  // Resolves the material decl names of id Tech 4 models; a name that resolves to
  // nothing is left to the loader, which keeps it on a default material
  const auto findMaterial = [&logger,
                             materialCache = m_materialCache](const auto& materialName) {
    return materialCache->findIdTech4Material(materialName)
           | kdl::transform_error([&](const auto& e) {
               logger.error() << "Could not load material '"
                              << materialName.generic_string() << "': " << e.msg;
               return std::optional<gl::Material>{};
             })
           | kdl::value();
  };

  // Makes the default skins of id Tech 4 OBJ models, which share one texture of the
  // default image instead of decoding it for every model
  const auto loadDefaultMaterial =
    [&logger, materialCache = m_materialCache](std::string materialName) {
      return materialCache->defaultMaterial(std::move(materialName), logger);
    };

  return mdl::loadEntityModelAsync(
    m_gameFileSystem,
    m_gameInfo.gameConfig.materialConfig,
    m_gameInfo.gameConfig.entityConfig.modelConvention,
    modelPath,
    loadMaterial,
    findMaterial,
    m_createResource,
    m_logger,
    loadDefaultMaterial);
}

void EntityModelManager::prepare(gl::Gl& gl, gl::VboManager& vboManager)
{
  prepareRenderers(gl, vboManager);
}

void EntityModelManager::prepareRenderers(gl::Gl& gl, gl::VboManager& vboManager)
{
  for (auto* renderer : m_unpreparedRenderers)
  {
    renderer->prepare(gl, vboManager);
  }
  m_unpreparedRenderers.clear();
}
} // namespace tb::mdl
