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
#include "fs/TestEnvironment.h"
#include "lit/CpuPool.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/FramePacer.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/LightRecord.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/ShaderReflection.h"
#include "lit/Snapshot.h"
#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
  // LP2.0: a warning that contains this text is counted in mirrored, not in warnings: the
  // engine's own LAYOUT_DRAW_VERT input layout (six locations, RenderProgs_NVRHI.cpp:
  // 250-256) against vertex shaders that read fewer of them, which the layer reports as
  // attributes "not consumed" (mirror.json M-PIPE-DEFECT-01); empty counts every message
  std::string_view mirroredWarning;
  std::atomic<int> mirrored = 0;

  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
  {
    auto& self = *static_cast<ValidationMessages*>(user);
    const auto text = std::string_view{data && data->pMessage ? data->pMessage : ""};
    if (
      !(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      && !self.mirroredWarning.empty() && text.find(self.mirroredWarning) != text.npos)
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
      REQUIRE(rows[i].size() == 14u);
      CHECK(rows[i][0] == "1");
      CHECK(rows[i][1] == "frame");
      CHECK(rows[i][2] == std::to_string(i - 2));
      CHECK_FALSE(rows[i][6].empty());
      CHECK(rows[i][7] == "headless");
      CHECK(rows[i][10] == "64");
      CHECK(rows[i][11] == "48");
    }
    // VK_EXT_memory_budget, which this machine's GPU has
    CHECK_FALSE(rows[1][12].empty());
    CHECK_FALSE(rows.back()[12].empty());
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

  SECTION("D71: pipelines made on the compile thread while the render thread draws")
  {
    const auto* gamePath = std::getenv("TB_LIT_GAME_PATH");
    if (!gamePath || !*gamePath)
    {
      SKIP("TB_LIT_GAME_PATH is not set: the pipelines need the engine's shader blobs");
    }

    auto messages = ValidationMessages{};
    messages.mirroredWarning = "not consumed by vertex shader";
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = gamePath;
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
  }

  SECTION("D71: a stop while the compile thread makes a pipeline")
  {
    const auto* gamePath = std::getenv("TB_LIT_GAME_PATH");
    if (!gamePath || !*gamePath)
    {
      SKIP("TB_LIT_GAME_PATH is not set: the pipelines need the engine's shader blobs");
    }

    auto messages = ValidationMessages{};
    messages.mirroredWarning = "not consumed by vertex shader";
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = gamePath;
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
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
    CHECK(resetDispatcher());
    messages.print("a stop while compiling, validation");
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
      REQUIRE(rows[i].size() == 14u);
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

namespace
{

// LP2.1: runs hook on the render thread in the session's next frame (RenderFrameAccess)
// and waits for it; a hook asserts nothing itself (Catch2 is not thread-safe), it
// gathers what the test checks
template <typename Hook>
bool inFrame(
  LitSession& session, Hook hook, const std::chrono::milliseconds timeout = 10000ms)
{
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  session.runInFrameForTesting(
    [hook = std::move(hook), done](RenderFrameAccess& access) mutable {
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
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto events = Events{};
      auto session = gpuSession(instance, events, 1);
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
      }

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
    }
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

      const auto lock = std::scoped_lock{mutex};
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
}

} // namespace tb::lit

#endif
