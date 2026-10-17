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

#include "ui/ToolSidebar.h"

#include <QAction>
#include <QSettings>
#include <QSizePolicy>
#include <QString>
#include <QWidget>

#include "ui/ImageUtils.h"
#include "ui/WidgetState.h"

#include "kd/contracts.h"

#include <algorithm>
#include <iterator>

namespace tb::ui
{
namespace
{

QString entryKey(const QAction& action)
{
  // no entry text has a literal ampersand, so every one marks a mnemonic
  return action.text().remove('&');
}

} // namespace

ToolSidebar::ToolSidebar(QWidget* parent)
  : QToolBar{parent}
{
  // first: the settings path of the expanded state is built from the object name
  setObjectName("MapWindow_ToolSidebar");
  setWindowTitle(tr("Tool Sidebar"));
  setAllowedAreas(Qt::LeftToolBarArea);
  // like the top toolbar: no handle, so the first button lines up under its first button
  setMovable(false);
  setFloatable(false);
  // the size of the top toolbar's icons, see MapWindow::createToolBar
  setIconSize(QSize{24, 24});
  setToolButtonStyle(Qt::ToolButtonIconOnly);

  // the layout gives the spare length of the strip to a widget that expands along it,
  // which puts the expander at the bottom while the strip is expanded
  auto* spacer = new QWidget{};
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_spacerAction = addWidget(spacer);

  // not checkable: a checked tool button is drawn pressed, and the strip is expanded
  // most of the time
  m_expandAction = new QAction{this};
  connect(
    m_expandAction, &QAction::triggered, this, [this]() { setExpanded(!expanded()); });
  addAction(m_expandAction);

  applyExpanded(QSettings{}.value(widgetSettingsPath(this, "Expanded"), true).toBool());
}

ToolSidebar::~ToolSidebar() = default;

QAction* ToolSidebar::expandAction() const
{
  return m_expandAction;
}

bool ToolSidebar::expanded() const
{
  return m_expanded;
}

void ToolSidebar::setExpanded(const bool expanded)
{
  if (expanded == m_expanded)
  {
    return;
  }

  applyExpanded(expanded);

  QSettings{}.setValue(widgetSettingsPath(this, "Expanded"), expanded);
  emit expandedChanged(expanded);
}

void ToolSidebar::addEntry(QAction* action)
{
  contract_pre(action != nullptr);
  contract_pre(
    std::ranges::none_of(m_entries, [&](const auto& entry) { return entry == action; }));

  dropDeadEntries();

  // before the first greater entry, so that an equal text goes after the existing ones
  const auto iNext = std::ranges::find_if(
    m_entries, [&](const auto& entry) { return toolSidebarEntryLess(*action, *entry); });
  const auto iEntry = m_entries.insert(iNext, action);

  if (m_expanded)
  {
    const auto iFollowing = std::next(iEntry);
    insertAction(
      iFollowing != m_entries.end() ? iFollowing->data() : m_spacerAction, action);
  }
}

std::vector<QAction*> ToolSidebar::entries() const
{
  auto result = std::vector<QAction*>{};
  for (const auto& entry : m_entries)
  {
    if (entry)
    {
      result.push_back(entry.data());
    }
  }
  return result;
}

void ToolSidebar::paintEvent(QPaintEvent* /* event */)
{
  // A lone left toolbar always spans the full height of the window, and both the system
  // style and Fusion would paint that column as a bordered panel. The strip is its
  // buttons and nothing else, so it paints nothing itself. This holds only while the
  // application's style sheet (base.qss, loaded in Main.cpp) has no background or border
  // rule for QToolBar: such a rule sets WA_StyledBackground, and Qt then paints the
  // panel as PE_Widget before paintEvent is called.
}

void ToolSidebar::applyExpanded(const bool expanded)
{
  m_expanded = expanded;

  // the entries are shared with menus, so the strip never hides them; it takes them out
  dropDeadEntries();
  for (const auto& entry : m_entries)
  {
    if (expanded)
    {
      insertAction(m_spacerAction, entry);
    }
    else
    {
      removeAction(entry);
    }
  }
  m_spacerAction->setVisible(expanded);

  if (expanded)
  {
    m_expandAction->setIcon(loadSVGIcon("ToolSidebarCollapse.svg"));
    m_expandAction->setText(tr("Collapse Tool Sidebar"));
  }
  else
  {
    m_expandAction->setIcon(loadSVGIcon("ToolSidebarExpand.svg"));
    m_expandAction->setText(tr("Expand Tool Sidebar"));
  }
}

void ToolSidebar::dropDeadEntries()
{
  std::erase_if(m_entries, [](const auto& entry) { return entry.isNull(); });
}

bool toolSidebarEntryLess(const QAction& lhs, const QAction& rhs)
{
  return QString::localeAwareCompare(entryKey(lhs), entryKey(rhs)) < 0;
}

} // namespace tb::ui
