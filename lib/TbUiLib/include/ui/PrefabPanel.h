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

#include <QSet>
#include <QString>

#include "base/NotifierConnection.h"
#include "base/Result.h"
#include "ui/SidebarPanel.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QScrollBar;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace tb::mdl
{
struct PrefabPlacement;
} // namespace tb::mdl

namespace tb::ui
{
class AppController;
class MapDocument;
class PrefabBrowserView;

/**
 * The prefab library of the document's map: a filterable grid of the prefab files under
 * the library roots, from which prefabs are dragged into the map views. A tree of the
 * library's folders above the grid shows the prefabs of one folder and the folders below
 * it; the filter box matches parts of names, tags and classes, and whole tags after
 * tag:. Without roots, or without prefabs, the grid gives way to a line that says so.
 *
 * The panel owns every scan of the library. The first one runs when the panel is first
 * shown, or when a caller first needs the entries (scanOnce); after it, Rescan, a new
 * document and a change of the mods, the game path or the Prefab Path rescan, coalesced
 * into one scan per pass of the event loop, and a save reads the saved file only. It
 * never changes the library's entries. While the panel is visible, a change in the
 * library's folders rescans half a second after the last one; the panel's own save is
 * not such a change. A closed panel watches no folder; it rescans when it is shown.
 *
 * Placed assemblies whose file has changed are refreshed only when the user says so
 * (D17): once a document is loaded and the library is scanned, a prompt lists them; the
 * Refresh button shows their number and offers the same prompt on demand.
 */
class PrefabPanel : public SidebarPanel
{
  Q_OBJECT
public:
  static constexpr auto PanelId = "Prefabs";

  // offers changed placements for a refresh; see setOpenPromptForTesting
  using RefreshPrompt = std::function<void(const std::vector<mdl::PrefabPlacement>&)>;

private:
  AppController& m_appController;
  MapDocument& m_document;

  QTreeWidget* m_categoryTree = nullptr;
  QComboBox* m_kindChoice = nullptr;
  QLineEdit* m_filterBox = nullptr;
  QToolButton* m_saveButton = nullptr;
  QToolButton* m_rescanButton = nullptr;
  QToolButton* m_refreshButton = nullptr;
  QToolButton* m_applyButton = nullptr;
  QScrollBar* m_scrollBar = nullptr;
  PrefabBrowserView* m_view = nullptr;
  // the grid, or the line that stands in for it while there is nothing to show
  QStackedWidget* m_browserStack = nullptr;
  QWidget* m_browserPage = nullptr;
  QLabel* m_emptyLabel = nullptr;
  QTimer* m_scanTimer = nullptr;
  QFileSystemWatcher* m_watcher = nullptr;
  // restarted by every change in a watched folder; rescans when it runs out
  QTimer* m_watchTimer = nullptr;

  // the first scan has started; until then, nothing rescans
  bool m_scanStarted = false;
  // a scan that the panel started has not finished yet
  bool m_scanRunning = false;
  // a rescan is posted to the event loop
  bool m_rescanPending = false;
  std::size_t m_scanCount = 0;

  // a document was loaded; once its library is scanned, the changed placements are
  // offered for a refresh
  bool m_promptAfterScan = false;
  // counts the loaded documents, so that a prompt posted for one is not shown for the
  // next
  std::size_t m_loadGeneration = 0;
  // replaces promptToRefreshPlacements for the prompt after a load, if set
  RefreshPrompt m_openPrompt;
  // a library scan has finished, so that missing files are missing indeed
  bool m_scanFinished = false;
  // the map changed while the panel was hidden, so the count on the Refresh button may
  // be out of date
  bool m_refreshCountStale = false;
  // the panel is open, so that it watches the library's folders: from panelShown to
  // panelHidden
  bool m_watching = false;
  // a folder may have changed while the panel was hidden or did not watch it
  bool m_watchMissed = false;
  // the watched paths that changed since the watch timer was started
  QSet<QString> m_changedPaths;
  // the panel deleted a file of the library since the watch timer was started
  bool m_libraryFilesChanged = false;
  // the files and folders that the panel's own save changed, whose changes the watcher
  // reports until m_ownWritesUntil; the library has read the saved file already
  QSet<QString> m_ownWrites;
  std::chrono::steady_clock::time_point m_ownWritesUntil;
  std::size_t m_changedPlacementCount = 0;
  std::size_t m_recountCount = 0;

  NotifierConnection m_notifierConnection;
  // to the library of the document's current map, renewed when the map is replaced
  NotifierConnection m_libraryConnection;

public:
  explicit PrefabPanel(const SidebarContext& context, QWidget* parent = nullptr);
  ~PrefabPanel() override;

  QString panelId() const override;
  QString title() const override;
  std::filesystem::path iconPath() const override;

  void panelShown() override;
  void panelHidden() override;

  /**
   * Opens the save dialog for the current selection and reads the saved file into the
   * library. Does nothing if the selection is empty.
   */
  void saveSelectionAsPrefab();

  /**
   * Reads the given file, which was just saved, into the library, and no other file.
   * Does nothing until the panel has started its first scan, which reads every file.
   */
  void prefabWasSaved(const std::filesystem::path& path);

  /**
   * DD-28: a prefab editor's panel cannot place prefabs, since a prefab holds no
   * placements of prefabs (RQ21): a drag of a cell carries nothing. Saving the selection
   * as a prefab stays. On by default.
   */
  void setPlacementEnabled(bool placementEnabled);

  /**
   * Starts the first scan of the library unless it has started; for a caller that needs
   * the entries before the panel was shown.
   */
  void scanOnce();

  /**
   * Reads the library roots again once the event loop runs; several calls before then
   * scan once. Does nothing before the first scan, which reads the roots as they are.
   */
  void rescan();

  /**
   * The number of scans the panel has started. For tests.
   */
  std::size_t scanCount() const;

  /**
   * The number of placed assemblies whose file has changed or is missing, as the Refresh
   * button shows it; 0 until the library was scanned.
   */
  std::size_t changedPlacementCount() const;

  /**
   * The number of times the panel has counted the changed placements, a walk of the
   * map's groups. For tests.
   */
  std::size_t recountCount() const;

  /**
   * Lists the given placements, of which those with a file are refreshed if the user
   * agrees, one transaction each (F9); reports the entities that lost their overrides
   * and any placement that could not be refreshed.
   */
  void promptToRefreshPlacements(const std::vector<mdl::PrefabPlacement>& placements);

  /**
   * Replaces the prompt that offers the changed placements of a loaded document once its
   * library is scanned, so that tests see when and with what it is shown; an empty
   * function restores promptToRefreshPlacements. The prompt is not shown if no changed
   * placement has a file (DD-24); the Refresh button counts them.
   */
  void setOpenPromptForTesting(RefreshPrompt prompt);

  /**
   * Makes the entities of the selected cell's class in the map selection match the
   * selected cell's entity snapshot.
   */
  void applyToSelection();

  /**
   * Opens the save dialog for the current selection, filled in from the prefab with the
   * given file, and overwrites that file.
   */
  void updatePrefabFromSelection(const std::filesystem::path& path);

  /**
   * Deletes the given prefab file and the thumbnail beside it, without asking, and
   * rescans: a visible panel once the library's folders are quiet, as after any change
   * in them, a hidden one at once. Placements of the prefab in the map are not touched;
   * they count as missing. Returns an error, and deletes nothing, if the path is not the
   * file of an entry of the library.
   */
  Result<void> deletePrefabFile(const std::filesystem::path& path);

  /**
   * Deletes the thumbnail of the given prefab file, without asking, and rescans as
   * deletePrefabFile does, so that the cell draws the prefab itself. Returns an error,
   * and deletes nothing, if the path
   * is not the file of an entry of the library or the entry has no thumbnail.
   */
  Result<void> removePrefabThumbnail(const std::filesystem::path& path);

private:
  void createGui();
  void connectObservers();
  void connectLibraryObserver();

  void startScan();
  void processPendingResults();
  bool scanJustFinished() const;
  void scanDidFinish();
  void libraryDidChange();

  void updateCategories();
  void updateFilter();
  void updateEmptyState();
  void updateSaveButton();
  void updateRefreshButton();
  void updateApplyButton();
  void updateWatchedPaths();
  void watchSavedFile(const std::filesystem::path& path);
  bool isOwnWrite(const QString& path) const;
  void folderDidChange(const QString& path);
  void watchTimerDidRunOut();
  void libraryFilesDidChange();
  void mapDidChange();
  void promptAfterScanIfNeeded();

  void editPrefab(const QString& path);
  void revealPrefabInFolder(const std::filesystem::path& path);
  void openPrefabThumbnail(const std::filesystem::path& path);
  void confirmDeletePrefabFile(const std::filesystem::path& path);
  void confirmRemovePrefabThumbnail(const std::filesystem::path& path);
};

} // namespace tb::ui
