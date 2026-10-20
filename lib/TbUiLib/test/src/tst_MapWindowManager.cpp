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

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDockWidget>
#include <QEvent>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QSize>
#include <QStatusBar>
#include <QStringList>
#include <QTest>
#include <QToolButton>
#include <QVariant>

#include "base/Preference.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "mdl/ContentHash.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/WorldNode.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/CreatePrefabTool.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/PrefabBrowserView.h"
#include "ui/PrefabPanel.h"
#include "ui/QPathUtils.h"
#include "ui/RenderView.h"
#include "ui/SavePrefabDialog.h"
#include "ui/ToolSidebar.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/**
 * Restores the given settings groups as they were when it was made. Closing a map window
 * saves its layout, and loading a file into one adds the file to the recent documents.
 */
class SettingsSnapshot
{
private:
  QStringList m_groups;
  std::vector<std::pair<QString, QVariant>> m_values;

public:
  explicit SettingsSnapshot(QStringList groups)
    : m_groups{std::move(groups)}
  {
    auto settings = QSettings{};
    for (const auto& group : m_groups)
    {
      settings.beginGroup(group);
      for (const auto& key : settings.allKeys())
      {
        m_values.emplace_back(group + "/" + key, settings.value(key));
      }
      settings.endGroup();
    }
  }

  ~SettingsSnapshot()
  {
    auto settings = QSettings{};
    for (const auto& group : m_groups)
    {
      settings.remove(group);
    }
    for (const auto& [key, value] : m_values)
    {
      settings.setValue(key, value);
    }
  }
};

void deleteClosedWindows()
{
  // the windows delete themselves when they are closed
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

QImage redImage(const int width, const int height)
{
  auto image = QImage{width, height, QImage::Format_RGBA8888};
  image.fill(Qt::red);
  return image;
}

/**
 * Runs what a save posted: a prefab editor writes its thumbnail and tells the other
 * windows once the event loop runs (R1-pf3-10).
 */
void finishSave()
{
  QCoreApplication::processEvents();
}

/**
 * Counts a change of the given window's document, so that its next save writes the
 * prefab's thumbnail (DD-38).
 */
void modify(MapWindow& mapWindow)
{
  mapWindow.document().map().incModificationCount();
}

/**
 * The message of the given result's error, or an empty string.
 */
std::string errorMessage(Result<void> result)
{
  auto message = std::string{};
  std::move(result) | kdl::transform_error([&](const auto& e) { message = e.msg; });
  return message;
}

/**
 * The saved state of the windows and their widgets, but for a prefab editor's own.
 */
std::map<QString, QVariant> mapWindowSettings()
{
  auto settings = QSettings{};
  settings.beginGroup("Windows");
  auto result = std::map<QString, QVariant>{};
  for (const auto& key : settings.allKeys())
  {
    if (!key.startsWith("PrefabEditorWindow/"))
    {
      result.emplace(key, settings.value(key));
    }
  }
  return result;
}

/**
 * Shows the given window with the dock of its prefab panel in front, but none of its GL
 * views, which would need GL resources that the tests do not provide, so that the panel
 * is visible and watches the library's folders.
 */
void showPrefabPanel(MapWindow& mapWindow, PrefabPanel& panel)
{
  for (auto* view : mapWindow.findChildren<RenderView*>())
  {
    view->hide();
  }
  mapWindow.show();

  auto* parent = panel.parentWidget();
  while (parent && !qobject_cast<QDockWidget*>(parent))
  {
    parent = parent->parentWidget();
  }
  auto* dock = qobject_cast<QDockWidget*>(parent);
  REQUIRE(dock != nullptr);
  dock->show();
  dock->raise();
  QApplication::processEvents();
  REQUIRE(panel.isVisible());
}

/**
 * Scans the given window's prefab library, unless its panel has, and waits for the scan.
 */
void scanLibrary(MapWindow& mapWindow)
{
  auto* panel = mapWindow.findChild<PrefabPanel*>();
  REQUIRE(panel != nullptr);
  panel->scanOnce();
  const auto& library = mapWindow.document().map().prefabLibrary();
  REQUIRE(QTest::qWaitFor([&] { return !library.scanning(); }));
}

bool hasThumbnail(
  const MapWindow& mapWindow,
  const std::string& relativePath,
  const std::filesystem::path& thumbnailPath)
{
  const auto& library = mapWindow.document().map().prefabLibrary();
  const auto* entry = library.find(mdl::PrefabRoot::User, relativePath);
  return entry && entry->thumbnailPath == thumbnailPath && !library.scanning();
}

} // namespace

TEST_CASE("MapWindowManager")
{
  const auto settingsSnapshot = SettingsSnapshot{{"Windows", "RecentDocuments"}};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto& manager = appController.mapWindowManager();

  // the single map window of Windows (NR D23), on every platform
  manager.setSingleMapWindowForTesting(true);
  // the windows' GL views would need GL resources that the tests do not provide
  manager.setShowMapWindowForTesting([](MapWindow&) {});

  auto env = fs::TestEnvironment{};
  const auto userRoot = env.dir() / "prefabs";

  // a preference of its own, which no other test sets
  auto gameInfo = mdl::detail::makeGameInfoFixture(mdl::DefaultGameInfo.gameConfig, {});
  gameInfo.prefabPathPreference = Preference<std::filesystem::path>{
    "Games/Test/Prefab Path (MapWindowManager)", userRoot};

  const auto mapFormat = mdl::MapFormat::Standard;
  const auto worldBounds = vm::bbox3d{8192.0};

  // a prefab, saved from a map that no window shows, so that closing the windows asks
  // nothing
  auto sourceDocument = MapDocument::createDocument(
                          appController.environmentConfig(),
                          gameInfo,
                          mapFormat,
                          worldBounds,
                          appController.taskManager(),
                          appController.glManager().resourceManager())
                        | kdl::value();
  auto& sourceMap = sourceDocument->map();
  auto* entityNode =
    new mdl::EntityNode{mdl::Entity{{{"classname", "light"}, {"name", "lamp"}}}};
  REQUIRE(
    !mdl::addNodes(sourceMap, {{&mdl::parentForNodes(sourceMap), {entityNode}}}).empty());
  const auto prefabPath =
    mdl::savePrefab(
      sourceMap,
      {entityNode},
      mdl::SavePrefabOptions{
        .name = "lamp", .category = "lights", .root = mdl::PrefabRoot::User})
    | kdl::value();
  REQUIRE(prefabPath == userRoot / "lights" / "lamp.map");

  // the windows go before the game info that their maps refer to
  const auto closeWindows = kdl::invoke_later{[&] {
    for (auto* mapWindow : manager.mapWindows())
    {
      // R2-pf3-5: a check that failed after an edit must not leave a modal prompt; the
      // count steps back to the last save's, since Map::clearModificationCount is private
      auto& map = mapWindow->document().map();
      while (map.modified() && map.modificationCount() > 0)
      {
        map.decModificationCount();
      }
      mapWindow->close();
    }
    deleteClosedWindows();
  }};

  REQUIRE(manager.createDocument(gameInfo, mapFormat, worldBounds) | kdl::is_success());
  REQUIRE(manager.mapWindows().size() == 1u);
  auto host = QPointer<MapWindow>{manager.topMapWindow()};
  REQUIRE(host->document().map().prefabLibrary().userRoot() == userRoot);

  SECTION("openPrefabEditor opens a window of its own beside the host")
  {
    const auto& hostMap = host->document().map();

    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    REQUIRE(manager.mapWindows().size() == 2u);

    auto* editor = manager.topMapWindow();
    CHECK(editor != host);
    CHECK(editor->isPrefabEditor());
    CHECK(editor->document().map().path() == prefabPath);
    CHECK(editor->windowTitle() == "lamp.map[*] [Prefab] - TrenchBroom");

    // the host keeps its document
    CHECK(!host->isPrefabEditor());
    CHECK(&host->document().map() == &hostMap);
    CHECK(!hostMap.persistent());

    // the prefab file's keys are the worldspawn's (C2)
    const auto& worldEntity = editor->document().map().worldNode().entity();
    REQUIRE(worldEntity.property(mdl::PrefabPropertyKeys::Name) != nullptr);
    CHECK(*worldEntity.property(mdl::PrefabPropertyKeys::Name) == "lamp");
  }

  SECTION("a prefab editor cannot place prefabs (DD-28, DD-29)")
  {
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());

    const auto cellsOf = [](MapWindow& mapWindow) {
      auto* panel = mapWindow.findChild<PrefabPanel*>();
      REQUIRE(panel != nullptr);
      const auto& library = mapWindow.document().map().prefabLibrary();
      panel->scanOnce();
      REQUIRE(QTest::qWaitFor([&] { return !library.scanning(); }));
      auto* view = panel->findChild<PrefabBrowserView*>();
      REQUIRE(view != nullptr);
      return view->cellSummaries();
    };

    const auto placementOff = QString{"Placement is off in a prefab editor"};

    const auto hostCells = cellsOf(*host);
    REQUIRE(hostCells.size() == 1u);
    CHECK(hostCells[0].dndData == "prefab:user/lights/lamp.map");
    CHECK(!hostCells[0].tooltip.contains(placementOff));

    const auto editorCells = cellsOf(*editor);
    REQUIRE(editorCells.size() == 1u);
    CHECK(editorCells[0].dndData.isEmpty());
    CHECK(editorCells[0].tooltip.contains(placementOff));

    // a drag out of the host's panel into the editor's views is refused too
    CHECK(host->toolBox().createPrefabTool().placementEnabled());
    CHECK_FALSE(editor->toolBox().createPrefabTool().placementEnabled());

    // saving from the editor stays
    auto* saveSelectionAction =
      editor->findAction("Menu/Edit/Save Selection as Prefab...");
    REQUIRE(saveSelectionAction != nullptr);
    mdl::selectAllNodes(editor->document().map());
    CHECK(QTest::qWaitFor([&] { return saveSelectionAction->isEnabled(); }));
    auto* saveButton = editor->findChild<QToolButton*>("PrefabPanel_saveButton");
    REQUIRE(saveButton != nullptr);
    CHECK(saveButton->isEnabled());
  }

  SECTION("opening a prefab again brings its editor to the front")
  {
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());

    // the host gets the focus
    emit qApp->focusChanged(nullptr, host);
    REQUIRE(manager.topMapWindow() == host);

    // another spelling of the same file
    REQUIRE(
      manager.openPrefabEditor(*host, userRoot / "lights" / "." / "lamp.map")
      | kdl::is_success());
    CHECK(manager.mapWindows().size() == 2u);
    CHECK(manager.topMapWindow() == editor);

    // R1-pf3-17: another path of the same file, which only equivalent recognises
    const auto linkPath = userRoot / "lights" / "lamp-link.map";
    auto linkError = std::error_code{};
    std::filesystem::create_hard_link(prefabPath, linkPath, linkError);
    REQUIRE(!linkError);

    emit qApp->focusChanged(nullptr, host);
    REQUIRE(manager.topMapWindow() == host);
    REQUIRE(manager.openPrefabEditor(*host, linkPath) | kdl::is_success());
    CHECK(manager.mapWindows().size() == 2u);
    CHECK(manager.topMapWindow() == editor);
  }

  SECTION("a prefab open as a map is brought to the front, not opened again")
  {
    // R1-pf3-12
    REQUIRE(
      manager.loadDocument(gameInfo, mapFormat, worldBounds, prefabPath)
      | kdl::is_success());
    REQUIRE(manager.mapWindows().size() == 1u);
    REQUIRE(host->document().map().path() == prefabPath);

    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    CHECK(manager.mapWindows() == std::vector<MapWindow*>{host});
  }

  SECTION("Open of a file that a window has open brings that window to the front")
  {
    // R8-app-4: with a single map window or one per document
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());

    for (const auto singleMapWindow : {true, false})
    {
      CAPTURE(singleMapWindow);
      manager.setSingleMapWindowForTesting(singleMapWindow);
      emit qApp->focusChanged(nullptr, host);
      REQUIRE(manager.topMapWindow() == host);

      REQUIRE(
        manager.loadDocument(
          gameInfo, mapFormat, worldBounds, userRoot / "lights" / "." / "lamp.map")
        | kdl::is_success());
      CHECK(manager.mapWindows().size() == 2u);
      CHECK(manager.topMapWindow() == editor);
      CHECK(!host->document().map().persistent());
    }
  }

  SECTION("a file that a window has open is not overwritten or deleted")
  {
    // DD-39: the editor's next save would write it again
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto editor = QPointer<MapWindow>{manager.topMapWindow()};
    REQUIRE(editor->isPrefabEditor());

    scanLibrary(*host);
    auto* hostPanel = host->findChild<PrefabPanel*>();
    REQUIRE(hostPanel != nullptr);

    auto& hostMap = host->document().map();
    auto* lampNode =
      new mdl::EntityNode{mdl::Entity{{{"classname", "light"}, {"name", "lamp"}}}};
    REQUIRE(
      !mdl::addNodes(hostMap, {{&mdl::parentForNodes(hostMap), {lampNode}}}).empty());
    mdl::selectNodes(hostMap, {lampNode});

    const auto fileBefore = readFile(prefabPath);
    const auto refusal =
      fmt::format("{} is open in a window; save or close it there first", prefabPath);

    // Delete File...
    emit qApp->focusChanged(nullptr, host);
    REQUIRE(manager.topMapWindow() == host);
    CHECK(errorMessage(hostPanel->deletePrefabFile(prefabPath)) == refusal);
    CHECK(std::filesystem::exists(prefabPath));
    CHECK(manager.topMapWindow() == editor);

    // Update from Selection
    emit qApp->focusChanged(nullptr, host);
    REQUIRE(manager.topMapWindow() == host);
    CHECK(errorMessage(hostPanel->updatePrefabFromSelection(prefabPath)) == refusal);
    CHECK(manager.topMapWindow() == editor);

    // the save dialog's Replace
    emit qApp->focusChanged(nullptr, host);
    REQUIRE(manager.topMapWindow() == host);
    {
      auto dialog = SavePrefabDialog{host->document(), host};
      dialog.setOpenFileCheck(
        [&](const auto& path) { return manager.checkNotOpen(path); });
      auto* categoryChoice =
        dialog.findChild<QComboBox*>("SavePrefabDialog_categoryChoice");
      auto* messageLabel = dialog.findChild<QLabel*>("SavePrefabDialog_messageLabel");
      REQUIRE(categoryChoice != nullptr);
      REQUIRE(messageLabel != nullptr);
      categoryChoice->setCurrentText("lights");

      dialog.accept();
      CHECK(!dialog.savedPath());
      CHECK(messageLabel->text().toStdString() == refusal);
      CHECK(manager.topMapWindow() == editor);
    }
    CHECK(readFile(prefabPath) == fileBefore);

    // once the editor is closed, the file may go
    editor->close();
    deleteClosedWindows();
    REQUIRE(editor == nullptr);
    CHECK(hostPanel->deletePrefabFile(prefabPath) | kdl::is_success());
    CHECK(!std::filesystem::exists(prefabPath));
  }

  SECTION("a save of an assembly whose body is not one group warns")
  {
    // R8-app-2: a node outside the group is lost by every placement
    const auto assemblyPath = mdl::savePrefab(
                                sourceMap,
                                {entityNode},
                                mdl::SavePrefabOptions{
                                  .name = "lampset",
                                  .category = "lights",
                                  .root = mdl::PrefabRoot::User,
                                  .forceAssembly = true})
                              | kdl::value();

    REQUIRE(manager.openPrefabEditor(*host, assemblyPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());
    editor->setThumbnailGrabberForTesting([] { return QImage{}; });

    auto& editorMap = editor->document().map();
    REQUIRE(!mdl::addNodes(
               editorMap,
               {{editorMap.worldNode().defaultLayer(),
                 {new mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}}}}})
               .empty());

    REQUIRE(editor->saveDocument());
    CHECK(
      editor->statusBar()->currentMessage()
      == "Prefab lampset.map is an assembly, but it no longer holds exactly one group");
    finishSave();
  }

  SECTION("a prefab is not opened from a closed window")
  {
    // R1-pf3-3: the panel posts the request, which may run after the window's close and
    // before its deletion
    host->close();
    REQUIRE(manager.mapWindows().empty());

    CHECK(manager.openPrefabEditor(*host, prefabPath).is_error());
    CHECK(manager.mapWindows().empty());

    deleteClosedWindows();
  }

  SECTION("New and Open with the prefab editor on top replace the host's document")
  {
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());

    const auto mapPath = env.dir() / "other.map";
    REQUIRE(sourceMap.saveTo(mapPath) | kdl::is_success());

    REQUIRE(
      manager.loadDocument(gameInfo, mapFormat, worldBounds, mapPath)
      | kdl::is_success());
    CHECK(manager.mapWindows().size() == 2u);
    CHECK(host->document().map().path() == mapPath);
    CHECK(editor->document().map().path() == prefabPath);

    // R1-pf3-4: the host comes to the front; its document had no changes to ask about
    CHECK(manager.topMapWindow() == host);

    // a map with a file compiles, a prefab does not
    CHECK(host->canCompile());
    CHECK(host->canLaunch());
    CHECK(!editor->canCompile());
    CHECK(!editor->canLaunch());

    // DD-26: and the Run menu says so
    for (const auto& actionPath : {"Menu/Run/Compile...", "Menu/Run/Launch..."})
    {
      auto* hostAction = host->findAction(actionPath);
      auto* editorAction = editor->findAction(actionPath);
      REQUIRE(hostAction != nullptr);
      REQUIRE(editorAction != nullptr);
      CHECK(hostAction->isEnabled());
      CHECK(!editorAction->isEnabled());
    }

    // the editor gets the focus again
    emit qApp->focusChanged(nullptr, editor);
    REQUIRE(manager.topMapWindow() == editor);

    REQUIRE(manager.createDocument(gameInfo, mapFormat, worldBounds) | kdl::is_success());
    CHECK(manager.mapWindows().size() == 2u);
    CHECK(!host->document().map().persistent());
    CHECK(editor->document().map().path() == prefabPath);
    CHECK(manager.topMapWindow() == host);
  }

  SECTION("closing the prefab editor leaves the host open")
  {
    // R1-pf3-1: the editor's layout is its own, and its widgets' saved state does not
    // replace the map windows' as they were when the editor opened
    QSettings{}.setValue(
      "Windows/MapWindow/Geometry", QByteArray{"the map windows' geometry"});
    const auto settings = mapWindowSettings();

    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto editor = QPointer<MapWindow>{manager.topMapWindow()};
    REQUIRE(editor->isPrefabEditor());

    // R8-app-6: nor does what the editor's widgets write while it is open, such as the
    // tool sidebar's toggle, which writes the key that the map windows read
    auto* toolSidebar = editor->findChild<ToolSidebar*>();
    REQUIRE(toolSidebar != nullptr);
    toolSidebar->setExpanded(!toolSidebar->expanded());
    CHECK(mapWindowSettings() == settings);
    CHECK(QSettings{}.contains(
      "Windows/PrefabEditorWindow/" + toolSidebar->objectName() + "/Expanded"));

    // H-F8-1: what a map window saves while the editor is open stays
    QSettings{}.setValue(
      "Windows/MapWindow/Geometry", QByteArray{"saved while the editor was open"});
    auto expected = settings;
    expected["MapWindow/Geometry"] = QByteArray{"saved while the editor was open"};

    editor->close();
    deleteClosedWindows();

    CHECK(editor == nullptr);
    CHECK(host != nullptr);
    CHECK(manager.mapWindows() == std::vector<MapWindow*>{host});

    CHECK(mapWindowSettings() == expected);
    CHECK(QSettings{}.contains("Windows/PrefabEditorWindow/Geometry"));
    CHECK(QSettings{}.contains("Windows/PrefabEditorWindow/State"));
  }

  SECTION("closing the host leaves the prefab editor open, and New opens a window")
  {
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto editor = QPointer<MapWindow>{manager.topMapWindow()};
    REQUIRE(editor->isPrefabEditor());

    host->close();
    deleteClosedWindows();

    CHECK(host == nullptr);
    CHECK(manager.mapWindows() == std::vector<MapWindow*>{editor});

    // no map window is left whose document New could replace
    REQUIRE(manager.createDocument(gameInfo, mapFormat, worldBounds) | kdl::is_success());
    REQUIRE(manager.mapWindows().size() == 2u);
    auto* newWindow = manager.topMapWindow();
    CHECK(newWindow != editor);
    CHECK(!newWindow->isPrefabEditor());
    CHECK(editor->document().map().path() == prefabPath);

    // R1-pf3-14: a save tells the windows that are left
    scanLibrary(*newWindow);
    REQUIRE(
      !hasThumbnail(*newWindow, "lights/lamp.map", userRoot / "lights" / "lamp.png"));

    editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
    modify(*editor);
    REQUIRE(editor->saveDocument());
    finishSave();
    CHECK(QTest::qWaitFor([&] {
      return hasThumbnail(
        *newWindow, "lights/lamp.map", userRoot / "lights" / "lamp.png");
    }));
  }

  SECTION("a prefab editor tells every other window of a save")
  {
    // R1-pf3-9: an editor opened from an editor; the host hears of its saves too
    const auto otherPrefabPath = userRoot / "lights" / "lamp2.map";
    std::filesystem::copy_file(prefabPath, otherPrefabPath);
    const auto otherThumbnailPath = userRoot / "lights" / "lamp2.png";

    scanLibrary(*host);
    REQUIRE(!hasThumbnail(*host, "lights/lamp2.map", otherThumbnailPath));

    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(manager.openPrefabEditor(*editor, otherPrefabPath) | kdl::is_success());
    auto* otherEditor = manager.topMapWindow();
    REQUIRE(manager.mapWindows().size() == 3u);
    REQUIRE(otherEditor->isPrefabEditor());
    REQUIRE(otherEditor->document().map().path() == otherPrefabPath);

    otherEditor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
    modify(*otherEditor);
    REQUIRE(otherEditor->saveDocument());
    finishSave();
    CHECK(QTest::qWaitFor(
      [&] { return hasThumbnail(*host, "lights/lamp2.map", otherThumbnailPath); }));
  }

  SECTION("saving the prefab writes its thumbnail and updates the host's library")
  {
    auto* hostPanel = host->findChild<PrefabPanel*>();
    REQUIRE(hostPanel != nullptr);
    const auto& library = host->document().map().prefabLibrary();

    // R1-pf3-13: a visible panel, which watches the library's folders
    showPrefabPanel(*host, *hostPanel);
    REQUIRE(QTest::qWaitFor(
      [&] { return hostPanel->scanCount() == 1u && !library.scanning(); }));
    const auto* entry = library.find(mdl::PrefabRoot::User, "lights/lamp.map");
    REQUIRE(entry != nullptr);
    CHECK(!entry->thumbnailPath);

    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto* editor = manager.topMapWindow();
    REQUIRE(editor->isPrefabEditor());

    const auto thumbnailPath = userRoot / "lights" / "lamp.png";

    SECTION("with a 3D view")
    {
      editor->setThumbnailGrabberForTesting([] { return redImage(512, 256); });
      modify(*editor);
      REQUIRE(editor->saveDocument());

      // R1-pf3-10: written once the event loop runs
      CHECK(!std::filesystem::exists(thumbnailPath));
      finishSave();

      // at most 256 pixels on the longer side
      const auto thumbnail = QImage{pathAsQString(thumbnailPath)};
      CHECK(thumbnail.size() == QSize{256, 128});

      // a plain save keeps the prefab's keys
      CHECK(
        readFile(prefabPath).find("\"_tb_prefab_name\" \"lamp\"") != std::string::npos);

      // the save reads the saved file and its thumbnail, and nothing else
      REQUIRE(QTest::qWaitFor(
        [&] { return hasThumbnail(*host, "lights/lamp.map", thumbnailPath); }));
      CHECK(hostPanel->scanCount() == 1u);

      // R2-pf3-2: nor is the file read by the editor's library, which no panel scanned
      auto* editorPanel = editor->findChild<PrefabPanel*>();
      REQUIRE(editorPanel != nullptr);
      CHECK(editorPanel->scanCount() == 0u);
      CHECK(editor->document().map().prefabLibrary().entries().empty());

      // R1-pf3-13: nor does the watcher's report of the save rescan, past the debounce
      QTest::qWait(1500);
      CHECK(hostPanel->scanCount() == 1u);

      // while a change that is not the panel's own does
      {
        auto stream = std::ofstream{userRoot / "other.map"};
        stream << "// not a prefab\n";
      }
      CHECK(QTest::qWaitFor([&] { return hostPanel->scanCount() == 2u; }, 5000));
    }

    SECTION("the editor's own panel takes the save for its own write")
    {
      // R2-pf3-3: its folder watcher reports the save, too
      auto* editorPanel = editor->findChild<PrefabPanel*>();
      REQUIRE(editorPanel != nullptr);
      const auto& editorLibrary = editor->document().map().prefabLibrary();
      showPrefabPanel(*editor, *editorPanel);
      REQUIRE(QTest::qWaitFor(
        [&] { return editorPanel->scanCount() == 1u && !editorLibrary.scanning(); }));
      REQUIRE(!hasThumbnail(*editor, "lights/lamp.map", thumbnailPath));

      editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();
      CHECK(QTest::qWaitFor(
        [&] { return hasThumbnail(*editor, "lights/lamp.map", thumbnailPath); }));

      QTest::qWait(1500);
      CHECK(editorPanel->scanCount() == 1u);
      CHECK(hostPanel->scanCount() == 1u);
    }

    SECTION("without a 3D view")
    {
      editor->setThumbnailGrabberForTesting([] { return QImage{}; });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();

      CHECK(!std::filesystem::exists(thumbnailPath));
      CHECK(editor->statusBar()->currentMessage().startsWith("No thumbnail saved"));
      CHECK(
        readFile(prefabPath).find("\"_tb_prefab_name\" \"lamp\"") != std::string::npos);
    }

    SECTION("an editor that is not shown grabs no thumbnail")
    {
      // R1-pf3-15: the grab of the 3D view itself, which a hidden view does not draw
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();

      CHECK(!std::filesystem::exists(thumbnailPath));
      CHECK(editor->statusBar()->currentMessage().startsWith("No thumbnail saved"));
    }

    SECTION("a thumbnail that cannot be written is reported")
    {
      // R1-pf3-7: a folder where the thumbnail goes
      std::filesystem::create_directory(thumbnailPath);

      editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();

      CHECK(std::filesystem::is_directory(thumbnailPath));
      CHECK(editor->statusBar()->currentMessage().startsWith(
        "Could not write thumbnail lamp.png: "));
    }

    SECTION("an unmodified save writes no thumbnail")
    {
      // DD-38
      editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
      REQUIRE(!editor->document().map().modified());
      REQUIRE(editor->saveDocument());
      finishSave();
      CHECK(!std::filesystem::exists(thumbnailPath));
    }

    SECTION("a thumbnail that the editor did not write is kept")
    {
      // DD-38: a hand-made thumbnail (C1) carries no mark of the editor
      REQUIRE(redImage(8, 8).save(pathAsQString(thumbnailPath)));
      REQUIRE(QImageReader{pathAsQString(thumbnailPath)}.text("Software").isEmpty());
      const auto thumbnailBefore = readFile(thumbnailPath);

      editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();

      CHECK(readFile(thumbnailPath) == thumbnailBefore);
      CHECK(editor->statusBar()->currentMessage().startsWith("Kept thumbnail lamp.png"));
    }

    SECTION("the editor's own thumbnail is replaced")
    {
      editor->setThumbnailGrabberForTesting([] { return redImage(16, 16); });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();
      CHECK(QImage{pathAsQString(thumbnailPath)}.size() == QSize{16, 16});
      CHECK(
        QImageReader{pathAsQString(thumbnailPath)}.text("Software")
        == "SinkBroom prefab editor");

      editor->setThumbnailGrabberForTesting([] { return redImage(32, 32); });
      modify(*editor);
      REQUIRE(editor->saveDocument());
      finishSave();
      CHECK(QImage{pathAsQString(thumbnailPath)}.size() == QSize{32, 32});
    }

    SECTION("a save brings the prefab's class up to date and nothing else")
    {
      // R1-pf3-11, DD-30
      auto& editorMap = editor->document().map();
      auto& worldNode = editorMap.worldNode();
      const auto* savedValue =
        worldNode.entity().property(mdl::PrefabPropertyKeys::Saved);
      REQUIRE(savedValue != nullptr);
      const auto savedKey =
        fmt::format("\"{}\" \"{}\"", mdl::PrefabPropertyKeys::Saved, *savedValue);

      REQUIRE(worldNode.defaultLayer()->children().size() == 1u);
      auto* lampNode =
        dynamic_cast<mdl::EntityNode*>(worldNode.defaultLayer()->children().front());
      REQUIRE(lampNode != nullptr);

      editor->setThumbnailGrabberForTesting([] { return QImage{}; });

      SECTION("an unmodified save writes the file as it was")
      {
        const auto fileBefore = readFile(prefabPath);
        REQUIRE(!editorMap.modified());

        REQUIRE(editor->saveDocument());
        finishSave();

        CHECK(
          mdl::fnv1a64HexNormalizedLineEndings(readFile(prefabPath))
          == mdl::fnv1a64HexNormalizedLineEndings(fileBefore));
        CHECK(!editorMap.canUndoCommand());
      }

      SECTION("a plain edit keeps the snapshot time")
      {
        auto lampEntity = lampNode->entity();
        lampEntity.addOrUpdateProperty("light_radius", "64 64 64");
        REQUIRE(mdl::updateNodeContents(
          editorMap,
          "Set Radius",
          {{lampNode, mdl::NodeContents{std::move(lampEntity)}}}));

        REQUIRE(editor->saveDocument());
        finishSave();

        const auto file = readFile(prefabPath);
        CHECK(file.find("\"light_radius\" \"64 64 64\"") != std::string::npos);
        CHECK(file.find(savedKey) != std::string::npos);
        CHECK(file.find("\"_tb_prefab_class\" \"light\"") != std::string::npos);

        // no undo step of its own
        REQUIRE(editorMap.undoCommandName() != nullptr);
        CHECK(*editorMap.undoCommandName() == "Set Radius");
        CHECK(!editorMap.modified());
      }

      SECTION("an entity snapshot of another class")
      {
        auto lampEntity = lampNode->entity();
        lampEntity.setClassname("info_null");
        REQUIRE(mdl::updateNodeContents(
          editorMap,
          "Set Classname",
          {{lampNode, mdl::NodeContents{std::move(lampEntity)}}}));

        REQUIRE(editor->saveDocument());
        finishSave();

        const auto file = readFile(prefabPath);
        CHECK(file.find("\"_tb_prefab_class\" \"info_null\"") != std::string::npos);
        CHECK(file.find(savedKey) != std::string::npos);

        REQUIRE(editorMap.undoCommandName() != nullptr);
        CHECK(*editorMap.undoCommandName() == "Update Prefab Metadata");
        CHECK(!editorMap.modified());
      }

      SECTION("an entity snapshot that holds two entities keeps its class")
      {
        REQUIRE(!mdl::addNodes(
                   editorMap,
                   {{worldNode.defaultLayer(),
                     {new mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}}}}})
                   .empty());

        REQUIRE(editor->saveDocument());
        CHECK(
          editor->statusBar()->currentMessage()
          == "Prefab lamp.map is an entity snapshot, but it no longer holds exactly "
             "one entity");
        finishSave();

        const auto file = readFile(prefabPath);
        CHECK(file.find("\"_tb_prefab_class\" \"light\"") != std::string::npos);
        CHECK(file.find(savedKey) != std::string::npos);
      }
    }
  }
}

} // namespace tb::ui
