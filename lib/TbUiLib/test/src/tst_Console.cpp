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

#include <QTest>
#include <QTextEdit>

#include "SidebarTestUtils.h"
#include "base/Logger.h"
#include "ui/CatchConfig.h"
#include "ui/Console.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("Console")
{
  auto panel = Console{};

  SECTION("panelId")
  {
    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "Console");
    CHECK(panel.panelId() == Console::PanelId);
  }

  SECTION("title")
  {
    CHECK(panel.title() == "Console");
  }

  SECTION("iconPath")
  {
    CHECK(panel.iconPath() == "Console.svg");
    checkGreyscaleIcon(panel.iconPath());
  }

  SECTION("shownByDefault")
  {
    CHECK(panel.shownByDefault());
  }

  SECTION("defaultArea")
  {
    // where the info panel was, under the map views
    CHECK(panel.defaultArea() == Qt::BottomDockWidgetArea);
  }

  SECTION("minimumHeight")
  {
    // as tall as the info panel that held the page
    CHECK(panel.minimumHeight() == 100);
  }

  SECTION("log")
  {
    auto* textView = panel.findChild<QTextEdit*>();
    REQUIRE(textView != nullptr);

    // the panel is a logger; it shows the messages that it collects on its next tick
    Logger& logger = panel;
    logger.info() << "a line for the console";
    CHECK(QTest::qWaitFor(
      [&] { return textView->toPlainText().contains("a line for the console"); }));
  }
}

} // namespace tb::ui
