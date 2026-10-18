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

#include "ui/MaterialBrowserView.h"

#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QTextStream>

#include "base/Macros.h"
#include "base/PreferenceManager.h"
#include "gl/ActiveShader.h"
#include "gl/FontManager.h"
#include "gl/Material.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"
#include "gl/MaterialRenderFunc.h"
#include "gl/PrimType.h"
#include "gl/ResourceId.h"
#include "gl/Shaders.h"
#include "gl/Texture.h"
#include "gl/TextureFont.h"
#include "gl/VertexArray.h"
#include "gl/VertexType.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Selection.h"
#include "mdl/UtilityMaterials.h"
#include "prefs/Preferences.h"
#include "render/Transformation.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/string_compare_natural.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/vec.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

namespace tb::ui
{

std::vector<const gl::Material*> filterMaterials(
  std::vector<const gl::Material*> materials,
  const bool hideUnused,
  const bool hideMissing,
  const std::string& filterText,
  const MaterialBrowserScope scope,
  const std::vector<mdl::UtilityMaterialConfig>& utilityMaterials)
{
  if (hideUnused)
  {
    std::erase_if(
      materials, [](const auto* material) { return material->usageCount() == 0; });
  }
  if (hideMissing)
  {
    std::erase_if(
      materials, [](const auto* material) { return material->previewImageMissing(); });
  }
  if (!filterText.empty())
  {
    std::erase_if(materials, [&](const auto* material) {
      return std::ranges::none_of(
        kdl::str_split(filterText, " "), [&](const auto& pattern) {
          return kdl::ci::str_contains(material->name(), pattern);
        });
    });
  }
  if (scope != MaterialBrowserScope::All)
  {
    // a regular browser hides the utility materials, which a utility browser lists alone
    const auto keepUtilityMaterials = scope == MaterialBrowserScope::Utility;
    std::erase_if(materials, [&](const auto* material) {
      return mdl::isUtilityMaterial(utilityMaterials, material->name())
             != keepUtilityMaterials;
    });
  }
  return materials;
}

std::string wrapWords(
  const std::string& text,
  const float maxWidth,
  const std::function<float(const std::string&)>& measureWidth)
{
  auto lines = std::vector<std::string>{};
  for (auto& word : kdl::str_split(text, " "))
  {
    if (!lines.empty())
    {
      if (auto line = lines.back() + " " + word; measureWidth(line) <= maxWidth)
      {
        lines.back() = std::move(line);
        continue;
      }
    }
    // the first word, or one that no longer fits, starts a line; a word wider than a
    // line stands alone on its own
    lines.push_back(std::move(word));
  }
  return kdl::str_join(lines, "\n");
}

MaterialBorder materialBorder(const gl::Material& material, const bool selected)
{
  if (selected)
  {
    return MaterialBorder::Selected;
  }
  // the coverage that the material script implies, not the preview's alpha or blending
  if (
    material.surfaceParms().contains("coverage_translucent")
    || material.surfaceParms().contains("coverage_perforated"))
  {
    return MaterialBorder::NonSealing;
  }
  if (material.usageCount() > 0)
  {
    return MaterialBorder::Used;
  }
  return MaterialBorder::Default;
}

MaterialBrowserView::MaterialBrowserView(
  AppController& appController,
  QScrollBar* scrollBar,
  MapDocument& document,
  const MaterialBrowserScope scope)
  : CellView{appController, scrollBar}
  , m_document{document}
  , m_scope{scope}
{
  m_notifierConnection += m_document.materialUsageCountsDidChangeNotifier.connect(
    this, &MaterialBrowserView::reloadMaterials);
  m_notifierConnection += m_document.resourcesWereProcessedNotifier.connect(
    this, &MaterialBrowserView::resourcesWereProcessed);
}

MaterialBrowserView::~MaterialBrowserView()
{
  clear();
}

void MaterialBrowserView::setSortOrder(const MaterialSortOrder sortOrder)
{
  if (sortOrder != m_sortOrder)
  {
    m_sortOrder = sortOrder;
    reloadMaterials();
  }
}

void MaterialBrowserView::setGroup(const bool group)
{
  if (group != m_group)
  {
    m_group = group;
    reloadMaterials();
  }
}

void MaterialBrowserView::setHideUnused(const bool hideUnused)
{
  if (hideUnused != m_hideUnused)
  {
    m_hideUnused = hideUnused;
    reloadMaterials();
  }
}

void MaterialBrowserView::setHideMissing(const bool hideMissing)
{
  if (hideMissing != m_hideMissing)
  {
    m_hideMissing = hideMissing;
    reloadMaterials();
  }
}

void MaterialBrowserView::setFilterText(const std::string& filterText)
{
  if (filterText != m_filterText)
  {
    m_filterText = filterText;
    reloadMaterials();
  }
}

const gl::Material* MaterialBrowserView::selectedMaterial() const
{
  return m_selectedMaterial;
}

void MaterialBrowserView::setSelectedMaterial(const gl::Material* selectedMaterial)
{
  if (m_selectedMaterial != selectedMaterial)
  {
    m_selectedMaterial = selectedMaterial;
    update();
  }
}

void MaterialBrowserView::revealMaterial(const gl::Material* material)
{
  scrollToCell([&](const Cell& cell) {
    const auto& cellMaterial = cellData(cell);
    return &cellMaterial == material;
  });
}

void MaterialBrowserView::resourcesWereProcessed(const std::vector<gl::ResourceId>&)
{
  reloadMaterials();
}

void MaterialBrowserView::reloadMaterials()
{
  invalidate();
  update();
}

void MaterialBrowserView::doInitLayout(Layout& layout)
{
  const auto scaleFactor = pref(Preferences::MaterialBrowserIconSize);

  layout.setOuterMargin(5.0f);
  layout.setGroupMargin(5.0f);
  layout.setRowMargin(15.0f);
  layout.setCellMargin(10.0f);
  layout.setTitleMargin(2.0f);
  layout.setCellWidth(scaleFactor * 64.0f, scaleFactor * 64.0f);
  layout.setCellHeight(scaleFactor * 64.0f, scaleFactor * 128.0f);
  if (m_scope == MaterialBrowserScope::Utility)
  {
    // a card per material, as wide as four thumbnails at 100 % so that the description
    // under the name wraps to a few lines; the browser font, and so the wrapped text,
    // does not scale with the icon size, so neither does the card, which grows only when
    // the thumbnail itself is wider: a scaled card would be cut off by a narrow dock or
    // break the text into many short lines. A thumbnail high, so that a large preview is
    // drawn at the size it has in a regular browser; the title lines still grow the card
    const auto cardWidth = std::max(256.0f, scaleFactor * 64.0f);
    layout.setCellWidth(cardWidth, cardWidth);
    layout.setCellHeight(scaleFactor * 64.0f, scaleFactor * 64.0f);
  }
}

void MaterialBrowserView::doReloadLayout(Layout& layout)
{
  const auto& fontPath = pref(Preferences::RendererFontPath);
  const auto fontSize = pref(Preferences::BrowserFontSize);
  contract_assert(fontSize > 0);

  const auto font = gl::FontDescriptor{fontPath, size_t(fontSize)};

  if (m_group)
  {
    for (const auto* collection : getCollections())
    {
      auto materials = getMaterials(*collection);
      if (m_scope != MaterialBrowserScope::All && materials.empty())
      {
        // a scoped browser leaves out the collections that hold none of its materials,
        // or the other tab's folders would show here as empty headings
        continue;
      }
      layout.addGroup(collection->path().string(), float(fontSize) + 2.0f);
      addMaterialsToLayout(layout, materials, font);
    }
  }
  else
  {
    addMaterialsToLayout(layout, getMaterials(), font);
  }
}

void MaterialBrowserView::addMaterialsToLayout(
  Layout& layout,
  const std::vector<const gl::Material*>& materials,
  const gl::FontDescriptor& font)
{
  for (const auto* material : materials)
  {
    addMaterialToLayout(layout, *material, font);
  }
}

void MaterialBrowserView::addMaterialToLayout(
  Layout& layout, const gl::Material& material, const gl::FontDescriptor& font)
{
  const auto maxCellWidth = layout.maxCellWidth();

  const auto title = cellTitle(material, maxCellWidth - 4.0f, font);
  const auto titleHeight = fontManager().font(font).measure(title).y();

  const auto scaleFactor = pref(Preferences::MaterialBrowserIconSize);
  const auto* texture = material.texture();
  const auto textureSize = texture ? texture->sizef() : vm::vec2f{64, 64};
  const auto scaledTextureSize = vm::round(scaleFactor * textureSize);

  layout.addItem(
    &material,
    title,
    scaledTextureSize.x(),
    scaledTextureSize.y(),
    maxCellWidth,
    titleHeight + 4.0f);
}

std::string MaterialBrowserView::cellTitle(
  const gl::Material& material, const float maxWidth, const gl::FontDescriptor& font)
{
  const auto materialName = std::filesystem::path{material.name()}.filename().string();
  if (m_scope == MaterialBrowserScope::Utility)
  {
    // a utility material's card says what the material does, under its name
    if (const auto* utilityMaterial =
          mdl::findUtilityMaterial(utilityMaterials(), material.name());
        utilityMaterial && !utilityMaterial->description.empty())
    {
      const auto& textureFont = fontManager().font(font);
      return materialName + "\n"
             + wrapWords(utilityMaterial->description, maxWidth, [&](const auto& line) {
                 return textureFont.measure(line).x();
               });
    }
  }
  return materialName;
}

const std::vector<mdl::UtilityMaterialConfig>& MaterialBrowserView::utilityMaterials()
  const
{
  // read when it is needed: a document of another game loaded into the same window
  // brings its own list
  return m_document.map().gameInfo().gameConfig.materialConfig.utilityMaterials;
}

std::vector<const gl::MaterialCollection*> MaterialBrowserView::getCollections() const
{
  const auto& map = m_document.map();
  const auto enabledMaterialCollections = mdl::enabledMaterialCollections(map);

  auto result = std::vector<const gl::MaterialCollection*>{};
  for (const auto& collection : map.materialManager().collections())
  {
    if (kdl::ranges::contains(enabledMaterialCollections, collection.path()))
    {
      result.push_back(&collection);
    }
  }
  return result;
}

std::vector<const gl::Material*> MaterialBrowserView::getMaterials(
  const gl::MaterialCollection& collection) const
{
  return sortMaterials(filterMaterials(
    collection.materials() | std::views::transform([](const auto& t) { return &t; })
    | kdl::ranges::to<std::vector>()));
}

std::vector<const gl::Material*> MaterialBrowserView::getMaterials() const
{
  auto materials = std::vector<const gl::Material*>{};
  for (const auto& collection : getCollections())
  {
    for (const auto& material : collection->materials())
    {
      materials.push_back(&material);
    }
  }
  return sortMaterials(filterMaterials(materials));
}

std::vector<const gl::Material*> MaterialBrowserView::filterMaterials(
  std::vector<const gl::Material*> materials) const
{
  return ui::filterMaterials(
    std::move(materials),
    m_hideUnused,
    m_hideMissing,
    m_filterText,
    m_scope,
    utilityMaterials());
}

std::vector<const gl::Material*> MaterialBrowserView::sortMaterials(
  std::vector<const gl::Material*> materials) const
{
  const auto compareNames = [](const auto& lhs, const auto& rhs) {
    return kdl::ci::string_less_natural{}(lhs->name(), rhs->name());
  };

  switch (m_sortOrder)
  {
  case MaterialSortOrder::Name:
    return kdl::vec_sort(std::move(materials), compareNames);
  case MaterialSortOrder::Usage:
    return kdl::vec_sort(std::move(materials), [&](const auto* lhs, const auto* rhs) {
      return lhs->usageCount() < rhs->usageCount()   ? false
             : lhs->usageCount() > rhs->usageCount() ? true
                                                     : compareNames(lhs, rhs);
    });
    switchDefault();
  }
}

void MaterialBrowserView::doClear() {}

void MaterialBrowserView::doRender(
  gl::Gl& gl, Layout& layout, const float y, const float height)
{
  const auto viewLeft = float(0);
  const auto viewTop = float(size().height());
  const auto viewRight = float(size().width());
  const auto viewBottom = float(0);

  const auto transformation = render::Transformation{
    gl,
    vm::ortho_matrix(-1.0f, 1.0f, viewLeft, viewTop, viewRight, viewBottom),
    vm::view_matrix(vm::vec3f{0, 0, -1}, vm::vec3f{0, 1, 0})
      * vm::translation_matrix(vm::vec3f{0.0f, 0.0f, 0.1f})};

  renderBounds(gl, layout, y, height);
  renderMaterials(gl, layout, y, height);
}

bool MaterialBrowserView::shouldRenderFocusIndicator() const
{
  return false;
}

const Color& MaterialBrowserView::getBackgroundColor()
{
  return pref(Preferences::BrowserBackgroundColor);
}

void MaterialBrowserView::renderBounds(
  gl::Gl& gl, Layout& layout, const float y, const float height)
{
  using BoundsVertex = gl::VertexTypes::P2C4::Vertex;
  auto vertices = std::vector<BoundsVertex>{};

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
            const auto& bounds = cell.itemBounds();
            const auto& material = cellData(cell);
            const auto& color = materialColor(material);
            vertices.emplace_back(
              vm::vec2f{bounds.left() - 2.0f, height - (bounds.top() - 2.0f - y)},
              color.to<RgbaF>().toVec());
            vertices.emplace_back(
              vm::vec2f{bounds.left() - 2.0f, height - (bounds.bottom() + 2.0f - y)},
              color.to<RgbaF>().toVec());
            vertices.emplace_back(
              vm::vec2f{bounds.right() + 2.0f, height - (bounds.bottom() + 2.0f - y)},
              color.to<RgbaF>().toVec());
            vertices.emplace_back(
              vm::vec2f{bounds.right() + 2.0f, height - (bounds.top() - 2.0f - y)},
              color.to<RgbaF>().toVec());
          }
        }
      }
    }
  }

  auto vertexArray = gl::VertexArray::move(std::move(vertices));
  auto shader =
    gl::ActiveShader{gl, shaderManager(), gl::Shaders::MaterialBrowserBorderShader};

  vertexArray.prepare(gl, vboManager());

  if (vertexArray.setup(gl, shader.program()))
  {
    vertexArray.render(gl, gl::PrimType::Quads);
    vertexArray.cleanup(gl, shader.program());
  }
}

const Color& MaterialBrowserView::materialColor(const gl::Material& material) const
{
  switch (materialBorder(material, &material == m_selectedMaterial))
  {
  case MaterialBorder::Selected:
    return pref(Preferences::MaterialBrowserSelectedColor);
  case MaterialBorder::NonSealing:
    return pref(Preferences::MaterialBrowserNonSealingColor);
  case MaterialBorder::Used:
    return pref(Preferences::MaterialBrowserUsedColor);
  case MaterialBorder::Default:
    return pref(Preferences::MaterialBrowserDefaultColor);
    switchDefault();
  }
}

void MaterialBrowserView::renderMaterials(
  gl::Gl& gl, Layout& layout, const float y, const float height)
{
  using Vertex = gl::VertexTypes::P2Uv2::Vertex;

  auto shader = gl::ActiveShader{gl, shaderManager(), gl::Shaders::MaterialBrowserShader};
  shader.set("ApplyTinting", false);
  shader.set("Material", 0);
  shader.set("Brightness", pref(Preferences::Brightness));

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
            const auto& bounds = cell.itemBounds();
            const auto& material = cellData(cell);

            auto vertexArray = gl::VertexArray::move(std::vector<Vertex>{
              Vertex{{bounds.left(), height - (bounds.top() - y)}, {0, 0}},
              Vertex{{bounds.left(), height - (bounds.bottom() - y)}, {0, 1}},
              Vertex{{bounds.right(), height - (bounds.bottom() - y)}, {1, 1}},
              Vertex{{bounds.right(), height - (bounds.top() - y)}, {1, 0}},
            });

            gl::setAlphaFuncUniforms(shader, &material);
            material.activate(
              gl,
              pref(Preferences::TextureMinFilter),
              pref(Preferences::TextureMagFilter));

            vertexArray.prepare(gl, vboManager());

            if (vertexArray.setup(gl, shader.program()))
            {
              vertexArray.render(gl, gl::PrimType::Quads);
              vertexArray.cleanup(gl, shader.program());
            }

            material.deactivate(gl);
          }
        }
      }
    }
  }
}

void MaterialBrowserView::doLeftClick(Layout& layout, const float x, const float y)
{
  if (const auto* cell = layout.cellAt(x, y))
  {
    const auto& material = cellData(*cell);
    setSelectedMaterial(&material);
    emit materialSelected(&material);

    update();
  }
}

QString MaterialBrowserView::tooltip(const Cell& cell)
{
  const auto& material = cellData(cell);

  auto tooltip = QString{};
  auto ss = QTextStream{&tooltip};
  ss << QString::fromStdString(material.name()) << "\n";

  // a utility material's description, in every browser that lists the material
  if (const auto* utilityMaterial =
        mdl::findUtilityMaterial(utilityMaterials(), material.name());
      utilityMaterial && !utilityMaterial->description.empty())
  {
    ss << QString::fromStdString(utilityMaterial->description) << "\n";
  }

  if (const auto* texture = material.texture())
  {
    ss << texture->width() << "x" << texture->height();
  }
  else
  {
    ss << "Loading...";
  }

  if (material.previewImageMissing())
  {
    ss << "\n" << tr("No preview image found");
  }

  if (const auto& surfaceParms = material.surfaceParms(); !surfaceParms.empty())
  {
    // the set is sorted
    ss << "\n" << QString::fromStdString(kdl::str_join(surfaceParms, ", "));
  }
  return tooltip;
}

void MaterialBrowserView::doContextMenu(
  Layout& layout, float x, float y, QContextMenuEvent* event)
{
  if (const auto* cell = layout.cellAt(x, y))
  {
    auto menu = QMenu{this};
    menu.addAction(tr("Select Faces"), this, [&, material = &cellData(*cell)]() {
      selectBrushFacesWithMaterial(m_document.map(), material->name());
    });

    menu.addAction(tr("Select Brushes"), this, [&, material = &cellData(*cell)]() {
      selectBrushesWithMaterial(m_document.map(), material->name());
    });

    menu.addSeparator();

    menu.addAction(tr("Copy Name"), this, [&, material = &cellData(*cell)]() {
      auto* clipboard = QApplication::clipboard();
      clipboard->setText(QString::fromStdString(material->name()));
    });

    menu.exec(event->globalPos());
  }
}

const gl::Material& MaterialBrowserView::cellData(const Cell& cell) const
{
  return *cell.itemAs<const gl::Material*>();
}

} // namespace tb::ui
