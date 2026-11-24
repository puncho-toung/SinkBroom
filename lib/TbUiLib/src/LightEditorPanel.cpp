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

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QRadioButton>
#include <QRect>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "base/PreferenceManager.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityRotation.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/Selection.h"
#include "prefs/Preferences.h"
#include "ui/BitmapButton.h"
#include "ui/ColorButton.h"
#include "ui/FormWithSectionsLayout.h"
#include "ui/LightVolumeTool.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/QStyleUtils.h"
#include "ui/SignalDelayer.h"
#include "ui/SpinControl.h"
#include "ui/TabBar.h"
#include "ui/TabBook.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
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

/**
 * The lights that an edit changes: the selected lights, or none if one of them has the
 * legacy light_origin, which makes the panel read-only until it is converted (D8).
 */
std::vector<mdl::EntityNode*> editableLightNodes(const mdl::Map& map)
{
  auto lightNodes = selectedLightNodes(map);
  return std::ranges::none_of(
           lightNodes,
           [](const auto* entityNode) {
             return entityNode->entity().hasProperty(mdl::LightPropertyKeys::LightOrigin);
           })
           ? lightNodes
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
// the emitter's floats, written with four decimals
constexpr auto PenumbraRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto SquarenessRange =
  SpinRange{mdl::MinEmitterSquareness, mdl::MaxEmitterSquareness, 4, 0.1, 1.0, 4.0};
constexpr auto PatternStrengthRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
// in world units since choice 5 (G0 section 5, pattern position): 1 is the reference size
constexpr auto PatternScaleRange = SpinRange{0.01, 64.0, 4, 0.1, 1.0, 4.0};
constexpr auto PatternPhaseRange = SpinRange{0.0, 100.0, 4, 0.1, 1.0, 10.0};
constexpr auto EffectSpeedRange = SpinRange{0.0, 100.0, 4, 0.1, 1.0, 10.0};
constexpr auto EffectAmountRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
// the effect's phase is used modulo 1
constexpr auto EffectPhaseRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
// the emitter's lengths in world units: a size of at least 1, a range of at least 24
constexpr auto EmitterSizeRange = SpinRange{1.0, 131072.0, 2, 1.0, 8.0, 64.0};
constexpr auto EmitterStartRange = SpinRange{0.0, 131072.0, 2, 1.0, 8.0, 64.0};
constexpr auto EmitterRangeRange =
  SpinRange{mdl::MinEmitterRange, 131072.0, 2, 1.0, 8.0, 64.0};
// a spot emitter's full cone angles in degrees
constexpr auto ConeRange =
  SpinRange{mdl::MinEmitterCone, mdl::MaxEmitterCone, 2, 1.0, 5.0, 15.0};
// the light volume handle threshold in percent, 0.01 to 0.5 (D53)
constexpr auto HandleThresholdRange = SpinRange{1.0, 50.0, 0, 1.0, 5.0, 10.0};
// how long the threshold's steps must pause before it is written
constexpr auto HandleThresholdWriteDelay = std::chrono::milliseconds{200};
// the Fog page's controls (V7), with the ranges of the engine's Fog tab: Density 0 to 100
// over a distance of 8,192 to 32 units, Distance 2 to 30,000, a faint fog's Strength,
// Albedo, Glow 0 to 8 (its slider to 2) and Soft sides 0 to 4,096 units (its slider to
// 256)
constexpr auto FogDensityRange = SpinRange{0.0, 100.0, 1, 1.0, 5.0, 12.5};
constexpr auto FogDistanceRange =
  SpinRange{mdl::MinFogDistance, mdl::MaxFogDistance, 0, 1.0, 16.0, 128.0};
constexpr auto FogStrengthRange = SpinRange{0.0, 1.0, 2, 0.01, 0.1, 0.25};
constexpr auto FogAlbedoRange = SpinRange{0.0, 1.0, 2, 0.01, 0.1, 0.25};
constexpr auto FogGlowRange = SpinRange{0.0, mdl::MaxFogEmission, 2, 0.05, 0.25, 1.0};
constexpr auto FogEdgeRange = SpinRange{0.0, mdl::MaxFogEdge, 0, 1.0, 8.0, 64.0};
constexpr auto FogGlowSliderMaximum = 2.0;
constexpr auto FogEdgeSliderMaximum = 256.0;
// the material that "Make it a fog light" gives a light, as the engine's
// LightMakeFogLight
const auto FogLightTexture = std::string{"fogs/basicFog"};

constexpr auto SliderSteps = 1000;
// a slider spans its spin box's range unless it has a maximum of its own in this property
// (the Fog page's Glow and Soft sides, whose spin boxes take typed values past the
// slider's end, as the engine's ctrl+click does)
constexpr auto SliderMaximumProperty = "LightEditorPanel_SliderMaximum";

// the pages of the two kinds of light, and the Fog page, which is no kind
constexpr auto EmitterPageIndex = 0;
constexpr auto TexturePageIndex = 1;
constexpr auto FogPageIndex = 2;

const auto Multi = QString{"multi"};

// a rotation whose entries are this close to the identity's is no rotation
constexpr auto NoRotationEpsilon = 1e-6;

/**
 * A spin box that shows "multi" where the selected lights differ. Like in the face
 * attribute editor, "multi" is the special value text of the minimum, but here the
 * minimum lies one unit of the last decimal below the range while "multi" is shown: were
 * it the range minimum, typing the range minimum into a "multi" field would not change
 * the value, and nothing would be written.
 */
class MultiSpinControl : public SpinControl
{
private:
  double m_rangeMinimum = 0.0;

public:
  /**
   * Sets the range of the values that the spin box shows and writes.
   */
  void setValueRange(const double minimum, const double maximum)
  {
    m_rangeMinimum = minimum;
    setRange(minimum, maximum);
  }

  /**
   * The lowest value that the spin box shows and writes; its minimum lies below it while
   * it shows "multi".
   */
  double rangeMinimum() const { return m_rangeMinimum; }

  /**
   * Whether the spin box shows "multi": its value is the minimum below the range.
   */
  bool isMulti() const { return minimum() < m_rangeMinimum && value() == minimum(); }

  /**
   * Shows "multi". It emits nothing, because it shows what the map holds.
   */
  void setMulti()
  {
    const auto blocker = QSignalBlocker{this};
    setSpecialValueText(Multi);
    setMinimum(m_rangeMinimum - std::pow(10.0, -decimals()));
    setValue(minimum());
  }

  /**
   * Shows the value of the selected lights. It emits nothing, because it shows what the
   * map holds; raising the minimum can change the value on its own.
   */
  void setLightValue(const double value)
  {
    const auto blocker = QSignalBlocker{this};
    setSpecialValueText({});
    setMinimum(m_rangeMinimum);
    setValue(value);
  }

  /**
   * From "multi", a step up starts at the range minimum, and a step down changes nothing
   * because the value is the lowest already. A value typed over "multi" is stepped from
   * as it is: raising the minimum first would replace it.
   */
  void stepBy(const int steps) override
  {
    if (isMulti() && text() == specialValueText())
    {
      if (steps <= 0)
      {
        return;
      }

      const auto blocker = QSignalBlocker{this};
      setMinimum(m_rangeMinimum);
    }

    SpinControl::stepBy(steps);
  }
};

MultiSpinControl* createSpin(const QString& name, const SpinRange& range)
{
  auto* spin = new MultiSpinControl{};
  spin->setObjectName(controlName(name));
  spin->setDigits(0, range.decimals);
  spin->setValueRange(range.min, range.max);
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
  std::array<MultiSpinControl*, 3>& spins, const QString& name, const SpinRange& range)
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
  QSlider*& slider,
  MultiSpinControl*& spin,
  const QString& name,
  const SpinRange& range,
  const std::optional<double> sliderMaximum = std::nullopt)
{
  slider = new QSlider{Qt::Horizontal};
  slider->setObjectName(controlName(name + "Slider"));
  slider->setRange(0, SliderSteps);
  if (sliderMaximum)
  {
    slider->setProperty(SliderMaximumProperty, *sliderMaximum);
  }
  // an arrow key moves the value by at least the spin box's last decimal, which a write
  // rounds to: a smaller step would write the same value back
  const auto span = sliderMaximum.value_or(range.max) - range.min;
  slider->setSingleStep(
    std::max(
      1, int(std::ceil(SliderSteps * std::pow(10.0, -range.decimals) / span - 1e-9))));

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

/**
 * Leaves a wheel event over an input that does not have the focus to the input's parents,
 * and so to the panel's scroll area: scrolling the panel must not edit the lights, one
 * command per notch. An input that has the focus still takes the wheel.
 */
class WheelGuard : public QObject
{
public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (event->type() == QEvent::Wheel)
    {
      if (const auto* widget = qobject_cast<QWidget*>(watched);
          widget && !widget->hasFocus())
      {
        // Qt passes an ignored wheel event on to the parents
        event->ignore();
        return true;
      }
    }
    return QObject::eventFilter(watched, event);
  }
};

/**
 * Gives the spin boxes, sliders and combo boxes among the given inputs the guard, and a
 * focus policy without the wheel: a wheel that gave them the focus would get past it.
 */
void guardWheel(const std::vector<QObject*>& inputs, WheelGuard* guard)
{
  for (auto* input : inputs)
  {
    if (
      qobject_cast<QAbstractSpinBox*>(input) || qobject_cast<QAbstractSlider*>(input)
      || qobject_cast<QComboBox*>(input))
    {
      auto* widget = static_cast<QWidget*>(input);
      widget->setFocusPolicy(Qt::StrongFocus);
      widget->installEventFilter(guard);
    }
  }
}

FormWithSectionsLayout* createForm()
{
  auto* form = new FormWithSectionsLayout{};
  form->setContentsMargins(0, LayoutConstants::WideVMargin, 0, 0);
  form->setHorizontalSpacing(LayoutConstants::NarrowHMargin);
  form->setVerticalSpacing(LayoutConstants::NarrowVMargin);
  return form;
}

QWidget* createFormWidget(QLayout* form)
{
  auto* widget = new QWidget{};
  widget->setLayout(form);
  return widget;
}

TabBookPage* createTabPage(const QString& name, QLayout* form)
{
  auto* page = new TabBookPage{};
  page->setObjectName(controlName(name));
  page->setLayout(form);
  return page;
}

/**
 * Sizes the tab book to the page that it shows: a stacked layout is as large as its
 * largest page, unless the other pages ignore their sizes.
 */
void fitTabPages(const std::array<TabBookPage*, 3>& pages, const int currentIndex)
{
  for (size_t i = 0; i < pages.size(); ++i)
  {
    const auto policy =
      int(i) == currentIndex ? QSizePolicy::Preferred : QSizePolicy::Ignored;
    pages[i]->setSizePolicy(policy, policy);
  }
}

QToolButton* createTextButton(const QString& name, const QString& text)
{
  // a tool button, unlike a push button, takes no focus when it is clicked, so the map
  // view keeps it and the next click in the view is not swallowed
  auto* button = new QToolButton{};
  button->setObjectName(controlName(name));
  button->setText(text);
  button->setToolButtonStyle(Qt::ToolButtonTextOnly);
  return button;
}

QToolButton* createVolumeToolButton(
  const QString& name, const std::filesystem::path& imagePath, const QString& text)
{
  auto* button = createBitmapButton(imagePath, text);
  button->setObjectName(controlName(name));
  button->setText(text);
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  return button;
}

/**
 * A notice in the editor's header: a text and, if given, the button that fixes what it
 * says.
 */
QWidget* createNotice(QLabel* label, QToolButton* button = nullptr)
{
  label->setWordWrap(true);

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(LayoutConstants::NarrowVMargin);
  layout->addWidget(label);
  if (button)
  {
    layout->addWidget(button, 0, Qt::AlignLeft);
  }

  auto* notice = new QWidget{};
  notice->setLayout(layout);
  return notice;
}

QLabel* createNoticeLabel(const QString& name, const QString& text)
{
  auto* label = new QLabel{text};
  label->setObjectName(controlName(name));
  return label;
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

int addRow(FormWithSectionsLayout& form, QWidget* label, QWidget* field)
{
  const auto row = form.rowCount();
  form.addRow(label, field);
  return row;
}

int addRow(FormWithSectionsLayout& form, QWidget* label, QLayout* field)
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

// a slider spans the range of its spin box's values, never the "multi" below it, up to
// its own maximum where it has one
double sliderMaximum(const QSlider& slider, const MultiSpinControl& spin)
{
  const auto maximum = slider.property(SliderMaximumProperty);
  return maximum.isValid() ? maximum.toDouble() : spin.maximum();
}

double sliderValue(
  const QSlider& slider, const MultiSpinControl& spin, const int position)
{
  const auto minimum = spin.rangeMinimum();
  const auto value =
    minimum + (sliderMaximum(slider, spin) - minimum) * position / SliderSteps;
  const auto scale = std::pow(10.0, spin.decimals());
  return std::round(value * scale) / scale;
}

int sliderPosition(
  const QSlider& slider, const MultiSpinControl& spin, const double value)
{
  const auto minimum = spin.rangeMinimum();
  const auto maximum = sliderMaximum(slider, spin);
  const auto t = (std::clamp(value, minimum, maximum) - minimum) / (maximum - minimum);
  return int(std::lround(t * SliderSteps));
}

// the colour of a light is its hue times its intensity (D41); the panel shows no
// intensity below 0, which every reach factor reads as 0 too
double intensityOf(const vm::vec3d& color)
{
  return std::max(mdl::lightIntensity(color), 0.0);
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
 * Resets the light's volume to the engine's default for its type, an emitter's to its
 * shape's defaults (a spot's cone too, its range from auto range), and keeps everything
 * else.
 */
void resetLightVolume(mdl::LightProperties& light)
{
  const auto defaults = mdl::defaultLight(light.type, light);
  light.radius = defaults.radius;
  light.equalRadius = defaults.equalRadius;
  light.hasCenter = defaults.hasCenter;
  light.center = defaults.center;
  light.target = defaults.target;
  light.right = defaults.right;
  light.up = defaults.up;
  light.explicitStartEnd = defaults.explicitStartEnd;
  light.start = defaults.start;
  light.end = defaults.end;
  // an emitter spot's cone is part of its volume; for any other light it is unchanged
  light.emitter.coneH = defaults.emitter.coneH;
  light.emitter.coneV = defaults.emitter.coneV;
}

/**
 * Whether the given sun still has all three spot vectors. With them, the engine reads
 * neither light_radius nor light_center (d3xp/Light.cpp), so the sun has a radius of 0
 * in the game until they are removed.
 * Never for an emitter, which ignores the target keys.
 */
bool hasStaleSpotKeys(const mdl::Entity& entity, const mdl::LightProperties& light)
{
  return !mdl::lightUsesEmitter(light) && light.type == mdl::LightType::Sun
         && entity.hasProperty(mdl::LightPropertyKeys::LightTarget)
         && entity.hasProperty(mdl::LightPropertyKeys::LightRight)
         && entity.hasProperty(mdl::LightPropertyKeys::LightUp);
}

/**
 * How far the far rectangle of an off-centre spot light moves when the first box edit
 * recentres it: the largest distance between one of its far corners and the same corner
 * of the light that its own edit box gives. Nothing for a centred or degenerate spot,
 * or for one whose own edit box is refused.
 * Never for an emitter, which ignores the target keys.
 */
std::optional<double> recentreDistance(const mdl::LightProperties& light)
{
  if (
    light.type != mdl::LightType::Spot || mdl::lightUsesEmitter(light)
    || mdl::isCentredSpot(light))
  {
    return std::nullopt;
  }

  const auto frustum = mdl::spotLightFrustum(light);
  const auto box = mdl::lightEditBox(light);
  if (!frustum || !box)
  {
    return std::nullopt;
  }

  const auto edited = mdl::lightFromEditBox(light, vm::mat3x3d::identity(), *box);
  const auto recentred = edited ? mdl::spotLightFrustum(*edited) : std::nullopt;
  if (!recentred)
  {
    return std::nullopt;
  }

  auto distance = 0.0;
  for (size_t i = 0; i < frustum->farCorners.size(); ++i)
  {
    distance =
      std::max(distance, vm::length(frustum->farCorners[i] - recentred->farCorners[i]));
  }
  return distance;
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

/**
 * The value of the first of the given values, and whether another one differs from it.
 */
template <typename T, typename Get>
auto firstValue(const std::vector<T>& values, const Get& get)
{
  const auto value = get(values.front());
  const auto multi =
    std::ranges::any_of(values, [&](const auto& other) { return get(other) != value; });
  return std::pair{value, multi};
}

void setSpinValue(MultiSpinControl* spin, const double value, const bool multi)
{
  if (multi)
  {
    spin->setMulti();
  }
  else
  {
    spin->setLightValue(value);
  }
}

template <typename T, typename Get>
void setSpin(MultiSpinControl* spin, const std::vector<T>& values, const Get& get)
{
  const auto [value, multi] = firstValue(values, get);
  setSpinValue(spin, double(value), multi);
}

template <typename T, typename Get>
void setVec3(
  const std::array<MultiSpinControl*, 3>& spins,
  const std::vector<T>& values,
  const Get& get)
{
  for (size_t i = 0; i < 3; ++i)
  {
    setSpin(spins[i], values, [&](const auto& value) { return get(value)[i]; });
  }
}

template <typename Get>
void setSlider(
  QSlider* slider,
  MultiSpinControl* spin,
  const std::vector<mdl::LightProperties>& lights,
  const Get& get)
{
  const auto [value, multi] = firstValue(lights, get);
  setSpinValue(spin, value, multi);
  // a handle the user holds stays where the press found it, so that its release compares
  // with the position it was pressed at; the release's refresh moves it
  if (!slider->isSliderDown())
  {
    slider->setValue(sliderPosition(*slider, *spin, value));
  }
}

template <typename Get>
void setCheckBox(
  QCheckBox* checkBox, const std::vector<mdl::LightProperties>& lights, const Get& get)
{
  const auto [checked, multi] = firstValue(lights, get);
  if (multi)
  {
    // a click checks it
    checkBox->setCheckState(Qt::PartiallyChecked);
  }
  else
  {
    checkBox->setTristate(false);
    checkBox->setChecked(checked);
  }
}

template <typename Get>
void setComboBox(
  QComboBox* comboBox, const std::vector<mdl::LightProperties>& lights, const Get& get)
{
  const auto [index, multi] = firstValue(lights, get);
  comboBox->setPlaceholderText(Multi);
  comboBox->setCurrentIndex(multi ? -1 : int(index));
}

/**
 * Like setComboBox, for a combo box whose items hold their values as their data, in an
 * order of their own.
 */
template <typename Get>
void setComboBoxData(
  QComboBox* comboBox, const std::vector<mdl::LightProperties>& lights, const Get& get)
{
  const auto [value, multi] = firstValue(lights, get);
  comboBox->setPlaceholderText(Multi);
  comboBox->setCurrentIndex(multi ? -1 : comboBox->findData(int(value)));
}

void updateTextureItems(QComboBox& comboBox, const mdl::Map& map)
{
  // the pointer is replaced by every reload of the shaders, so the names are copied
  const auto shaders = map.entityModelManager().shaders();
  const auto names = shaders ? mdl::shaderNamesWithPrefix(*shaders, {"lights/", "fogs/"})
                             : std::vector<std::string>{};

  comboBox.clear();
  comboBox.addItem(EngineDefaultTexture);
  comboBox.setItemData(0, "lights/squarelight1", Qt::ToolTipRole);
  for (const auto& name : names)
  {
    comboBox.addItem(QString::fromStdString(name));
  }
}

// the fog lights among the light materials, which the Fog page offers (V7)
void updateFogMaterialItems(QComboBox& comboBox, const mdl::Map& map)
{
  const auto shaders = map.entityModelManager().shaders();
  const auto names =
    shaders ? mdl::fogLightShaderNames(*shaders) : std::vector<std::string>{};

  comboBox.clear();
  for (const auto& name : names)
  {
    comboBox.addItem(QString::fromStdString(name));
  }
}

/**
 * What the engine's Fog tab reads of the light's material (V7): nothing for an emitter, a
 * light with the engine's default material, a material that no shader has, or while no
 * shaders are loaded.
 */
std::optional<mdl::LightShaderFacts> lightShaderFactsOf(
  const mdl::Map& map, const mdl::LightProperties& light)
{
  if (mdl::lightUsesEmitter(light) || light.texture.empty())
  {
    return std::nullopt;
  }
  const auto shaders = map.entityModelManager().shaders();
  return shaders ? mdl::lightShaderFacts(*shaders, light.texture) : std::nullopt;
}

/**
 * Whether the light is a fog light, as the engine's LightIsFogLight says: not an emitter,
 * and its material has the keyword fogLight.
 */
bool isFogLight(const mdl::Map& map, const mdl::LightProperties& light)
{
  const auto facts = lightShaderFactsOf(map, light);
  return facts && facts->fogLight;
}

/**
 * Whether the map has its materials, without which no light can be told to be a fog light
 * (no game path, or a load that failed): an editor's state the engine never is in.
 */
bool materialsLoaded(const mdl::Map& map)
{
  const auto shaders = map.entityModelManager().shaders();
  return shaders && !shaders->shaders().empty();
}

using StyleOptions = std::vector<std::pair<std::string, std::string>>;

/**
 * The value and the name of no style and of each of the engine's predefined light styles,
 * like the style choices of an entity definition.
 */
StyleOptions predefinedStyleOptions()
{
  auto options = StyleOptions{};
  for (int style = -1; style < mdl::LightStyleCount; ++style)
  {
    options.emplace_back(std::to_string(style), std::string{mdl::lightStyleName(style)});
  }
  return options;
}

/**
 * Fills a style combo box with items "value: name" whose data is the value.
 */
void setStyleItems(QComboBox& comboBox, const StyleOptions& options)
{
  comboBox.clear();
  for (const auto& [value, name] : options)
  {
    comboBox.addItem(
      QString::fromStdString(value + ": " + name), QString::fromStdString(value));
  }
}

int findStyleIndex(const QComboBox& comboBox, const int style)
{
  for (int i = 0; i < comboBox.count(); ++i)
  {
    auto ok = false;
    if (comboBox.itemData(i).toString().toInt(&ok) == style && ok)
    {
      return i;
    }
  }
  return -1;
}

/**
 * The draw button's click: turns the light volume tool off if it draws, else switches it
 * to draw mode and turns it on (D20). Its handles work without it (D56).
 */
void toggleLightVolumeDrawing(MapViewToolBox& toolBox)
{
  auto& tool = toolBox.lightVolumeTool();
  if (tool.active() && tool.mode() == LightVolumeTool::Mode::Draw)
  {
    toolBox.toggleTool(tool);
  }
  else
  {
    tool.setMode(LightVolumeTool::Mode::Draw);
    if (!tool.active())
    {
      toolBox.toggleTool(tool);
    }
  }
}

/**
 * Fills the effect combo box with the display names of the emitter effects, each with
 * its value as its data: "None" first, then the three waveforms (Strobe, Pulse,
 * Flicker), a separator, and the eleven named styles of the engine.
 */
void setEffectItems(QComboBox& comboBox)
{
  const auto addEffect = [&](const int effect) {
    comboBox.addItem(
      QString::fromStdString(std::string{mdl::emitterEffectDisplayName(effect)}), effect);
  };

  addEffect(0);
  for (int effect = mdl::LightStyleCount; effect < mdl::EmitterEffectCount; ++effect)
  {
    addEffect(effect);
  }
  comboBox.insertSeparator(comboBox.count());
  for (int effect = 1; effect < mdl::LightStyleCount; ++effect)
  {
    addEffect(effect);
  }
}

/**
 * Fills the falloff combo box with the curves, each with its value as its data: the
 * default, the clamped inverse square, first, the others in the order of their values.
 */
void setFalloffItems(QComboBox& comboBox)
{
  comboBox.addItem(
    QObject::tr("Inverse Square (clamped) - PBR safe"),
    int(mdl::LightFalloff::InvSquareClamped));
  comboBox.addItem(
    QObject::tr("Inverse Square (unbounded)"), int(mdl::LightFalloff::InvSquare));
  comboBox.addItem(QObject::tr("Linear"), int(mdl::LightFalloff::Linear));
  comboBox.addItem(QObject::tr("Smooth"), int(mdl::LightFalloff::Smooth));
  comboBox.addItem(QObject::tr("Exponential"), int(mdl::LightFalloff::Exponential));
}

/**
 * The given entity with only the properties that the given edit of its light changes
 * (lightKeyDiff), so that the light keeps its other values and a value that is written
 * unchanged is not rounded (D42), and with the given rotation written as the panel writes
 * angles. Nothing when the edit changes nothing.
 */
std::optional<mdl::Entity> editedLightEntity(
  const mdl::Entity& entity,
  const mdl::LightProperties& light,
  const std::optional<vm::mat3x3d>& rotation)
{
  // nothing to write is no copy of the entity; the light diff never touches the rotation
  // keys, so the rotation is compared with the entity's own
  const auto diff = mdl::lightKeyDiff(entity, light);
  const auto rotationUpdate =
    rotation ? mdl::lightRotationUpdate(entity, *rotation) : mdl::EntityRotationUpdate{};
  if (
    diff.set.empty() && diff.remove.empty() && !rotationUpdate.set
    && rotationUpdate.remove.empty())
  {
    return std::nullopt;
  }

  auto changed = mdl::applyLightPropertyUpdate(entity, diff);
  mdl::applyEntityRotationUpdate(changed, rotationUpdate);
  return changed == entity ? std::nullopt : std::optional{std::move(changed)};
}

} // namespace

struct LightEditorPanel::Controls
{
  QLabel* header = nullptr;
  QWidget* badTargetNotice = nullptr;
  QToolButton* fixBadTarget = nullptr;
  QWidget* staleSpotKeysNotice = nullptr;
  QToolButton* fixStaleSpotKeys = nullptr;
  QWidget* offCentreNotice = nullptr;
  QLabel* offCentreText = nullptr;
  QWidget* lightOriginNotice = nullptr;
  QToolButton* convertLightOrigin = nullptr;
  QWidget* styledNotice = nullptr;
  // the label of the centre row, "Bulb offset" for emitter omnis
  QLabel* centerLabel = nullptr;

  QWidget* editor = nullptr;
  // the light's geometry and colour
  FormWithSectionsLayout* form = nullptr;
  // the pages of emitters and texture lights, each with its own form, and the Fog page
  TabBook* tabs = nullptr;
  TabBookPage* emitterPage = nullptr;
  TabBookPage* texturePage = nullptr;
  TabBookPage* fogPage = nullptr;
  FormWithSectionsLayout* emitterForm = nullptr;
  FormWithSectionsLayout* textureForm = nullptr;
  FormWithSectionsLayout* fogForm = nullptr;
  FormWithSectionsLayout* miscForm = nullptr;
  // the page that the tab book shows; the Fog page stays shown while the selection is the
  // same (V7)
  int page = EmitterPageIndex;
  bool selectionChanged = true;

  // every control that the user changes, blocked while the controls are set
  std::vector<QObject*> inputs;

  QButtonGroup* type = nullptr;
  std::array<QRadioButton*, 3> typeRadios = {};

  QCheckBox* equalRadius = nullptr;
  // the session's choice of one radius for lights whose three radii are equal, off at
  // first, so that a light shows its three radii (D52)
  bool equilateral = false;
  // an emitter omni shows one Range, its three radii behind Equilateral, which shows
  // ticked while they are equal; the session's choice to show them while they are equal
  bool emitterRadiiShown = false;
  // the selection is emitter omnis only, whose Equilateral box follows the rule above
  bool emitterOmnis = false;
  MultiSpinControl* radius = nullptr;
  std::array<MultiSpinControl*, 3> radiusXyz = {};
  QCheckBox* center = nullptr;
  std::array<MultiSpinControl*, 3> centerXyz = {};

  std::array<MultiSpinControl*, 3> target = {};
  std::array<MultiSpinControl*, 3> right = {};
  std::array<MultiSpinControl*, 3> up = {};
  QCheckBox* explicitStartEnd = nullptr;
  std::array<MultiSpinControl*, 3> start = {};
  std::array<MultiSpinControl*, 3> end = {};

  std::array<MultiSpinControl*, 3> origin = {};
  std::array<MultiSpinControl*, 3> angles = {};

  ColorButton* color = nullptr;
  MultiSpinControl* intensity = nullptr;
  QComboBox* texture = nullptr;
  // the names of the light materials are copied again on the next update
  bool texturesStale = true;
  QComboBox* styleChoice = nullptr;
  // the values and descriptions of the definition's style choices, else of the engine's
  // predefined styles, copied
  StyleOptions styleOptions;

  // the emitter block
  QComboBox* falloff = nullptr;
  MultiSpinControl* emitSize = nullptr;
  MultiSpinControl* emitStart = nullptr;
  QCheckBox* autoRange = nullptr;
  MultiSpinControl* range = nullptr;
  // "Range", or "Range (auto)" while auto range is in force on every selected light
  QLabel* rangeLabel = nullptr;
  MultiSpinControl* coneH = nullptr;
  MultiSpinControl* coneV = nullptr;
  QSlider* penumbraSlider = nullptr;
  MultiSpinControl* penumbra = nullptr;
  QSlider* squarenessSlider = nullptr;
  MultiSpinControl* squareness = nullptr;
  QComboBox* pattern = nullptr;
  QSlider* patternStrengthSlider = nullptr;
  MultiSpinControl* patternStrength = nullptr;
  QSlider* patternScaleSlider = nullptr;
  MultiSpinControl* patternScale = nullptr;
  QSlider* patternPhaseSlider = nullptr;
  MultiSpinControl* patternPhase = nullptr;
  QCheckBox* energyClamp = nullptr;
  QComboBox* effect = nullptr;
  MultiSpinControl* effectSpeed = nullptr;
  QSlider* effectAmountSlider = nullptr;
  MultiSpinControl* effectAmount = nullptr;
  MultiSpinControl* effectPhase = nullptr;

  // the Fog page (V7)
  QLabel* fogNoticeText = nullptr;
  QToolButton* makeFogLight = nullptr;
  QComboBox* fogMaterial = nullptr;
  QComboBox* fogDensityForm = nullptr;
  QSlider* fogDensitySlider = nullptr;
  MultiSpinControl* fogDensity = nullptr;
  MultiSpinControl* fogDistance = nullptr;
  QLabel* fogDistanceText = nullptr;
  QSlider* fogStrengthSlider = nullptr;
  MultiSpinControl* fogStrength = nullptr;
  QSlider* fogAlbedoSlider = nullptr;
  MultiSpinControl* fogAlbedo = nullptr;
  QSlider* fogGlowSlider = nullptr;
  MultiSpinControl* fogGlow = nullptr;
  QSlider* fogEdgeSlider = nullptr;
  MultiSpinControl* fogEdge = nullptr;
  QLabel* fogColourText = nullptr;
  QComboBox* fogDrawnBy = nullptr;
  QComboBox* lightInTheFog = nullptr;

  QCheckBox* castShadows = nullptr;
  QCheckBox* skipSpecular = nullptr;

  QToolButton* drawVolumeButton = nullptr;
  QToolButton* resetVolumeButton = nullptr;
  MultiSpinControl* handleThreshold = nullptr;
  // the threshold that the spin box writes once its steps pause
  SignalDelayer* handleThresholdDelayer = nullptr;
  std::optional<double> pendingHandleThreshold;

  // the rows of form that depend on the light's type and options
  int mixedTypesRow = -1;
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
  // an emitter omni's or spot's range and what follows it, together; an emitter spot's
  // cone, in the place of the spot vectors
  int rangeRow = -1;
  int autoRangeRow = -1;
  int emitSizeRow = -1;
  int emitStartRow = -1;
  int autoRangeNoteRow = -1;
  int coneHRow = -1;
  int coneVRow = -1;
  // the rows of emitterForm
  int mixedEmitterRow = -1;
  int falloffRow = -1;
  int penumbraRow = -1;
  int squarenessRow = -1;
  int patternRow = -1;
  int patternStrengthRow = -1;
  int patternScaleRow = -1;
  int patternPhaseRow = -1;
  int energyClampRow = -1;
  int effectRow = -1;
  int effectSpeedRow = -1;
  int effectAmountRow = -1;
  int effectPhaseRow = -1;
  // the rows of fogForm and of the Misc form's Volumetric fog section
  int fogNoticeRow = -1;
  int fogStatusRow = -1;
  int fogMaterialRow = -1;
  int fogDensitySectionRow = -1;
  int ownFogDistanceRow = -1;
  int fogDensityFormRow = -1;
  int fogDensityRow = -1;
  int fogDistanceRow = -1;
  int fogDistanceTextRow = -1;
  int fogStrengthRow = -1;
  int fogMediumSectionRow = -1;
  int fogAlbedoRow = -1;
  int fogGlowRow = -1;
  int fogEdgeRow = -1;
  int fogColourTextRow = -1;
  int fogDrawnByRow = -1;
  int volumetricSectionRow = -1;
  int lightInTheFogRow = -1;
};

LightEditorPanel::LightEditorPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_document{context.document}
  , m_toolBox{context.toolBox}
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

std::filesystem::path LightEditorPanel::iconPath() const
{
  return "LightEditor.svg";
}

void LightEditorPanel::panelShown()
{
  // not through refresh(): a panel that comes back into sight is brought up to date
  m_updateSignalDelayer->queueSignal();
}

void LightEditorPanel::panelHidden()
{
  // closing the panel closes its tool (D18); hiding it behind a tab does not
  if (auto& tool = m_toolBox.lightVolumeTool(); tool.active())
  {
    m_toolBox.toggleTool(tool);
  }
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
  setEmphasizedStyle(c.header);

  auto* badTargetText = createNoticeLabel(
    "BadTargetNotice",
    tr("Bad target info: some, but not all, of light_target, light_right and "
       "light_up are set, so the engine lights a point light."));
  setErrorStyle(badTargetText);
  c.fixBadTarget = createTextButton("FixBadTarget", tr("Fix: remove the partial keys"));
  c.badTargetNotice = createNotice(badTargetText, c.fixBadTarget);

  auto* staleSpotKeysText = createNoticeLabel(
    "StaleSpotKeysNotice",
    tr("Stale spot keys on a parallel light: while light_target, light_right and "
       "light_up are set, the engine reads neither light_radius nor light_center."));
  setErrorStyle(staleSpotKeysText);
  c.fixStaleSpotKeys =
    createTextButton("FixStaleSpotKeys", tr("Fix: remove the spot keys"));
  c.staleSpotKeysNotice = createNotice(staleSpotKeysText, c.fixStaleSpotKeys);

  c.offCentreText = createNoticeLabel("OffCentreNotice", {});
  setInfoStyle(c.offCentreText);
  c.offCentreNotice = createNotice(c.offCentreText);

  auto* lightOriginText = createNoticeLabel(
    "LightOriginNotice",
    tr("Legacy light_origin: the light is read-only until light_origin is moved "
       "into origin."));
  c.convertLightOrigin =
    createTextButton("ConvertLightOrigin", tr("Convert: move light_origin into origin"));
  c.lightOriginNotice = createNotice(lightOriginText, c.convertLightOrigin);

  auto* styledText =
    createNoticeLabel("StyledNotice", tr("Styled: drawn at full radius"));
  setInfoStyle(styledText);
  c.styledNotice = createNotice(styledText);

  // volume tools, first (D50): the only entry point to the light volume tool's draw mode
  // (D20); its handles and surfaces work without it (D56)
  c.drawVolumeButton =
    createVolumeToolButton("DrawVolume", "BrushTool.svg", tr("Draw volume"));
  c.drawVolumeButton->setToolTip(
    tr("Draw a new volume for the light, aligned with the world, in the map views"));
  c.drawVolumeButton->setCheckable(true);
  c.resetVolumeButton =
    createVolumeToolButton("ResetVolume", "ResetUV.svg", tr("Reset volume to default"));
  c.resetVolumeButton->setToolTip(
    tr("Reset the volume to the engine's default for the light's type"));
  c.handleThreshold = createSpin("HandleThreshold", HandleThresholdRange);
  c.handleThreshold->setSuffix(" %");
  c.handleThreshold->setToolTip(
    tr("A point light's handle box lies where its falloff drops below this share of "
       "its brightness"));
  c.handleThresholdDelayer = new SignalDelayer{HandleThresholdWriteDelay, this};

  auto* toolLayout = createRowLayout();
  toolLayout->addWidget(c.drawVolumeButton);
  toolLayout->addStretch(1);

  auto* resetLayout = createRowLayout();
  resetLayout->addWidget(c.resetVolumeButton);
  resetLayout->addStretch(1);
  resetLayout->addWidget(new QLabel{tr("Handles at")});
  resetLayout->addWidget(c.handleThreshold);

  auto* volumeToolsLayout = new QVBoxLayout{};
  volumeToolsLayout->setContentsMargins(0, 0, LayoutConstants::WideHMargin, 0);
  volumeToolsLayout->setSpacing(LayoutConstants::NarrowVMargin);
  volumeToolsLayout->addLayout(toolLayout);
  volumeToolsLayout->addLayout(resetLayout);

  auto* volumeTools = new QWidget{};
  volumeTools->setObjectName(controlName("VolumeTools"));
  volumeTools->setLayout(volumeToolsLayout);

  auto* form = createForm();
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
  auto* mixedTypesText = createNoticeLabel(
    "MixedTypesNotice",
    tr("The selected lights are of different types: select lights of one type to "
       "edit their volumes."));
  mixedTypesText->setWordWrap(true);
  setInfoStyle(mixedTypesText);
  c.mixedTypesRow = addRow(*form, mixedTypesText);
  c.equalRadius = createCheckBox("EqualRadius", tr("Equilateral"));
  c.equalRadiusRow = addRow(*form, c.equalRadius);
  c.radius = createSpin("Radius", RadiusRange);
  c.radiusRow = addRow(*form, tr("Radius"), c.radius);
  c.radiusXyzRow =
    addRow(*form, tr("Radius"), createVec3Layout(c.radiusXyz, "Radius", RadiusRange));
  // the rows that set an emitter omni's or spot's extent, together and in this order
  // (G0 section 9, range and reach in the panel): its range, the largest radius, and what
  // the range follows under auto range
  c.range = createSpin("Range", EmitterRangeRange);
  c.range->setToolTip(tr("How far the light reaches from the bulb: its largest radius"));
  c.rangeLabel = new QLabel{tr("Range")};
  c.rangeLabel->setObjectName(controlName("RangeLabel"));
  c.rangeRow = addRow(*form, c.rangeLabel, c.range);
  c.autoRange = createCheckBox("AutoRange", tr("Auto range"));
  c.autoRange->setToolTip(
    tr("The range follows the intensity, the size, the start and the falloff"));
  c.autoRangeRow = addRow(*form, c.autoRange);
  c.emitSize = createSpin("EmitSize", EmitterSizeRange);
  c.emitSize->setToolTip(
    tr("The length of the falloff in world units: one size past the start, the clamped "
       "inverse square is at about half its brightness and the exponential at about "
       "37 %"));
  c.emitSizeRow = addRow(*form, tr("Size"), c.emitSize);
  c.emitStart = createSpin("EmitStart", EmitterStartRange);
  c.emitStart->setToolTip(tr("Where the falloff starts, in world units from the bulb"));
  c.emitStartRow = addRow(*form, tr("Start"), c.emitStart);
  auto* autoRangeNote = createNoticeLabel(
    "AutoRangeNote",
    tr("With auto range on, Range follows Intensity, Size, Start and Falloff, and "
       "editing Range sets Size."));
  autoRangeNote->setWordWrap(true);
  setInfoStyle(autoRangeNote);
  c.autoRangeNoteRow = addRow(*form, autoRangeNote);
  c.center = createCheckBox("Center", tr("Center"));
  c.centerRow = addRow(*form, c.center);
  c.centerLabel = new QLabel{tr("Center")};
  c.centerLabel->setObjectName(controlName("CenterLabel"));
  c.centerXyzRow =
    addRow(*form, c.centerLabel, createVec3Layout(c.centerXyz, "Center", PositionRange));
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
  // an emitter spot's cone, which with its range takes the place of the spot vectors
  c.coneH = createSpin("ConeH", ConeRange);
  c.coneH->setToolTip(tr("The full horizontal angle of the beam in degrees"));
  c.coneHRow = addRow(*form, tr("Cone h"), c.coneH);
  c.coneV = createSpin("ConeV", ConeRange);
  c.coneV->setToolTip(tr("The full vertical angle of the beam in degrees"));
  c.coneVRow = addRow(*form, tr("Cone v"), c.coneV);

  // transform
  form->addSection(tr("Transform"));
  form->addRow(tr("Origin"), createVec3Layout(c.origin, "Origin", PositionRange));
  form->addRow(tr("Angles"), createVec3Layout(c.angles, "Angles", AngleRange));
  c.angles[0]->setToolTip(tr("Pitch, positive points down"));
  c.angles[1]->setToolTip(tr("Yaw"));
  c.angles[2]->setToolTip(tr("Roll"));

  // colour
  form->addSection(tr("Color"));
  c.color = new ColorButton{};
  c.color->setObjectName(controlName("Color"));
  form->addRow(tr("Color"), c.color);
  c.intensity = createSpin("Intensity", IntensityRange);
  c.intensity->setToolTip(tr("The brightest component of the color"));
  form->addRow(tr("Intensity"), c.intensity);

  // the emitter page: the emitter block
  auto* emitterForm = createForm();
  c.emitterForm = emitterForm;
  auto* mixedEmitterText = createNoticeLabel(
    "MixedEmitterNotice",
    tr("The selected lights are emitters and texture lights: choose a tab to make all "
       "of them lights of its kind."));
  mixedEmitterText->setWordWrap(true);
  setInfoStyle(mixedEmitterText);
  c.mixedEmitterRow = addRow(*emitterForm, mixedEmitterText);
  c.falloff = createComboBox("Falloff", {});
  setFalloffItems(*c.falloff);
  c.falloffRow = addRow(*emitterForm, tr("Falloff"), c.falloff);
  c.penumbraRow = addRow(
    *emitterForm,
    tr("Penumbra"),
    createSliderLayout(c.penumbraSlider, c.penumbra, "Penumbra", PenumbraRange));
  c.squarenessRow = addRow(
    *emitterForm,
    tr("Squareness"),
    createSliderLayout(c.squarenessSlider, c.squareness, "Squareness", SquarenessRange));
  c.squareness->setToolTip(tr("2 is an ellipse, 16 a rectangle"));
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
  c.patternRow = addRow(*emitterForm, tr("Pattern"), c.pattern);
  c.patternStrengthRow = addRow(
    *emitterForm,
    tr("Strength"),
    createSliderLayout(
      c.patternStrengthSlider,
      c.patternStrength,
      "PatternStrength",
      PatternStrengthRange));
  c.patternScaleRow = addRow(
    *emitterForm,
    tr("Scale"),
    createSliderLayout(
      c.patternScaleSlider, c.patternScale, "PatternScale", PatternScaleRange));
  c.patternScale->setToolTip(
    tr("The pattern's size in the world, whatever the light's range: at 1 an ice cell is "
       "about 32 units across, as on a light of radius 256 before; 2 is twice as fine"));
  c.patternPhaseRow = addRow(
    *emitterForm,
    tr("Phase"),
    createSliderLayout(
      c.patternPhaseSlider, c.patternPhase, "PatternPhase", PatternPhaseRange));
  c.energyClamp = createCheckBox("EnergyClamp", tr("Energy clamp (PBR safe)"));
  c.energyClampRow = addRow(*emitterForm, c.energyClamp);
  c.effect = createComboBox("Effect", {});
  setEffectItems(*c.effect);
  c.effect->setToolTip(tr("Varies the light's brightness over time"));
  c.effectRow = addRow(*emitterForm, tr("Effect"), c.effect);
  c.effectSpeed = createSpin("EffectSpeed", EffectSpeedRange);
  c.effectSpeedRow = addRow(*emitterForm, tr("Speed"), c.effectSpeed);
  c.effectAmountRow = addRow(
    *emitterForm,
    tr("Amount"),
    createSliderLayout(
      c.effectAmountSlider, c.effectAmount, "EffectAmount", EffectAmountRange));
  c.effectPhase = createSpin("EffectPhase", EffectPhaseRange);
  c.effectPhase->setToolTip(tr(
    "Where in its cycle the effect starts, 0 to 1, so that neighbouring lights differ"));
  c.effectPhaseRow = addRow(*emitterForm, tr("Phase"), c.effectPhase);

  // the texture light page: the light material and the engine's style
  auto* textureForm = createForm();
  c.textureForm = textureForm;
  c.texture = createComboBox("Texture", {EngineDefaultTexture});
  c.texture->setEditable(true);
  c.texture->setInsertPolicy(QComboBox::NoInsert);
  c.texture->setItemData(0, "lights/squarelight1", Qt::ToolTipRole);
  textureForm->addRow(tr("Texture"), c.texture);
  // the choices of the entity definition, else the engine's predefined styles
  c.styleChoice = createComboBox("StyleChoice", {});
  textureForm->addRow(tr("Style"), c.styleChoice);

  // the Fog page (V7), the engine's Fog tab in its words and order: on lights that are
  // not fog lights a button that makes them fog lights; on fog lights the material, the
  // density and what shows only in the game's volumetric fog
  auto* fogForm = createForm();
  c.fogForm = fogForm;
  c.fogNoticeText = createNoticeLabel("FogNotice", tr("This light is not a fog light."));
  c.makeFogLight = createTextButton("MakeFogLight", tr("Make it a fog light"));
  c.fogNoticeRow = addRow(*fogForm, createNotice(c.fogNoticeText, c.makeFogLight));
  // the 3D preview draws fog lights by the classic fog pass's rules (row 212)
  auto* fogStatusText = createNoticeLabel(
    "FogStatus",
    tr(
      "The 3D preview draws fog lights as the classic fog pass does: Colour and "
      "Density show here. Albedo, Glow and Soft sides show only in the game, with the "
      "volumetric fog on."));
  fogStatusText->setWordWrap(true);
  setInfoStyle(fogStatusText);
  c.fogStatusRow = addRow(*fogForm, fogStatusText);
  c.fogMaterial = createComboBox("FogMaterial", {});
  c.fogMaterialRow = addRow(*fogForm, tr("Fog material"), c.fogMaterial);

  c.fogDensitySectionRow = fogForm->rowCount();
  fogForm->addSection(tr("Density"));
  auto* ownFogDistanceText = createNoticeLabel(
    "OwnFogDistanceNotice",
    tr(
      "This material sets its own fog distance: the light's density does not change "
      "it."));
  ownFogDistanceText->setWordWrap(true);
  setInfoStyle(ownFogDistanceText);
  c.ownFogDistanceRow = addRow(*fogForm, ownFogDistanceText);
  c.fogDensityForm =
    createComboBox("FogDensityForm", {tr("Distance"), tr("Faint ( fixed 500 units )")});
  c.fogDensityFormRow = addRow(*fogForm, tr("Form"), c.fogDensityForm);
  c.fogDensityRow = addRow(
    *fogForm,
    tr("Density"),
    createSliderLayout(c.fogDensitySlider, c.fogDensity, "FogDensity", FogDensityRange));
  c.fogDistance = createSpin("FogDistance", FogDistanceRange);
  c.fogDistance->setSuffix(" units");
  c.fogDistanceRow = addRow(*fogForm, tr("Distance"), c.fogDistance);
  c.fogDistanceText = createNoticeLabel("FogDistanceText", {});
  c.fogDistanceText->setWordWrap(true);
  setInfoStyle(c.fogDistanceText);
  c.fogDistanceTextRow = addRow(*fogForm, c.fogDistanceText);
  c.fogStrengthRow = addRow(
    *fogForm,
    tr("Strength"),
    createSliderLayout(
      c.fogStrengthSlider, c.fogStrength, "FogStrength", FogStrengthRange));

  c.fogMediumSectionRow = fogForm->rowCount();
  fogForm->addSection(tr("In the volumetric fog"));
  c.fogAlbedoRow = addRow(
    *fogForm,
    tr("Albedo"),
    createSliderLayout(c.fogAlbedoSlider, c.fogAlbedo, "FogAlbedo", FogAlbedoRange));
  c.fogGlowRow = addRow(
    *fogForm,
    tr("Glow"),
    createSliderLayout(
      c.fogGlowSlider, c.fogGlow, "FogGlow", FogGlowRange, FogGlowSliderMaximum));
  c.fogGlow->setToolTip(tr("The slider runs to 2; type a value up to 8"));
  c.fogEdgeRow = addRow(
    *fogForm,
    tr("Soft sides"),
    createSliderLayout(
      c.fogEdgeSlider, c.fogEdge, "FogEdge", FogEdgeRange, FogEdgeSliderMaximum));
  c.fogEdge->setSuffix(" units");
  c.fogEdge->setToolTip(tr("The slider runs to 256; type a value up to 4,096"));
  c.fogColourText = createNoticeLabel("FogColourText", {});
  c.fogColourText->setWordWrap(true);
  setInfoStyle(c.fogColourText);
  c.fogColourTextRow = addRow(*fogForm, c.fogColourText);
  // "Always" is a third item only while a selected fog light says volumetric 1
  c.fogDrawnBy = createComboBox("FogDrawnBy", {});
  c.fogDrawnBy->addItem(tr("Automatic"), 0);
  c.fogDrawnBy->addItem(tr("Classic fog pass only"), 2);
  c.fogDrawnByRow = addRow(*fogForm, tr("Drawn by"), c.fogDrawnBy);

  c.emitterPage = createTabPage("EmitterPage", emitterForm);
  c.texturePage = createTabPage("TexturePage", textureForm);
  c.fogPage = createTabPage("FogPage", fogForm);
  c.tabs = new TabBook{};
  c.tabs->setObjectName(controlName("Tabs"));
  c.tabs->addPage(c.emitterPage, tr("Emitter"));
  c.tabs->addPage(c.texturePage, tr("Texture"));
  c.tabs->addPage(c.fogPage, tr("Fog"));
  fitTabPages({c.emitterPage, c.texturePage, c.fogPage}, EmitterPageIndex);

  // misc
  auto* miscForm = createForm();
  c.miscForm = miscForm;
  miscForm->addSection(tr("Misc"));
  c.castShadows = createCheckBox("CastShadows", tr("Cast shadows"));
  miscForm->addRow(c.castShadows);
  c.skipSpecular = createCheckBox("SkipSpecular", tr("Skip specular"));
  miscForm->addRow(c.skipSpecular);
  // the key volumetric of a light that is not a fog light (row 51); a fog light's is the
  // Fog page's Drawn by
  c.volumetricSectionRow = miscForm->rowCount();
  miscForm->addSection(tr("Volumetric fog"));
  c.lightInTheFog =
    createComboBox("LightInTheFog", {tr("Automatic"), tr("Always"), tr("Never")});
  c.lightInTheFogRow = addRow(*miscForm, tr("Light in the fog"), c.lightInTheFog);

  c.inputs = {
    c.handleThreshold,
    c.equalRadius,
    c.radius,
    c.center,
    c.explicitStartEnd,
    c.color,
    c.intensity,
    c.texture,
    c.styleChoice,
    c.falloff,
    c.emitSize,
    c.emitStart,
    c.autoRange,
    c.range,
    c.coneH,
    c.coneV,
    c.penumbraSlider,
    c.penumbra,
    c.squarenessSlider,
    c.squareness,
    c.pattern,
    c.patternStrengthSlider,
    c.patternStrength,
    c.patternScaleSlider,
    c.patternScale,
    c.patternPhaseSlider,
    c.patternPhase,
    c.energyClamp,
    c.effect,
    c.effectSpeed,
    c.effectAmountSlider,
    c.effectAmount,
    c.effectPhase,
    c.fogMaterial,
    c.fogDensityForm,
    c.fogDensitySlider,
    c.fogDensity,
    c.fogDistance,
    c.fogStrengthSlider,
    c.fogStrength,
    c.fogAlbedoSlider,
    c.fogAlbedo,
    c.fogGlowSlider,
    c.fogGlow,
    c.fogEdgeSlider,
    c.fogEdge,
    c.fogDrawnBy,
    c.lightInTheFog,
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

  // the wheel scrolls the panel unless the input under it has the focus
  guardWheel(c.inputs, new WheelGuard{this});

  // top to bottom: the volume tools, the light's geometry and colour, the page of its
  // kind, the flags (D50)
  auto* editorLayout = new QVBoxLayout{};
  editorLayout->setContentsMargins(0, 0, 0, 0);
  editorLayout->setSpacing(LayoutConstants::WideVMargin);
  editorLayout->addWidget(volumeTools);
  editorLayout->addWidget(createFormWidget(form));
  editorLayout->addWidget(c.tabs);
  editorLayout->addWidget(createFormWidget(miscForm));

  c.editor = new QWidget{};
  c.editor->setObjectName(controlName("Editor"));
  c.editor->setLayout(editorLayout);

  auto* contentLayout = new QVBoxLayout{};
  contentLayout->setContentsMargins(
    LayoutConstants::WideHMargin,
    LayoutConstants::WideVMargin,
    0,
    LayoutConstants::WideVMargin);
  contentLayout->setSpacing(LayoutConstants::MediumVMargin);
  contentLayout->addWidget(c.header);
  contentLayout->addWidget(c.lightOriginNotice);
  contentLayout->addWidget(c.badTargetNotice);
  contentLayout->addWidget(c.staleSpotKeysNotice);
  contentLayout->addWidget(c.offCentreNotice);
  contentLayout->addWidget(c.styledNotice);
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
                             const std::array<MultiSpinControl*, 3>& spins,
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

  // a slider writes whenever the keyboard, a click on its groove or the wheel moves it,
  // but not while it is dragged; on release it writes only if the drag moved it, because
  // its position is the first light's value rounded to its steps, so a click on the
  // handle alone would write that rounded value to every selected light
  const auto connectSlider =
    [&](
      QSlider* slider, MultiSpinControl* spin, const std::string& commandName, auto set) {
      connectSpin(spin, commandName, set);

      const auto write = [=, this](const int position) {
        const auto value = sliderValue(*slider, *spin, position);
        editLights(commandName, [&](mdl::LightProperties& light) { set(light, value); });
      };
      // the position at which the handle was pressed
      const auto pressedPosition = std::make_shared<int>(slider->value());
      connect(slider, &QSlider::valueChanged, this, [=](const int position) {
        if (slider->isSliderDown())
        {
          const auto blocker = QSignalBlocker{spin};
          spin->setValue(sliderValue(*slider, *spin, position));
        }
        else
        {
          write(position);
        }
      });
      connect(slider, &QSlider::sliderPressed, this, [=]() {
        *pressedPosition = slider->value();
      });
      connect(slider, &QSlider::sliderReleased, this, [=, this]() {
        if (slider->value() != *pressedPosition)
        {
          write(slider->value());
        }
        else
        {
          // the drag set the spin box without writing; it shows the lights again,
          // "multi" too
          refresh();
        }
      });
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
      const auto policy =
        m_document.map().gameInfo().gameConfig.entityConfig.rotationPolicy;
      editOrientedLights(
        "Set Light Type",
        [&](const mdl::Entity& entity, mdl::LightProperties& light)
          -> std::optional<vm::mat3x3d> {
          // like the in-game editor: a change of type resets the light to the default
          // of the new type, keeping its colour and its emitter block; an
          // emitter keeps its range with auto range off and its rotation, and only one
          // without a rotation points down when it becomes a spot (G0 section 9)
          if (light.type == type)
          {
            return std::nullopt;
          }
          light = mdl::emitterShapeSwitch(type, light);
          return mdl::lightUsesEmitter(light) && type == mdl::LightType::Spot
                     && vm::is_equal(
                       mdl::lightRotation(entity, policy),
                       vm::mat3x3d::identity(),
                       NoRotationEpsilon)
                   ? std::optional{mdl::emitterSpotRotation()}
                   : std::nullopt;
        });
    });
  }

  connect(c.equalRadius, &QAbstractButton::clicked, this, [this](const bool checked) {
    auto& controls = *m_controls;
    if (controls.emitterOmnis)
    {
      // an emitter omni's Range row stays; unticking shows the three radii and writes
      // nothing, ticking sets all three to the range (G0 section 9)
      controls.emitterRadiiShown = !checked;
      controls.form->setRowVisible(controls.radiusXyzRow, !checked);
      if (checked)
      {
        editLights("Set Light Radius", [](mdl::LightProperties& light) {
          const auto range = mdl::emitterRange(light.radius);
          light.radius = vm::vec3d{range, range, range};
          light.equalRadius = true;
          light = mdl::applyEmitterOmniRangeFloor(light);
          light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Range);
        });
      }
      return;
    }

    controls.equilateral = checked;
    controls.form->setRowVisible(controls.radiusRow, checked);
    controls.form->setRowVisible(controls.radiusXyzRow, !checked);

    // unchecking writes nothing: the three components are equal until one of them is
    // edited
    if (checked)
    {
      editLights("Set Light Radius", [](mdl::LightProperties& light) {
        light.radius = vm::vec3d{light.radius.x(), light.radius.x(), light.radius.x()};
        light.equalRadius = true;
        light = mdl::applyEmitterOmniRangeFloor(light);
        light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Range);
      });
    }
  });
  connectSpin(
    c.radius, "Set Light Radius", [](mdl::LightProperties& light, const double value) {
      light.radius = vm::vec3d{value, value, value};
      light.equalRadius = true;
      // an emitter omni's range is at least 24, and its size follows its range
      light = mdl::applyEmitterOmniRangeFloor(light);
      light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Range);
    });
  for (size_t i = 0; i < 3; ++i)
  {
    connectSpin(
      c.radiusXyz[i],
      "Set Light Radius",
      [=](mdl::LightProperties& light, const double value) {
        light.radius[i] = value;
        light.equalRadius = false;
        light = mdl::applyEmitterOmniRangeFloor(light);
        light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Range);
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
      const auto intensity = intensityOf(light.color);
      light.color = hue * intensity;
      // a darker colour is a lower intensity, which an emitter's range follows
      if (intensityOf(light.color) != intensity)
      {
        light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Reach);
      }
    });
  });
  connectSpin(
    c.intensity,
    "Set Light Intensity",
    [](mdl::LightProperties& light, const double value) {
      light.color = hueOf(light.color) * value;
      // the range of an emitter follows its intensity
      light = mdl::applyEmitterAutoRange(light, mdl::EmitterEdit::Reach);
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
  connect(c.styleChoice, &QComboBox::activated, this, [this](const int index) {
    auto ok = false;
    const auto style = m_controls->styleChoice->itemData(index).toString().toInt(&ok);
    if (ok)
    {
      editLights(
        "Set Light Style", [&](mdl::LightProperties& light) { light.style = style; });
    }
  });

  // the tab is the switch between the two kinds of light: a click on it writes, the page
  // change of a refresh does not
  connect(
    c.tabs->tabBar(), &TabBar::tabClicked, this, &LightEditorPanel::switchLightKind);

  // an emitter control edits emitters only; after an edit of the reach or the range, auto
  // range recomputes each light from its own values; the start then stays below the range
  // (G0 section 5), which auto range ensures only where it applies
  const auto followReach = std::optional{mdl::EmitterEdit::Reach};
  const auto followRange = std::optional{mdl::EmitterEdit::Range};
  const auto followNothing = std::optional<mdl::EmitterEdit>{};
  const auto forEmitters = [](const std::optional<mdl::EmitterEdit> follow, auto set) {
    return [=](mdl::LightProperties& light, const auto value) {
      if (mdl::lightUsesEmitter(light))
      {
        set(light, value);
        if (follow)
        {
          light = mdl::applyEmitterAutoRange(light, *follow);
          light.emitter.start =
            mdl::clampEmitterStart(light.emitter.start, mdl::emitterRange(light.radius));
        }
      }
    };
  };

  // a curve that gains a reach factor tests the range again (G0 section 5)
  const auto setFalloff =
    forEmitters(followReach, [](mdl::LightProperties& light, const int falloff) {
      light = mdl::applyEmitterFalloff(light, mdl::LightFalloff(falloff));
    });
  connect(c.falloff, &QComboBox::activated, this, [=, this](const int index) {
    auto ok = false;
    const auto falloff = m_controls->falloff->itemData(index).toInt(&ok);
    if (ok)
    {
      editLights("Set Light Falloff", [&](mdl::LightProperties& light) {
        setFalloff(light, falloff);
      });
    }
  });
  connectSpin(
    c.emitSize,
    "Set Light Size",
    forEmitters(followReach, [](mdl::LightProperties& light, const double value) {
      light.emitter.size = value;
    }));
  connectSpin(
    c.emitStart,
    "Set Light Start",
    forEmitters(followReach, [](mdl::LightProperties& light, const double value) {
      light.emitter.start = value;
    }));
  connectCheckBox(
    c.autoRange,
    "Set Light Auto Range",
    forEmitters(followReach, [](mdl::LightProperties& light, const bool checked) {
      light.emitter.autoRange = checked;
    }));
  connectSpin(
    c.range,
    "Set Light Range",
    forEmitters(followRange, [](mdl::LightProperties& light, const double value) {
      light = mdl::withEmitterRange(light, value);
    }));
  connectSpin(
    c.coneH,
    "Set Light Cone",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.coneH = value;
    }));
  connectSpin(
    c.coneV,
    "Set Light Cone",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.coneV = value;
    }));
  connectSlider(
    c.penumbraSlider,
    c.penumbra,
    "Set Light Penumbra",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.penumbra = value;
    }));
  connectSlider(
    c.squarenessSlider,
    c.squareness,
    "Set Light Squareness",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.squareness = value;
    }));
  const auto setPattern =
    forEmitters(followNothing, [](mdl::LightProperties& light, const int index) {
      light.emitter.pattern = mdl::LightPattern(index);
    });
  connect(c.pattern, &QComboBox::activated, this, [=, this](const int index) {
    editLights("Set Light Pattern", [&](mdl::LightProperties& light) {
      setPattern(light, index);
    });
  });
  connectSlider(
    c.patternStrengthSlider,
    c.patternStrength,
    "Set Light Pattern Strength",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.patternStrength = value;
    }));
  connectSlider(
    c.patternScaleSlider,
    c.patternScale,
    "Set Light Pattern Scale",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.patternScale = value;
    }));
  connectSlider(
    c.patternPhaseSlider,
    c.patternPhase,
    "Set Light Pattern Phase",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.patternPhase = value;
    }));
  connectCheckBox(
    c.energyClamp,
    "Set Light Energy Clamp",
    forEmitters(followNothing, [](mdl::LightProperties& light, const bool checked) {
      light.emitter.energyClamp = checked;
    }));
  const auto setEffect = forEmitters(
    followNothing,
    [](mdl::LightProperties& light, const int effect) { light.emitter.effect = effect; });
  connect(c.effect, &QComboBox::activated, this, [=, this](const int index) {
    auto ok = false;
    const auto effect = m_controls->effect->itemData(index).toInt(&ok);
    if (ok)
    {
      editLights("Set Light Effect", [&](mdl::LightProperties& light) {
        setEffect(light, effect);
      });
    }
  });
  connectSpin(
    c.effectSpeed,
    "Set Light Effect Speed",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.effectSpeed = value;
    }));
  connectSlider(
    c.effectAmountSlider,
    c.effectAmount,
    "Set Light Effect Amount",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.effectAmount = value;
    }));
  connectSpin(
    c.effectPhase,
    "Set Light Effect Phase",
    forEmitters(followNothing, [](mdl::LightProperties& light, const double value) {
      light.emitter.effectPhase = value;
    }));

  // the Fog page (V7): every fog control edits fog lights only
  const auto forFogLights = [this](auto set) {
    return [this, set](mdl::LightProperties& light, const auto value) {
      if (isFogLight(m_document.map(), light))
      {
        set(light, value);
      }
    };
  };
  connect(c.makeFogLight, &QAbstractButton::clicked, this, [this]() {
    // without materials no light is a fog light, so every light would be rewritten
    if (!materialsLoaded(m_document.map()))
    {
      return;
    }
    editLights("Make Fog Light", [this](mdl::LightProperties& light) {
      // the stock fog material, an emitter made a texture light as the Texture tab makes
      // it one (LightMakeFogLight); a fog light keeps its material
      if (!isFogLight(m_document.map(), light))
      {
        if (mdl::lightUsesEmitter(light))
        {
          light = mdl::asTextureLight(light);
        }
        light.texture = FogLightTexture;
      }
    });
  });
  const auto setFogMaterial =
    forFogLights([](mdl::LightProperties& light, const std::string& texture) {
      // the list holds lowercase decl names: a re-pick of the material shown keeps the
      // light's own spelling
      if (!kdl::ci::str_is_equal(light.texture, texture))
      {
        light.texture = texture;
      }
    });
  connect(c.fogMaterial, &QComboBox::activated, this, [=, this](const int index) {
    const auto texture = m_controls->fogMaterial->itemText(index).toStdString();
    editLights("Set Light Texture", [&](mdl::LightProperties& light) {
      setFogMaterial(light, texture);
    });
  });
  // the form of the density: a distance, or a faint fog's strength at 500 units; picking
  // a light's own form changes nothing
  const auto setFogDensityForm =
    forFogLights([](mdl::LightProperties& light, const int index) {
      const auto faint = index == 1;
      if (mdl::fogIsFaint(light.fog.distance.value_or(1.0)) != faint)
      {
        light.fog.distance = faint ? mdl::FaintFogStrength : mdl::DefaultFogDistance;
      }
    });
  connect(c.fogDensityForm, &QComboBox::activated, this, [=, this](const int index) {
    editLights("Set Fog Density", [&](mdl::LightProperties& light) {
      setFogDensityForm(light, index);
    });
  });
  // whole units in the map, as the engine writes the slider's distance
  connectSlider(
    c.fogDensitySlider,
    c.fogDensity,
    "Set Fog Density",
    forFogLights([](mdl::LightProperties& light, const double value) {
      light.fog.distance = std::round(mdl::fogDensityToDistance(value));
    }));
  connectSpin(
    c.fogDistance,
    "Set Fog Density",
    forFogLights([](mdl::LightProperties& light, const double value) {
      light.fog.distance = value;
    }));
  connectSlider(
    c.fogStrengthSlider,
    c.fogStrength,
    "Set Fog Density",
    forFogLights([](mdl::LightProperties& light, const double value) {
      light.fog.distance = value;
    }));
  connectSlider(
    c.fogAlbedoSlider,
    c.fogAlbedo,
    "Set Fog Albedo",
    forFogLights(
      [](mdl::LightProperties& light, const double value) { light.fog.albedo = value; }));
  connectSlider(
    c.fogGlowSlider,
    c.fogGlow,
    "Set Fog Glow",
    forFogLights([](mdl::LightProperties& light, const double value) {
      light.fog.emission = value;
    }));
  connectSlider(
    c.fogEdgeSlider,
    c.fogEdge,
    "Set Fog Soft Sides",
    forFogLights(
      [](mdl::LightProperties& light, const double value) { light.fog.edge = value; }));
  const auto setFogDrawnBy = forFogLights(
    [](mdl::LightProperties& light, const int value) { light.fog.volumetric = value; });
  connect(c.fogDrawnBy, &QComboBox::activated, this, [=, this](const int index) {
    auto ok = false;
    const auto value = m_controls->fogDrawnBy->itemData(index).toInt(&ok);
    if (ok)
    {
      editLights("Set Fog Drawn By", [&](mdl::LightProperties& light) {
        setFogDrawnBy(light, value);
      });
    }
  });
  connect(c.lightInTheFog, &QComboBox::activated, this, [this](const int index) {
    editLights("Set Light In The Fog", [&](mdl::LightProperties& light) {
      light.fog.volumetric = index;
    });
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

  connect(c.resetVolumeButton, &QAbstractButton::clicked, this, [this]() {
    editLights("Reset Light Volume", resetLightVolume);
  });
  // not a light property: the threshold of every point light's handle box (D53); each
  // write saves the preferences and makes every map view and the map window reset their
  // shortcuts, so a burst of steps is written once, when the steps pause
  connect(
    c.handleThreshold, &QDoubleSpinBox::valueChanged, this, [this](const double value) {
      m_controls->pendingHandleThreshold = value;
      m_controls->handleThresholdDelayer->queueSignal();
    });
  // a write still pending when the panel is destroyed is dropped: at shutdown, the
  // preference manager may be gone already
  connect(c.handleThresholdDelayer, &SignalDelayer::processSignal, this, [this]() {
    if (auto& pending = m_controls->pendingHandleThreshold)
    {
      const auto value = *std::exchange(pending, std::nullopt);
      setPref(Preferences::LightVolumeHandleThreshold, float(value / 100.0));
    }
  });
  connect(
    c.drawVolumeButton,
    &QAbstractButton::clicked,
    this,
    &LightEditorPanel::toggleVolumeDrawing);

  connect(c.fixBadTarget, &QAbstractButton::clicked, this, [this]() {
    removeTargetProperties(
      "Remove Partial Spot Properties",
      [](const mdl::Entity&, const mdl::LightProperties& light) {
        return light.badTargetInfo;
      });
  });
  connect(c.fixStaleSpotKeys, &QAbstractButton::clicked, this, [this]() {
    removeTargetProperties("Remove Stale Spot Properties", hasStaleSpotKeys);
  });
  connect(
    c.convertLightOrigin,
    &QAbstractButton::clicked,
    this,
    &LightEditorPanel::convertLightOrigins);

  connect(c.tabs, &TabBook::pageChanged, this, [this](const int index) {
    auto& controls = *m_controls;
    controls.page = index;
    fitTabPages({controls.emitterPage, controls.texturePage, controls.fogPage}, index);
  });
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
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&] {
    m_controls->texturesStale = true;
    refresh();
  });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&] { refresh(); });
  // a new selection shows the page of its kind, the Fog page too (V7)
  m_notifierConnection += m_document.selectionDidChangeNotifier.connect([&](const auto&) {
    m_controls->selectionChanged = true;
    refresh();
  });
  m_notifierConnection +=
    m_document.entityDefinitionsDidChangeNotifier.connect([&] { refresh(); });
  // a reload of the material collections parses the shaders again
  m_notifierConnection += m_document.materialCollectionsDidChangeNotifier.connect([&] {
    m_controls->texturesStale = true;
    refresh();
  });

  // the shaders are loaded again when the mods or the game path change
  m_notifierConnection += m_document.modsDidChangeNotifier.connect([&] {
    m_controls->texturesStale = true;
    refresh();
  });
  // of the preferences, only two matter: the game path, whose change reloads the models
  // and materials (Map::setGamePath) with no other notification, and the handle
  // threshold, which the panel shows and View Options resets; the others are ignored,
  // because the steps of a spin box or the ticks of the fly speed wheel save them, and
  // reading the light materials again walks every shader
  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const std::filesystem::path& path) {
        if (path == m_document.map().gameInfo().gamePathPreference.path)
        {
          m_controls->texturesStale = true;
          refresh();
        }
        else if (path == Preferences::LightVolumeHandleThreshold.path)
        {
          refresh();
        }
      });

  // the buttons follow the tool, which Esc, another tool or a new map also deactivate
  const auto toolChanged = [&](Tool& tool) {
    if (&tool == &m_toolBox.lightVolumeTool())
    {
      updateVolumeToolButtons();
    }
  };
  m_notifierConnection += m_toolBox.toolActivatedNotifier.connect(toolChanged);
  m_notifierConnection += m_toolBox.toolDeactivatedNotifier.connect(toolChanged);
  m_notifierConnection += m_toolBox.lightVolumeTool().modeDidChangeNotifier.connect(
    [&]() { updateVolumeToolButtons(); });
}

void LightEditorPanel::refresh()
{
  // a panel out of sight is brought up to date when it is shown (panelShown)
  if (!isOutOfSight())
  {
    m_updateSignalDelayer->queueSignal();
  }
}

bool LightEditorPanel::isOutOfSight() const
{
  if (isWindow())
  {
    return false;
  }

  const auto* topLevel = window();
  return !isVisible()
         || !QRect{mapTo(topLevel, QPoint{0, 0}), size()}.intersects(topLevel->rect());
}

void LightEditorPanel::updateControls()
{
  // a drag of the volume tool changes the light at every mouse move; the commit at its
  // end refreshes the panel
  if (m_toolBox.lightVolumeTool().isDragging())
  {
    return;
  }

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

  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;
  const auto& firstEntity = entityNodes.front()->entity();
  const auto lights = entityNodes | std::views::transform([](const auto* entityNode) {
                        return mdl::parseLightProperties(entityNode->entity());
                      })
                      | kdl::ranges::to<std::vector>();
  const auto angles = entityNodes | std::views::transform([&](const auto* entityNode) {
                        return pitchYawRoll(entityNode->entity(), policy);
                      })
                      | kdl::ranges::to<std::vector>();
  const auto anyLight = [&](const auto& predicate) {
    return std::ranges::any_of(lights, predicate);
  };
  const auto allLights = [&](const auto& predicate) {
    return std::ranges::all_of(lights, predicate);
  };
  const auto anyEmitter =
    anyLight([](const auto& light) { return mdl::lightUsesEmitter(light); });
  const auto allEmitters =
    allLights([](const auto& light) { return mdl::lightUsesEmitter(light); });

  // setting a control must not write it
  auto blockers = std::vector<QSignalBlocker>{};
  blockers.reserve(c.inputs.size());
  for (auto* input : c.inputs)
  {
    blockers.emplace_back(input);
  }

  // header and notices
  c.header->setText(
    lights.size() == 1 ? tr("Light: %1").arg(lightName(firstEntity))
                       : tr("%1 lights selected: edits apply to all").arg(lights.size()));

  const auto readOnly = anyLight([](const auto& light) { return light.hasLightOrigin; });
  c.lightOriginNotice->setVisible(readOnly);
  c.editor->setEnabled(!readOnly);

  // shown after every edit until the keys are removed, because the light is read again
  c.badTargetNotice->setVisible(
    anyLight([](const auto& light) { return light.badTargetInfo; }));
  c.fixBadTarget->setEnabled(!readOnly);

  auto staleSpotKeys = false;
  for (size_t i = 0; i < lights.size(); ++i)
  {
    staleSpotKeys =
      staleSpotKeys || hasStaleSpotKeys(entityNodes[i]->entity(), lights[i]);
  }
  c.staleSpotKeysNotice->setVisible(staleSpotKeys);
  c.fixStaleSpotKeys->setEnabled(!readOnly);

  auto recentre = std::optional<double>{};
  for (const auto& light : lights)
  {
    if (const auto distance = recentreDistance(light))
    {
      recentre = std::max(recentre.value_or(0.0), *distance);
    }
  }
  // a threshold still to be written stays: the preference holds the one that it replaces
  if (!c.pendingHandleThreshold)
  {
    c.handleThreshold->setValue(
      double(pref(Preferences::LightVolumeHandleThreshold)) * 100.0);
  }
  // an emitter's handle box is its volume, without a threshold
  c.handleThreshold->setEnabled(!allEmitters);

  c.offCentreNotice->setVisible(recentre.has_value());
  if (recentre)
  {
    c.offCentreText->setText(
      tr("Off-centre projection: the first box edit recentres it (the far rectangle "
         "moves by up to %1 units)")
        .arg(std::ceil(*recentre)));
  }

  c.styledNotice->setVisible(anyLight([](const auto& light) {
    return light.style != -1 && !mdl::lightUsesEmitter(light);
  }));

  // type
  const auto [type, mixedTypes] =
    firstValue(lights, [](const auto& light) { return light.type; });
  c.type->setExclusive(false);
  for (auto* radio : c.typeRadios)
  {
    radio->setChecked(false);
  }
  c.type->setExclusive(true);
  if (!mixedTypes)
  {
    c.typeRadios[size_t(type)]->setChecked(true);
  }

  // volume, only for lights of one type
  const auto isPointOrSun = !mixedTypes && type != mdl::LightType::Spot;
  const auto isSpot = !mixedTypes && type == mdl::LightType::Spot;
  // an emitter has no target vectors: its range and cone take their place
  const auto isTextureSpot = isSpot && !anyEmitter;
  const auto isEmitterSpot = isSpot && allEmitters;
  // emitter omnis and spots, whatever the mix, share the rows that set their extent
  // (G0 section 9, range and reach in the panel)
  const auto isEmitterOmniOrSpot = allEmitters && allLights([](const auto& light) {
                                     return light.type != mdl::LightType::Sun;
                                   });
  c.emitterOmnis = isEmitterOmniOrSpot && !mixedTypes && type == mdl::LightType::Point;
  const auto allEqualRadius =
    allLights([](const auto& light) { return light.equalRadius; });
  // one radius only when the user chose it and every light has one (D52); an emitter
  // omni's Equilateral shows ticked while its radii are equal, unless the user chose to
  // see them
  const auto equilateral = c.emitterOmnis ? allEqualRadius && !c.emitterRadiiShown
                                          : c.equilateral && allEqualRadius;
  const auto hasCenter = allLights([](const auto& light) { return light.hasCenter; });
  const auto explicitStartEnd =
    allLights([](const auto& light) { return light.explicitStartEnd; });

  c.form->setRowVisible(c.mixedTypesRow, mixedTypes && !isEmitterOmniOrSpot);
  c.form->setRowVisible(c.equalRadiusRow, isPointOrSun);
  // an emitter omni's one radius is its Range
  c.form->setRowVisible(c.radiusRow, isPointOrSun && equilateral && !c.emitterOmnis);
  c.form->setRowVisible(c.radiusXyzRow, isPointOrSun && !equilateral);
  c.form->setRowVisible(c.centerRow, isPointOrSun);
  c.form->setRowVisible(c.centerXyzRow, isPointOrSun && hasCenter);
  c.form->setRowVisible(c.targetRow, isTextureSpot);
  c.form->setRowVisible(c.rightRow, isTextureSpot);
  c.form->setRowVisible(c.upRow, isTextureSpot);
  c.form->setRowVisible(c.explicitStartEndRow, isTextureSpot);
  c.form->setRowVisible(c.startRow, isTextureSpot && explicitStartEnd);
  c.form->setRowVisible(c.endRow, isTextureSpot && explicitStartEnd);
  // Range, Auto range, Size and Start, each light set by its own rule; Auto range and
  // Size only for a curve that uses them; none for a sun. The cone only when every light
  // is a spot.
  const auto hasReachCurve = anyLight(
    [](const auto& light) { return mdl::emitterCurveHasReach(light.emitter.falloff); });
  const auto autoRangeInForce =
    allLights([](const auto& light) { return mdl::emitterAutoRangeInForce(light); });
  c.form->setRowVisible(c.rangeRow, isEmitterOmniOrSpot);
  c.form->setRowVisible(c.autoRangeRow, isEmitterOmniOrSpot && hasReachCurve);
  c.form->setRowVisible(c.emitSizeRow, isEmitterOmniOrSpot && hasReachCurve);
  c.form->setRowVisible(c.emitStartRow, isEmitterOmniOrSpot);
  c.form->setRowVisible(c.autoRangeNoteRow, isEmitterOmniOrSpot && hasReachCurve);
  c.rangeLabel->setText(
    isEmitterOmniOrSpot && autoRangeInForce ? tr("Range (auto)") : tr("Range"));
  c.form->setRowVisible(c.coneHRow, isEmitterSpot);
  c.form->setRowVisible(c.coneVRow, isEmitterSpot);
  // an emitter omni's light_center is the bulb's offset from its origin
  const auto centerText = allEmitters && !mixedTypes && type == mdl::LightType::Point
                            ? tr("Bulb offset")
                            : tr("Center");
  c.center->setText(centerText);
  c.centerLabel->setText(centerText);

  c.equalRadius->setChecked(equilateral);
  setSpin(c.radius, lights, [](const auto& light) { return light.radius.x(); });
  setVec3(c.radiusXyz, lights, [](const auto& light) { return light.radius; });
  setCheckBox(c.center, lights, [](const auto& light) { return light.hasCenter; });
  setVec3(c.centerXyz, lights, [](const auto& light) { return light.center; });
  setVec3(c.target, lights, [](const auto& light) { return light.target; });
  setVec3(c.right, lights, [](const auto& light) { return light.right; });
  setVec3(c.up, lights, [](const auto& light) { return light.up; });
  setCheckBox(
    c.explicitStartEnd, lights, [](const auto& light) { return light.explicitStartEnd; });
  setVec3(c.start, lights, [](const auto& light) { return light.start; });
  setVec3(c.end, lights, [](const auto& light) { return light.end; });

  // transform
  setVec3(c.origin, lights, [](const auto& light) { return light.origin; });
  setVec3(c.angles, angles, [](const auto& pitchYawRoll) { return pitchYawRoll; });

  // colour and texture; the colour button shows the first light's hue
  c.color->setColor(toQColor(hueOf(lights.front().color)));
  setSpin(
    c.intensity, lights, [](const auto& light) { return intensityOf(light.color); });

  if (c.texturesStale)
  {
    updateTextureItems(*c.texture, map);
    updateFogMaterialItems(*c.fogMaterial, map);
    c.texturesStale = false;
  }
  const auto [texture, textureMulti] =
    firstValue(lights, [](const auto& light) { return light.texture; });
  c.texture->lineEdit()->setPlaceholderText(textureMulti ? Multi : QString{});
  if (textureMulti)
  {
    c.texture->setCurrentIndex(-1);
    c.texture->setEditText({});
  }
  else if (texture.empty())
  {
    c.texture->setCurrentIndex(0);
  }
  else
  {
    const auto text = QString::fromStdString(texture);
    c.texture->setCurrentIndex(c.texture->findText(text));
    c.texture->setEditText(text);
  }

  // the style choices of the first light's definition, else the engine's predefined
  // styles, copied: the definitions are freed when they are reloaded
  auto styleOptions = StyleOptions{};
  if (
    const auto* propertyDefinition =
      mdl::getPropertyDefinition(firstEntity.definition(), mdl::LightPropertyKeys::Style))
  {
    if (
      const auto* choice =
        std::get_if<mdl::PropertyValueTypes::Choice>(&propertyDefinition->valueType))
    {
      for (const auto& option : choice->options)
      {
        styleOptions.emplace_back(option.value, option.description);
      }
    }
  }
  if (styleOptions.empty())
  {
    styleOptions = predefinedStyleOptions();
  }
  if (styleOptions != c.styleOptions)
  {
    c.styleOptions = std::move(styleOptions);
    setStyleItems(*c.styleChoice, c.styleOptions);
  }

  const auto [style, styleMulti] =
    firstValue(lights, [](const auto& light) { return light.style; });
  c.styleChoice->setPlaceholderText(styleMulti ? Multi : QString::number(style));
  c.styleChoice->setCurrentIndex(styleMulti ? -1 : findStyleIndex(*c.styleChoice, style));

  // the page follows the lights: emitters, and emitters mixed with texture lights, on the
  // Emitter page, texture lights on the Texture page; the Fog page, which is no kind,
  // stays while the selection is the same (V7); the tab book's signals are not blocked,
  // because its tab bar follows them
  const auto mixedKinds = anyEmitter && !allEmitters;
  if (c.page != FogPageIndex || c.selectionChanged)
  {
    c.tabs->switchToPage(anyEmitter ? EmitterPageIndex : TexturePageIndex);
  }
  c.selectionChanged = false;
  c.emitterForm->setRowVisible(c.mixedEmitterRow, mixedKinds);
  c.texturePage->setEnabled(!mixedKinds);

  // the emitter fields, only for lights of one kind: lights of both kinds have the notice
  const auto oneKind = !mixedKinds;
  // a sun's light_radius is a box, not a reach: no falloff; size, start and auto range
  // are in the Volume section
  const auto isSun = !mixedTypes && type == mdl::LightType::Sun;
  const auto hasCurve = oneKind && !isSun;
  const auto patterned = anyLight(
    [](const auto& light) { return light.emitter.pattern != mdl::LightPattern::None; });
  const auto animated =
    anyLight([](const auto& light) { return light.emitter.effect != 0; });

  c.emitterForm->setRowVisible(c.falloffRow, hasCurve);
  c.emitterForm->setRowVisible(c.penumbraRow, oneKind && isSpot);
  c.emitterForm->setRowVisible(c.squarenessRow, oneKind && isSpot);
  c.emitterForm->setRowVisible(c.patternRow, oneKind);
  c.emitterForm->setRowVisible(c.patternStrengthRow, oneKind && patterned);
  c.emitterForm->setRowVisible(c.patternScaleRow, oneKind && patterned);
  c.emitterForm->setRowVisible(c.patternPhaseRow, oneKind && patterned);
  c.emitterForm->setRowVisible(c.energyClampRow, oneKind);
  c.emitterForm->setRowVisible(c.effectRow, oneKind);
  c.emitterForm->setRowVisible(c.effectSpeedRow, oneKind && animated);
  c.emitterForm->setRowVisible(c.effectAmountRow, oneKind && animated);
  c.emitterForm->setRowVisible(c.effectPhaseRow, oneKind && animated);

  setComboBoxData(
    c.falloff, lights, [](const auto& light) { return int(light.emitter.falloff); });
  setSpin(c.emitSize, lights, [](const auto& light) { return light.emitter.size; });
  setSpin(c.emitStart, lights, [](const auto& light) { return light.emitter.start; });
  setCheckBox(
    c.autoRange, lights, [](const auto& light) { return light.emitter.autoRange; });
  setSpin(
    c.range, lights, [](const auto& light) { return mdl::emitterRange(light.radius); });
  setSpin(c.coneH, lights, [](const auto& light) { return light.emitter.coneH; });
  setSpin(c.coneV, lights, [](const auto& light) { return light.emitter.coneV; });
  setSlider(c.penumbraSlider, c.penumbra, lights, [](const auto& light) {
    return light.emitter.penumbra;
  });
  setSlider(c.squarenessSlider, c.squareness, lights, [](const auto& light) {
    return light.emitter.squareness;
  });
  setComboBox(
    c.pattern, lights, [](const auto& light) { return int(light.emitter.pattern); });
  setSlider(c.patternStrengthSlider, c.patternStrength, lights, [](const auto& light) {
    return light.emitter.patternStrength;
  });
  setSlider(c.patternScaleSlider, c.patternScale, lights, [](const auto& light) {
    return light.emitter.patternScale;
  });
  setSlider(c.patternPhaseSlider, c.patternPhase, lights, [](const auto& light) {
    return light.emitter.patternPhase;
  });
  setCheckBox(
    c.energyClamp, lights, [](const auto& light) { return light.emitter.energyClamp; });
  setComboBoxData(
    c.effect, lights, [](const auto& light) { return light.emitter.effect; });
  setSpin(
    c.effectSpeed, lights, [](const auto& light) { return light.emitter.effectSpeed; });
  setSlider(c.effectAmountSlider, c.effectAmount, lights, [](const auto& light) {
    return light.emitter.effectAmount;
  });
  // the phase as the engine uses it, modulo 1
  setSpin(c.effectPhase, lights, [](const auto& light) {
    return light.emitter.effectPhase - std::floor(light.emitter.effectPhase);
  });

  // the Fog page (V7): the controls show the selected fog lights; the other lights get
  // the button that makes them fog lights. The density rows only where every fog light's
  // material takes its alpha from shaderParm3, as the engine's tab tests its stage 0
  auto fogLights = std::vector<mdl::LightProperties>{};
  auto densityFromLight = true;
  auto colourFromLight = true;
  for (const auto& light : lights)
  {
    if (const auto facts = lightShaderFactsOf(map, light); facts && facts->fogLight)
    {
      fogLights.push_back(light);
      densityFromLight = densityFromLight && facts->densityFromLight;
      colourFromLight = colourFromLight && facts->colourFromLight;
    }
  }
  const auto anyFog = !fogLights.empty();
  const auto allFog = fogLights.size() == lights.size();
  const auto parm3 = [](const auto& light) { return light.fog.distance.value_or(1.0); };
  const auto allFaint = anyFog && std::ranges::all_of(fogLights, [&](const auto& light) {
                          return mdl::fogIsFaint(parm3(light));
                        });
  const auto showDensity = anyFog && densityFromLight;

  const auto haveMaterials = materialsLoaded(map);
  c.fogNoticeText->setText(
    !haveMaterials ? tr("No materials are loaded: the editor cannot tell fog lights.")
    : anyFog
      ? tr(
          "Some of the selected lights are not fog lights: these controls change the "
          "fog lights only.")
      : tr("This light is not a fog light."));
  c.makeFogLight->setVisible(haveMaterials);
  c.fogForm->setRowVisible(c.fogNoticeRow, !allFog);
  for (const auto row :
       {c.fogStatusRow,
        c.fogMaterialRow,
        c.fogDensitySectionRow,
        c.fogMediumSectionRow,
        c.fogAlbedoRow,
        c.fogGlowRow,
        c.fogEdgeRow,
        c.fogColourTextRow,
        c.fogDrawnByRow})
  {
    c.fogForm->setRowVisible(row, anyFog);
  }
  c.fogForm->setRowVisible(c.ownFogDistanceRow, anyFog && !densityFromLight);
  c.fogForm->setRowVisible(c.fogDensityFormRow, showDensity);
  c.fogForm->setRowVisible(c.fogDensityRow, showDensity && !allFaint);
  c.fogForm->setRowVisible(c.fogDistanceRow, showDensity && !allFaint);
  c.fogForm->setRowVisible(c.fogDistanceTextRow, showDensity && !allFaint);
  c.fogForm->setRowVisible(c.fogStrengthRow, showDensity && allFaint);

  if (anyFog)
  {
    const auto [fogTexture, fogTextureMulti] =
      firstValue(fogLights, [](const auto& light) { return light.texture; });
    // a fog light whose material lies outside lights/ and fogs/ shows it, as the style
    // combo shows a style that is not in its list
    c.fogMaterial->setPlaceholderText(
      fogTextureMulti ? Multi : QString::fromStdString(fogTexture));
    c.fogMaterial->setCurrentIndex(
      fogTextureMulti ? -1
                      : c.fogMaterial->findText(
                          QString::fromStdString(fogTexture), Qt::MatchFixedString));

    // a distance at most 1 is no distance: the engine's DEFAULT_FOG_DISTANCE
    const auto distanceOf = [&](const auto& light) {
      const auto value = parm3(light);
      return value > 1.0 ? value : mdl::DefaultFogDistance;
    };
    setComboBox(c.fogDensityForm, fogLights, [&](const auto& light) {
      return mdl::fogIsFaint(parm3(light)) ? 1 : 0;
    });
    setSlider(c.fogDensitySlider, c.fogDensity, fogLights, [&](const auto& light) {
      return mdl::fogDistanceToDensity(distanceOf(light));
    });
    setSpin(c.fogDistance, fogLights, distanceOf);
    setSlider(c.fogStrengthSlider, c.fogStrength, fogLights, [&](const auto& light) {
      return std::max(parm3(light), 0.0);
    });
    setSlider(c.fogAlbedoSlider, c.fogAlbedo, fogLights, [](const auto& light) {
      return light.fog.albedo;
    });
    setSlider(c.fogGlowSlider, c.fogGlow, fogLights, [](const auto& light) {
      return light.fog.emission;
    });
    setSlider(c.fogEdgeSlider, c.fogEdge, fogLights, [](const auto& light) {
      return light.fog.edge;
    });

    // what the volumetric fog leaves of the light behind the fog, at the engine's rate,
    // in the engine's words; the line speaks of one distance, so it hides while the fog
    // lights' distances differ
    const auto [distance, distanceMulti] = firstValue(fogLights, distanceOf);
    c.fogForm->setRowVisible(
      c.fogDistanceTextRow, showDensity && !allFaint && !distanceMulti);
    const auto left = 100.0 * std::exp(-mdl::FogLightRate);
    c.fogDistanceText->setText(
      tr(
        "Classic fog: opaque at %1 units. Volumetric fog: %2 % of the light is left "
        "after %1 units, %3 % after %4.")
        .arg(distance, 0, 'f', 0)
        .arg(left, 0, 'f', 1)
        .arg(left * left / 100.0, 0, 'f', 2)
        .arg(2.0 * distance, 0, 'f', 0));
    c.fogColourText->setText(
      colourFromLight ? tr("The colour tints the glow only. Set it under Color above.")
                      : tr("This material ignores the light's colour."));
  }

  // a fog light that says volumetric 1 shows it as a third item of Drawn by, kept until
  // another is chosen (the engine's review CN3-6)
  const auto anyAlways = std::ranges::any_of(
    fogLights, [](const auto& light) { return light.fog.volumetric == 1; });
  if (anyAlways && c.fogDrawnBy->count() == 2)
  {
    c.fogDrawnBy->addItem(tr("Always ( as Automatic for a fog light )"), 1);
  }
  else if (!anyAlways && c.fogDrawnBy->count() == 3)
  {
    c.fogDrawnBy->removeItem(2);
  }
  if (anyFog)
  {
    setComboBoxData(
      c.fogDrawnBy, fogLights, [](const auto& light) { return light.fog.volumetric; });
  }

  // misc; the light in the fog while no selected light is a fog light
  c.miscForm->setRowVisible(c.volumetricSectionRow, !anyFog);
  c.miscForm->setRowVisible(c.lightInTheFogRow, !anyFog);
  setComboBox(
    c.lightInTheFog, lights, [](const auto& light) { return light.fog.volumetric; });
  setCheckBox(c.castShadows, lights, [](const auto& light) { return light.castShadows; });
  setCheckBox(
    c.skipSpecular, lights, [](const auto& light) { return light.skipSpecular; });

  updateVolumeToolButtons();
}

void LightEditorPanel::updateVolumeToolButtons()
{
  auto& c = *m_controls;
  const auto& tool = m_toolBox.lightVolumeTool();
  const auto active = tool.active();

  // the tool draws for exactly one light without light_origin; while it is active, the
  // button can always turn it off
  c.drawVolumeButton->setEnabled(active || tool.volumeBox().has_value());
  c.drawVolumeButton->setChecked(active && tool.mode() == LightVolumeTool::Mode::Draw);
}

void LightEditorPanel::toggleVolumeDrawing()
{
  toggleLightVolumeDrawing(m_toolBox);
  updateVolumeToolButtons();
}

void LightEditorPanel::editLights(
  const std::string& commandName, const std::function<void(mdl::LightProperties&)>& edit)
{
  editLightEntities(
    commandName, [&](const mdl::Entity&, mdl::LightProperties& light) { edit(light); });
}

void LightEditorPanel::editLightEntities(
  const std::string& commandName,
  const std::function<void(const mdl::Entity&, mdl::LightProperties&)>& edit)
{
  editOrientedLights(
    commandName, [&](const mdl::Entity& entity, mdl::LightProperties& light) {
      edit(entity, light);
      return std::optional<vm::mat3x3d>{};
    });
}

void LightEditorPanel::editOrientedLights(
  const std::string& commandName,
  const std::function<
    std::optional<vm::mat3x3d>(const mdl::Entity&, mdl::LightProperties&)>& edit)
{
  auto& map = m_document.map();

  // per light, only the properties that the edit changes (editedLightEntity)
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : editableLightNodes(map))
  {
    const auto& entity = entityNode->entity();
    auto light = mdl::parseLightProperties(entity);
    const auto rotation = edit(entity, light);
    if (auto changed = editedLightEntity(entity, light, rotation))
    {
      nodesToSwap.emplace_back(entityNode, mdl::NodeContents{std::move(*changed)});
    }
  }

  if (!swapNodeContents(map, commandName, std::move(nodesToSwap)))
  {
    // the control that the user changed may show a value that no light has
    refresh();
  }
}

void LightEditorPanel::switchLightKind(const int pageIndex)
{
  // the Fog page is no kind of light: its tab writes nothing (V7)
  if (pageIndex == FogPageIndex)
  {
    return;
  }

  auto& map = m_document.map();
  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

  if (pageIndex == TexturePageIndex)
  {
    editLights("Set Light Kind", [](mdl::LightProperties& light) {
      // the emit_* keys stay; a spot gets the pyramid round its cone with its rotation
      // unchanged, an omni or a sun keeps its volume keys, which it shares
      light = mdl::asTextureLight(light);
    });
    return;
  }

  // to emitters, in one command: a texture light gets its kept block or a new one
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : editableLightNodes(map))
  {
    const auto& entity = entityNode->entity();
    auto light = mdl::parseLightProperties(entity);
    if (mdl::lightUsesEmitter(light))
    {
      continue;
    }

    auto [emitter, rotation] = mdl::asEmitterLight(entity, light, policy);
    if (auto changed = editedLightEntity(entity, emitter, rotation))
    {
      nodesToSwap.emplace_back(entityNode, mdl::NodeContents{std::move(*changed)});
    }
  }

  if (!swapNodeContents(map, "Set Light Kind", std::move(nodesToSwap)))
  {
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
  for (auto* entityNode : editableLightNodes(map))
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

void LightEditorPanel::removeTargetProperties(
  const std::string& commandName,
  const std::function<bool(const mdl::Entity&, const mdl::LightProperties&)>& applies)
{
  auto& map = m_document.map();

  // not a light edit: the light that the engine reads does not change until the keys
  // are gone, so a light diff would keep them
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : editableLightNodes(map))
  {
    auto entity = entityNode->entity();
    if (applies(entity, mdl::parseLightProperties(entity)))
    {
      for (const auto* key :
           {&mdl::LightPropertyKeys::LightTarget,
            &mdl::LightPropertyKeys::LightRight,
            &mdl::LightPropertyKeys::LightUp})
      {
        if (entity.hasProperty(*key))
        {
          entity.removeProperty(*key);
        }
      }
      nodesToSwap.emplace_back(entityNode, mdl::NodeContents{std::move(entity)});
    }
  }

  if (!swapNodeContents(map, commandName, std::move(nodesToSwap)))
  {
    refresh();
  }
}

void LightEditorPanel::convertLightOrigins()
{
  auto& map = m_document.map();

  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : selectedLightNodes(map))
  {
    auto entity = entityNode->entity();
    if (const auto* lightOrigin = entity.property(mdl::LightPropertyKeys::LightOrigin))
    {
      const auto origin = *lightOrigin;

      // EntityRotationKeyValidator's "Convert to Angles": the rotation written unchanged
      // replaces light_rotation, rotation and light_angles by "angles"
      if (!mdl::idTech4ConvertedRotationKeys(entity).empty())
      {
        mdl::applyEntityRotationUpdate(
          entity,
          mdl::applyEntityRotation(
            entity, vm::mat4x4d::identity(), mdl::EntityRotationPolicy::IdTech4));
      }

      entity.addOrUpdateProperty(mdl::EntityPropertyKeys::Origin, origin);
      entity.removeProperty(mdl::LightPropertyKeys::LightOrigin);
      nodesToSwap.emplace_back(entityNode, mdl::NodeContents{std::move(entity)});
    }
  }

  if (!swapNodeContents(map, "Convert Light Origin", std::move(nodesToSwap)))
  {
    refresh();
  }
}

} // namespace tb::ui
