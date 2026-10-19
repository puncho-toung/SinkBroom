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
 * The panel owns every rescan of the library: on construction, on Rescan, after a save
 * and when the mods, the game path or the Prefab Path change. It never changes the
 * library's entries.
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
   * Opens the save dialog for the current selection and rescans the library after a
   * save. Does nothing if the selection is empty.
   */
  void saveSelectionAsPrefab();

  /**
   * Reads the library roots again.
   */
  void rescan();

private:
  void createGui();
  void connectObservers();
  void connectLibraryObserver();

  void processPendingResults();
  void libraryDidChange();

  void updateCategories();
  void updateFilter();
  void updateSaveButton();

  void editPrefab(const QString& path);
};

} // namespace tb::ui
