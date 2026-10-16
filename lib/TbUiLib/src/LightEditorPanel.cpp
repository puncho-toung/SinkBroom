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

#include "ui/LightEditorPanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "base/PreferenceManager.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityRotation.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Light.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/Selection.h"
#include "ui/ColorButton.h"
#include "ui/FormWithSectionsLayout.h"
#include "ui/MapDocument.h"
#include "ui/SignalDelayer.h"
#include "ui/SpinControl.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{

enum class PanelState
{
  NoLightEditor,
  NoLightSelected,
  NotOnlyLightsSelected,
  OnlyLightsSelected,
};

PanelState panelState(const mdl::Map& map)
{
  if (!mdl::hasLightEditor(map.gameInfo().gameConfig))
  {
    return PanelState::NoLightEditor;
  }

  // the selected entity nodes only: allEntities() also yields worldspawn for a selected
  // brush and the parent entity of a selected brush or patch
  const auto& selection = map.selection();
  const auto isLight = [](const auto* entityNode) {
    return mdl::isLightEntity(entityNode->entity());
  };

  if (selection.hasOnlyEntities() && std::ranges::all_of(selection.entities, isLight))
  {
    return PanelState::OnlyLightsSelected;
  }
  if (std::ranges::any_of(selection.entities, isLight))
  {
    return PanelState::NotOnlyLightsSelected;
  }
  return PanelState::NoLightSelected;
}

/**
 * The lights that a write changes: the selected light nodes, or none if the selection
 * does not consist of lights only. Every write reads the selection anew, because a spin
 * box that loses the focus writes after the selection has changed.
 */
std::vector<mdl::EntityNode*> selectedLightNodes(const mdl::Map& map)
{
  return panelState(map) == PanelState::OnlyLightsSelected
           ? map.selection().entities
           : std::vector<mdl::EntityNode*>{};
}

using NodesToSwap = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>;

/**
 * Swaps the given contents in as one command. Returns false if nothing was swapped in,
 * because there was nothing to swap or because the command failed (a conflict between
 * linked groups).
 */
bool swapNodeContents(
  mdl::Map& map, const std::string& commandName, NodesToSwap nodesToSwap)
{
  return !nodesToSwap.empty()
         && mdl::updateNodeContents(map, commandName, std::move(nodesToSwap));
}

QWidget* createMessagePage(QLabel* label)
{
  label->setAlignment(Qt::AlignCenter);
  label->setWordWrap(true);

  auto* layout = new QVBoxLayout{};
  layout->addWidget(label, 1);

  auto* page = new QWidget{};
  page->setLayout(layout);
  return page;
}

QString controlName(const QString& name)
{
  return "LightEditorPanel_" + name;
}

struct SpinRange
{
  double min;
  double max;
  int decimals;
  double regularIncrement;
  double shiftIncrement;
  double ctrlIncrement;
};

// positions and extents in world units, written with two decimals
constexpr auto PositionRange = SpinRange{-131072.0, 131072.0, 2, 1.0, 8.0, 64.0};
constexpr auto RadiusRange = SpinRange{0.0, 131072.0, 2, 1.0, 8.0, 64.0};
constexpr auto AngleRange = SpinRange{-360.0, 360.0, 2, 1.0, 15.0, 45.0};
// the in-game editor's le_maxIntensity; written with four decimals
constexpr auto IntensityRange = SpinRange{0.0, 8.0, 4, 0.1, 0.5, 1.0};
constexpr auto StyleRange = SpinRange{-1.0, 11.0, 0, 1.0, 1.0, 1.0};
// the slider ranges of the in-game editor (LightEditor.cpp); written with four decimals
constexpr auto FalloffExpRange = SpinRange{0.1, 16.0, 4, 0.1, 1.0, 2.0};
constexpr auto InnerRadiusRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto ShapeExpRange = SpinRange{1.5, 32.0, 4, 0.1, 1.0, 4.0};
constexpr auto PenumbraRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto PatternStrengthRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto PatternScaleRange = SpinRange{0.1, 64.0, 4, 0.1, 1.0, 4.0};
constexpr auto PatternPhaseRange = SpinRange{0.0, 100.0, 4, 0.1, 1.0, 10.0};

constexpr auto SliderSteps = 1000;

SpinControl* createSpin(const QString& name, const SpinRange& range)
{
  auto* spin = new SpinControl{};
  spin->setObjectName(controlName(name));
  spin->setDigits(0, range.decimals);
  spin->setRange(range.min, range.max);
  spin->setIncrements(range.regularIncrement, range.shiftIncrement, range.ctrlIncrement);
  return spin;
}

QHBoxLayout* createRowLayout()
{
  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(LayoutConstants::NarrowHMargin);
  return layout;
}

QLayout* createVec3Layout(
  std::array<SpinControl*, 3>& spins, const QString& name, const SpinRange& range)
{
  const auto suffixes = std::array<QString, 3>{"X", "Y", "Z"};

  auto* layout = createRowLayout();
  for (size_t i = 0; i < 3; ++i)
  {
    spins[i] = createSpin(name + suffixes[i], range);
    layout->addWidget(spins[i], 1);
  }
  return layout;
}

QLayout* createSliderLayout(
  QSlider*& slider, SpinControl*& spin, const QString& name, const SpinRange& range)
{
  slider = new QSlider{Qt::Horizontal};
  slider->setObjectName(controlName(name + "Slider"));
  slider->setRange(0, SliderSteps);

  spin = createSpin(name, range);

  auto* layout = createRowLayout();
  layout->addWidget(slider, 1);
  layout->addWidget(spin);
  return layout;
}

QCheckBox* createCheckBox(const QString& name, const QString& text)
{
  auto* checkBox = new QCheckBox{text};
  checkBox->setObjectName(controlName(name));
  return checkBox;
}

QComboBox* createComboBox(const QString& name, const QStringList& items)
{
  auto* comboBox = new QComboBox{};
  comboBox->setObjectName(controlName(name));
  comboBox->addItems(items);
  return comboBox;
}

int addRow(FormWithSectionsLayout& form, const QString& label, QWidget* field)
{
  const auto row = form.rowCount();
  form.addRow(label, field);
  return row;
}

int addRow(FormWithSectionsLayout& form, const QString& label, QLayout* field)
{
  const auto row = form.rowCount();
  form.addRow(label, field);
  return row;
}

int addRow(FormWithSectionsLayout& form, QWidget* field)
{
  const auto row = form.rowCount();
  form.addRow(field);
  return row;
}

double sliderValue(const SpinControl& spin, const int position)
{
  const auto value =
    spin.minimum() + (spin.maximum() - spin.minimum()) * position / SliderSteps;
  const auto scale = std::pow(10.0, spin.decimals());
  return std::round(value * scale) / scale;
}

int sliderPosition(const SpinControl& spin, const double value)
{
  const auto t = (std::clamp(value, spin.minimum(), spin.maximum()) - spin.minimum())
                 / (spin.maximum() - spin.minimum());
  return int(std::lround(t * SliderSteps));
}

// the colour of a light is its hue times its intensity (D41)
double intensityOf(const vm::vec3d& color)
{
  return std::max(vm::get_max_component(color), 0.0);
}

vm::vec3d hueOf(const vm::vec3d& color)
{
  const auto intensity = intensityOf(color);
  return intensity > 0.0 ? color / intensity : vm::vec3d{1, 1, 1};
}

QColor toQColor(const vm::vec3d& hue)
{
  return QColor::fromRgbF(
    float(std::clamp(hue.x(), 0.0, 1.0)),
    float(std::clamp(hue.y(), 0.0, 1.0)),
    float(std::clamp(hue.z(), 0.0, 1.0)));
}

/**
 * The math block that "Enable" gives a light without one: the in-game editor's
 * auto-seed (LightEditor.cpp), a soft linear falloff (D29).
 */
mdl::MathLightProperties seededMathLightProperties()
{
  auto math = mdl::MathLightProperties{};
  math.enabled = true;
  math.falloff = mdl::LightFalloff::Linear;
  math.falloffExp = 2.0;
  math.innerRadius = 0.1;
  math.shapeExp = 2.0;
  math.penumbra = 0.15;
  math.pattern = mdl::LightPattern::None;
  math.patternStrength = 0.0;
  math.patternScale = 1.0;
  math.patternPhase = 0.0;
  math.energyClamp = true;
  return math;
}

/**
 * The entity's rotation as pitch, yaw and roll in degrees with a positive pitch pointing
 * down, the way the id Tech 4 policy writes "angles".
 */
vm::vec3d pitchYawRoll(const mdl::Entity& entity, const mdl::EntityRotationPolicy policy)
{
  const auto yawPitchRoll =
    mdl::entityYawPitchRoll(vm::mat4x4d::identity(), mdl::entityRotation(entity, policy));
  return vm::is_nan(yawPitchRoll)
           ? vm::vec3d{0, 0, 0}
           : vm::vec3d{yawPitchRoll.y(), yawPitchRoll.x(), yawPitchRoll.z()};
}

std::string formatAngles(const vm::vec3d& pitchYawRoll)
{
  return mdl::formatIdTech4Float(pitchYawRoll.x(), 2) + " "
         + mdl::formatIdTech4Float(pitchYawRoll.y(), 2) + " "
         + mdl::formatIdTech4Float(pitchYawRoll.z(), 2);
}

QString lightName(const mdl::Entity& entity)
{
  const auto* name = entity.property("name");
  return QString::fromStdString(name && !name->empty() ? *name : entity.classname());
}

const auto EngineDefaultTexture = QString{"(engine default)"};

void setSpinValue(SpinControl* spin, const double value)
{
  spin->setSpecialValueText("");
  spin->setValue(value);
}

void setVec3Value(const std::array<SpinControl*, 3>& spins, const vm::vec3d& value)
{
  for (size_t i = 0; i < 3; ++i)
  {
    setSpinValue(spins[i], value[i]);
  }
}

void setSliderValue(QSlider* slider, SpinControl* spin, const double value)
{
  setSpinValue(spin, value);
  slider->setValue(sliderPosition(*spin, value));
}

} // namespace

struct LightEditorPanel::Controls
{
  QLabel* header = nullptr;
  QWidget* editor = nullptr;
  FormWithSectionsLayout* form = nullptr;

  // every control that the user changes, blocked while the controls are set
  std::vector<QObject*> inputs;

  QButtonGroup* type = nullptr;
  std::array<QRadioButton*, 3> typeRadios = {};

  QCheckBox* equalRadius = nullptr;
  SpinControl* radius = nullptr;
  std::array<SpinControl*, 3> radiusXyz = {};
  QCheckBox* center = nullptr;
  std::array<SpinControl*, 3> centerXyz = {};

  std::array<SpinControl*, 3> target = {};
  std::array<SpinControl*, 3> right = {};
  std::array<SpinControl*, 3> up = {};
  QCheckBox* explicitStartEnd = nullptr;
  std::array<SpinControl*, 3> start = {};
  std::array<SpinControl*, 3> end = {};

  std::array<SpinControl*, 3> origin = {};
  std::array<SpinControl*, 3> angles = {};

  ColorButton* color = nullptr;
  SpinControl* intensity = nullptr;
  QComboBox* texture = nullptr;
  SpinControl* style = nullptr;

  QCheckBox* mathEnabled = nullptr;
  QLabel* mathType = nullptr;
  QComboBox* falloff = nullptr;
  QSlider* falloffExpSlider = nullptr;
  SpinControl* falloffExp = nullptr;
  QSlider* innerRadiusSlider = nullptr;
  SpinControl* innerRadius = nullptr;
  QSlider* shapeExpSlider = nullptr;
  SpinControl* shapeExp = nullptr;
  QSlider* penumbraSlider = nullptr;
  SpinControl* penumbra = nullptr;
  QComboBox* pattern = nullptr;
  QSlider* patternStrengthSlider = nullptr;
  SpinControl* patternStrength = nullptr;
  QSlider* patternScaleSlider = nullptr;
  SpinControl* patternScale = nullptr;
  QSlider* patternPhaseSlider = nullptr;
  SpinControl* patternPhase = nullptr;
  QCheckBox* energyClamp = nullptr;

  QCheckBox* castShadows = nullptr;
  QCheckBox* skipSpecular = nullptr;

  // the rows that depend on the light's type and options
  int equalRadiusRow = -1;
  int radiusRow = -1;
  int radiusXyzRow = -1;
  int centerRow = -1;
  int centerXyzRow = -1;
  int targetRow = -1;
  int rightRow = -1;
  int upRow = -1;
  int explicitStartEndRow = -1;
  int startRow = -1;
  int endRow = -1;
  int mathTypeRow = -1;
  int falloffRow = -1;
  int falloffExpRow = -1;
  int innerRadiusRow = -1;
  int shapeExpRow = -1;
  int penumbraRow = -1;
  int patternRow = -1;
  int patternStrengthRow = -1;
  int patternScaleRow = -1;
  int patternPhaseRow = -1;
  int energyClampRow = -1;
};

LightEditorPanel::LightEditorPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_document{context.document}
  , m_controls{std::make_unique<Controls>()}
  , m_updateSignalDelayer{new SignalDelayer{this}}
{
  createGui();
  connectEditor();
  connectObservers();
  updateControls();
}

LightEditorPanel::~LightEditorPanel() = default;

QString LightEditorPanel::panelId() const
{
  return "LightEditor";
}

QString LightEditorPanel::title() const
{
  return tr("Light Editor");
}

void LightEditorPanel::panelShown()
{
  refresh();
}

void LightEditorPanel::createGui()
{
  m_messageLabel = new QLabel{};
  m_messageLabel->setObjectName("LightEditorPanel_Message");

  m_stack = new QStackedWidget{};
  m_stack->setObjectName("LightEditorPanel_Stack");
  m_stack->addWidget(createMessagePage(m_messageLabel));
  m_stack->addWidget(createEditorPage());

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_stack);
  setLayout(layout);
}

QWidget* LightEditorPanel::createEditorPage()
{
  auto& c = *m_controls;

  c.header = new QLabel{};
  c.header->setObjectName(controlName("Header"));
  c.header->setWordWrap(true);

  auto* form = new FormWithSectionsLayout{};
  form->setContentsMargins(0, LayoutConstants::WideVMargin, 0, 0);
  form->setHorizontalSpacing(LayoutConstants::NarrowHMargin);
  form->setVerticalSpacing(LayoutConstants::NarrowVMargin);
  c.form = form;

  // type
  form->addSection(tr("Type"));
  c.type = new QButtonGroup{this};
  auto* typeLayout = createRowLayout();
  const auto typeNames = std::array<QString, 3>{tr("Point"), tr("Spot"), tr("Sun")};
  const auto typeObjectNames = std::array<QString, 3>{"Point", "Spot", "Sun"};
  for (size_t i = 0; i < 3; ++i)
  {
    c.typeRadios[i] = new QRadioButton{typeNames[i]};
    c.typeRadios[i]->setObjectName(controlName(typeObjectNames[i]));
    c.type->addButton(c.typeRadios[i], int(i));
    typeLayout->addWidget(c.typeRadios[i]);
  }
  typeLayout->addStretch(1);
  form->addRow(typeLayout);

  // volume
  form->addSection(tr("Volume"));
  c.equalRadius = createCheckBox("EqualRadius", tr("Equilateral"));
  c.equalRadiusRow = addRow(*form, c.equalRadius);
  c.radius = createSpin("Radius", RadiusRange);
  c.radiusRow = addRow(*form, tr("Radius"), c.radius);
  c.radiusXyzRow =
    addRow(*form, tr("Radius"), createVec3Layout(c.radiusXyz, "Radius", RadiusRange));
  c.center = createCheckBox("Center", tr("Center"));
  c.centerRow = addRow(*form, c.center);
  c.centerXyzRow =
    addRow(*form, tr("Center"), createVec3Layout(c.centerXyz, "Center", PositionRange));
  c.targetRow =
    addRow(*form, tr("Target"), createVec3Layout(c.target, "Target", PositionRange));
  c.rightRow =
    addRow(*form, tr("Right"), createVec3Layout(c.right, "Right", PositionRange));
  c.upRow = addRow(*form, tr("Up"), createVec3Layout(c.up, "Up", PositionRange));
  c.explicitStartEnd =
    createCheckBox("ExplicitStartEnd", tr("Explicit start and end points"));
  c.explicitStartEndRow = addRow(*form, c.explicitStartEnd);
  c.startRow =
    addRow(*form, tr("Start"), createVec3Layout(c.start, "Start", PositionRange));
  c.endRow = addRow(*form, tr("End"), createVec3Layout(c.end, "End", PositionRange));

  // transform
  form->addSection(tr("Transform"));
  form->addRow(tr("Origin"), createVec3Layout(c.origin, "Origin", PositionRange));
  form->addRow(tr("Angles"), createVec3Layout(c.angles, "Angles", AngleRange));
  c.angles[0]->setToolTip(tr("Pitch, positive points down"));
  c.angles[1]->setToolTip(tr("Yaw"));
  c.angles[2]->setToolTip(tr("Roll"));

  // colour and texture
  form->addSection(tr("Color and Texture"));
  c.color = new ColorButton{};
  c.color->setObjectName(controlName("Color"));
  form->addRow(tr("Color"), c.color);
  c.intensity = createSpin("Intensity", IntensityRange);
  c.intensity->setToolTip(tr("The brightest component of the color"));
  form->addRow(tr("Intensity"), c.intensity);
  c.texture = createComboBox("Texture", {EngineDefaultTexture});
  c.texture->setEditable(true);
  c.texture->setInsertPolicy(QComboBox::NoInsert);
  c.texture->setItemData(0, tr("lights/squarelight1"), Qt::ToolTipRole);
  form->addRow(tr("Texture"), c.texture);
  c.style = createSpin("Style", StyleRange);
  form->addRow(tr("Style"), c.style);

  // math falloff
  form->addSection(tr("Math Falloff"));
  c.mathEnabled = createCheckBox("MathEnabled", tr("Enable math-driven falloff"));
  form->addRow(c.mathEnabled);
  c.mathType = new QLabel{};
  c.mathType->setObjectName(controlName("MathType"));
  c.mathTypeRow = addRow(*form, tr("Type"), c.mathType);
  c.falloff = createComboBox(
    "Falloff",
    {
      tr("Inverse Square (unbounded)"),
      tr("Linear"),
      tr("Smooth"),
      tr("Exponential"),
      tr("Inverse Square (clamped) - PBR safe"),
    });
  c.falloffRow = addRow(*form, tr("Curve"), c.falloff);
  c.falloffExpRow = addRow(
    *form,
    tr("Falloff exponent"),
    createSliderLayout(c.falloffExpSlider, c.falloffExp, "FalloffExp", FalloffExpRange));
  c.innerRadiusRow = addRow(
    *form,
    tr("Inner radius"),
    createSliderLayout(
      c.innerRadiusSlider, c.innerRadius, "InnerRadius", InnerRadiusRange));
  c.shapeExpRow = addRow(
    *form,
    tr("Shape"),
    createSliderLayout(c.shapeExpSlider, c.shapeExp, "ShapeExp", ShapeExpRange));
  c.shapeExp->setToolTip(tr("2 is an ellipse, 16 a rectangle"));
  c.penumbraRow = addRow(
    *form,
    tr("Penumbra"),
    createSliderLayout(c.penumbraSlider, c.penumbra, "Penumbra", PenumbraRange));
  c.pattern = createComboBox(
    "Pattern",
    {
      tr("None"),
      tr("Subsurface (warm halo)"),
      tr("Ice (cool noise)"),
      tr("Caustic (sharp noise)"),
      tr("Banded (sine rings)"),
      tr("Granular (stippled)"),
    });
  c.patternRow = addRow(*form, tr("Pattern"), c.pattern);
  c.patternStrengthRow = addRow(
    *form,
    tr("Strength"),
    createSliderLayout(
      c.patternStrengthSlider,
      c.patternStrength,
      "PatternStrength",
      PatternStrengthRange));
  c.patternScaleRow = addRow(
    *form,
    tr("Scale"),
    createSliderLayout(
      c.patternScaleSlider, c.patternScale, "PatternScale", PatternScaleRange));
  c.patternPhaseRow = addRow(
    *form,
    tr("Phase"),
    createSliderLayout(
      c.patternPhaseSlider, c.patternPhase, "PatternPhase", PatternPhaseRange));
  c.energyClamp = createCheckBox("EnergyClamp", tr("Energy clamp (PBR safe)"));
  c.energyClampRow = addRow(*form, c.energyClamp);

  // misc
  form->addSection(tr("Misc"));
  c.castShadows = createCheckBox("CastShadows", tr("Cast shadows"));
  form->addRow(c.castShadows);
  c.skipSpecular = createCheckBox("SkipSpecular", tr("Skip specular"));
  form->addRow(c.skipSpecular);

  c.inputs = {
    c.equalRadius,
    c.radius,
    c.center,
    c.explicitStartEnd,
    c.color,
    c.intensity,
    c.texture,
    c.style,
    c.mathEnabled,
    c.falloff,
    c.falloffExpSlider,
    c.falloffExp,
    c.innerRadiusSlider,
    c.innerRadius,
    c.shapeExpSlider,
    c.shapeExp,
    c.penumbraSlider,
    c.penumbra,
    c.pattern,
    c.patternStrengthSlider,
    c.patternStrength,
    c.patternScaleSlider,
    c.patternScale,
    c.patternPhaseSlider,
    c.patternPhase,
    c.energyClamp,
    c.castShadows,
    c.skipSpecular,
  };
  c.inputs.insert(c.inputs.end(), c.typeRadios.begin(), c.typeRadios.end());
  for (const auto* spins :
       {&c.radiusXyz,
        &c.centerXyz,
        &c.target,
        &c.right,
        &c.up,
        &c.start,
        &c.end,
        &c.origin,
        &c.angles})
  {
    c.inputs.insert(c.inputs.end(), spins->begin(), spins->end());
  }

  c.editor = new QWidget{};
  c.editor->setLayout(form);

  auto* contentLayout = new QVBoxLayout{};
  contentLayout->setContentsMargins(
    LayoutConstants::WideHMargin,
    LayoutConstants::WideVMargin,
    0,
    LayoutConstants::WideVMargin);
  contentLayout->setSpacing(LayoutConstants::MediumVMargin);
  contentLayout->addWidget(c.header);
  contentLayout->addWidget(c.editor);
  contentLayout->addStretch(1);

  auto* content = new QWidget{};
  content->setLayout(contentLayout);

  auto* scrollArea = new QScrollArea{};
  scrollArea->setWidgetResizable(true);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setWidget(content);
  return scrollArea;
}

void LightEditorPanel::connectEditor()
{
  auto& c = *m_controls;

  const auto connectSpin =
    [&](SpinControl* spin, const std::string& commandName, auto set) {
      connect(spin, &QDoubleSpinBox::valueChanged, this, [=, this](const double value) {
        editLights(commandName, [&](mdl::LightProperties& light) { set(light, value); });
      });
    };

  const auto connectVec3 = [&](
                             const std::array<SpinControl*, 3>& spins,
                             const std::string& commandName,
                             vm::vec3d mdl::LightProperties::* member) {
    for (size_t i = 0; i < 3; ++i)
    {
      connectSpin(
        spins[i], commandName, [=](mdl::LightProperties& light, const double value) {
          (light.*member)[i] = value;
        });
    }
  };

  // a slider writes when it is released and whenever the keyboard or the wheel moves
  // it, but not while it is dragged
  const auto connectSlider =
    [&](QSlider* slider, SpinControl* spin, const std::string& commandName, auto set) {
      connectSpin(spin, commandName, set);

      const auto write = [=, this](const int position) {
        const auto value = sliderValue(*spin, position);
        editLights(commandName, [&](mdl::LightProperties& light) { set(light, value); });
      };
      connect(slider, &QSlider::valueChanged, this, [=](const int position) {
        if (slider->isSliderDown())
        {
          const auto blocker = QSignalBlocker{spin};
          spin->setValue(sliderValue(*spin, position));
        }
        else
        {
          write(position);
        }
      });
      connect(slider, &QSlider::sliderReleased, this, [=]() { write(slider->value()); });
    };

  const auto connectCheckBox =
    [&](QCheckBox* checkBox, const std::string& commandName, auto set) {
      connect(checkBox, &QAbstractButton::clicked, this, [=, this](const bool checked) {
        editLights(
          commandName, [&](mdl::LightProperties& light) { set(light, checked); });
      });
    };

  for (const auto type :
       {mdl::LightType::Point, mdl::LightType::Spot, mdl::LightType::Sun})
  {
    connect(c.typeRadios[size_t(type)], &QAbstractButton::clicked, this, [=, this]() {
      editLights("Set Light Type", [&](mdl::LightProperties& light) {
        // like the in-game editor: a change of type resets the light to the default
        // of the new type, keeping its colour and its math block
        if (light.type != type)
        {
          light = mdl::defaultLight(type, light);
        }
      });
    });
  }

  connect(c.equalRadius, &QAbstractButton::clicked, this, [this](const bool checked) {
    if (checked)
    {
      editLights("Set Light Radius", [](mdl::LightProperties& light) {
        light.radius = vm::vec3d{light.radius.x(), light.radius.x(), light.radius.x()};
        light.equalRadius = true;
      });
    }
    else
    {
      // writes nothing: the three components are equal until one of them is edited
      m_controls->form->setRowVisible(m_controls->radiusRow, false);
      m_controls->form->setRowVisible(m_controls->radiusXyzRow, true);
    }
  });
  connectSpin(
    c.radius, "Set Light Radius", [](mdl::LightProperties& light, const double value) {
      light.radius = vm::vec3d{value, value, value};
      light.equalRadius = true;
    });
  for (size_t i = 0; i < 3; ++i)
  {
    connectSpin(
      c.radiusXyz[i],
      "Set Light Radius",
      [=](mdl::LightProperties& light, const double value) {
        light.radius[i] = value;
        light.equalRadius = false;
      });
  }
  connectCheckBox(
    c.center, "Set Light Center", [](mdl::LightProperties& light, const bool checked) {
      light.hasCenter = checked;
    });
  connectVec3(c.centerXyz, "Set Light Center", &mdl::LightProperties::center);

  connectVec3(c.target, "Set Light Target", &mdl::LightProperties::target);
  connectVec3(c.right, "Set Light Right", &mdl::LightProperties::right);
  connectVec3(c.up, "Set Light Up", &mdl::LightProperties::up);
  connectCheckBox(
    c.explicitStartEnd,
    "Set Light Start and End",
    [](mdl::LightProperties& light, const bool checked) {
      light.explicitStartEnd = checked;
    });
  connectVec3(c.start, "Set Light Start", &mdl::LightProperties::start);
  connectVec3(c.end, "Set Light End", &mdl::LightProperties::end);

  connectVec3(c.origin, "Set Light Origin", &mdl::LightProperties::origin);
  for (size_t i = 0; i < 3; ++i)
  {
    connect(
      c.angles[i], &QDoubleSpinBox::valueChanged, this, [=, this](const double value) {
        editAngles(i, value);
      });
  }

  connect(c.color, &ColorButton::colorChangedByUser, this, [this](const QColor& color) {
    const auto hue = vm::vec3d{color.redF(), color.greenF(), color.blueF()};
    editLights("Set Light Color", [&](mdl::LightProperties& light) {
      light.color = hue * intensityOf(light.color);
    });
  });
  connectSpin(
    c.intensity,
    "Set Light Intensity",
    [](mdl::LightProperties& light, const double value) {
      light.color = hueOf(light.color) * value;
    });

  connect(c.texture, &QComboBox::activated, this, [this](const int index) {
    const auto texture =
      index == 0 ? std::string{} : m_controls->texture->itemText(index).toStdString();
    editLights(
      "Set Light Texture", [&](mdl::LightProperties& light) { light.texture = texture; });
  });
  connect(c.texture->lineEdit(), &QLineEdit::returnPressed, this, [this]() {
    // an empty text is a placeholder: the engine default is chosen from the list
    const auto text = m_controls->texture->currentText().trimmed();
    if (!text.isEmpty())
    {
      const auto texture =
        text == EngineDefaultTexture ? std::string{} : text.toStdString();
      editLights("Set Light Texture", [&](mdl::LightProperties& light) {
        light.texture = texture;
      });
    }
  });
  connectSpin(
    c.style, "Set Light Style", [](mdl::LightProperties& light, const double value) {
      light.style = int(std::lround(value));
    });

  connectCheckBox(
    c.mathEnabled, "Set Light Math", [](mdl::LightProperties& light, const bool checked) {
      if (!checked)
      {
        light.math.enabled = false;
      }
      else if (!light.math.enabled)
      {
        light.math = seededMathLightProperties();
      }
    });
  connect(c.falloff, &QComboBox::activated, this, [this](const int index) {
    editLights("Set Light Falloff", [&](mdl::LightProperties& light) {
      light.math.falloff = mdl::LightFalloff(index);
    });
  });
  connectSlider(
    c.falloffExpSlider,
    c.falloffExp,
    "Set Light Falloff Exponent",
    [](mdl::LightProperties& light, const double value) {
      light.math.falloffExp = value;
    });
  connectSlider(
    c.innerRadiusSlider,
    c.innerRadius,
    "Set Light Inner Radius",
    [](mdl::LightProperties& light, const double value) {
      light.math.innerRadius = value;
    });
  connectSlider(
    c.shapeExpSlider,
    c.shapeExp,
    "Set Light Shape",
    [](mdl::LightProperties& light, const double value) { light.math.shapeExp = value; });
  connectSlider(
    c.penumbraSlider,
    c.penumbra,
    "Set Light Penumbra",
    [](mdl::LightProperties& light, const double value) { light.math.penumbra = value; });
  connect(c.pattern, &QComboBox::activated, this, [this](const int index) {
    editLights("Set Light Pattern", [&](mdl::LightProperties& light) {
      light.math.pattern = mdl::LightPattern(index);
    });
  });
  connectSlider(
    c.patternStrengthSlider,
    c.patternStrength,
    "Set Light Pattern Strength",
    [](mdl::LightProperties& light, const double value) {
      light.math.patternStrength = value;
    });
  connectSlider(
    c.patternScaleSlider,
    c.patternScale,
    "Set Light Pattern Scale",
    [](mdl::LightProperties& light, const double value) {
      light.math.patternScale = value;
    });
  connectSlider(
    c.patternPhaseSlider,
    c.patternPhase,
    "Set Light Pattern Phase",
    [](mdl::LightProperties& light, const double value) {
      light.math.patternPhase = value;
    });
  connectCheckBox(
    c.energyClamp,
    "Set Light Energy Clamp",
    [](mdl::LightProperties& light, const bool checked) {
      light.math.energyClamp = checked;
    });

  connectCheckBox(
    c.castShadows,
    "Set Light Shadows",
    [](mdl::LightProperties& light, const bool checked) { light.castShadows = checked; });
  connectCheckBox(
    c.skipSpecular,
    "Set Light Specular",
    [](
      mdl::LightProperties& light, const bool checked) { light.skipSpecular = checked; });
}

void LightEditorPanel::connectObservers()
{
  connect(
    m_updateSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &LightEditorPanel::updateControls);

  // the document outlives the map it holds: on load the old map is freed before these
  // notifications run, so they hold nothing of it and every update reads the map anew;
  // nodesDidChange is not observed, because it is sent inside a command, and the
  // controls are set only from the delayed update
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect([&] { refresh(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&] { refresh(); });
  m_notifierConnection +=
    m_document.selectionDidChangeNotifier.connect([&](const auto&) { refresh(); });
  m_notifierConnection +=
    m_document.entityDefinitionsDidChangeNotifier.connect([&] { refresh(); });
  m_notifierConnection += m_document.modsDidChangeNotifier.connect([&] { refresh(); });
  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const auto&) { refresh(); });
}

void LightEditorPanel::refresh()
{
  m_updateSignalDelayer->queueSignal();
}

void LightEditorPanel::updateControls()
{
  switch (panelState(m_document.map()))
  {
  case PanelState::NoLightEditor:
    m_messageLabel->setText(tr("This game has no light editor"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::NoLightSelected:
    m_messageLabel->setText(tr("Select a light"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::NotOnlyLightsSelected:
    m_messageLabel->setText(tr("Select only lights"));
    m_stack->setCurrentIndex(0);
    break;
  case PanelState::OnlyLightsSelected:
    updateEditor();
    m_stack->setCurrentIndex(1);
    break;
  }
}

void LightEditorPanel::updateEditor()
{
  auto& c = *m_controls;
  const auto& map = m_document.map();
  const auto& entityNodes = map.selection().entities;
  contract_pre(!entityNodes.empty());

  const auto& entity = entityNodes.front()->entity();
  const auto light = mdl::parseLightProperties(entity);
  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

  // setting a control must not write it
  auto blockers = std::vector<QSignalBlocker>{};
  blockers.reserve(c.inputs.size());
  for (auto* input : c.inputs)
  {
    blockers.emplace_back(input);
  }

  c.header->setText(tr("Light: %1").arg(lightName(entity)));

  c.typeRadios[size_t(light.type)]->setChecked(true);

  const auto isSpot = light.type == mdl::LightType::Spot;
  c.form->setRowVisible(c.equalRadiusRow, !isSpot);
  c.form->setRowVisible(c.radiusRow, !isSpot && light.equalRadius);
  c.form->setRowVisible(c.radiusXyzRow, !isSpot && !light.equalRadius);
  c.form->setRowVisible(c.centerRow, !isSpot);
  c.form->setRowVisible(c.centerXyzRow, !isSpot && light.hasCenter);
  c.form->setRowVisible(c.targetRow, isSpot);
  c.form->setRowVisible(c.rightRow, isSpot);
  c.form->setRowVisible(c.upRow, isSpot);
  c.form->setRowVisible(c.explicitStartEndRow, isSpot);
  c.form->setRowVisible(c.startRow, isSpot && light.explicitStartEnd);
  c.form->setRowVisible(c.endRow, isSpot && light.explicitStartEnd);

  c.equalRadius->setChecked(light.equalRadius);
  setSpinValue(c.radius, light.radius.x());
  setVec3Value(c.radiusXyz, light.radius);
  c.center->setChecked(light.hasCenter);
  setVec3Value(c.centerXyz, light.center);
  setVec3Value(c.target, light.target);
  setVec3Value(c.right, light.right);
  setVec3Value(c.up, light.up);
  c.explicitStartEnd->setChecked(light.explicitStartEnd);
  setVec3Value(c.start, light.start);
  setVec3Value(c.end, light.end);

  setVec3Value(c.origin, light.origin);
  setVec3Value(c.angles, pitchYawRoll(entity, policy));

  c.color->setColor(toQColor(hueOf(light.color)));
  setSpinValue(c.intensity, intensityOf(light.color));
  if (light.texture.empty())
  {
    c.texture->setCurrentIndex(0);
  }
  else
  {
    const auto texture = QString::fromStdString(light.texture);
    c.texture->setCurrentIndex(c.texture->findText(texture));
    c.texture->setEditText(texture);
  }
  setSpinValue(c.style, double(light.style));

  const auto& math = light.math;
  c.mathEnabled->setChecked(math.enabled);
  c.form->setRowVisible(c.mathTypeRow, math.enabled);
  c.form->setRowVisible(c.falloffRow, math.enabled);
  c.form->setRowVisible(c.falloffExpRow, math.enabled);
  c.form->setRowVisible(c.innerRadiusRow, math.enabled);
  c.form->setRowVisible(c.shapeExpRow, math.enabled && isSpot);
  c.form->setRowVisible(c.penumbraRow, math.enabled && isSpot);
  c.form->setRowVisible(c.patternRow, math.enabled);
  c.form->setRowVisible(
    c.patternStrengthRow, math.enabled && math.pattern != mdl::LightPattern::None);
  c.form->setRowVisible(
    c.patternScaleRow, math.enabled && math.pattern != mdl::LightPattern::None);
  c.form->setRowVisible(
    c.patternPhaseRow, math.enabled && math.pattern != mdl::LightPattern::None);
  c.form->setRowVisible(c.energyClampRow, math.enabled);

  // the engine derives the math type from the light's geometry (D40)
  c.mathType->setText(
    tr("%1 (follows the light type)")
      .arg(QString::fromStdString(std::string{mdl::mathTypeName(light.type)})));
  c.falloff->setCurrentIndex(int(math.falloff));
  const auto falloffExpUsed = math.falloff != mdl::LightFalloff::Linear
                              && math.falloff != mdl::LightFalloff::Smooth;
  c.falloffExp->setEnabled(falloffExpUsed);
  c.falloffExpSlider->setEnabled(falloffExpUsed);
  c.falloffExp->setToolTip(falloffExpUsed ? QString{} : tr("Unused by this curve"));
  setSliderValue(c.falloffExpSlider, c.falloffExp, math.falloffExp);
  setSliderValue(c.innerRadiusSlider, c.innerRadius, math.innerRadius);
  setSliderValue(c.shapeExpSlider, c.shapeExp, math.shapeExp);
  setSliderValue(c.penumbraSlider, c.penumbra, math.penumbra);
  c.pattern->setCurrentIndex(int(math.pattern));
  setSliderValue(c.patternStrengthSlider, c.patternStrength, math.patternStrength);
  setSliderValue(c.patternScaleSlider, c.patternScale, math.patternScale);
  setSliderValue(c.patternPhaseSlider, c.patternPhase, math.patternPhase);
  c.energyClamp->setChecked(math.energyClamp);

  c.castShadows->setChecked(light.castShadows);
  c.skipSpecular->setChecked(light.skipSpecular);
}

void LightEditorPanel::editLights(
  const std::string& commandName, const std::function<void(mdl::LightProperties&)>& edit)
{
  auto& map = m_document.map();

  // per light, only the properties that the edit changes, so that the lights keep their
  // other values and a value that is written unchanged is not rounded (D42)
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : selectedLightNodes(map))
  {
    const auto& entity = entityNode->entity();
    auto light = mdl::parseLightProperties(entity);
    edit(light);

    const auto update = mdl::lightKeyDiff(entity, light);
    if (!update.set.empty() || !update.remove.empty())
    {
      nodesToSwap.emplace_back(
        entityNode, mdl::NodeContents{mdl::applyLightPropertyUpdate(entity, update)});
    }
  }

  if (!swapNodeContents(map, commandName, std::move(nodesToSwap)))
  {
    // the control that the user changed may show a value that no light has
    refresh();
  }
}

void LightEditorPanel::editAngles(const std::size_t component, const double value)
{
  auto& map = m_document.map();
  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

  // the rotation belongs to the id Tech 4 rotation policy, which writes "angles" and
  // removes the properties that the engine would read instead (D7)
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : selectedLightNodes(map))
  {
    auto entity = entityNode->entity();
    const auto oldAngles = pitchYawRoll(entity, policy);
    auto newAngles = oldAngles;
    newAngles[component] = value;

    const auto update = mdl::idTech4AnglesUpdate(entity, newAngles);
    if (!update.remove.empty() || update.set->value() != formatAngles(oldAngles))
    {
      mdl::applyEntityRotationUpdate(entity, update);
      nodesToSwap.emplace_back(entityNode, mdl::NodeContents{std::move(entity)});
    }
  }

  if (!swapNodeContents(map, "Set Light Angles", std::move(nodesToSwap)))
  {
    refresh();
  }
}

} // namespace tb::ui
