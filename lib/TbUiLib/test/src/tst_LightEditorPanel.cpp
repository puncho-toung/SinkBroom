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

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "SidebarTestUtils.h"
#include "base/NotifierConnection.h"
#include "base/PreferenceManager.h"
#include "fs/DiskFileSystem.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/CatchConfig.h"
#include "ui/ColorButton.h"
#include "ui/LightEditorPanel.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/RotateTool.h" // IWYU pragma: keep
#include "ui/ScaleTool.h"
#include "ui/SpinControl.h"
#include "ui/TabBar.h"
#include "ui/TabBook.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/vec.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

template <typename T>
T& findControl(const LightEditorPanel& panel, const QString& name)
{
  auto* control = panel.findChild<T*>("LightEditorPanel_" + name);
  REQUIRE(control != nullptr);
  return *control;
}

bool showsEditor(const LightEditorPanel& panel)
{
  const auto* stack = panel.findChild<QStackedWidget*>("LightEditorPanel_Stack");
  REQUIRE(stack != nullptr);
  return stack->currentIndex() == 1;
}

mdl::EntityNode* addLight(mdl::Map& map, std::vector<mdl::EntityProperty> properties)
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

std::vector<mdl::EntityProperty> withValue(
  std::vector<mdl::EntityProperty> properties,
  const std::string& key,
  const std::string& value)
{
  for (auto& property : properties)
  {
    if (property.key() == key)
    {
      property.setValue(value);
    }
  }
  return properties;
}

/**
 * A light with a complete emitter block at the defaults of its keys, of the given shape
 * and radius: every key is there, so that an edit writes only what it changes. Auto range
 * gives 96 * 3 = 288 at intensity 1.
 */
std::vector<mdl::EntityProperty> emitterProperties(
  const std::string& origin, const std::string& shape, const std::string& radius)
{
  return {
    {"classname", "light"},
    {"origin", origin},
    {"light_radius", radius},
    {"emitter", "1"},
    {"emit_shape", shape},
    {"emit_auto_range", "1"},
    {"emit_falloff", "invsquare_clamped"},
    {"emit_size", "96"},
    {"emit_start", "0"},
    {"emit_clamp", "1"},
    {"emit_cone", "60"},
    {"emit_penumbra", "0.2"},
    {"emit_square", "2"},
    {"emit_pattern", "none"},
    {"emit_pattern_strength", "0"},
    {"emit_pattern_scale", "1"},
    {"emit_pattern_phase", "0"},
    {"emit_effect", "none"},
    {"emit_effect_speed", "1"},
    {"emit_effect_amount", "1"},
    {"emit_effect_phase", "0"},
  };
}

/**
 * Whether the spin box shows "multi", the special value text of its minimum.
 */
bool showsMulti(const SpinControl& spin)
{
  return spin.specialValueText() == "multi" && spin.value() == spin.minimum();
}

const std::string* property(const mdl::EntityNode& entityNode, const std::string& key)
{
  return entityNode.entity().property(key);
}

std::string valueOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto* value = property(entityNode, key);
  REQUIRE(value != nullptr);
  return *value;
}

/**
 * Processes events until the predicate holds or two seconds have passed; returns whether
 * it holds.
 */
template <typename Predicate>
bool waitFor(const Predicate& predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
  {
    QTest::qWait(10);
  }
  return predicate();
}

// the pages of the editor's tab book
constexpr auto EmitterPage = 0;
constexpr auto TexturePage = 1;

bool showsEmitterPage(const LightEditorPanel& panel)
{
  return findControl<TabBookPage>(panel, "EmitterPage").isVisibleTo(&panel);
}

bool showsTexturePage(const LightEditorPanel& panel)
{
  return findControl<TabBookPage>(panel, "TexturePage").isVisibleTo(&panel);
}

std::vector<TabBarButton*> tabButtons(const LightEditorPanel& panel)
{
  const auto buttons =
    findControl<TabBook>(panel, "Tabs").tabBar()->findChildren<TabBarButton*>();
  REQUIRE(buttons.size() == 2);
  return {buttons.begin(), buttons.end()};
}

/**
 * Clicks the tab of a page of the editor's tab book, as the user does.
 */
void clickTab(const LightEditorPanel& panel, const int page)
{
  tabButtons(panel)[size_t(page)]->clicked();
  QApplication::processEvents();
}

/**
 * The titles of the sections of the given widget's form, in their order.
 */
QStringList sectionTitles(const QWidget& formWidget)
{
  const auto* form = qobject_cast<const QFormLayout*>(formWidget.layout());
  REQUIRE(form != nullptr);

  auto result = QStringList{};
  for (int row = 0; row < form->rowCount(); ++row)
  {
    auto* item = form->itemAt(row, QFormLayout::SpanningRole);
    auto* titleLayout = item ? qobject_cast<QVBoxLayout*>(item->layout()) : nullptr;
    if (titleLayout && titleLayout->count() > 0)
    {
      if (
        const auto* title = qobject_cast<const QLabel*>(titleLayout->itemAt(0)->widget()))
      {
        result.push_back(title->text());
      }
    }
  }
  return result;
}

} // namespace

TEST_CASE("LightEditorPanel")
{
  SECTION("panelId")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(panel.panelId() == "LightEditor");
  }

  SECTION("title")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(panel.title() == "Light Editor");
  }

  SECTION("iconPath")
  {
    auto fixture = SidebarContextFixture{};
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(panel.iconPath() == "LightEditor.svg");
  }

  SECTION("needs a game with a light editor")
  {
    // the Quake game config has no entities.lightEditor
    auto fixture = SidebarContextFixture{mdl::QuakeFixtureConfig};
    REQUIRE(!fixture.document().map().gameInfo().gameConfig.entityConfig.lightEditor);

    auto panel = LightEditorPanel{fixture.context()};
    CHECK(currentText(panel) == "This game has no light editor");
  }

  SECTION("follows the selection")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    const auto* playerStartDefinition =
      map.entityDefinitionManager().definition("info_player_start");
    REQUIRE(lightDefinition != nullptr);
    REQUIRE(playerStartDefinition != nullptr);

    auto panel = LightEditorPanel{fixture.context()};
    CHECK(currentText(panel) == "Select a light");

    auto* lightNode = mdl::createPointEntity(map, *lightDefinition, {0, 0, 64});
    auto* playerStartNode =
      mdl::createPointEntity(map, *playerStartDefinition, {64, 0, 64});
    REQUIRE(lightNode != nullptr);
    REQUIRE(playerStartNode != nullptr);

    // a group that holds a light: allEntities() would yield the light
    auto* groupedLightNode = mdl::createPointEntity(map, *lightDefinition, {0, 64, 64});
    REQUIRE(groupedLightNode != nullptr);
    auto* groupNode = mdl::groupSelectedNodes(map, "group");
    REQUIRE(groupNode != nullptr);

    const auto builder =
      mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    auto* brushNode = new mdl::BrushNode{
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 32, 32}}, "wall") | kdl::value()};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});

    const auto select = [&](const std::vector<mdl::Node*>& nodes) {
      mdl::deselectAll(map);
      if (!nodes.empty())
      {
        mdl::selectNodes(map, nodes);
      }
      QApplication::processEvents();
      return currentText(panel);
    };

    CHECK(select({}) == "Select a light");
    CHECK(select({brushNode}) == "Select a light");
    CHECK(select({playerStartNode}) == "Select a light");
    CHECK(select({groupNode}) == "Select a light");
    CHECK(select({brushNode, lightNode}) == "Select only lights");
    CHECK(select({playerStartNode, lightNode}) == "Select only lights");
    select({lightNode});
    CHECK(showsEditor(panel));
    CHECK(select({}) == "Select a light");
  }

  SECTION("follows a selected light that stops being a light")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);

    auto panel = LightEditorPanel{fixture.context()};
    REQUIRE(mdl::createPointEntity(map, *lightDefinition, {0, 0, 64}) != nullptr);
    QApplication::processEvents();
    REQUIRE(showsEditor(panel));

    // the selection stays the same; only the document changes
    REQUIRE(mdl::setEntityProperty(map, "classname", "func_static"));
    QApplication::processEvents();
    CHECK(currentText(panel) == "Select a light");
  }

  SECTION("shows the editor at once when a light is already selected")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);
    REQUIRE(mdl::createPointEntity(map, *lightDefinition, {0, 0, 64}) != nullptr);

    // no events processed: the constructor shows the right page
    auto panel = LightEditorPanel{fixture.context()};
    CHECK(showsEditor(panel));
  }

  SECTION("shows the values of the selected light")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"name", "lamp"},
        {"origin", "16 32 64"},
        {"light_radius", "200 100 50"},
        {"_color", "2 1 0"},
        {"angles", "0 90 0"},
        {"style", "3"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    CHECK(currentText(panel) == "Light: lamp");
    CHECK(findControl<QRadioButton>(panel, "Point").isChecked());
    CHECK_FALSE(findControl<QCheckBox>(panel, "EqualRadius").isChecked());

    // three radius components, no spot vectors
    CHECK_FALSE(findControl<SpinControl>(panel, "Radius").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "RadiusX").isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "TargetX").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "RadiusX").value() == 200.0);
    CHECK(findControl<SpinControl>(panel, "RadiusY").value() == 100.0);
    CHECK(findControl<SpinControl>(panel, "RadiusZ").value() == 50.0);

    CHECK(findControl<SpinControl>(panel, "OriginX").value() == 16.0);
    CHECK(findControl<SpinControl>(panel, "OriginY").value() == 32.0);
    CHECK(findControl<SpinControl>(panel, "OriginZ").value() == 64.0);
    CHECK(findControl<SpinControl>(panel, "AnglesX").value() == Catch::Approx(0.0));
    CHECK(findControl<SpinControl>(panel, "AnglesY").value() == Catch::Approx(90.0));

    // _color is the hue times the intensity
    CHECK(findControl<SpinControl>(panel, "Intensity").value() == 2.0);
    CHECK(findControl<QComboBox>(panel, "StyleChoice").currentText() == "3: Candle A");

    // without an emitter block, the texture page is shown and the emitter rows are not
    CHECK(showsTexturePage(panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "EmitSize").isVisibleTo(&panel));
  }

  SECTION("a radius edit changes light_radius and no other property")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "1.234 5 6"},
        {"light_radius", "300 300 300"},
        {"_color", "1 0.5 0.25"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    const auto before = lightNode->entity().properties();
    findControl<SpinControl>(panel, "Radius").setValue(500.0);
    QApplication::processEvents();

    CHECK(
      lightNode->entity().properties()
      == withValue(before, "light_radius", "500 500 500"));
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Radius");
  }

  SECTION("the equilateral box is off at first and shows the three components")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* equalNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});
    auto* unequalNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "64 0 0"}, {"light_radius", "316 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {equalNode});

    auto& equalRadius = findControl<QCheckBox>(panel, "EqualRadius");
    auto& radius = findControl<SpinControl>(panel, "Radius");
    auto& radiusY = findControl<SpinControl>(panel, "RadiusY");

    // three radii by default, also for equal ones (D52)
    CHECK_FALSE(equalRadius.isChecked());
    CHECK_FALSE(radius.isVisibleTo(&panel));
    CHECK(radiusY.isVisibleTo(&panel));

    // checking writes nothing new for equal radii and shows one radius
    auto modificationCount = map.modificationCount();
    equalRadius.click();
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK(valueOf(*equalNode, "light_radius") == "300 300 300");
    CHECK(equalRadius.isChecked());
    CHECK(radius.isVisibleTo(&panel));
    CHECK_FALSE(radiusY.isVisibleTo(&panel));

    // a light with unequal radii shows three
    selectOnly(map, {unequalNode});
    CHECK_FALSE(equalRadius.isChecked());
    CHECK_FALSE(radius.isVisibleTo(&panel));
    CHECK(radiusY.isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "RadiusX").value() == 316.0);

    // the choice lasts while the radii are equal
    selectOnly(map, {equalNode});
    CHECK(equalRadius.isChecked());
    CHECK(radius.isVisibleTo(&panel));

    // checking makes the components equal to the first one
    selectOnly(map, {unequalNode});
    equalRadius.click();
    QApplication::processEvents();
    CHECK(valueOf(*unequalNode, "light_radius") == "316 316 316");
    CHECK(equalRadius.isChecked());
    CHECK(radius.isVisibleTo(&panel));

    radius.setValue(400.0);
    QApplication::processEvents();
    CHECK(valueOf(*unequalNode, "light_radius") == "400 400 400");

    // unchecking writes nothing and shows the three components
    modificationCount = map.modificationCount();
    equalRadius.click();
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK_FALSE(equalRadius.isChecked());
    CHECK(radiusY.isVisibleTo(&panel));

    radiusY.setValue(350.0);
    QApplication::processEvents();
    CHECK(valueOf(*unequalNode, "light_radius") == "400 350 400");

    // and stays off
    selectOnly(map, {equalNode});
    CHECK_FALSE(equalRadius.isChecked());
    CHECK(radiusY.isVisibleTo(&panel));
  }

  SECTION("switching the type rewrites the properties like the engine")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 64"},
        {"light_radius", "200 200 200"},
        {"_color", "1 0 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // DefaultProjected has no explicit start and end, so three vectors
    findControl<QRadioButton>(panel, "Spot").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_target") == "0 0 -256");
    CHECK(valueOf(*lightNode, "light_up") == "0 -128 0");
    CHECK(valueOf(*lightNode, "light_right") == "-128 0 0");
    CHECK(property(*lightNode, "light_start") == nullptr);
    CHECK(property(*lightNode, "light_end") == nullptr);
    CHECK(property(*lightNode, "light_radius") == nullptr);
    CHECK(valueOf(*lightNode, "_color") == "1 0 0");
    CHECK(valueOf(*lightNode, "origin") == "0 0 64");

    CHECK(findControl<QRadioButton>(panel, "Spot").isChecked());
    CHECK(findControl<SpinControl>(panel, "TargetZ").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "TargetZ").value() == -256.0);
    CHECK_FALSE(findControl<SpinControl>(panel, "Radius").isVisibleTo(&panel));

    // explicit start and end points
    findControl<QCheckBox>(panel, "ExplicitStartEnd").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_start") == "0 0 0");
    CHECK(valueOf(*lightNode, "light_end") == "0 0 -256");
    CHECK(findControl<SpinControl>(panel, "EndZ").isVisibleTo(&panel));

    findControl<QRadioButton>(panel, "Sun").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "parallel") == "1");
    CHECK(valueOf(*lightNode, "light_radius") == "2048 2048 1024");
    for (const auto* key :
         {"light_target", "light_up", "light_right", "light_start", "light_end"})
    {
      CAPTURE(key);
      CHECK(property(*lightNode, key) == nullptr);
    }

    findControl<QRadioButton>(panel, "Point").click();
    QApplication::processEvents();
    CHECK(property(*lightNode, "parallel") == nullptr);
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    CHECK(valueOf(*lightNode, "_color") == "1 0 0");
  }

  SECTION("the color button and the intensity write _color")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    findControl<SpinControl>(panel, "Intensity").setValue(2.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "_color") == "2 2 2");

    // the hue times the intensity, with four decimals
    findControl<ColorButton>(panel, "Color").colorChangedByUser(QColor{255, 128, 0});
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "_color") == "2 1.0039 0");
    CHECK(findControl<SpinControl>(panel, "Intensity").value() == 2.0);
  }

  SECTION("the Emitter tab makes a texture light an emitter with the seed")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsTexturePage(panel));

    const auto before = lightNode->entity().properties();
    clickTab(panel, EmitterPage);
    CHECK(showsEmitterPage(panel));
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Kind");

    // the whole block of a new emitter; the radius stays, and the size follows it: 300 is
    // three sizes of 100 at intensity 1
    CHECK(lightNode->entity().properties().size() == before.size() + 18);
    CHECK(valueOf(*lightNode, "emitter") == "1");
    CHECK(valueOf(*lightNode, "emit_shape") == "omni");
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    CHECK(valueOf(*lightNode, "emit_size") == "100");
    CHECK(valueOf(*lightNode, "emit_start") == "0");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "1");
    CHECK(valueOf(*lightNode, "emit_falloff") == "invsquare_clamped");

    CHECK(findControl<SpinControl>(panel, "EmitSize").isVisibleTo(&panel));
    // an emitter omni has one Range field, as a spot has (G0 section 9); the cone belongs
    // to spot lights
    CHECK(findControl<SpinControl>(panel, "Range").isVisibleTo(&panel));
    CHECK(findControl<QLabel>(panel, "RangeLabel").text() == "Range (auto)");
    CHECK_FALSE(findControl<SpinControl>(panel, "ConeH").isVisibleTo(&panel));

    // the Texture tab writes emitter 0 alone and keeps the other properties
    const auto blockProperties = lightNode->entity().properties();
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(lightNode->entity().properties() == withValue(blockProperties, "emitter", "0"));

    // the Emitter tab gives the light its kept block back and writes nothing else
    clickTab(panel, EmitterPage);
    CHECK(showsEmitterPage(panel));
    CHECK(lightNode->entity().properties() == blockProperties);

    // a click on the tab of the page that is shown writes nothing
    const auto modificationCount = map.modificationCount();
    clickTab(panel, EmitterPage);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(showsEmitterPage(panel));
  }

  SECTION("the volume tools come first and each kind of light has its own page")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // top to bottom: the volume tools, the light's geometry and colour, the pages of
    // the two kinds of light, the flags (D50)
    auto& editor = findControl<QWidget>(panel, "Editor");
    auto& volumeTools = findControl<QWidget>(panel, "VolumeTools");
    auto& tabs = findControl<TabBook>(panel, "Tabs");
    auto* editorLayout = editor.layout();
    REQUIRE(editorLayout != nullptr);
    REQUIRE(editorLayout->count() == 4);
    CHECK(editorLayout->itemAt(0)->widget() == &volumeTools);
    auto* geometry = editorLayout->itemAt(1)->widget();
    CHECK(editorLayout->itemAt(2)->widget() == &tabs);
    auto* flags = editorLayout->itemAt(3)->widget();
    REQUIRE(geometry != nullptr);
    REQUIRE(flags != nullptr);

    // the volume tools: the tool buttons, then Reset and the handle threshold
    auto* toolRows = qobject_cast<QVBoxLayout*>(volumeTools.layout());
    REQUIRE(toolRows != nullptr);
    REQUIRE(toolRows->count() == 2);
    auto* toolRow = toolRows->itemAt(0)->layout();
    auto* resetRow = toolRows->itemAt(1)->layout();
    REQUIRE(toolRow != nullptr);
    REQUIRE(resetRow != nullptr);
    // the handles work without a button (D56)
    CHECK(toolRow->indexOf(&findControl<QToolButton>(panel, "DrawVolume")) == 0);
    CHECK(panel.findChild<QToolButton*>("LightEditorPanel_EditVolumeHandles") == nullptr);
    CHECK(resetRow->indexOf(&findControl<QToolButton>(panel, "ResetVolume")) == 0);
    CHECK(resetRow->indexOf(&findControl<SpinControl>(panel, "HandleThreshold")) == 3);

    CHECK(
      sectionTitles(*geometry) == QStringList{"Type", "Volume", "Transform", "Color"});
    CHECK(sectionTitles(*flags) == QStringList{"Misc"});
    for (const auto* name : {"Point", "Spot", "Sun"})
    {
      CAPTURE(name);
      CHECK(geometry->isAncestorOf(&findControl<QRadioButton>(panel, name)));
    }
    // an emitter spot's range and cone are in the Volume section, where the spot
    // vectors are, and with the range the rows that follow it: size and start
    for (const auto* name :
         {"RadiusX",
          "TargetX",
          "Range",
          "EmitSize",
          "EmitStart",
          "ConeH",
          "ConeV",
          "OriginX",
          "AnglesX",
          "Intensity"})
    {
      CAPTURE(name);
      CHECK(geometry->isAncestorOf(&findControl<SpinControl>(panel, name)));
    }
    CHECK(geometry->isAncestorOf(&findControl<QCheckBox>(panel, "AutoRange")));

    // together, in this order: Range, Auto range, Size, Start, then the line that says
    // what follows what (G0 section 9, range and reach in the panel)
    const auto* geometryForm = qobject_cast<const QFormLayout*>(geometry->layout());
    REQUIRE(geometryForm != nullptr);
    // the form wraps a field in a layout of its own
    const auto holds = [](QLayoutItem* item, const QWidget& widget) {
      if (!item)
      {
        return false;
      }
      if (item->widget() == &widget)
      {
        return true;
      }
      const auto* layout = item->layout();
      return layout && layout->indexOf(const_cast<QWidget*>(&widget)) >= 0;
    };
    const auto rowOf = [&](QWidget& widget) {
      for (int row = 0; row < geometryForm->rowCount(); ++row)
      {
        for (const auto role : {QFormLayout::FieldRole, QFormLayout::SpanningRole})
        {
          if (holds(geometryForm->itemAt(row, role), widget))
          {
            return row;
          }
        }
      }
      return -1;
    };
    const auto rangeRow = rowOf(findControl<SpinControl>(panel, "Range"));
    REQUIRE(rangeRow >= 0);
    CHECK(rowOf(findControl<QCheckBox>(panel, "AutoRange")) == rangeRow + 1);
    CHECK(rowOf(findControl<SpinControl>(panel, "EmitSize")) == rangeRow + 2);
    CHECK(rowOf(findControl<SpinControl>(panel, "EmitStart")) == rangeRow + 3);
    CHECK(rowOf(findControl<QLabel>(panel, "AutoRangeNote")) == rangeRow + 4);
    CHECK(geometry->isAncestorOf(&findControl<ColorButton>(panel, "Color")));
    for (const auto* name : {"CastShadows", "SkipSpecular"})
    {
      CAPTURE(name);
      CHECK(flags->isAncestorOf(&findControl<QCheckBox>(panel, name)));
    }

    // the Emitter page, then the Texture page, each with only its own controls
    const auto buttons = tabButtons(panel);
    const auto title = [](const TabBarButton* button) {
      const auto* label = button->findChild<QLabel*>();
      REQUIRE(label != nullptr);
      return label->text();
    };
    CHECK(title(buttons[EmitterPage]) == "Emitter");
    CHECK(title(buttons[TexturePage]) == "Texture");

    auto& emitterPage = findControl<TabBookPage>(panel, "EmitterPage");
    auto& texturePage = findControl<TabBookPage>(panel, "TexturePage");
    CHECK(tabs.isAncestorOf(&emitterPage));
    CHECK(tabs.isAncestorOf(&texturePage));

    // the falloff stays with the curve's other settings on the Emitter page
    auto emitterControls = std::vector<QWidget*>{
      &findControl<QComboBox>(panel, "Falloff"),
      &findControl<QComboBox>(panel, "Pattern"),
      &findControl<QCheckBox>(panel, "EnergyClamp"),
      &findControl<QComboBox>(panel, "Effect"),
    };
    for (const auto* name :
         {"Penumbra",
          "Squareness",
          "PatternStrength",
          "PatternScale",
          "PatternPhase",
          "EffectSpeed",
          "EffectAmount",
          "EffectPhase"})
    {
      emitterControls.push_back(&findControl<SpinControl>(panel, name));
    }
    for (auto* control : emitterControls)
    {
      CAPTURE(control->objectName().toStdString());
      CHECK(emitterPage.isAncestorOf(control));
      CHECK_FALSE(texturePage.isAncestorOf(control));
    }
    for (const auto* name : {"Texture", "StyleChoice"})
    {
      CAPTURE(name);
      auto& control = findControl<QComboBox>(panel, name);
      CHECK(texturePage.isAncestorOf(&control));
      CHECK_FALSE(emitterPage.isAncestorOf(&control));
    }

    // the curve controls of the old page are gone, the tab is the switch, the style a
    // list
    for (const auto* name :
         {"LightEditorPanel_FalloffExp",
          "LightEditorPanel_InnerRadius",
          "LightEditorPanel_ShapeExp"})
    {
      CAPTURE(name);
      CHECK(panel.findChild<QWidget*>(name) == nullptr);
    }
    CHECK(panel.findChild<SpinControl*>("LightEditorPanel_Style") == nullptr);
  }

  SECTION("the page follows the selected lights and switching it writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* emitterNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* textureNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});
    // a texture light that kept its emitter block
    auto* keptNode = addLight(
      map,
      withValue(emitterProperties("128 0 0", "omni", "288 288 288"), "emitter", "0"));

    auto panel = LightEditorPanel{fixture.context()};
    auto& mixedNotice = findControl<QLabel>(panel, "MixedEmitterNotice");
    auto& texturePage = findControl<TabBookPage>(panel, "TexturePage");
    const auto modificationCount = map.modificationCount();
    const auto emitterBefore = emitterNode->entity().properties();
    const auto textureBefore = textureNode->entity().properties();
    const auto keptBefore = keptNode->entity().properties();

    selectOnly(map, {emitterNode});
    CHECK(showsEmitterPage(panel));
    CHECK_FALSE(showsTexturePage(panel));
    CHECK_FALSE(mixedNotice.isVisibleTo(&panel));
    CHECK(findControl<QComboBox>(panel, "Falloff").isVisibleTo(&panel));

    selectOnly(map, {textureNode});
    CHECK(showsTexturePage(panel));
    CHECK_FALSE(showsEmitterPage(panel));
    CHECK(findControl<QComboBox>(panel, "Texture").isVisibleTo(&panel));

    // kept emitter properties do not make an emitter
    selectOnly(map, {keptNode});
    CHECK(showsTexturePage(panel));

    // lights of both kinds: the Emitter page with the notice, the texture options off
    selectOnly(map, {emitterNode, textureNode});
    CHECK(showsEmitterPage(panel));
    CHECK(mixedNotice.isVisibleTo(&panel));
    CHECK_FALSE(findControl<QComboBox>(panel, "Falloff").isVisibleTo(&panel));
    CHECK_FALSE(texturePage.isEnabled());
    CHECK_FALSE(findControl<QComboBox>(panel, "Texture").isEnabled());

    selectOnly(map, {emitterNode});
    CHECK_FALSE(mixedNotice.isVisibleTo(&panel));
    CHECK(texturePage.isEnabled());

    // no light was written
    CHECK(map.modificationCount() == modificationCount);
    CHECK(emitterNode->entity().properties() == emitterBefore);
    CHECK(textureNode->entity().properties() == textureBefore);
    CHECK(keptNode->entity().properties() == keptBefore);
  }

  SECTION("only the page that is shown sizes the tab book")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* emitterNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* textureNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& emitterPage = findControl<TabBookPage>(panel, "EmitterPage");
    auto& texturePage = findControl<TabBookPage>(panel, "TexturePage");
    const auto sizedBy = [](const QWidget& page) {
      return page.sizePolicy().horizontalPolicy() != QSizePolicy::Ignored
             && page.sizePolicy().verticalPolicy() != QSizePolicy::Ignored;
    };
    const auto ignored = [](const QWidget& page) {
      return page.sizePolicy().horizontalPolicy() == QSizePolicy::Ignored
             && page.sizePolicy().verticalPolicy() == QSizePolicy::Ignored;
    };

    selectOnly(map, {textureNode});
    REQUIRE(showsTexturePage(panel));
    CHECK(sizedBy(texturePage));
    CHECK(ignored(emitterPage));

    selectOnly(map, {emitterNode});
    REQUIRE(showsEmitterPage(panel));
    CHECK(sizedBy(emitterPage));
    CHECK(ignored(texturePage));

    // also after a click on a tab
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(sizedBy(texturePage));
    CHECK(ignored(emitterPage));
  }

  SECTION("the Emitter tab keeps a kept block and the keys no light reads")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // an emitter block that the engine wrote with "%f" and switched off
    auto* engineNode = addLight(
      map,
      withValue(
        withValue(emitterProperties("0 0 0", "omni", "300 300 300"), "emitter", "0"),
        "emit_size",
        "96.000000"));
    // a texture light that carries keys no light reads
    auto* unknownNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "64 0 0"},
        {"light_radius", "300 300 300"},
        {"lamp_label", "kitchen"},
        {"lamp_level", "2"},
      });

    auto panel = LightEditorPanel{fixture.context()};

    // only emitter is written, and the kept values stay as they are written
    selectOnly(map, {engineNode});
    REQUIRE(showsTexturePage(panel));
    const auto engineBefore = engineNode->entity().properties();
    clickTab(panel, EmitterPage);
    CHECK(engineNode->entity().properties() == withValue(engineBefore, "emitter", "1"));
    CHECK(valueOf(*engineNode, "emit_size") == "96.000000");

    // a texture light becomes a new emitter, and the keys that no light reads stay as
    // they are, there and back
    selectOnly(map, {unknownNode});
    REQUIRE(showsTexturePage(panel));
    clickTab(panel, EmitterPage);
    CHECK(showsEmitterPage(panel));
    CHECK(valueOf(*unknownNode, "emitter") == "1");
    CHECK(valueOf(*unknownNode, "lamp_label") == "kitchen");
    CHECK(valueOf(*unknownNode, "lamp_level") == "2");

    clickTab(panel, TexturePage);
    CHECK(valueOf(*unknownNode, "emitter") == "0");
    CHECK(valueOf(*unknownNode, "lamp_label") == "kitchen");
    CHECK(valueOf(*unknownNode, "lamp_level") == "2");
  }

  SECTION("the Emitter tab tests a kept block's range when it brings the block back")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // a kept block whose range was set by hand: 500 where its auto range is 288
    auto* handSetNode = addLight(
      map,
      withValue(
        withValue(emitterProperties("0 0 0", "omni", "500 500 500"), "emitter", "0"),
        "emit_auto_range",
        "1"));
    // and one in step with it
    auto* inStepNode = addLight(
      map,
      withValue(
        withValue(emitterProperties("64 0 0", "omni", "288 288 288"), "emitter", "0"),
        "emit_auto_range",
        "1"));

    auto panel = LightEditorPanel{fixture.context()};

    // DP-T5-02: the test is made at the switch, so the stored "1" is not kept
    selectOnly(map, {handSetNode});
    REQUIRE(showsTexturePage(panel));
    clickTab(panel, EmitterPage);
    CHECK(valueOf(*handSetNode, "emitter") == "1");
    CHECK(valueOf(*handSetNode, "emit_auto_range") == "0");
    CHECK(valueOf(*handSetNode, "light_radius") == "500 500 500");
    CHECK_FALSE(findControl<QCheckBox>(panel, "AutoRange").isChecked());

    selectOnly(map, {inStepNode});
    const auto inStepBefore = inStepNode->entity().properties();
    clickTab(panel, EmitterPage);
    CHECK(inStepNode->entity().properties() == withValue(inStepBefore, "emitter", "1"));
    CHECK(findControl<QCheckBox>(panel, "AutoRange").isChecked());
  }

  SECTION("the tabs switch every selected light")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* emitterNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* textureNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});
    // as the Texture tab leaves an emitter
    auto* keptNode = addLight(
      map,
      withValue(emitterProperties("128 0 0", "omni", "288 288 288"), "emitter", "0"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {emitterNode, textureNode, keptNode});
    REQUIRE(showsEmitterPage(panel));

    const auto emitterBefore = emitterNode->entity().properties();
    const auto keptBefore = keptNode->entity().properties();

    // in one command: the texture light gets the seed, the kept block its values
    const auto modificationCount = map.modificationCount();
    clickTab(panel, EmitterPage);
    CHECK(map.modificationCount() == modificationCount + 1);
    CHECK(emitterNode->entity().properties() == emitterBefore);
    CHECK(valueOf(*textureNode, "emitter") == "1");
    CHECK(valueOf(*textureNode, "emit_size") == "100");
    CHECK(keptNode->entity().properties() == withValue(keptBefore, "emitter", "1"));
    CHECK_FALSE(findControl<QLabel>(panel, "MixedEmitterNotice").isVisibleTo(&panel));

    // the Texture tab keeps every emit_* key but emitter
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(emitterNode->entity().properties() == withValue(emitterBefore, "emitter", "0"));
    CHECK(keptNode->entity().properties() == keptBefore);
    CHECK(valueOf(*textureNode, "emitter") == "0");
    CHECK(valueOf(*textureNode, "emit_size") == "100");
  }

  SECTION("a tab click after the selection became something else writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});
    auto* playerStartNode = new mdl::EntityNode{
      mdl::Entity{{{"classname", "info_player_start"}, {"origin", "64 0 0"}}}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {playerStartNode}}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsTexturePage(panel));

    const auto lightProperties = lightNode->entity().properties();
    const auto playerStartProperties = playerStartNode->entity().properties();

    // the panel has not updated yet (D22)
    mdl::deselectAll(map);
    mdl::selectNodes(map, {playerStartNode});
    tabButtons(panel)[EmitterPage]->clicked();
    CHECK(lightNode->entity().properties() == lightProperties);
    CHECK(playerStartNode->entity().properties() == playerStartProperties);

    QApplication::processEvents();
    CHECK(currentText(panel) == "Select a light");
  }

  SECTION("the effect writes emit_effect and the texture style style")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsEmitterPage(panel));

    // none, the three waveforms, a separator and the engine's eleven styles (F-T-13);
    // each item holds its effect's value
    auto& effect = findControl<QComboBox>(panel, "Effect");
    REQUIRE(effect.count() == 16);
    CHECK(effect.itemText(0) == "None");
    CHECK(effect.itemText(1) == "Strobe");
    CHECK(effect.itemText(2) == "Pulse");
    CHECK(effect.itemText(3) == "Flicker");
    CHECK_FALSE(effect.itemData(4).isValid());
    CHECK(effect.itemText(5) == "Flicker A");
    CHECK(effect.itemText(15) == "Slow Pulse (no black)");
    for (int value = 0; value < mdl::EmitterEffectCount; ++value)
    {
      CAPTURE(value);
      const auto index = effect.findData(value);
      REQUIRE(index != -1);
      CHECK(
        effect.itemText(index).toStdString()
        == std::string{mdl::emitterEffectDisplayName(value)});
    }
    CHECK(effect.currentIndex() == 0);
    // the speed, the amount and the phase only with an effect
    CHECK_FALSE(findControl<SpinControl>(panel, "EffectSpeed").isVisibleTo(&panel));

    // fast_strobe, effect 4, is the fourth style after the separator
    const auto fastStrobe = effect.findData(4);
    REQUIRE(fastStrobe == 8);
    const auto before = lightNode->entity().properties();
    effect.setCurrentIndex(fastStrobe);
    effect.activated(fastStrobe);
    QApplication::processEvents();
    CHECK(
      lightNode->entity().properties()
      == withValue(before, "emit_effect", "fast_strobe"));
    CHECK(property(*lightNode, "style") == nullptr);
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Effect");
    CHECK(effect.currentIndex() == fastStrobe);
    CHECK(findControl<SpinControl>(panel, "EffectSpeed").isVisibleTo(&panel));

    // the texture page's style still writes style; the effect is kept
    clickTab(panel, TexturePage);
    auto& styleChoice = findControl<QComboBox>(panel, "StyleChoice");
    const auto index = styleChoice.findData("4");
    REQUIRE(index != -1);
    styleChoice.setCurrentIndex(index);
    styleChoice.activated(index);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "style") == "4");
    CHECK(valueOf(*lightNode, "emit_effect") == "fast_strobe");
  }

  SECTION("the effect shows multi where the selected lights differ")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto emitterWith = [&](const std::string& origin, const std::string& effect) {
      return addLight(
        map,
        withValue(
          emitterProperties(origin, "omni", "288 288 288"), "emit_effect", effect));
    };
    auto* strobeNode = emitterWith("0 0 0", "strobe");
    auto* otherStrobeNode = emitterWith("64 0 0", "strobe");
    auto* plainNode = emitterWith("128 0 0", "none");

    auto panel = LightEditorPanel{fixture.context()};
    auto& effect = findControl<QComboBox>(panel, "Effect");

    selectOnly(map, {strobeNode, plainNode});
    REQUIRE(showsEmitterPage(panel));
    CHECK(effect.currentIndex() == -1);
    CHECK(effect.placeholderText() == "multi");

    selectOnly(map, {strobeNode, otherStrobeNode});
    CHECK(effect.currentIndex() == 1);
    CHECK(effect.currentText() == "Strobe");

    // a multi field is written to every light when it is edited
    selectOnly(map, {strobeNode, plainNode});
    effect.setCurrentIndex(0);
    effect.activated(0);
    QApplication::processEvents();
    CHECK(valueOf(*strobeNode, "emit_effect") == "none");
    CHECK(valueOf(*plainNode, "emit_effect") == "none");
    CHECK(effect.currentIndex() == 0);
  }

  SECTION("the handle threshold spin writes the preference")
  {
    auto& prefs = PreferenceManager::instance();
    prefs.resetToDefault(Preferences::LightVolumeHandleThreshold);
    // also when a REQUIRE fails, so that no later test case sees a changed preference
    const auto resetThreshold = kdl::invoke_later{
      [&] { prefs.resetToDefault(Preferences::LightVolumeHandleThreshold); }};

    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // in percent (D53)
    auto& threshold = findControl<SpinControl>(panel, "HandleThreshold");
    CHECK(threshold.value() == 10.0);
    CHECK(threshold.minimum() == 1.0);
    CHECK(threshold.maximum() == 50.0);

    // not a light property; written once the steps pause
    const auto modificationCount = map.modificationCount();
    threshold.setValue(25.0);
    QApplication::processEvents();
    CHECK(waitFor([] { return pref(Preferences::LightVolumeHandleThreshold) == 0.25f; }));
    CHECK(map.modificationCount() == modificationCount);
    CHECK(threshold.value() == 25.0);

    // a burst of steps is written once, with its last value, which a refresh keeps
    // while the write waits
    auto thresholdChanges = 0;
    auto notifierConnection = NotifierConnection{};
    notifierConnection +=
      prefs.preferenceDidChangeNotifier.connect([&](const auto& path) {
        if (path == Preferences::LightVolumeHandleThreshold.path)
        {
          ++thresholdChanges;
        }
      });
    threshold.setValue(20.0);
    threshold.setValue(30.0);
    threshold.setValue(35.0);
    selectOnly(map, {lightNode});
    CHECK(threshold.value() == 35.0);
    CHECK(waitFor([] { return pref(Preferences::LightVolumeHandleThreshold) == 0.35f; }));
    QApplication::processEvents();
    CHECK(thresholdChanges == 1);
    CHECK(threshold.value() == 35.0);

    setPref(Preferences::LightVolumeHandleThreshold, 0.4f);
    QApplication::processEvents();
    CHECK(threshold.value() == 40.0);
  }

  SECTION("of the preferences, only the game path reads the texture list again")
  {
    auto& prefs = PreferenceManager::instance();
    // also when a check fails, so that no later test case sees a changed preference
    const auto resetPreferences = kdl::invoke_later{[&] {
      prefs.resetToDefault(Preferences::CameraFlyMoveSpeed);
      prefs.resetToDefault(Preferences::LightVolumeHandleThreshold);
    }};

    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // an item that only reading the list again removes
    auto& texture = findControl<QComboBox>(panel, "Texture");
    texture.addItem("marker");
    const auto hasMarker = [&]() { return texture.findText("marker") != -1; };

    // as a tick of the fly speed wheel does
    setPref(Preferences::CameraFlyMoveSpeed, 2.0f);
    QApplication::processEvents();
    CHECK(hasMarker());

    // the threshold refreshes the panel
    auto& threshold = findControl<SpinControl>(panel, "HandleThreshold");
    threshold.setValue(25.0);
    REQUIRE(
      waitFor([] { return pref(Preferences::LightVolumeHandleThreshold) == 0.25f; }));
    QApplication::processEvents();
    CHECK(hasMarker());
    CHECK(threshold.value() == 25.0);

    // the game path reloads the materials; there are none below base/base
    auto gamePath = map.gameInfo().gamePathPreference;
    const auto resetGamePath = kdl::invoke_later{[&] { prefs.resetToDefault(gamePath); }};
    setPref(gamePath, gamePath.defaultValue / "base");
    QApplication::processEvents();
    CHECK_FALSE(hasMarker());
    CHECK(texture.count() == 1);
  }

  SECTION("the angles are written by the rotation policy")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_rotation", "1 0 0 0 1 0 0 0 1"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    findControl<SpinControl>(panel, "AnglesY").setValue(90.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "angles") == "0 90 0");
    CHECK(property(*lightNode, "light_rotation") == nullptr);
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Angles");
  }

  SECTION("an edit reaches every selected light and keeps their origins")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode1 = addLight(
      map,
      {{"classname", "light"}, {"origin", "1.234 5 6"}, {"light_radius", "300 300 300"}});
    auto* lightNode2 = addLight(
      map, {{"classname", "light"}, {"origin", "-100.555 20.111 30"}, {"light", "200"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode1, lightNode2});
    REQUIRE(showsEditor(panel));
    CHECK(currentText(panel) == "2 lights selected: edits apply to all");

    const auto modificationCount = map.modificationCount();
    findControl<SpinControl>(panel, "Radius").setValue(500.0);
    QApplication::processEvents();

    // one command for both lights
    CHECK(map.modificationCount() == modificationCount + 1);
    CHECK(valueOf(*lightNode1, "light_radius") == "500 500 500");
    CHECK(valueOf(*lightNode2, "light_radius") == "500 500 500");
    CHECK(valueOf(*lightNode1, "origin") == "1.234 5 6");
    CHECK(valueOf(*lightNode2, "origin") == "-100.555 20.111 30");
  }

  SECTION("an edit that changes nothing writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    const auto undoName = [&]() {
      const auto* name = map.undoCommandName();
      return name ? *name : std::string{};
    };
    const auto modificationCount = map.modificationCount();
    const auto undoNameBefore = undoName();

    // re-entering the value that the light has
    auto& radius = findControl<SpinControl>(panel, "Radius");
    radius.valueChanged(radius.value());
    QApplication::processEvents();

    CHECK(map.modificationCount() == modificationCount);
    CHECK(undoName() == undoNameBefore);
  }

  SECTION("follows a change of the light and its undo")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& radius = findControl<SpinControl>(panel, "Radius");
    REQUIRE(radius.value() == 300.0);

    REQUIRE(mdl::setEntityProperty(map, "light_radius", "400 400 400"));
    QApplication::processEvents();
    CHECK(radius.value() == 400.0);

    map.undoCommand();
    QApplication::processEvents();
    CHECK(radius.value() == 300.0);

    // the update after an undo writes nothing, so the redo stack stays (D47)
    CHECK(map.canRedoCommand());
    map.redoCommand();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "400 400 400");
    CHECK(radius.value() == 400.0);
  }

  SECTION("an edit after the selection was cleared writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    const auto lightProperties = lightNode->entity().properties();
    const auto worldProperties = map.worldNode().entity().properties();

    // the panel has not updated yet, like a spin box that loses the focus after the
    // selection changed; with nothing selected, worldspawn is the selection's entity
    mdl::deselectAll(map);
    REQUIRE(showsEditor(panel));
    findControl<SpinControl>(panel, "Radius").setValue(500.0);

    CHECK(lightNode->entity().properties() == lightProperties);
    CHECK(map.worldNode().entity().properties() == worldProperties);

    QApplication::processEvents();
    CHECK(currentText(panel) == "Select a light");
  }

  SECTION("shows multi where the selected lights differ")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode1 = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_radius", "300 300 300"},
        {"noshadows", "1"},
      });
    auto* lightNode2 = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "64 0 0"},
        {"light_radius", "200 200 200"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode1, lightNode2});

    const auto isMulti = [](const SpinControl& spin) {
      return spin.specialValueText() == "multi" && spin.value() == spin.minimum();
    };

    auto& radius = findControl<SpinControl>(panel, "Radius");
    CHECK(isMulti(radius));
    CHECK(isMulti(findControl<SpinControl>(panel, "OriginX")));
    CHECK_FALSE(isMulti(findControl<SpinControl>(panel, "OriginY")));
    CHECK_FALSE(isMulti(findControl<SpinControl>(panel, "Intensity")));
    CHECK(
      findControl<QCheckBox>(panel, "CastShadows").checkState() == Qt::PartiallyChecked);
    CHECK(findControl<QRadioButton>(panel, "Point").isChecked());

    // a multi field is written only when it is edited, then to every light
    radius.setValue(400.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "light_radius") == "400 400 400");
    CHECK(valueOf(*lightNode2, "light_radius") == "400 400 400");
    CHECK(valueOf(*lightNode1, "noshadows") == "1");
    CHECK(property(*lightNode2, "noshadows") == nullptr);
    CHECK_FALSE(isMulti(radius));
    CHECK(radius.value() == 400.0);

    findControl<QCheckBox>(panel, "CastShadows").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "noshadows") == "0");
    CHECK(findControl<QCheckBox>(panel, "CastShadows").checkState() == Qt::Checked);
  }

  SECTION("a multi field writes every value of its range, its minimum too")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto emitterSpot = [&](const std::string& origin, const std::string& penumbra) {
      return addLight(
        map,
        withValue(
          emitterProperties(origin, "spot", "288 288 288"), "emit_penumbra", penumbra));
    };

    auto panel = LightEditorPanel{fixture.context()};
    auto& penumbra = findControl<SpinControl>(panel, "Penumbra");

    // typing the range minimum changes the value of a multi field, so it is written
    auto* typedNode1 = emitterSpot("0 0 0", "0.2");
    auto* typedNode2 = emitterSpot("64 0 0", "0.4");
    selectOnly(map, {typedNode1, typedNode2});
    REQUIRE(showsEmitterPage(panel));
    REQUIRE(showsMulti(penumbra));

    penumbra.selectAll();
    QTest::keyClicks(&penumbra, "0");
    QTest::keyClick(&penumbra, Qt::Key_Return);
    QApplication::processEvents();
    CHECK(valueOf(*typedNode1, "emit_penumbra") == "0");
    CHECK(valueOf(*typedNode2, "emit_penumbra") == "0");
    CHECK_FALSE(showsMulti(penumbra));
    CHECK(penumbra.value() == 0.0);

    // from multi, a step down writes nothing and a step up starts at the range minimum
    auto* steppedNode1 = emitterSpot("128 0 0", "0.3");
    auto* steppedNode2 = emitterSpot("192 0 0", "0.6");
    selectOnly(map, {steppedNode1, steppedNode2});
    REQUIRE(showsMulti(penumbra));

    const auto modificationCount = map.modificationCount();
    penumbra.stepBy(-1);
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK(valueOf(*steppedNode1, "emit_penumbra") == "0.3");
    CHECK(valueOf(*steppedNode2, "emit_penumbra") == "0.6");
    CHECK(showsMulti(penumbra));

    penumbra.stepBy(1);
    QApplication::processEvents();
    CHECK(valueOf(*steppedNode1, "emit_penumbra") == "0.01");
    CHECK(valueOf(*steppedNode2, "emit_penumbra") == "0.01");
    CHECK(penumbra.value() == 0.01);

    // the slider spans the range: it shows the first light's value and writes the range
    // minimum at its start, never the value below the range that "multi" holds
    auto* draggedNode1 = emitterSpot("256 0 0", "0.2");
    auto* draggedNode2 = emitterSpot("320 0 0", "0.4");
    selectOnly(map, {draggedNode1, draggedNode2});
    REQUIRE(showsMulti(penumbra));

    auto& slider = findControl<QSlider>(panel, "PenumbraSlider");
    CHECK(slider.value() == 200);

    slider.setSliderDown(true);
    slider.setSliderPosition(0);
    slider.setSliderDown(false);
    QApplication::processEvents();
    CHECK(valueOf(*draggedNode1, "emit_penumbra") == "0");
    CHECK(valueOf(*draggedNode2, "emit_penumbra") == "0");
  }

  SECTION("a slider writes on release only if the drag moved it")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    const auto emitterSpot = [&](const std::string& origin, const std::string& square) {
      return addLight(
        map,
        withValue(
          emitterProperties(origin, "spot", "288 288 288"), "emit_square", square));
    };
    auto* lightNode1 = emitterSpot("0 0 0", "2");
    auto* lightNode2 = emitterSpot("64 0 0", "3");

    auto panel = LightEditorPanel{fixture.context()};
    auto& slider = findControl<QSlider>(panel, "SquarenessSlider");
    auto& squareness = findControl<SpinControl>(panel, "Squareness");

    // setSliderDown emits sliderPressed and sliderReleased as a click on the handle does;
    // the slider shows 2 as 1.988, the nearest of its steps
    selectOnly(map, {lightNode1});
    REQUIRE(slider.isEnabled());
    const auto modificationCount = map.modificationCount();
    slider.setSliderDown(true);
    slider.setSliderDown(false);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "emit_square") == "2");
    CHECK(map.modificationCount() == modificationCount);
    CHECK(squareness.value() == 2.0);

    // nor does a click write the first light's value to lights that differ
    selectOnly(map, {lightNode1, lightNode2});
    REQUIRE(showsMulti(squareness));
    slider.setSliderDown(true);
    slider.setSliderDown(false);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "emit_square") == "2");
    CHECK(valueOf(*lightNode2, "emit_square") == "3");
    CHECK(map.modificationCount() == modificationCount);
    CHECK(showsMulti(squareness));

    // a drag that ends where it started writes nothing and shows the lights again
    const auto startPosition = slider.value();
    slider.setSliderDown(true);
    slider.setSliderPosition(700);
    CHECK_FALSE(showsMulti(squareness));
    slider.setSliderPosition(startPosition);
    slider.setSliderDown(false);
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK(showsMulti(squareness));

    // a drag writes once, on release, to every selected light
    slider.setSliderDown(true);
    slider.setSliderPosition(300);
    slider.setSliderPosition(500);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "emit_square") == "2");
    CHECK(map.modificationCount() == modificationCount);
    CHECK(squareness.value() == 16.75);

    slider.setSliderDown(false);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode1, "emit_square") == "16.75");
    CHECK(valueOf(*lightNode2, "emit_square") == "16.75");
    CHECK(map.modificationCount() == modificationCount + 1);
  }

  SECTION("the wheel over an input without the focus scrolls instead of editing")
  {
    auto& prefs = PreferenceManager::instance();
    // also when a check fails, so that no later test case sees a changed preference
    const auto resetThreshold = kdl::invoke_later{
      [&] { prefs.resetToDefault(Preferences::LightVolumeHandleThreshold); }};

    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "16 32 64"},
        {"light_radius", "200 100 50"},
        {"_color", "1 0.5 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsEditor(panel));

    const auto properties = lightNode->entity().properties();
    const auto modificationCount = map.modificationCount();
    const auto threshold = pref(Preferences::LightVolumeHandleThreshold);

    // Qt does not propagate a synthesized wheel event ("Synthesized events shouldn't
    // propagate", qapplication.cpp), so this checks that the input leaves the event to
    // its parents; that they scroll the panel, and that an input with the focus takes
    // the wheel, are GUI checks
    const auto wheelIsAccepted = [](QWidget& input) {
      auto event = QWheelEvent{
        QPointF{4, 4},
        QPointF{4, 4},
        QPoint{},
        QPoint{0, 120},
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false};
      QApplication::sendEvent(&input, &event);
      return event.isAccepted();
    };
    const auto checkWheel = [&](QWidget& input, const auto& valueOf) {
      CAPTURE(input.objectName().toStdString());
      CHECK(input.focusPolicy() == Qt::StrongFocus);
      const auto value = valueOf();
      CHECK_FALSE(wheelIsAccepted(input));
      CHECK(valueOf() == value);
    };

    const auto spins = panel.findChildren<QAbstractSpinBox*>();
    REQUIRE_FALSE(spins.isEmpty());
    for (auto* spin : spins)
    {
      checkWheel(*spin, [&] { return spin->property("value").toDouble(); });
    }

    auto sliderCount = 0;
    for (auto* slider : panel.findChildren<QAbstractSlider*>())
    {
      // the scroll bars of the panel and of the combo box lists scroll
      if (!qobject_cast<QScrollBar*>(slider))
      {
        checkWheel(*slider, [&] { return slider->value(); });
        ++sliderCount;
      }
    }
    CHECK(sliderCount > 0);

    const auto comboBoxes = panel.findChildren<QComboBox*>();
    REQUIRE_FALSE(comboBoxes.isEmpty());
    for (auto* comboBox : comboBoxes)
    {
      checkWheel(*comboBox, [&] { return comboBox->currentIndex(); });
    }

    CHECK(lightNode->entity().properties() == properties);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(pref(Preferences::LightVolumeHandleThreshold) == threshold);
  }

  SECTION("lights of different types hide the volume")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* pointNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});
    auto* spotNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "64 0 0"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {pointNode, spotNode});

    CHECK(findControl<QLabel>(panel, "MixedTypesNotice").isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "Radius").isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "TargetX").isVisibleTo(&panel));
    for (const auto* name : {"Point", "Spot", "Sun"})
    {
      CAPTURE(name);
      CHECK_FALSE(findControl<QRadioButton>(panel, name).isChecked());
    }

    selectOnly(map, {spotNode});
    CHECK_FALSE(findControl<QLabel>(panel, "MixedTypesNotice").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "TargetX").isVisibleTo(&panel));
  }

  SECTION("the bad target notice stays after an edit until its fix")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_radius", "300 300 300"},
        {"light_target", "0 0 -100"},
        {"light_up", "0 -64 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& notice = findControl<QLabel>(panel, "BadTargetNotice");
    CHECK(notice.isVisibleTo(&panel));
    CHECK(findControl<QRadioButton>(panel, "Point").isChecked());

    // an ordinary edit keeps the partial keys, so the notice stays
    findControl<SpinControl>(panel, "Radius").setValue(500.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "500 500 500");
    CHECK(valueOf(*lightNode, "light_target") == "0 0 -100");
    CHECK(notice.isVisibleTo(&panel));

    findControl<QToolButton>(panel, "FixBadTarget").click();
    QApplication::processEvents();
    CHECK(property(*lightNode, "light_target") == nullptr);
    CHECK(property(*lightNode, "light_up") == nullptr);
    CHECK(valueOf(*lightNode, "light_radius") == "500 500 500");
    CHECK_FALSE(notice.isVisibleTo(&panel));
  }

  SECTION("the stale spot keys of a parallel light are removed by their fix")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"parallel", "1"},
        {"light_radius", "2048 2048 1024"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& notice = findControl<QLabel>(panel, "StaleSpotKeysNotice");
    CHECK(findControl<QRadioButton>(panel, "Sun").isChecked());
    CHECK(notice.isVisibleTo(&panel));

    // a radius edit leaves the keys, which the engine reads instead of the radius
    findControl<SpinControl>(panel, "RadiusX").setValue(1024.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "1024 2048 1024");
    CHECK(notice.isVisibleTo(&panel));

    findControl<QToolButton>(panel, "FixStaleSpotKeys").click();
    QApplication::processEvents();
    for (const auto* key : {"light_target", "light_up", "light_right"})
    {
      CAPTURE(key);
      CHECK(property(*lightNode, key) == nullptr);
    }
    CHECK(valueOf(*lightNode, "parallel") == "1");
    CHECK(valueOf(*lightNode, "light_radius") == "1024 2048 1024");
    CHECK(findControl<QRadioButton>(panel, "Sun").isChecked());
    CHECK_FALSE(notice.isVisibleTo(&panel));
  }

  SECTION("an off-centre spot light says how far its first box edit moves it")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // the default spot with right turned 30 degrees towards the beam: its far corners
    // are at x = +-152.42, the recentred ones at +-132
    auto* skewedNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-110.85 0 -64"},
      });
    auto* centredNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};

    auto& notice = findControl<QLabel>(panel, "OffCentreNotice");
    selectOnly(map, {skewedNode});
    CHECK(notice.isVisibleTo(&panel));
    CHECK(
      notice.text()
      == "Off-centre projection: the first box edit recentres it (the far rectangle "
         "moves by up to 21 units)");

    selectOnly(map, {centredNode});
    CHECK_FALSE(notice.isVisibleTo(&panel));
  }

  SECTION("a light with light_origin is read-only until it is converted")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_origin", "10 20 30"},
        {"light_angles", "0 90 0"},
        {"light_radius", "300 300 300"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& notice = findControl<QLabel>(panel, "LightOriginNotice");
    auto& radius = findControl<SpinControl>(panel, "Radius");
    CHECK(notice.isVisibleTo(&panel));
    CHECK_FALSE(radius.isEnabled());
    CHECK(findControl<SpinControl>(panel, "OriginX").value() == 10.0);

    // a write that reaches the slot anyway changes nothing
    const auto before = lightNode->entity().properties();
    radius.setValue(500.0);
    QApplication::processEvents();
    CHECK(lightNode->entity().properties() == before);

    findControl<QToolButton>(panel, "ConvertLightOrigin").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "origin") == "10 20 30");
    CHECK(property(*lightNode, "light_origin") == nullptr);
    CHECK(valueOf(*lightNode, "angles") == "0 90 0");
    CHECK(property(*lightNode, "light_angles") == nullptr);
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    CHECK_FALSE(notice.isVisibleTo(&panel));
    CHECK(radius.isEnabled());
  }

  SECTION("a styled light is drawn at full radius")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* styledNode =
      addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}, {"style", "1"}});
    auto* plainNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};

    auto& notice = findControl<QLabel>(panel, "StyledNotice");
    selectOnly(map, {styledNode});
    CHECK(notice.isVisibleTo(&panel));
    selectOnly(map, {plainNode});
    CHECK_FALSE(notice.isVisibleTo(&panel));
  }

  SECTION("the style list holds the choices of the entity definition")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode =
      addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}, {"style", "1"}});
    // no definition: the engine's predefined styles
    auto* customNode =
      addLight(map, {{"classname", "light_custom"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& styleChoice = findControl<QComboBox>(panel, "StyleChoice");
    CHECK(styleChoice.isVisibleTo(&panel));
    CHECK(styleChoice.count() == 13);
    CHECK(styleChoice.itemText(0) == "-1: None (engine default)");
    CHECK(styleChoice.currentText() == "1: Flicker A");

    const auto index = styleChoice.findData("10");
    REQUIRE(index != -1);
    styleChoice.setCurrentIndex(index);
    styleChoice.activated(index);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "style") == "10");

    selectOnly(map, {customNode});
    REQUIRE(showsEditor(panel));
    CHECK(styleChoice.isVisibleTo(&panel));
    CHECK(styleChoice.count() == 13);
    CHECK(styleChoice.itemText(0) == "-1: None");
    CHECK(styleChoice.itemText(11) == "10: Fluorescent Flicker");
    CHECK(styleChoice.currentText() == "-1: None");

    styleChoice.setCurrentIndex(11);
    styleChoice.activated(11);
    QApplication::processEvents();
    CHECK(valueOf(*customNode, "style") == "10");
    CHECK(styleChoice.currentText() == "10: Fluorescent Flicker");
  }

  SECTION("the texture list holds the engine default and the light materials")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});
    auto* texturedNode = addLight(
      map,
      {{"classname", "light"},
       {"origin", "64 0 0"},
       {"texture", "lights/tb_test_point"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& texture = findControl<QComboBox>(panel, "Texture");
    const auto items = [&]() {
      auto result = QStringList{};
      for (int i = 0; i < texture.count(); ++i)
      {
        result.push_back(texture.itemText(i));
      }
      return result;
    };

    // the materials of the fixture's tb_test_lights.mtr, sorted
    const auto expectedItems =
      QStringList{"(engine default)", "fogs/tb_test_fog", "lights/tb_test_point"};
    CHECK(items() == expectedItems);
    CHECK(texture.currentIndex() == 0);

    texture.setCurrentIndex(2);
    texture.activated(2);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "texture") == "lights/tb_test_point");
    CHECK(texture.currentIndex() == 2);

    selectOnly(map, {texturedNode});
    CHECK(texture.currentIndex() == 2);
    CHECK(texture.currentText() == "lights/tb_test_point");
    selectOnly(map, {lightNode});

    // a name that is typed in
    texture.setEditText("lights/biground1");
    texture.lineEdit()->returnPressed();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "texture") == "lights/biground1");
    CHECK(texture.currentText() == "lights/biground1");

    texture.setCurrentIndex(0);
    texture.activated(0);
    QApplication::processEvents();
    CHECK(property(*lightNode, "texture") == nullptr);

    // the list is read again only when the shaders may have changed
    texture.removeItem(1);
    selectOnly(map, {texturedNode});
    CHECK(texture.count() == 2);

    map.entityModelManager().setShaders(mdl::loadIndexedShaders(
      fs::DiskFileSystem{map.gamePath() / "base"},
      map.gameInfo().gameConfig.materialConfig));
    fixture.document().modsDidChangeNotifier();
    QApplication::processEvents();
    CHECK(items() == expectedItems);
    CHECK(texture.currentIndex() == 2);
  }

  SECTION("the texture list is read again when the material collections are reloaded")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& texture = findControl<QComboBox>(panel, "Texture");
    REQUIRE(texture.count() == 3);

    // the reload parses the material scripts again, so the list may have changed
    texture.removeItem(1);
    REQUIRE(texture.count() == 2);
    mdl::reloadMaterialCollections(map);
    QApplication::processEvents();
    CHECK(texture.count() == 3);
    CHECK(texture.itemText(1) == "fogs/tb_test_fog");
  }

  SECTION("the volume tool buttons")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map, {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "64 32 16"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    const auto& drawButton = findControl<QToolButton>(panel, "DrawVolume");
    CHECK(drawButton.isCheckable());
    CHECK(drawButton.isEnabled());
    CHECK_FALSE(drawButton.isChecked());

    // tool buttons take no focus when they are clicked, unlike push buttons, so the
    // map view keeps the focus (LWR-28)
    for (const auto* name : {"DrawVolume", "ResetVolume"})
    {
      CAPTURE(name);
      CHECK((findControl<QToolButton>(panel, name).focusPolicy() & Qt::ClickFocus) == 0);
    }

    auto& reset = findControl<QToolButton>(panel, "ResetVolume");
    CHECK(reset.isEnabled());
    reset.click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Reset Light Volume");
  }

  SECTION("the draw button turns the light volume tool's draw mode on and off")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    auto& toolBox = fixture.context().toolBox;
    auto& tool = toolBox.lightVolumeTool();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // the handles work without the tool (D56)
    CHECK(tool.handles().active());
    CHECK_FALSE(tool.active());

    auto& draw = findControl<QToolButton>(panel, "DrawVolume");

    draw.click();
    CHECK(tool.active());
    CHECK(tool.mode() == LightVolumeTool::Mode::Draw);
    CHECK(draw.isChecked());
    // the tool is not modal: Delete, Cut, Group and the nudges keep working (D31)
    CHECK_FALSE(toolBox.anyModalToolActive());
    CHECK(tool.handles().active());

    draw.click();
    CHECK_FALSE(tool.active());
    CHECK_FALSE(draw.isChecked());

    // Esc or the end of a drawn volume leaves draw mode, and the tool turns off once the
    // drag has gone
    draw.click();
    REQUIRE(tool.active());
    tool.setMode(LightVolumeTool::Mode::Handles);
    CHECK_FALSE(draw.isChecked());
    QApplication::processEvents();
    CHECK_FALSE(tool.active());
    CHECK_FALSE(draw.isChecked());
  }

  SECTION("the volume tool buttons follow the tool when another tool replaces it")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    auto& toolBox = fixture.context().toolBox;

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& draw = findControl<QToolButton>(panel, "DrawVolume");
    draw.click();
    REQUIRE(toolBox.lightVolumeTool().active());

    toolBox.toggleTool(toolBox.rotateTool());
    CHECK_FALSE(toolBox.lightVolumeTool().active());
    CHECK_FALSE(draw.isChecked());
  }

  SECTION("closing the panel turns the light volume tool off")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    auto& toolBox = fixture.context().toolBox;

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& draw = findControl<QToolButton>(panel, "DrawVolume");
    draw.click();
    REQUIRE(toolBox.lightVolumeTool().active());

    // a panel that is merely hidden behind a tab keeps it (D18)
    panel.panelShown();
    QApplication::processEvents();
    CHECK(toolBox.lightVolumeTool().active());

    panel.panelHidden();
    CHECK_FALSE(toolBox.lightVolumeTool().active());
    CHECK_FALSE(draw.isChecked());
  }

  SECTION("the volume tool buttons need one light without light_origin")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});
    auto* otherNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});
    auto* legacyNode = addLight(
      map, {{"classname", "light"}, {"origin", "0 0 0"}, {"light_origin", "10 20 30"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& draw = findControl<QToolButton>(panel, "DrawVolume");

    selectOnly(map, {lightNode});
    CHECK(draw.isEnabled());

    selectOnly(map, {lightNode, otherNode});
    REQUIRE(showsEditor(panel));
    CHECK_FALSE(draw.isEnabled());

    // the editor page is read-only (D8)
    selectOnly(map, {legacyNode});
    REQUIRE(showsEditor(panel));
    CHECK_FALSE(draw.isEnabled());
  }

  SECTION("the panel is not refreshed while the volume tool drags")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    auto& tool = fixture.context().toolBox.lightVolumeTool();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 200 100"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // the handles work without the tool (D56)
    REQUIRE_FALSE(tool.active());

    auto& radiusX = findControl<SpinControl>(panel, "RadiusX");
    REQUIRE(radiusX.value() == 300.0);

    // the side scales the box about the light's origin, which stays (D64)
    REQUIRE(tool.beginDrag(mdl::Hit{
      LightVolumeTool::SideHitType,
      0.0,
      vm::vec3d{0, 0, 0},
      BBoxSide{vm::vec3d{1, 0, 0}}}));
    REQUIRE(tool.update(vm::vec3d{32, 0, 0}));
    REQUIRE(valueOf(*lightNode, "light_radius") == "339.79 200 100");
    REQUIRE(valueOf(*lightNode, "origin") == "0 0 0");
    QApplication::processEvents();
    CHECK(radiusX.value() == 300.0);

    tool.endDrag();
    QApplication::processEvents();
    CHECK(radiusX.value() == Catch::Approx(339.79).margin(0.005));
  }

  SECTION("the panel is not refreshed while it is out of sight")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* firstNode =
      addLight(map, {{"classname", "light"}, {"name", "first"}, {"origin", "0 0 0"}});
    auto* secondNode =
      addLight(map, {{"classname", "light"}, {"name", "second"}, {"origin", "64 0 0"}});

    // the panel in a window that never appears on the screen, which owns it
    auto window = QWidget{};
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(400, 800);
    auto* panel = new LightEditorPanel{fixture.context(), &window};
    panel->setGeometry(0, 0, 400, 800);

    // hidden, as in a closed dock
    selectOnly(map, {firstNode});
    CHECK_FALSE(showsEditor(*panel));

    window.show();
    panel->panelShown();
    QApplication::processEvents();
    CHECK(currentText(*panel) == "Light: first");

    // outside the window, as in a dock tabbed behind another
    panel->move(-2000, -2000);
    selectOnly(map, {secondNode});
    CHECK(currentText(*panel) == "Light: first");

    panel->move(0, 0);
    panel->panelShown();
    QApplication::processEvents();
    CHECK(currentText(*panel) == "Light: second");
  }
  SECTION("a write after the selection became something else writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});
    auto* playerStartNode = new mdl::EntityNode{
      mdl::Entity{{{"classname", "info_player_start"}, {"origin", "64 0 0"}}}};
    const auto builder =
      mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    auto* brushNode = new mdl::BrushNode{
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 32, 32}}, "wall") | kdl::value()};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {playerStartNode, brushNode}}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsEditor(panel));

    const auto undoName = [&]() {
      const auto* name = map.undoCommandName();
      return name ? *name : std::string{};
    };
    const auto lightProperties = lightNode->entity().properties();
    const auto playerStartProperties = playerStartNode->entity().properties();
    // the entity of the brush
    const auto worldProperties = map.worldNode().entity().properties();
    auto& radius = findControl<SpinControl>(panel, "Radius");

    // the panel has not updated yet: the light and a brush are selected now
    mdl::deselectAll(map);
    mdl::selectNodes(map, {lightNode, brushNode});
    REQUIRE(showsEditor(panel));
    auto undoNameBefore = undoName();
    radius.setValue(500.0);
    CHECK(undoName() == undoNameBefore);

    // a player start alone, which a write by the selection's entities would reach
    mdl::deselectAll(map);
    mdl::selectNodes(map, {playerStartNode});
    REQUIRE(showsEditor(panel));
    undoNameBefore = undoName();
    radius.setValue(600.0);
    CHECK(undoName() == undoNameBefore);

    CHECK(lightNode->entity().properties() == lightProperties);
    CHECK(playerStartNode->entity().properties() == playerStartProperties);
    CHECK(property(*playerStartNode, "light_radius") == nullptr);
    CHECK(map.worldNode().entity().properties() == worldProperties);
  }

  SECTION("resetting the volume keeps everything else")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* pointNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "8 16 24"},
        {"texture", "lights/tb_test_point"},
        {"style", "3"},
        {"noshadows", "1"},
        {"nospecular", "1"},
        {"_color", "2 1 0"},
        {"light_radius", "64 32 16"},
      });
    auto* spotNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 128"},
        {"light_target", "0 0 -100"},
        {"light_up", "0 -50 0"},
        {"light_right", "-50 0 0"},
      });
    auto* otherPointNode = addLight(
      map, {{"classname", "light"}, {"origin", "64 0 0"}, {"light_radius", "200 50 25"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& reset = findControl<QToolButton>(panel, "ResetVolume");

    // the texture, style, flags and colour stay
    selectOnly(map, {pointNode});
    const auto pointBefore = pointNode->entity().properties();
    reset.click();
    QApplication::processEvents();
    CHECK(
      pointNode->entity().properties()
      == withValue(pointBefore, "light_radius", "300 300 300"));

    // DefaultProjected's vectors, the origin unchanged
    selectOnly(map, {spotNode});
    const auto spotBefore = spotNode->entity().properties();
    reset.click();
    QApplication::processEvents();
    CHECK(
      spotNode->entity().properties()
      == withValue(
        withValue(
          withValue(spotBefore, "light_target", "0 0 -256"), "light_up", "0 -128 0"),
        "light_right",
        "-128 0 0"));

    // every selected light, in one command
    selectOnly(map, {pointNode});
    REQUIRE(mdl::setEntityProperty(map, "light_radius", "100 100 100"));
    selectOnly(map, {pointNode, otherPointNode});
    const auto modificationCount = map.modificationCount();
    reset.click();
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount + 1);
    CHECK(valueOf(*pointNode, "light_radius") == "300 300 300");
    CHECK(valueOf(*otherPointNode, "light_radius") == "300 300 300");
  }

  SECTION("the emitter falloff combos and the rows they show")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    clickTab(panel, EmitterPage);
    REQUIRE(valueOf(*lightNode, "emit_falloff") == "invsquare_clamped");

    const auto items = [](const QComboBox& comboBox) {
      auto result = QStringList{};
      for (int i = 0; i < comboBox.count(); ++i)
      {
        result.push_back(comboBox.itemText(i));
      }
      return result;
    };

    // the in-game editor's labels; the default curve first (F-T-16), each item with its
    // curve's value
    auto& falloff = findControl<QComboBox>(panel, "Falloff");
    auto& pattern = findControl<QComboBox>(panel, "Pattern");
    CHECK(
      items(falloff)
      == QStringList{
        "Inverse Square (clamped) - PBR safe",
        "Inverse Square (unbounded)",
        "Linear",
        "Smooth",
        "Exponential",
      });
    CHECK(falloff.itemData(0).toInt() == int(mdl::LightFalloff::InvSquareClamped));
    const auto linear = falloff.findData(int(mdl::LightFalloff::Linear));
    const auto exponential = falloff.findData(int(mdl::LightFalloff::Exponential));
    REQUIRE(linear == 2);
    REQUIRE(exponential == 4);
    CHECK(
      items(pattern)
      == QStringList{
        "None",
        "Subsurface (warm halo)",
        "Ice (cool noise)",
        "Caustic (sharp noise)",
        "Banded (sine rings)",
        "Granular (stippled)",
      });

    auto& emitSize = findControl<SpinControl>(panel, "EmitSize");
    auto& autoRange = findControl<QCheckBox>(panel, "AutoRange");
    CHECK(falloff.currentIndex() == 0);
    CHECK(emitSize.isVisibleTo(&panel));
    CHECK(autoRange.isVisibleTo(&panel));

    // under auto range a curve with a reach sets the range: ln(1 / 0.1) sizes of 100
    falloff.setCurrentIndex(exponential);
    falloff.activated(exponential);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_falloff") == "exponential");
    CHECK(emitSize.isVisibleTo(&panel));
    CHECK(valueOf(*lightNode, "light_radius") == "230.26 230.26 230.26");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "1");

    // the linear and the smooth curve have no size and no auto range; the range stays
    falloff.setCurrentIndex(linear);
    falloff.activated(linear);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_falloff") == "linear");
    CHECK_FALSE(emitSize.isVisibleTo(&panel));
    CHECK_FALSE(autoRange.isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "EmitStart").isVisibleTo(&panel));
    CHECK(valueOf(*lightNode, "light_radius") == "230.26 230.26 230.26");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "1");

    // back to a curve with a reach: the flag applies as it stands (G0 section 5, as
    // corrected for DP-T5-01), so the range is the clamped inverse square's 300 again and
    // the round trip changed nothing
    falloff.setCurrentIndex(0);
    falloff.activated(0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_falloff") == "invsquare_clamped");
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "1");
    CHECK(autoRange.isChecked());
    CHECK(findControl<QLabel>(panel, "RangeLabel").text() == "Range (auto)");

    // a range typed under a linear curve turns auto range off and is kept by a curve with
    // a reach
    falloff.setCurrentIndex(linear);
    falloff.activated(linear);
    QApplication::processEvents();
    findControl<SpinControl>(panel, "Range").setValue(400.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "400 400 400");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "0");
    falloff.setCurrentIndex(0);
    falloff.activated(0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "400 400 400");
    CHECK(valueOf(*lightNode, "emit_auto_range") == "0");
    CHECK_FALSE(autoRange.isChecked());
    CHECK(findControl<QLabel>(panel, "RangeLabel").text() == "Range");

    // the pattern fields only with a pattern
    for (const auto* name : {"PatternStrength", "PatternScale", "PatternPhase"})
    {
      CAPTURE(name);
      CHECK_FALSE(findControl<SpinControl>(panel, name).isVisibleTo(&panel));
    }
    pattern.setCurrentIndex(3);
    pattern.activated(3);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_pattern") == "caustic");
    for (const auto* name : {"PatternStrength", "PatternScale", "PatternPhase"})
    {
      CAPTURE(name);
      CHECK(findControl<SpinControl>(panel, name).isVisibleTo(&panel));
      CHECK(findControl<QSlider>(panel, QString{name} + "Slider").isVisibleTo(&panel));
    }

    findControl<QCheckBox>(panel, "EnergyClamp").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_clamp") == "0");
  }

  SECTION("the type radio gives an emitter its shape's defaults")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

    auto* lightNode = addLight(map, emitterProperties("0 0 64", "omni", "288 288 288"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // a new spot: the default cone, pointing down, its range from auto range
    findControl<QRadioButton>(panel, "Spot").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_shape") == "spot");
    CHECK(valueOf(*lightNode, "emit_cone") == "60");
    CHECK(valueOf(*lightNode, "light_radius") == "288 288 288");
    CHECK(vm::is_equal(
      mdl::lightRotation(lightNode->entity(), policy)[0], vm::vec3d{0, 0, -1}, 1e-4));
    CHECK(property(*lightNode, "light_target") == nullptr);
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Type");
    CHECK(findControl<SpinControl>(panel, "Range").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "ConeH").isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "TargetX").isVisibleTo(&panel));

    // a sun: today's sun, without a falloff, a size, a start or auto range
    findControl<QRadioButton>(panel, "Sun").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_shape") == "sun");
    CHECK(valueOf(*lightNode, "parallel") == "1");
    CHECK(valueOf(*lightNode, "light_radius") == "2048 2048 1024");
    CHECK_FALSE(findControl<QComboBox>(panel, "Falloff").isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "EmitSize").isVisibleTo(&panel));
    CHECK_FALSE(findControl<QCheckBox>(panel, "AutoRange").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "RadiusX").isVisibleTo(&panel));

    // an omni: the range from auto range again
    findControl<QRadioButton>(panel, "Point").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_shape") == "omni");
    CHECK(property(*lightNode, "parallel") == nullptr);
    CHECK(valueOf(*lightNode, "light_radius") == "288 288 288");
  }

  SECTION("the type radio keeps an emitter's rotation, and its range with auto range off")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

    // a turned omni with auto range off (F-T-17)
    auto properties = withValue(
      emitterProperties("0 0 64", "omni", "400 200 300"), "emit_auto_range", "0");
    properties.emplace_back("angles", "30 45 0");
    auto* lightNode = addLight(map, properties);
    const auto rotation = mdl::lightRotation(lightNode->entity(), policy);

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // the spot keeps the rotation and takes the omni's largest radius as its range
    findControl<QRadioButton>(panel, "Spot").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_shape") == "spot");
    CHECK(valueOf(*lightNode, "angles") == "30 45 0");
    CHECK(vm::is_equal(mdl::lightRotation(lightNode->entity(), policy), rotation, 1e-6));
    CHECK(valueOf(*lightNode, "light_radius") == "400 400 400");
    CHECK(valueOf(*lightNode, "emit_size") == "96");

    // and back: the omni keeps the spot's range, and the rotation still
    findControl<QRadioButton>(panel, "Point").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_shape") == "omni");
    CHECK(valueOf(*lightNode, "angles") == "30 45 0");
    CHECK(valueOf(*lightNode, "light_radius") == "400 400 400");

    // a spot made from an omni that has a rotation is not pointed down, so a spot made
    // from one without does
    auto* plainNode = addLight(map, emitterProperties("256 0 0", "omni", "288 288 288"));
    selectOnly(map, {plainNode});
    findControl<QRadioButton>(panel, "Spot").click();
    QApplication::processEvents();
    CHECK(vm::is_equal(
      mdl::lightRotation(plainNode->entity(), policy)[0], vm::vec3d{0, 0, -1}, 1e-4));
  }

  SECTION("emitter omnis and spots together share the range row")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* omniNode = addLight(
      map,
      withValue(
        emitterProperties("0 0 0", "omni", "400 200 200"), "emit_auto_range", "0"));
    auto spotProperties = withValue(
      emitterProperties("256 0 0", "spot", "300 300 300"), "emit_auto_range", "0");
    spotProperties.emplace_back("angles", "90 0 0");
    auto* spotNode = addLight(map, spotProperties);
    auto* sunNode = addLight(
      map,
      withValue(
        withValue(emitterProperties("512 0 0", "sun", "2048 2048 1024"), "emitter", "1"),
        "emit_auto_range",
        "0"));
    auto* textureNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "768 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& range = findControl<SpinControl>(panel, "Range");
    auto& coneH = findControl<SpinControl>(panel, "ConeH");

    // F-T-14: the range for omnis and spots, the cone only for spots
    selectOnly(map, {omniNode, spotNode});
    REQUIRE(showsEmitterPage(panel));
    CHECK(range.isVisibleTo(&panel));
    CHECK(showsMulti(range));
    CHECK_FALSE(coneH.isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "EmitSize").isVisibleTo(&panel));
    CHECK(findControl<QComboBox>(panel, "Effect").isVisibleTo(&panel));

    // an edit sets each light's range by its own rule: the omni's box scaled as a whole
    range.setValue(600.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "600 300 300");
    CHECK(valueOf(*spotNode, "light_radius") == "600 600 600");

    // the mixed-types notice is not shown over the shared rows (DP-T4-02)
    auto& mixedTypes = findControl<QLabel>(panel, "MixedTypesNotice");
    CHECK_FALSE(mixedTypes.isVisibleTo(&panel));

    // with a sun or a texture light there is no shared range, and the notice is shown;
    // spots alone have both
    selectOnly(map, {omniNode, sunNode});
    CHECK_FALSE(range.isVisibleTo(&panel));
    CHECK(mixedTypes.isVisibleTo(&panel));
    selectOnly(map, {omniNode, textureNode});
    CHECK_FALSE(range.isVisibleTo(&panel));
    selectOnly(map, {spotNode});
    CHECK(range.isVisibleTo(&panel));
    CHECK(coneH.isVisibleTo(&panel));
    // an omni alone has the Range row too, its unequal radii behind Equilateral
    // (G0 section 9, choice 4)
    selectOnly(map, {omniNode});
    CHECK(range.isVisibleTo(&panel));
    CHECK(range.value() == 600.0);
    CHECK_FALSE(findControl<QCheckBox>(panel, "EqualRadius").isChecked());
    CHECK(findControl<SpinControl>(panel, "RadiusX").isVisibleTo(&panel));
  }

  SECTION("an emitter omni shows one Range, its radii behind Equilateral")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* omniNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* textureNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "256 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& equalRadius = findControl<QCheckBox>(panel, "EqualRadius");
    auto& range = findControl<SpinControl>(panel, "Range");
    auto& radius = findControl<SpinControl>(panel, "Radius");
    auto& radiusX = findControl<SpinControl>(panel, "RadiusX");

    // a new emitter omni: Equilateral ticked, one Range field, no Radius rows
    selectOnly(map, {omniNode});
    CHECK(equalRadius.isVisibleTo(&panel));
    CHECK(equalRadius.isChecked());
    CHECK(range.isVisibleTo(&panel));
    CHECK(range.value() == 288.0);
    CHECK_FALSE(radius.isVisibleTo(&panel));
    CHECK_FALSE(radiusX.isVisibleTo(&panel));

    // unticking shows the three radii and writes nothing
    const auto modificationCount = map.modificationCount();
    equalRadius.click();
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK(radiusX.isVisibleTo(&panel));
    CHECK(range.isVisibleTo(&panel));

    // an axis edit makes the radii unequal, and the size follows the new range
    radiusX.setValue(400.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "400 288 288");
    CHECK_FALSE(equalRadius.isChecked());
    CHECK(range.value() == 400.0);

    // the Range edit scales the box as a whole
    range.setValue(200.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "200 144 144");

    // ticking sets all three to the range
    equalRadius.click();
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "200 200 200");
    CHECK(equalRadius.isChecked());
    CHECK_FALSE(radiusX.isVisibleTo(&panel));

    // a texture light keeps the session's choice of D52, off at first
    selectOnly(map, {textureNode});
    CHECK_FALSE(range.isVisibleTo(&panel));
    CHECK_FALSE(equalRadius.isChecked());
    CHECK(radiusX.isVisibleTo(&panel));
  }

  SECTION("Range, Auto range, Size and Start sit together and say what follows what")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* omniNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    // a hand-set range: the flag on, the range out of step (G0 section 5)
    auto* handSetNode =
      addLight(map, emitterProperties("256 0 0", "omni", "500 500 500"));
    auto* sunNode = addLight(
      map,
      withValue(emitterProperties("512 0 0", "sun", "2048 2048 1024"), "emitter", "1"));

    auto panel = LightEditorPanel{fixture.context()};
    auto& rangeLabel = findControl<QLabel>(panel, "RangeLabel");
    auto& autoRange = findControl<QCheckBox>(panel, "AutoRange");
    auto& note = findControl<QLabel>(panel, "AutoRangeNote");
    const auto rows = std::vector<QWidget*>{
      &findControl<SpinControl>(panel, "Range"),
      &autoRange,
      &findControl<SpinControl>(panel, "EmitSize"),
      &findControl<SpinControl>(panel, "EmitStart"),
      &note,
    };

    // in force: "Range (auto)", and the line that says what follows what
    selectOnly(map, {omniNode});
    for (auto* row : rows)
    {
      CHECK(row->isVisibleTo(&panel));
    }
    CHECK(rangeLabel.text() == "Range (auto)");
    CHECK(autoRange.isChecked());
    CHECK(note.text().contains("Range follows Intensity, Size, Start and Falloff"));

    // a hand-set range reads with auto range off
    selectOnly(map, {handSetNode});
    CHECK(rangeLabel.text() == "Range");
    CHECK_FALSE(autoRange.isChecked());

    // "Range (auto)" only while auto range is in force on every selected light
    selectOnly(map, {omniNode, handSetNode});
    CHECK(rangeLabel.text() == "Range");

    // an intensity edit leaves the hand-set range and writes emit_auto_range 0
    selectOnly(map, {handSetNode});
    findControl<SpinControl>(panel, "Intensity").setValue(2.0);
    QApplication::processEvents();
    CHECK(valueOf(*handSetNode, "light_radius") == "500 500 500");
    CHECK(valueOf(*handSetNode, "emit_auto_range") == "0");

    // ticking Auto range sets the range to the auto range and keeps the size
    autoRange.click();
    QApplication::processEvents();
    CHECK(valueOf(*handSetNode, "emit_auto_range") == "1");
    CHECK(valueOf(*handSetNode, "emit_size") == "96");
    CHECK(valueOf(*handSetNode, "light_radius") == "418.45 418.45 418.45");
    CHECK(rangeLabel.text() == "Range (auto)");

    // a sun shows none of the four rows
    selectOnly(map, {sunNode});
    for (auto* row : rows)
    {
      CHECK_FALSE(row->isVisibleTo(&panel));
    }
  }

  SECTION("the type radio keeps an emitter's noshadows and nospecular")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto properties = emitterProperties("0 0 0", "omni", "288 288 288");
    properties.emplace_back("noshadows", "1");
    properties.emplace_back("nospecular", "1");
    auto* lightNode = addLight(map, properties);

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    for (const auto* name : {"Spot", "Sun", "Point"})
    {
      CAPTURE(name);
      findControl<QRadioButton>(panel, name).click();
      QApplication::processEvents();
      CHECK(valueOf(*lightNode, "noshadows") == "1");
      CHECK(valueOf(*lightNode, "nospecular") == "1");
      CHECK_FALSE(findControl<QCheckBox>(panel, "CastShadows").isChecked());
      CHECK(findControl<QCheckBox>(panel, "SkipSpecular").isChecked());
    }
  }

  SECTION("the pattern scale runs from 0.01 and says what 1 means")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LightEditorPanel{fixture.context()};
    auto& patternScale = findControl<SpinControl>(panel, "PatternScale");
    CHECK(patternScale.minimum() == 0.01);
    CHECK(patternScale.toolTip().contains("at 1"));
  }

  SECTION("an emitter omni's light_center is its bulb offset")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* omniNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* textureNode = addLight(
      map,
      {{"classname", "light"},
       {"origin", "256 0 0"},
       {"light_radius", "300 300 300"},
       {"light_center", "0 0 16"}});

    auto panel = LightEditorPanel{fixture.context()};
    auto& center = findControl<QCheckBox>(panel, "Center");
    auto& centerLabel = findControl<QLabel>(panel, "CenterLabel");

    // F-T-16
    selectOnly(map, {omniNode});
    CHECK(center.text() == "Bulb offset");
    CHECK(centerLabel.text() == "Bulb offset");

    // a negative offset is written as it is
    center.click();
    QApplication::processEvents();
    auto& centerZ = findControl<SpinControl>(panel, "CenterZ");
    REQUIRE(centerZ.isVisibleTo(&panel));
    centerZ.setValue(-24.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_center") == "0 0 -24");

    selectOnly(map, {textureNode});
    CHECK(center.text() == "Center");
    CHECK(centerLabel.text() == "Center");

    // the size says what it is
    CHECK(findControl<SpinControl>(panel, "EmitSize").toolTip().contains("half"));
  }

  SECTION("the effect phase runs 0 to 1")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      withValue(
        withValue(
          emitterProperties("0 0 0", "omni", "288 288 288"), "emit_effect", "pulse"),
        "emit_effect_phase",
        "2.25"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // F-T-13: the phase as the engine uses it, modulo 1; the key is not rewritten
    auto& phase = findControl<SpinControl>(panel, "EffectPhase");
    REQUIRE(phase.isVisibleTo(&panel));
    CHECK(phase.maximum() == 1.0);
    CHECK(phase.value() == Catch::Approx(0.25));
    CHECK(valueOf(*lightNode, "emit_effect_phase") == "2.25");

    phase.setValue(0.5);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "emit_effect_phase") == "0.5");
  }

  SECTION("the Emitter tab seeds a light's first effect from its style")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* candleNode = addLight(
      map,
      {{"classname", "light"},
       {"origin", "0 0 0"},
       {"light_radius", "300 300 300"},
       {"style", "3"}});
    auto* plainNode = addLight(
      map,
      {{"classname", "light"},
       {"origin", "256 0 0"},
       {"light_radius", "300 300 300"},
       {"style", "0"}});
    // a kept block with its own effect keeps it
    auto keptProperties = withValue(
      withValue(emitterProperties("512 0 0", "omni", "288 288 288"), "emitter", "0"),
      "emit_effect",
      "pulse");
    keptProperties.emplace_back("style", "5");
    auto* keptNode = addLight(map, keptProperties);

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {candleNode, plainNode, keptNode});
    REQUIRE(showsTexturePage(panel));

    // F-T-05
    clickTab(panel, EmitterPage);
    CHECK(valueOf(*candleNode, "emit_effect") == "candle_a");
    CHECK(valueOf(*candleNode, "style") == "3");
    CHECK(valueOf(*plainNode, "emit_effect") == "none");
    CHECK(valueOf(*keptNode, "emit_effect") == "pulse");
    CHECK(valueOf(*keptNode, "style") == "5");

    // back to Texture: the style still flickers there
    clickTab(panel, TexturePage);
    CHECK(valueOf(*candleNode, "emitter") == "0");
    CHECK(valueOf(*candleNode, "style") == "3");
  }

  SECTION("an edit that changes nothing on any light makes no command")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(map, emitterProperties("0 0 0", "spot", "288 288 288"));
    REQUIRE(mdl::setEntityProperty(map, "angles", "90 0 0"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    const auto properties = lightNode->entity().properties();
    const auto* undoName = map.undoCommandName();
    const auto undoText = undoName ? *undoName : std::string{};

    // F-T-15: the spot's own type, and a kind switch to the kind it has, write nothing
    findControl<QRadioButton>(panel, "Spot").click();
    clickTab(panel, EmitterPage);
    QApplication::processEvents();
    CHECK(lightNode->entity().properties() == properties);
    CHECK((map.undoCommandName() ? *map.undoCommandName() : std::string{}) == undoText);
  }

  SECTION("the fixes are off for a light with light_origin")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"light_origin", "10 20 30"},
        {"light_target", "0 0 -100"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    CHECK(findControl<QLabel>(panel, "BadTargetNotice").isVisibleTo(&panel));
    auto& fix = findControl<QToolButton>(panel, "FixBadTarget");
    CHECK_FALSE(fix.isEnabled());

    // a click that reaches the slot anyway changes nothing
    const auto before = lightNode->entity().properties();
    fix.clicked();
    QApplication::processEvents();
    CHECK(lightNode->entity().properties() == before);
  }

  SECTION("each control family writes its own properties")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* pointNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "16 32 64"}, {"light_radius", "300 300 300"}});
    auto* spotNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "64 0 0"},
        {"light_target", "0 0 -256"},
        {"light_up", "0 -128 0"},
        {"light_right", "-128 0 0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {pointNode});

    const auto pointBefore = pointNode->entity().properties();
    findControl<SpinControl>(panel, "OriginY").setValue(48.0);
    QApplication::processEvents();
    CHECK(
      pointNode->entity().properties() == withValue(pointBefore, "origin", "16 48 64"));
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Origin");

    findControl<QCheckBox>(panel, "SkipSpecular").click();
    QApplication::processEvents();
    CHECK(valueOf(*pointNode, "nospecular") == "1");

    auto& centerX = findControl<SpinControl>(panel, "CenterX");
    CHECK_FALSE(centerX.isVisibleTo(&panel));
    findControl<QCheckBox>(panel, "Center").click();
    QApplication::processEvents();
    CHECK(valueOf(*pointNode, "light_center") == "0 0 0");
    CHECK(centerX.isVisibleTo(&panel));

    selectOnly(map, {spotNode});
    const auto spotBefore = spotNode->entity().properties();
    findControl<SpinControl>(panel, "RightX").setValue(-100.0);
    QApplication::processEvents();
    CHECK(
      spotNode->entity().properties()
      == withValue(spotBefore, "light_right", "-100 0 0"));
  }

  SECTION("reads spot lights, suns, emitters and colours")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* colorNode =
      addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}, {"_color", "2 1 0"}});
    auto* spotNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "64 0 0"},
        {"light_target", "0 0 -200"},
        {"light_up", "0 -32 0"},
        {"light_right", "-64 0 0"},
        {"light_start", "0 0 -16"},
        {"light_end", "0 0 -180"},
      });
    auto* sunNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "128 0 0"},
        {"parallel", "1"},
        {"light_radius", "2048 2048 1024"},
      });
    auto emitterSpot = emitterProperties("192 0 0", "spot", "288 288 288");
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"emit_falloff", "exponential"},
           {"emit_pattern", "banded"},
           {"emit_pattern_scale", "2"},
           {"emit_size", "64"},
           {"emit_cone", "50 30"},
           {"emit_effect", "candle_a"},
         })
    {
      emitterSpot = withValue(std::move(emitterSpot), key, value);
    }
    auto* emitterNode = addLight(map, std::move(emitterSpot));

    auto panel = LightEditorPanel{fixture.context()};

    // the hue of 2 1 0 is 1 0.5 0, shown by the button's colour indicator
    selectOnly(map, {colorNode});
    const auto& colorButton = findControl<ColorButton>(panel, "Color");
    const auto children = colorButton.findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    const auto indicator = std::ranges::find_if(children, [](const auto* child) {
      return qobject_cast<const QPushButton*>(child) == nullptr;
    });
    REQUIRE(indicator != children.end());
    CHECK((*indicator)->styleSheet().contains(QColor{255, 128, 0}.name()));
    CHECK(findControl<SpinControl>(panel, "Intensity").value() == 2.0);

    selectOnly(map, {spotNode});
    CHECK(findControl<QRadioButton>(panel, "Spot").isChecked());
    CHECK(findControl<SpinControl>(panel, "TargetZ").value() == -200.0);
    CHECK(findControl<SpinControl>(panel, "RightX").value() == -64.0);
    CHECK(findControl<SpinControl>(panel, "UpY").value() == -32.0);
    CHECK(findControl<QCheckBox>(panel, "ExplicitStartEnd").isChecked());
    CHECK(findControl<SpinControl>(panel, "StartZ").value() == -16.0);
    CHECK(findControl<SpinControl>(panel, "EndZ").value() == -180.0);

    selectOnly(map, {sunNode});
    CHECK(findControl<QRadioButton>(panel, "Sun").isChecked());
    CHECK(findControl<SpinControl>(panel, "RadiusX").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "RadiusX").value() == 2048.0);
    CHECK(findControl<SpinControl>(panel, "RadiusY").value() == 2048.0);
    CHECK(findControl<SpinControl>(panel, "RadiusZ").value() == 1024.0);

    // the sliders map the ranges to 0..1000
    selectOnly(map, {emitterNode});
    CHECK(showsEmitterPage(panel));
    // the falloff and the effect combos hold their values as their items' data
    CHECK(findControl<QComboBox>(panel, "Falloff").currentText() == "Exponential");
    CHECK(findControl<QComboBox>(panel, "Pattern").currentIndex() == 4);
    CHECK(findControl<QComboBox>(panel, "Effect").currentText() == "Candle A");
    CHECK(findControl<SpinControl>(panel, "EmitSize").value() == 64.0);
    CHECK(findControl<SpinControl>(panel, "Range").value() == 288.0);
    CHECK(findControl<SpinControl>(panel, "ConeH").value() == 50.0);
    CHECK(findControl<SpinControl>(panel, "ConeV").value() == 30.0);
    CHECK(findControl<SpinControl>(panel, "PatternScale").value() == 2.0);
    // the scale runs from 0.01 (C-T-04): 2 is at (2 - 0.01) / (64 - 0.01) of the slider
    CHECK(findControl<QSlider>(panel, "PatternScaleSlider").value() == 31);
  }

  SECTION("each emitter control writes its own key and nothing else")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // auto range off, so that no control moves the range or the size along
    auto* spotNode = addLight(
      map,
      withValue(
        emitterProperties("0 0 0", "spot", "288 288 288"), "emit_auto_range", "0"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {spotNode});
    REQUIRE(showsEmitterPage(panel));
    const auto before = spotNode->entity().properties();

    const auto checkWrites =
      [&](const std::string& key, const std::string& value, const auto& edit) {
        CAPTURE(key, value);
        edit();
        QApplication::processEvents();
        CHECK(spotNode->entity().properties() == withValue(before, key, value));
        map.undoCommand();
        QApplication::processEvents();
        REQUIRE(spotNode->entity().properties() == before);
      };
    const auto spin = [&](const QString& name, const double value) {
      return [&, name, value] { findControl<SpinControl>(panel, name).setValue(value); };
    };
    const auto combo = [&](const QString& name, const int index) {
      return [&, name, index] {
        auto& comboBox = findControl<QComboBox>(panel, name);
        comboBox.setCurrentIndex(index);
        comboBox.activated(index);
      };
    };
    // a combo box whose items hold their values as data, in an order of their own
    const auto comboData = [&](const QString& name, const int value) {
      return [&, name, value] {
        auto& comboBox = findControl<QComboBox>(panel, name);
        const auto index = comboBox.findData(value);
        REQUIRE(index != -1);
        comboBox.setCurrentIndex(index);
        comboBox.activated(index);
      };
    };
    const auto checkBox = [&](const QString& name) {
      return [&, name] { findControl<QCheckBox>(panel, name).click(); };
    };

    checkWrites(
      "emit_falloff",
      "exponential",
      comboData("Falloff", int(mdl::LightFalloff::Exponential)));
    checkWrites("emit_size", "128", spin("EmitSize", 128.0));
    checkWrites("emit_start", "16", spin("EmitStart", 16.0));
    // auto range on recomputes the range, which is 96 * 3 already
    checkWrites("emit_auto_range", "1", checkBox("AutoRange"));
    checkWrites("light_radius", "400 400 400", spin("Range", 400.0));
    checkWrites("emit_cone", "90 60", spin("ConeH", 90.0));
    checkWrites("emit_cone", "60 30", spin("ConeV", 30.0));
    checkWrites("emit_penumbra", "0.5", spin("Penumbra", 0.5));
    checkWrites("emit_square", "4", spin("Squareness", 4.0));
    checkWrites("emit_pattern", "ice", combo("Pattern", 2));
    checkWrites("emit_pattern_strength", "0.5", spin("PatternStrength", 0.5));
    checkWrites("emit_pattern_scale", "2", spin("PatternScale", 2.0));
    checkWrites("emit_pattern_phase", "0.25", spin("PatternPhase", 0.25));
    checkWrites("emit_clamp", "0", checkBox("EnergyClamp"));
    checkWrites("emit_effect", "pulse", comboData("Effect", 13));
    checkWrites("emit_effect_speed", "2", spin("EffectSpeed", 2.0));
    checkWrites("emit_effect_amount", "0.5", spin("EffectAmount", 0.5));
    checkWrites("emit_effect_phase", "0.25", spin("EffectPhase", 0.25));
  }

  SECTION("with auto range, the intensity sets the range and the range the size")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* omniNode = addLight(map, emitterProperties("0 0 0", "omni", "288 288 288"));
    auto* smallNode = addLight(
      map,
      withValue(emitterProperties("512 0 0", "omni", "144 144 144"), "emit_size", "48"));
    auto* spotNode = addLight(map, emitterProperties("1024 0 0", "spot", "288 288 288"));

    auto panel = LightEditorPanel{fixture.context()};
    auto& intensity = findControl<SpinControl>(panel, "Intensity");

    // intensity 2: the clamped inverse square reaches sqrt(2 / 0.1 - 1) sizes of 96
    selectOnly(map, {omniNode});
    intensity.setValue(2.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "_color") == "2 2 2");
    CHECK(valueOf(*omniNode, "light_radius") == "418.45 418.45 418.45");
    CHECK(valueOf(*omniNode, "emit_size") == "96");
    map.undoCommand();
    QApplication::processEvents();

    // every selected light from its own values, in one command
    selectOnly(map, {omniNode, smallNode});
    const auto modificationCount = map.modificationCount();
    intensity.setValue(2.0);
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount + 1);
    CHECK(valueOf(*omniNode, "light_radius") == "418.45 418.45 418.45");
    CHECK(valueOf(*smallNode, "light_radius") == "209.23 209.23 209.23");
    map.undoCommand();
    QApplication::processEvents();

    // a spot's range sets its size: 384 / 3
    selectOnly(map, {spotNode});
    findControl<SpinControl>(panel, "Range").setValue(384.0);
    QApplication::processEvents();
    CHECK(valueOf(*spotNode, "light_radius") == "384 384 384");
    CHECK(valueOf(*spotNode, "emit_size") == "128");

    // an omni's radius is its range: its largest component sets the size, 600 / 3
    selectOnly(map, {omniNode});
    findControl<SpinControl>(panel, "RadiusX").setValue(600.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "600 288 288");
    CHECK(valueOf(*omniNode, "emit_size") == "200");

    // an emitter omni's range is at least 24: a radius of 10 is written as 24, and the
    // size follows the clamped range, 24 / 3
    findControl<SpinControl>(panel, "Radius").setValue(10.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "24 24 24");
    CHECK(valueOf(*omniNode, "emit_size") == "8");

    // also through a per-axis spin: 24 6 12 with X set to 20 is 20 6 12, scaled as a
    // whole by 24 / 20 to 24 7.2 14.4; the size follows the range of 24
    findControl<SpinControl>(panel, "RadiusY").setValue(6.0);
    QApplication::processEvents();
    findControl<SpinControl>(panel, "RadiusZ").setValue(12.0);
    QApplication::processEvents();
    REQUIRE(valueOf(*omniNode, "light_radius") == "24 6 12");
    findControl<SpinControl>(panel, "RadiusX").setValue(20.0);
    QApplication::processEvents();
    CHECK(valueOf(*omniNode, "light_radius") == "24 7.2 14.4");
    CHECK(valueOf(*omniNode, "emit_size") == "8");
  }

  SECTION("the kind switch round trip keeps the rotation, the cone and the range")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

    auto spotProperties =
      withValue(emitterProperties("0 0 0", "spot", "288 288 288"), "emit_cone", "60 40");
    spotProperties.emplace_back("angles", "90 0 0");
    auto* spotNode = addLight(map, std::move(spotProperties));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {spotNode});
    REQUIRE(showsEmitterPage(panel));
    const auto rotation = mdl::lightRotation(spotNode->entity(), policy);

    // to a texture spot: the pyramid round the cone, T = 288 - 8, the rotation unchanged
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(valueOf(*spotNode, "emitter") == "0");
    CHECK(valueOf(*spotNode, "light_target") == "280 0 0");
    CHECK(valueOf(*spotNode, "light_right") == "0 -161.66 0");
    CHECK(valueOf(*spotNode, "light_up") == "0 0 101.91");
    CHECK(property(*spotNode, "light_radius") == nullptr);
    CHECK(vm::is_equal(mdl::lightRotation(spotNode->entity(), policy), rotation, 1e-4));
    CHECK(findControl<SpinControl>(panel, "TargetX").isVisibleTo(&panel));

    // and back: the same axes, the same cone within the written decimals, the same range
    clickTab(panel, EmitterPage);
    CHECK(showsEmitterPage(panel));
    CHECK(valueOf(*spotNode, "emitter") == "1");
    CHECK(valueOf(*spotNode, "light_radius") == "288 288 288");
    for (const auto* key : {"light_target", "light_right", "light_up"})
    {
      CAPTURE(key);
      CHECK(property(*spotNode, key) == nullptr);
    }
    const auto light = mdl::parseLightProperties(spotNode->entity());
    CHECK(light.emitter.coneH == Catch::Approx(60.0).margin(0.01));
    CHECK(light.emitter.coneV == Catch::Approx(40.0).margin(0.01));
    CHECK(vm::is_equal(mdl::lightRotation(spotNode->entity(), policy), rotation, 1e-4));
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Kind");
  }

  SECTION("the notices of texture lights do not apply to an emitter")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // an emitter sun with stale spot keys and a style, which an emitter ignores
    auto sunProperties = emitterProperties("0 0 0", "sun", "2048 2048 1024");
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"parallel", "1"},
           {"light_target", "0 0 -256"},
           {"light_right", "-128 0 0"},
           {"light_up", "0 -128 0"},
           {"style", "3"},
         })
    {
      sunProperties.emplace_back(key, value);
    }
    auto* sunNode = addLight(map, std::move(sunProperties));
    auto* textureNode =
      addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}, {"style", "3"}});

    auto panel = LightEditorPanel{fixture.context()};

    selectOnly(map, {sunNode});
    CHECK_FALSE(findControl<QLabel>(panel, "StaleSpotKeysNotice").isVisibleTo(&panel));
    CHECK_FALSE(findControl<QLabel>(panel, "StyledNotice").isVisibleTo(&panel));
    CHECK_FALSE(findControl<QLabel>(panel, "BadTargetNotice").isVisibleTo(&panel));
    // an emitter's handle box is its volume
    CHECK_FALSE(findControl<SpinControl>(panel, "HandleThreshold").isEnabled());

    selectOnly(map, {textureNode});
    CHECK(findControl<QLabel>(panel, "StyledNotice").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "HandleThreshold").isEnabled());
  }

  SECTION("resetting an emitter's volume gives its shape's defaults")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    // auto range off, so that the default range of 300 stays
    auto* spotNode = addLight(
      map,
      withValue(
        withValue(
          emitterProperties("0 0 0", "spot", "500 500 500"), "emit_cone", "30 20"),
        "emit_auto_range",
        "0"));

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {spotNode});
    const auto before = spotNode->entity().properties();

    findControl<QToolButton>(panel, "ResetVolume").click();
    QApplication::processEvents();
    CHECK(
      spotNode->entity().properties()
      == withValue(withValue(before, "light_radius", "300 300 300"), "emit_cone", "60"));
  }
}

} // namespace tb::ui
