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
#include "ui/CellView.h"

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
 * space-separated words.
 */
std::vector<const gl::Material*> filterMaterials(
  std::vector<const gl::Material*> materials,
  bool hideUnused,
  bool hideMissing,
  const std::string& filterText);

enum class MaterialBorder
{
  Default,
  Selected,
  Translucent,
  Used
};

/**
 * Returns the border that the material browser draws around the given material: the
 * selected one's, else the translucent one's if the material has the surface parm
 * coverage_translucent, else the used one's if the material is in use. A brush with a
 * translucent face, such as glass or a decal, doesn't seal a Doom 3 map, so the border
 * warns of leaks even for a material in use.
 */
MaterialBorder materialBorder(const gl::Material& material, bool selected);

class MaterialBrowserView : public CellView
{
  Q_OBJECT
private:
  MapDocument& m_document;
  bool m_group = false;
  bool m_hideUnused = false;
  bool m_hideMissing = false;
  MaterialSortOrder m_sortOrder = MaterialSortOrder::Name;
  std::string m_filterText;

  const gl::Material* m_selectedMaterial = nullptr;

  NotifierConnection m_notifierConnection;

public:
  MaterialBrowserView(
    AppController& appController, QScrollBar* scrollBar, MapDocument& document);
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
