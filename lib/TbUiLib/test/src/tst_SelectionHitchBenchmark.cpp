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

/*
 * A hidden benchmark of what a selection change and a resize of the sidebar docks cost
 * in a real MapWindow with a real game and map (package 15-hitch). It is meant to be run
 * in Release and in Debug, before and after a fix:
 *
 * - S1 select a light the way a click does (one transaction: deselectAll, selectNodes),
 *   from an empty selection;
 * - S2 deselect that light (deselectAll, as a click into the void does) while the Light
 *   Editor shows it and one of its spin boxes has the focus;
 * - S3 select a brush, S4 select a different light, S5 select an entity that is not a
 *   light, each from the light of S1;
 * - R1 the right dock area's width +40 px per step (10 steps), then -40 px (10 steps),
 *   with QMainWindow::AnimatedDocks on and off;
 * - R2 a switch of the inspector tab Map -> Entity -> Face -> Map.
 *
 * Each operation runs in the layout variants L0 (every panel shown, the Light Editor
 * too, with half of the right column), L1 (the Map, Entity and Face inspectors closed),
 * L2 (the Light Editor closed), L3 (every panel closed) and L4 (Console and Issues
 * closed). "headless" times the selection calls of S1 to S5 on a second document that no
 * window observes.
 *
 * An operation is timed from its call to the end of the last event that the event loop
 * delivered before it went quiet (30 ms without an event other than a periodic timer)
 * with every visible map view painted after the call ("settle"). A map view has painted
 * when it got a paint event (and, on screen, a frameSwapped signal). "sync" is the call
 * itself; "busy" is sync plus the time spent in the events delivered until it settled;
 * "late_busy" is the time spent in events until 700 ms after the call (the 500 ms
 * delayed status bar and issue updates). Every event that the event loop delivers is
 * timed by an application event filter that sends it again and consumes the original,
 * and so are the paint and resize events of every QOpenGLWidget at any depth, the raster
 * paint events of every widget (by dock or window child, marked when the widget lies
 * outside the window) and the resize events of the window's children (a dock's includes
 * the layout of its contents). The totals per sample are printed as BENCHTOP lines.
 *
 * Every test case is hidden and skips unless these environment variables are set:
 *   TB_BENCH_GAME_PATH    the game path (the folder that contains base/)
 *   TB_BENCH_GAME_CONFIG  the game's GameConfig.cfg in the resources folder
 *                         (resources/games/<game>/GameConfig.cfg); resources/defaults is
 *                         mounted as the app does, and resources/stylesheets/base.qss
 *                         is applied as the app does
 *   TB_BENCH_MAP          the map to load (a copy: nothing is saved, but autosave could)
 *   TB_BENCH_REPEAT       optional: the number of samples per operation (default 20)
 *   TB_BENCH_LAYOUTS      optional: the variants to run, e.g. "L0,L2" (default all)
 *   TB_BENCH_OPS          optional: the operations to run, e.g. "S1,S2,R1" (default all)
 *   TB_BENCH_ONSCREEN     optional: 1 shows a normal window on the desktop instead of
 *                         one with Qt::WA_DontShowOnScreen
 *   TB_BENCH_STYLE        optional: 0 skips the Fusion style and the style sheet
 *
 * The map views are QOpenGLWidgets; the benchmark sets the app's default surface format
 * and Qt::AA_ShareOpenGLContexts before the first GL widget is made, as the app's main
 * does before it creates the application.
 */

#include <QAbstractAnimation>
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QMetaEnum>
#include <QOpenGLWidget>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTest>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>

#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/PathInfo.h"
#include "fs/Reader.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "gl/ResourceManager.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/AppController.h"
#include "ui/Console.h"
#include "ui/MapDocument.h"
#include "ui/MapViewBase.h"
#include "ui/MapWindow.h"
#include "ui/RenderView.h"

#include "kd/invoke.h"
#include "kd/result.h"
#include "kd/string_utils.h"
#include "kd/task_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double millisecondsBetween(const Clock::time_point start, const Clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double millisecondsSince(const Clock::time_point start)
{
  return millisecondsBetween(start, Clock::now());
}

std::optional<std::string> environmentVariable(const char* name)
{
#ifdef _WIN32
  char* buffer = nullptr;
  auto length = size_t{0};
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
  {
    return std::nullopt;
  }
  auto value = std::string{buffer};
  std::free(buffer);
#else
  const auto* buffer = std::getenv(name);
  if (buffer == nullptr)
  {
    return std::nullopt;
  }
  auto value = std::string{buffer};
#endif
  return value.empty() ? std::nullopt : std::optional{std::move(value)};
}

std::optional<size_t> environmentNumber(const char* name)
{
  if (const auto str = environmentVariable(name))
  {
    return kdl::str_to_size(*str);
  }
  return std::nullopt;
}

/** Whether the comma separated list in the variable contains the item, or no list. */
bool environmentListContains(const char* name, const std::string& item)
{
  if (const auto str = environmentVariable(name))
  {
    const auto items = kdl::str_split(*str, ",");
    return std::ranges::find(items, item) != items.end();
  }
  return true;
}

std::string formatValue(const double value)
{
  auto str = std::ostringstream{};
  str << std::fixed << std::setprecision(2) << value;
  return str.str();
}

void report(const std::string& name, const double value, const std::string_view unit)
{
  std::cout << "BENCH " << name << " " << formatValue(value) << " " << unit << std::endl;
}

void reportCount(const std::string& name, const size_t value, const std::string_view unit)
{
  std::cout << "BENCH " << name << " " << value << " " << unit << std::endl;
}

void reportNote(const std::string& note)
{
  std::cout << "BENCHNOTE " << note << std::endl;
}

/** The values of one quantity over the samples of an operation. */
class Samples
{
private:
  std::string m_name;
  std::string m_unit;
  std::vector<double> m_values;

public:
  Samples(std::string name, std::string unit)
    : m_name{std::move(name)}
    , m_unit{std::move(unit)}
  {
  }

  void add(const double value) { m_values.push_back(value); }

  /** Prints the median, the minimum and the maximum. */
  void print() const
  {
    if (m_values.empty())
    {
      return;
    }

    auto sorted = m_values;
    std::ranges::sort(sorted);
    const auto n = sorted.size();
    const auto median =
      n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;

    report(m_name + ".median", median, m_unit);
    report(m_name + ".min", sorted.front(), m_unit);
    report(m_name + ".max", sorted.back(), m_unit);
  }
};

/** A set of named samples that prints them in the order they were first added. */
class SampleTable
{
private:
  std::vector<Samples> m_samples;
  std::map<std::string, size_t> m_index;

public:
  void add(const std::string& name, const double value, const std::string& unit = "ms")
  {
    auto it = m_index.find(name);
    if (it == m_index.end())
    {
      it = m_index.emplace(name, m_samples.size()).first;
      m_samples.emplace_back(name, unit);
    }
    m_samples[it->second].add(value);
  }

  void print() const
  {
    for (const auto& samples : m_samples)
    {
      samples.print();
    }
  }
};

struct Bucket
{
  double milliseconds = 0.0;
  size_t count = 0;
};

using Buckets = std::map<std::string, Bucket>;

void addBuckets(Buckets& to, const Buckets& from)
{
  for (const auto& [name, bucket] : from)
  {
    auto& toBucket = to[name];
    toBucket.milliseconds += bucket.milliseconds;
    toBucket.count += bucket.count;
  }
}

/** Prints the buckets with the largest totals, divided by the number of samples. */
void reportTopBuckets(
  const std::string& prefix,
  const Buckets& buckets,
  const size_t samples,
  const size_t limit = 14)
{
  auto sorted =
    std::vector<std::pair<std::string, Bucket>>{buckets.begin(), buckets.end()};
  std::ranges::sort(sorted, [](const auto& lhs, const auto& rhs) {
    return lhs.second.milliseconds > rhs.second.milliseconds;
  });
  const auto divisor = static_cast<double>(std::max(samples, size_t{1}));
  for (size_t i = 0; i < std::min(limit, sorted.size()); ++i)
  {
    const auto& [name, bucket] = sorted[i];
    std::cout << "BENCHTOP " << prefix << " "
              << formatValue(bucket.milliseconds / divisor) << " ms "
              << formatValue(static_cast<double>(bucket.count) / divisor) << " events "
              << name << std::endl;
  }
}

std::string describe(const QObject* object)
{
  auto result = std::string{object->metaObject()->className()};
  if (const auto name = object->objectName(); !name.isEmpty())
  {
    result += "#" + name.toStdString();
  }
  return result;
}

/**
 * The receiver and the objects that tell where it is: for a timer, its owners, and for
 * a widget, the nearest TrenchBroom widget and the nearest dock above it.
 */
std::string describeReceiver(const QObject* receiver)
{
  auto result = describe(receiver);
  if (const auto* timer = qobject_cast<const QTimer*>(receiver))
  {
    result += "(" + std::to_string(timer->interval()) + "ms)";
    auto* parent = receiver->parent();
    for (size_t i = 0; parent && i < 2; ++i, parent = parent->parent())
    {
      result += " < " + describe(parent);
    }
    return result;
  }

  const auto isTb = [](const QObject* object) {
    return std::string_view{object->metaObject()->className()}.starts_with("tb::");
  };

  auto* parent = receiver->parent();
  if (!isTb(receiver))
  {
    while (parent && !isTb(parent))
    {
      parent = parent->parent();
    }
    if (parent)
    {
      result += " < " + describe(parent);
    }
  }
  while (parent && !qobject_cast<const QDockWidget*>(parent))
  {
    parent = parent->parent();
  }
  if (parent)
  {
    result += " @ " + parent->objectName().toStdString();
  }
  return result;
}

/** Whether no part of the widget lies inside its window. */
bool isOutsideWindow(const QObject* object)
{
  const auto* widget = static_cast<const QWidget*>(object);
  const auto* window = widget->window();
  return widget != window
         && !QRect{widget->mapTo(window, QPoint{0, 0}), widget->size()}.intersects(
           window->rect());
}

/** The dock that holds the object, or the child of its window that does. */
std::string describeArea(const QObject* object)
{
  for (auto* parent = object; parent; parent = parent->parent())
  {
    if (qobject_cast<const QDockWidget*>(parent))
    {
      return "@ " + parent->objectName().toStdString();
    }
    const auto* widget = qobject_cast<const QWidget*>(parent);
    if (widget && !widget->isWindow() && widget->parentWidget()->isWindow())
    {
      return describe(widget);
    }
    if (widget && widget->isWindow())
    {
      return describe(widget);
    }
  }
  return describe(object);
}

std::string eventTypeName(const QEvent::Type type)
{
  if (const auto* key = QMetaEnum::fromType<QEvent::Type>().valueToKey(type))
  {
    return key;
  }
  return std::to_string(int(type));
}

/**
 * Events that are sent again to time them. Input, child and deletion events are let
 * through untimed: sending them again could change how they propagate.
 */
bool isTimedType(const QEvent::Type type)
{
  switch (type)
  {
  case QEvent::Timer:
  case QEvent::MetaCall:
  case QEvent::UpdateRequest:
  case QEvent::UpdateLater:
  case QEvent::LayoutRequest:
  case QEvent::PolishRequest:
    return true;
  default:
    return false;
  }
}

/**
 * Times every event that the event loop delivers (depth 0) by type and receiver, and the
 * paint and resize events of every QOpenGLWidget at any depth. It sends an event again
 * from the filter and consumes the original, so that the time spent in the receiver can
 * be read after it returns.
 */
class EventProfiler : public QObject
{
private:
  int m_depth = 0;
  const QObject* m_passReceiver = nullptr;
  const QEvent* m_passEvent = nullptr;
  std::string m_topLevelEvent = "the call";

public:
  Buckets buckets;
  Buckets glBuckets;
  Buckets paintBuckets;
  Buckets resizeBuckets;
  double workMilliseconds = 0.0;
  size_t workEvents = 0;
  std::unordered_map<const QObject*, size_t> paints;
  std::unordered_map<const QObject*, double> paintMilliseconds;

  /** Events delivered from inside a synchronous call are not top level. */
  void enterCall()
  {
    ++m_depth;
    m_topLevelEvent = "the call";
  }
  void leaveCall() { --m_depth; }

  void resetBuckets()
  {
    buckets.clear();
    glBuckets.clear();
    paintBuckets.clear();
    resizeBuckets.clear();
  }

  size_t paintCount(const QObject* object) const
  {
    const auto it = paints.find(object);
    return it != paints.end() ? it->second : 0;
  }

  double paintTime(const QObject* object) const
  {
    const auto it = paintMilliseconds.find(object);
    return it != paintMilliseconds.end() ? it->second : 0.0;
  }

  bool eventFilter(QObject* receiver, QEvent* event) override
  {
    if (receiver == m_passReceiver && event == m_passEvent)
    {
      m_passReceiver = nullptr;
      m_passEvent = nullptr;
      return false;
    }

    const auto type = event->type();

    // On Windows, the event dispatcher receives a timer event for some timers (the
    // 20 ms resource timer) and sends the timer's own event from inside it: the outer
    // event is let through, so that the inner one is seen as the top level event.
    if (qobject_cast<QAbstractEventDispatcher*>(receiver))
    {
      return false;
    }

    const auto isGlEvent = (type == QEvent::Paint || type == QEvent::Resize)
                           && receiver->isWidgetType()
                           && qobject_cast<QOpenGLWidget*>(receiver) != nullptr;
    const auto isTopLevel = m_depth == 0;
    const auto isWork = isTopLevel && !isPeriodic(receiver, event);
    if (isWork)
    {
      ++workEvents;
    }

    // raster paint events are not nested, so they can be summed by area
    const auto isRasterPaint =
      type == QEvent::Paint && !isGlEvent && receiver->isWidgetType();
    // the resize of a child of a window includes the layout of everything in it, and
    // the children of a window are not nested, so they can be summed too
    const auto isAreaResize =
      type == QEvent::Resize && !isGlEvent && receiver->isWidgetType()
      && !static_cast<QWidget*>(receiver)->isWindow()
      && static_cast<QWidget*>(receiver)->parentWidget()
      && static_cast<QWidget*>(receiver)->parentWidget()->isWindow();
    if (
      !isGlEvent && !isRasterPaint && !isAreaResize && !(isTopLevel && isTimedType(type)))
    {
      return false;
    }

    const auto name = isTopLevel || isGlEvent
                        ? eventTypeName(type) + " " + describeReceiver(receiver)
                        : std::string{};
    const auto start = Clock::now();

    if (isTopLevel)
    {
      m_topLevelEvent = name;
    }
    ++m_depth;
    m_passReceiver = receiver;
    m_passEvent = event;
    QCoreApplication::sendEvent(receiver, event);
    m_passReceiver = nullptr;
    m_passEvent = nullptr;
    --m_depth;

    const auto elapsed = millisecondsSince(start);
    if (isTopLevel)
    {
      auto& bucket = buckets[name];
      bucket.milliseconds += elapsed;
      ++bucket.count;
      if (isWork)
      {
        workMilliseconds += elapsed;
      }
    }
    if (isGlEvent)
    {
      auto& bucket = glBuckets[name];
      bucket.milliseconds += elapsed;
      ++bucket.count;
      if (type == QEvent::Paint)
      {
        ++paints[receiver];
        paintMilliseconds[receiver] += elapsed;
      }
    }
    if (isAreaResize)
    {
      auto& bucket = resizeBuckets["Resize " + describe(receiver)];
      bucket.milliseconds += elapsed;
      ++bucket.count;
    }
    if (isRasterPaint)
    {
      auto& bucket = paintBuckets
        ["Paint " + describeArea(receiver) + " : " + describe(receiver)
         + (isOutsideWindow(receiver) ? " (outside the window, in " + m_topLevelEvent + ")" : "")];
      bucket.milliseconds += elapsed;
      ++bucket.count;
    }
    return true;
  }

private:
  /**
   * Periodic timers fire whether or not anything changed: the resource processing, the
   * autosave, the recent documents and the frame counters, and the cursor blink.
   */
  static bool isPeriodic(const QObject* receiver, const QEvent* event)
  {
    if (event->type() != QEvent::Timer)
    {
      return false;
    }
    if (const auto* timer = qobject_cast<const QTimer*>(receiver))
    {
      return !timer->isSingleShot();
    }
    return std::string_view{receiver->metaObject()->className()} == "QWidgetLineControl";
  }
};

/** The game, read from the environment, or nullopt if a variable is missing. */
struct BenchSetup
{
  std::filesystem::path gamePath;
  std::filesystem::path mapPath;
  std::filesystem::path resourcesPath;
  mdl::GameInfo gameInfo;
  size_t repeat = 20;
  bool onScreen = false;
  bool style = true;
};

std::optional<BenchSetup> readBenchSetup()
{
  const auto gamePath = environmentVariable("TB_BENCH_GAME_PATH");
  const auto gameConfigPath = environmentVariable("TB_BENCH_GAME_CONFIG");
  const auto mapPath = environmentVariable("TB_BENCH_MAP");
  if (!gamePath || !gameConfigPath || !mapPath)
  {
    return std::nullopt;
  }

  const auto configPath = std::filesystem::path{*gameConfigPath};
  auto gameConfig = fs::Disk::openFile(configPath) | kdl::and_then([&](auto file) {
                      auto reader = file->reader().buffer();
                      return mdl::parseGameConfig(reader.stringView(), configPath);
                    });
  if (!gameConfig.is_success())
  {
    FAIL("Could not read the game config " << configPath);
  }

  return BenchSetup{
    .gamePath = std::filesystem::path{*gamePath},
    .mapPath = std::filesystem::path{*mapPath},
    // resources/games/<game>/GameConfig.cfg -> resources
    .resourcesPath = configPath.parent_path().parent_path().parent_path(),
    .gameInfo = mdl::makeGameInfo(std::move(gameConfig) | kdl::value()),
    .repeat = std::max(environmentNumber("TB_BENCH_REPEAT").value_or(20), size_t{1}),
    .onScreen = environmentVariable("TB_BENCH_ONSCREEN") == "1",
    .style = environmentVariable("TB_BENCH_STYLE") != "0",
  };
}

std::unique_ptr<AppController> createAppController(
  const BenchSetup& setup, fs::TestEnvironment& testEnvironment)
{
  auto environmentConfig = std::make_unique<mdl::EnvironmentConfig>(
    testEnvironment.dir() / "__app",
    testEnvironment.dir() / "user",
    testEnvironment.dir() / "__temp",
    std::vector<std::filesystem::path>{});

  // resources/defaults holds the default texture, as
  // SystemPaths::findResourceDirectories("defaults") gives it to the app
  if (const auto defaultsPath = setup.resourcesPath / "defaults";
      fs::Disk::pathInfo(defaultsPath / "assets") == fs::PathInfo::Directory)
  {
    environmentConfig->defaultAssetFolderPaths.push_back(defaultsPath);
  }

  testEnvironment.createDirectory("games");
  testEnvironment.createDirectory("user");
  auto gameManager = mdl::initializeGameManager(
                       {testEnvironment.dir() / "games"}, testEnvironment.dir() / "user")
                     | kdl::transform([](auto manager, const auto&) { return manager; })
                     | kdl::value();

  return std::make_unique<AppController>(
    std::make_unique<kdl::task_manager>(std::thread::hardware_concurrency()),
    std::move(environmentConfig),
    std::make_unique<mdl::GameManager>(std::move(gameManager)));
}

/** Applies the app's look: Fusion, as the Dark theme does, and the app's style sheet. */
void applyStyle(const BenchSetup& setup)
{
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  auto file = QFile{QString::fromStdString(
    (setup.resourcesPath / "stylesheets" / "base.qss").generic_string())};
  if (file.open(QFile::ReadOnly | QFile::Text))
  {
    qApp->setStyleSheet(QTextStream{&file}.readAll());
    reportNote("style: Fusion and resources/stylesheets/base.qss");
  }
  else
  {
    reportNote("style: Fusion, no style sheet found");
  }
}

/** The app's surface format and context sharing, as its main sets them. */
void setAppGlDefaults()
{
  auto format = QSurfaceFormat{};
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(2, 1);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);
  format.setDepthBufferSize(24);
  format.setAlphaBufferSize(0);
  format.setSamples(4);
  QSurfaceFormat::setDefaultFormat(format);

  // read lazily by QOpenGLContext::globalShareContext, which the first GL widget calls
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}

const auto WindowStateKeys = std::vector<QString>{
  "Windows/MapWindow/Geometry",
  "Windows/MapWindow/State",
  "Windows/MapWindow/DockLayoutVersion",
  "Windows/MapWindow_VerticalSplitterSplitter/State",
  "Windows/MapWindow_HorizontalSplitter/State",
  "Windows/FaceInspector_TabBook/State",
  "Windows/Inspector/State",
  "Windows/Inspector_TabBook/State",
  "Windows/MapWindow_InfoPanel/State",
  "Windows/InfoPanel_TabBook/State",
  "Windows/MapWindow_ToolSidebar/Expanded",
};

void removeWindowStateKeys()
{
  auto settings = QSettings{};
  for (const auto& key : WindowStateKeys)
  {
    settings.remove(key);
  }
}

/** The nodes that the operations select. */
struct Targets
{
  mdl::EntityNode* lightA = nullptr;
  mdl::EntityNode* lightB = nullptr;
  mdl::EntityNode* entity = nullptr;
  mdl::BrushNode* brush = nullptr;
  size_t brushes = 0;
  size_t entities = 0;
  size_t lights = 0;
};

void collectTargets(mdl::Node& node, Targets& targets)
{
  for (auto* child : node.children())
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(child))
    {
      ++targets.entities;
      if (entityNode->entity().classname() == "light")
      {
        ++targets.lights;
        if (!targets.lightA)
        {
          targets.lightA = entityNode;
        }
        else if (!targets.lightB)
        {
          targets.lightB = entityNode;
        }
      }
      else if (!targets.entity)
      {
        targets.entity = entityNode;
      }
    }
    else if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
    {
      ++targets.brushes;
      // a worldspawn brush: its parent is a layer
      if (!targets.brush && dynamic_cast<mdl::LayerNode*>(brushNode->parent()))
      {
        targets.brush = brushNode;
      }
    }
    collectTargets(*child, targets);
  }
}

/** Selects the node the way SelectionTool::mouseClick does for a plain click. */
void selectObject(mdl::Map& map, mdl::Node* node)
{
  auto transaction = mdl::Transaction{map, "Select Object"};
  mdl::deselectAll(map);
  mdl::selectNodes(map, {node});
  transaction.commit();
}

/** Runs the event loop for the given time, without sleeping. */
void pumpFor(const std::chrono::milliseconds duration)
{
  const auto end = Clock::now() + duration;
  while (Clock::now() < end)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  }
}

/**
 * Drives the sidebar layout of this branch: the inspectors, the console, the issue
 * browser and the Light Editor are docks.
 */
class LayoutDriver
{
private:
  MapWindow& m_window;

public:
  QDockWidget* mapDock = nullptr;
  QDockWidget* entityDock = nullptr;
  QDockWidget* faceDock = nullptr;
  QDockWidget* consoleDock = nullptr;
  QDockWidget* issuesDock = nullptr;
  QDockWidget* lightEditorDock = nullptr;

  explicit LayoutDriver(MapWindow& window)
    : m_window{window}
  {
    const auto find = [&](const char* panelId) {
      auto* dock =
        m_window.findChild<QDockWidget*>(QString{"MapWindow_Sidebar_"} + panelId);
      REQUIRE(dock != nullptr);
      return dock;
    };
    mapDock = find("MapInspector");
    entityDock = find("EntityInspector");
    faceDock = find("FaceInspector");
    consoleDock = find("Console");
    issuesDock = find("Issues");
    lightEditorDock = find("LightEditor");
  }

  static std::string name() { return "docks"; }

  std::vector<QDockWidget*> inspectors() const { return {mapDock, entityDock, faceDock}; }
  std::vector<QDockWidget*> infoPanels() const { return {consoleDock, issuesDock}; }
  std::vector<QDockWidget*> all() const
  {
    return {mapDock, entityDock, faceDock, consoleDock, issuesDock, lightEditorDock};
  }

  QWidget* lightEditor() const { return lightEditorDock->widget(); }

  /** Every panel shown, the inspectors and the info panels in their tab groups. */
  void showAll()
  {
    for (auto* dock : all())
    {
      dock->show();
    }
    if (m_window.tabifiedDockWidgets(mapDock).size() != 2)
    {
      m_window.tabifyDockWidget(mapDock, entityDock);
      m_window.tabifyDockWidget(entityDock, faceDock);
    }
    if (m_window.tabifiedDockWidgets(consoleDock).size() != 1)
    {
      m_window.tabifyDockWidget(consoleDock, issuesDock);
    }
    mapDock->raise();
    consoleDock->raise();
    m_window.resizeDocks({mapDock}, {350}, Qt::Horizontal);
    m_window.resizeDocks({consoleDock}, {100}, Qt::Vertical);
    // the Light Editor opens under the inspectors with the height of its size hint;
    // someone who works in it gives it about half of the column
    m_window.resizeDocks({mapDock, lightEditorDock}, {450, 450}, Qt::Vertical);
  }

  void apply(const std::string& variant)
  {
    showAll();
    auto toClose = std::vector<QDockWidget*>{};
    if (variant == "L1")
    {
      toClose = inspectors();
    }
    else if (variant == "L2")
    {
      toClose = {lightEditorDock};
    }
    else if (variant == "L3")
    {
      toClose = all();
    }
    else if (variant == "L4")
    {
      toClose = infoPanels();
    }
    for (auto* dock : toClose)
    {
      dock->close();
    }
  }

  /** Where each dock is and whether it can be seen. */
  std::string describeState() const
  {
    auto result = std::string{};
    for (const auto* dock : all())
    {
      const auto g = dock->geometry();
      result += " " + dock->objectName().toStdString() + "=" + std::to_string(g.x()) + ","
                + std::to_string(g.y()) + "," + std::to_string(g.width()) + "x"
                + std::to_string(g.height()) + (dock->isVisible() ? " shown" : " closed")
                + (dock->visibleRegion().isEmpty() ? " unseen" : " seen");
    }
    return result;
  }

  bool hasInspectors() const { return mapDock->isVisible(); }
  bool hasLightEditor() const { return lightEditorDock->isVisible(); }
  bool hasInfoPanels() const { return consoleDock->isVisible(); }

  /** The dock whose width R1 changes, or none. */
  QDockWidget* rightDock() const
  {
    if (mapDock->isVisible())
    {
      return mapDock;
    }
    if (lightEditorDock->isVisible())
    {
      return lightEditorDock;
    }
    return nullptr;
  }

  int rightWidth() const
  {
    const auto* dock = rightDock();
    return dock ? dock->width() : 0;
  }

  bool canResize() const { return rightDock() != nullptr; }

  void resizeRight(const int width)
  {
    m_window.resizeDocks({rightDock()}, {width}, Qt::Horizontal);
  }

  void setAnimated(const bool animated)
  {
    auto options = m_window.dockOptions();
    options.setFlag(QMainWindow::AnimatedDocks, animated);
    m_window.setDockOptions(options);
  }

  /** Shows inspector tab 0 (Map), 1 (Entity) or 2 (Face). */
  void showInspectorTab(const size_t index) { inspectors()[index]->raise(); }
};

/** One operation's samples and event totals. */
struct OpResults
{
  SampleTable table;
  Buckets settleBuckets;
  Buckets lateBuckets;
  Buckets glBuckets;
  Buckets paintBuckets;
  Buckets resizeBuckets;
  size_t samples = 0;
  size_t timeouts = 0;
  size_t focusGiven = 0;
  int maxAnimations = 0;
};

class Bench
{
private:
  MapWindow& m_window;
  EventProfiler& m_profiler;
  LayoutDriver& m_layout;
  bool m_onScreen;
  std::unordered_map<const QObject*, size_t> m_swaps;

public:
  Bench(
    MapWindow& window, EventProfiler& profiler, LayoutDriver& layout, const bool onScreen)
    : m_window{window}
    , m_profiler{profiler}
    , m_layout{layout}
    , m_onScreen{onScreen}
  {
    for (auto* view : m_window.findChildren<RenderView*>())
    {
      QObject::connect(
        view, &QOpenGLWidget::frameSwapped, view, [this, view]() { ++m_swaps[view]; });
    }
  }

  std::vector<RenderView*> visibleMapViews() const
  {
    auto result = std::vector<RenderView*>{};
    for (auto* view : m_window.findChildren<MapViewBase*>())
    {
      if (view->isVisible() && !view->visibleRegion().isEmpty())
      {
        result.push_back(view);
      }
    }
    return result;
  }

  std::vector<RenderView*> allGlViews() const
  {
    auto result = std::vector<RenderView*>{};
    for (auto* view : m_window.findChildren<RenderView*>())
    {
      result.push_back(view);
    }
    return result;
  }

  size_t swapCount(const QObject* view) const
  {
    const auto it = m_swaps.find(view);
    return it != m_swaps.end() ? it->second : 0;
  }

  struct Settled
  {
    Clock::time_point end;
    bool timedOut = false;
    int maxAnimations = 0;
  };

  /**
   * Runs the event loop until it is quiet for 30 ms and every view for which mustPaint
   * holds has painted since the call.
   */
  Settled settle(
    const Clock::time_point callStart,
    const Clock::time_point callEnd,
    const std::vector<RenderView*>& mapViews,
    const std::function<bool(const RenderView*)>& mustPaint,
    const std::unordered_map<const QObject*, size_t>& paintsBefore,
    const std::unordered_map<const QObject*, size_t>& swapsBefore,
    const bool checkAnimations)
  {
    auto result = Settled{.end = callEnd};
    const auto deadline = callStart + 5s;

    const auto countBefore = [](const auto& counts, const QObject* object) {
      const auto it = counts.find(object);
      return it != counts.end() ? it->second : size_t{0};
    };
    const auto hasPainted = [&](const RenderView* view) {
      return m_profiler.paintCount(view) > countBefore(paintsBefore, view)
             && (!m_onScreen || swapCount(view) > countBefore(swapsBefore, view));
    };
    const auto allPainted = [&]() {
      return std::ranges::all_of(
        mapViews, [&](const auto* view) { return !mustPaint(view) || hasPainted(view); });
    };

    while (true)
    {
      const auto workBefore = m_profiler.workEvents;
      QCoreApplication::processEvents(QEventLoop::AllEvents);
      const auto now = Clock::now();
      const auto didWork = m_profiler.workEvents != workBefore;
      if (didWork)
      {
        result.end = now;
      }

      if (checkAnimations)
      {
        auto animations = 0;
        for (const auto* animation : m_window.findChildren<QPropertyAnimation*>())
        {
          if (
            animation->state() == QAbstractAnimation::Running
            && animation->duration() > 0)
          {
            ++animations;
          }
        }
        result.maxAnimations = std::max(result.maxAnimations, animations);
      }

      if (!didWork && now - result.end >= 30ms && allPainted())
      {
        break;
      }
      if (now > deadline)
      {
        result.timedOut = true;
        break;
      }
    }

    return result;
  }

  /**
   * Times one call: the call, then the event loop until it settles, then the event loop
   * until 700 ms after the call. With allViewsPaint, every visible map view must paint;
   * otherwise only those whose size the call changed.
   */
  void timeCall(
    OpResults& results,
    const std::string& name,
    const std::function<void()>& call,
    const bool allViewsPaint,
    const bool checkAnimations = false)
  {
    const auto mapViews = visibleMapViews();
    const auto glViews = allGlViews();

    auto paintsBefore = std::unordered_map<const QObject*, size_t>{};
    auto swapsBefore = std::unordered_map<const QObject*, size_t>{};
    auto sizesBefore = std::unordered_map<const QObject*, QSize>{};
    auto glPaintTimeBefore = 0.0;
    for (const auto* view : glViews)
    {
      paintsBefore[view] = m_profiler.paintCount(view);
      swapsBefore[view] = swapCount(view);
      sizesBefore[view] = view->size();
      glPaintTimeBefore += m_profiler.paintTime(view);
    }
    auto mapPaintTimeBefore = 0.0;
    auto mapPaintsBefore = size_t{0};
    for (const auto* view : mapViews)
    {
      mapPaintTimeBefore += m_profiler.paintTime(view);
      mapPaintsBefore += m_profiler.paintCount(view);
    }

    const auto mustPaint = [&](const RenderView* view) {
      return allViewsPaint || sizesBefore[view] != view->size();
    };

    m_profiler.resetBuckets();
    const auto workBefore = m_profiler.workMilliseconds;

    const auto start = Clock::now();
    m_profiler.enterCall();
    call();
    m_profiler.leaveCall();
    const auto callEnd = Clock::now();

    // the GL events inside the call are counted with those after it
    const auto settled = settle(
      start, callEnd, mapViews, mustPaint, paintsBefore, swapsBefore, checkAnimations);
    const auto settleWork = m_profiler.workMilliseconds - workBefore;

    auto mapPaintTime = -mapPaintTimeBefore;
    auto mapPaints = -double(mapPaintsBefore);
    for (const auto* view : mapViews)
    {
      mapPaintTime += m_profiler.paintTime(view);
      mapPaints += double(m_profiler.paintCount(view));
    }
    auto glPaintTime = -glPaintTimeBefore;
    for (const auto* view : glViews)
    {
      glPaintTime += m_profiler.paintTime(view);
    }
    auto glResizeTime = 0.0;
    for (const auto& [bucketName, bucket] : m_profiler.glBuckets)
    {
      if (bucketName.starts_with("Resize "))
      {
        glResizeTime += bucket.milliseconds;
      }
    }

    addBuckets(results.settleBuckets, m_profiler.buckets);
    addBuckets(results.glBuckets, m_profiler.glBuckets);
    addBuckets(results.paintBuckets, m_profiler.paintBuckets);
    addBuckets(results.resizeBuckets, m_profiler.resizeBuckets);
    m_profiler.resetBuckets();

    const auto lateWorkBefore = m_profiler.workMilliseconds;
    const auto lateEnd = std::max(start + 700ms, settled.end + 50ms);
    while (Clock::now() < lateEnd)
    {
      QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    const auto lateWork = m_profiler.workMilliseconds - lateWorkBefore;
    addBuckets(results.lateBuckets, m_profiler.buckets);
    m_profiler.resetBuckets();

    const auto sync = millisecondsBetween(start, callEnd);
    results.table.add(name + ".sync", sync);
    results.table.add(name + ".settle", millisecondsBetween(start, settled.end));
    results.table.add(name + ".busy", sync + settleWork);
    results.table.add(name + ".mapview_paint", mapPaintTime);
    results.table.add(name + ".gl_paint_all", glPaintTime);
    results.table.add(name + ".gl_resize_all", glResizeTime);
    results.table.add(name + ".late_busy", lateWork);
    results.table.add(name + ".mapview_paints", mapPaints, "count");
    ++results.samples;
    if (settled.timedOut)
    {
      ++results.timeouts;
    }
    results.maxAnimations = std::max(results.maxAnimations, settled.maxAnimations);
  }

  /** Makes a change that is not measured and lets it settle. */
  void prepare(const std::function<void()>& call)
  {
    call();
    const auto now = Clock::now();
    settle(now, now, {}, [](const auto*) { return false; }, {}, {}, false);
    pumpFor(100ms);
  }

  QDoubleSpinBox* focusLightEditorSpinBox()
  {
    for (auto* spin : m_layout.lightEditor()->findChildren<QDoubleSpinBox*>())
    {
      if (spin->isVisible() && spin->isEnabled())
      {
        m_window.activateWindow();
        spin->setFocus(Qt::MouseFocusReason);
        QCoreApplication::processEvents();
        return spin;
      }
    }
    return nullptr;
  }
};

void printResults(const std::string& prefix, const OpResults& results)
{
  results.table.print();
  reportCount(prefix + ".samples", results.samples, "count");
  reportCount(prefix + ".timeouts", results.timeouts, "count");
  reportTopBuckets(prefix + ".settle", results.settleBuckets, results.samples);
  reportTopBuckets(prefix + ".late", results.lateBuckets, results.samples, 6);
  reportTopBuckets(prefix + ".gl", results.glBuckets, results.samples, 8);
  reportTopBuckets(prefix + ".raster_paint", results.paintBuckets, results.samples, 16);
  reportTopBuckets(prefix + ".area_resize", results.resizeBuckets, results.samples, 8);
}

/** Times the same selection commands on a document that no window observes. */
void runHeadlessSelection(
  AppController& appController, const BenchSetup& setup, const size_t repeat)
{
  auto document = MapDocument::loadDocument(
                    appController.environmentConfig(),
                    setup.gameInfo,
                    mdl::MapFormat::Unknown,
                    MapDocument::worldBoundsForGame(setup.gameInfo.gameConfig),
                    setup.mapPath,
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();
  auto& map = document->map();
  auto targets = Targets{};
  collectTargets(map.worldNode(), targets);
  REQUIRE(targets.lightA != nullptr);

  auto table = SampleTable{};
  const auto time = [&](const std::string& name, const auto& call) {
    const auto start = Clock::now();
    call();
    table.add("headless." + name + ".sync", millisecondsSince(start));
  };

  for (size_t i = 0; i < repeat; ++i)
  {
    mdl::deselectAll(map);
    time("S1", [&] { selectObject(map, targets.lightA); });
    time("S2", [&] { mdl::deselectAll(map); });
    selectObject(map, targets.lightA);
    time("S3", [&] { selectObject(map, targets.brush); });
    selectObject(map, targets.lightA);
    time("S4", [&] { selectObject(map, targets.lightB); });
    selectObject(map, targets.lightA);
    time("S5", [&] { selectObject(map, targets.entity); });
  }
  mdl::deselectAll(map);
  table.print();
}

} // namespace

TEST_CASE("SelectionHitchBenchmark", "[.][benchmark]")
{
  const auto setup = readBenchSetup();
  if (!setup)
  {
    SKIP("TB_BENCH_GAME_PATH, TB_BENCH_GAME_CONFIG and TB_BENCH_MAP are not set");
  }

  setAppGlDefaults();
  if (setup->style)
  {
    applyStyle(*setup);
  }
  const auto resetStyleSheet = kdl::invoke_later{[] { qApp->setStyleSheet(QString{}); }};

  removeWindowStateKeys();
  const auto removeWindowStateKeysLater =
    kdl::invoke_later{[] { removeWindowStateKeys(); }};

  // the user's preferences that matter here; the rest are the defaults
  auto gameInfo = setup->gameInfo;
  auto& prefs = PreferenceManager::instance();
  prefs.set(gameInfo.gamePathPreference, setup->gamePath);
  prefs.set(
    Preferences::ShowLightVolumes, std::string{Preferences::ShowLightVolumesSelected});
  prefs.set(Preferences::LightVolumeHandleThreshold, 0.35f);
  const auto resetPreferences = kdl::invoke_later{[&] {
    prefs.resetToDefault(gameInfo.gamePathPreference);
    prefs.resetToDefault(Preferences::ShowLightVolumes);
    prefs.resetToDefault(Preferences::LightVolumeHandleThreshold);
  }};

  auto testEnvironment = fs::TestEnvironment{};
  auto appController = createAppController(*setup, testEnvironment);

  auto profiler = EventProfiler{};
  qApp->installEventFilter(&profiler);
  const auto removeProfiler =
    kdl::invoke_later{[&] { qApp->removeEventFilter(&profiler); }};

  const auto loadStart = Clock::now();
  auto document = MapDocument::loadDocument(
                    appController->environmentConfig(),
                    gameInfo,
                    mdl::MapFormat::Unknown,
                    MapDocument::worldBoundsForGame(gameInfo.gameConfig),
                    setup->mapPath,
                    appController->taskManager(),
                    appController->glManager().resourceManager())
                  | kdl::value();
  reportNote("map: " + setup->mapPath.generic_string());
  report("setup.map_load", millisecondsSince(loadStart), "ms");

  auto window = std::make_unique<MapWindow>(*appController, std::move(document));
  auto& map = window->document().map();

  auto targets = Targets{};
  collectTargets(map.worldNode(), targets);
  REQUIRE(targets.lightA != nullptr);
  REQUIRE(targets.lightB != nullptr);
  REQUIRE(targets.entity != nullptr);
  REQUIRE(targets.brush != nullptr);
  reportCount("setup.brushes", targets.brushes, "count");
  reportCount("setup.entities", targets.entities, "count");
  reportCount("setup.lights", targets.lights, "count");
  reportNote("S5 entity: " + targets.entity->entity().classname());

  // the screen's size, at most 1920x1080
  const auto available = QGuiApplication::primaryScreen()->availableGeometry();
  const auto windowSize =
    QSize{std::min(1920, available.width()), std::min(1080, available.height())};
  window->setAttribute(Qt::WA_DontShowOnScreen, !setup->onScreen);
  window->resize(windowSize);
  window->move(available.topLeft());
  window->show();
  if (setup->onScreen)
  {
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
  }
  reportNote(
    std::string{"window: "} + (setup->onScreen ? "on screen" : "WA_DontShowOnScreen")
    + " " + std::to_string(window->width()) + "x" + std::to_string(window->height())
    + " dpr " + formatValue(window->devicePixelRatioF()));

  auto layout = LayoutDriver{*window};
  auto bench = Bench{*window, profiler, layout, setup->onScreen};

  // let the textures and the models load
  const auto pumpStart = Clock::now();
  auto& resourceManager = appController->glManager().resourceManager();
  pumpFor(500ms);
  while ((resourceManager.needsProcessing() || !appController->glManager().initialized())
         && Clock::now() - pumpStart < 300s)
  {
    pumpFor(50ms);
  }
  pumpFor(1s);
  report("setup.resources_settle", millisecondsSince(pumpStart), "ms");
  reportCount("setup.resources", resourceManager.resources().size(), "count");
  reportCount(
    "setup.resources_pending", resourceManager.needsProcessing() ? 1 : 0, "flag");
  reportCount(
    "setup.gl_initialized", appController->glManager().initialized() ? 1 : 0, "flag");
  reportNote(
    "gl: " + gl::GlManager::glInfo().vendor + " / " + gl::GlManager::glInfo().renderer
    + " / " + gl::GlManager::glInfo().version);
  reportNote("layout: " + LayoutDriver::name());

  auto mapViewPaints = size_t{0};
  for (const auto* view : bench.visibleMapViews())
  {
    mapViewPaints += profiler.paintCount(view);
    reportNote(
      std::string{"map view: "} + view->metaObject()->className() + " "
      + std::to_string(view->width()) + "x" + std::to_string(view->height()));
  }
  reportCount("setup.mapview_paints", mapViewPaints, "count");
  REQUIRE(mapViewPaints > 0);

  // show every inspector once, so that their GL views exist, as after a session's use
  layout.showAll();
  for (size_t i = 0; i < 3; ++i)
  {
    bench.prepare([&] { layout.showInspectorTab(2 - i); });
  }

  const auto repeat = setup->repeat;
  const auto warmup = size_t{2};
  const auto lightA = targets.lightA;

  for (const auto variant : {"L0", "L1", "L2", "L3", "L4"})
  {
    if (!environmentListContains("TB_BENCH_LAYOUTS", variant))
    {
      continue;
    }

    bench.prepare([&] { mdl::deselectAll(map); });
    bench.prepare([&] { layout.apply(variant); });
    reportNote(
      std::string{variant} + " inspectors " + (layout.hasInspectors() ? "1" : "0")
      + " light editor " + (layout.hasLightEditor() ? "1" : "0") + " console "
      + (layout.hasInfoPanels() ? "1" : "0") + " right width "
      + std::to_string(layout.rightWidth()) + " visible gl views "
      + std::to_string(std::ranges::count_if(bench.allGlViews(), [](const auto* view) {
          return view->isVisible() && !view->visibleRegion().isEmpty();
        })));
    reportNote(std::string{variant} + " docks" + layout.describeState());
    if (const auto* console = window->findChild<Console*>())
    {
      if (const auto* text = console->findChild<QTextEdit*>())
      {
        reportNote(
          std::string{variant} + " console text: "
          + std::to_string(text->document()->blockCount()) + " lines, "
          + std::to_string(text->document()->characterCount()) + " characters");
      }
    }

    const auto selectionOp = [&](
                               const std::string& op,
                               const std::function<void()>& precondition,
                               const std::function<void()>& call,
                               const bool focus = false) {
      if (!environmentListContains("TB_BENCH_OPS", op))
      {
        return;
      }
      const auto prefix = std::string{variant} + "." + op;
      auto results = OpResults{};
      for (size_t i = 0; i < repeat + warmup; ++i)
      {
        bench.prepare(precondition);
        if (focus)
        {
          if (auto* spin = bench.focusLightEditorSpinBox(); spin && spin->hasFocus())
          {
            ++results.focusGiven;
          }
        }
        if (i < warmup)
        {
          bench.prepare(call);
          continue;
        }
        bench.timeCall(results, prefix, call, true);
      }
      printResults(prefix, results);
      if (focus)
      {
        reportCount(prefix + ".focus_given", results.focusGiven, "count");
      }
    };

    selectionOp("S1", [&] { mdl::deselectAll(map); }, [&] { selectObject(map, lightA); });
    selectionOp(
      "S2", [&] { selectObject(map, lightA); }, [&] { mdl::deselectAll(map); }, true);
    selectionOp(
      "S3",
      [&] { selectObject(map, lightA); },
      [&] { selectObject(map, targets.brush); });
    selectionOp(
      "S4",
      [&] { selectObject(map, lightA); },
      [&] { selectObject(map, targets.lightB); });
    selectionOp(
      "S5",
      [&] { selectObject(map, lightA); },
      [&] { selectObject(map, targets.entity); });

    // R1 and R2 with the light selected, as when the user works in the Light Editor
    bench.prepare([&] { selectObject(map, lightA); });

    if (environmentListContains("TB_BENCH_OPS", "R1") && layout.canResize())
    {
      for (const auto animated : {true, false})
      {
        layout.setAnimated(animated);
        const auto prefix =
          std::string{variant} + (animated ? ".R1_animated" : ".R1_not_animated");
        auto results = OpResults{};
        const auto baseWidth = layout.rightWidth();
        auto maxWidth = baseWidth;
        for (size_t step = 0; step < repeat; ++step)
        {
          // wider by 40 px per step, then narrower by 40 px per step
          const auto half = repeat / 2;
          const auto offset =
            step < half ? int(step + 1) * 40 : int(repeat - step - 1) * 40;
          bench.timeCall(
            results,
            prefix,
            [&] { layout.resizeRight(baseWidth + offset); },
            false,
            true);
          maxWidth = std::max(maxWidth, layout.rightWidth());
        }
        reportNote(
          prefix + " width " + std::to_string(baseWidth) + " up to "
          + std::to_string(maxWidth) + " and back to "
          + std::to_string(layout.rightWidth()));
        printResults(prefix, results);
        reportCount(
          prefix + ".max_running_animations", size_t(results.maxAnimations), "count");
      }
      layout.setAnimated(true);
    }

    if (environmentListContains("TB_BENCH_OPS", "R2") && layout.hasInspectors())
    {
      auto results = std::vector<OpResults>(3);
      const auto names = std::vector<std::string>{
        std::string{variant} + ".R2_to_entity",
        std::string{variant} + ".R2_to_face",
        std::string{variant} + ".R2_to_map"};
      bench.prepare([&] { layout.showInspectorTab(0); });
      for (size_t i = 0; i < repeat + warmup; ++i)
      {
        for (size_t tab = 0; tab < 3; ++tab)
        {
          const auto next = (tab + 1) % 3;
          if (i < warmup)
          {
            bench.prepare([&] { layout.showInspectorTab(next); });
          }
          else
          {
            bench.timeCall(
              results[tab], names[tab], [&] { layout.showInspectorTab(next); }, false);
          }
        }
      }
      for (size_t tab = 0; tab < 3; ++tab)
      {
        printResults(names[tab], results[tab]);
      }
    }
  }

  bench.prepare([&] { mdl::deselectAll(map); });
  bench.prepare([&] { layout.showAll(); });

  if (environmentListContains("TB_BENCH_OPS", "headless"))
  {
    runHeadlessSelection(*appController, *setup, repeat);
  }

  // let the resources settle before the window and its GL views go
  while (resourceManager.needsProcessing())
  {
    pumpFor(50ms);
  }
  window.reset();
  QCoreApplication::processEvents();
}

} // namespace tb::ui
