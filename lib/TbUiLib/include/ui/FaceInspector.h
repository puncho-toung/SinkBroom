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

#include <QString>

#include "base/NotifierConnection.h"
#include "ui/SidebarPanel.h"

#include <filesystem>

class QSplitter;
class QStackedWidget;
class QWidget;

namespace tb
{
namespace gl
{
class Material;
} // namespace gl

namespace ui
{
class AppController;
class CollapsibleTitledPanel;
class FaceAttribsEditor;
class MapDocument;
class MaterialBrowser;
class TabBook;

class FaceInspector : public SidebarPanel
{
  Q_OBJECT
public:
  static constexpr auto PanelId = "FaceInspector";

private:
  MapDocument& m_document;
  QSplitter* m_splitter = nullptr;
  FaceAttribsEditor* m_faceAttribsEditor = nullptr;
  TabBook* m_tabBook = nullptr;
  MaterialBrowser* m_materialBrowser = nullptr;
  QWidget* m_materialBrowserInfo = nullptr;
  QStackedWidget* m_utilityStack = nullptr;
  MaterialBrowser* m_utilityMaterialBrowser = nullptr;

  NotifierConnection m_notifierConnection;

public:
  FaceInspector(
    AppController& appController, MapDocument& document, QWidget* parent = nullptr);
  ~FaceInspector() override;

  QString panelId() const override;
  QString title() const override;
  std::filesystem::path iconPath() const override;
  bool shownByDefault() const override;

  bool cancelMouseDrag();

  /**
   * Shows the tab that lists the given material, the Utility tab for a utility material
   * of the game and the Materials tab otherwise, and selects the material there. A null
   * material shows the Materials tab.
   */
  void revealMaterial(const gl::Material* material);

private:
  void createGui(AppController& appController);
  QWidget* createFaceAttribsEditor(AppController& appController);
  QWidget* createMaterialTabs(AppController& appController);
  QWidget* createMaterialBrowser(AppController& appController);
  QWidget* createMaterialBrowserInfo();
  QWidget* createUtilityMaterialBrowser(AppController& appController);

  void materialSelected(const gl::Material* material);

  void connectObservers();
  void documentWasLoaded();
  void updateUtilityPage();
};

} // namespace ui
} // namespace tb
