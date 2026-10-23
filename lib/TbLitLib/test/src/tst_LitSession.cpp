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

#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "PipelineCompiler.h"
#include "RenderThread.h"
#include "StringMakers.h"
#include "TonemapModel.h"
#include "fs/TestEnvironment.h"
#include "lit/BprocModel.h"
#include "lit/CpuPool.h"
#include "lit/EngineBuiltins.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/FramePacer.h"
#include "lit/GameDeclSource.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/LightRecord.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/NumberText.h"
#include "lit/ProcFile.h"
#include "lit/Projection.h"
#include "lit/ShaderBlobs.h"
#include "lit/ShaderReflection.h"
#include "lit/Snapshot.h"
#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#ifdef _WIN32
#include <windows.h>
// after windows.h
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL noProcAddr(VkInstance, const char*)
{
  return nullptr;
}

} // namespace

TEST_CASE("LitSession")
{
  SECTION("create")
  {
    // what create checks before it touches Vulkan
    const auto sink = EventSink{};
    CHECK(
      LitSession::create({}, VK_NULL_HANDLE, 64, 64, {}, sink, 1)
      == Result<std::unique_ptr<LitSession>>{Error{"No Vulkan instance"}});

    // a handle without an initialised dispatcher; nothing calls through it
    const auto unloaded = InstanceHandles{
      reinterpret_cast<VkInstance>(std::uintptr_t{0x1000}), &noProcAddr, {}};
    CHECK(
      LitSession::create(unloaded, VK_NULL_HANDLE, 64, 64, {}, sink, 1)
      == Result<std::unique_ptr<LitSession>>{
        Error{"The Vulkan dispatcher is not initialised for the instance"}});
  }
}

TEST_CASE("FrameConvergence: what keeps a frame from being settled (D77 DX.2 DX.4 U2.11)")
{
  CHECK(FrameConvergence{}.settled());
  CHECK_FALSE(FrameConvergence{}.retriggers());
  using Term = bool FrameConvergence::*;
  for (const auto& [term, retriggers] : std::vector<std::pair<Term, bool>>{
         {&FrameConvergence::pipelinesPending, false},
         {&FrameConvergence::materialsPending, false},
         {&FrameConvergence::drawsSkipped, true},
         {&FrameConvergence::uploadsLeft, true},
         {&FrameConvergence::imagesPending, false},
         {&FrameConvergence::provisionalChunks, false},
         {&FrameConvergence::lutPending, false},
         {&FrameConvergence::entityDefsPending, false},
         {&FrameConvergence::lightsProvisional, false},
         {&FrameConvergence::geometryInFlight, false},
         {&FrameConvergence::drawsOverflowed, false},
         {&FrameConvergence::lightTrianglesPending, false}})
  {
    auto convergence = FrameConvergence{};
    convergence.*term = true;
    CHECK_FALSE(convergence.settled());
    CHECK(convergence.retriggers() == retriggers);
  }
  // D9: a disabled pass counts as converged
  auto disabled = FrameConvergence{};
  disabled.passesDisabled = true;
  CHECK(disabled.settled());
}

TEST_CASE("LitSession: the panel's status line (D9, LP2.3)")
{
  CHECK(sessionStatusLine({}, {}) == "starting");

  auto status = SessionStatus{};
  status.frames = 12;
  status.settled = true;
  status.lightsVisible = 1;
  status.lightsDrawn = 1;
  status.draws = 38;
  status.vramBytes = 85 * 1024 * 1024 + 1024 * 1024 / 5;
  CHECK(
    sessionStatusLine(status, {})
    == "settled | 1 of 1 light drawn | 38 draws | VRAM 85.2 MB");

  status.settled = false;
  status.lightsVisible = 3;
  status.lightsDrawn = 2;
  status.lightsOverflowed = 1;
  status.skippedDraws = 4;
  status.pendingPipelines = 2;
  const auto programs = std::vector<ProgramReport>{
    {19, "builtin/lighting/interaction", true, ""},
    {113,
     "builtin/post/histogram",
     false,
     "no shader blob builtin/post/histogram.cs.bin"}};
  CHECK(
    sessionStatusLine(status, programs)
    == "drawing | 2 of 3 lights drawn, 1 overflowed | 38 draws, 4 skipped | 2 pipelines "
       "pending | VRAM 85.2 MB | disabled: builtin/post/histogram (no shader blob "
       "builtin/post/histogram.cs.bin)");

  // a disabled program is named before the first frame too
  CHECK(
    sessionStatusLine({}, programs)
    == "starting | disabled: builtin/post/histogram (no shader blob "
       "builtin/post/histogram.cs.bin)");

  // LP4.1 (D90): the view's portal flow while a tree exists, before the disabled
  // programs; without one nothing of it
  auto flowed = SessionStatus{};
  flowed.frames = 3;
  flowed.settled = true;
  flowed.lightsVisible = 1;
  flowed.lightsDrawn = 1;
  flowed.draws = 9;
  flowed.vramBytes = 1024 * 1024;
  flowed.viewArea = 1;
  flowed.areasFlowed = 2;
  flowed.areas = 8;
  flowed.lightsInView = 2;
  CHECK(
    sessionStatusLine(flowed, {})
    == "settled | 1 of 1 light drawn | 9 draws | VRAM 1.0 MB | area 1, 2 of 8 areas");
  flowed.viewArea = -1;
  flowed.areasFlowed = 8;
  CHECK(
    sessionStatusLine(flowed, programs)
    == "settled | 1 of 1 light drawn | 9 draws | VRAM 1.0 MB | area -1, 8 of 8 areas | "
       "disabled: builtin/post/histogram (no shader blob builtin/post/histogram.cs.bin)");
  auto noTree = flowed;
  noTree.areas = 0;
  CHECK(
    sessionStatusLine(noTree, {})
    == "settled | 1 of 1 light drawn | 9 draws | VRAM 1.0 MB");
  CHECK(sessionStatusLine(noTree, {}).find("area") == std::string::npos);
}

} // namespace tb::lit

// Hidden GPU tests ("[.][litgpu]"): they need a Vulkan driver and, for the window
// section, a desktop. The Vulkan loader is loaded at runtime here, as Qt does in the
// editor (C9); the test executable never links it.

#ifdef _WIN32

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

struct VulkanLibrary
{
  HMODULE module = LoadLibraryW(L"vulkan-1.dll");
  PFN_vkGetInstanceProcAddr getInstanceProcAddr =
    module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
               reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")))
           : nullptr;

  VulkanLibrary() = default;
  VulkanLibrary(const VulkanLibrary&) = delete;
  VulkanLibrary& operator=(const VulkanLibrary&) = delete;
  ~VulkanLibrary()
  {
    if (module)
    {
      FreeLibrary(module);
    }
  }

  template <typename F>
  F get(VkInstance instance, const char* name) const
  {
    return reinterpret_cast<F>(getInstanceProcAddr(instance, name));
  }
};

// LP2.0, T0.2: the validation layer's id of "Vertex attribute at location N not consumed
// by vertex shader" (mirror.json M-PIPE-DEFECT-01), as this machine's layer names it
constexpr auto OutputNotConsumedId = std::string_view{"WARNING-Shader-OutputNotConsumed"};

// T0.2: the Khronos layer's default duplicate_message_limit, the most messages of one id
// it reports when the test cannot raise it (VK_EXT_layer_settings)
constexpr auto ValidationDuplicateMessageLimit = 10;

// T0.2: the duplicate limit the test instance asks for through VK_EXT_layer_settings, so
// that every message of one id is counted
constexpr auto RaisedDuplicateMessageLimit = std::uint32_t{100000};

// Validation-layer messages, counted through a debug-utils messenger on the test's
// instance.
struct ValidationMessages
{
  std::atomic<int> errors = 0;
  std::atomic<int> warnings = 0;
  std::mutex mutex;
  std::vector<std::string> texts;
  // LP2.0, T0.2: a warning of this message id (pMessageIdName) is counted in mirrored,
  // not in warnings: the engine's own LAYOUT_DRAW_VERT input layout (six locations,
  // RenderProgs_NVRHI.cpp:250-256) against vertex shaders that read fewer of them
  // (mirror.json M-PIPE-DEFECT-01); empty counts every message
  std::string_view mirroredMessageId;
  std::atomic<int> mirrored = 0;

  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
  {
    auto& self = *static_cast<ValidationMessages*>(user);
    const auto id =
      std::string_view{data && data->pMessageIdName ? data->pMessageIdName : ""};
    if (
      !(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      && !self.mirroredMessageId.empty() && id == self.mirroredMessageId)
    {
      ++self.mirrored;
      return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
      ++self.errors;
    }
    else
    {
      ++self.warnings;
    }
    const auto lock = std::scoped_lock{self.mutex};
    self.texts.emplace_back(data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
  }

  void print(const std::string_view label)
  {
    const auto lock = std::scoped_lock{mutex};
    std::cout << label << ": validation errors " << errors << ", warnings " << warnings
              << ", mirrored engine warnings " << mirrored << "\n";
    for (const auto& text : texts)
    {
      std::cout << "  " << text << "\n";
    }
  }
};

// The test's own Vulkan instance, created as QVulkanInstance creates one: API 1.2, the
// requested extensions that the loader supports, the validation layer if present.
class TestInstance
{
public:
  TestInstance(
    const VulkanLibrary& library,
    const bool validation,
    const bool surfaceExtensions,
    ValidationMessages& messages)
    : m_library{library}
  {
    const auto createInstance =
      m_library.get<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
    const auto enumerateExtensions =
      m_library.get<PFN_vkEnumerateInstanceExtensionProperties>(
        nullptr, "vkEnumerateInstanceExtensionProperties");
    const auto enumerateLayers = m_library.get<PFN_vkEnumerateInstanceLayerProperties>(
      nullptr, "vkEnumerateInstanceLayerProperties");

    auto count = std::uint32_t{0};
    enumerateExtensions(nullptr, &count, nullptr);
    auto supported = std::vector<VkExtensionProperties>(count);
    enumerateExtensions(nullptr, &count, supported.data());

    // what the session needs beyond the surface: the debug messenger with validation; the
    // engine also asks for VK_KHR_get_physical_device_properties2 (core since 1.1),
    // VK_EXT_sampler_filter_minmax (a device extension) and VK_EXT_debug_report (replaced
    // by debug utils), see mirror.json
    auto wanted = std::vector<const char*>{};
    if (validation)
    {
      wanted.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (surfaceExtensions)
    {
      wanted.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
      wanted.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }
    auto enabled = std::vector<const char*>{};
    for (const auto* name : wanted)
    {
      if (std::ranges::any_of(supported, [&](const auto& e) {
            return std::string_view{e.extensionName} == name;
          }))
      {
        enabled.push_back(name);
        m_extensions.emplace_back(name);
      }
    }

    auto layers = std::vector<const char*>{};
    if (validation)
    {
      enumerateLayers(&count, nullptr);
      auto available = std::vector<VkLayerProperties>(count);
      enumerateLayers(&count, available.data());
      if (std::ranges::any_of(available, [](const auto& l) {
            return std::string_view{l.layerName} == "VK_LAYER_KHRONOS_validation";
          }))
      {
        layers.push_back("VK_LAYER_KHRONOS_validation");
      }
      m_validationLayer = !layers.empty();
    }

    // T0.2: every message of one id is reported (the layer's own VK_EXT_layer_settings)
    auto limit = RaisedDuplicateMessageLimit;
    auto setting = VkLayerSettingEXT{};
    setting.pLayerName = "VK_LAYER_KHRONOS_validation";
    setting.pSettingName = "duplicate_message_limit";
    setting.type = VK_LAYER_SETTING_TYPE_UINT32_EXT;
    setting.valueCount = 1;
    setting.pValues = &limit;
    auto layerSettings = VkLayerSettingsCreateInfoEXT{};
    layerSettings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layerSettings.settingCount = 1;
    layerSettings.pSettings = &setting;
    if (m_validationLayer)
    {
      enumerateExtensions("VK_LAYER_KHRONOS_validation", &count, nullptr);
      auto layerExtensions = std::vector<VkExtensionProperties>(count);
      enumerateExtensions("VK_LAYER_KHRONOS_validation", &count, layerExtensions.data());
      m_messageLimitRaised = std::ranges::any_of(layerExtensions, [](const auto& e) {
        return std::string_view{e.extensionName} == VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;
      });
      if (m_messageLimitRaised)
      {
        enabled.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
      }
    }

    auto messengerInfo = VkDebugUtilsMessengerCreateInfoEXT{};
    messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = &ValidationMessages::callback;
    messengerInfo.pUserData = &messages;
    const auto useMessenger =
      validation
      && std::ranges::find(m_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
           != m_extensions.end();

    auto application = VkApplicationInfo{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "TbLitLibTest";
    application.apiVersion = VK_API_VERSION_1_2;

    auto info = VkInstanceCreateInfo{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = useMessenger ? &messengerInfo : nullptr;
    if (m_messageLimitRaised)
    {
      layerSettings.pNext = info.pNext;
      info.pNext = &layerSettings;
    }
    info.pApplicationInfo = &application;
    info.enabledLayerCount = std::uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = std::uint32_t(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    if (createInstance(&info, nullptr, &m_instance) != VK_SUCCESS)
    {
      m_instance = VK_NULL_HANDLE;
      return;
    }

    if (useMessenger)
    {
      const auto createMessenger = m_library.get<PFN_vkCreateDebugUtilsMessengerEXT>(
        m_instance, "vkCreateDebugUtilsMessengerEXT");
      createMessenger(m_instance, &messengerInfo, nullptr, &m_messenger);
    }
  }

  TestInstance(const TestInstance&) = delete;
  TestInstance& operator=(const TestInstance&) = delete;

  ~TestInstance()
  {
    if (m_messenger != VK_NULL_HANDLE)
    {
      m_library.get<PFN_vkDestroyDebugUtilsMessengerEXT>(
        m_instance, "vkDestroyDebugUtilsMessengerEXT")(m_instance, m_messenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE)
    {
      m_library.get<PFN_vkDestroyInstance>(m_instance, "vkDestroyInstance")(
        m_instance, nullptr);
    }
  }

  bool valid() const { return m_instance != VK_NULL_HANDLE; }
  bool validationLayer() const { return m_validationLayer; }
  // T0.2: the layer reports every message of an id, not ValidationDuplicateMessageLimit
  bool messageLimitRaised() const { return m_messageLimitRaised; }

  InstanceHandles handles() const
  {
    return {m_instance, m_library.getInstanceProcAddr, m_extensions};
  }

  VkInstance instance() const { return m_instance; }

private:
  const VulkanLibrary& m_library;
  VkInstance m_instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
  std::vector<std::string> m_extensions;
  bool m_validationLayer = false;
  bool m_messageLimitRaised = false;
};

struct Events
{
  std::mutex mutex;
  std::vector<std::tuple<SessionEvent, std::string, std::uint64_t>> events;

  EventSink sink()
  {
    return
      [this](
        const SessionEvent event, std::string message, const std::uint64_t generation) {
        const auto lock = std::scoped_lock{mutex};
        events.emplace_back(event, std::move(message), generation);
      };
  }

  std::size_t count(const SessionEvent event)
  {
    const auto lock = std::scoped_lock{mutex};
    return std::size_t(std::ranges::count_if(
      events, [&](const auto& entry) { return std::get<0>(entry) == event; }));
  }

  std::size_t countContaining(const SessionEvent event, const std::string_view text)
  {
    const auto lock = std::scoped_lock{mutex};
    return std::size_t(std::ranges::count_if(events, [&](const auto& entry) {
      return std::get<0>(entry) == event
             && std::get<1>(entry).find(text) != std::string::npos;
    }));
  }

  bool allOfGeneration(const std::uint64_t generation)
  {
    const auto lock = std::scoped_lock{mutex};
    return std::ranges::all_of(
      events, [&](const auto& entry) { return std::get<2>(entry) == generation; });
  }

  void print(const std::string_view label)
  {
    const auto lock = std::scoped_lock{mutex};
    for (const auto& [event, message, generation] : events)
    {
      if (
        event != SessionEvent::Ready && event != SessionEvent::Parked
        && event != SessionEvent::ReadyForSnapshot)
      {
        std::cout << label << " event " << int(event) << " (gen " << generation
                  << "): " << message << "\n";
      }
    }
  }
};

// U2.12: the process's threads and OS handles, as TbUiLibTest's checkCounts counts them
// (tst_LitPreviewPanel.cpp), with its tolerance and the driver's measured cost of one
// device (one handle)
struct ProcessCounts
{
  long threads = 0;
  long handles = 0;
};

constexpr auto HandleTolerance = 8L;
constexpr auto DriverHandlesPerSession = 1L;

ProcessCounts processCounts()
{
  auto counts = ProcessCounts{};
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
      counts.threads += entry.th32OwnerProcessID == processId ? 1 : 0;
    }
    CloseHandle(snapshot);
  }
  return counts;
}

void pumpMessages()
{
  auto message = MSG{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

template <typename Predicate>
bool waitFor(
  Predicate predicate, const std::chrono::milliseconds timeout, const bool pump)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    if (pump)
    {
      pumpMessages();
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

CameraState cameraLookingAt(const float x, const float y, const float z)
{
  auto camera = CameraState{};
  camera.direction = vm::vec3f{x, y, z};
  camera.viewportWidth = 64;
  camera.viewportHeight = 48;
  return camera;
}

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

// how long a stop may take (as in TbUiLibTest's panel tests)
constexpr auto StopBound = 2000ms;

void teardown(LitSession& session)
{
  // a stop the render thread misses fails the check instead of hanging the join
  // (plans/lit-preview/19-LITGPU-HANG.md); stopRenderThread requests it again
  session.requestStop();
  CHECK(session.waitForRenderThread(5000ms));
  session.stopRenderThread();
  session.releaseGpuObjects();
  session.destroyVkDevice();
}

class TestWindow
{
public:
  TestWindow(const int width, const int height)
  {
    auto windowClass = WNDCLASSEXW{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"TbLitLibTestWindow";
    RegisterClassExW(&windowClass);

    auto rect = RECT{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    m_window = CreateWindowExW(
      0,
      L"TbLitLibTestWindow",
      L"TbLitLibTest",
      WS_OVERLAPPEDWINDOW,
      100,
      100,
      rect.right - rect.left,
      rect.bottom - rect.top,
      nullptr,
      nullptr,
      GetModuleHandleW(nullptr),
      nullptr);
    if (m_window)
    {
      ShowWindow(m_window, SW_SHOWNOACTIVATE);
      UpdateWindow(m_window);
      pumpMessages();
    }
  }

  TestWindow(const TestWindow&) = delete;
  TestWindow& operator=(const TestWindow&) = delete;

  ~TestWindow()
  {
    if (m_window)
    {
      DestroyWindow(m_window);
      pumpMessages();
    }
    UnregisterClassW(L"TbLitLibTestWindow", GetModuleHandleW(nullptr));
  }

  HWND handle() const { return m_window; }

  std::pair<std::uint32_t, std::uint32_t> clientSize() const
  {
    auto rect = RECT{};
    GetClientRect(m_window, &rect);
    return {std::uint32_t(rect.right - rect.left), std::uint32_t(rect.bottom - rect.top)};
  }

  void resize(const int width, const int height)
  {
    auto rect = RECT{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(
      m_window,
      nullptr,
      0,
      0,
      rect.right - rect.left,
      rect.bottom - rect.top,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pumpMessages();
  }

private:
  HWND m_window = nullptr;
};

// The GUI thread's side of the surface handshake (§3.1): park, pumping this thread's
// messages while it waits, then replace the swapchain and unpark.
Result<void> replaceSwapchain(
  LitSession& session, const VkSurfaceKHR surface, const TestWindow& window)
{
  if (!waitFor([&] { return session.park(100ms); }, 5000ms, true))
  {
    return Error{"the render thread did not park"};
  }
  session.destroySwapchain();
  const auto [width, height] = window.clientSize();
  auto result = session.createSwapchain(surface, width, height);
  session.unpark();
  return result;
}

} // namespace

// Each section runs the session on this machine's GPU and checks that teardown leaves
// no NVRHI reference, no NVRHI error and no validation error behind.
TEST_CASE("LitSession on a GPU", "[.][litgpu]")
{
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  SECTION("headless: 20 cycles of create, clear with a timer query, teardown")
  {
    const auto validation = GENERATE(false, true);
    auto messages = ValidationMessages{};
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-headless-frame-log.csv";
    std::filesystem::remove(logPath);
    auto deviceName = std::string{};

    {
      const auto instance = TestInstance{library, validation, false, messages};
      REQUIRE(instance.valid());
      if (validation)
      {
        CHECK(instance.validationLayer());
      }
      REQUIRE(initInstanceDispatcher(instance.handles()));

      for (std::uint64_t cycle = 0; cycle < 20; ++cycle)
      {
        CAPTURE(validation, cycle);
        auto events = Events{};
        auto options = SessionOptions{};
        options.validation = validation;
        options.frameLogPath = cycle == 0 ? logPath : std::filesystem::path{};

        auto session =
          LitSession::create(
            instance.handles(), VK_NULL_HANDLE, 64, 48, options, events.sink(), cycle + 1)
          | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
        deviceName = session->deviceName();
        CHECK(session->generation() == cycle + 1);
        CHECK_FALSE(session->hasSwapchain());

        // idle: nothing is drawn until the window is exposed
        session->requestFrame();
        std::this_thread::sleep_for(50ms);
        CHECK(session->framesPresented() == 0u);

        session->setExposed(true);
        for (std::uint64_t frame = 1; frame <= 3; ++frame)
        {
          session->setCamera(
            cameraLookingAt(float(frame) / 3.0f, -1.0f, float(cycle) / 20.0f));
          REQUIRE(
            waitFor([&] { return session->framesPresented() >= frame; }, 5000ms, false));
        }

        // no trigger, no frame
        const auto settled = session->framesPresented();
        std::this_thread::sleep_for(100ms);
        CHECK(session->framesPresented() == settled);

        // a headless session parks and unparks without a swapchain
        CHECK(session->park());
        CHECK(session->isParked());
        session->unpark();
        REQUIRE(
          waitFor([&] { return session->framesPresented() > settled; }, 5000ms, false));

        const auto frames = session->framesPresented();
        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->nvrhiErrorCount() == 0u);
        CHECK_FALSE(session->deviceLost());
        CHECK(events.count(SessionEvent::Ready) == 1u);
        CHECK(events.count(SessionEvent::Error) == 0u);
        CHECK(events.count(SessionEvent::DeviceLost) == 0u);
        CHECK(events.count(SessionEvent::Warning) == 0u);
        CHECK(events.allOfGeneration(cycle + 1));

        // a clean teardown, reported to the caller rather than posted
        const auto report = session->teardownReport();
        CHECK(report.deviceReleaseCount == 0u);
        CHECK(report.nvrhiErrors == 0u);
        CHECK_FALSE(report.deviceLost);
        CHECK_FALSE(report.frameLogError);
        CHECK(report.messages.empty());
        CHECK(report.framesPresented == frames);
        events.print("headless");
      }
    }
    CHECK(resetDispatcher());

    messages.print(validation ? "headless, validation" : "headless");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    std::cout << "headless device: " << deviceName << "\n";

    // the first cycle's frame log: its start, every frame with its GPU time, resolved by
    // stop, and its stop
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 7u);
    CHECK(rows[0][0] == "session");
    CHECK(rows[1][1] == "start");
    CHECK(rows.back()[1] == "stop");
    for (std::size_t i = 2; i + 1 < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 42u);
      CHECK(rows[i][0] == "1");
      CHECK(rows[i][1] == "frame");
      CHECK(rows[i][2] == std::to_string(i - 2));
      CHECK_FALSE(rows[i][6].empty());
      CHECK(rows[i][7] == "headless");
      CHECK(rows[i][10] == "64");
      CHECK(rows[i][11] == "48");
      // LP2.1: the allocations, nothing uploaded, no texture or arena
      CHECK_FALSE(rows[i][14].empty());
      CHECK(rows[i][15] == "0");
      CHECK(rows[i][16] == "0");
      CHECK(rows[i][17] == "0");
    }
    // VK_EXT_memory_budget, which this machine's GPU has
    CHECK_FALSE(rows[1][12].empty());
    CHECK_FALSE(rows.back()[12].empty());
    // the allocations at the start and the stop
    CHECK_FALSE(rows[1][14].empty());
    CHECK_FALSE(rows.back()[14].empty());
    std::cout << "headless frame log, first rows:\n";
    for (std::size_t i = 0; i < std::min(rows.size(), std::size_t{4}); ++i)
    {
      for (const auto& field : rows[i])
      {
        std::cout << field << " ";
      }
      std::cout << "\n";
    }
    std::filesystem::remove(logPath);
  }

  SECTION("C30: the device facts and the allocation counter")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      auto session =
        LitSession::create(
          instance.handles(), VK_NULL_HANDLE, 64, 48, options, events.sink(), 1)
        | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

      // the facts this machine's GPU reports, and the engine's rule on them
      const auto facts = session->deviceFacts();
      std::cout << "device facts: maxPushConstantsSize " << facts.maxPushConstantsSize
                << ", vendorID 0x" << std::hex << facts.vendorID << std::dec
                << ", maxMemoryAllocationCount " << facts.maxMemoryAllocationCount
                << ", push-constant limit " << facts.pushConstantLimit << ", D24S8 "
                << facts.d24s8Supported << "\n";
      CHECK(facts.maxPushConstantsSize >= 128u);
      CHECK(facts.vendorID != 0u);
      CHECK(facts.maxMemoryAllocationCount >= 4096u);
      CHECK(
        facts.pushConstantLimit
        == std::min(facts.maxPushConstantsSize, nvrhi::c_MaxPushConstantSize));
      for (const auto layout :
           {EngineBindingLayout::ConstantBufferOnly,
            EngineBindingLayout::Texture,
            EngineBindingLayout::DrawInteraction,
            EngineBindingLayout::Tonemap,
            EngineBindingLayout::Histogram,
            EngineBindingLayout::Exposure})
      {
        std::cout << "  pcEnabled(layout " << int(layout) << ") = "
                  << enginePushConstantsEnabled(
                       layout, facts.pushConstantLimit, facts.vendorID)
                  << "\n";
      }

      // NVRHI's allocations are counted: the headless target and the frame resources
      // already, a new texture adds one, and releasing it gives it back
      const auto before = session->deviceMemoryAllocations();
      CHECK(before > 0);
      auto* device = session->nvrhiDeviceForTesting();
      REQUIRE(device);
      auto texture = device->createTexture(nvrhi::TextureDesc{}
                                             .setWidth(256)
                                             .setHeight(256)
                                             .setFormat(nvrhi::Format::RGBA8_UNORM)
                                             .setDebugName("C30 counter test"));
      REQUIRE(texture);
      const auto withTexture = session->deviceMemoryAllocations();
      CHECK(withTexture == before + 1);
      texture = nullptr;
      CHECK(session->deviceMemoryAllocations() == before);
      std::cout << "device memory allocations: " << before << ", with a texture "
                << withTexture << "\n";

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      // every allocation is freed once the NVRHI device is gone
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("device facts, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("one live session in a process, and the dispatcher stays while it lives")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto first = LitSession::create(
                     instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 1)
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

      // review L2.10: a second session would share the process-wide dispatcher
      CHECK(
        LitSession::create(
          instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 2)
        == Result<std::unique_ptr<LitSession>>{
          Error{"Another lit preview session runs in this process"}});
      CHECK_FALSE(initInstanceDispatcher(instance.handles()));
      CHECK_FALSE(resetDispatcher());
      CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice != nullptr);

      teardown(*first);
      first.reset();

      // the slot is free again
      auto second = LitSession::create(
                      instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 3)
                    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      teardown(*second);
    }
    CHECK(resetDispatcher());
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice == nullptr);
    messages.print("one session, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("an injected fault ends drawing and tears down cleanly")
  {
    // a lost device is reported as lost; NVRHI's other errors and a failed frame are
    // errors, after which garbage is still collected (review L2.9)
    const auto fault = GENERATE(
      DebugFault::DeviceLostBeforeFrame,
      DebugFault::DeviceLostAfterAcquire,
      DebugFault::DeviceRemovedInSubmit,
      DebugFault::NvrhiError,
      DebugFault::ThrowWhileRecording);
    const auto lost = fault == DebugFault::DeviceLostBeforeFrame
                      || fault == DebugFault::DeviceLostAfterAcquire
                      || fault == DebugFault::DeviceRemovedInSubmit;
    const auto throughCallback =
      fault == DebugFault::DeviceRemovedInSubmit || fault == DebugFault::NvrhiError;
    CAPTURE(int(fault));

    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      auto session =
        LitSession::create(
          instance.handles(), VK_NULL_HANDLE, 32, 32, options, events.sink(), 7)
        | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1; }, 5000ms, false));

      session->debugInjectFault(fault);
      session->requestFrame();
      const auto expected = lost ? SessionEvent::DeviceLost : SessionEvent::Error;
      REQUIRE(waitFor([&] { return events.count(expected) == 1; }, 5000ms, false));
      CHECK(session->deviceLost() == lost);

      // nothing more is drawn
      std::this_thread::sleep_for(50ms);
      const auto frames = session->framesPresented();
      session->setCamera(cameraLookingAt(0, 0, 1));
      std::this_thread::sleep_for(150ms);
      CHECK(session->framesPresented() == frames);

      // the handshake still answers
      CHECK(session->park());
      session->unpark();

      teardown(*session);
      const auto report = session->teardownReport();
      CHECK(report.deviceReleaseCount == 0u);
      CHECK(report.deviceLost == lost);
      CHECK(report.nvrhiErrors == (throughCallback ? 1u : 0u));
      CHECK(events.count(SessionEvent::DeviceLost) == (lost ? 1u : 0u));
      CHECK(events.count(SessionEvent::Error) == (lost ? 0u : 1u));
      CHECK(events.allOfGeneration(7));
      events.print("fault");
    }
    CHECK(resetDispatcher());
    messages.print("fault, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("a stop that comes while an idle pass has released the mutex ends the thread")
  {
    // plans/lit-preview/19-LITGPU-HANG.md: a pass that draws no frame releases the render
    // thread's mutex for the chunk jobs; a stop requested in that gap notifies nobody,
    // and the pass must still see it instead of waiting without a deadline
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto session = LitSession::create(
                       instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 9)
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

      // the hook holds the next idle pass in the gap until the test releases it, once
      struct Gate
      {
        std::mutex mutex;
        std::condition_variable changed;
        bool used = false;
        bool entered = false;
        bool released = false;
      };
      auto gate = std::make_shared<Gate>();
      session->debugSetHook(DebugHook::IdleUnlocked, [gate]() {
        auto lock = std::unique_lock{gate->mutex};
        if (std::exchange(gate->used, true))
        {
          return;
        }
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait_for(lock, 5s, [&] { return gate->released; });
      });

      // not exposed: the pass draws nothing, and nothing else is pending
      session->requestFrame();
      {
        auto lock = std::unique_lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 5s, [&] { return gate->entered; }));
      }
      session->requestStop();
      {
        const auto lock = std::scoped_lock{gate->mutex};
        gate->released = true;
      }
      gate->changed.notify_all();
      CHECK(session->waitForRenderThread(2000ms));

      session->debugSetHook(DebugHook::IdleUnlocked, {});
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("stop in the idle gap, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("the snapshot inbox, the CPU pool and the material service")
  {
    auto messages = ValidationMessages{};
    auto poolThreads = std::make_shared<std::set<DWORD>>();
    // B0.14 (c): handles of the pool's threads, duplicated by the threads themselves, so
    // that a later thread with a recycled id is never the one waited for
    auto poolHandles = std::make_shared<std::vector<HANDLE>>();
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto serviceOptions = MaterialServiceOptions{};
      serviceOptions.pollInterval = 0ms;
      auto service = MaterialService::create(std::move(serviceOptions))
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      auto session = LitSession::create(
                       instance.handles(),
                       VK_NULL_HANDLE,
                       32,
                       32,
                       options,
                       events.sink(),
                       5,
                       service.get())
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      CHECK(session->materialServiceForTesting() == service.get());
      REQUIRE(session->cpuPoolForTesting() != nullptr);

      // S31: the render thread starts with an empty inbox and says so
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) == 1u; },
        5000ms,
        false));

      const auto snapshotOf = [](const std::uint64_t generation, const std::uint64_t n) {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->generation = generation;
        snapshot->sequence = n;
        return std::shared_ptr<const Snapshot>{std::move(snapshot)};
      };

      // D53: none, and another session's, are not taken
      CHECK_FALSE(session->offerSnapshot(nullptr));
      CHECK_FALSE(session->offerSnapshot(snapshotOf(4, 1)));

      // one in flight: a hidden window draws no frame, so the first one waits
      const auto first = snapshotOf(5, 1);
      const auto second = snapshotOf(5, 2);
      CHECK(session->offerSnapshot(first));
      CHECK_FALSE(session->offerSnapshot(second));
      std::this_thread::sleep_for(50ms);
      CHECK(session->snapshotsTakenForTesting() == 0u);

      // the next frame's start takes it, and the render thread is ready again
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return session->snapshotsTakenForTesting() == 1u; }, 5000ms, false));
      CHECK(session->lastSnapshotForTesting() == first);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) == 2u; },
        5000ms,
        false));
      CHECK(session->offerSnapshot(second));
      REQUIRE(waitFor(
        [&] { return session->snapshotsTakenForTesting() == 2u; }, 5000ms, false));
      CHECK(session->lastSnapshotForTesting() == second);

      // S3: jobs run on the pool's threads, which are not the render thread's
      auto mutex = std::make_shared<std::mutex>();
      auto futures = std::vector<std::future<CpuJobResult<DWORD>>>{};
      for (auto i = 0; i < 4; ++i)
      {
        futures.push_back(session->cpuPoolForTesting()->run(
          [mutex, poolThreads, poolHandles](const CpuJobContext&) -> Result<DWORD> {
            std::this_thread::sleep_for(20ms);
            const auto id = GetCurrentThreadId();
            const auto lock = std::scoped_lock{*mutex};
            if (poolThreads->insert(id).second)
            {
              auto handle = HANDLE{};
              if (DuplicateHandle(
                    GetCurrentProcess(),
                    GetCurrentThread(),
                    GetCurrentProcess(),
                    &handle,
                    SYNCHRONIZE,
                    FALSE,
                    0))
              {
                poolHandles->push_back(handle);
              }
            }
            return id;
          }));
      }
      for (auto& future : futures)
      {
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        CHECK(future.get().result.is_success());
      }
      CHECK(poolThreads->size() <= CpuPool::ThreadCount);
      CHECK_FALSE(poolThreads->contains(GetCurrentThreadId()));

      // teardown step 1 stops the pool and forgets the service, and takes no snapshot
      teardown(*session);
      CHECK(session->cpuPoolForTesting() == nullptr);
      CHECK(session->materialServiceForTesting() == nullptr);
      CHECK_FALSE(session->offerSnapshot(snapshotOf(5, 3)));
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(events.count(SessionEvent::ReadyForSnapshot) == 3u);
      CHECK(events.allOfGeneration(5));
      session.reset();
      service.reset();
    }
    CHECK(resetDispatcher());

    // the pool's threads are joined
    CHECK(poolHandles->size() == poolThreads->size());
    for (const auto handle : *poolHandles)
    {
      CHECK(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0);
      CloseHandle(handle);
    }
    messages.print("snapshots and pool, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("R2.1: a stop while a batch longer than the bound runs ends within the bound")
  {
    // V.9: 10,000 boxes over 16 cells, a batch of about 3 s in Debug, longer than the
    // stop bound: 16 chunk jobs queue on the pool's two threads when the render thread
    // takes the snapshot, and without the pool's cancel the stop would wait for them
    auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
    for (auto i = 0; i < 10000; ++i)
    {
      const auto x = (i % 4) * 1024 + ((i / 16) % 10) * 96 + 32;
      const auto y = ((i / 4) % 4) * 1024 + ((i / 160) % 10) * 96 + 32;
      const auto z = (i / 1600) * 96;
      text += boxBrush(x, y, z, x + 48, y + 48, z + 48, "lp1b/wall");
    }
    text += "}\n";
    auto snapshot = std::make_shared<Snapshot>(snapshotOfMapText(text));
    snapshot->generation = 11;
    snapshot->sequence = 1;

    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = LitSession::create(
                       instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 11)
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

      REQUIRE(session->offerSnapshot(snapshot));
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return session->snapshotsTakenForTesting() == 1u; }, 5000ms, false));
      // the batch still runs
      CHECK(session->geometryBatchesForTesting() == 0u);

      // teardown step 1: the stop cancels the pool, so the render thread's exit does not
      // wait for the batch
      const auto start = std::chrono::steady_clock::now();
      session->requestStop();
      CHECK(session->waitForRenderThread(StopBound));
      session->stopRenderThread();
      const auto took = std::chrono::duration<double, std::milli>{
        std::chrono::steady_clock::now() - start};
      std::cout << "R2.1: the stop with a 10,000-brush batch running took "
                << took.count() << " ms\n";
      CHECK(took < StopBound);
      session->releaseGpuObjects();
      session->destroyVkDevice();
      CHECK(session->lastDeviceReleaseCount() == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("stop with a batch queued, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("R2.2: a published material table wakes a render thread that waits for nothing")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      // the parse thread waits at the gate after its first build, so every lookup of the
      // first snapshot stays pending
      auto gate = std::make_shared<ParseGate>();
      gate->close();
      auto serviceOptions = MaterialServiceOptions{};
      serviceOptions.makeSource = [](const std::atomic<bool>&) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
            "materials/lp1bc.mtr",
            readTextFile(litFixtureFolder() / "materials" / "lp1bc.mtr"),
            0}})};
      };
      serviceOptions.pollInterval = 0ms;
      serviceOptions.beforeTakeWorkForTesting = [gate] { gate->pass(); };
      auto service = MaterialService::create(std::move(serviceOptions))
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      // destroyed before the service, so that its join never waits at the gate
      const auto atExit = OpenAtExit{gate};
      REQUIRE(service->waitForTableForTesting(
        [](const MaterialTable& t) { return t.registry != 0; }, 10s));

      auto events = Events{};
      auto session = LitSession::create(
                       instance.handles(),
                       VK_NULL_HANDLE,
                       16,
                       16,
                       {},
                       events.sink(),
                       12,
                       service.get())
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      auto passes = std::make_shared<std::atomic<int>>(0);
      session->debugSetHook(DebugHook::IdleUnlocked, [passes] { ++*passes; });

      auto snapshot =
        std::make_shared<Snapshot>(snapshotOfMapText(
          "{\n\"classname\" \"worldspawn\"\n" + boxBrush(0, 0, 0, 64, 64, 64, "lp1b/wall")
          + boxBrush(128, 0, 0, 160, 32, 32, "lp1b/grate")
          + "}\n{\n\"classname\" \"light\"\n\"origin\" \"32 32 96\"\n"
            "\"light_radius\" \"64 64 64\"\n}\n"));
      snapshot->generation = 12;
      snapshot->sequence = 1;
      const auto light = std::find_if(
        snapshot->nodes.begin(), snapshot->nodes.end(), [](const SnapshotNode& node) {
          return node.kind == SnapshotNodeKind::Light;
        });
      REQUIRE(light != snapshot->nodes.end());
      const auto lightId = light->entity;
      REQUIRE(session->offerSnapshot(snapshot));
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return session->geometryBatchesForTesting() >= 1u; }, 10000ms, false));

      // about a second after the last frame the idle work is done and the wait has no
      // deadline: no pass for 300 ms
      std::this_thread::sleep_for(IdleCollectInterval + 200ms);
      REQUIRE(waitFor(
        [&] {
          const auto before = passes->load();
          std::this_thread::sleep_for(300ms);
          return passes->load() == before;
        },
        10000ms,
        false));
      const auto batches = session->geometryBatchesForTesting();
      const auto idlePasses = passes->load();

      // the table arrives; its owner tells the session, as the host does on
      // MaterialsPublished
      gate->open();
      REQUIRE(service->waitForTableForTesting(
        [](const MaterialTable& t) {
          return t.find("textures/lp1b/wall") != nullptr
                 && t.find("textures/lp1b/grate") != nullptr;
        },
        10s));
      const auto published = std::chrono::steady_clock::now();
      session->tableChanged();
      CHECK(waitFor([&] { return passes->load() > idlePasses; }, 100ms, false));
      std::cout << "R2.2: the render thread's first pass came "
                << std::chrono::duration<
                     double,
                     std::milli>{std::chrono::steady_clock::now() - published}
                     .count()
                << " ms after tableChanged\n";
      // and the chunks built while the materials were pending are built again
      CHECK(waitFor(
        [&] { return session->geometryBatchesForTesting() > batches; }, 10000ms, false));
      // so is the light, whose entityDef the table now answers (R2.9: read from the
      // session, on this thread)
      CHECK(waitFor(
        [&] {
          const auto record = session->lightTable()->find(lightId);
          return record && !record->pendingEntityDef;
        },
        10000ms,
        false));

      session->debugSetHook(DebugHook::IdleUnlocked, {});
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      // R2.9: the records outlive the render thread
      CHECK(session->lightTable()->find(lightId) != nullptr);
      session.reset();
    }
    CHECK(resetDispatcher());
    messages.print("a table wakes the render thread, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("a window: present, resize, minimise, restore")
  {
    const auto forceFifo = GENERATE(false, true);
    auto messages = ValidationMessages{};
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-window-frame-log.csv";
    std::filesystem::remove(logPath);

    {
      auto window = TestWindow{320, 240};
      REQUIRE(window.handle() != nullptr);

      const auto instance = TestInstance{library, true, true, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      const auto createSurface = library.get<PFN_vkCreateWin32SurfaceKHR>(
        instance.instance(), "vkCreateWin32SurfaceKHR");
      REQUIRE(createSurface != nullptr);
      auto surfaceInfo = VkWin32SurfaceCreateInfoKHR{};
      surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
      surfaceInfo.hinstance = GetModuleHandleW(nullptr);
      surfaceInfo.hwnd = window.handle();
      auto surface = VkSurfaceKHR{VK_NULL_HANDLE};
      REQUIRE(
        createSurface(instance.instance(), &surfaceInfo, nullptr, &surface)
        == VK_SUCCESS);

      for (std::uint64_t cycle = 0; cycle < 3; ++cycle)
      {
        CAPTURE(forceFifo, cycle);
        window.resize(320, 240);

        auto events = Events{};
        auto options = SessionOptions{};
        options.validation = true;
        options.forceFifo = forceFifo;
        options.frameLogPath = cycle == 0 ? logPath : std::filesystem::path{};

        const auto [width, height] = window.clientSize();
        auto session = LitSession::create(
                         instance.handles(),
                         surface,
                         width,
                         height,
                         options,
                         events.sink(),
                         100 + cycle)
                       | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
        REQUIRE(session->hasSwapchain());
        if (cycle == 0)
        {
          std::cout << "window device: " << session->deviceName() << "\n";
        }

        // what the GUI thread checks again after a screen change (R5.33)
        REQUIRE(session->physicalDevice() != VK_NULL_HANDLE);
        auto presents = VkBool32{VK_FALSE};
        CHECK(
          library.get<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            instance.instance(), "vkGetPhysicalDeviceSurfaceSupportKHR")(
            session->physicalDevice(), session->queueFamily(), surface, &presents)
          == VK_SUCCESS);
        CHECK(presents == VK_TRUE);

        session->setExposed(true);
        const auto drawFrames = [&](const std::uint64_t count) {
          const auto target = session->framesPresented() + count;
          for (auto i = 0; i < 200 && session->framesPresented() < target; ++i)
          {
            session->setCamera(cameraLookingAt(float(i % 7) / 7.0f, 0.5f, -0.5f));
            if (events.count(SessionEvent::SwapchainOutOfDate) > 0)
            {
              // the GUI thread's answer to a recreate request
              {
                const auto lock = std::scoped_lock{events.mutex};
                std::erase_if(events.events, [](const auto& entry) {
                  return std::get<0>(entry) == SessionEvent::SwapchainOutOfDate;
                });
              }
              replaceSwapchain(*session, surface, window)
                | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
            }
            waitFor([&] { return false; }, 20ms, true);
          }
          return session->framesPresented() >= target;
        };

        REQUIRE(drawFrames(5));

        // resize: the handshake replaces the generation
        window.resize(400, 300);
        {
          replaceSwapchain(*session, surface, window)
            | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
        }
        REQUIRE(session->hasSwapchain());
        REQUIRE(drawFrames(3));

        // minimise: a 0×0 extent makes no swapchain, and nothing is drawn
        ShowWindow(window.handle(), SW_MINIMIZE);
        pumpMessages();
        {
          replaceSwapchain(*session, surface, window)
            | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
        }
        CHECK_FALSE(session->hasSwapchain());
        const auto minimisedFrames = session->framesPresented();
        session->setCamera(cameraLookingAt(0, 0, 1));
        waitFor([&] { return false; }, 100ms, true);
        CHECK(session->framesPresented() == minimisedFrames);

        ShowWindow(window.handle(), SW_SHOWNOACTIVATE);
        pumpMessages();
        {
          replaceSwapchain(*session, surface, window)
            | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
        }
        REQUIRE(session->hasSwapchain());
        REQUIRE(drawFrames(3));

        // Review N.3: a last frame that returned SUBOPTIMAL left a request for this
        // generation, which would suppress the resume's request below; answer it first
        for (auto attempt = 0; attempt < 3 && session->swapchainReplacementRequested();
             ++attempt)
        {
          replaceSwapchain(*session, surface, window)
            | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
          REQUIRE(drawFrames(3));
        }
        REQUIRE_FALSE(session->swapchainReplacementRequested());

        // Review V.1: a park that completed released the generation's image wrappers. An
        // unpark without a new generation, as the panel's host did after a park that had
        // timed out, must draw nothing on that generation and ask for the next one.
        {
          const auto lock = std::scoped_lock{events.mutex};
          std::erase_if(events.events, [](const auto& entry) {
            return std::get<0>(entry) == SessionEvent::SwapchainOutOfDate;
          });
        }
        REQUIRE(waitFor([&] { return session->park(100ms); }, 5000ms, true));
        const auto parkedFrames = session->framesPresented();
        const auto generations = session->swapchainGenerations();
        session->unpark();
        session->setCamera(cameraLookingAt(0.3f, 0.3f, 0.3f));
        REQUIRE(waitFor(
          [&] { return events.count(SessionEvent::SwapchainOutOfDate) > 0; },
          5000ms,
          true));
        CHECK(session->swapchainReplacementRequested());
        session->setCamera(cameraLookingAt(-0.3f, 0.3f, 0.3f));
        waitFor([&] { return false; }, 100ms, true);
        CHECK(session->framesPresented() == parkedFrames);
        // drawFrames answers the request with a new generation
        REQUIRE(drawFrames(3));
        CHECK(session->swapchainGenerations() > generations);
        // the request was for the generation that is gone
        CHECK_FALSE(session->swapchainReplacementRequested());

        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->nvrhiErrorCount() == 0u);
        CHECK_FALSE(session->deviceLost());
        CHECK(events.count(SessionEvent::Ready) == 1u);
        CHECK(events.count(SessionEvent::Error) == 0u);
        CHECK(events.count(SessionEvent::DeviceLost) == 0u);
        CHECK(events.allOfGeneration(100 + cycle));
        events.print("window");
      }

      // step 6: the surface goes after the swapchains and before the instance
      library.get<PFN_vkDestroySurfaceKHR>(instance.instance(), "vkDestroySurfaceKHR")(
        instance.instance(), surface, nullptr);
    }
    CHECK(resetDispatcher());

    messages.print(forceFifo ? "window, forced FIFO" : "window");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);

    // what the first cycle presented: present modes, generations and extents, and how
    // long the presents took
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 14u);
    auto seen = std::set<std::string>{};
    auto longestPresentMs = 0.0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 42u);
      if (rows[i][1] != "frame")
      {
        continue;
      }
      if (forceFifo)
      {
        CHECK(rows[i][7] == "fifo");
      }
      longestPresentMs = std::max(longestPresentMs, std::stod(rows[i][5]));
      seen.insert(
        rows[i][7] + " generation " + rows[i][8] + " " + rows[i][10] + "x" + rows[i][11]);
    }
    std::cout << (forceFifo ? "forced FIFO" : "default")
              << " longest present call: " << longestPresentMs << " ms\n";
    std::cout << (forceFifo ? "forced FIFO" : "default") << " present modes:\n";
    for (const auto& entry : seen)
    {
      std::cout << "  " << entry << "\n";
    }
    std::cout << "  last row: ";
    for (const auto& field : rows.back())
    {
      std::cout << field << " ";
    }
    std::cout << "\n";
    std::filesystem::remove(logPath);
  }
}


// D71 (LP2.0): the compile thread's pipelines from the engine's blobs. It needs
// TB_LIT_GAME_PATH, so it is its own case (T0.3): a run without the variable passes
// every other GPU case and skips this one; the [litgpu] gate runs with it set.
TEST_CASE("LitSession pipelines on a GPU (D71)", "[.][litgpu]")
{
  const auto* gamePath = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePath || !*gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pipelines need the engine's shader blobs");
  }
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  SECTION("D71: pipelines made on the compile thread while the render thread draws")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = gamePath;
      // the section counts the pipelines it asks for (DX.5's are not asked for)
      options.requestStartPipelines = false;
      auto session =
        LitSession::create(
          instance.handles(), VK_NULL_HANDLE, 64, 48, options, events.sink(), 1)
        | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      auto* compiler = session->pipelineCompilerForTesting();
      REQUIRE(compiler);
      session->setExposed(true);

      // D9: a wrong table row (set 0's second member at another offset) disables color
      auto wrong = *engineExpectedLayout(*engineBuiltin(EngineBuiltinColor), true);
      wrong.constants->members[1].offset = 20;
      compiler->setExpectedLayoutForTesting(EngineBuiltinColor, wrong);

      const auto depth = session->deviceFacts().d24s8Supported ? nvrhi::Format::D24S8
                                                               : nvrhi::Format::D32S8;
      const auto hdr = FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, depth};
      const auto ldr = FramebufferFormats{nvrhi::Format::RGBA8_UNORM, depth};
      const auto swapchain = FramebufferFormats{nvrhi::Format::BGRA8_UNORM};
      namespace gls = engine_gls;
      const auto interaction = gls::SrcBlendOne | gls::DstBlendOne | gls::DepthMask
                               | gls::DepthFuncEqual | gls::StencilFuncAlways;
      const auto keys = std::vector<PipelineKey>{
        {PipelineRecipe::Draw, EngineBuiltinDepth, gls::Default, 0, 0.0f, hdr},
        {PipelineRecipe::Draw,
         EngineBuiltinTextureVertexColor,
         gls::Default,
         0,
         0.0f,
         hdr},
        {PipelineRecipe::Draw,
         EngineBuiltinTextureVertexColor,
         gls::PolygonOffset,
         -600,
         0.0f,
         hdr},
        {PipelineRecipe::Draw, EngineBuiltinInteraction, interaction, 0, 0.0f, hdr},
        {PipelineRecipe::Draw, EngineBuiltinPbrInteraction, interaction, 0, 0.0f, hdr},
        {PipelineRecipe::Tonemap, EngineBuiltinTonemapping, 0, 0, 0.0f, ldr},
        {PipelineRecipe::Blit, EngineBuiltinBlit, 0, 0, 0.0f, swapchain},
      };
      const auto colorKey = PipelineKey{
        PipelineRecipe::Draw,
        EngineBuiltinColor,
        gls::SrcBlendDstColor | gls::DstBlendZero,
        0,
        0.0f,
        hdr};

      // the render thread draws a frame per camera change while the compile thread makes
      // the pipelines; each request returns at once
      const auto start = session->framesPresented();
      for (const auto& key : keys)
      {
        CHECK(
          compiler->graphicsPipeline(key).state != GraphicsPipelineLookup::State::Failed);
      }
      compiler->graphicsPipeline(colorKey);
      compiler->computePipeline(EngineBuiltinHistogram);
      compiler->computePipeline(EngineBuiltinExposure);

      const auto allMade = [&] {
        for (const auto& key : keys)
        {
          if (
            compiler->graphicsPipeline(key).state != GraphicsPipelineLookup::State::Ready)
          {
            return false;
          }
        }
        return compiler->computePipeline(EngineBuiltinHistogram).state
                 == ComputePipelineLookup::State::Ready
               && compiler->computePipeline(EngineBuiltinExposure).state
                    == ComputePipelineLookup::State::Ready
               && compiler->graphicsPipeline(colorKey).state
                    != GraphicsPipelineLookup::State::Pending;
      };
      auto turn = 0;
      CHECK(waitFor(
        [&] {
          session->setCamera(cameraLookingAt(float(++turn % 7) / 7.0f, -1.0f, 0.5f));
          return allMade();
        },
        30000ms,
        false));
      CHECK(session->framesPresented() > start);

      for (const auto& key : keys)
      {
        const auto lookup = compiler->graphicsPipeline(key);
        CAPTURE(key.program, int(key.recipe), lookup.error);
        CHECK(lookup.state == GraphicsPipelineLookup::State::Ready);
        CHECK(lookup.pipeline);
        CHECK_FALSE(lookup.bindingLayouts.empty());
      }
      // the same key is the same pipeline
      CHECK(
        compiler->graphicsPipeline(keys[0]).pipeline
        == compiler->graphicsPipeline(keys[0]).pipeline);
      CHECK(
        compiler->graphicsPipeline(keys[3]).bindingLayouts.size()
        == 4u); // DRAW_INTERACTION
      CHECK(compiler->computePipeline(EngineBuiltinHistogram).pipeline);

      const auto color = compiler->graphicsPipeline(colorKey);
      CHECK(color.state == GraphicsPipelineLookup::State::Failed);
      CHECK_THAT(
        color.error,
        Catch::Matchers::StartsWith(
          "disabled: renderprogs2/spirv/builtin/color.vs.bin: the "
          "push-constant block member 1 is rpColor at 16"));

      // D77: a pipeline that arrives is a frame trigger: with the camera still, a new key
      // draws one more frame once its pipeline is made
      std::this_thread::sleep_for(100ms);
      const auto settled = session->framesPresented();
      const auto newKey = PipelineKey{
        PipelineRecipe::Draw, EngineBuiltinDepth, gls::AlphaMask, 0, 0.0f, hdr};
      compiler->graphicsPipeline(newKey);
      CHECK(waitFor(
        [&] {
          return compiler->graphicsPipeline(newKey).state
                   == GraphicsPipelineLookup::State::Ready
                 && session->framesPresented() > settled;
        },
        5000ms,
        false));

      const auto report = session->programReport();
      CHECK(report.size() == 9u);
      for (const auto& program : report)
      {
        CAPTURE(program.name, program.reason);
        CHECK(program.enabled == (program.program != EngineBuiltinColor));
      }
      std::cout << "pipelines made: " << compiler->pipelinesCreatedForTesting()
                << ", frames drawn meanwhile: " << session->framesPresented() - start
                << "\n";
      for (const auto& program : report)
      {
        std::cout << "  " << program.name << ": "
                  << (program.enabled ? "enabled" : "disabled: " + program.reason)
                  << "\n";
      }

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      // the one disabled program's warning
      CHECK(events.count(SessionEvent::Warning) == 1u);
      events.print("pipelines");
    }
    CHECK(resetDispatcher());
    messages.print("pipelines, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    // T0.2 (M-PIPE-DEFECT-01): the six LAYOUT_DRAW_VERT pipelines made imply 18 of the
    // one mirrored id, each attribute their vertex shader does not read: depth (two keys)
    // reads location 0 of 6, 5 each; texture_color (two keys) 0, 1 and 4, 3 each;
    // interaction and interaction_PBR 0 to 4, 1 each
    const auto implied = 2 * 5 + 2 * 3 + 1 + 1;
    std::cout << "mirrored warnings: " << messages.mirrored << " of " << implied
              << " implied (the layer's duplicate limit "
              << (limitRaised ? "raised" : "at its default") << ")" << std::endl;
    CHECK(
      messages.mirrored
      == (limitRaised ? implied : std::min(implied, ValidationDuplicateMessageLimit)));
  }

  SECTION("D71: a stop while the compile thread makes a pipeline")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = gamePath;
      // the test orders the compile thread's jobs (DX.5's are not asked for)
      options.requestStartPipelines = false;
      auto session =
        LitSession::create(
          instance.handles(), VK_NULL_HANDLE, 64, 48, options, events.sink(), 1)
        | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      auto* compiler = session->pipelineCompilerForTesting();
      REQUIRE(compiler);
      session->setExposed(true);

      // the compile thread signals once it is about to make the first pipeline, and is
      // held there a while, so that the stop comes while it is at work
      auto creating = std::promise<void>{};
      auto signalled = std::atomic<bool>{false};
      compiler->setBeforeCreateForTesting([&] {
        if (!signalled.exchange(true))
        {
          creating.set_value();
        }
        std::this_thread::sleep_for(100ms);
      });
      const auto hdr = FramebufferFormats{
        nvrhi::Format::RGBA16_FLOAT,
        session->deviceFacts().d24s8Supported ? nvrhi::Format::D24S8
                                              : nvrhi::Format::D32S8};
      for (const auto program :
           {EngineBuiltinInteraction, EngineBuiltinPbrInteraction, EngineBuiltinDepth})
      {
        compiler->graphicsPipeline({PipelineRecipe::Draw, program, 0, 0, 0.0f, hdr});
      }
      REQUIRE(creating.get_future().wait_for(30000ms) == std::future_status::ready);

      const auto stopStart = std::chrono::steady_clock::now();
      teardown(*session);
      const auto stopMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - stopStart)
                            .count();
      std::cout << "a stop during a pipeline's creation took " << stopMs << " ms\n";
      CHECK(stopMs < double(StopBound.count()));
      // T0.7: the stop ends the thread before its next creation step; the jobs it did
      // not finish disable nothing
      for (const auto& program : session->programReport())
      {
        CAPTURE(program.name, program.reason);
        CHECK(program.enabled);
      }
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("a stop while compiling, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    // T0.2: the stop comes while the first pipeline, interaction's, is made; the thread
    // ends after it: one attribute (location 5) not read
    CHECK(messages.mirrored == 1);
    std::cout << "mirrored warnings of the stop: " << messages.mirrored
              << " (the duplicate limit " << (limitRaised ? "raised" : "at its default")
              << ")" << std::endl;
  }
}

namespace
{

// LP2.1: runs hook on the render thread in the session's next frame (RenderFrameAccess)
// and waits for it; a hook asserts nothing itself (Catch2 is not thread-safe), it
// gathers what the test checks
template <typename Hook>
bool inFrame(
  LitSession& session, Hook hook_, const std::chrono::milliseconds timeout = 10000ms)
{
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  session.runInFrameForTesting(
    [hook = std::move(hook_), done](RenderFrameAccess& access) mutable {
      hook(access);
      done->set_value();
    });
  return future.wait_for(timeout) == std::future_status::ready;
}

// the device-local VRAM in use now, in MB; 0 without VK_EXT_memory_budget
double vramMb(const LitSession& session)
{
  const auto vram = session.vramForTesting();
  return vram ? double(vram->usedBytes) / (1024.0 * 1024.0) : 0.0;
}

// a headless session with validation on this machine's GPU
std::unique_ptr<LitSession> gpuSession(
  const TestInstance& instance,
  Events& events,
  const std::uint64_t generation,
  MaterialService* materials = nullptr,
  const std::filesystem::path& gamePath = {},
  const std::filesystem::path& frameLogPath = {})
{
  auto options = SessionOptions{};
  options.validation = true;
  options.gamePath = gamePath;
  options.frameLogPath = frameLogPath;
  // its tests draw no lit scene and count what the session allocates (DX.5's pipelines
  // are not asked for)
  options.requestStartPipelines = false;
  return LitSession::create(
           instance.handles(),
           VK_NULL_HANDLE,
           64,
           48,
           options,
           events.sink(),
           generation,
           materials)
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// the frames in flight and the idle pass's garbage collection are done
void settle(LitSession& session)
{
  session.requestFrame();
  std::this_thread::sleep_for(IdleCollectInterval + 500ms);
}

// D36: count chunks of verts vertices each (a fan of their triangles), keyed by cell,
// with both versions version
std::shared_ptr<const std::map<ChunkKey, Chunk>> syntheticChunks(
  const std::size_t count,
  const std::size_t verts,
  const std::uint64_t version,
  const int firstCell = 0)
{
  auto chunks = std::make_shared<std::map<ChunkKey, Chunk>>();
  for (std::size_t i = 0; i < count; ++i)
  {
    auto chunk = Chunk{};
    chunk.key.material = "textures/lp2/arena";
    chunk.key.cell = {firstCell + int(i), 0, 0};
    chunk.verts.resize(verts);
    for (std::size_t v = 0; v < verts; ++v)
    {
      chunk.verts[v].xyz = {float(i), float(v), 0.0f};
    }
    for (std::size_t v = 1; v + 1 < verts; ++v)
    {
      chunk.indexes.insert(
        chunk.indexes.end(), {std::uint16_t(0), std::uint16_t(v), std::uint16_t(v + 1)});
    }
    chunk.casterVersion = version;
    chunk.appearanceVersion = version;
    const auto key = chunk.key;
    chunks->emplace(key, std::move(chunk));
  }
  return chunks;
}

// a chunk's bytes in the arenas: its vertices and its index words
std::uint64_t arenaBytesOf(const Chunk& chunk)
{
  return chunk.verts.size() * 32u + (chunk.indexes.size() * 2u + 3u) / 4u * 4u;
}

struct ArenaState
{
  std::size_t placed = 0;
  std::size_t queued = 0;
  std::size_t vertexArenas = 0;
  std::size_t indexArenas = 0;
  std::uint64_t usedBytes = 0;
};

ArenaState arenaState(LitSession& session)
{
  auto state = ArenaState{};
  inFrame(session, [&](RenderFrameAccess& access) {
    state.placed = access.arenas.placedChunks();
    state.queued = access.arenas.queuedChunks();
    state.vertexArenas = access.arenas.vertexArenas();
    state.indexArenas = access.arenas.indexArenas();
    state.usedBytes = access.arenas.usedBytes();
  });
  return state;
}

using CachedImages =
  std::vector<std::pair<std::string, std::shared_ptr<const CachedImage>>>;

// the LP1c cache fixtures, read as the cache reader reads them (RGB565 as RGBA8)
CachedImages goldenCaches()
{
  auto caches = CachedImages{};
  const auto root = litFixtureFolder() / "goldens" / "lp1c" / "generated" / "images";
  for (const auto& entry : std::filesystem::recursive_directory_iterator{root})
  {
    if (entry.path().extension() == ".bimage")
    {
      auto image = readBimage(readTextFile(entry.path())) | kdl::value();
      caches.emplace_back(
        entry.path().lexically_relative(root).generic_string(),
        std::make_shared<const CachedImage>(std::move(image)));
    }
  }
  return caches;
}

// a 2D image of one format with every level down to 1 × 1 (blocks down to 4 × 4), each
// level's bytes value + its level
std::shared_ptr<const CachedImage> syntheticImage(
  const int size, const int format, const std::uint8_t value, const int sides = 1)
{
  namespace ei = engine_image;
  auto image = CachedImage{};
  image.textureType = sides == 6 ? ei::TypeCubic : ei::Type2D;
  image.format = format;
  image.storedFormat = format;
  image.width = size;
  image.height = size;
  const auto block = format == ei::FormatDxt1   ? 8
                     : format == ei::FormatDxt5 ? 16
                     : format == ei::FormatBc6h ? 16
                                                : 0;
  for (auto level = 0; (size >> level) >= (block ? 4 : 1); ++level)
  {
    for (auto side = 0; side < sides; ++side)
    {
      const auto width = size >> level;
      const auto bytes =
        block ? std::size_t(width / 4) * std::size_t(width / 4) * std::size_t(block)
              : std::size_t(width) * std::size_t(width) * 4;
      image.levels.push_back(
        CachedImageLevel{level, side, width, width, image.data.size(), bytes});
      image.data.resize(
        image.data.size() + bytes, std::uint8_t(value + level * 8 + side * 40));
    }
    image.numLevels = level + 1;
  }
  return std::make_shared<const CachedImage>(std::move(image));
}

MaterialImageKey testImageKey(const std::string& name)
{
  auto key = MaterialImageKey{};
  key.name = name;
  key.usage = engine_image::UsageDefault;
  key.filter = engine_image::FilterDefault;
  return key;
}

struct SlotSeen
{
  ImageSlots::State state = ImageSlots::State::Pending;
  std::uint64_t version = 0;
  int firstWrittenMip = 0;
  int mipLevels = 0;
  nvrhi::TextureDesc desc;
  std::string reason;
  nvrhi::TextureSubresourceSet bound;
  bool boundIntrinsic = false;
};

SlotSeen slotSeen(const ImageSlots& slots, const ImageSlotId id)
{
  const auto* slot = slots.slot(id);
  auto seen = SlotSeen{};
  seen.state = slot->state;
  seen.version = slot->version;
  seen.firstWrittenMip = slot->firstWrittenMip;
  seen.mipLevels = slot->mipLevels;
  seen.reason = slot->reason;
  if (slot->texture)
  {
    seen.desc = slot->texture->getDesc();
  }
  const auto binding = slots.binding(id);
  seen.bound = binding.subresources;
  seen.boundIntrinsic = binding.intrinsic;
  return seen;
}

// waits until every slot of ids is Resident (or Default), asking each frame
bool waitForSlots(
  LitSession& session,
  const std::vector<ImageSlotId>& ids,
  const std::chrono::milliseconds timeout = 20000ms)
{
  return waitFor(
    [&] {
      auto settled = false;
      inFrame(session, [&](RenderFrameAccess& access) {
        settled = std::ranges::all_of(ids, [&](const ImageSlotId id) {
          const auto state = access.images.slot(id)->state;
          return state == ImageSlots::State::Resident
                 || state == ImageSlots::State::Default;
        });
      });
      return settled;
    },
    timeout,
    false);
}

} // namespace

// LP2.1: the render thread's GPU resources (FrameResources, the arenas, the image slots
// and textures, the binding sets, the samplers). Each section tears down with no NVRHI
// reference, no NVRHI error, no validation message and every device allocation freed.
TEST_CASE("LitSession GPU resources on a GPU (LP2.1)", "[.][litgpu]")
{
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  SECTION("C14, C28: the frame resources, and 100 resizes back at the baseline")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      CHECK(session->renderTargetGenerationForTesting() == 1u);

      // the registry's resources by name, at the headless extent, in the formats of C6
      struct Seen
      {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool exposureCleared = false;
        std::vector<std::string> found;
        std::vector<nvrhi::Format> formats;
        bool hdrUav = false;
        std::uint64_t targetBytes = 0;
        std::string unknown;
        std::size_t passWrites = 0;
      };
      auto seen = Seen{};
      const auto d24s8 = session->deviceFacts().d24s8Supported;
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        namespace fr = frame_resource;
        seen.width = access.frame.extent().width;
        seen.height = access.frame.extent().height;
        seen.exposureCleared = access.frame.exposureCleared();
        for (const auto name :
             {fr::CurrentRenderHdr,
              fr::CurrentDepth,
              fr::CurrentRenderLdr,
              fr::HdrFramebuffer,
              fr::LdrFramebuffer,
              fr::HistogramBuffer,
              fr::ExposureBuffer,
              fr::FrameTarget})
        {
          if (const auto resource = access.frame.find(name))
          {
            seen.found.emplace_back(name);
            if (resource->texture)
            {
              seen.formats.push_back(resource->texture->getDesc().format);
            }
          }
        }
        seen.hdrUav = access.frame.find(fr::CurrentRenderHdr)->texture->getDesc().isUAV;
        seen.targetBytes = access.frame.renderTargetBytes();
        auto pass = PassDesc{};
        pass.name = "a test pass";
        pass.reads = {std::string{fr::CurrentRenderHdr}, std::string{fr::ExposureBuffer}};
        pass.writes = {std::string{fr::LdrFramebuffer}};
        seen.passWrites = access.frame.resolve(pass)
                          | kdl::transform([](const auto& r) { return r.writes.size(); })
                          | kdl::value_or(std::size_t{0});
        pass.reads.emplace_back("_noSuchImage");
        access.frame.resolve(pass)
          | kdl::if_error([&](const auto& e) { seen.unknown = e.msg; }) | kdl::ignore();
      }));
      CHECK(seen.width == 64u);
      CHECK(seen.height == 48u);
      CHECK(seen.exposureCleared);
      CHECK(seen.found.size() == 8u);
      CHECK(
        seen.formats
        == std::vector<nvrhi::Format>{
          nvrhi::Format::RGBA16_FLOAT,
          d24s8 ? nvrhi::Format::D24S8 : nvrhi::Format::D32S8,
          nvrhi::Format::RGBA8_UNORM,
          nvrhi::Format::RGBA8_UNORM});
      CHECK(seen.hdrUav);
      CHECK(seen.targetBytes == 64u * 48u * (8u + (d24s8 ? 4u : 8u) + 4u));
      CHECK(seen.passWrites == 1u);
      CHECK(
        seen.unknown
        == "the pass a test pass reads _noSuchImage, which the frame has not");

      settle(*session);
      const auto baselineAllocations = session->deviceMemoryAllocations();
      const auto baselineVram = vramMb(*session);

      // C28: each new extent makes the targets again and bumps the generation once; the
      // last returns to the first extent
      for (std::uint32_t i = 1; i <= 100; ++i)
      {
        CAPTURE(i);
        const auto width = i == 100 ? 64u : 65u + i;
        const auto height = i == 100 ? 48u : 48u + (i % 5u);
        session->resizeHeadlessForTesting(width, height);
        REQUIRE(waitFor(
          [&] { return session->renderTargetGenerationForTesting() == 1u + i; },
          5000ms,
          false));
      }
      // the same extent again: no new targets
      const auto frames = session->framesPresented();
      session->resizeHeadlessForTesting(64, 48);
      REQUIRE(
        waitFor([&] { return session->framesPresented() > frames; }, 5000ms, false));
      CHECK(session->renderTargetGenerationForTesting() == 101u);

      // the old targets are collected: the allocations and the VRAM are the baseline's
      settle(*session);
      CHECK(waitFor(
        [&] { return session->deviceMemoryAllocations() == baselineAllocations; },
        5000ms,
        false));
      const auto vramAfter = vramMb(*session);
      std::cout << "100 resizes: allocations " << baselineAllocations << " -> "
                << session->deviceMemoryAllocations() << ", VRAM " << baselineVram
                << " MB -> " << vramAfter << " MB\n";
      CHECK(vramAfter <= baselineVram + 1.0);
      // U2.2: what was retired is collected, and both users' accounts are back at 0
      auto retired = std::array<std::uint64_t, 2>{1, 1};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        retired = {access.frame.retiredBytes(), access.images.retiredBytes()};
      }));
      CHECK(retired[0] == 0u);
      CHECK(retired[1] == 0u);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(events.count(SessionEvent::Warning) == 0u);
      events.print("frame resources");
    }
    CHECK(resetDispatcher());
    messages.print("frame resources, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("DX.4: no frame is settled while a geometry batch runs")
  {
    // R2.1's 10,000 boxes: a batch of seconds in Debug, of many frames in Release
    auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
    for (auto i = 0; i < 10000; ++i)
    {
      const auto x = (i % 4) * 1024 + ((i / 16) % 10) * 96 + 32;
      const auto y = ((i / 4) % 4) * 1024 + ((i / 160) % 10) * 96 + 32;
      const auto z = (i / 1600) * 96;
      text += boxBrush(x, y, z, x + 48, y + 48, z + 48, "lp1b/wall");
    }
    text += "}\n";
    auto snapshot = std::make_shared<Snapshot>(snapshotOfMapText(text));
    snapshot->generation = 12;
    snapshot->sequence = 1;

    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = LitSession::create(
                       instance.handles(), VK_NULL_HANDLE, 16, 16, {}, events.sink(), 12)
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      settle(*session);
      CHECK(session->lastFrameSettledForTesting());

      REQUIRE(session->offerSnapshot(snapshot));
      REQUIRE(waitFor(
        [&] { return session->snapshotsTakenForTesting() == 1u; }, 5000ms, false));
      // a frame drawn while the batch runs, and the convergence it left
      auto during = FrameConvergence{};
      auto batches = std::uint64_t{1};
      REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      REQUIRE(inFrame(
        *session, [&](RenderFrameAccess& access) { during = access.convergence; }));
      // the hook saw the convergence of the frame before it, which ended before the hook
      // returned: with no batch applied by now, that frame was drawn while it ran
      batches = session->geometryBatchesForTesting();
      if (batches == 0u)
      {
        CHECK(during.geometryInFlight);
        CHECK_FALSE(during.settled());
      }
      else
      {
        WARN("the batch was applied before a frame could look at it");
      }
      // the batch applies: the frames after it no longer wait for it (the session has no
      // material service, so its chunks stay provisional: it never settles)
      REQUIRE(waitFor(
        [&] { return session->geometryBatchesForTesting() >= 1u; }, 30000ms, false));
      auto after = FrameConvergence{};
      REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      REQUIRE(inFrame(
        *session, [&](RenderFrameAccess& access) { after = access.convergence; }));
      CHECK_FALSE(after.geometryInFlight);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("a batch in flight, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("T0.9: the render thread keeps the compiler's answers for its draws")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      // no game path: every program is disabled, a Failed answer (D9)
      auto session = gpuSession(instance, events, 1);
      auto* compiler = session->pipelineCompilerForTesting();
      REQUIRE(compiler);
      session->setExposed(true);
      const auto key = PipelineKey{
        PipelineRecipe::Draw,
        EngineBuiltinDepth,
        engine_gls::Default,
        0,
        0.0f,
        FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::D24S8}};

      // asked each frame while Pending, then kept
      auto state = GraphicsPipelineLookup::State::Pending;
      REQUIRE(waitFor(
        [&] {
          inFrame(*session, [&](RenderFrameAccess& access) {
            state = access.pipelines.graphics(key).state;
          });
          return state != GraphicsPipelineLookup::State::Pending;
        },
        10000ms,
        false));
      CHECK(state == GraphicsPipelineLookup::State::Failed);
      const auto asked = compiler->lookupsForTesting();
      auto kept = std::size_t{0};
      auto error = std::string{};
      for (auto i = 0; i < 5; ++i)
      {
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          const auto& lookup = access.pipelines.graphics(key);
          error = lookup.error;
          kept = access.pipelines.kept();
        }));
      }
      CHECK(compiler->lookupsForTesting() == asked);
      CHECK(kept == 1u);
      CHECK_THAT(error, Catch::Matchers::StartsWith("disabled: "));

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("kept lookups, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("T0.4: an empty or a relative game path disables every program (D9)")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      for (const auto& [gamePath, reason] :
           std::vector<std::pair<std::filesystem::path, std::string>>{
             {{}, "the game path is empty, so no shader blob is read (C1)"},
             {"relative/game",
              "the game path relative/game is relative, so no shader blob is read (C1)"}})
      {
        CAPTURE(gamePath);
        auto events = Events{};
        auto session = gpuSession(instance, events, 1, nullptr, gamePath);
        auto* compiler = session->pipelineCompilerForTesting();
        REQUIRE(compiler);
        const auto key = PipelineKey{
          PipelineRecipe::Draw,
          EngineBuiltinDepth,
          engine_gls::Default,
          0,
          0.0f,
          FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::D24S8}};
        compiler->graphicsPipeline(key);
        compiler->computePipeline(EngineBuiltinHistogram);
        REQUIRE(waitFor(
          [&] {
            return compiler->graphicsPipeline(key).state
                     != GraphicsPipelineLookup::State::Pending
                   && compiler->computePipeline(EngineBuiltinHistogram).state
                        != ComputePipelineLookup::State::Pending;
          },
          10000ms,
          false));
        const auto graphics = compiler->graphicsPipeline(key);
        CHECK(graphics.state == GraphicsPipelineLookup::State::Failed);
        CHECK_THAT(graphics.error, Catch::Matchers::EndsWith(reason));
        CHECK_THAT(graphics.error, Catch::Matchers::StartsWith("disabled: "));
        const auto compute = compiler->computePipeline(EngineBuiltinHistogram);
        CHECK(compute.state == ComputePipelineLookup::State::Failed);
        CHECK_THAT(compute.error, Catch::Matchers::EndsWith(reason));
        for (const auto& program : session->programReport())
        {
          CHECK_FALSE(program.enabled);
        }
        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->deviceMemoryAllocations() == 0);
      }
    }
    CHECK(resetDispatcher());
    messages.print("game paths, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("D36: 1000 chunks uploaded, freed and uploaded again; Stop at the baseline")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      settle(*session);
      const auto baseline = session->deviceMemoryAllocations();

      // 1000 chunks of 64 vertices, two frames' worth at most
      const auto chunks = syntheticChunks(1000, 64, 1);
      auto bytes = std::uint64_t{0};
      for (const auto& [key, chunk] : *chunks)
      {
        bytes += arenaBytesOf(chunk);
      }
      session->setChunksForTesting(chunks);
      REQUIRE(
        waitFor([&] { return arenaState(*session).placed == 1000u; }, 10000ms, false));
      const auto placed = arenaState(*session);
      CHECK(placed.queued == 0u);
      CHECK(placed.vertexArenas == 1u);
      CHECK(placed.indexArenas == 1u);
      CHECK(placed.usedBytes == bytes);
      // C30: the arenas and the list's upload chunks, never an allocation per chunk
      const auto withChunks = session->deviceMemoryAllocations();
      std::cout << "1000 chunks (" << bytes << " bytes): allocations " << baseline
                << " -> " << withChunks << "\n";
      CHECK(withChunks - baseline <= 4);

      // freed: the ranges wait for their frame, the arenas stay
      session->setChunksForTesting(syntheticChunks(0, 0, 1));
      REQUIRE(waitFor([&] { return arenaState(*session).placed == 0u; }, 10000ms, false));
      CHECK(arenaState(*session).usedBytes == 0u);
      // and uploaded again as changed chunks: the freed ranges are handed out again once
      // their frames have finished, so the arenas do not grow
      settle(*session);
      session->setChunksForTesting(syntheticChunks(1000, 64, 2));
      REQUIRE(
        waitFor([&] { return arenaState(*session).placed == 1000u; }, 10000ms, false));
      const auto again = arenaState(*session);
      CHECK(again.vertexArenas == 1u);
      CHECK(again.indexArenas == 1u);
      CHECK(session->deviceMemoryAllocations() - baseline <= 4);

      teardown(*session);
      const auto report = session->teardownReport();
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      REQUIRE(report.vramAtStart);
      REQUIRE(report.vramAtStop);
      const auto startMb = double(report.vramAtStart->usedBytes) / (1024.0 * 1024.0);
      const auto stopMb = double(report.vramAtStop->usedBytes) / (1024.0 * 1024.0);
      std::cout << "1000 chunks: VRAM at start " << startMb << " MB, at stop " << stopMb
                << " MB\n";
      CHECK(stopMb <= startMb + 1.0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(events.count(SessionEvent::Warning) == 0u);
      events.print("1000 chunks");
    }
    CHECK(resetDispatcher());
    messages.print("1000 chunks, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("D78: a 40 MB batch takes two frames under the 32 MB cap")
  {
    auto messages = ValidationMessages{};
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-upload-frame-log.csv";
    std::filesystem::remove(logPath);
    auto logged = std::vector<std::uint64_t>{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1, nullptr, {}, logPath);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // each frame's uploads, as its hook sees them (the hook runs after the uploads)
      auto mutex = std::mutex{};
      auto uploads = std::vector<std::uint64_t>{};
      session->setEveryFrameHookForTesting([&](RenderFrameAccess& access) {
        const auto lock = std::scoped_lock{mutex};
        if (access.bytesUploaded > 0)
        {
          uploads.push_back(access.bytesUploaded);
        }
      });
      // 17 chunks of 65535 vertices: about 2.5 MB each, about 42 MB in all
      const auto chunks = syntheticChunks(17, 65535, 1);
      auto bytes = std::uint64_t{0};
      for (const auto& [key, chunk] : *chunks)
      {
        bytes += arenaBytesOf(chunk);
      }
      REQUIRE(bytes > 40u * 1024u * 1024u);
      session->setChunksForTesting(chunks);
      // no camera move: the unconverged first frame triggers the second (D77)
      REQUIRE(waitFor(
        [&] { return arenaState(*session).placed == chunks->size(); }, 10000ms, false));
      session->setEveryFrameHookForTesting({});
      {
        const auto lock = std::scoped_lock{mutex};
        std::cout << "a " << bytes << "-byte batch: frames uploaded";
        for (const auto frameBytes : uploads)
        {
          std::cout << " " << frameBytes;
        }
        std::cout << "\n";
        REQUIRE(uploads.size() == 2u);
        CHECK(uploads[0] <= UploadBytesPerFrame);
        CHECK(uploads[0] + uploads[1] == bytes);
        logged = uploads;
      }

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    // C16 (LP2.1): the frame log's bytes_uploaded column says the same, and its
    // arena_bytes the two arenas
    auto fromLog = std::vector<std::uint64_t>{};
    auto arenaBytes = std::string{};
    for (const auto& row : readCsv(logPath))
    {
      if (row.size() == 42u && row[1] == "frame" && !row[15].empty() && row[15] != "0")
      {
        fromLog.push_back(std::stoull(row[15]));
        arenaBytes = row[17];
      }
    }
    CHECK(fromLog == logged);
    CHECK(arenaBytes == std::to_string(2 * ArenaBytes));
    std::filesystem::remove(logPath);
    CHECK(resetDispatcher());
    messages.print("40 MB batch, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("D36: a freed range is not handed out before its frame has finished")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      const auto placementOf = [&](const ChunkKey& key) {
        auto placement = std::optional<ChunkPlacement>{};
        inFrame(*session, [&](RenderFrameAccess& access) {
          placement = access.arenas.placement(key);
        });
        return placement;
      };
      const auto a = syntheticChunks(1, 256, 1, 0);
      const auto b = syntheticChunks(1, 256, 1, 1);
      const auto keyA = a->begin()->first;
      const auto keyB = b->begin()->first;
      session->setChunksForTesting(a);
      REQUIRE(waitFor([&] { return placementOf(keyA).has_value(); }, 5000ms, false));
      const auto first = *placementOf(keyA);

      // A goes and B comes while no frame counts as finished: A's ranges are held, so B
      // gets others
      session->holdFrameCompletionForTesting(true);
      session->setChunksForTesting(b);
      REQUIRE(waitFor([&] { return placementOf(keyB).has_value(); }, 5000ms, false));
      for (auto i = 0; i < 4; ++i)
      {
        session->requestFrame();
        std::this_thread::sleep_for(50ms);
      }
      const auto second = *placementOf(keyB);
      CHECK(second.vertices.offset != first.vertices.offset);
      CHECK(second.indexes.offset != first.indexes.offset);
      CHECK_FALSE(placementOf(keyA));

      // once the frame that freed them has finished, A's ranges are handed out again
      session->holdFrameCompletionForTesting(false);
      settle(*session);
      auto bc = std::make_shared<std::map<ChunkKey, Chunk>>(*b);
      const auto c = syntheticChunks(1, 256, 1, 2);
      const auto keyC = c->begin()->first;
      bc->insert(c->begin(), c->end());
      session->setChunksForTesting(bc);
      REQUIRE(waitFor([&] { return placementOf(keyC).has_value(); }, 5000ms, false));
      const auto third = *placementOf(keyC);
      CHECK(third.vertices.offset == first.vertices.offset);
      CHECK(third.indexes.offset == first.indexes.offset);
      std::cout << "freed range: A at " << first.vertices.offset << "/"
                << first.indexes.offset << ", B (held) at " << second.vertices.offset
                << "/" << second.indexes.offset << ", C (after) at "
                << third.vertices.offset << "/" << third.indexes.offset << "\n";

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("freed range, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("D78: a Stop in the middle of an upload batch")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // 1 MB a frame: 40 chunks of about 0.6 MB take about 25 frames
      session->setUploadCapForTesting(1024 * 1024);
      session->setChunksForTesting(syntheticChunks(40, 16384, 1));
      REQUIRE(waitFor(
        [&] {
          const auto state = arenaState(*session);
          return state.placed >= 4u && state.queued > 0u;
        },
        5000ms,
        false));
      const auto stopStart = std::chrono::steady_clock::now();
      teardown(*session);
      const auto stopMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - stopStart)
                            .count();
      std::cout << "a stop during an upload batch took " << stopMs << " ms\n";
      CHECK(stopMs < double(StopBound.count()));
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("stop during uploads, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("SC265-SC268: the LP1c caches and a BC6H image become textures")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      auto caches = goldenCaches();
      REQUIRE(caches.size() == 18u);
      // the HDR panoramas' BC6H caches are 8 MB and only their headers are fixtures: a
      // synthetic BC6H image of arbitrary blocks stands in
      caches.emplace_back("bc6h", syntheticImage(16, ei::FormatBc6h, 7));
      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto& [name, image] : caches)
        {
          ids.push_back(access.images.provideForTesting(testImageKey(name), image));
        }
      }));
      REQUIRE(waitForSlots(*session, ids));

      auto seen = std::vector<SlotSeen>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto id : ids)
        {
          seen.push_back(slotSeen(access.images, id));
        }
      }));
      const auto d24s8 = session->deviceFacts().d24s8Supported;
      for (std::size_t i = 0; i < caches.size(); ++i)
      {
        const auto& image = *caches[i].second;
        CAPTURE(caches[i].first, seen[i].reason);
        CHECK(seen[i].state == ImageSlots::State::Resident);
        CHECK(seen[i].desc.format == *engineTextureFormat(image.format, d24s8));
        CHECK(seen[i].desc.mipLevels == std::uint32_t(image.numLevels));
        CHECK(seen[i].firstWrittenMip == 0);
        CHECK(seen[i].bound.numMipLevels == std::uint32_t(image.numLevels));
        CHECK(
          seen[i].desc.dimension
          == (image.textureType == ei::TypeCubic ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D));
        const auto mapping = engineComponentMapping(image.format, image.colorFormat);
        CHECK(seen[i].desc.componentMapping.r == mapping.r);
        CHECK(seen[i].desc.componentMapping.g == mapping.g);
        CHECK(seen[i].desc.componentMapping.b == mapping.b);
        CHECK(seen[i].desc.componentMapping.a == mapping.a);
      }
      std::cout << "LP1c caches as textures:";
      for (std::size_t i = 0; i < caches.size(); ++i)
      {
        std::cout << " " << caches[i].first << " ("
                  << nvrhi::getFormatInfo(seen[i].desc.format).name << ")";
      }
      std::cout << "\n";

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("LP1c caches, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("200 textures uploaded, then Stop at the baseline")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      settle(*session);
      const auto baseline = session->deviceMemoryAllocations();

      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (auto i = 0; i < 200; ++i)
        {
          ids.push_back(access.images.provideForTesting(
            testImageKey(fmt::format("textures/lp2/t{}", i)),
            syntheticImage(
              i % 2 ? 64 : 32,
              i % 3 ? ei::FormatRgba8 : ei::FormatDxt5,
              std::uint8_t(i))));
        }
      }));
      REQUIRE(waitForSlots(*session, ids));
      auto resident = std::size_t{0};
      auto bytes = std::uint64_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        resident = access.images.resident();
        bytes = access.images.textureBytes();
      }));
      CHECK(resident == 200u);
      const auto withTextures = session->deviceMemoryAllocations();
      std::cout << "200 textures (" << bytes << " bytes): allocations " << baseline
                << " -> " << withTextures << "\n";
      // one allocation per texture (D72 measures what that means for a whole map)
      CHECK(withTextures - baseline >= 200);

      teardown(*session);
      const auto report = session->teardownReport();
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      REQUIRE(report.vramAtStart);
      REQUIRE(report.vramAtStop);
      const auto startMb = double(report.vramAtStart->usedBytes) / (1024.0 * 1024.0);
      const auto stopMb = double(report.vramAtStop->usedBytes) / (1024.0 * 1024.0);
      std::cout << "200 textures: VRAM at start " << startMb << " MB, at stop " << stopMb
                << " MB\n";
      CHECK(stopMb <= startMb + 1.0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("200 textures, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

#if defined(_MSC_VER) && defined(_DEBUG)
  SECTION("U2.4: a headless session's texture bumps between two debug-heap checkpoints")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      auto keys = std::vector<MaterialImageKey>{};
      auto images = std::vector<std::shared_ptr<const CachedImage>>{};
      for (auto i = 0; i < 8; ++i)
      {
        keys.push_back(testImageKey(fmt::format("textures/lp2/heap{}", i)));
        images.push_back(syntheticImage(
          i % 2 ? 32 : 16, i % 3 ? ei::FormatRgba8 : ei::FormatDxt5, std::uint8_t(i)));
      }
      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
          ids.push_back(access.images.provideForTesting(keys[i], images[i]));
        }
      }));
      REQUIRE(waitForSlots(*session, ids));

      // a round: every texture let go and given again (a re-probe's bump), written in
      // the next frames, the old ones retired and collected
      auto failures = 0;
      const auto round = [&] {
        failures += inFrame(
                      *session,
                      [&](RenderFrameAccess& access) {
                        for (std::size_t i = 0; i < keys.size(); ++i)
                        {
                          access.images.dropTexture(ids[i], access.frameIndex);
                          access.images.provideForTesting(keys[i], images[i]);
                        }
                      })
                      ? 0
                      : 1;
        failures += waitForSlots(*session, ids) ? 0 : 1;
      };
      round();
      round();
      settle(*session);
      // Z.2's pattern: settle before each checkpoint; the threads' few varying blocks
      // (a frame log row, a pacer's wait) are allowed, a leak per round shows 20 times
      const auto growth = heapGrowth([&] {
        for (auto i = 0; i < 20; ++i)
        {
          round();
        }
        settle(*session);
      });
      CHECK(failures == 0);
      CAPTURE(growth.blocks, growth.bytes);
      std::cout << "U2.4: 20 rounds of 8 texture bumps: heap blocks " << growth.blocks
                << ", bytes " << growth.bytes << "\n";
      CHECK(growth.blocks <= 16);
      CHECK(growth.bytes <= 16384);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("texture bumps on the debug heap, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
#endif

  SECTION("U3.16, U2.3: a level past the cap, the allocation headroom, a failed chunk")
  {
    namespace ei = engine_image;
    // the test hook is process-wide: it is off again whatever happens below
    struct HostAllocationsAgain
    {
      ~HostAllocationsAgain() { LitSession::failHostAllocationsForTesting(false); }
    };
    const auto again = HostAllocationsAgain{};
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // U3.16: a 2900 x 2900 RGBA8 level is 33,640,000 bytes, more than a frame's 32 MB:
      // refused, the slot Default with the reason, nothing written
      auto large = NoImageSlot;
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        large = access.images.provideForTesting(
          testImageKey("textures/lp2/large"), syntheticImage(2900, ei::FormatRgba8, 1));
      }));
      REQUIRE(waitForSlots(*session, {large}));
      auto largeSeen = SlotSeen{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        largeSeen = slotSeen(access.images, large);
      }));
      CHECK(largeSeen.state == ImageSlots::State::Default);
      CHECK(largeSeen.reason.find("M-TEX-DIFF-02") != std::string::npos);
      CHECK(largeSeen.reason.find("33640000 bytes") != std::string::npos);

      // U2.3: with the limit at the live allocations plus the headroom, no texture is
      // made: each slot keeps its stand-in, the session says so once
      settle(*session);
      const auto live = session->deviceMemoryAllocations();
      session->setAllocationLimitForTesting(
        std::uint32_t(live) + std::uint32_t(AllocationHeadroom));
      const auto warnings = events.count(SessionEvent::Warning);
      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (auto i = 0; i < 4; ++i)
        {
          ids.push_back(access.images.provideForTesting(
            testImageKey(fmt::format("textures/lp2/room{}", i)),
            syntheticImage(16, ei::FormatRgba8, std::uint8_t(i))));
        }
      }));
      REQUIRE(waitForSlots(*session, ids));
      auto refused = std::size_t{0};
      auto reason = std::string{};
      auto headroomRefusals = std::uint64_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto id : ids)
        {
          const auto seen = slotSeen(access.images, id);
          refused += seen.state == ImageSlots::State::Default ? 1 : 0;
          reason = seen.reason;
        }
        headroomRefusals = access.textures.headroomRefusals();
      }));
      CHECK(refused == ids.size());
      CHECK(headroomRefusals == ids.size());
      CHECK(reason.find("maxMemoryAllocationCount") != std::string::npos);
      CHECK(waitFor(
        [&] { return events.count(SessionEvent::Warning) == warnings + 1; },
        5000ms,
        false));
      CHECK(session->deviceMemoryAllocations() <= live + 1);
      session->setAllocationLimitForTesting(0);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    {
      // U2.3: an upload chunk that cannot be allocated ends the session with NVRHI's
      // Error (M-NVRHI-DEFECT-01's patch) instead of a crash in a null buffer's map
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      settle(*session);
      // two 21 MB levels in one frame need a chunk of their own even if one was made
      session->setUploadCapForTesting(std::uint64_t{64} * 1024 * 1024);
      LitSession::failHostAllocationsForTesting(true);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (auto i = 0; i < 2; ++i)
        {
          access.images.provideForTesting(
            testImageKey(fmt::format("textures/lp2/chunk{}", i)),
            syntheticImage(2304, ei::FormatRgba8, std::uint8_t(i)));
        }
      }));
      CHECK(
        waitFor([&] { return events.count(SessionEvent::Error) > 0u; }, 10000ms, false));
      LitSession::failHostAllocationsForTesting(false);
      events.print("a failed upload chunk");
      CHECK(session->nvrhiErrorCount() >= 1u);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
    }
    CHECK(resetDispatcher());
    messages.print("level past the cap, headroom, failed chunk, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION(
    "P2.14c: levels over three frames, smallest first, never an unwritten one bound")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // 256² RGBA8, 9 levels: 2 to 8 are 21844 bytes, 1 is 65536, 0 is 262144; under a
      // 70000-byte cap they take three frames, level 0 alone as its frame's first upload
      session->setUploadCapForTesting(70000);
      auto mutex = std::mutex{};
      auto frames = std::vector<std::pair<std::uint64_t, SlotSeen>>{};
      auto id = NoImageSlot;
      session->setEveryFrameHookForTesting([&](RenderFrameAccess& access) {
        const auto lock = std::scoped_lock{mutex};
        if (id != NoImageSlot && access.bytesUploaded > 0)
        {
          frames.emplace_back(access.bytesUploaded, slotSeen(access.images, id));
        }
      });
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        const auto lock = std::scoped_lock{mutex};
        id = access.images.provideForTesting(
          testImageKey("textures/lp2/levels"), syntheticImage(256, ei::FormatRgba8, 1));
      }));
      REQUIRE(waitForSlots(*session, {id}));
      session->setEveryFrameHookForTesting({});

      auto lock = std::unique_lock{mutex};
      REQUIRE(frames.size() == 3u);
      CHECK(frames[0].first == 21844u);
      CHECK(frames[1].first == 65536u);
      CHECK(frames[2].first == 262144u);
      const auto firsts = std::vector<int>{2, 1, 0};
      const auto versions = std::vector<std::uint64_t>{8, 9, 10};
      for (std::size_t i = 0; i < frames.size(); ++i)
      {
        CAPTURE(i);
        const auto& seen = frames[i].second;
        CHECK(seen.firstWrittenMip == firsts[i]);
        CHECK(seen.version == versions[i]);
        // the binding covers exactly the written levels
        CHECK(seen.bound.baseMipLevel == std::uint32_t(firsts[i]));
        CHECK(seen.bound.numMipLevels == std::uint32_t(9 - firsts[i]));
        CHECK_FALSE(seen.boundIntrinsic);
      }
      std::cout << "levels over three frames: bytes " << frames[0].first << ", "
                << frames[1].first << ", " << frames[2].first << "\n";

      // U2.12: the teardown checks of the other GPU sections (a frame that took the hook
      // before it was cleared may still wait for the lock)
      lock.unlock();
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("levels over three frames, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("SC268: a cube's six sides land in their slices")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // the LP1c sky cube (BC3, level-major records) and an RGBA8 cube of a colour per
      // side
      auto sky = std::shared_ptr<const CachedImage>{};
      for (const auto& [name, image] : goldenCaches())
      {
        if (image->textureType == ei::TypeCubic)
        {
          sky = image;
        }
      }
      REQUIRE(sky);
      const auto colours = syntheticImage(4, ei::FormatRgba8, 10, 6);
      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        // a permanent ShaderResource texture is never a copy source
        access.textures.setPermanentStateForTesting(false);
        ids.push_back(access.images.provideForTesting(testImageKey("env/sky"), sky));
        ids.push_back(
          access.images.provideForTesting(testImageKey("env/colours"), colours));
      }));
      REQUIRE(waitForSlots(*session, ids));

      // each side's first 4 × 4 block (one texel of the RGBA8 cube) copied into a staging
      // texture of its own, read back two frames later
      auto staging = std::vector<std::pair<nvrhi::StagingTextureHandle, std::size_t>>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (std::size_t c = 0; c < ids.size(); ++c)
        {
          auto* texture = access.images.slot(ids[c])->texture.Get();
          const auto format = texture->getDesc().format;
          const auto size = c == 0 ? 4u : 1u;
          for (std::uint32_t side = 0; side < 6; ++side)
          {
            auto handle = access.device.createStagingTexture(
              nvrhi::TextureDesc{}.setWidth(size).setHeight(size).setFormat(format),
              nvrhi::CpuAccessMode::Read);
            access.commandList.copyTexture(
              handle,
              nvrhi::TextureSlice{}.setSize(size, size, 1),
              texture,
              nvrhi::TextureSlice{}.setSize(size, size, 1).setArraySlice(side));
            staging.emplace_back(std::move(handle), c);
          }
        }
      }));
      settle(*session);
      auto read = std::vector<std::vector<std::uint8_t>>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (auto& [handle, c] : staging)
        {
          auto pitch = std::size_t{0};
          const auto* bytes =
            static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
              handle, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));

          read.emplace_back(bytes, bytes + (c == 0 ? 16 : 4));
          access.device.unmapStagingTexture(handle);
        }
        staging.clear();
      }));
      REQUIRE(read.size() == 12u);
      for (std::size_t side = 0; side < 6; ++side)
      {
        CAPTURE(side);
        // the cache's record of level 0 on this side (level-major: record side)
        const auto& record = sky->levels[side];
        REQUIRE(record.level == 0);
        REQUIRE(record.destZ == int(side));
        const auto expected = std::vector<std::uint8_t>(
          sky->data.begin() + std::ptrdiff_t(record.offset),
          sky->data.begin() + std::ptrdiff_t(record.offset) + 16);
        CHECK(read[side] == expected);
        // the colour cube's side s is 10 + 40 s in every channel
        CHECK(
          read[6 + side] == std::vector<std::uint8_t>(4, std::uint8_t(10 + 40 * side)));
      }

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("cube sides, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION(
    "C28: a slot's bump erases exactly its sets; a dropped texture binds the intrinsic")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    // a game with two textures; the service makes the intrinsics (D69)
    const auto env = fs::TestEnvironment{};
    for (const auto& [name, colour] : std::vector<std::pair<std::string, std::uint8_t>>{
           {"a", std::uint8_t{40}}, {"b", std::uint8_t{200}}})
    {
      writeTextFile(
        env.dir() / "base" / "textures" / "lp2" / (name + ".tga"),
        test::tgaFile(test::flat(16, 16, colour, colour, colour), 32));
    }
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [path = env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1, service.get());
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // the owner's part: a published table reaches the render thread (as the host does
      // on MaterialsPublished)
      const auto settledSlots = [&](const std::vector<ImageSlotId>& ids) {
        return waitFor(
          [&] {
            session->tableChanged();
            auto settled = false;
            inFrame(*session, [&](RenderFrameAccess& access) {
              settled = std::ranges::all_of(ids, [&](const ImageSlotId id) {
                return access.images.slot(id)->state == ImageSlots::State::Resident;
              });
            });
            return settled;
          },
          20000ms,
          false);
      };

      auto keyA = testImageKey("textures/lp2/a");
      keyA.usage = ei::UsageDiffuse;
      auto keyB = testImageKey("textures/lp2/b");
      keyB.usage = ei::UsageDiffuse;
      auto a = NoImageSlot;
      auto b = NoImageSlot;
      auto fallback = NoImageSlot;
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        a = access.images.request(keyA);
        b = access.images.request(keyB);
        fallback = *access.images.find(intrinsicImageKey("_default"));
      }));
      REQUIRE(settledSlots({a, b, fallback}));

      // TEXTURE's descriptor set 1, Texture_SRV(0), from the compile thread (C20)
      auto layout = nvrhi::BindingLayoutHandle{};
      REQUIRE(waitFor(
        [&] {
          inFrame(*session, [&](RenderFrameAccess& access) {
            const auto& lookup = access.pipelines.layouts(EngineBindingLayout::Texture);
            if (lookup.state == BindingLayoutLookup::State::Ready)
            {
              layout = lookup.layouts.at(1);
            }
          });
          return bool(layout);
        },
        10000ms,
        false));

      // the set of a slot as a pass makes it: the slot's binding, keyed by its versions
      struct SetSeen
      {
        nvrhi::IBindingSet* set = nullptr;
        nvrhi::ITexture* texture = nullptr;
        bool intrinsic = false;
      };
      const auto setOf = [&](RenderFrameAccess& access, const ImageSlotId slot) {
        const auto binding = access.images.binding(slot);
        auto key = BindingSetKey{};
        key.layout = layout.Get();
        key.slots = binding.versions;
        const auto set = access.bindingSets.get(key, access.frameIndex, [&] {
          return access.device.createBindingSet(
            nvrhi::BindingSetDesc{}.addItem(nvrhi::BindingSetItem::Texture_SRV(
              0, binding.texture, nvrhi::Format::UNKNOWN, binding.subresources)),
            layout);
        });
        return SetSeen{set.Get(), binding.texture, binding.intrinsic};
      };

      auto first = std::pair<SetSeen, SetSeen>{};
      auto textureA = static_cast<nvrhi::ITexture*>(nullptr);
      auto sets = std::size_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        first = {setOf(access, a), setOf(access, b)};
        textureA = access.images.slot(a)->texture.Get();
        sets = access.bindingSets.size();
      }));
      CHECK(first.first.texture == textureA);
      CHECK_FALSE(first.first.intrinsic);
      CHECK(sets == 2u);

      // A's texture is dropped (as an eviction would); no upload brings it back yet
      session->setUploadCapForTesting(0);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.images.dropTexture(a, access.frameIndex);
      }));
      // the next frame: exactly A's set went, B's stayed, and A binds the intrinsic
      auto setsOfA = std::size_t{9};
      auto setsOfB = std::size_t{9};
      auto dropped = SetSeen{};
      auto again = SetSeen{};
      auto fallbackTexture = static_cast<nvrhi::ITexture*>(nullptr);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        sets = access.bindingSets.size();
        setsOfA = access.bindingSets.setsOf(a);
        setsOfB = access.bindingSets.setsOf(b);
        dropped = setOf(access, a);
        again = setOf(access, b);
        fallbackTexture = access.images.slot(fallback)->texture.Get();
      }));
      CHECK(sets == 1u);
      CHECK(setsOfA == 0u);
      CHECK(setsOfB == 1u);
      CHECK(dropped.intrinsic);
      CHECK(dropped.texture == fallbackTexture);
      CHECK(again.set == first.second.set);

      // uploads again: the texture is back, and the intrinsic's set of A goes with the
      // version its arrival bumps
      session->setUploadCapForTesting(UploadBytesPerFrame);
      REQUIRE(settledSlots({a}));
      auto back = SetSeen{};
      auto textureBack = static_cast<nvrhi::ITexture*>(nullptr);
      auto made = std::uint64_t{0};
      auto madeBefore = std::uint64_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        madeBefore = access.bindingSets.made();
        back = setOf(access, a);
        made = access.bindingSets.made();
        textureBack = access.images.slot(a)->texture.Get();
        sets = access.bindingSets.size();
      }));
      CHECK_FALSE(back.intrinsic);
      CHECK(back.texture == textureBack);
      // a new set was made for A's new version
      CHECK(made == madeBefore + 1);
      CHECK(sets == 2u);

      // the test's own reference goes before the device
      layout = nullptr;
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    service->requestStop();
    service->join();
    messages.print("slot bump and drop, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION(
    "D68, Q19: every sampler, and DRAW_INTERACTION sets with the compare-enabled s0")
  {
    namespace ei = engine_image;
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      auto colour = NoImageSlot;
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        colour = access.images.provideForTesting(
          testImageKey("textures/lp2/colour"), syntheticImage(16, ei::FormatDxt5, 3));
      }));
      REQUIRE(waitForSlots(*session, {colour}));

      // DRAW_INTERACTION's sets from the compile thread (C20): set 1 is t0-t2, set 3
      // s0-s1
      auto layouts = std::vector<nvrhi::BindingLayoutHandle>{};
      REQUIRE(waitFor(
        [&] {
          inFrame(*session, [&](RenderFrameAccess& access) {
            const auto& lookup =
              access.pipelines.layouts(EngineBindingLayout::DrawInteraction);
            if (lookup.state == BindingLayoutLookup::State::Ready)
            {
              layouts = lookup.layouts;
            }
          });
          return !layouts.empty();
        },
        10000ms,
        false));
      REQUIRE(layouts.size() == 4u);

      struct Seen
      {
        bool common = false;
        nvrhi::SamplerReductionType s0 = nvrhi::SamplerReductionType::Standard;
        nvrhi::SamplerReductionType border = nvrhi::SamplerReductionType::Standard;
        int imageSamplers = 0;
        int refused = 0;
        nvrhi::SamplerReductionType depth = nvrhi::SamplerReductionType::Standard;
        std::size_t samplers = 0;
        bool textures = false;
        bool samplerSet = false;
      };
      auto seen = Seen{};
      auto sets = std::vector<nvrhi::BindingSetHandle>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        const auto* common =
          access.samplers.common(access.device) | kdl::value_or(nullptr);
        if (!common)
        {
          return;
        }
        seen.common = true;
        seen.s0 = common->anisotropicWrap->getDesc().reductionType;
        seen.border = common->linearBorder->getDesc().reductionType;
        // every filter and repeat of SC269, and a depth image's
        for (const auto filter :
             {ei::FilterLinear,
              ei::FilterNearest,
              ei::FilterNearestMipmap,
              ei::FilterDefault})
        {
          for (const auto repeat :
               {ei::RepeatRepeat,
                ei::RepeatClamp,
                ei::RepeatClampToZero,
                ei::RepeatClampToZeroAlpha})
          {
            access.samplers.imageSampler(access.device, ei::FormatRgba8, filter, repeat)
              | kdl::transform([&](nvrhi::ISampler*) { ++seen.imageSamplers; })
              | kdl::if_error([&](const auto&) { ++seen.refused; }) | kdl::ignore();
          }
        }
        // the same (filter, repeat, depth) is the same sampler
        const auto twice = [&] {
          return access.samplers.imageSampler(
                   access.device, ei::FormatDxt5, ei::FilterDefault, ei::RepeatRepeat)
                 | kdl::value_or(nullptr);
        };
        if (twice() != twice())
        {
          ++seen.refused;
        }
        seen.depth = (access.samplers.imageSampler(
                        access.device, ei::FormatDepth, ei::FilterLinear, ei::RepeatClamp)
                      | kdl::value_or(nullptr))
                       ->getDesc()
                       .reductionType;
        access.samplers.imageSampler(access.device, ei::FormatRgba8, 7, ei::RepeatRepeat)
          | kdl::if_error([&](const auto&) { ++seen.imageSamplers; }) | kdl::ignore();
        seen.samplers = access.samplers.size();

        // a colour texture in t0-t2 and the interaction's samplers in s0-s1
        const auto binding = access.images.binding(colour);
        auto textures = access.device.createBindingSet(
          nvrhi::BindingSetDesc{}
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
              0, binding.texture, nvrhi::Format::UNKNOWN, binding.subresources))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
              1, binding.texture, nvrhi::Format::UNKNOWN, binding.subresources))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
              2, binding.texture, nvrhi::Format::UNKNOWN, binding.subresources)),
          layouts[1]);
        auto samplers = access.device.createBindingSet(
          nvrhi::BindingSetDesc{}
            .addItem(nvrhi::BindingSetItem::Sampler(0, common->anisotropicWrap))
            .addItem(nvrhi::BindingSetItem::Sampler(1, common->linearBorder)),
          layouts[3]);
        seen.textures = bool(textures);
        seen.samplerSet = bool(samplers);
        sets = {std::move(textures), std::move(samplers)};
      }));
      CHECK(seen.common);
      // D68: the leaked desc: s0 keeps the Comparison reduction, the border sampler not
      CHECK(seen.s0 == nvrhi::SamplerReductionType::Comparison);
      CHECK(seen.border == nvrhi::SamplerReductionType::Standard);
      CHECK(seen.imageSamplers == 17);
      CHECK(seen.refused == 0);
      CHECK(seen.depth == nvrhi::SamplerReductionType::Comparison);
      // the common eight, 16 colour samplers and a depth one
      CHECK(seen.samplers == 25u);
      CHECK(seen.textures);
      CHECK(seen.samplerSet);
      std::cout
        << "Q19: DRAW_INTERACTION sets with the compare-enabled s0 made; validation "
           "errors "
        << messages.errors << ", warnings " << messages.warnings << "\n";

      REQUIRE(inFrame(*session, [&](RenderFrameAccess&) { sets.clear(); }));
      layouts.clear();
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("samplers, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
}

namespace
{

// the quoted value after each "texture" key of a .map's text (the light shaders)
std::set<std::string> textureKeys(const std::string& text)
{
  auto values = std::set<std::string>{};
  const auto key = std::string_view{"\"texture\""};
  for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
  {
    const auto open = text.find('"', at + key.size());
    const auto close = open == std::string::npos ? open : text.find('"', open + 1);
    if (close != std::string::npos && text.find('\n', at) > close)
    {
      values.insert(text.substr(open + 1, close - open - 1));
    }
  }
  return values;
}

} // namespace

// D72 (LP2.1 step 8): the texture heaps (D50) wait for a measurement: every image of the
// Q20 map's materials and of its lights' shaders made a texture, with the device memory
// allocations counted (C30). Hidden: it needs the retail game (TB_LIT_GAME_PATH, read
// only) and a GPU.
TEST_CASE("LitSession GPU resources on the retail game (D72)", "[.][retail]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the measurement needs the game folder");
  }
  const auto gamePath = std::filesystem::path{gamePathValue};
  const auto started = std::chrono::steady_clock::now();
  // within the 180 s wall-clock bound of a hidden GPU run, with room for its start and
  // stop
  const auto deadline = started + 150s;
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  // the map's materials: its .bproc's surfaces (BprocModel's reader records each
  // surface's material); its lights' shaders: the .map's "texture" keys, when the game
  // has the .map
  const auto mapName = std::string{"maps/game/admin"};
  auto materials = std::set<std::string>{};
  auto lightShaders = std::set<std::string>{};
  auto mapFound = false;
  {
    auto source = GameDeclSource{gamePath};
    auto found = GameFileProbe{};
    const auto bproc = source.readGameFile("generated/" + mapName + ".bproc", &found);
    REQUIRE(bproc);
    const auto file =
      readBprocFile(std::as_bytes(std::span{bproc->data(), bproc->size()}))
      | kdl::value();
    for (const auto& model : file.models)
    {
      for (const auto& surface : model.surfaces)
      {
        materials.insert(surface.material);
      }
    }
    if (const auto map = source.readGameFile(mapName + ".map", &found))
    {
      mapFound = true;
      lightShaders = textureKeys(*map);
    }
    source.closeFiles();
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  }
  REQUIRE_FALSE(materials.empty());

  // the service over the retail game, its image budget off so that nothing is evicted
  auto serviceOptions = MaterialServiceOptions{};
  serviceOptions.makeSource = [gamePath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<GameDeclSource>(gamePath, &cancel)};
  };
  serviceOptions.pollInterval = 0ms;
  serviceOptions.imageBudgetBytes = 0;
  auto service = MaterialService::create(std::move(serviceOptions))
                 | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

  // every stage image of the materials, and the light shaders' falloff and projection
  // images
  auto names = materials;
  names.insert(lightShaders.begin(), lightShaders.end());
  auto entries = std::vector<std::shared_ptr<const MaterialEntry>>{};
  REQUIRE(waitFor(
    [&] {
      entries.clear();
      for (const auto& name : names)
      {
        const auto lookup = service->lookup(name);
        if (lookup.state == MaterialLookup::State::Pending)
        {
          return std::chrono::steady_clock::now() > deadline;
        }
        if (lookup.entry)
        {
          entries.push_back(lookup.entry);
        }
      }
      return true;
    },
    150000ms,
    false));
  auto keys = std::vector<MaterialImageKey>{};
  auto unique = std::unordered_set<MaterialImageKey, MaterialImageKeyHash>{};
  const auto add = [&](const std::optional<MaterialImageKey>& key) {
    if (key && unique.insert(*key).second)
    {
      keys.push_back(*key);
    }
  };
  for (const auto& entry : entries)
  {
    for (const auto& stage : entry->stages)
    {
      add(stage.image);
    }
    if (lightShaders.contains(entry->name))
    {
      add(entry->lightFalloffImage);
    }
  }

  auto messages = ValidationMessages{};
  const auto logPath =
    std::filesystem::temp_directory_path() / "TbLitLibTest-d72-frame-log.csv";
  std::filesystem::remove(logPath);
  struct Peak
  {
    std::int64_t allocations = 0;
    std::size_t resident = 0;
    std::size_t defaulted = 0;
    std::size_t pending = 0;
    std::size_t slots = 0;
    GpuMemoryAccount account;
    std::optional<VramUsage> vram;
  };
  auto peak = Peak{};
  auto atStart = std::optional<VramUsage>{};
  auto allocationsAfterStop = std::int64_t{-1};
  auto loadSeconds = 0.0;
  {
    const auto instance = TestInstance{library, true, false, messages};
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto events = Events{};
    auto session = gpuSession(instance, events, 1, service.get(), gamePath, logPath);
    atStart = session->vramForTesting();
    session->setExposed(true);
    REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

    const auto requested = std::chrono::steady_clock::now();
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      for (const auto& key : keys)
      {
        access.images.request(key);
      }
    }));
    // until no slot is Pending nor Loading, as the owner passes every table on
    auto settled = false;
    while (!settled && std::chrono::steady_clock::now() < deadline)
    {
      session->tableChanged();
      inFrame(*session, [&](RenderFrameAccess& access) {
        auto loading = access.images.hasLoading();
        peak.pending = access.images.pending();
        peak.slots = access.images.size();
        settled = peak.pending == 0 && !loading;
        const auto account = accountGpuMemory(
          access.frame, access.arenas, access.images, access.passes, access.tonemap);
        if (account.total() >= peak.account.total())
        {
          peak.account = account;
          peak.resident = access.images.resident();
        }
      });
      peak.allocations = std::max(peak.allocations, session->deviceMemoryAllocations());
    }
    loadSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - requested).count();
    peak.vram = session->vramForTesting();
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      for (std::uint32_t id = 0; id < access.images.size(); ++id)
      {
        peak.defaulted +=
          access.images.slot(id)->state == ImageSlots::State::Default ? 1 : 0;
      }
    }));
    CHECK(settled);

    teardown(*session);
    allocationsAfterStop = session->deviceMemoryAllocations();
    CHECK(session->lastDeviceReleaseCount() == 0u);
    CHECK(session->nvrhiErrorCount() == 0u);
    CHECK(events.count(SessionEvent::Error) == 0u);
  }
  CHECK(resetDispatcher());
  service->requestStop();
  service->join();

  // the frame log's alloc_count column agrees
  auto loggedPeak = std::int64_t{0};
  auto stopAllocations = std::string{};
  for (const auto& row : readCsv(logPath))
  {
    if (row.size() == 42u && row[1] != "event" && !row[14].empty() && row[0] != "session")
    {
      loggedPeak = std::max(loggedPeak, std::int64_t(std::stoll(row[14])));
      if (row[1] == "stop")
      {
        stopAllocations = row[14];
      }
    }
  }
  std::filesystem::remove(logPath);

  const auto mb = [](const std::uint64_t bytes) {
    return double(bytes) / (1024.0 * 1024.0);
  };
  std::cout << fmt::format(
    "D72 on {}: {} materials from the .bproc's surfaces, {} light shaders from the .map "
    "({}), {} image keys, {} slots ({} intrinsics among them): {} with a texture, {} "
    "Default, {} still pending; loaded in {:.1f} s\n",
    mapName,
    materials.size(),
    lightShaders.size(),
    mapFound ? "found" : "not in the game",
    keys.size(),
    peak.slots,
    peak.slots >= keys.size() ? peak.slots - keys.size() : 0,
    peak.resident,
    peak.defaulted,
    peak.pending,
    loadSeconds);
  std::cout << fmt::format(
    "D72 peak: alloc_count {} (frame log {}; the limit {}, a quarter {}), textures "
    "{:.1f} "
    "MB, arenas {:.1f} MB, targets and buffers {:.1f} MB, released {:.1f} MB, total "
    "{:.1f} MB against the default budget {:.0f} MB\n",
    peak.allocations,
    loggedPeak,
    4096,
    1024,
    mb(peak.account.textureBytes),
    mb(peak.account.arenaBytes),
    mb(peak.account.renderTargetBytes),
    mb(peak.account.releasedBytes),
    mb(peak.account.total()),
    mb(DefaultVramBudgetBytes));
  std::cout << fmt::format(
    "D72 VRAM (VK_EXT_memory_budget): at start {:.1f} of {:.1f} MB, at the peak {:.1f} "
    "MB; allocations after Stop {} (the stop row before step 2: {})\n",
    atStart ? mb(atStart->usedBytes) : 0.0,
    atStart ? mb(atStart->budgetBytes) : 0.0,
    peak.vram ? mb(peak.vram->usedBytes) : 0.0,
    allocationsAfterStop,
    stopAllocations);
  // back at the baseline: every allocation freed with the session
  CHECK(allocationsAfterStop == 0);
  CHECK(loggedPeak == peak.allocations);
  messages.print("D72, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

namespace
{

// LP2.2a: the ten programs' blobs LP2 loads (C26), copied out of the game folder (read
// only) into a test game root
constexpr auto Lp2Programs = std::array<int, 10>{
  EngineBuiltinColor,
  EngineBuiltinTextureVertexColor,
  EngineBuiltinInteraction,
  EngineBuiltinPbrInteraction,
  EngineBuiltinDepth,
  EngineBuiltinBlit,
  EngineBuiltinRect,
  EngineBuiltinTonemapping,
  EngineBuiltinHistogram,
  EngineBuiltinExposure};

// the light stand-ins (a white light of no falloff: the default light shaders the
// fixture map's lights reach) and a lit translucent material, written into the root
const auto Lp22aMaterials = std::string{
  "lights/squarelight1\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultPointLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "textures/lp2/litglass\n{\n\ttranslucent\n\tqer_editorimage "
  "textures/lp1b/glass.tga\n\tdiffusemap textures/lp1b/glass.tga\n}\n"
  // V2.2: white to the volume's planes and past them, so that the static interaction
  // cull, not the image, is what darkens a face on a plane
  "lights/lp22a/flatclamp\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tclamp\n\t}\n}\n"};

/**
 * LP2.2a (P2.18): a game root assembled by name in a temp folder: the LP1b fixture map,
 * its .proc, the fixture's materials and textures, the stand-ins above, and copies of the
 * LP2 blobs from the game folder (never written to). An empty path when a blob is
 * missing.
 */
std::filesystem::path assembleLitRoot(const std::filesystem::path& gamePath)
{
  const auto root = std::filesystem::temp_directory_path() / "TbLitLibTest-lp22a";
  std::filesystem::remove_all(root);
  const auto base = root / "base";
  const auto fixture = litFixtureFolder();
  std::filesystem::create_directories(base / "maps");
  std::filesystem::create_directories(base / "materials");
  std::filesystem::copy_file(
    fixture / "maps" / "lp1b_geometry.map", base / "maps" / "lp1b_geometry.map");
  std::filesystem::copy_file(
    fixture / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc",
    base / "maps" / "lp1b_geometry.proc");
  std::filesystem::copy_file(
    fixture / "materials" / "lp1bc.mtr", base / "materials" / "lp1bc.mtr");
  writeTextFile(base / "materials" / "zz_lp22a.mtr", Lp22aMaterials);
  // R_ParseImageProgram turns a stage's _white into guis/assets/white, a file of the
  // game (Image_program.cpp:490-494): a 4 × 4 white TGA (uncompressed, 32 bits)
  {
    auto tga = std::string(18, '\0');
    tga[2] = 2;
    tga[12] = 4;
    tga[14] = 4;
    tga[16] = 32;
    tga[17] = 8;
    tga += std::string(4 * 4 * 4, char(-1));
    std::filesystem::create_directories(base / "guis" / "assets");
    auto file = std::ofstream{base / "guis" / "assets" / "white.tga", std::ios::binary};
    file.write(tga.data(), std::streamsize(tga.size()));
  }
  std::filesystem::copy(
    fixture / "textures", base / "textures", std::filesystem::copy_options::recursive);
  for (const auto program : Lp2Programs)
  {
    const auto* builtin = engineBuiltin(program);
    for (const auto& [flag, stage] :
         {std::pair{EngineStageVertex, ShaderStage::Vertex},
          std::pair{EngineStageFragment, ShaderStage::Pixel},
          std::pair{EngineStageCompute, ShaderStage::Compute}})
    {
      if (!(builtin->stages & flag))
      {
        continue;
      }
      const auto relative = shaderBlobPath(builtin->name, stage);
      const auto from = gamePath / "base" / relative;
      if (!std::filesystem::exists(from))
      {
        return {};
      }
      const auto to = base / relative;
      std::filesystem::create_directories(to.parent_path());
      std::filesystem::copy_file(
        from, to, std::filesystem::copy_options::overwrite_existing);
    }
  }
  return root;
}

// the fixture map's text with a box of lp2/litglass in front of the pillars (in
// lp1b_light), and with every light's colour set to colour when given
std::string lp22aMapText(const std::optional<std::string>& colour = std::nullopt)
{
  auto map = readTextFile(litFixtureFolder() / "maps" / "lp1b_geometry.map");
  const auto entity1 = map.find("// entity 1");
  map.insert(
    map.rfind('}', entity1), boxBrush(-40, -16, 16, -36, 16, 48, "lp2/litglass"));
  if (!colour)
  {
    return map;
  }
  auto lines = std::vector<std::string>{};
  auto stream = std::istringstream{map};
  auto line = std::string{};
  auto out = std::string{};
  while (std::getline(stream, line))
  {
    if (line.find("\"_color\"") != std::string::npos)
    {
      continue;
    }
    out += line + "\n";
    if (line == "\"classname\" \"light\"")
    {
      out += fmt::format("\"_color\" \"{}\"\n", *colour);
    }
  }
  return out;
}

std::shared_ptr<const Snapshot> lp22aSnapshot(
  const std::string& text,
  const std::filesystem::path& root,
  const std::uint64_t generation,
  const std::uint64_t sequence)
{
  auto snapshot = snapshotOfMapText(text);
  snapshot.generation = generation;
  snapshot.sequence = sequence;
  snapshot.mapPath = (root / "base" / "maps" / "lp1b_geometry.map").string();
  return std::make_shared<const Snapshot>(std::move(snapshot));
}

CameraState litCamera(
  const vm::vec3f& position, const vm::vec3f& direction, const vm::vec3f& up)
{
  auto camera = CameraState{};
  camera.position = position;
  camera.direction = direction;
  camera.up = up;
  camera.fovDegrees = 90.0f;
  camera.viewportWidth = 160;
  camera.viewportHeight = 120;
  return camera;
}

float halfToFloat(const std::uint16_t half)
{
  const auto sign = (half >> 15) & 1;
  const auto exponent = (half >> 10) & 0x1f;
  const auto mantissa = half & 0x3ff;
  auto value = 0.0f;
  if (exponent == 0)
  {
    value = std::ldexp(float(mantissa), -24);
  }
  else if (exponent == 31)
  {
    value = mantissa ? std::numeric_limits<float>::quiet_NaN()
                     : std::numeric_limits<float>::infinity();
  }
  else
  {
    value = std::ldexp(float(mantissa | 0x400), exponent - 25);
  }
  return sign ? -value : value;
}

// the HDR target read back (RGBA16F as floats, rows top-down)
struct HdrImage
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> rgba;

  std::array<float, 4> at(const std::uint32_t x, const std::uint32_t y) const
  {
    const auto i = (std::size_t(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
  }

  // the largest rgb component in the (2r + 1)² pixels around (x, y)
  float brightest(const std::uint32_t x, const std::uint32_t y, const int r = 2) const
  {
    auto most = 0.0f;
    for (auto dy = -r; dy <= r; ++dy)
    {
      for (auto dx = -r; dx <= r; ++dx)
      {
        const auto p = at(std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy));
        most = std::max({most, p[0], p[1], p[2]});
      }
    }
    return most;
  }
};

// what a settled frame recorded, gathered on the render thread
struct SettledFrame
{
  LitFrameStats stats;
  std::size_t drawSurfs = 0;
  std::size_t lights = 0;
};

/**
 * Waits until a frame of camera is settled (no pending pipeline, image, chunk, material
 * or evaluation; every slot answered and written; the lists hold drawSurfs) and reads
 * its HDR target back through a staging texture copied after the passes; nullopt at the
 * deadline.
 */
std::optional<std::pair<HdrImage, SettledFrame>> settledHdr(
  LitSession& session,
  const CameraState& camera,
  const std::size_t minLights,
  const std::chrono::milliseconds timeout = 60000ms)
{
  session.setCamera(camera);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.tableChanged();
    auto staging = nvrhi::StagingTextureHandle{};
    auto frame = SettledFrame{};
    auto settled = false;
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterPassesForTesting([&, done](RenderFrameAccess& access) {
      const auto& stats = access.passes.lastStats();
      const auto& lists = access.passes.lists();
      frame.stats = stats;
      frame.drawSurfs = lists.drawSurfs.size();
      frame.lights = lists.lights.size();
      settled = stats.camera == camera && !stats.unconverged
                && access.images.pending() == 0 && !access.images.hasLoading()
                && !lists.drawSurfs.empty() && lists.lights.size() >= minLights
                && !access.arenas.pending();
      if (settled)
      {
        auto* hdr = access.frame.find(frame_resource::CurrentRenderHdr)->texture;
        const auto& desc = hdr->getDesc();
        staging = access.device.createStagingTexture(
          nvrhi::TextureDesc{}
            .setWidth(desc.width)
            .setHeight(desc.height)
            .setFormat(desc.format),
          nvrhi::CpuAccessMode::Read);
        access.commandList.copyTexture(
          staging, nvrhi::TextureSlice{}, hdr, nvrhi::TextureSlice{});
      }
      done->set_value();
    });
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (!settled)
    {
      std::this_thread::sleep_for(50ms);
      continue;
    }
    auto image = HdrImage{};
    const auto read = inFrame(session, [&](RenderFrameAccess& access) {
      const auto& desc = staging->getDesc();
      image.width = desc.width;
      image.height = desc.height;
      image.rgba.resize(std::size_t(desc.width) * desc.height * 4);
      auto pitch = std::size_t{0};
      const auto* bytes =
        static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
          staging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
      for (std::uint32_t y = 0; y < desc.height; ++y)
      {
        const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
        for (std::uint32_t x = 0; x < desc.width * 4; ++x)
        {
          image.rgba[std::size_t(y) * desc.width * 4 + x] = halfToFloat(row[x]);
        }
      }
      access.device.unmapStagingTexture(staging);
      staging = nullptr;
    });
    if (!read)
    {
      return std::nullopt;
    }
    return std::pair{std::move(image), frame};
  }
  return std::nullopt;
}

// a material service over a game root, its image budget off
std::unique_ptr<MaterialService> rootService(
  const std::filesystem::path& root, const std::size_t imageBudgetBytes = 0)
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [root](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
  };
  options.pollInterval = 0ms;
  options.imageBudgetBytes = imageBudgetBytes;
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// a headless 160 × 120 session with validation over the root; startPipelines false for a
// test that holds the compile thread from the start (DX.5)
std::unique_ptr<LitSession> rootSession(
  const TestInstance& instance,
  Events& events,
  MaterialService& service,
  const std::filesystem::path& root,
  const bool startPipelines = true)
{
  auto options = SessionOptions{};
  options.validation = true;
  options.gamePath = root;
  options.requestStartPipelines = startPipelines;
  return LitSession::create(
           instance.handles(),
           VK_NULL_HANDLE,
           160,
           120,
           options,
           events.sink(),
           7,
           &service)
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// T0.2: the mirrored vertex-input warnings a pipeline of program implies (its vertex
// shader's unread LAYOUT_DRAW_VERT locations)
// DX.5: the lit passes' keys the compile thread was asked for: the draws' and the ones
// the session asked for at Start
std::set<PipelineKey, PipelineKeyLess> litKeysMade(
  const std::set<PipelineKey, PipelineKeyLess>& requested,
  const nvrhi::Format depthFormat)
{
  auto keys = requested;
  for (const auto& key : litStartPipelineKeys(depthFormat))
  {
    keys.insert(key);
  }
  return keys;
}

int unreadLocations(const int program)
{
  switch (program)
  {
  case EngineBuiltinDepth:
    return 5;
  case EngineBuiltinTextureVertexColor:
    return 3;
  case EngineBuiltinInteraction:
  case EngineBuiltinPbrInteraction:
    return 1;
  default:
    return 0;
  }
}

} // namespace

// LP2.2a: the depth prepass and the interactions draw the LP1b fixture map lit into the
// HDR target, read back through a staging texture (the tonemap and the blit are
// LP2.2b's). Hidden: the blobs live only in the game folder (TB_LIT_GAME_PATH, P2.18),
// copied out of it into a test game root; it needs a GPU.
TEST_CASE("LitSession lit passes on a GPU (LP2.2a)", "[.][litgpu]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  SECTION("the fixture map lit: probes, D76, the perforated prepass, the lists' order")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    auto programsAsked = std::vector<int>{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root, false);
      auto* compiler = session->pipelineCompilerForTesting();
      REQUIRE(compiler);

      // D77: the compile thread is held, so the first frames find their pipelines pending
      auto gate = std::make_shared<ParseGate>();
      gate->close();
      const auto openGate = OpenAtExit{gate};
      compiler->setBeforeCreateForTesting([gate] { gate->pass(); });

      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      // every draw of the session is recorded, for the pipelines it implies (T0.2)
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setDrawLogForTesting(true);
      }));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));

      // camera 1: above the floor at (60 0 0), looking down, inside lp1b_light
      const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
      session->setCamera(lit);
      auto pendingSeen = LitFrameStats{};
      REQUIRE(waitFor(
        [&] {
          session->tableChanged();
          inFrame(*session, [&](RenderFrameAccess& access) {
            const auto& stats = access.passes.lastStats();
            if (stats.pendingPipelines > 0 && access.passes.lists().lights.size() > 0)
            {
              pendingSeen = stats;
            }
          });
          return pendingSeen.pendingPipelines > 0;
        },
        60000ms,
        false));
      // a pending pipeline skips its draws and leaves the frame unconverged
      CHECK(pendingSeen.unconverged);
      CHECK(pendingSeen.skippedDraws > 0u);
      gate->open();

      // the next frames converge
      const auto lit1 = settledHdr(*session, lit, 1);
      REQUIRE(lit1);
      const auto& [litImage, litFrame] = *lit1;
      CHECK(litFrame.stats.pendingPipelines == 0u);
      CHECK(litFrame.stats.prepassDraws > 0u);
      CHECK(litFrame.stats.interactionDraws > 0u);
      CHECK_FALSE(litFrame.stats.prepassDisabled);
      CHECK_FALSE(litFrame.stats.interactionsDisabled);
      CHECK(litFrame.stats.lightsOverflowed == 0u);
      // the floor at (60 0 0), at the centre of the target: lit
      const auto centre = litImage.brightest(80, 60);
      CHECK(centre > 0.0f);
      CHECK(litImage.at(80, 60)[3] == 1.0f);
      // LP4.1: the frame was drawn through the view's portal flow over the fixture's
      // .proc (two areas; the camera in area 0, the east room), and both of its lights
      // reference both areas, so the pixels are LP2.2a's
      CHECK(litFrame.stats.areas == 2u);
      CHECK(litFrame.stats.viewArea == 0);
      CHECK(litFrame.stats.areasFlowed >= 1u);
      CHECK(litFrame.stats.lightsInView >= litFrame.stats.lightsVisible);
      std::cout << fmt::format(
        "LP4.1 flow of the lit frame: view area {}, {} of {} areas flowed, {} chains, {} "
        "lights in view, {} with interactions\n",
        litFrame.stats.viewArea,
        litFrame.stats.areasFlowed,
        litFrame.stats.areas,
        litFrame.stats.portalChains,
        litFrame.stats.lightsInView,
        litFrame.stats.lightsVisible);
      std::cout << fmt::format(
        "LP2.2a lit probe (floor at 60 0 0): {:.4f} {:.4f} {:.4f}; lights {}, "
        "interaction draws {}, prepass draws {}, constant writes {} ({} bytes, the "
        "frame's "
        "budget {}), push-constant bytes {}\n",
        litImage.at(80, 60)[0],
        litImage.at(80, 60)[1],
        litImage.at(80, 60)[2],
        litFrame.lights,
        litFrame.stats.interactionDraws,
        litFrame.stats.prepassDraws,
        litFrame.stats.constantWrites,
        litFrame.stats.constantBytes,
        litFrame.stats.constantWriteBudget,
        litFrame.stats.pushConstantBytes);
      {
        const auto lock = std::scoped_lock{messages.mutex};
        std::cout << "Q19, the first interaction draws' validation output: "
                  << messages.errors << " errors, " << messages.warnings << " warnings";
        for (const auto& text : messages.texts)
        {
          std::cout << "\n  " << text;
        }
        std::cout << std::endl;
      }

      // camera 2: from x -100 looking west at the west wall, outside every light
      const auto dark =
        settledHdr(*session, litCamera({-100, 0, 64}, {-1, 0, 0}, {0, 0, 1}), 0);
      REQUIRE(dark);
      CHECK(dark->first.brightest(80, 60) == 0.0f);
      std::cout << fmt::format(
        "LP2.2a unlit probe (west wall at -256 0 64): {}\n",
        dark->first.brightest(80, 60));

      // camera 3: from the west at y -80 looking east: the pillar's west face at
      // (-8 -80 80) faces away from lp1b_light and hides the lit faces behind it, which
      // the prepass and DEPTHFUNC_EQUAL keep dark
      const auto hidden =
        settledHdr(*session, litCamera({-240, -80, 80}, {1, 0, 0}, {0, 0, 1}), 1);
      REQUIRE(hidden);
      CHECK(hidden->first.brightest(80, 60) == 0.0f);
      std::cout << fmt::format(
        "LP2.2a occluded probe (pillar face at -8 -80 80): {}\n",
        hidden->first.brightest(80, 60));

      // camera 4: from x 200 at y 32, z 4 looking west at the grate's east face (x 104),
      // which faces away from lp1b_light: through its hole (y 32..40, z 0..8: texels 0-7
      // of rows 24-31, alpha 0) the lit pillar at x 8 shows, its opaque part (y 40..48)
      // stays black
      const auto grate =
        settledHdr(*session, litCamera({200, 32, 4}, {-1, 0, 0}, {0, 0, 1}), 1);
      REQUIRE(grate);
      const auto hole = grate->first.brightest(83, 60, 1);
      const auto bar = grate->first.brightest(90, 60, 1);
      CHECK(hole > 0.0f);
      CHECK(bar == 0.0f);
      std::cout << fmt::format(
        "LP2.2a perforated probes: behind the hole {}, on the opaque part {}\n",
        hole,
        bar);

      // the recorded draw list of one frame from x -100 looking east at the lit glass
      // (x -40..-36, in lp1b_light): per light, the translucent interactions after the
      // local and global ones
      const auto glass = litCamera({-100, 0, 32}, {1, 0, 0}, {0, 0, 1});
      REQUIRE(settledHdr(*session, glass, 1));
      auto logged = std::size_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        logged = access.passes.drawLogForTesting().size();
      }));
      auto log = std::vector<LitDrawRecord>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        const auto& all = access.passes.drawLogForTesting();
        log.assign(all.begin() + std::ptrdiff_t(logged), all.end());
      }));
      auto translucent = 0;
      auto lightsWithTranslucent = std::set<PreviewId>{};
      for (std::size_t i = 0; i < log.size(); ++i)
      {
        if (
          log[i].pass != LitDrawRecord::Pass::Interaction
          || log[i].list != InteractionList::Translucent)
        {
          continue;
        }
        ++translucent;
        lightsWithTranslucent.insert(log[i].light);
        CHECK(log[i].chunk.material == "textures/lp2/litglass");
        // DEPTHFUNC_LESS for the translucent list
        CHECK((log[i].state & (3ull << 13)) == engine_gls::DepthFuncLess);
        for (std::size_t j = i + 1; j < log.size(); ++j)
        {
          if (
            log[j].pass == LitDrawRecord::Pass::Interaction
            && log[j].light == log[i].light)
          {
            CHECK(log[j].list == InteractionList::Translucent);
          }
        }
      }
      CHECK(translucent > 0);
      // the prepass first, every draw of a light within its scissor
      auto prepassDone = false;
      for (const auto& record : log)
      {
        if (record.pass == LitDrawRecord::Pass::Interaction)
        {
          prepassDone = true;
          CHECK(
            (record.state & (3ull << 13))
            == (record.list == InteractionList::Translucent ? engine_gls::DepthFuncLess : engine_gls::DepthFuncEqual));
        }
        else
        {
          CHECK_FALSE(prepassDone);
        }
      }

      // D76: the frame's writes at maxVersions 3 (a budget of one write) overflow every
      // light, without NVRHI's "maxVersions insufficient" error, and say so once
      const auto warningsBefore = events.count(SessionEvent::Warning);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setConstantBufferVersionsForTesting(3);
      }));
      auto overflowed = LitFrameStats{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        overflowed = access.passes.lastStats();
      }));
      CHECK(overflowed.constantWriteBudget == 1u);
      CHECK(overflowed.lightsOverflowed > 0u);
      CHECK(overflowed.lightsOverflowed == overflowed.lightsVisible);
      CHECK(overflowed.constantWrites <= 1u);
      CHECK(events.count(SessionEvent::Warning) == warningsBefore + 1);
      CHECK(session->nvrhiErrorCount() == 0u);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setConstantBufferVersionsForTesting(
          EngineFrame.constantBufferVersions);
      }));
      const auto again = settledHdr(*session, lit, 1);
      REQUIRE(again);
      CHECK(again->second.stats.lightsOverflowed == 0u);
      CHECK(again->first.brightest(80, 60) > 0.0f);

      // the lights-off control: every light's _color 0 0 0 is black everywhere
      REQUIRE(waitFor(
        [&] {
          return session->offerSnapshot(lp22aSnapshot(lp22aMapText("0 0 0"), root, 7, 2));
        },
        10000ms,
        false));
      auto control = std::optional<std::pair<HdrImage, SettledFrame>>{};
      REQUIRE(waitFor(
        [&] {
          control = settledHdr(*session, lit, 0, 10000ms);
          return control && control->second.lights == 0u;
        },
        60000ms,
        false));
      auto brightest = 0.0f;
      for (std::size_t i = 0; i < control->first.rgba.size(); i += 4)
      {
        brightest = std::max(
          {brightest,
           control->first.rgba[i],
           control->first.rgba[i + 1],
           control->first.rgba[i + 2]});
      }
      CHECK(brightest == 0.0f);
      std::cout << fmt::format("LP2.2a lights-off control: brightest {}\n", brightest);

      // the pipeline keys the session's draws asked for, drawn or not
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto& key : access.passes.requestedKeysForTesting())
        {
          programsAsked.push_back(key.program);
        }
        access.passes.setDrawLogForTesting(false);
      }));
      std::cout << "LP2.2a pipelines asked for: " << programsAsked.size() << ", made "
                << compiler->pipelinesCreatedForTesting() << "\n";
      // LP2.2b: and the tonemap passes' four, histogram, exposure, tonemapping and the
      // headless blit, none with an input layout (no mirrored warning)
      CHECK(programsAsked.size() + 4u == compiler->pipelinesCreatedForTesting());

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      events.print("lit passes");
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("lit passes, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    // T0.2 (M-PIPE-DEFECT-01): the mirrored warnings the pipelines made imply
    auto implied = 0;
    for (const auto program : programsAsked)
    {
      implied += unreadLocations(program);
    }
    std::cout << "lit passes, mirrored warnings: " << messages.mirrored << " of "
              << implied << " implied by the pipelines made" << std::endl;
    CHECK(
      messages.mirrored
      == (limitRaised ? implied : std::min(implied, ValidationDuplicateMessageLimit)));
  }

  SECTION("DX.4: no frame is settled while the bake or an entityDef is pending")
  {
    // V2.4 (32-LP2-VERIFY.md): once the parse thread answers, a settled frame comes by
    // itself (framesPresented moves without a frame asked for): the snapshot with its
    // map path (the bake and the lights' entityDefs), the same without a map path (the
    // entityDefs alone), and its worldspawn alone with the map path (the bake)
    const auto text = lp22aMapText();
    const auto worldspawnOnly = text.substr(0, text.find("// entity 1"));
    struct Variant
    {
      std::string label;
      std::shared_ptr<const Snapshot> snapshot;
      bool entityDefs = false;
      bool bake = false;
    };
    auto withoutBake = snapshotOfMapText(text);
    withoutBake.generation = 7;
    withoutBake.sequence = 1;
    const auto variants = std::vector<Variant>{
      {"the bake and the entityDefs", lp22aSnapshot(text, root, 7, 1), true, true},
      {"the entityDefs alone",
       std::make_shared<const Snapshot>(std::move(withoutBake)),
       true,
       false},
      // the worldspawn asks for its entityDef too
      {"the bake (and the worldspawn's entityDef)",
       lp22aSnapshot(worldspawnOnly, root, 7, 1),
       true,
       true},
    };
    for (const auto& variant : variants)
    {
      CAPTURE(variant.label);
      auto messages = ValidationMessages{};
      messages.mirroredMessageId = OutputNotConsumedId;
      {
        const auto instance = TestInstance{library, true, false, messages};
        REQUIRE(instance.valid());
        REQUIRE(initInstanceDispatcher(instance.handles()));
        // the parse thread waits at the gate after its first build: the snapshot's
        // entityDefs and the bake of its map path stay pending
        auto gate = std::make_shared<ParseGate>();
        gate->close();
        auto options = MaterialServiceOptions{};
        options.makeSource = [root](const std::atomic<bool>& cancel) {
          return std::unique_ptr<DeclSource>{
            std::make_unique<GameDeclSource>(root, &cancel)};
        };
        options.pollInterval = 0ms;
        options.imageBudgetBytes = 0;
        options.beforeTakeWorkForTesting = [gate] { gate->pass(); };
        auto service = MaterialService::create(std::move(options))
                       | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
        const auto atExit = OpenAtExit{gate};
        REQUIRE(service->waitForTableForTesting(
          [](const MaterialTable& t) { return t.registry != 0; }, 10s));
        auto events = Events{};
        auto session = rootSession(instance, events, *service, root);
        session->setExposed(true);
        REQUIRE(waitFor(
          [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
          5000ms,
          false));
        // V2.7: every pipeline key the draws ask for, from the first frame on
        REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
          access.passes.setDrawLogForTesting(true);
        }));
        REQUIRE(session->offerSnapshot(variant.snapshot));
        session->setCamera(litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0}));

        // the first batch applies with the parse thread held: not settled
        auto seen = FrameConvergence{};
        REQUIRE(waitFor(
          [&] {
            inFrame(
              *session, [&](RenderFrameAccess& access) { seen = access.convergence; });
            return session->geometryBatchesForTesting() >= 1u
                   && (seen.entityDefsPending || seen.geometryInFlight);
          },
          30000ms,
          false));
        CHECK(seen.entityDefsPending == variant.entityDefs);
        CHECK((!variant.bake || seen.geometryInFlight));
        CHECK_FALSE(seen.settled());
        CHECK_FALSE(session->lastFrameSettledForTesting());

        // the service answers; no frame is asked for: the scene's change triggers the
        // frames that settle
        const auto framesAtOpen = session->framesPresented();
        gate->open();
        CHECK(waitFor(
          [&] {
            return session->framesPresented() > framesAtOpen
                   && session->lastFrameSettledForTesting();
          },
          60000ms,
          false));
        const auto framesSettled = session->framesPresented();
        auto settledSeen = FrameConvergence{};
        auto asked = std::set<PipelineKey, PipelineKeyLess>{};
        auto startKeys = std::vector<PipelineKey>{};
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          settledSeen = access.convergence;
          asked = access.passes.requestedKeysForTesting();
          startKeys = litStartPipelineKeys(access.frame.depthFormat());
          access.passes.setDrawLogForTesting(false);
        }));
        // V2.7 (DX.5): the lit passes ask for no key the session did not ask for at Start
        CHECK_FALSE(asked.empty());
        for (const auto& key : asked)
        {
          CAPTURE(key.program, key.state, key.depthBias, key.slopeBias);
          CHECK(std::ranges::find(startKeys, key) != startKeys.end());
        }
        CHECK(settledSeen.settled());
        CHECK_FALSE(settledSeen.entityDefsPending);
        CHECK_FALSE(settledSeen.geometryInFlight);
        CHECK_FALSE(settledSeen.lightsProvisional);
        std::cout << fmt::format(
          "V2.4 {}: settled by itself after {} frames\n",
          variant.label,
          framesSettled - framesAtOpen);

        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->nvrhiErrorCount() == 0u);
        CHECK(session->deviceMemoryAllocations() == 0);
        service->requestStop();
      }
      CHECK(resetDispatcher());
      messages.print("DX.4 the bake and entityDefs pending, validation");
      CHECK(messages.errors == 0);
      CHECK(messages.warnings == 0);
    }
  }

  SECTION("U2.1: a small image budget lets the uploaded images' bytes go")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      // S2.4 at one byte: every image a consumer has taken is dropped from the table at
      // the next publication
      auto service = rootService(root, 1);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));

      // every image a slot is handed, seen while it waits to be written: nothing is
      // uploaded until every slot has its answer
      session->setUploadCapForTesting(0);
      auto mutex = std::mutex{};
      auto seen = std::map<ImageSlotId, std::weak_ptr<const CachedImage>>{};
      auto seenBytes = std::map<ImageSlotId, std::size_t>{};
      session->setEveryFrameHookForTesting([&](RenderFrameAccess& access) {
        const auto lock = std::scoped_lock{mutex};
        for (ImageSlotId id = 0; id < access.images.size(); ++id)
        {
          const auto& pixels = access.images.slot(id)->pixels;
          if (pixels && !seen.contains(id))
          {
            seen.emplace(id, pixels);
            seenBytes.emplace(id, pixels->data.size());
          }
        }
      });
      auto ids = std::vector<ImageSlotId>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto* name :
             {"floor", "wall", "crate", "checker", "patch", "legacy", "pbr"})
        {
          ids.push_back(
            access.images.request(testImageKey(fmt::format("textures/lp2/{}", name))));
          auto bump = testImageKey(fmt::format("textures/lp2/{}_local", name));
          bump.usage = engine_image::UsageBump;
          if (std::string_view{name} != "checker" && std::string_view{name} != "legacy")
          {
            ids.push_back(access.images.request(bump));
          }
        }
      }));
      REQUIRE(waitFor(
        [&] {
          auto answered = false;
          inFrame(*session, [&](RenderFrameAccess& access) {
            answered = std::ranges::all_of(ids, [&](const ImageSlotId id) {
              return access.images.slot(id)->state != ImageSlots::State::Pending;
            });
          });
          return answered;
        },
        60000ms,
        false));
      session->setUploadCapForTesting(UploadBytesPerFrame);
      REQUIRE(waitForSlots(*session, ids, 60000ms));
      session->setEveryFrameHookForTesting({});
      settle(*session);

      auto alive = std::size_t{0};
      auto aliveBytes = std::size_t{0};
      auto totalBytes = std::size_t{0};
      {
        const auto lock = std::scoped_lock{mutex};
        for (const auto& [id, weak] : seen)
        {
          totalBytes += seenBytes[id];
          if (!weak.expired())
          {
            ++alive;
            aliveBytes += seenBytes[id];
          }
        }
      }
      const auto stats = service->stats();
      std::cout << "U2.1: " << seen.size() << " images handed to slots (" << totalBytes
                << " bytes); alive after their upload " << alive << " (" << aliveBytes
                << " bytes); the table holds " << stats.imageBytes << " bytes, "
                << stats.evictedImages << " evicted\n";
      CHECK(seen.size() >= ids.size());
      CHECK(stats.evictedImages > 0u);
      // only what the service's table still holds is alive: no slot keeps the bytes
      CHECK(aliveBytes <= stats.imageBytes);
      CHECK(alive < seen.size());

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      service->requestStop();
    }
    CHECK(resetDispatcher());
    messages.print("U2.1 image budget, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("a Stop in the middle of a frame")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto stoppedEarly = std::make_shared<std::atomic<bool>>(false);
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));
      const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
      REQUIRE(settledHdr(*session, lit, 1));

      // the next frame is held before its passes until the stop is asked for: the passes
      // stop at their first boundary, the frame is submitted and the thread exits
      auto held = std::make_shared<std::promise<void>>();
      auto heldFuture = held->get_future();
      auto release = std::make_shared<std::promise<void>>();
      auto releaseFuture = release->get_future().share();
      auto once = std::make_shared<std::atomic<bool>>(false);
      // the capture of the held frame's stats is queued by the hook itself, so that it
      // runs after that frame's passes and no earlier frame's
      auto* const lit2 = session.get();
      session->setEveryFrameHookForTesting(
        [held, releaseFuture, once, lit2, stoppedEarly](RenderFrameAccess&) {
          if (!once->exchange(true))
          {
            lit2->runAfterPassesForTesting([stoppedEarly](RenderFrameAccess& access) {
              *stoppedEarly = access.passes.lastStats().stoppedEarly;
            });
            held->set_value();
            releaseFuture.wait();
          }
        });
      session->requestFrame();
      REQUIRE(heldFuture.wait_for(10000ms) == std::future_status::ready);
      const auto stopStart = std::chrono::steady_clock::now();
      session->requestStop();
      release->set_value();
      CHECK(session->waitForRenderThread(StopBound));
      std::cout << "LP2.2a stop mid-frame: "
                << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - stopStart)
                     .count()
                << " ms\n";
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    CHECK(*stoppedEarly);
    messages.print("lit passes stop, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  std::filesystem::remove_all(root);
}

namespace
{

// LP2.2b: what a frame of the tonemap passes left, gathered after them on the render
// thread
struct ChainFrame
{
  TonemapFrameStats tonemap;
  LitFrameStats lit;
  FrameConvergence convergence;
  bool settled = false;
  std::optional<std::array<float, 4>> hdrClear;
  nvrhi::Format targetFormat = nvrhi::Format::UNKNOWN;
};

ChainFrame chainFrameOf(const RenderFrameAccess& access)
{
  auto frame = ChainFrame{};
  frame.tonemap = access.tonemap.lastStats();
  frame.lit = access.passes.lastStats();
  frame.convergence = access.convergence;
  frame.settled = access.convergence.settled();
  frame.hdrClear = access.hdrClear;
  if (const auto target = access.frame.find(frame_resource::FrameTarget))
  {
    frame.targetFormat = target->texture->getDesc().format;
  }
  return frame;
}

// the readbacks of one frame: its exposure buffer, and the centre pixels of its LDR
// target and of its frame target
struct ChainReadback
{
  ChainFrame frame;
  float exposure = 0.0f;
  std::array<std::uint8_t, 4> ldr = {};
  std::array<std::uint8_t, 4> target = {};
};

/**
 * LP2.2b: sets the HDR clear colour and reads back the first frame whose tonemap passes
 * ran on it. An every-frame hook sees each frame's colour at the frame's start and
 * queues, in that same frame, a hook after its tonemap passes that copies the exposure
 * buffer, the LDR target and the frame's target into staging resources; the next frame
 * reads them. nullopt at the deadline.
 */
std::optional<ChainReadback> firstChainFrame(
  LitSession& session,
  const std::array<float, 4>& colour,
  const std::chrono::milliseconds timeout = 60000ms)
{
  struct State
  {
    std::mutex mutex;
    bool queued = false;
    bool captured = false;
    ChainFrame frame;
    nvrhi::BufferHandle exposure;
    nvrhi::StagingTextureHandle ldr;
    nvrhi::StagingTextureHandle target;
  };
  auto state = std::make_shared<State>();
  auto* const owner = &session;
  const auto capture = [state](RenderFrameAccess& access) {
    auto frame = chainFrameOf(access);
    const auto lock = std::scoped_lock{state->mutex};
    if (!frame.tonemap.tonemapped || !frame.tonemap.blitted)
    {
      // a pipeline of the passes is pending: the next frame of the colour tries again
      state->queued = false;
      return;
    }
    namespace fr = frame_resource;
    auto bufferDesc = nvrhi::BufferDesc{};
    bufferDesc.byteSize = 4;
    bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    bufferDesc.debugName = "LP2.2b exposure readback";
    bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
    bufferDesc.keepInitialState = true;
    state->exposure = access.device.createBuffer(bufferDesc);
    access.commandList.copyBuffer(
      state->exposure, 0, access.frame.find(fr::ExposureBuffer)->buffer, 0, 4);
    const auto stage = [&](nvrhi::ITexture* texture) {
      const auto& desc = texture->getDesc();
      auto staging = access.device.createStagingTexture(
        nvrhi::TextureDesc{}
          .setWidth(desc.width)
          .setHeight(desc.height)
          .setFormat(desc.format),
        nvrhi::CpuAccessMode::Read);
      access.commandList.copyTexture(
        staging, nvrhi::TextureSlice{}, texture, nvrhi::TextureSlice{});
      return staging;
    };
    state->ldr = stage(access.frame.find(fr::CurrentRenderLdr)->texture);
    state->target = stage(access.frame.find(fr::FrameTarget)->texture);
    state->frame = std::move(frame);
    state->captured = true;
  };
  session.setEveryFrameHookForTesting(
    [state, owner, colour, capture](RenderFrameAccess& access) {
      if (access.hdrClear != colour)
      {
        return;
      }
      {
        const auto lock = std::scoped_lock{state->mutex};
        if (state->queued || state->captured)
        {
          return;
        }
        state->queued = true;
      }
      // it runs after this frame's tonemap passes
      owner->runAfterTonemapForTesting(capture);
    });
  session.setHdrClearForTesting(colour);
  const auto captured = waitFor(
    [&] {
      const auto lock = std::scoped_lock{state->mutex};
      if (!state->captured && !state->queued)
      {
        // frames are drawn on triggers only (D77)
        owner->requestFrame();
      }
      return state->captured;
    },
    timeout,
    false);
  session.setEveryFrameHookForTesting({});
  if (!captured)
  {
    return std::nullopt;
  }

  auto readback = ChainReadback{};
  {
    const auto lock = std::scoped_lock{state->mutex};
    readback.frame = state->frame;
  }
  const auto read = inFrame(session, [&](RenderFrameAccess& access) {
    auto& device = access.device;
    if (
      const auto* value = static_cast<const float*>(
        device.mapBuffer(state->exposure, nvrhi::CpuAccessMode::Read)))
    {
      readback.exposure = *value;
      device.unmapBuffer(state->exposure);
    }
    const auto centre = [&](nvrhi::IStagingTexture* staging) {
      auto pixel = std::array<std::uint8_t, 4>{};
      const auto& desc = staging->getDesc();
      auto pitch = std::size_t{0};
      if (
        const auto* bytes = static_cast<const std::uint8_t*>(device.mapStagingTexture(
          staging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch)))
      {
        std::copy_n(
          bytes + std::size_t(desc.height / 2) * pitch + std::size_t(desc.width / 2) * 4,
          4,
          pixel.begin());
        device.unmapStagingTexture(staging);
      }
      return pixel;
    };
    readback.ldr = centre(state->ldr);
    readback.target = centre(state->target);
    state->exposure = nullptr;
    state->ldr = nullptr;
    state->target = nullptr;
  });
  if (!read)
  {
    return std::nullopt;
  }
  return readback;
}

/**
 * LP2.2b: asks for frames of camera until one is settled (D77) with its lit and tonemap
 * passes drawn and blitted; that frame's record, or nullopt at the deadline.
 */
std::optional<ChainFrame> settledChain(
  LitSession& session,
  const CameraState& camera,
  const std::chrono::milliseconds timeout = 60000ms)
{
  session.setCamera(camera);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.tableChanged();
    auto frame = std::make_shared<ChainFrame>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterTonemapForTesting([frame, done](RenderFrameAccess& access) {
      *frame = chainFrameOf(access);
      done->set_value();
    });
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (
      frame->settled && frame->lit.camera == camera && frame->tonemap.tonemapped
      && frame->tonemap.blitted)
    {
      return *frame;
    }
    if (std::chrono::steady_clock::now() + 50ms >= deadline)
    {
      const auto& c = frame->convergence;
      std::cout << fmt::format(
        "not settled: pipelines {} materials {} skipped {} uploads {} images {} "
        "provisional {} lut {} disabled {}; camera {}, tonemapped {}, blitted {}\n",
        c.pipelinesPending,
        c.materialsPending,
        c.drawsSkipped,
        c.uploadsLeft,
        c.imagesPending,
        c.provisionalChunks,
        c.lutPending,
        c.passesDisabled,
        frame->lit.camera == camera,
        frame->tonemap.tonemapped,
        frame->tonemap.blitted);
    }
    std::this_thread::sleep_for(50ms);
  }
  return std::nullopt;
}

// a headless 160 × 120 session with validation over the root, logging its frames
std::unique_ptr<LitSession> loggedRootSession(
  const TestInstance& instance,
  Events& events,
  MaterialService& service,
  const std::filesystem::path& root,
  const std::filesystem::path& frameLogPath)
{
  auto options = SessionOptions{};
  options.validation = true;
  options.gamePath = root;
  options.frameLogPath = frameLogPath;
  return LitSession::create(
           instance.handles(),
           VK_NULL_HANDLE,
           160,
           120,
           options,
           events.sink(),
           7,
           &service)
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

double fieldValue(const std::vector<std::string>& row, const std::size_t column)
{
  return row.size() > column && !row[column].empty() ? std::stod(row[column]) : 0.0;
}

} // namespace

// LP2.2b: the tonemap passes and the blit put the lit picture on the frame's target.
// Hidden as LP2.2a's lit case: the blobs come from the game folder (TB_LIT_GAME_PATH),
// copied into a test game root; it needs a GPU.
TEST_CASE("LitSession tonemap and blit on a GPU (LP2.2b)", "[.][litgpu]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);

  SECTION(
    "three HDR values tonemap as the shaders compute them, the exposure is its clamped "
    "target from the first frame, and the blit copies the LDR target")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));

      // a bright target (bin 0, whose luminance exp2( -10 ) is clamped up to
      // minAdaptedLuminance; red is the channel ACESFilm does not saturate), one inside
      // the bounds (bins 112 and 113), a dark one (bin 255, clamped down to
      // maxAdaptedLuminance); each value is a half, so the RGBA16F target holds it as is
      const auto colours = std::array{
        std::array<float, 4>{0.03125f, 1.5f, 0.0f, 1.0f},
        std::array<float, 4>{0.875f, 0.875f, 0.875f, 1.0f},
        std::array<float, 4>{0.25f, 0.25f, 0.25f, 1.0f}};
      // D70: the exposure buffer is cleared at Start
      auto oldExposure = 0.0f;
      for (const auto& colour : colours)
      {
        CAPTURE(colour);
        const auto readback = firstChainFrame(*session, colour);
        REQUIRE(readback);
        const auto& frame = readback->frame;
        CHECK(frame.targetFormat == nvrhi::Format::RGBA8_UNORM);

        const auto hdr = std::array<float, 3>{
          modelHalf(colour[0]), modelHalf(colour[1]), modelHalf(colour[2])};
        CHECK(hdr == std::array<float, 3>{colour[0], colour[1], colour[2]});
        const auto histogram = modelHistogram(hdr, frame.tonemap.histogramConstants);
        const auto exposure =
          modelExposure(histogram, oldExposure, frame.tonemap.exposureConstants);
        // the first frame of the colour is at its clamped target, whatever the last
        // frame's exposure was: no adaptation tail (D70); the engine's own frameTime of
        // 0.016 s would have left one
        CHECK(
          exposure
          == modelExposure(histogram, exposure, frame.tonemap.exposureConstants));
        CHECK(std::abs(readback->exposure - exposure) <= 1e-5f * exposure);
        auto engineFirstFrame = frame.tonemap.exposureConstants;
        engineFirstFrame.frameTime = 0.016f;
        const auto withTail = modelExposure(histogram, oldExposure, engineFirstFrame);

        const auto ldr = modelTonemap(hdr, exposure, frame.tonemap.tonemapConstants);
        const auto expected =
          std::array{modelUnorm8(ldr[0]), modelUnorm8(ldr[1]), modelUnorm8(ldr[2])};
        for (std::size_t i = 0; i < 3; ++i)
        {
          CAPTURE(i);
          // U3.10: the shader maths exactly
          CHECK(int(readback->ldr[i]) == int(expected[i]));
        }
        // the HDR target's alpha passes through (SC263)
        CHECK(readback->ldr[3] == 255);
        // the blit: the headless target's centre pixel is the LDR target's
        CHECK(readback->target == readback->ldr);

        std::cout << fmt::format(
          "LP2.2b tonemap: HDR ({}, {}, {}) -> CPU ({}, {}, {}) exposure {:.6f} -> GPU "
          "({}, {}, {}, {}) exposure {:.6f}; target ({}, {}, {}, {}); the engine's first "
          "frame would adapt from {} to {:.6f}\n",
          hdr[0],
          hdr[1],
          hdr[2],
          expected[0],
          expected[1],
          expected[2],
          exposure,
          readback->ldr[0],
          readback->ldr[1],
          readback->ldr[2],
          readback->ldr[3],
          readback->exposure,
          readback->target[0],
          readback->target[1],
          readback->target[2],
          readback->target[3],
          oldExposure,
          withTail);
        oldExposure = readback->exposure;
      }
      session->setHdrClearForTesting(std::nullopt);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(events.count(SessionEvent::Warning) == 0u);
      events.print("tonemap values");
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("tonemap values, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    // the session draws no chunk: only DX.5's pipelines with an input layout were made,
    // and they imply the mirrored warnings (M-PIPE-DEFECT-01)
    auto implied = 0;
    for (const auto& key : litStartPipelineKeys(nvrhi::Format::D24S8))
    {
      implied += unreadLocations(key.program);
    }
    CHECK(
      messages.mirrored
      == (limitRaised ? implied : std::min(implied, ValidationDuplicateMessageLimit)));
  }

  SECTION(
    "idle is zero frames, a camera change one frame; the frame log's columns; the "
    "LDR readback")
  {
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-lp22b-frame-log.csv";
    std::filesystem::remove(logPath);
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    auto programsAsked = std::vector<int>{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = loggedRootSession(instance, events, *service, root, logPath);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setDrawLogForTesting(true);
      }));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));

      // camera A over the lit floor, camera B at the west wall outside every light: each
      // settled once, so that a change between them needs nothing new
      const auto litView = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
      const auto darkView = litCamera({-100, 0, 64}, {-1, 0, 0}, {0, 0, 1});
      const auto lit = settledChain(*session, litView);
      REQUIRE(lit);
      CHECK(lit->lit.prepassDraws > 0u);
      CHECK(lit->lit.interactionDraws > 0u);
      CHECK(lit->tonemap.draws == 2u);
      CHECK(lit->tonemap.dispatches == 2u);
      REQUIRE(settledChain(*session, darkView));
      REQUIRE(settledChain(*session, litView));

      // idle = zero frames (D77): a settled session draws nothing without a trigger
      std::this_thread::sleep_for(300ms);
      CHECK(session->lastFrameSettledForTesting());
      const auto idleFrames = session->framesPresented();
      std::this_thread::sleep_for(3000ms);
      CHECK(session->framesPresented() == idleFrames);
      // a camera change is exactly one frame, settled at once
      session->setCamera(darkView);
      std::this_thread::sleep_for(1000ms);
      CHECK(session->framesPresented() == idleFrames + 1);
      CHECK(session->lastFrameSettledForTesting());
      std::cout << "LP2.2b idle: " << idleFrames << " frames before 3 s of idling, "
                << session->framesPresented() << " after it and one camera change\n";

      // C29: the settled frame's LDR target; the dark view is black, the lit one lit on
      // the floor at the centre
      const auto dark = session->readbackLdrForTesting();
      REQUIRE(dark);
      CHECK(dark->width == 160u);
      CHECK(dark->height == 120u);
      REQUIRE(dark->rgba.size() == 160u * 120u * 4u);
      const auto darkCentre = std::size_t(60 * 160 + 80) * 4;
      CHECK(dark->rgba[darkCentre] == 0u);
      CHECK(dark->rgba[darkCentre + 3] == 255u);
      session->setCamera(litView);
      const auto litImage = session->readbackLdrForTesting();
      REQUIRE(litImage);
      const auto litCentre = std::size_t(60 * 160 + 80) * 4;
      CHECK(
        int(litImage->rgba[litCentre]) + litImage->rgba[litCentre + 1]
          + litImage->rgba[litCentre + 2]
        > 0);
      std::cout << fmt::format(
        "LP2.2b LDR readback: the lit floor's centre ({}, {}, {}, {}), the dark wall's "
        "({}, {}, {}, {})\n",
        litImage->rgba[litCentre],
        litImage->rgba[litCentre + 1],
        litImage->rgba[litCentre + 2],
        litImage->rgba[litCentre + 3],
        dark->rgba[darkCentre],
        dark->rgba[darkCentre + 1],
        dark->rgba[darkCentre + 2],
        dark->rgba[darkCentre + 3]);

      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto& key : litKeysMade(
               access.passes.requestedKeysForTesting(), access.frame.depthFormat()))
        {
          programsAsked.push_back(key.program);
        }
        access.passes.setDrawLogForTesting(false);
      }));

      // U2.6: the account holds the volatile constant buffers and the upload pool; a
      // settled frame past the budget says so once, a later settled frame not again
      auto account = GpuMemoryAccount{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        account = accountGpuMemory(
          access.frame, access.arenas, access.images, access.passes, access.tonemap);
      }));
      CHECK(account.volatileBufferBytes > 0u);
      CHECK(account.uploadPoolBytes >= UploadChunkBytes);
      const auto budgetText = std::string_view{"passes the device's budget"};
      CHECK(events.countContaining(SessionEvent::Warning, budgetText) == 0u);
      session->setVramBudgetForTesting(std::uint64_t{1024} * 1024);
      session->setCamera(darkView);
      CHECK(waitFor(
        [&] { return events.countContaining(SessionEvent::Warning, budgetText) == 1u; },
        5000ms,
        false));
      session->setCamera(litView);
      std::this_thread::sleep_for(1000ms);
      CHECK(session->lastFrameSettledForTesting());
      CHECK(events.countContaining(SessionEvent::Warning, budgetText) == 1u);
      session->setVramBudgetForTesting(0);
      std::cout << fmt::format(
        "U2.6: the account {:.1f} MB device-local and {:.1f} MB host-visible: arenas "
        "{:.1f}, textures {:.1f}, targets {:.1f}, released {:.1f}, volatile buffers "
        "{:.1f}, "
        "upload pool {:.1f}\n",
        double(account.total()) / (1024.0 * 1024.0),
        double(account.hostVisible()) / (1024.0 * 1024.0),
        double(account.arenaBytes) / (1024.0 * 1024.0),
        double(account.textureBytes) / (1024.0 * 1024.0),
        double(account.renderTargetBytes) / (1024.0 * 1024.0),
        double(account.releasedBytes) / (1024.0 * 1024.0),
        double(account.volatileBufferBytes) / (1024.0 * 1024.0),
        double(account.uploadPoolBytes) / (1024.0 * 1024.0));

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(!session->teardownReport().frameLogError);
      events.print("idle");
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("idle, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    auto implied = 0;
    for (const auto program : programsAsked)
    {
      implied += unreadLocations(program);
    }
    CHECK(
      messages.mirrored
      == (limitRaised ? implied : std::min(implied, ValidationDuplicateMessageLimit)));

    // C16: the columns, plausible: every pass that drew has a GPU time, a settled frame
    // that drew the scene says so
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 3u);
    REQUIRE(rows[0].size() == 42u);
    CHECK(rows[0][18] == "gpu_prepass_ms");
    CHECK(rows[0][34] == "settled");
    CHECK(rows[0][35] == "alloc_refused");
    CHECK(rows[0][36] == "culled_triangles");
    // LP4.1 (C16): the view's portal flow's five columns
    CHECK(rows[0][37] == "view_area");
    CHECK(rows[0][41] == "flow_ms");
    auto lastLit = std::optional<std::vector<std::string>>{};
    auto settledRows = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 42u);
      if (rows[i][1] != "frame")
      {
        continue;
      }
      CHECK((rows[i][34] == "0" || rows[i][34] == "1"));
      if (rows[i][34] == "1")
      {
        ++settledRows;
      }
      // a pass that drew has its time (the times arrive for every shown frame here)
      if (fieldValue(rows[i], 23) > 0.0)
      {
        CHECK(fieldValue(rows[i], 18) > 0.0);
      }
      if (fieldValue(rows[i], 24) > 0.0)
      {
        CHECK(fieldValue(rows[i], 19) > 0.0);
      }
      if (rows[i][34] == "1" && fieldValue(rows[i], 23) > 0.0)
      {
        CHECK(fieldValue(rows[i], 22) > 0.0);
        CHECK(fieldValue(rows[i], 20) > 0.0);
        CHECK(fieldValue(rows[i], 21) > 0.0);
        CHECK(fieldValue(rows[i], 6) > 0.0);
        lastLit = rows[i];
      }
    }
    CHECK(settledRows > 0);
    REQUIRE(lastLit);
    std::cout << fmt::format(
      "LP2.2b fixture frame GPU ms: frame {}, prepass {}, interactions {}, tonemap {}, "
      "blit {}; draws {} (prepass {}, interactions {}), lights {}, constant bytes {}, "
      "volatile writes {}, push-constant bytes {}, CPU record {} ms\n",
      (*lastLit)[6],
      (*lastLit)[18],
      (*lastLit)[19],
      (*lastLit)[20],
      (*lastLit)[21],
      (*lastLit)[22],
      (*lastLit)[23],
      (*lastLit)[24],
      (*lastLit)[27],
      (*lastLit)[30],
      (*lastLit)[31],
      (*lastLit)[33],
      (*lastLit)[3]);
    std::filesystem::remove(logPath);
  }

  SECTION("a window: the lit picture is blitted into the swapchain, validation 0")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    auto programsAsked = std::vector<int>{};
    {
      auto window = TestWindow{320, 240};
      REQUIRE(window.handle() != nullptr);
      const auto instance = TestInstance{library, true, true, messages};
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto createSurface = library.get<PFN_vkCreateWin32SurfaceKHR>(
        instance.instance(), "vkCreateWin32SurfaceKHR");
      REQUIRE(createSurface != nullptr);
      auto surfaceInfo = VkWin32SurfaceCreateInfoKHR{};
      surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
      surfaceInfo.hinstance = GetModuleHandleW(nullptr);
      surfaceInfo.hwnd = window.handle();
      auto surface = VkSurfaceKHR{VK_NULL_HANDLE};
      REQUIRE(
        createSurface(instance.instance(), &surfaceInfo, nullptr, &surface)
        == VK_SUCCESS);
      {
        auto service = rootService(root);
        auto events = Events{};
        auto options = SessionOptions{};
        options.validation = true;
        options.gamePath = root;
        const auto [width, height] = window.clientSize();
        auto session = LitSession::create(
                         instance.handles(),
                         surface,
                         width,
                         height,
                         options,
                         events.sink(),
                         7,
                         service.get())
                       | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
        REQUIRE(session->hasSwapchain());
        session->setExposed(true);
        REQUIRE(waitFor(
          [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
          5000ms,
          true));
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          access.passes.setDrawLogForTesting(true);
        }));
        REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));

        // the lit view until a settled frame is blitted into a swapchain image
        const auto litView = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
        session->setCamera(litView);
        auto blitted = std::optional<ChainFrame>{};
        const auto deadline = std::chrono::steady_clock::now() + 60000ms;
        while (!blitted && std::chrono::steady_clock::now() < deadline)
        {
          if (events.count(SessionEvent::SwapchainOutOfDate) > 0)
          {
            {
              const auto lock = std::scoped_lock{events.mutex};
              std::erase_if(events.events, [](const auto& entry) {
                return std::get<0>(entry) == SessionEvent::SwapchainOutOfDate;
              });
            }
            replaceSwapchain(*session, surface, window)
              | kdl::transform_error([](const auto& e) { FAIL(e.msg); });
          }
          session->tableChanged();
          auto frame = std::make_shared<ChainFrame>();
          auto done = std::make_shared<std::promise<void>>();
          auto future = done->get_future();
          session->runAfterTonemapForTesting([frame, done](RenderFrameAccess& access) {
            *frame = chainFrameOf(access);
            done->set_value();
          });
          if (!waitFor(
                [&] { return future.wait_for(0ms) == std::future_status::ready; },
                10000ms,
                true))
          {
            break;
          }
          if (frame->settled && frame->tonemap.blitted)
          {
            blitted = *frame;
          }
          waitFor([&] { return false; }, 20ms, true);
        }
        REQUIRE(blitted);
        CHECK(blitted->targetFormat == nvrhi::Format::BGRA8_UNORM);
        CHECK(blitted->tonemap.tonemapped);
        CHECK(blitted->lit.interactionDraws > 0u);
        const auto frames = session->framesPresented();
        session->setCamera(litCamera({60, 0, 81}, {0, 0, -1}, {1, 0, 0}));
        REQUIRE(
          waitFor([&] { return session->framesPresented() > frames; }, 5000ms, true));
        std::cout << "LP2.2b window: " << session->framesPresented()
                  << " frames presented, the last settled one blitted into a " << width
                  << " x " << height << " BGRA8 swapchain image\n";

        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          for (const auto& key : litKeysMade(
                 access.passes.requestedKeysForTesting(), access.frame.depthFormat()))
          {
            programsAsked.push_back(key.program);
          }
          access.passes.setDrawLogForTesting(false);
        }));
        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->nvrhiErrorCount() == 0u);
        CHECK(session->deviceMemoryAllocations() == 0);
        CHECK(events.count(SessionEvent::Error) == 0u);
        CHECK(events.count(SessionEvent::DeviceLost) == 0u);
        events.print("window blit");
        session.reset();
        service->requestStop();
        service->join();
      }
      library.get<PFN_vkDestroySurfaceKHR>(instance.instance(), "vkDestroySurfaceKHR")(
        instance.instance(), surface, nullptr);
    }
    CHECK(resetDispatcher());
    messages.print("window blit, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
    auto implied = 0;
    for (const auto program : programsAsked)
    {
      implied += unreadLocations(program);
    }
    CHECK(
      messages.mirrored
      == (limitRaised ? implied : std::min(implied, ValidationDuplicateMessageLimit)));
  }

  SECTION("U2.12: a blob-backed session under a thread and handle baseline")
  {
    // the compile thread's real pipelines, the fixture's textures, the lit and tonemap
    // passes and the LDR readback, Start to Stop; the first cycle pays the process's
    // one-time costs (the driver's and the loader's), the baseline is taken after it
    const auto cycle = [&] {
      auto messages = ValidationMessages{};
      messages.mirroredMessageId = OutputNotConsumedId;
      {
        const auto instance = TestInstance{library, true, false, messages};
        REQUIRE(instance.valid());
        REQUIRE(initInstanceDispatcher(instance.handles()));
        auto service = rootService(root);
        auto events = Events{};
        auto session = rootSession(instance, events, *service, root);
        session->setExposed(true);
        REQUIRE(waitFor(
          [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
          5000ms,
          false));
        REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));
        const auto lit =
          settledChain(*session, litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0}));
        REQUIRE(lit);
        CHECK(lit->lit.interactionDraws > 0u);
        CHECK(session->readbackLdrForTesting());
        teardown(*session);
        CHECK(session->lastDeviceReleaseCount() == 0u);
        CHECK(session->nvrhiErrorCount() == 0u);
        CHECK(session->deviceMemoryAllocations() == 0);
        CHECK(events.count(SessionEvent::Error) == 0u);
        session.reset();
        service->requestStop();
        service->join();
      }
      CHECK(resetDispatcher());
      CHECK(messages.errors == 0);
      CHECK(messages.warnings == 0);
    };
    cycle();
    const auto baseline = processCounts();
    constexpr auto cycles = 2L;
    for (auto i = 0L; i < cycles; ++i)
    {
      cycle();
    }
    const auto maxHandles =
      baseline.handles + HandleTolerance + cycles * DriverHandlesPerSession;
    auto counts = processCounts();
    const auto settled = waitFor(
      [&] {
        counts = processCounts();
        return counts.threads <= baseline.threads && counts.handles <= maxHandles;
      },
      3000ms,
      false);
    std::cout << "U2.12: " << cycles << " blob-backed sessions: threads "
              << baseline.threads << " -> " << counts.threads << ", handles "
              << baseline.handles << " -> " << counts.handles << " (allowed "
              << maxHandles << ")" << (settled ? "" : " (not settled)") << "\n";
    CHECK(counts.threads <= baseline.threads);
    CHECK(counts.handles <= maxHandles);
  }

  SECTION(
    "V2.2: the static interaction cull end to end and the fixture measured with and "
    "without it")
  {
    // a light whose +x plane is x 64 and +y plane y 64.125: W1's -x face lies on the +x
    // plane (culled: 0), W2's -y face 1/8 inside the +y plane (kept: lit); both walls are
    // one chunk (worldspawn, lp1b/wall), drawn as index runs
    auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
    text += boxBrush(64, -32, 32, 80, 32, 96, "lp1b/wall");
    text += boxBrush(-32, 64, 32, 32, 80, 96, "lp1b/wall");
    text +=
      "}\n{\n\"classname\" \"light\"\n\"name\" \"v22_light\"\n\"origin\" \"0 0 64\"\n"
      "\"light_radius\" \"64 64.125 64\"\n\"texture\" \"lights/lp22a/flatclamp\"\n"
      "\"_color\" \"1 1 1\"\n\"noshadows\" \"1\"\n}\n";
    auto snapshot = std::make_shared<Snapshot>(snapshotOfMapText(text));
    snapshot->generation = 7;
    snapshot->sequence = 1;

    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-v22-frame-log.csv";
    std::filesystem::remove(logPath);
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    struct Phase
    {
      std::uint64_t firstFrame = 0;
      std::uint64_t lastFrame = 0;
      LitFrameStats stats;
    };
    auto measured = std::array<Phase, 2>{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = loggedRootSession(instance, events, *service, root, logPath);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(session->offerSnapshot(snapshot));

      const auto atW1 = litCamera({0, 0, 64}, {1, 0, 0}, {0, 0, 1});
      const auto atW2 = litCamera({0, 0, 64}, {0, 1, 0}, {0, 0, 1});
      const auto w1 = settledHdr(*session, atW1, 1);
      REQUIRE(w1);
      CHECK(w1->first.brightest(80, 60) == 0.0f);
      CHECK(w1->second.stats.lightTriangleRuns > 0u);
      CHECK(w1->second.stats.culledTriangles > 0u);
      const auto w2 = settledHdr(*session, atW2, 1);
      REQUIRE(w2);
      CHECK(w2->first.brightest(80, 60) > 0.0f);
      CHECK(w2->second.stats.lightTriangleRuns > 0u);

      // without the cull W1's face is lit: the cull is what darkened it
      REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
        access.passes.setLightTriangleCullForTesting(false);
      }));
      const auto w1Whole = settledHdr(*session, atW1, 1);
      REQUIRE(w1Whole);
      CHECK(w1Whole->first.brightest(80, 60) > 0.0f);
      CHECK(w1Whole->second.stats.culledTriangles == 0u);
      CHECK(w1Whole->second.stats.lightTriangleRuns == 0u);
      std::cout << fmt::format(
        "V2.2: W1 on the plane {} (whole {}), W2 1/8 inside {}; runs {}, culled "
        "triangles "
        "{}\n",
        w1->first.brightest(80, 60),
        w1Whole->first.brightest(80, 60),
        w2->first.brightest(80, 60),
        w1->second.stats.lightTriangleRuns,
        w1->second.stats.culledTriangles);

      // the fixture measured: the LP2.2a map from camera 1, 30 frames with the cull and
      // 30 without (the frame log's GPU times, interaction draws, culled triangles)
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 2)));
      const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
      for (const auto cull : {true, false})
      {
        auto& phase = measured[cull ? 0 : 1];
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          access.passes.setLightTriangleCullForTesting(cull);
        }));
        const auto settledFrame = settledHdr(*session, lit, 1);
        REQUIRE(settledFrame);
        phase.stats = settledFrame->second.stats;
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          phase.firstFrame = access.frameIndex + 1;
        }));
        for (auto i = 0; i < 30; ++i)
        {
          const auto before = session->framesPresented();
          session->requestFrame();
          REQUIRE(
            waitFor([&] { return session->framesPresented() > before; }, 5000ms, false));
        }
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          phase.lastFrame = access.frameIndex;
        }));
      }
      REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
        access.passes.setLightTriangleCullForTesting(true);
      }));

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);

    // the medians of each phase's rows
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() > 2u);
    CHECK(rows[0][36] == "culled_triangles");
    // LP4.1 (C16): the view's portal flow's five columns
    CHECK(rows[0][37] == "view_area");
    CHECK(rows[0][41] == "flow_ms");
    for (std::size_t p = 0; p < measured.size(); ++p)
    {
      auto frameMs = std::vector<double>{};
      auto interactionsMs = std::vector<double>{};
      auto culled = std::set<std::string>{};
      auto draws = std::set<std::string>{};
      for (std::size_t i = 1; i < rows.size(); ++i)
      {
        const auto& row = rows[i];
        if (row.size() != 42u || row[1] != "frame")
        {
          continue;
        }
        const auto frame = std::stoull(row[2]);
        if (frame < measured[p].firstFrame || frame > measured[p].lastFrame)
        {
          continue;
        }
        if (!row[6].empty())
        {
          frameMs.push_back(fieldValue(row, 6));
        }
        if (!row[19].empty())
        {
          interactionsMs.push_back(fieldValue(row, 19));
        }
        culled.insert(row[36]);
        draws.insert(row[24]);
      }
      const auto median = [](std::vector<double> values) {
        if (values.empty())
        {
          return 0.0;
        }
        std::ranges::sort(values);
        return values[values.size() / 2];
      };
      CHECK(frameMs.size() >= 10u);
      std::cout << fmt::format(
        "V2.2 fixture measurement (LP2.2a map, camera 1), cull {}: interaction draws {} "
        "(log {}), culled triangles {} (log {}), GPU frame median {:.4f} ms, "
        "interactions median {:.4f} ms over {} frames\n",
        p == 0 ? "on" : "off",
        measured[p].stats.interactionDraws,
        fmt::join(draws, "/"),
        measured[p].stats.culledTriangles,
        fmt::join(culled, "/"),
        median(frameMs),
        median(interactionsMs),
        frameMs.size());
    }
    CHECK(measured[1].stats.culledTriangles == 0u);
    std::filesystem::remove(logPath);
  }

  SECTION("the livelock guard: a frame that cannot progress stops re-triggering itself")
  {
    // D77: uploads left re-trigger the next frame at once; with no upload allowed they
    // never progress, so after MaxStalledFrames frames without progress the thread waits
    // for another trigger
    auto messages = ValidationMessages{};
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
      session->setExposed(true);
      REQUIRE(waitFor([&] { return session->framesPresented() >= 1u; }, 5000ms, false));
      std::this_thread::sleep_for(200ms);
      CHECK(session->lastFrameSettledForTesting());

      session->setUploadCapForTesting(0);
      const auto before = session->framesPresented();
      session->setChunksForTesting(syntheticChunks(10, 300, 1));
      std::this_thread::sleep_for(1000ms);
      const auto stalled = session->framesPresented();
      // the trigger's frame, then at most MaxStalledFrames frames that re-trigger
      CHECK(stalled > before);
      CHECK(stalled - before <= MaxStalledFrames + 1);
      CHECK_FALSE(session->lastFrameSettledForTesting());
      std::this_thread::sleep_for(500ms);
      CHECK(session->framesPresented() == stalled);
      std::cout << "LP2.2b livelock guard: " << stalled - before
                << " frames re-triggered without progress, then none\n";

      // a trigger lets the uploads through again, and the frames settle
      session->setUploadCapForTesting(UploadBytesPerFrame);
      session->requestFrame();
      CHECK(
        waitFor([&] { return session->lastFrameSettledForTesting(); }, 5000ms, false));

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(events.count(SessionEvent::Warning) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("livelock guard, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("a Stop between the lit passes and the tonemap passes")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto stoppedEarly = std::make_shared<std::atomic<bool>>(false);
    auto blittedAfterStop = std::make_shared<std::atomic<bool>>(true);
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));
      REQUIRE(settledChain(*session, litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0})));

      // the next frame is held after its lit passes until the stop is asked for: the
      // tonemap passes stop at their first boundary, the frame is submitted and the
      // thread exits
      auto held = std::make_shared<std::promise<void>>();
      auto heldFuture = held->get_future();
      auto release = std::make_shared<std::promise<void>>();
      auto releaseFuture = release->get_future().share();
      auto once = std::make_shared<std::atomic<bool>>(false);
      auto* const owner = session.get();
      session->setEveryFrameHookForTesting(
        [held, releaseFuture, once, owner, stoppedEarly, blittedAfterStop](
          RenderFrameAccess&) {
          if (once->exchange(true))
          {
            return;
          }
          owner->runAfterPassesForTesting([held, releaseFuture](RenderFrameAccess&) {
            held->set_value();
            releaseFuture.wait();
          });
          owner->runAfterTonemapForTesting(
            [stoppedEarly, blittedAfterStop](RenderFrameAccess& access) {
              *stoppedEarly = access.tonemap.lastStats().stoppedEarly;
              *blittedAfterStop = access.tonemap.lastStats().blitted;
            });
        });
      session->requestFrame();
      REQUIRE(heldFuture.wait_for(10000ms) == std::future_status::ready);
      const auto stopStart = std::chrono::steady_clock::now();
      session->requestStop();
      release->set_value();
      CHECK(session->waitForRenderThread(StopBound));
      std::cout << "LP2.2b stop between the passes: "
                << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - stopStart)
                     .count()
                << " ms\n";
      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    CHECK(*stoppedEarly);
    CHECK_FALSE(*blittedAfterStop);
    messages.print("tonemap stop, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  std::filesystem::remove_all(root);
}

namespace
{

/**
 * LP2.3: a JSON value of parity.py's rows.json (objects, arrays, strings whose only
 * escapes are \" and \\, numbers, true, false, null); enough for that file, no more.
 */
struct JsonValue
{
  enum class Kind
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
  };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  const JsonValue* find(const std::string_view key) const
  {
    for (const auto& [name, value] : object)
    {
      if (name == key)
      {
        return &value;
      }
    }
    return nullptr;
  }

  std::string text(const std::string_view key) const
  {
    const auto* value = find(key);
    return value && value->kind == Kind::String ? value->string : std::string{};
  }

  double numberOr(const std::string_view key, const double fallback) const
  {
    const auto* value = find(key);
    return value && value->kind == Kind::Number ? value->number : fallback;
  }

  bool flag(const std::string_view key) const
  {
    const auto* value = find(key);
    return value && value->kind == Kind::Bool && value->boolean;
  }
};

class JsonReader
{
private:
  std::string_view m_text;
  std::size_t m_at = 0;

  void skipSpace()
  {
    while (
      m_at < m_text.size()
      && (m_text[m_at] == ' ' || m_text[m_at] == '\n' || m_text[m_at] == '\r' || m_text[m_at] == '\t'))
    {
      ++m_at;
    }
  }

  bool take(const char c)
  {
    skipSpace();
    if (m_at < m_text.size() && m_text[m_at] == c)
    {
      ++m_at;
      return true;
    }
    return false;
  }

  std::optional<std::string> string()
  {
    if (!take('"'))
    {
      return std::nullopt;
    }
    auto out = std::string{};
    while (m_at < m_text.size() && m_text[m_at] != '"')
    {
      if (m_text[m_at] == '\\' && m_at + 1 < m_text.size())
      {
        ++m_at;
      }
      out += m_text[m_at++];
    }
    if (m_at >= m_text.size())
    {
      return std::nullopt;
    }
    ++m_at;
    return out;
  }

public:
  explicit JsonReader(const std::string_view text)
    : m_text{text}
  {
  }

  std::optional<JsonValue> value()
  {
    skipSpace();
    if (m_at >= m_text.size())
    {
      return std::nullopt;
    }
    auto result = JsonValue{};
    const auto c = m_text[m_at];
    if (c == '{')
    {
      ++m_at;
      result.kind = JsonValue::Kind::Object;
      if (take('}'))
      {
        return result;
      }
      do
      {
        auto key = string();
        if (!key || !take(':'))
        {
          return std::nullopt;
        }
        auto member = value();
        if (!member)
        {
          return std::nullopt;
        }
        result.object.emplace_back(std::move(*key), std::move(*member));
      } while (take(','));
      return take('}') ? std::optional{std::move(result)} : std::nullopt;
    }
    if (c == '[')
    {
      ++m_at;
      result.kind = JsonValue::Kind::Array;
      if (take(']'))
      {
        return result;
      }
      do
      {
        auto element = value();
        if (!element)
        {
          return std::nullopt;
        }
        result.array.push_back(std::move(*element));
      } while (take(','));
      return take(']') ? std::optional{std::move(result)} : std::nullopt;
    }
    if (c == '"')
    {
      auto text = string();
      if (!text)
      {
        return std::nullopt;
      }
      result.kind = JsonValue::Kind::String;
      result.string = std::move(*text);
      return result;
    }
    for (const auto& [word, kind, flag] :
         {std::tuple{std::string_view{"true"}, JsonValue::Kind::Bool, true},
          std::tuple{std::string_view{"false"}, JsonValue::Kind::Bool, false},
          std::tuple{std::string_view{"null"}, JsonValue::Kind::Null, false}})
    {
      if (m_text.substr(m_at, word.size()) == word)
      {
        m_at += word.size();
        result.kind = kind;
        result.boolean = flag;
        return result;
      }
    }
    auto number = 0.0;
    const auto* first = m_text.data() + m_at;
    const auto* last = m_text.data() + m_text.size();
    const auto* end = parseFloatChars(first, last, number);
    if (!end)
    {
      return std::nullopt;
    }
    m_at += std::size_t(end - first);
    result.kind = JsonValue::Kind::Number;
    result.number = number;
    return result;
  }
};

// LP4.1 (C38): the view lights of a parity row's frame, by their record's "name", with
// their scissors (GL's unflipped space), and the time of the flow that built its lists
struct ParityLists
{
  std::vector<std::pair<std::string, ScreenRect>> lights;
  double flowMs = 0.0;
};

struct ParityFrame
{
  ChainFrame frame;
  ParityLists lists;
};

/**
 * LP2.3 (C29, D77): asks for frames of camera until one is settled with the passes drawn
 * and tonemapped, the scene's geometry drawn (prepass draws) and, for a lit row, a light
 * drawn (LP4.1: unless the row expects none). DX.4: "settled" waits for every batch, bake
 * read, entityDef and provisional light itself, so LP2.3's guard of 300 ms without a new
 * batch is gone. That frame's record with its lists' view lights, or nullopt at the
 * deadline.
 */
std::optional<ParityFrame> settledParityFrame(
  LitSession& session,
  const CameraState& camera,
  const bool needsLight,
  const std::chrono::milliseconds timeout)
{
  session.setCamera(camera);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.tableChanged();
    auto frame = std::make_shared<ParityFrame>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterTonemapForTesting([frame, done](RenderFrameAccess& access) {
      frame->frame = chainFrameOf(access);
      const auto& lists = access.passes.lists();
      frame->lists.flowMs = lists.flowMs;
      for (const auto& light : lists.lights)
      {
        const auto* name = findKey(light.record->spawnArgs, "name");
        frame->lists.lights.emplace_back(name ? *name : std::string{}, light.scissor);
      }
      done->set_value();
    });
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    const auto& f = frame->frame;
    const auto ready = f.settled && f.lit.camera == camera && f.tonemap.tonemapped
                       && f.tonemap.blitted && f.lit.prepassDraws > 0
                       && (!needsLight || f.lit.lightsDrawn > 0);
    if (ready)
    {
      return *frame;
    }
    std::this_thread::sleep_for(50ms);
  }
  return std::nullopt;
}

// U3.4: the exposure the preview's exposure pass left in its buffer, and the CPU model's
// exposure of the same frame's HDR target (TonemapModel: histogram.cs and exposure.cs
// over every pixel of the histogram's view, with the constants the frame gave the passes)
struct ParityExposure
{
  float preview = 0.0f;
  float model = 0.0f;
};

std::optional<ParityExposure> parityExposure(
  LitSession& session, const std::chrono::milliseconds timeout)
{
  struct State
  {
    bool copied = false;
    nvrhi::BufferHandle exposure;
    nvrhi::StagingTextureHandle hdr;
    EngineToneMappingConstants histogram;
    EngineToneMappingConstants exposureConstants;
  };
  auto state = std::make_shared<State>();
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  session.runAfterTonemapForTesting([state, done](RenderFrameAccess& access) {
    const auto& tonemap = access.tonemap.lastStats();
    if (tonemap.tonemapped)
    {
      namespace fr = frame_resource;
      auto bufferDesc = nvrhi::BufferDesc{};
      bufferDesc.byteSize = 4;
      bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
      bufferDesc.debugName = "U3.4 exposure readback";
      bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
      bufferDesc.keepInitialState = true;
      state->exposure = access.device.createBuffer(bufferDesc);
      access.commandList.copyBuffer(
        state->exposure, 0, access.frame.find(fr::ExposureBuffer)->buffer, 0, 4);
      auto* hdr = access.frame.find(fr::CurrentRenderHdr)->texture;
      const auto& desc = hdr->getDesc();
      state->hdr = access.device.createStagingTexture(
        nvrhi::TextureDesc{}
          .setWidth(desc.width)
          .setHeight(desc.height)
          .setFormat(desc.format),
        nvrhi::CpuAccessMode::Read);
      access.commandList.copyTexture(
        state->hdr, nvrhi::TextureSlice{}, hdr, nvrhi::TextureSlice{});
      state->histogram = tonemap.histogramConstants;
      state->exposureConstants = tonemap.exposureConstants;
      state->copied = true;
    }
    done->set_value();
  });
  if (future.wait_for(timeout) != std::future_status::ready || !state->copied)
  {
    return std::nullopt;
  }
  auto image = HdrImage{};
  auto value = std::optional<float>{};
  const auto read = inFrame(
    session,
    [&](RenderFrameAccess& access) {
      const auto* bits = static_cast<const std::uint32_t*>(
        access.device.mapBuffer(state->exposure, nvrhi::CpuAccessMode::Read));
      if (bits)
      {
        auto exposure = 0.0f;
        std::memcpy(&exposure, bits, sizeof(exposure));
        value = exposure;
        access.device.unmapBuffer(state->exposure);
      }
      const auto& desc = state->hdr->getDesc();
      image.width = desc.width;
      image.height = desc.height;
      image.rgba.resize(std::size_t(desc.width) * desc.height * 4);
      auto pitch = std::size_t{0};
      const auto* bytes =
        static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
          state->hdr, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
      for (std::uint32_t y = 0; bytes && y < desc.height; ++y)
      {
        const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
        for (std::uint32_t x = 0; x < desc.width * 4; ++x)
        {
          image.rgba[std::size_t(y) * desc.width * 4 + x] = halfToFloat(row[x]);
        }
      }
      if (bytes)
      {
        access.device.unmapStagingTexture(state->hdr);
      }
      state->exposure = nullptr;
      state->hdr = nullptr;
    },
    timeout);
  if (!read || !value)
  {
    return std::nullopt;
  }
  const auto histogram =
    modelHistogramOfImage(image.rgba, image.width, image.height, state->histogram);
  return ParityExposure{
    *value, modelExposure(histogram, *value, state->exposureConstants)};
}

// LP4.1: the .proc beside a map of the parity root (the engine's dmap of it), read once
const ProcTree& parityTree(const std::filesystem::path& root, const std::string& map)
{
  static auto trees = std::map<std::string, ProcTree>{};
  auto proc = (root / "base" / map).replace_extension(".proc");
  const auto key = proc.string();
  if (const auto it = trees.find(key); it != trees.end())
  {
    return it->second;
  }
  auto tree = readProcFile(readTextFile(proc))
              | kdl::if_error([&](const auto& e) { FAIL(key << ": " << e.msg); })
              | kdl::value();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return trees.emplace(key, std::move(tree)).first->second;
}

// the snapshot of a map of the parity root, its .proc beside it
std::shared_ptr<const Snapshot> paritySnapshot(
  const std::filesystem::path& root, const std::string& map, const std::uint64_t sequence)
{
  auto snapshot = snapshotOfMapText(readTextFile(root / "base" / map));
  snapshot.generation = 7;
  snapshot.sequence = sequence;
  snapshot.mapPath = (root / "base" / map).string();
  return std::make_shared<const Snapshot>(std::move(snapshot));
}

} // namespace

/**
 * LP2.3 (C29): the parity rows that TOOLS/lit-preview/parity.py run shot in the engine,
 * drawn by one headless session over the preview game root parity.py assembled
 * (TB_LIT_PARITY_ROOT, read only: the LP2 blobs, the maps and the engine's .proc files,
 * the fixture's materials, entityDef and images, the engine's .bimage caches), each row
 * at its shot's size with the camera of its line and the fov of its integer g_fov,
 * settled (D77; U2.13: at most 10 s a row with its readback, the whole test under
 * ParityTestBound, so that a stuck row is named before the run's 180 s bound), its LDR
 * target read back and written as
 * <row>.rgba with <row>.json into TB_LIT_PARITY_OUT, with the row's frames of the frame
 * log as <row>.framelog.csv; parity.py compare compares them with the engine's shots.
 * Asserted here: every row settles, the control rows are black, and the validation
 * layer reports nothing beyond M-PIPE-DEFECT-01's warnings. LP4.1 (C38): each row's .json
 * also holds the frame's portal-flow statistics and its view lights' scissors; a row's
 * "expect" (parity.py's row table) is asserted, and every light of the row's map that the
 * engine's listRenderLightDefs printed has as many area references in the preview as
 * there, and the areas of the room centres its volume is built to reach. Hidden: it needs
 * a GPU and the root of a parity.py run.
 */
TEST_CASE("LitSession parity rows (LP2.3)", "[.][parity]")
{
  // U2.13
  constexpr auto ParityRowBound = std::chrono::milliseconds{10000};
  constexpr auto ParityTestBound = std::chrono::milliseconds{150000};
  const auto testDeadline = std::chrono::steady_clock::now() + ParityTestBound;
  const auto* rootValue = std::getenv("TB_LIT_PARITY_ROOT");
  const auto* outValue = std::getenv("TB_LIT_PARITY_OUT");
  if (!rootValue || !*rootValue || !outValue || !*outValue)
  {
    SKIP(
      "TB_LIT_PARITY_ROOT or TB_LIT_PARITY_OUT is not set: the rows need the preview "
      "root of a parity.py run");
  }
  const auto root = std::filesystem::path{rootValue};
  const auto out = std::filesystem::path{outValue};
  const auto rowsText = readTextFile(root / "parity" / "rows.json");
  const auto record = JsonReader{rowsText}.value();
  REQUIRE(record);
  const auto* rows = record->find("rows");
  REQUIRE(rows);
  REQUIRE(rows->kind == JsonValue::Kind::Array);
  REQUIRE_FALSE(rows->array.empty());
  const auto pinned = record->flag("pinned");
  // DX.4: R09's _color comes from an entityDef; read first, right after the parity map is
  // offered, its pixels prove that a settled frame has the entityDef applied
  auto order = std::vector<std::size_t>{};
  for (std::size_t i = 0; i < rows->array.size(); ++i)
  {
    if (rows->array[i].text("id") == "R09")
    {
      order.insert(order.begin(), i);
    }
    else
    {
      order.push_back(i);
    }
  }

  // this test's outputs from an earlier run, by name
  std::filesystem::create_directories(out);
  const auto frameLog = out / "preview.framelog.csv";
  std::filesystem::remove(frameLog);
  for (const auto& row : rows->array)
  {
    const auto id = row.text("id");
    for (const auto* suffix : {".rgba", ".json", ".framelog.csv"})
    {
      std::filesystem::remove(out / (id + suffix));
    }
  }

  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  // per row: the frames it presented, from and to
  auto frameRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>{};
  {
    const auto instance = TestInstance{library, true, false, messages};
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto service = rootService(root);
    auto events = Events{};
    auto width = std::uint32_t(record->numberOr("width", 1280.0));
    auto height = std::uint32_t(record->numberOr("height", 720.0));
    auto options = SessionOptions{};
    options.validation = true;
    options.gamePath = root;
    options.frameLogPath = frameLog;
    const auto created = std::chrono::steady_clock::now();
    auto session = LitSession::create(
                     instance.handles(),
                     VK_NULL_HANDLE,
                     width,
                     height,
                     options,
                     events.sink(),
                     7,
                     service.get())
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    if (pinned)
    {
      // Q21's diagnostic: the exposure pinned as parity.py pinned the engine's
      const auto* luminance = record->find("pinned_luminance");
      REQUIRE(luminance);
      REQUIRE(luminance->array.size() == 2u);
      auto tonemap = EngineTonemap;
      tonemap.hdrMinLuminance = float(luminance->array[0].number);
      tonemap.hdrMaxLuminance = float(luminance->array[1].number);
      session->setTonemapForTesting(tonemap);
    }
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));

    // the written stamp of the rows.json each read-back was made from (U1.9)
    const auto written = record->text("written");

    auto currentMap = std::string{};
    auto sequence = std::uint64_t{0};
    for (const auto index : order)
    {
      const auto& row = rows->array[index];
      const auto id = row.text("id");
      CAPTURE(id);
      const auto map = row.text("map");
      if (map != currentMap)
      {
        REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
        REQUIRE(session->offerSnapshot(paritySnapshot(root, map, ++sequence)));
        currentMap = map;
      }
      const auto parsed = parityCameraOfLine(row.text("camera"));
      REQUIRE(parsed);
      // C29: the fov of the integer g_fov, the shot's size
      CHECK(parsed->gFov == int(row.numberOr("g_fov", 0.0)));
      CHECK(viewProjectionOf(parsed->camera).gFov == float(parsed->gFov));
      const auto rowWidth = std::uint32_t(row.numberOr("width", double(width)));
      const auto rowHeight = std::uint32_t(row.numberOr("height", double(height)));
      if (rowWidth != width || rowHeight != height)
      {
        width = rowWidth;
        height = rowHeight;
        session->resizeHeadlessForTesting(width, height);
      }
      auto camera = parsed->camera;
      camera.viewportWidth = width;
      camera.viewportHeight = height;

      const auto control = row.flag("control");
      const auto framesBefore = session->framesPresented();
      const auto started = std::chrono::steady_clock::now();
      // U2.13: what is left of the row's bound, and of the test's
      const auto rowDeadline = std::min(started + ParityRowBound, testDeadline);
      const auto left = [&] {
        return std::max(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            rowDeadline - std::chrono::steady_clock::now()),
          std::chrono::milliseconds{1});
      };
      // LP4.1 (C38): the row's expected flow statistics; a lit row that expects no light
      // drawn (P02) settles without one
      const auto* expect = row.find("expect");
      const auto expectsNoLight =
        expect && expect->find("lights_drawn")
        && expect->find("lights_drawn")->kind == JsonValue::Kind::Number
        && expect->find("lights_drawn")->number == 0.0;
      const auto settledRow =
        settledParityFrame(*session, camera, !control && !expectsNoLight, left());
      REQUIRE(settledRow);
      const auto* settled = &settledRow->frame;
      const auto image = session->readbackLdrForTesting(left());
      REQUIRE(image);
      const auto settleMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
      frameRanges.emplace_back(framesBefore, session->framesPresented());
      // U3.4: the exposure the frame left, beside the CPU model's of its HDR target
      const auto exposure = parityExposure(*session, left());
      REQUIRE(exposure);
      REQUIRE(image->width == width);
      REQUIRE(image->height == height);
      REQUIRE(image->rgba.size() == std::size_t(width) * height * 4);

      {
        auto file = std::ofstream{out / (id + ".rgba"), std::ios::binary};
        file.write(
          reinterpret_cast<const char*>(image->rgba.data()),
          std::streamsize(image->rgba.size()));
      }
      const auto& lit = settled->lit;
      // LP4.1 (C38): the view lights' scissors, in GL's unflipped space, and whether one
      // is smaller than the view's (0, 0, w - 1, h - 1)
      auto viewLights = std::vector<std::string>{};
      auto smallerScissor = false;
      for (const auto& [name, scissor] : settledRow->lists.lights)
      {
        viewLights.push_back(fmt::format(
          "{{\"name\": \"{}\", \"scissor\": [{}, {}, {}, {}]}}",
          name,
          scissor.x1,
          scissor.y1,
          scissor.x2,
          scissor.y2));
        smallerScissor = smallerScissor || scissor.x1 > 0 || scissor.y1 > 0
                         || scissor.x2 < int(width) - 1 || scissor.y2 < int(height) - 1;
      }
      writeTextFile(
        out / (id + ".json"),
        fmt::format(
          "{{\"width\": {}, \"height\": {}, \"settle_ms\": {:.1f}, \"frames\": {}, "
          "\"lights_visible\": {}, \"lights_drawn\": {}, \"prepass_draws\": {}, "
          "\"interaction_draws\": {}, \"session_ms\": {:.1f}, \"exposure\": {:.9g}, "
          "\"exposure_model\": {:.9g}, \"rows_written\": \"{}\", \"order\": {}, "
          "\"view_area\": {}, \"areas_flowed\": {}, \"areas\": {}, \"lights_in_view\": "
          "{}, "
          "\"portal_chains\": {}, \"flow_ms\": {:.4f}, \"scissor_smaller_than_view\": "
          "{}, "
          "\"view_lights\": [{}]}}\n",
          width,
          height,
          settleMs,
          session->framesPresented() - framesBefore,
          lit.lightsVisible,
          lit.lightsDrawn,
          lit.prepassDraws,
          lit.interactionDraws,
          std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - created)
            .count(),
          exposure->preview,
          exposure->model,
          written,
          frameRanges.size() - 1,
          lit.viewArea,
          lit.areasFlowed,
          lit.areas,
          lit.lightsInView,
          lit.portalChains,
          settledRow->lists.flowMs,
          smallerScissor ? "true" : "false",
          fmt::join(viewLights, ", ")));

      // LP4.1 (C38): the row's expected flow statistics, as parity.py's row table records
      // them (view_area "known": an area, not -1)
      if (expect)
      {
        for (const auto& [key, value] : expect->object)
        {
          CAPTURE(key);
          if (key == "view_area" && value.kind == JsonValue::Kind::String)
          {
            CHECK(value.string == "known");
            CHECK(lit.viewArea >= 0);
          }
          else if (key == "view_area")
          {
            CHECK(lit.viewArea == int(value.number));
          }
          else if (key == "areas_flowed")
          {
            CHECK(lit.areasFlowed == std::size_t(value.number));
          }
          else if (key == "lights_in_view")
          {
            CHECK(lit.lightsInView == std::size_t(value.number));
          }
          else if (key == "lights_drawn")
          {
            CHECK(lit.lightsDrawn == std::size_t(value.number));
          }
          else if (key == "scissor_smaller_than_view")
          {
            CHECK(smallerScissor == value.boolean);
          }
          else
          {
            FAIL("an expectation this test does not know: " << key);
          }
        }
      }

      // LP4.1 (C38, LP1b.3's golden per parity map): the engine's listRenderLightDefs of
      // the row's map against the preview's light records: each light's number of area
      // references (the engine prints how many, not which) equals its derived.areas', and
      // its areas are those of the room centres its volume is built to reach (PointInArea
      // over the map's .proc)
      if (const auto* listed = row.find("lights"))
      {
        if (const auto* problem = listed->find("problem"))
        {
          FAIL(
            "parity.py could not match the engine's light listing: " << problem->string);
        }
        const auto* entries = listed->find("lights");
        REQUIRE(entries);
        const auto& tree = parityTree(root, map);
        const auto table = session->lightTable();
        REQUIRE(table);
        for (const auto& entry : entries->array)
        {
          const auto name = entry.text("name");
          CAPTURE(name);
          auto found = std::shared_ptr<const LightRecord>{};
          table->records.forEach([&](const PreviewId&, const auto& record) {
            const auto* recordName =
              record ? findKey(record->spawnArgs, "name") : nullptr;
            if (recordName && *recordName == name)
            {
              found = record;
            }
          });
          REQUIRE(found);
          CHECK(found->derived.areas.size() == std::size_t(entry.numberOr("refs", -1.0)));
          auto expected = std::set<int>{};
          if (const auto* reaches = entry.find("reaches"))
          {
            for (const auto& point : reaches->array)
            {
              REQUIRE(point.array.size() == 3u);
              const auto area = pointInArea(
                tree,
                EngineVec3{
                  float(point.array[0].number),
                  float(point.array[1].number),
                  float(point.array[2].number)});
              CHECK(area >= 0);
              expected.insert(area);
            }
          }
          CHECK(
            std::set<int>{found->derived.areas.begin(), found->derived.areas.end()}
            == expected);
        }
      }

      // the lights-off control: black (every channel at most 1); R17's light takes a dark
      // copy of its material there (V2.8)
      if (control)
      {
        auto lightPixels = std::size_t{0};
        for (std::size_t i = 0; i < image->rgba.size(); i += 4)
        {
          if (image->rgba[i] > 1 || image->rgba[i + 1] > 1 || image->rgba[i + 2] > 1)
          {
            ++lightPixels;
          }
        }
        CHECK(lightPixels == 0u);
      }
      std::cout << fmt::format(
        "LP2.3 parity {}: settled in {:.0f} ms over {} frames; lights {} of {}, prepass "
        "draws {}, interaction draws {}; exposure {:.6f}, the CPU model's {:.6f}\n",
        id,
        settleMs,
        session->framesPresented() - framesBefore,
        lit.lightsDrawn,
        lit.lightsVisible,
        lit.prepassDraws,
        lit.interactionDraws,
        exposure->preview,
        exposure->model);
    }

    for (const auto& program : session->programReport())
    {
      CAPTURE(program.name, program.reason);
      CHECK(program.enabled);
    }
    teardown(*session);
    CHECK(session->lastDeviceReleaseCount() == 0u);
    CHECK(session->nvrhiErrorCount() == 0u);
    CHECK(session->deviceMemoryAllocations() == 0);
    CHECK(events.count(SessionEvent::Error) == 0u);
    events.print("parity");
    session.reset();
    service->requestStop();
    service->join();
  }
  CHECK(resetDispatcher());

  // the frame log's frames of each row (one session; the k-th frame row is the k-th
  // frame presented)
  {
    const auto text = readTextFile(frameLog);
    auto stream = std::istringstream{text};
    auto header = std::string{};
    std::getline(stream, header);
    auto frames = std::vector<std::string>{};
    auto line = std::string{};
    while (std::getline(stream, line))
    {
      if (line.find(",frame,") != std::string::npos)
      {
        frames.push_back(line);
      }
    }
    for (std::size_t i = 0; i < frameRanges.size(); ++i)
    {
      auto rowLog = header + "\n";
      for (auto f = frameRanges[i].first; f < frameRanges[i].second && f < frames.size();
           ++f)
      {
        rowLog += frames[std::size_t(f)] + "\n";
      }
      writeTextFile(out / (rows->array[order[i]].text("id") + ".framelog.csv"), rowLog);
    }
  }

  messages.print("parity, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

} // namespace tb::lit

#endif
