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
// the slider ranges of the in-game editor (LightEditor.cpp); written with four decimals
constexpr auto FalloffExpRange = SpinRange{0.1, 16.0, 4, 0.1, 1.0, 2.0};
constexpr auto InnerRadiusRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto ShapeExpRange = SpinRange{1.5, 32.0, 4, 0.1, 1.0, 4.0};
constexpr auto PenumbraRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto PatternStrengthRange = SpinRange{0.0, 1.0, 4, 0.01, 0.1, 0.25};
constexpr auto PatternScaleRange = SpinRange{0.1, 64.0, 4, 0.1, 1.0, 4.0};
constexpr auto PatternPhaseRange = SpinRange{0.0, 100.0, 4, 0.1, 1.0, 10.0};
// the light volume handle threshold in percent, 0.01 to 0.5 (D53)
constexpr auto HandleThresholdRange = SpinRange{1.0, 50.0, 0, 1.0, 5.0, 10.0};
// how long the threshold's steps must pause before it is written
constexpr auto HandleThresholdWriteDelay = std::chrono::milliseconds{200};

constexpr auto SliderSteps = 1000;

// the pages of the two kinds of light (D50)
constexpr auto MathPageIndex = 0;
constexpr auto TexturePageIndex = 1;

const auto Multi = QString{"multi"};

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
  QSlider*& slider, MultiSpinControl*& spin, const QString& name, const SpinRange& range)
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
void fitTabPages(const std::array<TabBookPage*, 2>& pages, const int currentIndex)
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

int addRow(FormWithSectionsLayout& form, QWidget* field)
{
  const auto row = form.rowCount();
  form.addRow(field);
  return row;
}

// a slider spans the range of its spin box's values, never the "multi" below it
double sliderValue(const MultiSpinControl& spin, const int position)
{
  const auto minimum = spin.rangeMinimum();
  const auto value = minimum + (spin.maximum() - minimum) * position / SliderSteps;
  const auto scale = std::pow(10.0, spin.decimals());
  return std::round(value * scale) / scale;
}

int sliderPosition(const MultiSpinControl& spin, const double value)
{
  const auto minimum = spin.rangeMinimum();
  const auto t =
    (std::clamp(value, minimum, spin.maximum()) - minimum) / (spin.maximum() - minimum);
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
 * Resets the light's volume to the engine's default for its type and keeps everything
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
}

/**
 * Whether the given sun still has all three spot vectors. With them, the engine reads
 * neither light_radius nor light_center (d3xp/Light.cpp), so the sun has a radius of 0
 * in the game until they are removed.
 */
bool hasStaleSpotKeys(const mdl::Entity& entity, const mdl::LightProperties& light)
{
  return light.type == mdl::LightType::Sun
         && entity.hasProperty(mdl::LightPropertyKeys::LightTarget)
         && entity.hasProperty(mdl::LightPropertyKeys::LightRight)
         && entity.hasProperty(mdl::LightPropertyKeys::LightUp);
}

/**
 * How far the far rectangle of an off-centre spot light moves when the first box edit
 * recentres it: the largest distance between one of its far corners and the same corner
 * of the light that its own edit box gives. Nothing for a centred or degenerate spot,
 * or for one whose own edit box is refused.
 */
std::optional<double> recentreDistance(const mdl::LightProperties& light)
{
  if (light.type != mdl::LightType::Spot || mdl::isCentredSpot(light))
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
  slider->setValue(sliderPosition(*spin, value));
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

  QWidget* editor = nullptr;
  // the light's geometry and colour
  FormWithSectionsLayout* form = nullptr;
  // the pages of math lights and texture lights, each with its own form (D50)
  TabBook* tabs = nullptr;
  TabBookPage* mathPage = nullptr;
  TabBookPage* texturePage = nullptr;
  FormWithSectionsLayout* mathForm = nullptr;

  // every control that the user changes, blocked while the controls are set
  std::vector<QObject*> inputs;

  QButtonGroup* type = nullptr;
  std::array<QRadioButton*, 3> typeRadios = {};

  QCheckBox* equalRadius = nullptr;
  // the session's choice of one radius for lights whose three radii are equal, off at
  // first, so that a light shows its three radii (D52)
  bool equilateral = false;
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

  QLabel* mathType = nullptr;
  QComboBox* mathStyle = nullptr;
  QComboBox* falloff = nullptr;
  QSlider* falloffExpSlider = nullptr;
  MultiSpinControl* falloffExp = nullptr;
  QSlider* innerRadiusSlider = nullptr;
  MultiSpinControl* innerRadius = nullptr;
  QSlider* shapeExpSlider = nullptr;
  MultiSpinControl* shapeExp = nullptr;
  QSlider* penumbraSlider = nullptr;
  MultiSpinControl* penumbra = nullptr;
  QComboBox* pattern = nullptr;
  QSlider* patternStrengthSlider = nullptr;
  MultiSpinControl* patternStrength = nullptr;
  QSlider* patternScaleSlider = nullptr;
  MultiSpinControl* patternScale = nullptr;
  QSlider* patternPhaseSlider = nullptr;
  MultiSpinControl* patternPhase = nullptr;
  QCheckBox* energyClamp = nullptr;

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
  // the rows of mathForm
  int mixedMathRow = -1;
  int mathTypeRow = -1;
  int mathStyleRow = -1;
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

  // colour
  form->addSection(tr("Color"));
  c.color = new ColorButton{};
  c.color->setObjectName(controlName("Color"));
  form->addRow(tr("Color"), c.color);
  c.intensity = createSpin("Intensity", IntensityRange);
  c.intensity->setToolTip(tr("The brightest component of the color"));
  form->addRow(tr("Intensity"), c.intensity);

  // the math light page: the math block of the engine's math lights
  auto* mathForm = createForm();
  c.mathForm = mathForm;
  auto* mixedMathText = createNoticeLabel(
    "MixedMathNotice",
    tr("The selected lights are math and texture lights: choose a tab to make all of "
       "them lights of its kind."));
  mixedMathText->setWordWrap(true);
  setInfoStyle(mixedMathText);
  c.mixedMathRow = addRow(*mathForm, mixedMathText);
  c.mathType = new QLabel{};
  c.mathType->setObjectName(controlName("MathType"));
  c.mathTypeRow = addRow(*mathForm, tr("Type"), c.mathType);
  // the engine's predefined styles, which change the light's colour (D54)
  c.mathStyle = createComboBox("MathStyle", {});
  setStyleItems(*c.mathStyle, predefinedStyleOptions());
  c.mathStyle->setToolTip(tr("Varies the light's brightness over time like a style"));
  c.mathStyleRow = addRow(*mathForm, tr("Style"), c.mathStyle);
  c.falloff = createComboBox(
    "Falloff",
    {
      tr("Inverse Square (unbounded)"),
      tr("Linear"),
      tr("Smooth"),
      tr("Exponential"),
      tr("Inverse Square (clamped) - PBR safe"),
    });
  c.falloffRow = addRow(*mathForm, tr("Curve"), c.falloff);
  c.falloffExpRow = addRow(
    *mathForm,
    tr("Falloff exponent"),
    createSliderLayout(c.falloffExpSlider, c.falloffExp, "FalloffExp", FalloffExpRange));
  c.innerRadiusRow = addRow(
    *mathForm,
    tr("Inner radius"),
    createSliderLayout(
      c.innerRadiusSlider, c.innerRadius, "InnerRadius", InnerRadiusRange));
  c.shapeExpRow = addRow(
    *mathForm,
    tr("Shape"),
    createSliderLayout(c.shapeExpSlider, c.shapeExp, "ShapeExp", ShapeExpRange));
  c.shapeExp->setToolTip(tr("2 is an ellipse, 16 a rectangle"));
  c.penumbraRow = addRow(
    *mathForm,
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
  c.patternRow = addRow(*mathForm, tr("Pattern"), c.pattern);
  c.patternStrengthRow = addRow(
    *mathForm,
    tr("Strength"),
    createSliderLayout(
      c.patternStrengthSlider,
      c.patternStrength,
      "PatternStrength",
      PatternStrengthRange));
  c.patternScaleRow = addRow(
    *mathForm,
    tr("Scale"),
    createSliderLayout(
      c.patternScaleSlider, c.patternScale, "PatternScale", PatternScaleRange));
  c.patternPhaseRow = addRow(
    *mathForm,
    tr("Phase"),
    createSliderLayout(
      c.patternPhaseSlider, c.patternPhase, "PatternPhase", PatternPhaseRange));
  c.energyClamp = createCheckBox("EnergyClamp", tr("Energy clamp (PBR safe)"));
  c.energyClampRow = addRow(*mathForm, c.energyClamp);

  // the texture light page: the light material and the engine's style
  auto* textureForm = createForm();
  c.texture = createComboBox("Texture", {EngineDefaultTexture});
  c.texture->setEditable(true);
  c.texture->setInsertPolicy(QComboBox::NoInsert);
  c.texture->setItemData(0, "lights/squarelight1", Qt::ToolTipRole);
  textureForm->addRow(tr("Texture"), c.texture);
  // the choices of the entity definition, else the engine's predefined styles
  c.styleChoice = createComboBox("StyleChoice", {});
  textureForm->addRow(tr("Style"), c.styleChoice);

  c.mathPage = createTabPage("MathPage", mathForm);
  c.texturePage = createTabPage("TexturePage", textureForm);
  c.tabs = new TabBook{};
  c.tabs->setObjectName(controlName("Tabs"));
  c.tabs->addPage(c.mathPage, tr("Math"));
  c.tabs->addPage(c.texturePage, tr("Texture"));
  fitTabPages({c.mathPage, c.texturePage}, MathPageIndex);

  // misc
  auto* miscForm = createForm();
  miscForm->addSection(tr("Misc"));
  c.castShadows = createCheckBox("CastShadows", tr("Cast shadows"));
  miscForm->addRow(c.castShadows);
  c.skipSpecular = createCheckBox("SkipSpecular", tr("Skip specular"));
  miscForm->addRow(c.skipSpecular);

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
    c.mathStyle,
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
        const auto value = sliderValue(*spin, position);
        editLights(commandName, [&](mdl::LightProperties& light) { set(light, value); });
      };
      // the position at which the handle was pressed
      const auto pressedPosition = std::make_shared<int>(slider->value());
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
    auto& controls = *m_controls;
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
      });
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
  connect(c.styleChoice, &QComboBox::activated, this, [this](const int index) {
    auto ok = false;
    const auto style = m_controls->styleChoice->itemData(index).toString().toInt(&ok);
    if (ok)
    {
      editLights(
        "Set Light Style", [&](mdl::LightProperties& light) { light.style = style; });
    }
  });

  // the tab is the switch between the two kinds of light (D51): a click on it writes, the
  // page change of a refresh does not
  connect(c.tabs->tabBar(), &TabBar::tabClicked, this, [this](const int index) {
    editLightEntities(
      "Set Light Math", [&](const mdl::Entity& entity, mdl::LightProperties& light) {
        if (index == TexturePageIndex)
        {
          // the other math keys stay
          light.math.enabled = false;
        }
        else if (!light.math.enabled)
        {
          if (mdl::hasMathLightBlock(entity))
          {
            // a disabled block gets its kept values back
            light.math.enabled = true;
          }
          else
          {
            // a light without a math block of its own gets the in-game editor's seed
            // (D29), as its Math tab gives it
            light.math = mdl::seededMathLightProperties();
          }
        }
      });
  });
  connect(c.mathStyle, &QComboBox::activated, this, [this](const int index) {
    auto ok = false;
    const auto style = m_controls->mathStyle->itemData(index).toString().toInt(&ok);
    if (ok)
    {
      editLights("Set Light Math Style", [&](mdl::LightProperties& light) {
        light.math.style = style;
      });
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
    fitTabPages({m_controls->mathPage, m_controls->texturePage}, index);
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
  m_notifierConnection +=
    m_document.selectionDidChangeNotifier.connect([&](const auto&) { refresh(); });
  m_notifierConnection +=
    m_document.entityDefinitionsDidChangeNotifier.connect([&] { refresh(); });

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

  c.offCentreNotice->setVisible(recentre.has_value());
  if (recentre)
  {
    c.offCentreText->setText(
      tr("Off-centre projection: the first box edit recentres it (the far rectangle "
         "moves by up to %1 units)")
        .arg(std::ceil(*recentre)));
  }

  c.styledNotice->setVisible(
    anyLight([](const auto& light) { return light.style != -1; }));

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
  // one radius only when the user chose it and every light has one (D52)
  const auto equilateral =
    c.equilateral && allLights([](const auto& light) { return light.equalRadius; });
  const auto hasCenter = allLights([](const auto& light) { return light.hasCenter; });
  const auto explicitStartEnd =
    allLights([](const auto& light) { return light.explicitStartEnd; });

  c.form->setRowVisible(c.mixedTypesRow, mixedTypes);
  c.form->setRowVisible(c.equalRadiusRow, isPointOrSun);
  c.form->setRowVisible(c.radiusRow, isPointOrSun && equilateral);
  c.form->setRowVisible(c.radiusXyzRow, isPointOrSun && !equilateral);
  c.form->setRowVisible(c.centerRow, isPointOrSun);
  c.form->setRowVisible(c.centerXyzRow, isPointOrSun && hasCenter);
  c.form->setRowVisible(c.targetRow, isSpot);
  c.form->setRowVisible(c.rightRow, isSpot);
  c.form->setRowVisible(c.upRow, isSpot);
  c.form->setRowVisible(c.explicitStartEndRow, isSpot);
  c.form->setRowVisible(c.startRow, isSpot && explicitStartEnd);
  c.form->setRowVisible(c.endRow, isSpot && explicitStartEnd);

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

  // the page follows the lights: math lights, and math lights mixed with texture lights,
  // on the Math page, texture lights on the Texture page; the tab book's signals are not
  // blocked, because its tab bar follows them
  const auto mathEnabled =
    allLights([](const auto& light) { return light.math.enabled; });
  const auto anyMathEnabled =
    anyLight([](const auto& light) { return light.math.enabled; });
  const auto mixedMath = anyMathEnabled && !mathEnabled;
  c.tabs->switchToPage(anyMathEnabled ? MathPageIndex : TexturePageIndex);
  c.mathForm->setRowVisible(c.mixedMathRow, mixedMath);
  c.texturePage->setEnabled(!mixedMath);

  // the math fields, only for lights of one kind: lights of both kinds have the notice
  const auto oneKind = !mixedMath;
  const auto patterned = anyLight(
    [](const auto& light) { return light.math.pattern != mdl::LightPattern::None; });

  c.mathForm->setRowVisible(c.mathTypeRow, oneKind);
  c.mathForm->setRowVisible(c.mathStyleRow, oneKind);
  c.mathForm->setRowVisible(c.falloffRow, oneKind);
  c.mathForm->setRowVisible(c.falloffExpRow, oneKind);
  c.mathForm->setRowVisible(c.innerRadiusRow, oneKind);
  c.mathForm->setRowVisible(c.shapeExpRow, oneKind && isSpot);
  c.mathForm->setRowVisible(c.penumbraRow, oneKind && isSpot);
  c.mathForm->setRowVisible(c.patternRow, oneKind);
  c.mathForm->setRowVisible(c.patternStrengthRow, oneKind && patterned);
  c.mathForm->setRowVisible(c.patternScaleRow, oneKind && patterned);
  c.mathForm->setRowVisible(c.patternPhaseRow, oneKind && patterned);
  c.mathForm->setRowVisible(c.energyClampRow, oneKind);

  // the engine derives the math type from the light's geometry (D40)
  c.mathType->setText(
    mixedTypes ? Multi
               : tr("%1 (follows the light type)")
                   .arg(QString::fromStdString(std::string{mdl::mathTypeName(type)})));
  const auto [mathStyle, mathStyleMulti] =
    firstValue(lights, [](const auto& light) { return light.math.style; });
  c.mathStyle->setPlaceholderText(Multi);
  c.mathStyle->setCurrentIndex(
    mathStyleMulti ? -1 : findStyleIndex(*c.mathStyle, mathStyle));
  setComboBox(
    c.falloff, lights, [](const auto& light) { return int(light.math.falloff); });
  const auto falloffExpUsed = anyLight([](const auto& light) {
    return light.math.falloff != mdl::LightFalloff::Linear
           && light.math.falloff != mdl::LightFalloff::Smooth;
  });
  c.falloffExp->setEnabled(falloffExpUsed);
  c.falloffExpSlider->setEnabled(falloffExpUsed);
  c.falloffExp->setToolTip(falloffExpUsed ? QString{} : tr("Unused by this curve"));
  setSlider(c.falloffExpSlider, c.falloffExp, lights, [](const auto& light) {
    return light.math.falloffExp;
  });
  setSlider(c.innerRadiusSlider, c.innerRadius, lights, [](const auto& light) {
    return light.math.innerRadius;
  });
  setSlider(c.shapeExpSlider, c.shapeExp, lights, [](const auto& light) {
    return light.math.shapeExp;
  });
  setSlider(c.penumbraSlider, c.penumbra, lights, [](const auto& light) {
    return light.math.penumbra;
  });
  setComboBox(
    c.pattern, lights, [](const auto& light) { return int(light.math.pattern); });
  setSlider(c.patternStrengthSlider, c.patternStrength, lights, [](const auto& light) {
    return light.math.patternStrength;
  });
  setSlider(c.patternScaleSlider, c.patternScale, lights, [](const auto& light) {
    return light.math.patternScale;
  });
  setSlider(c.patternPhaseSlider, c.patternPhase, lights, [](const auto& light) {
    return light.math.patternPhase;
  });
  setCheckBox(
    c.energyClamp, lights, [](const auto& light) { return light.math.energyClamp; });

  // misc
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
  auto& map = m_document.map();

  // per light, only the properties that the edit changes, so that the lights keep their
  // other values and a value that is written unchanged is not rounded (D42)
  auto nodesToSwap = NodesToSwap{};
  for (auto* entityNode : editableLightNodes(map))
  {
    const auto& entity = entityNode->entity();
    auto light = mdl::parseLightProperties(entity);
    edit(entity, light);

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
