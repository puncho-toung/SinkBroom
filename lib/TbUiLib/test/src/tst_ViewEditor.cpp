/*
 Copyright (C) 2026 Kristian Duske

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
#include <QButtonGroup>
#include <QCheckBox>
#include <QPushButton>
#include <QRadioButton>
#include <QtTest/QTest>

#include "base/PreferenceManager.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "prefs/Preferences.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/ViewEditor.h"

#include "kd/invoke.h"

#include <algorithm>
#include <ranges>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

namespace tb::ui
{
using Catch::Matchers::RangeEquals;

namespace
{

auto getDefinitionCheckBoxes(EntityDefinitionCheckBoxList& list)
{
  return list.findChildren<QCheckBox*>("entityDefinition_checkboxWidget");
}

auto getGroupCheckBoxes(EntityDefinitionCheckBoxList& list)
{
  return list.findChildren<QCheckBox*>() | std::views::filter([](const auto* checkBox) {
           return checkBox->objectName() != "entityDefinition_checkboxWidget";
         });
}

QCheckBox* findCheckBox(auto checkBoxes, const QString& text)
{
  if (const auto iGroupCheckBox = std::ranges::find_if(
        checkBoxes, [&](const auto* checkBox) { return checkBox->text() == text; });
      iGroupCheckBox != checkBoxes.end())
  {
    return *iGroupCheckBox;
  }

  return nullptr;
}

/**
 * The light volume radio named by the given value of Preferences::ShowLightVolumes. The
 * radios are found through their group because "all" also names an entity link radio.
 */
QRadioButton* findLightVolumesRadio(ViewEditor& editor, const std::string& value)
{
  const auto* selectedRadio = editor.findChild<QRadioButton*>(
    QString::fromStdString(Preferences::ShowLightVolumesSelected));
  if (selectedRadio && selectedRadio->group())
  {
    for (auto* button : selectedRadio->group()->buttons())
    {
      if (button->objectName() == QString::fromStdString(value))
      {
        return qobject_cast<QRadioButton*>(button);
      }
    }
  }
  return nullptr;
}

QPushButton* findButton(EntityDefinitionCheckBoxList& list, const QString& text)
{
  auto buttons = list.findChildren<QPushButton*>();
  if (const auto iButton = std::ranges::find_if(
        buttons, [&](const auto* button) { return button->text() == text; });
      iButton != buttons.end())
  {
    return *iButton;
  }

  return nullptr;
}

} // namespace

TEST_CASE("EntityDefinitionCheckBoxList")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  map.entityDefinitionManager().setDefinitions({
    {"func_zeta", {}, "", {}, tb::mdl::PointEntityDefinition{}},
    {"func_alpha", {}, "", {}, tb::mdl::PointEntityDefinition{}},
    {"trigger_once", {}, "", {}, tb::mdl::PointEntityDefinition{}},
  });

  auto list =
    EntityDefinitionCheckBoxList{map.entityDefinitionManager(), map.editorContext()};
  list.resize(400, 400);
  list.show();
  QApplication::processEvents();

  const auto* funcZetaDefinition = map.entityDefinitionManager().definition("func_zeta");
  const auto* funcAlphaDefinition =
    map.entityDefinitionManager().definition("func_alpha");
  const auto* triggerOnceDefinition =
    map.entityDefinitionManager().definition("trigger_once");

  REQUIRE(funcZetaDefinition != nullptr);
  REQUIRE(funcAlphaDefinition != nullptr);
  REQUIRE(triggerOnceDefinition != nullptr);

  SECTION("EntityDefinitionCheckBoxList")
  {
    SECTION("keeps definition order from group definitions")
    {
      const auto definitionCheckBoxes = getDefinitionCheckBoxes(list);

      REQUIRE(definitionCheckBoxes.size() == 3u);

      CHECK_THAT(
        definitionCheckBoxes | std::views::transform([](const auto* checkBox) {
          return checkBox->text().toStdString();
        }),
        RangeEquals({"alpha", "zeta", "once"}));
    }
  }

  SECTION("refresh")
  {
    map.editorContext().setEntityDefinitionHidden(*funcZetaDefinition, true);
    map.editorContext().setEntityDefinitionHidden(*funcAlphaDefinition, false);
    map.editorContext().setEntityDefinitionHidden(*triggerOnceDefinition, false);
    list.refresh();

    const auto* funcGroupCheckBox = findCheckBox(getGroupCheckBoxes(list), "Func");
    REQUIRE(funcGroupCheckBox != nullptr);
    CHECK(funcGroupCheckBox->checkState() == Qt::PartiallyChecked);
  }

  SECTION("groupCheckBoxChanged")
  {
    map.editorContext().setEntityDefinitionHidden(*funcZetaDefinition, true);
    map.editorContext().setEntityDefinitionHidden(*funcAlphaDefinition, true);
    list.refresh();

    auto* funcGroupCheckBox = findCheckBox(getGroupCheckBoxes(list), "Func");
    REQUIRE(funcGroupCheckBox != nullptr);

    QTest::mouseClick(funcGroupCheckBox, Qt::LeftButton);
    QApplication::processEvents();

    CHECK(!map.editorContext().entityDefinitionHidden(*funcZetaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*funcAlphaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*triggerOnceDefinition));
  }

  SECTION("defCheckBoxChanged")
  {
    auto* funcZetaCheckBox = findCheckBox(getDefinitionCheckBoxes(list), "zeta");
    REQUIRE(funcZetaCheckBox != nullptr);

    QTest::mouseClick(funcZetaCheckBox, Qt::LeftButton);
    QApplication::processEvents();

    CHECK(map.editorContext().entityDefinitionHidden(*funcZetaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*funcAlphaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*triggerOnceDefinition));
  }

  SECTION("showAllClicked and hideAllClicked")
  {
    auto* hideAllButton = findButton(list, "Hide all");
    auto* showAllButton = findButton(list, "Show all");

    REQUIRE(hideAllButton != nullptr);
    REQUIRE(showAllButton != nullptr);

    QTest::mouseClick(hideAllButton, Qt::LeftButton);
    QApplication::processEvents();

    CHECK(map.editorContext().entityDefinitionHidden(*funcZetaDefinition));
    CHECK(map.editorContext().entityDefinitionHidden(*funcAlphaDefinition));
    CHECK(map.editorContext().entityDefinitionHidden(*triggerOnceDefinition));

    QTest::mouseClick(showAllButton, Qt::LeftButton);
    QApplication::processEvents();

    CHECK(!map.editorContext().entityDefinitionHidden(*funcZetaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*funcAlphaDefinition));
    CHECK(!map.editorContext().entityDefinitionHidden(*triggerOnceDefinition));
  }
}

TEST_CASE("ViewEditor")
{
  auto& prefs = PreferenceManager::instance();
  prefs.resetToDefault(Preferences::ShowLightVolumes);
  prefs.resetToDefault(Preferences::LightVolumeHandleThreshold);

  // also when a REQUIRE fails, so that no later test case sees a changed preference
  const auto resetShowLightVolumes = kdl::invoke_later{[&] {
    prefs.resetToDefault(Preferences::ShowLightVolumes);
    prefs.resetToDefault(Preferences::LightVolumeHandleThreshold);
  }};

  auto fixture = MapDocumentFixture{};

  SECTION("lightVolumesChanged")
  {
    SECTION("the light volume radios read and write the preference")
    {
      auto& document = fixture.create(mdl::Doom3BFGFixtureConfig);

      auto editor = ViewEditor{document};
      editor.show();
      QApplication::processEvents();

      auto* offRadio = findLightVolumesRadio(editor, Preferences::ShowLightVolumesOff);
      auto* selectedRadio =
        findLightVolumesRadio(editor, Preferences::ShowLightVolumesSelected);
      auto* allRadio = findLightVolumesRadio(editor, Preferences::ShowLightVolumesAll);
      REQUIRE(offRadio != nullptr);
      REQUIRE(selectedRadio != nullptr);
      REQUIRE(allRadio != nullptr);

      CHECK(offRadio->text() == "Light volumes: Off");
      CHECK(selectedRadio->text() == "Light volumes: Selected");
      CHECK(allRadio->text() == "Light volumes: All");

      // the default
      CHECK(selectedRadio->isChecked());

      QTest::mouseClick(allRadio, Qt::LeftButton);
      QApplication::processEvents();
      CHECK(pref(Preferences::ShowLightVolumes) == Preferences::ShowLightVolumesAll);
      CHECK(allRadio->isChecked());

      QTest::mouseClick(offRadio, Qt::LeftButton);
      QApplication::processEvents();
      CHECK(pref(Preferences::ShowLightVolumes) == Preferences::ShowLightVolumesOff);

      QTest::mouseClick(selectedRadio, Qt::LeftButton);
      QApplication::processEvents();
      CHECK(pref(Preferences::ShowLightVolumes) == Preferences::ShowLightVolumesSelected);

      setPref(Preferences::ShowLightVolumes, Preferences::ShowLightVolumesAll);
      QApplication::processEvents();
      CHECK(allRadio->isChecked());
      CHECK(!offRadio->isChecked());

      // the light editor's handle threshold has no control here, but is restored too
      setPref(Preferences::LightVolumeHandleThreshold, 0.3f);

      auto buttons = editor.findChildren<QPushButton*>();
      const auto iRestoreDefaults = std::ranges::find_if(
        buttons, [](const auto* button) { return button->text() == "Restore Defaults"; });
      REQUIRE(iRestoreDefaults != buttons.end());

      QTest::mouseClick(*iRestoreDefaults, Qt::LeftButton);
      QApplication::processEvents();
      CHECK(pref(Preferences::ShowLightVolumes) == Preferences::ShowLightVolumesSelected);
      CHECK(selectedRadio->isChecked());
      CHECK(pref(Preferences::LightVolumeHandleThreshold) == 0.10f);
    }

    SECTION("a game without a light editor has no light volume radios")
    {
      auto& document = fixture.create(mdl::QuakeFixtureConfig);

      auto editor = ViewEditor{document};
      CHECK(
        editor.findChild<QRadioButton*>(
          QString::fromStdString(Preferences::ShowLightVolumesSelected))
        == nullptr);
      CHECK(
        editor.findChild<QRadioButton*>(
          QString::fromStdString(Preferences::ShowLightVolumesOff))
        == nullptr);
    }
  }

  SECTION("the light volume preferences")
  {
    // not under "Map view/", whose changes invalidate every map renderer cache
    CHECK(!Preferences::ShowLightVolumes.path.generic_string().starts_with("Map view"));
    CHECK(
      Preferences::ShowLightVolumes.defaultValue
      == Preferences::ShowLightVolumesSelected);
    CHECK(Preferences::LightVolumeAlpha.defaultValue == 0.45f);
    CHECK(Preferences::LightVolumeShells.defaultValue == 4);
    CHECK(!Preferences::LightVolumeHandleThreshold.path.generic_string().starts_with(
      "Map view"));
    CHECK(
      Preferences::LightVolumeHandleThreshold.path.generic_string()
      == "render/Light volume handle threshold");
    CHECK(Preferences::LightVolumeHandleThreshold.defaultValue == 0.10f);

    const auto colors = Preferences::colorPreferences();
    CHECK(
      std::ranges::find(colors, &Preferences::LightVolumeHandleColor) != colors.end());
  }
}

} // namespace tb::ui