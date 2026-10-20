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

#include "ui/MapWindowManager.h"

#include <QApplication>

#include "gl/GlManager.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"

#include "kd/contracts.h"
#include "kd/ranges/contains.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <memory>
#include <system_error>

namespace tb::ui
{
namespace
{

// the form of a path that findMapWindow takes
std::filesystem::path absoluteNormalPath(const std::filesystem::path& path)
{
  auto error = std::error_code{};
  return (path.is_absolute() ? path : std::filesystem::absolute(path, error))
    .lexically_normal();
}

} // namespace

MapWindowManager::MapWindowManager(
  AppController& appController, const bool singleMapWindow)
  : QObject{&appController}
  , m_appController{appController}
  , m_singleMapWindow{singleMapWindow}
{
  connect(qApp, &QApplication::focusChanged, this, &MapWindowManager::onFocusChange);
}

MapWindowManager::~MapWindowManager() = default;

std::vector<MapWindow*> MapWindowManager::mapWindows() const
{
  return m_mapWindows;
}

MapWindow* MapWindowManager::topMapWindow() const
{
  return m_mapWindows.empty() ? nullptr : m_mapWindows.front();
}

void MapWindowManager::setSingleMapWindowForTesting(const bool singleMapWindow)
{
  m_singleMapWindow = singleMapWindow;
}

void MapWindowManager::setShowMapWindowForTesting(ShowMapWindow showMapWindow)
{
  m_showMapWindow = std::move(showMapWindow);
}

Result<void> MapWindowManager::createDocument(
  const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat, const vm::bbox3d& worldBounds)
{
  if (shouldCreateWindowForDocument())
  {
    return MapDocument::createDocument(
             m_appController.environmentConfig(),
             gameInfo,
             mapFormat,
             worldBounds,
             m_appController.taskManager(),
             m_appController.glManager().resourceManager())
           | kdl::transform([&](auto document) { createMapWindow(std::move(document)); });
  }

  auto* mapWindow = documentWindow();
  contract_assert(mapWindow != nullptr);
  if (!prepareDocumentWindow(*mapWindow))
  {
    return Result<void>{};
  }

  return mapWindow->document().create(
    m_appController.environmentConfig(), gameInfo, mapFormat, worldBounds);
}

Result<void> MapWindowManager::loadDocument(
  const mdl::GameInfo& gameInfo,
  mdl::MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  std::filesystem::path path)
{
  // R8-app-4: a file that another window has open, a prefab editor too, is not opened
  // twice; that window comes to the front. The single window opening its own file again
  // reloads it after its unsaved-changes prompt, as it always did (H-F8-6).
  if (auto* mapWindow = findMapWindow(absoluteNormalPath(path));
      mapWindow && (shouldCreateWindowForDocument() || mapWindow != documentWindow()))
  {
    moveToTop(*mapWindow);
    showMapWindow(*mapWindow);
    return Result<void>{};
  }

  if (shouldCreateWindowForDocument())
  {
    return MapDocument::loadDocument(
             m_appController.environmentConfig(),
             gameInfo,
             mapFormat,
             worldBounds,
             std::move(path),
             m_appController.taskManager(),
             m_appController.glManager().resourceManager())
           | kdl::transform([&](auto document) { createMapWindow(std::move(document)); });
  }

  auto* mapWindow = documentWindow();
  contract_assert(mapWindow != nullptr);
  if (!prepareDocumentWindow(*mapWindow))
  {
    return Result<void>{};
  }

  return mapWindow->document().load(
    m_appController.environmentConfig(),
    gameInfo,
    mapFormat,
    worldBounds,
    std::move(path));
}

Result<void> MapWindowManager::openPrefabEditor(
  MapWindow& hostWindow, const std::filesystem::path& path)
{
  // R1-pf3-3: the panel posts the request, which may run after the window's close
  if (!kdl::ranges::contains(m_mapWindows, &hostWindow))
  {
    return Error{"The window that the prefab was opened from is closed"};
  }

  auto absolutePath = absoluteNormalPath(path);

  // R1-pf3-12: a prefab open as a map is not opened twice either
  if (auto* mapWindow = findMapWindow(absolutePath))
  {
    moveToTop(*mapWindow);
    showMapWindow(*mapWindow);
    return Result<void>{};
  }

  // the prefab library reads the files with the document's game, format and world bounds
  // (R1-pf3-8), and the file is saved from a map of that game (C2), so the game is not
  // asked for
  const auto& hostMap = hostWindow.document().map();
  return MapDocument::loadDocument(
           m_appController.environmentConfig(),
           hostMap.gameInfo(),
           hostMap.worldNode().mapFormat(),
           hostMap.worldBounds(),
           std::move(absolutePath),
           m_appController.taskManager(),
           m_appController.glManager().resourceManager())
         | kdl::transform([&](auto document) {
             createMapWindow(std::move(document), true, &hostWindow);
           });
}

void MapWindowManager::prefabWasSaved(const std::filesystem::path& path)
{
  // R1-pf3-9: not only the window that opened the editor, which may have switched its
  // game or been closed, or be another editor; each library filters by its roots.
  // R2-pf3-3: the editor too, so that its own panel takes the save for its own write and
  // does not rescan its library when its folder watcher reports it; a copy of the list,
  // which a window that the notification closes would change
  const auto mapWindows = m_mapWindows;
  for (auto* mapWindow : mapWindows)
  {
    mapWindow->prefabWasSaved(path);
  }
}

Result<void> MapWindowManager::checkNotOpen(const std::filesystem::path& path)
{
  if (auto* mapWindow = findMapWindow(absoluteNormalPath(path)))
  {
    moveToTop(*mapWindow);
    showMapWindow(*mapWindow);
    return Error{
      fmt::format("{} is open in a window; save or close it there first", path)};
  }
  return Result<void>{};
}

bool MapWindowManager::allMapWindowsClosed() const
{
  return m_mapWindows.empty();
}

void MapWindowManager::onFocusChange(QWidget* /* old */, QWidget* now)
{
  if (now)
  {
    // The QApplication::focusChanged signal also notifies us of focus changes between
    // child widgets, so get the top-level widget with QWidget::window()
    if (auto* mapWindow = dynamic_cast<MapWindow*>(now->window()))
    {
      if (auto it = std::ranges::find(m_mapWindows, mapWindow);
          it != m_mapWindows.end() && it != m_mapWindows.begin())
      {
        std::rotate(m_mapWindows.begin(), it, std::next(it));
      }
    }
  }
}

MapWindow* MapWindowManager::documentWindow() const
{
  const auto it = std::ranges::find_if(
    m_mapWindows, [](const auto* mapWindow) { return !mapWindow->isPrefabEditor(); });
  return it != m_mapWindows.end() ? *it : nullptr;
}

MapWindow* MapWindowManager::findMapWindow(const std::filesystem::path& path) const
{
  const auto it = std::ranges::find_if(m_mapWindows, [&](const auto* mapWindow) {
    const auto& mapPath = mapWindow->document().map().path();
    if (!mapPath.is_absolute())
    {
      // a map without a file, whose path is its name
      return false;
    }

    auto error = std::error_code{};
    // equivalent also matches another spelling of the path, such as its case on Windows
    return mapPath.lexically_normal() == path
           || std::filesystem::equivalent(mapPath, path, error);
  });
  return it != m_mapWindows.end() ? *it : nullptr;
}

bool MapWindowManager::prepareDocumentWindow(MapWindow& mapWindow)
{
  // R1-pf3-4: with a prefab editor on top, the window whose document is replaced is
  // behind it, so it asks about its changes and comes to the front
  if (&mapWindow == topMapWindow())
  {
    return true;
  }

  if (!mapWindow.confirmOrDiscardChanges())
  {
    return false;
  }
  moveToTop(mapWindow);
  showMapWindow(mapWindow);
  return true;
}

bool MapWindowManager::shouldCreateWindowForDocument() const
{
  // prefab editors do not count: New and Open never replace their documents
  return !m_singleMapWindow || documentWindow() == nullptr;
}

MapWindow* MapWindowManager::createMapWindow(
  std::unique_ptr<MapDocument> document, const bool prefabEditor, QWidget* reference)
{
  contract_pre(document != nullptr);

  auto* mapWindow = new MapWindow{m_appController, std::move(document), prefabEditor};
  mapWindow->positionOnScreen(reference ? reference : topMapWindow());
  m_mapWindows.insert(m_mapWindows.begin(), mapWindow);

  showMapWindow(*mapWindow);
  return mapWindow;
}

void MapWindowManager::showMapWindow(MapWindow& mapWindow)
{
  if (m_showMapWindow)
  {
    m_showMapWindow(mapWindow);
    return;
  }

  // R1-pf3-5: a minimised window is restored, as it was before
  mapWindow.setWindowState(mapWindow.windowState() & ~Qt::WindowMinimized);
  mapWindow.show();
  mapWindow.raise();
  mapWindow.activateWindow();
}

void MapWindowManager::moveToTop(MapWindow& mapWindow)
{
  // as a focus change does, which may come later or not at all
  if (auto it = std::ranges::find(m_mapWindows, &mapWindow);
      it != m_mapWindows.end() && it != m_mapWindows.begin())
  {
    std::rotate(m_mapWindows.begin(), it, std::next(it));
  }
}

void MapWindowManager::removeMapWindow(MapWindow* mapWindow)
{
  // This is called from MapWindow::closeEvent
  if (auto it = std::ranges::find(m_mapWindows, mapWindow); it != m_mapWindows.end())
  {
    m_mapWindows.erase(it);
    // MapWindow uses Qt::WA_DeleteOnClose so we don't need to delete it here
  }
}

} // namespace tb::ui
