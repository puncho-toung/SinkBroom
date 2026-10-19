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
#include "gl/TextureFont.h"
#include "gl/TextureResource.h"
#include "gl/VertexArray.h"
#include "gl/VertexType.h"
#include "mdl/AssetUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityColorPropertyValue.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityPropertiesVariableStore.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadImageTexture.h"
#include "mdl/Map.h"
#include "mdl/ModelDefinition.h"
#include "mdl/ModelSpecification.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "render/Transformation.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/quat.h"
#include "vm/vec.h"

#include <algorithm>
#include <ranges>
#include <string>
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
  return std::filesystem::path{entry.relativePath.parent_path().generic_string()};
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

void addEdge(
  std::vector<EdgeVertex>& vertices,
  const vm::vec3d& start,
  const vm::vec3d& end,
  const vm::vec4f& color)
{
  if (vertices.size() / 2 < MaxEdgeSegments)
  {
    vertices.emplace_back(vm::vec3f{start}, color);
    vertices.emplace_back(vm::vec3f{end}, color);
  }
}

/**
 * The edges of the brushes and the grid lines of the patches under the given nodes, at
 * most MaxEdgeSegments of them.
 */
std::vector<EdgeVertex> collectEdges(
  const std::vector<const mdl::Node*>& nodes, const vm::vec4f& color)
{
  auto vertices = std::vector<EdgeVertex>{};
  const auto visitor = kdl::overload(
    [](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      groupNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const mdl::EntityNode& entityNode) {
      entityNode.visitChildren(thisLambda);
    },
    [&](const mdl::BrushNode& brushNode) {
      for (const auto* edge : brushNode.brush().edges())
      {
        const auto segment = edge->segment();
        addEdge(vertices, segment.start(), segment.end(), color);
      }
    },
    [&](const mdl::PatchNode& patchNode) {
      const auto& grid = patchNode.grid();
      for (size_t row = 0; row < grid.pointRowCount; ++row)
      {
        for (size_t col = 0; col < grid.pointColumnCount; ++col)
        {
          const auto& position = grid.point(row, col).position;
          if (col + 1 < grid.pointColumnCount)
          {
            addEdge(vertices, position, grid.point(row, col + 1).position, color);
          }
          if (row + 1 < grid.pointRowCount)
          {
            addEdge(vertices, position, grid.point(row + 1, col).position, color);
          }
        }
      }
    });

  for (const auto* node : nodes)
  {
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
 * A texture resource that reads and decodes the image on a worker; the resource manager
 * uploads it in its tick and drops it once the last cell lets go of it.
 */
std::shared_ptr<gl::TextureResource> createThumbnail(
  gl::ResourceManager& resourceManager, const std::filesystem::path& path)
{
  auto resource = std::make_shared<gl::TextureResource>([path]() -> Result<gl::Texture> {
    return fs::Disk::openFile(path) | kdl::and_then([](auto file) {
             auto reader = file->reader().buffer();
             const auto bytes = reader.stringView();
             return mdl::loadImageTextureFromMemory(
               reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
           });
  });
  resourceManager.addResource(resource);
  return resource;
}

} // namespace

std::vector<std::filesystem::path> prefabCategories(
  const std::vector<mdl::PrefabEntry>& entries)
{
  auto result = std::vector<std::filesystem::path>{};
  for (const auto& entry : entries)
  {
    if (auto category = categoryOf(entry);
        std::ranges::find(result, category) == result.end())
    {
      result.push_back(std::move(category));
    }
  }
  std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return result;
}

bool matchesPrefabFilter(const mdl::PrefabEntry& entry, const PrefabFilter& filter)
{
  return matchesKind(entry, filter.kind)
         && (!filter.category || categoryOf(entry) == *filter.category)
         && std::ranges::all_of(kdl::str_split(filter.text, " "), [&](const auto& word) {
              return matchesWord(entry, word);
            });
}

struct PrefabBrowserView::EntryPreview
{
  std::shared_ptr<gl::TextureResource> thumbnail;
  // the model of an entity snapshot; empty if it has none
  mdl::ModelSpecification modelSpecification;
  vm::vec3f modelScale = vm::vec3f::fill(1.0f);
  std::shared_ptr<gl::VertexArray> edges;
  vm::bbox3f bounds;
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
  // the previews and the cells point into the entries that were just replaced
  m_previews.clear();
  invalidate();
  update();
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
    if (matchesPrefabFilter(entry, m_filter))
    {
      addEntryToLayout(layout, entry, font);
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

const PrefabBrowserView::EntryPreview& PrefabBrowserView::preview(
  const mdl::PrefabEntry& entry)
{
  if (const auto it = m_previews.find(&entry); it != m_previews.end())
  {
    return *it->second;
  }

  auto& map = m_document.map();
  auto preview = std::make_unique<EntryPreview>();

  if (entry.thumbnailPath)
  {
    preview->thumbnail = createThumbnail(map.resourceManager(), *entry.thumbnailPath);
  }

  auto bounds = DefaultBounds;
  auto vertices = std::vector<EdgeVertex>{};
  if (entry.world)
  {
    const auto nodes = bodyNodes(*entry.world);
    const auto* entityNode = firstEntityNode(nodes);
    const auto color = previewColor(entityNode).toVec();

    if (!nodes.empty())
    {
      bounds = nodes.front()->logicalBounds();
      for (const auto* node : nodes)
      {
        bounds = vm::merge(bounds, node->logicalBounds());
      }
    }

    if (entry.metadata.kind == mdl::PrefabKind::Entity && entityNode)
    {
      const auto& entity = entityNode->entity();
      if (
        const auto* pointEntityDefinition =
          mdl::getPointEntityDefinition(entity.definition()))
      {
        preview->modelSpecification =
          mdl::safeGetModelSpecification(map.logger(), entity.classname(), [&]() {
            return entity.modelSpecification();
          });
        preview->modelScale = vm::vec3f{mdl::safeGetModelScale(
          pointEntityDefinition->modelDefinition,
          mdl::EntityPropertiesVariableStore{entity},
          map.worldNode().entityPropertyConfig().defaultModelScaleExpression)};
      }
    }

    vertices = collectEdges(nodes, color);
    if (vertices.empty())
    {
      vertices = boundsEdges(vm::bbox3f{bounds}, color);
    }
  }
  else
  {
    vertices = boundsEdges(vm::bbox3f{bounds}, ErrorColor.toVec());
  }

  preview->bounds = vm::bbox3f{bounds};
  preview->edges =
    std::make_shared<gl::VertexArray>(gl::VertexArray::move(std::move(vertices)));

  return *m_previews.emplace(&entry, std::move(preview)).first->second;
}

void PrefabBrowserView::addEntryToLayout(
  Layout& layout, const mdl::PrefabEntry& entry, const gl::FontDescriptor& font)
{
  const auto& preview = this->preview(entry);
  const auto& name = entry.metadata.name;

  const auto maxCellWidth = layout.maxCellWidth();
  const auto actualFont = fontManager().selectFontSize(font, name, maxCellWidth, 5);
  const auto actualSize = fontManager().font(actualFont).measure(name);

  // an image that cannot be read gives way to the next preview
  const auto hasThumbnail =
    preview.thumbnail
    && !std::holds_alternative<gl::ResourceFailed>(preview.thumbnail->state());

  auto cellData = PrefabCellData{
    .entry = &entry,
    .fontDescriptor = actualFont,
    .thumbnail = hasThumbnail ? preview.thumbnail : nullptr,
    .modelRenderer = nullptr,
    .modelOrientation = mdl::Orientation::Oriented,
    .edges = preview.edges,
    .bounds = preview.bounds,
    .transform = vm::mat4x4f::identity(),
  };

  auto& entityModelManager = m_document.map().entityModelManager();
  const auto& spec = preview.modelSpecification;
  const auto* model = hasThumbnail ? nullptr : entityModelManager.model(spec.path);
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
  const auto itemWidth = cellData.thumbnail ? 64.0f : rotatedSize.y();
  const auto itemHeight = cellData.thumbnail ? 64.0f : rotatedSize.z();

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
    if (!cellData.thumbnail && !cellData.modelRenderer && cellData.edges)
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

  forEachVisibleCell(layout, y, height, [&](const auto& cell) {
    const auto& cellData = this->cellData(cell);
    const auto* texture = cellData.thumbnail ? cellData.thumbnail->get() : nullptr;
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

      auto vertexArray = gl::VertexArray::move(std::vector<Vertex>{
        Vertex{{left, height - (top - y)}, {0, 0}},
        Vertex{{left, height - (bottom - y)}, {0, 1}},
        Vertex{{right, height - (bottom - y)}, {1, 1}},
        Vertex{{right, height - (top - y)}, {1, 0}},
      });

      if (texture->activate(
            gl, pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter)))
      {
        vertexArray.prepare(gl, vboManager());
        if (vertexArray.setup(gl, shader.program()))
        {
          vertexArray.render(gl, gl::PrimType::Quads);
          vertexArray.cleanup(gl, shader.program());
        }
        texture->deactivate(gl);
      }
    }
  });
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
  ss << QString::fromStdString(metadata.name);
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

void PrefabBrowserView::doContextMenu(
  Layout& layout, const float x, const float y, QContextMenuEvent* event)
{
  if (const auto* cell = layout.cellAt(x, y))
  {
    // copied, because the action may replace the document and with it the library
    const auto path = pathAsQString(cellData(*cell).entry->absolutePath);

    auto menu = QMenu{this};
    menu.addAction(
      tr("Edit Prefab..."), this, [this, path]() { emit editPrefabRequested(path); });
    menu.exec(event->globalPos());
  }
}

const PrefabCellData& PrefabBrowserView::cellData(const Cell& cell) const
{
  return cell.itemAs<PrefabCellData>();
}

} // namespace tb::ui
