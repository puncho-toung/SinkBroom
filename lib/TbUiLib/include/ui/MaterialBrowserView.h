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

#include "base/NotifierConnection.h"
#include "gl/FontDescriptor.h"
#include "mdl/UtilityMaterialConfig.h"
#include "ui/CellView.h"
#include "ui/MaterialBrowserScope.h"

#include <functional>
#include <string>
#include <vector>

class QScrollBar;

namespace tb
{
namespace gl
{
class Material;
class MaterialCollection;
class ResourceId;
} // namespace gl

namespace ui
{
class AppController;
class MapDocument;

using MaterialGroupData = std::string;

enum class MaterialSortOrder
{
  Name,
  Usage
};

/**
 * Returns the given materials without the ones that the material browser's filters hide:
 * unused materials if hideUnused is set, materials without a preview image if hideMissing
 * is set, and, if filterText isn't empty, materials whose names contain none of its
 * space-separated words. By the scope, a regular browser also hides the given utility
 * materials and a utility browser lists nothing else; All ignores them.
 */
std::vector<const gl::Material*> filterMaterials(
  std::vector<const gl::Material*> materials,
  bool hideUnused,
  bool hideMissing,
  const std::string& filterText,
  MaterialBrowserScope scope = MaterialBrowserScope::All,
  const std::vector<mdl::UtilityMaterialConfig>& utilityMaterials = {});

/**
 * Breaks text between words so that no line measures wider than maxWidth; a single word
 * wider than maxWidth stands on its own line. Lines are joined with '\n'.
 */
std::string wrapWords(
  const std::string& text,
  float maxWidth,
  const std::function<float(const std::string&)>& measureWidth);

enum class MaterialBorder
{
  Default,
  Selected,
  NonSealing,
  Used
};

/**
 * Returns the border that the material browser draws around the given material: the
 * selected one's, else the non-sealing one's if the material has translucent or
 * alpha-tested (perforated) coverage, such as glass, decals and grates: a brush with such
 * a face does not seal a Doom 3 map. Else the used one's if the material is in use. The
 * coverage is read from the surface parms coverage_translucent and coverage_perforated,
 * never from the alpha or blend function, which describe the preview image.
 */
MaterialBorder materialBorder(const gl::Material& material, bool selected);

class MaterialBrowserView : public CellView
{
  Q_OBJECT
private:
  MapDocument& m_document;
  MaterialBrowserScope m_scope;
  bool m_group = false;
  bool m_hideUnused = false;
  bool m_hideMissing = false;
  MaterialSortOrder m_sortOrder = MaterialSortOrder::Name;
  std::string m_filterText;

  const gl::Material* m_selectedMaterial = nullptr;

  NotifierConnection m_notifierConnection;

public:
  MaterialBrowserView(
    AppController& appController,
    QScrollBar* scrollBar,
    MapDocument& document,
    MaterialBrowserScope scope);
  ~MaterialBrowserView() override;

  void setSortOrder(MaterialSortOrder sortOrder);
  void setGroup(bool group);
  void setHideUnused(bool hideUnused);
  void setHideMissing(bool hideMissing);
  void setFilterText(const std::string& filterText);

  const gl::Material* selectedMaterial() const;
  void setSelectedMaterial(const gl::Material* selectedMaterial);

  void revealMaterial(const gl::Material* material);

private:
  void resourcesWereProcessed(const std::vector<gl::ResourceId>& resources);

  void reloadMaterials();

  void doInitLayout(Layout& layout) override;
  void doReloadLayout(Layout& layout) override;

  void addMaterialsToLayout(
    Layout& layout,
    const std::vector<const gl::Material*>& materials,
    const gl::FontDescriptor& font);
  void addMaterialToLayout(
    Layout& layout, const gl::Material& material, const gl::FontDescriptor& font);
  std::string cellTitle(
    const gl::Material& material, float maxWidth, const gl::FontDescriptor& font);

  const std::vector<mdl::UtilityMaterialConfig>& utilityMaterials() const;
  std::vector<const gl::MaterialCollection*> getCollections() const;
  std::vector<const gl::Material*> getMaterials(
    const gl::MaterialCollection& collection) const;
  std::vector<const gl::Material*> getMaterials() const;

  std::vector<const gl::Material*> filterMaterials(
    std::vector<const gl::Material*> materials) const;
  std::vector<const gl::Material*> sortMaterials(
    std::vector<const gl::Material*> materials) const;

  void doClear() override;
  void doRender(gl::Gl& gl, Layout& layout, float y, float height) override;
  bool shouldRenderFocusIndicator() const override;
  const Color& getBackgroundColor() override;

  void renderBounds(gl::Gl& gl, Layout& layout, float y, float height);
  const Color& materialColor(const gl::Material& material) const;
  void renderMaterials(gl::Gl& gl, Layout& layout, float y, float height);

  void doLeftClick(Layout& layout, float x, float y) override;
  QString tooltip(const Cell& cell) override;
  void doContextMenu(Layout& layout, float x, float y, QContextMenuEvent* event) override;

  const gl::Material& cellData(const Cell& cell) const;
signals:
  void materialSelected(const gl::Material* material);
};

} // namespace ui
} // namespace tb
