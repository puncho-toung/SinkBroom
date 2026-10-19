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

#include "base/NotifierConnection.h"
#include "ui/SidebarPanel.h"

#include <cstddef>
#include <filesystem>

class QComboBox;
class QLineEdit;
class QScrollBar;
class QTimer;
class QToolButton;

namespace tb::ui
{
class AppController;
class MapDocument;
class PrefabBrowserView;

/**
 * The prefab library of the document's map: a filterable grid of the prefab files under
 * the library roots, from which prefabs are dragged into the map views.
 *
 * The panel owns every scan of the library. The first one runs when the panel is first
 * shown, or when a caller first needs the entries (scanOnce); after it, Rescan, a new
 * document and a change of the mods, the game path or the Prefab Path rescan, coalesced
 * into one scan per pass of the event loop, and a save reads the saved file only. It
 * never changes the library's entries.
 */
class PrefabPanel : public SidebarPanel
{
  Q_OBJECT
public:
  static constexpr auto PanelId = "Prefabs";

private:
  AppController& m_appController;
  MapDocument& m_document;

  QComboBox* m_categoryChoice = nullptr;
  QComboBox* m_kindChoice = nullptr;
  QLineEdit* m_filterBox = nullptr;
  QToolButton* m_saveButton = nullptr;
  QToolButton* m_rescanButton = nullptr;
  QScrollBar* m_scrollBar = nullptr;
  PrefabBrowserView* m_view = nullptr;
  QTimer* m_scanTimer = nullptr;

  // the first scan has started; until then, nothing rescans
  bool m_scanStarted = false;
  // a rescan is posted to the event loop
  bool m_rescanPending = false;
  std::size_t m_scanCount = 0;

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

  /**
   * Opens the save dialog for the current selection and reads the saved file into the
   * library. Does nothing if the selection is empty.
   */
  void saveSelectionAsPrefab();

  /**
   * Reads the given file, which was just saved, into the library, and no other file.
   */
  void prefabWasSaved(const std::filesystem::path& path);

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

private:
  void createGui();
  void connectObservers();
  void connectLibraryObserver();

  void startScan();
  void processPendingResults();
  void libraryDidChange();

  void updateCategories();
  void updateFilter();
  void updateSaveButton();

  void editPrefab(const QString& path);
};

} // namespace tb::ui
