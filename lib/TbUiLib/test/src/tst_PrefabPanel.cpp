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
#include <QFileSystemWatcher>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

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
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/PrefabBrowserView.h"
#include "ui/PrefabPanel.h"
#include "ui/QPathUtils.h"
#include "ui/SavePrefabDialog.h"

#include "kd/result.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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
  entry.relativePathString = relativePath.generic_string();
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

/**
 * Places the body of the given assembly in the map as the drop tool does, with the given
 * prefab path and placed hash. The clones are owned here until the map takes them, so
 * that a failed check leaks none (R2-pf-ui-11).
 */
mdl::GroupNode* placeAssembly(
  mdl::Map& map,
  const mdl::PrefabEntry& entry,
  const std::string& prefabPath,
  const std::string& placedHash)
{
  auto clones = std::vector<std::unique_ptr<mdl::Node>>{};
  for (auto* node : map.prefabLibrary().cloneBody(entry, map.worldBounds()))
  {
    clones.emplace_back(node);
  }
  REQUIRE(clones.size() == 1u);
  auto* groupNode = dynamic_cast<mdl::GroupNode*>(clones.front().get());
  REQUIRE(groupNode != nullptr);

  auto group = groupNode->group();
  group.setPrefabPath(prefabPath);
  group.setPrefabHash(placedHash);
  groupNode->setGroup(std::move(group));

  // addNodes owns the node from here; its command deletes a node that it did not add
  REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {clones.front().release()}}})
             .empty());
  return groupNode;
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

/**
 * Waits until the panel has started the given number of scans and the library has
 * taken the last one over.
 */
bool waitForScan(
  const PrefabPanel& panel, const mdl::PrefabLibrary& library, const size_t scanCount)
{
  return QTest::qWaitFor(
    [&] { return panel.scanCount() == scanCount && !library.scanning(); });
}

/**
 * The texts of the category tree's items, depth first, each indented by two spaces per
 * level.
 */
std::vector<std::string> categoryTreeTexts(const QTreeWidget& tree)
{
  auto result = std::vector<std::string>{};
  const auto addItem =
    [&](const auto& self, const QTreeWidgetItem& item, const int depth) -> void {
    result.push_back(std::string(size_t(depth) * 2, ' ') + item.text(0).toStdString());
    for (int i = 0; i < item.childCount(); ++i)
    {
      self(self, *item.child(i), depth + 1);
    }
  };
  for (int i = 0; i < tree.topLevelItemCount(); ++i)
  {
    addItem(addItem, *tree.topLevelItem(i), 0);
  }
  return result;
}

/**
 * The first item of the category tree with the given text.
 */
QTreeWidgetItem* findCategoryItem(const QTreeWidget& tree, const QString& text)
{
  const auto items = tree.findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
  return items.empty() ? nullptr : items.front();
}

std::vector<std::filesystem::path> cellPaths(PrefabBrowserView& view)
{
  auto result = std::vector<std::filesystem::path>{};
  for (const auto& cell : view.cellSummaries())
  {
    result.push_back(cell.entry->relativePath);
  }
  return result;
}

const PrefabBrowserView::CellSummary* findCell(
  const std::vector<PrefabBrowserView::CellSummary>& cells,
  const std::filesystem::path& relativePath)
{
  const auto it = std::ranges::find_if(
    cells, [&](const auto& cell) { return cell.entry->relativePath == relativePath; });
  return it != cells.end() ? &*it : nullptr;
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

  SECTION("prefabCategoryCounts")
  {
    // every folder above an entry, counting the entries below it; the top level is not
    // listed
    const auto counts = prefabCategoryCounts(entries);
    REQUIRE(counts.size() == 3u);
    CHECK(counts[0].path == "doors");
    CHECK(counts[0].count == 1u);
    CHECK(counts[1].path == "doors/metal");
    CHECK(counts[1].count == 1u);
    CHECK(counts[2].path == "lights");
    CHECK(counts[2].count == 2u);
    CHECK(prefabCategoryCounts({}).empty());
  }

  SECTION("prefabFilterFromText")
  {
    const auto filter = prefabFilterFromText(" torch  tag:Fire TAG:wall tag: ");
    CHECK(filter.kind == PrefabKindFilter::All);
    CHECK(!filter.category);
    CHECK(filter.words == std::vector<std::string>{"torch"});
    CHECK(filter.tags == std::vector<std::string>{"Fire", "wall"});

    CHECK(prefabFilterFromText("").words.empty());
    CHECK(prefabFilterFromText("").tags.empty());
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

    // category: the folder and the folders below it, by whole names
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.category = "lights"}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.category = "doors"}));
    CHECK(matchesPrefabFilter(hatch, PrefabFilter{.category = "doors"}));
    CHECK(matchesPrefabFilter(hatch, PrefabFilter{.category = "doors/metal"}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.category = "door"}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.category = "doors/metal/old"}));
    CHECK(!matchesPrefabFilter(*entries[2], PrefabFilter{.category = "lights"}));
    // the top level holds everything
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.category = ""}));
    CHECK(matchesPrefabFilter(*entries[2], PrefabFilter{.category = ""}));

    // words: name, tags and class, case-insensitive, every word
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"tor"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"FIRE"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"light"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.words = {"torch", "wall"}}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.words = {"torch", "door"}}));
    CHECK(!matchesPrefabFilter(hatch, PrefabFilter{.words = {"light"}}));

    // tags: whole tags, case-insensitive, every tag
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.tags = {"fire"}}));
    CHECK(matchesPrefabFilter(torch, PrefabFilter{.tags = {"WALL", "fire"}}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.tags = {"fir"}}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.tags = {"fire", "door"}}));
    CHECK(!matchesPrefabFilter(torch, PrefabFilter{.tags = {"torch"}}));
    CHECK(matchesPrefabFilter(torch, prefabFilterFromText("tor tag:fire")));
    CHECK(!matchesPrefabFilter(torch, prefabFilterFromText("tag:fir")));
    CHECK(!matchesPrefabFilter(hatch, prefabFilterFromText("tag:fire")));
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

  SECTION("forEntry updates the prefab's file")
  {
    auto* lampNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    REQUIRE(
      mdl::savePrefab(
        map,
        {lampNode},
        mdl::SavePrefabOptions{
          .name = "Lamp",
          .category = "lights",
          .tags = {"old"},
          .root = mdl::PrefabRoot::User})
      | kdl::is_success());

    auto& library = map.prefabLibrary();
    library.rescan();
    REQUIRE(QTest::qWaitFor([&] {
      library.processPendingResults();
      return !library.scanning();
    }));
    const auto* entry = library.find(mdl::PrefabRoot::User, "lights/Lamp.map");
    REQUIRE(entry != nullptr);
    const auto entryPath = entry->absolutePath;

    selectOnly(map, {lampNode});
    auto dialog = SavePrefabDialog{document};
    dialog.forEntry(*entry);

    auto* nameEdit = dialog.findChild<QLineEdit*>("SavePrefabDialog_nameEdit");
    auto* tagsEdit = dialog.findChild<QLineEdit*>("SavePrefabDialog_tagsEdit");
    auto* categoryChoice =
      dialog.findChild<QComboBox*>("SavePrefabDialog_categoryChoice");
    auto* messageLabel = dialog.findChild<QLabel*>("SavePrefabDialog_messageLabel");
    auto* saveButton = dialog.findChild<QPushButton*>("SavePrefabDialog_saveButton");
    REQUIRE(nameEdit != nullptr);
    REQUIRE(tagsEdit != nullptr);
    REQUIRE(categoryChoice != nullptr);
    REQUIRE(messageLabel != nullptr);
    REQUIRE(saveButton != nullptr);

    // filled in from the prefab
    CHECK(nameEdit->text() == "Lamp");
    CHECK(categoryChoice->currentText() == "lights");
    CHECK(tagsEdit->text() == "old");
    CHECK(saveButton->text() == "Update");
    CHECK(saveButton->isEnabled());
    CHECK(messageLabel->text().isEmpty());

    SECTION("Another name names another file")
    {
      nameEdit->setText("Other");
      CHECK(saveButton->isEnabled());
      CHECK(!messageLabel->text().isEmpty());
    }

    SECTION("The prefab's file is overwritten without asking")
    {
      tagsEdit->setText("new");
      dialog.accept();
      REQUIRE(dialog.savedPath());
      CHECK(std::filesystem::equivalent(*dialog.savedPath(), entryPath));
      CHECK(dialog.result() == QDialog::Accepted);

      library.updateEntry(*dialog.savedPath());
      REQUIRE(QTest::qWaitFor([&] {
        library.processPendingResults();
        return !library.scanning();
      }));
      const auto* updatedEntry = library.find(mdl::PrefabRoot::User, "lights/Lamp.map");
      REQUIRE(updatedEntry != nullptr);
      CHECK(updatedEntry->metadata.tags == std::vector<std::string>{"new"});
    }
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

    // the panel is hidden by default and scans only once it is shown
    auto panel = PrefabPanel{fixture.context()};
    QApplication::processEvents();
    CHECK(panel.scanCount() == 0u);
    CHECK(!library.scanning());
    CHECK(library.entries().empty());

    // a rescan before the first scan waits for it
    panel.rescan();
    QApplication::processEvents();
    CHECK(panel.scanCount() == 0u);

    // the first scan takes the results on the panel's timer
    panel.panelShown();
    CHECK(panel.scanCount() == 1u);
    CHECK(waitForScan(panel, library, 1));
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front()->relativePath == "lights/lamp.map");

    auto* categoryTree = panel.findChild<QTreeWidget*>("PrefabPanel_categoryTree");
    REQUIRE(categoryTree != nullptr);
    CHECK(
      categoryTreeTexts(*categoryTree)
      == std::vector<std::string>{"All (1)", "lights (1)"});

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
      CHECK(waitForScan(panel, library, 2));
      CHECK(library.entries().size() == 2);
      // a prefab at the top level is in no folder
      CHECK(
        categoryTreeTexts(*categoryTree)
        == std::vector<std::string>{"All (2)", "lights (1)"});

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

    SECTION("The category tree shows a folder and the folders below it")
    {
      for (const auto& [name, category] :
           std::vector<std::pair<std::string, std::string>>{
             {"hatch", "doors"},
             {"grate", "doors/metal"},
             {"crate", ""},
           })
      {
        REQUIRE(
          mdl::savePrefab(
            map,
            {entityNode},
            mdl::SavePrefabOptions{
              .name = name, .category = category, .root = mdl::PrefabRoot::User})
          | kdl::is_success());
      }
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 2));
      REQUIRE(library.entries().size() == 4u);

      // nested folders are nested, each counting the prefabs below it
      CHECK(
        categoryTreeTexts(*categoryTree)
        == std::vector<std::string>{"All (4)", "doors (2)", "  metal (1)", "lights (1)"});
      CHECK(categoryTree->currentItem() == categoryTree->topLevelItem(0));
      CHECK(cellPaths(*view).size() == 4u);

      auto* doorsItem = findCategoryItem(*categoryTree, "doors (2)");
      auto* metalItem = findCategoryItem(*categoryTree, "metal (1)");
      REQUIRE(doorsItem != nullptr);
      REQUIRE(metalItem != nullptr);

      categoryTree->setCurrentItem(doorsItem);
      CHECK(
        cellPaths(*view)
        == std::vector<std::filesystem::path>{
          "doors/hatch.map", "doors/metal/grate.map"});

      categoryTree->setCurrentItem(metalItem);
      CHECK(
        cellPaths(*view) == std::vector<std::filesystem::path>{"doors/metal/grate.map"});

      // the chosen folder and the collapsed folders survive a rescan
      doorsItem->setExpanded(false);
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 3));
      REQUIRE(categoryTree->currentItem() != nullptr);
      CHECK(categoryTree->currentItem()->text(0) == "metal (1)");
      CHECK(!findCategoryItem(*categoryTree, "doors (2)")->isExpanded());
      CHECK(
        cellPaths(*view) == std::vector<std::filesystem::path>{"doors/metal/grate.map"});

      categoryTree->setCurrentItem(categoryTree->topLevelItem(0));
      CHECK(cellPaths(*view).size() == 4u);
    }

    SECTION("tag: in the filter box matches whole tags")
    {
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "torch", .tags = {"Fire", "wall"}, .root = mdl::PrefabRoot::User})
        | kdl::is_success());
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 2));
      REQUIRE(library.entries().size() == 2u);

      auto* filterBox = panel.findChild<QLineEdit*>("PrefabPanel_filterBox");
      REQUIRE(filterBox != nullptr);
      CHECK(filterBox->placeholderText().contains("tag:"));

      const auto setFilterText = [&](const QString& text) {
        filterBox->setText(text);
        emit filterBox->textEdited(text);
      };

      setFilterText("tag:fire");
      CHECK(cellPaths(*view) == std::vector<std::filesystem::path>{"torch.map"});

      // a part of a tag is not a tag
      setFilterText("tag:fir");
      CHECK(cellPaths(*view).empty());

      // but it is a part of a word
      setFilterText("fir");
      CHECK(cellPaths(*view) == std::vector<std::filesystem::path>{"torch.map"});

      setFilterText("");
      CHECK(cellPaths(*view).size() == 2u);
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
      CHECK(waitForScan(panel, library, 2));

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
      CHECK(waitForScan(panel, newLibrary, 2));
      REQUIRE(newLibrary.entries().size() == 2u);

      // the panel observes the new library: the categories and the cells follow it
      CHECK(
        categoryTreeTexts(*categoryTree)
        == std::vector<std::string>{"All (2)", "doors (1)", "lights (1)"});

      const auto newCells = view->cellSummaries();
      REQUIRE(newCells.size() == 2u);
      CHECK(newCells[0].entry == newLibrary.entries()[0].get());
      CHECK(newCells[1].entry == newLibrary.entries()[1].get());
    }

    SECTION("Triggers before the event loop runs scan once")
    {
      auto changeCount = 0;
      auto connection =
        library.libraryDidChangeNotifier.connect([&]() { ++changeCount; });

      // a file that only the coalesced scan finds
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{.name = "lamp2", .root = mdl::PrefabRoot::User})
        | kdl::is_success());

      fixture.document().modsDidChangeNotifier();
      panel.rescan();
      panel.rescan();
      CHECK(panel.scanCount() == 1u);

      CHECK(waitForScan(panel, library, 2));
      CHECK(library.entries().size() == 2u);

      // nothing else is posted
      QTest::qWait(100);
      CHECK(panel.scanCount() == 2u);
      CHECK(changeCount == 1);
    }

    SECTION("A save reads the saved file only")
    {
      // a second prefab, so that one is saved again and the other is not
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "crate", .category = "boxes", .root = mdl::PrefabRoot::User})
        | kdl::is_success());
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 2));
      REQUIRE(library.entries().size() == 2u);

      const auto savedCells = view->cellSummaries();
      const auto* crateCell = findCell(savedCells, "boxes/crate.map");
      const auto* lampCell = findCell(savedCells, "lights/lamp.map");
      REQUIRE(crateCell != nullptr);
      REQUIRE(lampCell != nullptr);
      const auto* lampEntry = lampCell->entry;

      SECTION("The previews survive a clear and new definitions")
      {
        view->entityModelsWillChange();
        fixture.document().entityDefinitionsWillChangeNotifier();
        fixture.document().entityDefinitionsDidChangeNotifier();

        const auto newCells = view->cellSummaries();
        REQUIRE(newCells.size() == 2u);
        CHECK(findCell(newCells, "boxes/crate.map")->previewId == crateCell->previewId);
        CHECK(findCell(newCells, "lights/lamp.map")->previewId == lampCell->previewId);
      }

      SECTION("A save updates one entry")
      {
        auto readMutex = std::mutex{};
        auto readPaths = std::vector<std::filesystem::path>{};
        library.setReadHookForTesting([&](const auto& path) {
          const auto lock = std::lock_guard{readMutex};
          readPaths.push_back(path);
        });

        // the tags make the file differ
        const auto lampPath = mdl::savePrefab(
                                map,
                                {entityNode},
                                mdl::SavePrefabOptions{
                                  .name = "lamp",
                                  .category = "lights",
                                  .tags = {"new"},
                                  .root = mdl::PrefabRoot::User})
                              | kdl::value();

        panel.prefabWasSaved(lampPath);
        CHECK(QTest::qWaitFor([&] { return !library.scanning(); }));
        CHECK(panel.scanCount() == 2u);
        library.setReadHookForTesting({});

        {
          const auto lock = std::lock_guard{readMutex};
          REQUIRE(readPaths.size() == 1u);
          CHECK(readPaths.front().lexically_normal() == lampPath.lexically_normal());
        }

        // the entry keeps its address and takes the new file
        REQUIRE(library.entries().size() == 2u);
        const auto* entry = library.find(mdl::PrefabRoot::User, "lights/lamp.map");
        CHECK(entry == lampEntry);
        CHECK(entry->metadata.tags == std::vector<std::string>{"new"});

        // the other entry keeps its preview, the saved one gets a new one
        const auto newCells = view->cellSummaries();
        REQUIRE(newCells.size() == 2u);
        CHECK(findCell(newCells, "boxes/crate.map")->previewId == crateCell->previewId);
        CHECK(findCell(newCells, "lights/lamp.map")->previewId != lampCell->previewId);
      }
    }

    SECTION("The selected entry follows a rescan that reads its file again")
    {
      const auto* oldEntry = library.entries().front().get();
      view->selectEntry(oldEntry->absolutePath);
      REQUIRE(view->selectedEntry() == oldEntry);

      // another spelling of the file's path names the same entry
      view->selectEntry({});
      CHECK(view->selectedEntry() == nullptr);
      view->selectEntry(env.dir() / "lights" / ".." / "lights" / "." / "lamp.map");
      CHECK(view->selectedEntry() == oldEntry);

      // a rescan that changes nothing keeps the entry
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 2));
      CHECK(view->selectedEntry() == oldEntry);

      // the tags make the file differ, so that the rescan reads it again
      REQUIRE(
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "lamp",
            .category = "lights",
            .tags = {"new"},
            .root = mdl::PrefabRoot::User})
        | kdl::is_success());
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 3));

      // the selection names a file, so it finds the entry that replaced the old one
      const auto* newEntry = library.find(mdl::PrefabRoot::User, "lights/lamp.map");
      REQUIRE(newEntry != nullptr);
      CHECK(view->selectedEntry() == newEntry);
      CHECK(view->selectedEntry()->metadata.tags == std::vector<std::string>{"new"});

      // and none once the file is gone
      REQUIRE(std::filesystem::remove(newEntry->absolutePath));
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 4));
      CHECK(view->selectedEntry() == nullptr);
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
    panel.panelShown();
    CHECK(waitForScan(panel, library, 1));
    CHECK(library.entries().empty());

    // the document pushes the new path into the library, the panel rescans it
    setPref(prefabPath, otherEnv.dir());
    CHECK(QTest::qWaitFor([&] { return library.userRoot() == otherEnv.dir(); }));
    CHECK(waitForScan(panel, library, 2));
    REQUIRE(library.entries().size() == 1u);
    CHECK(library.entries().front()->relativePath == "doors/hatch.map");

    auto* categoryTree = panel.findChild<QTreeWidget*>("PrefabPanel_categoryTree");
    REQUIRE(categoryTree != nullptr);
    CHECK(
      categoryTreeTexts(*categoryTree)
      == std::vector<std::string>{"All (1)", "doors (1)"});
  }

  SECTION("Empty states")
  {
    // the text of the line that stands in for the grid, nullopt if the grid shows
    const auto emptyText = [](const PrefabPanel& panel) -> std::optional<QString> {
      const auto* stack = panel.findChild<QStackedWidget*>();
      const auto* label = panel.findChild<QLabel*>("PrefabPanel_emptyLabel");
      REQUIRE(stack != nullptr);
      REQUIRE(label != nullptr);
      return stack->currentWidget() == label ? std::optional{label->text()}
                                             : std::nullopt;
    };

    SECTION("No roots")
    {
      auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot({})};
      auto panel = PrefabPanel{fixture.context()};
      CHECK(emptyText(panel) == "Set a game path or a Prefab Path in Preferences");

      panel.panelShown();
      CHECK(waitForScan(panel, fixture.document().map().prefabLibrary(), 1));
      CHECK(emptyText(panel) == "Set a game path or a Prefab Path in Preferences");
    }

    SECTION("Empty roots")
    {
      auto env = fs::TestEnvironment{};
      auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
      auto& map = fixture.document().map();
      auto& library = map.prefabLibrary();
      auto panel = PrefabPanel{fixture.context()};

      // until a scan has finished, the grid stays
      CHECK(emptyText(panel) == std::nullopt);

      panel.panelShown();
      REQUIRE(waitForScan(panel, library, 1));
      CHECK(emptyText(panel) == "No prefabs yet. Select something and press +");

      // the first prefab brings the grid back
      auto* entityNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
      const auto path =
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{.name = "lamp", .root = mdl::PrefabRoot::User})
        | kdl::value();
      panel.prefabWasSaved(path);
      REQUIRE(QTest::qWaitFor([&] { return library.entries().size() == 1u; }));
      CHECK(emptyText(panel) == std::nullopt);
    }
  }

  SECTION("Delete File and Remove Thumbnail")
  {
    auto env = fs::TestEnvironment{};
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
    auto& map = fixture.document().map();
    auto& library = map.prefabLibrary();

    // an assembly with a thumbnail, and an entity snapshot
    auto* lampNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    auto* markerNode = addEntity(map, {{"classname", "info_null"}, {"name", "marker"}});
    const auto pairPath =
      mdl::savePrefab(
        map,
        {lampNode, markerNode},
        mdl::SavePrefabOptions{
          .name = "pair", .category = "sets", .root = mdl::PrefabRoot::User})
      | kdl::value();
    const auto lampPath =
      mdl::savePrefab(
        map,
        {lampNode},
        mdl::SavePrefabOptions{.name = "lamp", .root = mdl::PrefabRoot::User})
      | kdl::value();
    const auto pairThumbnailPath = env.dir() / "sets" / "pair.png";
    auto image = QImage{8, 8, QImage::Format_RGBA8888};
    image.fill(Qt::red);
    REQUIRE(image.save(pathAsQString(pairThumbnailPath)));

    auto panel = PrefabPanel{fixture.context()};
    panel.panelShown();
    REQUIRE(waitForScan(panel, library, 1));
    REQUIRE(library.entries().size() == 2u);

    const auto* pairEntry = library.find(mdl::PrefabRoot::User, "sets/pair.map");
    REQUIRE(pairEntry != nullptr);
    REQUIRE(pairEntry->thumbnailPath);

    // a placement of the assembly, as the drop tool makes it
    auto* groupNode =
      placeAssembly(map, *pairEntry, "user/sets/pair.map", pairEntry->hash);
    const auto groupChildCount = groupNode->childCount();

    auto* view = panel.findChild<PrefabBrowserView*>();
    REQUIRE(view != nullptr);
    REQUIRE(cellPaths(*view).size() == 2u);

    SECTION("Delete File")
    {
      CHECK(panel.deletePrefabFile(pairPath) | kdl::is_success());

      // the file and its thumbnail are gone, and with them the entry and the cell
      CHECK(!std::filesystem::exists(pairPath));
      CHECK(!std::filesystem::exists(pairThumbnailPath));
      REQUIRE(waitForScan(panel, library, 2));
      CHECK(library.find(mdl::PrefabRoot::User, "sets/pair.map") == nullptr);
      CHECK(cellPaths(*view) == std::vector<std::filesystem::path>{"lamp.map"});

      // the placement stays as it was, and counts as missing
      CHECK(groupNode->parent() != nullptr);
      CHECK(groupNode->childCount() == groupChildCount);
      CHECK(groupNode->group().prefabPath() == "user/sets/pair.map");
      CHECK(panel.changedPlacementCount() == 1u);

      // the other file stays
      CHECK(std::filesystem::exists(lampPath));
    }

    SECTION("Remove Thumbnail")
    {
      CHECK(panel.removePrefabThumbnail(pairPath) | kdl::is_success());
      CHECK(!std::filesystem::exists(pairThumbnailPath));
      CHECK(std::filesystem::exists(pairPath));

      REQUIRE(waitForScan(panel, library, 2));
      const auto* entry = library.find(mdl::PrefabRoot::User, "sets/pair.map");
      REQUIRE(entry != nullptr);
      CHECK(!entry->thumbnailPath);
      CHECK(cellPaths(*view).size() == 2u);
    }

    SECTION("Only the files of entries are deleted")
    {
      // a file in the root that no scan has read, and one outside the root
      env.createFile("sets/unread.map", "{}");
      env.createFile("sets/unread.png", "png");
      auto otherEnv = fs::TestEnvironment{};
      otherEnv.createFile("other.map", "{}");

      CHECK(panel.deletePrefabFile(env.dir() / "sets" / "unread.map").is_error());
      CHECK(panel.deletePrefabFile(otherEnv.dir() / "other.map").is_error());
      CHECK(panel.removePrefabThumbnail(env.dir() / "sets" / "unread.map").is_error());
      CHECK(std::filesystem::exists(env.dir() / "sets" / "unread.map"));
      CHECK(std::filesystem::exists(env.dir() / "sets" / "unread.png"));
      CHECK(std::filesystem::exists(otherEnv.dir() / "other.map"));

      // an entry without a thumbnail has none to remove
      CHECK(panel.removePrefabThumbnail(lampPath).is_error());
      CHECK(std::filesystem::exists(lampPath));

      // nothing was rescanned
      QApplication::processEvents();
      CHECK(panel.scanCount() == 1u);
    }
  }

  SECTION("Refresh count and Apply to Selection")
  {
    auto env = fs::TestEnvironment{};
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
    auto& map = fixture.document().map();
    auto& library = map.prefabLibrary();

    // an entity snapshot and an assembly in the user root
    auto* lampNode =
      addEntity(map, {{"classname", "light"}, {"name", "lamp"}, {"light_radius", "300"}});
    auto* markerNode = addEntity(map, {{"classname", "info_null"}, {"name", "marker"}});
    REQUIRE(
      mdl::savePrefab(
        map,
        {lampNode},
        mdl::SavePrefabOptions{
          .name = "lamp", .category = "lights", .root = mdl::PrefabRoot::User})
      | kdl::is_success());
    const auto pairOptions =
      mdl::SavePrefabOptions{.name = "pair", .root = mdl::PrefabRoot::User};
    const auto pairPath =
      mdl::savePrefab(map, {lampNode, markerNode}, pairOptions) | kdl::value();

    auto panel = PrefabPanel{fixture.context()};
    panel.panelShown();
    REQUIRE(waitForScan(panel, library, 1));
    REQUIRE(library.entries().size() == 2u);

    auto* refreshButton = panel.findChild<QToolButton*>("PrefabPanel_refreshButton");
    auto* applyButton = panel.findChild<QToolButton*>("PrefabPanel_applyButton");
    auto* view = panel.findChild<PrefabBrowserView*>();
    REQUIRE(refreshButton != nullptr);
    REQUIRE(applyButton != nullptr);
    REQUIRE(view != nullptr);

    SECTION("The Refresh button counts the changed placements")
    {
      CHECK(panel.changedPlacementCount() == 0u);
      CHECK(refreshButton->text() == "Refresh");
      CHECK(!refreshButton->isEnabled());

      // a placement of the assembly, as the drop tool makes it
      const auto* pairEntry = library.find(mdl::PrefabRoot::User, "pair.map");
      REQUIRE(pairEntry != nullptr);
      auto* groupNode = placeAssembly(map, *pairEntry, "user/pair.map", pairEntry->hash);

      // the file changes on disk; the count follows the library without touching the
      // map
      auto tagged = pairOptions;
      tagged.tags = {"new"};
      REQUIRE(mdl::savePrefab(map, {lampNode, markerNode}, tagged) | kdl::is_success());
      panel.prefabWasSaved(pairPath);
      REQUIRE(QTest::qWaitFor([&] { return !library.scanning(); }));

      CHECK(panel.changedPlacementCount() == 1u);
      CHECK(refreshButton->text() == "Refresh (1)");
      CHECK(refreshButton->isEnabled());
      CHECK(
        groupNode->group().prefabHash()
        != library.find(mdl::PrefabRoot::User, "pair.map")->hash);

      // a change of the map is counted when the hidden panel is shown
      mdl::removeNodes(map, {groupNode});
      CHECK(panel.changedPlacementCount() == 1u);
      panel.panelShown();
      CHECK(panel.changedPlacementCount() == 0u);
      CHECK(refreshButton->text() == "Refresh");
    }

    SECTION("A visible panel counts after the outermost transaction only")
    {
      panel.show();
      QApplication::processEvents();

      const auto* pairEntry = library.find(mdl::PrefabRoot::User, "pair.map");
      REQUIRE(pairEntry != nullptr);
      const auto recounts = panel.recountCount();

      // a selection changes nothing that the button counts
      selectOnly(map, {lampNode});
      CHECK(panel.recountCount() == recounts);

      // a changed placement added in a nested transaction, as every step of a drag is
      map.startTransaction("Outer", mdl::TransactionScope::LongRunning);
      placeAssembly(map, *pairEntry, "user/pair.map", "an older hash");
      QApplication::processEvents();
      CHECK(panel.recountCount() == recounts);
      CHECK(refreshButton->text() == "Refresh");

      REQUIRE(map.commitTransaction());
      QApplication::processEvents();
      CHECK(panel.recountCount() == recounts + 1);
      CHECK(refreshButton->text() == "Refresh (1)");

      // an undo of a modification counts once
      map.undoCommand();
      QApplication::processEvents();
      CHECK(panel.recountCount() == recounts + 2);
      CHECK(refreshButton->text() == "Refresh");
      panel.hide();
    }

    SECTION("Apply to Selection needs an entity prefab and an entity of its class")
    {
      const auto* lampEntry = library.find(mdl::PrefabRoot::User, "lights/lamp.map");
      const auto* pairEntry = library.find(mdl::PrefabRoot::User, "pair.map");
      REQUIRE(lampEntry != nullptr);
      REQUIRE(pairEntry != nullptr);

      auto* otherLightNode = addEntity(
        map,
        {{"classname", "light"},
         {"name", "other"},
         {"light_radius", "64"},
         {"extra", "1"}});

      // no cell is selected
      CHECK(view->selectedEntry() == nullptr);
      selectOnly(map, {otherLightNode});
      CHECK(!applyButton->isEnabled());

      view->selectEntry(lampEntry->absolutePath);
      CHECK(view->selectedEntry() == lampEntry);
      CHECK(applyButton->isEnabled());

      // another class
      selectOnly(map, {markerNode});
      CHECK(!applyButton->isEnabled());

      // an assembly
      selectOnly(map, {otherLightNode});
      view->selectEntry(pairEntry->absolutePath);
      CHECK(!applyButton->isEnabled());

      // the snapshot's keys, the light's own name
      view->selectEntry(lampEntry->absolutePath);
      REQUIRE(applyButton->isEnabled());
      applyButton->click();
      CHECK(*otherLightNode->entity().property("light_radius") == "300");
      CHECK(*otherLightNode->entity().property("name") == "other");
      CHECK(otherLightNode->entity().property("extra") == nullptr);
    }
  }

  SECTION("The watcher")
  {
    auto env = fs::TestEnvironment{[](auto& e) {
      e.createDirectory("lights");
      e.createDirectory("empty/deeper");
    }};
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
    auto& map = fixture.document().map();
    auto& library = map.prefabLibrary();

    auto* entityNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    const auto lampPath =
      mdl::savePrefab(
        map,
        {entityNode},
        mdl::SavePrefabOptions{
          .name = "lamp", .category = "lights", .root = mdl::PrefabRoot::User})
      | kdl::value();

    // made before the panel, so that the watcher lets go of its folders before it is
    // deleted (R2-pf-ui-11)
    auto otherEnv = fs::TestEnvironment{[](auto& e) { e.createDirectory("doors"); }};

    auto panel = PrefabPanel{fixture.context()};
    panel.panelShown();
    REQUIRE(waitForScan(panel, library, 1));

    const auto* watcher = panel.findChild<QFileSystemWatcher*>("PrefabPanel_watcher");
    REQUIRE(watcher != nullptr);
    const auto watched = [](const QStringList& paths, const std::filesystem::path& path) {
      return paths.contains(pathAsQString(path.lexically_normal()));
    };

    // every folder under the root, an empty one too, and the prefab files
    CHECK(watched(watcher->directories(), env.dir()));
    CHECK(watched(watcher->directories(), env.dir() / "lights"));
    CHECK(watched(watcher->directories(), env.dir() / "empty"));
    CHECK(watched(watcher->directories(), env.dir() / "empty" / "deeper"));
    CHECK(watched(watcher->files(), lampPath));
    CHECK(watcher->directories().size() == 4);
    CHECK(watcher->files().size() == 1);

    SECTION("A closed panel watches nothing until it is shown again")
    {
      panel.panelHidden();
      CHECK(watcher->directories().empty());
      CHECK(watcher->files().empty());

      // the folders may have changed in the meantime
      panel.panelShown();
      REQUIRE(waitForScan(panel, library, 2));
      CHECK(watched(watcher->directories(), env.dir() / "empty" / "deeper"));
      CHECK(watcher->directories().size() == 4);
      CHECK(watcher->files().size() == 1);
    }

    SECTION("A save watches the saved file without a rescan")
    {
      panel.show();
      QApplication::processEvents();

      // into a watched folder, and into a folder that the save makes
      const auto savedPaths = std::vector<std::filesystem::path>{
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "lamp2", .category = "lights", .root = mdl::PrefabRoot::User})
          | kdl::value(),
        mdl::savePrefab(
          map,
          {entityNode},
          mdl::SavePrefabOptions{
            .name = "lamp3", .category = "new/deeper", .root = mdl::PrefabRoot::User})
          | kdl::value(),
      };
      for (const auto& savedPath : savedPaths)
      {
        panel.prefabWasSaved(savedPath);
      }
      REQUIRE(QTest::qWaitFor(
        [&] { return !library.scanning() && library.entries().size() == 3u; }));

      CHECK(watched(watcher->files(), savedPaths[0]));
      CHECK(watched(watcher->files(), savedPaths[1]));
      CHECK(watcher->files().size() == 3);
      CHECK(watched(watcher->directories(), env.dir() / "new"));
      CHECK(watched(watcher->directories(), env.dir() / "new" / "deeper"));
      CHECK(watcher->directories().size() == 6);

      // the watcher's report of the save does not rescan
      QTest::qWait(1500);
      CHECK(panel.scanCount() == 1u);
      panel.hide();
    }

    SECTION("A visible panel's delete scans once")
    {
      panel.show();
      QApplication::processEvents();

      CHECK(panel.deletePrefabFile(lampPath) | kdl::is_success());
      CHECK(QTest::qWaitFor(
        [&] {
          return panel.scanCount() == 2u && !library.scanning()
                 && library.entries().empty();
        },
        5000));

      // the watcher's report of the delete is part of that scan
      QTest::qWait(1000);
      CHECK(panel.scanCount() == 2u);
      panel.hide();
    }

    SECTION("A new root is watched instead of the old one")
    {
      library.setUserRoot(otherEnv.dir());
      panel.rescan();
      REQUIRE(waitForScan(panel, library, 2));

      CHECK(watched(watcher->directories(), otherEnv.dir()));
      CHECK(watched(watcher->directories(), otherEnv.dir() / "doors"));
      CHECK(watcher->directories().size() == 2);
      CHECK(watcher->files().empty());
    }

    SECTION("A file copied into a new empty folder rescans after the debounce")
    {
      // a hidden panel rescans when it is shown; a visible one after the debounce
      panel.show();
      QApplication::processEvents();

      const auto copyPath = env.dir() / "empty" / "deeper" / "copy.map";
      std::filesystem::copy_file(lampPath, copyPath);
      CHECK(QTest::qWaitFor(
        [&] {
          return panel.scanCount() > 1u && !library.scanning()
                 && library.find(mdl::PrefabRoot::User, "empty/deeper/copy.map");
        },
        5000));
      CHECK(watched(watcher->files(), copyPath));
      panel.hide();
    }
  }

  SECTION("The prompt after a load")
  {
    auto env = fs::TestEnvironment{};
    auto mapEnv = fs::TestEnvironment{};
    auto fixture = SidebarContextFixture{fixtureConfigWithUserRoot(env.dir())};
    auto& document = fixture.document();
    auto& map = document.map();

    // a placement of an assembly whose file changes afterwards
    auto* lampNode = addEntity(map, {{"classname", "light"}, {"name", "lamp"}});
    auto* markerNode = addEntity(map, {{"classname", "info_null"}, {"name", "marker"}});
    const auto pairOptions =
      mdl::SavePrefabOptions{.name = "pair", .root = mdl::PrefabRoot::User};
    const auto pairPath =
      mdl::savePrefab(map, {lampNode, markerNode}, pairOptions) | kdl::value();
    map.prefabLibrary().rescan();
    REQUIRE(QTest::qWaitFor([&] {
      map.prefabLibrary().processPendingResults();
      return !map.prefabLibrary().scanning();
    }));
    const auto* pairEntry = map.prefabLibrary().find(mdl::PrefabRoot::User, "pair.map");
    REQUIRE(pairEntry != nullptr);

    auto* groupNode = placeAssembly(map, *pairEntry, "user/pair.map", pairEntry->hash);
    mdl::removeNodes(map, {lampNode, markerNode});

    auto tagged = pairOptions;
    tagged.tags = {"new"};
    REQUIRE(mdl::savePrefab(map, {groupNode}, tagged) | kdl::is_success());

    const auto mapPath = mapEnv.dir() / "level.map";
    REQUIRE(map.saveTo(mapPath) | kdl::is_success());

    const auto environmentConfig = map.environmentConfig();
    const auto gameInfo = map.gameInfo();
    const auto mapFormat = map.worldNode().mapFormat();
    const auto worldBounds = map.worldBounds();
    const auto load = [&]() {
      REQUIRE(
        document.load(environmentConfig, gameInfo, mapFormat, worldBounds, mapPath)
        | kdl::is_success());
    };

    // the panel exists before the map's placements are checked, so it scans at once
    auto panel = PrefabPanel{fixture.context()};
    auto prompts = std::vector<std::vector<std::string>>{};
    panel.setOpenPromptForTesting([&](const auto& placements) {
      auto paths = std::vector<std::string>{};
      for (const auto& placement : placements)
      {
        paths.push_back(placement.prefabPath);
      }
      prompts.push_back(std::move(paths));
    });
    REQUIRE(panel.scanCount() == 1u);

    // never before the scan has finished; the scan is taken over without the event
    // loop, so that the prompt is posted but not shown
    CHECK(prompts.empty());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (document.map().prefabLibrary().scanning()
           && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      panel.panelShown();
    }
    REQUIRE_FALSE(document.map().prefabLibrary().scanning());
    CHECK(prompts.empty());

    // a load before the posted prompt runs: the prompt is for the old map, so it is
    // dropped, and the new map gets its own once its library is scanned
    load();
    QApplication::processEvents();
    CHECK(prompts.empty());
    CHECK(waitForScan(panel, document.map().prefabLibrary(), 2));
    CHECK(QTest::qWaitFor([&] { return prompts.size() == 1u; }));
    CHECK(prompts == std::vector<std::vector<std::string>>{{"user/pair.map"}});

    // once per load: a rescan does not ask again
    panel.rescan();
    CHECK(waitForScan(panel, document.map().prefabLibrary(), 3));
    QTest::qWait(100);
    CHECK(prompts.size() == 1u);

    SECTION("Not when every changed placement's file is missing")
    {
      REQUIRE(std::filesystem::remove(pairPath));
      load();
      CHECK(waitForScan(panel, document.map().prefabLibrary(), 4));
      QTest::qWait(100);
      CHECK(prompts.size() == 1u);
      // DD-24: the Refresh button counts it
      CHECK(panel.changedPlacementCount() == 1u);
    }

    SECTION("Not for a map without placements")
    {
      REQUIRE(document.create(environmentConfig, gameInfo, mapFormat, worldBounds));
      CHECK(waitForScan(panel, document.map().prefabLibrary(), 4));
      QTest::qWait(100);
      CHECK(prompts.size() == 1u);
      CHECK(panel.changedPlacementCount() == 0u);
    }
  }
}

} // namespace tb::ui
