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
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QPointer>
#include <QSettings>
#include <QSize>
#include <QStringList>
#include <QTest>
#include <QVariant>

#include "base/Preference.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/WorldNode.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/PrefabPanel.h"
#include "ui/QPathUtils.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

    REQUIRE(manager.createDocument(gameInfo, mapFormat, worldBounds) | kdl::is_success());
    CHECK(manager.mapWindows().size() == 2u);
    CHECK(!host->document().map().persistent());
    CHECK(editor->document().map().path() == prefabPath);
  }

  SECTION("closing the prefab editor leaves the host open")
  {
    REQUIRE(manager.openPrefabEditor(*host, prefabPath) | kdl::is_success());
    auto editor = QPointer<MapWindow>{manager.topMapWindow()};
    REQUIRE(editor->isPrefabEditor());

    editor->close();
    deleteClosedWindows();

    CHECK(editor == nullptr);
    CHECK(host != nullptr);
    CHECK(manager.mapWindows() == std::vector<MapWindow*>{host});
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
    CHECK(manager.topMapWindow() != editor);
    CHECK(!manager.topMapWindow()->isPrefabEditor());
    CHECK(editor->document().map().path() == prefabPath);

    // a save tells the windows that are left
    editor->setThumbnailGrabberForTesting([] { return QImage{}; });
    CHECK(editor->saveDocument());
  }

  SECTION("saving the prefab writes its thumbnail and updates the host's library")
  {
    auto* hostPanel = host->findChild<PrefabPanel*>();
    REQUIRE(hostPanel != nullptr);
    const auto& library = host->document().map().prefabLibrary();

    hostPanel->scanOnce();
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
      REQUIRE(editor->saveDocument());

      // at most 256 pixels on the longer side
      const auto thumbnail = QImage{pathAsQString(thumbnailPath)};
      CHECK(thumbnail.size() == QSize{256, 128});

      // a plain save keeps the prefab's keys
      CHECK(
        readFile(prefabPath).find("\"_tb_prefab_name\" \"lamp\"") != std::string::npos);

      // the save reads the saved file and its thumbnail, and nothing else
      REQUIRE(QTest::qWaitFor([&] {
        const auto* savedEntry = library.find(mdl::PrefabRoot::User, "lights/lamp.map");
        return savedEntry && savedEntry->thumbnailPath && !library.scanning();
      }));
      CHECK(
        library.find(mdl::PrefabRoot::User, "lights/lamp.map")->thumbnailPath
        == thumbnailPath);
      CHECK(hostPanel->scanCount() == 1u);
    }

    SECTION("without a 3D view")
    {
      editor->setThumbnailGrabberForTesting([] { return QImage{}; });
      REQUIRE(editor->saveDocument());

      CHECK(!std::filesystem::exists(thumbnailPath));
      CHECK(
        readFile(prefabPath).find("\"_tb_prefab_name\" \"lamp\"") != std::string::npos);
    }
  }
}

} // namespace tb::ui
