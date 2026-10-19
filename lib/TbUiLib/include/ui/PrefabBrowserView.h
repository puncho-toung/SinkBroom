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

#pragma once

#include <QString>

#include "base/NotifierConnection.h"
#include "gl/FontDescriptor.h"
#include "ui/CellView.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/quat.h" // IWYU pragma: keep

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb
{
namespace gl
{
class MaterialRenderer;
class ResourceId;
class Texture;
class VertexArray;

template <typename T>
class Resource;
} // namespace gl

namespace mdl
{
enum class Orientation;

struct PrefabEntry;
} // namespace mdl

namespace render
{
class Transformation;
} // namespace render

namespace ui
{
class AppController;
class MapDocument;

enum class PrefabKindFilter
{
  All,
  Entities,
  Assemblies,
};

/**
 * Which library entries the prefab browser shows.
 */
struct PrefabFilter
{
  PrefabKindFilter kind = PrefabKindFilter::All;
  // the folder an entry must be in, relative to its root; nullopt shows every folder
  std::optional<std::filesystem::path> category;
  // space-separated words, each of which must be in the name, a tag or the class name
  std::string text;
};

/**
 * The distinct folders of the given entries, relative to their roots, in the order of
 * their generic strings; the top level is the empty path.
 */
std::vector<std::filesystem::path> prefabCategories(
  const std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries);

/**
 * Whether the given entry passes the given filter. An entry whose file failed to load
 * passes only a filter that shows both kinds.
 */
bool matchesPrefabFilter(const mdl::PrefabEntry& entry, const PrefabFilter& filter);

/**
 * What a cell of the prefab browser draws, in this order of preference: the thumbnail
 * image beside the file, the model of an entity snapshot, the edges of the prefab's
 * brushes and patches, or the prefab's bounds.
 */
struct PrefabCellData
{
  const mdl::PrefabEntry* entry = nullptr;
  gl::FontDescriptor fontDescriptor;

  std::shared_ptr<gl::Resource<gl::Texture>> thumbnail;

  gl::MaterialRenderer* modelRenderer = nullptr;
  mdl::Orientation modelOrientation;

  // the edges or the bounds as lines in prefab space, built once per library change
  std::shared_ptr<gl::VertexArray> edges;

  vm::bbox3f bounds;
  vm::mat4x4f transform;
};

class PrefabBrowserView : public CellView
{
  Q_OBJECT
private:
  static constexpr auto CameraPosition = vm::vec3f{256.0f, 0.0f, 0.0f};
  static constexpr auto CameraDirection = vm::vec3f{-1, 0, 0};
  static constexpr auto CameraUp = vm::vec3f{0, 0, 1};

  MapDocument& m_document;
  vm::quatf m_rotation;
  PrefabFilter m_filter;

  struct EntryPreview;
  // per library entry, built when a cell first needs it and dropped when the library
  // changes, so that a filter change neither reads an image nor walks a brush again
  std::unordered_map<const mdl::PrefabEntry*, std::unique_ptr<EntryPreview>> m_previews;

  NotifierConnection m_notifierConnection;

public:
  PrefabBrowserView(
    AppController& appController, QScrollBar* scrollBar, MapDocument& document);
  ~PrefabBrowserView() override;

  void setFilter(PrefabFilter filter);

  /**
   * Call when the library's entries changed: the cells point into them.
   */
  void libraryDidChange();

  /**
   * Call when the entity models may be reloaded: the cells hold model renderers that the
   * entity model manager deletes when it is cleared.
   */
  void entityModelsWillChange();

  /**
   * A cell as the view lays it out, for tests.
   */
  struct CellSummary
  {
    const mdl::PrefabEntry* entry;
    std::string title;
    // what a drag of the cell carries
    QString dndData;
  };

  /**
   * The cells in layout order, laid out first if the layout is not valid. For tests.
   */
  std::vector<CellSummary> cellSummaries();

signals:
  void editPrefabRequested(const QString& path);

private:
  void doInitLayout(Layout& layout) override;
  void doReloadLayout(Layout& layout) override;

  bool dndEnabled() override;
  QString dndData(const Cell& cell) override;

  void resourcesWereProcessed(const std::vector<gl::ResourceId>& resources);
  void preferenceDidChange(const std::filesystem::path& path);

  const EntryPreview& preview(const mdl::PrefabEntry& entry);
  void addEntryToLayout(
    Layout& layout, const mdl::PrefabEntry& entry, const gl::FontDescriptor& font);

  void doClear() override;
  void doRender(gl::Gl& gl, Layout& layout, float y, float height) override;
  bool shouldRenderFocusIndicator() const override;
  const Color& getBackgroundColor() override;

  void renderEdges(
    gl::Gl& gl,
    Layout& layout,
    float y,
    float height,
    render::Transformation& transformation);
  void renderModels(
    gl::Gl& gl,
    Layout& layout,
    float y,
    float height,
    render::Transformation& transformation);
  void renderThumbnails(
    gl::Gl& gl,
    Layout& layout,
    float y,
    float height,
    render::Transformation& transformation);

  vm::mat4x4f itemTransformation(const Cell& cell, float y, float height) const;

  QString tooltip(const Cell& cell) override;
  void doContextMenu(Layout& layout, float x, float y, QContextMenuEvent* event) override;

  const PrefabCellData& cellData(const Cell& cell) const;
};

} // namespace ui
} // namespace tb
