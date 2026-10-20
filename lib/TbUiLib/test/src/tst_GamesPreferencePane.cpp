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
#include <QLineEdit>

#include "base/Preference.h"
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameInfo.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/GamesPreferencePane.h"
#include "ui/QPathUtils.h"

#include "kd/invoke.h"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("GamePreferencePane")
{
  auto appControllerFixture = AppControllerFixture{};
  auto env = fs::TestEnvironment{};

  // a preference of its own, which no other test sets, empty again afterwards
  auto gameInfo = mdl::detail::makeGameInfoFixture(mdl::DefaultGameInfo.gameConfig, {});
  gameInfo.prefabPathPreference = Preference<std::filesystem::path>{
    "Games/GamePreferencePaneTest/Prefab Path", std::filesystem::path{}};
  auto& prefabPath = gameInfo.prefabPathPreference;
  setPref(prefabPath, std::filesystem::path{});
  const auto clearPrefabPath =
    kdl::invoke_later{[&] { setPref(prefabPath, std::filesystem::path{}); }};

  auto pane = GamePreferencePane{appControllerFixture.appController(), nullptr, gameInfo};
  const auto* icon = pane.findChild<QAction*>("GamePreferencePane_prefabPathIcon");
  REQUIRE(icon != nullptr);

  SECTION("updateControls flags the stored Prefab Path")
  {
    // R8-ui-5: the box is filled under a signal blocker, which kept textChanged, and so
    // the icon, from following the stored value
    pane.updateControls();
    CHECK(icon->toolTip().isEmpty());
    CHECK(icon->icon().isNull());

    setPref(prefabPath, env.dir());
    pane.updateControls();
    CHECK(icon->toolTip().isEmpty());
    CHECK(icon->icon().isNull());

    setPref(prefabPath, env.dir() / "missing");
    pane.updateControls();
    CHECK(icon->toolTip() == "Directory not found");
    CHECK(!icon->icon().isNull());

    // R8-lib-4: the library takes a relative path for no path at all
    setPref(prefabPath, std::filesystem::path{"prefabs"});
    pane.updateControls();
    CHECK(icon->toolTip() == "Not an absolute path; the Prefab Path counts as unset");
    CHECK(!icon->icon().isNull());
  }

  SECTION("typing in the box flags the text")
  {
    auto* edit = qobject_cast<QLineEdit*>(icon->parent());
    REQUIRE(edit != nullptr);

    edit->setText("prefabs");
    CHECK(icon->toolTip() == "Not an absolute path; the Prefab Path counts as unset");

    edit->setText(pathAsQString(env.dir()));
    CHECK(icon->toolTip().isEmpty());

    edit->setText("");
    CHECK(icon->toolTip().isEmpty());
  }
}

} // namespace tb::ui
