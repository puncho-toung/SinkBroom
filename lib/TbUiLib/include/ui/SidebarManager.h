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

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <vector>

class QAction;
class QDockWidget;
class QMainWindow;

namespace tb::ui
{
class SidebarPanel;

/**
 * Puts sidebar panels into docks of a main window.
 *
 * The manager is a child of the main window and owns nothing it must delete: each dock
 * is a child of the main window and owns its panel. The manager only keeps guarded
 * pointers, so it never touches a dock or a panel that Qt has already destroyed.
 */
class SidebarManager : public QObject
{
  Q_OBJECT
private:
  struct Entry
  {
    QString panelId;
    QPointer<QDockWidget> dock;
    QPointer<SidebarPanel> panel;
  };

  QMainWindow& m_mainWindow;
  std::vector<Entry> m_entries;

public:
  explicit SidebarManager(QMainWindow& mainWindow);
  ~SidebarManager() override;

  /**
   * Wraps the given panel in a dock at the panel's default edge of the main window,
   * hidden unless the panel is shown by default. The dock may be moved to every edge. It
   * is named "MapWindow_Sidebar_<panelId>", which keys its place in the saved window
   * state. The panel's id must not be registered yet.
   */
  QDockWidget* registerPanel(std::unique_ptr<SidebarPanel> panel);

  /**
   * The registered panel with the given id, or null.
   */
  SidebarPanel* panel(const QString& panelId) const;

  /**
   * The action that shows and closes the given panel's dock, or null if no such panel
   * is registered.
   */
  QAction* toggleAction(const QString& panelId) const;

  /**
   * Shows the given panel's dock and brings it to the front of its tab group.
   */
  void showPanel(const QString& panelId);

  /**
   * The registered panels that still exist, in the order of registration.
   */
  std::vector<SidebarPanel*> panels() const;

  /**
   * The docks of the registered panels that still exist, in the order of registration.
   */
  std::vector<QDockWidget*> docks() const;

  /**
   * Docks every floating dock whose frame is on no screen, for example because the
   * window state was saved with a monitor that is no longer connected.
   */
  void redockOffScreenDocks();

private:
  QDockWidget* findDock(const QString& panelId) const;
};

} // namespace tb::ui
