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

// LPF.0a (C39, C40, C41, D96-D103, D111): the material service's image override: the
// Feed work kind and its counters, the matching, the conversion and the store with its
// one publication per hand-over. CPU only: the render side is LPF.1's.

#include "LitTestUtils.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Timeout = 30s;

std::unique_ptr<DeclSource> fixtureSource(const std::atomic<bool>&)
{
  return std::make_unique<MemoryDeclSource>(materialDeclsFixture());
}

MaterialServiceOptions options(
  std::function<std::unique_ptr<DeclSource>(const std::atomic<bool>&)> makeSource)
{
  auto result = MaterialServiceOptions{};
  result.makeSource = std::move(makeSource);
  result.pollInterval = 0ms;
  return result;
}

std::unique_ptr<MaterialService> start(MaterialServiceOptions serviceOptions)
{
  auto service = MaterialService::create(std::move(serviceOptions));
  REQUIRE(service.is_success());
  return std::move(service).value();
}

// holds the parse thread before it takes its next request, and tells when it is held
struct Hold
{
  std::mutex mutex;
  std::condition_variable changed;
  bool closed = false;
  bool held = false;

  void pass()
  {
    auto lock = std::unique_lock{mutex};
    held = true;
    changed.notify_all();
    changed.wait(lock, [&] { return !closed; });
    held = false;
  }

  void close()
  {
    const auto lock = std::scoped_lock{mutex};
    closed = true;
  }

  bool waitUntilHeld()
  {
    auto lock = std::unique_lock{mutex};
    return changed.wait_for(lock, Timeout, [&] { return held; });
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

// opens the hold when the test leaves, so that the service's join never waits for it
struct OpenHoldAtExit
{
  std::shared_ptr<Hold> hold;
  ~OpenHoldAtExit() { hold->open(); }
};

// the work items the parse thread ran, in order
struct Kinds
{
  std::mutex mutex;
  std::vector<std::string> kinds;

  void add(const std::string_view kind)
  {
    const auto lock = std::scoped_lock{mutex};
    kinds.emplace_back(kind);
  }

  std::vector<std::string> taken()
  {
    const auto lock = std::scoped_lock{mutex};
    return std::exchange(kinds, {});
  }
};

// a w × h RGBA8 picture of one colour
RawImagePixels rgba8(
  const int width,
  const int height,
  const std::uint8_t r,
  const std::uint8_t g,
  const std::uint8_t b,
  const std::uint8_t a = 255,
  const int faces = 1)
{
  auto pixels = RawImagePixels{};
  pixels.width = width;
  pixels.height = height;
  pixels.faces = faces;
  pixels.format = ImagePixelFormat::RGBA8;
  for (auto i = 0; i < width * height * faces; ++i)
  {
    pixels.bytes.insert(pixels.bytes.end(), {r, g, b, a});
  }
  return pixels;
}

ImageOverride overrideOf(RawImagePixels pixels)
{
  auto image = ImageOverride{};
  image.image = std::move(pixels);
  return image;
}

std::shared_ptr<const MaterialTable> waitForFeeds(MaterialService& service)
{
  const auto requested = service.feedRequested();
  return service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.feedApplied >= requested; }, Timeout);
}

std::shared_ptr<const MaterialEntry> waitForEntry(
  MaterialService& service, const std::string& name)
{
  static_cast<void>(service.lookup(name));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.find(name);
      return slot && (!slot->entry || slot->sizeMeasured);
    },
    Timeout);
  const auto slot = table->find(name);
  REQUIRE(slot);
  REQUIRE(slot->entry);
  return slot->entry;
}

bool waitUntilIdle(MaterialService& service)
{
  return service.waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout);
}

/**
 * Holds the parse thread before its next request: the hold closes, then an evaluation
 * the table lacks wakes the thread, which runs it and is held when it comes back for
 * the next one.
 */
void holdThread(MaterialService& service, Hold& hold, const MaterialEntry& entry)
{
  static auto parm = 0.0f;
  hold.close();
  auto parms = ShaderParms{};
  parm += 1.0f;
  parms[0] = parm;
  static_cast<void>(service.evaluate(MaterialId::of(entry), parms, 0.0f));
  REQUIRE(hold.waitUntilHeld());
}

} // namespace

TEST_CASE("MaterialService image overrides: the Feed work kind (D96, D100)")
{
  auto hold = std::make_shared<Hold>();
  auto kinds = std::make_shared<Kinds>();
  auto serviceOptions = options(fixtureSource);
  serviceOptions.beforeTakeWorkForTesting = [hold] { hold->pass(); };
  // the image hook a section sets, called on the parse thread before each item
  auto onItem = std::make_shared<std::function<void(std::string_view)>>();
  serviceOptions.beforeWorkItemForTesting = [kinds, onItem](const std::string_view kind) {
    kinds->add(kind);
    if (*onItem)
    {
      (*onItem)(kind);
    }
  };
  auto service = start(std::move(serviceOptions));
  const auto openAtExit = OpenHoldAtExit{hold};
  const auto entry = waitForEntry(*service, "textures/lp1a/wall");
  REQUIRE(waitUntilIdle(*service));

  SECTION("served after the evaluations and ahead of the lookups")
  {
    holdThread(*service, *hold, *entry);
    static_cast<void>(kinds->taken());
    static_cast<void>(service->lookup("textures/lp1a/expr"));
    REQUIRE(service->setImageOverride("textures/feed/a", overrideOf(rgba8(4, 4, 1, 2, 3)))
              .is_success());
    auto parms = ShaderParms{};
    parms[1] = 0.5f;
    static_cast<void>(service->evaluate(MaterialId::of(*entry), parms, 0.0f));
    hold->open();
    waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    const auto ran = kinds->taken();
    REQUIRE(ran.size() >= 3u);
    CHECK(ran[0] == "evaluate");
    CHECK(ran[1] == "feed");
    CHECK(ran[2] == "lookup");
  }

  SECTION("one item per name: the newest hand-over wins, the older is dropped")
  {
    holdThread(*service, *hold, *entry);
    static_cast<void>(kinds->taken());
    const auto before = service->feedRequested();
    REQUIRE(service->setImageOverride("textures/feed/b", overrideOf(rgba8(4, 4, 1, 2, 3)))
              .is_success());
    REQUIRE(service->setImageOverride("textures/feed/b", overrideOf(rgba8(4, 4, 4, 5, 6)))
              .is_success());
    CHECK(service->feedRequested() == before + 2);
    hold->open();
    const auto table = waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    CHECK(table->feedApplied == service->feedRequested());
    CHECK(service->stats().droppedFeeds == 1u);
    const auto ran = kinds->taken();
    CHECK(std::count(ran.begin(), ran.end(), "feed") == 1);
  }

  SECTION("a running image batch yields after the image in hand when a feed arrives")
  {
    holdThread(*service, *hold, *entry);
    for (auto i = 0; i < 256; ++i)
    {
      auto key = MaterialImageKey{};
      key.name = fmt::format("textures/batch/i{}", i);
      key.usage = 1;
      key.filter = 3;
      static_cast<void>(service->image(key));
    }
    auto imagesBeforeFeed = std::make_shared<std::atomic<std::size_t>>(0);
    auto handedOver = std::make_shared<std::atomic<bool>>(false);
    auto* raw = service.get();
    *onItem = [raw, imagesBeforeFeed, handedOver](const std::string_view kind) {
      if (kind == "image" && !handedOver->exchange(true))
      {
        // a hand-over while the image batch is about to run
        static_cast<void>(
          raw->setImageOverride("textures/feed/c", overrideOf(rgba8(4, 4, 1, 2, 3))));
      }
      else if (kind == "feed")
      {
        auto count = std::size_t{0};
        raw->table()->images.forEach([&](const auto&, const auto&) { ++count; });
        *imagesBeforeFeed = count;
      }
    };
    hold->open();
    waitForFeeds(*service);
    REQUIRE(waitUntilIdle(*service));
    *onItem = {};
    CHECK(handedOver->load());
    CHECK(imagesBeforeFeed->load() == 1u);
  }

  SECTION("feedRequested and feedApplied agree after every item")
  {
    for (auto i = 0; i < 5; ++i)
    {
      if (i % 2 == 0)
      {
        REQUIRE(service
                  ->setImageOverride(
                    fmt::format("textures/feed/d{}", i), overrideOf(rgba8(2, 2, 1, 2, 3)))
                  .is_success());
      }
      else
      {
        service->clearImageOverride(fmt::format("textures/feed/d{}", i - 1));
      }
      const auto table = waitForFeeds(*service);
      CHECK(table->feedApplied == service->feedRequested());
    }
    CHECK(service->feedRequested() == 5u);
  }
}

} // namespace tb::lit
