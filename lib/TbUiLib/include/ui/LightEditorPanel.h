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

class QLabel;
class QStackedWidget;

namespace tb::ui
{
class MapDocument;
class SignalDelayer;

/**
 * The sidebar panel that edits id Tech 4 lights. It applies to games whose config sets
 * entities.lightEditor, and edits the selection only if it consists of light entities.
 *
 * Page 0 tells the user what to select; page 1 is the editor.
 */
class LightEditorPanel : public SidebarPanel
{
  Q_OBJECT
private:
  MapDocument& m_document;

  QStackedWidget* m_stack = nullptr;
  QLabel* m_messageLabel = nullptr;

  SignalDelayer* m_updateSignalDelayer = nullptr;

  NotifierConnection m_notifierConnection;

public:
  explicit LightEditorPanel(const SidebarContext& context, QWidget* parent = nullptr);
  ~LightEditorPanel() override;

  QString panelId() const override;
  QString title() const override;

  void panelShown() override;

private:
  void createGui();
  void connectObservers();

  void refresh();
  void updateControls();
};

} // namespace tb::ui
