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

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QToolButton>

#include "SidebarTestUtils.h"
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "gl/Texture.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/PrefabBrowserView.h"
#include "ui/PrefabPanel.h"
#include "ui/QPathUtils.h"
#include "ui/SavePrefabDialog.h"

#include "kd/result.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

mdl::PrefabEntry makeEntry(
  const std::filesystem::path& relativePath,
  const mdl::PrefabKind kind,
  std::string name,
  std::string className = {},
  std::vector<std::string> tags = {})
{
  auto entry = mdl::PrefabEntry{};
  entry.relativePath = relativePath;
  entry.metadata.kind = kind;
  entry.metadata.name = std::move(name);
  entry.metadata.className = std::move(className);
  entry.metadata.tags = std::move(tags);
  return entry;
}

void pushEntry(
  std::vector<std::unique_ptr<mdl::PrefabEntry>>& entries, mdl::PrefabEntry entry)
{
  entries.push_back(std::make_unique<mdl::PrefabEntry>(std::move(entry)));
}

mdl::EntityNode* addEntity(mdl::Map& map, std::vector<mdl::EntityProperty> properties)
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(properties)}};
  REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}}).empty());
  return entityNode;
}

void selectOnly(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  QApplication::processEvents();
}

/**
 * The default game without a game path, so that it has no mod root, and with its Prefab
 * Path preference, under the given preference path if one is given, defaulting to the
 * given folder.
 */
mdl::MapFixtureConfig fixtureConfigWithUserRoot(
  const std::filesystem::path& userRoot,
  std::optional<std::filesystem::path> preferencePath = std::nullopt)
{
  auto gameInfo = mdl::detail::makeGameInfoFixture(mdl::DefaultGameInfo.gameConfig, {});
  gameInfo.prefabPathPreference = Preference<std::filesystem::path>{
    preferencePath.value_or(gameInfo.prefabPathPreference.path), userRoot};
  return mdl::MapFixtureConfig{.gameInfo = std::move(gameInfo)};
}

} // namespace

TEST_CASE("PrefabBrowserView")
{
  auto entries = std::vector<std::unique_ptr<mdl::PrefabEntry>>{};
  pushEntry(
    entries,
    makeEntry(
      "lights/torch.map", mdl::PrefabKind::Entity, "Torch", "light", {"fire", "wall"}));
  pushEntry(
    entries, makeEntry("doors/metal/hatch.map", mdl::PrefabKind::Assembly, "Hatch"));
  pushEntry(entries, makeEntry("crate.map", mdl::PrefabKind::Assembly, "Crate"));
  pushEntry(
    entries, makeEntry("lights/lamp.map", mdl::PrefabKind::Entity, "Lamp", "light_lamp"));

  auto broken = makeEntry("broken.map", mdl::PrefabKind::Entity, "broken");
  broken.error = "Could not parse";

  SECTION("prefabCategories")
  {
    // distinct, sorted, the top level as the empty path
    CHECK(
      prefabCategories(entries)
      == std::vector<std::filesystem::path>{"", "doors/metal", "lights"});
    CHECK(prefabCategories({}).empty());
  }

  SECTION("matchesPrefabFilter")
  {
    const auto& torch = *entries[0];
    const auto& hatch = *entries[1];

    CHECK(matchesPrefabFilter(torch, PrefabFilter{}));
    CHECK(matchesPrefabFilter(hatch, PrefabFilter{}));

    // kind
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.kind = PrefabKindFilter::Entities}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.kind = PrefabKindFilter::Entities}));
    CHECK(matchesPrefabFilter(hatch, PrefabFilter{.kind = PrefabKindFilter::Assemblies}));
    CHECK(
      !matchesPrefabFilter(torch, PrefabFilter{.kind = PrefabKindFilter::Assemblies}));

    // a broken file has no kind
    CHECK(matchesPrefabFilter(broken, PrefabFilter{}));
    CHECK(!matchesPrefabFilter(broken, PrefabFilter{.kind = PrefabKindFilter::Entities}));

    // category: the entry's own folder only
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.category = "lights"}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.category = ""}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.category = "doors"}));
    CHECK(matchesPrefabFilter(hatch, PrefabFilter{.category = "doors/metal"}));
    CHECK(matchesPrefabFilter(*entries[2], PrefabFilter{.category = ""}));

    // words: name, tags and class, case-insensitive, every word
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"tor"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"FIRE"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"light"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"torch", "wall"}}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.words = {"torch", "door"}}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.words = {"light"}}));
  }

  SECTION("loadPrefabThumbnail")
  {
    auto env = fs::TestEnvironment{};

    // scaled down to at most 128 on the longest side, keeping the aspect
    auto wideImage = QImage{300, 150, QImage::Format_RGBA8888};
    wideImage.fill(Qt::red);
    REQUIRE(wideImage.save(pathAsQString(env.dir() / "wide.png")));

    const auto wide = loadPrefabThumbnail(env.dir() / "wide.png") | kdl::value();
    CHECK(wide.width() == PrefabThumbnailMaxSize);
    CHECK(wide.height() == PrefabThumbnailMaxSize / 2);

    auto tallImage = QImage{40, 1000, QImage::Format_RGBA8888};
    tallImage.fill(Qt::blue);
    REQUIRE(tallImage.save(pathAsQString(env.dir() / "tall.png")));

    const auto tall = loadPrefabThumbnail(env.dir() / "tall.png") | kdl::value();
    CHECK(tall.height() == PrefabThumbnailMaxSize);
    CHECK(tall.width() == 5u);

    // a small image is kept as it is
    auto smallImage = QImage{32, 16, QImage::Format_RGBA8888};
    smallImage.fill(Qt::green);
    REQUIRE(smallImage.save(pathAsQString(env.dir() / "small.png")));

    const auto small = loadPrefabThumbnail(env.dir() / "small.png") | kdl::value();
    CHECK(small.width() == 32u);
    CHECK(small.height() == 16u);

    // not an image
    env.createFile("broken.png", "not an image");
    CHECK(loadPrefabThumbnail(env.dir() / "broken.png").is_error());
  }
}

TEST_CASE("SavePrefabDialog")
{
  auto env = fs::TestEnvironment{};
  auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
  auto& document = fixture.document();
  auto& map = document.map();

  SECTION("defaultPrefabName")
  {
    auto* namedNode = addEntity(map, {{"classname", "light"}, {"name", "lamp_1"}});
    auto* unnamedNode = addEntity(map, {{"classname", "monster_imp"}});

    selectOnly(map, {namedNode});
    CHECK(defaultPrefabName(document) == "lamp_1");

    selectOnly(map, {unnamedNode});
    CHECK(defaultPrefabName(document) == "monster_imp");

    auto* groupNode = new mdl::GroupNode{mdl::Group{"pillars"}};
    REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {groupNode}}}).empty());
    selectOnly(map, {groupNode});
    CHECK(defaultPrefabName(document) == "pillars");

    mdl::deselectAll(map);
    CHECK(defaultPrefabName(document) == "prefab");
  }

  SECTION("prefabFilePath")
  {
    auto* entityNode = addEntity(map, {{"classname", "light"}});

    // the dialog asks before replacing the file that savePrefab writes
    const auto& library = map.prefabLibrary();
    const auto options = mdl::SavePrefabOptions{
      .name = " a:b? ", .category = "lights", .root = mdl::PrefabRoot::User};
    const auto path = mdl::prefabFilePath(library, options) | kdl::value();
    CHECK(path == env.dir() / "lights" / "a_b_.map");
    CHECK((mdl::savePrefab(map, {entityNode}, options) | kdl::value()) == path);

    // no name, or no root
    CHECK(mdl::prefabFilePath(library, mdl::SavePrefabOptions{.name = " . "}).is_error());
    CHECK(mdl::prefabFilePath(
            library, mdl::SavePrefabOptions{.name = "x", .root = mdl::PrefabRoot::Mod})
            .is_error());
  }

  SECTION("a category outside the library cannot be saved to")
  {
    selectOnly(map, {addEntity(map, {{"classname", "light"}, {"name", "lamp"}})});

    auto dialog = SavePrefabDialog{document};
    auto* categoryChoice =
      dialog.findChild<QComboBox*>("SavePrefabDialog_categoryChoice");
    auto* messageLabel = dialog.findChild<QLabel*>("SavePrefabDialog_messageLabel");
    auto* saveButton = dialog.findChild<QPushButton*>("SavePrefabDialog_saveButton");
    REQUIRE(categoryChoice != nullptr);
    REQUIRE(messageLabel != nullptr);
    REQUIRE(saveButton != nullptr);
    CHECK(saveButton->isEnabled());
    CHECK(messageLabel->text().isEmpty());

    for (const auto* category : {"../out", "lights/../..", "/lights"})
    {
      CAPTURE(category);
      categoryChoice->setCurrentText(category);
      CHECK(!saveButton->isEnabled());
      CHECK(!messageLabel->text().isEmpty());
    }

    categoryChoice->setCurrentText("lights");
    CHECK(saveButton->isEnabled());
    CHECK(messageLabel->text().isEmpty());
  }
}

TEST_CASE("PrefabPanel")
{
  SECTION("panel")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = PrefabPanel{fixture.context()};

    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "Prefabs");
    CHECK(panel.title() == "Prefabs");
    CHECK(!panel.shownByDefault());
    CHECK(panel.defaultArea() == Qt::RightDockWidgetArea);
    CHECK(panel.iconPath() == "Prefabs.svg");
    checkGreyscaleIcon(panel.iconPath());
  }

  SECTION("save button without a root")
  {
    // no game path and no Prefab Path: there is nowhere to save
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot({})};
    auto& map = fixture.document().map();
    auto panel = PrefabPanel{fixture.context()};

    auto* saveButton = panel.findChild<QToolButton*>("PrefabPanel_saveButton");
    REQUIRE(saveButton != nullptr);

    selectOnly(map, {addEntity(map, {{"classname", "light"}})});
    CHECK(!saveButton->isEnabled());
    CHECK(saveButton->toolTip() == "Set a game path or a Prefab Path first");
  }

  SECTION("scan")
  {
    auto env = fs::TestEnvironment{};
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
    auto& map = fixture.document().map();
    auto& library = map.prefabLibrary();

    // the document pushes the Prefab Path into the library
    REQUIRE(library.userRoot() == env.dir());

    auto* entityNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    REQUIRE(
      mdl::savePrefab(
        map,
        {entityNode},
        mdl::SavePrefabOptions{
          .name = "lamp", .category = "lights", .root = mdl::PrefabRoot::User})
      | kdl::is_success());

    // the panel scans on construction and takes the results on its timer
    auto panel = PrefabPanel{fixture.context()};
    CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front()->relativePath == "lights/lamp.map");

    auto* categoryChoice = panel.findChild<QComboBox*>("PrefabPanel_categoryChoice");
    REQUIRE(categoryChoice != nullptr);
    REQUIRE(categoryChoice->count() == 2);
    CHECK(categoryChoice->itemText(0) == "All");
    CHECK(categoryChoice->itemText(1) == "lights");

    // the cells point at the library's entries and carry their C4 payloads
    auto* view = panel.findChild<PrefabBrowserView*>();
    REQUIRE(view != nullptr);
    const auto cells = view->cellSummaries();
    REQUIRE(cells.size() == 1u);
    CHECK(cells[0].entry == library.entries()[0].get());
    CHECK(cells[0].title == "lamp");
    CHECK(cells[0].dndData == "prefab:user/lights/lamp.map");

    auto* saveButton = panel.findChild<QToolButton*>("PrefabPanel_saveButton");
    REQUIRE(saveButton != nullptr);

    mdl::deselectAll(map);
    QApplication::processEvents();
    CHECK(!saveButton->isEnabled());

    selectOnly(map, {entityNode});
    CHECK(saveButton->isEnabled());
    CHECK(saveButton->toolTip() == "Save selection as prefab...");

    SECTION("Rescan finds a new file")
    {
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{.name = "lamp2", .root = mdl::PrefabRoot::User})
        | kdl::is_success());

      auto* rescanButton = panel.findChild<QToolButton*>("PrefabPanel_rescanButton");
      REQUIRE(rescanButton != nullptr);
      rescanButton->click();
      CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));
      CHECK(library.entries().size() == 2);
      CHECK(categoryChoice->count() == 3);
      CHECK(categoryChoice->itemText(1) == "(top level)");

      // the cells point at the new entries
      const auto newCells = view->cellSummaries();
      REQUIRE(newCells.size() == 2u);
      for (size_t i = 0; i < newCells.size(); ++i)
      {
        const auto& entry = *library.entries()[i];
        CHECK(newCells[i].entry == &entry);
        CHECK(newCells[i].dndData == QString::fromStdString(library.payloadFor(entry)));
      }
    }

    SECTION("A prefab without a name is titled by its file")
    {
      env.createFile("unnamed.map", R"(// entity 0
{
"classname" "worldspawn"
"_tb_prefab_kind" "entity"
}
// entity 1
{
"classname" "light"
"origin" "0 0 0"
}
)");
      panel.rescan();
      CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));

      const auto* entry = library.find(mdl::PrefabRoot::User, "unnamed.map");
      REQUIRE(entry != nullptr);
      REQUIRE(!entry->error);
      REQUIRE(entry->metadata.name.empty());

      const auto newCells = view->cellSummaries();
      const auto it = std::ranges::find_if(
        newCells, [&](const auto& cell) { return cell.entry == entry; });
      REQUIRE(it != newCells.end());
      CHECK(it->title == "unnamed");
    }

    SECTION("The panel follows the library of a new map")
    {
      // a file that only a scan of the new map's library finds
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "hatch", .category = "doors", .root = mdl::PrefabRoot::User})
        | kdl::is_success());

      // the fixture owns the game, so it outlives the old map
      REQUIRE(fixture.document().create(
        map.environmentConfig(),
        map.gameInfo(),
        map.worldNode().mapFormat(),
        map.worldBounds()));

      // map and library are gone with the old map
      auto& newLibrary = fixture.document().map().prefabLibrary();
      REQUIRE(&newLibrary != &library);
      CHECK(newLibrary.userRoot() == env.dir());
      CHECK(QTest::qWaitFor([&] { return !newLibrary.scanning(); }));
      REQUIRE(newLibrary.entries().size() == 2u);

      // the panel observes the new library: the categories and the cells follow it
      REQUIRE(categoryChoice->count() == 3);
      CHECK(categoryChoice->itemText(1) == "doors");
      CHECK(categoryChoice->itemText(2) == "lights");

      const auto newCells = view->cellSummaries();
      REQUIRE(newCells.size() == 2u);
      CHECK(newCells[0].entry == newLibrary.entries()[0].get());
      CHECK(newCells[1].entry == newLibrary.entries()[1].get());
    }
  }

  SECTION("A Prefab Path change moves the user root and rescans")
  {
    auto env = fs::TestEnvironment{};
    auto otherEnv = fs::TestEnvironment{};

    // a preference of its own, so that the value it keeps after the test reaches no
    // other test, set first, so that no earlier value counts
    auto prefabPath = Preference<std::filesystem::path>{
      "Games/PrefabPanelTest/Prefab Path", std::filesystem::path{}};
    setPref(prefabPath, env.dir());

    auto fixture =
      SidebarContextFixture{fixtureConfigWithUserRoot(env.dir(), prefabPath.path)};
    auto& map = fixture.document().map();
    auto& library = map.prefabLibrary();
    REQUIRE(library.userRoot() == env.dir());

    // a prefab in the other folder only
    auto* entityNode = addEntity(map, {{"classname", "light"}, {"name", "hatch"}});
    library.setUserRoot(otherEnv.dir());
    REQUIRE(
      mdl::savePrefab(
        map,
        {entityNode},
        mdl::SavePrefabOptions{
          .name = "hatch", .category = "doors", .root = mdl::PrefabRoot::User})
      | kdl::is_success());
    library.setUserRoot(env.dir());

    auto panel = PrefabPanel{fixture.context()};
    CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));
    CHECK(library.entries().empty());

    // the document pushes the new path into the library, the panel rescans it
    setPref(prefabPath, otherEnv.dir());
    CHECK(QTest::qWaitFor([&] { return library.userRoot() == otherEnv.dir(); }));
    CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));
    REQUIRE(library.entries().size() == 1u);
    CHECK(library.entries().front()->relativePath == "doors/hatch.map");

    auto* categoryChoice = panel.findChild<QComboBox*>("PrefabPanel_categoryChoice");
    REQUIRE(categoryChoice != nullptr);
    REQUIRE(categoryChoice->count() == 2);
    CHECK(categoryChoice->itemText(1) == "doors");
  }
}

} // namespace tb::ui
