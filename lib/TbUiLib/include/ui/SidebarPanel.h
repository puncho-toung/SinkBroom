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

#include <QDockWidget>
#include <QString>
#include <QWidget>

#include <filesystem>

class QCloseEvent;

namespace tb::ui
{
class AppController;
class MapDocument;
class MapViewToolBox;

/**
 * What a sidebar panel may use. The references outlive every panel: the document and the
 * tool box live as long as the map window that owns the panel's dock. A panel holds no
 * pointer into the document's map between notifications.
 */
struct SidebarContext
{
  AppController& appController;
  MapDocument& document;
  MapViewToolBox& toolBox;
};

/**
 * A widget shown in a dock of the map window's sidebar. Each panel decides on its own
 * whether it applies to the current game; the sidebar itself is game agnostic.
 */
class SidebarPanel : public QWidget
{
  Q_OBJECT
public:
  explicit SidebarPanel(QWidget* parent = nullptr);
  ~SidebarPanel() override;

  /**
   * A stable identifier. It names the panel's dock and therefore its place in the saved
   * window state, so it must not change once released.
   */
  virtual QString panelId() const = 0;

  /**
   * The dock's title and the label of its entry in View > Sidebar.
   */
  virtual QString title() const = 0;

  /**
   * The icon of the panel's button in the tool sidebar, an image path as loadSVGIcon
   * takes it. Empty by default: a panel without an icon gets a text button there.
   */
  virtual std::filesystem::path iconPath() const;

  /**
   * Called when the panel's dock becomes visible: opened, raised from behind a tab, or
   * shown again after the window was minimised. A refresh hint only.
   */
  virtual void panelShown();

  /**
   * Called when the panel's dock is closed by the user, from the dock's close button or
   * from its entry in View > Sidebar. Not called when the dock is merely hidden: when
   * another dock is tabbed over it, when it is floated or docked again, or when the
   * window is minimised. Not called when the dock is deleted either.
   */
  virtual void panelHidden();
};

/**
 * The dock that hosts a sidebar panel. Closing it tells the panel and does nothing else.
 */
class SidebarDock : public QDockWidget
{
  Q_OBJECT
public:
  explicit SidebarDock(const QString& title, QWidget* parent = nullptr);
  ~SidebarDock() override;

protected:
  void closeEvent(QCloseEvent* event) override;
};

} // namespace tb::ui
