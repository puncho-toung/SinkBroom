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
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "SidebarTestUtils.h"
#include "base/PreferenceManager.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/Light.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
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
#include "kd/task_manager.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <string>
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

// the pages of the editor's tab book
constexpr auto MathPage = 0;
constexpr auto TexturePage = 1;

bool showsMathPage(const LightEditorPanel& panel)
{
  return findControl<TabBookPage>(panel, "MathPage").isVisibleTo(&panel);
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

    // without math, the texture page is shown and the math rows are not
    CHECK(showsTexturePage(panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "InnerRadius").isVisibleTo(&panel));
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

  SECTION("the Math tab makes a light without math a math light with the seed")
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
    clickTab(panel, MathPage);
    CHECK(showsMathPage(panel));
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Math");

    // the in-game editor's auto-seed, with math_type from the light's type: twelve
    // properties and nothing else
    CHECK(lightNode->entity().properties().size() == before.size() + 12);
    CHECK(property(*lightNode, "math_style") == nullptr);
    CHECK(valueOf(*lightNode, "math_enabled") == "1");
    CHECK(valueOf(*lightNode, "math_type") == "point");
    CHECK(valueOf(*lightNode, "math_falloff") == "linear");
    CHECK(valueOf(*lightNode, "math_pattern") == "none");
    CHECK(valueOf(*lightNode, "math_falloff_exp") == "2");
    CHECK(valueOf(*lightNode, "math_inner_radius") == "0.1");
    CHECK(valueOf(*lightNode, "math_shape_exp") == "2");
    CHECK(valueOf(*lightNode, "math_penumbra") == "0.15");
    CHECK(valueOf(*lightNode, "math_pattern_strength") == "0");
    CHECK(valueOf(*lightNode, "math_pattern_scale") == "1");
    CHECK(valueOf(*lightNode, "math_pattern_phase") == "0");
    CHECK(valueOf(*lightNode, "math_energy_clamp") == "1");
    CHECK(
      mdl::parseLightProperties(lightNode->entity()).math
      == mdl::seededMathLightProperties());

    CHECK(findControl<SpinControl>(panel, "InnerRadius").isVisibleTo(&panel));
    // shape and penumbra belong to spot lights
    CHECK_FALSE(findControl<SpinControl>(panel, "ShapeExp").isVisibleTo(&panel));

    findControl<SpinControl>(panel, "InnerRadius").setValue(0.25);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_inner_radius") == "0.25");

    // a slider that the keyboard moves writes at once
    findControl<QSlider>(panel, "InnerRadiusSlider").setValue(500);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_inner_radius") == "0.5");
    CHECK(findControl<SpinControl>(panel, "InnerRadius").value() == 0.5);

    // the Texture tab writes math_enabled 0 alone and keeps the other properties (D51)
    auto mathProperties = lightNode->entity().properties();
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(
      lightNode->entity().properties() == withValue(mathProperties, "math_enabled", "0"));

    // the Math tab gives the light its kept values back and writes nothing else
    clickTab(panel, MathPage);
    CHECK(showsMathPage(panel));
    CHECK(lightNode->entity().properties() == mathProperties);
    CHECK(findControl<SpinControl>(panel, "InnerRadius").value() == 0.5);

    // a click on the tab of the page that is shown writes nothing
    const auto modificationCount = map.modificationCount();
    clickTab(panel, MathPage);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(showsMathPage(panel));
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
    for (const auto* name : {"RadiusX", "TargetX", "OriginX", "AnglesX", "Intensity"})
    {
      CAPTURE(name);
      CHECK(geometry->isAncestorOf(&findControl<SpinControl>(panel, name)));
    }
    CHECK(geometry->isAncestorOf(&findControl<ColorButton>(panel, "Color")));
    for (const auto* name : {"CastShadows", "SkipSpecular"})
    {
      CAPTURE(name);
      CHECK(flags->isAncestorOf(&findControl<QCheckBox>(panel, name)));
    }

    // the Math page, then the Texture page, each with only its own controls
    const auto buttons = tabButtons(panel);
    const auto title = [](const TabBarButton* button) {
      const auto* label = button->findChild<QLabel*>();
      REQUIRE(label != nullptr);
      return label->text();
    };
    CHECK(title(buttons[MathPage]) == "Math");
    CHECK(title(buttons[TexturePage]) == "Texture");

    auto& mathPage = findControl<TabBookPage>(panel, "MathPage");
    auto& texturePage = findControl<TabBookPage>(panel, "TexturePage");
    CHECK(tabs.isAncestorOf(&mathPage));
    CHECK(tabs.isAncestorOf(&texturePage));

    auto mathControls = std::vector<QWidget*>{
      &findControl<QLabel>(panel, "MathType"),
      &findControl<QComboBox>(panel, "MathStyle"),
      &findControl<QComboBox>(panel, "Falloff"),
      &findControl<QComboBox>(panel, "Pattern"),
      &findControl<QCheckBox>(panel, "EnergyClamp"),
    };
    for (const auto* name :
         {"FalloffExp",
          "InnerRadius",
          "ShapeExp",
          "Penumbra",
          "PatternStrength",
          "PatternScale",
          "PatternPhase"})
    {
      mathControls.push_back(&findControl<SpinControl>(panel, name));
    }
    for (auto* control : mathControls)
    {
      CAPTURE(control->objectName().toStdString());
      CHECK(mathPage.isAncestorOf(control));
      CHECK_FALSE(texturePage.isAncestorOf(control));
    }
    for (const auto* name : {"Texture", "StyleChoice"})
    {
      CAPTURE(name);
      auto& control = findControl<QComboBox>(panel, name);
      CHECK(texturePage.isAncestorOf(&control));
      CHECK_FALSE(mathPage.isAncestorOf(&control));
    }

    // the tab is the switch, and the style a list
    CHECK(panel.findChild<QCheckBox*>("LightEditorPanel_MathEnabled") == nullptr);
    CHECK(panel.findChild<SpinControl*>("LightEditorPanel_Style") == nullptr);
  }

  SECTION("the page follows the selected lights and switching it writes nothing")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* mathNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"math_enabled", "1"},
        {"math_falloff", "smooth"},
      });
    auto* textureNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});
    auto* disabledNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "128 0 0"},
        {"math_enabled", "0"},
        {"math_falloff", "smooth"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    auto& mixedNotice = findControl<QLabel>(panel, "MixedMathNotice");
    auto& texturePage = findControl<TabBookPage>(panel, "TexturePage");
    const auto modificationCount = map.modificationCount();

    selectOnly(map, {mathNode});
    CHECK(showsMathPage(panel));
    CHECK_FALSE(showsTexturePage(panel));
    CHECK_FALSE(mixedNotice.isVisibleTo(&panel));
    CHECK(findControl<QComboBox>(panel, "Falloff").isVisibleTo(&panel));

    selectOnly(map, {textureNode});
    CHECK(showsTexturePage(panel));
    CHECK_FALSE(showsMathPage(panel));
    CHECK(findControl<QComboBox>(panel, "Texture").isVisibleTo(&panel));

    // kept math properties do not make a math light
    selectOnly(map, {disabledNode});
    CHECK(showsTexturePage(panel));

    // lights of both kinds: the Math page with the notice, the texture options off
    selectOnly(map, {mathNode, textureNode});
    CHECK(showsMathPage(panel));
    CHECK(mixedNotice.isVisibleTo(&panel));
    CHECK_FALSE(findControl<QComboBox>(panel, "Falloff").isVisibleTo(&panel));
    CHECK_FALSE(texturePage.isEnabled());
    CHECK_FALSE(findControl<QComboBox>(panel, "Texture").isEnabled());

    selectOnly(map, {mathNode});
    CHECK_FALSE(mixedNotice.isVisibleTo(&panel));
    CHECK(texturePage.isEnabled());

    CHECK(map.modificationCount() == modificationCount);
  }

  SECTION("the tabs switch every selected light")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* mathNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"math_enabled", "1"},
        {"math_type", "point"},
        {"math_falloff", "smooth"},
        {"math_style", "4"},
      });
    auto* legacyNode = addLight(map, {{"classname", "light"}, {"origin", "64 0 0"}});
    // as the Texture tab leaves a math light
    auto* disabledNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "128 0 0"},
        {"math_enabled", "0"},
        {"math_type", "point"},
        {"math_falloff", "exponential"},
        {"math_pattern", "ice"},
        {"math_falloff_exp", "3"},
        {"math_inner_radius", "0.3"},
        {"math_shape_exp", "2"},
        {"math_penumbra", "0.15"},
        {"math_pattern_strength", "0.5"},
        {"math_pattern_scale", "2"},
        {"math_pattern_phase", "0"},
        {"math_energy_clamp", "0"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {mathNode, legacyNode, disabledNode});
    REQUIRE(showsMathPage(panel));

    const auto mathBefore = mathNode->entity().properties();
    const auto disabledBefore = disabledNode->entity().properties();

    // in one command: the legacy light gets the seed, the disabled one its kept values
    const auto modificationCount = map.modificationCount();
    clickTab(panel, MathPage);
    CHECK(map.modificationCount() == modificationCount + 1);
    CHECK(mathNode->entity().properties() == mathBefore);
    CHECK(
      mdl::parseLightProperties(legacyNode->entity()).math
      == mdl::seededMathLightProperties());
    CHECK(
      disabledNode->entity().properties()
      == withValue(disabledBefore, "math_enabled", "1"));
    CHECK_FALSE(findControl<QLabel>(panel, "MixedMathNotice").isVisibleTo(&panel));

    // the texture tab keeps every math property but math_enabled, math_style too
    clickTab(panel, TexturePage);
    CHECK(showsTexturePage(panel));
    CHECK(mathNode->entity().properties() == withValue(mathBefore, "math_enabled", "0"));
    CHECK(disabledNode->entity().properties() == disabledBefore);
    CHECK(valueOf(*legacyNode, "math_enabled") == "0");
    CHECK(valueOf(*legacyNode, "math_falloff") == "linear");
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
    tabButtons(panel)[MathPage]->clicked();
    CHECK(lightNode->entity().properties() == lightProperties);
    CHECK(playerStartNode->entity().properties() == playerStartProperties);

    QApplication::processEvents();
    CHECK(currentText(panel) == "Select a light");
  }

  SECTION("the math style writes math_style and the texture style style")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "0 0 0"},
        {"math_enabled", "1"},
        {"math_type", "point"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});
    REQUIRE(showsMathPage(panel));

    // no style and the engine's twelve predefined styles (D54)
    auto& mathStyle = findControl<QComboBox>(panel, "MathStyle");
    REQUIRE(mathStyle.count() == 13);
    CHECK(mathStyle.itemText(0) == "-1: None");
    CHECK(mathStyle.itemText(1) == "0: Normal");
    CHECK(mathStyle.itemText(5) == "4: Fast Strobe");
    CHECK(mathStyle.itemText(12) == "11: Slow Pulse (no black)");
    CHECK(mathStyle.currentIndex() == 0);

    const auto before = lightNode->entity().properties();
    mathStyle.setCurrentIndex(5);
    mathStyle.activated(5);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_style") == "4");
    CHECK(property(*lightNode, "style") == nullptr);
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Set Light Math Style");
    CHECK(mathStyle.currentIndex() == 5);

    mathStyle.setCurrentIndex(0);
    mathStyle.activated(0);
    QApplication::processEvents();
    CHECK(lightNode->entity().properties() == before);
    CHECK(mathStyle.currentIndex() == 0);

    // the texture page's style still writes style
    clickTab(panel, TexturePage);
    auto& styleChoice = findControl<QComboBox>(panel, "StyleChoice");
    const auto index = styleChoice.findData("4");
    REQUIRE(index != -1);
    styleChoice.setCurrentIndex(index);
    styleChoice.activated(index);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "style") == "4");
    CHECK(property(*lightNode, "math_style") == nullptr);
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

    // not a light property
    const auto modificationCount = map.modificationCount();
    threshold.setValue(25.0);
    QApplication::processEvents();
    CHECK(pref(Preferences::LightVolumeHandleThreshold) == 0.25f);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(threshold.value() == 25.0);

    setPref(Preferences::LightVolumeHandleThreshold, 0.4f);
    QApplication::processEvents();
    CHECK(threshold.value() == 40.0);
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
    CHECK(findControl<QLabel>(panel, "MathType").text() == "multi");

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

    auto taskManager = kdl::task_manager{};
    map.entityModelManager().reloadShaders(taskManager);
    fixture.document().modsDidChangeNotifier();
    QApplication::processEvents();
    CHECK(items() == expectedItems);
    CHECK(texture.currentIndex() == 2);
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

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    // the handles work without the tool (D56)
    REQUIRE_FALSE(tool.active());

    auto& originX = findControl<SpinControl>(panel, "OriginX");
    REQUIRE(originX.value() == 0.0);

    REQUIRE(tool.beginDrag(mdl::Hit{
      LightVolumeTool::SideHitType,
      0.0,
      vm::vec3d{0, 0, 0},
      BBoxSide{vm::vec3d{1, 0, 0}}}));
    REQUIRE(tool.update(vm::vec3d{32, 0, 0}));
    REQUIRE(valueOf(*lightNode, "origin") == "16 0 0");
    QApplication::processEvents();
    CHECK(originX.value() == 0.0);

    tool.endDrag();
    QApplication::processEvents();
    CHECK(originX.value() == 16.0);
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
        {"math_enabled", "1"},
        {"math_type", "point"},
        {"math_falloff", "smooth"},
        {"math_pattern", "none"},
        {"math_falloff_exp", "3"},
        {"math_inner_radius", "0.2"},
        {"math_shape_exp", "2"},
        {"math_penumbra", "0.15"},
        {"math_pattern_strength", "0"},
        {"math_pattern_scale", "1"},
        {"math_pattern_phase", "0"},
        {"math_energy_clamp", "1"},
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

    // the texture, style, flags, colour and math block stay
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

  SECTION("the math falloff combos and the rows they show")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    clickTab(panel, MathPage);
    REQUIRE(valueOf(*lightNode, "math_falloff") == "linear");

    const auto items = [](const QComboBox& comboBox) {
      auto result = QStringList{};
      for (int i = 0; i < comboBox.count(); ++i)
      {
        result.push_back(comboBox.itemText(i));
      }
      return result;
    };

    // the in-game editor's labels
    auto& falloff = findControl<QComboBox>(panel, "Falloff");
    auto& pattern = findControl<QComboBox>(panel, "Pattern");
    CHECK(
      items(falloff)
      == QStringList{
        "Inverse Square (unbounded)",
        "Linear",
        "Smooth",
        "Exponential",
        "Inverse Square (clamped) - PBR safe",
      });
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

    // the exponent is unused by the linear and the smooth curve
    auto& falloffExp = findControl<SpinControl>(panel, "FalloffExp");
    auto& falloffExpSlider = findControl<QSlider>(panel, "FalloffExpSlider");
    CHECK(falloff.currentIndex() == 1);
    CHECK_FALSE(falloffExp.isEnabled());
    CHECK_FALSE(falloffExpSlider.isEnabled());

    falloff.setCurrentIndex(3);
    falloff.activated(3);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_falloff") == "exponential");
    CHECK(falloffExp.isEnabled());
    CHECK(falloffExpSlider.isEnabled());

    falloff.setCurrentIndex(2);
    falloff.activated(2);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_falloff") == "smooth");
    CHECK_FALSE(falloffExp.isEnabled());
    CHECK_FALSE(falloffExpSlider.isEnabled());

    // the pattern fields only with a pattern
    for (const auto* name : {"PatternStrength", "PatternScale", "PatternPhase"})
    {
      CAPTURE(name);
      CHECK_FALSE(findControl<SpinControl>(panel, name).isVisibleTo(&panel));
    }
    pattern.setCurrentIndex(3);
    pattern.activated(3);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_pattern") == "caustic");
    for (const auto* name : {"PatternStrength", "PatternScale", "PatternPhase"})
    {
      CAPTURE(name);
      CHECK(findControl<SpinControl>(panel, name).isVisibleTo(&panel));
      CHECK(findControl<QSlider>(panel, QString{name} + "Slider").isVisibleTo(&panel));
    }

    findControl<QCheckBox>(panel, "EnergyClamp").click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "math_energy_clamp") == "0");
  }

  SECTION("the math type follows the light type")
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
    auto* sunNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "128 0 0"},
        {"parallel", "1"},
        {"light_radius", "2048 2048 1024"},
      });

    auto panel = LightEditorPanel{fixture.context()};
    auto& mathType = findControl<QLabel>(panel, "MathType");

    selectOnly(map, {pointNode});
    clickTab(panel, MathPage);
    CHECK(valueOf(*pointNode, "math_type") == "point");
    CHECK(mathType.text() == "point (follows the light type)");

    // a change of type keeps the math block and writes its type (D40)
    const auto mathBefore = pointNode->entity().properties();
    findControl<QRadioButton>(panel, "Spot").click();
    QApplication::processEvents();
    CHECK(valueOf(*pointNode, "math_type") == "spot");
    for (const auto& key : mdl::mathLightPropertyKeys())
    {
      // the seed has no math_style
      if (key != "math_type" && key != "math_style")
      {
        CAPTURE(key);
        const auto before = std::ranges::find_if(
          mathBefore, [&](const auto& property) { return property.key() == key; });
        REQUIRE(before != mathBefore.end());
        CHECK(valueOf(*pointNode, key) == before->value());
      }
    }
    CHECK(mathType.text() == "spot (follows the light type)");

    selectOnly(map, {spotNode});
    clickTab(panel, MathPage);
    CHECK(valueOf(*spotNode, "math_type") == "spot");
    // shape and penumbra belong to spot lights
    CHECK(findControl<SpinControl>(panel, "ShapeExp").isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "Penumbra").isVisibleTo(&panel));

    selectOnly(map, {sunNode});
    clickTab(panel, MathPage);
    CHECK(valueOf(*sunNode, "math_type") == "sun");
    CHECK(mathType.text() == "sun (follows the light type)");
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

  SECTION("reads spot lights, suns, math lights and colours")
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
    auto* mathNode = addLight(
      map,
      {
        {"classname", "light"},
        {"origin", "192 0 0"},
        {"math_enabled", "1"},
        {"math_falloff", "exponential"},
        {"math_pattern", "banded"},
        {"math_falloff_exp", "4"},
        {"math_inner_radius", "0.25"},
        {"math_pattern_scale", "2"},
      });

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

    // the sliders map the in-game ranges to 0..1000
    selectOnly(map, {mathNode});
    CHECK(showsMathPage(panel));
    CHECK(findControl<QComboBox>(panel, "Falloff").currentIndex() == 3);
    CHECK(findControl<QComboBox>(panel, "Pattern").currentIndex() == 4);
    CHECK(findControl<SpinControl>(panel, "FalloffExp").value() == 4.0);
    CHECK(findControl<QSlider>(panel, "FalloffExpSlider").value() == 245);
    CHECK(findControl<SpinControl>(panel, "InnerRadius").value() == 0.25);
    CHECK(findControl<QSlider>(panel, "InnerRadiusSlider").value() == 250);
    CHECK(findControl<SpinControl>(panel, "PatternScale").value() == 2.0);
    CHECK(findControl<QSlider>(panel, "PatternScaleSlider").value() == 30);
  }
}

} // namespace tb::ui
