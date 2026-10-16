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

#include <QString>
#include <QWidget>

#include "mdl/MapFixture.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapViewToolBox.h"
#include "ui/SidebarPanel.h"

class QDockWidget;

namespace tb::ui
{
class LightEditorPanel;
class MapDocument;
class SidebarManager;

/**
 * A sidebar panel that counts the calls of its hooks. Test sources are not moc'd, so it
 * declares no Q_OBJECT.
 */
class StubPanel : public SidebarPanel
{
private:
  QString m_panelId;

public:
  int shownCount = 0;
  int hiddenCount = 0;

  explicit StubPanel(QString panelId);

  QString panelId() const override;
  QString title() const override;
  void panelShown() override;
  void panelHidden() override;
};

struct RegisteredStub
{
  QDockWidget* dock;
  StubPanel* panel;
};

/**
 * Registers a stub panel with the given id; its title is the id followed by " title".
 */
RegisteredStub registerStubPanel(SidebarManager& manager, const QString& panelId);

/**
 * A document of the given game with a tool box, as a map window provides them.
 */
class SidebarContextFixture
{
private:
  AppControllerFixture m_appControllerFixture;
  MapDocumentFixture m_documentFixture;
  MapDocument& m_document;
  QWidget m_toolPages;
  MapViewToolBox m_toolBox;

public:
  explicit SidebarContextFixture(mdl::MapFixtureConfig config = {});
  ~SidebarContextFixture();

  MapDocument& document();
  SidebarContext context();
};

/**
 * The text of the light editor panel's message on page 0, or of the editor's header on
 * page 1.
 */
QString currentText(const LightEditorPanel& panel);

} // namespace tb::ui
