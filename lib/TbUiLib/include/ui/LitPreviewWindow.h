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

#include <QWindow>

class QEvent;
class QExposeEvent;
class QResizeEvent;

namespace tb::ui
{
class LitSessionHost;

/**
 * The lit preview's Vulkan surface (§3.1). A new one is made for every session and lives
 * in a window container on the panel's surface page.
 *
 * It never takes the focus and ignores all input, so the 3D view keeps the keyboard; it
 * does not use Qt::WindowTransparentForInput, which makes the native child a layered
 * window that breaks presentation (D44). Its expose, resize, DPR, screen and surface
 * events go to the session host, which teardown detaches first; detached, the window does
 * nothing.
 */
class LitPreviewWindow : public QWindow
{
  Q_OBJECT
private:
  LitSessionHost* m_host = nullptr;

public:
  explicit LitPreviewWindow(LitSessionHost& host);
  ~LitPreviewWindow() override;

  // Teardown calls this before its first step; nothing reaches the host afterwards.
  void detach();

protected:
  bool event(QEvent* event) override;
  void exposeEvent(QExposeEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
};

} // namespace tb::ui
