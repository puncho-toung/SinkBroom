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
#include <QtTest/QTest>

#include "ui/CatchConfig.h"
#include "ui/TabBar.h"
#include "ui/TabBook.h"

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("TabBar")
{
  SECTION("tabClicked")
  {
    auto tabBook = TabBook{};
    tabBook.addPage(new TabBookPage{}, "First");
    tabBook.addPage(new TabBookPage{}, "Second");
    tabBook.show();
    QApplication::processEvents();

    auto pages = std::vector<int>{};
    QObject::connect(
      &tabBook, &TabBook::pageChanged, [&](const int index) { pages.push_back(index); });

    // each click with the pages shown by then
    auto clicks = std::vector<std::pair<int, std::vector<int>>>{};
    QObject::connect(tabBook.tabBar(), &TabBar::tabClicked, [&](const int index) {
      clicks.emplace_back(index, pages);
    });

    // a switch by the program changes the page and is no click
    tabBook.switchToPage(1);
    CHECK(pages == std::vector<int>{1});
    CHECK(clicks.empty());

    const auto buttons = tabBook.tabBar()->findChildren<TabBarButton*>();
    REQUIRE(buttons.size() == 2);

    // a click on a tab shows its page first
    QTest::mouseClick(buttons[0], Qt::LeftButton);
    CHECK(pages == std::vector<int>{1, 0});
    CHECK(
      clicks
      == std::vector<std::pair<int, std::vector<int>>>{{0, std::vector<int>{1, 0}}});

    // a click on the tab of the page that is shown changes no page, but is a click
    QTest::mouseClick(buttons[0], Qt::LeftButton);
    CHECK(pages == std::vector<int>{1, 0});
    REQUIRE(clicks.size() == 2);
    CHECK(clicks[1].first == 0);

    tabBook.switchToPage(0);
    tabBook.switchToPage(1);
    CHECK(clicks.size() == 2);
  }
}

} // namespace tb::ui
