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
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>

#include "SidebarTestUtils.h"
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
#include "ui/CatchConfig.h"
#include "ui/ColorButton.h"
#include "ui/LightEditorPanel.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/RotateTool.h" // IWYU pragma: keep
#include "ui/ScaleTool.h"
#include "ui/SpinControl.h"

#include "kd/result.h"
#include "kd/task_manager.h"

#include "vm/bbox.h"
#include "vm/vec.h"

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
    CHECK(findControl<SpinControl>(panel, "Style").value() == 3.0);

    // without math, the math rows are hidden
    CHECK_FALSE(findControl<QCheckBox>(panel, "MathEnabled").isChecked());
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

  SECTION("the equilateral box shows the three components")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& equalRadius = findControl<QCheckBox>(panel, "EqualRadius");
    REQUIRE(equalRadius.isChecked());

    // unchecking writes nothing
    const auto modificationCount = map.modificationCount();
    equalRadius.click();
    QApplication::processEvents();
    CHECK(map.modificationCount() == modificationCount);
    CHECK(findControl<SpinControl>(panel, "RadiusY").isVisibleTo(&panel));

    findControl<SpinControl>(panel, "RadiusY").setValue(400.0);
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "300 400 300");
    CHECK_FALSE(equalRadius.isChecked());

    // checking makes the components equal to the first one
    equalRadius.click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    CHECK(findControl<SpinControl>(panel, "Radius").isVisibleTo(&panel));
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

  SECTION("enabling the math falloff writes the twelve math properties")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();

    auto* lightNode = addLight(
      map,
      {{"classname", "light"}, {"origin", "0 0 0"}, {"light_radius", "300 300 300"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& mathEnabled = findControl<QCheckBox>(panel, "MathEnabled");
    mathEnabled.click();
    QApplication::processEvents();

    // the in-game editor's auto-seed, with math_type from the light's type
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

    mathEnabled.click();
    QApplication::processEvents();
    for (const auto& key : mdl::mathLightPropertyKeys())
    {
      CAPTURE(key);
      CHECK(property(*lightNode, key) == nullptr);
    }
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
    // no definition: the style is a number
    auto* customNode =
      addLight(map, {{"classname", "light_custom"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& styleChoice = findControl<QComboBox>(panel, "StyleChoice");
    CHECK(styleChoice.isVisibleTo(&panel));
    CHECK_FALSE(findControl<SpinControl>(panel, "Style").isVisibleTo(&panel));
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
    CHECK_FALSE(styleChoice.isVisibleTo(&panel));
    CHECK(findControl<SpinControl>(panel, "Style").isVisibleTo(&panel));
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

    // tool buttons take no focus when they are clicked
    for (const auto* name : {"EditVolumeHandles", "DrawVolume"})
    {
      CAPTURE(name);
      const auto& button = findControl<QToolButton>(panel, name);
      CHECK(button.isCheckable());
      CHECK(button.isEnabled());
      CHECK_FALSE(button.isChecked());
    }

    auto& reset = findControl<QToolButton>(panel, "ResetVolume");
    CHECK(reset.isEnabled());
    reset.click();
    QApplication::processEvents();
    CHECK(valueOf(*lightNode, "light_radius") == "300 300 300");
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "Reset Light Volume");
  }

  SECTION("the volume tool buttons turn the light volume tool on and off")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto& map = fixture.document().map();
    auto& toolBox = fixture.context().toolBox;
    auto& tool = toolBox.lightVolumeTool();

    auto* lightNode = addLight(map, {{"classname", "light"}, {"origin", "0 0 0"}});

    auto panel = LightEditorPanel{fixture.context()};
    selectOnly(map, {lightNode});

    auto& handles = findControl<QToolButton>(panel, "EditVolumeHandles");
    auto& draw = findControl<QToolButton>(panel, "DrawVolume");

    handles.click();
    CHECK(tool.active());
    CHECK(tool.mode() == LightVolumeTool::Mode::Handles);
    CHECK(handles.isChecked());
    CHECK_FALSE(draw.isChecked());
    // the tool is not modal: Delete, Cut, Group and the nudges keep working (D31)
    CHECK_FALSE(toolBox.anyModalToolActive());

    draw.click();
    CHECK(tool.active());
    CHECK(tool.mode() == LightVolumeTool::Mode::Draw);
    CHECK_FALSE(handles.isChecked());
    CHECK(draw.isChecked());

    // Esc or the end of a drawn volume
    tool.setMode(LightVolumeTool::Mode::Handles);
    CHECK(handles.isChecked());
    CHECK_FALSE(draw.isChecked());

    handles.click();
    CHECK_FALSE(tool.active());
    CHECK_FALSE(handles.isChecked());
    CHECK_FALSE(draw.isChecked());

    draw.click();
    CHECK(tool.active());
    CHECK(tool.mode() == LightVolumeTool::Mode::Draw);
    draw.click();
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

    auto& handles = findControl<QToolButton>(panel, "EditVolumeHandles");
    handles.click();
    REQUIRE(toolBox.lightVolumeTool().active());

    toolBox.toggleTool(toolBox.rotateTool());
    CHECK_FALSE(toolBox.lightVolumeTool().active());
    CHECK_FALSE(handles.isChecked());
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
    auto& handles = findControl<QToolButton>(panel, "EditVolumeHandles");
    auto& draw = findControl<QToolButton>(panel, "DrawVolume");

    selectOnly(map, {lightNode});
    CHECK(handles.isEnabled());
    CHECK(draw.isEnabled());

    selectOnly(map, {lightNode, otherNode});
    REQUIRE(showsEditor(panel));
    CHECK_FALSE(handles.isEnabled());
    CHECK_FALSE(draw.isEnabled());

    // the editor page is read-only (D8)
    selectOnly(map, {legacyNode});
    REQUIRE(showsEditor(panel));
    CHECK_FALSE(handles.isEnabled());
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

    findControl<QToolButton>(panel, "EditVolumeHandles").click();
    REQUIRE(tool.active());

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
}

} // namespace tb::ui
