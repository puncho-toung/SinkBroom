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

#include "ui/LightEditorPanel.h"

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Light.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "ui/MapDocument.h"
#include "ui/SignalDelayer.h"

#include <algorithm>

namespace tb::ui
{
namespace
{

enum class PanelState
{
  NoLightEditor,
  NoLightSelected,
  NotOnlyLightsSelected,
  OnlyLightsSelected,
};

PanelState panelState(const mdl::Map& map)
{
  if (!map.gameInfo().gameConfig.entityConfig.lightEditor)
  {
    return PanelState::NoLightEditor;
  }

  // the selected entity nodes only: allEntities() also yields worldspawn for a selected
  // brush and the parent entity of a selected brush or patch
  const auto& selection = map.selection();
  const auto isLight = [](const auto* entityNode) {
    return mdl::isLightEntity(entityNode->entity());
  };

  if (selection.hasOnlyEntities() && std::ranges::all_of(selection.entities, isLight))
  {
    return PanelState::OnlyLightsSelected;
  }
  if (std::ranges::any_of(selection.entities, isLight))
  {
    return PanelState::NotOnlyLightsSelected;
  }
  return PanelState::NoLightSelected;
}

QWidget* createMessagePage(QLabel* label)
{
  label->setAlignment(Qt::AlignCenter);
  label->setWordWrap(true);

  auto* layout = new QVBoxLayout{};
  layout->addWidget(label, 1);

  auto* page = new QWidget{};
  page->setLayout(layout);
  return page;
}

} // namespace

LightEditorPanel::LightEditorPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_document{context.document}
  , m_updateSignalDelayer{new SignalDelayer{this}}
{
  createGui();
  connectObservers();
  updateControls();
}

LightEditorPanel::~LightEditorPanel() = default;

QString LightEditorPanel::panelId() const
{
  return "LightEditor";
}

QString LightEditorPanel::title() const
{
  return tr("Light Editor");
}

void LightEditorPanel::panelShown()
{
  refresh();
}

void LightEditorPanel::createGui()
{
  m_messageLabel = new QLabel{};
  m_messageLabel->setObjectName("LightEditorPanel_Message");

  // replaced by the editor in WP-LW3
  auto* editorLabel = new QLabel{tr("Light selected (editor comes with LW3)")};

  m_stack = new QStackedWidget{};
  m_stack->setObjectName("LightEditorPanel_Stack");
  m_stack->addWidget(createMessagePage(m_messageLabel));
  m_stack->addWidget(createMessagePage(editorLabel));

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_stack);
  setLayout(layout);
}

void LightEditorPanel::connectObservers()
{
  connect(
    m_updateSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &LightEditorPanel::updateControls);

  // the document outlives the map it holds: on load the old map is freed before these
  // notifications run, so they hold nothing of it and every update reads the map anew
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect([&] { refresh(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&] { refresh(); });
  m_notifierConnection +=
    m_document.selectionDidChangeNotifier.connect([&](const auto&) { refresh(); });
  m_notifierConnection +=
    m_document.entityDefinitionsDidChangeNotifier.connect([&] { refresh(); });
}

void LightEditorPanel::refresh()
{
  m_updateSignalDelayer->queueSignal();
}

void LightEditorPanel::updateControls()
{
  switch (panelState(m_document.map()))
  {
  case PanelState::NoLightEditor:
    m_messageLabel->setText(tr("This game has no light editor"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::NoLightSelected:
    m_messageLabel->setText(tr("Select a light"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::NotOnlyLightsSelected:
    m_messageLabel->setText(tr("Select only lights"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::OnlyLightsSelected:
    m_stack->setCurrentIndex(1);
    break;
  }
}

} // namespace tb::ui
