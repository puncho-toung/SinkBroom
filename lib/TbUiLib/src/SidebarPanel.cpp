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

#include "ui/SidebarPanel.h"

#include <QCloseEvent>

namespace tb::ui
{

SidebarPanel::SidebarPanel(QWidget* parent)
  : QWidget{parent}
{
}

SidebarPanel::~SidebarPanel() = default;

void SidebarPanel::panelShown() {}

void SidebarPanel::panelHidden() {}

SidebarDock::SidebarDock(const QString& title, QWidget* parent)
  : QDockWidget{title, parent}
{
}

SidebarDock::~SidebarDock() = default;

void SidebarDock::closeEvent(QCloseEvent* event)
{
  QDockWidget::closeEvent(event);

  // visibilityChanged(false) also fires when another dock is tabbed over this one and
  // around floating and docking, so only an accepted close means the user closed it
  if (event->isAccepted())
  {
    if (auto* panel = qobject_cast<SidebarPanel*>(widget()))
    {
      panel->panelHidden();
    }
  }
}

} // namespace tb::ui
