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

#include <QObject>

#include "base/Result.h"

#include "vm/bbox.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
namespace gl
{
class ResourceManager;
}

namespace mdl
{
enum class MapFormat;

struct EnvironmentConfig;
struct GameInfo;
} // namespace mdl

namespace ui
{
class AppController;
class MapDocument;
class MapWindow;

class MapWindowManager : public QObject
{
  Q_OBJECT
public:
  // shows a new window or brings one to the front; see setShowMapWindowForTesting
  using ShowMapWindow = std::function<void(MapWindow&)>;

private:
  AppController& m_appController;
  bool m_singleMapWindow;
  std::vector<MapWindow*> m_mapWindows;
  // replaces showMapWindow's show, raise and activate, if set
  ShowMapWindow m_showMapWindow;

public:
  MapWindowManager(AppController& appController, bool singleMapWindow);
  ~MapWindowManager() override;

  /**
   * Creates a document in a new window, or, if the manager keeps a single map window, in
   * the top window that is not a prefab editor. If a prefab editor is on top, that window
   * first offers to save its changes and is brought to the front; nothing is created if
   * the user cancels. The same holds for loadDocument.
   */
  Result<void> createDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const vm::bbox3d& worldBounds);

  Result<void> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const vm::bbox3d& worldBounds,
    std::filesystem::path path);

  /**
   * Opens the given prefab file in a prefab editor beside the given window, with the
   * game, map format and world bounds of that window's document, which the prefab
   * library reads its files with. The editor is a window of its own even if the manager
   * keeps a single map window (the one exception, NR D23), and New and Open never
   * replace its document. If a window has the file open, an editor or not, it is brought
   * to the front instead. Returns an error if the given window was closed.
   */
  Result<void> openPrefabEditor(MapWindow& hostWindow, const std::filesystem::path& path);

  /**
   * Tells every window, the prefab editor that saved the given file too, of the save; the
   * prefab library of each reads it if it is in one of its roots.
   */
  void prefabWasSaved(const std::filesystem::path& path);

  std::vector<MapWindow*> mapWindows() const;
  MapWindow* topMapWindow() const;
  bool allMapWindowsClosed() const;

  /**
   * Makes the manager keep a single map window, or one per document, as on Windows and
   * on the other platforms. For tests.
   */
  void setSingleMapWindowForTesting(bool singleMapWindow);

  /**
   * Replaces how a new window is shown and an open prefab editor brought to the front;
   * an empty function restores both. For tests, whose GL views must not be shown.
   */
  void setShowMapWindowForTesting(ShowMapWindow showMapWindow);

private:
  void onFocusChange(QWidget* old, QWidget* now);

  /**
   * The window whose document New and Open replace if the manager keeps a single map
   * window: the top one that is not a prefab editor.
   */
  MapWindow* documentWindow() const;
  /**
   * The window whose document has the given file, which is absolute and lexically
   * normal.
   */
  MapWindow* findMapWindow(const std::filesystem::path& path) const;
  /**
   * Returns false if the user keeps the changes of the given window, whose document New
   * or Open replace; see createDocument.
   */
  bool prepareDocumentWindow(MapWindow& mapWindow);

  bool shouldCreateWindowForDocument() const;
  MapWindow* createMapWindow(
    std::unique_ptr<MapDocument> document,
    bool prefabEditor = false,
    QWidget* reference = nullptr);
  void showMapWindow(MapWindow& mapWindow);
  void moveToTop(MapWindow& mapWindow);
  void removeMapWindow(MapWindow* mapWindow);

  friend class MapWindow;
};

} // namespace ui
} // namespace tb
