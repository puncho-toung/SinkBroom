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

#include <QAbstractItemModel>
#include <QApplication>
#include <QStringList>
#include <QWidget>

#include "SidebarTestUtils.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "ui/CatchConfig.h"
#include "ui/EntityInspector.h"
#include "ui/EntityPropertyModel.h"
#include "ui/EntityPropertyTable.h"
#include "ui/MapDocument.h"
#include "ui/RenderView.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QStringList propertyKeys(const EntityPropertyTable& table)
{
  const auto* model = table.model();
  auto keys = QStringList{};
  for (int row = 0; row < model->rowCount(); ++row)
  {
    keys.push_back(model->index(row, EntityPropertyModel::ColumnKey).data().toString());
  }
  return keys;
}

void selectOnly(mdl::Map& map, mdl::Node* node)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, {node});
  QApplication::processEvents();
}

} // namespace

TEST_CASE("EntityInspector")
{
  auto fixture = SidebarContextFixture{};
  const auto context = fixture.context();
  auto panel = EntityInspector{context.appController, context.document};

  SECTION("panelId")
  {
    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "EntityInspector");
    CHECK(panel.panelId() == EntityInspector::PanelId);
  }

  SECTION("title")
  {
    CHECK(panel.title() == "Entity Inspector");
  }

  SECTION("iconPath")
  {
    CHECK(panel.iconPath() == "EntityInspector.svg");
    checkGreyscaleIcon(panel.iconPath());
  }

  SECTION("shownByDefault")
  {
    CHECK(panel.shownByDefault());
  }

  SECTION("minimumWidth")
  {
    // as wide as the inspector that held the page
    CHECK(panel.minimumWidth() == 350);
  }

  SECTION("the property table is not rebuilt while it cannot be seen")
  {
    auto& map = context.document.map();
    auto* firstNode =
      new mdl::EntityNode{mdl::Entity{{{"classname", "first"}, {"first_key", "1"}}}};
    auto* secondNode =
      new mdl::EntityNode{mdl::Entity{{{"classname", "second"}, {"second_key", "2"}}}};
    REQUIRE(!mdl::addNodes(map, {{&mdl::parentForNodes(map), {firstNode, secondNode}}})
               .empty());

    // the inspector in a window that never appears on the screen, which owns it; its GL
    // views stay hidden, since the tests provide no renderer resources
    auto window = QWidget{};
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(400, 800);
    auto* inspector =
      new EntityInspector{context.appController, context.document, &window};
    inspector->setGeometry(0, 0, 400, 800);
    for (auto* view : inspector->findChildren<RenderView*>())
    {
      view->hide();
    }

    const auto* table = inspector->findChild<EntityPropertyTable*>();
    REQUIRE(table != nullptr);

    // a window that was never shown keeps its table up to date
    selectOnly(map, firstNode);
    CHECK(propertyKeys(*table).contains("first_key"));

    window.show();
    QApplication::processEvents();

    // hidden, as in a closed dock or one behind another tab
    inspector->hide();
    selectOnly(map, secondNode);
    CHECK(propertyKeys(*table).contains("first_key"));
    CHECK(!propertyKeys(*table).contains("second_key"));

    inspector->show();
    QApplication::processEvents();
    CHECK(!propertyKeys(*table).contains("first_key"));
    CHECK(propertyKeys(*table).contains("second_key"));
  }
}

} // namespace tb::ui
