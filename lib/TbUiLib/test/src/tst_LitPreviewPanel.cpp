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
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QWidget>
#include <QWindow>

// clang-format off: the host is private to TbUiLib, so it is found beside its sources
#include "../../src/LitSessionHost.h"
// clang-format on
#include "SidebarTestUtils.h"
#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "gl/PerspectiveCamera.h"
#include "lit/CpuPool.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/Snapshot.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameInfo.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

// a session exists once phase A has made the window's container
bool sessionRunning(const LitPreviewPanel& panel)
{
  return panel.findChild<QWidget*>("LitPreviewPanel_Container") != nullptr;
}

const auto PressStartText =
  std::string{"Press Start to see the map lit as the engine draws it."};

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
    CHECK(panel.iconPath() == "LitPreview.svg");
    checkGreyscaleIcon(panel.iconPath());
    CHECK(!panel.shownByDefault());
    CHECK(panel.defaultArea() == Qt::RightDockWidgetArea);

    // a click on Start or Stop leaves the keyboard with the 3D view (review L3.12)
    CHECK(findButton(panel, "Start").focusPolicy() == Qt::TabFocus);
    CHECK(findButton(panel, "Stop").focusPolicy() == Qt::TabFocus);
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
      CHECK(!sessionRunning(*panel));
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
    CHECK(!sessionRunning(panel));
  }

  SECTION("Start is unavailable in a prefab editor")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    panel.setPrefabEditor(true);
    CHECK(!findButton(panel, "Start").isEnabled());
    CHECK(messageText(panel) == "The lit preview is not available in a prefab editor.");

    panel.start();
    CHECK(!sessionRunning(panel));
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

    // as Start claims it
    CHECK(LitPreviewPanel::claimSessionForTesting(owner));
    CHECK(!LitPreviewPanel::claimSessionForTesting(other));
    CHECK(!findButton(other, "Start").isEnabled());
    CHECK(messageText(other) == "Another window already runs the lit preview.");
    CHECK(owner.unavailableReason().isEmpty());

    // as teardown releases it
    LitPreviewPanel::releaseSessionForTesting(other);
    CHECK(!findButton(other, "Start").isEnabled());
    LitPreviewPanel::releaseSessionForTesting(owner);
    CHECK(findButton(other, "Start").isEnabled());
    CHECK(messageText(other) == PressStartText);

    // R5.26: a panel also looks again when it is shown
    other.panelShown();
    CHECK(messageText(other) == PressStartText);
  }

  SECTION("a panel that goes away releases the session guard")
  {
    // review L2.13: a dead owner would keep Start disabled in every window
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto other = LitPreviewPanel{fixture.context()};
    {
      auto owner = LitPreviewPanel{fixture.context()};
      REQUIRE(LitPreviewPanel::claimSessionForTesting(owner));
      CHECK(!findButton(other, "Start").isEnabled());
    }
    CHECK(findButton(other, "Start").isEnabled());
    CHECK(messageText(other) == PressStartText);
  }

  SECTION("a load without a session keeps the Start page")
  {
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto panel = LitPreviewPanel{fixture.context()};
    fixture.document().documentWasLoadedNotifier();
    CHECK(!sessionRunning(panel));
    CHECK(findButton(panel, "Start").isEnabled());
    CHECK(messageText(panel) == PressStartText);
  }

  SECTION("a report counts a lost device as a problem (review R.1)")
  {
    auto report = LitPreviewReport{};
    CHECK(report.problems().empty());
    report.deviceLost = true;
    CHECK(report.problems() == std::vector<std::string>{"The GPU device was lost."});
  }

  SECTION("the session the panel starts reads the document's game path (C1, LP2.1)")
  {
    // as LitPreviewPanel::start makes its host: the panel's options carry no game path,
    // phase B gives the session the document's, whose blobs its compile thread reads
    auto fixture = SidebarContextFixture{mdl::Doom3BFGFixtureConfig};
    auto env = fs::TestEnvironment{};
    auto& map = fixture.document().map();
    map.setGamePath(env.dir());
    auto options = lit::SessionOptions{};
    options.validation = true;
    auto host = LitSessionHost{
      options, 1, {}, {}, fixture.document(), fixture.document().map().gamePath()};
    const auto sessionOptions = host.sessionOptionsForTesting();
    CHECK(sessionOptions.gamePath == env.dir());
    CHECK(sessionOptions.validation);

    // a session is made with the path its document has then (D41: a change ends it)
    map.setGamePath(env.dir() / "other");
    CHECK(host.sessionOptionsForTesting().gamePath == env.dir() / "other");
  }
}

// Hidden GPU tests ("[.][litgpu]"): they need a Vulkan 1.2 driver and a desktop, and they
// show windows. Every section checks that each teardown reports a clean stop (no NVRHI
// device reference left, no NVRHI error, no overrun), that the validation layer reported
// nothing, and that the process's threads, handles, USER and GDI objects come back to
// what they were after the first Stop, which loads the loader and the driver for good
// (D5).

#ifdef _WIN32

namespace
{

using namespace std::chrono_literals;

constexpr auto ValidationLayerName = "VK_LAYER_KHRONOS_validation";

// how long a stop may take when the render thread holds a frame for HoldTime
constexpr auto HoldTime = 300ms;
constexpr auto StopBound = 2000ms;

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

// the preview's window of the process's one session
QWindow* findSurfaceWindow()
{
  for (auto* window : QGuiApplication::allWindows())
  {
    if (window->objectName() == "LitPreviewWindow")
    {
      return window;
    }
  }
  return nullptr;
}

std::chrono::milliseconds millisecondsSince(
  const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);
}

/**
 * One thread of this process: its id and its Win32 start address
 * (NtQueryInformationThread class 9, ThreadQuerySetWin32StartAddress), resolved against
 * the loaded modules as "module+0xOFFSET" ("?" when it cannot be read).
 */
struct ProcessThread
{
  unsigned long id = 0;
  std::uintptr_t start = 0;
  std::string where;
  // its start inside an OS worker's module: ntdll (the thread pool) or crypt32
  bool osWorker = false;
};

/**
 * LP5.3 step 0 (44-FEED-THREAD-FLAKE.md): the thread count alone cannot tell a leak from
 * the OS's own thread-pool workers, which a session's create or teardown may add (start
 * inside ntdll) and which live until the process exits, or retire on their own (the
 * "14 -> 13" readings); so the set is recorded with each thread's start, and a check
 * fails only on a new thread whose start is not inside ntdll or crypt32 (its chain
 * engine's worker, which the OS starts on its own). TbLitLibTest's LitTestUtils holds the
 * same measure.
 */
struct ProcessCounts
{
  long threads = 0;
  long handles = 0;
  long userObjects = 0;
  long gdiObjects = 0;
  std::vector<ProcessThread> threadSet;
};

long handleCount()
{
  auto handles = DWORD{0};
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  return long(handles);
}

ProcessCounts processCounts()
{
  auto counts = ProcessCounts{};
  counts.handles = handleCount();
  // GetProcessHandleCount leaves out USER and GDI objects, a leaked window among them
  counts.userObjects = long(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
  counts.gdiObjects = long(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));

  using QueryThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  // LP5.3 step 11: crypt32's chain-engine worker (CRYPT32.dll+0x1BF50), which the OS
  // starts on its own (2 of 3 [litgpu] runs of the float/redock section)
  const auto crypt32 = GetModuleHandleW(L"crypt32.dll");
  const auto query = ntdll ? reinterpret_cast<QueryThread>(reinterpret_cast<void*>(
                               GetProcAddress(ntdll, "NtQueryInformationThread")))
                           : nullptr;
  constexpr auto ThreadQuerySetWin32StartAddress = 9;

  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE)
  {
    auto entry = THREADENTRY32{};
    entry.dwSize = sizeof(entry);
    const auto processId = GetCurrentProcessId();
    for (auto found = Thread32First(snapshot, &entry); found;
         found = Thread32Next(snapshot, &entry))
    {
      if (entry.th32OwnerProcessID != processId)
      {
        continue;
      }
      ++counts.threads;
      auto thread = ProcessThread{entry.th32ThreadID, 0, "?", false};
      if (const auto handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.id))
      {
        auto start = static_cast<void*>(nullptr);
        if (
          query
          && query(
               handle,
               ThreadQuerySetWin32StartAddress,
               &start,
               ULONG(sizeof(start)),
               nullptr)
               >= 0)
        {
          thread.start = reinterpret_cast<std::uintptr_t>(start);
        }
        CloseHandle(handle);
      }
      auto module = HMODULE{nullptr};
      if (
        thread.start != 0
        && GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(thread.start),
          &module))
      {
        auto name = std::array<wchar_t, MAX_PATH>{};
        GetModuleFileNameW(module, name.data(), DWORD(name.size()));
        auto where = std::ostringstream{};
        where << std::filesystem::path{name.data()}.filename().string() << "+0x"
              << std::hex << std::uppercase
              << (thread.start - reinterpret_cast<std::uintptr_t>(module));
        thread.where = where.str();
        thread.osWorker = module == ntdll || (crypt32 && module == crypt32);
      }
      else if (thread.start != 0)
      {
        auto where = std::ostringstream{};
        where << "0x" << std::hex << std::uppercase << thread.start;
        thread.where = where.str();
      }
      counts.threadSet.push_back(std::move(thread));
    }
    CloseHandle(snapshot);
  }
  return counts;
}

// the threads of now that baseline did not have (the same id and start counts as had)
std::vector<ProcessThread> newThreads(
  const ProcessCounts& baseline, const ProcessCounts& now)
{
  auto result = std::vector<ProcessThread>{};
  for (const auto& thread : now.threadSet)
  {
    const auto had = std::any_of(
      baseline.threadSet.begin(), baseline.threadSet.end(), [&](const auto& old) {
        return old.id == thread.id && old.start == thread.start;
      });
    if (!had)
    {
      result.push_back(thread);
    }
  }
  return result;
}

// those of newThreads whose start is not an OS worker's: what the check fails on
std::vector<ProcessThread> newForeignThreads(
  const ProcessCounts& baseline, const ProcessCounts& now)
{
  auto result = newThreads(baseline, now);
  std::erase_if(result, [](const auto& thread) { return thread.osWorker; });
  return result;
}

std::string describeThreads(const std::vector<ProcessThread>& threads)
{
  if (threads.empty())
  {
    return "none";
  }
  auto text = std::string{};
  for (const auto& thread : threads)
  {
    text += (text.empty() ? "" : ", ") + thread.where + " (id "
            + std::to_string(thread.id) + ")";
  }
  return text;
}

/**
 * What the driver keeps of one session's Vulkan objects after they are destroyed.
 */
struct DriverCost
{
  double handles = 0.0;
  double gdiObjects = 0.0;
};

/**
 * One bare Vulkan session without the preview: a QVulkanInstance, a QWindow with its
 * surface, a VkDevice on the discrete GPU and one swapchain, all destroyed again.
 */
bool bareVulkanCycle()
{
  auto instance = QVulkanInstance{};
  instance.setApiVersion(QVersionNumber{1, 2});
  instance.setFlags(QVulkanInstance::NoDebugOutputRedirect);
  if (!instance.create())
  {
    return false;
  }
  auto window = QWindow{};
  window.setSurfaceType(QSurface::VulkanSurface);
  window.setVulkanInstance(&instance);
  window.resize(200, 150);
  window.show();
  if (!QTest::qWaitForWindowExposed(&window))
  {
    return false;
  }
  const auto surface = QVulkanInstance::surfaceForWindow(&window);

  auto* f = instance.functions();
  auto count = std::uint32_t{0};
  f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr);
  auto physicalDevices = std::vector<VkPhysicalDevice>(count);
  f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, physicalDevices.data());
  if (physicalDevices.empty())
  {
    return false;
  }
  auto physicalDevice = physicalDevices.front();
  for (const auto candidate : physicalDevices)
  {
    auto properties = VkPhysicalDeviceProperties{};
    f->vkGetPhysicalDeviceProperties(candidate, &properties);
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      physicalDevice = candidate;
      break;
    }
  }

  const auto getSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
    instance.getInstanceProcAddr("vkGetPhysicalDeviceSurfaceSupportKHR"));
  const auto getCapabilities =
    reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
      instance.getInstanceProcAddr("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
  auto supported = VkBool32{VK_FALSE};
  getSupport(physicalDevice, 0, surface, &supported);
  if (!supported)
  {
    return false;
  }
  auto capabilities = VkSurfaceCapabilitiesKHR{};
  getCapabilities(physicalDevice, surface, &capabilities);

  const auto priority = 1.0f;
  auto queue = VkDeviceQueueCreateInfo{};
  queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue.queueFamilyIndex = 0;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  auto deviceInfo = VkDeviceCreateInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queue;
  deviceInfo.enabledExtensionCount = 1;
  deviceInfo.ppEnabledExtensionNames = &extension;
  auto device = VkDevice{VK_NULL_HANDLE};
  if (f->vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS)
  {
    return false;
  }

  const auto createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
    f->vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
  const auto destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
    f->vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR"));
  auto swapchainInfo = VkSwapchainCreateInfoKHR{};
  swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchainInfo.surface = surface;
  swapchainInfo.minImageCount = std::max(capabilities.minImageCount, 2u);
  swapchainInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  swapchainInfo.imageExtent = capabilities.currentExtent;
  swapchainInfo.imageArrayLayers = 1;
  swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchainInfo.preTransform = capabilities.currentTransform;
  swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapchainInfo.clipped = VK_TRUE;
  auto swapchain = VkSwapchainKHR{VK_NULL_HANDLE};
  const auto made = createSwapchain(device, &swapchainInfo, nullptr, &swapchain);
  if (made == VK_SUCCESS)
  {
    destroySwapchain(device, swapchain, nullptr);
  }
  instance.deviceFunctions(device)->vkDestroyDevice(device, nullptr);
  instance.resetDeviceFunctions(device);
  window.destroy();
  return made == VK_SUCCESS;
}

/**
 * The driver's cost per session, measured once: the bare cycle above ten times, after two
 * that load the loader and the driver. A bare instance costs nothing here and a device
 * one handle; the NVIDIA driver also keeps one GDI object of a device that made a
 * swapchain (RTX 2070 Max-Q, measured 2026-10-21). The allowance is that, per session
 * with a device, and not an assumed +1.
 */
const DriverCost& driverCostPerSession()
{
  static const auto cost = [] {
    REQUIRE(bareVulkanCycle());
    REQUIRE(bareVulkanCycle());
    QApplication::processEvents();
    const auto before = processCounts();
    constexpr auto cycles = 10;
    for (auto i = 0; i < cycles; ++i)
    {
      REQUIRE(bareVulkanCycle());
    }
    QApplication::processEvents();
    const auto after = processCounts();
    const auto measured = DriverCost{
      double(after.handles - before.handles) / double(cycles),
      double(after.gdiObjects - before.gdiObjects) / double(cycles),
    };
    std::cout << "driver cost per bare session: " << measured.handles << " handles, "
              << measured.gdiObjects << " GDI objects\n";
    return measured;
  }();
  return cost;
}

/**
 * W.2 (plans/lit-preview/13-LITGPU-FLAKE.md): pays the window system's one-time costs of
 * the process before the first baseline. The first time a window of the process becomes
 * the foreground window, Windows sets up the GUI thread's text input (about 23 handles,
 * among them a [CoreUI] ALPC port, and 3 USER objects), and the first minimise or restore
 * opens the Segoe MDL2 font (2 handles); all of it stays for the life of the process.
 * When Windows keeps the harness window from the foreground at show (its foreground lock,
 * while the user works in another application), a later section that minimises, restores
 * or activates the window would pay them after its baseline. One minimise and restore,
 * then an activation as the keyboard section's, pay them here, once per process; the log
 * says whether the process then held the foreground.
 */
void payWindowSystemCostsOnce(QMainWindow& window)
{
  static auto paid = false;
  if (std::exchange(paid, true))
  {
    return;
  }

  window.showMinimized();
  QTest::qWait(300);
  window.showNormal();
  REQUIRE(QTest::qWaitForWindowExposed(&window));
  window.activateWindow();
  // best effort: Windows may refuse the foreground, which the log line below shows
  static_cast<void>(QTest::qWaitForWindowActive(&window));
  QTest::qWait(200);

  auto foregroundProcess = DWORD{0};
  if (const auto foreground = GetForegroundWindow())
  {
    GetWindowThreadProcessId(foreground, &foregroundProcess);
  }
  std::cout << "window-system warm-up: the foreground window "
            << (foregroundProcess == GetCurrentProcessId()
                  ? "belongs to this process"
                  : "does not belong to this process (Windows refused it): repeat "
                    "the run if a count check fails")
            << "\n";
}

// beyond the counts after the first Stop: what Qt and the driver make on demand. R2.14:
// no thread may stay, so that a session's CPU pool left unjoined (CpuPool::ThreadCount
// threads) fails; the pool's own threads are also checked one by one (PanelHarness)
constexpr auto ThreadTolerance = 0;
constexpr auto HandleTolerance = 8;
constexpr auto GuiObjectTolerance = 4;

long driverAllowance(const double costPerSession, const std::uint64_t sessions)
{
  return long(std::lround(std::max(costPerSession, 0.0) * double(sessions)));
}

/**
 * Checks the counts against the baseline, waiting a little for driver threads that are
 * still winding down. The handles and the GDI objects may grow by the driver's measured
 * cost per session that had a device, no more.
 */
void checkProcessCounts(
  const ProcessCounts& baseline,
  const std::uint64_t sessionsWithDevice,
  const std::string_view label)
{
  const auto& cost = driverCostPerSession();
  const auto driverHandles = driverAllowance(cost.handles, sessionsWithDevice);
  const auto driverGdiObjects = driverAllowance(cost.gdiObjects, sessionsWithDevice);
  const auto maxHandles = baseline.handles + HandleTolerance + driverHandles;
  const auto maxUserObjects = baseline.userObjects + GuiObjectTolerance;
  const auto maxGdiObjects = baseline.gdiObjects + GuiObjectTolerance + driverGdiObjects;
  auto counts = processCounts();
  const auto settled = QTest::qWaitFor(
    [&] {
      counts = processCounts();
      return newForeignThreads(baseline, counts).size() <= std::size_t(ThreadTolerance)
             && counts.handles <= maxHandles && counts.userObjects <= maxUserObjects
             && counts.gdiObjects <= maxGdiObjects;
    },
    3000);
  std::cout << label << " (" << sessionsWithDevice << " sessions with a device): threads "
            << baseline.threads << " -> " << counts.threads
            << " (new outside the OS workers: "
            << describeThreads(newForeignThreads(baseline, counts))
            << "; new in all: " << describeThreads(newThreads(baseline, counts))
            << "), handles " << baseline.handles << " -> " << counts.handles
            << " (driver allowance " << driverHandles << "), USER "
            << baseline.userObjects << " -> " << counts.userObjects << ", GDI "
            << baseline.gdiObjects << " -> " << counts.gdiObjects << " (driver allowance "
            << driverGdiObjects << ")" << (settled ? "" : " (not settled)") << "\n";
  CHECK(newForeignThreads(baseline, counts).size() <= std::size_t(ThreadTolerance));
  CHECK(counts.handles <= maxHandles);
  CHECK(counts.userObjects <= maxUserObjects);
  CHECK(counts.gdiObjects <= maxGdiObjects);
}

/**
 * Counts the validation layer's messages, which Qt's debug messenger logs with qDebug as
 * "vkDebug: ..." (warnings and errors). Qt's messenger exists only between the instance's
 * creation and its destruction, so messages about the instance itself, and about objects
 * left when it is destroyed, are not seen here; TbLitLibTest's own hidden test covers
 * them with a messenger chained into VkInstanceCreateInfo. In Debug the layer must be
 * present, because the GPU tests claim it; Release has no validation.
 */
class ValidationMessages
{
private:
  static inline std::atomic<int> s_count = 0;
  static inline std::mutex s_mutex;
  static inline std::vector<std::string> s_texts;
  static inline QtMessageHandler s_previous = nullptr;

  std::string m_label;

  static void handler(
    const QtMsgType type, const QMessageLogContext& context, const QString& message)
  {
    if (message.startsWith("vkDebug:"))
    {
      ++s_count;
      const auto lock = std::scoped_lock{s_mutex};
      s_texts.push_back(message.toStdString());
    }
    if (s_previous)
    {
      s_previous(type, context, message);
    }
  }

public:
  explicit ValidationMessages(std::string label)
    : m_label{std::move(label)}
  {
#ifndef NDEBUG
    auto instance = QVulkanInstance{};
    REQUIRE(instance.supportedLayers().contains(ValidationLayerName));
#endif
    s_count = 0;
    {
      const auto lock = std::scoped_lock{s_mutex};
      s_texts.clear();
    }
    s_previous = qInstallMessageHandler(&handler);
  }

  ~ValidationMessages() { qInstallMessageHandler(s_previous); }

  ValidationMessages(const ValidationMessages&) = delete;
  ValidationMessages& operator=(const ValidationMessages&) = delete;

  void check() const
  {
#ifndef NDEBUG
    std::cout << m_label << ": validation messages " << s_count << "\n";
    {
      const auto lock = std::scoped_lock{s_mutex};
      for (const auto& text : s_texts)
      {
        std::cout << "  " << text << "\n";
      }
    }
    CHECK(s_count == 0);
#else
    std::cout << m_label << ": validation not checked (Release)\n";
#endif
  }
};

// The reports of a panel's sessions, as sessionDidEndNotifier passes them on.
class Reports
{
private:
  NotifierConnection m_connection;

public:
  std::vector<LitPreviewReport> reports;

  explicit Reports(LitPreviewPanel& panel)
  {
    m_connection += panel.sessionDidEndNotifier.connect(
      [&](const LitPreviewReport& report) { reports.push_back(report); });
  }

  const LitPreviewReport& last() const
  {
    REQUIRE(!reports.empty());
    return reports.back();
  }

  std::uint64_t sessionsWithDevice() const
  {
    return std::uint64_t(std::ranges::count_if(
      reports, [](const auto& report) { return report.hadDevice; }));
  }

  // the last report is clean; whether it had a device is checked by the caller
  void checkLast() const
  {
    const auto& report = last();
    CAPTURE(report.generation);
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK(!report.frameLogError);
    CHECK(report.messages.empty());
    CHECK(!report.stopOverran);
    CHECK(report.problems().empty());
  }
};

/**
 * Holds a session's render thread once at a debug hook: by default the next frame between
 * its submit and its present, as a present that waits would hold it. It holds for
 * HoldTime, or for whatever whileHeld does.
 */
class FrameHold
{
private:
  struct State
  {
    std::atomic<bool> used = false;
    std::atomic<bool> entered = false;
    std::atomic<bool> left = false;
  };
  std::shared_ptr<State> m_state = std::make_shared<State>();

public:
  explicit FrameHold(
    lit::LitSession& session,
    const lit::DebugHook point = lit::DebugHook::BeforePresent,
    std::function<void()> whileHeld = {})
  {
    session.debugSetHook(point, [state = m_state, hold = std::move(whileHeld)]() {
      if (state->used.exchange(true))
      {
        return;
      }
      state->entered = true;
      if (hold)
      {
        hold();
      }
      else
      {
        std::this_thread::sleep_for(HoldTime);
      }
      state->left = true;
    });
  }

  bool waitUntilEntered() const
  {
    return QTest::qWaitFor([&] { return m_state->entered.load(); }, 5000);
  }

  bool entered() const { return m_state->entered; }
  bool left() const { return m_state->left; }
};

// waits on the GUI thread without processing any event
template <typename Predicate>
bool waitWithoutEvents(Predicate predicate, const std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// keeps every message the document logs
class CapturingLogger : public Logger
{
private:
  mutable std::mutex m_mutex;
  std::vector<std::pair<LogLevel, std::string>> m_messages;

public:
  bool contains(const LogLevel level, const std::string& part) const
  {
    const auto lock = std::scoped_lock{m_mutex};
    return std::ranges::any_of(m_messages, [&](const auto& message) {
      return message.first == level && message.second.find(part) != std::string::npos;
    });
  }

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_messages.emplace_back(level, std::string{message});
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
  std::unique_ptr<Reports> reports;

  PanelHarness()
  {
    auto* central = new QWidget{};
    auto* layout = new QVBoxLayout{central};
    lineEdit = new QLineEdit{};
    lineEdit->setObjectName("HarnessLineEdit");
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
    reports = std::make_unique<Reports>(*panel);

    window.resize(1000, 700);
    window.show();
    dock->show();
    dock->raise();
    lineEdit->setFocus();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
  }

  lit::LitSession* session() const { return panel->sessionForTesting(); }

  std::uint64_t frames() const { return session() ? session()->framesPresented() : 0; }

  void moveCamera()
  {
    ++cameraStep;
    camera.moveTo(vm::vec3f{float(cameraStep % 512), float(cameraStep / 512), 64.0f});
    fixture.context().cameraDidChangeNotifier(camera);
  }

  // moves the camera until a frame more than now is presented
  bool waitForFrame(const int timeout = 10000)
  {
    const auto before = frames();
    return QTest::qWaitFor(
      [&] {
        if (frames() > before)
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
    const auto running = sessionRunning(*panel) && waitForFrame();
    if (running)
    {
      capturePoolThreads();
    }
    return running;
  }

  // R2.14: handles of the running session's pool threads, which each thread duplicates
  // itself while all of them wait for each other, so that every thread is counted once
  void capturePoolThreads()
  {
    auto* pool = session() ? session()->cpuPoolForTesting() : nullptr;
    if (!pool)
    {
      return;
    }
    auto met = std::make_shared<std::latch>(std::ptrdiff_t(lit::CpuPool::ThreadCount));
    auto handles = std::make_shared<std::vector<HANDLE>>();
    auto mutex = std::make_shared<std::mutex>();
    auto futures = std::vector<std::future<lit::CpuJobResult<bool>>>{};
    for (auto i = std::size_t{0}; i < lit::CpuPool::ThreadCount; ++i)
    {
      futures.push_back(
        pool->run([met, handles, mutex](const lit::CpuJobContext&) -> Result<bool> {
          met->arrive_and_wait();
          auto handle = HANDLE{};
          const auto duplicated = DuplicateHandle(
            GetCurrentProcess(),
            GetCurrentThread(),
            GetCurrentProcess(),
            &handle,
            SYNCHRONIZE,
            FALSE,
            0);
          if (duplicated)
          {
            const auto lock = std::scoped_lock{*mutex};
            handles->push_back(handle);
          }
          return duplicated != 0;
        }));
    }
    for (auto& future : futures)
    {
      REQUIRE(future.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
      CHECK(future.get().result == Result<bool>{true});
    }
    REQUIRE(handles->size() == lit::CpuPool::ThreadCount);
    poolThreads.insert(poolThreads.end(), handles->begin(), handles->end());
  }

  // every pool thread captured so far has ended (the session's stop joined it)
  void checkPoolThreadsJoined()
  {
    for (const auto handle : poolThreads)
    {
      CHECK(WaitForSingleObject(handle, 3000) == WAIT_OBJECT_0);
      CloseHandle(handle);
    }
    poolThreads.clear();
  }

  std::vector<HANDLE> poolThreads;

  // stops and returns how long the stop took
  std::chrono::milliseconds timedStop()
  {
    const auto start = std::chrono::steady_clock::now();
    panel->stop();
    return millisecondsSince(start);
  }

  // no camera change, no frame
  void checkIdle()
  {
    // a change that arrived within the 30 Hz pacing may still be drawn
    QTest::qWait(100);
    const auto settled = frames();
    QTest::qWait(200);
    CHECK(frames() == settled);
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
    if (!line.empty() && line.back() == ',')
    {
      fields.emplace_back();
    }
    rows.push_back(std::move(fields));
  }
  return rows;
}

} // namespace

TEST_CASE("LitPreviewPanel on a GPU", "[.][litgpu]")
{
  // The process counts depend on desktop focus: a run whose log says the process never
  // held the foreground (the window-system warm-up line) is to be repeated, not read as a
  // leak.
  const auto validation = ValidationMessages{"panel"};
  auto harness = PanelHarness{};
  auto& panel = *harness.panel;
  auto& dock = *harness.dock;
  auto& reports = *harness.reports;

  // the first session loads the loader and the driver, which stay (D5)
  REQUIRE(harness.startAndWaitForFrame());
  CHECK(harness.session() != nullptr);
  CHECK(!startPageShown(panel));
  CHECK(!findButton(panel, "Start").isEnabled());
  CHECK(findButton(panel, "Stop").isEnabled());
  panel.stop();
  reports.checkLast();
  CHECK(reports.last().hadDevice);
  CHECK(startPageShown(panel));
  CHECK(messageText(panel) == PressStartText);
  QTest::qWait(200);
  payWindowSystemCostsOnce(harness.window);
  driverCostPerSession();
  const auto baseline = processCounts();
  const auto devicesAtBaseline = reports.sessionsWithDevice();
  const auto checkCounts = [&](const std::string_view label) {
    harness.checkPoolThreadsJoined();
    checkProcessCounts(baseline, reports.sessionsWithDevice() - devicesAtBaseline, label);
  };

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
        REQUIRE(sessionRunning(panel));
        CHECK(harness.session() == nullptr);
        panel.stop();
        CHECK(!reports.last().hadDevice);
        break;
      case 1: {
        // D, review L3.7: the render thread holds a frame between its submit and its
        // present when Stop comes, so the frame is in flight
        REQUIRE(harness.startAndWaitForFrame());
        const auto hold = FrameHold{*harness.session()};
        harness.moveCamera();
        REQUIRE(hold.waitUntilEntered());
        CHECK(!hold.left());
        CHECK(harness.timedStop() < StopBound);
        CHECK(reports.last().hadDevice);
        break;
      }
      default: {
        REQUIRE(harness.startAndWaitForFrame());
        harness.checkIdle();
        REQUIRE(harness.waitForFrame());
        panel.stop();
        CHECK(reports.last().framesPresented >= 2u);
        break;
      }
      }
      reports.checkLast();
      CHECK(!sessionRunning(panel));
    }
    // messages of the stopped sessions that were still queued are dropped (R5.17)
    QTest::qWait(100);
    CHECK(startPageShown(panel));
    CHECK(messageText(panel) == PressStartText);
    checkCounts("Start/Stop x 50");
  }

  SECTION("float and dock again 50 times")
  {
    // Floating and docking reparent the preview's native window (QWindowContainer moves
    // it to a fake parent and back with SetParent); Qt 6.10 does not destroy its surface,
    // so this covers the reparenting and the resizes, and "the window's surface is
    // destroyed and made again" covers the surface handshake.
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
    reports.checkLast();
    // the floating dock has another size, so new generations were made
    CHECK(reports.last().swapchainGenerations >= 2u);
    checkCounts("float/redock x 50");
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
      REQUIRE(sessionRunning(panel));
    }
    timer.stop();
    REQUIRE(harness.waitForFrame());
    panel.stop();
    reports.checkLast();
    checkCounts("float during camera moves x 50");
  }

  SECTION(
    "a park that timed out before the thread parked is released when the size "
    "comes back")
  {
    // F2: the render thread holds a frame past the handshake's 100 ms park, the dock is
    // resized and resized back before it parks; without the release the preview would
    // stay parked and draw nothing
    dock.setFloating(true);
    REQUIRE(harness.startAndWaitForFrame());
    REQUIRE(harness.waitForFrame());
    const auto size = dock.size();
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      const auto hold = FrameHold{*harness.session()};
      harness.moveCamera();
      REQUIRE(hold.waitUntilEntered());
      dock.resize(size + QSize{80, 60});
      QTest::qWait(20);
      dock.resize(size);
      QTest::qWait(20);
      REQUIRE(QTest::qWaitFor([&] { return hold.left(); }, 5000));
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    reports.checkLast();
    CHECK(reports.last().parkTimeouts >= 1u);
    dock.setFloating(false);
    checkCounts("park released");
  }

  SECTION(
    "a park that timed out before the thread parked is released after a float and "
    "a redock")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      const auto hold = FrameHold{*harness.session()};
      harness.moveCamera();
      REQUIRE(hold.waitUntilEntered());
      dock.setFloating(true);
      QTest::qWait(20);
      dock.setFloating(false);
      QTest::qWait(20);
      REQUIRE(QTest::qWaitFor([&] { return hold.left(); }, 5000));
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    reports.checkLast();
    checkCounts("park released after float");
  }

  SECTION(
    "a thread that parked after its park timed out resumes only on a new generation")
  {
    // Review V.1, L3.2's sequence. The handshake's 100 ms park times out while the render
    // thread is still inside its park; the thread then parks for good, and only then does
    // the size come back, so nothing needs replacing by size. The park released the
    // generation's image wrappers, so the host must not resume the thread on it.
    dock.setFloating(true);
    REQUIRE(harness.startAndWaitForFrame());
    REQUIRE(harness.waitForFrame());
    const auto size = dock.size();
    // review N.1: an iteration in which no swapchain replacement was pending at the
    // restore takes the host's parked branch; one with a pending request takes the stale
    // branch and proves nothing
    auto pinned = 0;
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      auto& session = *harness.session();
      const auto generations = session.swapchainGenerations();
      // inside the park, past the handshake's 100 ms
      const auto parking = FrameHold{session, lit::DebugHook::WhileParking};
      // parked, before the thread posts Parked
      const auto parked = FrameHold{session, lit::DebugHook::AfterParking};
      dock.resize(size + QSize{80, 60});
      REQUIRE(parking.waitUntilEntered());
      // the host's park has timed out; no event is processed until the thread has parked
      REQUIRE(waitWithoutEvents([&] { return parked.entered(); }, 2000ms));
      CHECK(session.isParked());
      pinned += session.swapchainReplacementRequested() ? 0 : 1;
      dock.resize(size);
      REQUIRE(QTest::qWaitFor([&] { return parked.left(); }, 5000));
      REQUIRE(harness.waitForFrame());
      CHECK(session.swapchainGenerations() > generations);
    }
    CHECK(pinned >= 1);
    panel.stop();
    reports.checkLast();
    CHECK(reports.last().parkTimeouts >= 3u);
    dock.setFloating(false);
    checkCounts("resumed on a new generation");
  }

  SECTION("an unpark that comes while the thread parks resumes it on a new generation")
  {
    // Review V.1, its second route: the size comes back while the thread is inside its
    // park, after it released the image wrappers, so the host unparks a thread that it
    // sees as not parked; the thread then resumes on a generation without wrappers.
    dock.setFloating(true);
    REQUIRE(harness.startAndWaitForFrame());
    REQUIRE(harness.waitForFrame());
    const auto size = dock.size();
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      auto& session = *harness.session();
      const auto generations = session.swapchainGenerations();
      const auto parking = FrameHold{session, lit::DebugHook::WhileParking};
      dock.resize(size + QSize{80, 60});
      REQUIRE(parking.waitUntilEntered());
      CHECK(!session.isParked());
      dock.resize(size);
      QTest::qWait(20);
      // review N.2: the restore came while the thread was still inside its park
      CHECK(!parking.left());
      REQUIRE(QTest::qWaitFor([&] { return parking.left(); }, 5000));
      REQUIRE(harness.waitForFrame());
      CHECK(session.swapchainGenerations() > generations);
    }
    panel.stop();
    reports.checkLast();
    dock.setFloating(false);
    checkCounts("unpark while parking");
  }

  SECTION("a frame held past the park's bound: Stop and a surface destruction")
  {
    // Review V.9: the hold is longer than ParkTimeout (500 ms), so the park of the stop
    // or of SurfaceAboutToBeDestroyed times out and the slow paths run: the stop's wait
    // with servicing, and the surface's fallback that stops the thread before Qt destroys
    // the surface. The held frame sends its window a message at 600 ms, which only the
    // GUI thread answers.
    for (const auto destroySurface : {false, true})
    {
      CAPTURE(destroySurface);
      REQUIRE(harness.startAndWaitForFrame());
      auto* window = findSurfaceWindow();
      REQUIRE(window != nullptr);
      const auto hwnd = reinterpret_cast<HWND>(window->winId());
      auto sent = std::atomic<bool>{false};
      const auto hold = FrameHold{
        *harness.session(), lit::DebugHook::BeforePresent, [&] {
          std::this_thread::sleep_for(600ms);
          auto result = DWORD_PTR{0};
          sent =
            SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 3000, &result) != 0;
          std::this_thread::sleep_for(100ms);
        }};
      harness.moveCamera();
      REQUIRE(hold.waitUntilEntered());
      const auto start = std::chrono::steady_clock::now();
      if (destroySurface)
      {
        window->destroy();
      }
      else
      {
        panel.stop();
      }
      CHECK(millisecondsSince(start) < 1500ms);
      CHECK(sent);
      CHECK(hold.left());
      if (destroySurface)
      {
        // the thread did not park before the surface went, so the session failed
        REQUIRE(QTest::qWaitFor([&] { return !sessionRunning(panel); }));
        CHECK_THAT(
          messageText(panel), Catch::Matchers::ContainsSubstring("did not stop drawing"));
      }
      CHECK(!sessionRunning(panel));
      reports.checkLast();
      CHECK(reports.last().hadDevice);
    }
    checkCounts("held past the park's bound");
  }

  SECTION("Stop, a dock close and a map load finish while a frame is held")
  {
    // F1: the render thread holds its frame as a waiting present would; each way of
    // ending the session waits for it, servicing messages, and never longer than the
    // bound
    const auto endings = std::vector<std::pair<std::string, std::function<void()>>>{
      {"Stop", [&] { panel.stop(); }},
      {"dock close", [&] { dock.close(); }},
      {"map load", [&] { harness.fixture.document().documentWasLoadedNotifier(); }},
    };
    for (const auto& [name, end] : endings)
    {
      CAPTURE(name);
      dock.show();
      dock.raise();
      REQUIRE(harness.startAndWaitForFrame());
      const auto hold = FrameHold{*harness.session()};
      harness.moveCamera();
      REQUIRE(hold.waitUntilEntered());
      const auto start = std::chrono::steady_clock::now();
      end();
      CHECK(millisecondsSince(start) < StopBound);
      CHECK(!sessionRunning(panel));
      CHECK(hold.left());
      reports.checkLast();
    }
    checkCounts("ends while held");
  }

  SECTION("a present that waits for the GUI thread does not hang the Stop")
  {
    // R5.18's premise, made certain: the render thread sends a message to the preview's
    // window, which only the GUI thread answers, just before its present, while the GUI
    // thread is inside Stop. A Stop that joined without servicing would wait for the
    // SendMessage's three seconds.
    REQUIRE(harness.startAndWaitForFrame());
    auto* window = findSurfaceWindow();
    REQUIRE(window != nullptr);
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    auto sent = std::atomic<bool>{false};
    auto sendDone = std::atomic<std::chrono::steady_clock::rep>{0};
    const auto hold = FrameHold{
      *harness.session(), lit::DebugHook::BeforePresent, [&] {
        std::this_thread::sleep_for(100ms);
        auto result = DWORD_PTR{0};
        sent = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 3000, &result) != 0;
        sendDone = std::chrono::steady_clock::now().time_since_epoch().count();
      }};
    harness.moveCamera();
    REQUIRE(hold.waitUntilEntered());
    const auto stopStart = std::chrono::steady_clock::now();
    panel.stop();
    CHECK(millisecondsSince(stopStart) < 1500ms);
    CHECK(sent);
    // the message was answered from inside Stop
    CHECK(sendDone > stopStart.time_since_epoch().count());
    reports.checkLast();
    checkCounts("SendMessage during Stop");
  }

  SECTION("a tab over the dock stops the frames, and closing the dock stops the session")
  {
    REQUIRE(harness.startAndWaitForFrame());

    const auto stub = registerStubPanel(*harness.manager, "Stub");
    harness.window.tabifyDockWidget(&dock, stub.dock);
    stub.dock->show();
    stub.dock->raise();
    REQUIRE(QTest::qWaitFor([&] { return !findSurfaceWindow()->isExposed(); }));
    QTest::qWait(100);
    const auto hiddenFrames = harness.frames();
    for (auto i = 0; i < 10; ++i)
    {
      harness.moveCamera();
      QTest::qWait(20);
    }
    CHECK(harness.frames() == hiddenFrames);
    CHECK(sessionRunning(panel));

    dock.raise();
    REQUIRE(harness.waitForFrame());

    // the user's close
    dock.close();
    CHECK(!sessionRunning(panel));
    reports.checkLast();
    CHECK(reports.last().hadDevice);
    checkCounts("tab and close");
  }

  SECTION("minimise and restore")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 3; ++i)
    {
      CAPTURE(i);
      harness.window.showMinimized();
      QTest::qWait(300);
      const auto minimisedFrames = harness.frames();
      for (auto j = 0; j < 10; ++j)
      {
        harness.moveCamera();
        QTest::qWait(20);
      }
      CHECK(harness.frames() == minimisedFrames);

      harness.window.showNormal();
      REQUIRE(QTest::qWaitForWindowExposed(&harness.window));
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    reports.checkLast();
    checkCounts("minimise");
  }

  SECTION("a forced FIFO present mode, timed in the frame log")
  {
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbUiLibTest-lit-fifo-frame-log.csv";
    std::filesystem::remove(logPath);
    qputenv("TB_LIT_FORCE_FIFO", "yes");
    qputenv("TB_LIT_FRAME_LOG", pathAsQString(logPath).toUtf8());
    const auto unsetEnvironment = kdl::invoke_later{[] {
      qunsetenv("TB_LIT_FORCE_FIFO");
      qunsetenv("TB_LIT_FRAME_LOG");
    }};

    // two sessions into one log
    for (auto i = 0; i < 2; ++i)
    {
      REQUIRE(harness.startAndWaitForFrame());
      // a new size, a new swapchain generation
      harness.window.resize(i == 0 ? 800 : 1000, i == 0 ? 560 : 700);
      for (auto j = 0; j < 10; ++j)
      {
        REQUIRE(harness.waitForFrame());
      }
      panel.stop();
      reports.checkLast();
    }

    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 10u);
    CHECK(rows[0][0] == "session");
    // LP2.2b (C16): the frame's GPU time, the passes' and the frame's counts
    // LP4.1 (C16, P4.27): the view's portal flow's five columns; LP4.2a (C34, P4.27): the
    // ambient pass's two; LP5.1 (C45): the math and ambient lights, 51; LP5.2a (C46): the
    // stage pass's three, 54
    REQUIRE(rows[0].size() == 57u);
    CHECK(rows[0][6] == "gpu_frame_ms");
    CHECK(rows[0][18] == "gpu_prepass_ms");
    CHECK(rows[0][21] == "gpu_blit_ms");
    CHECK(rows[0][22] == "draws");
    CHECK(rows[0][31] == "volatile_writes");
    CHECK(rows[0][34] == "settled");
    CHECK(rows[0][35] == "alloc_refused");
    CHECK(rows[0][36] == "culled_triangles");
    CHECK(rows[0][37] == "view_area");
    CHECK(rows[0][38] == "areas_flowed");
    CHECK(rows[0][39] == "lights_in_view");
    CHECK(rows[0][40] == "portal_chains");
    CHECK(rows[0][41] == "flow_ms");
    CHECK(rows[0][42] == "ambient_draws");
    CHECK(rows[0][43] == "gpu_ambient_ms");
    auto sessions = std::set<std::string>{};
    auto starts = 0;
    auto stops = 0;
    auto generations = std::set<std::string>{};
    auto presentMs = std::vector<double>{};
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 57u);
      sessions.insert(rows[i][0]);
      if (rows[i][1] == "start")
      {
        ++starts;
        CHECK(!rows[i][12].empty());
        continue;
      }
      if (rows[i][1] == "stop")
      {
        ++stops;
        CHECK(!rows[i][12].empty());
        continue;
      }
      CHECK(rows[i][7] == "fifo");
      // a frame row has its draws (0 without lit pipelines) and says whether it is
      // settled (D77)
      CHECK(!rows[i][22].empty());
      CHECK((rows[i][34] == "0" || rows[i][34] == "1"));
      generations.insert(rows[i][0] + "/" + rows[i][8]);
      presentMs.push_back(std::stod(rows[i][5]));
    }
    CHECK(sessions.size() == 2u);
    CHECK(starts == 2);
    CHECK(stops == 2);
    CHECK(generations.size() >= 3u);
    std::ranges::sort(presentMs);
    std::cout << "forced FIFO: " << presentMs.size() << " frames in "
              << generations.size() << " swapchain generations; present call median "
              << presentMs[presentMs.size() / 2] << " ms, longest " << presentMs.back()
              << " ms\n";
    std::filesystem::remove(logPath);
    checkCounts("forced FIFO");
  }

  SECTION("an injected device loss or NVRHI error shows its reason, and Start runs again")
  {
    // D, review L3.13: a loss before any GPU work, a loss in the middle of a frame (after
    // the acquire, before the present), and an NVRHI error through its message callback,
    // which is an error, not a loss
    const auto faults = std::vector<lit::DebugFault>{
      lit::DebugFault::DeviceLostBeforeFrame,
      lit::DebugFault::DeviceLostAfterAcquire,
      lit::DebugFault::DeviceRemovedInSubmit,
      lit::DebugFault::NvrhiError,
    };
    for (const auto fault : faults)
    {
      const auto lost = fault != lit::DebugFault::NvrhiError;
      CAPTURE(int(fault));
      REQUIRE(harness.startAndWaitForFrame());
      harness.session()->debugInjectFault(fault);
      harness.moveCamera();
      REQUIRE(QTest::qWaitFor([&] { return !sessionRunning(panel); }));

      const auto& report = reports.last();
      CHECK(report.deviceLost == lost);
      CHECK(report.deviceReleaseCount == 0u);
      CHECK(
        report.nvrhiErrors
        == (fault == lit::DebugFault::DeviceRemovedInSubmit || fault == lit::DebugFault::NvrhiError ? 1u : 0u));
      CHECK(startPageShown(panel));
      CHECK(findButton(panel, "Start").isEnabled());
      if (lost)
      {
        CHECK_THAT(messageText(panel), Catch::Matchers::ContainsSubstring("lost"));
        CHECK_THAT(messageText(panel), Catch::Matchers::ContainsSubstring("Press Start"));
      }
      else
      {
        CHECK_THAT(messageText(panel), Catch::Matchers::ContainsSubstring("NVRHI error"));
        CHECK_THAT(
          messageText(panel), !Catch::Matchers::ContainsSubstring("device was lost"));
      }
    }
    REQUIRE(harness.startAndWaitForFrame());
    CHECK(messageText(panel) == PressStartText);
    panel.stop();
    reports.checkLast();
    checkCounts("faults");
  }

  SECTION("the window's surface is destroyed and made again")
  {
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 10; ++i)
    {
      CAPTURE(i);
      auto* window = findSurfaceWindow();
      REQUIRE(window != nullptr);
      const auto generations = harness.session()->swapchainGenerations();
      // SurfaceAboutToBeDestroyed parks the render thread and destroys the swapchain
      // before Qt destroys the surface
      window->destroy();
      QApplication::processEvents();
      REQUIRE(sessionRunning(panel));
      window->show();
      REQUIRE(harness.waitForFrame());
      CHECK(harness.session()->swapchainGenerations() > generations);
    }
    panel.stop();
    reports.checkLast();
    checkCounts("surface remade");
  }

  SECTION("a screen change and a simulated DPR change")
  {
    // A real DPR or monitor change needs a second monitor or a scale change, which the
    // user checks by hand. Here the window gets the events Qt would send: the screen
    // change re-checks the GPU's present support (R5.33) and replaces the swapchain; the
    // DPR event carries the same DPR, so it replaces nothing.
    REQUIRE(harness.startAndWaitForFrame());
    for (auto i = 0; i < 10; ++i)
    {
      CAPTURE(i);
      auto* window = findSurfaceWindow();
      REQUIRE(window != nullptr);
      const auto generations = harness.session()->swapchainGenerations();
      emit window->screenChanged(window->screen());
      REQUIRE(harness.waitForFrame());
      CHECK(harness.session()->swapchainGenerations() > generations);

      auto dprChange = QEvent{QEvent::DevicePixelRatioChange};
      QCoreApplication::sendEvent(window, &dprChange);
      REQUIRE(sessionRunning(panel));
      REQUIRE(harness.waitForFrame());
    }
    panel.stop();
    reports.checkLast();
    checkCounts("screen and DPR");
  }

  SECTION("Start pressed with the mouse leaves the keyboard where it was")
  {
    // the line edit stands for the 3D view
    harness.window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&harness.window));
    harness.lineEdit->setFocus();
    REQUIRE(QApplication::focusWidget() == harness.lineEdit);

    QTest::mouseClick(&findButton(panel, "Start"), Qt::LeftButton);
    REQUIRE(sessionRunning(panel));
    REQUIRE(harness.waitForFrame());
    CHECK(QApplication::focusWidget() == harness.lineEdit);

    auto* window = findSurfaceWindow();
    REQUIRE(window != nullptr);
    // D44
    CHECK(window->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK(!window->flags().testFlag(Qt::WindowTransparentForInput));
    auto* container = panel.findChild<QWidget*>("LitPreviewPanel_Container");
    REQUIRE(container != nullptr);
    CHECK(container->focusPolicy() == Qt::NoFocus);

    // a click on the preview, as Qt delivers it and as Windows sends it
    QTest::mouseClick(window, Qt::LeftButton, {}, QPoint{10, 10});
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
    CHECK(QApplication::focusWidget() == harness.lineEdit);
    CHECK(QGuiApplication::focusWindow() == harness.window.windowHandle());

    // a key goes through the application's routing to the focused widget
    QTest::keyClick(harness.window.windowHandle(), Qt::Key_A);
    CHECK(harness.lineEdit->text() == "a");

    // Stop with the mouse as well
    QTest::mouseClick(&findButton(panel, "Stop"), Qt::LeftButton);
    CHECK(!sessionRunning(panel));
    CHECK(QApplication::focusWidget() == harness.lineEdit);
    reports.checkLast();
    checkCounts("keyboard");
  }

  SECTION("an error or a loss posted just before Stop reaches the report (review R.1)")
  {
    for (const auto fault :
         {lit::DebugFault::NvrhiError, lit::DebugFault::DeviceLostBeforeFrame})
    {
      CAPTURE(int(fault));
      REQUIRE(harness.startAndWaitForFrame());
      auto& session = *harness.session();
      session.debugInjectFault(fault);
      harness.moveCamera();
      // the render thread posts its message; no event is processed, so none is delivered
      REQUIRE(waitWithoutEvents(
        [&] { return session.nvrhiErrorCount() > 0 || session.deviceLost(); }, 5000ms));
      std::this_thread::sleep_for(50ms);
      panel.stop();

      const auto& report = reports.last();
      CHECK(std::ranges::any_of(report.messages, [](const auto& message) {
        return message.find("(not delivered before the stop)") != std::string::npos;
      }));
      const auto problems = report.problems();
      CHECK(!problems.empty());
      if (fault == lit::DebugFault::DeviceLostBeforeFrame)
      {
        CHECK(report.deviceLost);
        CHECK(std::ranges::count(problems, std::string{"The GPU device was lost."}) == 1);
      }
      CHECK_THAT(
        messageText(panel), Catch::Matchers::ContainsSubstring("did not stop cleanly"));
    }
    REQUIRE(harness.startAndWaitForFrame());
    panel.stop();
    reports.checkLast();
    checkCounts("undelivered");
  }

  SECTION("the status line shows the session's state and its disabled programs (D9)")
  {
    // LP2.3: a game with one material and no shader blobs: the cube drawn with it makes
    // the passes ask for their programs, which are disabled, each with its reason, and
    // the line says so under the surface
    const auto game = std::filesystem::temp_directory_path() / "TbUiLibTest-lp23-status";
    std::filesystem::remove_all(game);
    const auto removeGame = kdl::invoke_later{[&] { std::filesystem::remove_all(game); }};
    std::filesystem::create_directories(game / "base" / "materials");
    {
      auto file = std::ofstream{game / "base" / "materials" / "lp23_status.mtr"};
      file << "textures/lp23/status\n{\n\tdiffusemap\ttextures/lp23/status.tga\n}\n";
    }
    auto& map = harness.fixture.document().map();
    map.setGamePath(game);
    auto* brushNode = mdl::createBrushNode(map, "lp23/status");
    mdl::addNodes(map, {{map.editorContext().currentLayer(), {brushNode}}});

    REQUIRE(harness.startAndWaitForFrame());
    auto* status = panel.findChild<QLabel*>("LitPreviewPanel_Status");
    REQUIRE(status != nullptr);
    // in front of the cube at the origin, looking at it, moved by a unit back and forth
    // so that frames are drawn
    harness.camera.setDirection(vm::vec3f{1, 0, 0}, vm::vec3f{0, 0, 1});
    // the line names every program the session reports, each with its reason
    const auto namesEveryProgram = [&](const QString& text) {
      const auto programs = harness.session()->programReport();
      return !programs.empty()
             && std::ranges::all_of(programs, [&](const lit::ProgramReport& program) {
                  return !program.enabled
                         && text.contains(QString::fromStdString(
                           "disabled: " + program.name + " (" + program.reason + ")"));
                });
    };
    REQUIRE(harness.session() != nullptr);
    auto nudge = 0;
    const auto disabledShown = QTest::qWaitFor(
      [&] {
        ++nudge;
        harness.camera.moveTo(vm::vec3f{-128.0f - float(nudge % 2), 0, 0});
        harness.fixture.context().cameraDidChangeNotifier(harness.camera);
        return namesEveryProgram(status->text());
      },
      10000);
    const auto text = status->text();
    CAPTURE(text.toStdString());
    CHECK(disabledShown);
    CHECK(text.contains("disabled: builtin/depth ("));
    CHECK((text.startsWith("settled | ") || text.startsWith("drawing | ")));
    CHECK(text.contains(" draws"));
    CHECK(text.contains("VRAM "));
    // LP4.1 (D90): the document has no .proc, so nothing of the portal flow
    CHECK_FALSE(text.contains("| area "));

    panel.stop();
    CHECK(status->text().isEmpty());
    reports.checkLast();
    checkCounts("status line");
  }

  SECTION("the status line's bake notices (LP4.3, D90, K8, K29)")
  {
    // a saved document in a game folder with a .proc of two areas (x > 0, y > 0 area 0,
    // y <= 0 area 1) whose half x <= 0 is solid, so that half of the cube at the origin
    // reaches solid (K14: pieces outside the baked hull)
    const auto game = std::filesystem::temp_directory_path() / "TbUiLibTest-lp43-bake";
    std::filesystem::remove_all(game);
    const auto removeGame = kdl::invoke_later{[&] { std::filesystem::remove_all(game); }};
    std::filesystem::create_directories(game / "base" / "materials");
    std::filesystem::create_directories(game / "base" / "maps");
    {
      auto file = std::ofstream{game / "base" / "materials" / "lp43_bake.mtr"};
      file << "textures/lp43/bake\n{\n\tdiffusemap\ttextures/lp43/bake.tga\n}\n";
    }
    auto& map = harness.fixture.document().map();
    map.setGamePath(game);
    auto* brushNode = mdl::createBrushNode(map, "lp43/bake");
    mdl::addNodes(map, {{map.editorContext().currentLayer(), {brushNode}}});
    const auto mapPath = game / "base" / "maps" / "lp43_bake.map";
    REQUIRE(map.saveAs(mapPath).is_success());
    REQUIRE_FALSE(map.modified());
    const auto procPath = game / "base" / "maps" / "lp43_bake.proc";
    {
      auto file = std::ofstream{procPath};
      file << "mapProcFile004\n\ninterAreaPortals { 2 0 }\n\nnodes { 2\n( 1 0 0 0 ) 1 "
              "0\n( 0 1 0 0 ) -1 -2\n}\n";
    }
    auto* status = panel.findChild<QLabel*>("LitPreviewPanel_Status");
    REQUIRE(status != nullptr);
    const auto waitForText = [&](const std::function<bool(const QString&)>& wanted) {
      return QTest::qWaitFor(
        [&] {
          harness.moveCamera();
          return wanted(status->text());
        },
        15000);
    };
    using namespace std::chrono_literals;

    // the .proc newer than the map file, nothing unsaved: "bake: proc" and the pieces in
    // solid, not stale
    std::filesystem::last_write_time(
      procPath, std::filesystem::last_write_time(mapPath) + 100s);
    REQUIRE(harness.startAndWaitForFrame());
    CHECK(waitForText([](const QString& text) {
      return text.contains("| bake: proc, ") && text.contains(" outside the baked hull");
    }));
    auto text = status->text();
    CAPTURE(text.toStdString());
    CHECK_FALSE(text.contains("stale"));
    CHECK(text.contains("| area "));
    // an edit: unsaved changes make the bake stale
    auto* second = mdl::createBrushNode(map, "lp43/bake");
    mdl::addNodes(map, {{map.editorContext().currentLayer(), {second}}});
    REQUIRE(map.modified());
    CHECK(
      waitForText([](const QString& t) { return t.contains("| bake: proc, stale, "); }));
    panel.stop();
    reports.checkLast();

    // saved again, the .proc now older than the map file: stale by the time
    REQUIRE(map.saveAs(mapPath).is_success());
    REQUIRE_FALSE(map.modified());
    std::filesystem::last_write_time(
      procPath, std::filesystem::last_write_time(mapPath) - 100s);
    REQUIRE(harness.startAndWaitForFrame());
    CHECK(
      waitForText([](const QString& t) { return t.contains("| bake: proc, stale, "); }));
    panel.stop();
    reports.checkLast();

    // a light grid with one entry too few: the bake dropped with the reason (K29)
    {
      auto file = std::ofstream{game / "base" / "maps" / "lp43_bake.lightgrid"};
      file << "LGRID \"4\"\n\nlightGridPoints { 0 1 16 2 ( 32 32 0 ) ( 64 64 128 ) 1 1 1 "
              "1 ( 32 32 0 ) }\n";
    }
    REQUIRE(harness.startAndWaitForFrame());
    CHECK(waitForText([](const QString& t) {
      return t.contains(
        "| bake: dropped (the light grid has 1 entries for 1 distinct areas, the map has "
        "2 "
        "areas)");
    }));
    text = status->text();
    CAPTURE(text.toStdString());
    panel.stop();
    CHECK(status->text().isEmpty());
    reports.checkLast();
    mdl::removeNodes(map, {brushNode, second});
    checkCounts("bake notices");
  }

  SECTION("the material service reads the game and logs to the document")
  {
    auto logger = CapturingLogger{};
    harness.fixture.document().setTargetLogger(&logger);
    const auto resetLogger =
      kdl::invoke_later{[&] { harness.fixture.document().setTargetLogger(nullptr); }};

    // between the phases too: the parse thread starts with phase A
    panel.start();
    REQUIRE(panel.materialsForTesting() != nullptr);
    panel.stop();
    CHECK(panel.materialsForTesting() == nullptr);
    reports.checkLast();

    REQUIRE(harness.startAndWaitForFrame());
    auto* materials = panel.materialsForTesting();
    REQUIRE(materials != nullptr);
    // C1 (LP2.1): the session's compile thread reads the same game's blobs
    REQUIRE(harness.session() != nullptr);
    CHECK(
      harness.session()->optionsForTesting().gamePath
      == harness.fixture.document().map().gamePath());
    CHECK_FALSE(harness.session()->optionsForTesting().gamePath.empty());
    REQUIRE(materials->waitForStatsForTesting(
      [](const auto& s) { return s.parseAllDone; }, 10s));
    CHECK(materials->stats().materials > 0);
    REQUIRE(QTest::qWaitFor(
      [&] { return logger.contains(LogLevel::Info, "Lit preview materials: Parsed "); },
      5000));
    CHECK(logger.contains(LogLevel::Info, "Lit preview materials: Read "));

    panel.stop();
    CHECK(panel.materialsForTesting() == nullptr);
    reports.checkLast();
    checkCounts("materials");
  }

  SECTION("tables reach the GUI thread, a collection change reloads, the session holds")
  {
    REQUIRE(harness.startAndWaitForFrame());
    auto* materials = panel.materialsForTesting();
    REQUIRE(materials != nullptr);
    auto* session = harness.session();
    REQUIRE(session != nullptr);
    // phase B handed the service to the session, which also runs its CPU pool
    CHECK(session->materialServiceForTesting() == materials);
    CHECK(session->cpuPoolForTesting() != nullptr);

    // Published is marshalled to the GUI thread, and the panel reads the latest table
    REQUIRE(materials->waitForStatsForTesting(
      [](const auto& s) { return s.parseAllDone; }, 10s));
    REQUIRE(QTest::qWaitFor(
      [&] { return panel.materialsVersionForTesting() == materials->table()->version; },
      5000));
    CHECK(materials->stats().builds == 1u);

    // R5.24: a change of the material collections builds the registry again
    harness.fixture.document().materialCollectionsDidChangeNotifier();
    REQUIRE(materials->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u && s.parseAllDone; }, 10s));
    REQUIRE(QTest::qWaitFor(
      [&] { return panel.materialsVersionForTesting() == materials->table()->version; },
      5000));

    panel.stop();
    reports.checkLast();
    // the subscription went with the session's document observer
    harness.fixture.document().materialCollectionsDidChangeNotifier();
    CHECK(panel.materialsForTesting() == nullptr);
    checkCounts("tables, reload and the session's service and pool");
  }

  SECTION("the session takes the document's snapshots, also after an edit (LP1b.1)")
  {
    auto& document = harness.fixture.document();
    auto& map = document.map();
    REQUIRE(harness.startAndWaitForFrame());
    auto* session = harness.session();
    REQUIRE(session != nullptr);

    // S31: the first snapshot, complete, of the session's generation
    REQUIRE(QTest::qWaitFor(
      [&] {
        harness.moveCamera();
        return session->snapshotsTakenForTesting() >= 1u;
      },
      10000));
    const auto first = session->lastSnapshotForTesting();
    REQUIRE(first != nullptr);
    CHECK(first->generation == session->generation());
    CHECK(first->sequence == 1u);
    CHECK(std::ranges::any_of(first->entities, [&](const auto& entity) {
      return first->string(entity.classname) == "worldspawn";
    }));

    // an edit reaches the render thread, which takes it (nothing is drawn of it yet)
    auto* brushNode = mdl::createBrushNode(map, "lp1b/edit");
    mdl::addNodes(map, {{map.editorContext().currentLayer(), {brushNode}}});
    REQUIRE(QTest::qWaitFor(
      [&] {
        harness.moveCamera();
        const auto last = session->lastSnapshotForTesting();
        return last && last->sequence >= 2u
               && std::ranges::any_of(last->faces, [&](const auto& face) {
                    return last->string(face.material) == "lp1b/edit";
                  });
      },
      10000));
    CHECK(session->snapshotSlotFree());

    panel.stop();
    reports.checkLast();
    CHECK(reports.last().hadDevice);
    // the observer went after the render thread had stopped and before the material
    // service
    CHECK(
      reports.last().teardownSteps
      == std::vector<std::string>{
        "render thread stopped",
        "document observer destroyed",
        "GPU objects released",
        "container deleted",
        "device destroyed",
        "instance destroyed",
        "dispatcher reset",
        "materials destroyed",
      });

    // the session's subscriptions went with it
    mdl::removeNodes(map, {brushNode});
    checkCounts("snapshots");
  }

  SECTION("a document load and a game path change stop the session")
  {
    REQUIRE(harness.startAndWaitForFrame());
    harness.fixture.document().documentWasLoadedNotifier();
    CHECK(!sessionRunning(panel));
    reports.checkLast();
    CHECK(startPageShown(panel));
    CHECK(messageText(panel) == "A map was loaded. Press Start to preview it.");

    REQUIRE(harness.startAndWaitForFrame());
    auto& prefs = PreferenceManager::instance();
    auto gamePath = harness.fixture.document().map().gameInfo().gamePathPreference;
    const auto resetGamePath = kdl::invoke_later{[&] { prefs.resetToDefault(gamePath); }};
    setPref(gamePath, gamePath.defaultValue / "base");
    CHECK(!sessionRunning(panel));
    reports.checkLast();
    CHECK(messageText(panel) == "The game path changed. Press Start to preview with it.");
    checkCounts("load and game path");
  }

  validation.check();
}

TEST_CASE("LitPreviewPanel in a map window on a GPU", "[.][litgpu]")
{
  // closing a window saves its layout
  const auto settingsSnapshot = SettingsSnapshot{{"Windows"}};
  const auto validation = ValidationMessages{"map window"};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

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

  const auto perspectiveCamera = [](MapWindow& window) -> gl::PerspectiveCamera& {
    auto* mapView = window.findChild<SwitchableMapViewContainer*>();
    REQUIRE(mapView != nullptr);
    return mapView->perspectiveCamera();
  };

  const auto frames = [](const LitPreviewPanel& panel) {
    auto* session = panel.sessionForTesting();
    return session ? session->framesPresented() : 0u;
  };

  // moves the window's current 3D camera until a frame more than now is presented
  const auto waitForFrame = [&](MapWindow& window, const LitPreviewPanel& panel) {
    const auto before = frames(panel);
    auto step = 0;
    return QTest::qWaitFor(
      [&] {
        if (frames(panel) > before)
        {
          return true;
        }
        perspectiveCamera(window).moveTo(vm::vec3f{float(++step), 0.0f, 64.0f});
        return false;
      },
      10000);
  };

  SECTION("phase B starts from the 3D view's camera; a layout switch 20 times")
  {
    auto window = MapWindow{appController, createDoom3Document(appController)};
    auto& panel = showWindow(window);
    const auto reports = Reports{panel};

    // review L3.22: the camera was never moved, yet the session has the 3D view's, not a
    // default one
    REQUIRE(perspectiveCamera(window).position() != lit::CameraState{}.position);
    panel.start();
    REQUIRE(QTest::qWaitFor([&] { return frames(panel) > 0u; }, 10000));
    const auto viewport = perspectiveCamera(window).viewport();
    REQUIRE(viewport.width > 0);
    CHECK(
      panel.sessionForTesting()->camera().viewportWidth == std::uint32_t(viewport.width));
    CHECK(
      panel.sessionForTesting()->camera().position
      == perspectiveCamera(window).position());

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
      REQUIRE(sessionRunning(panel));
      REQUIRE(waitForFrame(window, panel));
      // the notifier delivers the new layout's camera
      CHECK(
        panel.sessionForTesting()->camera().position
        == perspectiveCamera(window).position());
    }
    panel.stop();
    reports.checkLast();
  }

  SECTION("closing the map window stops the session")
  {
    for (const auto held : {false, true})
    {
      CAPTURE(held);
      auto* window = new MapWindow{appController, createDoom3Document(appController)};
      const auto guard = QPointer<MapWindow>{window};
      auto& panel = showWindow(*window);
      auto reports = std::make_unique<Reports>(panel);
      // the window goes with its panel; its report arrives from the panel's destructor
      auto lastReport = std::optional<LitPreviewReport>{};
      auto connection = NotifierConnection{};
      connection += panel.sessionDidEndNotifier.connect(
        [&](const LitPreviewReport& report) { lastReport = report; });
      panel.start();
      REQUIRE(waitForFrame(*window, panel));

      auto hold = std::optional<FrameHold>{};
      if (held)
      {
        // F1: the render thread is inside its held frame when the window closes
        hold.emplace(*panel.sessionForTesting());
        perspectiveCamera(*window).moveTo(vm::vec3f{1000.0f, 0.0f, 64.0f});
        REQUIRE(hold->waitUntilEntered());
      }

      // the window deletes itself on close
      const auto start = std::chrono::steady_clock::now();
      reports.reset();
      window->close();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      CHECK(millisecondsSince(start) < StopBound);
      CHECK(guard.isNull());
      REQUIRE(lastReport);
      CHECK(lastReport->hadDevice);
      CHECK(lastReport->deviceReleaseCount == 0u);
      CHECK(lastReport->problems().empty());
      if (hold)
      {
        CHECK(hold->left());
      }
    }
  }

  validation.check();
}

#endif

} // namespace tb::ui
