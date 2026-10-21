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

#include "StringMakers.h"
#include "lit/LitSession.h"
#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <vulkan/vulkan.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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

// Validation-layer messages, counted through a debug-utils messenger on the test's
// instance.
struct ValidationMessages
{
  std::atomic<int> errors = 0;
  std::atomic<int> warnings = 0;
  std::mutex mutex;
  std::vector<std::string> texts;

  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
  {
    auto& self = *static_cast<ValidationMessages*>(user);
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
              << "\n";
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
      if (event != SessionEvent::Ready && event != SessionEvent::Parked)
      {
        std::cout << label << " event " << int(event) << " (gen " << generation
                  << "): " << message << "\n";
      }
    }
  }
};

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

void teardown(LitSession& session)
{
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
      initInstanceDispatcher(instance.handles());

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
    resetDispatcher();

    messages.print(validation ? "headless, validation" : "headless");
    CHECK(messages.errors == 0);
    std::cout << "headless device: " << deviceName << "\n";

    // the first cycle's frame log: every frame with its GPU time, resolved by stop
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 5u);
    CHECK(rows[0][0] == "frame");
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 8u);
      CHECK(rows[i][0] == std::to_string(i - 1));
      CHECK_FALSE(rows[i][2].empty());
      CHECK(rows[i][3] == "headless");
      CHECK(rows[i][6] == "64");
      CHECK(rows[i][7] == "48");
    }
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
      initInstanceDispatcher(instance.handles());

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
    resetDispatcher();
    messages.print("fault, validation");
    CHECK(messages.errors == 0);
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
      initInstanceDispatcher(instance.handles());

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
    resetDispatcher();

    messages.print(forceFifo ? "window, forced FIFO" : "window");
    CHECK(messages.errors == 0);

    // what the first cycle presented: present modes, generations and extents
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 12u);
    auto seen = std::set<std::string>{};
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 8u);
      if (forceFifo)
      {
        CHECK(rows[i][3] == "fifo");
      }
      seen.insert(
        rows[i][3] + " generation " + rows[i][4] + " " + rows[i][6] + "x" + rows[i][7]);
    }
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

} // namespace tb::lit

#endif
