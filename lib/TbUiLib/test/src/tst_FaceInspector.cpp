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

#include <QLabel>
#include <QSettings>
#include <QStackedWidget>

#include "SidebarTestUtils.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/MapFormat.h"
#include "ui/CatchConfig.h"
#include "ui/FaceInspector.h"
#include "ui/MapDocument.h"
#include "ui/MaterialBrowser.h"
#include "ui/TabBar.h"
#include "ui/TabBook.h"

#include "kd/invoke.h"

#include "vm/bbox.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto TabBookKey = QString{"Windows/FaceInspector_TabBook/State"};

void removeTabBookKey()
{
  QSettings{}.remove(TabBookKey);
}

// the pages of the material tab book
constexpr auto MaterialsPage = 0;
constexpr auto UtilityPage = 1;

TabBook& tabBook(const FaceInspector& panel)
{
  auto* tabBook = panel.findChild<TabBook*>("FaceInspector_TabBook");
  REQUIRE(tabBook != nullptr);
  return *tabBook;
}

std::vector<QString> tabTitles(const FaceInspector& panel)
{
  auto titles = std::vector<QString>{};
  for (const auto* button : tabBook(panel).tabBar()->findChildren<TabBarButton*>())
  {
    const auto* label = button->findChild<QLabel*>();
    REQUIRE(label != nullptr);
    titles.push_back(label->text());
  }
  return titles;
}

/**
 * The page of the material tab book that the panel shows; the tab book's stacked layout
 * hides the other page.
 */
int currentPage(const FaceInspector& panel)
{
  const auto* materialsPage =
    panel.findChild<TabBookPage*>("FaceInspector_MaterialsPage");
  const auto* utilityPage = panel.findChild<TabBookPage*>("FaceInspector_UtilityPage");
  REQUIRE(materialsPage != nullptr);
  REQUIRE(utilityPage != nullptr);
  REQUIRE(materialsPage->isVisibleTo(&panel) != utilityPage->isVisibleTo(&panel));
  return utilityPage->isVisibleTo(&panel) ? UtilityPage : MaterialsPage;
}

/**
 * Whether the Utility tab shows its browser rather than the label of a game without
 * utility materials.
 */
bool showsUtilityBrowser(const FaceInspector& panel)
{
  const auto* stack = panel.findChild<QStackedWidget*>("FaceInspector_UtilityStack");
  REQUIRE(stack != nullptr);
  return stack->currentIndex() == 0;
}

const MaterialBrowser& materialBrowser(const FaceInspector& panel, const QString& name)
{
  const auto* browser = panel.findChild<MaterialBrowser*>("FaceInspector_" + name);
  REQUIRE(browser != nullptr);
  return *browser;
}

void createDocument(
  const SidebarContext& context, const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat)
{
  REQUIRE(context.document.create(
    context.appController.environmentConfig(), gameInfo, mapFormat, vm::bbox3d{8192.0}));
}

/**
 * The Doom 3 BFG fixture game without its materials on disk. Revealing a material lays
 * out the browser that lists it, which measures its titles with the renderer font, and
 * the tests have no font; with no materials, no browser has a title to measure.
 */
const mdl::GameInfo& doom3GameWithoutMaterials()
{
  // a map keeps a reference to its game, so the game must outlive every document
  static const auto gameInfo = [] {
    auto result = mdl::Doom3BFGGameInfo;
    result.gameConfig.materialConfig.root = "no_materials";
    return result;
  }();
  return gameInfo;
}

} // namespace

TEST_CASE("FaceInspector")
{
  // the panel restores its material tab when it is built and saves it when it goes:
  // start without the key, and leave none behind for the other cases and the user
  removeTabBookKey();
  const auto removeTabBookKeyLater = kdl::invoke_later{[] { removeTabBookKey(); }};

  auto fixture = SidebarContextFixture{};
  const auto context = fixture.context();
  auto panel = FaceInspector{context.appController, context.document};

  SECTION("panelId")
  {
    // keys the dock's place in the saved window state
    CHECK(panel.panelId() == "FaceInspector");
    CHECK(panel.panelId() == FaceInspector::PanelId);
  }

  SECTION("title")
  {
    CHECK(panel.title() == "Face Inspector");
  }

  SECTION("iconPath")
  {
    CHECK(panel.iconPath() == "FaceInspector.svg");
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

  SECTION("the material tabs")
  {
    CHECK(tabTitles(panel) == std::vector<QString>{"Materials", "Utility"});

    // without a saved tab, the Materials tab is shown
    CHECK(currentPage(panel) == MaterialsPage);
  }

  SECTION("the Utility tab follows the game")
  {
    // a game whose config declares no utility materials
    createDocument(context, mdl::QuakeGameInfo, mdl::MapFormat::Valve);
    CHECK(!showsUtilityBrowser(panel));
    const auto* label = panel.findChild<QLabel*>("FaceInspector_NoUtilityMaterials");
    REQUIRE(label != nullptr);
    CHECK(label->text() == "This game declares no utility materials.");

    // loading a document of a game with utility materials shows the browser, and a
    // game without them the label again
    createDocument(context, mdl::Doom3BFGGameInfo, mdl::MapFormat::Doom3_Valve);
    CHECK(showsUtilityBrowser(panel));

    createDocument(context, mdl::QuakeGameInfo, mdl::MapFormat::Valve);
    CHECK(!showsUtilityBrowser(panel));
  }

  SECTION("the Utility tab of a panel built for a game with utility materials")
  {
    createDocument(context, mdl::Doom3BFGGameInfo, mdl::MapFormat::Doom3_Valve);

    auto doom3Panel = FaceInspector{context.appController, context.document};
    CHECK(showsUtilityBrowser(doom3Panel));
  }

  SECTION("revealMaterial")
  {
    const auto caulk =
      gl::Material{"common/caulk", gl::createTextureResource(gl::Texture{4, 4})};
    const auto wall =
      gl::Material{"base_wall/x", gl::createTextureResource(gl::Texture{4, 4})};

    const auto& regularBrowser = materialBrowser(panel, "MaterialBrowser");
    const auto& utilityBrowser = materialBrowser(panel, "UtilityMaterialBrowser");

    SECTION("in a game with utility materials")
    {
      createDocument(context, doom3GameWithoutMaterials(), mdl::MapFormat::Doom3_Valve);

      // the tab that lists the material, which is selected there
      panel.revealMaterial(&caulk);
      CHECK(currentPage(panel) == UtilityPage);
      CHECK(utilityBrowser.selectedMaterial() == &caulk);

      panel.revealMaterial(&wall);
      CHECK(currentPage(panel) == MaterialsPage);
      CHECK(regularBrowser.selectedMaterial() == &wall);

      // the map view passes a null material for a face whose material is missing
      panel.revealMaterial(&caulk);
      REQUIRE(currentPage(panel) == UtilityPage);
      panel.revealMaterial(nullptr);
      CHECK(currentPage(panel) == MaterialsPage);
      CHECK(regularBrowser.selectedMaterial() == nullptr);
    }

    SECTION("in a game without utility materials")
    {
      createDocument(context, mdl::QuakeGameInfo, mdl::MapFormat::Valve);

      tabBook(panel).switchToPage(UtilityPage);
      panel.revealMaterial(&caulk);
      CHECK(currentPage(panel) == MaterialsPage);
      CHECK(regularBrowser.selectedMaterial() == &caulk);
    }
  }

  SECTION("the material tab is saved and restored")
  {
    {
      auto otherPanel = FaceInspector{context.appController, context.document};
      tabBook(otherPanel).switchToPage(UtilityPage);
    }
    CHECK(QSettings{}.contains(TabBookKey));

    auto restoredPanel = FaceInspector{context.appController, context.document};
    CHECK(currentPage(restoredPanel) == UtilityPage);
  }
}

} // namespace tb::ui
