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

#include <QPointer>
#include <QToolBar>

#include <vector>

class QAction;
class QPaintEvent;

namespace tb::ui
{

/**
 * A strip of icon buttons at the left edge of the map window, one for each window that
 * the editor can open from it, sorted by their texts. Its last button, the expander,
 * collapses the strip to that one button and expands it again; the state is saved.
 *
 * The entries are shared actions (a dock's toggle action is also in View > Sidebar), so
 * the strip never changes their visibility: collapsing removes them from the strip and
 * expanding inserts them again. The strip owns only its spacer and its expander.
 */
class ToolSidebar : public QToolBar
{
  Q_OBJECT
private:
  std::vector<QPointer<QAction>> m_entries;
  QAction* m_spacerAction = nullptr;
  QAction* m_expandAction = nullptr;
  bool m_expanded = false;

public:
  explicit ToolSidebar(QWidget* parent = nullptr);
  ~ToolSidebar() override;

  /**
   * The strip's last action; not checkable. Its icon and text follow expanded().
   */
  QAction* expandAction() const;

  bool expanded() const;

  /**
   * Inserts the entries before the spacer and shows the spacer, or removes the entries
   * and hides the spacer; saves the state and emits expandedChanged. Does nothing if the
   * state does not change.
   */
  void setExpanded(bool expanded);

  /**
   * Adds the given action at its place in the order of toolSidebarEntryLess, after the
   * entries that compare equal to it. The action is in the strip only while it is
   * expanded. The action must not be an entry already.
   */
  void addEntry(QAction* action);

  /**
   * The entries that still exist, in sorted order.
   */
  std::vector<QAction*> entries() const;

signals:
  void expandedChanged(bool expanded);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void applyExpanded(bool expanded);
  void dropDeadEntries();
};

/**
 * The order of the tool sidebar's entries and of View > Sidebar: the actions' texts
 * without mnemonic ampersands, compared with QString::localeAwareCompare.
 */
bool toolSidebarEntryLess(const QAction& lhs, const QAction& rhs);

} // namespace tb::ui
