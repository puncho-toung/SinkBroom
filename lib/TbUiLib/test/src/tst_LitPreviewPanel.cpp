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
#include <QDockWidget>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include "SidebarTestUtils.h"
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "prefs/Preferences.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/LitPreviewPanel.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/QPathUtils.h"
#include "ui/RenderView.h"
#include "ui/SidebarManager.h"
#include "ui/SwitchableMapViewContainer.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// after windows.h
#include <tlhelp32.h>
#endif

namespace tb::ui
{
namespace
{

QLabel& findMessage(const LitPreviewPanel& panel)
{
  auto* label = panel.findChild<QLabel*>("LitPreviewPanel_Message");
  REQUIRE(label != nullptr);
  return *label;
}

std::string messageText(const LitPreviewPanel& panel)
{
  return findMessage(panel).text().toStdString();
}

QPushButton& findButton(const LitPreviewPanel& panel, const QString& name)
{
  auto* button = panel.findChild<QPushButton*>("LitPreviewPanel_" + name);
  REQUIRE(button != nullptr);
  return *button;
}

bool startPageShown(const LitPreviewPanel& panel)
{
  const auto* page = panel.findChild<QWidget*>("LitPreviewPanel_StartPage");
  REQUIRE(page != nullptr);
  return !page->isHidden();
}

const auto PressStartText =
  std::string{"Press Start to see the map lit as the engine draws it."};

/**
 * Restores the given settings groups as they were when it was made; a map window saves
 * its layout when it is closed.
 */
class SettingsSnapshot
{
private:
  QStringList m_groups;
  std::vector<std::pair<QString, QVariant>> m_values;

public:
  explicit SettingsSnapshot(QStringList groups)
    : m_groups{std::move(groups)}
  {
    auto settings = QSettings{};
    for (const auto& group : m_groups)
    {
      settings.beginGroup(group);
      for (const auto& key : settings.allKeys())
      {
        m_values.emplace_back(group + "/" + key, settings.value(key));
      }
      settings.endGroup();
    }
  }

  ~SettingsSnapshot()
  {
    auto settings = QSettings{};
    for (const auto& group : m_groups)
    {
      settings.remove(group);
    }
    for (const auto& [key, value] : m_values)
    {
      settings.setValue(key, value);
    }
  }
};

std::unique_ptr<MapDocument> createDoom3Document(AppController& appController)
{
  return MapDocument::createDocument(
           appController.environmentConfig(),
           mdl::Doom3BFGGameInfo,
           mdl::MapFormat::Doom3_Valve,
           vm::bbox3d{8192.0},
           appController.taskManager(),
           appController.glManager().resourceManager())
         | kdl::value();
}

/**
 * Hides the window's GL views, which would create GL contexts and load resources that the
 * tests do not provide; a layout switch makes new ones, which this hides again before
 * they are shown.
 */
void hideRenderViews(QWidget& window)
{
  for (auto* view : window.findChildren<RenderView*>())
  {
    view->hide();
  }
}

LitPreviewPanel& findLitPreviewPanel(const MapWindow& window)
{
  auto* dock = window.findChild<QDockWidget*>("MapWindow_Sidebar_LitPreview");
  REQUIRE(dock != nullptr);
  auto* panel = qobject_cast<LitPreviewPanel*>(dock->widget());
  REQUIRE(panel != nullptr);
  return *panel;
}

} // namespace

TEST_CASE("LitPreviewPanel")
{
  SECTION("identity")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    CHECK(panel.panelId() == "LitPreview");
    CHECK(panel.panelId() == LitPreviewPanel::PanelId);
    CHECK(panel.title() == "Lit Preview");
    // no SVG exists for it, so the tool sidebar shows a text button
    CHECK(panel.iconPath().empty());
    CHECK(!panel.shownByDefault());
    CHECK(panel.defaultArea() == Qt::RightDockWidgetArea);
  }

#ifdef _WIN32
  SECTION("opening the panel starts nothing and loads no Vulkan library (C9)")
  {
    // a driver or an overlay may have loaded the loader before; only then may it be there
    const auto* loadedBefore = GetModuleHandleW(L"vulkan-1.dll");
    if (loadedBefore)
    {
      WARN("vulkan-1.dll was loaded before the panel was made; C9 is not checked");
    }
    {
      auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
      auto window = QMainWindow{};
      window.setCentralWidget(new QWidget{});
      auto* manager = new SidebarManager{window};
      auto* dock =
        manager->registerPanel(std::make_unique<LitPreviewPanel>(fixture.context()));
      auto* panel = qobject_cast<LitPreviewPanel*>(dock->widget());
      REQUIRE(panel != nullptr);
      CHECK(dock->isHidden());

      window.show();
      dock->show();
      QApplication::processEvents();
      auto camera = gl::PerspectiveCamera{};
      fixture.context().cameraDidChangeNotifier(camera);
      QApplication::processEvents();

      CHECK(dock->isVisible());
      CHECK(!panel->sessionRunning());
      CHECK(panel->surfaceWindow() == nullptr);
      CHECK(startPageShown(*panel));
      if (!loadedBefore)
      {
        CHECK(GetModuleHandleW(L"vulkan-1.dll") == nullptr);
      }
    }
    if (!loadedBefore)
    {
      CHECK(GetModuleHandleW(L"vulkan-1.dll") == nullptr);
    }
  }
#endif

  SECTION("Start is available in an idTech 4 game")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    CHECK(panel.unavailableReason().isEmpty());
    CHECK(findButton(panel, "Start").isEnabled());
    CHECK(!findButton(panel, "Stop").isEnabled());
    CHECK(messageText(panel) == PressStartText);
    CHECK(startPageShown(panel));
  }

  SECTION("Start is unavailable in a game without an idTech 4 light editor")
  {
    auto fixture = SidebarContextFixture{mdl::QuakeFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    CHECK(!findButton(panel, "Start").isEnabled());
    CHECK(!findButton(panel, "Stop").isEnabled());
    CHECK(messageText(panel) == "The lit preview needs an idTech 4 game.");

    panel.start();
    CHECK(!panel.sessionRunning());
  }

  SECTION("Start is unavailable in a prefab editor")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    panel.setPrefabEditor(true);
    CHECK(!findButton(panel, "Start").isEnabled());
    CHECK(messageText(panel) == "The lit preview is not available in a prefab editor.");

    panel.start();
    CHECK(!panel.sessionRunning());
  }

  SECTION("a prefab editor window configures its panel")
  {
    auto appControllerFixture = AppControllerFixture{};
    auto& appController = appControllerFixture.appController();

    auto mapWindow = MapWindow{appController, createDoom3Document(appController)};
    CHECK(findLitPreviewPanel(mapWindow).unavailableReason().isEmpty());

    auto env = fs::TestEnvironment{};
    auto prefabDocument = createDoom3Document(appController);
    REQUIRE(prefabDocument->map().saveAs(env.dir() / "crate.map") | kdl::is_success());
    auto prefabEditor = MapWindow{appController, std::move(prefabDocument), true};
    CHECK(
      findLitPreviewPanel(prefabEditor).unavailableReason()
      == "The lit preview is not available in a prefab editor.");
  }

  SECTION("the session guard updates every panel")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto owner = LitPreviewPanel{fixture.context()};
    auto other = LitPreviewPanel{fixture.context()};
    REQUIRE(LitPreviewPanel::sessionOwner() == nullptr);

    auto notifications = 0;
    auto connection = NotifierConnection{};
    connection +=
      LitPreviewPanel::sessionOwnerDidChangeNotifier().connect([&] { ++notifications; });

    // as Start claims it
    CHECK(LitPreviewPanel::claimSession(owner));
    CHECK(notifications == 1);
    CHECK(LitPreviewPanel::sessionOwner() == &owner);
    CHECK(!LitPreviewPanel::claimSession(other));
    CHECK(notifications == 1);

    CHECK(!findButton(other, "Start").isEnabled());
    CHECK(messageText(other) == "Another window already runs the lit preview.");
    CHECK(owner.unavailableReason().isEmpty());

    // as teardown releases it
    LitPreviewPanel::releaseSession(other);
    CHECK(LitPreviewPanel::sessionOwner() == &owner);
    LitPreviewPanel::releaseSession(owner);
    CHECK(notifications == 2);
    CHECK(LitPreviewPanel::sessionOwner() == nullptr);
    CHECK(findButton(other, "Start").isEnabled());
    CHECK(messageText(other) == PressStartText);

    // R5.26: a panel also looks again when it is shown
    other.panelShown();
    CHECK(messageText(other) == PressStartText);
  }

  SECTION("a load without a session keeps the Start page")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    fixture.document().documentWasLoadedNotifier();
    CHECK(!panel.sessionRunning());
    CHECK(findButton(panel, "Start").isEnabled());
    CHECK(messageText(panel) == PressStartText);
  }
}

// Hidden GPU tests ("[.][litgpu]"): they need a Vulkan 1.2 driver and a desktop, and they
// show windows. Every section checks that each teardown leaves no NVRHI device reference
// and no NVRHI error, that the validation layer (in Debug) reported no error, and that
// the process's threads and handles come back to what they were after the first Stop,
// which loads the loader and the driver for good (D5).

#ifdef _WIN32

namespace
{

// What the process may hold beyond its counts after the first Stop: a few threads and
// handles that Qt and the driver make on demand, and one handle for every VkDevice that
// was created on a VkInstance since then. The last is the driver's: a bare
// vkCreateInstance, vkCreateDevice, vkDestroyDevice, vkDestroyInstance cycle leaves one
// handle behind on this machine's NVIDIA driver, and a cycle without a device none, while
// sessions on one instance leave none (measured 2026-10-21 in TbLitLibTest).
constexpr auto ThreadTolerance = 2;
constexpr auto HandleTolerance = 8;
constexpr auto DriverHandlesPerDevice = 1;

struct ProcessCounts
{
  long threads = 0;
  long handles = 0;
  // the sessions so far that reached phase B and so created a VkDevice
  std::uint64_t devices = 0;
};

ProcessCounts processCounts()
{
  auto counts = ProcessCounts{};
  counts.devices = LitPreviewPanel::diagnostics().sessionsWithDevice;
  auto handles = DWORD{0};
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  counts.handles = long(handles);

  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE)
  {
    auto entry = THREADENTRY32{};
    entry.dwSize = sizeof(entry);
    const auto processId = GetCurrentProcessId();
    for (auto found = Thread32First(snapshot, &entry); found;
         found = Thread32Next(snapshot, &entry))
    {
      if (entry.th32OwnerProcessID == processId)
      {
        ++counts.threads;
      }
    }
    CloseHandle(snapshot);
  }
  return counts;
}

void checkTeardown()
{
  const auto& diagnostics = LitPreviewPanel::diagnostics();
  CHECK(diagnostics.lastDeviceReleaseCount == 0u);
  CHECK(diagnostics.lastNvrhiErrorCount == 0u);
  CHECK(LitPreviewPanel::sessionOwner() == nullptr);
}

/**
 * Checks the counts against the baseline, waiting a little for driver threads that are
 * still winding down.
 */
void checkProcessCounts(const ProcessCounts& baseline, const std::string_view label)
{
  auto counts = processCounts();
  const auto devices = long(counts.devices - baseline.devices);
  const auto maxThreads = baseline.threads + ThreadTolerance;
  const auto maxHandles =
    baseline.handles + HandleTolerance + devices * DriverHandlesPerDevice;
  const auto settled = QTest::qWaitFor(
    [&] {
      counts = processCounts();
      return counts.threads <= maxThreads && counts.handles <= maxHandles;
    },
    3000);
  std::cout << label << ": threads " << baseline.threads << " -> " << counts.threads
            << ", handles " << baseline.handles << " -> " << counts.handles << " after "
            << devices << " devices" << (settled ? "" : " (not settled)") << "\n";
  CHECK(counts.threads <= maxThreads);
  CHECK(counts.handles <= maxHandles);
}

/**
 * The validation layer's errors (Debug builds) since it was made; printed with the
 * warnings when it goes.
 */
class ValidationCheck
{
private:
  std::string m_label;
  int m_errors;
  int m_warnings;

public:
  explicit ValidationCheck(std::string label)
    : m_label{std::move(label)}
    , m_errors{LitPreviewPanel::diagnostics().validationErrors}
    , m_warnings{LitPreviewPanel::diagnostics().validationWarnings}
  {
  }

  int errors() const
  {
    return LitPreviewPanel::diagnostics().validationErrors - m_errors;
  }

  int warnings() const
  {
    return LitPreviewPanel::diagnostics().validationWarnings - m_warnings;
  }

  void check() const
  {
    std::cout << m_label << ": validation layer "
              << (LitPreviewPanel::diagnostics().validationLayer ? "on" : "off")
              << ", errors " << errors() << ", warnings " << warnings() << "\n";
    CHECK(errors() == 0);
  }
};

/**
 * A main window with the panel in a sidebar dock, a line edit that has the focus, and a
 * camera that the test moves through the context's notifier, as the 3D view's would.
 */
class PanelHarness
{
public:
  SidebarContextFixture fixture{mdl::Doom3BFGFixtureConfig};
  gl::PerspectiveCamera camera;
  int cameraStep = 0;
  QMainWindow window;
  QLineEdit* lineEdit = nullptr;
  SidebarManager* manager = nullptr;
  QDockWidget* dock = nullptr;
  LitPreviewPanel* panel = nullptr;

  PanelHarness()
  {
    auto* central = new QWidget{};
    auto* layout = new QVBoxLayout{central};
    lineEdit = new QLineEdit{};
    layout->addWidget(lineEdit);
    layout->addStretch(1);
    window.setCentralWidget(central);
    // as the map window has them (MapWindow.cpp)
    window.setDockOptions(
      QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks
      | QMainWindow::GroupedDragging);

    manager = new SidebarManager{window};
    dock = manager->registerPanel(std::make_unique<LitPreviewPanel>(fixture.context()));
    panel = qobject_cast<LitPreviewPanel*>(dock->widget());
    REQUIRE(panel != nullptr);

    window.resize(1000, 700);
    window.show();
    dock->show();
    dock->raise();
    lineEdit->setFocus();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
  }

  void moveCamera()
  {
    ++cameraStep;
    camera.moveTo(vm::vec3f{float(cameraStep % 512), float(cameraStep / 512), 64.0f});
    fixture.context().cameraDidChangeNotifier(camera);
  }

  // moves the camera until a frame more than now is presented
  bool waitForFrame(const int timeout = 10000)
  {
    const auto before = panel->framesPresented();
    return QTest::qWaitFor(
      [&] {
        if (panel->framesPresented() > before)
        {
          return true;
        }
        moveCamera();
        return false;
      },
      timeout);
  }

  bool startAndWaitForFrame()
  {
    panel->start();
    return panel->sessionRunning() && waitForFrame();
  }

  // no camera change, no frame
  void checkIdle()
  {
    // a change that arrived within the 30 Hz pacing may still be drawn
    QTest::qWait(100);
    const auto settled = panel->framesPresented();
    QTest::qWait(200);
    CHECK(panel->framesPresented() == settled);
  }
};

std::vector<std::vector<std::string>> readCsv(const std::filesystem::path& path)
{
  auto rows = std::vector<std::vector<std::string>>{};
  auto stream = std::ifstream{path};
  auto line = std::string{};
  while (std::getline(stream, line))
  {
    auto fields = std::vector<std::string>{};
    auto field = std::string{};
    auto lineStream = std::stringstream{line};
    while (std::getline(lineStream, field, ','))
    {
      fields.push_back(field);
    }
    rows.push_back(std::move(fields));
  }
  return rows;
}

} // namespace

TEST_CASE("LitPreviewPanel on a GPU", "[.][litgpu]")
{
  auto harness = PanelHarness{};
  auto& panel = *harness.panel;
  auto& dock = *harness.dock;
  const auto validation = ValidationCheck{"panel"};

  // the first session loads the loader and the driver, which stay (D5)
  REQUIRE(harness.startAndWaitForFrame());
  CHECK(panel.sessionDrawing());
  CHECK(LitPreviewPanel::sessionOwner() == &panel);
  CHECK(!startPageShown(panel));
  CHECK(!findButton(panel, "Start").isEnabled());
  CHECK(findButton(panel, "Stop").isEnabled());
  panel.stop();
  checkTeardown();
  CHECK(LitPreviewPanel::diagnostics().lastReachedPhaseB);
  CHECK(startPageShown(panel));
  CHECK(messageText(panel) == PressStartText);
  QTest::qWait(200);
  const auto baseline = processCounts();

  SECTION("Start and Stop 50 times, also between the two phases and inside a frame")
  {
    for (auto i = 0; i < 50; ++i)
    {
      CAPTURE(i);
      switch (i % 3)
      {
      case 0:
        // no event is processed between the two calls, so the window has not been
        // exposed and phase B has not run
        panel.start();
        REQUIRE(panel.sessionRunning());
        CHECK(!panel.sessionDrawing());
        panel.stop();
        CHECK(!LitPreviewPanel::diagnostics().lastReachedPhaseB);
        break;
      case 1:
        // a camera change wakes the render thread, which is drawing when Stop comes
        REQUIRE(harness.startAndWaitForFrame());
        harness.moveCamera();
        panel.stop();
        CHECK(LitPreviewPanel::diagnostics().lastReachedPhaseB);
        break;
      default: {
        REQUIRE(harness.startAndWaitForFrame());
        harness.checkIdle();
        REQUIRE(harness.waitForFrame());
        panel.stop();
        CHECK(LitPreviewPanel::diagnostics().lastFramesPresented >= 2u);
        break;
      }
      }
      checkTeardown();
      CHECK(!panel.sessionRunning());
    }
    // messages of the stopped sessions that were still queued are dropped (R5.17)
    QTest::qWait(100);
    CHECK(startPageShown(panel));
    CHECK(messageText(panel) == PressStartText);
    checkProcessCounts(baseline, "Start/Stop x 50");
  }

  SECTION("float and dock again 50 times")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 50; ++i)
    {
      CAPTURE(i);
      dock.setFloating(true);
      REQUIRE(harness.waitForFrame());
      dock.setFloating(false);
      REQUIRE(harness.waitForFrame());
    }
    harness.checkIdle();
    panel.stop();
    checkTeardown();
    checkProcessCounts(baseline, "float/redock x 50");
  }

  SECTION("float and dock a running preview 50 times while the camera moves")
  {
    REQUIRE(harness.startAndWaitForFrame());
    auto timer = QTimer{};
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, [&] { harness.moveCamera(); });
    timer.start();
    for (auto i = 0; i < 50; ++i)
    {
      CAPTURE(i);
      dock.setFloating(true);
      QTest::qWait(40);
      dock.setFloating(false);
      QTest::qWait(40);
      REQUIRE(panel.sessionRunning());
    }
    timer.stop();
    REQUIRE(harness.waitForFrame());
    panel.stop();
    checkTeardown();
    checkProcessCounts(baseline, "float during camera moves x 50");
  }

  SECTION("a tab over the dock stops the frames, and closing the dock stops the session")
  {
    REQUIRE(harness.startAndWaitForFrame());

    const auto stub = registerStubPanel(*harness.manager, "Stub");
    harness.window.tabifyDockWidget(&dock, stub.dock);
    stub.dock->show();
    stub.dock->raise();
    REQUIRE(QTest::qWaitFor([&] { return !panel.surfaceWindow()->isExposed(); }));
    QTest::qWait(100);
    const auto hiddenFrames = panel.framesPresented();
    for (auto i = 0; i < 10; ++i)
    {
      harness.moveCamera();
      QTest::qWait(20);
    }
    CHECK(panel.framesPresented() == hiddenFrames);
    CHECK(panel.sessionRunning());

    dock.raise();
    REQUIRE(harness.waitForFrame());

    // the user's close
    dock.close();
    CHECK(!panel.sessionRunning());
    checkTeardown();
    CHECK(LitPreviewPanel::diagnostics().lastReachedPhaseB);
  }

  SECTION("minimise and restore")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      harness.window.showMinimized();
      QTest::qWait(300);
      const auto minimisedFrames = panel.framesPresented();
      for (auto j = 0; j < 10; ++j)
      {
        harness.moveCamera();
        QTest::qWait(20);
      }
      CHECK(panel.framesPresented() == minimisedFrames);

      harness.window.showNormal();
      REQUIRE(QTest::qWaitForWindowExposed(&harness.window));
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    checkTeardown();
  }

  SECTION("a forced FIFO present mode")
  {
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbUiLibTest-lit-fifo-frame-log.csv";
    std::filesystem::remove(logPath);
    qputenv("TB_LIT_FORCE_FIFO", "1");
    qputenv("TB_LIT_FRAME_LOG", pathAsQString(logPath).toUtf8());
    const auto unsetEnvironment = kdl::invoke_later{[] {
      qunsetenv("TB_LIT_FORCE_FIFO");
      qunsetenv("TB_LIT_FRAME_LOG");
    }};

    REQUIRE(harness.startAndWaitForFrame());
    // a new size, a new swapchain generation
    harness.window.resize(800, 560);
    REQUIRE(harness.waitForFrame());
    REQUIRE(harness.waitForFrame());
    panel.stop();
    checkTeardown();

    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 4u);
    auto generations = std::set<std::string>{};
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() >= 8u);
      CHECK(rows[i][3] == "fifo");
      generations.insert(rows[i][4]);
    }
    CHECK(generations.size() >= 2u);
    std::cout << "forced FIFO: " << rows.size() - 1 << " frames in " << generations.size()
              << " swapchain generations\n";
    std::filesystem::remove(logPath);
  }

  SECTION("an injected device loss shows the reason, and Start runs again")
  {
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      REQUIRE(harness.startAndWaitForFrame());
      panel.debugInjectDeviceLost();
      harness.moveCamera();
      REQUIRE(QTest::qWaitFor([&] { return !panel.sessionRunning(); }));

      CHECK(LitPreviewPanel::diagnostics().lastDeviceLost);
      checkTeardown();
      CHECK(startPageShown(panel));
      CHECK(findButton(panel, "Start").isEnabled());
      CHECK_THAT(messageText(panel), Catch::Matchers::ContainsSubstring("lost"));
      CHECK_THAT(messageText(panel), Catch::Matchers::ContainsSubstring("Press Start"));
    }
    REQUIRE(harness.startAndWaitForFrame());
    CHECK(messageText(panel) == PressStartText);
    panel.stop();
    checkTeardown();
  }

  SECTION("the window's surface is destroyed and made again")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 10; ++i)
    {
      CAPTURE(i);
      auto* window = panel.surfaceWindow();
      REQUIRE(window != nullptr);
      // SurfaceAboutToBeDestroyed parks the render thread and destroys the swapchain
      // before Qt destroys the surface
      window->destroy();
      QApplication::processEvents();
      REQUIRE(panel.sessionRunning());
      window->show();
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    checkTeardown();
  }

  SECTION("a screen change and a DPR change replace the swapchain")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 10; ++i)
    {
      CAPTURE(i);
      auto* window = panel.surfaceWindow();
      REQUIRE(window != nullptr);
      // as Qt sends them when the dock moves to another screen; the present check of
      // R5.33 passes on the same screen
      emit window->screenChanged(window->screen());
      auto dprChange = QEvent{QEvent::DevicePixelRatioChange};
      QCoreApplication::sendEvent(window, &dprChange);
      REQUIRE(panel.sessionRunning());
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    checkTeardown();
  }

  SECTION("the 3D view keeps the keyboard")
  {
    REQUIRE(harness.startAndWaitForFrame());
    auto* window = panel.surfaceWindow();
    auto* container = panel.surfaceContainer();
    REQUIRE(window != nullptr);
    REQUIRE(container != nullptr);
    REQUIRE(harness.window.focusWidget() == harness.lineEdit);

    // D44
    CHECK(window->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK(!window->flags().testFlag(Qt::WindowTransparentForInput));
    CHECK(container->focusPolicy() == Qt::NoFocus);

    QTest::mouseClick(container, Qt::LeftButton);
    QTest::mouseClick(window, Qt::LeftButton, {}, QPoint{10, 10});
    QTest::keyClick(window, Qt::Key_A);
    QApplication::processEvents();
    CHECK(harness.window.focusWidget() == harness.lineEdit);
    CHECK(QGuiApplication::focusWindow() != window);
    CHECK(harness.lineEdit->text().isEmpty());

    // what a real click sends: Windows asks the child whether it activates
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const auto topLevel = reinterpret_cast<HWND>(harness.window.winId());
    CHECK(
      SendMessageW(
        hwnd,
        WM_MOUSEACTIVATE,
        reinterpret_cast<WPARAM>(topLevel),
        MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN))
      == MA_NOACTIVATE);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    QApplication::processEvents();
    CHECK(harness.window.focusWidget() == harness.lineEdit);
    CHECK(GetFocus() != hwnd);

    panel.stop();
    checkTeardown();
  }

  SECTION("a document load and a game path change stop the session")
  {
    REQUIRE(harness.startAndWaitForFrame());
    harness.fixture.document().documentWasLoadedNotifier();
    CHECK(!panel.sessionRunning());
    checkTeardown();
    CHECK(startPageShown(panel));
    CHECK(messageText(panel) == "A map was loaded. Press Start to preview it.");

    REQUIRE(harness.startAndWaitForFrame());
    auto& prefs = PreferenceManager::instance();
    auto gamePath = harness.fixture.document().map().gameInfo().gamePathPreference;
    const auto resetGamePath = kdl::invoke_later{[&] { prefs.resetToDefault(gamePath); }};
    setPref(gamePath, gamePath.defaultValue / "base");
    CHECK(!panel.sessionRunning());
    checkTeardown();
    CHECK(messageText(panel) == "The game path changed. Press Start to preview with it.");
  }

  validation.check();
}

TEST_CASE("LitPreviewPanel in a map window on a GPU", "[.][litgpu]")
{
  // closing a window saves its layout
  const auto settingsSnapshot = SettingsSnapshot{{"Windows"}};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  const auto validation = ValidationCheck{"map window"};

  // shows the window without its GL views, and the lit preview's dock
  const auto showWindow = [](MapWindow& window) -> LitPreviewPanel& {
    hideRenderViews(window);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto* dock = window.findChild<QDockWidget*>("MapWindow_Sidebar_LitPreview");
    REQUIRE(dock != nullptr);
    dock->show();
    dock->raise();
    QApplication::processEvents();
    return findLitPreviewPanel(window);
  };

  // moves the window's current 3D camera until a frame more than now is presented
  const auto waitForFrame = [](MapWindow& window, const LitPreviewPanel& panel) {
    const auto before = panel.framesPresented();
    auto step = 0;
    return QTest::qWaitFor(
      [&] {
        if (panel.framesPresented() > before)
        {
          return true;
        }
        auto* mapView = window.findChild<SwitchableMapViewContainer*>();
        REQUIRE(mapView != nullptr);
        mapView->perspectiveCamera().moveTo(vm::vec3f{float(++step), 0.0f, 64.0f});
        return false;
      },
      10000);
  };

  SECTION("a layout switch 20 times; the camera notifier keeps delivering")
  {
    auto window = MapWindow{appController, createDoom3Document(appController)};
    auto& panel = showWindow(window);
    panel.start();
    REQUIRE(waitForFrame(window, panel));

    auto& prefs = PreferenceManager::instance();
    const auto resetLayout =
      kdl::invoke_later{[&] { prefs.resetToDefault(Preferences::MapViewLayout); }};
    for (auto i = 0; i < 20; ++i)
    {
      CAPTURE(i);
      // A fault of the tree before the lit preview: a switch deletes the views, and if
      // one of them has the application's focus, ~QWidget's clearFocus makes
      // MapWindow::focusChange call the container that is being deleted (a pure virtual
      // call). In the app the layout changes from the preferences dialog, which has the
      // focus; here the window is active, so its focus goes first.
      if (auto* focusWidget = QApplication::focusWidget())
      {
        focusWidget->clearFocus();
      }
      setPref(Preferences::MapViewLayout, (i + 1) % 4);
      hideRenderViews(window);
      QApplication::processEvents();
      REQUIRE(panel.sessionRunning());
      REQUIRE(waitForFrame(window, panel));
    }
    panel.stop();
    checkTeardown();
  }

  SECTION("closing the map window stops the session")
  {
    auto* window = new MapWindow{appController, createDoom3Document(appController)};
    const auto guard = QPointer<MapWindow>{window};
    auto& panel = showWindow(*window);
    panel.start();
    REQUIRE(waitForFrame(*window, panel));

    const auto teardowns = LitPreviewPanel::diagnostics().teardowns;
    // the window deletes itself on close
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(guard.isNull());
    CHECK(LitPreviewPanel::diagnostics().teardowns == teardowns + 1);
    CHECK(LitPreviewPanel::diagnostics().lastReachedPhaseB);
    checkTeardown();
  }

  validation.check();
}

#endif

} // namespace tb::ui
