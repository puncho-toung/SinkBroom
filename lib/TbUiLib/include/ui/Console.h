/*
 Copyright (C) 2010 Kristian Duske

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

#include <QMutex>
#include <QString>

#include "base/Logger.h"
#include "base/LoggerCache.h"
#include "ui/SidebarPanel.h"

#include <filesystem>
#include <string_view>

class QTextEdit;
class QTimer;
class QWidget;

namespace tb::ui
{
class Console : public SidebarPanel, public Logger
{
  Q_OBJECT
public:
  static constexpr auto PanelId = "Console";

private:
  QTextEdit* m_textView = nullptr;
  QTimer* m_timer = nullptr;

  LoggerCache m_cache;
  QMutex m_cacheMutex;

public:
  explicit Console(QWidget* parent = nullptr);

  QString panelId() const override;
  QString title() const override;
  std::filesystem::path iconPath() const override;
  bool shownByDefault() const override;
  Qt::DockWidgetArea defaultArea() const override;

private:
  void doLog(LogLevel level, std::string_view message) override;
  void logToDebugOut(LogLevel level, const std::string& message);
  void logToConsole(LogLevel level, const std::string& message);

  void logCachedMessages();

  void showContextMenu(const QPoint& pos);
};

} // namespace tb::ui
