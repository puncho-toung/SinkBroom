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
#include "lit/EngineGeometry.h"
#include "lit/EngineImageData.h"
#include "lit/EnginePipelineState.h"
#include "lit/EngineTables.h"
#include "lit/FramePacer.h"
#include "lit/GameDeclSource.h"
#include "lit/GameFile.h"
#include "lit/GameFileSource.h"
#include "lit/GridTable.h"
#include "lit/ImageCache.h"
#include "lit/IntrinsicImages.h"
#include "lit/LightGridFile.h"
#include "lit/LightRecord.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/NumberText.h"
#include "lit/ProbeRecord.h"
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
#include <bit>
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

#include <catch2/catch_approx.hpp>
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

TEST_CASE(
  "FrameConvergence: what keeps a frame from being settled (D77 DX.2 DX.4 U2.11 LP4.2b)")
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
         {&FrameConvergence::lightTrianglesPending, false},
         // LP4.2a (D88): the slot's arrival is the trigger
         {&FrameConvergence::probeImagesPending, false},
         // LP4.2b step 0: the probe build's collection is the trigger
         {&FrameConvergence::probesInFlight, false}})
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

  // LP4.2a (D9, C34): a disabled ambient program (4 to 11) says the ambient pass is off
  const auto ambientPrograms = std::vector<ProgramReport>{
    {EngineBuiltinAmbientLightingIbl,
     "builtin/lighting/ambient_lighting_IBL",
     false,
     "no shader blob builtin/lighting/ambient_lighting_IBL.ps.bin"},
    {EngineBuiltinAmbientLightgridIblPbr + 1,
     "builtin/lighting/ambient_lightgrid_IBL_PBR_skinned",
     true,
     ""}};
  CHECK(
    sessionStatusLine({}, ambientPrograms)
    == "starting | ambient disabled: builtin/lighting/ambient_lighting_IBL (no shader "
       "blob builtin/lighting/ambient_lighting_IBL.ps.bin)");

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

  // LP4.3 (D90, K8, K29): the bake, once known, after the flow and before the programs
  using Bake = SessionStatus::Bake;
  auto baked = flowed;
  baked.viewArea = 1;
  baked.areasFlowed = 2;
  const auto head = std::string{
    "settled | 1 of 1 light drawn | 9 draws | VRAM 1.0 MB | area 1, 2 of 8 areas"};
  CHECK(sessionStatusLine(baked, {}) == head);
  baked.bake = Bake::Proc;
  baked.bakeTime = 1000;
  baked.mapFileTime = 1000;
  CHECK(sessionStatusLine(baked, {}) == head + " | bake: proc");
  baked.bake = Bake::Bproc;
  CHECK(sessionStatusLine(baked, {}) == head + " | bake: bproc");
  // stale: the map file written after the tree's file, or unsaved changes
  baked.mapFileTime = 1001;
  CHECK(sessionStatusLine(baked, {}) == head + " | bake: bproc, stale");
  baked.mapFileTime = 999;
  baked.documentModified = true;
  CHECK(sessionStatusLine(baked, {}) == head + " | bake: bproc, stale");
  // a container's time 0 says nothing of the map file's
  baked.documentModified = false;
  baked.bakeTime = 0;
  baked.mapFileTime = 5000;
  CHECK(sessionStatusLine(baked, {}) == head + " | bake: bproc");
  // the pieces in solid, one and more
  baked.solidPieces = 1;
  CHECK(
    sessionStatusLine(baked, {})
    == head + " | bake: bproc, 1 piece outside the baked hull");
  baked.bake = Bake::Proc;
  baked.bakeTime = 10;
  baked.solidPieces = 6;
  CHECK(
    sessionStatusLine(baked, programs)
    == head
         + " | bake: proc, stale, 6 pieces outside the baked hull | disabled: "
           "builtin/post/histogram (no shader blob builtin/post/histogram.cs.bin)");
  // dropped: the reason; the tree is kept, so it may be stale too
  baked.bake = Bake::Dropped;
  baked.bakeDropReason =
    "the light grid has 1 entries for 1 distinct areas, the map has 2 areas";
  baked.solidPieces = 0;
  baked.mapFileTime = 5;
  CHECK(
    sessionStatusLine(baked, {})
    == head
         + " | bake: dropped (the light grid has 1 entries for 1 distinct areas, the map "
           "has 2 areas)");
  // none: no tree, nothing to be stale
  auto none = noTree;
  none.bake = Bake::None;
  none.documentModified = true;
  CHECK(
    sessionStatusLine(none, {})
    == "settled | 1 of 1 light drawn | 9 draws | VRAM 1.0 MB | bake: none");
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
      REQUIRE(rows[i].size() == 49u);
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
      REQUIRE(rows[i].size() == 49u);
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
      // LP4.2b step 0: the probe build of a scene without a material service is final
      // once it landed (no tree: no probe)
      CHECK_FALSE(after.probesInFlight);

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
      if (row.size() == 49u && row[1] == "frame" && !row[15].empty() && row[15] != "0")
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
      // LP4.2a (D85): the raw intrinsic path's two images, the RG16F BRDF LUT and the
      // 8 x 8 BC6H default radiance cube
      caches.emplace_back("_brdfLut", *rawIntrinsicImage("_brdfLut"));
      caches.emplace_back(
        "_defaultUACRadiance", *rawIntrinsicImage("_defaultUACRadiance"));
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
    if (row.size() == 49u && row[1] != "event" && !row[14].empty() && row[0] != "session")
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
// only) into a test game root; LP4.2a: and the ambient pass's probe path
constexpr auto Lp2Programs = std::array<int, 14>{
  EngineBuiltinColor,
  EngineBuiltinTextureVertexColor,
  EngineBuiltinInteraction,
  EngineBuiltinPbrInteraction,
  EngineBuiltinDepth,
  EngineBuiltinBlit,
  EngineBuiltinRect,
  EngineBuiltinTonemapping,
  EngineBuiltinHistogram,
  EngineBuiltinExposure,
  EngineBuiltinAmbientLightingIbl,
  EngineBuiltinAmbientLightingIblPbr,
  // LP4.3 (C36): the light-grid path's
  EngineBuiltinAmbientLightgridIbl,
  EngineBuiltinAmbientLightgridIblPbr};

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
 * missing. LP4.2a (C37, P4.5): also the game's env_probe entityDef
 * (def/_rbdoom_env_probe.def), so that the fixture's two areas spawn their automatic
 * probes as the engine's would.
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
  {
    const auto def = gamePath / "base" / "def" / "_rbdoom_env_probe.def";
    if (!std::filesystem::exists(def))
    {
      return {};
    }
    std::filesystem::create_directories(base / "def");
    std::filesystem::copy_file(def, base / "def" / "_rbdoom_env_probe.def");
  }
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
  // LP4.2a: the ambient pass's vertex shader reads locations 0-4, not COLOR1
  case EngineBuiltinAmbientLightingIbl:
  case EngineBuiltinAmbientLightingIblPbr:
  // LP4.3: the light-grid path's vertex shader likewise
  case EngineBuiltinAmbientLightgridIbl:
  case EngineBuiltinAmbientLightgridIblPbr:
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
      // LP4.2a (D86): one write of set 10's buffer at most, and one of the ambient pass's
      // set 9, a buffer and a budget of its own, whose own overflow (a second space in
      // the view) is said by a warning of its own (the D86 section of LP4.2a's test)
      CHECK(overflowed.constantWrites <= 2u);
      CHECK(
        events.countContaining(SessionEvent::Warning, "interactions overflowed") == 1u);
      CHECK(
        events.count(SessionEvent::Warning)
        == warningsBefore + 1 + (overflowed.ambientOverflowed > 0 ? 1 : 0));
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
        // LP4.2b step 0: the env_probe entityDef the scene asked for at its start waits
        // at the gate too: the probe table is provisional
        CHECK(seen.probesInFlight);
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
        // LP4.2b step 0: the probes too: built with the bake's tree (the fixture's two
        // areas, one automatic probe each) and the answered env_probe entityDef
        CHECK_FALSE(settledSeen.probesInFlight);
        const auto probes = session->probeTable();
        CHECK(probes->version > 0u);
        CHECK_FALSE(probes->provisional);
        CHECK(probes->records.size() == (variant.bake ? 2u : 0u));
        std::cout << fmt::format(
          "V2.4 {}: settled by itself after {} frames (the probes in flight while the "
          "parse thread was held: {})\n",
          variant.label,
          framesSettled - framesAtOpen,
          seen.probesInFlight);

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
    REQUIRE(rows[0].size() == 49u);
    CHECK(rows[0][18] == "gpu_prepass_ms");
    CHECK(rows[0][34] == "settled");
    CHECK(rows[0][35] == "alloc_refused");
    CHECK(rows[0][36] == "culled_triangles");
    // LP4.1 (C16): the view's portal flow's five columns
    CHECK(rows[0][37] == "view_area");
    CHECK(rows[0][41] == "flow_ms");
    // LP4.2a (C34): the ambient pass's two
    CHECK(rows[0][42] == "ambient_draws");
    CHECK(rows[0][43] == "gpu_ambient_ms");
    auto lastLit = std::optional<std::vector<std::string>>{};
    auto settledRows = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == 49u);
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
        if (row.size() != 49u || row[1] != "frame")
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
  // LP4.3 (C36): per chunk entity, the grid areas of its drawn surfaces (-1: none, the
  // probe path)
  std::map<PreviewId, std::vector<int>> gridAreas;
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
 * batch is gone. LP4.2b (C38): the frame's interactions skipped exactly when the row is
 * ambient-only (a frame begun before the row's switch is not taken). That frame's record
 * with its lists' view lights, or nullopt at the deadline.
 */
std::optional<ParityFrame> settledParityFrame(
  LitSession& session,
  const CameraState& camera,
  const bool needsLight,
  const bool interactionsSkipped,
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
      for (const auto& surface : lists.drawSurfs)
      {
        frame->lists.gridAreas[surface.chunk->key.entity].push_back(
          surface.gridArea.value_or(-1));
      }
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
                       && (!needsLight || f.lit.lightsDrawn > 0)
                       && f.lit.interactionsSkipped == interactionsSkipped;
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

// LP4.2b: every preview id of a snapshot, its entities' and its nodes'
// LP4.3: a snapshot's entities by their "name" key (the last one, as findKey reads it)
std::map<std::string, PreviewId> snapshotEntityNames(const Snapshot& snapshot)
{
  auto names = std::map<std::string, PreviewId>{};
  for (const auto& entity : snapshot.entities)
  {
    for (auto i = entity.keys.first; i < entity.keys.first + entity.keys.count; ++i)
    {
      const auto& keyValue = snapshot.keyValues[i];
      if (snapshot.string(keyValue.key) == "name")
      {
        names[std::string{snapshot.string(keyValue.value)}] = entity.id;
      }
    }
  }
  return names;
}

// LP4.3: an expectation held by a count: a number is its equal, {"min": n} at least n
bool parityCountHolds(const JsonValue& expected, const std::size_t actual)
{
  if (expected.kind == JsonValue::Kind::Object)
  {
    const auto* min = expected.find("min");
    return min && double(actual) >= min->number;
  }
  return actual == std::size_t(expected.number);
}

std::vector<PreviewId> snapshotIds(const Snapshot& snapshot)
{
  auto ids = std::vector<PreviewId>{};
  for (const auto& entity : snapshot.entities)
  {
    ids.push_back(entity.id);
  }
  for (const auto& node : snapshot.nodes)
  {
    ids.push_back(node.id);
  }
  return ids;
}

/**
 * The snapshot of a map of the parity root, its .proc beside it. LP4.2b: it replaces the
 * map offered before it whole: its ids are offset by the sequence (sequence << 32) so
 * that none is one of the earlier map's, and it removes every id of that map (removed,
 * B0.4); without it a node of an earlier map whose id the next map does not reach stays
 * in the scene (LP2's rooms 5 and 7 in the ambient map's A05 and A06 views)
 */
std::shared_ptr<const Snapshot> paritySnapshot(
  const std::filesystem::path& root,
  const std::string& map,
  const std::uint64_t sequence,
  std::vector<PreviewId> removed = {})
{
  auto snapshot = snapshotOfMapText(readTextFile(root / "base" / map));
  const auto offset = PreviewId{sequence} << 32;
  for (auto& entity : snapshot.entities)
  {
    entity.id += offset;
  }
  for (auto& node : snapshot.nodes)
  {
    node.id += offset;
    node.entity += offset;
  }
  snapshot.removed = std::move(removed);
  snapshot.generation = 7;
  snapshot.sequence = sequence;
  snapshot.mapPath = (root / "base" / map).string();
  return std::make_shared<const Snapshot>(std::move(snapshot));
}

/**
 * LP4.2b (C38, P4.24): a parity row's switch for as long as the row is drawn: an
 * ambient-only row ("ambient_only": true, the engine's shot with r_skipInteractions 1)
 * skips the interactions, every other row draws them; reset to drawn when the row ends.
 * set is the session's skipInteractionsForTesting.
 */
class ParityRowSwitch
{
private:
  std::function<void(bool)> m_set;

public:
  ParityRowSwitch(const JsonValue& row, std::function<void(bool)> set)
    : m_set{std::move(set)}
  {
    m_set(row.flag("ambient_only"));
  }

  ~ParityRowSwitch() { m_set(false); }

  deleteCopyAndMove(ParityRowSwitch);
};

/**
 * LP4.2b (C37, C38): the preview's probe table of a baked map against the engine's
 * .shprobes of the parity run: every probe automatic, one per entry, each matched to the
 * first entry of its area and snapped point (R_DeriveEnvprobeData's match,
 * RenderWorld_defs.cpp:836-851) with its SH the entry's 27 floats bit for bit; the count
 * the bake process reported (rows.json's bake record).
 */
void checkProbesAgainstShProbes(
  const ProbeTable& table,
  const std::vector<ShProbe>& entries,
  const std::size_t bakeProbes)
{
  CHECK(table.records.size() == entries.size());
  CHECK(entries.size() == bakeProbes);
  CHECK(table.placed == 0u);
  CHECK(table.envProbeDef == EnvProbeDefState::Found);
  CHECK_FALSE(table.provisional);
  for (const auto& record : table.records)
  {
    CAPTURE(record.areaNum, record.point[0], record.point[1], record.point[2]);
    CHECK(record.automatic);
    CHECK(record.shFound);
    const auto entry = std::ranges::find_if(entries, [&](const ShProbe& e) {
      return e.area == record.areaNum && int(e.origin[0]) == record.point[0]
             && int(e.origin[1]) == record.point[1]
             && int(e.origin[2]) == record.point[2];
    });
    REQUIRE(entry != entries.end());
    auto differing = 0;
    for (std::size_t i = 0; i < entry->sh.size(); ++i)
    {
      differing += std::bit_cast<std::uint32_t>(record.shIrradiance[i / 3][i % 3])
                       != std::bit_cast<std::uint32_t>(entry->sh[i])
                     ? 1
                     : 0;
    }
    CHECK(differing == 0);
  }
}

} // namespace

// LP4.2b (C38, P4.24): the parity test's per-row switch over a synthetic rows.json: an
// ambient-only row skips the interactions while it is drawn, every row ends with them on
TEST_CASE("LitSession parity rows: the ambient-only switch (LP4.2b)")
{
  const auto record =
    JsonReader{
      R"({"rows": [{"id": "A01", "ambient_only": false}, {"id": "A01a", "ambient_only": true},)"
      R"( {"id": "R01"}]})"}
      .value();
  REQUIRE(record);
  const auto* rows = record->find("rows");
  REQUIRE(rows);
  REQUIRE(rows->array.size() == 3u);
  auto calls = std::vector<bool>{};
  auto during = std::vector<bool>{};
  for (const auto& row : rows->array)
  {
    const auto rowSwitch =
      ParityRowSwitch{row, [&](const bool skip) { calls.push_back(skip); }};
    during.push_back(calls.back());
  }
  CHECK(during == std::vector<bool>{false, true, false});
  CHECK(calls == std::vector<bool>{false, false, true, false, false, false});
}

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
    // LP4.2b: the baked maps whose probe table was held to their .shprobes; the ids of
    // the map offered last, which the next map's snapshot removes
    auto checkedBakes = std::set<std::string>{};
    auto previousIds = std::vector<PreviewId>{};
    // LP4.3: the current map's brush entities by name
    auto entityNames = std::map<std::string, PreviewId>{};
    for (const auto index : order)
    {
      const auto& row = rows->array[index];
      const auto id = row.text("id");
      CAPTURE(id);
      const auto map = row.text("map");
      if (map != currentMap)
      {
        REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
        auto snapshot = paritySnapshot(root, map, ++sequence, std::move(previousIds));
        previousIds = snapshotIds(*snapshot);
        entityNames = snapshotEntityNames(*snapshot);
        REQUIRE(session->offerSnapshot(std::move(snapshot)));
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
      // C38 (LP4.2b): an ambient-only row draws no interactions, as the engine's shot
      // with r_skipInteractions 1, and the next row draws them again
      const auto ambientOnly = row.flag("ambient_only");
      auto* const owner = session.get();
      const auto rowSwitch = ParityRowSwitch{
        row, [owner](const bool skip) { owner->skipInteractionsForTesting(skip); }};
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
      const auto settledRow = settledParityFrame(
        *session,
        camera,
        !control && !expectsNoLight && !ambientOnly,
        ambientOnly,
        left());
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
          R"({{"name": "{}", "scissor": [{}, {}, {}, {}]}})",
          name,
          scissor.x1,
          scissor.y1,
          scissor.x2,
          scissor.y2));
        smallerScissor = smallerScissor || scissor.x1 > 0 || scissor.y1 > 0
                         || scissor.x2 < int(width) - 1 || scissor.y2 < int(height) - 1;
      }
      // LP4.3 (C36): the named entities' grid areas, and the row's expected one (the area
      // of a point, over the map's .proc)
      auto entityGrid = std::vector<std::string>{};
      for (const auto& [name, entityId] : entityNames)
      {
        if (const auto it = settledRow->lists.gridAreas.find(entityId);
            it != settledRow->lists.gridAreas.end())
        {
          entityGrid.push_back(
            fmt::format("\"{}\": [{}]", name, fmt::join(it->second, ", ")));
        }
      }
      auto expectedEntityGrid = std::string{"null"};
      auto entityGridHolds = true;
      if (const auto* entityExpect = expect ? expect->find("entity_grid_area") : nullptr)
      {
        REQUIRE(entityExpect->array.size() == 4u);
        const auto& name = entityExpect->array[0].string;
        const auto expectedArea = pointInArea(
          parityTree(root, map),
          EngineVec3{
            float(entityExpect->array[1].number),
            float(entityExpect->array[2].number),
            float(entityExpect->array[3].number)});
        auto areas = std::vector<int>{};
        if (const auto named = entityNames.find(name); named != entityNames.end())
        {
          if (const auto it = settledRow->lists.gridAreas.find(named->second);
              it != settledRow->lists.gridAreas.end())
          {
            areas = it->second;
          }
        }
        entityGridHolds = !areas.empty() && expectedArea >= 0
                          && std::ranges::all_of(
                            areas, [&](const int area) { return area == expectedArea; });
        expectedEntityGrid = fmt::format(
          R"({{"name": "{}", "expected": {}, "areas": [{}]}})",
          name,
          expectedArea,
          fmt::join(areas, ", "));
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
          "{}, \"ambient_only\": {}, \"ambient_draws\": {}, \"probe_candidates\": {}, "
          "\"grid_surfaces\": {}, \"ambient_grid_surfaces\": {}, \"entity_grid_areas\": "
          "{{{}}}, "
          "\"entity_grid_area\": {}, \"view_lights\": [{}]}}\n",
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
          ambientOnly ? "true" : "false",
          lit.ambientDraws,
          lit.probeCandidates,
          lit.gridSurfaces,
          lit.ambientGridSurfaces,
          fmt::join(entityGrid, ", "),
          expectedEntityGrid,
          fmt::join(viewLights, ", ")));

      // C38 (LP4.2b): an ambient-only frame drew no interaction and was lit by the
      // ambient pass alone
      if (ambientOnly)
      {
        CHECK(lit.interactionsSkipped);
        CHECK(lit.interactionDraws == 0u);
      }

      // LP4.1 (C38): the row's expected flow statistics, as parity.py's row table records
      // them (view_area "known": an area, not -1); LP4.2b: the view's probe candidates
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
            CHECK(parityCountHolds(value, lit.lightsInView));
          }
          else if (key == "grid_surfaces")
          {
            // LP4.3 (C36): the drawn surfaces with a grid area
            CAPTURE(lit.gridSurfaces);
            CHECK(parityCountHolds(value, lit.gridSurfaces));
          }
          else if (key == "entity_grid_area")
          {
            CAPTURE(expectedEntityGrid);
            CHECK(entityGridHolds);
          }
          else if (key == "lights_drawn")
          {
            CHECK(lit.lightsDrawn == std::size_t(value.number));
          }
          else if (key == "scissor_smaller_than_view")
          {
            CHECK(smallerScissor == value.boolean);
          }
          else if (key == "probe_candidates")
          {
            CHECK(lit.probeCandidates == std::size_t(value.number));
          }
          else
          {
            FAIL("an expectation this test does not know: " << key);
          }
        }
      }

      // LP4.2b (C37, C38): at a baked map's first settled frame, the preview's probe
      // table against the engine's .shprobes of the run (beside the map in the preview
      // root)
      if (const auto shProbesPath = (root / "base" / map).replace_extension(".shprobes");
          std::filesystem::exists(shProbesPath) && checkedBakes.insert(map).second)
      {
        auto entries = readShProbes(readTextFile(shProbesPath))
                       | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
        static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
        const auto* bake = record->find("bake");
        REQUIRE(bake);
        const auto* bakeMaps = bake->find("maps");
        REQUIRE(bakeMaps);
        const auto bakeName =
          std::filesystem::path{map}.replace_extension().generic_string().substr(
            std::string_view{"maps/"}.size());
        const auto* bakeMap = bakeMaps->find(bakeName);
        REQUIRE(bakeMap);
        const auto table = session->probeTable();
        checkProbesAgainstShProbes(
          *table, entries, std::size_t(bakeMap->numberOr("probes", -1.0)));
        // LP4.3 (C36): the grid table of the .lightgrid against the bake's per-area
        // shapes: the same areas with points and the same bounds
        const auto* gridRecord = bakeMap->find("grid");
        REQUIRE(gridRecord);
        const auto* gridAreas = gridRecord->find("areas");
        REQUIRE(gridAreas);
        const auto grid = session->gridTable();
        REQUIRE(grid->areas.size() == gridAreas->array.size());
        auto withPoints = std::size_t{0};
        for (const auto& area : gridAreas->array)
        {
          const auto areaIndex = int(area.numberOr("area", -1.0));
          CAPTURE(areaIndex);
          const auto points = area.numberOr("points", 0.0) > 0.0;
          const auto* entry = grid->find(areaIndex);
          REQUIRE(entry);
          CHECK(entry->hasPoints == points);
          if (points)
          {
            ++withPoints;
            const auto* bounds = area.find("bounds");
            REQUIRE(bounds);
            REQUIRE(bounds->array.size() == 3u);
            for (std::size_t c = 0; c < 3; ++c)
            {
              CHECK(entry->bounds[c] == int(bounds->array[c].number));
            }
            CHECK(entry->probeSize == 16);
            CHECK(entry->borderSize == 2);
          }
        }
        std::cout << fmt::format(
          "LP4.3 parity {}: grid table of {} areas, {} with points, as the bake's record "
          "(version {})\n",
          map,
          grid->areas.size(),
          withPoints,
          grid->version);
        std::cout << fmt::format(
          "LP4.2b parity {}: {} automatic probes, every one with its .shprobes entry and "
          "SH (table version {})\n",
          map,
          table->records.size(),
          table->version);
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

    // LP4.2b (P4.24): the ambient pass's blobs are in the parity root: every program,
    // the ambient pass's too, is enabled
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

namespace
{

// LP4.2a: a .bimage as WriteGeneratedFile writes it (BinaryImage.cpp: the big-endian
// header, then per record level, destZ, width, height, dataSize and the bytes)
struct BimageRecord
{
  int level = 0;
  int destZ = 0;
  int width = 0;
  int height = 0;
  std::string data;
};

std::string bimageFile(
  const std::int64_t sourceTime,
  const int textureType,
  const int format,
  const int width,
  const int height,
  const int numLevels,
  const std::vector<BimageRecord>& records)
{
  const auto big = [](std::string& out, const std::uint64_t value, const int bytes) {
    for (auto i = bytes - 1; i >= 0; --i)
    {
      out.push_back(char((value >> (8 * i)) & 0xFF));
    }
  };
  auto out = std::string{};
  big(out, std::uint64_t(sourceTime), 8);
  for (const auto value :
       {int(engine_image::BimageMagic), textureType, format, 0, width, height, numLevels})
  {
    big(out, std::uint32_t(value), 4);
  }
  for (const auto& record : records)
  {
    for (const auto value :
         {record.level,
          record.destZ,
          record.width,
          record.height,
          int(record.data.size())})
    {
      big(out, std::uint32_t(value), 4);
    }
    out += record.data;
  }
  return out;
}

// DXGI R11G11B10_FLOAT of three values whose mantissas fit (5-bit exponents biased by
// 15; 6, 6 and 5 mantissa bits), little endian
std::string r11g11b10Texel(const float r, const float g, const float b)
{
  const auto field = [](const float v, const int mantissaBits) -> std::uint32_t {
    if (v == 0.0f)
    {
      return 0;
    }
    auto e = 0;
    const auto m = std::frexp(v, &e);
    const auto exponent = std::uint32_t(e - 1 + 15);
    const auto fraction =
      std::uint32_t(std::lround((2.0 * double(m) - 1.0) * double(1 << mantissaBits)));
    return (exponent << mantissaBits) | fraction;
  };
  const auto bits = field(r, 6) | (field(g, 6) << 11) | (field(b, 5) << 22);
  auto out = std::string(4, '\0');
  for (auto i = 0; i < 4; ++i)
  {
    out[std::size_t(i)] = char((bits >> (8 * i)) & 0xFF);
  }
  return out;
}

// the synthetic radiance cube: an 8 x 8 R11G11B10F cube of one level (A3: R11G11B10F
// stands in for BC6H with TD_HDR_LIGHTPROBE, Image_load.cpp:674-680), every texel rgb;
// stored time -1, as the engine's bake writes its _spec caches (SC347)
std::string constantCubeCache(const float r, const float g, const float b)
{
  auto face = std::string{};
  for (auto i = 0; i < 64; ++i)
  {
    face += r11g11b10Texel(r, g, b);
  }
  auto records = std::vector<BimageRecord>{};
  for (auto side = 0; side < 6; ++side)
  {
    records.push_back(BimageRecord{0, side, 8, 8, face});
  }
  return bimageFile(
    -1, engine_image::TypeCubic, engine_image::FormatR11g11b10f, 8, 8, 1, records);
}

// the LP4.2a materials of the synthetic-bake section: a flat legacy diffuse, a PBR
// metal (metallic 1, roughness 128/255, ao 1), two bump/diffuse groups whose second has
// vertexColor
const auto Lp42aMaterials = std::string{
  "textures/lp42a/flat\n{\n\tdiffusemap textures/lp42a/flat.tga\n}\n"
  "textures/lp42a/metal\n{\n\tbasecolormap textures/lp42a/metal.tga\n"
  "\tspecularmap textures/lp42a/metal_s.tga\n}\n"
  "textures/lp42a/twogroup\n{\n\tbumpmap _flat\n\tdiffusemap textures/lp42a/flat.tga\n"
  "\tbumpmap _flat\n\t{\n\t\tblend diffusemap\n\t\tmap textures/lp42a/second.tga\n"
  "\t\tvertexColor\n\t}\n}\n"};

// the fixture map with three slabs on the floor of area 0 (top at z 2, under cameras at
// z 80): the flat one at (60 0), the metal one at (60 80), the two-group one at (60 -80)
std::string lp42aMapText()
{
  auto map = lp22aMapText();
  const auto entity1 = map.find("// entity 1");
  const auto close = map.rfind('}', entity1);
  map.insert(
    close,
    boxBrush(52, -8, 0, 68, 8, 2, "lp42a/flat")
      + boxBrush(52, 72, 0, 68, 88, 2, "lp42a/metal")
      + boxBrush(52, -88, 0, 68, -72, 2, "lp42a/twogroup"));
  return map;
}

// what one settled frame left after its tonemap passes: its stats, the exposure buffer,
// the HDR and the LDR targets
struct AmbientReadback
{
  ChainFrame frame;
  float exposure = 0.0f;
  HdrImage hdr;
  std::vector<std::uint8_t> ldr;
};

std::optional<AmbientReadback> ambientReadback(
  LitSession& session, const std::chrono::milliseconds timeout = 10000ms)
{
  struct State
  {
    bool copied = false;
    ChainFrame frame;
    nvrhi::BufferHandle exposure;
    nvrhi::StagingTextureHandle hdr;
    nvrhi::StagingTextureHandle ldr;
  };
  auto state = std::make_shared<State>();
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  session.runAfterTonemapForTesting([state, done](RenderFrameAccess& access) {
    state->frame = chainFrameOf(access);
    if (state->frame.tonemap.tonemapped)
    {
      namespace fr = frame_resource;
      auto bufferDesc = nvrhi::BufferDesc{};
      bufferDesc.byteSize = 4;
      bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
      bufferDesc.debugName = "LP4.2a exposure readback";
      bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
      bufferDesc.keepInitialState = true;
      state->exposure = access.device.createBuffer(bufferDesc);
      access.commandList.copyBuffer(
        state->exposure, 0, access.frame.find(fr::ExposureBuffer)->buffer, 0, 4);
      // std::addressof: RefCountPtr's operator& is the raw pointer's address (nvrhi
      // rule 1)
      for (const auto& [name, staging] :
           {std::pair{fr::CurrentRenderHdr, std::addressof(state->hdr)},
            std::pair{fr::CurrentRenderLdr, std::addressof(state->ldr)}})
      {
        auto* texture = access.frame.find(name)->texture;
        const auto& desc = texture->getDesc();
        *staging = access.device.createStagingTexture(
          nvrhi::TextureDesc{}
            .setWidth(desc.width)
            .setHeight(desc.height)
            .setFormat(desc.format),
          nvrhi::CpuAccessMode::Read);
        access.commandList.copyTexture(
          *staging, nvrhi::TextureSlice{}, texture, nvrhi::TextureSlice{});
      }
      state->copied = true;
    }
    done->set_value();
  });
  if (future.wait_for(timeout) != std::future_status::ready || !state->copied)
  {
    return std::nullopt;
  }
  auto result = AmbientReadback{};
  result.frame = state->frame;
  const auto read = inFrame(
    session,
    [&](RenderFrameAccess& access) {
      if (
        const auto* bits = static_cast<const std::uint32_t*>(
          access.device.mapBuffer(state->exposure, nvrhi::CpuAccessMode::Read)))
      {
        std::memcpy(&result.exposure, bits, sizeof(float));
        access.device.unmapBuffer(state->exposure);
      }
      {
        const auto& desc = state->hdr->getDesc();
        result.hdr.width = desc.width;
        result.hdr.height = desc.height;
        result.hdr.rgba.resize(std::size_t(desc.width) * desc.height * 4);
        auto pitch = std::size_t{0};
        const auto* bytes =
          static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
            state->hdr, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
        for (std::uint32_t y = 0; bytes && y < desc.height; ++y)
        {
          const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
          for (std::uint32_t x = 0; x < desc.width * 4; ++x)
          {
            result.hdr.rgba[std::size_t(y) * desc.width * 4 + x] = halfToFloat(row[x]);
          }
        }
        if (bytes)
        {
          access.device.unmapStagingTexture(state->hdr);
        }
      }
      {
        const auto& desc = state->ldr->getDesc();
        result.ldr.resize(std::size_t(desc.width) * desc.height * 4);
        auto pitch = std::size_t{0};
        const auto* bytes =
          static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
            state->ldr, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
        for (std::uint32_t y = 0; bytes && y < desc.height; ++y)
        {
          std::memcpy(
            result.ldr.data() + std::size_t(y) * desc.width * 4,
            bytes + y * pitch,
            std::size_t(desc.width) * 4);
        }
        if (bytes)
        {
          access.device.unmapStagingTexture(state->ldr);
        }
      }
      state->exposure = nullptr;
      state->hdr = nullptr;
      state->ldr = nullptr;
    },
    timeout);
  if (!read)
  {
    return std::nullopt;
  }
  return result;
}

// ----- LP4.2a: the CPU model of ambient_lighting_IBL.ps.hlsl for one pixel (SC331) ----

// global_inc.hlsl:177-185 Linear1, :205-214 sRGBToLinearRGB (clamped first)
float modelLinear1(const float c)
{
  const auto v = std::clamp(c, 0.0f, 1.0f);
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// global_inc.hlsl:344-361 ConvertYCoCgToRGB of a stored texel (Co, Cg, scale, Y) as the
// UNORM8 sample returns it, then sRGBToLinearRGB: the shader's diffuseMap (ALP:231-235)
std::array<float, 3> modelDiffuseMap(const std::array<std::uint8_t, 4>& texel)
{
  auto x = float(texel[0]) / 255.0f;
  auto y = float(texel[1]) / 255.0f;
  auto z = float(texel[2]) / 255.0f;
  const auto w = float(texel[3]) / 255.0f;
  z = z * 31.875f + 1.0f;
  z = 1.0f / z;
  x *= z;
  y *= z;
  const auto r = x * 1.0f + y * -1.0f + z * 0.0f + w * 1.0f;
  const auto g = x * 0.0f + y * 1.0f + z * -0.50196078f + w * 1.0f;
  const auto b = x * -1.0f + y * -1.0f + z * 1.00392156f + w * 1.0f;
  return {modelLinear1(r), modelLinear1(g), modelLinear1(b)};
}

// t_BrdfLut.Sample( s_LinearClamp, ( u, v ) ).rg over the copied LUT's halves (texel
// centres at ( i + 0.5 ) / 256, clamped at the edges)
std::array<float, 2> modelEnvBrdf(const float u, const float v)
{
  const auto* bytes = engineBrdfLutData();
  const auto size = engineBrdfLutWidth();
  const auto texel = [&](const int x, const int y) {
    const auto at = (std::size_t(std::clamp(y, 0, size - 1)) * std::size_t(size)
                     + std::size_t(std::clamp(x, 0, size - 1)))
                    * 4;
    return std::array<float, 2>{
      engineF16toF32(std::uint16_t(bytes[at] | (bytes[at + 1] << 8))),
      engineF16toF32(std::uint16_t(bytes[at + 2] | (bytes[at + 3] << 8)))};
  };
  const auto fx = u * float(size) - 0.5f;
  const auto fy = v * float(size) - 0.5f;
  const auto x0 = int(std::floor(fx));
  const auto y0 = int(std::floor(fy));
  const auto tx = fx - float(x0);
  const auto ty = fy - float(y0);
  auto result = std::array<float, 2>{};
  for (std::size_t c = 0; c < 2; ++c)
  {
    const auto top = texel(x0, y0)[c] * (1.0f - tx) + texel(x0 + 1, y0)[c] * tx;
    const auto bottom =
      texel(x0, y0 + 1)[c] * (1.0f - tx) + texel(x0 + 1, y0 + 1)[c] * tx;
    result[c] = top * (1.0f - ty) + bottom * ty;
  }
  return result;
}

// ALP:386-394: specularScatter of one channel's specularColor (the Fdez-Aguera
// multiple-scattering compensation)
float modelSpecularScatter(const float specularColor, const std::array<float, 2>& brdf)
{
  const auto singleScatterSpec = specularColor * brdf[0] + brdf[1];
  const auto singleScatterEnergy = brdf[0] + brdf[1];
  const auto multiScatterEnergy = 1.0f - singleScatterEnergy;
  const auto averageFresnel = specularColor + (1.0f - specularColor) * (1.0f / 21.0f);
  const auto multiScatterSpec =
    singleScatterSpec * averageFresnel / (1.0f - multiScatterEnergy * averageFresnel);
  return singleScatterSpec + multiScatterSpec * multiScatterEnergy;
}

// ALP:184-214, 381: shEvaluateDiffuseL2RGB( sh, n ) / pi of an L0-only SH: the first
// coefficient times Y00 = 1 / ( 2 sqrt( pi ) ) times A0 = pi, over pi
constexpr auto ModelY00 = 0.28209479f;

// the first texel of an image's first level, as the service holds it
std::optional<std::array<std::uint8_t, 4>> firstTexel(
  MaterialService& service, const MaterialImageKey& key)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline)
  {
    const auto lookup = service.image(key);
    if (lookup.state != ImageLookup::State::Pending)
    {
      if (!lookup.image || lookup.image->levels.empty())
      {
        return std::nullopt;
      }
      const auto* t = lookup.image->data.data() + lookup.image->levels[0].offset;
      return std::array<std::uint8_t, 4>{t[0], t[1], t[2], t[3]};
    }
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

// the image a material's stage of the given lighting binds, from the table
std::optional<MaterialImageKey> stageImage(
  MaterialService& service, const std::string& material, const int lighting, int nth = 0)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (const auto slot = service.table()->find(material); slot && slot->entry)
    {
      for (const auto& stage : slot->entry->stages)
      {
        if (stage.lighting == lighting && stage.image && nth-- == 0)
        {
          return stage.image;
        }
      }
      return std::nullopt;
    }
    service.lookup(material);
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

// the model's HDR value through the RGBA16F target and the tonemap passes (LP2.2b's
// TonemapModel), as the LDR target's bytes
std::array<int, 3> modelLdr(
  const std::array<float, 3>& hdr, const float exposure, const ChainFrame& frame)
{
  const auto stored =
    std::array<float, 3>{modelHalf(hdr[0]), modelHalf(hdr[1]), modelHalf(hdr[2])};
  const auto ldr = modelTonemap(stored, exposure, frame.tonemap.tonemapConstants);
  return {int(modelUnorm8(ldr[0])), int(modelUnorm8(ldr[1])), int(modelUnorm8(ldr[2]))};
}

} // namespace

// LP4.2a (C34, C37, D85-D88): the ambient pass on the probe path over the LP2.2a fixture,
// with the IBL blobs and the env_probe entityDef of the game folder (TB_LIT_GAME_PATH,
// read only), first without a bake (black: the default cube, zero SH), then with a
// synthetic .shprobes and radiance cubes against a CPU model of the shader for three
// pixels. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession ambient pass on a GPU (LP4.2a)", "[.][litgpu]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pass needs the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);
  const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});

  SECTION("no bake: every probe defaulted, the pass black over the prepass's black")
  {
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-lp42a-frame-log.csv";
    std::filesystem::remove(logPath);
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = root;
      options.frameLogPath = logPath;
      auto session = LitSession::create(
                       instance.handles(),
                       VK_NULL_HANDLE,
                       160,
                       120,
                       options,
                       events.sink(),
                       7,
                       service.get())
                     | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setDrawLogForTesting(true);
      }));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));

      const auto litFrame = settledHdr(*session, lit, 1);
      REQUIRE(litFrame);
      const auto& [litImage, stats] = *litFrame;
      CHECK_FALSE(stats.stats.ambientDisabled);
      CHECK(stats.stats.ambientDraws > 0u);
      CHECK(stats.stats.ambientOverflowed == 0u);
      CHECK(stats.stats.ambientSkipped == 0u);
      CHECK(stats.stats.ambientGridSurfaces == 0u);
      CHECK(stats.stats.ambientTimed);
      // the fixture has no env_probe entity and the game's env_probe def is found: an
      // automatic probe per area, both connected to the view's area 0 through the portal
      CHECK(stats.stats.probeCandidates == 2u);
      CHECK_FALSE(stats.stats.probeImagesPending);
      // LP2.2a's lit floor probe is the interactions' alone: the pass wrote black
      const auto centre = litImage.at(80, 60);
      CHECK(centre[0] == Catch::Approx(0.0309f).margin(0.001f));
      CHECK(centre[1] == Catch::Approx(1.4531f).margin(0.001f));
      CHECK(centre[2] == 0.0f);
      CHECK(centre[3] == 1.0f);

      // the probes and their slots: _spec and _depth asked for, Default (no bake), _amb
      // never; the view's choice the defaults
      const auto probes = session->probeTable();
      REQUIRE(probes->records.size() == 2u);
      CHECK(probes->envProbeDef == EnvProbeDefState::Found);
      CHECK_FALSE(probes->provisional);
      CHECK(
        probes->records[0].images[1].name
        == "env/maps/lp1b_geometry/area0_envprobe_132_0_64_spec");
      CHECK(
        probes->records[1].images[1].name
        == "env/maps/lp1b_geometry/area1_envprobe__124_0_64_spec");
      auto states = std::vector<std::optional<ImageSlots::State>>{};
      auto ambSlots = 0;
      auto view = ViewProbes{};
      auto prepassChunks = std::multiset<ChunkKey>{};
      auto ambientChunks = std::multiset<ChunkKey>{};
      auto ambientMaterials = std::set<std::string>{};
      auto ambientStates = std::set<std::uint64_t>{};
      auto ambientPrograms = std::set<int>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto& probe : probes->records)
        {
          for (const auto index : {1, 2})
          {
            const auto id = access.images.find(probe.images[std::size_t(index)]);
            states.push_back(
              id ? std::optional{access.images.slot(*id)->state} : std::nullopt);
          }
          ambSlots += access.images.find(probe.images[0]) ? 1 : 0;
        }
        view = access.passes.viewProbes();
        // the last frame's draws: the prepass's and the ambient pass's
        const auto& log = access.passes.drawLogForTesting();
        auto lastPrepass = std::size_t{0};
        for (std::size_t i = 0; i < log.size(); ++i)
        {
          if (
            log[i].pass == LitDrawRecord::Pass::Prepass
            && (i == 0 || log[i - 1].pass != LitDrawRecord::Pass::Prepass))
          {
            lastPrepass = i;
          }
        }
        for (std::size_t i = lastPrepass; i < log.size(); ++i)
        {
          if (log[i].pass == LitDrawRecord::Pass::Prepass)
          {
            prepassChunks.insert(log[i].chunk);
          }
          else if (log[i].pass == LitDrawRecord::Pass::Ambient)
          {
            ambientChunks.insert(log[i].chunk);
            ambientMaterials.insert(log[i].chunk.material);
            ambientStates.insert(log[i].state);
            ambientPrograms.insert(log[i].program);
          }
        }
      }));
      REQUIRE(states.size() == 4u);
      for (const auto& state : states)
      {
        CHECK(state == ImageSlots::State::Default);
      }
      CHECK(ambSlots == 0);
      CHECK(engineBoundsIsCleared(view.globalProbeBounds));
      CHECK_FALSE(view.radiance[0]);
      CHECK_FALSE(view.depth);
      CHECK(view.sh[0] == std::array<float, 4>{0, 0, 0, 0});
      CHECK(view.candidates == 2u);
      // P4.20: one draw per prepass surface, every one of a single bump group; the
      // translucent glass and the unlit glass have none
      CHECK(ambientChunks == prepassChunks);
      CHECK(ambientChunks.size() == stats.stats.ambientDraws);
      CHECK_FALSE(ambientMaterials.contains("textures/lp2/litglass"));
      CHECK_FALSE(ambientMaterials.contains("textures/lp1b/glass"));
      CHECK(ambientStates == std::set<std::uint64_t>{ambientStateBits(0)});
      CHECK(ambientPrograms == std::set<int>{EngineBuiltinAmbientLightingIbl});

      // the interactions skipped (r_skipInteractions 1's stand-in): every pixel black,
      // the alpha 1, the ambient pass drawn
      // (a camera of its own: a frame that began before the switch cannot be taken)
      session->skipInteractionsForTesting(true);
      const auto dark =
        settledHdr(*session, litCamera({60, 0, 79}, {0, 0, -1}, {1, 0, 0}), 0);
      REQUIRE(dark);
      CHECK(dark->second.stats.interactionsSkipped);
      CHECK(dark->second.stats.interactionDraws == 0u);
      CHECK(dark->second.stats.ambientDraws == stats.stats.ambientDraws);
      auto nonBlack = 0;
      auto halfBelowOne = 0;
      auto otherAlpha = 0;
      for (std::size_t i = 0; i < dark->first.rgba.size(); i += 4)
      {
        nonBlack += dark->first.rgba[i] != 0.0f || dark->first.rgba[i + 1] != 0.0f
                        || dark->first.rgba[i + 2] != 0.0f
                      ? 1
                      : 0;
        // the vertex alpha 1, interpolated: a pixel whose interpolant lands below 1 is
        // stored as the half below it (0.99951, the RGBA16F target's conversion)
        const auto alpha = dark->first.rgba[i + 3];
        halfBelowOne += alpha == 0.99951171875f ? 1 : 0;
        otherAlpha += alpha != 1.0f && alpha != 0.99951171875f ? 1 : 0;
      }
      CHECK(nonBlack == 0);
      CHECK(otherAlpha == 0);
      CHECK(dark->first.at(80, 60)[3] == 1.0f);
      session->skipInteractionsForTesting(false);
      std::cout << fmt::format(
        "LP4.2a no bake: {} ambient draws ({} prepass draws), {} probe candidates; the "
        "lit "
        "floor ({:.4f} {:.4f} {:.4f}); with the interactions skipped {} non-black "
        "pixels, "
        "{} of alpha 0.99951\n",
        stats.stats.ambientDraws,
        stats.stats.prepassDraws,
        stats.stats.probeCandidates,
        centre[0],
        centre[1],
        centre[2],
        nonBlack,
        halfBelowOne);

      teardown(*session);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      CHECK(events.count(SessionEvent::Warning) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("LP4.2a no bake, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);

    // C16: the frame log's two columns: a frame that drew the pass has its time
    const auto rows = readCsv(logPath);
    REQUIRE(rows.size() >= 2u);
    REQUIRE(rows[0].size() == 49u);
    CHECK(rows[0][42] == "ambient_draws");
    CHECK(rows[0][43] == "gpu_ambient_ms");
    auto timed = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      if (
        rows[i].size() == 49u && rows[i][1] == "frame" && !rows[i][42].empty()
        && rows[i][42] != "0" && !rows[i][43].empty() && std::stod(rows[i][43]) > 0.0)
      {
        ++timed;
      }
    }
    CHECK(timed > 0);
    std::filesystem::remove(logPath);
  }

  SECTION("a synthetic bake: the probe path's pixels against the shader's arithmetic")
  {
    const auto base = root / "base";
    // C37: one .shprobes entry per automatic probe, an L0 SH whose irradiance is E = 1/4
    constexpr auto Irradiance = 0.25f;
    const auto c0 = Irradiance / ModelY00;
    const auto sh = fmt::format(
      "( {0:.9g} {0:.9g} {0:.9g} 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 )", c0);
    writeTextFile(
      base / "maps" / "lp1b_geometry.shprobes",
      fmt::format("SHPROBES \"1\"\n2\n0 ( 132 0 64 ) {0}\n1 ( -124 0 64 ) {0}\n", sh));
    // each probe's radiance cube a constant colour; no _depth cache (defaulted: the
    // bounds cleared, no march)
    constexpr auto Radiance = std::array<float, 3>{0.5f, 0.25f, 0.125f};
    const auto cubes = base / "generated" / "images" / "env" / "maps" / "lp1b_geometry";
    std::filesystem::create_directories(cubes);
    for (const auto* name :
         {"area0_envprobe_132_0_64_spec#__2108.bimage",
          "area1_envprobe__124_0_64_spec#__2108.bimage"})
    {
      auto file = std::ofstream{cubes / name, std::ios::binary};
      const auto bytes = constantCubeCache(Radiance[0], Radiance[1], Radiance[2]);
      file.write(bytes.data(), std::streamsize(bytes.size()));
    }
    // the materials and their flat images
    writeTextFile(base / "materials" / "zz_lp42a.mtr", Lp42aMaterials);
    std::filesystem::create_directories(base / "textures" / "lp42a");
    const auto writeBinary =
      [](const std::filesystem::path& path, const std::string& bytes) {
        auto file = std::ofstream{path, std::ios::binary};
        file.write(bytes.data(), std::streamsize(bytes.size()));
      };
    writeBinary(
      base / "textures" / "lp42a" / "flat.tga",
      test::tgaFile(test::flat(4, 4, 200, 120, 60)));
    writeBinary(
      base / "textures" / "lp42a" / "second.tga",
      test::tgaFile(test::flat(4, 4, 40, 90, 180)));
    writeBinary(
      base / "textures" / "lp42a" / "metal.tga",
      test::tgaFile(test::flat(4, 4, 230, 180, 100)));
    writeBinary(
      base / "textures" / "lp42a" / "metal_s.tga",
      test::tgaFile(test::flat(4, 4, 128, 255, 255)));
    writeBinary(
      base / "textures" / "lp42a" / "metal_rmao.png",
      test::pngFile(test::flat(4, 4, 128, 255, 255)));

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
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp42aMapText(), root, 7, 1)));
      // the ambient-only shots of LP4.2b's rows: the interactions not drawn
      session->skipInteractionsForTesting(true);

      struct Shot
      {
        const char* name;
        CameraState camera;
      };
      const auto shots = std::array<Shot, 3>{{
        {"flat", litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0})},
        {"metal", litCamera({60, 80, 80}, {0, 0, -1}, {1, 0, 0})},
        {"twogroup", litCamera({60, -80, 80}, {0, 0, -1}, {1, 0, 0})},
      }};
      auto readbacks = std::vector<AmbientReadback>{};
      auto views = std::vector<ViewProbes>{};
      for (const auto& shot : shots)
      {
        CAPTURE(shot.name);
        // the readback's frame is the one after a settled frame: taken again until it is
        // settled itself (a late table or arrival unsettles one now and then)
        auto readback = std::optional<AmbientReadback>{};
        for (auto attempt = 0; attempt < 20
                               && !(
                                 readback && readback->frame.settled
                                 && readback->frame.lit.camera == shot.camera);
             ++attempt)
        {
          REQUIRE(settledHdr(*session, shot.camera, 0));
          readback = ambientReadback(*session);
        }
        REQUIRE(readback);
        CHECK(readback->frame.settled);
        CHECK(readback->frame.lit.camera == shot.camera);
        CHECK(readback->frame.lit.ambientDraws > 0u);
        CHECK(readback->frame.lit.interactionDraws == 0u);
        auto view = ViewProbes{};
        REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
          view = access.passes.viewProbes();
        }));
        views.push_back(view);
        readbacks.push_back(std::move(*readback));
      }

      // the probes found their SH and radiance cubes; no depth cube: the bounds cleared
      const auto probes = session->probeTable();
      REQUIRE(probes->records.size() == 2u);
      CHECK(probes->records[0].shFound);
      CHECK(probes->records[1].shFound);
      for (const auto& view : views)
      {
        CHECK(view.radiance[0]);
        CHECK(view.radiance[1]);
        CHECK_FALSE(view.radiance[2]);
        CHECK_FALSE(view.depth);
        CHECK(engineBoundsIsCleared(view.globalProbeBounds));
        CHECK(view.sh[0][0] == Catch::Approx(c0).epsilon(1e-5));
        CHECK(view.blends[0] + view.blends[1] == Catch::Approx(1.0f).epsilon(1e-5));
      }

      // the model's inputs: the images as the service holds them
      const auto flatKey = stageImage(*service, "textures/lp42a/flat", 2);
      const auto secondKey = stageImage(*service, "textures/lp42a/twogroup", 2, 1);
      const auto metalKey = stageImage(*service, "textures/lp42a/metal", 2);
      REQUIRE(flatKey);
      REQUIRE(secondKey);
      REQUIRE(metalKey);
      const auto flatTexel = firstTexel(*service, *flatKey);
      const auto secondTexel = firstTexel(*service, *secondKey);
      const auto metalTexel = firstTexel(*service, *metalKey);
      REQUIRE(flatTexel);
      REQUIRE(secondTexel);
      REQUIRE(metalTexel);
      const auto flatAlbedo = modelDiffuseMap(*flatTexel);
      const auto secondAlbedo = modelDiffuseMap(*secondTexel);
      const auto metalBase = modelDiffuseMap(*metalTexel);
      // the irradiance as the shader evaluates it (ALP:381): c0 · Y00 · A0 / pi
      const auto irradiance =
        c0 * (1.0f / (2.0f * std::sqrt(3.1415927f))) * 3.1415927f / 3.14159265f;

      // the legacy diffuse (ALP:344-396): no specular stage, so _black: roughness 1
      // (EstimateLegacyRoughness of 0), specularColor 0; vDotN within 1e-4 of 1 at the
      // centre pixel, so the LUT's last column; diffuse colour clamp01( 1 ) · 3; ao 1
      // (_white, K31); AMBIENT_COLOR 0.5; the specular modifier 0 (no stage)
      const auto legacyBrdf = modelEnvBrdf(0.99995f, 1.0f);
      const auto legacy = [&](const std::array<float, 3>& albedo) {
        auto out = std::array<float, 3>{};
        for (std::size_t c = 0; c < 3; ++c)
        {
          const auto scatter = modelSpecularScatter(0.0f, legacyBrdf);
          out[c] = (1.0f - scatter) * irradiance * albedo[c] * 1.0f * 3.0f
                   * EngineFrame.forceAmbient;
        }
        return out;
      };
      // the metal (ALP:306-331, 459-474): metallic 1 (no diffuse, specVisibility 1),
      // roughness 128/255, ao 1, specularColor the base colour; the radiance cube's
      // colour (one level: every mip lod reads it) times the blends' sum (1); specAO
      // clamp( pow( vDotN + 1, exp2( -16 r - 1 ) ) - 1 + 1 ) = 1; the specular modifier
      // clamp01( 1 ) · 3; horizon fade 1 at vDotN 1
      const auto metalBrdf = modelEnvBrdf(0.99995f, 128.0f / 255.0f);
      auto metal = std::array<float, 3>{};
      for (std::size_t c = 0; c < 3; ++c)
      {
        metal[c] = Radiance[c] * modelSpecularScatter(metalBase[c], metalBrdf) * 1.0f
                   * 1.0f * 3.0f;
      }
      const auto models = std::array<std::array<float, 3>, 3>{
        legacy(flatAlbedo),
        metal,
        // the two groups: the first overwrites (ONE / ZERO), the second adds (ONE / ONE)
        [&] {
          const auto first = legacy(flatAlbedo);
          const auto second = legacy(secondAlbedo);
          return std::array<float, 3>{
            first[0] + second[0], first[1] + second[1], first[2] + second[2]};
        }(),
      };
      const auto lastGroupOnly = legacy(secondAlbedo);

      for (std::size_t s = 0; s < shots.size(); ++s)
      {
        CAPTURE(shots[s].name);
        const auto& readback = readbacks[s];
        const auto hdr = readback.hdr.at(80, 60);
        const auto at = (std::size_t(60) * readback.hdr.width + 80) * 4;
        const auto modelHdr = models[s];
        const auto expected = modelLdr(modelHdr, readback.exposure, readback.frame);
        // the exposure the GPU computed, against the CPU model's of the same HDR target
        const auto histogram = modelHistogramOfImage(
          readback.hdr.rgba,
          readback.hdr.width,
          readback.hdr.height,
          readback.frame.tonemap.histogramConstants);
        const auto cpuExposure = modelExposure(
          histogram, readback.exposure, readback.frame.tonemap.exposureConstants);
        CHECK(std::abs(cpuExposure - readback.exposure) <= 1e-5f * readback.exposure);
        for (std::size_t c = 0; c < 3; ++c)
        {
          CAPTURE(c);
          CHECK(hdr[c] == Catch::Approx(modelHdr[c]).epsilon(0.01));
          CHECK(std::abs(int(readback.ldr[at + c]) - expected[c]) <= 1);
        }
        CHECK(hdr[3] == 1.0f);
        std::cout << fmt::format(
          "LP4.2a probe path {}: HDR model ({:.5f} {:.5f} {:.5f}) read-back ({:.5f} "
          "{:.5f} "
          "{:.5f}); LDR model ({} {} {}) read-back ({} {} {}) diff ({} {} {}); exposure "
          "{:.6f} (CPU {:.6f})\n",
          shots[s].name,
          modelHdr[0],
          modelHdr[1],
          modelHdr[2],
          hdr[0],
          hdr[1],
          hdr[2],
          expected[0],
          expected[1],
          expected[2],
          int(readback.ldr[at]),
          int(readback.ldr[at + 1]),
          int(readback.ldr[at + 2]),
          int(readback.ldr[at]) - expected[0],
          int(readback.ldr[at + 1]) - expected[1],
          int(readback.ldr[at + 2]) - expected[2],
          readback.exposure,
          cpuExposure);
      }
      // A04: the two groups' pixel is the sum, not the last group's alone
      const auto twoGroup = readbacks[2].hdr.at(80, 60);
      CHECK(std::abs(twoGroup[0] - lastGroupOnly[0]) > 0.1f);
      std::cout << fmt::format(
        "LP4.2a the two groups: the last group alone ({:.5f} {:.5f} {:.5f}), the sum "
        "read "
        "back ({:.5f} {:.5f} {:.5f}); irradiance {:.6f} from c0 {:.6f}\n",
        lastGroupOnly[0],
        lastGroupOnly[1],
        lastGroupOnly[2],
        twoGroup[0],
        twoGroup[1],
        twoGroup[2],
        irradiance,
        c0);

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
    messages.print("LP4.2a synthetic bake, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("D86: set 9's own buffer overflows at maxVersions 3, said once")
  {
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
      // through the doorway at the east room: the world and the crate's space, two
      // writes of set 9 where the frame's budget is one
      const auto glass = litCamera({-100, 0, 32}, {1, 0, 0}, {0, 0, 1});
      REQUIRE(settledHdr(*session, glass, 1));
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setConstantBufferVersionsForTesting(3);
      }));
      const auto overflowText = std::string{"draws overflowed its constant buffer"};
      auto overflowed = LitFrameStats{};
      CHECK(waitFor(
        [&] {
          session->setCamera(litCamera({-100, 0, 33}, {1, 0, 0}, {0, 0, 1}));
          inFrame(*session, [&](RenderFrameAccess& access) {
            if (access.passes.lastStats().ambientOverflowed > 0)
            {
              overflowed = access.passes.lastStats();
            }
          });
          return overflowed.ambientOverflowed > 0;
        },
        20000ms,
        false));
      CHECK(overflowed.ambientOverflowed > 0u);
      CHECK(overflowed.unconverged);
      // two more frames: the warning is said once
      session->setCamera(litCamera({-100, 0, 34}, {1, 0, 0}, {0, 0, 1}));
      REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      session->setCamera(litCamera({-100, 0, 35}, {1, 0, 0}, {0, 0, 1}));
      REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      CHECK(events.countContaining(SessionEvent::Warning, overflowText) == 1u);
      std::cout << fmt::format(
        "LP4.2a D86 at maxVersions 3: {} ambient draws, {} overflowed, {} overflowed "
        "draws "
        "in all\n",
        overflowed.ambientDraws,
        overflowed.ambientOverflowed,
        overflowed.overflowedDraws);
      teardown(*session);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->lastDeviceReleaseCount() == 0u);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("LP4.2a overflow, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("200 automatic probes on a 200-area tree: 400 slots, all Default, settled")
  {
    // a chain of 199 planes x = 64 (i + 1): area i is x in ( 64 i, 64 ( i + 1 ) ], each
    // area model one triangle inside it, no portal
    constexpr auto Areas = 200;
    auto proc = std::string{"mapProcFile004\n"};
    for (auto i = 0; i < Areas; ++i)
    {
      const auto x = 64 * i;
      proc += fmt::format(
        "model {{ \"_area{}\" 1\n{{ \"textures/lp1b/floor\" 3 3 0\n( {} 0 8 0 0 0 0 1 ) "
        "( {} 0 8 0 0 0 0 1 ) ( {} 16 8 0 0 0 0 1 )\n0 1 2 }}\n}}\n",
        i,
        x + 16,
        x + 48,
        x + 16);
    }
    proc += fmt::format("interAreaPortals {{ {} 0 }}\nnodes {{ {}\n", Areas, Areas - 1);
    for (auto i = 0; i < Areas - 1; ++i)
    {
      // d = x - 64 ( i + 1 ) > 0: the next node (or the last area); else area i
      const auto front = i + 1 < Areas - 1 ? i + 1 : -1 - (Areas - 1);
      proc += fmt::format("( 1 0 0 {} ) {} {}\n", -64 * (i + 1), front, -1 - i);
    }
    proc += "}\n";
    writeTextFile(root / "base" / "maps" / "lp42a_areas.proc", proc);
    const auto mapText = std::string{"{\n\"classname\" \"worldspawn\"\n"}
                         + boxBrush(16, -16, 0, 48, 16, 8, "lp1b/floor") + "}\n";
    writeTextFile(root / "base" / "maps" / "lp42a_areas.map", mapText);

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
      auto snapshot = snapshotOfMapText(mapText);
      snapshot.generation = 7;
      snapshot.sequence = 1;
      snapshot.mapPath = (root / "base" / "maps" / "lp42a_areas.map").string();
      REQUIRE(
        session->offerSnapshot(std::make_shared<const Snapshot>(std::move(snapshot))));
      const auto camera = litCamera({32, 0, 40}, {0, 0, -1}, {1, 0, 0});
      REQUIRE(waitFor(
        [&] { return session->probeTable()->records.size() == std::size_t(Areas); },
        30000ms,
        false));
      const auto settled = settledHdr(*session, camera, 0);
      REQUIRE(settled);
      CHECK(settled->second.stats.probeCandidates == 1u);
      CHECK_FALSE(settled->second.stats.probeImagesPending);
      auto requested = 0;
      auto defaulted = 0;
      auto withTexture = 0;
      const auto probes = session->probeTable();
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto& probe : probes->records)
        {
          for (const auto index : {1, 2})
          {
            if (const auto id = access.images.find(probe.images[std::size_t(index)]))
            {
              ++requested;
              const auto* slot = access.images.slot(*id);
              defaulted += slot->state == ImageSlots::State::Default ? 1 : 0;
              withTexture += slot->texture ? 1 : 0;
            }
          }
        }
      }));
      CHECK(requested == 2 * Areas);
      CHECK(defaulted == 2 * Areas);
      CHECK(withTexture == 0);
      CHECK(session->lastFrameSettledForTesting());
      std::cout << fmt::format(
        "LP4.2a 200 areas: {} probes, {} slots asked for, {} Default, {} textures\n",
        probes->records.size(),
        requested,
        defaulted,
        withTexture);
      teardown(*session);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(session->deviceMemoryAllocations() == 0);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("LP4.2a 200 probes, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  // LP4.2b step 0 (the director's decision after LP4.2a): a probe build in flight holds
  // "settled". With the builds held, every other term of the fixture's frames converges
  // and none is settled; released, the build lands and a settled frame comes by itself
  // (V2.4's shape: no frame asked for); no frame was settled before the probe table had a
  // version
  SECTION("LP4.2b: no frame is settled before the probe table has a version")
  {
    struct Hold
    {
      Hold() { LitSession::holdProbeBuildsForTesting(true); }
      ~Hold() { LitSession::holdProbeBuildsForTesting(false); }

      deleteCopyAndMove(Hold);
    };
    // per frame: the probe table's version it drew with and, from the next frame's
    // hook, whether it ended settled
    struct FrameSeen
    {
      std::uint64_t version = 0;
      bool known = false;
      bool settled = false;
    };
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto frames = std::make_shared<std::vector<FrameSeen>>();
    auto framesMutex = std::make_shared<std::mutex>();
    {
      auto hold = std::optional<Hold>{std::in_place};
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      auto* const owner = session.get();
      session->setEveryFrameHookForTesting(
        [frames, framesMutex, owner](RenderFrameAccess& access) {
          // the hooks other than runAfterTonemap's see the last frame's convergence
          const auto lock = std::scoped_lock{*framesMutex};
          if (!frames->empty())
          {
            frames->back().known = true;
            frames->back().settled = access.convergence.settled();
          }
          frames->push_back(FrameSeen{owner->probeTable()->version, false, false});
        });
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));
      session->setCamera(lit);

      // held: the batch applies, the bake and the entityDefs are read, the images
      // arrive; every term but the probes' converges, and no frame is settled
      auto held = FrameConvergence{};
      CHECK(waitFor(
        [&] {
          inFrame(
            *session, [&](RenderFrameAccess& access) { held = access.convergence; });
          auto others = held;
          others.probesInFlight = false;
          return session->geometryBatchesForTesting() >= 1u && others.settled();
        },
        30000ms,
        false));
      CHECK(held.probesInFlight);
      CHECK_FALSE(held.geometryInFlight);
      CHECK_FALSE(held.settled());
      CHECK_FALSE(session->lastFrameSettledForTesting());
      CHECK(session->probeTable()->version == 0u);

      // released: the next geometry poll starts the build, its collection triggers the
      // frames that settle
      const auto framesAtRelease = session->framesPresented();
      hold.reset();
      CHECK(waitFor(
        [&] {
          return session->framesPresented() > framesAtRelease
                 && session->lastFrameSettledForTesting();
        },
        30000ms,
        false));
      const auto probes = session->probeTable();
      CHECK(probes->version > 0u);
      CHECK_FALSE(probes->provisional);
      CHECK(probes->records.size() == 2u);
      // one frame more, whose hook records how the settled one ended
      REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      session->setEveryFrameHookForTesting({});

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
    {
      const auto lock = std::scoped_lock{*framesMutex};
      auto settledFrames = std::size_t{0};
      auto heldFrames = std::size_t{0};
      for (const auto& frame : *frames)
      {
        heldFrames += frame.version == 0u ? 1u : 0u;
        if (frame.known && frame.settled)
        {
          ++settledFrames;
          CHECK(frame.version > 0u);
        }
      }
      CHECK(settledFrames > 0u);
      std::cout << fmt::format(
        "LP4.2b step 0: {} frames before the probe table had a version, none settled; "
        "{} settled frames after\n",
        heldFrames,
        settledFrames);
    }
    messages.print("LP4.2b probes held, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("a Stop between the prepass and the ambient pass")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto stopped = std::make_shared<LitFrameStats>();
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
      REQUIRE(settledHdr(*session, lit, 1));

      // the next frame is held after its prepass until the stop is asked for: the
      // ambient pass's boundary sees it
      auto held = std::make_shared<std::promise<void>>();
      auto heldFuture = held->get_future();
      auto release = std::make_shared<std::promise<void>>();
      auto releaseFuture = release->get_future().share();
      auto once = std::make_shared<std::atomic<bool>>(false);
      auto* const owner = session.get();
      session->debugSetHook(
        DebugHook::AfterPrepass, [held, releaseFuture, once, owner, stopped] {
          if (!once->exchange(true))
          {
            owner->runAfterPassesForTesting([stopped](RenderFrameAccess& access) {
              *stopped = access.passes.lastStats();
            });
            held->set_value();
            releaseFuture.wait();
          }
        });
      session->setCamera(litCamera({60, 0, 81}, {0, 0, -1}, {1, 0, 0}));
      REQUIRE(heldFuture.wait_for(10000ms) == std::future_status::ready);
      session->requestStop();
      release->set_value();
      CHECK(session->waitForRenderThread(StopBound));
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
    CHECK(stopped->stoppedEarly);
    CHECK(stopped->prepassDraws > 0u);
    CHECK(stopped->ambientDraws == 0u);
    CHECK(stopped->interactionDraws == 0u);
    messages.print("LP4.2a stop after the prepass, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

#if defined(_MSC_VER) && defined(_DEBUG)
  SECTION("100 frames with the ambient pass on the debug heap")
  {
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
      REQUIRE(settledHdr(*session, lit, 1));
      auto failures = 0;
      auto frame = 0;
      const auto frames = [&](const int count) {
        for (auto i = 0; i < count; ++i)
        {
          // a camera that moves by a unit: the lists are kept or rebuilt, every frame
          // runs the probe choice and the pass
          session->setCamera(
            litCamera({60, 0, 80.0f + float(frame++ % 2)}, {0, 0, -1}, {1, 0, 0}));
          failures += inFrame(*session, [](RenderFrameAccess&) {}) ? 0 : 1;
        }
      };
      frames(10);
      settle(*session);
      const auto growth = heapGrowth([&] {
        frames(100);
        settle(*session);
      });
      CHECK(failures == 0);
      CAPTURE(growth.blocks, growth.bytes);
      std::cout << "LP4.2a: 100 frames with the ambient pass: heap blocks "
                << growth.blocks << ", bytes " << growth.bytes << "\n";
      CHECK(growth.blocks <= 16);
      CHECK(growth.bytes <= 16384);
      teardown(*session);
      CHECK(session->nvrhiErrorCount() == 0u);
      CHECK(events.count(SessionEvent::Error) == 0u);
      session.reset();
      service->requestStop();
      service->join();
    }
    CHECK(resetDispatcher());
    messages.print("LP4.2a 100 frames, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
#endif

  std::filesystem::remove_all(root);
}


namespace
{

// ----- LP4.3: the light-grid path's synthetic bake (C36, P4.20) -----

// the two areas' grids, 2 x 2 x 1 each (probe size 16, border 2): area 0's cell spans
// x 80..176 and y -100..100 at z 2 (the slabs' top: the z blend's fraction is 0), area
// 1's the same 256 units west
constexpr auto Lp43GridOrigins =
  std::array<std::array<float, 3>, 2>{{{80, -100, 2}, {-176, -100, 2}}};
constexpr auto Lp43GridSize = std::array<float, 3>{96, 200, 256};

std::string lp43GridText(const int areas = 2)
{
  auto out = std::string{"LGRID \"4\"\n\n"};
  for (auto a = 0; a < areas; ++a)
  {
    const auto& o = Lp43GridOrigins[std::size_t(a)];
    const auto& s = Lp43GridSize;
    out += fmt::format(
      "lightGridPoints {{ /* area = */ {} /* numLightGridPoints = */ 4 "
      "/* imageSingleProbeSize = */ 16 /* imageBorderSize = */ 2 \n"
      "/* gridMins */ ( {} {} {} )\n/* gridSize */ ( {} {} {} )\n"
      "/* gridBounds */ 2 2 1\n\n",
      a,
      o[0],
      o[1],
      o[2],
      s[0],
      s[1],
      s[2]);
    for (auto gy = 0; gy < 2; ++gy)
    {
      for (auto gx = 0; gx < 2; ++gx)
      {
        out += fmt::format(
          " 1 ( {} {} {} )", o[0] + float(gx) * s[0], o[1] + float(gy) * s[1], o[2]);
      }
    }
    out += "}\n\n";
  }
  return out;
}

// an area's irradiance atlas as the bake lays it out (RenderWorld_lightgrid.cpp:
// 1387-1441): 16 x 16 tiles, the tile of grid point ( gx, gy, gz ) at column gx + gz bx,
// row gy, here one constant irradiance per tile (border included), as R11G11B10F (A3:
// accepted for BC6H, Image_load.cpp:674-680); a 2 x 2 x 1 grid's atlas is 32 x 32
std::string lp43AtlasCache(const std::array<std::array<float, 3>, 2>& columns)
{
  auto level = std::string{};
  for (auto y = 0; y < 32; ++y)
  {
    for (auto x = 0; x < 32; ++x)
    {
      const auto& e = columns[std::size_t(x / 16)];
      level += r11g11b10Texel(e[0], e[1], e[2]);
    }
  }
  return bimageFile(
    -1,
    engine_image::Type2D,
    engine_image::FormatR11g11b10f,
    32,
    32,
    1,
    {BimageRecord{0, 0, 32, 32, level}});
}

// the tiles: area 0's vary with gx alone (column 0 the smaller), area 1's all one colour
constexpr auto Lp43TilesA =
  std::array<std::array<float, 3>, 2>{{{0.25f, 0.25f, 0.125f}, {0.75f, 0.5f, 0.375f}}};
constexpr auto Lp43TilesB =
  std::array<std::array<float, 3>, 2>{{{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}}};

// the fixture map with a wide legacy slab (top at z 2) in each room, under the cameras
std::string lp43MapText()
{
  auto map = lp22aMapText();
  const auto entity1 = map.find("// entity 1");
  const auto close = map.rfind('}', entity1);
  map.insert(
    close,
    boxBrush(40, -60, 0, 216, 60, 2, "lp42a/flat")
      + boxBrush(-216, -60, 0, -40, 60, 2, "lp42a/flat"));
  return map;
}

/**
 * The CPU model of ambient_lightgrid_IBL.ps.hlsl's irradiance (AGP:342-478) at a world
 * position for constant tiles: g = floor( max( 0, ( p - origin ) / size ) ) clamped to
 * bounds - 1 with the fraction before the clamp, the 8 corners' trilinear weights, a
 * corner past the atlas's columns reading the last column (the clamp sampler at a
 * constant tile's edge), samples with an rgb sum below 1e-4 skipped and the sum
 * renormalised below 0.9999; every octahedral direction of a constant tile reads its
 * constant.
 */
std::array<float, 3> modelGridIrradiance(
  const std::array<float, 3>& origin,
  const std::array<float, 3>& size,
  const std::array<int, 3>& bounds,
  const std::array<std::array<float, 3>, 2>& columns,
  const std::array<float, 3>& position)
{
  auto g = std::array<int, 3>{};
  auto frac = std::array<float, 3>{};
  for (std::size_t j = 0; j < 3; ++j)
  {
    const auto v = std::max(0.0f, (position[j] - origin[j]) * (1.0f / size[j]));
    g[j] = int(std::floor(v));
    frac[j] = v - float(g[j]);
    g[j] = std::clamp(g[j], 0, bounds[j] - 1);
  }
  auto irradiance = std::array<float, 3>{};
  auto total = 0.0f;
  for (auto i = 0; i < 8; ++i)
  {
    auto factor = 1.0f;
    auto c = g;
    for (std::size_t j = 0; j < 3; ++j)
    {
      if (i & (1 << j))
      {
        factor *= frac[j];
        c[j] += 1;
      }
      else
      {
        factor *= 1.0f - frac[j];
      }
    }
    const auto column = std::min(c[0] + c[2] * bounds[0], bounds[0] * bounds[2] - 1);
    const auto& e = columns[std::size_t(std::clamp(column, 0, 1))];
    if (e[0] + e[1] + e[2] < 0.0001f)
    {
      continue;
    }
    for (std::size_t k = 0; k < 3; ++k)
    {
      irradiance[k] += e[k] * factor;
    }
    total += factor;
  }
  if (total > 0.0f && total < 0.9999f)
  {
    for (auto& v : irradiance)
    {
      v *= 1.0f / total;
    }
  }
  return irradiance;
}

// the drawn slab surface of an area in the last lists (its grid area), if drawn
std::optional<std::optional<int>> slabGridArea(const FrontendLists& lists, const int area)
{
  for (const auto& surface : lists.drawSurfs)
  {
    if (
      surface.chunk->key.material == "textures/lp42a/flat"
      && surface.chunk->key.area == area)
    {
      return surface.gridArea;
    }
  }
  return std::nullopt;
}

} // namespace

// LP4.3 (C34, C36, D86, D88, P4.20): the ambient pass's light-grid path over the LP2.2a
// fixture with a synthetic .lightgrid (two areas, a 2 x 2 x 1 grid each) and a synthetic
// R11G11B10F atlas per area whose tiles hold constant irradiance: the floor slab's pixel
// against a CPU model of the grid path, the trilinear blend monotone across the cell (the
// ratio of area 0's varying grid to area 1's constant one at the same relative view), a
// Pending atlas (held through the slot table's request) keeping the frame unconverged, a
// Missing atlas sending its area to the probe path (black: no probe images), and the
// ambient draws the same on both paths. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession light-grid path on a GPU (LP4.3)", "[.][litgpu]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pass needs the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);
  const auto base = root / "base";
  writeTextFile(base / "materials" / "zz_lp42a.mtr", Lp42aMaterials);
  std::filesystem::create_directories(base / "textures" / "lp42a");
  {
    auto file = std::ofstream{base / "textures" / "lp42a" / "flat.tga", std::ios::binary};
    const auto bytes = test::tgaFile(test::flat(4, 4, 200, 120, 60));
    file.write(bytes.data(), std::streamsize(bytes.size()));
  }
  const auto atlases = base / "generated" / "images" / "env" / "maps" / "lp1b_geometry";
  std::filesystem::create_directories(atlases);
  const auto atlasPath = [&](const int area) {
    return atlases / fmt::format("area{}_lightgrid_amb#__2100.bimage", area);
  };
  const auto writeAtlas = [&](const int area, const auto& tiles) {
    auto file = std::ofstream{atlasPath(area), std::ios::binary};
    const auto bytes = lp43AtlasCache(tiles);
    file.write(bytes.data(), std::streamsize(bytes.size()));
  };
  writeTextFile(base / "maps" / "lp1b_geometry.lightgrid", lp43GridText());
  // the cameras 48 above the slabs' top, each over its cell's middle in x
  const auto cameraA = litCamera({128, 0, 50}, {0, 0, -1}, {1, 0, 0});
  const auto cameraB = litCamera({-128, 0, 50}, {0, 0, -1}, {1, 0, 0});
  const auto atlas0 = lightGridImageKey("maps/lp1b_geometry", 0);
  const auto atlas1 = lightGridImageKey("maps/lp1b_geometry", 1);

  // camera B's ambient draws on the grid path (the first section), which the second's on
  // the probe path equal (the sections run in order in one process)
  static auto gridDrawsB = std::size_t{0};

  // a settled ambient-only readback of a camera (taken again until the frame is its own)
  const auto readbackOf = [](LitSession& session, const CameraState& camera) {
    auto readback = std::optional<AmbientReadback>{};
    for (auto attempt = 0;
         attempt < 20
         && !(
           readback && readback->frame.settled && readback->frame.lit.camera == camera);
         ++attempt)
    {
      REQUIRE(settledHdr(session, camera, 0));
      readback = ambientReadback(session);
    }
    REQUIRE(readback);
    REQUIRE(readback->frame.settled);
    REQUIRE(readback->frame.lit.camera == camera);
    return std::move(*readback);
  };

  SECTION("both atlases: the model, the blend, a Pending atlas")
  {
    writeAtlas(0, Lp43TilesA);
    writeAtlas(1, Lp43TilesB);
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
      // D88: area 0's atlas held Pending through the slot table's request until released
      auto held = std::make_shared<std::atomic<bool>>(true);
      auto* const materials = service.get();
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.images.setRequest([materials, held, atlas0](const MaterialImageKey& key) {
          return held->load() && key == atlas0 ? ImageLookup{} : materials->image(key);
        });
      }));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp43MapText(), root, 7, 1)));
      session->skipInteractionsForTesting(true);
      session->setCamera(cameraA);

      // the table is adopted, area 1's atlas arrives, area 0's stays Pending: the frame
      // is not settled, and the slab of area 0 takes the probe path meanwhile
      auto pending = FrameConvergence{};
      auto heldArea = std::optional<std::optional<int>>{};
      CHECK(waitFor(
        [&] {
          inFrame(*session, [&](RenderFrameAccess& access) {
            pending = access.convergence;
            heldArea = slabGridArea(access.passes.lists(), 0);
          });
          return session->gridTable()->areas.size() == 2u && pending.gridImagesPending
                 && heldArea.has_value();
        },
        30000ms,
        false));
      CHECK(pending.gridImagesPending);
      CHECK_FALSE(pending.settled());
      CHECK_FALSE(session->lastFrameSettledForTesting());
      REQUIRE(heldArea);
      CHECK(*heldArea == std::nullopt);

      // released: the slot asked again, its image arrives, the frames settle
      held->store(false);
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        const auto id = access.images.find(atlas0);
        REQUIRE(id);
        access.images.dropTexture(*id, access.frameIndex);
      }));

      const auto a = readbackOf(*session, cameraA);
      const auto b = readbackOf(*session, cameraB);
      gridDrawsB = b.frame.lit.ambientDraws;

      // the grid table and the slots: both atlases Resident, 32 x 32
      const auto grid = session->gridTable();
      REQUIRE(grid->areas.size() == 2u);
      CHECK(grid->hasPoints(0));
      CHECK(grid->hasPoints(1));
      auto slabArea = std::optional<std::optional<int>>{};
      auto sizes = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        for (const auto area : {0, 1})
        {
          const auto id = access.images.find(area == 0 ? atlas0 : atlas1);
          REQUIRE(id);
          const auto* slot = access.images.slot(*id);
          CHECK(slot->state == ImageSlots::State::Resident);
          REQUIRE(slot->texture);
          sizes[std::size_t(area)] = {
            slot->texture->getDesc().width, slot->texture->getDesc().height};
        }
        // the lists are camera B's
        slabArea = slabGridArea(access.passes.lists(), 1);
      }));
      CHECK(sizes[0] == std::pair{32u, 32u});
      CHECK(sizes[1] == std::pair{32u, 32u});
      REQUIRE(slabArea);
      CHECK(*slabArea == 1);
      for (const auto* readback : {&a, &b})
      {
        CHECK(readback->frame.lit.ambientGridSurfaces > 0u);
        CHECK(readback->frame.lit.gridSurfaces > 0u);
        CHECK(readback->frame.lit.interactionDraws == 0u);
        CHECK_FALSE(readback->frame.lit.gridImagesPending);
      }

      // the model at camera A's centre pixel: its world point ~( 127.7, -0.3, 2 ) (the
      // pixel centre's offset at 48 units, right = -y, down = -x)
      const auto flatKey = stageImage(*service, "textures/lp42a/flat", 2);
      REQUIRE(flatKey);
      const auto flatTexel = firstTexel(*service, *flatKey);
      REQUIRE(flatTexel);
      const auto albedo = modelDiffuseMap(*flatTexel);
      const auto brdf = modelEnvBrdf(0.99995f, 1.0f);
      const auto scatter = modelSpecularScatter(0.0f, brdf);
      const auto pointA = std::array<float, 3>{128.0f - 0.3f, -0.3f, 2.0f};
      const auto e = modelGridIrradiance(
        Lp43GridOrigins[0], Lp43GridSize, {2, 2, 1}, Lp43TilesA, pointA);
      auto modelHdr = std::array<float, 3>{};
      for (std::size_t c = 0; c < 3; ++c)
      {
        // (1 - specularScatter) E albedo ao (stage colour x 3), times r_forceAmbient; the
        // specular term is the default cube's black
        modelHdr[c] =
          (1.0f - scatter) * e[c] * albedo[c] * 1.0f * 3.0f * EngineFrame.forceAmbient;
      }
      const auto hdr = a.hdr.at(80, 60);
      const auto at = (std::size_t(60) * a.hdr.width + 80) * 4;
      const auto expected = modelLdr(modelHdr, a.exposure, a.frame);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CAPTURE(c);
        CHECK(hdr[c] == Catch::Approx(modelHdr[c]).epsilon(0.01));
        CHECK(std::abs(int(a.ldr[at + c]) - expected[c]) <= 1);
      }
      std::cout << fmt::format(
        "LP4.3 grid path A: E ({:.5f} {:.5f} {:.5f}); HDR model ({:.5f} {:.5f} {:.5f}) "
        "read-back ({:.5f} {:.5f} {:.5f}); LDR model ({} {} {}) read-back ({} {} {}); "
        "exposure {:.6f}\n",
        e[0],
        e[1],
        e[2],
        modelHdr[0],
        modelHdr[1],
        modelHdr[2],
        hdr[0],
        hdr[1],
        hdr[2],
        expected[0],
        expected[1],
        expected[2],
        int(a.ldr[at]),
        int(a.ldr[at + 1]),
        int(a.ldr[at + 2]),
        a.exposure);

      // the blend down the centre column (rows top to bottom: world x from ~164 down,
      // inside the cell x 80..176; rows 2 to 88, x ~111 and more, short of the fixture's
      // grate at x 100..104): the ratio to area 1's constant grid at the same relative
      // view is area 0's irradiance over 0.5, decreasing with the row, strictly between
      // the two columns' values
      auto ratios = std::vector<float>{};
      auto worst = 0.0f;
      for (std::uint32_t row = 2; row <= 88; ++row)
      {
        const auto pa = a.hdr.at(80, row);
        const auto pb = b.hdr.at(80, row);
        REQUIRE(pb[0] > 0.0f);
        const auto ratio = pa[0] / pb[0];
        if (!ratios.empty())
        {
          worst = std::max(worst, ratio - ratios.back());
        }
        ratios.push_back(ratio);
      }
      const auto lo = Lp43TilesA[0][0] / Lp43TilesB[0][0];
      const auto hi = Lp43TilesA[1][0] / Lp43TilesB[0][0];
      CHECK(worst <= 0.002f);
      CHECK(ratios.front() > ratios.back() + 0.3f);
      CHECK(*std::ranges::min_element(ratios) > lo);
      CHECK(*std::ranges::max_element(ratios) < hi);
      std::cout << fmt::format(
        "LP4.3 grid blend down the column: ratio {:.4f} (top) to {:.4f} (bottom), "
        "between {:.2f} and {:.2f}; the largest rise {:.5f}; B's centre HDR ({:.5f} "
        "{:.5f} {:.5f}), {} ambient draws; atlases {}x{}\n",
        ratios.front(),
        ratios.back(),
        lo,
        hi,
        worst,
        b.hdr.at(80, 60)[0],
        b.hdr.at(80, 60)[1],
        b.hdr.at(80, 60)[2],
        b.frame.lit.ambientDraws,
        sizes[0].first,
        sizes[0].second);

      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.images.setRequest(
          [materials](const MaterialImageKey& key) { return materials->image(key); });
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
    messages.print("LP4.3 grid path, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("area 1's atlas Missing: its slab on the probe path, black; the draws the same")
  {
    writeAtlas(0, Lp43TilesA);
    std::filesystem::remove(atlasPath(1));
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
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp43MapText(), root, 7, 1)));
      session->skipInteractionsForTesting(true);
      const auto b = readbackOf(*session, cameraB);
      const auto a = readbackOf(*session, cameraA);
      auto state = std::optional<ImageSlots::State>{};
      auto slabArea = std::optional<std::optional<int>>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        if (const auto id = access.images.find(atlas1))
        {
          state = access.images.slot(*id)->state;
        }
        slabArea = slabGridArea(access.passes.lists(), 0);
      }));
      CHECK(state == ImageSlots::State::Default);
      // camera B: the slab black (no SH, the default cube), drawn all the same, the same
      // number of ambient draws as on the grid path (the draws do not depend on the path)
      const auto centre = b.hdr.at(80, 60);
      CHECK(centre[0] == 0.0f);
      CHECK(centre[1] == 0.0f);
      CHECK(centre[2] == 0.0f);
      CHECK(b.frame.lit.ambientDraws > 0u);
      if (gridDrawsB > 0u)
      {
        CHECK(b.frame.lit.ambientDraws == gridDrawsB);
      }
      CHECK_FALSE(b.frame.lit.gridImagesPending);
      // camera A on its own grid as before
      CHECK(a.hdr.at(80, 60)[0] > 0.0f);
      CHECK(a.frame.lit.ambientGridSurfaces > 0u);
      REQUIRE(slabArea);
      CHECK(*slabArea == 0);
      std::cout << fmt::format(
        "LP4.3 Missing atlas: area 1's slab ({} {} {}), {} ambient draws, {} on the "
        "grid; "
        "area 0's slab {:.5f}\n",
        centre[0],
        centre[1],
        centre[2],
        b.frame.lit.ambientDraws,
        b.frame.lit.ambientGridSurfaces,
        a.hdr.at(80, 60)[0]);
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
    messages.print("LP4.3 Missing atlas, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  // LP4.3 step 5 (D93, K29, K16): a light grid with one entry too few. readMapBake drops
  // the grid and the probes with the reason (the .shprobes would light the slab); the
  // line says so, the pass is black, and the tree is still used
  SECTION("the mismatched bake: the grid and the probes dropped, the pass black")
  {
    writeAtlas(0, Lp43TilesA);
    writeAtlas(1, Lp43TilesB);
    writeTextFile(base / "maps" / "lp1b_geometry.lightgrid", lp43GridText(1));
    const auto c0 = 0.25f / ModelY00;
    const auto sh = fmt::format(
      "( {0:.9g} {0:.9g} {0:.9g} 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 )", c0);
    writeTextFile(
      base / "maps" / "lp1b_geometry.shprobes",
      fmt::format("SHPROBES \"1\"\n2\n0 ( 132 0 64 ) {0}\n1 ( -124 0 64 ) {0}\n", sh));
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
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp43MapText(), root, 7, 1)));
      session->skipInteractionsForTesting(true);
      const auto a = readbackOf(*session, cameraA);
      // the tree is used (K16): the view flowed, area 0 of 2
      CHECK(a.frame.lit.areas == 2u);
      CHECK(a.frame.lit.viewArea == 0);
      // no grid, no SH: the slab black
      CHECK(session->gridTable()->areas.empty());
      CHECK(a.frame.lit.gridSurfaces == 0u);
      CHECK(a.frame.lit.ambientGridSurfaces == 0u);
      CHECK(a.frame.lit.ambientDraws > 0u);
      const auto centre = a.hdr.at(80, 60);
      CHECK(centre[0] == 0.0f);
      CHECK(centre[1] == 0.0f);
      CHECK(centre[2] == 0.0f);
      const auto probes = session->probeTable();
      REQUIRE(probes->records.size() == 2u);
      CHECK_FALSE(probes->records[0].shFound);
      CHECK_FALSE(probes->records[1].shFound);
      // the status line says why
      const auto line = sessionStatusLine(session->status(), {});
      CAPTURE(line);
      CHECK(
        line.find(
          "| bake: dropped (the light grid has 1 entries for 1 distinct areas, the "
          "map has 2 areas)")
        != std::string::npos);
      std::cout << "LP4.3 mismatched bake: " << line << "\n";
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
    messages.print("LP4.3 mismatched bake, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  // LP4.3 step 5 (D94): the LP2.2a fixture without its .proc: no flow, the lists LP2's
  // (every chunk a candidate, the lights through walls), the ambient pass black (no tree:
  // no probes, no grid), and the line says "bake: none"
  SECTION("the no-tree map: the flow skipped, the pass black, bake: none")
  {
    std::filesystem::remove(base / "maps" / "lp1b_geometry.proc");
    writeAtlas(0, Lp43TilesA);
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
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp43MapText(), root, 7, 1)));
      // lit: the interactions draw as in LP2, through the doorway's wall too (K1)
      const auto lit = settledHdr(*session, cameraA, 1);
      REQUIRE(lit);
      CHECK(lit->second.stats.areas == 0u);
      CHECK(lit->second.stats.viewArea == -1);
      CHECK(lit->second.stats.areasFlowed == 0u);
      CHECK(lit->second.stats.gridSurfaces == 0u);
      CHECK(lit->second.stats.interactionDraws > 0u);
      auto flowed = true;
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        flowed = access.passes.lists().flowed;
      }));
      CHECK_FALSE(flowed);
      CHECK(session->gridTable()->areas.empty());
      CHECK(session->probeTable()->records.empty());
      // ambient only: black
      session->skipInteractionsForTesting(true);
      const auto a = readbackOf(*session, cameraA);
      CHECK(a.frame.lit.ambientDraws > 0u);
      const auto centre = a.hdr.at(80, 60);
      CHECK(centre[0] == 0.0f);
      CHECK(centre[1] == 0.0f);
      CHECK(centre[2] == 0.0f);
      const auto line = sessionStatusLine(session->status(), {});
      CAPTURE(line);
      CHECK(line.find("| bake: none") != std::string::npos);
      CHECK(line.find("| area ") == std::string::npos);
      std::cout << "LP4.3 no tree: " << line << "\n";
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
    messages.print("LP4.3 no tree, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  std::filesystem::remove_all(root);
}

// LP4.3 step 5 (D81, D93, P4.8, §6's exit test): the stale bake on the LP2 parity map and
// its .proc (TB_LIT_PARITY_ROOT, read only): a world brush and a light added outside the
// hull through a snapshot (the edit the saved file does not hold), and a light inside
// room 1 whose volume reaches through the wall to the brush. The brush's faces are pieces
// in solid (area -1, the outside set): drawn from the void (view area -1), lit by the new
// light (no references: it lights the outside set alone, by volume) and by the inside one
// (D81: a light with references lights the outside geometry its volume meets); the
// status line says "stale" for the unsaved edit and counts the pieces outside the baked
// hull. Hidden: it needs a GPU and the root of a parity.py run.
TEST_CASE(
  "LitSession: the stale bake on the parity map (LP4.3, D81, P4.8)", "[.][parity]")
{
  const auto* rootValue = std::getenv("TB_LIT_PARITY_ROOT");
  if (!rootValue || !*rootValue)
  {
    SKIP(
      "TB_LIT_PARITY_ROOT is not set: the test needs the preview root of a parity run");
  }
  const auto root = std::filesystem::path{rootValue};
  const auto map = std::string{"maps/test/litpreview_parity.map"};
  REQUIRE(std::filesystem::exists(root / "base" / map));
  REQUIRE(std::filesystem::exists((root / "base" / map).replace_extension(".proc")));

  // the parity map's text with the brush in the void east of room 1 (interior x 0..384,
  // its wall x 384..400) and the two lights
  auto edited = readTextFile(root / "base" / map);
  {
    const auto entity1 = edited.find("// entity 1");
    REQUIRE(entity1 != std::string::npos);
    edited.insert(
      edited.rfind('}', entity1), boxBrush(420, 160, 64, 452, 224, 128, "lp2/white"));
    edited +=
      "// entity lp43\n{\n\"classname\" \"light\"\n\"name\" \"lp43_stale_outside\"\n"
      "\"origin\" \"500 192 96\"\n\"light_radius\" \"60 60 60\"\n"
      "\"texture\" \"lights/lp2/round\"\n\"_color\" \"1 1 1\"\n\"noshadows\" "
      "\"1\"\n}\n";
    edited +=
      "// entity lp43b\n{\n\"classname\" \"light\"\n\"name\" \"lp43_stale_inside\"\n"
      "\"origin\" \"360 192 96\"\n\"light_radius\" \"120 120 120\"\n"
      "\"texture\" \"lights/lp2/round\"\n\"_color\" \"1 1 1\"\n\"noshadows\" "
      "\"1\"\n}\n";
  }
  const auto inBrush = [](const Chunk& chunk) {
    return chunk.bounds[0][0] >= 419.0f && chunk.bounds[1][0] <= 453.0f
           && chunk.bounds[0][1] >= 159.0f && chunk.bounds[1][1] <= 225.0f
           && chunk.bounds[0][2] >= 63.0f && chunk.bounds[1][2] <= 129.0f;
  };

  const auto library = VulkanLibrary{};
  REQUIRE(library.getInstanceProcAddr != nullptr);
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
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    // from the void east of the brush, looking west at it
    const auto camera = litCamera({600, 192, 96}, {-1, 0, 0}, {0, 0, 1});

    // the saved map first: its own pieces in solid (the walls' outer faces, K14)
    REQUIRE(session->offerSnapshot(paritySnapshot(root, map, 1)));
    REQUIRE(settledHdr(*session, camera, 0));
    const auto before = session->status();
    REQUIRE(before.bake == SessionStatus::Bake::Proc);

    // the edit
    REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
    auto snapshot = snapshotOfMapText(edited);
    const auto offset = PreviewId{2} << 32;
    for (auto& entity : snapshot.entities)
    {
      entity.id += offset;
    }
    for (auto& node : snapshot.nodes)
    {
      node.id += offset;
      node.entity += offset;
    }
    snapshot.removed = snapshotIds(*paritySnapshot(root, map, 1));
    snapshot.generation = 7;
    snapshot.sequence = 2;
    snapshot.mapPath = (root / "base" / map).string();
    REQUIRE(
      session->offerSnapshot(std::make_shared<const Snapshot>(std::move(snapshot))));
    const auto frame =
      settledHdr(*session, litCamera({600, 192, 97}, {-1, 0, 0}, {0, 0, 1}), 2);
    REQUIRE(frame);
    CHECK(frame->second.stats.viewArea == -1);

    auto brushDrawn = 0;
    auto litBy = std::map<std::string, int>{};
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      const auto& lists = access.passes.lists();
      for (const auto& surface : lists.drawSurfs)
      {
        if (inBrush(*surface.chunk))
        {
          CHECK(surface.chunk->key.area == -1);
          CHECK_FALSE(surface.gridArea);
          ++brushDrawn;
        }
      }
      for (const auto& light : lists.lights)
      {
        const auto* name = findKey(light.record->spawnArgs, "name");
        for (const auto* list :
             {&light.localInteractions,
              &light.globalInteractions,
              &light.translucentInteractions})
        {
          for (const auto& surface : *list)
          {
            if (name && inBrush(*surface.chunk))
            {
              ++litBy[*name];
            }
          }
        }
      }
    }));
    const auto table = session->lightTable();
    auto outsideAreas = std::optional<std::size_t>{};
    auto insideAreas = std::optional<std::size_t>{};
    table->records.forEach([&](const PreviewId&, const auto& record) {
      const auto* name = record ? findKey(record->spawnArgs, "name") : nullptr;
      if (name && *name == "lp43_stale_outside")
      {
        outsideAreas = record->derived.areas.size();
      }
      if (name && *name == "lp43_stale_inside")
      {
        insideAreas = record->derived.areas.size();
      }
    });
    // the new light has no references (the outside set), the inside one references room 1
    CHECK(outsideAreas == std::size_t{0});
    REQUIRE(insideAreas);
    CHECK(*insideAreas >= 1u);
    CHECK(brushDrawn > 0);
    CHECK(litBy["lp43_stale_outside"] > 0);
    CHECK(litBy["lp43_stale_inside"] > 0);

    // the status: more pieces in solid; stale for the unsaved edit (the host's flag), not
    // without it (the .proc was written after the .map)
    auto status = session->status();
    CHECK(status.bake == SessionStatus::Bake::Proc);
    CHECK(status.solidPieces > before.solidPieces);
    if (const auto stat = statGameFile(root / "base" / map))
    {
      status.mapFileTime = stat->unixSeconds;
    }
    const auto saved = sessionStatusLine(status, {});
    status.documentModified = true;
    const auto line = sessionStatusLine(status, {});
    CAPTURE(saved, line);
    CHECK(saved.find("stale") == std::string::npos);
    CHECK(line.find("| bake: proc, stale, ") != std::string::npos);
    CHECK(line.find(" pieces outside the baked hull") != std::string::npos);
    std::cout << fmt::format(
      "LP4.3 stale bake: {} pieces in solid before the edit, {} after; the brush drawn "
      "in "
      "{} surfaces, lit by the outside light {} and the inside light {} times; {}\n",
      before.solidPieces,
      status.solidPieces,
      brushDrawn,
      litBy["lp43_stale_outside"],
      litBy["lp43_stale_inside"],
      line);

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
  messages.print("LP4.3 stale bake, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

namespace
{

// ----- LPF.1: the live feed on the render side -----

/**
 * LPF.1 (P5.12's pattern): what a host's guarded listener does: every publication of the
 * service tells the attached session (tableChanged) and is timed; the parse thread's Feed
 * and Reparse items are counted as they start, so the publication each one made (the next
 * one: an item publishes exactly once) is known. recording off keeps the vectors as they
 * are (a heap measurement).
 */
struct FeedListener
{
  std::mutex mutex;
  LitSession* session = nullptr;
  bool recording = true;
  std::vector<std::chrono::steady_clock::time_point> publications;
  // per Feed or Reparse item, the publications made before it started
  std::vector<std::size_t> itemStarts;

  void attach(LitSession* attached)
  {
    const auto lock = std::scoped_lock{mutex};
    session = attached;
  }

  void record(const bool on)
  {
    const auto lock = std::scoped_lock{mutex};
    recording = on;
  }

  std::size_t items()
  {
    const auto lock = std::scoped_lock{mutex};
    return itemStarts.size();
  }

  // the time of the nth Feed or Reparse item's publication (0 the first), once it is made
  std::optional<std::chrono::steady_clock::time_point> itemPublication(
    const std::size_t nth)
  {
    const auto lock = std::scoped_lock{mutex};
    if (nth >= itemStarts.size() || itemStarts[nth] >= publications.size())
    {
      return std::nullopt;
    }
    return publications[itemStarts[nth]];
  }
};

// a material service over a game root as rootService's, whose listener is listener's; a
// closed feedGate holds every Feed and Reparse item before it runs
std::unique_ptr<MaterialService> feedService(
  const std::filesystem::path& root,
  const std::shared_ptr<FeedListener>& listener,
  const std::shared_ptr<ParseGate>& feedGate)
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [root](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
  };
  options.pollInterval = 0ms;
  options.imageBudgetBytes = 0;
  options.listener = [listener](const MaterialServiceEvent& event) {
    if (event.kind != MaterialServiceEvent::Kind::Published)
    {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto lock = std::scoped_lock{listener->mutex};
    if (listener->recording)
    {
      listener->publications.push_back(now);
    }
    if (listener->session)
    {
      listener->session->tableChanged();
    }
  };
  options.beforeWorkItemForTesting = [listener, feedGate](const std::string_view kind) {
    if (kind != "feed" && kind != "reparse")
    {
      return;
    }
    feedGate->pass();
    const auto lock = std::scoped_lock{listener->mutex};
    if (listener->recording)
    {
      listener->itemStarts.push_back(listener->publications.size());
    }
  };
  return MaterialService::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

// LPF.1 (P5.23): what a frame's start bound for one key's slot, recorded by an
// every-frame hook without a copy or a GPU wait
struct FeedFrame
{
  std::uint64_t index = 0;
  std::chrono::steady_clock::time_point start;
  // the key has a slot
  bool slotted = false;
  // its binding: the texture (compared, never used), whether the intrinsic stands in, the
  // slot's version in it, and the slot's state
  const nvrhi::ITexture* texture = nullptr;
  bool intrinsic = false;
  std::uint64_t version = 0;
  ImageSlots::State state = ImageSlots::State::Pending;
  // the last frame's convergence: a geometry batch was in flight
  bool lastGeometryInFlight = false;
  // the last frame's lit passes: its skipped and interaction draws, whether it built its
  // lists again
  std::size_t lastSkippedDraws = 0;
  std::size_t lastInteractionDraws = 0;
  bool lastListsRebuilt = false;
  bool lastListsTableChanged = false;
};

struct FeedFrames
{
  std::mutex mutex;
  std::vector<FeedFrame> frames;

  std::optional<FeedFrame> last()
  {
    const auto lock = std::scoped_lock{mutex};
    return frames.empty() ? std::nullopt : std::optional{frames.back()};
  }

  // the frames whose start is at or after from
  std::vector<FeedFrame> since(const std::chrono::steady_clock::time_point from)
  {
    const auto lock = std::scoped_lock{mutex};
    auto out = std::vector<FeedFrame>{};
    std::ranges::copy_if(frames, std::back_inserter(out), [&](const FeedFrame& f) {
      return f.start >= from;
    });
    return out;
  }
};

void recordFeedFrames(
  LitSession& session,
  const std::shared_ptr<FeedFrames>& frames,
  const MaterialImageKey& key)
{
  session.setEveryFrameHookForTesting([frames, key](RenderFrameAccess& access) {
    auto frame = FeedFrame{};
    frame.index = access.frameIndex;
    frame.start = std::chrono::steady_clock::now();
    frame.lastGeometryInFlight = access.convergence.geometryInFlight;
    const auto& stats = access.passes.lastStats();
    frame.lastSkippedDraws = stats.skippedDraws;
    frame.lastInteractionDraws = stats.interactionDraws;
    frame.lastListsRebuilt = stats.listsRebuilt;
    frame.lastListsTableChanged = stats.listsTableChanged;
    if (const auto id = access.images.find(key))
    {
      const auto binding = access.images.binding(*id);
      frame.slotted = true;
      frame.texture = binding.texture;
      frame.intrinsic = binding.intrinsic;
      frame.version = binding.versions.front().version;
      frame.state = access.images.slot(*id)->state;
    }
    const auto lock = std::scoped_lock{frames->mutex};
    frames->frames.push_back(frame);
  });
}

// waits until the session has presented no frame for quiet; false at the deadline
bool waitQuiet(
  LitSession& session,
  const std::chrono::milliseconds quiet = 400ms,
  const std::chrono::milliseconds timeout = 30000ms)
{
  auto last = session.framesPresented();
  auto since = std::chrono::steady_clock::now();
  const auto deadline = since + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(20ms);
    const auto presented = session.framesPresented();
    const auto now = std::chrono::steady_clock::now();
    if (presented != last)
    {
      last = presented;
      since = now;
    }
    else if (now - since >= quiet)
    {
      return true;
    }
  }
  return false;
}

// a w × h RGBA8 picture of one colour, alpha 255
ImageOverride constantOverride(
  const int width, const int height, const std::array<std::uint8_t, 3>& rgb)
{
  auto pixels = RawImagePixels{};
  pixels.width = width;
  pixels.height = height;
  pixels.format = ImagePixelFormat::RGBA8;
  pixels.bytes.resize(std::size_t(width) * std::size_t(height) * 4);
  for (std::size_t i = 0; i < pixels.bytes.size(); i += 4)
  {
    pixels.bytes[i] = rgb[0];
    pixels.bytes[i + 1] = rgb[1];
    pixels.bytes[i + 2] = rgb[2];
    pixels.bytes[i + 3] = 255;
  }
  auto image = ImageOverride{};
  image.image = std::move(pixels);
  return image;
}

// waits until the latest table holds every hand-over made so far
std::shared_ptr<const MaterialTable> waitForFeed(
  MaterialService& service, const std::chrono::milliseconds timeout = 20000ms)
{
  const auto requested = service.feedRequested();
  return service.waitForTableForTesting(
    [&](const MaterialTable& table) { return table.feedApplied >= requested; }, timeout);
}

// the first texel of every level of key's image as the service holds it
std::vector<std::array<std::uint8_t, 4>> levelTexels(
  MaterialService& service, const MaterialImageKey& key)
{
  auto texels = std::vector<std::array<std::uint8_t, 4>>{};
  const auto lookup = service.image(key);
  if (lookup.image)
  {
    for (const auto& level : lookup.image->levels)
    {
      const auto* t = lookup.image->data.data() + level.offset;
      texels.push_back({t[0], t[1], t[2], t[3]});
    }
  }
  return texels;
}

// the exposure the feed tests pin (D110, as the parity rows pin it)
constexpr auto FeedPinnedLuminance = 1.0f;

EngineTonemapDefaults feedTonemap()
{
  auto tonemap = EngineTonemap;
  tonemap.hdrMinLuminance = FeedPinnedLuminance;
  tonemap.hdrMaxLuminance = FeedPinnedLuminance;
  return tonemap;
}

// what one override cost on the render side
struct FeedLanding
{
  // from the item's publication to the start of the first frame after it
  double latencyMs = 0.0;
  // the frames started after the publication until the session was quiet again
  std::vector<FeedFrame> frames;
  bool settled = false;
  std::chrono::steady_clock::time_point published;
};

// the floor's probe pixel and the linear colour of the diffuse texel under it
struct FeedProbe
{
  std::array<float, 3> linear = {};
  std::array<float, 3> hdr = {};
  std::array<int, 3> ldr = {};
  float exposure = 0.0f;
  ChainFrame frame;
};

// above the floor at (60 0 0), looking down, inside lp1b_light (LP2.2a's camera 1)
CameraState feedCamera()
{
  return litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
}

/**
 * LPF.1: a headless 160 × 120 session over the LP2.2a fixture with the feed listener and
 * the gate, the exposure pinned (D110), its frames logged, settled once on feedCamera,
 * and the floor's diffuse slot's binding recorded every frame from then on.
 */
struct FeedRig
{
  std::shared_ptr<FeedListener> listener = std::make_shared<FeedListener>();
  std::shared_ptr<ParseGate> gate = std::make_shared<ParseGate>();
  std::unique_ptr<MaterialService> service;
  Events events;
  std::unique_ptr<LitSession> session;
  // the listener lets the session go and the gate opens before the session goes
  struct Release
  {
    FeedRig* rig;
    ~Release()
    {
      rig->listener->attach(nullptr);
      rig->gate->open();
    }
  };
  Release release{this};
  std::shared_ptr<FeedFrames> frames = std::make_shared<FeedFrames>();
  MaterialImageKey floorKey;
  HdrImage firstHdr;

  FeedRig(
    const TestInstance& instance,
    const std::filesystem::path& root,
    const std::filesystem::path& logPath)
  {
    service = feedService(root, listener, gate);
    std::filesystem::remove(logPath);
    session = loggedRootSession(instance, events, *service, root, logPath);
    listener->attach(session.get());
    session->setTonemapForTesting(feedTonemap());
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));
    auto first = settledHdr(*session, feedCamera(), 1);
    REQUIRE(first);
    firstHdr = std::move(first->first);
    const auto key = stageImage(*service, "textures/lp1b/floor", 2);
    REQUIRE(key);
    floorKey = *key;
    recordFeedFrames(*session, frames, floorKey);
    REQUIRE(waitQuiet(*session));
    REQUIRE(session->lastFrameSettledForTesting());
  }

  /**
   * An override of the floor's diffuse (quietFirst: the camera still and the session
   * quiet, so that the pacer's slot is free), its publication, the frames after it until
   * the session is quiet again; the first of them is drawn without a camera move.
   */
  FeedLanding feed(const ImageOverride& image, const bool quietFirst = true) const
  {
    if (quietFirst)
    {
      REQUIRE(waitQuiet(*session));
    }
    const auto item = listener->items();
    REQUIRE(service->setImageOverride(floorKey.name, image).is_success());
    REQUIRE(waitForFeed(*service));
    auto landing = FeedLanding{};
    REQUIRE(waitFor(
      [&] {
        if (const auto published = listener->itemPublication(item))
        {
          landing.published = *published;
          return true;
        }
        return false;
      },
      5000ms,
      false));
    REQUIRE(
      waitFor([&] { return !frames->since(landing.published).empty(); }, 5000ms, false));
    REQUIRE(waitQuiet(*session));
    landing.frames = frames->since(landing.published);
    landing.latencyMs = std::chrono::duration<double, std::milli>(
                          landing.frames.front().start - landing.published)
                          .count();
    landing.settled = session->lastFrameSettledForTesting();
    return landing;
  }

  FeedLanding feed(const std::array<std::uint8_t, 3>& rgb, const int size = 64) const
  {
    return feed(constantOverride(size, size, rgb));
  }

  // the probe pixel of a settled frame (D110), and the converted texel under it
  FeedProbe probe(const bool constant = true) const
  {
    const auto texels = levelTexels(*service, floorKey);
    REQUIRE_FALSE(texels.empty());
    // a constant picture: every level holds the same texel (the gamma mips of one
    // colour), so the sampled mip does not matter
    for (const auto& texel : texels)
    {
      CHECK((!constant || texel == texels.front()));
    }
    auto readback = std::optional<AmbientReadback>{};
    REQUIRE(waitFor(
      [&] {
        readback = ambientReadback(*session);
        return readback && readback->frame.settled;
      },
      10000ms,
      false));
    auto result = FeedProbe{};
    result.linear = modelDiffuseMap(texels.front());
    const auto hdr = readback->hdr.at(80, 60);
    const auto at = (std::size_t(60) * readback->hdr.width + 80) * 4;
    for (std::size_t c = 0; c < 3; ++c)
    {
      result.hdr[c] = hdr[c];
      result.ldr[c] = int(readback->ldr[at + c]);
    }
    result.exposure = readback->exposure;
    result.frame = readback->frame;
    return result;
  }

  // teardown step 1 to 5, and what it must leave
  void close(const std::string_view label)
  {
    session->setEveryFrameHookForTesting({});
    listener->attach(nullptr);
    teardown(*session);
    CHECK(session->lastDeviceReleaseCount() == 0u);
    CHECK(session->nvrhiErrorCount() == 0u);
    CHECK(session->deviceMemoryAllocations() == 0);
    CHECK(events.count(SessionEvent::Error) == 0u);
    events.print(label);
    session.reset();
    // a feed held at the gate runs, with no session to tell
    gate->open();
    service->requestStop();
    service->join();
  }

  /**
   * A re-parse of a material (C42) and its publication; then the frames until the
   * session is settled and quiet again, the scene's batches it started applied. The
   * frames from the publication on.
   */
  std::vector<FeedFrame> reparse(
    const std::string& name, const std::optional<std::string>& text) const
  {
    REQUIRE(waitQuiet(*session));
    const auto item = listener->items();
    const auto batches = session->geometryBatchesForTesting();
    if (text)
    {
      REQUIRE(service->reparseMaterial(name, *text).is_success());
    }
    else
    {
      service->clearMaterialReparse(name);
    }
    REQUIRE(waitForFeed(*service));
    auto published = std::optional<std::chrono::steady_clock::time_point>{};
    REQUIRE(waitFor(
      [&] {
        published = listener->itemPublication(item);
        return published.has_value();
      },
      5000ms,
      false));
    REQUIRE(waitFor([&] { return !frames->since(*published).empty(); }, 5000ms, false));
    REQUIRE(waitFor(
      [&] {
        return session->lastFrameSettledForTesting()
               && session->geometryBatchesForTesting() >= batches;
      },
      60000ms,
      false));
    REQUIRE(waitQuiet(*session));
    CHECK(session->lastFrameSettledForTesting());
    return frames->since(*published);
  }
};

// the light record of a light by its name key, null when there is none
std::shared_ptr<const LightRecord> namedLight(
  const LightTable& table, const std::string_view name)
{
  auto found = std::shared_ptr<const LightRecord>{};
  table.records.forEach([&](const PreviewId&, const auto& record) {
    const auto* key = record ? findKey(record->spawnArgs, "name") : nullptr;
    if (key && *key == name)
    {
      found = record;
    }
  });
  return found;
}

// lp1bc.mtr's floor, two-sided (C43: its cells built again with back sides)
const auto FloorTwoSided = std::string{
  "{\n\ttwoSided\n\tqer_editorimage\ttextures/lp1b/floor.tga\n"
  "\tdiffusemap\ttextures/lp1b/floor.tga\n}\n"};
// zz_lp22a.mtr's lights/squarelight1, the default light texture (idLight::Spawn), at half
// its colour (the stage's rgb after colored)
const auto HalfSquareLight = std::string{
  "{\n\tlightFalloffImage _noFalloff\n\t{\n\t\tmap _white\n\t\tcolored\n"
  "\t\trgb 0.5\n\t\tzeroClamp\n\t}\n}\n"};

/**
 * D110, the probe pixel's model: with the exposure pinned, the floor's HDR value is
 * linear in its diffuse map's linear colour (the interactions and the ambient pass
 * multiply it by what does not depend on it): a black and a white override's probes
 * measure the two terms per channel, and a colour is predicted through the texel the
 * service converted (the engine's YCoCg layout, decoded as the shader decodes it). Checks
 * the probe's HDR value and, through the tonemap model, its LDR bytes; the prediction.
 */
std::array<float, 3> checkProbe(
  const FeedProbe& black, const FeedProbe& white, const FeedProbe& colour)
{
  CHECK(colour.exposure == Catch::Approx(FeedPinnedLuminance).epsilon(1e-5));
  auto predicted = std::array<float, 3>{};
  for (std::size_t c = 0; c < 3; ++c)
  {
    CAPTURE(c);
    const auto span = white.linear[c] - black.linear[c];
    REQUIRE(span > 0.5f);
    const auto term = (white.hdr[c] - black.hdr[c]) / span;
    predicted[c] = black.hdr[c] + term * (colour.linear[c] - black.linear[c]);
    CHECK(colour.hdr[c] == Catch::Approx(predicted[c]).epsilon(0.01).margin(2e-4));
  }
  const auto expected = modelLdr(predicted, colour.exposure, colour.frame);
  for (std::size_t c = 0; c < 3; ++c)
  {
    CAPTURE(c);
    CHECK(std::abs(colour.ldr[c] - expected[c]) <= 1);
  }
  std::cout << fmt::format(
    "LPF.1 floor probe: HDR ({:.5f} {:.5f} {:.5f}) model ({:.5f} {:.5f} {:.5f}); LDR "
    "({} {} {}) model ({} {} {})\n",
    colour.hdr[0],
    colour.hdr[1],
    colour.hdr[2],
    predicted[0],
    predicted[1],
    predicted[2],
    colour.ldr[0],
    colour.ldr[1],
    colour.ldr[2],
    expected[0],
    expected[1],
    expected[2]);
  return predicted;
}

// what checkReplacement found: the report's text, the swap's frame and the frames before
struct Replacement
{
  std::string text;
  std::uint64_t swap = 0;
  std::vector<std::uint64_t> oldFrames;
};

/**
 * C40 (P5.4, P5.23): what the frames of a replacement bound for the floor's slot: every
 * frame the old texture or the new one (never the intrinsic, never a third), the old one
 * with its version until the swap, the new one after it with the next, one swap; the
 * frames as "old x n, new x m" for the report.
 */
Replacement checkReplacement(
  const std::vector<FeedFrame>& frames,
  const FeedFrame& before,
  const std::string_view label)
{
  auto result = Replacement{};
  CAPTURE(label);
  REQUIRE_FALSE(frames.empty());
  const auto* replacement = frames.back().texture;
  CHECK(replacement != before.texture);
  auto oldFrames = std::size_t{0};
  auto newFrames = std::size_t{0};
  auto swaps = std::size_t{0};
  const auto* previous = before.texture;
  for (const auto& frame : frames)
  {
    CAPTURE(frame.index);
    CHECK(frame.slotted);
    CHECK_FALSE(frame.intrinsic);
    CHECK(frame.texture);
    CHECK((frame.texture == before.texture || frame.texture == replacement));
    if (frame.texture == before.texture)
    {
      CHECK(frame.version == before.version);
      CHECK(newFrames == 0u);
      ++oldFrames;
      result.oldFrames.push_back(frame.index);
    }
    else
    {
      CHECK(frame.version == before.version + 1);
      if (newFrames == 0)
      {
        result.swap = frame.index;
      }
      ++newFrames;
    }
    swaps += frame.texture != previous ? 1 : 0;
    previous = frame.texture;
  }
  CHECK(swaps == 1u);
  CHECK(frames.back().state == ImageSlots::State::Resident);
  result.text = fmt::format("{}: old x {}, new x {}", label, oldFrames, newFrames);
  return result;
}

// the frame log's columns of LPF.1 (FrameLog.cpp)
constexpr auto FeedAppliedColumn = std::size_t{46};
constexpr auto FeedMsColumn = std::size_t{47};
constexpr auto OverrideBytesColumn = std::size_t{48};

/**
 * C44 (P5.13, P5.21): a frame log's frame rows by frame index; every row has the header's
 * 49 fields, the last three the feed's, empty in a start or stop row.
 */
std::map<std::uint64_t, std::vector<std::string>> feedLogFrames(
  const std::filesystem::path& path)
{
  const auto rows = readCsv(path);
  REQUIRE(rows.size() >= 3u);
  REQUIRE(rows[0].size() == 49u);
  CHECK(rows[0][FeedAppliedColumn] == "feed_applied");
  CHECK(rows[0][FeedMsColumn] == "feed_ms");
  CHECK(rows[0][OverrideBytesColumn] == "override_bytes");
  auto frames = std::map<std::uint64_t, std::vector<std::string>>{};
  for (std::size_t i = 1; i < rows.size(); ++i)
  {
    REQUIRE(rows[i].size() == 49u);
    if (rows[i][1] == "frame")
    {
      CHECK_FALSE(rows[i][FeedAppliedColumn].empty());
      frames[std::stoull(rows[i][2])] = rows[i];
    }
    else
    {
      CHECK(rows[i][FeedAppliedColumn].empty());
      CHECK(rows[i][FeedMsColumn].empty());
      CHECK(rows[i][OverrideBytesColumn].empty());
    }
  }
  return frames;
}

// the map text with count small wall boxes below the floor (hidden by it), for a chunk
// batch of that many brushes
std::string feedBatchMapText(const int count)
{
  auto map = lp22aMapText();
  const auto entity1 = map.find("// entity 1");
  auto boxes = std::string{};
  for (auto i = 0; i < count; ++i)
  {
    const auto x = -256 + (i % 50) * 10;
    const auto y = -128 + (i / 50) * 6;
    boxes += boxBrush(x, y, -120, x + 4, y + 4, -116, "lp1b/wall");
  }
  map.insert(map.rfind('}', entity1), boxes);
  return map;
}

} // namespace

// LPF.1 (C40, C43, C44, D98, D100-D102, D106, D110): the live feed on the render side
// over the LP2.2a fixture, headless, with the exposure pinned: a feed publication draws a
// frame at once, the counters keep a frame behind a hand-over from being settled, a
// replaced image never shows the intrinsic, the re-parse reaches the cells, the lights
// and the lists, and the frame log times the feed. Hidden: it needs a GPU and the game's
// blobs.
TEST_CASE("LitSession live feed on a GPU (LPF.1)", "[.][litgpu]")
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
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;

  SECTION("step 1: a feed draws at once, the counters in settled, refusals, the storm")
  {
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto logPath =
        std::filesystem::temp_directory_path() / "TbLitLibTest-lpf1-trigger.csv";
      auto rig = FeedRig{instance, root, logPath};
      auto& service = *rig.service;
      auto& session = *rig.session;

      // today's rule, unchanged: a publication without a feed (a plain lookup's) draws no
      // frame in a settled session
      {
        const auto presented = session.framesPresented();
        static_cast<void>(service.lookup("textures/lpf1/plain_lookup"));
        REQUIRE(service.waitForTableForTesting(
          [](const MaterialTable& table) {
            return table.find("textures/lpf1/plain_lookup") != nullptr;
          },
          10000ms));
        std::this_thread::sleep_for(300ms);
        CHECK(session.framesPresented() == presented);
      }

      // three overrides of the lit floor's diffuse: each draws a frame without a camera
      // move within one pacer slot of its publication, and the session settles again; the
      // probe pixel follows the fed colour through the model
      const auto black = rig.feed({0, 0, 0});
      const auto blackProbe = rig.probe();
      const auto white = rig.feed({255, 255, 255});
      const auto whiteProbe = rig.probe();
      const auto colour = rig.feed({200, 120, 60});
      const auto colourProbe = rig.probe();
      for (const auto* landing : {&black, &white, &colour})
      {
        CHECK(landing->latencyMs < 50.0);
        CHECK_FALSE(landing->frames.empty());
        CHECK(landing->settled);
      }
      checkProbe(blackProbe, whiteProbe, colourProbe);
      // the probe pixel changed to the override's colour: it differs from the fixture's
      // own floor and from the white override
      CHECK(colourProbe.hdr != whiteProbe.hdr);
      CHECK(colourProbe.hdr[1] != rig.firstHdr.at(80, 60)[1]);
      std::cout << fmt::format(
        "LPF.1 feed latency (Debug: publication to the frame's start): black {:.2f} ms, "
        "white {:.2f} ms, colour {:.2f} ms; frames {} {} {}\n",
        black.latencyMs,
        white.latencyMs,
        colour.latencyMs,
        black.frames.size(),
        white.frames.size(),
        colour.frames.size());
      // C44: the frames feed_ms is written on (outside the storm)
      const auto landed = std::vector<std::uint64_t>{
        black.frames.front().index,
        white.frames.front().index,
        colour.frames.front().index};

      // refusals: a "_" name is refused at the hand-over, nothing queued and no count
      // moved; a raw 4096² is refused on the parse thread (its level passes the 32 MB a
      // frame uploads) and publishes once, the slots unchanged: a settled session draws
      // no frame for either
      REQUIRE(waitQuiet(session));
      {
        const auto presented = session.framesPresented();
        const auto requested = service.feedRequested();
        CHECK(service.setImageOverride("_default", constantOverride(4, 4, {1, 2, 3}))
                .is_error());
        CHECK(service.feedRequested() == requested);
        REQUIRE(
          service
            .setImageOverride(rig.floorKey.name, constantOverride(4096, 4096, {9, 9, 9}))
            .is_success());
        REQUIRE(waitForFeed(service));
        std::this_thread::sleep_for(300ms);
        CHECK(
          service.imageOverrideStatus(rig.floorKey.name).state
          == ImageOverrideStatus::State::Refused);
        CHECK(session.framesPresented() == presented);
        CHECK(session.lastFrameSettledForTesting());
      }
      // a frame drawn while a hand-over is queued is not settled (feedBehind), and the
      // refusal's publication, the slots unchanged, draws the frame that settles
      {
        rig.gate->close();
        REQUIRE(
          service
            .setImageOverride(rig.floorKey.name, constantOverride(4096, 4096, {8, 8, 8}))
            .is_success());
        auto behind = std::optional<ChainFrame>{};
        REQUIRE(waitFor(
          [&] {
            auto frame = std::make_shared<ChainFrame>();
            auto done = std::make_shared<std::promise<void>>();
            auto future = done->get_future();
            session.runAfterTonemapForTesting([frame, done](RenderFrameAccess& access) {
              *frame = chainFrameOf(access);
              done->set_value();
            });
            if (future.wait_for(5000ms) != std::future_status::ready)
            {
              return false;
            }
            behind = *frame;
            return true;
          },
          10000ms,
          false));
        CHECK(behind->convergence.feedBehind);
        CHECK_FALSE(behind->settled);
        REQUIRE(waitQuiet(session));
        CHECK_FALSE(session.lastFrameSettledForTesting());
        const auto presented = session.framesPresented();
        const auto item = rig.listener->items();
        rig.gate->open();
        REQUIRE(waitForFeed(service));
        REQUIRE(waitFor(
          [&] { return rig.listener->itemPublication(item).has_value(); },
          5000ms,
          false));
        REQUIRE(waitQuiet(session));
        CHECK(session.lastFrameSettledForTesting());
        CHECK(session.framesPresented() >= presented + 1);
        CHECK(session.framesPresented() <= presented + 2);
      }

      // the feed storm (D96, P5.3): an override every 10 ms for 2 s draws at most the
      // pacer's 30 frames a second; an evaluation and a lookup are answered meanwhile;
      // one frame after the storm's last publication is settled
      REQUIRE(waitQuiet(session));
      auto stormFrames = std::pair<std::uint64_t, std::uint64_t>{};
      {
        const auto lightEntry = service.table()->find("lights/defaultPointLight");
        REQUIRE(lightEntry);
        REQUIRE(lightEntry->entry);
        const auto lightId = MaterialId::of(*lightEntry->entry);
        auto running = std::atomic<bool>{true};
        auto handed = std::atomic<int>{0};
        const auto stormStart = std::chrono::steady_clock::now();
        auto feeder = std::thread{[&] {
          auto i = 0;
          while (std::chrono::steady_clock::now() - stormStart < 2s)
          {
            const auto v = std::uint8_t(40 + (i * 37) % 200);
            if (service
                  .setImageOverride(
                    rig.floorKey.name, constantOverride(16, 16, {v, v, 64}))
                  .is_success())
            {
              ++handed;
            }
            ++i;
            std::this_thread::sleep_for(10ms);
          }
          running = false;
        }};
        std::this_thread::sleep_for(300ms);
        // an evaluation of the light's registers at a new _color, and a new name's lookup
        auto parms = ShaderParms{};
        parms[0] = 0.25f;
        parms[1] = 0.5f;
        parms[2] = 0.75f;
        parms[3] = 1.0f;
        const auto asked = std::chrono::steady_clock::now();
        auto evaluated = std::optional<std::chrono::steady_clock::time_point>{};
        auto looked = std::optional<std::chrono::steady_clock::time_point>{};
        while (running && (!evaluated || !looked))
        {
          if (!evaluated && service.evaluate(lightId, parms, 0.0f))
          {
            evaluated = std::chrono::steady_clock::now();
          }
          if (
            !looked
            && service.lookup("textures/lpf1/storm_lookup").state
                 != MaterialLookup::State::Pending)
          {
            looked = std::chrono::steady_clock::now();
          }
          std::this_thread::sleep_for(2ms);
        }
        feeder.join();
        const auto stormEnd = std::chrono::steady_clock::now();
        CHECK(evaluated);
        CHECK(looked);
        REQUIRE(waitForFeed(service));
        const auto lastItem = rig.listener->items() - 1;
        auto lastPublication = std::optional<std::chrono::steady_clock::time_point>{};
        REQUIRE(waitFor(
          [&] {
            lastPublication = rig.listener->itemPublication(lastItem);
            return lastPublication.has_value();
          },
          5000ms,
          false));
        REQUIRE(waitQuiet(session));
        CHECK(session.lastFrameSettledForTesting());
        auto inStorm = std::vector<FeedFrame>{};
        for (const auto& frame : rig.frames->since(stormStart))
        {
          if (frame.start <= stormEnd)
          {
            inStorm.push_back(frame);
          }
        }
        const auto stormSeconds =
          std::chrono::duration<double>(stormEnd - stormStart).count();
        auto closest = std::chrono::steady_clock::duration::max();
        for (std::size_t i = 1; i < inStorm.size(); ++i)
        {
          closest = std::min(closest, inStorm[i].start - inStorm[i - 1].start);
        }
        // the pacer's floor: 30 frame starts a second at most (the hook's time follows
        // each start by the frame's uploads, so a little jitter)
        CHECK(double(inStorm.size()) <= stormSeconds * 30.0 + 2.0);
        CHECK(inStorm.size() >= 10u);
        CHECK(closest >= 25ms);
        const auto afterStorm = rig.frames->since(*lastPublication);
        CHECK_FALSE(afterStorm.empty());
        CHECK(afterStorm.size() <= 2u);
        REQUIRE_FALSE(inStorm.empty());
        stormFrames = {inStorm.front().index, rig.frames->last()->index};
        std::cout << fmt::format(
          "LPF.1 feed storm: {} hand-overs in {:.2f} s, {} frames ({:.1f} a second, "
          "closest starts {:.1f} ms apart), {} dropped; evaluation answered after {:.1f} "
          "ms, lookup after {:.1f} ms; {} frames after the last publication\n",
          handed.load(),
          stormSeconds,
          inStorm.size(),
          double(inStorm.size()) / stormSeconds,
          std::chrono::duration<double, std::milli>(closest).count(),
          service.stats().droppedFeeds,
          evaluated
            ? std::chrono::duration<double, std::milli>(*evaluated - asked).count()
            : -1.0,
          looked ? std::chrono::duration<double, std::milli>(*looked - asked).count()
                 : -1.0,
          afterStorm.size());
      }

      const auto requested = service.feedRequested();
      rig.close("LPF.1 trigger");

      // C44 (P5.13): feed_ms on exactly the frames an override landed on, outside the
      // storm; feed_applied rising to every hand-over; override_bytes the store's (the
      // storm's last 16 × 16 RGBA8: the 4096² refusals stored nothing)
      const auto log = feedLogFrames(logPath);
      auto timed = std::vector<std::uint64_t>{};
      auto stormTimes = std::vector<double>{};
      auto previous = std::uint64_t{0};
      for (const auto& [index, row] : log)
      {
        const auto applied = std::stoull(row[FeedAppliedColumn]);
        CHECK(applied >= previous);
        previous = applied;
        if (row[FeedMsColumn].empty())
        {
          continue;
        }
        if (index >= stormFrames.first && index <= stormFrames.second)
        {
          stormTimes.push_back(std::stod(row[FeedMsColumn]));
          continue;
        }
        timed.push_back(index);
        CHECK(std::stod(row[FeedMsColumn]) > 0.0);
        CHECK(std::stod(row[FeedMsColumn]) < 1000.0);
      }
      CHECK(timed == landed);
      CHECK_FALSE(stormTimes.empty());
      REQUIRE_FALSE(log.empty());
      CHECK(std::stoull(log.rbegin()->second[FeedAppliedColumn]) == requested);
      CHECK(log.rbegin()->second[OverrideBytesColumn] == "1024");
      std::ranges::sort(stormTimes);
      std::cout << fmt::format(
        "LPF.1 feed_ms (Debug, hand-over to the presented landing frame): black {}, "
        "white {}, colour {}; the storm's {} landings median {:.2f} ms, max {:.2f} ms\n",
        log.at(landed[0])[FeedMsColumn],
        log.at(landed[1])[FeedMsColumn],
        log.at(landed[2])[FeedMsColumn],
        stormTimes.size(),
        stormTimes[stormTimes.size() / 2],
        stormTimes.back());
    }
    CHECK(resetDispatcher());
    messages.print("LPF.1 trigger, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("step 2: Replacing: no frame between two overrides binds the intrinsic")
  {
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto logPath =
        std::filesystem::temp_directory_path() / "TbLitLibTest-lpf1-replacing.csv";
      auto rig = FeedRig{instance, root, logPath};
      auto& session = *rig.session;
      auto report = std::vector<std::string>{};
      // the replacements whose landing frame the log is checked for
      auto timedCases = std::vector<Replacement>{};

      // the model's two probes, the first two replacements of the fixture's floor image
      // (D110), and the first frame's replacement: the fixture's own texture was bound
      const auto beforeBlack = *rig.frames->last();
      const auto black = rig.feed({0, 0, 0});
      report.push_back(
        checkReplacement(black.frames, beforeBlack, "the first override").text);
      const auto blackProbe = rig.probe();
      rig.feed({255, 255, 255});
      const auto whiteProbe = rig.probe();

      // (a) during a chunk batch of 2000 brushes (offered first): the chunks' uploads
      // come first in the frame, the image's after them
      {
        REQUIRE(waitQuiet(session));
        const auto batches = session.geometryBatchesForTesting();
        const auto before = *rig.frames->last();
        REQUIRE(waitFor([&] { return session.snapshotSlotFree(); }, 10000ms, false));
        REQUIRE(session.offerSnapshot(lp22aSnapshot(feedBatchMapText(2000), root, 7, 2)));
        const auto landing = rig.feed(constantOverride(64, 64, {30, 200, 90}), false);
        // the batch's own end draws the frame that settles (geometryTick)
        REQUIRE(waitFor(
          [&] {
            return session.geometryBatchesForTesting() > batches
                   && session.lastFrameSettledForTesting();
          },
          60000ms,
          false));
        REQUIRE(waitQuiet(session));
        auto all = landing.frames;
        for (const auto& frame : rig.frames->since(landing.published))
        {
          if (frame.start > all.back().start)
          {
            all.push_back(frame);
          }
        }
        report.push_back(checkReplacement(all, before, "(a) chunk batch").text);
        // the override's first frame was drawn while the batch was in flight (the frame
        // after it found it so)
        REQUIRE(all.size() >= 2u);
        CHECK(all[1].lastGeometryInFlight);
      }
      // the allocations and the retired textures once the frames in flight are done
      settle(session);
      const auto allocations = session.deviceMemoryAllocations();

      // (b) under a lowered upload cap: the new 1024² chain takes several frames
      {
        session.setUploadCapForTesting(64 * 1024);
        const auto before = *rig.frames->last();
        const auto landing = rig.feed({220, 40, 40}, 1024);
        timedCases.push_back(checkReplacement(landing.frames, before, "(b) 64 KB cap"));
        report.push_back(timedCases.back().text);
        CHECK(landing.frames.size() >= 3u);
        CHECK(landing.settled);
        session.setUploadCapForTesting(UploadBytesPerFrame);
      }

      // (c) the allocation headroom refuses the new texture: the old one stays bound, the
      // frames unconverged, one warning; the room back, it settles
      {
        // the probes' staging let go, so the allocations hold still: the limit puts the
        // next texture within the headroom
        settle(session);
        REQUIRE(waitQuiet(session));
        const auto before = *rig.frames->last();
        const auto warnings =
          rig.events.countContaining(SessionEvent::Warning, "allocations reached");
        session.setAllocationLimitForTesting(std::uint32_t(
          std::uint64_t(session.deviceMemoryAllocations()) + AllocationHeadroom));
        const auto refused = rig.feed(constantOverride(64, 64, {40, 40, 220}), false);
        CHECK_FALSE(refused.settled);
        for (const auto& frame : refused.frames)
        {
          CHECK(frame.texture == before.texture);
          CHECK(frame.state == ImageSlots::State::Replacing);
        }
        CHECK(
          rig.events.countContaining(SessionEvent::Warning, "allocations reached")
          == warnings + 1);
        const auto from = std::chrono::steady_clock::now();
        session.setAllocationLimitForTesting(0);
        session.requestFrame();
        REQUIRE(waitFor(
          [&] {
            const auto last = rig.frames->last();
            return last && last->start >= from
                   && last->state == ImageSlots::State::Resident;
          },
          10000ms,
          false));
        REQUIRE(waitQuiet(session));
        CHECK(session.lastFrameSettledForTesting());
        auto all = refused.frames;
        const auto after = rig.frames->since(from);
        all.insert(all.end(), after.begin(), after.end());
        timedCases.push_back(checkReplacement(all, before, "(c) headroom refusal"));
        report.push_back(timedCases.back().text);
        CHECK(
          rig.events.countContaining(SessionEvent::Warning, "allocations reached")
          == warnings + 1);
      }

      // (d) a third override before the second landed: no level is written meanwhile
      // (cap 0); the unfinished second is let go, the third swapped in, once
      auto finalProbe = FeedProbe{};
      {
        REQUIRE(waitQuiet(session));
        const auto before = *rig.frames->last();
        session.setUploadCapForTesting(0);
        const auto second = rig.feed({250, 180, 20}, 1024);
        const auto third = rig.feed({90, 160, 230}, 1024);
        CHECK_FALSE(second.settled);
        CHECK_FALSE(third.settled);
        const auto from = std::chrono::steady_clock::now();
        session.setUploadCapForTesting(UploadBytesPerFrame);
        session.requestFrame();
        REQUIRE(waitFor(
          [&] {
            const auto last = rig.frames->last();
            return last && last->start >= from
                   && last->state == ImageSlots::State::Resident;
          },
          10000ms,
          false));
        REQUIRE(waitQuiet(session));
        CHECK(session.lastFrameSettledForTesting());
        auto all = second.frames;
        all.insert(all.end(), third.frames.begin(), third.frames.end());
        const auto after = rig.frames->since(from);
        all.insert(all.end(), after.begin(), after.end());
        timedCases.push_back(
          checkReplacement(all, before, "(d) a third before the second"));
        report.push_back(timedCases.back().text);
        // one pixel readback at the end against the model (D110)
        finalProbe = rig.probe();
        checkProbe(blackProbe, whiteProbe, finalProbe);
      }

      // the replaced textures were retired and collected once their frames finished: the
      // allocations are back, nothing is held
      settle(session);
      CHECK(session.deviceMemoryAllocations() == allocations);
      auto retired = std::uint64_t{1};
      auto replacing = std::size_t{1};
      REQUIRE(inFrame(session, [&](RenderFrameAccess& access) {
        retired = access.images.retiredBytes();
        replacing = access.images.replacing();
      }));
      CHECK(replacing == 0u);
      std::cout << fmt::format(
        "LPF.1 Replacing, the floor slot's binding per frame: {}; retired bytes after "
        "the frames in flight {} (a frame's own let-go at most), allocations {} -> {}\n",
        fmt::join(report, "; "),
        retired,
        allocations,
        session.deviceMemoryAllocations());

#if defined(_MSC_VER) && defined(_DEBUG)
      // 100 overrides, each drawn and settled, on the debug heap (the recording off: its
      // vectors would grow)
      {
        auto& service = *rig.service;
        session.setEveryFrameHookForTesting({});
        rig.listener->record(false);
        const auto cycle = [&](const int i) {
          const auto presented = session.framesPresented();
          const auto v = std::uint8_t(20 + (i * 53) % 220);
          REQUIRE(
            service
              .setImageOverride(rig.floorKey.name, constantOverride(64, 64, {v, 90, v}))
              .is_success());
          REQUIRE(waitForFeed(service));
          REQUIRE(waitFor(
            [&] {
              return session.framesPresented() > presented
                     && session.lastFrameSettledForTesting();
            },
            5000ms,
            false));
        };
        // the warm-up fills the service's feed records to their bound
        // (MaterialTable::FeedRecordsKept)
        for (auto i = 0; i < int(MaterialTable::FeedRecordsKept) + 4; ++i)
        {
          cycle(i);
        }
        const auto growth = heapGrowth([&] {
          for (auto i = 0; i < 100; ++i)
          {
            cycle(i);
          }
        });
        std::cout << fmt::format(
          "LPF.1 100 override frames on the debug heap: {} blocks, {} bytes\n",
          growth.blocks,
          growth.bytes);
        CHECK(growth.blocks <= 64);
        settle(session);
        CHECK(session.deviceMemoryAllocations() == allocations);
      }
#endif

      rig.close("LPF.1 Replacing");

      // C44 (P5.13): feed_ms is written on the frame of the swap, the first whose slot is
      // Resident with every level written, and not on the frames that bound the old
      // texture meanwhile (the superseded second image of (d) never lands)
      const auto log = feedLogFrames(logPath);
      auto times = std::vector<std::string>{};
      for (const auto& replacement : timedCases)
      {
        CAPTURE(replacement.text);
        REQUIRE(log.contains(replacement.swap));
        CHECK_FALSE(log.at(replacement.swap)[FeedMsColumn].empty());
        for (const auto index : replacement.oldFrames)
        {
          REQUIRE(log.contains(index));
          CHECK(log.at(index)[FeedMsColumn].empty());
        }
        times.push_back(log.at(replacement.swap)[FeedMsColumn]);
      }
      std::cout << fmt::format(
        "LPF.1 feed_ms at the swap frames (b), (c), (d): {} ms\n",
        fmt::join(times, ", "));
    }
    CHECK(resetDispatcher());
    messages.print("LPF.1 Replacing, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("step 4: the re-parse through the render side; Stop with a feed queued")
  {
    {
      const auto instance = TestInstance{library, true, false, messages};
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto logPath =
        std::filesystem::temp_directory_path() / "TbLitLibTest-lpf1-reparse.csv";
      auto rig = FeedRig{instance, root, logPath};
      auto& service = *rig.service;
      auto& session = *rig.session;
      // inside the floor's brush, looking down at its bottom face (z -16) from behind
      const auto inside = litCamera({60, 0, -8}, {0, 0, -1}, {1, 0, 0});
      const auto insideBefore = settledHdr(session, inside, 0);
      REQUIRE(insideBefore);
      // one-sided: the face is culled, nothing behind it
      CHECK(insideBefore->first.brightest(80, 60) == 0.0f);
      REQUIRE(settledHdr(session, feedCamera(), 1));
      const auto baseline = rig.probe(false);

      // (i) the floor's material two-sided: its cells built again with back sides, the
      // lists once more for the revision (and once per batch that applies), the registers
      // kept through it (D52: no frame skips the floor's draws), the picture above the
      // floor unchanged, the back face below it lit
      const auto batches = session.geometryBatchesForTesting();
      const auto twoSided = rig.reparse("textures/lp1b/floor", FloorTwoSided);
      const auto batchesApplied = session.geometryBatchesForTesting() - batches;
      const auto afterTwoSided = rig.probe(false);
      auto rebuilds = std::size_t{0};
      auto tableRebuilds = std::size_t{0};
      {
        // the stats a frame's hook saw are its predecessor's; the probe's frames follow
        const auto all = rig.frames->since(twoSided.front().start);
        for (std::size_t k = 1; k < all.size(); ++k)
        {
          CAPTURE(all[k].index);
          CHECK(all[k].lastSkippedDraws == 0u);
          CHECK(all[k].lastInteractionDraws > 0u);
          CHECK_FALSE(all[k].intrinsic);
          rebuilds += all[k].lastListsRebuilt ? 1 : 0;
          tableRebuilds += all[k].lastListsTableChanged ? 1 : 0;
        }
      }
      // the lists' key moved once for the re-parse (ListsTableKey); the other rebuilds
      // are the batch's chunks, its light triangles and lights
      CHECK(tableRebuilds == 1u);
      CHECK(rebuilds >= 2u);
      CHECK(batchesApplied >= 1u);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(afterTwoSided.hdr[c] == Catch::Approx(baseline.hdr[c]).epsilon(1e-3));
      }
      const auto insideAfter = settledHdr(session, inside, 0);
      REQUIRE(insideAfter);
      CHECK(insideAfter->first.brightest(80, 60) > 0.0f);
      std::cout << fmt::format(
        "LPF.1 re-parse twoSided: {} frames, lists built {} times ({} for the table's "
        "key) with {} batches applied; inside the floor brush: brightest {} -> {:.4f}, "
        "interaction draws {} -> {}, prepass draws {} -> {}\n",
        twoSided.size(),
        rebuilds,
        tableRebuilds,
        batchesApplied,
        insideBefore->first.brightest(80, 60),
        insideAfter->first.brightest(80, 60),
        insideBefore->second.stats.interactionDraws,
        insideAfter->second.stats.interactionDraws,
        insideBefore->second.stats.prepassDraws,
        insideAfter->second.stats.prepassDraws);
      REQUIRE(settledHdr(session, feedCamera(), 1));

      // (ii) the light's texture material (lp1b_light has none: idLight::Spawn's default,
      // lights/squarelight1) at half its colour: the light derived again (a new record
      // version), the floor darker; the re-parse cleared: the light as before, the
      // picture back
      const auto lightBefore = namedLight(*session.lightTable(), "lp1b_light");
      REQUIRE(lightBefore);
      const auto half = rig.reparse("lights/squarelight1", HalfSquareLight);
      const auto lightAfter = namedLight(*session.lightTable(), "lp1b_light");
      REQUIRE(lightAfter);
      CHECK(lightAfter->version > lightBefore->version);
      const auto halfProbe = rig.probe(false);
      // the floor's probe is lp1b_light's alone: half
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(halfProbe.hdr[c] == Catch::Approx(0.5f * baseline.hdr[c]).epsilon(1e-3));
      }
      const auto restored = rig.reparse("lights/squarelight1", std::nullopt);
      const auto restoredProbe = rig.probe(false);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(restoredProbe.hdr[c] == Catch::Approx(baseline.hdr[c]).epsilon(1e-3));
      }
      CHECK(
        namedLight(*session.lightTable(), "lp1b_light")->version > lightAfter->version);
      std::cout << fmt::format(
        "LPF.1 re-parse of the light's material: record version {} -> {}, the floor's "
        "HDR green {:.5f} -> {:.5f} -> {:.5f} (cleared)\n",
        lightBefore->version,
        lightAfter->version,
        baseline.hdr[1],
        halfProbe.hdr[1],
        restoredProbe.hdr[1]);

      // (iii) a re-parse the stage bound refuses (M-ID-GUARD-05: 129 alpha-tested
      // shortcuts) of a material out of the view (the patches'): published Failed, the
      // picture unchanged, the session settled again (feedBehind cleared)
      {
        auto refused = std::string{"{\n"};
        for (auto i = 0; i < 129; ++i)
        {
          refused += "\tdiffusemap \"_white alphaTest 0.5\"\n";
        }
        refused += "}\n";
        const auto frames = rig.reparse("textures/lp1b/patch", refused);
        CHECK_FALSE(frames.empty());
        const auto slot = service.table()->find("textures/lp1b/patch");
        REQUIRE(slot);
        CHECK_FALSE(slot->entry);
        const auto probe = rig.probe(false);
        CHECK_FALSE(probe.frame.convergence.feedBehind);
        CHECK(probe.frame.settled);
        CHECK(probe.hdr == restoredProbe.hdr);
      }

      // (iv) Stop with a feed queued and a slot Replacing: no level is written (cap 0)
      // and the next hand-over is held at the gate; the teardown is clean
      session.setUploadCapForTesting(0);
      const auto replacing = rig.feed(constantOverride(1024, 1024, {10, 20, 30}), true);
      CHECK_FALSE(replacing.settled);
      CHECK(replacing.frames.back().state == ImageSlots::State::Replacing);
      rig.gate->close();
      REQUIRE(
        service.setImageOverride(rig.floorKey.name, constantOverride(64, 64, {1, 2, 3}))
          .is_success());
      REQUIRE(waitFor(
        [&] { return service.feedRequested() > service.table()->feedApplied; },
        5000ms,
        false));
      // the re-parses' landing frames, for the log
      const auto twoSidedFrames =
        std::vector<std::uint64_t>{twoSided.front().index, twoSided.back().index};
      const auto halfFrames =
        std::vector<std::uint64_t>{half.front().index, half.back().index};
      const auto restoredFrames =
        std::vector<std::uint64_t>{restored.front().index, restored.back().index};
      const auto stopStart = std::chrono::steady_clock::now();
      rig.close("LPF.1 re-parse");
      std::cout << fmt::format(
        "LPF.1 Stop with a feed queued and a slot Replacing: {:.1f} ms\n",
        std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - stopStart)
          .count());

      // C44 (P5.13): feed_ms once per re-parse, on the frame its lists and cells landed:
      // after the frame that found the twoSided batch in flight
      const auto log = feedLogFrames(logPath);
      const auto timedIn = [&](const std::vector<std::uint64_t>& range) {
        auto timed = std::vector<std::pair<std::uint64_t, double>>{};
        for (const auto& [index, row] : log)
        {
          if (index >= range[0] && index <= range[1] && !row[FeedMsColumn].empty())
          {
            timed.emplace_back(index, std::stod(row[FeedMsColumn]));
          }
        }
        return timed;
      };
      const auto twoSidedTimed = timedIn(twoSidedFrames);
      REQUIRE(twoSidedTimed.size() == 1u);
      if (twoSided.size() >= 2 && twoSided[1].lastGeometryInFlight)
      {
        CHECK(twoSidedTimed.front().first > twoSided.front().index);
      }
      const auto halfTimed = timedIn(halfFrames);
      const auto restoredTimed = timedIn(restoredFrames);
      CHECK(halfTimed.size() == 1u);
      CHECK(restoredTimed.size() == 1u);
      std::cout << fmt::format(
        "LPF.1 feed_ms of the re-parses (Debug): twoSided {:.3f} ms at frame {} (the "
        "publication's first frame {}), the light's {:.3f} ms, cleared {:.3f} ms\n",
        twoSidedTimed.front().second,
        twoSidedTimed.front().first,
        twoSided.front().index,
        halfTimed.empty() ? -1.0 : halfTimed.front().second,
        restoredTimed.empty() ? -1.0 : restoredTimed.front().second);
    }
    CHECK(resetDispatcher());
    messages.print("LPF.1 re-parse, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
}

} // namespace tb::lit

#endif
