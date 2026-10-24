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

// LPF.2 (D107, D108, D110): the Qt-free host API on this machine's GPU. Hidden
// ("[.][feed]", its own tag, never in "[.][litgpu]"): it needs a Vulkan driver and, for
// the lit sections, the game folder's shader blobs and light entityDef (TB_LIT_GAME_PATH,
// read only). Windows only, as every GPU case.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/EngineTables.h"
#include "lit/FeedHost.h"
#include "lit/FeedScene.h"
#include "lit/GameDeclSource.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
// after windows.h
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#ifdef _WIN32

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

// ----- the host's parts, in FEED.md's order -----

// TB_LIT_GAME_PATH (read only): the shader blobs (C1) and def/misc.def's light entityDef
std::optional<std::filesystem::path> feedGamePath()
{
  const auto* value = std::getenv("TB_LIT_GAME_PATH");
  if (!value || !*value)
  {
    return std::nullopt;
  }
  const auto path = std::filesystem::path{value};
  if (
    !std::filesystem::exists(path / "base" / "renderprogs2" / "spirv")
    || !std::filesystem::exists(path / "base" / "def" / "misc.def"))
  {
    return std::nullopt;
  }
  return path;
}

// the default light shaders a light without a texture falls back to, as stand-ins of the
// game's: a white light of no falloff (_white is the game's guis/assets/white,
// Image_program.cpp:490-494, written into the root)
const auto FeedLightMaterials = std::string{
  "lights/defaultPointLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"
  "lights/defaultProjectedLight\n{\n\tlightFalloffImage _noFalloff\n"
  "\t{\n\t\tmap _white\n\t\tcolored\n\t\tzeroClamp\n\t}\n}\n"};

void writeBinaryFile(const std::filesystem::path& path, const std::string& bytes)
{
  std::filesystem::create_directories(path.parent_path());
  auto file = std::ofstream{path, std::ios::binary};
  file.write(bytes.data(), std::streamsize(bytes.size()));
}

/**
 * A game root of the feed's own in the temp folder (never the game folder, C8): the
 * fixture's lp5_feed.mtr, the light stand-ins, a 4 × 4 white guis/assets/white.tga and a
 * copy of the game's def/misc.def (the light entityDef, C22). No image of the feed
 * materials exists in it.
 */
std::filesystem::path assembleFeedRoot(
  const std::string& name, const std::filesystem::path& gamePath)
{
  const auto root = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(root);
  const auto base = root / "base";
  std::filesystem::create_directories(base / "materials");
  std::filesystem::copy_file(
    litFixtureFolder() / "materials" / "lp5_feed.mtr",
    base / "materials" / "lp5_feed.mtr");
  writeTextFile(base / "materials" / "zz_feed_lights.mtr", FeedLightMaterials);
  {
    auto tga = std::string(18, '\0');
    tga[2] = 2;
    tga[12] = 4;
    tga[14] = 4;
    tga[16] = 32;
    tga[17] = 8;
    tga += std::string(4 * 4 * 4, char(-1));
    writeBinaryFile(base / "guis" / "assets" / "white.tga", tga);
  }
  std::filesystem::create_directories(base / "def");
  std::filesystem::copy_file(
    gamePath / "base" / "def" / "misc.def", base / "def" / "misc.def");
  return root;
}

struct HostEvents
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

  void print(const std::string_view label)
  {
    const auto lock = std::scoped_lock{mutex};
    for (const auto& [event, message, generation] : events)
    {
      if (event == SessionEvent::Warning || event == SessionEvent::Error)
      {
        std::cout << label << " event " << int(event) << " (gen " << generation
                  << "): " << message << "\n";
      }
    }
  }
};

// LP2.0, T0.2: the layer's id of "Vertex attribute at location N not consumed by vertex
// shader", raised by the engine's own LAYOUT_DRAW_VERT input layout (mirror.json
// M-PIPE-DEFECT-01; the engine filters it with VK_LAYER_MESSAGE_ID_FILTER)
constexpr auto OutputNotConsumedId = std::string_view{"WARNING-Shader-OutputNotConsumed"};

// the instance's validation messages, counted; the engine's by-design warning apart
struct ValidationCount
{
  std::atomic<int> errors = 0;
  std::atomic<int> warnings = 0;
  std::atomic<int> mirrored = 0;
  std::mutex mutex;
  std::vector<std::string> texts;

  VulkanMessageSink sink()
  {
    return [this](const VulkanMessage& message) {
      if (!message.error && message.id == OutputNotConsumedId)
      {
        ++mirrored;
        return;
      }
      ++(message.error ? errors : warnings);
      const auto lock = std::scoped_lock{mutex};
      texts.push_back(message.id + ": " + message.text);
    };
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

// the exposure every feed session pins (D110, as the parity rows pin it)
constexpr auto FeedPinnedLuminance = 1.0f;

EngineTonemapDefaults pinnedTonemap()
{
  auto tonemap = EngineTonemap;
  tonemap.hdrMinLuminance = FeedPinnedLuminance;
  tonemap.hdrMaxLuminance = FeedPinnedLuminance;
  return tonemap;
}

struct HostOptions
{
  std::uint32_t width = 160;
  std::uint32_t height = 120;
  std::uint64_t generation = 1;
  bool validation = true;
  std::filesystem::path frameLogPath;
  std::function<void(std::string_view)> beforeWorkItem;
};

/**
 * FEED.md's host: the instance, the listener and the material service over root, the
 * headless session with the game's blobs (gamePath), the exposure pinned (D110). Torn
 * down by close() (lit::feed::teardown), or by the destructor when a check failed first.
 */
struct Host
{
  ValidationCount validation;
  HostEvents events;
  std::shared_ptr<feed::GuardedListener> listener =
    std::make_shared<feed::GuardedListener>();
  std::optional<VulkanInstance> instance;
  std::unique_ptr<MaterialService> service;
  std::unique_ptr<LitSession> session;

  Host(
    const std::filesystem::path& root,
    const std::filesystem::path& gamePath,
    const HostOptions& options = {})
  {
    auto instanceOptions = VulkanInstanceOptions{};
    instanceOptions.validation = options.validation;
    instanceOptions.messages = validation.sink();
    instanceOptions.applicationName = "TbLitLibTest feed host";
    instance = VulkanInstance::create(std::move(instanceOptions))
               | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    REQUIRE(initInstanceDispatcher(instance->handles()));

    auto serviceOptions = MaterialServiceOptions{};
    serviceOptions.makeSource = [root](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(root, &cancel)};
    };
    serviceOptions.parseAll = true;
    serviceOptions.listener = feed::GuardedListener::listener(listener);
    serviceOptions.beforeWorkItemForTesting = options.beforeWorkItem;
    service = MaterialService::create(std::move(serviceOptions))
              | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();

    auto sessionOptions = SessionOptions{};
    sessionOptions.validation = options.validation;
    sessionOptions.gamePath = gamePath;
    sessionOptions.frameLogPath = options.frameLogPath;
    session = LitSession::create(
                instance->handles(),
                VK_NULL_HANDLE,
                options.width,
                options.height,
                sessionOptions,
                events.sink(),
                options.generation,
                service.get())
              | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    listener->attach(session.get());
    session->setTonemapForTesting(pinnedTonemap());
    session->setExposed(true);
  }

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  ~Host()
  {
    if (session || service || instance)
    {
      static_cast<void>(close());
    }
  }

  // offers snapshot once the inbox is free
  void offer(Snapshot snapshot) const
  {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
    while (!session->offerSnapshot(shared))
    {
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  }

  TeardownReport close()
  {
    auto report = TeardownReport{};
    if (instance)
    {
      report = feed::teardown(
        std::move(session), std::move(service), std::move(*instance), listener.get());
      instance.reset();
    }
    return report;
  }
};

// the process's threads and OS handles (U2.12's measure, tst_LitSession's processCounts)
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

template <typename Predicate>
bool waitFor(Predicate predicate, const std::chrono::milliseconds timeout)
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

// a w × h RGBA8 picture of one colour, alpha 255, rows top first
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

// step 3's scene, through the map text's reader: a 512-unit slab of lp5/feed_side facing
// -y and a white light before it
std::string slabMapText()
{
  return "{\n\"classname\" \"worldspawn\"\n"
         + boxBrush(-256, 0, -256, 256, 16, 256, "lp5/feed_side")
         + "}\n{\n\"classname\" \"light\"\n\"origin\" \"0 -200 0\"\n"
           "\"light_radius\" \"600 600 600\"\n}\n";
}

} // namespace

TEST_CASE("Feed host: lit::VulkanInstance (LPF.2, D107)", "[.][feed]")
{
  SECTION("the message sink gets what the messenger reports")
  {
    // a message submitted through VK_EXT_debug_utils reaches the product's sink, as the
    // validation layer's do (the D71 case's intentional OutputNotConsumed warnings)
    auto mutex = std::mutex{};
    auto received = std::vector<VulkanMessage>{};
    for (const auto validation : {false, true})
    {
      CAPTURE(validation);
      {
        const auto lock = std::scoped_lock{mutex};
        received.clear();
      }
      auto options = VulkanInstanceOptions{};
      options.validation = validation;
      options.messages = [&](const VulkanMessage& message) {
        const auto lock = std::scoped_lock{mutex};
        received.push_back(message);
      };
      auto instance = VulkanInstance::create(std::move(options))
                      | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      REQUIRE(instance.valid());
      CHECK(instance.handles().instance == instance.instance());
      CHECK(instance.handles().getInstanceProcAddr != nullptr);
      if (!validation)
      {
        // no messenger, no layer: nothing to report through
        CHECK_FALSE(instance.messenger());
        CHECK_FALSE(instance.validationLayer());
        continue;
      }
      REQUIRE(instance.messenger());
      CHECK(instance.validationLayer());
      const auto submit =
        instance.get<PFN_vkSubmitDebugUtilsMessageEXT>("vkSubmitDebugUtilsMessageEXT");
      REQUIRE(submit != nullptr);
      auto data = VkDebugUtilsMessengerCallbackDataEXT{};
      data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
      data.pMessageIdName = "LPF2-feed-host";
      data.messageIdNumber = 4242;
      data.pMessage = "an intentional message";
      submit(
        instance.instance(),
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        &data);
      submit(
        instance.instance(),
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        &data);
      const auto lock = std::scoped_lock{mutex};
      std::erase_if(
        received, [](const auto& message) { return message.id != "LPF2-feed-host"; });
      REQUIRE(received.size() == 2u);
      CHECK(received[0].error);
      CHECK(received[0].idNumber == 4242);
      CHECK(received[0].text == "an intentional message");
      CHECK_FALSE(received[1].error);
    }
  }

  SECTION("destroy runs once, and a moved-from instance is none")
  {
    auto instance = VulkanInstance::create({})
                    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    auto moved = std::move(instance);
    CHECK_FALSE(instance.valid()); // NOLINT(bugprone-use-after-move)
    CHECK(instance.handles().instance == VK_NULL_HANDLE);
    CHECK(moved.valid());
    CHECK(moved.extensions().empty());
    moved.destroy();
    moved.destroy();
    CHECK_FALSE(moved.valid());
    CHECK(moved.procAddr("vkCreateDevice") == nullptr);
  }
}

// LPF.2 step 3 (D107, P5.12, P5.24): readbackLdr as a product call, the guarded listener
// and the teardown helper, on a headless host made in FEED.md's order
TEST_CASE(
  "Feed host: readbackLdr, the guarded listener, the teardown (LPF.2)", "[.][feed]")
{
  const auto gamePath = feedGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH does not name a game folder with blobs and def/misc.def");
  }
  const auto root = assembleFeedRoot("TbLitLibTest-feed-host", *gamePath);
  const auto camera = feed::cameraLookingAt({0, -400, 0}, {0, 0, 0});

  SECTION(
    "readbackLdr: a settled frame, one staging texture per extent, a storm's timeout")
  {
    auto host = Host{root, *gamePath};
    REQUIRE(waitFor(
      [&] { return host.events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms));
    host.offer(snapshotOfMapText(slabMapText(), 1, 1) | kdl::value());
    host.session->setCamera(camera);

    const auto first = host.session->readbackLdr(60000ms);
    REQUIRE(first.is_success());
    const auto& image = first.value();
    CHECK(image.width == 160u);
    CHECK(image.height == 120u);
    CHECK(image.rgba.size() == 160u * 120u * 4u);
    // the slab is lit: the centre is not black
    const auto centre = (std::size_t(60) * 160 + 80) * 4;
    CHECK(int(image.rgba[centre]) + image.rgba[centre + 1] + image.rgba[centre + 2] > 0);
    const auto creations = host.session->ldrStagingCreationsForTesting();
    const auto allocations = host.session->deviceMemoryAllocations();
    CHECK(creations == 1u);

    // the test hook forwards to the product call: the same settled picture, and the
    // staging texture is the one kept (no allocation more)
    const auto second = host.session->readbackLdrForTesting(10000ms);
    REQUIRE(second);
    CHECK(second->rgba == image.rgba);
    CHECK(host.session->ldrStagingCreationsForTesting() == creations);
    CHECK(host.session->deviceMemoryAllocations() == allocations);

    // another extent: one more staging texture, then kept again
    host.session->resizeHeadlessForTesting(96, 64);
    const auto resized = host.session->readbackLdr(60000ms);
    REQUIRE(resized.is_success());
    CHECK(resized.value().width == 96u);
    CHECK(resized.value().height == 64u);
    CHECK(host.session->ldrStagingCreationsForTesting() == creations + 1);
    REQUIRE(host.session->readbackLdr(10000ms).is_success());
    CHECK(host.session->ldrStagingCreationsForTesting() == creations + 1);

    // a storm: a hand-over every millisecond keeps every frame behind its counters, so no
    // frame settles and the readback times out with the reason (FEED.md)
    auto stop = std::atomic<bool>{false};
    auto handed = std::atomic<int>{0};
    auto storm = std::thread{[&] {
      auto next = std::chrono::steady_clock::now();
      for (auto i = 0; !stop; ++i)
      {
        static_cast<void>(host.service->setImageOverride(
          "textures/lp5/feed_side",
          constantOverride(
            64, 64, {std::uint8_t(i), std::uint8_t(255 - i), std::uint8_t(i * 7)})));
        ++handed;
        next += 1ms;
        while (std::chrono::steady_clock::now() < next)
        {
          std::this_thread::yield();
        }
      }
    }};
    std::this_thread::sleep_for(100ms);
    const auto stormed = host.session->readbackLdr(1500ms);
    stop = true;
    storm.join();
    REQUIRE(stormed.is_error());
    CHECK_THAT(
      std::get<Error>(stormed.error()).msg,
      Catch::Matchers::StartsWith("No settled frame within 1500 ms"));
    std::cout << "LPF.2 storm: " << handed
              << " hand-overs, readback: " << std::get<Error>(stormed.error()).msg
              << "\n";

    // the storm over, the session settles on the newest hand-over
    const auto after = host.session->readbackLdr(60000ms);
    CHECK(after.is_success());

    const auto report = host.close();
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK_FALSE(report.deviceLost);
    CHECK(report.messages.empty());
    CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);
    host.events.print("LPF.2 readback");
    host.validation.print("LPF.2 readback");
    CHECK(host.validation.errors == 0);
    CHECK(host.validation.warnings == 0);
    CHECK(host.events.count(SessionEvent::Error) == 0u);
  }

  SECTION("detach, then a stop while the parse thread publishes 100 tables")
  {
    // the first host pays the process's one-time costs; the baseline is taken after it
    {
      auto warm = Host{root, *gamePath};
      REQUIRE(warm.close().deviceReleaseCount == 0u);
    }
    const auto baseline = processCounts();
    auto host = Host{
      root,
      *gamePath,
      HostOptions{160, 120, 2, true, {}, [](const std::string_view kind) {
                    // every Feed item 2 ms longer, so that the 100 span the teardown
                    if (kind == "feed")
                    {
                      std::this_thread::sleep_for(2ms);
                    }
                  }}};
    REQUIRE(waitFor(
      [&] { return host.events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms));
    host.offer(snapshotOfMapText(slabMapText(), 2, 1) | kdl::value());
    host.session->setCamera(camera);
    REQUIRE(host.session->readbackLdr(60000ms).is_success());

    // 100 clears of names no key uses: 100 Feed items (one per name), 100 publications
    const auto before = host.service->feedRequested();
    for (auto i = 0; i < 100; ++i)
    {
      host.service->clearImageOverride(fmt::format("textures/lp5/storm_{}", i));
    }
    // the host's first teardown step and the session's own, while they publish
    host.listener->detach();
    const auto atDetach = host.listener->counts();
    host.session->requestStop();
    REQUIRE(host.session->waitForRenderThread(5000ms));
    host.session->stopRenderThread();
    host.session->releaseGpuObjects();
    host.session->destroyVkDevice();
    const auto report = host.session->teardownReport();
    host.session.reset();
    const auto atReset = host.listener->counts();

    // the parse thread goes on with no session to tell
    REQUIRE(host.service->waitForTableForTesting(
      [&](const MaterialTable& table) { return table.feedApplied >= before + 100; },
      20000ms));
    const auto after = host.listener->counts();
    CHECK(after.forwarded == atDetach.forwarded);
    CHECK(after.published - atDetach.published == after.dropped - atDetach.dropped);
    CHECK(after.published > atReset.published);
    std::cout << "LPF.2 detach: publications " << atDetach.published << " at detach, "
              << atReset.published << " at the session's end, " << after.published
              << " after the 100 (forwarded " << after.forwarded << ", dropped "
              << after.dropped << ")\n";
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);

    // the rest of the teardown: no session left, the service and the instance
    const auto rest = host.close();
    CHECK(rest.messages.empty());
#if defined(_MSC_VER) && defined(_DEBUG)
    // the CRT's debug heap: every block's guard bytes intact (a write through a freed
    // session would have hit 0xDD-filled memory)
    CHECK(_CrtCheckMemory());
#endif
    CHECK(host.validation.errors == 0);
    CHECK(host.validation.warnings == 0);
    auto counts = processCounts();
    const auto maxHandles = baseline.handles + HandleTolerance + DriverHandlesPerSession;
    CHECK(waitFor(
      [&] {
        counts = processCounts();
        return counts.threads <= baseline.threads && counts.handles <= maxHandles;
      },
      3000ms));
    std::cout << "LPF.2 detach: threads " << baseline.threads << " -> " << counts.threads
              << ", handles " << baseline.handles << " -> " << counts.handles << "\n";
  }

  SECTION("the teardown helper, 20 hosts")
  {
    {
      auto warm = Host{root, *gamePath};
      REQUIRE(warm.close().deviceReleaseCount == 0u);
    }
    const auto baseline = processCounts();
    constexpr auto cycles = 20L;
    for (auto cycle = 0L; cycle < cycles; ++cycle)
    {
      CAPTURE(cycle);
      auto host = Host{root, *gamePath, HostOptions{160, 120, std::uint64_t(10 + cycle)}};
      REQUIRE(waitFor(
        [&] { return host.events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms));
      host.offer(
        snapshotOfMapText(slabMapText(), std::uint64_t(10 + cycle), 1) | kdl::value());
      host.session->setCamera(camera);
      // a frame or two drawn, the scene still loading: the teardown meets it mid-way
      REQUIRE(waitFor([&] { return host.session->framesPresented() >= 1u; }, 10000ms));
      if (cycle % 2 == 1)
      {
        host.service->clearImageOverride("textures/lp5/feed_side");
      }
      const auto report = host.close();
      CHECK(report.deviceReleaseCount == 0u);
      CHECK(report.nvrhiErrors == 0u);
      CHECK_FALSE(report.deviceLost);
      CHECK(report.messages.empty());
      CHECK(host.events.count(SessionEvent::Error) == 0u);
      CHECK(host.validation.errors == 0);
      CHECK(host.validation.warnings == 0);
      CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);
    }
    const auto maxHandles =
      baseline.handles + HandleTolerance + cycles * DriverHandlesPerSession;
    auto counts = processCounts();
    CHECK(waitFor(
      [&] {
        counts = processCounts();
        return counts.threads <= baseline.threads && counts.handles <= maxHandles;
      },
      3000ms));
    std::cout << "LPF.2 teardown x" << cycles << ": threads " << baseline.threads
              << " -> " << counts.threads << ", handles " << baseline.handles << " -> "
              << counts.handles << " (allowed " << maxHandles << ")\n";
  }
}

} // namespace tb::lit

#endif
