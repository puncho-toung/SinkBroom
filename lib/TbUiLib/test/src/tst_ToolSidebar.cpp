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
#include <QImage>
#include <QLocale>
#include <QMainWindow>
#include <QPainter>
#include <QSettings>
#include <QSize>
#include <QSvgRenderer>
#include <QWidget>
#include <QWidgetAction>

#include "ui/CatchConfig.h"
#include "ui/ImageUtils.h"
#include "ui/QPathUtils.h"
#include "ui/SystemPaths.h"
#include "ui/ToolSidebar.h"

#include "kd/invoke.h"

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto ExpandedKey = QString{"Windows/MapWindow_ToolSidebar/Expanded"};

void removeExpandedKey()
{
  QSettings{}.remove(ExpandedKey);
}

QAction* spacerAction(const ToolSidebar& sidebar)
{
  const auto actions = sidebar.actions();
  REQUIRE(actions.size() >= 2);
  return actions[actions.size() - 2];
}

qint64 iconKey(const std::filesystem::path& imagePath)
{
  return loadSVGIcon(imagePath).cacheKey();
}

} // namespace

TEST_CASE("ToolSidebar")
{
  // the strip reads and writes its Expanded key: start without it, also when a REQUIRE
  // fails leave none behind, so that no other case and no settings of the user see it
  removeExpandedKey();
  const auto removeExpandedKeyLater = kdl::invoke_later{[] { removeExpandedKey(); }};

  SECTION("ToolSidebar")
  {
    auto sidebar = ToolSidebar{};
    CHECK(sidebar.objectName() == "MapWindow_ToolSidebar");
    CHECK(sidebar.windowTitle() == "Tool Sidebar");
    CHECK(sidebar.allowedAreas() == Qt::ToolBarAreas{Qt::LeftToolBarArea});
    CHECK(!sidebar.isMovable());
    CHECK(!sidebar.isFloatable());
    CHECK(sidebar.iconSize() == QSize{24, 24});
    CHECK(sidebar.toolButtonStyle() == Qt::ToolButtonIconOnly);

    const auto actions = sidebar.actions();
    REQUIRE(actions.size() == 2);
    CHECK(qobject_cast<QWidgetAction*>(actions[0]) != nullptr);
    CHECK(sidebar.entries().empty());

    SECTION("expanded when the key is absent")
    {
      CHECK(sidebar.expanded());
      CHECK(spacerAction(sidebar)->isVisible());
      CHECK(sidebar.expandAction()->text() == "Collapse Tool Sidebar");
      CHECK(
        sidebar.expandAction()->icon().cacheKey() == iconKey("ToolSidebarCollapse.svg"));

      // constructing writes nothing
      CHECK(!QSettings{}.contains(ExpandedKey));
    }

    SECTION("collapsed when the key is false")
    {
      QSettings{}.setValue(ExpandedKey, false);

      auto collapsed = ToolSidebar{};
      auto* spacer = spacerAction(collapsed);
      CHECK(!collapsed.expanded());
      CHECK(collapsed.actions() == QList<QAction*>{spacer, collapsed.expandAction()});
      CHECK(!spacer->isVisible());
      CHECK(collapsed.expandAction()->text() == "Expand Tool Sidebar");
      CHECK(
        collapsed.expandAction()->icon().cacheKey() == iconKey("ToolSidebarExpand.svg"));
    }
  }

  SECTION("expandAction")
  {
    auto sidebar = ToolSidebar{};
    auto* expander = sidebar.expandAction();
    CHECK(expander == sidebar.actions().last());
    CHECK(!expander->isCheckable());

    expander->trigger();
    CHECK(!sidebar.expanded());
    CHECK(!expander->isChecked());

    expander->trigger();
    CHECK(sidebar.expanded());
    CHECK(!expander->isChecked());
  }

  SECTION("addEntry")
  {
    auto apple = QAction{"Apple"};
    auto banana = QAction{"Banana"};
    auto cherry = QAction{"Cherry"};

    auto sidebar = ToolSidebar{};
    auto* spacer = spacerAction(sidebar);
    auto* expander = sidebar.expandAction();

    SECTION("sorts the entries and inserts them before the spacer while expanded")
    {
      sidebar.addEntry(&cherry);
      sidebar.addEntry(&apple);
      sidebar.addEntry(&banana);

      CHECK(sidebar.entries() == std::vector<QAction*>{&apple, &banana, &cherry});
      CHECK(
        sidebar.actions() == QList<QAction*>{&apple, &banana, &cherry, spacer, expander});
    }

    SECTION("puts an equal text after the existing entries")
    {
      auto secondApple = QAction{"&Apple"};
      sidebar.addEntry(&banana);
      sidebar.addEntry(&apple);
      sidebar.addEntry(&secondApple);

      CHECK(sidebar.entries() == std::vector<QAction*>{&apple, &secondApple, &banana});
      CHECK(
        sidebar.actions()
        == QList<QAction*>{&apple, &secondApple, &banana, spacer, expander});
    }

    SECTION("lists the entry but leaves it out of the strip while collapsed")
    {
      sidebar.setExpanded(false);
      sidebar.addEntry(&banana);
      sidebar.addEntry(&apple);

      CHECK(sidebar.entries() == std::vector<QAction*>{&apple, &banana});
      CHECK(sidebar.actions() == QList<QAction*>{spacer, expander});

      sidebar.setExpanded(true);
      CHECK(sidebar.actions() == QList<QAction*>{&apple, &banana, spacer, expander});
    }
  }

  SECTION("setExpanded")
  {
    auto apple = QAction{"Apple"};
    auto banana = QAction{"Banana"};

    auto sidebar = ToolSidebar{};
    sidebar.addEntry(&banana);
    sidebar.addEntry(&apple);
    auto* spacer = spacerAction(sidebar);
    auto* expander = sidebar.expandAction();

    auto changes = std::vector<bool>{};
    QObject::connect(&sidebar, &ToolSidebar::expandedChanged, [&](const bool expanded) {
      changes.push_back(expanded);
    });

    SECTION("removes and inserts the entries")
    {
      sidebar.setExpanded(false);
      CHECK(!sidebar.expanded());
      CHECK(sidebar.actions() == QList<QAction*>{spacer, expander});
      CHECK(sidebar.entries() == std::vector<QAction*>{&apple, &banana});
      CHECK(!spacer->isVisible());
      CHECK(expander->text() == "Expand Tool Sidebar");
      CHECK(expander->icon().cacheKey() == iconKey("ToolSidebarExpand.svg"));
      CHECK(QSettings{}.contains(ExpandedKey));
      CHECK(!QSettings{}.value(ExpandedKey).toBool());
      CHECK(changes == std::vector<bool>{false});

      // the entries are shared with menus: the strip never hides them
      CHECK(apple.isVisible());
      CHECK(banana.isVisible());

      sidebar.setExpanded(true);
      CHECK(sidebar.expanded());
      CHECK(sidebar.actions() == QList<QAction*>{&apple, &banana, spacer, expander});
      CHECK(spacer->isVisible());
      CHECK(expander->text() == "Collapse Tool Sidebar");
      CHECK(expander->icon().cacheKey() == iconKey("ToolSidebarCollapse.svg"));
      CHECK(QSettings{}.value(ExpandedKey).toBool());
      CHECK(changes == std::vector<bool>{false, true});
    }

    SECTION("does nothing for the current state")
    {
      sidebar.setExpanded(true);
      CHECK(sidebar.actions() == QList<QAction*>{&apple, &banana, spacer, expander});
      CHECK(!QSettings{}.contains(ExpandedKey));
      CHECK(changes.empty());
    }
  }

  SECTION("entries")
  {
    auto apple = QAction{"Apple"};
    auto* banana = new QAction{"Banana"};

    auto sidebar = ToolSidebar{};
    sidebar.addEntry(banana);
    sidebar.addEntry(&apple);
    auto* spacer = spacerAction(sidebar);
    auto* expander = sidebar.expandAction();

    // a destroyed entry leaves the strip and the list
    delete banana;
    CHECK(sidebar.entries() == std::vector<QAction*>{&apple});
    CHECK(sidebar.actions() == QList<QAction*>{&apple, spacer, expander});

    sidebar.setExpanded(false);
    sidebar.setExpanded(true);
    CHECK(sidebar.actions() == QList<QAction*>{&apple, spacer, expander});
  }

  SECTION("stays at the left edge when a window state from before it is restored")
  {
    // a window layout saved before the strip existed knows the top toolbar only (F7)
    auto window = QMainWindow{};
    window.setCentralWidget(new QWidget{});
    window.addToolBar("Toolbar")->setObjectName("MapWindowToolBar");
    const auto oldState = window.saveState();

    auto* sidebar = new ToolSidebar{};
    window.addToolBar(Qt::LeftToolBarArea, sidebar);
    REQUIRE(window.restoreState(oldState));

    CHECK(window.toolBarArea(sidebar) == Qt::LeftToolBarArea);
    CHECK(!sidebar->isHidden());
  }

  SECTION("the icons are greyscale 24x24 images")
  {
    for (const auto* name :
         {"ToolSidebarExpand.svg", "ToolSidebarCollapse.svg", "LightEditor.svg"})
    {
      CAPTURE(name);

      auto renderer = QSvgRenderer{pathAsQPath(
        SystemPaths::findResourceFile(std::filesystem::path{"images"} / name))};
      REQUIRE(renderer.isValid());
      CHECK(renderer.defaultSize() == QSize{24, 24});

      // the dark theme inverts an icon only if it is greyscale
      auto image = QImage{renderer.defaultSize(), QImage::Format_ARGB32_Premultiplied};
      image.fill(Qt::transparent);
      auto painter = QPainter{&image};
      renderer.render(&painter);
      painter.end();
      CHECK(image.isGrayscale());
    }
  }
}

TEST_CASE("toolSidebarEntryLess")
{
  const auto less = [](const QString& lhs, const QString& rhs) {
    return toolSidebarEntryLess(QAction{lhs}, QAction{rhs});
  };

  SECTION("ignores mnemonic ampersands")
  {
    CHECK(less("Apple", "&Banana"));
    CHECK(!less("&Banana", "Apple"));
    CHECK(less("&Banana", "Cherry"));
    CHECK(!less("&Apple", "Apple"));
    CHECK(!less("Apple", "&Apple"));
  }

  SECTION("is locale aware")
  {
    // An ICU collator follows the default locale, which is pinned here because a C or
    // POSIX locale would compare code points. A Windows build without ICU, like the one
    // this is developed with, compares in the user's locale whatever the default is.
    const auto previousLocale = QLocale{};
    QLocale::setDefault(QLocale{QLocale::English, QLocale::UnitedStates});
    const auto restoreLocale =
      kdl::invoke_later{[&] { QLocale::setDefault(previousLocale); }};

    // comparing code points puts every capital letter before every small one
    REQUIRE(QString{"Banana"} < QString{"apple"});

    CHECK(less("apple", "Banana"));
    CHECK(!less("Banana", "apple"));
  }
}

} // namespace tb::ui
