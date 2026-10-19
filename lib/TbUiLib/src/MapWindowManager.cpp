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

#include <algorithm>
#include <memory>
#include <system_error>

namespace tb::ui
{

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

  return mapWindow->document().create(
    m_appController.environmentConfig(), gameInfo, mapFormat, worldBounds);
}

Result<void> MapWindowManager::loadDocument(
  const mdl::GameInfo& gameInfo,
  mdl::MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  std::filesystem::path path)
{
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
  auto error = std::error_code{};
  auto absolutePath = (path.is_absolute() ? path : std::filesystem::absolute(path, error))
                        .lexically_normal();

  if (auto* prefabEditor = findPrefabEditor(absolutePath))
  {
    moveToTop(*prefabEditor);
    showMapWindow(*prefabEditor);
    return Result<void>{};
  }

  // the prefab library reads the files with the document's game and format, and the file
  // is saved from a map of that game (C2), so the game is not asked for
  const auto& hostMap = hostWindow.document().map();
  const auto& gameInfo = hostMap.gameInfo();
  return MapDocument::loadDocument(
           m_appController.environmentConfig(),
           gameInfo,
           hostMap.worldNode().mapFormat(),
           MapDocument::worldBoundsForGame(gameInfo.gameConfig),
           std::move(absolutePath),
           m_appController.taskManager(),
           m_appController.glManager().resourceManager())
         | kdl::transform([&](auto document) {
             auto* prefabEditor = createMapWindow(std::move(document), true, &hostWindow);
             m_prefabEditors.push_back({.editor = prefabEditor, .host = &hostWindow});
           });
}

void MapWindowManager::prefabWasSaved(
  const MapWindow& prefabEditor, const std::filesystem::path& path)
{
  const auto it = std::ranges::find_if(
    m_prefabEditors, [&](const auto& entry) { return entry.editor == &prefabEditor; });
  if (it != m_prefabEditors.end() && it->host)
  {
    it->host->prefabWasSaved(path);
    return;
  }

  // the window that opened the editor was closed, so any window may show the prefab
  for (auto* mapWindow : m_mapWindows)
  {
    if (mapWindow != &prefabEditor)
    {
      mapWindow->prefabWasSaved(path);
    }
  }
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

MapWindow* MapWindowManager::findPrefabEditor(const std::filesystem::path& path) const
{
  const auto it = std::ranges::find_if(m_prefabEditors, [&](const auto& entry) {
    const auto& editorPath = entry.editor->document().map().path();
    auto error = std::error_code{};
    // equivalent also matches another spelling of the path, such as its case on Windows
    return editorPath.lexically_normal() == path
           || std::filesystem::equivalent(editorPath, path, error);
  });
  return it != m_prefabEditors.end() ? it->editor : nullptr;
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

  // a closed editor is forgotten; the editors of a closed window tell every window
  std::erase_if(
    m_prefabEditors, [&](const auto& entry) { return entry.editor == mapWindow; });
  for (auto& entry : m_prefabEditors)
  {
    if (entry.host == mapWindow)
    {
      entry.host = nullptr;
    }
  }
}

} // namespace tb::ui
