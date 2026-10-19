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
#include "base/Result.h"
#include "gl/FontDescriptor.h"
#include "ui/CellView.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/quat.h" // IWYU pragma: keep

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
} // namespace gl

namespace mdl
{
enum class Orientation;

struct PrefabEntry;
class PrefabLibrary;
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
  // the folder an entry must be in or below, relative to its root; nullopt shows every
  // folder
  std::optional<std::filesystem::path> category;
  // words, each of which must be in the name, a tag or the class name; split once when
  // the filter text changes
  std::vector<std::string> words;
  // tags, each of which must be one of the entry's tags, whole, ignoring case
  std::vector<std::string> tags;
};

// the prefix of a filter word that names a whole tag, as in tag:torch
constexpr auto PrefabTagFilterPrefix = std::string_view{"tag:"};

/**
 * A filter of both kinds and every folder with the words and the tags of the given text:
 * the text is split at spaces; a word with the prefix tag: names a whole tag, any other
 * word is matched as a part of the name, a tag or the class name.
 */
PrefabFilter prefabFilterFromText(std::string_view text);

/**
 * The distinct folders of the given entries, relative to their roots, in the order of
 * their generic strings; the top level is the empty path.
 */
std::vector<std::filesystem::path> prefabCategories(
  const std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries);

struct PrefabCategoryCount
{
  // relative to the roots; never the top level
  std::filesystem::path path;
  // the number of entries in the folder and in the folders below it
  std::size_t count = 0;
};

/**
 * Every folder that holds an entry or a folder that does, with the number of entries in
 * it and below it, in path order, so that a folder comes before the folders below it.
 * The top level is not listed.
 */
std::vector<PrefabCategoryCount> prefabCategoryCounts(
  const std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries);

/**
 * Whether the given entry passes the given filter. An entry whose file failed to load
 * passes only a filter that shows both kinds.
 */
bool matchesPrefabFilter(const mdl::PrefabEntry& entry, const PrefabFilter& filter);

// the longest side of a thumbnail as the browser keeps it
constexpr auto PrefabThumbnailMaxSize = std::size_t(128);

/**
 * Reads and decodes the given image and scales it down, keeping its aspect, so that its
 * longest side is at most PrefabThumbnailMaxSize. Runs on a resource manager worker.
 */
Result<gl::Texture> loadPrefabThumbnail(const std::filesystem::path& path);

/**
 * What the browser keeps of a library entry across layouts, clears and library changes;
 * defined in the implementation.
 */
struct PrefabPreview;

/**
 * What a cell of the prefab browser draws, in this order of preference: the thumbnail
 * image beside the file, the model of an entity snapshot, the edges of the prefab's
 * brushes and patches, or the prefab's bounds.
 */
struct PrefabCellData
{
  const mdl::PrefabEntry* entry = nullptr;
  // owned by the view; outlives the cell
  PrefabPreview* preview = nullptr;
  gl::FontDescriptor fontDescriptor;

  // the thumbnail is drawn; its texture is created when the cell is first drawn
  bool showsThumbnail = false;

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

  // per library entry, by its absolute path, built when a cell first needs it and kept
  // while the entry's file is unchanged, so that neither a filter change, nor a clear,
  // nor a rescan or an update of another file reads an image or walks a brush again
  std::unordered_map<std::filesystem::path::string_type, std::unique_ptr<PrefabPreview>>
    m_previews;
  // the number of previews built so far, which identifies each of them
  std::size_t m_previewCount = 0;
  // bumped when the entity definitions change, so that each preview takes its colour and
  // its model from the new definitions the next time a cell needs it
  std::size_t m_definitionGeneration = 0;

  // the absolute path of the entry that was clicked last, lexically normal, kept across
  // library changes
  std::filesystem::path m_selectedPath;
  // the entry of m_selectedPath in m_selectedLibrary, looked up when first asked for
  // after a library change or a click; nullopt until then
  mutable std::optional<const mdl::PrefabEntry*> m_selectedEntry;
  mutable const mdl::PrefabLibrary* m_selectedLibrary = nullptr;

  NotifierConnection m_notifierConnection;

public:
  PrefabBrowserView(
    AppController& appController, QScrollBar* scrollBar, MapDocument& document);
  ~PrefabBrowserView() override;

  void setFilter(PrefabFilter filter);

  /**
   * Call when the library's entries changed: the cells point into them. Drops the cells
   * and the previews of files that are no longer in the library.
   */
  void libraryDidChange();

  /**
   * Call when the entity models may be reloaded: the cells hold model renderers that the
   * entity model manager deletes when it is cleared. The previews look their models up
   * again.
   */
  void entityModelsWillChange();

  /**
   * Drops the thumbnails, so that each is read again when a cell next draws it; for an
   * explicit rescan, since a library change keeps the thumbnails of unchanged files.
   */
  void reloadThumbnails();

  /**
   * The entry that the panel's buttons act on: the one whose cell was clicked last, by
   * the left or the right button. A cell is selected by its file, so the selection
   * survives a rescan; nullptr if that file is no longer in the library.
   */
  const mdl::PrefabEntry* selectedEntry() const;
  void selectEntry(const std::filesystem::path& absolutePath);

  /**
   * A cell as the view lays it out, for tests.
   */
  struct CellSummary
  {
    const mdl::PrefabEntry* entry;
    std::string title;
    // what a drag of the cell carries
    QString dndData;
    // identifies the cell's preview: equal while the preview is kept
    std::size_t previewId;
  };

  /**
   * The cells in layout order, laid out first if the layout is not valid. For tests.
   */
  std::vector<CellSummary> cellSummaries();

signals:
  void editPrefabRequested(const QString& path);
  void applyPrefabRequested(const QString& path);
  void updatePrefabRequested(const QString& path);
  void refreshPrefabRequested(const QString& path);
  void revealPrefabRequested(const QString& path);
  void deletePrefabRequested(const QString& path);
  void openThumbnailRequested(const QString& path);
  void removeThumbnailRequested(const QString& path);
  void selectedEntryChanged();

private:
  void doInitLayout(Layout& layout) override;
  void doReloadLayout(Layout& layout) override;

  bool dndEnabled() override;
  QString dndData(const Cell& cell) override;

  void resourcesWereProcessed(const std::vector<gl::ResourceId>& resources);
  void preferenceDidChange(const std::filesystem::path& path);
  void entityDefinitionsDidChange();

  PrefabPreview& preview(const mdl::PrefabEntry& entry);
  void updatePreviewAppearance(PrefabPreview& preview, const mdl::PrefabEntry& entry);
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
  void doLeftClick(Layout& layout, float x, float y) override;
  void doContextMenu(Layout& layout, float x, float y, QContextMenuEvent* event) override;

  const PrefabCellData& cellData(const Cell& cell) const;
};

} // namespace ui
} // namespace tb
