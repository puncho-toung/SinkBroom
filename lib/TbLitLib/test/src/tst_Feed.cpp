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
// ("[.][feed]", its own tag, in neither "[.][litgpu]" nor "[.][litgpu2]"): it needs a
// Vulkan driver and, for the lit sections, the game folder's shader blobs and light
// entityDef (TB_LIT_GAME_PATH, read only). Windows only, as every GPU case.

#include "FrameResources.h"
#include "ImageSlots.h"
#include "ImageTestFiles.h"
#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "RenderThread.h"
#include "TonemapModel.h"
#include "lit/EngineTables.h"
#include "lit/FeedHost.h"
#include "lit/FeedScene.h"
#include "lit/GameDeclSource.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nvrhi/nvrhi.h>

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
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
  // the host's own handler of the service's events, after the guarded listener's
  MaterialListener listenerNext;
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
  std::shared_ptr<feed::GuardedListener> listener;
  std::optional<VulkanInstance> instance;
  std::unique_ptr<MaterialService> service;
  std::unique_ptr<LitSession> session;

  Host(
    const std::filesystem::path& root,
    const std::filesystem::path& gamePath,
    const HostOptions& options = {})
  {
    listener = std::make_shared<feed::GuardedListener>(options.listenerNext);
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

  // LP5.F (D77 as A41 restates it): a chunk built at the Default size while its
  // material's measure is to come is provisional, and no readback shows it
  SECTION("readbackLdr: no settled frame while a fed image's measure is held, then one")
  {
    auto host = Host{root, *gamePath};
    host.service->holdMeasuresForTesting(true);
    REQUIRE(waitFor(
      [&] { return host.events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms));
    // the override stands for a 64 x 64 file (D111)
    auto fed = constantOverride(64, 64, {40, 160, 220});
    fed.editorSize = EditorImageSize{64, 64, EditorImageSize::State::Ready, {}};
    REQUIRE(host.service->setImageOverride("textures/lp5/feed_side", std::move(fed))
              .is_success());
    host.offer(snapshotOfMapText(slabMapText(), 1, 1) | kdl::value());
    host.session->setCamera(camera);

    // the image is fed and served, the slab's size measure held: no frame settles
    const auto held = host.session->readbackLdr(3000ms);
    REQUIRE(held.is_error());
    CHECK_THAT(
      std::get<Error>(held.error()).msg,
      Catch::Matchers::StartsWith("No settled frame within 3000 ms"));
    const auto heldSlot = host.service->table()->find("textures/lp5/feed_side");
    REQUIRE(heldSlot);
    REQUIRE(heldSlot->entry);
    CHECK_FALSE(heldSlot->sizeMeasured);
    CHECK(heldSlot->entry->editorImageSize.state == EditorImageSize::State::Default);
    CHECK_FALSE(host.session->lastFrameSettledForTesting());

    // released: the override's 64 x 64 is measured (D111), the slab built again, and
    // the readback has its settled frame
    host.service->holdMeasuresForTesting(false);
    const auto measured = host.session->readbackLdr(60000ms);
    REQUIRE(measured.is_success());
    const auto slot = host.service->table()->find("textures/lp5/feed_side");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->sizeMeasured);
    CHECK(slot->entry->editorImageSize.state == EditorImageSize::State::Ready);
    CHECK(slot->entry->editorImageSize.width == 64);
    const auto centre = (std::size_t(60) * 160 + 80) * 4;
    const auto& image = measured.value();
    CHECK(int(image.rgba[centre]) + image.rgba[centre + 1] + image.rgba[centre + 2] > 0);
    std::cout << "LP5.F held measure: readback " << std::get<Error>(held.error()).msg
              << "; released: the centre " << int(image.rgba[centre]) << " "
              << int(image.rgba[centre + 1]) << " " << int(image.rgba[centre + 2])
              << "\n";

    const auto report = host.close();
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK_FALSE(report.deviceLost);
    CHECK(report.messages.empty());
    host.events.print("LP5.F held measure");
    host.validation.print("LP5.F held measure");
    CHECK(host.validation.errors == 0);
    CHECK(host.validation.warnings == 0);
    CHECK(host.events.count(SessionEvent::Error) == 0u);
  }

  // LP5.F addendum (D111): an override of another editorSize remeasures the material
  // (Ready, then unmeasured): no readback shows the old UVs under the new image
  SECTION("readbackLdr: an override's new editorSize, its remeasure held, then new UVs")
  {
    // 8 x 1 texels, 0-3 red and 4-7 blue: pure red for s in [1/16, 7/16], pure blue for
    // s in [9/16, 15/16] (bilinear, wrapped); the slab's -y face has s = x / width
    const auto stripes = [](const int editorWidth) {
      auto image = ImageOverride{};
      auto pixels = RawImagePixels{};
      pixels.width = 8;
      pixels.height = 1;
      pixels.format = ImagePixelFormat::RGBA8;
      for (auto i = 0; i < 8; ++i)
      {
        const auto red = i < 4;
        pixels.bytes.insert(
          pixels.bytes.end(),
          {std::uint8_t(red ? 255 : 0), 0, std::uint8_t(red ? 0 : 255), 255});
      }
      image.image = std::move(pixels);
      image.editorSize =
        EditorImageSize{editorWidth, editorWidth, EditorImageSize::State::Ready, {}};
      return image;
    };
    // the centre row's probe at world x: the slab spans x -256..256 over its lit columns
    const auto probe = [](const LdrImage& image, const float x) {
      const auto row = std::size_t(image.height / 2);
      auto first = -1;
      auto last = -1;
      for (auto c = 0; c < int(image.width); ++c)
      {
        const auto* p = &image.rgba[(row * image.width + std::size_t(c)) * 4];
        if (int(p[0]) + p[1] + p[2] > 0)
        {
          first = first < 0 ? c : first;
          last = c;
        }
      }
      const auto column = first + int((x + 256.0f) / 512.0f * float(last + 1 - first));
      const auto* p = &image.rgba[(row * image.width + std::size_t(column)) * 4];
      return std::array<int, 3>{int(p[0]), int(p[1]), int(p[2])};
    };
    const auto isRed = [](const std::array<int, 3>& p) { return p[0] > 4 * p[2] + 8; };
    const auto isBlue = [](const std::array<int, 3>& p) { return p[2] > 4 * p[0] + 8; };

    auto host = Host{root, *gamePath};
    REQUIRE(waitFor(
      [&] { return host.events.count(SessionEvent::ReadyForSnapshot) >= 1u; }, 5000ms));
    REQUIRE(host.service->setImageOverride("textures/lp5/feed_side", stripes(512))
              .is_success());
    host.offer(snapshotOfMapText(slabMapText(), 1, 1) | kdl::value());
    host.session->setCamera(camera);
    const auto wide = host.session->readbackLdr(60000ms);
    REQUIRE(wide.is_success());
    const auto wideSlot = host.service->table()->find("textures/lp5/feed_side");
    REQUIRE(wideSlot);
    REQUIRE(wideSlot->entry);
    CHECK(wideSlot->sizeMeasured);
    CHECK(wideSlot->entry->editorImageSize.width == 512);
    // the model at 512: x 192 at s 0.375 (red), x -192 at s 0.625 (blue)
    CHECK(isRed(probe(wide.value(), 192.0f)));
    CHECK(isBlue(probe(wide.value(), -192.0f)));

    // 256 with the measures held: the slot is published again unmeasured at 512
    host.service->holdMeasuresForTesting(true);
    REQUIRE(host.service->setImageOverride("textures/lp5/feed_side", stripes(256))
              .is_success());
    const auto held = host.session->readbackLdr(3000ms);
    CHECK(held.is_error());
    const auto heldSlot = host.service->table()->find("textures/lp5/feed_side");
    REQUIRE(heldSlot);
    REQUIRE(heldSlot->entry);
    CHECK_FALSE(heldSlot->sizeMeasured);
    CHECK(heldSlot->entry->editorImageSize.width == 512);
    CHECK_FALSE(host.session->lastFrameSettledForTesting());
    if (held.is_success())
    {
      const auto p = probe(held.value(), 192.0f);
      std::cout << "LP5.F remeasure held: a settled frame at x 192 (" << p[0] << " "
                << p[1] << " " << p[2] << "), the old UVs\n";
    }

    // released: measured at 256, built again: x 192 at s 0.75 (blue), x -192 at s 0.25
    // (red)
    host.service->holdMeasuresForTesting(false);
    const auto narrow = host.session->readbackLdr(60000ms);
    REQUIRE(narrow.is_success());
    const auto slot = host.service->table()->find("textures/lp5/feed_side");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->sizeMeasured);
    CHECK(slot->entry->editorImageSize.width == 256);
    const auto right = probe(narrow.value(), 192.0f);
    const auto left = probe(narrow.value(), -192.0f);
    CHECK(isBlue(right));
    CHECK(isRed(left));
    std::cout << "LP5.F remeasure: at 512 x 192 "
              << fmt::format("{}", probe(wide.value(), 192.0f)) << ", x -192 "
              << fmt::format("{}", probe(wide.value(), -192.0f))
              << "; held: " << (held.is_error() ? "no settled frame" : "settled")
              << "; at 256 x 192 " << fmt::format("{}", right) << ", x -192 "
              << fmt::format("{}", left) << "\n";

    const auto report = host.close();
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK_FALSE(report.deviceLost);
    CHECK(report.messages.empty());
    host.validation.print("LP5.F remeasure");
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

// ----- LPF.2 step 5 (D110, P5.10, P5.13): the fidelity of a fed image and a re-parse,
// the latency, a stop mid-feed -----

namespace
{

// the feed scene's materials and image names (test/fixture/lit/materials/lp5_feed.mtr)
constexpr auto QuadMaterial = "textures/lp5/feed_pbr";
constexpr auto SphereMaterial = "textures/lp5/feed_sphere";
constexpr auto QuadDiffuse = "textures/lp5/feed_pbr";
constexpr auto QuadBump = "textures/lp5/feed_pbr_local";
constexpr auto QuadRmao = "textures/lp5/feed_rmao";
constexpr auto SphereRmao = "textures/lp5/feed_sphere_rmao";

// stageLighting_t (Material.h): the stage kinds of the quad's three images
constexpr auto LightingBump = 1;
constexpr auto LightingDiffuse = 2;
constexpr auto LightingSpecular = 3;

constexpr auto FeedSide = 1024;
// the screen (160 × 120) and its centre pixel, where the quad's centre is (D110's probe)
constexpr auto ProbeX = std::uint32_t{80};
constexpr auto ProbeY = std::uint32_t{60};

/**
 * The scene of the [.][feed] fidelity sections: worldspawn, the 1024-unit quad facing -y
 * (its texture's centre on the quad's, the editor size 1024 giving one texel a unit), the
 * sphere before it, one white point light; the camera on a 3/4 view of the quad's
 * centre.
 */
Snapshot feedScene(const std::uint64_t generation)
{
  auto snapshot = Snapshot{};
  snapshot.generation = generation;
  snapshot.sequence = 1;
  feed::worldspawn(snapshot, 1);
  REQUIRE(feed::quadBrush(
            snapshot,
            1,
            2,
            {0, 0, 0},
            {0, -1, 0},
            512,
            "lp5/feed_pbr",
            "lp5/feed_side",
            1.0,
            {512, 512})
            .is_success());
  REQUIRE(feed::patchDef3Sphere(snapshot, 1, 3, {300, -300, -200}, 128, "lp5/feed_sphere")
            .is_success());
  feed::pointLight(snapshot, 4, {0, -500, 0}, 1200, {1, 1, 1});
  return snapshot;
}

CameraState feedCamera()
{
  return feed::cameraLookingAt({-600, -900, 400}, {0, 0, 0});
}

// RGBA8 pixels, rows top first (C39)
struct Pixels
{
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;

  std::array<std::uint8_t, 4> at(const int x, const int y) const
  {
    const auto i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
  }
};

template <typename F>
Pixels generated(const int side, const F& texel)
{
  auto pixels =
    Pixels{side, side, std::vector<std::uint8_t>(std::size_t(side) * side * 4)};
  for (auto y = 0; y < side; ++y)
  {
    for (auto x = 0; x < side; ++x)
    {
      const auto t = texel(x, y);
      std::copy(
        t.begin(), t.end(), pixels.rgba.begin() + (std::ptrdiff_t(y) * side + x) * 4);
    }
  }
  return pixels;
}

// the inner half of the picture, which every pixel near the screen's centre samples
bool onPlateau(const int x, const int y)
{
  return std::abs(x - FeedSide / 2) < FeedSide / 4
         && std::abs(y - FeedSide / 2) < FeedSide / 4;
}

// a colour whose stored YCoCg texel every mip level shares (200 120 40 does not: its Co
// 208 becomes 207 below level 0, see centreTexels)
constexpr auto PlateauColour = std::array<std::uint8_t, 4>{200, 120, 60, 255};

// the diffuse: a plateau of PlateauColour; around it a red top half and a blue bottom
// half with a horizontal green gradient (so that the rows' order shows on the screen)
Pixels diffusePixels()
{
  return generated(FeedSide, [](const int x, const int y) {
    if (onPlateau(x, y))
    {
      return PlateauColour;
    }
    const auto green = std::uint8_t(60 + x / 8);
    return y < FeedSide / 2 ? std::array<std::uint8_t, 4>{230, green, 30, 255}
                            : std::array<std::uint8_t, 4>{30, green, 230, 255};
  });
}

// the bump: a tangent-space normal tilted by (0.25, 0) on the plateau, waving around it
Pixels bumpPixels()
{
  return generated(FeedSide, [](const int x, const int y) {
    auto sx = 0.25;
    auto sy = 0.0;
    if (!onPlateau(x, y))
    {
      sx += 0.15 * std::sin(double(x) / 40.0);
      sy = 0.15 * std::cos(double(y) / 40.0);
    }
    const auto length = std::sqrt(sx * sx + sy * sy + 1.0);
    const auto encode = [](const double v) {
      return std::uint8_t(std::lround((v * 0.5 + 0.5) * 255.0));
    };
    return std::array<std::uint8_t, 4>{
      encode(sx / length), encode(sy / length), encode(1.0 / length), 255};
  });
}

// the RMAO: roughness 0.6, metal 0, occlusion 1 (R, G, B)
Pixels rmaoPixels(const std::uint8_t metal = 0, const int side = FeedSide)
{
  return generated(side, [metal](int, int) {
    return std::array<std::uint8_t, 4>{153, metal, 255, 255};
  });
}

Pixels constantPixels(const std::array<std::uint8_t, 3>& rgb, const int side = FeedSide)
{
  return generated(side, [rgb](int, int) {
    return std::array<std::uint8_t, 4>{rgb[0], rgb[1], rgb[2], 255};
  });
}

ImageOverride overrideOf(const Pixels& pixels, const bool editorSize = true)
{
  auto raw = RawImagePixels{};
  raw.width = pixels.width;
  raw.height = pixels.height;
  raw.format = ImagePixelFormat::RGBA8;
  raw.bytes = pixels.rgba;
  auto image = ImageOverride{};
  image.image = std::move(raw);
  if (editorSize)
  {
    image.editorSize =
      EditorImageSize{pixels.width, pixels.height, EditorImageSize::State::Ready, {}};
  }
  return image;
}

/**
 * D110 (P5.10): the same pixels as a loose TGA: type 2, 32 bits, no flip bit (attribute
 * bit 5 clear), rows bottom first and each texel B, G, R, A. The copied LoadTGA
 * (Image_files.cpp:259-302) fills its picture from its last row up (row = rows - 1 down
 * to 0) and turns BGRA into RGBA, and flips only when bit 5 is set (:414-420), so it
 * returns exactly the caller's top-first RGBA rows.
 */
std::string tgaBytes(const Pixels& pixels)
{
  auto out = std::string(18, '\0');
  out[2] = 2;
  out[12] = char(pixels.width & 0xff);
  out[13] = char(pixels.width >> 8);
  out[14] = char(pixels.height & 0xff);
  out[15] = char(pixels.height >> 8);
  out[16] = 32;
  out[17] = 0;
  out.reserve(out.size() + pixels.rgba.size());
  for (auto y = pixels.height - 1; y >= 0; --y)
  {
    for (auto x = 0; x < pixels.width; ++x)
    {
      const auto t = pixels.at(x, y);
      out += char(t[2]);
      out += char(t[1]);
      out += char(t[0]);
      out += char(t[3]);
    }
  }
  return out;
}

// the image a material's stage of the given lighting binds, from the table
std::optional<MaterialImageKey> stageImage(
  MaterialService& service, const std::string& material, const int lighting)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (const auto slot = service.table()->find(material); slot && slot->entry)
    {
      for (const auto& stage : slot->entry->stages)
      {
        if (stage.lighting == lighting && stage.image)
        {
          return stage.image;
        }
      }
      return std::nullopt;
    }
    static_cast<void>(service.lookup(material));
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

/**
 * The centre texel of every level of key's image as the service converted it, down to
 * the levels the plateau still covers (8 texels a side): the GPU samples one of them, and
 * the engine's gamma mips (R_MipMapWithGamma) may move a constant colour's stored chroma
 * by one step below level 0 (an LPF.2 finding), so the model holds only for a colour
 * whose texel all of them share.
 */
std::vector<std::array<std::uint8_t, 4>> centreTexels(
  MaterialService& service, const MaterialImageKey& key)
{
  auto texels = std::vector<std::array<std::uint8_t, 4>>{};
  const auto lookup = service.image(key);
  if (!lookup.image)
  {
    return texels;
  }
  for (const auto& level : lookup.image->levels)
  {
    if (level.width < 8 || level.height < 8)
    {
      break;
    }
    const auto* t = lookup.image->data.data() + level.offset
                    + (std::size_t(level.height / 2) * std::size_t(level.width)
                       + std::size_t(level.width / 2))
                        * 4;
    texels.push_back({t[0], t[1], t[2], t[3]});
  }
  return texels;
}

// ----- the CPU model of the probe pixel (D110), as LPF.1's -----

// global_inc.hlsl:177-185 Linear1, :205-214 sRGBToLinearRGB (clamped first)
float modelLinear1(const float c)
{
  const auto v = std::clamp(c, 0.0f, 1.0f);
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// global_inc.hlsl:344-361 ConvertYCoCgToRGB of a stored texel (Co, Cg, scale, Y) as the
// UNORM8 sample returns it, then sRGBToLinearRGB: the shaders' diffuse map
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

template <typename Hook>
bool inFrame(
  LitSession& session, Hook hook_, const std::chrono::milliseconds timeout = 10s)
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

// one pixel of a settled frame's HDR target, its exposure and the tonemap's constants
struct HdrProbe
{
  float exposure = 0.0f;
  std::array<float, 3> hdr = {};
  EngineToneMappingConstants constants;
};

std::optional<HdrProbe> hdrProbe(
  LitSession& session, const std::uint32_t x, const std::uint32_t y)
{
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline)
  {
    struct State
    {
      bool copied = false;
      EngineToneMappingConstants constants;
      nvrhi::BufferHandle exposure;
      nvrhi::StagingTextureHandle hdr;
    };
    auto state = std::make_shared<State>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    session.runAfterTonemapForTesting([state, done](RenderFrameAccess& access) {
      const auto& tonemap = access.tonemap.lastStats();
      if (access.convergence.settled() && tonemap.tonemapped)
      {
        namespace fr = frame_resource;
        auto bufferDesc = nvrhi::BufferDesc{};
        bufferDesc.byteSize = 4;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        bufferDesc.debugName = "LPF.2 exposure readback";
        bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
        bufferDesc.keepInitialState = true;
        state->exposure = access.device.createBuffer(bufferDesc);
        access.commandList.copyBuffer(
          state->exposure, 0, access.frame.find(fr::ExposureBuffer)->buffer, 0, 4);
        auto* texture = access.frame.find(fr::CurrentRenderHdr)->texture;
        const auto& desc = texture->getDesc();
        state->hdr = access.device.createStagingTexture(
          nvrhi::TextureDesc{}
            .setWidth(desc.width)
            .setHeight(desc.height)
            .setFormat(desc.format),
          nvrhi::CpuAccessMode::Read);
        access.commandList.copyTexture(
          state->hdr, nvrhi::TextureSlice{}, texture, nvrhi::TextureSlice{});
        state->constants = tonemap.tonemapConstants;
        state->copied = true;
      }
      done->set_value();
    });
    if (future.wait_for(10s) != std::future_status::ready)
    {
      return std::nullopt;
    }
    if (!state->copied)
    {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    auto probe = HdrProbe{};
    probe.constants = state->constants;
    const auto read = inFrame(session, [&](RenderFrameAccess& access) {
      if (
        const auto* bits = static_cast<const std::uint32_t*>(
          access.device.mapBuffer(state->exposure, nvrhi::CpuAccessMode::Read)))
      {
        std::memcpy(&probe.exposure, bits, sizeof(float));
        access.device.unmapBuffer(state->exposure);
      }
      auto pitch = std::size_t{0};
      if (
        const auto* bytes =
          static_cast<const std::uint8_t*>(access.device.mapStagingTexture(
            state->hdr, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch)))
      {
        const auto* row = reinterpret_cast<const std::uint16_t*>(bytes + y * pitch);
        for (std::size_t c = 0; c < 3; ++c)
        {
          probe.hdr[c] = halfToFloat(row[std::size_t(x) * 4 + c]);
        }
        access.device.unmapStagingTexture(state->hdr);
      }
      state->exposure = nullptr;
      state->hdr = nullptr;
    });
    if (!read)
    {
      return std::nullopt;
    }
    return probe;
  }
  return std::nullopt;
}

// the model's HDR value through the RGBA16F target and the tonemap passes, as LDR bytes
std::array<int, 3> modelLdr(
  const std::array<float, 3>& hdr,
  const float exposure,
  const EngineToneMappingConstants& constants)
{
  const auto stored =
    std::array<float, 3>{modelHalf(hdr[0]), modelHalf(hdr[1]), modelHalf(hdr[2])};
  const auto ldr = modelTonemap(stored, exposure, constants);
  return {int(modelUnorm8(ldr[0])), int(modelUnorm8(ldr[1])), int(modelUnorm8(ldr[2]))};
}

// the first differing pixel of two pictures, and how many differ
std::string pictureDifference(const LdrImage& a, const LdrImage& b)
{
  if (a.width != b.width || a.height != b.height)
  {
    return fmt::format("sizes {}x{} and {}x{}", a.width, a.height, b.width, b.height);
  }
  auto first = std::optional<std::size_t>{};
  auto count = std::size_t{0};
  for (std::size_t i = 0; i < a.rgba.size(); i += 4)
  {
    if (!std::equal(
          a.rgba.begin() + std::ptrdiff_t(i),
          a.rgba.begin() + std::ptrdiff_t(i) + 4,
          b.rgba.begin() + std::ptrdiff_t(i)))
    {
      first = first.value_or(i / 4);
      ++count;
    }
  }
  if (!first)
  {
    return "equal";
  }
  const auto i = *first * 4;
  return fmt::format(
    "{} pixels differ, the first at ({} {}): ({} {} {} {}) and ({} {} {} {})",
    count,
    *first % a.width,
    *first / a.width,
    a.rgba[i],
    a.rgba[i + 1],
    a.rgba[i + 2],
    a.rgba[i + 3],
    b.rgba[i],
    b.rgba[i + 1],
    b.rgba[i + 2],
    b.rgba[i + 3]);
}

// a picture into the temp folder as a PNG, for a look by eye
void savePicture(const LdrImage& image, const std::string& name)
{
  auto rgba = test::Rgba{int(image.width), int(image.height), image.rgba};
  for (std::size_t i = 3; i < rgba.pixels.size(); i += 4)
  {
    rgba.pixels[i] = 255;
  }
  writeBinaryFile(
    std::filesystem::temp_directory_path() / "TbLitLibTest-feed-pictures"
      / (name + ".png"),
    test::pngFile(rgba));
}

std::array<int, 3> ldrAt(
  const LdrImage& image, const std::uint32_t x, const std::uint32_t y)
{
  const auto i = (std::size_t(y) * image.width + x) * 4;
  return {int(image.rgba[i]), int(image.rgba[i + 1]), int(image.rgba[i + 2])};
}

// waits until the latest table holds every hand-over made so far
bool waitForFeed(MaterialService& service)
{
  const auto requested = service.feedRequested();
  return service.waitForTableForTesting(
           [&](const MaterialTable& table) { return table.feedApplied >= requested; },
           20s)
         != nullptr;
}

// the parse thread's work items as they start and its publications, in order
struct ParseLog
{
  std::mutex mutex;
  // "feed", "reparse", "lookup", ..., or "published"
  std::vector<std::string> entries;

  void add(std::string entry)
  {
    const auto lock = std::scoped_lock{mutex};
    entries.push_back(std::move(entry));
  }

  std::size_t size()
  {
    const auto lock = std::scoped_lock{mutex};
    return entries.size();
  }

  // the publications of the first item of kind at or after from, until the next item
  std::optional<std::size_t> publicationsOfItem(
    const std::size_t from, const std::string& kind)
  {
    const auto lock = std::scoped_lock{mutex};
    for (auto i = from; i < entries.size(); ++i)
    {
      if (entries[i] == kind)
      {
        auto count = std::size_t{0};
        for (auto j = i + 1; j < entries.size() && entries[j] == "published"; ++j)
        {
          ++count;
        }
        return count;
      }
    }
    return std::nullopt;
  }
};

// what a frame's start bound for a key's slot (LPF.1's binding record, P5.23)
struct BoundFrame
{
  std::uint64_t index = 0;
  std::chrono::steady_clock::time_point start;
  bool slotted = false;
  bool intrinsic = false;
  ImageSlots::State state = ImageSlots::State::Pending;
};

struct BoundFrames
{
  std::mutex mutex;
  std::map<std::string, std::vector<BoundFrame>> frames;

  std::vector<BoundFrame> of(const std::string& name)
  {
    const auto lock = std::scoped_lock{mutex};
    return frames[name];
  }
};

void recordBindings(
  LitSession& session,
  const std::shared_ptr<BoundFrames>& record,
  const std::vector<MaterialImageKey>& keys)
{
  session.setEveryFrameHookForTesting([record, keys](RenderFrameAccess& access) {
    const auto start = std::chrono::steady_clock::now();
    const auto lock = std::scoped_lock{record->mutex};
    for (const auto& key : keys)
    {
      auto frame = BoundFrame{};
      frame.index = access.frameIndex;
      frame.start = start;
      if (const auto id = access.images.find(key))
      {
        frame.slotted = true;
        frame.intrinsic = access.images.binding(*id).intrinsic;
        frame.state = access.images.slot(*id)->state;
      }
      record->frames[key.name].push_back(frame);
    }
  });
}

/**
 * The feed's own temp roots: assembleFeedRoot's, and with the quad's three images as
 * loose TGAs (D110's control: the same pixels the overrides hand over); sphereText, when
 * given, replaces lp5/feed_sphere's decl in the copy of lp5_feed.mtr.
 */
std::filesystem::path assembleFileRoot(
  const std::string& name,
  const std::filesystem::path& gamePath,
  const std::optional<std::string>& sphereText = std::nullopt)
{
  const auto root = assembleFeedRoot(name, gamePath);
  const auto textures = root / "base" / "textures" / "lp5";
  writeBinaryFile(textures / "feed_pbr.tga", tgaBytes(diffusePixels()));
  writeBinaryFile(textures / "feed_pbr_local.tga", tgaBytes(bumpPixels()));
  writeBinaryFile(textures / "feed_rmao.tga", tgaBytes(rmaoPixels()));
  if (sphereText)
  {
    const auto mtr = root / "base" / "materials" / "lp5_feed.mtr";
    auto text = readTextFile(mtr);
    const auto start = text.find("textures/lp5/feed_sphere\n");
    REQUIRE(start != std::string::npos);
    const auto end = text.find("\n}\n", start);
    REQUIRE(end != std::string::npos);
    text.replace(start, end + 3 - start, *sphereText);
    writeTextFile(mtr, text);
  }
  return root;
}

// (b): the sphere's text with twoSided, every stage as the fixture's
const auto TwoSidedSphereText = std::string{
  "textures/lp5/feed_sphere\n{\n\ttwoSided\n\tqer_editorimage\ttextures/lp5/feed_sphere\n"
  "\tbasecolormap\ttextures/lp5/feed_sphere\n\tbumpmap\ttextures/lp5/feed_sphere_local\n"
  "\tspecularmap\ttextures/lp5/feed_sphere_rmao\n}\n"};

// a session's picture of the feed scene from its files (the control sessions of D110)
LdrImage controlPicture(
  const std::filesystem::path& root,
  const std::filesystem::path& gamePath,
  const std::uint64_t generation,
  const std::function<void(Host&)>& feedFirst = {})
{
  auto host = Host{root, gamePath, HostOptions{160, 120, generation}};
  if (feedFirst)
  {
    feedFirst(host);
  }
  host.offer(feedScene(generation));
  host.session->setCamera(feedCamera());
  auto picture = host.session->readbackLdr(60s);
  REQUIRE(picture.is_success());
  const auto report = host.close();
  CHECK(report.deviceReleaseCount == 0u);
  CHECK(report.nvrhiErrors == 0u);
  CHECK(host.validation.errors == 0);
  CHECK(host.validation.warnings == 0);
  return std::move(picture).value();
}

} // namespace

/**
 * D110, P5.10 (LPF.2 step 5 (a), (b), (d)): a headless host with no document feeds the
 * quad's three images and re-parses the sphere's material, under a pinned exposure; every
 * picture is compared with a control: the intrinsics before, the same pixels as loose
 * TGAs in a fresh session, a CPU model of the probe pixel, the re-parse's text in a fresh
 * session's .mtr; then every feed is cleared back to the first picture.
 */
TEST_CASE(
  "Feed host: a fed image and a re-parse against their files (LPF.2, D110)", "[.][feed]")
{
  const auto gamePath = feedGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH does not name a game folder with blobs and def/misc.def");
  }
  const auto rootA = assembleFeedRoot("TbLitLibTest-feed-a", *gamePath);
  auto log = std::make_shared<ParseLog>();
  auto pictureA = LdrImage{};
  auto pictureC = LdrImage{};

  {
    auto host = Host{
      rootA,
      *gamePath,
      HostOptions{
        160,
        120,
        1,
        true,
        {},
        [log](const std::string_view kind) { log->add(std::string{kind}); },
        [log](const MaterialServiceEvent& event) {
          if (event.kind == MaterialServiceEvent::Kind::Published)
          {
            log->add("published");
          }
        }}};
    auto& service = *host.service;
    auto& session = *host.session;
    host.offer(feedScene(1));
    session.setCamera(feedCamera());

    // the first picture: every feed image is missing, so its intrinsic stands in
    const auto first = session.readbackLdr(60s);
    REQUIRE(first.is_success());
    const auto& picture0 = first.value();
    savePicture(picture0, "0-intrinsics");
    const auto diffuseKey = stageImage(service, QuadMaterial, LightingDiffuse);
    const auto bumpKey = stageImage(service, QuadMaterial, LightingBump);
    const auto rmaoKey = stageImage(service, QuadMaterial, LightingSpecular);
    const auto sphereRmaoKey = stageImage(service, SphereMaterial, LightingSpecular);
    REQUIRE(diffuseKey);
    REQUIRE(bumpKey);
    REQUIRE(rmaoKey);
    REQUIRE(sphereRmaoKey);
    CHECK(diffuseKey->name == QuadDiffuse);
    CHECK(bumpKey->name == QuadBump);
    CHECK(rmaoKey->name == QuadRmao);
    // the _rmao name gives the PBR RMAO usage without a sibling file (C39)
    CHECK(rmaoKey->usage == 12);
    CHECK(sphereRmaoKey->name == SphereRmao);
    auto bound = std::make_shared<BoundFrames>();
    recordBindings(session, bound, {*diffuseKey, *bumpKey, *rmaoKey});
    session.requestFrame();
    REQUIRE(waitFor([&] { return !bound->of(QuadDiffuse).empty(); }, 10000ms));
    for (const auto* name : {QuadDiffuse, QuadBump, QuadRmao})
    {
      CAPTURE(name);
      const auto frames = bound->of(name);
      REQUIRE_FALSE(frames.empty());
      CHECK(frames.back().slotted);
      CHECK(frames.back().intrinsic);
      CHECK(frames.back().state == ImageSlots::State::Default);
      CHECK(service.imageOverrideStatus(name).state == ImageOverrideStatus::State::None);
    }

    // (a) the quad's bump and RMAO, then three diffuse pictures: black and white for the
    // model's two terms, then the generated picture
    REQUIRE(service.setImageOverride(QuadBump, overrideOf(bumpPixels())).is_success());
    REQUIRE(service.setImageOverride(QuadRmao, overrideOf(rmaoPixels())).is_success());
    const auto probeWith = [&](const Pixels& diffuse) {
      REQUIRE(service.setImageOverride(QuadDiffuse, overrideOf(diffuse)).is_success());
      REQUIRE(waitForFeed(service));
      auto picture = session.readbackLdr(60s);
      REQUIRE(picture.is_success());
      const auto probe = hdrProbe(session, ProbeX, ProbeY);
      REQUIRE(probe);
      const auto texels = centreTexels(service, *diffuseKey);
      REQUIRE(texels.size() >= 7u);
      for (const auto& texel : texels)
      {
        CHECK(texel == texels.front());
      }
      return std::tuple{
        std::move(picture).value(), *probe, modelDiffuseMap(texels.front())};
    };
    const auto [blackPicture, black, blackLinear] = probeWith(constantPixels({0, 0, 0}));
    const auto [whitePicture, white, whiteLinear] =
      probeWith(constantPixels({255, 255, 255}));
    const auto [fedPicture, fed, fedLinear] = probeWith(diffusePixels());
    pictureA = fedPicture;
    savePicture(pictureA, "a-fed");
    for (const auto* name : {QuadDiffuse, QuadBump, QuadRmao})
    {
      CAPTURE(name);
      CHECK(
        service.imageOverrideStatus(name).state == ImageOverrideStatus::State::Applied);
    }

    // A differs from the first picture
    CHECK(pictureA.rgba != picture0.rgba);
    std::cout << "LPF.2 (a): A against the intrinsics: "
              << pictureDifference(pictureA, picture0) << "\n";

    // the probe pixel's model: with the exposure pinned the HDR value is linear in the
    // diffuse map's linear colour (the light's terms do not depend on it): the black and
    // white overrides measure the two terms, and the plateau's colour is predicted
    // through the texel the service converted (the engine's YCoCg layout, decoded as the
    // shader decodes it), then through the tonemap model to the LDR bytes
    CHECK(fed.exposure == Catch::Approx(FeedPinnedLuminance).epsilon(1e-5));
    auto predicted = std::array<float, 3>{};
    for (std::size_t c = 0; c < 3; ++c)
    {
      CAPTURE(c);
      const auto span = whiteLinear[c] - blackLinear[c];
      REQUIRE(span > 0.5f);
      const auto term = (white.hdr[c] - black.hdr[c]) / span;
      predicted[c] = black.hdr[c] + term * (fedLinear[c] - blackLinear[c]);
      CHECK(fed.hdr[c] == Catch::Approx(predicted[c]).epsilon(0.01).margin(2e-4));
    }
    // the converted texel is the fed colour's within the YCoCg layout's rounding
    for (std::size_t c = 0; c < 3; ++c)
    {
      CHECK(
        fedLinear[c]
        == Catch::Approx(modelLinear1(float(PlateauColour[c]) / 255.0f)).margin(0.01));
    }
    const auto expected = modelLdr(predicted, fed.exposure, fed.constants);
    const auto centre = ldrAt(pictureA, ProbeX, ProbeY);
    for (std::size_t c = 0; c < 3; ++c)
    {
      CAPTURE(c);
      CHECK(std::abs(centre[c] - expected[c]) <= 1);
    }
    std::cout << fmt::format(
      "LPF.2 (a) probe: fed ({} {} {}) linear ({:.5f} {:.5f} {:.5f}); HDR ({:.5f} {:.5f} "
      "{:.5f}) model ({:.5f} {:.5f} {:.5f}); LDR ({} {} {}) model ({} {} {})\n",
      PlateauColour[0],
      PlateauColour[1],
      PlateauColour[2],
      fedLinear[0],
      fedLinear[1],
      fedLinear[2],
      fed.hdr[0],
      fed.hdr[1],
      fed.hdr[2],
      predicted[0],
      predicted[1],
      predicted[2],
      centre[0],
      centre[1],
      centre[2],
      expected[0],
      expected[1],
      expected[2]);
    // the rows' order on the screen: the picture's top half (red) above the centre, its
    // bottom half (blue) below
    const auto above = ldrAt(pictureA, ProbeX, ProbeY - 25);
    const auto below = ldrAt(pictureA, ProbeX, ProbeY + 22);
    CHECK(above[0] > above[2]);
    CHECK(below[2] > below[0]);
    std::cout << fmt::format(
      "LPF.2 (a) rows: above ({} {} {}), below ({} {} {})\n",
      above[0],
      above[1],
      above[2],
      below[0],
      below[1],
      below[2]);

    // (b) the sphere's material re-parsed (an existing name) with twoSided, and its RMAO
    // fed at metal 1
    REQUIRE(service.reparseMaterial(SphereMaterial, TwoSidedSphereText).is_success());
    REQUIRE(service.setImageOverride(SphereRmao, overrideOf(rmaoPixels(255, 64), false))
              .is_success());
    REQUIRE(waitForFeed(service));
    auto c = session.readbackLdr(60s);
    REQUIRE(c.is_success());
    pictureC = std::move(c).value();
    savePicture(pictureC, "c-reparsed");
    CHECK(pictureC.rgba != pictureA.rgba);
    std::cout << "LPF.2 (b): C against A: " << pictureDifference(pictureC, pictureA)
              << "\n";

    // (d) the three quad overrides cleared one by one: one publication each (the drop and
    // the files' answer in one table, C40), and no frame binds a Pending slot of the name
    for (const auto* name : {QuadDiffuse, QuadBump, QuadRmao})
    {
      CAPTURE(name);
      REQUIRE(service.waitForStatsForTesting([](const auto& s) { return s.idle; }, 20s));
      const auto from = log->size();
      const auto clearedAt = std::chrono::steady_clock::now();
      service.clearImageOverride(name);
      REQUIRE(waitForFeed(service));
      REQUIRE(session.readbackLdr(60s).is_success());
      REQUIRE(service.waitForStatsForTesting([](const auto& s) { return s.idle; }, 20s));
      CHECK(log->publicationsOfItem(from, "feed") == std::optional<std::size_t>{1});
      auto frames = bound->of(name);
      std::erase_if(frames, [&](const auto& f) { return f.start < clearedAt; });
      CHECK_FALSE(frames.empty());
      CHECK(std::ranges::none_of(
        frames, [](const auto& f) { return f.state == ImageSlots::State::Pending; }));
      CHECK(frames.back().intrinsic);
      CHECK(service.imageOverrideStatus(name).state == ImageOverrideStatus::State::None);
    }
    // the sphere's RMAO cleared and its text restored: the first picture again
    service.clearImageOverride(SphereRmao);
    service.clearMaterialReparse(SphereMaterial);
    REQUIRE(waitForFeed(service));
    const auto restored = session.readbackLdr(60s);
    REQUIRE(restored.is_success());
    savePicture(restored.value(), "d-restored");
    CHECK(restored.value().rgba == picture0.rgba);
    std::cout << "LPF.2 (d): the cleared picture against the intrinsics: "
              << pictureDifference(restored.value(), picture0) << "\n";

    session.setEveryFrameHookForTesting({});
    const auto report = host.close();
    CHECK(report.deviceReleaseCount == 0u);
    CHECK(report.nvrhiErrors == 0u);
    CHECK(report.messages.empty());
    host.events.print("LPF.2 fidelity");
    host.validation.print("LPF.2 fidelity");
    CHECK(host.validation.errors == 0);
    CHECK(host.validation.warnings == 0);
    CHECK(host.events.count(SessionEvent::Error) == 0u);
  }

  // (a)'s control: the same pixels as loose TGAs in a fresh session's game root
  const auto pictureB =
    controlPicture(assembleFileRoot("TbLitLibTest-feed-b", *gamePath), *gamePath, 2);
  savePicture(pictureB, "b-files");
  std::cout << "LPF.2 (a): A against B (the files): "
            << pictureDifference(pictureA, pictureB) << "\n";
  CHECK(pictureA.rgba == pictureB.rgba);

  // (b)'s control: the re-parse's text in the fresh session's .mtr, the same RMAO fed
  const auto pictureD = controlPicture(
    assembleFileRoot("TbLitLibTest-feed-d", *gamePath, TwoSidedSphereText),
    *gamePath,
    3,
    [](Host& host) {
      REQUIRE(
        host.service->setImageOverride(SphereRmao, overrideOf(rmaoPixels(255, 64), false))
          .is_success());
    });
  savePicture(pictureD, "d-files");
  std::cout << "LPF.2 (b): C against D (the .mtr): "
            << pictureDifference(pictureC, pictureD) << "\n";
  CHECK(pictureC.rgba == pictureD.rgba);
  CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);
}

// LPF.2 step 5 (e): a stop while overrides and a re-parse are queued and converting
TEST_CASE("Feed host: a stop mid-feed (LPF.2)", "[.][feed]")
{
  const auto gamePath = feedGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH does not name a game folder with blobs and def/misc.def");
  }
  const auto root = assembleFeedRoot("TbLitLibTest-feed-e", *gamePath);
  auto host = Host{root, *gamePath, HostOptions{160, 120, 5}};
  host.offer(feedScene(5));
  host.session->setCamera(feedCamera());
  REQUIRE(host.session->readbackLdr(60s).is_success());

  const auto diffuse = diffusePixels();
  auto stop = std::atomic<bool>{false};
  auto feeder = std::thread{[&] {
    for (auto i = 0; !stop; ++i)
    {
      auto pixels = diffuse;
      pixels.rgba[0] = std::uint8_t(i);
      static_cast<void>(host.service->setImageOverride(QuadDiffuse, overrideOf(pixels)));
      static_cast<void>(
        host.service->setImageOverride(QuadBump, overrideOf(bumpPixels())));
      std::this_thread::sleep_for(5ms);
    }
  }};
  std::this_thread::sleep_for(300ms);
  stop = true;
  feeder.join();
  // the last hand-overs right before the stop: a 1024² conversion each, and a re-parse
  static_cast<void>(host.service->setImageOverride(QuadDiffuse, overrideOf(diffuse)));
  static_cast<void>(host.service->setImageOverride(QuadRmao, overrideOf(rmaoPixels(90))));
  static_cast<void>(host.service->reparseMaterial(SphereMaterial, TwoSidedSphereText));
  const auto requested = host.service->feedRequested();
  const auto applied = host.service->table()->feedApplied;
  const auto report = host.close();
  std::cout << "LPF.2 (e): stopped with " << requested - applied
            << " hand-overs not yet in a table\n";
  CHECK(report.deviceReleaseCount == 0u);
  CHECK(report.nvrhiErrors == 0u);
  CHECK_FALSE(report.deviceLost);
  CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);
  CHECK(host.validation.errors == 0);
  CHECK(host.validation.warnings == 0);
}

/**
 * LPF.2 step 5 (c) (C44, Q29, P5.13): feed_ms from the frame log over 100 overrides of
 * the quad's 1024² diffuse at 100 ms intervals, each of a colour of its own, after three
 * to warm up; then five with a 1024² PNG source decode queued just before (the item in
 * flight: printed, not asserted). Release only: the exe runs alone on the machine.
 */
TEST_CASE("Feed host: the latency of a fed image (LPF.2, Q29)", "[.][feed]")
{
#ifndef NDEBUG
  SKIP("the latency is measured in Release only (Q29)");
#else
  const auto gamePath = feedGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH does not name a game folder with blobs and def/misc.def");
  }
  constexpr auto Warm = 3;
  constexpr auto Samples = 100;
  constexpr auto Decodes = 5;
  const auto root = assembleFeedRoot("TbLitLibTest-feed-latency", *gamePath);
  for (auto i = 0; i < Decodes; ++i)
  {
    auto source = test::gradient(FeedSide, FeedSide);
    source.pixels[0] = std::uint8_t(i);
    writeBinaryFile(
      root / "base" / "textures" / "lp5" / fmt::format("feed_decode_{}.png", i),
      test::pngFile(source));
  }
  const auto logPath =
    std::filesystem::temp_directory_path() / "TbLitLibTest-feed-latency.csv";
  std::filesystem::remove(logPath);
  {
    auto host = Host{root, *gamePath, HostOptions{160, 120, 6, false, logPath}};
    host.offer(feedScene(6));
    host.session->setCamera(feedCamera());
    REQUIRE(host.session->readbackLdr(60s).is_success());
    const auto diffuseKey = stageImage(*host.service, QuadMaterial, LightingDiffuse);
    REQUIRE(diffuseKey);

    const auto colourOf = [](const int i) {
      return std::array<std::uint8_t, 3>{
        std::uint8_t(i * 37), std::uint8_t(i * 91 + 17), std::uint8_t(i * 53 + 101)};
    };
    auto next = std::chrono::steady_clock::now();
    for (auto i = 0; i < Warm + Samples; ++i)
    {
      std::this_thread::sleep_until(next);
      REQUIRE(host.service
                ->setImageOverride(QuadDiffuse, overrideOf(constantPixels(colourOf(i))))
                .is_success());
      next += 100ms;
    }
    std::this_thread::sleep_until(next);
    REQUIRE(host.session->readbackLdr(10s).is_success());

    // the worst case: a source decode queued just before the hand-over
    for (auto i = 0; i < Decodes; ++i)
    {
      auto key = *diffuseKey;
      key.name = fmt::format("textures/lp5/feed_decode_{}", i);
      static_cast<void>(host.service->image(key));
      std::this_thread::sleep_for(1ms);
      REQUIRE(host.service
                ->setImageOverride(
                  QuadDiffuse, overrideOf(constantPixels(colourOf(Warm + Samples + i))))
                .is_success());
      std::this_thread::sleep_for(300ms);
    }
    REQUIRE(host.session->readbackLdr(10s).is_success());
    const auto report = host.close();
    CHECK(report.deviceReleaseCount == 0u);
    CHECK_FALSE(report.frameLogError);
  }

  // feed_ms (column 47) of every frame row that has one, in order
  auto values = std::vector<double>{};
  {
    auto stream = std::ifstream{logPath};
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
      if (fields.size() > 47 && fields[1] == "frame" && !fields[47].empty())
      {
        values.push_back(std::stod(fields[47]));
      }
    }
  }
  REQUIRE(values.size() >= std::size_t(Warm + Samples + Decodes));
  const auto steady =
    std::vector<double>{values.begin() + Warm, values.begin() + Warm + Samples};
  const auto decodes = std::vector<double>{values.end() - Decodes, values.end()};
  auto sorted = steady;
  std::ranges::sort(sorted);
  const auto median = (sorted[Samples / 2 - 1] + sorted[Samples / 2]) / 2.0;
  // nearest rank
  const auto p95 = sorted[std::size_t(std::ceil(0.95 * Samples)) - 1];
  const auto worst = sorted.back();
  std::cout << fmt::format(
    "LPF.2 latency (Release, headless, 1024x1024 RGBA8 diffuse, {} samples at 100 ms): "
    "median {:.3f} ms, p95 {:.3f} ms, max {:.3f} ms, min {:.3f} ms; warm-up {}; with a "
    "1024x1024 PNG decode queued first: {}; frame rows with feed_ms {}\n",
    Samples,
    median,
    p95,
    worst,
    sorted.front(),
    fmt::join(std::vector<double>{values.begin(), values.begin() + Warm}, " "),
    fmt::join(decodes, " "),
    values.size());
  // Q29 as recorded (37-LPF-PACKAGES.md revision 2, moved after LPF.0a's 27.7 ms
  // conversion): a median of at most 50 ms, a p95 of at most 80 ms
  CHECK(median <= 50.0);
  CHECK(p95 <= 80.0);
#endif
}

// ----- FEED.md's examples, verbatim (lib/TbLitLib/FEED.md section 9) -----

namespace
{

// the examples' error handling, kept short: a refused step throws
void must(const Result<void>& result)
{
  if (result.is_error())
  {
    throw std::runtime_error{std::get<Error>(result.error()).msg};
  }
}

// FEED.md example 1: a quad, a sphere and a light from the record builders; one image
// fed; the picture read back. gamePath holds the engine's shader blobs (C1); declPath
// is the game folder the materials, images and entityDefs are read from (for a host the
// same folder; the test's temp root, which holds the fixture's lp5_feed.mtr).
Result<LdrImage> feedExampleRecords(
  const std::filesystem::path& gamePath, const std::filesystem::path& declPath)
{
  using namespace std::chrono_literals;

  // 1. the instance and the dispatcher
  auto instance = VulkanInstance::create({}) | kdl::value();
  initInstanceDispatcher(instance.handles());

  // 2. the listener, then the material service over the game folder
  auto listener = std::make_shared<feed::GuardedListener>();
  auto serviceOptions = MaterialServiceOptions{};
  serviceOptions.makeSource = [declPath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<GameDeclSource>(declPath, &cancel)};
  };
  serviceOptions.parseAll = true;
  serviceOptions.listener = feed::GuardedListener::listener(listener);
  auto service = MaterialService::create(std::move(serviceOptions)) | kdl::value();

  // 3. the session: headless (a null surface), the blobs from the game path
  auto sessionOptions = SessionOptions{};
  sessionOptions.gamePath = gamePath;
  const auto generation = std::uint64_t{1};
  auto session = LitSession::create(
                   instance.handles(),
                   VK_NULL_HANDLE,
                   320,
                   240,
                   sessionOptions,
                   [](SessionEvent, std::string, std::uint64_t) {},
                   generation,
                   service.get())
                 | kdl::value();
  listener->attach(session.get());
  session->setExposed(true);

  // 4. the first snapshot: worldspawn first, then its geometry and the light
  auto snapshot = Snapshot{};
  snapshot.generation = generation;
  snapshot.sequence = 1;
  feed::worldspawn(snapshot, 1);
  must(feed::quadBrush(
    snapshot,
    1,
    2,
    {0, 0, 0},
    {0, -1, 0},
    512,
    "lp5/feed_pbr",
    "lp5/feed_side",
    1.0,
    {512, 512}));
  must(feed::patchDef3Sphere(snapshot, 1, 3, {300, -300, -200}, 128, "lp5/feed_sphere"));
  feed::pointLight(snapshot, 4, {0, -500, 0}, 1200, {1, 1, 1});
  const auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
  while (!session->offerSnapshot(shared))
  {
    std::this_thread::sleep_for(1ms);
  }

  // 5. the camera
  session->setCamera(feed::cameraLookingAt({-600, -900, 400}, {0, 0, 0}));

  // 6. an image under its game image name: 1024², rows top first, RGBA8
  auto pixels = RawImagePixels{};
  pixels.width = 1024;
  pixels.height = 1024;
  pixels.bytes.resize(std::size_t{1024} * 1024 * 4);
  for (std::size_t i = 0; i < pixels.bytes.size(); i += 4)
  {
    pixels.bytes[i] = 200;
    pixels.bytes[i + 1] = 120;
    pixels.bytes[i + 2] = 40;
    pixels.bytes[i + 3] = 255;
  }
  auto image = ImageOverride{};
  image.image = std::move(pixels);
  image.editorSize = EditorImageSize{1024, 1024, EditorImageSize::State::Ready, {}};
  must(service->setImageOverride("textures/lp5/feed_pbr", std::move(image)));

  // 7. the picture of the first settled frame that holds the override
  auto picture = session->readbackLdr(60s);

  // 8. the teardown, in the editor's order
  feed::teardown(
    std::move(session), std::move(service), std::move(instance), listener.get());
  return picture;
}

// FEED.md example 2: the same kind of scene from a map text (a quad brush, a patch, a
// light), read by snapshotOfMapText; the session made as in example 1.
Result<LdrImage> feedExampleMapText(
  const std::filesystem::path& gamePath, const std::filesystem::path& declPath)
{
  using namespace std::chrono_literals;

  const auto mapText = std::string{R"({
"classname" "worldspawn"
{
( -512 0 -512 ) ( -512 0 -511 ) ( -511 0 -512 ) lp5/feed_pbr [ 1 0 0 512 ] [ 0 0 -1 512 ] 0 1 1
( 512 16 512 ) ( 513 16 512 ) ( 512 16 513 ) lp5/feed_side [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -512 0 -512 ) ( -512 1 -512 ) ( -512 0 -511 ) lp5/feed_side [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 512 16 512 ) ( 512 16 513 ) ( 512 17 512 ) lp5/feed_side [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -512 0 -512 ) ( -511 0 -512 ) ( -512 1 -512 ) lp5/feed_side [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 512 16 512 ) ( 512 17 512 ) ( 513 16 512 ) lp5/feed_side [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
}
{
patchDef3
{
lp5/feed_sphere
( 3 3 4 4 0 0 0 )
(
( ( 200 -300 -300 0 0 ) ( 300 -400 -300 0.5 0 ) ( 400 -300 -300 1 0 ) )
( ( 200 -300 -200 0 0.5 ) ( 300 -400 -200 0.5 0.5 ) ( 400 -300 -200 1 0.5 ) )
( ( 200 -300 -100 0 1 ) ( 300 -400 -100 0.5 1 ) ( 400 -300 -100 1 1 ) )
)
}
}
}
{
"classname" "light"
"origin" "0 -500 0"
"light_radius" "1200 1200 1200"
"_color" "1 1 1"
}
)"};
  const auto generation = std::uint64_t{1};
  auto snapshot = snapshotOfMapText(mapText, generation, 1);
  if (snapshot.is_error())
  {
    return std::get<Error>(snapshot.error());
  }

  auto instance = VulkanInstance::create({}) | kdl::value();
  initInstanceDispatcher(instance.handles());
  auto listener = std::make_shared<feed::GuardedListener>();
  auto serviceOptions = MaterialServiceOptions{};
  serviceOptions.makeSource = [declPath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<GameDeclSource>(declPath, &cancel)};
  };
  serviceOptions.parseAll = true;
  serviceOptions.listener = feed::GuardedListener::listener(listener);
  auto service = MaterialService::create(std::move(serviceOptions)) | kdl::value();
  auto sessionOptions = SessionOptions{};
  sessionOptions.gamePath = gamePath;
  auto session = LitSession::create(
                   instance.handles(),
                   VK_NULL_HANDLE,
                   320,
                   240,
                   sessionOptions,
                   [](SessionEvent, std::string, std::uint64_t) {},
                   generation,
                   service.get())
                 | kdl::value();
  listener->attach(session.get());
  session->setExposed(true);

  const auto shared = std::make_shared<const Snapshot>(std::move(snapshot).value());
  while (!session->offerSnapshot(shared))
  {
    std::this_thread::sleep_for(1ms);
  }
  session->setCamera(feed::cameraLookingAt({-600, -900, 400}, {0, 0, 0}));

  // the patch's material re-parsed in place: its text, as an .mtr file would hold it
  must(service->reparseMaterial(
    "textures/lp5/feed_sphere",
    "{\n\ttwoSided\n\tbasecolormap\ttextures/lp5/feed_sphere\n}\n"));

  auto picture = session->readbackLdr(60s);
  feed::teardown(
    std::move(session), std::move(service), std::move(instance), listener.get());
  return picture;
}

} // namespace

TEST_CASE("Feed host: FEED.md's examples (LPF.2, D108)", "[.][feed]")
{
  const auto gamePath = feedGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH does not name a game folder with blobs and def/misc.def");
  }
  const auto root = assembleFeedRoot("TbLitLibTest-feed-examples", *gamePath);
  // a picture of something: the lit quad covers the centre
  const auto lit = [](const LdrImage& image) {
    const auto at = (std::size_t(image.height / 2) * image.width + image.width / 2) * 4;
    return image.rgba.size() == std::size_t(image.width) * image.height * 4
           && int(image.rgba[at]) + image.rgba[at + 1] + image.rgba[at + 2] > 0;
  };

  SECTION("example 1: the record builders")
  {
    const auto picture = feedExampleRecords(*gamePath, root);
    REQUIRE(picture.is_success());
    CHECK(picture.value().width == 320u);
    CHECK(lit(picture.value()));
  }

  SECTION("example 2: a map text")
  {
    const auto picture = feedExampleMapText(*gamePath, root);
    REQUIRE(picture.is_success());
    CHECK(picture.value().height == 240u);
    CHECK(lit(picture.value()));
  }
  CHECK(LitSession::liveDeviceMemoryAllocationsForTesting() == 0);
}

} // namespace tb::lit

#endif
