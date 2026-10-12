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

#include <QAbstractButton>
#include <QListWidget>

#include "TestEnvironment.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Assets.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MaterialCollectionEditor.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MaterialCollectionEditor")
{
  SECTION("removeMaterialCollectionsAtRows")
  {
    using Paths = std::vector<std::filesystem::path>;

    const auto collections = Paths{"textures", "textures/a", "textures/b", "textures/c"};

    CHECK(
      removeMaterialCollectionsAtRows(collections, {1})
      == Paths{"textures", "textures/b", "textures/c"});

    // an empty selection removes nothing
    CHECK(removeMaterialCollectionsAtRows(collections, {}) == collections);

    // rows come in the order the items were selected; erasing row 0 first would shift
    // row 2 onto the wrong collection, or past the end of a shorter list
    CHECK(
      removeMaterialCollectionsAtRows(collections, {2, 0})
      == Paths{"textures/a", "textures/c"});
    CHECK(removeMaterialCollectionsAtRows(collections, {0, 3, 1}) == Paths{"textures/b"});

    // a row that is selected twice is removed once
    CHECK(
      removeMaterialCollectionsAtRows(collections, {2, 2})
      == Paths{"textures", "textures/a", "textures/c"});

    // rows past the end, left by a list that the collections outlived, are ignored, and
    // so are the negative rows that QListWidget::row returns for foreign items
    CHECK(
      removeMaterialCollectionsAtRows(collections, {4, 1, 7})
      == Paths{"textures", "textures/b", "textures/c"});
    CHECK(removeMaterialCollectionsAtRows(collections, {-1}) == collections);
    CHECK(removeMaterialCollectionsAtRows(Paths{}, {0}) == Paths{});

    // every row
    CHECK(removeMaterialCollectionsAtRows(collections, {3, 2, 1, 0}) == Paths{});
  }

  SECTION("enabled collections list")
  {
    auto appControllerFixture = AppControllerFixture{};
    auto documentFixture = MapDocumentFixture{};
    auto& document = documentFixture.create(mdl::Quake2FixtureConfig);
    auto& map = document.map();

    auto editor = MaterialCollectionEditor{document};
    const auto lists = editor.findChildren<QListWidget*>();
    REQUIRE(lists.size() == 2);
    auto* enabledList = lists[1];

    // textures, textures/e1m1 and textures/e1m1/f1, all enabled
    const auto collections = mdl::enabledMaterialCollections(map);
    REQUIRE(collections.size() == 3);
    REQUIRE(enabledList->count() == 3);

    SECTION("removes the rows selected out of order")
    {
      enabledList->item(2)->setSelected(true);
      enabledList->item(0)->setSelected(true);

      auto selectedRows = std::vector<int>{};
      for (const auto* item : enabledList->selectedItems())
      {
        selectedRows.push_back(enabledList->row(item));
      }
      REQUIRE(selectedRows == std::vector<int>{2, 0});

      const auto buttons = editor.findChildren<QAbstractButton*>();
      const auto iRemoveButton = std::ranges::find_if(buttons, [](const auto* button) {
        return button->toolTip() == "Disable the selected material collections";
      });
      REQUIRE(iRemoveButton != buttons.end());
      (*iRemoveButton)->click();

      CHECK(mdl::enabledMaterialCollections(map) == std::vector{collections[1]});
      CHECK(enabledList->count() == 1);
    }

    SECTION("follows the collections when they change outside of a transaction")
    {
      // Map::setGamePath replaces the collections without a notification, so the rows
      // outlive them until the collections are reloaded
      map.setGamePath(getFixtureRoot() / "test/ui");
      REQUIRE(mdl::enabledMaterialCollections(map).empty());
      CHECK(enabledList->count() == 3);

      mdl::reloadMaterialCollections(map);
      CHECK(enabledList->count() == 0);
    }
  }
}

} // namespace tb::ui
