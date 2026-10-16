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
#include <functional>
#include <memory>
#include <string>

class QLabel;
class QStackedWidget;

namespace tb::mdl
{
class Entity;
struct LightProperties;
} // namespace tb::mdl

namespace tb::ui
{
class MapDocument;
class SignalDelayer;

/**
 * The sidebar panel that edits id Tech 4 lights. It applies to games whose config sets
 * entities.lightEditor, and edits the selection only if it consists of light entities.
 *
 * Page 0 tells the user what to select; page 1 is the editor. Every control writes when
 * the user changes it, as one undoable command for all selected lights, and only the
 * properties that its change alters on each light; a field in which the selected lights
 * differ shows "multi". The controls are set from the map once the event loop has
 * processed the notifications of a change. The header names the light and shows what
 * the engine reads differently from what the editor shows, with a fix where there is
 * one; a light with the legacy light_origin is read-only until it is converted.
 */
class LightEditorPanel : public SidebarPanel
{
  Q_OBJECT
private:
  struct Controls;

  MapDocument& m_document;

  QStackedWidget* m_stack = nullptr;
  QLabel* m_messageLabel = nullptr;
  std::unique_ptr<Controls> m_controls;

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
  QWidget* createEditorPage();
  void connectEditor();
  void connectObservers();

  void refresh();
  void updateControls();
  void updateEditor();

  void editLights(
    const std::string& commandName,
    const std::function<void(mdl::LightProperties&)>& edit);
  void editAngles(std::size_t component, double value);
  void removeTargetProperties(
    const std::string& commandName,
    const std::function<bool(const mdl::Entity&, const mdl::LightProperties&)>& applies);
  void convertLightOrigins();
};

} // namespace tb::ui
