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

#include "ui/LitPreviewWindow.h"

#include <QEvent>
#include <QExposeEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QScreen>

#include "LitSessionHost.h"

namespace tb::ui
{

LitPreviewWindow::LitPreviewWindow(LitSessionHost& host)
  : m_host{&host}
{
  setObjectName("LitPreviewWindow");
  // before the first show, which creates the platform window and its surface (R5.2)
  setSurfaceType(QSurface::VulkanSurface);
  setFlag(Qt::WindowDoesNotAcceptFocus);

  // a dock that floats onto another screen moves its window there; on a hybrid-GPU
  // laptop the new screen may belong to the other GPU (R5.33)
  connect(this, &QWindow::screenChanged, this, [this](QScreen*) {
    if (m_host)
    {
      m_host->screenChanged();
    }
  });
}

LitPreviewWindow::~LitPreviewWindow() = default;

void LitPreviewWindow::detach()
{
  m_host = nullptr;
}

bool LitPreviewWindow::event(QEvent* event)
{
  switch (event->type())
  {
  case QEvent::PlatformSurface:
    if (
      m_host
      && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
           == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
    {
      m_host->surfaceAboutToBeDestroyed();
    }
    break;
  case QEvent::UpdateRequest:
    if (m_host)
    {
      m_host->updateRequested();
    }
    return true;
  case QEvent::DevicePixelRatioChange:
    if (m_host)
    {
      m_host->windowChanged();
    }
    break;
  // D44: the preview takes no input, so that every key and click stays with the views
  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  case QEvent::MouseButtonDblClick:
  case QEvent::MouseMove:
  case QEvent::Wheel:
  case QEvent::KeyPress:
  case QEvent::KeyRelease:
  case QEvent::ShortcutOverride:
  case QEvent::TouchBegin:
  case QEvent::TouchUpdate:
  case QEvent::TouchEnd:
  case QEvent::TouchCancel:
  case QEvent::TabletPress:
  case QEvent::TabletMove:
  case QEvent::TabletRelease:
  case QEvent::NativeGesture:
  case QEvent::ContextMenu:
    event->ignore();
    return true;
  default:
    break;
  }
  return QWindow::event(event);
}

void LitPreviewWindow::exposeEvent(QExposeEvent* /* event */)
{
  if (m_host)
  {
    m_host->windowChanged();
    if (isExposed())
    {
      // the region may need drawing again; the frame is asked for on this thread
      // (R4-1.15)
      requestUpdate();
    }
  }
}

void LitPreviewWindow::resizeEvent(QResizeEvent* /* event */)
{
  if (m_host)
  {
    m_host->windowChanged();
  }
}

} // namespace tb::ui
