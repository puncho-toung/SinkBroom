/*
 Copyright (C) 2026 TrenchBroom contributors

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

#include "ui/PrefabBrowserView.h"

#include <QContextMenuEvent>
#include <QImage>
#include <QMenu>
#include <QTextStream>

#include "base/PreferenceManager.h"
#include "el/VariableStore.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "gl/ActiveShader.h"
#include "gl/FontDescriptor.h"
#include "gl/FontManager.h"
#include "gl/GlInterface.h"
#include "gl/MaterialIndexRangeRenderer.h"
#include "gl/MaterialRenderFunc.h"
#include "gl/PrimType.h"
#include "gl/Resource.h"
#include "gl/ResourceId.h"
#include "gl/ResourceManager.h"
#include "gl/Shaders.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "gl/TextureFont.h"
#include "gl/TextureResource.h"
#include "gl/VertexArray.h"
#include "gl/VertexType.h"
#include "img/DecodeImage.h"
#include "img/Image.h"
#include "mdl/AssetUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityColorPropertyValue.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityPropertiesVariableStore.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadImageTexture.h"
#include "mdl/Map.h"
#include "mdl/ModelDefinition.h"
#include "mdl/ModelSpecification.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PrefabRefresh.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "render/Transformation.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/quat.h"
#include "vm/vec.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace tb::ui
{
namespace
{

using EdgeVertex = gl::VertexTypes::P3C4::Vertex;

// the UT fork's cap, which keeps a large assembly's cell cheap to draw
constexpr auto MaxEdgeSegments = size_t(1200);

// the bounds of an entry whose file failed or whose body is empty
const auto DefaultBounds = vm::bbox3d{16.0};

const auto DefaultColor = RgbaF{0.8f, 0.8f, 0.8f, 1.0f};
const auto ErrorColor = RgbaF{1.0f, 0.3f, 0.3f, 1.0f};

std::filesystem::path categoryOf(const mdl::PrefabEntry& entry)
{
  return entry.relativePath.parent_path();
}

/**
 * Whether the entry is in the given folder or in a folder below it; every entry is below
 * the top level.
 */
bool isInCategory(const mdl::PrefabEntry& entry, const std::filesystem::path& category)
{
  const auto entryCategory = categoryOf(entry);
  return std::ranges::mismatch(category, entryCategory).in1 == category.end();
}

bool matchesTag(const mdl::PrefabEntry& entry, const std::string& tag)
{
  return std::ranges::any_of(entry.metadata.tags, [&](const auto& entryTag) {
    return kdl::ci::str_is_equal(entryTag, tag);
  });
}

/**
 * The cell title: the prefab's name, or the file name without its extension if the file
 * failed or names no prefab.
 */
std::string titleOf(const mdl::PrefabEntry& entry)
{
  return !entry.metadata.name.empty()
           ? entry.metadata.name
           : pathAsQString(entry.relativePath.stem()).toStdString();
}

bool matchesWord(const mdl::PrefabEntry& entry, const std::string& word)
{
  const auto& metadata = entry.metadata;
  return kdl::ci::str_contains(metadata.name, word)
         || kdl::ci::str_contains(metadata.className, word)
         || std::ranges::any_of(metadata.tags, [&](const auto& tag) {
              return kdl::ci::str_contains(tag, word);
            });
}

bool matchesKind(const mdl::PrefabEntry& entry, const PrefabKindFilter kind)
{
  switch (kind)
  {
  case PrefabKindFilter::All:
    return true;
  case PrefabKindFilter::Entities:
    return !entry.error && entry.metadata.kind == mdl::PrefabKind::Entity;
  case PrefabKindFilter::Assemblies:
    return !entry.error && entry.metadata.kind == mdl::PrefabKind::Assembly;
  }
  return false;
}

/**
 * The prefab's body: every node in the world's layers, which for a well-formed file is
 * the one entity or the one group.
 */
std::vector<const mdl::Node*> bodyNodes(const mdl::WorldNode& worldNode)
{
  auto result = std::vector<const mdl::Node*>{};
  for (const auto* layerNode : worldNode.allLayers())
  {
    for (const auto* node : layerNode->children())
    {
      result.push_back(node);
    }
  }
  return result;
}

const mdl::EntityNode* firstEntityNode(const std::vector<const mdl::Node*>& nodes)
{
  auto result = static_cast<const mdl::EntityNode*>(nullptr);
  const auto visitor = kdl::overload(
    [&](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      if (!result)
      {
        groupNode.visitChildren(thisLambda);
      }
    },
    [&](const mdl::EntityNode& entityNode) {
      if (!result)
      {
        result = &entityNode;
      }
    },
    [](const mdl::BrushNode&) {},
    [](const mdl::PatchNode&) {});

  for (const auto* node : nodes)
  {
    node->accept(visitor);
  }
  return result;
}

/**
 * The definition colour of the first entity, or its _color for a light.
 */
RgbaF previewColor(const mdl::EntityNode* entityNode)
{
  if (!entityNode)
  {
    return DefaultColor;
  }

  const auto& entity = entityNode->entity();
  const auto* definition = entity.definition();
  if (kdl::cs::str_is_prefix(entity.classname(), "light"))
  {
    if (const auto* value = entity.property("_color"))
    {
      if (
        const auto color =
          mdl::parseEntityColorPropertyValue(definition, "_color", *value))
      {
        return color.value().color.to<RgbaF>();
      }
    }
  }
  return definition ? definition->color.to<RgbaF>() : DefaultColor;
}

/**
 * The edges of the brushes and the grid lines of the patches under the given nodes, at
 * most MaxEdgeSegments of them; the walk stops once that many are collected.
 */
std::vector<EdgeVertex> collectEdges(
  const std::vector<const mdl::Node*>& nodes, const vm::vec4f& color)
{
  auto vertices = std::vector<EdgeVertex>{};
  const auto full = [&]() { return vertices.size() / 2 >= MaxEdgeSegments; };
  const auto addEdge = [&](const vm::vec3d& start, const vm::vec3d& end) {
    vertices.emplace_back(vm::vec3f{start}, color);
    vertices.emplace_back(vm::vec3f{end}, color);
  };

  const auto visitor = kdl::overload(
    [&](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      if (!full())
      {
        worldNode.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      if (!full())
      {
        layerNode.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      if (!full())
      {
        groupNode.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::EntityNode& entityNode) {
      if (!full())
      {
        entityNode.visitChildren(thisLambda);
      }
    },
    [&](const mdl::BrushNode& brushNode) {
      for (const auto* edge : brushNode.brush().edges())
      {
        if (full())
        {
          return;
        }
        const auto segment = edge->segment();
        addEdge(segment.start(), segment.end());
      }
    },
    [&](const mdl::PatchNode& patchNode) {
      const auto& grid = patchNode.grid();
      for (size_t row = 0; row < grid.pointRowCount && !full(); ++row)
      {
        for (size_t col = 0; col < grid.pointColumnCount; ++col)
        {
          const auto& position = grid.point(row, col).position;
          if (col + 1 < grid.pointColumnCount && !full())
          {
            addEdge(position, grid.point(row, col + 1).position);
          }
          if (row + 1 < grid.pointRowCount && !full())
          {
            addEdge(position, grid.point(row + 1, col).position);
          }
        }
      }
    });

  for (const auto* node : nodes)
  {
    if (full())
    {
      break;
    }
    node->accept(visitor);
  }
  return vertices;
}

std::vector<EdgeVertex> boundsEdges(const vm::bbox3f& bounds, const vm::vec4f& color)
{
  auto vertices = std::vector<EdgeVertex>{};
  bounds.for_each_edge([&](const vm::vec3f& v1, const vm::vec3f& v2) {
    vertices.emplace_back(v1, color);
    vertices.emplace_back(v2, color);
  });
  return vertices;
}

/**
 * The given image, scaled down, keeping its aspect, so that its longest side is at most
 * PrefabThumbnailMaxSize; a smaller image is returned as it is.
 */
img::Image scaleThumbnail(img::Image image)
{
  const auto longestSide = std::max(image.width, image.height);
  if (longestSide <= PrefabThumbnailMaxSize)
  {
    return image;
  }

  const auto width =
    std::max(image.width * PrefabThumbnailMaxSize / longestSide, size_t(1));
  const auto height =
    std::max(image.height * PrefabThumbnailMaxSize / longestSide, size_t(1));

  // wraps the pixels without copying them
  const auto source = QImage{
    image.pixels.data(),
    int(image.width),
    int(image.height),
    qsizetype(image.width * 4),
    QImage::Format_RGBA8888};
  const auto scaled =
    source
      .scaled(int(width), int(height), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
      .convertToFormat(QImage::Format_RGBA8888);

  auto pixels = std::vector<unsigned char>(width * height * 4);
  for (size_t row = 0; row < height; ++row)
  {
    std::memcpy(
      pixels.data() + row * width * 4, scaled.constScanLine(int(row)), width * 4);
  }

  image.width = width;
  image.height = height;
  image.pixels = std::move(pixels);
  return image;
}

/**
 * A texture resource that reads, decodes and scales the image on a worker; the resource
 * manager uploads it in its tick and drops it once the last cell lets go of it.
 */
std::shared_ptr<gl::TextureResource> createThumbnail(
  gl::ResourceManager& resourceManager, const std::filesystem::path& path)
{
  auto resource = std::make_shared<gl::TextureResource>(
    [path]() -> Result<gl::Texture> { return loadPrefabThumbnail(path); });
  resourceManager.addResource(resource);
  return resource;
}

} // namespace

std::vector<std::filesystem::path> prefabCategories(
  const std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries)
{
  auto categories = std::set<std::filesystem::path>{};
  for (const auto& entry : entries)
  {
    categories.insert(categoryOf(*entry));
  }
  return categories | kdl::ranges::to<std::vector>();
}

std::vector<PrefabCategoryCount> prefabCategoryCounts(
  const std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries)
{
  // ordered by path components, so that a folder comes before the folders below it
  auto counts = std::map<std::filesystem::path, std::size_t>{};
  for (const auto& entry : entries)
  {
    for (auto folder = categoryOf(*entry); !folder.empty(); folder = folder.parent_path())
    {
      ++counts[folder];
    }
  }

  return counts | std::views::transform([](const auto& pathAndCount) {
           return PrefabCategoryCount{pathAndCount.first, pathAndCount.second};
         })
         | kdl::ranges::to<std::vector>();
}

PrefabFilter prefabFilterFromText(const std::string_view text)
{
  auto filter = PrefabFilter{};
  for (auto& word : kdl::str_split(text, " "))
  {
    if (kdl::ci::str_is_prefix(word, PrefabTagFilterPrefix))
    {
      // a bare tag: names no tag and filters nothing
      if (auto tag = word.substr(PrefabTagFilterPrefix.size()); !tag.empty())
      {
        filter.tags.push_back(std::move(tag));
      }
    }
    else
    {
      filter.words.push_back(std::move(word));
    }
  }
  return filter;
}

bool matchesPrefabFilter(const mdl::PrefabEntry& entry, const PrefabFilter& filter)
{
  return matchesKind(entry, filter.kind)
         && (!filter.category || isInCategory(entry, *filter.category))
         && std::ranges::all_of(
           filter.words, [&](const auto& word) { return matchesWord(entry, word); })
         && std::ranges::all_of(
           filter.tags, [&](const auto& tag) { return matchesTag(entry, tag); });
}

Result<gl::Texture> loadPrefabThumbnail(const std::filesystem::path& path)
{
  return fs::Disk::openFile(path) | kdl::and_then([](auto file) {
           auto reader = file->reader().buffer();
           const auto bytes = reader.stringView();
           return img::decodeImage(
             reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
         })
         | kdl::and_then([](img::Image image) -> Result<gl::Texture> {
             if (image.width == 0 || image.height == 0)
             {
               return Error{"The thumbnail image is empty"};
             }

             image = scaleThumbnail(std::move(image));

             constexpr auto format = GLenum(GL_RGBA);
             auto buffer = gl::TextureBuffer{image.pixels.size()};
             std::memcpy(buffer.data(), image.pixels.data(), image.pixels.size());
             const auto averageColor = mdl::getAverageColor(buffer, format);

             auto texture = gl::Texture{
               image.width,
               image.height,
               averageColor,
               format,
               gl::NoEmbeddedDefaults{},
               std::move(buffer)};
             texture.setAlphaDomain(image.alphaDomain);
             return texture;
           });
}

struct PrefabPreview
{
  // identifies this preview among those that the view built
  std::size_t id = 0;

  // what the preview was built from; another value means that the file changed
  std::string hash;
  bool failed = false;

  std::optional<std::filesystem::path> thumbnailPath;
  // created when a cell first draws the thumbnail
  std::shared_ptr<gl::TextureResource> thumbnail;

  vm::bbox3f bounds;

  // what the entity definitions decide, as of definitionGeneration
  std::size_t definitionGeneration = 0;
  vm::vec4f edgeColor;
  std::shared_ptr<gl::VertexArray> edges;
  // the model of an entity snapshot; empty if it has none
  mdl::ModelSpecification modelSpecification;
  vm::vec3f modelScale = vm::vec3f::fill(1.0f);
  // the entity model manager found no model; it is asked again once the models change
  bool modelLookupFailed = false;
};

PrefabBrowserView::PrefabBrowserView(
  AppController& appController, QScrollBar* scrollBar, MapDocument& document)
  : CellView{appController, scrollBar}
  , m_document{document}
{
  const auto hRotation = vm::quatf{vm::vec3f{0, 0, 1}, vm::to_radians(-30.0f)};
  const auto vRotation = vm::quatf{vm::vec3f{0, 1, 0}, vm::to_radians(20.0f)};
  m_rotation = vRotation * hRotation;

  m_notifierConnection += m_document.resourcesWereProcessedNotifier.connect(
    this, &PrefabBrowserView::resourcesWereProcessed);

  // the map clears its entity models when the mods or the definitions change, and
  // reloads them when the material collections change; the panel handles the game path
  m_notifierConnection += m_document.modsWillChangeNotifier.connect(
    this, &PrefabBrowserView::entityModelsWillChange);
  m_notifierConnection += m_document.entityDefinitionsWillChangeNotifier.connect(
    this, &PrefabBrowserView::entityModelsWillChange);
  m_notifierConnection += m_document.entityDefinitionsDidChangeNotifier.connect(
    this, &PrefabBrowserView::entityDefinitionsDidChange);
  m_notifierConnection += m_document.materialCollectionsDidChangeNotifier.connect(
    this, &PrefabBrowserView::entityModelsWillChange);

  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      this, &PrefabBrowserView::preferenceDidChange);
}

PrefabBrowserView::~PrefabBrowserView()
{
  clear();
}

void PrefabBrowserView::setFilter(PrefabFilter filter)
{
  m_filter = std::move(filter);
  invalidate();
  update();
}

void PrefabBrowserView::libraryDidChange()
{
  // the cells point into the entries, which may have been replaced; a preview is kept
  // while its file is in the library, and preview() notices a change of the file
  auto paths = std::unordered_set<std::filesystem::path::string_type>{};
  for (const auto& entry : m_document.map().prefabLibrary().entries())
  {
    paths.insert(entry->absolutePath.native());
  }
  std::erase_if(m_previews, [&](const auto& pathAndPreview) {
    return !paths.contains(pathAndPreview.first);
  });

  clear();
  invalidate();
  update();
}

void PrefabBrowserView::entityModelsWillChange()
{
  // drops the cells and their model renderers; the next paint lays them out again
  clear();
  invalidate();
  update();

  for (auto& [path, preview] : m_previews)
  {
    preview->modelLookupFailed = false;
  }
}

void PrefabBrowserView::reloadThumbnails()
{
  for (auto& [path, preview] : m_previews)
  {
    preview->thumbnail.reset();
  }

  // a cell whose image could not be read shows it again
  invalidate();
  update();
}

const mdl::PrefabEntry* PrefabBrowserView::selectedEntry() const
{
  if (m_selectedPath.empty())
  {
    return nullptr;
  }

  const auto& entries = m_document.map().prefabLibrary().entries();
  const auto it = std::ranges::find_if(
    entries, [&](const auto& entry) { return entry->absolutePath == m_selectedPath; });
  return it != entries.end() ? it->get() : nullptr;
}

void PrefabBrowserView::selectEntry(const std::filesystem::path& absolutePath)
{
  if (absolutePath != m_selectedPath)
  {
    m_selectedPath = absolutePath;
    emit selectedEntryChanged();
  }
}

std::vector<PrefabBrowserView::CellSummary> PrefabBrowserView::cellSummaries()
{
  auto result = std::vector<CellSummary>{};
  scrollToCell([&](const Cell& cell) {
    const auto& cellData = this->cellData(cell);
    result.push_back(
      CellSummary{cellData.entry, cell.title(), dndData(cell), cellData.preview->id});
    return false;
  });
  return result;
}

void PrefabBrowserView::doInitLayout(Layout& layout)
{
  layout.setOuterMargin(5.0f);
  layout.setGroupMargin(5.0f);
  layout.setRowMargin(5.0f);
  layout.setCellMargin(5.0f);
  layout.setCellWidth(93.0f, 93.0f);
  layout.setCellHeight(64.0f, 128.0f);
  layout.setMaxUpScale(1.5f);
}

void PrefabBrowserView::doReloadLayout(Layout& layout)
{
  const auto& fontPath = pref(Preferences::RendererFontPath);
  const auto fontSize = pref(Preferences::BrowserFontSize);
  contract_assert(fontSize > 0);

  const auto font = gl::FontDescriptor{fontPath, static_cast<size_t>(fontSize)};
  for (const auto& entry : m_document.map().prefabLibrary().entries())
  {
    if (matchesPrefabFilter(*entry, m_filter))
    {
      addEntryToLayout(layout, *entry, font);
    }
  }
}

bool PrefabBrowserView::dndEnabled()
{
  return true;
}

QString PrefabBrowserView::dndData(const Cell& cell)
{
  // C4: the payload names the file; the drop tool takes the body from the library
  const auto& library = m_document.map().prefabLibrary();
  return QString::fromStdString(library.payloadFor(*cellData(cell).entry));
}

void PrefabBrowserView::resourcesWereProcessed(const std::vector<gl::ResourceId>&)
{
  // a model or a thumbnail may have loaded
  invalidate();
}

void PrefabBrowserView::preferenceDidChange(const std::filesystem::path& path)
{
  if (path == m_document.map().gameInfo().gamePathPreference.path)
  {
    // the map has cleared its entity models
    entityModelsWillChange();
  }
  else
  {
    // the font, its size or the colours may have changed
    invalidate();
    update();
  }
}

void PrefabBrowserView::entityDefinitionsDidChange()
{
  // the library assigns the new definitions to its entries before the next layout
  ++m_definitionGeneration;
}

PrefabPreview& PrefabBrowserView::preview(const mdl::PrefabEntry& entry)
{
  auto& preview = m_previews[entry.absolutePath.native()];
  const auto failed = entry.error.has_value();
  if (
    !preview || preview->hash != entry.hash || preview->failed != failed
    || preview->thumbnailPath != entry.thumbnailPath)
  {
    // new, or the file has changed
    preview = std::make_unique<PrefabPreview>();
    preview->id = ++m_previewCount;
    preview->hash = entry.hash;
    preview->failed = failed;
    preview->thumbnailPath = entry.thumbnailPath;

    auto bounds = DefaultBounds;
    if (entry.world)
    {
      if (const auto nodes = bodyNodes(*entry.world); !nodes.empty())
      {
        bounds = nodes.front()->logicalBounds();
        for (const auto* node : nodes)
        {
          bounds = vm::merge(bounds, node->logicalBounds());
        }
      }
    }
    else if (entry.bodyPreview)
    {
      bounds = entry.bodyPreview->bounds;
    }
    preview->bounds = vm::bbox3f{bounds};

    updatePreviewAppearance(*preview, entry);
  }
  else if (preview->definitionGeneration != m_definitionGeneration)
  {
    updatePreviewAppearance(*preview, entry);
  }

  return *preview;
}

void PrefabBrowserView::updatePreviewAppearance(
  PrefabPreview& preview, const mdl::PrefabEntry& entry)
{
  auto& map = m_document.map();

  preview.definitionGeneration = m_definitionGeneration;
  preview.modelSpecification = mdl::ModelSpecification{};
  preview.modelScale = vm::vec3f::fill(1.0f);
  preview.modelLookupFailed = false;

  auto nodes = std::vector<const mdl::Node*>{};
  auto color = ErrorColor.toVec();
  if (entry.world)
  {
    nodes = bodyNodes(*entry.world);
    const auto* entityNode = firstEntityNode(nodes);
    color = previewColor(entityNode).toVec();

    if (entry.metadata.kind == mdl::PrefabKind::Entity && entityNode)
    {
      const auto& entity = entityNode->entity();
      if (
        const auto* pointEntityDefinition =
          mdl::getPointEntityDefinition(entity.definition()))
      {
        preview.modelSpecification =
          mdl::safeGetModelSpecification(map.logger(), entity.classname(), [&]() {
            return entity.modelSpecification();
          });
        preview.modelScale = vm::vec3f{mdl::safeGetModelScale(
          pointEntityDefinition->modelDefinition,
          mdl::EntityPropertiesVariableStore{entity},
          map.worldNode().entityPropertyConfig().defaultModelScaleExpression)};
      }
    }
  }
  else if (entry.bodyPreview)
  {
    // a large assembly whose world the library dropped (DD-19) keeps its colour (DD-21)
    color = entry.bodyPreview->color.value_or(DefaultColor).toVec();
  }

  if (preview.edges && preview.edgeColor == color)
  {
    // the edges are kept unless their colour changes
    return;
  }

  auto vertices = std::vector<EdgeVertex>{};
  if (entry.world)
  {
    vertices = collectEdges(nodes, color);
  }
  else if (entry.bodyPreview)
  {
    vertices = entry.bodyPreview->edgeVertices
               | std::views::transform(
                 [&](const auto& position) { return EdgeVertex{position, color}; })
               | kdl::ranges::to<std::vector>();
  }
  if (vertices.empty())
  {
    vertices = boundsEdges(preview.bounds, color);
  }

  preview.edgeColor = color;
  preview.edges =
    std::make_shared<gl::VertexArray>(gl::VertexArray::move(std::move(vertices)));
}

void PrefabBrowserView::addEntryToLayout(
  Layout& layout, const mdl::PrefabEntry& entry, const gl::FontDescriptor& font)
{
  auto& preview = this->preview(entry);
  const auto name = titleOf(entry);

  const auto maxCellWidth = layout.maxCellWidth();
  const auto actualFont = fontManager().selectFontSize(font, name, maxCellWidth, 5);
  const auto actualSize = fontManager().font(actualFont).measure(name);

  // an image that cannot be read gives way to the next preview
  const auto showsThumbnail =
    preview.thumbnailPath
    && !(
      preview.thumbnail
      && std::holds_alternative<gl::ResourceFailed>(preview.thumbnail->state()));

  auto cellData = PrefabCellData{
    .entry = &entry,
    .preview = &preview,
    .fontDescriptor = actualFont,
    .showsThumbnail = showsThumbnail,
    .modelRenderer = nullptr,
    .modelOrientation = mdl::Orientation::Oriented,
    .edges = preview.edges,
    .bounds = preview.bounds,
    .transform = vm::mat4x4f::identity(),
  };

  // a model that is not found is looked up again only once the models change
  auto& entityModelManager = m_document.map().entityModelManager();
  const auto& spec = preview.modelSpecification;
  const auto* model = static_cast<const mdl::EntityModel*>(nullptr);
  if (!showsThumbnail && !spec.path.empty() && !preview.modelLookupFailed)
  {
    model = entityModelManager.model(spec.path);
    preview.modelLookupFailed = model == nullptr;
  }
  const auto* modelData = model ? model->data() : nullptr;
  if (const auto* modelFrame = modelData ? modelData->frame(spec.frameIndex) : nullptr)
  {
    cellData.modelRenderer = entityModelManager.renderer(spec);
    cellData.modelOrientation = modelData->orientation();
    cellData.bounds = modelFrame->bounds();

    const auto scalingMatrix = vm::scaling_matrix(preview.modelScale);
    const auto center = cellData.bounds.center();
    const auto scaledCenter = scalingMatrix * center;
    cellData.transform = vm::translation_matrix(scaledCenter)
                         * vm::rotation_matrix(m_rotation) * scalingMatrix
                         * vm::translation_matrix(-center);
  }
  else
  {
    const auto center = cellData.bounds.center();
    cellData.transform = vm::translation_matrix(center) * vm::rotation_matrix(m_rotation)
                         * vm::translation_matrix(-center);
  }

  // a thumbnail fills the cell; its aspect is kept when it is drawn
  const auto rotatedSize = cellData.bounds.transform(cellData.transform).size();
  const auto itemWidth = cellData.showsThumbnail ? 64.0f : rotatedSize.y();
  const auto itemHeight = cellData.showsThumbnail ? 64.0f : rotatedSize.z();

  layout.addItem(
    std::move(cellData),
    name,
    std::max(itemWidth, 1.0f),
    std::max(itemHeight, 1.0f),
    actualSize.x(),
    static_cast<float>(font.size()) + 2.0f);
}

void PrefabBrowserView::doClear() {}

void PrefabBrowserView::doRender(
  gl::Gl& gl, Layout& layout, const float y, const float height)
{
  const auto viewLeft = static_cast<float>(0);
  const auto viewTop = static_cast<float>(size().height());
  const auto viewRight = static_cast<float>(size().width());
  const auto viewBottom = static_cast<float>(0);

  const auto projection =
    vm::ortho_matrix(-1024.0f, 1024.0f, viewLeft, viewTop, viewRight, viewBottom);
  const auto view =
    vm::view_matrix(CameraDirection, CameraUp) * vm::translation_matrix(CameraPosition);
  auto transformation = render::Transformation{gl, projection, view};

  renderEdges(gl, layout, y, height, transformation);
  renderModels(gl, layout, y, height, transformation);
  renderThumbnails(gl, layout, y, height, transformation);
}

bool PrefabBrowserView::shouldRenderFocusIndicator() const
{
  return false;
}

const Color& PrefabBrowserView::getBackgroundColor()
{
  return pref(Preferences::BrowserBackgroundColor);
}

namespace
{
template <typename F>
void forEachVisibleCell(CellLayout& layout, const float y, const float height, const F& f)
{
  for (const auto& group : layout.groups())
  {
    if (group.intersectsY(y, height))
    {
      for (const auto& row : group.rows())
      {
        if (row.intersectsY(y, height))
        {
          for (const auto& cell : row.cells())
          {
            f(cell);
          }
        }
      }
    }
  }
}
} // namespace

void PrefabBrowserView::renderEdges(
  gl::Gl& gl,
  Layout& layout,
  const float y,
  const float height,
  render::Transformation& transformation)
{
  auto shader = gl::ActiveShader{gl, shaderManager(), gl::Shaders::VaryingPCShader};

  forEachVisibleCell(layout, y, height, [&](const auto& cell) {
    const auto& cellData = this->cellData(cell);
    if (!cellData.showsThumbnail && !cellData.modelRenderer && cellData.edges)
    {
      const auto multMatrix =
        render::MultiplyModelMatrix{transformation, itemTransformation(cell, y, height)};

      auto& edges = *cellData.edges;
      edges.prepare(gl, vboManager());
      if (edges.setup(gl, shader.program()))
      {
        edges.render(gl, gl::PrimType::Lines);
        edges.cleanup(gl, shader.program());
      }
    }
  });
}

void PrefabBrowserView::renderModels(
  gl::Gl& gl,
  Layout& layout,
  const float y,
  const float height,
  render::Transformation& transformation)
{
  gl.frontFace(GL_CW);

  auto& entityModelManager = m_document.map().entityModelManager();
  entityModelManager.prepare(gl, vboManager());

  auto shader = gl::ActiveShader{gl, shaderManager(), gl::Shaders::EntityModelShader};
  shader.set("ApplyTinting", false);
  shader.set("Brightness", pref(Preferences::Brightness));
  shader.set("GrayScale", false);

  shader.set("CameraPosition", CameraPosition);
  shader.set("CameraDirection", CameraDirection);
  shader.set("CameraRight", vm::cross(CameraDirection, CameraUp));
  shader.set("CameraUp", CameraUp);
  shader.set("ViewMatrix", transformation.viewMatrix());

  forEachVisibleCell(layout, y, height, [&](const auto& cell) {
    const auto& cellData = this->cellData(cell);
    if (auto* modelRenderer = cellData.modelRenderer)
    {
      shader.set("Orientation", static_cast<int>(cellData.modelOrientation));

      const auto itemTrans = itemTransformation(cell, y, height);
      shader.set("ModelMatrix", itemTrans);

      const auto multMatrix = render::MultiplyModelMatrix{transformation, itemTrans};

      auto renderFunc = gl::AlphaTestedMaterialRenderFunc{
        shader, pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter)};
      modelRenderer->render(gl, shader.program(), renderFunc);
    }
  });
}

void PrefabBrowserView::renderThumbnails(
  gl::Gl& gl,
  Layout& layout,
  const float y,
  const float height,
  render::Transformation& transformation)
{
  using Vertex = gl::VertexTypes::P2Uv2::Vertex;

  // the models are drawn clockwise, the thumbnails counter-clockwise
  gl.frontFace(GL_CCW);

  const auto viewLeft = float(0);
  const auto viewTop = float(size().height());
  const auto viewRight = float(size().width());
  const auto viewBottom = float(0);

  // the material browser's flat projection
  const auto replaceTransformation = render::ReplaceTransformation{
    transformation,
    vm::ortho_matrix(-1.0f, 1.0f, viewLeft, viewTop, viewRight, viewBottom),
    vm::view_matrix(vm::vec3f{0, 0, -1}, vm::vec3f{0, 1, 0})
      * vm::translation_matrix(vm::vec3f{0.0f, 0.0f, 0.1f})};

  auto shader = gl::ActiveShader{gl, shaderManager(), gl::Shaders::MaterialBrowserShader};
  shader.set("ApplyTinting", false);
  shader.set("Material", 0);
  shader.set("Brightness", pref(Preferences::Brightness));
  gl::setAlphaFuncUniforms(shader, nullptr);

  // one quad per thumbnail in one vertex array, drawn per texture by offset
  auto vertices = std::vector<Vertex>{};
  auto textures = std::vector<const gl::Texture*>{};

  auto& resourceManager = m_document.map().resourceManager();
  forEachVisibleCell(layout, y, height, [&](const auto& cell) {
    const auto& cellData = this->cellData(cell);
    if (!cellData.showsThumbnail)
    {
      return;
    }

    // the image is read when a cell first shows it
    auto& preview = *cellData.preview;
    if (!preview.thumbnail)
    {
      preview.thumbnail = createThumbnail(resourceManager, *preview.thumbnailPath);
    }

    const auto* texture = preview.thumbnail->get();
    if (texture && texture->isReady())
    {
      // fit the image into the item bounds, keeping its aspect
      const auto& bounds = cell.itemBounds();
      const auto scale = std::min(
        bounds.width / float(std::max(texture->width(), size_t(1))),
        bounds.height / float(std::max(texture->height(), size_t(1))));
      const auto w = float(texture->width()) * scale;
      const auto h = float(texture->height()) * scale;
      const auto left = bounds.left() + (bounds.width - w) / 2.0f;
      const auto top = bounds.top() + (bounds.height - h) / 2.0f;
      const auto right = left + w;
      const auto bottom = top + h;

      vertices.push_back(Vertex{{left, height - (top - y)}, {0, 0}});
      vertices.push_back(Vertex{{left, height - (bottom - y)}, {0, 1}});
      vertices.push_back(Vertex{{right, height - (bottom - y)}, {1, 1}});
      vertices.push_back(Vertex{{right, height - (top - y)}, {1, 0}});
      textures.push_back(texture);
    }
  });

  if (textures.empty())
  {
    return;
  }

  auto vertexArray = gl::VertexArray::move(std::move(vertices));
  vertexArray.prepare(gl, vboManager());
  if (vertexArray.setup(gl, shader.program()))
  {
    for (size_t i = 0; i < textures.size(); ++i)
    {
      if (textures[i]->activate(
            gl, pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter)))
      {
        vertexArray.render(gl, gl::PrimType::Quads, GLint(i * 4), 4);
        textures[i]->deactivate(gl);
      }
    }
    vertexArray.cleanup(gl, shader.program());
  }
}

vm::mat4x4f PrefabBrowserView::itemTransformation(
  const Cell& cell, const float y, const float height) const
{
  const auto& cellData = this->cellData(cell);

  const auto offset =
    vm::vec3f{0.0f, cell.itemBounds().left(), height - (cell.itemBounds().bottom() - y)};
  const auto scaling = cell.scale();
  const auto& rotatedBounds = cellData.bounds.transform(cellData.transform);
  const auto rotationOffset =
    vm::vec3f{0.0f, -rotatedBounds.min.y(), -rotatedBounds.min.z()};

  return vm::translation_matrix(offset) * vm::scaling_matrix(vm::vec3f::fill(scaling))
         * vm::translation_matrix(rotationOffset) * cellData.transform;
}

QString PrefabBrowserView::tooltip(const Cell& cell)
{
  const auto& entry = *cellData(cell).entry;
  const auto& metadata = entry.metadata;

  auto tooltip = QString{};
  auto ss = QTextStream{&tooltip};
  ss << QString::fromStdString(titleOf(entry));
  if (!metadata.className.empty())
  {
    ss << "\n" << tr("Class: ") << QString::fromStdString(metadata.className);
  }
  if (!metadata.tags.empty())
  {
    ss << "\n"
       << tr("Tags: ") << QString::fromStdString(kdl::str_join(metadata.tags, "; "));
  }
  ss << "\n"
     << QString::fromStdString(m_document.map().prefabLibrary().payloadFor(entry))
          .mid(QString{"prefab:"}.size());
  if (entry.error)
  {
    ss << "\n" << tr("Error: ") << QString::fromStdString(*entry.error);
  }
  return tooltip;
}

void PrefabBrowserView::doLeftClick(Layout& layout, const float x, const float y)
{
  if (const auto* cell = layout.cellAt(x, y))
  {
    selectEntry(cellData(*cell).entry->absolutePath);
  }
}

void PrefabBrowserView::doContextMenu(
  Layout& layout, const float x, const float y, QContextMenuEvent* event)
{
  if (const auto* cell = layout.cellAt(x, y))
  {
    const auto& entry = *cellData(*cell).entry;
    selectEntry(entry.absolutePath);

    // copied, because the action may replace the document and with it the library
    const auto path = pathAsQString(entry.absolutePath);
    const auto& map = m_document.map();

    auto menu = QMenu{this};
    menu.addAction(
      tr("Edit Prefab..."), this, [this, path]() { emit editPrefabRequested(path); });
    menu.addSeparator();

    auto* applyAction = menu.addAction(tr("Apply to Selection"), this, [this, path]() {
      emit applyPrefabRequested(path);
    });
    applyAction->setEnabled(mdl::canApplyPrefabToSelection(map, entry));

    auto* updateAction =
      menu.addAction(tr("Update from Selection..."), this, [this, path]() {
        emit updatePrefabRequested(path);
      });
    updateAction->setEnabled(map.selection().hasNodes());

    auto* refreshAction =
      menu.addAction(tr("Refresh Placements..."), this, [this, path]() {
        emit refreshPrefabRequested(path);
      });
    refreshAction->setEnabled(std::ranges::any_of(
      map.prefabLibrary().changedPlacements(),
      [&](const auto& placement) { return placement.entry == &entry; }));

    menu.addSeparator();
    menu.addAction(
      tr("Reveal in Folder"), this, [this, path]() { emit revealPrefabRequested(path); });

    const auto hasThumbnail = entry.thumbnailPath.has_value();
    auto* openThumbnailAction = menu.addAction(
      tr("Open Thumbnail"), this, [this, path]() { emit openThumbnailRequested(path); });
    openThumbnailAction->setEnabled(hasThumbnail);
    auto* removeThumbnailAction =
      menu.addAction(tr("Remove Thumbnail..."), this, [this, path]() {
        emit removeThumbnailRequested(path);
      });
    removeThumbnailAction->setEnabled(hasThumbnail);

    menu.addSeparator();
    menu.addAction(
      tr("Delete File..."), this, [this, path]() { emit deletePrefabRequested(path); });

    menu.exec(event->globalPos());
  }
}

const PrefabCellData& PrefabBrowserView::cellData(const Cell& cell) const
{
  return cell.itemAs<PrefabCellData>();
}

} // namespace tb::ui
