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
#include "lit/EntityState.h"
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
         {&FrameConvergence::probesInFlight, false},
         // LP7.2 (D124): the table that answers is the trigger
         {&FrameConvergence::modelsPending, false},
         // LP3.0 (D151, C62): the shadows' term (LP3.3's), whose triggers are the still
         // timer, the pipeline's onReady and the light triangles' job
         {&FrameConvergence::shadowsPending, false}})
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
  // LP3.0 (D151), LP3.3: a frame without shadows leaves both terms false
  CHECK_FALSE(FrameConvergence{}.shadowsPending);
  CHECK_FALSE(LitFrameStats{}.shadowsPending);
  CHECK_FALSE(LitFrameStats{}.shadowSettleWanted);
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

  // LP3.4 (D158, P3.25, Q40): the lights the atlas left unshadowed: none, no clause and
  // the tooltip the line; two, the clause after the lights drawn and their names on the
  // tooltip's second line
  auto shadowed = flowed;
  shadowed.lightsVisible = 43;
  shadowed.lightsDrawn = 43;
  CHECK(sessionStatusLine(shadowed, {}).find("unshadowed") == std::string::npos);
  CHECK(sessionStatusTooltip(shadowed, {}) == sessionStatusLine(shadowed, {}));
  shadowed.lightsUnshadowed = 2;
  shadowed.unshadowedLights = {"lp3p_h11_00", "lp3p_h11_21"};
  CHECK(
    sessionStatusLine(shadowed, programs)
    == "settled | 43 of 43 lights drawn, 2 unshadowed (atlas full) | 9 draws | VRAM 1.0 "
       "MB | area -1, 8 of 8 areas | disabled: builtin/post/histogram (no shader blob "
       "builtin/post/histogram.cs.bin)");
  CHECK(
    sessionStatusTooltip(shadowed, programs)
    == sessionStatusLine(shadowed, programs) + "\nunshadowed: lp3p_h11_00, lp3p_h11_21");
  // with an overflowed light too: after it
  shadowed.lightsOverflowed = 1;
  CHECK(
    sessionStatusLine(shadowed, {})
      .starts_with(
        "settled | 43 of 43 lights drawn, 1 overflowed, 2 unshadowed (atlas full) |"));
  // before the first frame: "starting", no names
  CHECK(sessionStatusTooltip({}, {}) == "starting");
}

} // namespace tb::lit

// Hidden GPU tests: they need a Vulkan driver and, for the window section, a desktop. The
// Vulkan loader is loaded at runtime here, as Qt does in the editor (C9); the test
// executable never links it. Three tags, each suite under 180 s of wall clock in Debug
// (LP5.0's split, P6.24; LP7.0's third tag, D134, C56): "[.][litgpu]" holds LP0-LP2's
// cases (on a GPU, pipelines, GPU resources, lit passes, tonemap and blit);
// "[.][litgpu2]" LP4's and LPF's (ambient pass, light-grid path, live feed);
// "[.][litgpu3]" LP5's (the programs and layouts, math and ambientLight lights, pass 8,
// the cube and sky texgens, the provisional sizes, fog and blend lights, the material
// reload) and LP7's until it passes 150 s, then a fourth. LPF.2's host cases are
// "[.][feed]" (tst_Feed.cpp). Every case makes its own instance, device and session,
// bounds every wait (H.1, 19-LITGPU-HANG.md) and leaves the dispatcher reset: none
// depends on another having run, in any suite or in any order.

#ifdef _WIN32

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

// the frame log's fields (lit/FrameLog.cpp, frameLogHeader): LP5.3's 57, LP7.0's
// proc_bytes and the eleven model, dress and adoption counts (C52, P7.20), LP7.3's
// inlined_surfaces (C54), LP3.1's pass 6 and caster columns (D157), LP3.2's per-list
// shadow-atlas interaction draws, LP3.4's perforated_draws (D155), LP6.0's ten split
// columns of the CPU record (C65)
constexpr auto FrameLogFields = std::size_t{105};

// LP2.0, T0.2: the validation layer's id of "Vertex attribute at location N not consumed
// by vertex shader" (mirror.json M-PIPE-DEFECT-01), as this machine's layer names it
constexpr auto OutputNotConsumedId = std::string_view{"WARNING-Shader-OutputNotConsumed"};

// LP3.1 (C59): the layer's id of "the fragment shader writes to output Location 0 but
// there is no pColorAttachments[0]": builtin/depth's colour write in the depth-only
// shadow-atlas framebuffer, as the engine's own pipelines of it have it (mirror.json
// M-SHADOW-DEFECT-01)
constexpr auto DepthOnlyOutputId =
  std::string_view{"Undefined-Value-ShaderOutputNotConsumed"};

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
  // LP3.1: while mirroredMessageId is set, the warnings of DepthOnlyOutputId
  std::atomic<int> mirroredDepthOnly = 0;

  // LPF.2: the product instance's message sink (VulkanInstanceOptions::messages)
  void receive(const VulkanMessage& message)
  {
    if (!message.error && !mirroredMessageId.empty() && message.id == mirroredMessageId)
    {
      ++mirrored;
      return;
    }
    // LP3.1 (mirror.json M-SHADOW-DEFECT-01): the engine's depth-only shadow-atlas
    // pipelines too, counted apart
    if (!message.error && !mirroredMessageId.empty() && message.id == DepthOnlyOutputId)
    {
      ++mirroredDepthOnly;
      return;
    }
    if (message.error)
    {
      ++errors;
    }
    else
    {
      ++warnings;
    }
    const auto lock = std::scoped_lock{mutex};
    texts.push_back(message.text);
  }

  void print(const std::string_view label)
  {
    const auto lock = std::scoped_lock{mutex};
    std::cout << label << ": validation errors " << errors << ", warnings " << warnings
              << ", mirrored engine warnings " << mirrored << " (" << mirroredDepthOnly
              << " of the shadow atlas's depth-only pipelines)\n";
    for (const auto& text : texts)
    {
      std::cout << "  " << text << "\n";
    }
  }
};

// The test's Vulkan instance (LPF.2: lit::VulkanInstance, D107), as QVulkanInstance
// creates one: API 1.2, the platform's surface extensions when asked for, the validation
// layer if present with every message of one id reported (T0.2), each message counted
// in messages through the product's message sink.
VulkanInstance testInstance(
  const bool validation, const bool surfaceExtensions, ValidationMessages& messages)
{
  auto options = VulkanInstanceOptions{};
  options.validation = validation;
  options.surface = surfaceExtensions;
  options.duplicateMessageLimit = RaisedDuplicateMessageLimit;
  options.messages = [&messages](const VulkanMessage& message) {
    messages.receive(message);
  };
  options.applicationName = "TbLitLibTest";
  return VulkanInstance::create(std::move(options))
         | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
}

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
// device (one handle); LitTestUtils' processCounts, a new thread failing only when its
// start is not an OS worker's (LP5.3 step 0)
constexpr auto HandleTolerance = 8L;
constexpr auto DriverHandlesPerSession = 1L;

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

/**
 * LP3.3 (D151, C62, A49): stillTimerFirings, the still timer's firings the case expects:
 * 0 on every fixture whose lights cast nothing or whose camera never leaves a held LOD, a
 * pending cascade budget or a held layout behind it (no case's frame counts move); none
 * (not checked, printed) for the shadow cases that move their camera on purpose
 */
void teardown(
  LitSession& session,
  const std::optional<std::uint64_t> stillTimerFirings = std::uint64_t{0})
{
  if (stillTimerFirings)
  {
    CHECK(session.stillTimerFiringsForTesting() == *stillTimerFirings);
  }
  else
  {
    std::cout << "the still timer fired " << session.stillTimerFiringsForTesting()
              << " times\n";
  }
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

  SECTION("headless: 20 cycles of create, clear with a timer query, teardown")
  {
    const auto validation = GENERATE(false, true);
    auto messages = ValidationMessages{};
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-headless-frame-log.csv";
    std::filesystem::remove(logPath);
    auto deviceName = std::string{};

    {
      const auto instance = testInstance(validation, false, messages);
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
      REQUIRE(rows[i].size() == FrameLogFields);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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

      const auto instance = testInstance(true, true, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));

      const auto createSurface =
        instance.get<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
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
          instance.get<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            "vkGetPhysicalDeviceSurfaceSupportKHR")(
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
      instance.get<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(
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
      REQUIRE(rows[i].size() == FrameLogFields);
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

  SECTION("D71: pipelines made on the compile thread while the render thread draws")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    {
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
  const VulkanInstance& instance,
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
TEST_CASE("LitSession LP5's programs and layouts on a GPU (LP5.0)", "[.][litgpu3]")
{
  const auto* gamePath = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePath || !*gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pipelines need the engine's shader blobs");
  }

  // LP5.0 (C45-C49, D121): the compile thread makes the pipelines of the programs LP5
  // binds, a material program's among them (EngineMaterialProgramBase + its place), and
  // the binding layouts of the nine new layout types, on the device with validation on;
  // nothing is drawn with them yet (the picture is LP5.1's and later)
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));

    auto events = Events{};
    auto options = SessionOptions{};
    options.validation = true;
    options.gamePath = gamePath;
    options.requestStartPipelines = false;
    auto session =
      LitSession::create(
        instance.handles(), VK_NULL_HANDLE, 64, 48, options, events.sink(), 1)
      | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    auto* compiler = session->pipelineCompilerForTesting();
    REQUIRE(compiler);

    const auto depth =
      session->deviceFacts().d24s8Supported ? nvrhi::Format::D24S8 : nvrhi::Format::D32S8;
    const auto hdr = FramebufferFormats{nvrhi::Format::RGBA16_FLOAT, depth};
    const auto* voronoi = findEngineMaterialProgram("vornoipattern");
    const auto* heathaze = findEngineMaterialProgram("heatHaze.vfp");
    REQUIRE(voronoi);
    REQUIRE(heathaze);
    // the programs and the number of binding layouts of their layout type
    const auto programs = std::vector<std::pair<int, std::size_t>>{
      {18, 3},                  // TEXGEN
      {21, 4},                  // DRAW_INTERACTION
      {55, 3},                  // DEFAULT
      {57, 3},                  // NORMAL_CUBE
      {65, 3},                  // BLENDLIGHT
      {67, 3},                  // FOG
      {69, 3},                  // LEGACY
      {70, 3},                  // LEGACY
      {voronoi->row.index, 2},  // POST_PROCESS_INGAME
      {heathaze->row.index, 2}, // POST_PROCESS_INGAME
    };
    auto keys = std::vector<PipelineKey>{};
    for (const auto& [program, layouts] : programs)
    {
      keys.push_back(
        PipelineKey{PipelineRecipe::Draw, program, engine_gls::Default, 0, 0.0f, hdr});
    }
    using L = EngineBindingLayout;
    const auto layoutTypes = std::vector<L>{
      L::Default,
      L::DefaultSkinned,
      L::TextureSkinned,
      L::Texgen,
      L::Legacy,
      L::NormalCube,
      L::Fog,
      L::Blendlight,
      L::PostProcessIngame,
      L::DrawInteractionSkinned,
    };
    for (const auto& key : keys)
    {
      compiler->graphicsPipeline(key);
    }
    for (const auto layout : layoutTypes)
    {
      compiler->layoutsOf(layout);
    }
    CHECK(waitFor(
      [&] {
        for (const auto& key : keys)
        {
          if (
            compiler->graphicsPipeline(key).state
            == GraphicsPipelineLookup::State::Pending)
          {
            return false;
          }
        }
        for (const auto layout : layoutTypes)
        {
          if (compiler->layoutsOf(layout).state == BindingLayoutLookup::State::Pending)
          {
            return false;
          }
        }
        return true;
      },
      30000ms,
      false));

    for (std::size_t i = 0; i < keys.size(); ++i)
    {
      const auto lookup = compiler->graphicsPipeline(keys[i]);
      CAPTURE(keys[i].program, lookup.error);
      CHECK(lookup.state == GraphicsPipelineLookup::State::Ready);
      CHECK(lookup.pipeline);
      CHECK(lookup.bindingLayouts.size() == programs[i].second);
    }
    for (const auto layout : layoutTypes)
    {
      const auto lookup = compiler->layoutsOf(layout);
      CAPTURE(int(layout), lookup.error);
      CHECK(lookup.state == BindingLayoutLookup::State::Ready);
      CHECK(lookup.layouts.size() == engineBindingLayoutDescs(layout, false)->size());
    }
    auto voronoiReported = false;
    for (const auto& program : session->programReport())
    {
      CAPTURE(program.name, program.reason);
      CHECK(program.enabled);
      voronoiReported = voronoiReported || program.name == "vornoipattern";
    }
    CHECK(voronoiReported);

    teardown(*session);
    CHECK(session->lastDeviceReleaseCount() == 0u);
    CHECK(session->nvrhiErrorCount() == 0u);
    CHECK(session->deviceMemoryAllocations() == 0);
    CHECK(events.count(SessionEvent::Error) == 0u);
    CHECK(events.count(SessionEvent::Warning) == 0u);
    events.print("LP5 programs");
  }
  CHECK(resetDispatcher());
  messages.print("LP5 programs, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
  // M-PIPE-DEFECT-01: the vertex attributes a vertex shader does not read are the one
  // mirrored warning id, counted apart
  std::cout << "LP5 programs: mirrored warnings " << messages.mirrored << std::endl;
}

TEST_CASE("LitSession GPU resources on a GPU (LP2.1)", "[.][litgpu]")
{

  SECTION("C14, C28: the frame resources, and 100 resizes back at the baseline")
  {
    auto messages = ValidationMessages{};
    {
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      if (
        row.size() == FrameLogFields && row[1] == "frame" && !row[15].empty()
        && row[15] != "0")
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
    const auto instance = testInstance(true, false, messages);
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
    if (
      row.size() == FrameLogFields && row[1] != "event" && !row[14].empty()
      && row[0] != "session")
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
// only) into a test game root; LP4.2a: and the ambient pass's probe path; LP3.1 (C60):
// and the shadow-atlas interactions (one pair of blob files for the six), which the cases
// that run with shadows draw with
constexpr auto Lp2Programs = std::array<int, 22>{
  EngineBuiltinColor,
  EngineBuiltinTextureVertexColor,
  EngineBuiltinInteraction,
  // LP5.1 (C45, D114): an ambientLight light's
  EngineBuiltinInteractionAmbient,
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
  EngineBuiltinAmbientLightgridIblPbr,
  EngineBuiltinInteractionShadowAtlasSpot,
  EngineBuiltinInteractionShadowAtlasPoint,
  EngineBuiltinInteractionShadowAtlasParallel,
  EngineBuiltinPbrInteractionShadowAtlasSpot,
  EngineBuiltinPbrInteractionShadowAtlasPoint,
  EngineBuiltinPbrInteractionShadowAtlasParallel,
  // LP3.4 (D158): the shadow atlas debug view's (builtin/texture)
  EngineBuiltinTextured};

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
      // LP3.1: the shadow-atlas programs share their files; each is copied once
      if (std::filesystem::exists(to))
      {
        continue;
      }
      std::filesystem::create_directories(to.parent_path());
      std::filesystem::copy_file(from, to);
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
  // LP5.F: the frame's own convergence (D77), as its tonemap passes left it
  FrameConvergence convergence;
};

/**
 * Waits until a frame of camera has nothing pending in its own lists (no pending
 * pipeline, image, chunk upload, material or evaluation; every slot answered and
 * written; the lists hold drawSurfs) and, with productSettled, is settled by the
 * product's rule too (its FrameConvergence, D77); reads its HDR target back through a
 * staging texture copied after the tonemap passes (LP5.F: the frame's own convergence
 * is at hand there); nullopt at the deadline.
 */
std::optional<std::pair<HdrImage, SettledFrame>> frameHdr(
  LitSession& session,
  const CameraState& camera,
  const std::size_t minLights,
  const std::chrono::milliseconds timeout,
  const bool productSettled)
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
    session.runAfterTonemapForTesting([&, done](RenderFrameAccess& access) {
      const auto& stats = access.passes.lastStats();
      const auto& lists = access.passes.lists();
      frame.stats = stats;
      frame.drawSurfs = lists.drawSurfs.size();
      frame.lights = lists.lights.size();
      frame.convergence = access.convergence;
      settled = stats.camera == camera && !stats.unconverged
                && access.images.pending() == 0 && !access.images.hasLoading()
                && !lists.drawSurfs.empty() && lists.lights.size() >= minLights
                && !access.arenas.pending()
                && (!productSettled || access.convergence.settled());
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

// the HDR target of the first frame of camera with nothing pending in its lists that
// the product calls settled too (frameHdr; LP5.F: a test accepts no frame the product
// does not)
std::optional<std::pair<HdrImage, SettledFrame>> settledHdr(
  LitSession& session,
  const CameraState& camera,
  const std::size_t minLights,
  const std::chrono::milliseconds timeout = 60000ms)
{
  return frameHdr(session, camera, minLights, timeout, true);
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
/**
 * LP5.2a (D118, P6.9): the LP2.2a fixture's lp1b/glass is a blend-only SL_AMBIENT
 * material, and the models of the cases on this root exclude pass 8: their sessions skip
 * the shader passes (the parity test's switch, r_skipAmbient 1's stand-in) unless a case
 * asks for them (shaderPasses).
 */
std::unique_ptr<LitSession> rootSession(
  const VulkanInstance& instance,
  Events& events,
  MaterialService& service,
  const std::filesystem::path& root,
  const bool startPipelines = true,
  const bool shaderPasses = false,
  const bool shadows = false)
{
  auto options = SessionOptions{};
  options.validation = true;
  options.gamePath = root;
  options.requestStartPipelines = startPipelines;
  // LP3.1 (C57): r_skipShadows 1 unless a case runs with shadows: the fixture's eight
  // lights cast, and these cases' models are the unshadowed interaction's (SC733)
  options.skipShadowsForTesting = !shadows;
  auto session =
    LitSession::create(
      instance.handles(), VK_NULL_HANDLE, 160, 120, options, events.sink(), 7, &service)
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  session->skipShaderPassesForTesting(!shaderPasses);
  return session;
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

  SECTION("the fixture map lit: probes, D76, the perforated prepass, the lists' order")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    auto programsAsked = std::vector<int>{};
    {
      const auto instance = testInstance(true, false, messages);
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

      // LP5.0 (P6.16, C46): r_skipInteractions gates the interactions alone
      // (RenderBackend.cpp:4181): with them skipped the frame records the other passes,
      // the ambient pass with as many draws as with them, and the probe pixel is the
      // ambient-only picture of LP4.2a's model without a bake: black, alpha 1 (a camera
      // of its own: a frame that began before the switch cannot be taken)
      session->skipInteractionsForTesting(true);
      const auto ambientOnly =
        settledHdr(*session, litCamera({60, 0, 79}, {0, 0, -1}, {1, 0, 0}), 1);
      REQUIRE(ambientOnly);
      CHECK(ambientOnly->second.stats.interactionsSkipped);
      CHECK(ambientOnly->second.stats.interactionDraws == 0u);
      CHECK_FALSE(ambientOnly->second.stats.interactionsTimed);
      CHECK(ambientOnly->second.stats.prepassDraws == litFrame.stats.prepassDraws);
      CHECK(ambientOnly->second.stats.ambientDraws == litFrame.stats.ambientDraws);
      CHECK(ambientOnly->second.stats.ambientDraws > 0u);
      CHECK(ambientOnly->second.stats.ambientTimed);
      const auto ambientProbe = ambientOnly->first.at(80, 60);
      CHECK(ambientProbe[0] == 0.0f);
      CHECK(ambientProbe[1] == 0.0f);
      CHECK(ambientProbe[2] == 0.0f);
      CHECK(ambientProbe[3] == 1.0f);
      CHECK(ambientOnly->first.brightest(80, 60) == 0.0f);
      session->skipInteractionsForTesting(false);
      std::cout << fmt::format(
        "LP5.0 (P6.16) interactions skipped: {} ambient draws ({} with them), probe "
        "{} {} {} {}\n",
        ambientOnly->second.stats.ambientDraws,
        litFrame.stats.ambientDraws,
        ambientProbe[0],
        ambientProbe[1],
        ambientProbe[2],
        ambientProbe[3]);

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
        const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      // LP3.1 (C57): with shadows: a stop at a pass boundary (pass 6 among them)
      auto session = rootSession(instance, events, *service, root, true, false, true);
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
// LP5.2a (D118): as rootSession, the shader passes skipped unless asked for
std::unique_ptr<LitSession> loggedRootSession(
  const VulkanInstance& instance,
  Events& events,
  MaterialService& service,
  const std::filesystem::path& root,
  const std::filesystem::path& frameLogPath,
  const bool shaderPasses = false)
{
  auto options = SessionOptions{};
  options.validation = true;
  options.gamePath = root;
  options.frameLogPath = frameLogPath;
  // LP3.1 (C57): r_skipShadows 1: the fixture's lights cast, and these cases' models and
  // counts are the unshadowed interaction's (SC733)
  options.skipShadowsForTesting = true;
  auto session =
    LitSession::create(
      instance.handles(), VK_NULL_HANDLE, 160, 120, options, events.sink(), 7, &service)
    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
  session->skipShaderPassesForTesting(!shaderPasses);
  return session;
}

double fieldValue(const std::vector<std::string>& row, const std::size_t column)
{
  return row.size() > column && !row[column].empty() ? std::stod(row[column]) : 0.0;
}

// LP6.0 (C65): the frame log's columns of the CPU record and its split: cpu_record_ms,
// LP3.1's shadow_cpu_ms (pass 6), LP3.0's caster_ms, and LP6.0's ten after
// perforated_draws, lists_ms ... record_other_ms
constexpr auto CpuRecordMsColumn = std::size_t{3};
constexpr auto ShadowCpuMsColumn = std::size_t{82};
constexpr auto CasterMsColumn = std::size_t{92};
constexpr auto ListsMsColumn = std::size_t{95};
constexpr auto RecordOtherMsColumn = std::size_t{104};

/**
 * LP6.0 (C65): a frame row's CPU record split: the ten columns are numbers, none below 0;
 * they and shadow_cpu_ms sum to cpu_record_ms within 0.05 ms (eleven fields, each rounded
 * to 0.001 ms); the caster gathering's time is inside the lists'.
 */
void checkCpuSplit(const std::vector<std::string>& row)
{
  REQUIRE(row.size() == FrameLogFields);
  auto sum = fieldValue(row, ShadowCpuMsColumn);
  for (auto c = ListsMsColumn; c <= RecordOtherMsColumn; ++c)
  {
    CAPTURE(c);
    REQUIRE_FALSE(row[c].empty());
    CHECK(fieldValue(row, c) >= 0.0);
    sum += fieldValue(row, c);
  }
  CAPTURE(row[2]);
  CHECK(std::abs(sum - fieldValue(row, CpuRecordMsColumn)) <= 0.05);
  CHECK(fieldValue(row, ListsMsColumn) >= fieldValue(row, CasterMsColumn));
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

  SECTION(
    "three HDR values tonemap as the shaders compute them, the exposure is its clamped "
    "target from the first frame, and the blit copies the LDR target")
  {
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    auto limitRaised = false;
    {
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      // LP3.0 (D151), LP3.3: the still timer, 200 ms after that change, did not fire (the
      // root's lights draw no shadow here: skipShadowsForTesting, so no settle is wanted)
      CHECK(session->stillTimerFiringsForTesting() == 0u);
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
    REQUIRE(rows[0].size() == FrameLogFields);
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
    // LP6.0 (C65): the CPU record's ten split columns after perforated_draws
    CHECK(rows[0][ListsMsColumn] == "lists_ms");
    CHECK(rows[0][ListsMsColumn + 1] == "prepass_cpu_ms");
    CHECK(rows[0][RecordOtherMsColumn - 2] == "upload_ms");
    CHECK(rows[0][RecordOtherMsColumn] == "record_other_ms");
    auto lastLit = std::optional<std::vector<std::string>>{};
    auto settledRows = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      REQUIRE(rows[i].size() == FrameLogFields);
      if (rows[i][1] != "frame")
      {
        continue;
      }
      CHECK((rows[i][34] == "0" || rows[i][34] == "1"));
      // LP6.0 (C65): every frame's split sums to its record time, the settled ones too
      checkCpuSplit(rows[i]);
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
    std::cout << fmt::format(
      "LP2.2b fixture settled frame's CPU record split (LP6.0): lists {}, prepass {}, "
      "ambient {}, shadows {}, interactions {}, stages {}, fog {}, tonemap {}, uploads "
      "{}, "
      "submit {}, other {} ms\n",
      (*lastLit)[ListsMsColumn],
      (*lastLit)[ListsMsColumn + 1],
      (*lastLit)[ListsMsColumn + 2],
      (*lastLit)[ShadowCpuMsColumn],
      (*lastLit)[ListsMsColumn + 3],
      (*lastLit)[ListsMsColumn + 4],
      (*lastLit)[ListsMsColumn + 5],
      (*lastLit)[ListsMsColumn + 6],
      (*lastLit)[ListsMsColumn + 7],
      (*lastLit)[ListsMsColumn + 8],
      (*lastLit)[RecordOtherMsColumn]);
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
      const auto instance = testInstance(true, true, messages);
      REQUIRE(instance.valid());
      limitRaised = instance.messageLimitRaised();
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto createSurface =
        instance.get<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
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
        // LP3.1 (C57): r_skipShadows 1: a case about the window's present, whose
        // fixture lights cast
        options.skipShadowsForTesting = true;
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
        // LP5.2a (D118): the case's draws and keys exclude pass 8 (the fixture's glass)
        session->skipShaderPassesForTesting(true);
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
      instance.get<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(
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
        const auto instance = testInstance(true, false, messages);
        REQUIRE(instance.valid());
        REQUIRE(initInstanceDispatcher(instance.handles()));
        auto service = rootService(root);
        auto events = Events{};
        // LP3.1 (C57): with shadows: the thread and handle baseline with the shadow
        // pass's objects
        auto session = rootSession(instance, events, *service, root, true, false, true);
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
      // LP5.3 step 0: the library's own threads, exactly (none outlives the teardown)
      CHECK(LitSession::liveThreadsForTesting() == 0);
    }
    const auto maxHandles =
      baseline.handles + HandleTolerance + cycles * DriverHandlesPerSession;
    auto counts = processCounts();
    const auto settled = waitFor(
      [&] {
        counts = processCounts();
        return newForeignThreads(baseline, counts).empty()
               && counts.handles <= maxHandles;
      },
      3000ms,
      false);
    std::cout << "U2.12: " << cycles << " blob-backed sessions: threads "
              << baseline.threads << " -> " << counts.threads
              << " (new outside the OS workers: "
              << describeThreads(newForeignThreads(baseline, counts))
              << "; new in all: " << describeThreads(newThreads(baseline, counts))
              << "), handles " << baseline.handles << " -> " << counts.handles
              << " (allowed " << maxHandles << ")" << (settled ? "" : " (not settled)")
              << "\n";
    CHECK(newForeignThreads(baseline, counts).empty());
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
      const auto instance = testInstance(true, false, messages);
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
        if (row.size() != FrameLogFields || row[1] != "frame")
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      // LP3.1 (C57): with shadows: a stop at a pass boundary (pass 6 among them)
      auto session = rootSession(instance, events, *service, root, true, false, true);
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
  // LP5.3 (C49, D123): the portal sides the flow found fogged out
  std::size_t portalsFogged = 0;
  // LP7.2 (C52): the scene's model entities, instances, Pending records and skinned
  // entities when the frame was drawn
  std::uint64_t modelEntities = 0;
  std::size_t modelInstances = 0;
  std::uint64_t modelsPending = 0;
  std::uint64_t modelsSkinned = 0;
  // LP7.3 (C54): the scene's dress totals when the frame was drawn
  std::uint64_t dressIdentified = 0;
  std::uint64_t dressUnmatched = 0;
  std::uint64_t inlinedSurfaces = 0;
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
      frame->lists.portalsFogged = lists.portalsFogged;
      frame->lists.modelEntities = access.models.entities;
      frame->lists.modelInstances = access.instances.size();
      frame->lists.modelsPending = access.models.pending;
      frame->lists.modelsSkinned = access.models.skinned;
      frame->lists.dressIdentified = access.dress.surfaces;
      frame->lists.dressUnmatched = access.dress.unmatched;
      frame->lists.inlinedSurfaces = access.dress.inlined;
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

// LP3.2 (D157 extended, SC709): the shadow counters a parity row's "expect" can pin (a
// number or {"min": n}), by key, from the settled frame's stats; none for another key.
// LP3.3 (C61, D146): tiles_drawn, shadow_draws and atlas_bytes_written are the layout's
// (the tiles the settled frame samples, with the draws and bytes each was last drawn
// with), so that a row's counts do not depend on whether its settled frame drew its tiles
// or kept them from an earlier frame of the row (LP3.2's counts, every tile drawn every
// frame, are these)
std::optional<std::size_t> parityShadowCount(
  const LitFrameStats& lit, const std::string_view key)
{
  const auto counts = std::array<std::pair<std::string_view, std::size_t>, 11>{{
    {"lights_casting", lit.lightsCasting},
    {"lights_placed", lit.lightsPlaced},
    {"lights_unplaced", lit.lightsUnplaced},
    {"tiles_drawn", lit.layoutTiles},
    {"shadow_draws", lit.layoutShadowDraws},
    {"perforated_skipped", lit.perforatedCastersSkipped},
    // LP3.4 (D156, K53): the settled frame's own provisional tiles (0)
    {"tiles_provisional", lit.tilesProvisional},
    {"atlas_bytes_written", std::size_t(lit.layoutAtlasBytes)},
    {"sm_draws_local", lit.smDrawsLocal},
    {"sm_draws_global", lit.smDrawsGlobal},
    {"sm_draws_translucent", lit.smDrawsTranslucent},
  }};
  const auto found =
    std::ranges::find_if(counts, [&](const auto& count) { return count.first == key; });
  return found != counts.end() ? std::optional{found->second} : std::nullopt;
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
 * LP4.2b (C38, P4.24): a parity row's switches for as long as the row is drawn: an
 * ambient-only row ("ambient_only": true, the engine's shot with r_skipInteractions 1)
 * skips the interactions, every other row draws them. LP5.0 (D118, P6.9): a row's
 * "skip_ambient" (its map's r_skipAmbient as the cvar block printed it) skips the shader
 * passes when 1; a rows.json written before the field (the three maps shot with
 * r_skipAmbient 1, SWITCHES) is read as 1. Both reset to drawn when the row ends.
 * skipInteractions and skipShaderPasses are the session's ...ForTesting.
 */
class ParityRowSwitch
{
private:
  std::function<void(bool)> m_skipInteractions;
  std::function<void(bool)> m_skipShaderPasses;

public:
  ParityRowSwitch(
    const JsonValue& row,
    std::function<void(bool)> skipInteractions,
    std::function<void(bool)> skipShaderPasses)
    : m_skipInteractions{std::move(skipInteractions)}
    , m_skipShaderPasses{std::move(skipShaderPasses)}
  {
    m_skipInteractions(row.flag("ambient_only"));
    m_skipShaderPasses(row.numberOr("skip_ambient", 1.0) == 1.0);
  }

  ~ParityRowSwitch()
  {
    m_skipInteractions(false);
    m_skipShaderPasses(false);
  }

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
// ambient-only row skips the interactions while it is drawn, every row ends with them on.
// LP5.0 (D118, P6.9): skip_ambient 1 skips the shader passes, 0 draws them, a row
// without it (a rows.json of the maps shot before LP5.0) skips them
TEST_CASE("LitSession parity rows: the ambient-only switch (LP4.2b)")
{
  const auto record =
    JsonReader{
      R"({"rows": [{"id": "A01", "ambient_only": false, "skip_ambient": 1},)"
      R"( {"id": "A01a", "ambient_only": true, "skip_ambient": 1},)"
      R"( {"id": "S01", "ambient_only": false, "skip_ambient": 0}, {"id": "R01"}]})"}
      .value();
  REQUIRE(record);
  const auto* rows = record->find("rows");
  REQUIRE(rows);
  REQUIRE(rows->array.size() == 4u);
  auto calls = std::vector<bool>{};
  auto during = std::vector<bool>{};
  auto shaderCalls = std::vector<bool>{};
  auto shaderDuring = std::vector<bool>{};
  for (const auto& row : rows->array)
  {
    const auto rowSwitch = ParityRowSwitch{
      row,
      [&](const bool skip) { calls.push_back(skip); },
      [&](const bool skip) { shaderCalls.push_back(skip); }};
    during.push_back(calls.back());
    shaderDuring.push_back(shaderCalls.back());
  }
  CHECK(during == std::vector<bool>{false, true, false, false});
  CHECK(
    calls == std::vector<bool>{false, false, true, false, false, false, false, false});
  CHECK(shaderDuring == std::vector<bool>{true, true, false, true});
  CHECK(
    shaderCalls
    == std::vector<bool>{true, false, true, false, false, false, true, false});
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
 * there, and the areas of the room centres its volume is built to reach. LP5.1 (C45): the
 * .json and "expect" gain the interaction phases' counters (texture_lights, math_lights,
 * ambient_lights). LP5.2a (C46, C50): and the stage pass's (stage_draws, stage_skipped);
 * a control of mode "row" is not black (its unlit stages draw); LP5.2b (C48): and
 * stage_pending; LP7.2 (C52): and the models' (model_draws, models_pending,
 * models_skinned); LP7.3 (C54): and the dress decals' (dress_surfaces, the dress surfaces
 * the frame drew; dress_unmatched, the scene's func_decals without a fragment surface;
 * the .json also the scene's identified surfaces and inlined copies); LP3.2 (D157
 * extended, SC709): and pass 6's counters (lights_casting, lights_placed,
 * lights_unplaced, tiles_drawn, shadow_draws, perforated_skipped, atlas_bytes_written)
 * and the per-list shadow-atlas interaction draws (sm_draws_local, sm_draws_global,
 * sm_draws_translucent), the .json also the atlas's unshadowed lights by name. Hidden: it
 * needs a GPU and the root of a parity.py run.
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

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  // per row: the frames it presented, from and to
  auto frameRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>{};
  {
    const auto instance = testInstance(true, false, messages);
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
      // LP5.0 (D118): and the shader passes as the row's skip_ambient says
      const auto rowSwitch = ParityRowSwitch{
        row,
        [owner](const bool skip) { owner->skipInteractionsForTesting(skip); },
        [owner](const bool skip) { owner->skipShaderPassesForTesting(skip); }};
      CHECK(
        session->shaderPassesSkippedForTesting()
        == (row.numberOr("skip_ambient", 1.0) == 1.0));
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
      // LP3.2 (D157 extended, SC709, P3.10): pass 6's counters a row can pin, the
      // per-list shadow-atlas interaction draws, and the lights the atlas left unshadowed
      // (their "name" keys, in viewLights order: H11's two, D162); LP3.3 (C61, D146): the
      // tiles, draws and bytes are the layout's, as parityShadowCount's
      auto unplaced = std::vector<std::string>{};
      for (const auto& name : lit.unplacedLights)
      {
        unplaced.push_back(fmt::format("\"{}\"", name));
      }
      const auto shadowStats = fmt::format(
        "\"lights_casting\": {}, \"lights_placed\": {}, \"lights_unplaced\": {}, "
        "\"tiles_drawn\": {}, \"shadow_draws\": {}, \"perforated_skipped\": {}, "
        "\"tiles_provisional\": {}, \"atlas_bytes_written\": {}, \"sm_draws_local\": {}, "
        "\"sm_draws_global\": {}, \"sm_draws_translucent\": {}, \"unplaced_lights\": "
        "[{}]",
        lit.lightsCasting,
        lit.lightsPlaced,
        lit.lightsUnplaced,
        lit.layoutTiles,
        lit.layoutShadowDraws,
        lit.perforatedCastersSkipped,
        lit.tilesProvisional,
        lit.layoutAtlasBytes,
        lit.smDrawsLocal,
        lit.smDrawsGlobal,
        lit.smDrawsTranslucent,
        fmt::join(unplaced, ", "));
      writeTextFile(
        out / (id + ".json"),
        fmt::format(
          "{{\"width\": {}, \"height\": {}, \"settle_ms\": {:.1f}, \"frames\": {}, "
          "\"lights_visible\": {}, \"lights_drawn\": {}, \"texture_lights\": {}, "
          "\"math_lights\": {}, \"ambient_lights\": {}, \"prepass_draws\": {}, "
          "\"interaction_draws\": {}, \"session_ms\": {:.1f}, \"exposure\": {:.9g}, "
          "\"exposure_model\": {:.9g}, \"rows_written\": \"{}\", \"order\": {}, "
          "\"view_area\": {}, \"areas_flowed\": {}, \"areas\": {}, \"lights_in_view\": "
          "{}, "
          "\"portal_chains\": {}, \"flow_ms\": {:.4f}, \"scissor_smaller_than_view\": "
          "{}, \"ambient_only\": {}, \"ambient_draws\": {}, \"probe_candidates\": {}, "
          "\"grid_surfaces\": {}, \"ambient_grid_surfaces\": {}, \"entity_grid_areas\": "
          "{{{}}}, "
          "\"entity_grid_area\": {}, \"stage_draws\": {}, \"stage_skipped\": {}, "
          "\"stage_pending\": {}, \"fog_draws\": {}, \"blend_draws\": {}, "
          "\"fog_lights\": {}, "
          "\"blend_lights\": {}, \"portals_fogged\": {}, \"model_entities\": {}, "
          "\"model_instances\": {}, \"model_draws\": {}, \"models_pending\": {}, "
          "\"models_skinned\": {}, \"dress_surfaces\": {}, \"dress_unmatched\": {}, "
          "\"dress_identified\": {}, \"inlined_surfaces\": {}, {}, \"view_lights\": "
          "[{}]}}\n",
          width,
          height,
          settleMs,
          session->framesPresented() - framesBefore,
          lit.lightsVisible,
          lit.lightsDrawn,
          lit.textureLights,
          lit.mathLights,
          lit.ambientLights,
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
          lit.stageDraws,
          lit.stageSkipped,
          lit.stagePending,
          lit.fogDraws,
          lit.blendDraws,
          lit.fogLights,
          lit.blendLights,
          settledRow->lists.portalsFogged,
          settledRow->lists.modelEntities,
          settledRow->lists.modelInstances,
          lit.modelDraws,
          settledRow->lists.modelsPending,
          settledRow->lists.modelsSkinned,
          lit.dressSurfaces,
          settledRow->lists.dressUnmatched,
          settledRow->lists.dressIdentified,
          settledRow->lists.inlinedSurfaces,
          shadowStats,
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
          else if (key == "texture_lights")
          {
            // LP5.1 (C45): pc.c_textureLights, pc.c_mathLights and the preview's own
            // count of the ambientLight lights
            CHECK(lit.textureLights == std::size_t(value.number));
          }
          else if (key == "math_lights")
          {
            CHECK(lit.mathLights == std::size_t(value.number));
          }
          else if (key == "ambient_lights")
          {
            CHECK(lit.ambientLights == std::size_t(value.number));
          }
          else if (key == "stage_draws")
          {
            // LP5.2a (C46): the stage pass's draws and its SL_AMBIENT stages that drew
            // nothing
            CHECK(lit.stageDraws == std::size_t(value.number));
          }
          else if (key == "stage_skipped")
          {
            CHECK(lit.stageSkipped == std::size_t(value.number));
          }
          else if (key == "stage_pending")
          {
            // LP5.2b (C48, D124): the surfaces skipped as pending and the cube draws made
            // with the zero cube (0 on a settled frame)
            CHECK(lit.stagePending == std::size_t(value.number));
          }
          else if (key == "fog_draws")
          {
            // LP5.3 (C49): pass 9's fog draws (the lists' surfaces and the cubes) and
            // blend draws
            CAPTURE(lit.fogDraws);
            CHECK(parityCountHolds(value, lit.fogDraws));
          }
          else if (key == "blend_draws")
          {
            // a count or a minimum (F05: the preview draws two surfaces where the
            // picture, the gate, matches the engine's)
            CAPTURE(lit.blendDraws);
            CHECK(parityCountHolds(value, lit.blendDraws));
          }
          else if (key == "scissor_smaller_than_view")
          {
            CHECK(smallerScissor == value.boolean);
          }
          else if (key == "probe_candidates")
          {
            CHECK(lit.probeCandidates == std::size_t(value.number));
          }
          else if (key == "model_draws")
          {
            // LP7.2 (C52): the frame's draws of model instances (a count or a minimum),
            // no model entity Pending at the settled frame, the skinned entities drawn
            // as nothing (K41)
            CAPTURE(lit.modelDraws);
            CHECK(parityCountHolds(value, lit.modelDraws));
          }
          else if (key == "models_pending")
          {
            CHECK(settledRow->lists.modelsPending == std::uint64_t(value.number));
          }
          else if (key == "models_skinned")
          {
            CHECK(settledRow->lists.modelsSkinned == std::uint64_t(value.number));
          }
          else if (key == "dress_surfaces")
          {
            // LP7.3 (C54): the dress surfaces the frame drew (a count or a minimum), the
            // scene's func_decals without a fragment surface
            CAPTURE(lit.dressSurfaces);
            CHECK(parityCountHolds(value, lit.dressSurfaces));
          }
          else if (key == "dress_unmatched")
          {
            CHECK(settledRow->lists.dressUnmatched == std::uint64_t(value.number));
          }
          else if (const auto count = parityShadowCount(lit, key))
          {
            // LP3.2 (D157 extended, SC709): pass 6's counters (lights casting, placed and
            // unplaced by the atlas, tiles, shadow draws, perforated casters skipped,
            // atlas bytes: the LOD) and the per-list shadow-atlas interaction draws, a
            // count or a minimum
            CAPTURE(*count);
            CHECK(parityCountHolds(value, *count));
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
      // copy of its material there (V2.8). LP5.2a (C50, P6.1): a control of mode "row"
      // draws the row's unlit stages, compared by parity.py under the row's own class
      const auto* controlMode = row.find("control_mode");
      const auto rowControl = controlMode && controlMode->kind == JsonValue::Kind::String
                              && controlMode->string == "row";
      if (control && !rowControl)
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
  // LP6.0 (C65): every frame's CPU record split sums to its record time; the rows with
  // casters time their gathering inside the lists
  {
    const auto logRows = readCsv(frameLog);
    REQUIRE(logRows.size() > 1u);
    REQUIRE(logRows[0].size() == FrameLogFields);
    CHECK(logRows[0][ListsMsColumn] == "lists_ms");
    CHECK(logRows[0][RecordOtherMsColumn] == "record_other_ms");
    auto frameRows = std::size_t{0};
    auto gathered = std::size_t{0};
    for (std::size_t i = 1; i < logRows.size(); ++i)
    {
      if (logRows[i].size() > 1u && logRows[i][1] == "frame")
      {
        checkCpuSplit(logRows[i]);
        ++frameRows;
        gathered += fieldValue(logRows[i], CasterMsColumn) > 0.0 ? 1u : 0u;
      }
    }
    std::cout << fmt::format(
      "parity frame log (LP6.0): {} frames' CPU record split checked, {} with caster_ms "
      "above 0\n",
      frameRows,
      gathered);
    CHECK(frameRows > 0u);
    CHECK(gathered > 0u);
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
TEST_CASE("LitSession ambient pass on a GPU (LP4.2a)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pass needs the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});

  SECTION("no bake: every probe defaulted, the pass black over the prepass's black")
  {
    const auto logPath =
      std::filesystem::temp_directory_path() / "TbLitLibTest-lp42a-frame-log.csv";
    std::filesystem::remove(logPath);
    auto messages = ValidationMessages{};
    messages.mirroredMessageId = OutputNotConsumedId;
    {
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      auto options = SessionOptions{};
      options.validation = true;
      options.gamePath = root;
      options.frameLogPath = logPath;
      // LP3.1 (C57): r_skipShadows 1: the floor's model is the unshadowed interaction's
      options.skipShadowsForTesting = true;
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
      // LP5.2a (D118): the model excludes pass 8 (the fixture's glass)
      session->skipShaderPassesForTesting(true);
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
    REQUIRE(rows[0].size() == FrameLogFields);
    CHECK(rows[0][42] == "ambient_draws");
    CHECK(rows[0][43] == "gpu_ambient_ms");
    auto timed = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      if (
        rows[i].size() == FrameLogFields && rows[i][1] == "frame" && !rows[i][42].empty()
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      // LP3.1 (C57): with shadows: the stop comes before pass 6
      auto session = rootSession(instance, events, *service, root, true, false, true);
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
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      auto events = Events{};
      // LP3.1 (C57): with shadows: the shadow pass's per-frame work on the debug heap too
      auto session = rootSession(instance, events, *service, root, true, false, true);
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
TEST_CASE("LitSession light-grid path on a GPU (LP4.3)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the pass needs the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  {
    const auto instance = testInstance(true, false, messages);
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
  // LP5.G: per registry build (a reload, the poll's changed decl file), likewise
  std::vector<std::size_t> reloadStarts;

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

  // LP5.G addendum: the publications so far, and the time of the nth (0 the first)
  std::size_t publicationCount()
  {
    const auto lock = std::scoped_lock{mutex};
    return publications.size();
  }

  std::optional<std::chrono::steady_clock::time_point> publication(const std::size_t nth)
  {
    const auto lock = std::scoped_lock{mutex};
    return nth < publications.size() ? std::optional{publications[nth]} : std::nullopt;
  }

  std::size_t reloads()
  {
    const auto lock = std::scoped_lock{mutex};
    return reloadStarts.size();
  }

  // LP5.G: the time of the nth build's publication (a build publishes once), once made
  std::optional<std::chrono::steady_clock::time_point> reloadPublication(
    const std::size_t nth)
  {
    const auto lock = std::scoped_lock{mutex};
    if (nth >= reloadStarts.size() || reloadStarts[nth] >= publications.size())
    {
      return std::nullopt;
    }
    return publications[reloadStarts[nth]];
  }
};

// a material service over a game root as rootService's, whose listener is listener's; a
// closed feedGate holds every Feed and Reparse item before it runs. LP5.G: pollInterval
// on, the poll's changed decl file builds the registry again after autoReloadInterval
std::unique_ptr<MaterialService> feedService(
  const std::filesystem::path& root,
  const std::shared_ptr<FeedListener>& listener,
  const std::shared_ptr<ParseGate>& feedGate,
  const std::chrono::milliseconds pollInterval = 0ms)
{
  auto options = MaterialServiceOptions{};
  options.makeSource = [root](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
  };
  options.pollInterval = pollInterval;
  if (pollInterval > 0ms)
  {
    options.autoReloadInterval = 100ms;
  }
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
    if (kind == "reload")
    {
      const auto lock = std::scoped_lock{listener->mutex};
      if (listener->recording)
      {
        listener->reloadStarts.push_back(listener->publications.size());
      }
      return;
    }
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
  // the hook's time: after the frame's snapshot, acquire and uploads
  std::chrono::steady_clock::time_point start;
  // LP7.5 step 0: the frame's true start, the pacer's own stamp (RenderFrameAccess::
  // frameStart), before any of them
  std::chrono::steady_clock::time_point paced;
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
    frame.paced = access.frameStart;
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
    const VulkanInstance& instance,
    const std::filesystem::path& root,
    const std::filesystem::path& logPath,
    const std::chrono::milliseconds pollInterval = 0ms)
  {
    service = feedService(root, listener, gate, pollInterval);
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

// the frame log's columns of LPF.1 (FrameLog.cpp), and LP7.0's last ones
constexpr auto FeedAppliedColumn = std::size_t{46};
constexpr auto FeedMsColumn = std::size_t{47};
constexpr auto OverrideBytesColumn = std::size_t{48};
constexpr auto ProcBytesColumn = std::size_t{57};
constexpr auto ModelEntitiesColumn = std::size_t{58};
// LP7.4 (C55): the adoption's four counts are drawn from the scene from here on
constexpr auto AdoptedIslandsColumn = std::size_t{65};
constexpr auto CurvedIslandsColumn = std::size_t{68};

/**
 * C44 (P5.13, P5.21): a frame log's frame rows by frame index; every row has the header's
 * FrameLogFields fields, the feed's three empty in a start or stop row; LP7.0: LP7's
 * eleven counts empty in a start or stop row and, in a frame row, the model and dress
 * counts 0 and (LP7.4) the adoption counts numbers, proc_bytes empty in a frame row.
 */
std::map<std::uint64_t, std::vector<std::string>> feedLogFrames(
  const std::filesystem::path& path)
{
  const auto rows = readCsv(path);
  REQUIRE(rows.size() >= 3u);
  REQUIRE(rows[0].size() == FrameLogFields);
  CHECK(rows[0][FeedAppliedColumn] == "feed_applied");
  CHECK(rows[0][FeedMsColumn] == "feed_ms");
  CHECK(rows[0][OverrideBytesColumn] == "override_bytes");
  CHECK(rows[0][ProcBytesColumn] == "proc_bytes");
  CHECK(rows[0][ModelEntitiesColumn] == "model_entities");
  CHECK(rows[0][CurvedIslandsColumn] == "curved_islands");
  auto frames = std::map<std::uint64_t, std::vector<std::string>>{};
  for (std::size_t i = 1; i < rows.size(); ++i)
  {
    REQUIRE(rows[i].size() == FrameLogFields);
    if (rows[i][1] == "frame")
    {
      CHECK_FALSE(rows[i][FeedAppliedColumn].empty());
      CHECK(rows[i][ProcBytesColumn].empty());
      for (auto k = ModelEntitiesColumn; k < AdoptedIslandsColumn; ++k)
      {
        CHECK(rows[i][k] == "0");
      }
      for (auto k = AdoptedIslandsColumn; k <= CurvedIslandsColumn; ++k)
      {
        CHECK_FALSE(rows[i][k].empty());
        CHECK(rows[i][k].find_first_not_of("0123456789") == std::string::npos);
      }
      frames[std::stoull(rows[i][2])] = rows[i];
    }
    else
    {
      CHECK(rows[i][FeedAppliedColumn].empty());
      CHECK(rows[i][FeedMsColumn].empty());
      CHECK(rows[i][OverrideBytesColumn].empty());
      for (auto k = ModelEntitiesColumn; k <= CurvedIslandsColumn; ++k)
      {
        CHECK(rows[i][k].empty());
      }
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

namespace
{

// LP5.1 (C45): the light shaders of the math, ambientLight and held-image lights, written
// into the root beside LP2.2a's stand-ins
const auto Lp51Materials = std::string{
  "lights/lp51/ambient\n{\n\tambientLight\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/lp51/held\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap lights/lp51/held.tga\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  // LP5.3 (C50): the sweep's fog light, as fogs/basicFog (fogs.mtr:14-22), and a blend
  // light adding its colour
  "lights/lp51/fog\n{\n\tfogLight\n\tnoShadows\n\t{\n\t\tmap _fog\n\t\tcolored\n\t}\n}\n"
  "lights/lp51/blend\n{\n\tblendLight\n\tnoShadows\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tblend add\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"};

// LP4.2a's slabs (lp42aMapText) without the fixture's lights, and these light entities
std::string lp51MapText(const std::vector<std::string>& lights)
{
  const auto map = lp42aMapText();
  auto out = std::string{};
  auto start = std::size_t{0};
  auto index = 0;
  while (start < map.size())
  {
    const auto next = map.find("// entity ", start + 1);
    const auto block = map.substr(start, next == std::string::npos ? next : next - start);
    if (block.find(R"("classname" "light")") == std::string::npos)
    {
      out += block;
      ++index;
    }
    start = next == std::string::npos ? map.size() : next;
  }
  for (const auto& light : lights)
  {
    out +=
      fmt::format("// entity {}\n{{\n\"classname\" \"light\"\n{}}}\n", index++, light);
  }
  return out;
}

// a light entity's keys at the slab's (60 0 2), 48 above it, radius 100
std::string lp51Light(
  const std::string& name, const std::string& keys, const float colour)
{
  return fmt::format(
    "\"name\" \"{}\"\n\"origin\" \"60 0 50\"\n\"light_radius\" \"100 100 100\"\n"
    "\"_color\" \"{} {} {}\"\n{}",
    name,
    colour,
    colour,
    colour,
    keys);
}

// MLFalloffCurve( t, MLF_INVSQUARE_CLAMPED, k, inner ) (mathlight.inc.hlsl, the curves):
// the Karis-windowed inverse square
float modelInvSquareClamped(const float t, const float k, const float inner)
{
  const auto u = std::clamp((t - inner) / std::max(1.0f - inner, 1e-3f), 0.0f, 1.0f);
  const auto w = std::clamp(1.0f - u * u * u * u, 0.0f, 1.0f);
  const auto d = u * k + 1e-3f;
  return (w * w) / (d * d + 1.0f);
}

// the HDR of a frame whose lists hold, after its passes, what holds says, read back
// through a staging texture as settledHdr reads one; nullopt at the deadline
std::optional<std::pair<HdrImage, SettledFrame>> hdrOfFrame(
  LitSession& session,
  const std::function<bool(const RenderFrameAccess&)>& holds,
  const std::chrono::milliseconds timeout = 10000ms)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.tableChanged();
    auto staging = nvrhi::StagingTextureHandle{};
    auto frame = SettledFrame{};
    auto taken = false;
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterPassesForTesting([&, done](RenderFrameAccess& access) {
      frame.stats = access.passes.lastStats();
      frame.drawSurfs = access.passes.lists().drawSurfs.size();
      frame.lights = access.passes.lists().lights.size();
      taken = holds(access);
      if (taken)
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
    if (!taken)
    {
      std::this_thread::sleep_for(20ms);
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

/**
 * The parse thread's gate for the sweep: MaterialService calls pass() before it waits for
 * its next work item, so a thread that passed an open gate takes the next item whatever
 * the gate says since; close() then park() gives it one item of work and returns once it
 * waits at the gate, so that no item queued after park() is taken until open().
 */
struct ParkingGate
{
  std::mutex mutex;
  std::condition_variable changed;
  bool closed = false;
  bool parked = false;

  void pass()
  {
    auto lock = std::unique_lock{mutex};
    parked = closed;
    changed.notify_all();
    changed.wait(lock, [&] { return !closed; });
    parked = false;
  }

  void close()
  {
    const auto lock = std::scoped_lock{mutex};
    closed = true;
  }

  // work: a request that gives the parse thread one item (an evaluation not made yet)
  bool park(const std::function<void()>& work)
  {
    work();
    auto lock = std::unique_lock{mutex};
    return changed.wait_for(lock, 10s, [&] { return parked; });
  }

  void open()
  {
    {
      const auto lock = std::scoped_lock{mutex};
      closed = false;
    }
    changed.notify_all();
  }
};

// opens a parking gate when the test leaves
struct OpenParkingAtExit
{
  std::shared_ptr<ParkingGate> gate;
  ~OpenParkingAtExit() { gate->open(); }
};

// the 5 x 5 patch around (80, 60), its red channel
std::vector<float> litPatch(const HdrImage& image)
{
  auto patch = std::vector<float>{};
  for (std::uint32_t y = 58; y <= 62; ++y)
  {
    for (std::uint32_t x = 78; x <= 82; ++x)
    {
      patch.push_back(image.at(x, y)[0]);
    }
  }
  return patch;
}

float meanOf(const std::vector<float>& values)
{
  auto sum = 0.0;
  for (const auto value : values)
  {
    sum += double(value);
  }
  return values.empty() ? 0.0f : float(sum / double(values.size()));
}

} // namespace

// LP5.1 (C45, D112, D114; C50, P6.7): the second interaction phase's math lights, an
// ambientLight light through interactionAmbient, and the 60-step light-colour sweep with
// lagging evaluations, over LP4.2a's slabs with the fixture's lights replaced by one
// light of each kind in turn, against CPU models of the blobs (interaction.ps.hlsl with
// mathlight.inc.hlsl's MLEvaluate, interactionAmbient.ps.hlsl), the exposure pinned as
// the parity test pins it. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession math and ambientLight lights on a GPU (LP5.1)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto base = root / "base";
  writeTextFile(base / "materials" / "zz_lp51.mtr", Lp51Materials);
  // LP5.3 (C50): the sweep's fog and blend lights' programs beyond LP2's
  for (const auto* name : {"builtin/fog/fog", "builtin/fog/blendlight"})
  {
    for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
      const auto gamePath = std::filesystem::path{gamePathValue};
      const auto relative = shaderBlobPath(name, stage);
      REQUIRE(std::filesystem::exists(gamePath / "base" / relative));
      std::filesystem::create_directories((base / relative).parent_path());
      std::filesystem::copy_file(
        gamePath / "base" / relative,
        base / relative,
        std::filesystem::copy_options::overwrite_existing);
    }
  }
  writeTextFile(base / "materials" / "zz_lp42a.mtr", Lp42aMaterials);
  std::filesystem::create_directories(base / "textures" / "lp42a");
  std::filesystem::create_directories(base / "lights" / "lp51");
  {
    auto file = std::ofstream{base / "textures" / "lp42a" / "flat.tga", std::ios::binary};
    const auto bytes = test::tgaFile(test::flat(4, 4, 200, 120, 60));
    file.write(bytes.data(), std::streamsize(bytes.size()));
  }
  for (const auto* name : {"second.tga", "metal.tga", "metal_s.tga"})
  {
    auto file = std::ofstream{base / "textures" / "lp42a" / name, std::ios::binary};
    const auto bytes = test::tgaFile(test::flat(4, 4, 128, 128, 128));
    file.write(bytes.data(), std::streamsize(bytes.size()));
  }
  {
    auto file = std::ofstream{base / "lights" / "lp51" / "held.tga", std::ios::binary};
    const auto bytes = test::tgaFile(test::flat(4, 4, 255, 255, 255));
    file.write(bytes.data(), std::streamsize(bytes.size()));
  }
  const auto camera = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
  const auto frameLog = root / "lp51.framelog.csv";
  std::filesystem::remove(frameLog);

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  auto programsAsked = std::set<int>{};
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    // the parse thread's gate: the sweep withholds evaluations by holding it
    auto gate = std::make_shared<ParkingGate>();
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    serviceOptions.imageBudgetBytes = 0;
    serviceOptions.beforeTakeWorkForTesting = [gate] { gate->pass(); };
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto atExit = OpenParkingAtExit{gate};
    auto events = Events{};
    auto options = SessionOptions{};
    options.validation = true;
    options.gamePath = root;
    options.frameLogPath = frameLog;
    // LP3.1 (C57): r_skipShadows 1: the math and ambientLight lights' models are the
    // unshadowed interaction's
    options.skipShadowsForTesting = true;
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
    // LP5.2a (D118): the models exclude pass 8 (the fixture's glass)
    session->skipShaderPassesForTesting(true);
    // the exposure pinned as the parity test pins it (Q21, parity.py --pinned)
    auto tonemap = EngineTonemap;
    tonemap.hdrMinLuminance = 0.1f;
    tonemap.hdrMaxLuminance = 0.1001f;
    session->setTonemapForTesting(tonemap);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));

    auto sequence = std::uint64_t{0};
    const auto offer = [&](const std::vector<std::string>& lights) {
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(
        session->offerSnapshot(lp22aSnapshot(lp51MapText(lights), root, 7, ++sequence)));
    };
    // a settled frame of the camera after its tonemap passes (LP4.2a's readback)
    const auto shoot = [&] {
      auto readback = std::optional<AmbientReadback>{};
      for (auto attempt = 0;
           attempt < 20
           && !(
             readback && readback->frame.settled && readback->frame.lit.camera == camera);
           ++attempt)
      {
        REQUIRE(settledHdr(*session, camera, 1));
        readback = ambientReadback(*session);
      }
      REQUIRE(readback);
      REQUIRE(readback->frame.settled);
      return std::move(*readback);
    };
    // the programs the interaction draws of one more settled frame use
    const auto drawnPrograms = [&] {
      auto logged = std::size_t{0};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        logged = access.passes.drawLogForTesting().size();
      }));
      REQUIRE(settledHdr(*session, camera, 1));
      auto programs = std::set<int>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        const auto& log = access.passes.drawLogForTesting();
        for (auto i = logged; i < log.size(); ++i)
        {
          if (log[i].pass == LitDrawRecord::Pass::Interaction)
          {
            programs.insert(log[i].program);
          }
        }
        access.passes.setDrawLogForTesting(false);
        access.passes.setDrawLogForTesting(true);
      }));
      programsAsked.insert(programs.begin(), programs.end());
      return programs;
    };
    // the Lambert term at a point of the slab's top (normal +z; _flat's xy within
    // 1e-3): the tangent-space light vector to the one light's globalLightOrigin
    const auto lambertAt = [&](const std::array<float, 3>& p) {
      auto origin = EngineVec3{};
      const auto table = session->lightTable();
      REQUIRE(table);
      table->records.forEach([&](const PreviewId&, const auto& record) {
        if (record)
        {
          origin = record->derived.projection.globalLightOrigin;
        }
      });
      const auto d =
        std::array<float, 3>{origin[0] - p[0], origin[1] - p[1], origin[2] - p[2]};
      return std::clamp(
        d[2] / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 0.0f, 1.0f);
    };

    // the model's inputs: the flat slab's albedo as the service holds its image
    const auto flatKey = stageImage(*service, "textures/lp42a/flat", 2);
    REQUIRE(flatKey);
    const auto flatTexel = firstTexel(*service, *flatKey);
    REQUIRE(flatTexel);
    const auto albedo = modelDiffuseMap(*flatTexel);
    // the gate closed with the parse thread waiting at it: an evaluation of the flat
    // slab's material it has not made, its parm 11 counting, is its one item first
    auto parkedEvaluations = 0;
    const auto withhold = [&] {
      gate->close();
      const auto slot = service->table()->find("textures/lp42a/flat");
      REQUIRE(slot);
      REQUIRE(slot->entry);
      auto parms = ShaderParms{};
      parms[11] = float(++parkedEvaluations);
      REQUIRE(gate->park([&] {
        static_cast<void>(service->evaluate(MaterialId::of(*slot->entry), parms, 0.0f));
      }));
    };
    const auto at =
      [](const AmbientReadback& readback, const std::uint32_t x, const std::uint32_t y) {
        return (std::size_t(y) * readback.hdr.width + x) * 4;
      };
    // the pixel (80, 60): its centre is 0.5 pixel right of and below the view's, so at
    // the slab's top (z 2, 78 below the eye; fov 90 over 160: 80 pixels per 78 units) it
    // is 0.4875 along -x (down) and -y (right) from (60 0 2)
    const auto pixelPoint = [](const float px, const float py) {
      const auto unit = 78.0f / 80.0f;
      return std::array<float, 3>{
        60.0f - (py + 0.5f - 60.0f) * unit, -(px + 0.5f - 80.0f) * unit, 2.0f};
    };
    // the CPU model of a pixel against its read-back: the HDR within 1 %, the LDR +-1
    const auto compare = [&](
                           const char* what,
                           const AmbientReadback& readback,
                           const std::uint32_t x,
                           const std::uint32_t y,
                           const std::array<float, 3>& model) {
      CAPTURE(what, x, y);
      const auto hdr = readback.hdr.at(x, y);
      const auto expected = modelLdr(model, readback.exposure, readback.frame);
      const auto i = at(readback, x, y);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CAPTURE(c);
        CHECK(hdr[c] == Catch::Approx(model[c]).epsilon(0.01));
        CHECK(std::abs(int(readback.ldr[i + c]) - expected[c]) <= 1);
      }
      std::cout << fmt::format(
        "LP5.1 {} ({} {}): HDR model ({:.5f} {:.5f} {:.5f}) read-back ({:.5f} {:.5f} "
        "{:.5f}); LDR model ({} {} {}) read-back ({} {} {}); exposure {:.6f}\n",
        what,
        x,
        y,
        model[0],
        model[1],
        model[2],
        hdr[0],
        hdr[1],
        hdr[2],
        expected[0],
        expected[1],
        expected[2],
        int(readback.ldr[i]),
        int(readback.ldr[i + 1]),
        int(readback.ldr[i + 2]),
        readback.exposure);
    };
    // a light's diffuse on the flat slab: albedo x lambert x r_lightScale x colour (1) x
    // lightColor (interaction.ps.hlsl: no specular stage, so the specular modifier 0)
    const auto lit = [&](const float lambert, const float lightColor) {
      const auto k = lambert * 3.0f * lightColor;
      return std::array<float, 3>{albedo[0] * k, albedo[1] * k, albedo[2] * k};
    };

    // 1. a math point light (invsquare_clamped, falloff_exp 2, inner 0, the energy clamp
    // on): MLEvaluate's t = |( uv, axial )| = |P - O| / 100 for this axial light, the
    // curve clamped to 0..1, then sRGBToLinearRGB( proj 1 x falloff ) (IPS:179)
    offer({lp51Light("lp51_math_point", "\"math_enabled\" \"1\"\n", 1.0f)});
    const auto point = shoot();
    CHECK(point.frame.lit.mathLights == 1u);
    CHECK(point.frame.lit.textureLights == 0u);
    CHECK(point.frame.lit.ambientLights == 0u);
    CHECK(point.frame.lit.interactionDraws > 0u);
    CHECK(drawnPrograms() == std::set<int>{EngineBuiltinInteraction});
    for (const auto& [x, y] : {std::pair{80u, 60u}, std::pair{85u, 65u}})
    {
      const auto p = pixelPoint(float(x), float(y));
      const auto t = std::min(
        std::sqrt(
          (p[0] - 60.0f) * (p[0] - 60.0f) + p[1] * p[1] + (p[2] - 50.0f) * (p[2] - 50.0f))
          / 100.0f,
        1.0f);
      const auto falloff = std::clamp(modelInvSquareClamped(t, 2.0f, 0.0f), 0.0f, 1.0f);
      compare("math point light", point, x, y, lit(lambertAt(p), modelLinear1(falloff)));
    }

    // 2. a math sun: falloff and mask 1, the whole volume unattenuated: both pixels the
    // same, albedo x 3 x sRGBToLinearRGB( 1 )
    offer({lp51Light(
      "lp51_math_sun",
      "\"math_enabled\" \"1\"\n\"parallel\" \"1\"\n\"light_center\" \"0 0 64\"\n",
      1.0f)});
    const auto sun = shoot();
    CHECK(sun.frame.lit.mathLights == 1u);
    CHECK(sun.frame.lit.textureLights == 0u);
    for (const auto& [x, y] :
         {std::pair{80u, 60u}, std::pair{85u, 65u}, std::pair{74u, 54u}})
    {
      compare(
        "math sun", sun, x, y, lit(lambertAt(pixelPoint(float(x), float(y))), 1.0f));
    }

    // 3. an ambientLight light (a texture light: _white through _noFalloff, 1 at the
    // slab): interactionAmbient.ps.hlsl's output is ( diffuseMap x rpDiffuseModifier +
    // specMap(_black) ... ) x lightColor x vertex colour: its Lambert term of the fixed
    // tangent-space vector ( 0.5, 9.5 - 0.385, 0.8925 ) (the blob's typo of 0.5 - 0.385)
    // is computed and never multiplied in, so the slab reads albedo x 3
    offer({lp51Light("lp51_ambient", "\"texture\" \"lights/lp51/ambient\"\n", 1.0f)});
    const auto ambient = shoot();
    CHECK(ambient.frame.lit.ambientLights == 1u);
    CHECK(ambient.frame.lit.textureLights == 1u);
    CHECK(ambient.frame.lit.mathLights == 0u);
    CHECK(drawnPrograms() == std::set<int>{EngineBuiltinInteractionAmbient});
    compare("ambientLight light", ambient, 80, 60, lit(1.0f, 1.0f));
    compare("ambientLight light", ambient, 85, 65, lit(1.0f, 1.0f));
    {
      // what the Lambert of the typo'd vector would have made of it, for the record
      const auto v = std::array<float, 3>{0.5f, 9.5f - 0.385f, 0.8925f};
      const auto length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      std::cout << fmt::format(
        "LP5.1 ambientLight light: the fixed vector's ldotN on a flat normal {:.5f} "
        "(with "
        "0.5 - 0.385 it would be {:.5f}); the output does not multiply it in\n",
        v[2] / length,
        0.8925f
          / std::sqrt(0.25f + (0.5f - 0.385f) * (0.5f - 0.385f) + 0.8925f * 0.8925f));
    }
    // and as a math light: both phases' counts (a math light, an ambient light)
    offer({lp51Light(
      "lp51_ambient_math",
      "\"texture\" \"lights/lp51/ambient\"\n\"math_enabled\" \"1\"\n",
      1.0f)});
    const auto ambientMath = shoot();
    CHECK(ambientMath.frame.lit.ambientLights == 1u);
    CHECK(ambientMath.frame.lit.mathLights == 1u);
    CHECK(ambientMath.frame.lit.textureLights == 0u);
    CHECK(drawnPrograms() == std::set<int>{EngineBuiltinInteractionAmbient});

    // 4. a math light whose stage image is never loaded: units 3 and 4 bind _white, its
    // stage image is never asked for, the frame converges with it drawn (no _default);
    // the same light as a texture light with the parse thread held asks for it: its slot
    // Pending, the _white stand-in drawn and the frame unconverged until it arrives
    offer({lp51Light(
      "lp51_held", "\"texture\" \"lights/lp51/held\"\n\"math_enabled\" \"1\"\n", 1.0f)});
    const auto held = shoot();
    CHECK(held.frame.lit.mathLights == 1u);
    auto heldKey = std::optional<MaterialImageKey>{};
    {
      const auto table = session->lightTable();
      REQUIRE(table);
      table->records.forEach([&](const PreviewId&, const auto& record) {
        if (record && record->derived.shader.material)
        {
          heldKey = record->derived.shader.material->stages.front().image;
        }
      });
    }
    REQUIRE(heldKey);
    auto heldAsked = true;
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      heldAsked = access.images.find(*heldKey).has_value();
    }));
    CHECK_FALSE(heldAsked);
    compare("math light, its stage image never loaded", held, 80, 60, [&] {
      const auto p = pixelPoint(80.0f, 60.0f);
      const auto t =
        std::sqrt((p[0] - 60.0f) * (p[0] - 60.0f) + p[1] * p[1] + 48.0f * 48.0f) / 100.0f;
      return lit(
        lambertAt(p),
        modelLinear1(std::clamp(modelInvSquareClamped(t, 2.0f, 0.0f), 0.0f, 1.0f)));
    }());
    auto heldState = std::optional<ImageSlots::State>{};
    offer({lp51Light("lp51_held", "\"texture\" \"lights/lp51/held\"\n", 1.0f)});
    const auto pending = hdrOfFrame(*session, [&](const RenderFrameAccess& access) {
      const auto& lights = access.passes.lists().lights;
      if (
        lights.size() != 1u || engineLightUsesMathPath(lights.front().record->light.parms)
        || access.passes.lastStats().textureLights != 1u)
      {
        return false;
      }
      // the frame that first asked for the stage image: not written yet
      if (const auto id = access.images.find(*heldKey))
      {
        heldState = access.images.slot(*id)->state;
      }
      return true;
    });
    REQUIRE(pending);
    CHECK(
      (heldState == std::optional{ImageSlots::State::Pending}
       || heldState == std::optional{ImageSlots::State::Loading}));
    CHECK(pending->second.stats.unconverged);
    const auto arrived = shoot();
    CHECK(arrived.frame.lit.textureLights == 1u);
    std::cout << fmt::format(
      "LP5.1 held stage image: the math light converged with it never asked for; as a "
      "texture light its slot was Pending and the frame unconverged ({} {}), then "
      "settled\n",
      heldState ? int(*heldState) : -1,
      pending->second.stats.unconverged);

    // 5. the sweep (C50, P6.7): per kind 60 steps of _color, the parse thread held on
    // every second step and for the run 40-49 (the evaluation withheld: the light drawn
    // from its kept registers, provisional, the frame unconverged); the lit patch never
    // decreases and is equal to the step before on the withheld ones
    struct Kind
    {
      const char* name;
      std::string keys;
      // each kind's colours its own, so that no kind finds another's evaluations (the
      // texture and the math light share their shader)
      float offset = 0.0f;
    };
    for (const auto& kind : {
           Kind{"texture", "", 0.0f},
           Kind{"math", "\"math_enabled\" \"1\"\n", 0.001f},
           Kind{"ambient", "\"texture\" \"lights/lp51/ambient\"\n", 0.002f},
           // LP5.3 (C50): a fog light's fogged pixel and a blend light's blended pixel
           Kind{"fog", "\"texture\" \"lights/lp51/fog\"\n", 0.003f},
           Kind{"blend", "\"texture\" \"lights/lp51/blend\"\n", 0.004f},
         })
    {
      CAPTURE(kind.name);
      const auto sweepStart = std::chrono::steady_clock::now();
      auto previous = std::optional<std::vector<float>>{};
      auto kept = 0;
      for (auto step = 0; step < 60; ++step)
      {
        CAPTURE(step);
        const auto colourText =
          fmt::format("{:.3f}", 0.30f + 0.01f * float(step) + kind.offset);
        const auto colour = std::stof(colourText);
        const auto withheld = step % 2 == 1 || (step >= 40 && step < 50);
        if (withheld)
        {
          withhold();
        }
        else
        {
          gate->open();
        }
        offer({fmt::format(
          "\"name\" \"lp51_sweep\"\n\"origin\" \"60 0 50\"\n\"light_radius\" \"100 100 "
          "100\"\n\"_color\" \"{0} {0} {0}\"\n{1}",
          colourText,
          kind.keys)});
        const auto frame = hdrOfFrame(*session, [&](const RenderFrameAccess& access) {
          const auto& lights = access.passes.lists().lights;
          if (lights.size() != 1u)
          {
            return false;
          }
          const auto& light = lights.front();
          const auto stats = access.passes.lastStats();
          if (
            light.record->light.parms.shaderParms[0] != colour || stats.camera != camera)
          {
            return false;
          }
          // a withheld step: drawn from the kept registers; an answered one: settled
          return withheld ? light.provisional
                          : !stats.unconverged && access.images.pending() == 0
                              && !access.images.hasLoading();
        });
        REQUIRE(frame);
        const auto patch = litPatch(frame->first);
        if (previous)
        {
          CHECK(meanOf(patch) >= meanOf(*previous));
          if (withheld)
          {
            ++kept;
            CHECK(patch == *previous);
          }
          else
          {
            CHECK(meanOf(patch) > meanOf(*previous));
          }
        }
        previous = patch;
      }
      gate->open();
      std::cout << fmt::format(
        "LP5.1 sweep {}: 60 steps, {} kept frames, the patch's mean {:.5f} at the last "
        "step, {:.1f} s\n",
        kind.name,
        kept,
        previous ? meanOf(*previous) : 0.0f,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - sweepStart)
          .count());
    }

    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(false);
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
  messages.print("LP5.1 math and ambientLight lights, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);

  // the frame log's two columns: a frame of each light kind
  {
    const auto text = readTextFile(frameLog);
    auto lines = std::vector<std::string>{};
    auto stream = std::istringstream{text};
    auto line = std::string{};
    while (std::getline(stream, line))
    {
      lines.push_back(line);
    }
    REQUIRE(lines.size() > 1u);
    const auto split = [](const std::string& row) {
      auto fields = std::vector<std::string>{};
      auto field = std::string{};
      for (const auto c : row)
      {
        if (c == ',')
        {
          fields.push_back(field);
          field.clear();
        }
        else
        {
          field += c;
        }
      }
      fields.push_back(field);
      return fields;
    };
    const auto header = split(lines.front());
    REQUIRE(header.size() == FrameLogFields);
    CHECK(header[49] == "math_lights");
    CHECK(header[50] == "ambient_lights");
    auto seen = std::set<std::pair<std::string, std::string>>{};
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
      const auto fields = split(lines[i]);
      REQUIRE(fields.size() == FrameLogFields);
      if (fields[1] == "frame")
      {
        seen.emplace(fields[49], fields[50]);
      }
    }
    // the math lights (1, 0), the ambient light (0, 1), the ambient math light (1, 1)
    CHECK(seen.contains({"1", "0"}));
    CHECK(seen.contains({"0", "1"}));
    CHECK(seen.contains({"1", "1"}));
  }
}

// LPF.1 (C40, C43, C44, D98, D100-D102, D106, D110): the live feed on the render side
// over the LP2.2a fixture, headless, with the exposure pinned: a feed publication draws a
// frame at once, the counters keep a frame behind a hand-over from being settled, a
// replaced image never shows the intrinsic, the re-parse reaches the cells, the lights
// and the lists, and the frame log times the feed. Hidden: it needs a GPU and the game's
// blobs.
TEST_CASE("LitSession live feed on a GPU (LPF.1)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;

  SECTION("step 1: a feed draws at once, the counters in settled, refusals, the storm")
  {
    {
      const auto instance = testInstance(true, false, messages);
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
        // LP7.5 step 0 (the LPF.1 storm flake: 20.5 and 23.1 ms at two gates): the gaps
        // are measured between the frames' true starts, the pacer's stamps; the hook's
        // time follows each start by the frame's snapshot, acquire and uploads, so one
        // late hook shrank the old measure while the pacer's floor stood
        using Ms = std::chrono::duration<double, std::milli>;
        auto closest = std::chrono::steady_clock::duration::max();
        auto hookClosest = std::chrono::steady_clock::duration::max();
        for (std::size_t i = 1; i < inStorm.size(); ++i)
        {
          closest = std::min(closest, inStorm[i].paced - inStorm[i - 1].paced);
          hookClosest = std::min(hookClosest, inStorm[i].start - inStorm[i - 1].start);
        }
        // the hook never precedes its frame's start
        auto lags = std::vector<double>{};
        for (const auto& frame : inStorm)
        {
          CHECK(frame.start >= frame.paced);
          lags.push_back(Ms(frame.start - frame.paced).count());
        }
        std::ranges::sort(lags);
        // the pacer's floor: 30 frame starts a second at most
        CHECK(double(inStorm.size()) <= stormSeconds * 30.0 + 2.0);
        CHECK(inStorm.size() >= 10u);
        CHECK(closest >= 25ms);
        CHECK(closest >= MinFrameInterval - 1ms);
        if (!lags.empty())
        {
          std::cout << fmt::format(
            "LP7.5 step 0, the storm's {} frames: the hook after the frame's start by "
            "min {:.2f}, median {:.2f}, p90 {:.2f}, max {:.2f} ms; closest true starts "
            "{:.2f} ms apart (the floor {:.2f} ms), closest hook stamps {:.2f} ms\n",
            lags.size(),
            lags.front(),
            lags[lags.size() / 2],
            lags[lags.size() * 9 / 10],
            lags.back(),
            Ms(closest).count(),
            Ms(MinFrameInterval).count(),
            Ms(hookClosest).count());
        }
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
      const auto instance = testInstance(true, false, messages);
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
      const auto instance = testInstance(true, false, messages);
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


namespace
{

// LP5.2a: the stage pass's materials over a white wall lit head-on; the images are pure
// colours (0 or 255 per channel, exact through any block compression) and the stages'
// colour registers set the values between
const auto Lp52aMaterials = std::string{
  "textures/lp52a/white\n{\n\tqer_editorimage textures/lp52a/white.tga\n"
  "\tdiffusemap textures/lp52a/white.tga\n}\n"
  "textures/lp52a/add\n{\n\t{\n\t\tblend add\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tred 0.5\n\t\tgreen 0.25\n\t\tblue 0\n\t}\n}\n"
  "textures/lp52a/add_late\n{\n\t{\n\t\tblend add\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tred 0.5\n\t\tgreen 0.25\n\t\tblue 0\n\t}\n}\n"
  "textures/lp52a/blend\n{\n\t{\n\t\tblend blend\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tred 0.2\n\t\tgreen 0.6\n\t\tblue 1\n\t\talpha 0.5\n\t}\n}\n"
  "textures/lp52a/inverse\n{\n\t{\n\t\tblend add\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tinverseVertexColor\n\t}\n}\n"
  "textures/lp52a/colored\n{\n\t{\n\t\tblend add\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tcolored\n\t}\n}\n"
  "textures/lp52a/grate\n{\n\t{\n\t\tmap textures/lp52a/holes.tga\n\t\talphaTest 0.5\n"
  "\t}\n}\n"
  "textures/lp52a/far\n{\n\t{\n\t\tblend blend\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tred 0.8\n\t\tgreen 0.2\n\t\tblue 0.2\n\t\talpha 0.6\n\t}\n}\n"
  "textures/lp52a/near\n{\n\t{\n\t\tblend blend\n\t\tmap textures/lp52a/white.tga\n"
  "\t\tred 0.2\n\t\tgreen 0.2\n\t\tblue 0.8\n\t\talpha 0.6\n\t}\n}\n"
  "textures/lp52a/screen\n{\n\t{\n\t\tblend add\n\t\tmap textures/lp52a/quadrants.tga\n"
  "\t\tscreen\n\t}\n}\n"
  "textures/lp52a/voronoi\n{\n\t{\n\t\tblend add\n\t\tprogram vornoipattern\n"
  "\t\tvertexParm 0 0.5, 40, 40\n\t}\n}\n"
  "textures/lp52a/fogwl\n{\n\t{\n\t\tblend add\n\t\tprogram fogwithlights\n"
  "\t\tvertexParm 0 0.5, 160, 120\n\t}\n}\n"
  "textures/lp52a/haze\n{\n\t{\n\t\tprogram heathaze\n\t\tvertexParm 0 0.1, 0.2\n"
  "\t\tfragmentMap 0 _currentRender\n\t\tfragmentMap 1 textures/lp52a/white.tga\n\t}\n}\n"
  "textures/lp52a/movie\n{\n\t{\n\t\tblend add\n\t\tvideomap video/lp52a.roq\n\t}\n}\n"
  "textures/lp52a/backbox\n{\n\ttranslucent\n\tbackSided\n\t{\n\t\tblend blend\n"
  "\t\tmap textures/lp52a/white.tga\n\t\tred 0.3\n\t\tgreen 0.7\n\t\tblue 0.9\n"
  "\t\talpha 0.5\n\t}\n}\n"};

// the wall at x 200 lit head-on by a parallel light from -x; the stage surfaces 2 units
// thick in front of it (a camera at the origin's height 64 looking down +x); the
// back-sided box straddles the wall's face: its front faces before it, its back face
// behind it
std::string lp52aMapText(const std::string& addMaterial = "lp52a/add")
{
  auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
  text += boxBrush(200, -400, -200, 216, 400, 400, "lp52a/white");
  text += boxBrush(150, -120, 100, 152, -100, 120, addMaterial);
  text += boxBrush(150, -80, 100, 152, -60, 120, "lp52a/blend");
  text += boxBrush(150, -40, 100, 152, -20, 120, "lp52a/inverse");
  text += boxBrush(150, 40, 100, 152, 60, 120, "lp52a/implicit");
  text += boxBrush(150, 80, 100, 152, 100, 120, "lp52a/haze");
  text += boxBrush(150, 120, 100, 152, 140, 120, "lp52a/movie");
  text += boxBrush(150, -120, 10, 152, -60, 70, "lp52a/grate");
  text += boxBrush(170, -30, 20, 172, 10, 50, "lp52a/far");
  text += boxBrush(140, -10, 30, 142, 30, 60, "lp52a/near");
  text += boxBrush(150, 50, 10, 152, 90, 60, "lp52a/screen");
  text += boxBrush(150, 100, 10, 152, 140, 60, "lp52a/voronoi");
  text += boxBrush(150, 100, 65, 152, 140, 95, "lp52a/fogwl");
  text += boxBrush(190, -40, -60, 230, 0, -20, "lp52a/backbox");
  text += "}\n";
  text +=
    "{\n\"classname\" \"func_static\"\n\"name\" \"lp52a_colored\"\n\"model\" "
    "\"lp52a_colored\"\n\"_color\" \"0.5 0.25 1\"\n"
    + boxBrush(150, 0, 100, 152, 20, 120, "lp52a/colored") + "}\n";
  text +=
    "{\n\"classname\" \"light\"\n\"name\" \"lp52a_parallel\"\n\"origin\" \"100 0 64\"\n"
    "\"light_radius\" \"300 300 300\"\n\"light_center\" \"-1 0 0\"\n\"parallel\" \"1\"\n"
    "\"texture\" \"lights/lp22a/flatclamp\"\n\"_color\" \"0.3 0.3 0.3\"\n"
    "\"noshadows\" \"1\"\n}\n";
  return text;
}

// Linear1 (global_inc.hlsl:177-180)
float linearOf(const float c)
{
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

using Rgb = std::array<float, 3>;

Rgb rgbOf(const std::array<float, 4>& pixel)
{
  return {pixel[0], pixel[1], pixel[2]};
}

// texture_color's sRGBAToLinearRGBA( tex · rpColor ) of a white texel
Rgb linearRgb(const Rgb& colour)
{
  return {linearOf(colour[0]), linearOf(colour[1]), linearOf(colour[2])};
}

Rgb plus(const Rgb& a, const Rgb& b)
{
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// SRC_ALPHA, ONE_MINUS_SRC_ALPHA of a linearised source and alpha over dst
Rgb blendOver(const Rgb& colour, const float alpha, const Rgb& dst)
{
  const auto src = linearRgb(colour);
  const auto a = linearOf(alpha);
  return {
    src[0] * a + dst[0] * (1.0f - a),
    src[1] * a + dst[1] * (1.0f - a),
    src[2] * a + dst[2] * (1.0f - a)};
}

bool near3(const Rgb& a, const Rgb& b, const float tolerance = 4e-3f)
{
  for (std::size_t i = 0; i < 3; ++i)
  {
    if (std::abs(a[i] - b[i]) > tolerance * std::max(1.0f, std::abs(b[i])))
    {
      return false;
    }
  }
  return true;
}

std::string rgbText(const Rgb& c)
{
  return fmt::format("({:.5f} {:.5f} {:.5f})", c[0], c[1], c[2]);
}

// the pixel (rows top-down) a world point projects to, through the view's matrices
std::pair<std::uint32_t, std::uint32_t> pixelOf(
  const LitView& view, const EngineVec3& point)
{
  const auto ndc = engineGlobalToNormalizedDeviceCoordinates(
    view.worldModelViewMatrix, view.projectionMatrix, point);
  return {
    std::uint32_t((ndc[0] * 0.5f + 0.5f) * float(view.width)),
    std::uint32_t((0.5f - ndc[1] * 0.5f) * float(view.height))};
}

// the frame's stats after its passes, once its camera is the one asked for and pred holds
std::optional<LitFrameStats> framesUntil(
  LitSession& session,
  const CameraState& camera,
  const std::function<bool(const LitFrameStats&)>& pred,
  const std::chrono::milliseconds timeout = 20000ms)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.setCamera(camera);
    session.tableChanged();
    auto stats = LitFrameStats{};
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterPassesForTesting([&, done](RenderFrameAccess& access) {
      stats = access.passes.lastStats();
      done->set_value();
    });
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (stats.camera == camera && pred(stats))
    {
      return stats;
    }
    std::this_thread::sleep_for(20ms);
  }
  return std::nullopt;
}

} // namespace

// LP5.2a (C46, C47, D115-D121, D124): pass 8 over a synthetic map of stage materials in
// front of a wall lit head-on, each pixel against a CPU model over the frame drawn with
// the shader passes skipped (the background); the draw order, the counters, the pending
// rule and the frame log's columns. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession pass 8, the material stage pass, on a GPU (LP5.2a)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto gamePath = std::filesystem::path{gamePathValue};
  const auto root = assembleLitRoot(gamePath);
  REQUIRE_FALSE(root.empty());
  const auto base = root / "base";
  writeTextFile(base / "materials" / "zz_lp52a.mtr", Lp52aMaterials);
  std::filesystem::create_directories(base / "textures" / "lp52a");
  const auto writeTga = [&](const std::string& name, const test::Rgba& image) {
    auto file = std::ofstream{base / "textures" / "lp52a" / name, std::ios::binary};
    const auto bytes = test::tgaFile(image);
    file.write(bytes.data(), std::streamsize(bytes.size()));
  };
  writeTga("white.tga", test::flat(4, 4, 255, 255, 255));
  // the implicit material's image: magenta, opaque
  writeTga("implicit.tga", test::flat(4, 4, 255, 0, 255));
  // red with 16 × 16 holes of alpha 0 in a checker
  {
    auto holes = test::flat(64, 64, 255, 0, 0);
    for (int y = 0; y < 64; ++y)
    {
      for (int x = 0; x < 64; ++x)
      {
        if ((x / 16 + y / 16) % 2)
        {
          holes.pixels[std::size_t((y * 64 + x) * 4 + 3)] = 0;
        }
      }
    }
    writeTga("holes.tga", holes);
  }
  // four quadrants (top row first): red, green / blue, yellow
  {
    auto quadrants = test::flat(64, 64, 0, 0, 0);
    for (int y = 0; y < 64; ++y)
    {
      for (int x = 0; x < 64; ++x)
      {
        const auto i = std::size_t((y * 64 + x) * 4);
        const auto right = x >= 32;
        const auto bottom = y >= 32;
        quadrants.pixels[i + 0] = std::uint8_t(right == bottom ? 255 : 0);
        quadrants.pixels[i + 1] = std::uint8_t(right ? 255 : 0);
        quadrants.pixels[i + 2] = std::uint8_t(!right && bottom ? 255 : 0);
      }
    }
    writeTga("quadrants.tga", quadrants);
  }
  // the stage pass's programs beyond LP2's: texture_color_texgen (18) and the material
  // programs vornoipattern and heathaze
  for (const auto& [name, stage] :
       {std::pair{std::string{"builtin/texture_color_texgen"}, ShaderStage::Vertex},
        std::pair{std::string{"builtin/texture_color_texgen"}, ShaderStage::Pixel},
        std::pair{std::string{"vornoipattern"}, ShaderStage::Vertex},
        std::pair{std::string{"vornoipattern"}, ShaderStage::Pixel},
        std::pair{std::string{"fogwithlights"}, ShaderStage::Vertex},
        std::pair{std::string{"fogwithlights"}, ShaderStage::Pixel},
        std::pair{std::string{"heathaze"}, ShaderStage::Vertex},
        std::pair{std::string{"heathaze"}, ShaderStage::Pixel}})
  {
    const auto relative = shaderBlobPath(name, stage);
    REQUIRE(std::filesystem::exists(gamePath / "base" / relative));
    std::filesystem::create_directories((base / relative).parent_path());
    std::filesystem::copy_file(
      gamePath / "base" / relative,
      base / relative,
      std::filesystem::copy_options::overwrite_existing);
  }
  writeTextFile(base / "maps" / "lp52a.map", lp52aMapText());
  const auto snapshotOf = [&](const std::string& text, const std::uint64_t sequence) {
    auto snapshot = snapshotOfMapText(text);
    snapshot.generation = 7;
    snapshot.sequence = sequence;
    snapshot.mapPath = (base / "maps" / "lp52a.map").string();
    return std::make_shared<const Snapshot>(std::move(snapshot));
  };
  const auto camera = litCamera({0, 0, 64}, {1, 0, 0}, {0, 0, 1});
  const auto view = litViewOf(camera, 160, 120);
  const auto frameLog = root / "lp52a.framelog.csv";
  std::filesystem::remove(frameLog);

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto gate = std::make_shared<ParkingGate>();
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    serviceOptions.imageBudgetBytes = 0;
    serviceOptions.beforeTakeWorkForTesting = [gate] { gate->pass(); };
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto atExit = OpenParkingAtExit{gate};
    auto events = Events{};
    auto session = loggedRootSession(instance, events, *service, root, frameLog, true);
    auto tonemap = EngineTonemap;
    tonemap.hdrMinLuminance = 0.1f;
    tonemap.hdrMaxLuminance = 0.1001f;
    session->setTonemapForTesting(tonemap);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
    REQUIRE(session->offerSnapshot(snapshotOf(lp52aMapText(), 1)));

    // the background: the shader passes skipped (D118), the pass's counters 0
    session->skipShaderPassesForTesting(true);
    auto background = settledHdr(*session, camera, 1);
    for (auto attempt = 0;
         attempt < 20 && background && !background->second.stats.stagesSkipped;
         ++attempt)
    {
      background = settledHdr(*session, camera, 1);
    }
    REQUIRE(background);
    {
      const auto& stats = background->second.stats;
      CHECK(stats.stagesSkipped);
      CHECK(stats.stageDraws == 0u);
      CHECK(stats.stageSkipped == 0u);
      CHECK(stats.stagePostProcess == 0u);
    }
    const auto& bg = background->first;

    // pass 8 drawn
    session->skipShaderPassesForTesting(false);
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
    auto drawn = settledHdr(*session, camera, 1);
    for (auto attempt = 0; attempt < 20 && drawn && drawn->second.stats.stagesSkipped;
         ++attempt)
    {
      drawn = settledHdr(*session, camera, 1);
    }
    REQUIRE(drawn);
    auto log = std::vector<LitDrawRecord>{};
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      log = access.passes.drawLogForTesting();
    }));
    const auto& stats = drawn->second.stats;
    const auto& image = drawn->first;
    CHECK_FALSE(stats.stagesSkipped);
    CHECK(stats.stagesTimed);
    CHECK_FALSE(stats.stagesDisabled);
    // twelve stage draws; the haze stopped at (K38), the movie skipped (D120)
    CHECK(stats.stageDraws == 12u);
    CHECK(stats.stagePostProcess == 1u);
    CHECK(stats.stageCinematic == 1u);
    CHECK(stats.stageSkipped == 1u);
    CHECK(stats.stageTexgenLp6 == 0u);
    CHECK(stats.stageProgramMissing == 0u);
    CHECK(stats.stagePending == 0u);

    const auto at = [&](const HdrImage& hdr, const EngineVec3& point) {
      const auto [x, y] = pixelOf(view, point);
      return rgbOf(hdr.at(x, y));
    };
    const auto check = [&](const char* what, const EngineVec3& point, const Rgb& model) {
      const auto got = at(image, point);
      std::cout << fmt::format(
        "LP5.2a {}: background {} drawn {} model {}\n",
        what,
        rgbText(at(bg, point)),
        rgbText(got),
        rgbText(model));
      CAPTURE(what, rgbText(got), rgbText(model));
      CHECK(near3(got, model));
    };
    // blend add: the lit value plus the stage's colour (texture_color's linearised
    // tex · rpColor)
    const auto addPoint = EngineVec3{150, -110, 110};
    check("blend add", addPoint, plus(at(bg, addPoint), linearRgb({0.5f, 0.25f, 0.0f})));
    // blend blend: SRC_ALPHA over the lit value, the alpha linearised as well
    const auto blendPoint = EngineVec3{150, -70, 110};
    check(
      "blend blend", blendPoint, blendOver({0.2f, 0.6f, 1.0f}, 0.5f, at(bg, blendPoint)));
    // inverseVertexColor on white brush vertices: ( -1 · 1 + 1 ) = 0, the stage adds
    // nothing
    const auto inversePoint = EngineVec3{150, -30, 110};
    check("inverseVertexColor", inversePoint, at(bg, inversePoint));
    // colored on the func_static: its _color through parm0-2
    const auto coloredPoint = EngineVec3{150, 10, 110};
    check(
      "colored",
      coloredPoint,
      plus(at(bg, coloredPoint), linearRgb({0.5f, 0.25f, 1.0f})));
    // the implicit material: blend blend colored map clamp, its image magenta, alpha 1
    const auto implicitPoint = EngineVec3{150, 50, 110};
    check("implicit material", implicitPoint, Rgb{1.0f, 0.0f, 1.0f});
    // K38: the _currentRender material draws nothing; the wall behind shows
    const auto hazePoint = EngineVec3{150, 90, 110};
    check("SS_POST_PROCESS stop", hazePoint, at(bg, hazePoint));
    // D120: the cinematic stage is not drawn
    const auto moviePoint = EngineVec3{150, 130, 110};
    check("cinematic", moviePoint, at(bg, moviePoint));

    // the alpha-tested stage, EQUAL against the prepass's alpha-tested depth: the stage
    // where the grate is solid, the lit wall in its holes
    {
      const auto [x0, y0] = pixelOf(view, EngineVec3{150, -118, 68});
      const auto [x1, y1] = pixelOf(view, EngineVec3{150, -62, 12});
      auto stage = 0;
      auto wall = 0;
      auto other = 0;
      for (auto y = y0; y <= y1; ++y)
      {
        for (auto x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
        {
          const auto got = rgbOf(image.at(x, y));
          if (near3(got, Rgb{1.0f, 0.0f, 0.0f}))
          {
            ++stage;
          }
          else if (near3(got, rgbOf(bg.at(x, y))))
          {
            ++wall;
          }
          else
          {
            ++other;
          }
        }
      }
      std::cout << fmt::format(
        "LP5.2a alpha-tested stage: {} stage pixels, {} wall pixels, {} others\n",
        stage,
        wall,
        other);
      CHECK(stage > 20);
      CHECK(wall > 20);
      CHECK(other == 0);
    }

    // two translucent materials at one sort value: the farther drawn first (the log), the
    // overlap the near one's blend over the far one's
    {
      auto farIndex = std::optional<std::size_t>{};
      auto nearIndex = std::optional<std::size_t>{};
      for (std::size_t i = 0; i < log.size(); ++i)
      {
        if (log[i].pass != LitDrawRecord::Pass::Stage)
        {
          continue;
        }
        if (log[i].chunk.material == "textures/lp52a/far")
        {
          farIndex = i;
        }
        if (log[i].chunk.material == "textures/lp52a/near")
        {
          nearIndex = i;
        }
      }
      REQUIRE(farIndex);
      REQUIRE(nearIndex);
      CHECK(*farIndex < *nearIndex);
      const auto overlap = EngineVec3{140, -5, 40};
      const auto farColour = Rgb{0.8f, 0.2f, 0.2f};
      const auto nearColour = Rgb{0.2f, 0.2f, 0.8f};
      const auto farFirst =
        blendOver(nearColour, 0.6f, blendOver(farColour, 0.6f, at(bg, overlap)));
      const auto nearFirst =
        blendOver(farColour, 0.6f, blendOver(nearColour, 0.6f, at(bg, overlap)));
      check("far first (the engine's order)", overlap, farFirst);
      CHECK_FALSE(near3(at(image, overlap), nearFirst));
      const auto nearOnly = EngineVec3{140, 25, 55};
      check("near alone", nearOnly, blendOver(nearColour, 0.6f, at(bg, nearOnly)));
    }

    // TG_SCREEN: the texcoords are rows 0, 1 and 3 of modelViewMatrix × projectionMatrix
    // (the world's) at the point, the image's quadrant there added
    {
      const auto mat =
        engineGlMatrixMultiply(view.worldModelViewMatrix, view.projectionMatrix);
      const auto plane = [&](const int row, const EngineVec3& p) {
        return mat[0 * 4 + std::size_t(row)] * p[0] + mat[1 * 4 + std::size_t(row)] * p[1]
               + mat[2 * 4 + std::size_t(row)] * p[2] + mat[3 * 4 + std::size_t(row)];
      };
      auto checked = 0;
      for (const auto& point :
           {EngineVec3{150, 60, 20}, EngineVec3{150, 80, 50}, EngineVec3{150, 70, 35}})
      {
        const auto q = plane(3, point);
        const auto s = plane(0, point) / q;
        const auto t = plane(1, point) / q;
        const auto u = s - std::floor(s);
        const auto v = t - std::floor(t);
        // away from a quadrant's edges (bilinear, mip 0)
        if (
          std::abs(u - 0.5f) < 0.08f || u < 0.04f || u > 0.96f
          || std::abs(v - 0.5f) < 0.08f || v < 0.04f || v > 0.96f)
        {
          continue;
        }
        const auto right = u >= 0.5f;
        const auto bottom = v >= 0.5f;
        const auto quadrant = Rgb{
          right == bottom ? 1.0f : 0.0f,
          right ? 1.0f : 0.0f,
          !right && bottom ? 1.0f : 0.0f};
        check("TG_SCREEN", point, plus(at(bg, point), quadrant));
        ++checked;
      }
      CHECK(checked > 0);
    }

    // newStage programs with a constant USER0 (C47): the pixels of a panel that differ
    // from the background
    const auto differing = [&](const EngineVec3& corner0, const EngineVec3& corner1) {
      const auto [x0, y0] = pixelOf(view, corner0);
      const auto [x1, y1] = pixelOf(view, corner1);
      auto differs = 0;
      for (auto y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
      {
        for (auto x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
        {
          differs += near3(rgbOf(image.at(x, y)), rgbOf(bg.at(x, y))) ? 0 : 1;
        }
      }
      return differs;
    };
    // vornoipattern's vertex shader writes its clip position to a POSITION output, not
    // SV_Position (vornoipattern.vs.hlsl:50), so no clip position reaches the rasterizer
    // and its blob draws nothing: an engine fact, mirrored (the draw is made, counted
    // above)
    std::cout << fmt::format(
      "LP5.2a vornoipattern: {} pixels differ\n",
      differing(EngineVec3{150, 102, 58}, EngineVec3{150, 138, 12}));
    // fogwithlights (no _currentRender fragment map, so not SS_POST_PROCESS) draws its
    // clouds from SV_Position and USER0
    {
      const auto differs = differing(EngineVec3{150, 102, 93}, EngineVec3{150, 138, 67});
      std::cout << fmt::format("LP5.2a fogwithlights: {} pixels differ\n", differs);
      CHECK(differs > 0);
    }

    // CT_BACK_SIDED drawn as front-sided (D117): the front face before the wall is
    // blended once; a correct back-sided cull would draw the back face alone, behind the
    // wall's depth, and show nothing
    check(
      "backSided box",
      EngineVec3{190, -20, -40},
      blendOver({0.3f, 0.7f, 0.9f}, 0.5f, at(bg, EngineVec3{190, -20, -40})));

    // the draw log: the programs and the back-sided cull bit
    {
      auto programs = std::map<std::string, int>{};
      for (const auto& record : log)
      {
        if (record.pass != LitDrawRecord::Pass::Stage)
        {
          continue;
        }
        programs[record.chunk.material] = record.program;
        if (record.chunk.material == "textures/lp52a/backbox")
        {
          CHECK((record.state & (3ull << 15)) == engine_gls::CullBackSided);
        }
      }
      CHECK(programs["textures/lp52a/add"] == EngineBuiltinTextureVertexColor);
      CHECK(programs["textures/lp52a/screen"] == EngineBuiltinTextureTexgenVertexColor);
      CHECK(
        programs["textures/lp52a/voronoi"]
        == findEngineMaterialProgram("vornoipattern")->row.index);
      CHECK_FALSE(programs.contains("textures/lp52a/haze"));
      CHECK_FALSE(programs.contains("textures/lp52a/movie"));
    }

    // D124: a material not answered yet: its surface is skipped, the frame unconverged,
    // the other stages drawn; then it lands and is drawn
    {
      gate->close();
      REQUIRE(gate->park([&] { service->lookup("textures/lp52a/park"); }));
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(snapshotOf(lp52aMapText("lp52a/add_late"), 2)));
      const auto pending = framesUntil(
        *session, camera, [](const LitFrameStats& s) { return s.stagePending > 0; });
      REQUIRE(pending);
      CHECK(pending->stagePending == 1u);
      CHECK(pending->unconverged);
      CHECK(pending->stageDraws == 11u);
      gate->open();
      auto landed = settledHdr(*session, camera, 1);
      for (auto attempt = 0;
           attempt < 20 && landed && landed->second.stats.stageDraws != 12u;
           ++attempt)
      {
        landed = settledHdr(*session, camera, 1);
      }
      REQUIRE(landed);
      CHECK(landed->second.stats.stagePending == 0u);
      CHECK(landed->second.stats.stageDraws == 12u);
      const auto got = rgbOf(
        landed->first.at(pixelOf(view, addPoint).first, pixelOf(view, addPoint).second));
      CHECK(near3(got, plus(at(bg, addPoint), linearRgb({0.5f, 0.25f, 0.0f}))));
    }

    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(false);
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
  messages.print("LP5.2a stage pass, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);

  // the frame log's three columns: a frame with stage draws and their GPU time
  {
    const auto rows = readCsv(frameLog);
    REQUIRE(rows.size() > 1u);
    REQUIRE(rows[0].size() == FrameLogFields);
    CHECK(rows[0][51] == "stage_draws");
    CHECK(rows[0][52] == "stage_skipped");
    CHECK(rows[0][53] == "gpu_stages_ms");
    auto timedWithDraws = false;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      if (
        rows[i].size() == FrameLogFields && rows[i][1] == "frame" && !rows[i][51].empty()
        && rows[i][51] != "0" && !rows[i][53].empty())
      {
        timedWithDraws = true;
      }
    }
    CHECK(timedWithDraws);
  }
}

namespace
{

// LP5.2b: the cube and sky texgens' materials over a white wall lit head-on; the cube's
// six faces pure colours (0 or 255 per channel: exact through any block compression)
const auto Lp52bMaterials = std::string{
  "textures/lp52b/white\n{\n\tqer_editorimage textures/lp52b/white.tga\n"
  "\tdiffusemap textures/lp52b/white.tga\n}\n"
  // TG_SKYBOX_CUBE on an opaque panel: skybox (69), EQUAL against its prepass depth
  "textures/lp52b/sky\n{\n\t{\n\t\tcubeMap textures/lp52b/cube\n\t\ttexgen skybox\n"
  "\t}\n}\n"
  // TG_REFLECT_CUBE without and with a bump stage, translucent, added over the wall
  "textures/lp52b/env\n{\n\t{\n\t\tblend add\n\t\tcubeMap textures/lp52b/cube\n"
  "\t\ttexgen reflect\n\t}\n}\n"
  "textures/lp52b/bumpy_env\n{\n\ttranslucent\n\tbumpmap _flat\n\t{\n\t\tblend add\n"
  "\t\tcubeMap textures/lp52b/cube\n\t\ttexgen reflect\n\t}\n}\n"
  // TG_REFLECT_CUBE2: not drawn (LP6's inputs)
  "textures/lp52b/reflect2\n{\n\t{\n\t\tblend add\n\t\tcubeMap textures/lp52b/cube\n"
  "\t\ttexgen reflect2\n\t}\n}\n"
  // the prepass's alpha-tested skybox stage: drawn by pass 8 too, or skipped there as
  // gl_zero, gl_one (its prepass black alone)
  "textures/lp52b/sky_tested\n{\n\t{\n\t\tcubeMap textures/lp52b/cube\n"
  "\t\ttexgen skybox\n\t\talphaTest 0.5\n\t}\n}\n"
  "textures/lp52b/sky_mask\n{\n\t{\n\t\tblend gl_zero, gl_one\n"
  "\t\tcubeMap textures/lp52b/cube\n\t\ttexgen skybox\n\t\talphaTest 0.5\n\t}\n}\n"
  // the prepass's alpha-tested wobbleSky stage (rates 0): the sky alone, or the sky plus
  // pass 8's
  "textures/lp52b/wobble_mask\n{\n\t{\n\t\tblend gl_zero, gl_one\n"
  "\t\tcubeMap textures/lp52b/cube\n\t\ttexgen wobbleSky 0 0 0\n\t\talphaTest 0.5\n"
  "\t}\n}\n"
  "textures/lp52b/wobble_add\n{\n\t{\n\t\tblend add\n\t\tcubeMap textures/lp52b/cube\n"
  "\t\ttexgen wobbleSky 0 0 0\n\t\talphaTest 0.5\n\t}\n}\n"
  // a cube asked for only once its surface is in the view
  "textures/lp52b/sky_late\n{\n\t{\n\t\tcubeMap textures/lp52b/late\n"
  "\t\ttexgen skybox\n\t}\n}\n"};

// the wall at x 200; the panels 2 units thick facing the camera at ( 0, 0, 64 ) that
// looks down +x; the wobbleSky panels high and near (x 120, z 124-150), where the
// direction from the world's origin (the prepass's LOCALVIEWORIGIN 0) is +z's and the one
// from the eye +x's
std::string lp52bMapText(const bool late = false)
{
  auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
  text += boxBrush(200, -400, -200, 216, 400, 400, "lp52b/white");
  text += boxBrush(150, -120, 90, 152, -80, 120, "lp52b/sky");
  text += boxBrush(150, -60, 90, 152, -20, 120, "lp52b/env");
  text += boxBrush(150, 0, 90, 152, 40, 120, "lp52b/bumpy_env");
  text += boxBrush(150, 60, 90, 152, 100, 120, "lp52b/reflect2");
  text += boxBrush(150, -120, 20, 152, -80, 60, "lp52b/sky_tested");
  text += boxBrush(150, -60, 20, 152, -20, 60, "lp52b/sky_mask");
  text += boxBrush(120, 10, 124, 122, 40, 150, "lp52b/wobble_mask");
  text += boxBrush(120, 50, 124, 122, 80, 150, "lp52b/wobble_add");
  if (late)
  {
    text += boxBrush(150, 0, 20, 152, 40, 60, "lp52b/sky_late");
  }
  text += "}\n";
  text +=
    "{\n\"classname\" \"light\"\n\"name\" \"lp52b_parallel\"\n\"origin\" \"100 0 64\"\n"
    "\"light_radius\" \"300 300 300\"\n\"light_center\" \"-1 0 0\"\n\"parallel\" \"1\"\n"
    "\"texture\" \"lights/lp22a/flatclamp\"\n\"_color\" \"0.3 0.3 0.3\"\n"
    "\"noshadows\" \"1\"\n}\n";
  return text;
}

// the cube's faces in the engine's R_LoadCubeImages order (_px _nx _py _ny _pz _nz) and
// colours
constexpr auto Lp52bFaces = std::array<std::pair<const char*, std::array<int, 3>>, 6>{{
  {"_px", {255, 0, 0}},
  {"_nx", {0, 255, 0}},
  {"_py", {0, 0, 255}},
  {"_ny", {255, 255, 0}},
  {"_pz", {0, 255, 255}},
  {"_nz", {255, 0, 255}},
}};

/**
 * A CPU model of a TextureCube's face choice (the major axis of the direction, D3D's and
 * Vulkan's), returning the face's colour as the shaders read it: sRGBToLinear of a 0 or
 * 255 channel is that channel's 0 or 1.
 */
Rgb lp52bFaceColour(const EngineVec3& direction)
{
  const auto ax = std::abs(direction[0]);
  const auto ay = std::abs(direction[1]);
  const auto az = std::abs(direction[2]);
  auto face = std::size_t{0};
  if (ax >= ay && ax >= az)
  {
    face = direction[0] >= 0.0f ? 0u : 1u;
  }
  else if (ay >= az)
  {
    face = direction[1] >= 0.0f ? 2u : 3u;
  }
  else
  {
    face = direction[2] >= 0.0f ? 4u : 5u;
  }
  const auto& c = Lp52bFaces[face].second;
  return {float(c[0]) / 255.0f, float(c[1]) / 255.0f, float(c[2]) / 255.0f};
}

// environment.ps / bumpyenvironment.ps's reflection of the eye vector about a normal:
// 2 ( E · N ) N - E of the normalised E = localViewOrigin - position
EngineVec3 lp52bReflect(
  const EngineVec3& eye, const EngineVec3& position, const EngineVec3& normal)
{
  auto e = EngineVec3{eye[0] - position[0], eye[1] - position[1], eye[2] - position[2]};
  const auto length = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  for (auto& c : e)
  {
    c /= length;
  }
  const auto d = e[0] * normal[0] + e[1] * normal[1] + e[2] * normal[2];
  return {
    2.0f * d * normal[0] - e[0],
    2.0f * d * normal[1] - e[1],
    2.0f * d * normal[2] - e[2]};
}

} // namespace

// LP5.2b (C48, D124, P6.17-P6.19): the cube and sky texgens in pass 8 and the prepass
// over a synthetic map, each pixel against a CPU model of the shader's texcoord and the
// cube's face choice: skybox, reflect with and without a bump, the prepass's alpha-tested
// skybox and wobbleSky stages, reflect2 skipped, a cube not written yet drawn as the zero
// cube with the frame unconverged; the validation layer's answer on 57's compare-enabled
// sampler. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession cube and sky texgens on a GPU (LP5.2b)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto gamePath = std::filesystem::path{gamePathValue};
  const auto root = assembleLitRoot(gamePath);
  REQUIRE_FALSE(root.empty());
  const auto base = root / "base";
  writeTextFile(base / "materials" / "zz_lp52b.mtr", Lp52bMaterials);
  std::filesystem::create_directories(base / "textures" / "lp52b");
  const auto writeTga = [&](const std::string& name, const test::Rgba& image) {
    auto file = std::ofstream{base / "textures" / "lp52b" / name, std::ios::binary};
    const auto bytes = test::tgaFile(image);
    file.write(bytes.data(), std::streamsize(bytes.size()));
  };
  writeTga("white.tga", test::flat(4, 4, 255, 255, 255));
  for (const auto& [suffix, colour] : Lp52bFaces)
  {
    writeTga(
      std::string{"cube"} + suffix + ".tga",
      test::flat(
        8, 8, std::uint8_t(colour[0]), std::uint8_t(colour[1]), std::uint8_t(colour[2])));
    // the late cube: white faces
    writeTga(std::string{"late"} + suffix + ".tga", test::flat(8, 8, 255, 255, 255));
  }
  // the cube programs' blobs beyond LP2's
  for (const auto* name :
       {"builtin/legacy/environment",
        "builtin/legacy/bumpyenvironment",
        "builtin/legacy/skybox",
        "builtin/legacy/wobblesky"})
  {
    for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
      const auto relative = shaderBlobPath(name, stage);
      REQUIRE(std::filesystem::exists(gamePath / "base" / relative));
      std::filesystem::create_directories((base / relative).parent_path());
      std::filesystem::copy_file(
        gamePath / "base" / relative,
        base / relative,
        std::filesystem::copy_options::overwrite_existing);
    }
  }
  writeTextFile(base / "maps" / "lp52b.map", lp52bMapText());
  const auto snapshotOf = [&](const std::string& text, const std::uint64_t sequence) {
    auto snapshot = snapshotOfMapText(text);
    snapshot.generation = 7;
    snapshot.sequence = sequence;
    snapshot.mapPath = (base / "maps" / "lp52b.map").string();
    return std::make_shared<const Snapshot>(std::move(snapshot));
  };
  const auto eye = EngineVec3{0, 0, 64};
  const auto camera = litCamera({0, 0, 64}, {1, 0, 0}, {0, 0, 1});
  const auto view = litViewOf(camera, 160, 120);
  const auto frameLog = root / "lp52b.framelog.csv";
  std::filesystem::remove(frameLog);

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  auto firstBumpyMessages = std::optional<std::pair<int, int>>{};
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    serviceOptions.imageBudgetBytes = 0;
    // the image work items held while the gate is closed (the late cube's), the
    // evaluations and lookups going on
    auto gate = std::make_shared<ParkingGate>();
    serviceOptions.beforeWorkItemForTesting = [gate](const std::string_view kind) {
      if (kind == "image")
      {
        gate->pass();
      }
    };
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto atExit = OpenParkingAtExit{gate};
    auto events = Events{};
    auto session = loggedRootSession(instance, events, *service, root, frameLog, true);
    auto tonemap = EngineTonemap;
    tonemap.hdrMinLuminance = 0.1f;
    tonemap.hdrMaxLuminance = 0.1001f;
    session->setTonemapForTesting(tonemap);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    REQUIRE(session->offerSnapshot(snapshotOf(lp52bMapText(), 1)));

    // the background: the shader passes skipped (D118)
    session->skipShaderPassesForTesting(true);
    auto background = settledHdr(*session, camera, 1);
    for (auto attempt = 0;
         attempt < 20 && background && !background->second.stats.stagesSkipped;
         ++attempt)
    {
      background = settledHdr(*session, camera, 1);
    }
    REQUIRE(background);
    const auto& bg = background->first;
    const auto messagesBefore = std::pair{int(messages.errors), int(messages.warnings)};

    // pass 8 drawn
    session->skipShaderPassesForTesting(false);
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
    auto drawn = settledHdr(*session, camera, 1);
    for (auto attempt = 0; attempt < 20 && drawn && drawn->second.stats.stagesSkipped;
         ++attempt)
    {
      drawn = settledHdr(*session, camera, 1);
    }
    REQUIRE(drawn);
    // P6.19: what the validation layer said from the first frames that drew 57 (its s0
    // the leaked compare-enabled LinearWrap over a colour cube and a normal map) on
    firstBumpyMessages = std::pair{
      int(messages.errors) - messagesBefore.first,
      int(messages.warnings) - messagesBefore.second};
    auto log = std::vector<LitDrawRecord>{};
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      log = access.passes.drawLogForTesting();
    }));
    const auto& stats = drawn->second.stats;
    const auto& image = drawn->first;
    CHECK_FALSE(stats.stagesSkipped);
    CHECK_FALSE(stats.stagesDisabled);
    // sky, env, bumpy_env, sky_tested, wobble_add; reflect2 (LP6), the two gl_zero,
    // gl_one stages skipped
    CHECK(stats.stageDraws == 5u);
    CHECK(stats.stageSkipped == 3u);
    CHECK(stats.stageTexgenLp6 == 1u);
    CHECK(stats.stagePending == 0u);

    const auto at = [&](const HdrImage& hdr, const EngineVec3& point) {
      const auto [x, y] = pixelOf(view, point);
      return rgbOf(hdr.at(x, y));
    };
    const auto check = [&](const char* what, const EngineVec3& point, const Rgb& model) {
      const auto got = at(image, point);
      std::cout << fmt::format(
        "LP5.2b {}: background {} drawn {} model {}\n",
        what,
        rgbText(at(bg, point)),
        rgbText(got),
        rgbText(model));
      CAPTURE(what, rgbText(got), rgbText(model));
      CHECK(near3(got, model));
    };
    const auto minus = [](const EngineVec3& a, const EngineVec3& b) {
      return EngineVec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto faceNormal = EngineVec3{-1, 0, 0};

    // skybox: texcoord = position - LOCALVIEWORIGIN (the eye), colour rpColor 1: the face
    // at the view direction, over an opaque panel (its prepass black)
    for (const auto& point :
         {EngineVec3{150, -100, 105},
          EngineVec3{150, -85, 95},
          EngineVec3{150, -115, 116}})
    {
      check("skybox", point, lp52bFaceColour(minus(point, eye)));
    }
    // reflect without a bump (55): the reflected eye vector's face, sRGBAToLinear(
    // rpColor 1 ), added over the lit wall
    {
      const auto point = EngineVec3{150, -40, 105};
      const auto r = lp52bReflect(eye, point, faceNormal);
      CHECK(lp52bFaceColour(r) == Rgb{0.0f, 1.0f, 0.0f});
      check("reflect (55)", point, plus(at(bg, point), lp52bFaceColour(r)));
    }
    // with a bump stage (57): _flat raw at unit 1, a flat normal; rpColor 1
    {
      const auto point = EngineVec3{150, 20, 105};
      const auto r = lp52bReflect(eye, point, faceNormal);
      check("reflect with a bump (57)", point, plus(at(bg, point), lp52bFaceColour(r)));
    }
    // reflect2: not drawn, the wall behind shows
    check("reflect2 skipped", EngineVec3{150, 80, 105}, at(bg, EngineVec3{150, 80, 105}));
    // the prepass's alpha-tested skybox: its whole surface's depth hides the lit wall
    // behind, pass 8 then draws the sky EQUAL; the gl_zero, gl_one twin shows the
    // prepass's black (rpColor 0) alone
    {
      const auto point = EngineVec3{150, -100, 40};
      check("alpha-tested skybox", point, lp52bFaceColour(minus(point, eye)));
      check("alpha-tested skybox, prepass alone", EngineVec3{150, -40, 40}, Rgb{0, 0, 0});
      // the background frame (pass 8 skipped) shows the prepass's black too: the wall
      // behind is hidden by the depth alone
      CHECK(near3(at(bg, point), Rgb{0, 0, 0}));
    }
    // the prepass's alpha-tested wobbleSky stage: its colour the sky's texel at position
    // - 0 (LOCALVIEWORIGIN 0 in the prepass, SC528) through the identity transform (rates
    // 0), the vertex colour 1 (SVC_IGNORE): +z's face here; the blend add twin adds pass
    // 8's sky at position - the eye: +x's face (the points away from a face's edge, where
    // the cube's seamless bilinear filter reads the next face)
    {
      const auto origin = EngineVec3{0, 0, 0};
      const auto maskPoint = EngineVec3{120, 25, 148};
      CHECK(lp52bFaceColour(minus(maskPoint, origin)) == Rgb{0.0f, 1.0f, 1.0f});
      check(
        "wobbleSky, prepass alone", maskPoint, lp52bFaceColour(minus(maskPoint, origin)));
      const auto addPoint = EngineVec3{120, 65, 148};
      CHECK(lp52bFaceColour(minus(addPoint, eye)) == Rgb{1.0f, 0.0f, 0.0f});
      check(
        "wobbleSky, prepass plus pass 8",
        addPoint,
        plus(
          lp52bFaceColour(minus(addPoint, origin)),
          lp52bFaceColour(minus(addPoint, eye))));
    }

    // the draw log: the programs by texgen
    {
      auto programs = std::map<std::string, std::set<int>>{};
      auto prepassPrograms = std::map<std::string, std::set<int>>{};
      for (const auto& record : log)
      {
        if (record.pass == LitDrawRecord::Pass::Stage)
        {
          programs[record.chunk.material].insert(record.program);
        }
        if (record.pass == LitDrawRecord::Pass::Prepass)
        {
          prepassPrograms[record.chunk.material].insert(record.program);
        }
      }
      CHECK(programs["textures/lp52b/sky"] == std::set{EngineBuiltinSkyBox});
      CHECK(programs["textures/lp52b/env"] == std::set{EngineBuiltinEnvironment});
      CHECK(
        programs["textures/lp52b/bumpy_env"] == std::set{EngineBuiltinBumpyEnvironment});
      CHECK(programs["textures/lp52b/wobble_add"] == std::set{EngineBuiltinWobbleSky});
      CHECK_FALSE(programs.contains("textures/lp52b/reflect2"));
      CHECK_FALSE(programs.contains("textures/lp52b/sky_mask"));
      CHECK(
        prepassPrograms["textures/lp52b/sky_tested"] == std::set{EngineBuiltinSkyBox});
      CHECK(prepassPrograms["textures/lp52b/sky_mask"] == std::set{EngineBuiltinSkyBox});
      CHECK(
        prepassPrograms["textures/lp52b/wobble_mask"]
        == std::set{EngineBuiltinWobbleSky});
      // the opaque sky panel: the prepass's fast-path depth draw
      CHECK(prepassPrograms["textures/lp52b/sky"] == std::set{EngineBuiltinDepth});
    }

    // D124, P6.17: a cube not written yet: the stage drawn with the zero cube (black),
    // the frame unconverged and counted; then it lands and is drawn
    {
      // the image work items held: the late cube stays Pending
      gate->close();
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        access.passes.setDrawLogForTesting(true);
      }));
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(snapshotOf(lp52bMapText(true), 2)));
      const auto latePoint = EngineVec3{150, 20, 40};
      auto lateState = std::optional<ImageSlots::State>{};
      const auto pending = hdrOfFrame(*session, [&](const RenderFrameAccess& access) {
        if (access.arenas.pending())
        {
          return false;
        }
        // the late panel drawn by pass 8 in this frame
        const auto& frameLog = access.passes.drawLogForTesting();
        if (std::ranges::none_of(frameLog, [](const LitDrawRecord& record) {
              return record.pass == LitDrawRecord::Pass::Stage
                     && record.chunk.material == "textures/lp52b/sky_late";
            }))
        {
          return false;
        }
        for (const auto& surface : access.passes.lists().drawSurfs)
        {
          if (
            surface.chunk && surface.chunk->key.material == "textures/lp52b/sky_late"
            && surface.material && !surface.material->stages.empty()
            && surface.material->stages[0].image)
          {
            if (const auto id = access.images.find(*surface.material->stages[0].image))
            {
              const auto* slot = access.images.slot(*id);
              const auto binding = access.images.binding(*id);
              if (
                slot
                && (slot->state == ImageSlots::State::Pending || !binding.texture || binding.intrinsic)
                && access.passes.lastStats().stagePending > 0)
              {
                lateState = slot->state;
                return true;
              }
            }
          }
        }
        return false;
      });
      REQUIRE(pending);
      CHECK(pending->second.stats.unconverged);
      CHECK(pending->second.stats.stagePending == 1u);
      CHECK(pending->second.stats.stageDraws == 6u);
      // the zero cube, black (the prepass's black under it), not a slot's 2D stand-in
      CHECK(near3(at(pending->first, latePoint), Rgb{0, 0, 0}));
      std::cout << fmt::format(
        "LP5.2b late cube: drawn with the zero cube while its slot was {}: {}\n",
        lateState ? int(*lateState) : -1,
        rgbText(at(pending->first, latePoint)));
      gate->open();
      auto landed = settledHdr(*session, camera, 1);
      for (auto attempt = 0;
           attempt < 20 && landed && landed->second.stats.stageDraws != 6u;
           ++attempt)
      {
        landed = settledHdr(*session, camera, 1);
      }
      REQUIRE(landed);
      CHECK(landed->second.stats.stagePending == 0u);
      CHECK(near3(at(landed->first, latePoint), Rgb{1, 1, 1}));
    }

    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(false);
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
  REQUIRE(firstBumpyMessages);
  std::cout << fmt::format(
    "LP5.2b validation from the first frames with 57 on: {} errors, {} warnings\n",
    firstBumpyMessages->first,
    firstBumpyMessages->second);
  messages.print("LP5.2b cube and sky texgens, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

namespace
{

// LP5.F: the scene's chunks of a material in the next frame: how many there are, and how
// many of them are provisional (Chunk::provisional: built at a C21 Default size)
std::optional<std::pair<std::size_t, std::size_t>> chunksOf(
  LitSession& session, const std::string& material)
{
  auto counts = std::make_shared<std::pair<std::size_t, std::size_t>>(0, 0);
  if (!inFrame(session, [counts, material](RenderFrameAccess& access) {
        for (const auto& [key, chunk] : access.chunks)
        {
          if (key.material == material)
          {
            ++counts->first;
            counts->second += chunk.provisional ? 1 : 0;
          }
        }
      }))
  {
    return std::nullopt;
  }
  return *counts;
}

// LP5.F: a session's start-up on a map: the frames it drew from the offer up to the first
// frame the product called settled and up to the frame from which it stayed settled (the
// frame log's settled column), every frame it drew, and the milliseconds from the offer
// to that last turn to settled
struct StartUp
{
  std::size_t firstSettledFrame = 0;
  std::size_t settledFrame = 0;
  std::size_t frames = 0;
  double milliseconds = 0.0;
};

std::optional<StartUp> startUp(
  const VulkanInstance& instance,
  const std::filesystem::path& root,
  const std::string& mapText,
  const std::filesystem::path& logPath)
{
  std::filesystem::remove(logPath);
  auto result = StartUp{};
  auto quiet = false;
  auto framesBefore = std::uint64_t{0};
  {
    auto service = rootService(root);
    auto events = Events{};
    auto session = loggedRootSession(instance, events, *service, root, logPath);
    session->setCamera(litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0}));
    session->setExposed(true);
    if (waitFor(
          [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
          5000ms,
          false))
    {
      // the frames of the empty scene before the offer are not the start-up's
      std::this_thread::sleep_for(200ms);
      framesBefore = session->framesPresented();
      const auto start = std::chrono::steady_clock::now();
      if (session->offerSnapshot(lp22aSnapshot(mapText, root, 7, 1)))
      {
        // settled, and no frame drawn for 500 ms since
        auto frames = session->framesPresented();
        auto changedAt = std::chrono::steady_clock::now();
        auto wasSettled = false;
        auto settledAt = start;
        const auto deadline = start + 60s;
        while (std::chrono::steady_clock::now() < deadline)
        {
          const auto now = std::chrono::steady_clock::now();
          const auto settled = session->lastFrameSettledForTesting();
          if (const auto drawn = session->framesPresented(); drawn != frames)
          {
            frames = drawn;
            changedAt = now;
          }
          if (settled && !wasSettled)
          {
            settledAt = now;
          }
          wasSettled = settled;
          if (settled && frames > framesBefore && now - changedAt >= 500ms)
          {
            quiet = true;
            break;
          }
          std::this_thread::sleep_for(1ms);
        }
        result.milliseconds =
          std::chrono::duration<double, std::milli>{settledAt - start}.count();
      }
    }
    teardown(*session);
    CHECK(events.count(SessionEvent::Error) == 0u);
    session.reset();
    service->requestStop();
    service->join();
  }
  if (!quiet)
  {
    return std::nullopt;
  }

  const auto rows = readCsv(logPath);
  if (rows.empty())
  {
    return std::nullopt;
  }
  const auto column = [&](const std::string& name) {
    const auto it = std::ranges::find(rows[0], name);
    return std::size_t(it - rows[0].begin());
  };
  const auto eventColumn = column("event");
  const auto frameColumn = column("frame");
  const auto settledColumn = column("settled");
  auto previous = false;
  for (std::size_t i = 1; i < rows.size(); ++i)
  {
    const auto& row = rows[i];
    if (
      row.size() <= settledColumn || row[eventColumn] != "frame"
      || std::stoull(row[frameColumn]) < framesBefore)
    {
      continue;
    }
    ++result.frames;
    const auto settled = row[settledColumn] == "1";
    if (settled && result.firstSettledFrame == 0)
    {
      result.firstSettledFrame = result.frames;
    }
    if (settled && !previous)
    {
      result.settledFrame = result.frames;
    }
    previous = settled;
  }
  return result;
}

} // namespace

// LP5.F (D77 as A41 restates it, A6, D18; plans/lit-preview/43-FLOOR-PROBE-FLAKE.md): a
// chunk built at the C21 Default size while its material's measure is still queued
// (Chunk::provisional) is drawn provisionally, at the 8 x 8 default, and no frame that
// draws it is settled. The measures held (MaterialService::holdMeasuresForTesting) while
// every image is served give that picture on demand: LP2.2a's floor probe through the
// 8 x 8 UVs (texel ~ (225, 226, 40) of floor.tga). Then the start-up's frames to the
// settled frame, on the LP2.2a fixture and with 2000 boxes more. Hidden: it needs a GPU
// and the game's blobs.
TEST_CASE("LitSession provisional editor-image sizes on a GPU (LP5.F)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto lit = litCamera({60, 0, 80}, {0, 0, -1}, {1, 0, 0});
  const auto floor = std::string{"textures/lp1b/floor"};
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;

  SECTION("the measures held: the floor drawn at 8 x 8, never settled; then measured")
  {
    {
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      auto service = rootService(root);
      service->holdMeasuresForTesting(true);
      auto events = Events{};
      auto session = rootSession(instance, events, *service, root);
      session->setExposed(true);
      REQUIRE(waitFor(
        [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; },
        5000ms,
        false));
      REQUIRE(session->offerSnapshot(lp22aSnapshot(lp22aMapText(), root, 7, 1)));

      // every slot answered and written, nothing pending in the frame's lists: the
      // floor's slot unmeasured at the registry's Default, its chunks provisional
      const auto held = frameHdr(*session, lit, 1, 60000ms, false);
      REQUIRE(held);
      const auto slot = service->table()->find(floor);
      REQUIRE(slot);
      REQUIRE(slot->entry);
      CHECK_FALSE(slot->sizeMeasured);
      CHECK(slot->entry->editorImageSize.state == EditorImageSize::State::Default);
      const auto heldChunks = chunksOf(*session, floor);
      REQUIRE(heldChunks);
      CHECK(heldChunks->first > 0u);
      CHECK(heldChunks->second == heldChunks->first);
      // the 8 x 8 UVs' picture (43-FLOOR-PROBE-FLAKE.md section 2)
      const auto heldCentre = held->first.at(80, 60);
      CHECK(heldCentre[0] == Catch::Approx(2.2441f).margin(0.01f));
      CHECK(heldCentre[1] == Catch::Approx(2.2773f).margin(0.01f));
      CHECK(heldCentre[2] == Catch::Approx(0.0608f).margin(0.01f));
      // D77 (A41): no frame that draws a chunk built against an unmeasured size is
      // settled
      CHECK_FALSE(held->second.convergence.settled());
      CHECK(held->second.convergence.provisionalChunks);
      CHECK_FALSE(session->lastFrameSettledForTesting());
      // and the test's predicate takes none of these frames: the probe is not read
      CHECK_FALSE(settledHdr(*session, lit, 1, 3000ms));
      CHECK_FALSE(session->lastFrameSettledForTesting());

      // released: the measure lands, the floor's cells are built again at 64 x 32, and
      // the first settled frame is the measured floor's
      service->holdMeasuresForTesting(false);
      const auto measured = settledHdr(*session, lit, 1);
      REQUIRE(measured);
      const auto measuredSlot = service->table()->find(floor);
      REQUIRE(measuredSlot);
      CHECK(measuredSlot->sizeMeasured);
      const auto measuredChunks = chunksOf(*session, floor);
      REQUIRE(measuredChunks);
      CHECK(measuredChunks->first > 0u);
      CHECK(measuredChunks->second == 0u);
      const auto centre = measured->first.at(80, 60);
      CHECK(centre[0] == Catch::Approx(0.0309f).margin(0.001f));
      CHECK(centre[1] == Catch::Approx(1.4531f).margin(0.001f));
      CHECK(centre[2] == 0.0f);
      CHECK(measured->second.convergence.settled());
      std::cout << fmt::format(
        "LP5.F the measures held: the floor ({:.4f} {:.4f} {:.4f}), {} of {} floor "
        "chunks provisional, settled {}; measured: ({:.4f} {:.4f} {:.4f})\n",
        heldCentre[0],
        heldCentre[1],
        heldCentre[2],
        heldChunks->second,
        heldChunks->first,
        held->second.convergence.settled(),
        centre[0],
        centre[1],
        centre[2]);

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
    messages.print("LP5.F measures held, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  SECTION("the start-up's frames to the settled frame: the fixture, and 2000 boxes more")
  {
    {
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto logPath =
        std::filesystem::temp_directory_path() / "TbLitLibTest-lp5f-startup.csv";
      const auto maps = std::vector<std::pair<std::string, std::string>>{
        {"the LP2.2a fixture", lp22aMapText()},
        {"the fixture with 2000 boxes", feedBatchMapText(2000)},
      };
      for (const auto& [name, text] : maps)
      {
        CAPTURE(name);
        const auto measured = startUp(instance, root, text, logPath);
        REQUIRE(measured);
        CHECK(measured->settledFrame > 0u);
        std::cout << fmt::format(
          "LP5.F start-up, {}: settled from frame {} on (the first settled frame {}), "
          "{} frames, {:.0f} ms\n",
          name,
          measured->settledFrame,
          measured->firstSettledFrame,
          measured->frames,
          measured->milliseconds);
      }
      std::filesystem::remove(logPath);
    }
    CHECK(resetDispatcher());
    messages.print("LP5.F start-up, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
}

namespace
{

// LP5.3: the fog and blend materials of the GPU case: fogs/basicFog's shape
// (fogs.mtr:14-22) and a blend light adding its colour over the intrinsic no-falloff
// table; a white wall and a noSelfShadow twin of it
const auto Lp53Materials = std::string{
  "textures/lp53/white\n{\n\tqer_editorimage textures/lp1b/wall.tga\n"
  "\tdiffusemap guis/assets/white.tga\n}\n"
  "textures/lp53/noself\n{\n\tnoSelfShadow\n\tqer_editorimage textures/lp1b/wall.tga\n"
  "\tdiffusemap guis/assets/white.tga\n}\n"
  "lights/lp53/fog\n{\n\tfogLight\n\tnoShadows\n\t{\n\t\tmap _fog\n\t\tcolored\n\t}\n}\n"
  "lights/lp53/blend\n{\n\tblendLight\n\tnoShadows\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tblend add\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"};

// a fog light's axis with its z along world +x: the fog plane faces away from a camera
// looking +x
constexpr auto Lp53FogAxis = "\"rotation\" \"0 0 -1 0 1 0 1 0 0\"\n";

/**
 * LP5.3: a wall at x 500 lit head-on by a parallel light from -x (lp52a's light), and the
 * extra brushes and entities of a scenario
 */
std::string lp53MapText(const std::string& brushes, const std::string& entities)
{
  auto text = std::string{"{\n\"classname\" \"worldspawn\"\n"};
  text += boxBrush(500, -600, -300, 516, 600, 500, "lp53/white");
  text += brushes;
  text += "}\n";
  text +=
    "{\n\"classname\" \"light\"\n\"name\" \"lp53_parallel\"\n\"origin\" \"250 0 64\"\n"
    "\"light_radius\" \"400 400 400\"\n\"light_center\" \"-1 0 0\"\n\"parallel\" \"1\"\n"
    "\"texture\" \"lights/lp22a/flatclamp\"\n\"_color\" \"0.3 0.3 0.3\"\n"
    "\"noshadows\" \"1\"\n}\n";
  text += entities;
  return text;
}

std::string lp53Light(const std::string& name, const std::string& keys)
{
  return fmt::format("{{\n\"classname\" \"light\"\n\"name\" \"{}\"\n{}}}\n", name, keys);
}

// a bilinear sample (linearClamp: texel centres at ( i + 0.5 ) / size, clamped) of an
// intrinsic recipe's alpha, 0..1
float modelAlphaSample(const IntrinsicRecipe& recipe, const float s, const float t)
{
  const auto texel = [&](int x, int y) {
    x = std::clamp(x, 0, recipe.width - 1);
    y = std::clamp(y, 0, recipe.height - 1);
    return float(
             recipe.rgba
               [(std::size_t(y) * std::size_t(recipe.width) + std::size_t(x)) * 4 + 3])
           / 255.0f;
  };
  const auto u = s * float(recipe.width) - 0.5f;
  const auto v = t * float(recipe.height) - 0.5f;
  const auto x0 = int(std::floor(u));
  const auto y0 = int(std::floor(v));
  const auto fx = u - float(x0);
  const auto fy = v - float(y0);
  const auto top = texel(x0, y0) * (1.0f - fx) + texel(x0 + 1, y0) * fx;
  const auto bottom = texel(x0, y0 + 1) * (1.0f - fx) + texel(x0 + 1, y0 + 1) * fx;
  return top * (1.0f - fy) + bottom * fy;
}

} // namespace

// LP5.3 (C49, C50, D122-D124): pass 9 on a GPU: FogPass over a lit surface inside a fog
// volume and the cube from outside, each pixel against a CPU model of _fog( p0 · pos, 0.5
// ) · _fogEnter( p3, p2 · pos ) · rpColor (fog.ps.hlsl: sRGBAToLinearRGBA, the alpha
// too) blended SRC_ALPHA over the frame without the fog (the background); a blend light
// whose surfaces are all local beside a twin with a global surface; the fogged-out portal
// on the LP1b fixture's tree, open while its fog light's registers are first-seen; the
// frame log's columns. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession fog and blend lights on a GPU (LP5.3)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto gamePath = std::filesystem::path{gamePathValue};
  const auto root = assembleLitRoot(gamePath);
  REQUIRE_FALSE(root.empty());
  const auto base = root / "base";
  writeTextFile(base / "materials" / "zz_lp53.mtr", Lp53Materials);
  // FogAllLights' programs beyond LP2's
  for (const auto* name : {"builtin/fog/fog", "builtin/fog/blendlight"})
  {
    for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
      const auto relative = shaderBlobPath(name, stage);
      REQUIRE(std::filesystem::exists(gamePath / "base" / relative));
      std::filesystem::create_directories((base / relative).parent_path());
      std::filesystem::copy_file(
        gamePath / "base" / relative,
        base / relative,
        std::filesystem::copy_options::overwrite_existing);
    }
  }
  writeTextFile(base / "maps" / "lp53.map", lp53MapText("", ""));
  auto sequence = std::uint64_t{0};
  // each snapshot replaces the one before it whole (paritySnapshot's rule): its ids
  // offset by its sequence, every id of the one before removed (B0.4), so that a light
  // or brush the next map lacks leaves the scene
  auto previousIds = std::vector<PreviewId>{};
  const auto replacing = [&](Snapshot snapshot) {
    const auto offset = PreviewId{snapshot.sequence} << 32;
    for (auto& entity : snapshot.entities)
    {
      entity.id += offset;
    }
    for (auto& node : snapshot.nodes)
    {
      node.id += offset;
      node.entity += offset;
    }
    snapshot.removed = std::exchange(previousIds, snapshotIds(snapshot));
    return std::make_shared<const Snapshot>(std::move(snapshot));
  };
  const auto snapshotOf = [&](const std::string& text) {
    auto snapshot = snapshotOfMapText(text);
    snapshot.generation = 7;
    snapshot.sequence = ++sequence;
    snapshot.mapPath = (base / "maps" / "lp53.map").string();
    return replacing(std::move(snapshot));
  };
  const auto camera = litCamera({0, 0, 64}, {1, 0, 0}, {0, 0, 1});
  const auto view = litViewOf(camera, 160, 120);
  const auto frameLog = root / "lp53.framelog.csv";
  std::filesystem::remove(frameLog);
  const auto fogRecipe = intrinsicRecipe("_fog");
  const auto fogEnterRecipe = intrinsicRecipe("_fogEnter");
  REQUIRE(fogRecipe);
  REQUIRE(fogEnterRecipe);

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto gate = std::make_shared<ParkingGate>();
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    serviceOptions.imageBudgetBytes = 0;
    serviceOptions.beforeTakeWorkForTesting = [gate] { gate->pass(); };
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto atExit = OpenParkingAtExit{gate};
    auto events = Events{};
    auto session = loggedRootSession(instance, events, *service, root, frameLog);
    auto tonemap = EngineTonemap;
    tonemap.hdrMinLuminance = 0.1f;
    tonemap.hdrMaxLuminance = 0.1001f;
    session->setTonemapForTesting(tonemap);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));

    // a settled frame of a map text, its stats meeting pred
    const auto settledOf = [&](
                             const std::string& text,
                             const std::function<bool(const LitFrameStats&)>& pred) {
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(snapshotOf(text)));
      // the snapshot applied (a settled frame of the last one may come first)
      REQUIRE(framesUntil(*session, camera, pred));
      auto frame = settledHdr(*session, camera, 1);
      for (auto attempt = 0; attempt < 40 && frame && !pred(frame->second.stats);
           ++attempt)
      {
        frame = settledHdr(*session, camera, 1);
      }
      REQUIRE(frame);
      CAPTURE(
        frame->second.stats.fogLights,
        frame->second.stats.blendLights,
        frame->second.stats.lightsVisible,
        frame->second.stats.lightsInView,
        frame->second.stats.fogDraws);
      REQUIRE(pred(frame->second.stats));
      return std::move(*frame);
    };
    const auto at = [&](const HdrImage& hdr, const EngineVec3& point) {
      const auto [x, y] = pixelOf(view, point);
      return rgbOf(hdr.at(x, y));
    };
    const auto check = [&](
                         const char* what,
                         const HdrImage& image,
                         const EngineVec3& point,
                         const Rgb& model) {
      const auto got = at(image, point);
      std::cout << fmt::format(
        "LP5.3 {}: drawn {} model {}\n", what, rgbText(got), rgbText(model));
      CAPTURE(what, rgbText(got), rgbText(model));
      CHECK(near3(got, model));
    };
    // FogPass' alpha at a point (the CPU model): _fog at ( 0.5 + 0.5 · depth / distance,
    // 0.5 ), _fogEnter at ( 0.001 · s + FOG_ENTER, 0.001 · d + FOG_ENTER ) with s the
    // eye's and d the point's distance to the fog plane x = plane (normal +x), rpColor.a
    // clamp( distance ) (1 here)
    const auto fogAlpha =
      [&](const float depth, const float distance, const float plane, const float x) {
        const auto s0 = 0.5f + (0.5f / distance) * depth;
        const auto fog = modelAlphaSample(*fogRecipe, s0, 0.5f);
        const auto enter = modelAlphaSample(
          *fogEnterRecipe,
          0.001f * (camera.position.x() - plane) + 65.0f / 128.0f,
          0.001f * (x - plane) + 65.0f / 128.0f);
        std::cout << fmt::format(
          "LP5.3 fog alpha at depth {}: _fog({:.5f}) {:.5f}, _fogEnter {:.5f}\n",
          depth,
          s0,
          fog,
          enter);
        return fog * enter;
      };

    // 1. a lit panel inside a fog volume that holds the camera (x -100..450, its fog
    // plane x 450 facing +x, every point 125 or more below it), the wall beyond it
    const auto panel = boxBrush(300, -40, 24, 302, 40, 104, "lp53/white");
    const auto inside = lp53Light(
      "lp53_inside",
      std::string{"\"origin\" \"175 0 64\"\n\"light_radius\" \"250 300 275\"\n"}
        + Lp53FogAxis
        + "\"texture\" \"lights/lp53/fog\"\n\"_color\" \"0.2 0.5 0.9\"\n"
          "\"shaderParm3\" \"400\"\n\"noshadows\" \"1\"\n");
    const auto panelBackground = settledOf(
      lp53MapText(panel, ""), [](const auto& stats) { return stats.fogLights == 0u; });
    const auto fogged = settledOf(lp53MapText(panel, inside), [](const auto& stats) {
      return stats.fogLights == 1u;
    });
    {
      const auto& stats = fogged.second.stats;
      CHECK(stats.fogTimed);
      CHECK_FALSE(stats.fogDisabled);
      CHECK(stats.fogSurfaceDraws >= 1u);
      CHECK(stats.fogCubeDraws == 1u);
      CHECK(stats.fogDraws == stats.fogSurfaceDraws + stats.fogCubeDraws);
      CHECK(stats.lightsVisible == 2u);
      CHECK(stats.lightsDrawn == 1u);
      const auto point = EngineVec3{300, 10, 70};
      check(
        "a lit panel inside the fog",
        fogged.first,
        point,
        blendOver(
          {0.2f, 0.5f, 0.9f},
          fogAlpha(300.0f, 400.0f, 450.0f, 300.0f),
          at(panelBackground.first, point)));
      // the wall beyond the volume is not fogged, and nothing of the cube draws from
      // inside it
      const auto wall = EngineVec3{500, 150, 64};
      check(
        "the wall beyond the fog", fogged.first, wall, at(panelBackground.first, wall));
    }

    // 2. a surfaceless fog volume seen from outside (x 150..330, its fog plane x 330):
    // the cube's near face over the lit wall
    const auto outside = lp53Light(
      "lp53_outside",
      std::string{"\"origin\" \"240 0 64\"\n\"light_radius\" \"50 50 90\"\n"} + Lp53FogAxis
        + "\"texture\" \"lights/lp53/fog\"\n\"_color\" \"0.9 0.4 0.1\"\n"
          "\"shaderParm3\" \"400\"\n\"noshadows\" \"1\"\n");
    const auto wallBackground = settledOf(
      lp53MapText("", ""), [](const auto& stats) { return stats.fogLights == 0u; });
    const auto cube = settledOf(
      lp53MapText("", outside), [](const auto& stats) { return stats.fogLights == 1u; });
    {
      const auto& stats = cube.second.stats;
      CHECK(stats.fogSurfaceDraws == 0u);
      CHECK(stats.fogCubeDraws == 1u);
      CHECK(stats.fogDraws == 1u);
      const auto face = EngineVec3{150, 10, 70};
      check(
        "the cube's near face from outside",
        cube.first,
        face,
        blendOver(
          {0.9f, 0.4f, 0.1f},
          fogAlpha(150.0f, 400.0f, 330.0f, 150.0f),
          at(wallBackground.first, face)));
      // beside the volume, the wall as it was
      const auto beside = EngineVec3{500, 200, 64};
      check(
        "the wall beside the cube", cube.first, beside, at(wallBackground.first, beside));
    }

    // 3. two blend lights without noshadows 1, each holding one panel: a noSelfShadow one
    // (every surface local: BlendLight returns) and a plain one (drawn: blend add of
    // _white x _noFalloff x rpColor)
    const auto panels = boxBrush(300, -100, 24, 302, -20, 104, "lp53/noself")
                        + boxBrush(300, 20, 24, 302, 100, 104, "lp53/white");
    const auto blends =
      lp53Light(
        "lp53_blend_local",
        "\"origin\" \"301 -60 64\"\n\"light_radius\" \"10 45 45\"\n"
        "\"texture\" \"lights/lp53/blend\"\n\"_color\" \"0.4 0.3 0.2\"\n")
      + lp53Light(
        "lp53_blend_global",
        "\"origin\" \"301 60 64\"\n\"light_radius\" \"10 45 45\"\n"
        "\"texture\" \"lights/lp53/blend\"\n\"_color\" \"0.4 0.3 0.2\"\n");
    const auto panelsBackground =
      settledOf(lp53MapText(panels, ""), [](const auto& stats) {
        return stats.blendLights == 0u && stats.lightsVisible == 1u;
      });
    const auto blended = settledOf(lp53MapText(panels, blends), [](const auto& stats) {
      return stats.lightsVisible == 3u;
    });
    {
      const auto& stats = blended.second.stats;
      CHECK(stats.blendLights == 1u);
      CHECK(stats.blendDraws == 1u);
      const auto local = EngineVec3{300, -60, 64};
      const auto global = EngineVec3{300, 60, 64};
      check(
        "a blend light whose surfaces are all local",
        blended.first,
        local,
        at(panelsBackground.first, local));
      check(
        "its twin over a plain panel",
        blended.first,
        global,
        plus(at(panelsBackground.first, global), linearRgb({0.4f, 0.3f, 0.2f})));
    }

    // 4. the fogged-out portal on the LP1b fixture's tree: from area 0 at x 200 looking
    // west, the doorway (x 8) 192 ahead; lp1b_light's volume (64 0 96, radius 200) holds
    // it; a fog distance of 150 closes it, its registers first-seen leave it open
    const auto west = litCamera({200, 0, 64}, {-1, 0, 0}, {0, 0, 1});
    const auto treeOf = [&](const std::vector<std::string>& lights) {
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(
        replacing(*lp22aSnapshot(lp51MapText(lights), root, 7, ++sequence))));
    };
    const auto fogKeys = [](const char* parm) {
      return fmt::format(
        "\"name\" \"lp53_portal_fog\"\n\"origin\" \"64 0 96\"\n\"light_radius\" \"200 "
        "200 "
        "200\"\n\"texture\" \"lights/lp53/fog\"\n\"_color\" \"0.5 0.5 0.5\"\n"
        "\"shaderParm3\" \"{}\"\n",
        parm);
    };
    treeOf({lp51Light("lp53_tree_light", "", 1.0f)});
    const auto open = framesUntil(*session, west, [](const LitFrameStats& stats) {
      return stats.areasFlowed == 2u && !stats.unconverged;
    });
    REQUIRE(open);
    CHECK(open->portalsFogged == 0u);
    // the fog material in the table before the parse thread is held
    service->lookup("lights/lp53/fog");
    REQUIRE(waitFor(
      [&] { return service->table()->find("lights/lp53/fog") != nullptr; },
      10000ms,
      false));
    // held: the fog light's evaluation is first-seen, so its portal stays open
    gate->close();
    REQUIRE(gate->park([&] { service->lookup("textures/lp53/lp53_park_lookup"); }));
    treeOf({lp51Light("lp53_tree_light", "", 1.0f), fogKeys("150")});
    const auto pending = framesUntil(*session, west, [](const LitFrameStats& stats) {
      return stats.portalFogPending == 1u;
    });
    REQUIRE(pending);
    CHECK(pending->areasFlowed == 2u);
    CHECK(pending->unconverged);
    gate->open();
    const auto closed = framesUntil(*session, west, [](const LitFrameStats& stats) {
      return stats.portalsFogged >= 1u && !stats.unconverged;
    });
    REQUIRE(closed);
    CHECK(closed->areasFlowed == 1u);
    CHECK(closed->portalFogPending == 0u);
    std::cout << fmt::format(
      "LP5.3 fog portal: open {} areas flowed; first-seen registers {} areas flowed, "
      "unconverged; fogged out {} areas flowed ({} sides)\n",
      open->areasFlowed,
      pending->areasFlowed,
      closed->areasFlowed,
      closed->portalsFogged);
    // a fog distance of 300: the doorway within it, flowed
    treeOf({lp51Light("lp53_tree_light", "", 1.0f), fogKeys("300")});
    const auto within = framesUntil(*session, west, [](const LitFrameStats& stats) {
      return stats.portalsFogged == 0u && stats.fogLights == 1u && !stats.unconverged;
    });
    REQUIRE(within);
    CHECK(within->areasFlowed == 2u);

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
  messages.print("LP5.3 fog and blend lights, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);

  // the frame log's three columns: a frame with fog draws and pass 9's time, one with a
  // blend draw
  {
    const auto text = readTextFile(frameLog);
    auto stream = std::istringstream{text};
    auto line = std::string{};
    auto header = std::vector<std::string>{};
    auto fogRow = false;
    auto blendRow = false;
    while (std::getline(stream, line))
    {
      auto fields = std::vector<std::string>{};
      auto field = std::string{};
      for (const auto c : line)
      {
        if (c == ',')
        {
          fields.push_back(field);
          field.clear();
        }
        else
        {
          field += c;
        }
      }
      fields.push_back(field);
      if (header.empty())
      {
        header = fields;
        REQUIRE(header.size() == FrameLogFields);
        CHECK(header[54] == "fog_draws");
        CHECK(header[55] == "blend_draws");
        CHECK(header[56] == "gpu_fog_ms");
        continue;
      }
      if (fields.size() == FrameLogFields && fields[1] == "frame")
      {
        fogRow =
          fogRow || (!fields[54].empty() && fields[54] != "0" && !fields[56].empty());
        blendRow = blendRow || (!fields[55].empty() && fields[55] != "0");
      }
    }
    CHECK(fogRow);
    CHECK(blendRow);
  }
}

namespace
{

// LP5.G: the root's zz_lp22a.mtr with lights/squarelight1, the default light texture
// (idLight::Spawn), at half its colour, as an editor's save of the file leaves it (the
// stage of HalfSquareLight's re-parse)
std::string lp5gHalfLightMaterials()
{
  auto text = Lp22aMaterials;
  const auto stage = std::string{
    "lights/squarelight1\n{\n\tlightFalloffImage _noFalloff\n\t{\n\t\tmap _white\n"
    "\t\tcolored\n"};
  REQUIRE(text.starts_with(stage));
  text.insert(stage.size(), "\t\trgb 0.5\n");
  return text;
}

// LP5.G: what a reload's publication cost on the render side: whether a frame was drawn
// for it, from the publication to that frame's start, the frames until the session was
// quiet again, and whether the last of them was settled
struct ReloadLanding
{
  bool drawn = false;
  double latencyMs = -1.0;
  std::vector<FeedFrame> frames;
  bool settled = false;
};

double millisecondsSince(
  const std::chrono::steady_clock::time_point from,
  const std::chrono::steady_clock::time_point to)
{
  return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace

namespace
{

// the LP5.G addendum: a w x h TGA of one colour (uncompressed, 32 bits, alpha 255)
std::string constantTga(
  const int width, const int height, const std::array<std::uint8_t, 3>& rgb)
{
  auto tga = std::string(18, '\0');
  tga[2] = 2;
  tga[12] = char(width & 0xff);
  tga[13] = char(width >> 8);
  tga[14] = char(height & 0xff);
  tga[15] = char(height >> 8);
  tga[16] = 32;
  tga[17] = 8;
  for (auto i = 0; i < width * height; ++i)
  {
    tga += char(rgb[2]);
    tga += char(rgb[1]);
    tga += char(rgb[0]);
    tga += char(-1);
  }
  return tga;
}

// the LP5.G addendum: what a re-saved image file cost on the render side
struct ImageLanding
{
  bool drawn = false;
  bool landed = false;
  double firstMs = -1.0;
  double swapMs = -1.0;
  std::size_t frames = 0;
  bool settled = false;
  std::string text;
};

} // namespace

// LP5.G (D18, D77, C44 generalised; plans/lit-preview/briefs/RELOAD-TRIGGER-FIX.md): a
// material reload on a settled session draws a frame without a camera move, through the
// poll's changed-file path (the editor saves the .mtr, the parse thread builds its
// registry again) and through the explicit re-parse (LPF.0b): the first frame within one
// pacer slot of the publication, the probe pixel at the new colour (the exposure pinned,
// the floor's HDR value is linear in its light's colour: half), settled again. The poll
// with no file changed and a publication that changes nothing drawn (a lookup) draw no
// frame. Hidden: it needs a GPU and the game's blobs.
TEST_CASE(
  "LitSession a material reload on a settled session on a GPU (LP5.G)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto materialFile = root / "base" / "materials" / "zz_lp22a.mtr";
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;

  SECTION("a decl file changed, and the re-parse")
  {
    {
      const auto instance = testInstance(true, false, messages);
      REQUIRE(instance.valid());
      REQUIRE(initInstanceDispatcher(instance.handles()));
      const auto logPath =
        std::filesystem::temp_directory_path() / "TbLitLibTest-lp5g-reload.csv";
      // the poll on, as the editor's material service runs it
      auto rig = FeedRig{instance, root, logPath, 100ms};
      auto& service = *rig.service;
      auto& session = *rig.session;
      const auto baseline = rig.probe(false);
      REQUIRE(baseline.frame.settled);

      // the poll with no file changed: nothing published moves what is drawn, no frame
      {
        REQUIRE(waitQuiet(session));
        const auto presented = session.framesPresented();
        std::this_thread::sleep_for(2000ms);
        CHECK(session.framesPresented() == presented);
        CHECK(service.stats().pollReloads == 0u);
      }
      // today's rule, kept: a lookup's publication draws no frame
      {
        const auto presented = session.framesPresented();
        static_cast<void>(service.lookup("textures/lp5g/plain_lookup"));
        REQUIRE(service.waitForTableForTesting(
          [](const MaterialTable& table) {
            return table.find("textures/lp5g/plain_lookup") != nullptr;
          },
          10000ms));
        std::this_thread::sleep_for(500ms);
        CHECK(session.framesPresented() == presented);
      }

      // the poll's path: the file written, the registry built again (one publication),
      // the frames from that publication until the session is quiet again; no camera move
      const auto reloadThroughPoll = [&](const std::string& text) {
        REQUIRE(waitQuiet(session));
        const auto nth = rig.listener->reloads();
        const auto registry = service.table()->registry;
        const auto reloads = service.stats().pollReloads;
        writeTextFile(materialFile, text);
        auto published = std::optional<std::chrono::steady_clock::time_point>{};
        REQUIRE(waitFor(
          [&] {
            published = rig.listener->reloadPublication(nth);
            return published.has_value();
          },
          10000ms,
          false));
        CHECK(service.stats().pollReloads == reloads + 1);
        CHECK(service.table()->registry != registry);
        auto landing = ReloadLanding{};
        // before LP5.G: no frame within 2 s (none until the camera moved)
        landing.drawn =
          waitFor([&] { return !rig.frames->since(*published).empty(); }, 2000ms, false);
        CHECK(landing.drawn);
        if (landing.drawn)
        {
          REQUIRE(waitQuiet(session));
          landing.frames = rig.frames->since(*published);
          landing.latencyMs = millisecondsSince(*published, landing.frames.front().start);
          landing.settled = session.lastFrameSettledForTesting();
        }
        return landing;
      };

      const auto lightBefore = namedLight(*session.lightTable(), "lp1b_light");
      REQUIRE(lightBefore);
      const auto half = reloadThroughPoll(lp5gHalfLightMaterials());
      CHECK(half.latencyMs < 50.0);
      CHECK(half.settled);
      const auto halfProbe = rig.probe(false);
      CHECK(halfProbe.frame.settled);
      // the floor's probe is lp1b_light's alone: half
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(halfProbe.hdr[c] == Catch::Approx(0.5f * baseline.hdr[c]).epsilon(1e-3));
      }
      const auto lightHalf = namedLight(*session.lightTable(), "lp1b_light");
      REQUIRE(lightHalf);
      CHECK(lightHalf->version > lightBefore->version);
      // the file as it was: the picture back
      const auto restored = reloadThroughPoll(Lp22aMaterials);
      CHECK(restored.latencyMs < 50.0);
      CHECK(restored.settled);
      const auto restoredProbe = rig.probe(false);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(restoredProbe.hdr[c] == Catch::Approx(baseline.hdr[c]).epsilon(1e-3));
      }

      // the explicit re-parse (LPF.0b), the same stage: the feed trigger (LPF.1), kept
      const auto item = rig.listener->items();
      const auto reparsed = rig.reparse("lights/squarelight1", HalfSquareLight);
      const auto reparsePublished = rig.listener->itemPublication(item);
      REQUIRE(reparsePublished);
      REQUIRE_FALSE(reparsed.empty());
      const auto reparseMs = millisecondsSince(*reparsePublished, reparsed.front().start);
      CHECK(reparseMs < 50.0);
      const auto reparseProbe = rig.probe(false);
      CHECK(reparseProbe.frame.settled);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(reparseProbe.hdr[c] == Catch::Approx(0.5f * baseline.hdr[c]).epsilon(1e-3));
      }
      const auto cleared = rig.reparse("lights/squarelight1", std::nullopt);
      REQUIRE_FALSE(cleared.empty());
      const auto clearedProbe = rig.probe(false);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(clearedProbe.hdr[c] == Catch::Approx(baseline.hdr[c]).epsilon(1e-3));
      }

      std::cout << fmt::format(
        "LP5.G the poll's reload: drawn {} after {:.2f} ms, {} frames, settled {}; "
        "restored: drawn {} after {:.2f} ms, {} frames, settled {}; the re-parse: "
        "{:.2f} ms, {} frames; the floor's HDR green {:.5f} -> {:.5f} (poll) -> {:.5f} "
        "-> "
        "{:.5f} (re-parse) -> {:.5f}; light record version {} -> {}\n",
        half.drawn,
        half.latencyMs,
        half.frames.size(),
        half.settled,
        restored.drawn,
        restored.latencyMs,
        restored.frames.size(),
        restored.settled,
        reparseMs,
        reparsed.size(),
        baseline.hdr[1],
        halfProbe.hdr[1],
        restoredProbe.hdr[1],
        reparseProbe.hdr[1],
        clearedProbe.hdr[1],
        lightBefore->version,
        lightHalf->version);

      rig.close("LP5.G reload");
      std::filesystem::remove(logPath);
    }
    CHECK(resetDispatcher());
    messages.print("LP5.G reload, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }

  // the LP5.G addendum: the floor's texture re-saved by a paint program while the
  // session is settled, the poll on: the same size (64 x 32: a constant black, white,
  // then red), then another size (32 x 16, green), the probe checked through the CPU
  // model of checkProbe; (a) the floor's editor image is that file (the poll also
  // remeasures the material: sizesRevision), (b) the editor image is wall.tga (the
  // fixture's, 32 x 64), which no re-save touches: the diffuse alone changes
  // (imageChanges). Pure colours (0 or 255 per channel) are immune to sRGB and
  // compression, and red and green are the ones whose YCoCg bytes, the layout the engine
  // stores a diffuse map in, are fixed points of its gamma mips (R_MipMapWithGamma), so
  // every level holds the same texel and the model holds whichever level the sampler
  // takes (the editor image's size scales the floor's texture coordinates: level 1 in
  // (a), level 0 in (b)); magenta's, cyan's and blue's Co or Cg of 1 and cyan's and
  // yellow's Y of 191 lose one per level
  SECTION("the floor texture re-saved, the same size and another")
  {
    const auto floorFile = root / "base" / "textures" / "lp1b" / "floor.tga";
    const auto declFile = root / "base" / "materials" / "lp1bc.mtr";
    const auto declText = readTextFile(declFile);
    for (const auto editorOther : {false, true})
    {
      CAPTURE(editorOther);
      {
        auto text = declText;
        if (editorOther)
        {
          const auto from = std::string{"\tqer_editorimage\ttextures/lp1b/floor.tga\n"};
          const auto at = text.find(from);
          REQUIRE(at != std::string::npos);
          text.replace(at, from.size(), "\tqer_editorimage\ttextures/lp1b/wall.tga\n");
        }
        writeTextFile(declFile, text);
        std::filesystem::copy_file(
          litFixtureFolder() / "textures" / "lp1b" / "floor.tga",
          floorFile,
          std::filesystem::copy_options::overwrite_existing);
      }
      // read after the copy, which may give the file another time than the last loop's
      // stamps (MSVC's copy keeps the fixture's, libstdc++'s takes the clock's)
      auto stamp = std::filesystem::last_write_time(floorFile);
      {
        const auto instance = testInstance(true, false, messages);
        REQUIRE(instance.valid());
        REQUIRE(initInstanceDispatcher(instance.handles()));
        const auto logPath =
          std::filesystem::temp_directory_path() / "TbLitLibTest-lp5g-image.csv";
        auto rig = FeedRig{instance, root, logPath, 100ms};
        auto& session = *rig.session;

        // the file written, its time set 2 s after the last one: the poll's probe
        // compares the file's seconds and its clock's ticks by inequality (sameFile,
        // probeFile), and 2 s is a whole tick of every file-system clock (FAT's, the
        // coarsest), so every re-save is seen, also two within one tick and the 64 x 32
        // ones of an unchanged size; then the poll's publication (the first after the
        // write: the session is quiet and the poll publishes nothing else), the frames
        // from it until the session is quiet again; no camera move
        const auto resave = [&](
                              const int width,
                              const int height,
                              const std::array<std::uint8_t, 3>& rgb,
                              const std::string& label) {
          CAPTURE(label);
          REQUIRE(waitQuiet(session));
          const auto before = rig.frames->last();
          REQUIRE(before);
          const auto n = rig.listener->publicationCount();
          {
            auto file = std::ofstream{floorFile, std::ios::binary};
            const auto bytes = constantTga(width, height, rgb);
            file.write(bytes.data(), std::streamsize(bytes.size()));
          }
          stamp += std::chrono::seconds{2};
          std::filesystem::last_write_time(floorFile, stamp);
          auto published = std::optional<std::chrono::steady_clock::time_point>{};
          REQUIRE(waitFor(
            [&] {
              published = rig.listener->publication(n);
              return published.has_value();
            },
            5000ms,
            false));
          auto landing = ImageLanding{};
          // the defect, if any: no frame within 2 s of the poll's publication
          landing.drawn = waitFor(
            [&] { return !rig.frames->since(*published).empty(); }, 2000ms, false);
          CHECK(landing.drawn);
          if (!landing.drawn)
          {
            return landing;
          }
          // the new texture lands: Resident, another texture than before
          landing.landed = waitFor(
            [&] {
              const auto last = rig.frames->last();
              return last && last->state == ImageSlots::State::Resident
                     && last->texture != before->texture;
            },
            10000ms,
            false);
          CHECK(landing.landed);
          REQUIRE(waitQuiet(session));
          const auto frames = rig.frames->since(*published);
          landing.frames = frames.size();
          landing.firstMs = millisecondsSince(*published, frames.front().start);
          // the Replacing rule: the old texture until the swap, never the intrinsic
          const auto replaced = checkReplacement(frames, *before, label);
          for (const auto& frame : frames)
          {
            if (frame.index == replaced.swap)
            {
              landing.swapMs = millisecondsSince(*published, frame.start);
            }
          }
          landing.text = replaced.text;
          landing.settled = session.lastFrameSettledForTesting();
          CHECK(landing.settled);
          return landing;
        };

        const auto black = resave(64, 32, {0, 0, 0}, "black");
        const auto blackProbe = rig.probe(true);
        const auto white = resave(64, 32, {255, 255, 255}, "white");
        const auto whiteProbe = rig.probe(true);
        const auto colour = resave(64, 32, {255, 0, 0}, "colour");
        const auto colourProbe = rig.probe(true);
        CHECK(colourProbe.frame.settled);
        checkProbe(blackProbe, whiteProbe, colourProbe);
        // the control: another size
        const auto resized = resave(32, 16, {0, 255, 0}, "32 x 16");
        const auto resizedProbe = rig.probe(true);
        CHECK(resizedProbe.frame.settled);
        checkProbe(blackProbe, whiteProbe, resizedProbe);
        for (const auto* landing : {&black, &white, &colour, &resized})
        {
          CHECK(landing->firstMs < 50.0);
          std::cout << fmt::format(
            "LP5.G addendum, editor image {}: {} drawn {}, the first frame {:.2f} ms and "
            "the swap {:.2f} ms after the poll's publication, {} frames, settled {}; "
            "{}\n",
            editorOther ? "wall.tga" : "floor.tga",
            landing == &resized ? "32 x 16" : "64 x 32",
            landing->drawn,
            landing->firstMs,
            landing->swapMs,
            landing->frames,
            landing->settled,
            landing->text);
        }

        rig.close("LP5.G image re-saved");
        std::filesystem::remove(logPath);
      }
      CHECK(resetDispatcher());
    }
    writeTextFile(declFile, declText);
    std::filesystem::copy_file(
      litFixtureFolder() / "textures" / "lp1b" / "floor.tga",
      floorFile,
      std::filesystem::copy_options::overwrite_existing);
    messages.print("LP5.G image re-saved, validation");
    CHECK(messages.errors == 0);
    CHECK(messages.warnings == 0);
  }
}

namespace
{

// LP7.4: one presented frame's HDR pixel, whether the product called the frame settled,
// and whether it was drawn from camera
struct FramePixel
{
  std::array<float, 4> rgba = {};
  bool settled = false;
  bool ofCamera = false;
  // the adopted chunks' triangles the frame drew from
  std::size_t adoptedTriangles = 0;
};

std::optional<FramePixel> nextFramePixel(
  LitSession& session,
  const CameraState& camera,
  const std::uint32_t x,
  const std::uint32_t y)
{
  session.tableChanged();
  auto staging = nvrhi::StagingTextureHandle{};
  auto pixel = FramePixel{};
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  session.runAfterTonemapForTesting([&, done](RenderFrameAccess& access) {
    pixel.settled = access.convergence.settled();
    pixel.ofCamera = access.passes.lastStats().camera == camera;
    for (const auto& [key, chunk] : access.chunks)
    {
      pixel.adoptedTriangles += chunk.adopted ? chunk.indexes.size() / 3 : 0;
    }
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
    done->set_value();
  });
  if (future.wait_for(10000ms) != std::future_status::ready)
  {
    return std::nullopt;
  }
  const auto read = inFrame(session, [&](RenderFrameAccess& access) {
    auto pitch = std::size_t{0};
    const auto* bytes = static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
      staging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
    const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
    for (auto c = std::size_t{0}; c < 4; ++c)
    {
      pixel.rgba[c] = halfToFloat(row[x * 4 + c]);
    }
    access.device.unmapStagingTexture(staging);
    staging = nullptr;
  });
  if (!read)
  {
    return std::nullopt;
  }
  return pixel;
}

// the LP2.2a fixture's snapshot with the east wall's face (brush 3 at x 256, facing -x)
// moved by dx, its polygon and plane points alone
std::shared_ptr<const Snapshot> lp74Snapshot(
  const std::filesystem::path& root, const std::uint64_t sequence, const double dx)
{
  auto snapshot = snapshotOfMapText(lp22aMapText());
  snapshot.generation = 7;
  snapshot.sequence = sequence;
  snapshot.mapPath = (root / "base" / "maps" / "lp1b_geometry.map").string();
  for (auto& face : snapshot.faces)
  {
    auto corners = 0;
    for (auto v = face.polygon.first; v < face.polygon.first + face.polygon.count; ++v)
    {
      const auto& p = snapshot.vertices[v];
      corners += (p == vm::vec3d{256, -144, 0} || p == vm::vec3d{256, 144, 128}) ? 1 : 0;
    }
    if (corners == 2 && face.polygon.count == 4)
    {
      for (auto v = face.polygon.first; v < face.polygon.first + face.polygon.count; ++v)
      {
        snapshot.vertices[v] = snapshot.vertices[v] + vm::vec3d{dx, 0, 0};
      }
      for (auto& point : face.points)
      {
        point = point + vm::vec3d{dx, 0, 0};
      }
    }
  }
  return std::make_shared<const Snapshot>(std::move(snapshot));
}

} // namespace

// LP7.4 (C55, D77, K44): the LP2.2a fixture on a GPU with its .proc: a settled frame
// draws the adopted islands (the frame log's adopted_islands, the frame settled, no
// validation message); then the lit east wall's face moved by one unit through a
// snapshot: the next settled frame has one island fewer adopted, the wall's probe pixel
// differs from the adopted frame's by no more than K28's envelope, and no frame drawn
// meanwhile flashes a default (an unlit wall from a chunk gone, or a doubled one from the
// island and the edited face both drawn). Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession the .proc's triangles adopted on a GPU (LP7.4)", "[.][litgpu3]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  // from inside lp1b_light (64 0 96, radius 200) at the east wall, x 256
  const auto camera = litCamera({60, 0, 64}, {1, 0, 0}, {0, 0, 1});
  const auto logPath = std::filesystem::temp_directory_path() / "TbLitLibTest-lp74.csv";
  std::filesystem::remove(logPath);
  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  auto editFrame = std::uint64_t{0};
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto service = rootService(root);
    auto events = Events{};
    auto session = loggedRootSession(instance, events, *service, root, logPath);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));

    REQUIRE(session->offerSnapshot(lp74Snapshot(root, 1, 0)));
    const auto adopted = settledHdr(*session, camera, 1);
    REQUIRE(adopted);
    CHECK(adopted->second.convergence.settled());
    const auto adoptedLdr = session->readbackLdrForTesting(10000ms);
    REQUIRE(adoptedLdr);
    const auto adoptedChunks = chunksOf(*session, "textures/lp1b/wall");
    REQUIRE(adoptedChunks);
    const auto start = nextFramePixel(*session, camera, 80, 60);
    REQUIRE(start);
    CHECK(start->adoptedTriangles > 0u);

    // the face moved: every frame until the next settled one keeps the wall lit, never
    // black and never doubled
    editFrame = session->framesPresented();
    REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
    REQUIRE(session->offerSnapshot(lp74Snapshot(root, 2, -1)));
    auto between = std::vector<FramePixel>{};
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    auto settledAgain = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
      session->setCamera(camera);
      const auto pixel = nextFramePixel(*session, camera, 80, 60);
      REQUIRE(pixel);
      between.push_back(*pixel);
      // the edit's batch drawn: the east wall's island (two triangles) no more adopted
      if (
        pixel->settled && pixel->ofCamera
        && pixel->adoptedTriangles + 2u == start->adoptedTriangles)
      {
        settledAgain = true;
        break;
      }
    }
    REQUIRE(settledAgain);
    const auto moved = settledHdr(*session, camera, 1);
    REQUIRE(moved);
    const auto movedLdr = session->readbackLdrForTesting(10000ms);
    REQUIRE(movedLdr);

    const auto a = adopted->first.at(80, 60);
    const auto b = moved->first.at(80, 60);
    // the wall is lit (its texture's red and green)
    CHECK(std::max({a[0], a[1], a[2]}) > 0.25f);
    auto flashes = 0;
    for (const auto& pixel : between)
    {
      if (!pixel.ofCamera)
      {
        continue;
      }
      for (auto c = std::size_t{0}; c < 3; ++c)
      {
        const auto low = std::min(a[c], b[c]);
        const auto high = std::max(a[c], b[c]);
        if (pixel.rgba[c] < low * 0.8f - 0.01f || pixel.rgba[c] > high * 1.2f + 0.01f)
        {
          ++flashes;
        }
      }
    }
    CHECK(flashes == 0);
    // K28's envelope: up to about twelve LDR levels
    const auto at =
      [](const LdrImage& image, const std::uint32_t x, const std::uint32_t y) {
        const auto i = (std::size_t(y) * image.width + x) * 4;
        return std::array<int, 3>{image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]};
      };
    const auto la = at(*adoptedLdr, 80, 60);
    const auto lb = at(*movedLdr, 80, 60);
    auto worst = 0;
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      worst = std::max(worst, std::abs(la[c] - lb[c]));
    }
    CHECK(worst <= 12);
    std::cout << fmt::format(
      "LP7.4 on a GPU: the east wall's probe adopted ({:.4f} {:.4f} {:.4f}), the face "
      "moved by one unit ({:.4f} {:.4f} {:.4f}), LDR difference {}; {} frames between, "
      "{} flashes; wall chunks {} -> {}\n",
      a[0],
      a[1],
      a[2],
      b[0],
      b[1],
      b[2],
      worst,
      between.size(),
      flashes,
      adoptedChunks->first,
      chunksOf(*session, "textures/lp1b/wall")->first);

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
  messages.print("LP7.4 adoption, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);

  // the frame log: the settled frames before the edit adopt every planar island, the
  // last settled one after it one fewer
  const auto rows = readCsv(logPath);
  REQUIRE_FALSE(rows.empty());
  const auto column = [&](const std::string& name) {
    const auto it = std::ranges::find(rows[0], name);
    REQUIRE(it != rows[0].end());
    return std::size_t(it - rows[0].begin());
  };
  const auto eventColumn = column("event");
  const auto frameColumn = column("frame");
  const auto settledColumn = column("settled");
  const auto adoptedColumn = column("adopted_islands");
  const auto unmatchedColumn = column("unmatched_islands");
  auto before = std::optional<std::uint64_t>{};
  auto after = std::optional<std::uint64_t>{};
  auto unmatchedAfter = std::optional<std::uint64_t>{};
  for (auto i = std::size_t{1}; i < rows.size(); ++i)
  {
    const auto& row = rows[i];
    if (
      row.size() <= adoptedColumn || row[eventColumn] != "frame"
      || row[settledColumn] != "1")
    {
      continue;
    }
    const auto frame = std::stoull(row[frameColumn]);
    const auto count = std::stoull(row[adoptedColumn]);
    if (frame < editFrame)
    {
      before = count;
    }
    else
    {
      after = count;
      unmatchedAfter = std::stoull(row[unmatchedColumn]);
    }
  }
  REQUIRE(before);
  REQUIRE(after);
  CHECK(*before > 0u);
  CHECK(*after + 1u == *before);
  CHECK(unmatchedAfter == 1u);
  std::cout << fmt::format(
    "LP7.4 frame log: adopted_islands {} settled before the edit, {} after\n",
    *before,
    *after);
  std::filesystem::remove(logPath);
}

namespace
{

// LP7.2 (C52, C53): the GPU case's materials: N01's usemtl made as LP4.2a's flat slab is
// (a uniform diffuse of another colour, the implicit _flat bump: the CPU model's lit
// path), a red one, and a
// two-sided red one (ShouldCreateBackSides: a skin to it must not add back sides)
const auto Lp72Materials = std::string{
  "textures/lp7/model\n{\n\tdiffusemap textures/lp72/model.tga\n}\n"
  "textures/lp72/red\n{\n\tdiffusemap textures/lp72/red.tga\n}\n"
  "textures/lp72/redsided\n{\n\ttwoSided\n\tdiffusemap textures/lp72/red.tga\n}\n"};

const auto Lp72Skins = std::string{
  "skin skins/lp72/red\n{\n\ttextures/lp7/model\ttextures/lp72/red\n}\n"
  "skin skins/lp72/crate\n{\n\ttextures/lp1b/crate\ttextures/lp72/redsided\n}\n"};

// the LP4.2a slabs with the GPU case's light above the flat slab (and a second one in the
// west room), and these entities
std::string lp72MapText(const std::string& entities, const bool westLight = false)
{
  auto lights = std::vector<std::string>{
    "\"name\" \"lp72_light\"\n\"origin\" \"60 0 120\"\n\"light_radius\" \"200 200 "
    "200\"\n\"_color\" \"1 1 1\"\n\"texture\" \"lights/lp51/held\"\n"};
  if (westLight)
  {
    lights.emplace_back(
      "\"name\" \"lp72_west\"\n\"origin\" \"-120 80 64\"\n\"light_radius\" \"150 150 "
      "150\"\n\"_color\" \"1 1 1\"\n\"texture\" \"lights/lp51/held\"\n");
  }
  return lp51MapText(lights) + entities;
}

std::string lp72Model(
  const std::string& name,
  const std::string& model,
  const std::string& origin,
  const std::string& keys = {},
  const std::string& classname = "func_static")
{
  return fmt::format(
    "{{\n\"classname\" \"{}\"\n\"name\" \"{}\"\n\"model\" \"{}\"\n\"origin\" "
    "\"{}\"\n{}}}\n",
    classname,
    name,
    model,
    origin,
    keys);
}

// a scenario's frames and what the frame log's last settled one in them must hold
struct Lp72Expect
{
  std::string name;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::uint64_t entities = 0;
  std::uint64_t instances = 0;
  std::uint64_t skinned = 0;
};

} // namespace

// LP7.2 (C52, C53, D124, D129, D130): model entities on a GPU over the LP2.2a fixture's
// bake: LP7.1a's N01 box from the engine's own binary (its tangents the golden's) lit by
// one light against a CPU model of the lit path (LP5.1's: albedo x lambert x
// r_lightScale, the HDR within 1 %, the LDR +-1); two entities of one model drawn from
// one placement; a model through the wall seen from both rooms; skin and shader on the
// model and on the brush func_static; hide; a Pending model (nothing drawn, unconverged)
// then Ready; a Missing model's cube; a light with a model; an .md5mesh; 0 validation
// messages. Hidden: it needs a GPU and the game's blobs.
TEST_CASE("LitSession model entities on a GPU (LP7.2)", "[.][litgpu3]")
{
  const auto started = std::chrono::steady_clock::now();
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto root = assembleLitRoot(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  const auto base = root / "base";
  const auto fixture = litFixtureFolder();
  writeTextFile(base / "materials" / "zz_lp51.mtr", Lp51Materials);
  writeTextFile(base / "materials" / "zz_lp42a.mtr", Lp42aMaterials);
  writeTextFile(base / "materials" / "zz_lp72.mtr", Lp72Materials);
  writeTextFile(base / "skins" / "lp72.skin", Lp72Skins);
  std::filesystem::create_directories(base / "textures" / "lp42a");
  std::filesystem::create_directories(base / "textures" / "lp72");
  std::filesystem::create_directories(base / "lights" / "lp51");
  const auto writeTga = [](const std::filesystem::path& path, const test::Rgba& image) {
    auto file = std::ofstream{path, std::ios::binary};
    const auto bytes = test::tgaFile(image);
    file.write(bytes.data(), std::streamsize(bytes.size()));
  };
  writeTga(base / "textures" / "lp42a" / "flat.tga", test::flat(4, 4, 200, 120, 60));
  writeTga(base / "textures" / "lp72" / "red.tga", test::flat(4, 4, 220, 30, 30));
  writeTga(base / "textures" / "lp72" / "model.tga", test::flat(4, 4, 60, 160, 200));
  writeTga(base / "lights" / "lp51" / "held.tga", test::flat(4, 4, 255, 255, 255));
  // N01's engine binary under three names (no source: its time test passes, -1): the
  // golden's vertices and tangents (C51, D125)
  const auto rendermodels = base / "generated" / "rendermodels" / "models" / "test";
  std::filesystem::create_directories(rendermodels);
  for (const auto* name : {"n01.bobj", "n01p.bobj", "n01w.bobj"})
  {
    std::filesystem::copy_file(
      fixture / "models" / "test" / "n01.bobj",
      rendermodels / name,
      std::filesystem::copy_options::overwrite_existing);
  }
  const auto frameLog = root / "lp72.framelog.csv";
  std::filesystem::remove(frameLog);
  const auto down = litCamera({60, 0, 100}, {0, 0, -1}, {1, 0, 0});

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  auto expects = std::vector<Lp72Expect>{};
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto gate = std::make_shared<ParkingGate>();
    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.pollInterval = 0ms;
    serviceOptions.imageBudgetBytes = 0;
    serviceOptions.beforeTakeWorkForTesting = [gate] { gate->pass(); };
    auto service = MaterialService::create(std::move(serviceOptions))
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    const auto atExit = OpenParkingAtExit{gate};
    auto events = Events{};
    auto options = SessionOptions{};
    options.validation = true;
    options.gamePath = root;
    options.frameLogPath = frameLog;
    // LP3.1 (C57): r_skipShadows 1: the N01's and the shaded models' pixels are the
    // unshadowed interaction's
    options.skipShadowsForTesting = true;
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
    session->skipShaderPassesForTesting(true);
    auto tonemap = EngineTonemap;
    tonemap.hdrMinLuminance = 0.1f;
    tonemap.hdrMaxLuminance = 0.1001f;
    session->setTonemapForTesting(tonemap);
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));

    auto sequence = std::uint64_t{0};
    auto offered = std::shared_ptr<const Snapshot>{};
    // B0.4: a scenario's snapshot removes the entities and the nodes the last one had
    // and this one has not (snapshotOfMapText numbers entities, then nodes: a number may
    // pass from an entity to a node, so each kind is compared apart)
    auto previousEntities = std::set<PreviewId>{};
    auto previousNodes = std::set<PreviewId>{};
    const auto offer = [&](const std::string& text) {
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      auto snapshot = snapshotOfMapText(text);
      snapshot.generation = 7;
      snapshot.sequence = ++sequence;
      snapshot.mapPath = (root / "base" / "maps" / "lp1b_geometry.map").string();
      auto entities = std::set<PreviewId>{};
      for (const auto& entity : snapshot.entities)
      {
        entities.insert(entity.id);
      }
      auto nodes = std::set<PreviewId>{};
      for (const auto& node : snapshot.nodes)
      {
        nodes.insert(node.id);
      }
      auto removed = std::set<PreviewId>{};
      std::ranges::set_difference(
        previousEntities, entities, std::inserter(removed, removed.end()));
      std::ranges::set_difference(
        previousNodes, nodes, std::inserter(removed, removed.end()));
      snapshot.removed.assign(removed.begin(), removed.end());
      previousEntities = std::move(entities);
      previousNodes = std::move(nodes);
      offered = std::make_shared<const Snapshot>(std::move(snapshot));
      REQUIRE(session->offerSnapshot(offered));
    };
    const auto shoot = [&](const CameraState& camera) {
      auto readback = std::optional<AmbientReadback>{};
      for (auto attempt = 0;
           attempt < 20
           && !(
             readback && readback->frame.settled && readback->frame.lit.camera == camera);
           ++attempt)
      {
        REQUIRE(settledHdr(*session, camera, 1));
        readback = ambientReadback(*session);
      }
      REQUIRE(readback);
      REQUIRE(readback->frame.settled);
      return std::move(*readback);
    };
    // the frame log's frame number (RenderThread's m_frameIndex) of a frame now
    const auto frameNow = [&] {
      auto index = std::uint64_t{0};
      REQUIRE(
        inFrame(*session, [&](RenderFrameAccess& access) { index = access.frameIndex; }));
      return index;
    };
    const auto scenario = [&](
                            const std::string& name,
                            const std::string& text,
                            const CameraState& camera,
                            const std::uint64_t entities,
                            const std::uint64_t instances,
                            const std::uint64_t skinned) {
      auto expect = Lp72Expect{name, frameNow(), 0, entities, instances, skinned};
      offer(text);
      auto readback = shoot(camera);
      expect.last = frameNow();
      expects.push_back(expect);
      return readback;
    };
    const auto pixel =
      [](const AmbientReadback& readback, const std::uint32_t x, const std::uint32_t y) {
        return readback.hdr.at(x, y);
      };
    const auto ldrAt =
      [](const AmbientReadback& readback, const std::uint32_t x, const std::uint32_t y) {
        const auto i = (std::size_t(y) * readback.hdr.width + x) * 4;
        return std::array<int, 3>{
          readback.ldr[i], readback.ldr[i + 1], readback.ldr[i + 2]};
      };
    struct Arenas
    {
      std::size_t placedShared = 0;
      std::uint64_t usedBytes = 0;
      std::size_t instances = 0;
      std::size_t sharedInstances = 0;
      // the brush func_static lp1b_rotator's chunks: their indexes and materials
      std::size_t rotatorIndexes = 0;
      std::set<std::string> rotatorMaterials;
    };
    const auto arenas = [&] {
      auto rotator = NoPreviewId;
      for (const auto& entity : offered->entities)
      {
        for (auto k = entity.keys.first; k < entity.keys.first + entity.keys.count; ++k)
        {
          if (
            offered->string(offered->keyValues[k].key) == "name"
            && offered->string(offered->keyValues[k].value) == "lp1b_rotator")
          {
            rotator = entity.id;
          }
        }
      }
      auto state = Arenas{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        state.placedShared = access.arenas.placedShared();
        state.usedBytes = access.arenas.usedBytes();
        state.instances = access.instances.size();
        for (const auto& [key, instance] : access.instances)
        {
          state.sharedInstances = std::max(
            state.sharedInstances, access.arenas.sharedInstances(instance.placement()));
        }
        for (const auto& [key, chunk] : access.chunks)
        {
          if (key.entity == rotator)
          {
            state.rotatorIndexes += chunk.indexes.size();
            state.rotatorMaterials.insert(key.material);
          }
        }
      }));
      return state;
    };

    // 0. no model: the flat slab under the camera
    const auto bare = scenario("bare", lp72MapText(""), down, 0, 0, 0);
    const auto background = pixel(bare, 80, 60);

    // 1. the N01 box over the slab, its top at z 50: the CPU model of its lit pixel (the
    // golden's normal (0 0 1) and tangents, LP5.1's lit path)
    const auto one = scenario(
      "one",
      lp72MapText(lp72Model("lp72_a", "models/test/n01.obj", "60 0 2")),
      down,
      1,
      1,
      0);
    const auto flatKey = stageImage(*service, "textures/lp7/model", 2);
    REQUIRE(flatKey);
    const auto flatTexel = firstTexel(*service, *flatKey);
    REQUIRE(flatTexel);
    const auto albedo = modelDiffuseMap(*flatTexel);
    // the one light's origin (lp72_light)
    auto lightOrigin = EngineVec3{};
    session->lightTable()->records.forEach([&](const PreviewId&, const auto& record) {
      if (record)
      {
        lightOrigin = record->derived.projection.globalLightOrigin;
      }
    });
    for (const auto& [x, y] : {std::pair{80u, 60u}, std::pair{85u, 65u}})
    {
      // the pixel's point on the box top (z 50, 50 below the eye; fov 90 over 160)
      const auto unit = 50.0f / 80.0f;
      const auto p = std::array<float, 3>{
        60.0f - (float(y) + 0.5f - 60.0f) * unit,
        -(float(x) + 0.5f - 80.0f) * unit,
        50.0f};
      const auto d = std::array<float, 3>{
        lightOrigin[0] - p[0], lightOrigin[1] - p[1], lightOrigin[2] - p[2]};
      const auto lambert =
        std::clamp(d[2] / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 0.0f, 1.0f);
      const auto k = lambert * 3.0f;
      const auto model =
        std::array<float, 3>{albedo[0] * k, albedo[1] * k, albedo[2] * k};
      const auto hdr = pixel(one, x, y);
      const auto expected = modelLdr(model, one.exposure, one.frame);
      const auto ldr = ldrAt(one, x, y);
      CAPTURE(x, y);
      for (std::size_t c = 0; c < 3; ++c)
      {
        CHECK(hdr[c] == Catch::Approx(model[c]).epsilon(0.01));
        CHECK(std::abs(ldr[c] - expected[c]) <= 1);
      }
      std::cout << fmt::format(
        "LP7.2 N01 lit pixel ({} {}): HDR model ({:.5f} {:.5f} {:.5f}) read-back ({:.5f} "
        "{:.5f} {:.5f}); LDR model ({} {} {}) read-back ({} {} {})\n",
        x,
        y,
        model[0],
        model[1],
        model[2],
        hdr[0],
        hdr[1],
        hdr[2],
        expected[0],
        expected[1],
        expected[2],
        ldr[0],
        ldr[1],
        ldr[2]);
    }
    CHECK(one.frame.lit.modelDraws > 0u);
    const auto oneArenas = arenas();
    CHECK(oneArenas.placedShared == 1u);
    CHECK(oneArenas.instances == 1u);

    // 2. two entities of one model: one placement, its bytes once, both drawn
    const auto two = scenario(
      "two",
      lp72MapText(
        lp72Model("lp72_a", "models/test/n01.obj", "60 0 2")
        + lp72Model("lp72_b", "models/test/n01.obj", "60 60 2")),
      down,
      2,
      2,
      0);
    const auto twoArenas = arenas();
    CHECK(twoArenas.placedShared == 1u);
    CHECK(twoArenas.instances == 2u);
    CHECK(twoArenas.sharedInstances == 2u);
    CHECK(twoArenas.usedBytes == oneArenas.usedBytes);
    CHECK(two.frame.lit.modelDraws == 2 * one.frame.lit.modelDraws);

    // the model's colour is not the slab's
    CHECK(pixel(one, 80, 60) != background);

    // 3. a model through the wall beside the doorway (x -24..24 across the solid x
    // -8..8), seen from both rooms: the model where the wall was
    const auto east = litCamera({60, 80, 54}, {-1, 0, 0}, {0, 0, 1});
    const auto west = litCamera({-60, 80, 54}, {1, 0, 0}, {0, 0, 1});
    const auto wallEast =
      scenario("wall-east-bare", lp72MapText("", true), east, 0, 0, 0);
    const auto wallWest = shoot(west);
    const auto through = lp72Model("lp72_w", "models/test/n01w.obj", "0 80 30");
    const auto modelEast =
      scenario("wall-east", lp72MapText(through, true), east, 1, 1, 0);
    const auto modelWest = shoot(west);
    for (const auto* readback : {&modelEast, &modelWest})
    {
      // the model's albedo's order (60 160 200): lit, blue above green above red
      const auto rgb = pixel(*readback, 80, 60);
      CHECK(rgb[2] > rgb[1]);
      CHECK(rgb[1] > rgb[0]);
      CHECK(rgb[2] > 0.01f);
    }
    CHECK(pixel(modelEast, 80, 60) != pixel(wallEast, 80, 60));
    CHECK(pixel(modelWest, 80, 60) != pixel(wallWest, 80, 60));

    // 4. skin and shader: the model's probe moves to red; the brush func_static's crate
    // too, its vertices, back sides and st the crate's (the chunk's indexes unchanged)
    const auto skinned = scenario(
      "skin",
      lp72MapText(lp72Model(
        "lp72_a", "models/test/n01.obj", "60 0 2", "\"skin\" \"skins/lp72/red\"\n")),
      down,
      1,
      1,
      0);
    const auto shaded = scenario(
      "shader",
      lp72MapText(lp72Model(
        "lp72_a", "models/test/n01.obj", "60 0 2", "\"shader\" \"textures/lp72/red\"\n")),
      down,
      1,
      1,
      0);
    for (const auto* readback : {&skinned, &shaded})
    {
      const auto rgb = pixel(*readback, 80, 60);
      CHECK(rgb[0] > 4.0f * rgb[1]);
      CHECK(rgb[0] > 4.0f * rgb[2]);
    }
    CHECK(pixel(skinned, 80, 60) == pixel(shaded, 80, 60));
    const auto above = litCamera({152, 64, 100}, {0, 0, -1}, {1, 0, 0});
    const auto crate = scenario("crate", lp72MapText(""), above, 0, 0, 0);
    const auto crateArenas = arenas();
    CHECK(crateArenas.rotatorMaterials.contains("textures/lp1b/crate"));
    auto rotatorSkinned = lp72MapText("");
    {
      const auto at = rotatorSkinned.find("\"model\" \"lp1b_rotator\"\n");
      REQUIRE(at != std::string::npos);
      rotatorSkinned.insert(at, "\"skin\" \"skins/lp72/crate\"\n");
    }
    const auto crateRed = scenario("crate-skin", rotatorSkinned, above, 0, 0, 0);
    const auto redArenas = arenas();
    // the draw material the skin's; no back sides from the two-sided one (the crate's
    // own role, Model.cpp:1252-1276): the same indexes
    CHECK(redArenas.rotatorMaterials.contains("textures/lp72/redsided"));
    CHECK_FALSE(redArenas.rotatorMaterials.contains("textures/lp1b/crate"));
    CHECK(crateArenas.rotatorIndexes > 0u);
    CHECK(redArenas.rotatorIndexes == crateArenas.rotatorIndexes);
    CHECK(crateRed.frame.lit.interactionDraws == crate.frame.lit.interactionDraws);
    CHECK(crateRed.frame.lit.prepassDraws == crate.frame.lit.prepassDraws);
    {
      const auto rgb = pixel(crateRed, 80, 60);
      CHECK(rgb[0] > 4.0f * rgb[1]);
    }
    CHECK(pixel(crateRed, 80, 60) != pixel(crate, 80, 60));

    // 5. hide 1: the slab behind shows, as without the model
    const auto hidden = scenario(
      "hide",
      lp72MapText(
        lp72Model("lp72_a", "models/test/n01.obj", "60 0 2", "\"hide\" \"1\"\n")),
      down,
      0,
      0,
      0);
    CHECK(pixel(hidden, 80, 60) == background);

    // 6. a Pending model: the parse thread held, its frames draw nothing at its place and
    // are not settled; then Ready settles with the model
    {
      gate->close();
      const auto slot = service->table()->find("textures/lp7/model");
      REQUIRE(slot);
      REQUIRE(slot->entry);
      auto parms = ShaderParms{};
      parms[11] = 72.0f;
      REQUIRE(gate->park([&] {
        static_cast<void>(service->evaluate(MaterialId::of(*slot->entry), parms, 0.0f));
      }));
      auto expect = Lp72Expect{"pending", frameNow(), 0, 1, 1, 0};
      offer(lp72MapText(lp72Model("lp72_a", "models/test/n01p.obj", "60 0 2")));
      // frames come with camera moves (a wait for a model is no re-trigger): off and back
      const auto aside = litCamera({60, 1, 100}, {0, 0, -1}, {1, 0, 0});
      auto unsettled = 0;
      auto drawnNothing = 0;
      for (auto i = 0; i < 40 && unsettled < 3; ++i)
      {
        session->setCamera(i % 2 == 0 ? aside : down);
        const auto frame = nextFramePixel(*session, down, 80, 60);
        REQUIRE(frame);
        if (!frame->ofCamera)
        {
          continue;
        }
        CHECK_FALSE(frame->settled);
        unsettled += frame->settled ? 0 : 1;
        drawnNothing += frame->rgba[0] == background[0] && frame->rgba[1] == background[1]
                            && frame->rgba[2] == background[2]
                          ? 1
                          : 0;
      }
      CHECK(unsettled >= 3);
      CHECK(drawnNothing == unsettled);
      gate->open();
      const auto ready = shoot(down);
      expect.last = frameNow();
      expects.push_back(expect);
      CHECK(pixel(ready, 80, 60) == pixel(one, 80, 60));
    }

    // 7. a Missing file: the default cube (its silhouette: _default is black at developer
    // 0), converged; _default's blended stage draws in pass 8, which this case's frames
    // take
    session->skipShaderPassesForTesting(false);
    const auto missing = scenario(
      "missing",
      lp72MapText(lp72Model("lp72_m", "models/test/none.obj", "60 0 2")),
      down,
      1,
      1,
      0);
    session->skipShaderPassesForTesting(true);
    CHECK(missing.frame.lit.modelDraws > 0u);
    CHECK(pixel(missing, 80, 60) != background);
    // the entity's once-notice, through the session's warnings (the panel's log)
    CHECK(
      events.countContaining(
        SessionEvent::Warning,
        "'lp72_m' (func_static): 'models/test/none.obj' draws the "
        "default cube: no file models/test/none.obj")
      == 1u);

    // 8. a light with a model: drawn under the light's id
    const auto lamp = scenario(
      "lamp",
      lp72MapText(lp72Model(
        "lp72_lamp",
        "models/test/n01.obj",
        "60 -40 2",
        "\"light_radius\" \"40 40 40\"\n\"texture\" \"lights/lp51/held\"\n",
        "light")),
      down,
      1,
      1,
      0);
    CHECK(lamp.frame.lit.modelDraws > 0u);

    // 9. an .md5mesh on a func_static: nothing, models_skinned 1
    const auto md5 = scenario(
      "md5mesh",
      lp72MapText(lp72Model("lp72_md5", "models/test/n11.md5mesh", "60 0 2")),
      down,
      1,
      0,
      1);
    CHECK(pixel(md5, 80, 60) == background);
    CHECK(md5.frame.lit.modelDraws == 0u);

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
  messages.print("LP7.2 model entities, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);

  // the frame log's model columns at each scenario's last settled frame
  const auto rows = readCsv(frameLog);
  REQUIRE_FALSE(rows.empty());
  const auto column = [&](const std::string& name) {
    const auto it = std::ranges::find(rows[0], name);
    REQUIRE(it != rows[0].end());
    return std::size_t(it - rows[0].begin());
  };
  const auto eventColumn = column("event");
  const auto frameColumn = column("frame");
  const auto settledColumn = column("settled");
  const auto entitiesColumn = column("model_entities");
  const auto instancesColumn = column("model_instances");
  const auto drawsColumn = column("model_draws");
  const auto pendingColumn = column("models_pending");
  const auto skinnedColumn = column("models_skinned");
  for (const auto& expect : expects)
  {
    CAPTURE(expect.name);
    auto found = std::optional<std::vector<std::string>>{};
    auto pendingSeen = false;
    for (auto i = std::size_t{1}; i < rows.size(); ++i)
    {
      const auto& row = rows[i];
      if (row.size() <= skinnedColumn || row[eventColumn] != "frame")
      {
        continue;
      }
      const auto frame = std::stoull(row[frameColumn]);
      if (frame < expect.first || frame > expect.last)
      {
        continue;
      }
      pendingSeen = pendingSeen || row[pendingColumn] != "0";
      if (row[settledColumn] == "1")
      {
        found = row;
      }
    }
    REQUIRE(found);
    CHECK(std::stoull((*found)[entitiesColumn]) == expect.entities);
    CHECK(std::stoull((*found)[instancesColumn]) == expect.instances);
    CHECK(std::stoull((*found)[pendingColumn]) == 0u);
    CHECK(std::stoull((*found)[skinnedColumn]) == expect.skinned);
    if (expect.instances == 0)
    {
      CHECK(std::stoull((*found)[drawsColumn]) == 0u);
    }
    if (expect.name == "pending")
    {
      CHECK(pendingSeen);
    }
    std::cout << fmt::format(
      "LP7.2 frame log {}: model_entities {}, model_instances {}, model_draws {}, "
      "models_pending {}, models_skinned {}\n",
      expect.name,
      (*found)[entitiesColumn],
      (*found)[instancesColumn],
      (*found)[drawsColumn],
      (*found)[pendingColumn],
      (*found)[skinnedColumn]);
  }
  std::filesystem::remove(frameLog);
  std::cout << fmt::format(
    "LP7.2 model entities on a GPU: {:.1f} s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

namespace
{

// LP3.1 (D160, D161): the LP2.2a root with the synthetic shadow scenes (fixture/lit/
// maps/lp3, materials/lp3_shadows.mtr) and the fixture's light images (lights/lp2's spot)
std::filesystem::path assembleLp3Root(const std::filesystem::path& gamePath)
{
  const auto root = assembleLitRoot(gamePath);
  if (root.empty())
  {
    return {};
  }
  const auto base = root / "base";
  const auto fixture = litFixtureFolder();
  std::filesystem::copy(
    fixture / "maps" / "lp3",
    base / "maps" / "lp3",
    std::filesystem::copy_options::recursive);
  std::filesystem::copy_file(
    fixture / "materials" / "lp3_shadows.mtr", base / "materials" / "lp3_shadows.mtr");
  std::filesystem::copy(
    fixture / "lights", base / "lights", std::filesystem::copy_options::recursive);
  return root;
}

std::string lp3MapText(const std::string& name)
{
  return readTextFile(litFixtureFolder() / "maps" / "lp3" / name);
}

std::shared_ptr<const Snapshot> lp3Snapshot(
  const std::string& text,
  const std::filesystem::path& root,
  const std::string& name,
  const std::uint64_t sequence)
{
  auto snapshot = snapshotOfMapText(text);
  snapshot.generation = 7;
  snapshot.sequence = sequence;
  // no .proc beside it: no tree (D94), every chunk a candidate
  snapshot.mapPath = (root / "base" / "maps" / "lp3" / name).string();
  return std::make_shared<const Snapshot>(std::move(snapshot));
}

// a map's text with an entity's key added after its name line
std::string withKey(
  std::string text,
  const std::string& name,
  const std::string& key,
  const std::string& value)
{
  const auto line = fmt::format("\"name\" \"{}\"\n", name);
  const auto at = text.find(line);
  REQUIRE(at != std::string::npos);
  text.insert(at + line.size(), fmt::format("\"{}\" \"{}\"\n", key, value));
  return text;
}

// what a settled frame of the shadow cases leaves: its HDR picture, its stats and lists,
// the casting lights' records, the draw log and a region of the atlas read back
struct ShadowFrame
{
  HdrImage hdr;
  LitFrameStats stats;
  std::vector<ShadowLightRecord> records;
  // the lists' lights in viewLights order: their "name" keys
  std::vector<std::string> lights;
  std::vector<LitDrawRecord> drawLog;
  DepthReadback atlas;
  bool atlasMade = false;
  std::uint64_t atlasBytes = 0;
  std::uint64_t smWrites = 0;
  std::uint64_t plainWrites = 0;
  // LP3.4: the frame's convergence (D77) settled
  bool converged = false;
};

// the atlas rect of a record's side
AtlasRect tileOf(const ShadowLightRecord& record, const int side)
{
  const auto slice = std::size_t(std::max(0, side));
  return AtlasRect{
    record.atlasOffsets[slice][0], record.atlasOffsets[slice][1], record.imageSize[1]};
}

// the bounds of every placed tile of the records, and of extra rects
std::optional<AtlasRect> regionOf(
  const std::vector<ShadowLightRecord>& records, const std::vector<AtlasRect>& extra)
{
  auto x0 = 8192;
  auto y0 = 8192;
  auto x1 = 0;
  auto y1 = 0;
  auto add = [&](const AtlasRect& rect) {
    x0 = std::min(x0, rect.x);
    y0 = std::min(y0, rect.y);
    x1 = std::max(x1, rect.x + rect.size);
    y1 = std::max(y1, rect.y + rect.size);
  };
  for (const auto& record : records)
  {
    for (std::size_t s = 0; s < std::size_t(record.sides); ++s)
    {
      if (record.placements[s].placed)
      {
        add(AtlasRect{
          record.placements[s].x, record.placements[s].y, record.placements[s].size});
      }
    }
  }
  for (const auto& rect : extra)
  {
    add(rect);
  }
  if (x1 <= x0 || y1 <= y0)
  {
    return std::nullopt;
  }
  // a square-free region (the readback's width and height)
  return AtlasRect{x0, y0, std::max(x1 - x0, y1 - y0)};
}

/**
 * The first frame of camera that is settled by the product's rule (D77) with at least
 * minLights lights in its lists: its HDR target, its records and stats, and (readAtlas)
 * the atlas over its tiles' bounds and extra, copied after its tonemap passes and read in
 * the next frame; nullopt at the deadline. LP3.3: unsettled, the first frame of camera
 * drawn after the call whatever its convergence (a moving frame's).
 */
std::optional<ShadowFrame> settledShadowFrame(
  LitSession& session,
  const CameraState& camera,
  const std::size_t minLights,
  const bool readAtlas,
  const std::vector<AtlasRect>& extra = {},
  const std::optional<AtlasRect> regionOverride = std::nullopt,
  const std::chrono::milliseconds timeout = 60000ms,
  const bool requireSettled = true)
{
  struct State
  {
    ShadowFrame frame;
    bool settled = false;
    nvrhi::StagingTextureHandle hdr;
    nvrhi::StagingTextureHandle atlas;
    std::optional<AtlasRect> region;
  };
  session.setCamera(camera);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    session.tableChanged();
    auto state = std::make_shared<State>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterTonemapForTesting(
      [state, done, camera, minLights, readAtlas, extra, regionOverride, requireSettled](
        RenderFrameAccess& access) {
        const auto& stats = access.passes.lastStats();
        const auto& lists = access.passes.lists();
        state->settled =
          stats.camera == camera && !lists.drawSurfs.empty()
          && lists.lights.size() >= minLights
          && (!requireSettled
              || (!stats.unconverged && access.images.pending() == 0
                  && !access.images.hasLoading() && !access.arenas.pending()
                  && access.convergence.settled()));
        if (state->settled)
        {
          auto& frame = state->frame;
          frame.stats = stats;
          frame.converged = access.convergence.settled();
          frame.records = access.passes.shadowLights();
          for (const auto& light : lists.lights)
          {
            frame.lights.emplace_back(spawnString(light.record->spawnArgs, "name"));
          }
          frame.drawLog = access.passes.drawLogForTesting();
          frame.atlasBytes = access.passes.shadowAtlasBytes();
          auto* hdr = access.frame.find(frame_resource::CurrentRenderHdr)->texture;
          const auto& desc = hdr->getDesc();
          state->hdr = access.device.createStagingTexture(
            nvrhi::TextureDesc{}
              .setWidth(desc.width)
              .setHeight(desc.height)
              .setFormat(desc.format),
            nvrhi::CpuAccessMode::Read);
          access.commandList.copyTexture(
            state->hdr, nvrhi::TextureSlice{}, hdr, nvrhi::TextureSlice{});
          auto* atlas = access.passes.shadowAtlas();
          frame.atlasMade = atlas != nullptr;
          state->region =
            regionOverride ? regionOverride : regionOf(frame.records, extra);
          if (readAtlas && atlas && state->region)
          {
            state->atlas = recordDepthReadback(
              access.device,
              access.commandList,
              atlas,
              std::uint32_t(state->region->x),
              std::uint32_t(state->region->y),
              std::uint32_t(state->region->size),
              std::uint32_t(state->region->size));
          }
        }
        done->set_value();
      });
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (!state->settled)
    {
      std::this_thread::sleep_for(50ms);
      continue;
    }
    const auto read = inFrame(session, [state](RenderFrameAccess& access) {
      access.device.waitForIdle();
      const auto& desc = state->hdr->getDesc();
      auto& image = state->frame.hdr;
      image.width = desc.width;
      image.height = desc.height;
      image.rgba.resize(std::size_t(desc.width) * desc.height * 4);
      auto pitch = std::size_t{0};
      const auto* bytes =
        static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
          state->hdr, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
      for (std::uint32_t y = 0; y < desc.height; ++y)
      {
        const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
        for (std::uint32_t x = 0; x < desc.width * 4; ++x)
        {
          image.rgba[std::size_t(y) * desc.width * 4 + x] = halfToFloat(row[x]);
        }
      }
      access.device.unmapStagingTexture(state->hdr);
      state->hdr = nullptr;
      if (state->atlas)
      {
        state->frame.atlas = mapDepthReadback(
          access.device,
          state->atlas,
          std::uint32_t(state->region->x),
          std::uint32_t(state->region->y));
        state->atlas = nullptr;
      }
    });
    if (!read)
    {
      return std::nullopt;
    }
    return std::move(state->frame);
  }
  return std::nullopt;
}

// the folder the cases' pictures go to (TB_LIT_PNG_OUT), or none
std::optional<std::filesystem::path> pngFolder()
{
  const auto* value = std::getenv("TB_LIT_PNG_OUT");
  if (!value || !*value)
  {
    return std::nullopt;
  }
  return std::filesystem::path{value};
}

// the atlas region as a grey PNG: 1.0 (the far clear) black, nearer brighter
void saveAtlasPng(const DepthReadback& atlas, const std::string& name)
{
  const auto folder = pngFolder();
  if (!folder || atlas.depth.empty())
  {
    return;
  }
  auto grey = std::vector<std::uint8_t>(atlas.depth.size());
  for (std::size_t i = 0; i < grey.size(); ++i)
  {
    const auto d = std::clamp(atlas.depth[i], 0.0f, 1.0f);
    grey[i] = std::uint8_t(std::lround(255.0 * std::sqrt(double(1.0f - d))));
  }
  const auto path = *folder / (name + "_atlas.png");
  CHECK(writePng(path, atlas.width, atlas.height, 1, grey));
  std::cout << fmt::format(
    "LP3.1 {}: atlas region ({}, {}) {} x {} -> {}\n",
    name,
    atlas.x,
    atlas.y,
    atlas.width,
    atlas.height,
    path.string());
}

// the HDR picture as an RGB PNG, scaled by its brightest component
void saveHdrPng(const HdrImage& hdr, const std::string& name)
{
  const auto folder = pngFolder();
  if (!folder || hdr.rgba.empty())
  {
    return;
  }
  auto most = 1e-6f;
  for (std::size_t i = 0; i < hdr.rgba.size(); ++i)
  {
    if (i % 4 != 3)
    {
      most = std::max(most, hdr.rgba[i]);
    }
  }
  auto rgb = std::vector<std::uint8_t>{};
  rgb.reserve(std::size_t(hdr.width) * hdr.height * 3);
  for (std::size_t i = 0; i < hdr.rgba.size(); ++i)
  {
    if (i % 4 != 3)
    {
      rgb.push_back(
        std::uint8_t(std::lround(255.0f * std::clamp(hdr.rgba[i] / most, 0.0f, 1.0f))));
    }
  }
  const auto path = *folder / (name + "_hdr.png");
  CHECK(writePng(path, hdr.width, hdr.height, 3, rgb));
  std::cout << fmt::format("LP3.1 {}: picture -> {}\n", name, path.string());
}

// the atlas texel a world point projects to through a record's stored side matrices, as
// interactionSM samples it (SC729): clipToWindow · shadowP · shadowV (point, parallel),
// shadowP · shadowV (spot), divided by w, then offset + st · size; none behind the side
std::optional<std::array<float, 2>> texelOf(
  const ShadowLightRecord& record, const int side, const EngineVec3& point)
{
  const auto slice = std::size_t(std::max(0, side));
  auto matrix = engineMatrixMultiply(record.shadowP[slice], record.shadowV[slice]);
  if (record.type != ShadowLightType::Spot)
  {
    matrix = engineMatrixMultiply(engineClipSpaceToWindowSpace(), matrix);
  }
  const auto p = std::array<float, 4>{point[0], point[1], point[2], 1.0f};
  auto out = std::array<float, 4>{};
  for (std::size_t r = 0; r < 4; ++r)
  {
    out[r] = matrix[r * 4 + 0] * p[0] + matrix[r * 4 + 1] * p[1]
             + matrix[r * 4 + 2] * p[2] + matrix[r * 4 + 3] * p[3];
  }
  if (!(out[3] > 1e-4f))
  {
    return std::nullopt;
  }
  const auto tile = tileOf(record, side);
  return std::array<float, 2>{
    float(tile.x) + out[0] / out[3] * float(tile.size),
    float(tile.y) + out[1] / out[3] * float(tile.size)};
}

// the texel bounds of a box's eight corners in a side (none when a corner is behind it)
std::optional<std::array<float, 4>> footprintOf(
  const ShadowLightRecord& record, const int side, const EngineBounds& box)
{
  auto bounds = std::array<float, 4>{1e9f, 1e9f, -1e9f, -1e9f};
  for (auto c = 0; c < 8; ++c)
  {
    const auto corner = EngineVec3{
      box[(c & 1) ? 1 : 0][0], box[(c & 2) ? 1 : 0][1], box[(c & 4) ? 1 : 0][2]};
    const auto texel = texelOf(record, side, corner);
    if (!texel)
    {
      return std::nullopt;
    }
    bounds[0] = std::min(bounds[0], (*texel)[0]);
    bounds[1] = std::min(bounds[1], (*texel)[1]);
    bounds[2] = std::max(bounds[2], (*texel)[0]);
    bounds[3] = std::max(bounds[3], (*texel)[1]);
  }
  return bounds;
}

/**
 * The atlas check of a light's tiles (D160): every texel below 1.0 lies inside the
 * footprint of a caster box (its corners' texels, 2 texels of margin; a footprint with a
 * corner behind the side is the whole tile), the box's own footprint centre is below 1.0
 * in the side holding it, and every other texel is the far clear, exactly 1.0. Returns
 * the tile's texels below 1.0 per side.
 */
std::vector<std::size_t> checkTiles(
  const ShadowFrame& frame,
  const ShadowLightRecord& record,
  const std::vector<EngineBounds>& casters,
  const std::optional<EngineBounds>& box)
{
  auto nearer = std::vector<std::size_t>(std::size_t(record.sides), 0);
  const auto first = record.type == ShadowLightType::Spot ? -1 : 0;
  for (auto side = first; side < first + record.sides; ++side)
  {
    CAPTURE(side);
    const auto tile = tileOf(record, side);
    auto footprints = std::vector<std::array<float, 4>>{};
    for (const auto& caster : casters)
    {
      const auto footprint = footprintOf(record, side, caster);
      footprints.push_back(
        footprint ? *footprint
                  : std::array<float, 4>{
                      float(tile.x),
                      float(tile.y),
                      float(tile.x + tile.size),
                      float(tile.y + tile.size)});
    }
    auto outside = std::size_t{0};
    for (auto ty = tile.y; ty < tile.y + tile.size; ++ty)
    {
      for (auto tx = tile.x; tx < tile.x + tile.size; ++tx)
      {
        const auto d = frame.atlas.at(std::uint32_t(tx), std::uint32_t(ty));
        if (d >= 1.0f)
        {
          continue;
        }
        ++nearer[std::size_t(side - first)];
        const auto inside = std::ranges::any_of(footprints, [&](const auto& f) {
          return float(tx) + 0.5f >= f[0] - 2.0f && float(tx) + 0.5f <= f[2] + 2.0f
                 && float(ty) + 0.5f >= f[1] - 2.0f && float(ty) + 0.5f <= f[3] + 2.0f;
        });
        if (!inside)
        {
          ++outside;
        }
      }
    }
    CHECK(outside == 0u);
    if (box)
    {
      const auto footprint = footprintOf(record, side, *box);
      if (footprint)
      {
        const auto cx = ((*footprint)[0] + (*footprint)[2]) * 0.5f;
        const auto cy = ((*footprint)[1] + (*footprint)[3]) * 0.5f;
        if (
          cx >= float(tile.x) && cx < float(tile.x + tile.size) && cy >= float(tile.y)
          && cy < float(tile.y + tile.size))
        {
          CHECK(frame.atlas.at(std::uint32_t(cx), std::uint32_t(cy)) < 1.0f);
        }
      }
    }
  }
  return nearer;
}

// the receiver check (SC733): per pixel, darker, brighter or equal between two pictures
struct PictureDiff
{
  std::size_t darker = 0;
  std::size_t brighter = 0;
  std::size_t equal = 0;
};

PictureDiff comparePictures(const HdrImage& shadowed, const HdrImage& unshadowed)
{
  auto diff = PictureDiff{};
  for (std::uint32_t y = 0; y < shadowed.height; ++y)
  {
    for (std::uint32_t x = 0; x < shadowed.width; ++x)
    {
      const auto a = shadowed.at(x, y);
      const auto b = unshadowed.at(x, y);
      if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
      {
        ++diff.equal;
      }
      else if (a[0] <= b[0] && a[1] <= b[1] && a[2] <= b[2])
      {
        ++diff.darker;
      }
      else
      {
        ++diff.brighter;
      }
    }
  }
  return diff;
}

// the interaction programs a frame's draw log holds
std::set<int> interactionPrograms(const std::vector<LitDrawRecord>& log)
{
  auto programs = std::set<int>{};
  for (const auto& record : log)
  {
    if (record.pass == LitDrawRecord::Pass::Interaction)
    {
      programs.insert(record.program);
    }
  }
  return programs;
}

// the sides a chunk of an entity's was drawn into (the draw log's Shadow records)
std::set<int> shadowSidesOf(const std::vector<LitDrawRecord>& log, const PreviewId entity)
{
  auto sides = std::set<int>{};
  for (const auto& record : log)
  {
    if (record.pass == LitDrawRecord::Pass::Shadow && record.chunk.entity == entity)
    {
      sides.insert(record.side);
    }
  }
  return sides;
}

// the expected placements of a light's sides when it is packed alone (D145)
std::vector<EngineAtlasPlacement> packedAlone(const int sides, const int size)
{
  auto map = EngineTileMap{};
  REQUIRE(map.init(8192, 1024, 8));
  return engineShadowAtlasLayout(
    map,
    std::vector<EngineAtlasSize>(std::size_t(sides), EngineAtlasSize{size, size}),
    8192);
}

constexpr auto Lp3Box = EngineBounds{EngineVec3{32, -16, 80}, EngineVec3{64, 16, 112}};

} // namespace

// LP3.1 (C57-C61, D145-D149, D154, D160, D161; SC714-SC736): the shadow atlas and the
// shadow-atlas interactions on synthetic scenes (fixture/lit/maps/lp3): (a) a point light
// and a box, (b) a spot light, (c) a parallel light, (d) a translucent-only caster, (e)
// 43 point lights overflowing the atlas, (f) the vacated rects' clear, (g) D76's two
// layouts. Hidden: the blobs live only in the game folder; it needs a GPU.
TEST_CASE("LitSession shadows on a GPU (LP3.1)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto root = assembleLp3Root(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  const auto instance = testInstance(true, false, messages);
  REQUIRE(instance.valid());
  REQUIRE(initInstanceDispatcher(instance.handles()));
  auto service = rootService(root);
  auto events = Events{};
  auto session = rootSession(instance, events, *service, root, true, false, true);
  session->setExposed(true);
  REQUIRE(waitFor(
    [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
  REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
    access.passes.setDrawLogForTesting(true);
  }));
  auto sequence = std::uint64_t{0};
  const auto offer = [&](const std::string& text, const std::string& name) {
    REQUIRE(session->offerSnapshot(lp3Snapshot(text, root, name, ++sequence)));
    // the draw log of the new scene's frames alone
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
  };

  // ---------------------------------------------------------------------------------
  SECTION("(a) a point light and a box: six 512 tiles, the box's footprint, its shadow")
  {
    const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    const auto text = lp3MapText("lp3_point.map");
    offer(text, "lp3_point.map");
    const auto shadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    CHECK(frame.stats.lightsCasting == 1u);
    CHECK(frame.stats.lightsPlaced == 1u);
    CHECK(frame.stats.lightsUnplaced == 0u);
    // LP3.3 (D146): the settled frame drew its tiles or kept them from the frame that did
    CHECK(frame.stats.tilesDrawn + frame.stats.tilesCached == 6u);
    CHECK(frame.stats.layoutTiles == 6u);
    CHECK(frame.stats.layoutShadowDraws > 0u);
    CHECK(frame.stats.perforatedCastersSkipped == 0u);
    CHECK(frame.atlasMade);
    CHECK(frame.atlasBytes == 8192ull * 8192ull * 4ull);
    REQUIRE(frame.records.size() == 1u);
    const auto& record = frame.records[0];
    CHECK(record.type == ShadowLightType::Point);
    // the camera inside the volume: LOD 0, a point light's 1 (FAL:463-466), 512
    CHECK(record.lod == 1);
    CHECK(record.imageSize == std::array<int, 2>{512, 512});
    // the copied packing's own positions (D145)
    const auto expected = packedAlone(6, 512);
    for (std::size_t side = 0; side < 6; ++side)
    {
      CAPTURE(side);
      CHECK(record.placements[side].placed);
      CHECK(record.placements[side].x == expected[side].x);
      CHECK(record.placements[side].y == expected[side].y);
      CHECK(
        record.atlasOffsets[side]
        == std::array<int, 2>{expected[side].x, expected[side].y});
    }
    // the shadow-atlas point programs alone (legacy or PBR by the specular image)
    for (const auto program : interactionPrograms(frame.drawLog))
    {
      CHECK(
        (program == EngineBuiltinInteractionShadowAtlasPoint
         || program == EngineBuiltinPbrInteractionShadowAtlasPoint));
    }
    CHECK_FALSE(interactionPrograms(frame.drawLog).empty());
    // D149: the box is drawn into the sides whose true frustum meets it
    auto boxSides = std::set<int>{};
    for (const auto& draw : frame.drawLog)
    {
      if (draw.pass == LitDrawRecord::Pass::Shadow)
      {
        boxSides.insert(draw.side);
      }
    }
    std::cout << fmt::format(
      "LP3.1 (a): tiles {}, shadow draws {}, culled {}, sides drawn {}\n",
      frame.stats.layoutTiles,
      frame.stats.layoutShadowDraws,
      frame.stats.shadowCastersCulled,
      fmt::join(boxSides, " "));
    // the atlas: the box's and the anchor's footprints below 1.0, the far clear elsewhere
    REQUIRE_FALSE(frame.atlas.depth.empty());
    const auto anchor = EngineBounds{EngineVec3{92, -208, 24}, EngineVec3{108, -192, 40}};
    const auto nearer = checkTiles(frame, record, {Lp3Box, anchor}, Lp3Box);
    std::cout << fmt::format(
      "LP3.1 (a): texels below 1.0 per side {}\n", fmt::join(nearer, " "));
    saveAtlasPng(frame.atlas, "lp31_a_point");
    saveHdrPng(frame.hdr, "lp31_a_point");

    // the same scene with the box noshadows 1: the anchor keeps the light casting, so
    // the shadow-atlas program draws both pictures (SC733)
    offer(withKey(text, "lp3_box", "noshadows", "1"), "lp3_point.map");
    const auto unshadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(unshadowed);
    CHECK(unshadowed->stats.lightsPlaced == 1u);
    const auto diff = comparePictures(frame.hdr, unshadowed->hdr);
    std::cout << fmt::format(
      "LP3.1 (a): the box's shadow: {} pixels darker, {} brighter, {} equal\n",
      diff.darker,
      diff.brighter,
      diff.equal);
    CHECK(diff.darker > 100u);
    CHECK(diff.brighter == 0u);
    saveHdrPng(unshadowed->hdr, "lp31_a_point_unshadowed");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(b) a spot light and a box: one 512 tile, the box's footprint, its shadow")
  {
    // inside the spot's frustum, beside the light, so that the box's shadow on the far
    // wall is not behind the box
    const auto camera = litCamera({0, -100, 140}, {0.9f, 0.4359f, 0}, {0, 0, 1});
    const auto text = lp3MapText("lp3_spot.map");
    offer(text, "lp3_spot.map");
    const auto shadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    CHECK(frame.stats.lightsCasting == 1u);
    CHECK(frame.stats.lightsPlaced == 1u);
    CHECK(frame.stats.tilesDrawn + frame.stats.tilesCached == 1u);
    CHECK(frame.stats.layoutTiles == 1u);
    CHECK(frame.stats.layoutShadowDraws > 0u);
    REQUIRE(frame.records.size() == 1u);
    const auto& record = frame.records[0];
    CHECK(record.type == ShadowLightType::Spot);
    CHECK(record.sides == 1);
    CHECK(record.lod == 1);
    CHECK(record.imageSize == std::array<int, 2>{512, 512});
    const auto expected = packedAlone(1, 512);
    CHECK(record.placements[0].x == expected[0].x);
    CHECK(record.placements[0].y == expected[0].y);
    for (const auto program : interactionPrograms(frame.drawLog))
    {
      CHECK(
        (program == EngineBuiltinInteractionShadowAtlasSpot
         || program == EngineBuiltinPbrInteractionShadowAtlasSpot));
    }
    CHECK_FALSE(interactionPrograms(frame.drawLog).empty());
    REQUIRE_FALSE(frame.atlas.depth.empty());
    const auto anchor = EngineBounds{EngineVec3{232, 192, 8}, EngineVec3{248, 208, 24}};
    const auto nearer = checkTiles(frame, record, {Lp3Box, anchor}, Lp3Box);
    std::cout << fmt::format(
      "LP3.1 (b): tile ({}, {}), shadow draws {}, texels below 1.0 {}\n",
      record.placements[0].x,
      record.placements[0].y,
      frame.stats.layoutShadowDraws,
      nearer[0]);
    CHECK(nearer[0] > 0u);
    saveAtlasPng(frame.atlas, "lp31_b_spot");
    saveHdrPng(frame.hdr, "lp31_b_spot");

    offer(withKey(text, "lp3_box", "noshadows", "1"), "lp3_spot.map");
    const auto unshadowed = settledShadowFrame(*session, camera, 1, false);
    REQUIRE(unshadowed);
    CHECK(unshadowed->stats.lightsPlaced == 1u);
    const auto diff = comparePictures(frame.hdr, unshadowed->hdr);
    std::cout << fmt::format(
      "LP3.1 (b): the box's shadow: {} pixels darker, {} brighter, {} equal\n",
      diff.darker,
      diff.brighter,
      diff.equal);
    CHECK(diff.darker > 100u);
    CHECK(diff.brighter == 0u);
    saveHdrPng(unshadowed->hdr, "lp31_b_spot_unshadowed");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(c) a parallel light and a box: four 1024 cascades, the box's shadow")
  {
    const auto camera = litCamera({-112, 0, 96}, {1, 0, 0}, {0, 0, 1});
    const auto text = lp3MapText("lp3_parallel.map");
    offer(text, "lp3_parallel.map");
    const auto shadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    CHECK(frame.stats.lightsCasting == 1u);
    CHECK(frame.stats.lightsPlaced == 1u);
    CHECK(frame.stats.tilesDrawn + frame.stats.tilesCached == 4u);
    CHECK(frame.stats.layoutTiles == 4u);
    CHECK(frame.stats.layoutShadowDraws > 0u);
    REQUIRE(frame.records.size() == 1u);
    const auto& record = frame.records[0];
    CHECK(record.type == ShadowLightType::Parallel);
    CHECK(record.sides == 4);
    CHECK(record.lod == 0);
    CHECK(record.imageSize == std::array<int, 2>{1024, 1024});
    // the copied packing: cascade 0 the fourth tile, at (1024, 7168) (D160)
    const auto expected = packedAlone(4, 1024);
    for (std::size_t side = 0; side < 4; ++side)
    {
      CAPTURE(side);
      CHECK(record.placements[side].x == expected[side].x);
      CHECK(record.placements[side].y == expected[side].y);
    }
    CHECK(record.placements[0].x == 1024);
    CHECK(record.placements[0].y == 7168);
    for (const auto program : interactionPrograms(frame.drawLog))
    {
      CHECK(
        (program == EngineBuiltinInteractionShadowAtlasParallel
         || program == EngineBuiltinPbrInteractionShadowAtlasParallel));
    }
    CHECK_FALSE(interactionPrograms(frame.drawLog).empty());
    REQUIRE_FALSE(frame.atlas.depth.empty());
    const auto box = EngineBounds{EngineVec3{16, -16, 64}, EngineVec3{48, 16, 96}};
    const auto anchor = EngineBounds{EngineVec3{-88, 72, 24}, EngineVec3{-72, 88, 40}};
    const auto nearer = checkTiles(frame, record, {box, anchor}, box);
    std::cout << fmt::format(
      "LP3.1 (c): shadow draws {}, culled {}, texels below 1.0 per cascade {}\n",
      frame.stats.layoutShadowDraws,
      frame.stats.shadowCastersCulled,
      fmt::join(nearer, " "));
    CHECK(nearer[0] > 0u);
    saveAtlasPng(frame.atlas, "lp31_c_parallel");
    saveHdrPng(frame.hdr, "lp31_c_parallel");

    offer(withKey(text, "lp3_box", "noshadows", "1"), "lp3_parallel.map");
    const auto unshadowed = settledShadowFrame(*session, camera, 1, false);
    REQUIRE(unshadowed);
    CHECK(unshadowed->stats.lightsPlaced == 1u);
    const auto diff = comparePictures(frame.hdr, unshadowed->hdr);
    std::cout << fmt::format(
      "LP3.1 (c): the box's shadow: {} pixels darker, {} brighter, {} equal\n",
      diff.darker,
      diff.brighter,
      diff.equal);
    CHECK(diff.darker > 100u);
    CHECK(diff.brighter == 0u);
    saveHdrPng(unshadowed->hdr, "lp31_c_parallel_unshadowed");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(d) a translucent-only caster: tiles and the program, nothing drawn")
  {
    const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_translucent.map"), "lp3_translucent.map");
    const auto shadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    // P3.17: the panel counts as a caster, so the light takes its tiles and the program,
    // but it draws nothing into them (RB:3506-3509)
    CHECK(frame.stats.lightsCasting == 1u);
    CHECK(frame.stats.lightsPlaced == 1u);
    CHECK(frame.stats.tilesDrawn + frame.stats.tilesCached == 6u);
    CHECK(frame.stats.layoutTiles == 6u);
    CHECK(frame.stats.shadowDraws == 0u);
    CHECK(frame.stats.layoutShadowDraws == 0u);
    REQUIRE(frame.records.size() == 1u);
    for (const auto program : interactionPrograms(frame.drawLog))
    {
      CHECK(
        (program == EngineBuiltinInteractionShadowAtlasPoint
         || program == EngineBuiltinPbrInteractionShadowAtlasPoint));
    }
    CHECK_FALSE(interactionPrograms(frame.drawLog).empty());
    REQUIRE_FALSE(frame.atlas.depth.empty());
    // every texel of its tiles the far clear, 1.0: the wall unshadowed
    const auto nearer = checkTiles(frame, frame.records[0], {}, std::nullopt);
    CHECK(std::ranges::all_of(nearer, [](const auto n) { return n == 0u; }));
    std::cout << fmt::format(
      "LP3.1 (d): tiles {}, shadow draws {}, texels below 1.0 {}\n",
      frame.stats.layoutTiles,
      frame.stats.layoutShadowDraws,
      fmt::join(nearer, " "));
    saveAtlasPng(frame.atlas, "lp31_d_translucent");
    saveHdrPng(frame.hdr, "lp31_d_translucent");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(e) 43 point lights: 41 placed, the first and the 22nd unplaced")
  {
    const auto camera = litCamera({0, 0, 128}, {1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_overflow.map"), "lp3_overflow.map");
    const auto shadowed =
      settledShadowFrame(*session, camera, 43, true, {}, AtlasRect{0, 6144, 2048});
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    CHECK(frame.stats.lightsCasting == 43u);
    CHECK(frame.stats.lightsPlaced == 41u);
    CHECK(frame.stats.lightsUnplaced == 2u);
    CHECK(frame.stats.lightsDrawn == 43u);
    CHECK(frame.stats.lightsOverflowed == 0u);
    REQUIRE(frame.records.size() == 43u);
    REQUIRE(frame.lights.size() == 43u);
    // D160, D162: the copied packing of 258 equal sides leaves shadowIndex 0 and 128
    // without a tile: the first light in viewLights order and the 22nd
    CHECK_FALSE(frame.records[0].placed());
    CHECK_FALSE(frame.records[21].placed());
    REQUIRE(frame.stats.unplacedLights.size() == 2u);
    CHECK(frame.stats.unplacedLights[0] == frame.lights[0]);
    CHECK(frame.stats.unplacedLights[1] == frame.lights[21]);
    std::cout << fmt::format(
      "LP3.1 (e): unplaced {} and {} (viewLights 0 and 21 of 43)\n",
      frame.stats.unplacedLights[0],
      frame.stats.unplacedLights[1]);
    // every side where engineShadowAtlasLayout puts it
    {
      auto map = EngineTileMap{};
      REQUIRE(map.init(8192, 1024, 8));
      const auto expected = engineShadowAtlasLayout(
        map, std::vector<EngineAtlasSize>(258, EngineAtlasSize{512, 512}), 8192);
      auto mismatches = 0;
      for (std::size_t r = 0; r < 43; ++r)
      {
        for (std::size_t s = 0; s < 6; ++s)
        {
          const auto& got = frame.records[r].placements[s];
          const auto& want = expected[r * 6 + s];
          mismatches += got.placed != want.placed || got.x != want.x || got.y != want.y;
        }
      }
      CHECK(mismatches == 0);
    }
    // the unplaced lights draw the plain interaction, the others the atlas's (K26, C60);
    // LP3.3: in a settled frame of the scene (the lights may have entered the held layout
    // over several moving frames before the settle placed them as the engine does)
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
    const auto again = settledShadowFrame(*session, camera, 43, false);
    REQUIRE(again);
    auto plainOf = std::set<PreviewId>{};
    auto atlasOf = std::set<PreviewId>{};
    for (const auto& draw : again->drawLog)
    {
      if (draw.pass != LitDrawRecord::Pass::Interaction)
      {
        continue;
      }
      if (
        draw.program == EngineBuiltinInteraction
        || draw.program == EngineBuiltinPbrInteraction)
      {
        plainOf.insert(draw.light);
      }
      else
      {
        atlasOf.insert(draw.light);
      }
    }
    CHECK(
      plainOf == std::set<PreviewId>{frame.records[0].light, frame.records[21].light});
    CHECK(atlasOf.size() == 41u);
    saveAtlasPng(frame.atlas, "lp31_e_overflow");
    saveHdrPng(frame.hdr, "lp31_e_overflow");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(f) a light leaving the view leaves its rects cleared to 1.0")
  {
    offer(lp3MapText("lp3_vacated.map"), "lp3_vacated.map");
    const auto both = settledShadowFrame(
      *session, litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1}), 2, false);
    REQUIRE(both);
    REQUIRE(both->records.size() == 2u);
    CHECK(both->stats.lightsPlaced == 2u);
    auto before = std::vector<AtlasRect>{};
    for (const auto& record : both->records)
    {
      for (std::size_t s = 0; s < std::size_t(record.sides); ++s)
      {
        before.push_back(AtlasRect{
          record.placements[s].x, record.placements[s].y, record.placements[s].size});
      }
    }
    // turned round: light B's volume behind the view, A's around the camera
    const auto one = settledShadowFrame(
      *session, litCamera({-200, 0, 128}, {-1, 0, 0}, {0, 0, 1}), 1, true, before);
    REQUIRE(one);
    REQUIRE(one->records.size() == 1u);
    auto after = std::vector<AtlasRect>{};
    for (std::size_t s = 0; s < 6; ++s)
    {
      const auto& p = one->records[0].placements[s];
      after.push_back(AtlasRect{p.x, p.y, p.size});
    }
    // P3.3: every rect of the first frame's tiles the second's do not cover reads 1.0
    auto vacated = 0;
    auto notCleared = std::size_t{0};
    for (const auto& rect : before)
    {
      auto covered = std::int64_t{0};
      for (const auto& tile : after)
      {
        const auto x0 = std::max(rect.x, tile.x);
        const auto y0 = std::max(rect.y, tile.y);
        const auto x1 = std::min(rect.x + rect.size, tile.x + tile.size);
        const auto y1 = std::min(rect.y + rect.size, tile.y + tile.size);
        covered += x1 > x0 && y1 > y0 ? std::int64_t(x1 - x0) * (y1 - y0) : 0;
      }
      if (covered == std::int64_t(rect.size) * rect.size)
      {
        continue;
      }
      ++vacated;
      for (auto ty = rect.y; ty < rect.y + rect.size; ++ty)
      {
        for (auto tx = rect.x; tx < rect.x + rect.size; ++tx)
        {
          notCleared += one->atlas.at(std::uint32_t(tx), std::uint32_t(ty)) != 1.0f;
        }
      }
    }
    std::cout << fmt::format(
      "LP3.1 (f): {} rects of the first frame vacated, {} of their texels not 1.0\n",
      vacated,
      notCleared);
    CHECK(vacated > 0);
    CHECK(notCleared == 0u);
    saveAtlasPng(one->atlas, "lp31_f_vacated");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(g) D76: both interaction layouts in one frame, a light overflowed whole")
  {
    const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_layouts.map"), "lp3_layouts.map");
    const auto settled = settledShadowFrame(*session, camera, 2, true);
    REQUIRE(settled);
    const auto& frame = *settled;
    REQUIRE(frame.records.size() == 1u);
    const auto programs = interactionPrograms(frame.drawLog);
    const auto atlasProgram = std::ranges::any_of(programs, [](const int program) {
      return program == EngineBuiltinInteractionShadowAtlasPoint
             || program == EngineBuiltinPbrInteractionShadowAtlasPoint;
    });
    const auto plainProgram = std::ranges::any_of(programs, [](const int program) {
      return program == EngineBuiltinInteraction
             || program == EngineBuiltinPbrInteraction;
    });
    CHECK(atlasProgram);
    CHECK(plainProgram);
    CHECK(frame.stats.interactionWrites > 0u);
    CHECK(frame.stats.interactionSmWrites > 0u);
    CHECK(frame.stats.interactionWrites <= frame.stats.constantWriteBudget);
    CHECK(frame.stats.interactionSmWrites <= frame.stats.constantWriteBudget);
    CHECK(frame.stats.lightsOverflowed == 0u);
    std::cout << fmt::format(
      "LP3.1 (g): set 10 writes {}, set 11 writes {}, budget {}\n",
      frame.stats.interactionWrites,
      frame.stats.interactionSmWrites,
      frame.stats.constantWriteBudget);
    saveAtlasPng(frame.atlas, "lp31_g_layouts");
    saveHdrPng(frame.hdr, "lp31_g_layouts");

    // a budget one write short of the casting light's: it is not drawn, whole (no draw of
    // its program), and the frame never meets NVRHI's maxVersions error
    const auto versions = std::uint32_t(3 * (frame.stats.interactionSmWrites - 1));
    REQUIRE(inFrame(*session, [versions](RenderFrameAccess& access) {
      access.passes.setConstantBufferVersionsForTesting(versions);
      access.passes.setDrawLogForTesting(true);
    }));
    auto overflowed = LitFrameStats{};
    auto log = std::vector<LitDrawRecord>{};
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      overflowed = access.passes.lastStats();
      log = access.passes.drawLogForTesting();
    }));
    CHECK(overflowed.lightsOverflowed >= 1u);
    CHECK(overflowed.interactionSmWrites == 0u);
    for (const auto program : interactionPrograms(log))
    {
      CHECK(
        (program == EngineBuiltinInteraction || program == EngineBuiltinPbrInteraction));
    }
    std::cout << fmt::format(
      "LP3.1 (g): at maxVersions {} (budget {}): {} lights overflowed, set 11 writes "
      "{}\n",
      versions,
      overflowed.constantWriteBudget,
      overflowed.lightsOverflowed,
      overflowed.interactionSmWrites);
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setConstantBufferVersionsForTesting(
        EngineFrame.constantBufferVersions);
    }));
  }

  std::cout << fmt::format(
    "LP3.1 shadows on a GPU: {:.1f} s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  // LP3.3: case (f) turns its camera, leaving light A's tiles where the two-light packing
  // put them: the still timer's frame settles them (it fires once there, never elsewhere)
  teardown(*session, std::nullopt);
  messages.print("LP3.1 shadows, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

namespace
{

// LP3.3: a map's text with every line of an entity's block (its keys and brushes, found
// by its "name") passed through edit
std::string withEntityLines(
  const std::string& text,
  const std::string& name,
  const std::function<std::string(const std::string&)>& edit)
{
  auto lines = std::vector<std::string>{};
  {
    auto stream = std::istringstream{text};
    auto line = std::string{};
    while (std::getline(stream, line))
    {
      lines.push_back(line);
    }
  }
  const auto nameLine = fmt::format(R"("name" "{}")", name);
  const auto at = std::ranges::find(lines, nameLine);
  REQUIRE(at != lines.end());
  auto open = std::size_t(at - lines.begin());
  while (open > 0 && lines[open] != "{")
  {
    --open;
  }
  auto depth = 0;
  for (auto i = open; i < lines.size(); ++i)
  {
    if (lines[i] == "{")
    {
      ++depth;
    }
    else if (lines[i] == "}")
    {
      if (--depth == 0)
      {
        break;
      }
    }
    else
    {
      lines[i] = edit(lines[i]);
    }
  }
  auto out = std::string{};
  for (const auto& line : lines)
  {
    out += line;
    out += '\n';
  }
  return out;
}

// a brush line's three points moved by d, and an "origin" key with them
std::string movedLine(const std::string& line, const EngineVec3& d)
{
  if (line.starts_with(R"("origin" ")"))
  {
    auto stream = std::istringstream{line.substr(10)};
    auto x = 0.0f;
    auto y = 0.0f;
    auto z = 0.0f;
    stream >> x >> y >> z;
    return fmt::format(R"("origin" "{} {} {}")", x + d[0], y + d[1], z + d[2]);
  }
  if (!line.starts_with("( "))
  {
    return line;
  }
  auto out = std::string{};
  auto rest = std::string_view{line};
  for (auto p = 0; p < 3; ++p)
  {
    const auto open = rest.find("( ");
    const auto close = rest.find(" )");
    REQUIRE(open != std::string_view::npos);
    REQUIRE(close != std::string_view::npos);
    auto stream =
      std::istringstream{std::string{rest.substr(open + 2, close - open - 2)}};
    auto x = 0.0f;
    auto y = 0.0f;
    auto z = 0.0f;
    stream >> x >> y >> z;
    out += fmt::format("( {} {} {} ) ", x + d[0], y + d[1], z + d[2]);
    rest = rest.substr(close + 3);
  }
  return out + std::string{rest};
}

// the draw log's tile clears and caster draws of a light (pass 6)
std::size_t tileClearsOf(const std::vector<LitDrawRecord>& log, const PreviewId light)
{
  return std::size_t(std::ranges::count_if(log, [&](const LitDrawRecord& record) {
    return record.pass == LitDrawRecord::Pass::ShadowClear && record.light == light;
  }));
}

std::size_t shadowDrawsOf(const std::vector<LitDrawRecord>& log)
{
  return std::size_t(std::ranges::
                       count_if(log, [](const LitDrawRecord& record) {
                         return record.pass == LitDrawRecord::Pass::Shadow
           || (record.pass == LitDrawRecord::Pass::ShadowClear
               && record.light != NoPreviewId);
                       }));
}

// whether the light has a kept record, after the last frame
bool keptRecord(LitSession& session, const PreviewId light)
{
  auto kept = false;
  REQUIRE(inFrame(session, [&](RenderFrameAccess& access) {
    kept = access.passes.shadowCache().find(light) != nullptr;
  }));
  return kept;
}

} // namespace

// LP3.3 (C61, C62, D146, D150-D152, D160, P3.4, P3.7, P3.26): the shadow records, the
// cache, the relocation at settle and the held layout on the synthetic scenes
// (fixture/lit/ maps/lp3, edited in the test's text). Hidden: it needs a GPU and the
// engine's blobs.
TEST_CASE("LitSession shadow records on a GPU (LP3.3)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto root = assembleLp3Root(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  const auto instance = testInstance(true, false, messages);
  REQUIRE(instance.valid());
  REQUIRE(initInstanceDispatcher(instance.handles()));
  auto service = rootService(root);
  auto events = Events{};
  auto session = rootSession(instance, events, *service, root, true, false, true);
  session->setExposed(true);
  REQUIRE(waitFor(
    [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
  auto sequence = std::uint64_t{0};
  const auto offer = [&](const std::string& text, const std::string& name) {
    REQUIRE(session->offerSnapshot(lp3Snapshot(text, root, name, ++sequence)));
    // the draw log of the new scene's frames alone
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
  };
  const auto clearLog = [&] {
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
  };

  // ---------------------------------------------------------------------------------
  SECTION("(h) the cache: a still scene, cached-vs-fresh, a UV drag, a moved occluder")
  {
    const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    const auto text = lp3MapText("lp3_point.map");
    offer(text, "lp3_point.map");
    const auto first = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(first);
    REQUIRE(first->records.size() == 1u);
    const auto light = first->records[0].light;
    CHECK(first->stats.layoutTiles == 6u);
    CHECK(tileClearsOf(first->drawLog, light) >= 6u);
    const auto drawsWhenDrawn = first->stats.layoutShadowDraws;

    // a still scene draws no tile: the second settled frame keeps all six (D146)
    clearLog();
    const auto still = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(still);
    CHECK(still->stats.tilesDrawn == 0u);
    CHECK(still->stats.tilesCached == 6u);
    CHECK(still->stats.shadowDraws == 0u);
    CHECK(still->stats.atlasBytesWritten == 0u);
    CHECK(shadowDrawsOf(still->drawLog) == 0u);
    CHECK(still->stats.layoutTiles == 6u);
    CHECK(still->stats.layoutShadowDraws == drawsWhenDrawn);
    CHECK(still->atlas.depth == first->atlas.depth);
    CHECK(still->hdr.rgba == first->hdr.rgba);
    std::cout << fmt::format(
      "LP3.3 (h) still: tiles drawn {}, cached {}, shadow draws {} (the drawing frame's "
      "{}), atlas bytes equal {}\n",
      still->stats.tilesDrawn,
      still->stats.tilesCached,
      still->stats.shadowDraws,
      drawsWhenDrawn,
      still->atlas.depth == first->atlas.depth);

    // cached-vs-fresh (D160): every tile drawn again is byte-identical
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
      access.passes.invalidateShadowTilesForTesting();
    }));
    const auto fresh = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(fresh);
    CHECK(tileClearsOf(fresh->drawLog, light) == 6u);
    CHECK(fresh->atlas.depth.size() == first->atlas.depth.size());
    CHECK(fresh->atlas.depth == first->atlas.depth);
    CHECK(fresh->hdr.rgba == first->hdr.rgba);
    std::cout << fmt::format(
      "LP3.3 (h) fresh: {} tiles drawn again, {} texels, byte-identical {}\n",
      tileClearsOf(fresh->drawLog, light),
      fresh->atlas.depth.size(),
      fresh->atlas.depth == first->atlas.depth);

    // a UV drag on an opaque caster redraws no tile (S36: st is in the caster version of
    // a perforated chunk alone)
    const auto dragged = withEntityLines(text, "lp3_box", [](const std::string& line) {
      auto out = line;
      for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"[ 0 1 0 0 ]", "[ 0 1 0 8 ]"}, {"[ 1 0 0 0 ]", "[ 1 0 0 8 ]"}})
      {
        if (const auto at = out.find(from); at != std::string::npos)
        {
          out.replace(at, from.size(), to);
        }
      }
      return out;
    });
    REQUIRE(dragged != text);
    offer(dragged, "lp3_point.map");
    const auto uv = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(uv);
    CHECK(shadowDrawsOf(uv->drawLog) == 0u);
    CHECK(uv->stats.tilesCached == 6u);
    CHECK(uv->atlas.depth == first->atlas.depth);
    std::cout << fmt::format(
      "LP3.3 (h) UV drag: {} tile clears or caster draws since the edit\n",
      shadowDrawsOf(uv->drawLog));

    // a moved occluder redraws its light's tiles (its record's caster hash moved)
    const auto moved = withEntityLines(dragged, "lp3_box", [](const std::string& line) {
      return movedLine(line, EngineVec3{0, 8, 0});
    });
    offer(moved, "lp3_point.map");
    const auto shifted = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shifted);
    CHECK(tileClearsOf(shifted->drawLog, light) >= 6u);
    CHECK(shifted->stats.layoutShadowDraws > 0u);
    CHECK(shifted->atlas.depth != first->atlas.depth);
    std::cout << fmt::format(
      "LP3.3 (h) moved occluder: {} tile clears since the move, layout draws {}\n",
      tileClearsOf(shifted->drawLog, light),
      shifted->stats.layoutShadowDraws);

    // the light's noshadows: its record dropped, and made again when it casts once more
    const auto toggled = [&](const std::string& value) {
      return withEntityLines(moved, "lp3_point", [&](const std::string& line) {
        return line == R"("noshadows" "0")" ? fmt::format(R"("noshadows" "{}")", value)
                                            : line;
      });
    };
    offer(toggled("1"), "lp3_point.map");
    const auto off = settledShadowFrame(*session, camera, 1, false);
    REQUIRE(off);
    CHECK(off->stats.lightsCasting == 0u);
    CHECK(off->records.empty());
    CHECK_FALSE(keptRecord(*session, light));
    offer(toggled("0"), "lp3_point.map");
    const auto on = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(on);
    CHECK(on->stats.lightsPlaced == 1u);
    CHECK(keptRecord(*session, light));
    CHECK(tileClearsOf(on->drawLog, light) >= 6u);
    CHECK(on->atlas.depth == shifted->atlas.depth);
    std::cout << fmt::format(
      "LP3.3 (h) noshadows: record kept {} while off, {} tile clears after it casts "
      "again, "
      "the atlas as before {}\n",
      false,
      tileClearsOf(on->drawLog, light),
      on->atlas.depth == shifted->atlas.depth);
    saveAtlasPng(on->atlas, "lp33_h_cache");
    saveHdrPng(on->hdr, "lp33_h_cache");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(i) relocation at settle: a third light enters, the moved tiles copied")
  {
    // H01's room with a second point light and a parallel light, both around the camera;
    // the parallel light cast-enabled or not
    const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    const auto scene = [](const std::string& sunShadows) {
      return lp3MapText("lp3_point.map")
             + "// entity 4\n{\n\"classname\" \"light\"\n\"name\" \"lp3_point_b\"\n"
               "\"origin\" \"-60 40 160\"\n\"light_radius\" \"400 400 400\"\n"
               "\"_color\" \"1 1 1\"\n\"noshadows\" \"0\"\n}\n"
             + fmt::format(
               "// entity 5\n{{\n\"classname\" \"light\"\n\"name\" \"lp3_sun\"\n"
               "\"origin\" \"-100 0 120\"\n\"light_radius\" \"220 220 110\"\n"
               "\"light_center\" \"-60 20 200\"\n\"parallel\" \"1\"\n"
               "\"_color\" \"1 1 1\"\n\"noshadows\" \"{}\"\n}}\n",
               sunShadows);
    };
    const auto tilesOf = [](const ShadowFrame& frame) {
      auto rects = std::map<std::pair<PreviewId, int>, AtlasRect>{};
      for (const auto& record : frame.records)
      {
        const auto first = record.type == ShadowLightType::Spot ? -1 : 0;
        for (auto side = first; side < first + record.sides; ++side)
        {
          if (record.placements[std::size_t(std::max(0, side))].placed)
          {
            rects[{record.light, side}] = tileOf(record, side);
          }
        }
      }
      return rects;
    };
    const auto sameTexels = [](
                              const ShadowFrame& a,
                              const AtlasRect& ra,
                              const ShadowFrame& b,
                              const AtlasRect& rb) {
      REQUIRE(ra.size == rb.size);
      auto differ = std::size_t{0};
      for (auto y = 0; y < ra.size; ++y)
      {
        for (auto x = 0; x < ra.size; ++x)
        {
          differ += a.atlas.at(std::uint32_t(ra.x + x), std::uint32_t(ra.y + y))
                    != b.atlas.at(std::uint32_t(rb.x + x), std::uint32_t(rb.y + y));
        }
      }
      return differ;
    };
    const auto movesOf = [](const std::vector<LitDrawRecord>& log) {
      return std::size_t(std::ranges::count_if(log, [](const LitDrawRecord& record) {
        return record.pass == LitDrawRecord::Pass::ShadowMove;
      }));
    };

    offer(scene("1"), "lp3_point.map");
    const auto two = settledShadowFrame(*session, camera, 3, true);
    REQUIRE(two);
    REQUIRE(two->records.size() == 2u);
    const auto before = tilesOf(*two);
    CHECK(before.size() == 12u);

    // the camera still past the still timer's delay: the next frames settle (C62)
    std::this_thread::sleep_for(300ms);
    offer(scene("0"), "lp3_point.map");
    auto oldRects = std::vector<AtlasRect>{};
    for (const auto& [side, rect] : before)
    {
      oldRects.push_back(rect);
    }
    const auto three = settledShadowFrame(*session, camera, 3, true, oldRects);
    REQUIRE(three);
    REQUIRE(three->records.size() == 3u);
    // the layout is the engine's packing of the frame's sides (D145, D160: the copied
    // code's tie orders, the parallel light's 1024 cascades first)
    {
      auto sizes = std::vector<EngineAtlasSize>{};
      for (const auto& record : three->records)
      {
        for (auto s = 0; s < record.sides; ++s)
        {
          sizes.push_back(EngineAtlasSize{
            EngineShadowMapResolutions[std::size_t(record.lod)],
            EngineShadowMapResolutions[std::size_t(record.lod)]});
        }
      }
      auto map = EngineTileMap{};
      REQUIRE(map.init(8192, 1024, 8));
      const auto expected = engineShadowAtlasLayout(map, sizes, 8192);
      auto index = std::size_t{0};
      auto mismatches = 0;
      for (const auto& record : three->records)
      {
        for (auto s = 0; s < record.sides; ++s, ++index)
        {
          const auto& got = record.placements[std::size_t(s)];
          mismatches += got.placed != expected[index].placed || got.x != expected[index].x
                        || got.y != expected[index].y;
        }
      }
      CHECK(mismatches == 0);
    }
    const auto after = tilesOf(*three);
    auto moved = std::vector<std::pair<PreviewId, int>>{};
    for (const auto& [side, rect] : before)
    {
      REQUIRE(after.contains(side));
      if (after.at(side) != rect)
      {
        moved.push_back(side);
      }
    }
    CHECK_FALSE(moved.empty());
    // the path the device takes (P3.4a): the copies, or the fallback's redraws
    const auto copies = session->shadowDepthCopyForTesting();
    auto pointLights = std::set<PreviewId>{};
    for (const auto& [side, rect] : before)
    {
      pointLights.insert(side.first);
    }
    auto pointClears = std::size_t{0};
    for (const auto light : pointLights)
    {
      pointClears += tileClearsOf(three->drawLog, light);
    }
    if (copies)
    {
      CHECK(movesOf(three->drawLog) == moved.size());
      CHECK(pointClears == 0u);
    }
    else
    {
      CHECK(movesOf(three->drawLog) == 0u);
      CHECK(pointClears > 0u);
      CHECK(std::ranges::any_of(session->programReport(), [](const ProgramReport& r) {
        return r.name == "shadow atlas relocation" && !r.enabled;
      }));
    }
    // bytes equal after a move (D160)
    auto differing = std::size_t{0};
    for (const auto& side : moved)
    {
      differing += sameTexels(*two, before.at(side), *three, after.at(side));
    }
    CHECK(differing == 0u);
    std::cout << fmt::format(
      "LP3.3 (i) relocation: {} of 12 tiles moved when the parallel light entered, the "
      "device copies D32 {}, {} moves logged, {} point-light tile clears, {} texels "
      "differing\n",
      moved.size(),
      copies,
      movesOf(three->drawLog),
      pointClears,
      differing);
    saveAtlasPng(three->atlas, "lp33_i_relocated");
    saveHdrPng(three->hdr, "lp33_i_relocated");

    // the fallback (P3.4a) on this device: the copy off, the parallel light leaving moves
    // the point lights' tiles back, drawn again; their texels as at first
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setShadowDepthCopyForTesting(false);
    }));
    std::this_thread::sleep_for(300ms);
    offer(scene("1"), "lp3_point.map");
    const auto back = settledShadowFrame(*session, camera, 3, true);
    REQUIRE(back);
    REQUIRE(back->records.size() == 2u);
    const auto again = tilesOf(*back);
    CHECK(again == before);
    CHECK(movesOf(back->drawLog) == 0u);
    auto redrawn = std::size_t{0};
    for (const auto light : pointLights)
    {
      redrawn += tileClearsOf(back->drawLog, light);
    }
    CHECK(redrawn >= moved.size());
    auto differingBack = std::size_t{0};
    for (const auto& [side, rect] : before)
    {
      differingBack += sameTexels(*two, rect, *back, again.at(side));
    }
    CHECK(differingBack == 0u);
    std::cout << fmt::format(
      "LP3.3 (i) fallback: {} point-light tile clears with the copy off, {} texels "
      "differing from the first layout's\n",
      redrawn,
      differingBack);
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setShadowDepthCopyForTesting(true);
    }));
  }

  // ---------------------------------------------------------------------------------
  SECTION("(j) moving: the held layout and LOD, the cascade budget, H11's overflow")
  {
    const auto setMoving = [&](const std::optional<bool> moving) {
      REQUIRE(inFrame(*session, [moving](RenderFrameAccess& access) {
        access.passes.setShadowsMovingForTesting(moving);
        access.passes.setDrawLogForTesting(true);
      }));
    };
    const auto clearedSides = [](const std::vector<LitDrawRecord>& log) {
      auto sides = std::set<int>{};
      for (const auto& record : log)
      {
        if (
          record.pass == LitDrawRecord::Pass::ShadowClear && record.light != NoPreviewId)
        {
          sides.insert(record.side);
        }
      }
      return sides;
    };

    const auto samePlacements = [](
                                  const ShadowLightRecord& a,
                                  const ShadowLightRecord& b) {
      for (std::size_t s = 0; s < 6; ++s)
      {
        const auto& pa = a.placements[s];
        const auto& pb = b.placements[s];
        if (pa.placed != pb.placed || pa.x != pb.x || pa.y != pb.y || pa.size != pb.size)
        {
          return false;
        }
      }
      return true;
    };
    // every frame's stats, records and draws, read at the start of the next frame
    struct Seen
    {
      LitFrameStats stats;
      std::vector<ShadowLightRecord> records;
      std::vector<LitDrawRecord> log;
    };
    auto seen = std::make_shared<std::vector<Seen>>();
    auto seenMutex = std::make_shared<std::mutex>();
    // the frames of camera, set now, of the n frames asked for after it (the last one's
    // not read)
    const auto framesOf = [&](const CameraState& camera, const int n) {
      session->setEveryFrameHookForTesting([seen, seenMutex](RenderFrameAccess& access) {
        const auto lock = std::scoped_lock{*seenMutex};
        seen->push_back(Seen{
          access.passes.lastStats(),
          access.passes.shadowLights(),
          access.passes.drawLogForTesting()});
        access.passes.setDrawLogForTesting(true);
      });
      session->setCamera(camera);
      for (auto i = 0; i < n; ++i)
      {
        REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
      }
      session->setEveryFrameHookForTesting({});
      const auto lock = std::scoped_lock{*seenMutex};
      auto frames = std::vector<Seen>{};
      for (const auto& frame : *seen)
      {
        if (frame.stats.camera == camera)
        {
          frames.push_back(frame);
        }
      }
      seen->clear();
      return frames;
    };

    // a parallel light: while the camera moves, MovingCascadeBudget (2) cascades a frame,
    // the stalest first; the others stale, sampled with the matrices they were drawn
    // with (P3.7: the frame's record holds them, the consumer reads nothing else)
    const auto c0 = litCamera({-112, 0, 96}, {1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_parallel.map"), "lp3_parallel.map");
    const auto rest = settledShadowFrame(*session, c0, 1, false);
    REQUIRE(rest);
    REQUIRE(rest->records.size() == 1u);
    REQUIRE(rest->records[0].sides == 4);
    setMoving(true);
    const auto atC1 = framesOf(litCamera({-112, 6, 96}, {1, 0, 0}, {0, 0, 1}), 3);
    REQUIRE(atC1.size() >= 2u);
    const auto& a = atC1[0];
    const auto& b = atC1[1];
    REQUIRE(a.records.size() == 1u);
    CHECK(a.stats.shadowsMoving);
    CHECK(a.stats.tilesDrawn == 2u);
    CHECK(a.stats.cascadesStale == 2u);
    CHECK(samePlacements(a.records[0], rest->records[0]));
    const auto drawnA = clearedSides(a.log);
    CHECK(drawnA.size() == 2u);
    for (std::size_t s = 0; s < 4; ++s)
    {
      CAPTURE(s);
      if (drawnA.contains(int(s)))
      {
        CHECK(a.records[0].shadowV[s] != rest->records[0].shadowV[s]);
      }
      else
      {
        CHECK(a.records[0].shadowV[s] == rest->records[0].shadowV[s]);
        CHECK(a.records[0].shadowP[s] == rest->records[0].shadowP[s]);
      }
    }
    // the same camera's next frame: the two left stale
    const auto drawnB = clearedSides(b.log);
    CHECK(b.stats.tilesDrawn == 2u);
    CHECK(b.stats.cascadesStale == 0u);
    for (const auto side : drawnB)
    {
      CHECK_FALSE(drawnA.contains(side));
    }
    // a second move: the stalest first, those drawn at the first frame
    const auto atC2 = framesOf(litCamera({-112, 12, 96}, {1, 0, 0}, {0, 0, 1}), 3);
    REQUIRE(atC2.size() >= 2u);
    CHECK(clearedSides(atC2[0].log) == drawnA);
    CHECK(atC2[0].stats.cascadesStale == 2u);
    CHECK(clearedSides(atC2[1].log) == drawnB);
    CHECK(atC2[1].stats.cascadesStale == 0u);
    std::cout << fmt::format(
      "LP3.3 (j) cascades while moving: frames drew {} / {} ({} / {} stale), after a "
      "second "
      "move {} / {} ({} / {} stale)\n",
      a.stats.tilesDrawn,
      b.stats.tilesDrawn,
      a.stats.cascadesStale,
      b.stats.cascadesStale,
      atC2[0].stats.tilesDrawn,
      atC2[1].stats.tilesDrawn,
      atC2[0].stats.cascadesStale,
      atC2[1].stats.cascadesStale);

    // a point light's LOD held while the camera moves away: it changes on the third frame
    // in a row whose exact LOD differs (D152)
    setMoving(std::nullopt);
    const auto inside = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_point.map"), "lp3_point.map");
    const auto nearFrame = settledShadowFrame(*session, inside, 1, false);
    REQUIRE(nearFrame);
    REQUIRE(nearFrame->records.size() == 1u);
    REQUIRE(nearFrame->records[0].lod == 1);
    setMoving(true);
    const auto atFar = framesOf(litCamera({-1400, 0, 128}, {1, 0, 0}, {0, 0, 1}), 4);
    REQUIRE(atFar.size() >= 3u);
    auto lods = std::vector<std::pair<int, int>>{};
    for (std::size_t i = 0; i < 3; ++i)
    {
      REQUIRE(atFar[i].records.size() == 1u);
      lods.emplace_back(atFar[i].records[0].lod, atFar[i].records[0].exactLod);
      CHECK(atFar[i].stats.shadowLodHeld == (i < 2));
    }
    REQUIRE(lods[0].second != 1);
    CHECK(lods[0].first == 1);
    CHECK(lods[1].first == 1);
    CHECK(lods[2].first == lods[2].second);
    CHECK(atFar[2].stats.tilesDrawn == 6u);
    std::cout << fmt::format(
      "LP3.3 (j) LOD while moving away: held {} {} then {} (exact {}), {} tiles drawn "
      "then\n",
      lods[0].first,
      lods[1].first,
      lods[2].first,
      lods[2].second,
      atFar[2].stats.tilesDrawn);

    // H11's overflow while moving: the two unplaced lights stay unplaced, no tile moves
    setMoving(std::nullopt);
    offer(lp3MapText("lp3_overflow.map"), "lp3_overflow.map");
    const auto full = settledShadowFrame(
      *session, litCamera({0, 0, 128}, {1, 0, 0}, {0, 0, 1}), 43, false);
    REQUIRE(full);
    REQUIRE(full->stats.lightsUnplaced == 2u);
    setMoving(true);
    const auto nudged = framesOf(litCamera({0, 2, 128}, {1, 0, 0}, {0, 0, 1}), 2);
    REQUIRE_FALSE(nudged.empty());
    const auto& n = nudged.front();
    CHECK(n.stats.shadowsMoving);
    CHECK(n.stats.lightsUnplaced == 2u);
    CHECK(n.stats.unplacedLights == full->stats.unplacedLights);
    CHECK(n.stats.tilesDrawn == 0u);
    CHECK(n.stats.tilesRelocated == 0u);
    CHECK(n.stats.layoutTiles == 256u);
    REQUIRE(n.records.size() == full->records.size());
    auto held = true;
    for (std::size_t r = 0; r < full->records.size(); ++r)
    {
      held = held && n.records[r].light == full->records[r].light
             && samePlacements(n.records[r], full->records[r]);
    }
    CHECK(held);
    std::cout << fmt::format(
      "LP3.3 (j) 43 lights while moving: {} unplaced ({}), {} tiles drawn, {} kept\n",
      n.stats.lightsUnplaced,
      fmt::join(n.stats.unplacedLights, ", "),
      n.stats.tilesDrawn,
      n.stats.tilesCached);
    setMoving(std::nullopt);
  }

  // ---------------------------------------------------------------------------------
  SECTION("(k) settled: the still timer's frame settles a held layout (D151, C62, A49)")
  {
    const auto ahead = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    const auto behind = litCamera({-200, 0, 128}, {-1, 0, 0}, {0, 0, 1});
    offer(lp3MapText("lp3_vacated.map"), "lp3_vacated.map");
    const auto both = settledShadowFrame(*session, ahead, 2, false);
    REQUIRE(both);
    REQUIRE(both->records.size() == 2u);
    std::this_thread::sleep_for(300ms);
    const auto firings = session->stillTimerFiringsForTesting();
    // every frame's stats and records, read at the start of the next frame
    struct Seen
    {
      LitFrameStats stats;
      std::vector<ShadowLightRecord> records;
    };
    auto seen = std::make_shared<std::vector<Seen>>();
    auto seenMutex = std::make_shared<std::mutex>();
    session->setEveryFrameHookForTesting([seen, seenMutex](RenderFrameAccess& access) {
      const auto lock = std::scoped_lock{*seenMutex};
      seen->push_back(Seen{access.passes.lastStats(), access.passes.shadowLights()});
    });
    // turned round: light B's volume behind the view leaves the layout, A's tiles stay
    // where the two lights' packing put them while the camera moves; then no trigger
    // but the still timer's, 200 ms after the turn
    session->setCamera(behind);
    REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
    std::this_thread::sleep_for(600ms);
    REQUIRE(inFrame(*session, [](RenderFrameAccess&) {}));
    session->setEveryFrameHookForTesting({});
    auto frames = std::vector<Seen>{};
    {
      const auto lock = std::scoped_lock{*seenMutex};
      for (const auto& frame : *seen)
      {
        if (frame.stats.camera == behind)
        {
          frames.push_back(frame);
        }
      }
    }
    REQUIRE(frames.size() >= 2u);
    const auto& first = frames.front();
    const auto& last = frames.back();
    REQUIRE(first.records.size() == 1u);
    CHECK(first.stats.shadowsMoving);
    CHECK(first.stats.shadowLayoutDrift);
    CHECK(first.stats.shadowSettleWanted);
    CHECK(first.stats.shadowsPending);
    CHECK(session->stillTimerFiringsForTesting() == firings + 1);
    // the still timer's frame: settled, A at the engine's packing of its sides alone
    CHECK_FALSE(last.stats.shadowsMoving);
    CHECK_FALSE(last.stats.shadowLayoutDrift);
    CHECK_FALSE(last.stats.shadowSettleWanted);
    REQUIRE(last.records.size() == 1u);
    const auto alone = packedAlone(6, 512);
    auto engine = true;
    for (std::size_t s = 0; s < 6; ++s)
    {
      engine = engine && last.records[0].placements[s].x == alone[s].x
               && last.records[0].placements[s].y == alone[s].y;
    }
    CHECK(engine);
    std::cout << fmt::format(
      "LP3.3 (k) turned round: {} frames, the first moving {} with drift {} (settle "
      "wanted "
      "{}), the still timer fired {} time(s), the last frame moving {} relocated {} "
      "drawn {}, A at the engine's placements {}\n",
      frames.size(),
      first.stats.shadowsMoving,
      first.stats.shadowLayoutDrift,
      first.stats.shadowSettleWanted,
      session->stillTimerFiringsForTesting() - firings,
      last.stats.shadowsMoving,
      last.stats.tilesRelocated,
      last.stats.tilesDrawn,
      engine);
    const auto settled = settledShadowFrame(*session, behind, 1, false);
    REQUIRE(settled);
    CHECK_FALSE(settled->stats.shadowsPending);
  }

  // ---------------------------------------------------------------------------------
  SECTION("(l) D160: a cached tile beside a vacated rect; 100 lights; D76's layouts")
  {
    // a parallel light's four cascades (the first 2048 block) and a point light's six 512
    // tiles beside them; the point light stops casting: its rects vacated and cleared,
    // the cascades kept, their texels as they were (P3.3)
    const auto camera = litCamera({-112, 0, 96}, {1, 0, 0}, {0, 0, 1});
    const auto withPoint = [](const std::string& noShadows) {
      return lp3MapText("lp3_parallel.map")
             + fmt::format(
               "// entity 4\n{{\n\"classname\" \"light\"\n\"name\" \"lp3_beside\"\n"
               "\"origin\" \"0 -40 120\"\n\"light_radius\" \"300 300 300\"\n"
               "\"_color\" \"1 1 1\"\n\"noshadows\" \"{}\"\n}}\n",
               noShadows);
    };
    offer(withPoint("0"), "lp3_parallel.map");
    const auto two = settledShadowFrame(*session, camera, 2, true);
    REQUIRE(two);
    REQUIRE(two->records.size() == 2u);
    const auto parallelAt = std::ranges::find_if(two->records, [](const auto& record) {
      return record.type == ShadowLightType::Parallel;
    });
    const auto pointAt = std::ranges::find_if(two->records, [](const auto& record) {
      return record.type == ShadowLightType::Point;
    });
    REQUIRE(parallelAt != two->records.end());
    REQUIRE(pointAt != two->records.end());
    const auto parallel = *parallelAt;
    const auto point = *pointAt;
    auto vacated = std::vector<AtlasRect>{};
    for (auto s = 0; s < 6; ++s)
    {
      vacated.push_back(tileOf(point, s));
    }
    // beside: a point tile shares an edge with a cascade
    auto adjacent = false;
    for (const auto& rect : vacated)
    {
      for (auto s = 0; s < 4; ++s)
      {
        const auto c = tileOf(parallel, s);
        adjacent = adjacent
                   || ((rect.x == c.x + c.size || c.x == rect.x + rect.size)
                       && rect.y < c.y + c.size && c.y < rect.y + rect.size)
                   || ((rect.y == c.y + c.size || c.y == rect.y + rect.size)
                       && rect.x < c.x + c.size && c.x < rect.x + rect.size);
      }
    }
    CHECK(adjacent);
    // the point light stops casting while the layout is held (a moving frame: a settling
    // one would pack the parallel light alone, whose equal-sized cascades the copied sort
    // may order otherwise, so they would move rather than stay)
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setShadowsMovingForTesting(true);
    }));
    offer(withPoint("1"), "lp3_parallel.map");
    auto one = std::optional<ShadowFrame>{};
    for (auto attempt = 0; attempt < 200 && (!one || one->records.size() != 1u);
         ++attempt)
    {
      one = settledShadowFrame(
        *session, camera, 2, true, vacated, std::nullopt, 10000ms, false);
    }
    REQUIRE(one);
    REQUIRE(one->records.size() == 1u);
    CHECK(one->stats.shadowsMoving);
    CHECK(one->stats.tilesCached == 4u);
    CHECK(tileClearsOf(one->drawLog, parallel.light) == 0u);
    auto cascadesChanged = std::size_t{0};
    for (auto s = 0; s < 4; ++s)
    {
      const auto rect = tileOf(parallel, s);
      CHECK(tileOf(one->records[0], s) == rect);
      for (auto y = rect.y; y < rect.y + rect.size; ++y)
      {
        for (auto x = rect.x; x < rect.x + rect.size; ++x)
        {
          cascadesChanged += two->atlas.at(std::uint32_t(x), std::uint32_t(y))
                             != one->atlas.at(std::uint32_t(x), std::uint32_t(y));
        }
      }
    }
    auto notCleared = std::size_t{0};
    for (const auto& rect : vacated)
    {
      for (auto y = rect.y; y < rect.y + rect.size; ++y)
      {
        for (auto x = rect.x; x < rect.x + rect.size; ++x)
        {
          notCleared += one->atlas.at(std::uint32_t(x), std::uint32_t(y)) != 1.0f;
        }
      }
    }
    CHECK(cascadesChanged == 0u);
    CHECK(notCleared == 0u);
    std::cout << fmt::format(
      "LP3.3 (l) vacated neighbour: point tiles beside the cascades {}, the cascades' "
      "texels changed {}, the vacated texels not 1.0 {}\n",
      adjacent,
      cascadesChanged,
      notCleared);
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setShadowsMovingForTesting(std::nullopt);
    }));

    // D76 (P3.5) with cached lights: the consumer draws them, so a still frame writes set
    // 11 as the frame that drew its tiles did
    offer(lp3MapText("lp3_layouts.map"), "lp3_layouts.map");
    const auto layouts = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    const auto drawnLayouts = settledShadowFrame(*session, layouts, 2, false);
    REQUIRE(drawnLayouts);
    const auto cachedLayouts = settledShadowFrame(*session, layouts, 2, false);
    REQUIRE(cachedLayouts);
    CHECK(cachedLayouts->stats.tilesDrawn == 0u);
    CHECK(cachedLayouts->stats.tilesCached == cachedLayouts->stats.layoutTiles);
    CHECK(cachedLayouts->stats.interactionSmWrites > 0u);
    CHECK(
      cachedLayouts->stats.interactionSmWrites
      == drawnLayouts->stats.interactionSmWrites);
    CHECK(
      cachedLayouts->stats.interactionWrites == drawnLayouts->stats.interactionWrites);
    std::cout << fmt::format(
      "LP3.3 (l) D76 with cached tiles: set 11 writes {} (the drawing frame's {}), set "
      "10 "
      "{}\n",
      cachedLayouts->stats.interactionSmWrites,
      drawnLayouts->stats.interactionSmWrites,
      cachedLayouts->stats.interactionWrites);

    // S30 (P3.26): 100 lights added, deleted, added and deleted again: the record table
    // and the held layout's tiles return to their starting size
    const auto base = lp3MapText("lp3_point.map");
    const auto inside = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
    auto hundred = base;
    for (auto i = 0; i < 100; ++i)
    {
      hundred += fmt::format(
        "// entity {}\n{{\n\"classname\" \"light\"\n\"name\" \"lp3_many_{:02}\"\n"
        "\"origin\" \"{} {} {}\"\n\"light_radius\" \"120 120 120\"\n"
        "\"_color\" \"0.01 0.01 0.01\"\n\"noshadows\" \"0\"\n}}\n",
        4 + i,
        i,
        48 + 24 * (i % 5 - 2),
        24 * (i / 5 % 5 - 2),
        66 + 20 * (i / 25));
    }
    const auto sizes = [&] {
      auto counts = std::pair<std::size_t, std::size_t>{};
      REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
        counts = {
          access.passes.shadowCache().size(),
          access.passes.shadowLayoutTilesForTesting()};
      }));
      return counts;
    };
    const auto offerSnapshot = [&](std::shared_ptr<const Snapshot> snapshot) {
      REQUIRE(waitFor([&] { return session->snapshotSlotFree(); }, 10000ms, false));
      REQUIRE(session->offerSnapshot(std::move(snapshot)));
    };
    offer(base, "lp3_point.map");
    const auto startFrame = settledShadowFrame(*session, inside, 1, false);
    REQUIRE(startFrame);
    const auto start = sizes();
    auto added = std::pair<std::size_t, std::size_t>{};
    for (auto round = 0; round < 2; ++round)
    {
      CAPTURE(round);
      auto many = lp3Snapshot(hundred, root, "lp3_point.map", ++sequence);
      offerSnapshot(many);
      REQUIRE(settledShadowFrame(*session, inside, 101, false));
      const auto full = sizes();
      CHECK(full.first > start.first);
      CHECK(full.second > start.second);
      if (round == 0)
      {
        added = full;
      }
      CHECK(full == added);
      // a snapshot that removes the 100 lights: their entities' ids and their nodes'
      auto few = Snapshot{};
      few.generation = 7;
      few.sequence = ++sequence;
      few.mapPath = (root / "base" / "maps" / "lp3" / "lp3_point.map").string();
      const auto names = snapshotEntityNames(*many);
      auto lights = std::set<PreviewId>{};
      for (const auto& [name, id] : names)
      {
        if (name.starts_with("lp3_many_"))
        {
          lights.insert(id);
          few.removed.push_back(id);
        }
      }
      for (const auto& node : many->nodes)
      {
        if (lights.contains(node.entity) && node.id != node.entity)
        {
          few.removed.push_back(node.id);
        }
      }
      CHECK(lights.size() == 100u);
      offerSnapshot(std::make_shared<const Snapshot>(std::move(few)));
      const auto alone = settledShadowFrame(*session, inside, 1, false);
      REQUIRE(alone);
      CHECK(alone->stats.lightsCasting == startFrame->stats.lightsCasting);
      CHECK(sizes() == start);
    }
    std::cout << fmt::format(
      "LP3.3 (l) 100 lights: records / layout tiles {} / {} at the start, {} / {} with "
      "the 100, {} / {} after each delete\n",
      start.first,
      start.second,
      added.first,
      added.second,
      sizes().first,
      sizes().second);
  }

  std::cout << fmt::format(
    "LP3.3 shadow records on a GPU: {:.1f} s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  teardown(*session, std::nullopt);
  messages.print("LP3.3 shadow records, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

namespace
{

// LP3.4: lp3_point.map with its box replaced by a grate of material (TrenchBroom's name),
// a 2-unit slab at x 32..34, y -24..24, z 104..152 between the light (0, 0, 128) and the
// far wall, whole inside the light's +x side (its corners 36.9 degrees off the axis)
std::string lp3GrateMapText(const std::string& material)
{
  auto text = lp3MapText("lp3_point.map");
  const auto from = text.find("// entity 2");
  const auto to = text.find("// entity 3");
  REQUIRE(from != std::string::npos);
  REQUIRE(to != std::string::npos);
  const auto grate = fmt::format(
    "// entity 2\n{{\n\"classname\" \"func_static\"\n\"name\" \"lp3_grate\"\n\"model\" "
    "\"lp3_grate\"\n\"origin\" \"33 0 128\"\n{}}}\n",
    boxBrush(32, -24, 104, 34, 24, 152, material));
  return text.substr(0, from) + grate + text.substr(to);
}

// the grate's two faces facing x (the light's and the far wall's), in world space
constexpr auto Lp3GrateNear = 32.0f;
constexpr auto Lp3GrateFar = 34.0f;
constexpr auto Lp3Grate = EngineBounds{EngineVec3{32, -24, 104}, EngineVec3{34, 24, 152}};
constexpr auto Lp3GrateNearFace =
  EngineBounds{EngineVec3{32, -24, 104}, EngineVec3{32, 24, 152}};
constexpr auto Lp3GrateFarFace =
  EngineBounds{EngineVec3{34, -24, 104}, EngineVec3{34, 24, 152}};

// a world point's depth in a side's tile as the atlas holds it and interactionSM compares
// it: clipToWindow · P · V · p, z / w (the depth range's 0.5 · ndc + 0.5)
float tileDepthOf(
  const ShadowLightRecord& record, const int side, const EngineVec3& point)
{
  const auto slice = std::size_t(std::max(0, side));
  const auto matrix = engineMatrixMultiply(
    engineClipSpaceToWindowSpace(),
    engineMatrixMultiply(record.shadowP[slice], record.shadowV[slice]));
  const auto p = std::array<float, 4>{point[0], point[1], point[2], 1.0f};
  auto out = std::array<float, 4>{};
  for (std::size_t r = 0; r < 4; ++r)
  {
    out[r] = matrix[r * 4 + 0] * p[0] + matrix[r * 4 + 1] * p[1]
             + matrix[r * 4 + 2] * p[2] + matrix[r * 4 + 3] * p[3];
  }
  return out[2] / out[3];
}

// what the grate's tile holds inside the footprint both its x faces cover (3 texels in
// from its edges: the slab's four thin side faces, seen from the light at a grazing
// angle, lie between the two faces' footprints): the side whose tile holds the whole
// slab, the texels nearer than the far clear (bars) and at it (holes), the bars' depth
// range, and the texels nearer outside the slab's footprint (with 2 texels of margin)
struct GrateTile
{
  int side = -100;
  std::size_t bars = 0;
  std::size_t holes = 0;
  float barMin = 2.0f;
  float barMax = -1.0f;
  std::size_t outside = 0;
  std::vector<float> texels;
};

GrateTile grateTileOf(const ShadowFrame& frame, const ShadowLightRecord& record)
{
  auto result = GrateTile{};
  for (auto side = 0; side < record.sides; ++side)
  {
    const auto footprint = footprintOf(record, side, Lp3Grate);
    const auto tile = tileOf(record, side);
    if (
      !footprint || (*footprint)[0] < float(tile.x) || (*footprint)[1] < float(tile.y)
      || (*footprint)[2] > float(tile.x + tile.size)
      || (*footprint)[3] > float(tile.y + tile.size))
    {
      continue;
    }
    result.side = side;
    const auto& f = *footprint;
    const auto nearFace = footprintOf(record, side, Lp3GrateNearFace);
    const auto farFace = footprintOf(record, side, Lp3GrateFarFace);
    REQUIRE(nearFace);
    REQUIRE(farFace);
    const auto both = std::array<float, 4>{
      std::max((*nearFace)[0], (*farFace)[0]),
      std::max((*nearFace)[1], (*farFace)[1]),
      std::min((*nearFace)[2], (*farFace)[2]),
      std::min((*nearFace)[3], (*farFace)[3])};
    for (auto ty = tile.y; ty < tile.y + tile.size; ++ty)
    {
      for (auto tx = tile.x; tx < tile.x + tile.size; ++tx)
      {
        const auto d = frame.atlas.at(std::uint32_t(tx), std::uint32_t(ty));
        const auto x = float(tx) + 0.5f;
        const auto y = float(ty) + 0.5f;
        const auto inner = x >= both[0] + 3.0f && x <= both[2] - 3.0f
                           && y >= both[1] + 3.0f && y <= both[3] - 3.0f;
        const auto margin =
          x >= f[0] - 2.0f && x <= f[2] + 2.0f && y >= f[1] - 2.0f && y <= f[3] + 2.0f;
        if (inner)
        {
          result.texels.push_back(d);
          if (d < 1.0f)
          {
            ++result.bars;
            result.barMin = std::min(result.barMin, d);
            result.barMax = std::max(result.barMax, d);
          }
          else
          {
            ++result.holes;
          }
        }
        else if (!margin && d < 1.0f)
        {
          ++result.outside;
        }
      }
    }
  }
  return result;
}

// LP3.4 (D158): a frame's LDR target (RGBA8, after its tonemap pass: a debug view drawn
// over it), its stats, its index and its kept tiles
struct LdrShot
{
  LdrImage ldr;
  LitFrameStats stats;
  std::uint64_t frameIndex = 0;
  std::vector<ShadowTileRecord> tiles;
};

/**
 * The first frame drawn after trigger (called once per try) whose stats wanted accepts,
 * read back from its LDR target after the tonemap pass; nullopt at the deadline.
 */
std::optional<LdrShot> ldrShot(
  LitSession& session,
  const std::function<bool(const LitFrameStats&)>& wanted,
  const std::function<void()>& trigger,
  const std::chrono::milliseconds timeout = 30000ms)
{
  struct State
  {
    LdrShot shot;
    bool taken = false;
    nvrhi::StagingTextureHandle staging;
  };
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto state = std::make_shared<State>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterTonemapForTesting([state, done, wanted](RenderFrameAccess& access) {
      const auto& stats = access.passes.lastStats();
      if (wanted(stats))
      {
        state->taken = true;
        state->shot.stats = stats;
        state->shot.frameIndex = access.frameIndex;
        for (const auto& [light, record] : access.passes.shadowCache().records())
        {
          for (const auto& tile : record.tiles)
          {
            if (tile.placed)
            {
              state->shot.tiles.push_back(tile);
            }
          }
        }
        auto* ldr = access.frame.find(frame_resource::CurrentRenderLdr)->texture;
        const auto& desc = ldr->getDesc();
        state->staging = access.device.createStagingTexture(
          nvrhi::TextureDesc{}
            .setWidth(desc.width)
            .setHeight(desc.height)
            .setFormat(desc.format),
          nvrhi::CpuAccessMode::Read);
        access.commandList.copyTexture(
          state->staging, nvrhi::TextureSlice{}, ldr, nvrhi::TextureSlice{});
      }
      done->set_value();
    });
    trigger();
    if (future.wait_for(10000ms) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (!state->taken)
    {
      std::this_thread::sleep_for(50ms);
      continue;
    }
    const auto read = inFrame(session, [state](RenderFrameAccess& access) {
      access.device.waitForIdle();
      const auto& desc = state->staging->getDesc();
      auto& image = state->shot.ldr;
      image.width = desc.width;
      image.height = desc.height;
      image.rgba.resize(std::size_t(desc.width) * desc.height * 4);
      auto pitch = std::size_t{0};
      const auto* bytes =
        static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
          state->staging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
      for (std::uint32_t y = 0; y < desc.height; ++y)
      {
        std::memcpy(
          image.rgba.data() + std::size_t(y) * desc.width * 4,
          bytes + y * pitch,
          std::size_t(desc.width) * 4);
      }
      access.device.unmapStagingTexture(state->staging);
      state->staging = nullptr;
    });
    if (!read)
    {
      return std::nullopt;
    }
    return std::move(state->shot);
  }
  return std::nullopt;
}

// an LDR shot as an RGB PNG under TB_LIT_PNG_OUT
void saveLdrPng(const LdrImage& ldr, const std::string& name)
{
  const auto folder = pngFolder();
  if (!folder || ldr.rgba.empty())
  {
    return;
  }
  auto rgb = std::vector<std::uint8_t>{};
  rgb.reserve(std::size_t(ldr.width) * ldr.height * 3);
  for (std::size_t i = 0; i < ldr.rgba.size(); ++i)
  {
    if (i % 4 != 3)
    {
      rgb.push_back(ldr.rgba[i]);
    }
  }
  const auto path = *folder / (name + ".png");
  CHECK(writePng(path, ldr.width, ldr.height, 3, rgb));
  std::cout << fmt::format("LP3.4 {}: picture -> {}\n", name, path.string());
}

// the debug views' square (the atlas fitted to the view): its left, top and side
struct DebugSquare
{
  int x0 = 0;
  int y0 = 0;
  int side = 0;
};

DebugSquare debugSquareOf(const LdrImage& ldr)
{
  const auto side = int(std::min(ldr.width, ldr.height));
  return DebugSquare{(int(ldr.width) - side) / 2, (int(ldr.height) - side) / 2, side};
}

// the tile-age view's tint of a kept tile in the frame frameIndex (LitPasses::
// recordDebugView's rule), RGB 0..1
std::array<float, 3> tileTintOf(
  const ShadowTileRecord& tile, const std::uint64_t frameIndex)
{
  if (tile.provisional)
  {
    return {1.0f, 0.0f, 1.0f};
  }
  if (tile.relocatedFrame == frameIndex && frameIndex != 0)
  {
    return {0.0f, 0.5f, 1.0f};
  }
  if (tile.drawnFrame == frameIndex)
  {
    return {0.0f, 1.0f, 0.0f};
  }
  const auto age = double(frameIndex - std::min(frameIndex, tile.drawnFrame));
  const auto t = float(std::clamp(std::log2(std::max(age, 1.0)) / 6.0, 0.0, 1.0));
  return {1.0f, 1.0f - t, 0.0f};
}

/**
 * The debug view's check over an atlas read-back: each pixel of the square whose atlas
 * texel (its centre's, the square fitted to 8192²) lies in the read-back region with an
 * even neighbourhood (17 x 17 texels within 1e-6: the bilinear tap reads that depth) is
 * the atlas's depth in red, (255 d, 0, 0), or under a tile with tint, 0.5 tint + 0.5
 * that; returns the pixels checked, those off by more than 2, and those checked below 250
 * red.
 */
struct DebugViewCheck
{
  std::size_t checked = 0;
  std::size_t wrong = 0;
  std::size_t nearer = 0;
  std::size_t tinted = 0;
};

DebugViewCheck checkDebugView(
  const LdrShot& shot, const DepthReadback& atlas, const bool tinted)
{
  auto result = DebugViewCheck{};
  const auto square = debugSquareOf(shot.ldr);
  for (auto py = square.y0; py < square.y0 + square.side; py += 3)
  {
    for (auto px = square.x0; px < square.x0 + square.side; px += 3)
    {
      const auto u = (float(px - square.x0) + 0.5f) / float(square.side);
      const auto v = (float(py - square.y0) + 0.5f) / float(square.side);
      const auto tx = int(u * 8192.0f);
      const auto ty = int(v * 8192.0f);
      if (
        tx - 8 < int(atlas.x) || ty - 8 < int(atlas.y)
        || tx + 8 >= int(atlas.x + atlas.width) || ty + 8 >= int(atlas.y + atlas.height))
      {
        continue;
      }
      auto lo = 2.0f;
      auto hi = -1.0f;
      for (auto dy = -8; dy <= 8; ++dy)
      {
        for (auto dx = -8; dx <= 8; ++dx)
        {
          const auto d = atlas.at(std::uint32_t(tx + dx), std::uint32_t(ty + dy));
          lo = std::min(lo, d);
          hi = std::max(hi, d);
        }
      }
      if (hi - lo > 1e-6f)
      {
        continue;
      }
      auto expected = std::array<float, 3>{255.0f * lo, 0.0f, 0.0f};
      if (tinted)
      {
        // the tiles' rects that hold the texel, and the tint of the last one drawn
        for (const auto& tile : shot.tiles)
        {
          if (
            tx >= tile.rect.x && tx < tile.rect.x + tile.rect.size && ty >= tile.rect.y
            && ty < tile.rect.y + tile.rect.size)
          {
            const auto tint = tileTintOf(tile, shot.frameIndex);
            for (std::size_t c = 0; c < 3; ++c)
            {
              expected[c] = 0.5f * 255.0f * tint[c] + 0.5f * expected[c];
            }
            ++result.tinted;
          }
        }
      }
      const auto* pixel =
        &shot.ldr.rgba[(std::size_t(py) * shot.ldr.width + std::size_t(px)) * 4];
      ++result.checked;
      auto off = false;
      for (std::size_t c = 0; c < 3; ++c)
      {
        off = off || std::abs(float(pixel[c]) - expected[c]) > 2.0f;
      }
      if (off && result.wrong < 4)
      {
        std::cout << fmt::format(
          "LP3.4 debug view off at ({}, {}) texel ({}, {}) depth {}: expected {:.1f} "
          "{:.1f} "
          "{:.1f}, got {} {} {}\n",
          px,
          py,
          tx,
          ty,
          lo,
          expected[0],
          expected[1],
          expected[2],
          pixel[0],
          pixel[1],
          pixel[2]);
      }
      result.wrong += off ? 1 : 0;
      result.nearer += lo < 250.0f / 255.0f ? 1 : 0;
    }
  }
  return result;
}

// the clears of a light's side in the draw log (its tile drawn again)
std::size_t sideClearsOf(
  const std::vector<LitDrawRecord>& log, const PreviewId light, const int side)
{
  return std::size_t(std::ranges::count_if(log, [&](const LitDrawRecord& record) {
    return record.pass == LitDrawRecord::Pass::ShadowClear && record.light == light
           && record.side == side;
  }));
}

} // namespace

// LP3.4 (C61, D155, D156, K52, K53; D160, D161): perforated casters in pass 6 on the
// synthetic point scene (fixture/lit/maps/lp3/lp3_point.map, its box replaced by a grate
// of the shadows map's materials/lp3_parity.mtr): (m) the grate's bars at its depth and
// its holes at the far clear, the noShadows grate the same; (n) its image slot not
// complete: the tile provisional, then redrawn once it is. Hidden: it needs a GPU and the
// blobs.
TEST_CASE("LitSession perforated shadows on a GPU (LP3.4)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  if (!gamePathValue || !*gamePathValue)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the passes need the engine's shader blobs");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto root = assembleLp3Root(std::filesystem::path{gamePathValue});
  REQUIRE_FALSE(root.empty());
  // the shadows map's grates (H04, H08) and their images (textures/lp3p, copied whole)
  std::filesystem::copy_file(
    litFixtureFolder() / "materials" / "lp3_parity.mtr",
    root / "base" / "materials" / "lp3_parity.mtr");

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  const auto instance = testInstance(true, false, messages);
  REQUIRE(instance.valid());
  REQUIRE(initInstanceDispatcher(instance.handles()));
  auto service = rootService(root);
  auto events = Events{};
  auto session = rootSession(instance, events, *service, root, true, false, true);
  session->setExposed(true);
  REQUIRE(waitFor(
    [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
  auto sequence = std::uint64_t{0};
  const auto offer = [&](const std::string& text) {
    REQUIRE(session->offerSnapshot(lp3Snapshot(text, root, "lp3_point.map", ++sequence)));
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
  };
  const auto clearLog = [&] {
    REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
      access.passes.setDrawLogForTesting(true);
    }));
  };
  const auto camera = litCamera({-200, 0, 128}, {1, 0, 0}, {0, 0, 1});
  // the grate's texture: bars 5 texels wide every 16 (parity.py lp3p_grate_texture), so
  // 135 of every 256 texels opaque, 52.7 %; its face is perpendicular to the +x side's
  // axis, so its projection keeps the fraction up to the edges' rounding: a band of 45 to
  // 60 % (stated before the run)
  constexpr auto BarsMin = 0.45;
  constexpr auto BarsMax = 0.60;

  // ---------------------------------------------------------------------------------
  SECTION("(m) a perforated grate between a point light and a wall: bars and holes")
  {
    offer(lp3GrateMapText("lp3p/grate"));
    const auto shadowed = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(shadowed);
    const auto& frame = *shadowed;
    REQUIRE(frame.records.size() == 1u);
    const auto& record = frame.records[0];
    CHECK(frame.stats.lightsPlaced == 1u);
    CHECK(frame.stats.perforatedCastersSkipped == 0u);
    CHECK(frame.stats.tilesProvisional == 0u);
    CHECK_FALSE(record.provisional);
    CHECK(frame.converged);
    // the draws: the grate's texture_color draws in pass 6, the side's state 0 (E-LIT-12)
    auto perforatedDraws = std::size_t{0};
    for (const auto& draw : frame.drawLog)
    {
      if (
        draw.pass == LitDrawRecord::Pass::Shadow
        && draw.program == EngineBuiltinTextureVertexColor)
      {
        ++perforatedDraws;
        CHECK(draw.state == 0u);
      }
    }
    CHECK(perforatedDraws > 0u);
    const auto grate = grateTileOf(frame, record);
    REQUIRE(grate.side >= 0);
    const auto total = grate.bars + grate.holes;
    REQUIRE(total > 0u);
    const auto fraction = double(grate.bars) / double(total);
    const auto nearDepth = tileDepthOf(record, grate.side, {Lp3GrateNear, 0, 128});
    const auto farDepth = tileDepthOf(record, grate.side, {Lp3GrateFar, 0, 128});
    std::cout << fmt::format(
      "LP3.4 (m) grate: side {}, {} bars and {} holes in its footprint ({:.1f} % bars, "
      "band {:.0f}-{:.0f} %), bar depth {:.7f}..{:.7f} (the near face {:.7f}, the far "
      "face {:.7f}), {} texels nearer outside it, {} perforated draws of {} shadow "
      "draws\n",
      grate.side,
      grate.bars,
      grate.holes,
      100.0 * fraction,
      100.0 * BarsMin,
      100.0 * BarsMax,
      grate.barMin,
      grate.barMax,
      nearDepth,
      farDepth,
      grate.outside,
      perforatedDraws,
      frame.stats.layoutShadowDraws);
    CHECK(fraction >= BarsMin);
    CHECK(fraction <= BarsMax);
    // every bar at the grate's depth: one plane perpendicular to the side's axis, the
    // near face's or the far face's (one alone, whichever the side's FRONTSIDED cull
    // keeps: E-LIT-12), no polygon offset
    CHECK(grate.barMax - grate.barMin < 1e-5f);
    CHECK(
      (std::abs(grate.barMin - nearDepth) < 1e-4f
       || std::abs(grate.barMin - farDepth) < 1e-4f));
    // nothing else below the far clear in the grate's tile (the room's faces cast
    // nothing, the anchor is in another side)
    CHECK(grate.outside == 0u);
    saveAtlasPng(frame.atlas, "lp34_m_grate");
    saveHdrPng(frame.hdr, "lp34_m_grate");

    // the noShadows grate: the same draws, the same texels (C57: a perforated material
    // casts whatever its noShadows says)
    offer(lp3GrateMapText("lp3p/grate_noshadows"));
    const auto noShadows = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(noShadows);
    REQUIRE(noShadows->records.size() == 1u);
    const auto other = grateTileOf(*noShadows, noShadows->records[0]);
    CHECK(other.side == grate.side);
    CHECK(other.bars == grate.bars);
    CHECK(other.holes == grate.holes);
    CHECK(other.texels == grate.texels);
    std::cout << fmt::format(
      "LP3.4 (m) noShadows grate: {} bars, {} holes, texels equal {}\n",
      other.bars,
      other.holes,
      other.texels == grate.texels);

    // its shadow on the far wall: the grate's entity noshadows 1 (the anchor keeps the
    // light casting, so both pictures take the shadow-atlas program, SC733)
    offer(withKey(lp3GrateMapText("lp3p/grate"), "lp3_grate", "noshadows", "1"));
    const auto unshadowed = settledShadowFrame(*session, camera, 1, false);
    REQUIRE(unshadowed);
    const auto diff = comparePictures(frame.hdr, unshadowed->hdr);
    std::cout << fmt::format(
      "LP3.4 (m) the grate's shadow: {} pixels darker, {} brighter, {} equal\n",
      diff.darker,
      diff.brighter,
      diff.equal);
    CHECK(diff.darker > 100u);
    CHECK(diff.brighter == 0u);
    saveHdrPng(unshadowed->hdr, "lp34_m_grate_unshadowed");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(n) the grate's image not complete: provisional, then redrawn once")
  {
    offer(lp3GrateMapText("lp3p/grate"));
    const auto first = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(first);
    REQUIRE(first->records.size() == 1u);
    const auto light = first->records[0].light;
    const auto before = grateTileOf(*first, first->records[0]);
    REQUIRE(before.side >= 0);
    // the grate's alpha-tested stage image (its coverage stage's, TD_COVERAGE), dropped
    // with no uploads: its slot binds its intrinsic until its texture is back
    auto image = std::optional<MaterialImageKey>{};
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      for (const auto& viewLight : access.passes.lists().lights)
      {
        for (const auto& caster : viewLight.casters)
        {
          if (caster.chunk->castClass == CastClass::Perforated && caster.material)
          {
            for (const auto& stage : caster.material->stages)
            {
              if (stage.hasAlphaTest && stage.image)
              {
                image = stage.image;
              }
            }
          }
        }
      }
    }));
    REQUIRE(image);
    session->setUploadCapForTesting(0);
    auto dropped = false;
    REQUIRE(inFrame(*session, [&](RenderFrameAccess& access) {
      if (const auto id = access.images.find(*image))
      {
        access.images.dropTexture(*id, access.frameIndex);
        dropped = true;
      }
      access.passes.setDrawLogForTesting(true);
    }));
    REQUIRE(dropped);
    auto provisionalFrames = 0;
    auto provisional = std::optional<ShadowFrame>{};
    for (auto i = 0; i < 3; ++i)
    {
      auto frame =
        settledShadowFrame(*session, camera, 1, true, {}, std::nullopt, 10000ms, false);
      REQUIRE(frame);
      if (frame->stats.tilesProvisional > 0)
      {
        ++provisionalFrames;
        // the grate's tile alone, the light's record flagged, the frame not settled
        CHECK(frame->stats.tilesProvisional == 1u);
        CHECK(frame->stats.shadowsPending);
        CHECK_FALSE(frame->converged);
        REQUIRE(frame->records.size() == 1u);
        CHECK(frame->records[0].provisional);
        provisional = std::move(frame);
      }
    }
    CHECK(provisionalFrames == 3);
    REQUIRE(provisional);
    const auto stand = grateTileOf(*provisional, provisional->records[0]);
    std::cout << fmt::format(
      "LP3.4 (n) provisional: {} frames with the tile provisional, the grate drawn with "
      "its slot's stand-in: {} bars, {} holes (complete: {} and {})\n",
      provisionalFrames,
      stand.bars,
      stand.holes,
      before.bars,
      before.holes);
    saveAtlasPng(provisional->atlas, "lp34_n_provisional");

    // uploads again: the slot complete, the tile drawn once more and settled
    session->setUploadCapForTesting(UploadBytesPerFrame);
    clearLog();
    const auto settled = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(settled);
    CHECK(settled->converged);
    CHECK(settled->stats.tilesProvisional == 0u);
    REQUIRE(settled->records.size() == 1u);
    CHECK_FALSE(settled->records[0].provisional);
    const auto after = grateTileOf(*settled, settled->records[0]);
    CHECK(after.texels == before.texels);
    const auto grateClears = sideClearsOf(settled->drawLog, light, before.side);
    auto sideClears = std::vector<std::size_t>{};
    for (auto side = 0; side < 6; ++side)
    {
      sideClears.push_back(sideClearsOf(settled->drawLog, light, side));
      CHECK(sideClears.back() <= 1u);
    }
    CHECK(grateClears == 1u);
    // then nothing: every tile cached
    clearLog();
    const auto still = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(still);
    CHECK(still->stats.tilesDrawn == 0u);
    CHECK(still->stats.tilesProvisional == 0u);
    CHECK(tileClearsOf(still->drawLog, light) == 0u);
    std::cout << fmt::format(
      "LP3.4 (n) complete again: the grate's tile (side {}) drawn {} time(s), the six "
      "sides' clears {}, texels as before {}; the next settled frame drew {} tiles\n",
      before.side,
      grateClears,
      fmt::join(sideClears, " "),
      after.texels == before.texels,
      still->stats.tilesDrawn);
    saveAtlasPng(settled->atlas, "lp34_n_complete");
  }

  // ---------------------------------------------------------------------------------
  SECTION("(o) the debug views: the atlas where its tiles are, the tiles' tints")
  {
    // a view large enough for the square to show the tiles (the atlas 8192² in 480)
    session->resizeHeadlessForTesting(640, 480);
    offer(lp3MapText("lp3_point.map"));
    const auto settled = settledShadowFrame(*session, camera, 1, true);
    REQUIRE(settled);
    REQUIRE(settled->records.size() == 1u);
    CHECK(session->debugView() == DebugView::None);
    // no view: no debug draw
    CHECK(settled->stats.debugViewDraws == 0u);

    // ShadowAtlas: one quad, every even pixel the atlas's depth in red
    session->setDebugView(DebugView::ShadowAtlas);
    CHECK(session->debugView() == DebugView::ShadowAtlas);
    const auto atlasShot = ldrShot(
      *session,
      [](const LitFrameStats& stats) { return stats.debugViewDraws == 1u; },
      [&] { session->tableChanged(); });
    REQUIRE(atlasShot);
    const auto atlasCheck = checkDebugView(*atlasShot, settled->atlas, false);
    std::cout << fmt::format(
      "LP3.4 (o) atlas view: {} pixels checked against the read-back, {} off by more "
      "than 2, {} of a caster's depth (red below 250)\n",
      atlasCheck.checked,
      atlasCheck.wrong,
      atlasCheck.nearer);
    CHECK(atlasCheck.checked > 1000u);
    CHECK(atlasCheck.wrong == 0u);
    CHECK(atlasCheck.nearer > 0u);
    saveLdrPng(atlasShot->ldr, "lp34_o_atlas");

    // TileAge: the quad and the six tiles' rects; a frame that drew every tile (green),
    // then a cached one (yellow towards red by age)
    session->setDebugView(DebugView::TileAge);
    const auto freshShot = ldrShot(
      *session,
      [](const LitFrameStats& stats) {
        return stats.tilesDrawn == 6u && stats.debugViewDraws == 7u;
      },
      [&] {
        REQUIRE(inFrame(*session, [](RenderFrameAccess& access) {
          access.passes.invalidateShadowTilesForTesting();
        }));
      });
    REQUIRE(freshShot);
    const auto freshCheck = checkDebugView(*freshShot, settled->atlas, true);
    const auto cachedShot = ldrShot(
      *session,
      [](const LitFrameStats& stats) {
        return stats.tilesDrawn == 0u && stats.tilesCached == 6u
               && stats.debugViewDraws == 7u;
      },
      [&] { session->tableChanged(); });
    REQUIRE(cachedShot);
    const auto cachedCheck = checkDebugView(*cachedShot, settled->atlas, true);
    auto ages = std::vector<std::uint64_t>{};
    for (const auto& tile : cachedShot->tiles)
    {
      ages.push_back(cachedShot->frameIndex - tile.drawnFrame);
    }
    std::cout << fmt::format(
      "LP3.4 (o) tile-age view: fresh frame {} pixels checked ({} under a tile), {} off; "
      "cached frame {} checked ({} under a tile), {} off, the tiles' ages {}\n",
      freshCheck.checked,
      freshCheck.tinted,
      freshCheck.wrong,
      cachedCheck.checked,
      cachedCheck.tinted,
      cachedCheck.wrong,
      fmt::join(ages, " "));
    CHECK(freshCheck.tinted > 100u);
    CHECK(freshCheck.wrong == 0u);
    CHECK(cachedCheck.tinted > 100u);
    CHECK(cachedCheck.wrong == 0u);
    CHECK(std::ranges::all_of(ages, [](const auto age) { return age >= 1u; }));
    saveLdrPng(freshShot->ldr, "lp34_o_tileage_fresh");
    saveLdrPng(cachedShot->ldr, "lp34_o_tileage_cached");

    session->setDebugView(DebugView::None);
    const auto plainShot = ldrShot(
      *session,
      [](const LitFrameStats& stats) { return stats.debugViewDraws == 0u; },
      [&] { session->tableChanged(); });
    REQUIRE(plainShot);
    CHECK(plainShot->stats.debugViewDraws == 0u);
  }

  std::cout << fmt::format(
    "LP3.4 perforated shadows on a GPU: {:.1f} s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  teardown(*session, std::nullopt);
  messages.print("LP3.4 perforated shadows, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

// LP3.4 (D158, §4's exit): the debug views on the shadows map's rooms H01 and H11, as
// PNGs under TB_LIT_PNG_OUT for the director: a copy of the parity preview root (read
// only; the debug view's builtin/texture blobs added from the game folder), H01's and
// H11's cameras of its rows.json, each room's plain picture and both views. Hidden: it
// needs a GPU, the game's blobs and the root of a parity.py run.
TEST_CASE("LitSession shadow debug views on the shadows map (LP3.4)", "[.][litgpu2]")
{
  const auto* gamePathValue = std::getenv("TB_LIT_GAME_PATH");
  const auto* parityValue = std::getenv("TB_LIT_PARITY_ROOT");
  if (!gamePathValue || !*gamePathValue || !parityValue || !*parityValue)
  {
    SKIP("TB_LIT_GAME_PATH or TB_LIT_PARITY_ROOT is not set");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto parity = std::filesystem::path{parityValue};
  const auto gamePath = std::filesystem::path{gamePathValue};
  const auto root = std::filesystem::temp_directory_path() / "TbLitLibTest-lp34-views";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  std::filesystem::copy(
    parity / "base", root / "base", std::filesystem::copy_options::recursive);
  {
    const auto* builtin = engineBuiltin(EngineBuiltinTextured);
    REQUIRE(builtin);
    for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
      const auto relative = shaderBlobPath(builtin->name, stage);
      std::filesystem::create_directories((root / "base" / relative).parent_path());
      std::filesystem::copy_file(
        gamePath / "base" / relative,
        root / "base" / relative,
        std::filesystem::copy_options::overwrite_existing);
    }
  }
  const auto rowsText = readTextFile(parity / "parity" / "rows.json");
  const auto record = JsonReader{rowsText}.value();
  REQUIRE(record);
  const auto* rows = record->find("rows");
  REQUIRE(rows);

  auto messages = ValidationMessages{};
  messages.mirroredMessageId = OutputNotConsumedId;
  {
    const auto instance = testInstance(true, false, messages);
    REQUIRE(instance.valid());
    REQUIRE(initInstanceDispatcher(instance.handles()));
    auto service = rootService(root);
    auto events = Events{};
    auto options = SessionOptions{};
    options.validation = true;
    options.gamePath = root;
    auto session = LitSession::create(
                     instance.handles(),
                     VK_NULL_HANDLE,
                     1280,
                     720,
                     options,
                     events.sink(),
                     7,
                     service.get())
                   | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    session->setExposed(true);
    REQUIRE(waitFor(
      [&] { return events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms, false));
    auto offered = false;
    for (const auto* id : {"H01", "H11"})
    {
      CAPTURE(id);
      const auto* row = static_cast<const JsonValue*>(nullptr);
      for (const auto& candidate : rows->array)
      {
        if (candidate.text("id") == id)
        {
          row = &candidate;
        }
      }
      REQUIRE(row);
      if (!offered)
      {
        REQUIRE(session->offerSnapshot(paritySnapshot(root, row->text("map"), 1)));
        offered = true;
      }
      session->skipShaderPassesForTesting(row->numberOr("skip_ambient", 1.0) == 1.0);
      const auto parsed = parityCameraOfLine(row->text("camera"));
      REQUIRE(parsed);
      auto camera = parsed->camera;
      camera.viewportWidth = 1280;
      camera.viewportHeight = 720;
      const auto lights = std::string{id} == "H11" ? std::size_t{43} : std::size_t{1};
      session->setDebugView(DebugView::None);
      const auto settled = settledShadowFrame(*session, camera, lights, false);
      REQUIRE(settled);
      const auto lower = std::string{id} == "H11" ? "h11" : "h01";
      const auto plain = ldrShot(
        *session,
        [](const LitFrameStats& stats) { return stats.debugViewDraws == 0u; },
        [&] { session->tableChanged(); });
      REQUIRE(plain);
      saveLdrPng(plain->ldr, fmt::format("lp34_{}_none", lower));
      session->setDebugView(DebugView::ShadowAtlas);
      const auto atlas = ldrShot(
        *session,
        [](const LitFrameStats& stats) { return stats.debugViewDraws == 1u; },
        [&] { session->tableChanged(); });
      REQUIRE(atlas);
      saveLdrPng(atlas->ldr, fmt::format("lp34_{}_atlas", lower));
      session->setDebugView(DebugView::TileAge);
      const auto tiles = ldrShot(
        *session,
        [](const LitFrameStats& stats) { return stats.debugViewDraws > 1u; },
        [&] { session->tableChanged(); });
      REQUIRE(tiles);
      saveLdrPng(tiles->ldr, fmt::format("lp34_{}_tileage", lower));
      std::cout << fmt::format(
        "LP3.4 {} views: lights placed {}, unplaced {}, layout tiles {}, the tile-age "
        "view's draws {} (the atlas quad and a rect per tile)\n",
        id,
        settled->stats.lightsPlaced,
        settled->stats.lightsUnplaced,
        settled->stats.layoutTiles,
        tiles->stats.debugViewDraws);
      CHECK(tiles->stats.debugViewDraws == 1u + tiles->tiles.size());
      session->setDebugView(DebugView::None);
    }
    teardown(*session, std::nullopt);
  }
  std::filesystem::remove_all(root);
  std::cout << fmt::format(
    "LP3.4 debug views on the shadows map: {:.1f} s\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  messages.print("LP3.4 debug views, validation");
  CHECK(messages.errors == 0);
  CHECK(messages.warnings == 0);
}

} // namespace tb::lit

#endif
