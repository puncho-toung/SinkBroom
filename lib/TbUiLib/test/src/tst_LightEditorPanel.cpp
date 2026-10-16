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
#include <QRadioButton>
#include <QSlider>
#include <QStackedWidget>

#include "SidebarTestUtils.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
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
#include "ui/MapDocument.h"
#include "ui/SpinControl.h"

#include "kd/result.h"

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
}

} // namespace tb::ui
