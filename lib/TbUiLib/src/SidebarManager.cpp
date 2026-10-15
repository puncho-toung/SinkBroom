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

#include "ui/SidebarManager.h"

#include <QAction>
#include <QDockWidget>
#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>

#include "ui/SidebarPanel.h"

#include "kd/contracts.h"

#include <algorithm>

namespace tb::ui
{

SidebarManager::SidebarManager(QMainWindow& mainWindow)
  : QObject{&mainWindow}
  , m_mainWindow{mainWindow}
{
}

SidebarManager::~SidebarManager() = default;

QDockWidget* SidebarManager::registerPanel(std::unique_ptr<SidebarPanel> panel)
{
  contract_pre(panel != nullptr);

  const auto panelId = panel->panelId();
  contract_pre(!panelId.isEmpty());
  contract_pre(std::ranges::none_of(
    m_entries, [&](const auto& entry) { return entry.panelId == panelId; }));

  auto* dock = new SidebarDock{panel->title(), &m_mainWindow};
  dock->setObjectName(QString{"MapWindow_Sidebar_"} + panelId);
  dock->setFeatures(
    QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable
    | QDockWidget::DockWidgetFloatable);
  dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

  auto* panelPtr = panel.get();
  dock->setWidget(panel.release());

  m_mainWindow.addDockWidget(Qt::RightDockWidgetArea, dock);
  dock->hide();

  // connected after hiding so that registering never reports a panel as shown; the panel
  // is the context, so the connection goes away with it
  connect(
    dock, &QDockWidget::visibilityChanged, panelPtr, [panelPtr](const bool visible) {
      if (visible)
      {
        panelPtr->panelShown();
      }
    });

  m_entries.push_back({panelId, dock, panelPtr});
  return dock;
}

QAction* SidebarManager::toggleAction(const QString& panelId) const
{
  auto* dock = findDock(panelId);
  return dock ? dock->toggleViewAction() : nullptr;
}

void SidebarManager::showPanel(const QString& panelId)
{
  if (auto* dock = findDock(panelId))
  {
    dock->show();
    dock->raise();
  }
}

std::vector<SidebarPanel*> SidebarManager::panels() const
{
  auto result = std::vector<SidebarPanel*>{};
  for (const auto& entry : m_entries)
  {
    if (entry.panel)
    {
      result.push_back(entry.panel.data());
    }
  }
  return result;
}

std::vector<QDockWidget*> SidebarManager::docks() const
{
  auto result = std::vector<QDockWidget*>{};
  for (const auto& entry : m_entries)
  {
    if (entry.dock)
    {
      result.push_back(entry.dock.data());
    }
  }
  return result;
}

void SidebarManager::redockOffScreenDocks()
{
  const auto screens = QGuiApplication::screens();
  for (auto* dock : docks())
  {
    if (dock->isFloating())
    {
      const auto frame = dock->frameGeometry();
      const auto onScreen = std::ranges::any_of(screens, [&](const auto* screen) {
        return screen->availableGeometry().intersects(frame);
      });
      if (!onScreen)
      {
        dock->setFloating(false);
      }
    }
  }
}

QDockWidget* SidebarManager::findDock(const QString& panelId) const
{
  const auto it = std::ranges::find_if(
    m_entries, [&](const auto& entry) { return entry.panelId == panelId; });
  return it != m_entries.end() ? it->dock.data() : nullptr;
}

} // namespace tb::ui
