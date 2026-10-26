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
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"
#include "lit/ModelRecord.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto Timeout = 30s;
constexpr auto NoParms = ShaderParms{};

using SourceFactory =
  std::function<std::unique_ptr<DeclSource>(const std::atomic<bool>& cancel)>;

std::unique_ptr<DeclSource> fixtureSource(const std::atomic<bool>&)
{
  return std::make_unique<MemoryDeclSource>(materialDeclsFixture());
}

// one file of many small materials, so that the bulk parse takes many steps
std::unique_ptr<DeclSource> bulkSource(const int count)
{
  auto text = std::string{};
  for (int i = 0; i < count; ++i)
  {
    text += materialText(
      fmt::format("textures/bulk/m{}", i), fmt::format("textures/bulk/m{}", i));
  }
  return std::make_unique<MemoryDeclSource>(
    std::vector<DeclFile>{DeclFile{"materials/bulk.mtr", std::move(text), 0}});
}

SourceFactory bulkSourceOf(const int count)
{
  return [count](const std::atomic<bool>&) { return bulkSource(count); };
}

// a one-shot gate: a thread waits in it until the test opens it
class Gate
{
private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_entered = false;
  bool m_open = false;

public:
  // the entering side, which goes on
  void enter()
  {
    auto lock = std::scoped_lock{m_mutex};
    m_entered = true;
    m_changed.notify_all();
  }

  // the waiting side
  void enterAndWait()
  {
    auto lock = std::unique_lock{m_mutex};
    m_entered = true;
    m_changed.notify_all();
    m_changed.wait(lock, [&] { return m_open; });
  }

  bool waitUntilEntered()
  {
    auto lock = std::unique_lock{m_mutex};
    return m_changed.wait_for(lock, Timeout, [&] { return m_entered; });
  }

  void open()
  {
    auto lock = std::scoped_lock{m_mutex};
    m_open = true;
    m_changed.notify_all();
  }
};

/**
 * A2.17: opens the gate when the test leaves its scope, also through a failed REQUIRE, so
 * that the service's destructor never joins a thread that waits in the gate. Declared
 * after the service, so that it goes first.
 */
class GateOpener
{
private:
  std::shared_ptr<Gate> m_gate;

public:
  explicit GateOpener(std::shared_ptr<Gate> gate)
    : m_gate{std::move(gate)}
  {
  }

  ~GateOpener() { m_gate->open(); }

  GateOpener(const GateOpener&) = delete;
  GateOpener& operator=(const GateOpener&) = delete;
};

// a source that waits in the gate when it reads its second file
class GatedSource : public DeclSource
{
private:
  MemoryDeclSource m_source;
  std::shared_ptr<Gate> m_gate;
  std::shared_ptr<std::atomic<int>> m_reads;

public:
  GatedSource(std::shared_ptr<Gate> gate, std::shared_ptr<std::atomic<int>> reads)
    : m_source{materialDeclsFixture()}
    , m_gate{std::move(gate)}
    , m_reads{std::move(reads)}
  {
  }

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override
  {
    return m_source.listFiles(folder, extension);
  }

  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override
  {
    if (++*m_reads == 2)
    {
      m_gate->enterAndWait();
    }
    return m_source.readFile(path, timestamp);
  }
};

// The fixture with one loose file whose time the test sets. A source made with throws
// set throws when the build ends its read batch, after the registry is read, so that the
// build fails with the source made (W.4).
class ThrowOnceSource : public MemoryDeclSource
{
private:
  std::shared_ptr<std::atomic<std::int64_t>> m_looseTime;
  bool m_throws;

public:
  ThrowOnceSource(std::shared_ptr<std::atomic<std::int64_t>> looseTime, const bool throws)
    : MemoryDeclSource{materialDeclsFixture()}
    , m_looseTime{std::move(looseTime)}
    , m_throws{throws}
  {
  }

  std::vector<DeclFileTime> looseFileTimes(
    const std::string& folder, const std::string&) const override
  {
    if (folder != "materials")
    {
      return {};
    }
    return {DeclFileTime{"materials/lp1a.mtr", m_looseTime->load(), 1}};
  }

  void closeFiles() override
  {
    if (std::exchange(m_throws, false))
    {
      throw std::runtime_error{"an injected build failure"};
    }
  }
};

// a source whose files always change while they are read (A3.5)
class ChangingSource : public MemoryDeclSource
{
public:
  ChangingSource()
    : MemoryDeclSource{materialDeclsFixture()}
  {
  }

  bool changedWhileRead() const override { return true; }
};

struct Collected
{
  std::mutex mutex;
  std::vector<MaterialServiceEvent> events;

  MaterialListener listener()
  {
    return [this](const MaterialServiceEvent& event) {
      auto lock = std::scoped_lock{mutex};
      events.push_back(event);
    };
  }

  std::vector<std::string> messages(const DeclMessageKind kind)
  {
    auto lock = std::scoped_lock{mutex};
    auto texts = std::vector<std::string>{};
    for (const auto& event : events)
    {
      if (event.kind == MaterialServiceEvent::Kind::Message && event.level == kind)
      {
        texts.push_back(event.text);
      }
    }
    return texts;
  }

  // the warnings and errors whose text holds part
  std::size_t problemsWith(const std::string& part)
  {
    auto lock = std::scoped_lock{mutex};
    return std::size_t(
      std::count_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == MaterialServiceEvent::Kind::Message
               && event.level != DeclMessageKind::Info
               && event.text.find(part) != std::string::npos;
      }));
  }
};

MaterialServiceOptions options(
  SourceFactory makeSource, MaterialListener listener = {}, const bool parseAll = false)
{
  auto result = MaterialServiceOptions{};
  result.makeSource = std::move(makeSource);
  result.listener = std::move(listener);
  result.pollInterval = 0ms;
  result.parseAll = parseAll;
  return result;
}

std::unique_ptr<MaterialService> start(MaterialServiceOptions serviceOptions)
{
  auto service = MaterialService::create(std::move(serviceOptions));
  REQUIRE(service.is_success());
  return std::move(service).value();
}

// LP1c fix: a game folder of materials with one diffuse image each, and their images
class ImageServiceGame
{
private:
  fs::TestEnvironment m_env;

public:
  explicit ImageServiceGame(const int materials = 0)
  {
    auto text = std::string{};
    for (auto i = 0; i < materials; ++i)
    {
      text += materialText(
        fmt::format("textures/fix/m{}", i), fmt::format("textures/fix/m{}", i));
    }
    text += materialText("textures/fix/later", "textures/fix/later");
    text += materialText("textures/fix/kept", "textures/fix/kept");
    writeTextFile(base() / "materials" / "fix.mtr", text);
  }

  // a w × h TGA as textures/fix/<name>.tga
  void image(const std::string& name, const int width, const int height) const
  {
    writeTextFile(
      base() / "textures" / "fix" / (name + ".tga"),
      test::tgaFile(test::gradient(width, height), 32));
  }

  void remove(const std::string& name) const
  {
    std::filesystem::remove(base() / "textures" / "fix" / (name + ".tga"));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }

  MaterialServiceOptions options(const std::chrono::milliseconds pollInterval) const
  {
    auto result = MaterialServiceOptions{};
    result.makeSource = [path = m_env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    };
    result.pollInterval = pollInterval;
    return result;
  }
};

MaterialImageKey diffuseKey(const std::string& name)
{
  auto key = MaterialImageKey{};
  key.name = "textures/fix/" + name;
  key.usage = 1;
  key.filter = 3;
  return key;
}

// the measured slot of a name, once its size satisfies predicate
std::shared_ptr<const MaterialSlot> waitForSize(
  MaterialService& service,
  const std::string& name,
  const std::function<bool(const EditorImageSize&)>& predicate)
{
  static_cast<void>(service.lookup(name));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto slot = t.find(name);
      return slot && slot->entry && slot->sizeMeasured
             && predicate(slot->entry->editorImageSize);
    },
    Timeout);
  return table->find(name);
}

// the entry once its editor-image size is measured (S2.2: the measure publishes it again)
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
  INFO(slot->error);
  REQUIRE(slot->entry);
  return slot->entry;
}

std::shared_ptr<const EvaluatedRegisters> waitForRegisters(
  MaterialService& service,
  const MaterialEntry& entry,
  const ShaderParms& parms,
  const float time)
{
  if (auto registers = service.evaluate(MaterialId::of(entry), parms, time))
  {
    return registers;
  }
  const auto key = RegisterKey{MaterialId::of(entry), parms, time};
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.findRegisters(key) != nullptr; }, Timeout);
  auto registers = table->findRegisters(key);
  REQUIRE(registers);
  return registers;
}

std::string diffuseImage(const MaterialEntry& entry)
{
  for (const auto& stage : entry.stages)
  {
    if (stage.image && stage.lighting == 2)
    {
      return stage.image->name;
    }
  }
  return {};
}

// W.6, X.3: how long a full container scan must take, so that a stop 5 ms into it lands
// inside it: 2 s in Debug, and in Release, which reads the tables about ten times faster,
// 250 ms, still fifty times those 5 ms. The test doubles the containers from
// FirstScanContainers until a scan takes that long (about 128 here in either build, 22 MB
// of temporary files), and skips at MaxScanContainers.
#ifdef NDEBUG
constexpr auto LongScan = std::chrono::milliseconds{250};
#else
constexpr auto LongScan = std::chrono::milliseconds{2000};
#endif
constexpr auto FirstScanContainers = 64;
constexpr auto MaxScanContainers = 8 * FirstScanContainers;

// adds the .resources files first to count - 1 to the game folder's base, of entries
// each, so that a full container scan takes long
void writeScanHeavyGame(
  const std::filesystem::path& gamePath,
  const int first,
  const int count,
  const int entries)
{
  auto files = std::vector<std::pair<std::string, std::string>>{};
  for (int i = 0; i < entries; ++i)
  {
    files.emplace_back(fmt::format("materials/scan/entry{:05}.mtr", i), "x");
  }
  const auto bytes = resourcesBytes(files);
  for (int i = first; i < count; ++i)
  {
    writeTextFile(gamePath / "base" / fmt::format("scan{:03}.resources", i), bytes);
  }
}

} // namespace

TEST_CASE("MaterialService")
{
  SECTION("a lookup is pending, then a later table has it")
  {
    auto service = start(options(fixtureSource));
    const auto first = service->lookup("textures/lp1a/wall");
    CHECK(
      (first.state == MaterialLookup::State::Pending
       || first.state == MaterialLookup::State::Ready));

    const auto entry = waitForEntry(*service, "textures/lp1a/wall");
    CHECK(entry->name == "textures/lp1a/wall");
    CHECK(entry->registry == service->table()->registry);
    CHECK(service->lookup("textures/lp1a/wall").state == MaterialLookup::State::Ready);

    // the name as asked and the decl's canonical name lead to the same entry
    const auto spelled = waitForEntry(*service, "Textures\\LP1A\\Wall.tga");
    CHECK(spelled == entry);
    CHECK(service->table()->find("textures/lp1a/wall")->entry == entry);
  }

  SECTION("a parse that reached an engine error is published as failed")
  {
    auto service = start(options([](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{
        std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
          "materials/conflict.mtr",
          "textures/lp1a/flat2d { { blend add map env/conflict } }"
          "textures/lp1a/cube { { blend add cubeMap env/conflict } }",
          0}})};
    }));
    const auto flat = waitForEntry(*service, "textures/lp1a/flat2d");
    service->lookup("textures/lp1a/cube");
    const auto table = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.find("textures/lp1a/cube") != nullptr; },
      Timeout);
    const auto slot = table->find("textures/lp1a/cube");
    REQUIRE(slot);
    CHECK_FALSE(slot->entry);
    CHECK(
      slot->error
      == "Image 'env/conflict' has been referenced with conflicting cube map states");
    CHECK(service->lookup("textures/lp1a/cube").state == MaterialLookup::State::Failed);
    CHECK(flat);
  }

  SECTION("an evaluation publishes a delta keyed by material, parms and time")
  {
    auto service = start(options(fixtureSource));
    const auto entry = waitForEntry(*service, "textures/lp1a/expr");

    auto parms = NoParms;
    parms[0] = 0.5f;
    const auto atHalf = waitForRegisters(*service, *entry, parms, 0.0f);
    const auto atZero = waitForRegisters(*service, *entry, NoParms, 0.0f);
    CHECK(atHalf != atZero);
    CHECK(atHalf->error.empty());
    CHECK(service->evaluate(MaterialId::of(*entry), parms, 0.0f) == atHalf);

    // a delta is shared by later tables until it is dropped
    const auto table = service->table();
    CHECK(
      table->findRegisters(RegisterKey{MaterialId::of(*entry), parms, 0.0f}) == atHalf);
    CHECK(shaderParmsHash(parms, 0.0f) != shaderParmsHash(NoParms, 0.0f));
  }

  SECTION("a reload builds a new registry, keeps the looked-up names, refuses old ids")
  {
    auto service = start(options(fixtureSource));
    const auto before = waitForEntry(*service, "textures/lp1a/screen");
    const auto firstRegistry = service->table()->registry;

    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u; }, Timeout));
    const auto table = service->table();
    CHECK(table->registry != firstRegistry);
    // published at the switch, with the name looked up again
    const auto slot = table->find("textures/lp1a/screen");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->entry->registry == table->registry);
    CHECK(slot->entry != before);
    CHECK(slot->entry->stages.front().cinematic == before->stages.front().cinematic);

    // an evaluation of the old entry is refused, and the refusal is published (A2.13)
    CHECK_FALSE(service->evaluate(MaterialId::of(*before), NoParms, 0.0f));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.refusedEvaluations == 1u; }, Timeout));
    const auto refused = waitForRegisters(*service, *before, NoParms, 0.0f);
    CHECK(refused->registers.empty());
    CHECK(refused->error == "the material belongs to an earlier registry");
    CHECK(service->stats().refusedEvaluations == 1u);

    const auto registers = waitForRegisters(*service, *slot->entry, NoParms, 0.0f);
    CHECK(registers->error.empty());
  }

  SECTION("the bulk parse publishes every material and table")
  {
    auto collected = Collected{};
    auto service = start(options(fixtureSource, collected.listener(), true));
    // G17: the complete table and the summary are there once the stats say done
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.parseAllDone; }, Timeout));
    const auto table = service->table();
    REQUIRE(table->complete);

    const auto stats = service->stats();
    CHECK(stats.materials > 15);
    CHECK(stats.tables == 5);
    CHECK(stats.defaultedMaterials == 1);
    CHECK(int(table->materials.size()) >= stats.materials);
    CHECK(stats.declTextBytes > 0u);
    CHECK(stats.declCounts.size() == 15u);
    CHECK(stats.failedWorkItems == 0u);

    // the copied code's warnings and the service's summaries reach the listener
    const auto warnings = collected.messages(DeclMessageKind::Warning);
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto& text) {
      return text.find("previously defined") != std::string::npos;
    }));
    const auto infos = collected.messages(DeclMessageKind::Info);
    CHECK(std::any_of(infos.begin(), infos.end(), [](const auto& text) {
      return text.find("Parsed ") == 0;
    }));
    CHECK(stats.warnings >= warnings.size());
  }

  SECTION("evaluations and lookups jump ahead of the bulk parse")
  {
    auto gate = std::make_shared<Gate>();
    auto steps = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(bulkSourceOf(20000), {}, true);
    serviceOptions.beforeBulkStepForTesting = [gate, steps] {
      if (++*steps == 1)
      {
        gate->enterAndWait();
      }
    };
    auto service = start(std::move(serviceOptions));
    const auto opener = GateOpener{gate};

    // queued during the build, so served before the bulk's first step
    const auto last = waitForEntry(*service, "textures/bulk/m19999");
    REQUIRE(gate->waitUntilEntered());
    CHECK_FALSE(service->table()->complete);

    // queued while the bulk waits before its first step
    CHECK_FALSE(service->evaluate(MaterialId::of(*last), NoParms, 1.0f));
    gate->open();
    const auto registers = waitForRegisters(*service, *last, NoParms, 1.0f);
    CHECK(registers);
    // one 20 ms step cannot have parsed 20000 materials
    CHECK_FALSE(service->table()->complete);
  }

  SECTION("a stop during the bulk parse ends after the current step")
  {
    auto gate = std::make_shared<Gate>();
    auto steps = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(bulkSourceOf(20000), {}, true);
    serviceOptions.beforeBulkStepForTesting = [gate, steps] {
      if (++*steps == 3)
      {
        gate->enterAndWait();
      }
    };
    auto service = start(std::move(serviceOptions));
    const auto opener = GateOpener{gate};
    REQUIRE(gate->waitUntilEntered());

    service->requestStop();
    CHECK(service->stopRequested());
    const auto started = Clock::now();
    gate->open();
    service->join();
    CHECK(Clock::now() - started < 2s);
    CHECK_FALSE(service->stats().parseAllDone);
    CHECK(
      service->lookup("textures/bulk/m19999").state == MaterialLookup::State::Pending);
  }

  SECTION("a stop during the build ends after the current decl file")
  {
    auto gate = std::make_shared<Gate>();
    auto reads = std::make_shared<std::atomic<int>>(0);
    auto service = start(options([gate, reads](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{std::make_unique<GatedSource>(gate, reads)};
    }));
    const auto opener = GateOpener{gate};
    REQUIRE(gate->waitUntilEntered());

    service->requestStop();
    gate->open();
    service->join();
    // the fixture has 9 decl files; the files after the second were never read
    CHECK(reads->load() == 2);
    CHECK(service->stats().buildCancelled);
  }

  SECTION("a stop during the container scan ends within half a second")
  {
    // G4: containers of 4000 entries; the stop flag is checked for every directory entry
    // and every container table entry. W.6, X.3: the full scan takes long (LongScan), so
    // a stop 5 ms into it lands inside it, and the cancelled source holds fewer
    // containers
    const auto folder = fs::TestEnvironment{};
    auto containers = 0;
    auto fullScan = Clock::duration{};
    for (auto wanted = FirstScanContainers; fullScan <= LongScan; wanted *= 2)
    {
      if (wanted > MaxScanContainers)
      {
        SKIP(fmt::format(
          "{} containers scan in {:.0f} ms, not over {} ms: this machine is too fast for "
          "the test",
          containers,
          std::chrono::duration<double, std::milli>(fullScan).count(),
          LongScan.count()));
      }
      writeScanHeavyGame(folder.dir(), containers, wanted, 4000);
      containers = wanted;

      const auto fullStarted = Clock::now();
      {
        const auto full = GameFileSource{folder.dir()};
        CHECK(full.searchOrder().size() == std::size_t(containers));
        CHECK(full.containerCount() == std::size_t(containers));
      }
      fullScan = Clock::now() - fullStarted;
      std::cout << "container scan: " << containers << " containers in "
                << std::chrono::duration<double, std::milli>(fullScan).count() << " ms\n";
    }

    auto entered = std::make_shared<Gate>();
    auto service = start(options([path = folder.dir(),
                                  entered](const std::atomic<bool>& cancel) {
      // the test goes on when the scan begins
      entered->enter();
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    }));
    const auto opener = GateOpener{entered};
    REQUIRE(entered->waitUntilEntered());
    std::this_thread::sleep_for(5ms);

    const auto stopped = Clock::now();
    service->requestStop();
    service->join();
    const auto stopTime = Clock::now() - stopped;
    const auto fullScanMs = std::chrono::duration<double, std::milli>(fullScan).count();
    const auto stopMs = std::chrono::duration<double, std::milli>(stopTime).count();
    CAPTURE(fullScanMs, stopMs);
    CHECK(stopTime < 500ms);
    CHECK(service->stats().buildCancelled);
    CHECK(service->stats().sourceContainers < std::size_t(containers));
  }

  SECTION("a work item that throws is logged, and the thread goes on")
  {
    // G3: the first source throws while it is made; the build publishes an empty table
    auto collected = Collected{};
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto service = start(options(
      [calls](const std::atomic<bool>& cancel) -> std::unique_ptr<DeclSource> {
        if (++*calls == 1)
        {
          throw std::runtime_error{"an injected failure"};
        }
        return fixtureSource(cancel);
      },
      collected.listener()));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.failedWorkItems == 1u && s.idle; }, Timeout));
    CHECK(service->table()->registry == 0u);
    CHECK(service->stats().builds == 0u);
    CHECK(collected.problemsWith("an injected failure") == 1u);

    service->reload();
    CHECK(waitForEntry(*service, "textures/lp1a/wall"));
    CHECK(service->stats().builds == 1u);
  }

  SECTION("after a build that threw, requests fail, and a changed loose file builds")
  {
    // W.4: the first source throws once its files are read; nothing is mute meanwhile
    auto collected = Collected{};
    auto looseTime = std::make_shared<std::atomic<std::int64_t>>(1);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(
      [looseTime, calls](const std::atomic<bool>&) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<ThrowOnceSource>(looseTime, ++*calls == 1)};
      },
      collected.listener());
    serviceOptions.pollInterval = 10ms;
    serviceOptions.autoReloadInterval = 50ms;
    auto service = start(std::move(serviceOptions));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.failedWorkItems == 1u && s.idle; }, Timeout));
    CHECK(service->stats().builds == 0u);
    CHECK(collected.problemsWith("an injected build failure") == 1u);

    // a lookup is published as failed, with the build's failure
    const auto name = std::string{"textures/lp1a/wall"};
    service->lookup(name);
    const auto failedTable = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.find(name) != nullptr; }, Timeout);
    const auto slot = failedTable->find(name);
    REQUIRE(slot);
    CHECK_FALSE(slot->entry);
    CHECK_THAT(
      slot->error, Catch::Matchers::ContainsSubstring("an injected build failure"));
    CHECK(service->lookup(name).state == MaterialLookup::State::Failed);

    // an evaluation is refused, with the same reason
    const auto material = MaterialId{42, 0};
    CHECK_FALSE(service->evaluate(material, NoParms, 0.0f));
    const auto key = RegisterKey{material, NoParms, 0.0f};
    const auto refusedTable = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findRegisters(key) != nullptr; }, Timeout);
    CHECK_THAT(
      refusedTable->findRegisters(key)->error,
      Catch::Matchers::ContainsSubstring("an injected build failure"));

    // polls of an unchanged file build nothing
    std::this_thread::sleep_for(100ms);
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    CHECK(service->stats().builds == 0u);
    CHECK(service->stats().pollReloads == 0u);

    // a changed loose file builds again, and the asked name resolves
    looseTime->store(2);
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto found = t.find(name);
        return found && found->entry;
      },
      Timeout);
    REQUIRE(table->find(name)->entry);
    CHECK(table->find(name)->entry->name == name);
    CHECK(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 1u; }, Timeout));
    CHECK(service->stats().pollReloads == 1u);
    CHECK(calls->load() == 2);
  }

  SECTION("a request the thread cannot take ends it, and every later request fails")
  {
    // X.2: the hook throws the first time the thread takes a request, after its build
    auto collected = Collected{};
    auto takes = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(fixtureSource, collected.listener());
    serviceOptions.beforeTakeWorkForTesting = [takes] {
      if (++*takes == 1)
      {
        throw std::runtime_error{"an injected request failure"};
      }
    };
    auto service = start(std::move(serviceOptions));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return !s.threadError.empty(); }, Timeout));
    CHECK_THAT(
      service->stats().threadError,
      Catch::Matchers::ContainsSubstring("an injected request failure"));
    CHECK(service->stats().builds == 1u);
    CHECK(service->stats().failedWorkItems == 1u);
    CHECK(collected.problemsWith("an injected request failure") == 1u);

    // each lookup and evaluation is answered at once, and nothing waits for a thread that
    // is gone
    for (auto i = 0; i < 2; ++i)
    {
      CAPTURE(i);
      const auto found = service->lookup("textures/lp1a/wall");
      CHECK(found.state == MaterialLookup::State::Failed);
      CHECK_FALSE(found.entry);
      CHECK_THAT(
        found.error, Catch::Matchers::ContainsSubstring("an injected request failure"));

      const auto registers = service->evaluate(MaterialId{1, 0}, NoParms, 0.0f);
      REQUIRE(registers);
      CHECK(registers->registers.empty());
      CHECK_THAT(
        registers->error,
        Catch::Matchers::ContainsSubstring("an injected request failure"));
    }
    service->join();
    CHECK(takes->load() == 1);
    CHECK(service->stats().failedWorkItems == 1u);
  }

  SECTION("a thread that ends by itself answers when it cannot say why (Y.1)")
  {
    // the request fails, and so does the description of the end: two allocation
    // failures in a row
    auto collected = Collected{};
    auto takes = std::make_shared<std::atomic<int>>(0);
    auto descriptions = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(fixtureSource, collected.listener());
    serviceOptions.beforeTakeWorkForTesting = [takes] {
      if (++*takes == 1)
      {
        throw std::bad_alloc{};
      }
    };
    serviceOptions.beforeEndDescriptionForTesting = [descriptions] {
      ++*descriptions;
      throw std::bad_alloc{};
    };
    auto service = start(std::move(serviceOptions));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.threadEnded; }, Timeout));
    service->join();
    CHECK(descriptions->load() == 1);
    CHECK(service->stats().threadError.empty());

    for (auto i = 0; i < 2; ++i)
    {
      CAPTURE(i);
      const auto found = service->lookup("textures/lp1a/wall");
      CHECK(found.state == MaterialLookup::State::Failed);
      CHECK(found.error == "The material parse thread stopped by itself");

      const auto def = service->entityDef("lp1a_lamp");
      CHECK(def.state == EntityDefLookup::State::Failed);
      CHECK(def.error == "The material parse thread stopped by itself");

      const auto registers = service->evaluate(MaterialId{1, 0}, NoParms, 0.0f);
      REQUIRE(registers);
      CHECK(registers->registers.empty());
      CHECK(registers->error == "The material parse thread stopped by itself");
    }
    CHECK(takes->load() == 1);
  }

  SECTION("an entityDef query is pending, then a later table has it, once (C22)")
  {
    auto published = std::make_shared<std::atomic<int>>(0);
    auto service = start(options(fixtureSource, [published](const auto& event) {
      if (event.kind == MaterialServiceEvent::Kind::Published)
      {
        ++*published;
      }
    }));
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));

    // asked twice before the thread takes it: one query, one publication
    const auto publishedBefore = published->load();
    const auto first = service->entityDef("lp1a_lamp");
    service->entityDef("lp1a_lamp");
    CHECK(
      (first.state == EntityDefLookup::State::Pending
       || first.state == EntityDefLookup::State::Ready));
    const auto table = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.findEntityDef("lp1a_lamp") != nullptr; },
      Timeout);
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    CHECK(published->load() == publishedBefore + 1);

    const auto ready = service->entityDef("lp1a_lamp");
    REQUIRE(ready.state == EntityDefLookup::State::Ready);
    CHECK(ready.entry == table->findEntityDef("lp1a_lamp")->entry);
    CHECK(ready.entry->registry == table->registry);
    CHECK_FALSE(ready.entry->dmapOnly);
    REQUIRE(findKey(ready.entry->keys, "SPAWNCLASS"));
    CHECK(*findKey(ready.entry->keys, "SPAWNCLASS") == "idStaticEntity");
    // answered from the table: nothing more is queued or published
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    CHECK(published->load() == publishedBefore + 1);

    // another spelling shares the entry
    service->entityDef("LP1A_Lamp");
    const auto spelled = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.findEntityDef("LP1A_Lamp") != nullptr; },
      Timeout);
    CHECK(spelled->findEntityDef("LP1A_Lamp")->entry == ready.entry);

    // an unknown classname fails, once
    service->entityDef("lp1a_missing");
    const auto missingTable = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.findEntityDef("lp1a_missing") != nullptr; },
      Timeout);
    CHECK_FALSE(missingTable->findEntityDef("lp1a_missing")->entry);
    const auto missing = service->entityDef("lp1a_missing");
    CHECK(missing.state == EntityDefLookup::State::Failed);
    CHECK(missing.error == "no entityDef 'lp1a_missing'");

    // a reload asks again, so the new table has it from the new registry
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u; }, Timeout));
    const auto reloaded = service->table();
    REQUIRE(reloaded->findEntityDef("lp1a_lamp"));
    REQUIRE(reloaded->findEntityDef("lp1a_lamp")->entry);
    CHECK(reloaded->findEntityDef("lp1a_lamp")->entry->registry == reloaded->registry);
    CHECK(reloaded->registry != table->registry);
  }

  SECTION("editor-image sizes: Default, and a Ready one outlives a reload (C21)")
  {
    // what the image loads of LP1c.2 would measure: wall is 64×32 in the first build,
    // undecided (Default) in the second, 128×128 in the third
    auto build = std::make_shared<std::atomic<int>>(1);
    auto serviceOptions = options(fixtureSource);
    serviceOptions.editorImageSizeForTesting = [build](const MaterialEntry& entry) {
      if (entry.name != "textures/lp1a/wall" || build->load() == 2)
      {
        return EditorImageSize{};
      }
      return build->load() == 1
               ? EditorImageSize{64, 32, EditorImageSize::State::Ready, {}}
               : EditorImageSize{128, 128, EditorImageSize::State::Ready, {}};
    };
    auto service = start(std::move(serviceOptions));

    const auto wall = waitForEntry(*service, "textures/lp1a/wall");
    CHECK(
      wall->editorImageSize
      == EditorImageSize{64, 32, EditorImageSize::State::Ready, {}});
    const auto expr = waitForEntry(*service, "textures/lp1a/expr");
    CHECK(expr->editorImageSize == EditorImageSize{});
    // another spelling shares the sized entry
    CHECK(waitForEntry(*service, "Textures/LP1A/Wall") == wall);

    // a reload whose measure is Default keeps the Ready size (D52)
    build->store(2);
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u; }, Timeout));
    const auto kept = service->table()->find("textures/lp1a/wall");
    REQUIRE(kept);
    REQUIRE(kept->entry);
    CHECK(kept->entry->registry == service->table()->registry);
    CHECK(
      kept->entry->editorImageSize
      == EditorImageSize{64, 32, EditorImageSize::State::Ready, {}});
    CHECK(
      service->table()->find("textures/lp1a/expr")->entry->editorImageSize
      == EditorImageSize{});

    // a new Ready size replaces it
    build->store(3);
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 3u; }, Timeout));
    CHECK(
      service->table()->find("textures/lp1a/wall")->entry->editorImageSize
      == EditorImageSize{128, 128, EditorImageSize::State::Ready, {}});
  }

  SECTION("a source that keeps changing rebuilds once per autoReloadInterval")
  {
    // A2.3, A3.5: the first build asks for one more at once, later ones wait
    auto serviceOptions = options([](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{std::make_unique<ChangingSource>()};
    });
    serviceOptions.autoReloadInterval = 400ms;
    auto service = start(std::move(serviceOptions));

    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds >= 2u; }, Timeout));
    const auto second = Clock::now();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds >= 3u; }, Timeout));
    const auto third = Clock::now();
    CHECK(third - second >= 300ms);
    CHECK(service->stats().pollReloads >= 2u);
  }

  SECTION("a changed loose decl file reloads the registry")
  {
    const auto folder = fs::TestEnvironment{};
    const auto file = folder.dir() / "base" / "materials" / "poll.mtr";
    writeTextFile(file, materialText("textures/poll/a", "textures/poll/first"));

    auto serviceOptions = options([path = folder.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(path, &cancel)};
    });
    serviceOptions.pollInterval = 20ms;
    auto service = start(std::move(serviceOptions));
    CHECK(
      diffuseImage(*waitForEntry(*service, "textures/poll/a")) == "textures/poll/first");

    writeTextFile(file, materialText("textures/poll/a", "textures/poll/second"));
    bumpModificationTime(file);
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.pollReloads >= 1u; }, Timeout));
    const auto table = service->waitForTableForTesting(
      [](const MaterialTable& t) {
        const auto slot = t.find("textures/poll/a");
        return slot && slot->entry
               && diffuseImage(*slot->entry) == "textures/poll/second";
      },
      Timeout);
    CHECK(diffuseImage(*table->find("textures/poll/a")->entry) == "textures/poll/second");
  }

#if defined(_WIN32)
  SECTION("a loose decl file that cannot be read warns once, until it changes")
  {
    // G5: the file lists, and its time is polled, but no one may open it
    const auto folder = fs::TestEnvironment{};
    const auto materials = folder.dir() / "base" / "materials";
    const auto locked = materials / "locked.mtr";
    writeTextFile(locked, materialText("textures/locked/a", "textures/locked/first"));
    writeTextFile(
      materials / "open.mtr", materialText("textures/locked/open", "textures/locked/o"));

    auto handle = CreateFileW(
      locked.c_str(),
      GENERIC_READ,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    auto closeHandle =
      std::unique_ptr<void, decltype(&CloseHandle)>{handle, &CloseHandle};

    auto collected = Collected{};
    auto serviceOptions = options(
      [path = folder.dir()](const std::atomic<bool>& cancel) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<GameFileSource>(path, &cancel)};
      },
      collected.listener());
    serviceOptions.pollInterval = 10ms;
    serviceOptions.autoReloadInterval = 50ms;
    auto service = start(std::move(serviceOptions));
    CHECK(waitForEntry(*service, "textures/locked/open"));

    // many polls and auto-reload intervals later, still the one build and one warning
    std::this_thread::sleep_for(400ms);
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    CHECK(service->stats().builds == 1u);
    CHECK(service->stats().pollReloads == 0u);
    CHECK(collected.problemsWith("locked.mtr") == 1u);

    // once it changes, it is read
    closeHandle.reset();
    writeTextFile(locked, materialText("textures/locked/a", "textures/locked/second"));
    bumpModificationTime(locked);
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.pollReloads >= 1u; }, Timeout));
    CHECK(
      diffuseImage(*waitForEntry(*service, "textures/locked/a"))
      == "textures/locked/second");
  }
#endif
}

TEST_CASE("MaterialService, the LP1c fix round's work items (S2.2-S2.5, S3.13)")
{
  using State = EditorImageSize::State;

  SECTION("S2.2: the bulk parse measures nothing; a lookup queues the measure")
  {
    const auto game = ImageServiceGame{4};
    game.image("m1", 16, 8);
    auto serviceOptions = game.options(0ms);
    serviceOptions.parseAll = true;
    auto service = start(std::move(serviceOptions));
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.parseAllDone; }, Timeout));
    const auto bulk = service->table()->find("textures/fix/m1");
    REQUIRE(bulk);
    REQUIRE(bulk->entry);
    CHECK_FALSE(bulk->sizeMeasured);
    CHECK(bulk->entry->editorImageSize.state == State::Default);

    const auto measured =
      waitForSize(*service, "textures/fix/m1", [](const auto&) { return true; });
    CHECK(measured->entry->editorImageSize == EditorImageSize{16, 8, State::Ready, {}});
    // a name nobody asked for stays unmeasured
    CHECK_FALSE(service->table()->find("textures/fix/m2")->sizeMeasured);
  }

  SECTION("S2.3: an evaluation behind a batch of 100 images is answered within 50 ms")
  {
    const auto game = ImageServiceGame{100};
    for (auto i = 0; i < 100; ++i)
    {
      game.image(fmt::format("m{}", i), 64, 64);
    }
    auto service = start(game.options(0ms));
    const auto entry = waitForEntry(*service, "textures/fix/m0");
    REQUIRE(
      service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));

    for (auto i = 0; i < 100; ++i)
    {
      static_cast<void>(service->image(diffuseKey(fmt::format("m{}", i))));
    }
    auto parms = NoParms;
    parms[0] = 0.25f;
    const auto asked = Clock::now();
    CHECK_FALSE(service->evaluate(MaterialId::of(*entry), parms, 0.0f));
    const auto key = RegisterKey{MaterialId::of(*entry), parms, 0.0f};
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findRegisters(key) != nullptr; }, Timeout);
    const auto waited = Clock::now() - asked;
    REQUIRE(table->findRegisters(key));
    // the images were still coming: the evaluation did not wait for the batch
    CHECK_FALSE(table->findImage(diffuseKey("m99")));
    CHECK(waited < 50ms);
    std::cout << fmt::format(
      "S2.3: an evaluation behind 100 images answered in {:.2f} ms\n",
      std::chrono::duration<double, std::milli>(waited).count());
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(diffuseKey("m99")) != nullptr; },
      Timeout));
  }

  SECTION("S2.4: images beyond the budget that a consumer took are dropped, then re-read")
  {
    const auto game = ImageServiceGame{10};
    for (auto i = 0; i < 10; ++i)
    {
      game.image(fmt::format("m{}", i), 64, 64);
    }
    auto serviceOptions = game.options(0ms);
    // about three of the images (64 × 64 RGBA8 with its mips is 21,844 bytes)
    serviceOptions.imageBudgetBytes = 70000;
    auto service = start(std::move(serviceOptions));
    auto bytes = std::size_t{0};
    for (auto i = 0; i < 10; ++i)
    {
      const auto key = diffuseKey(fmt::format("m{}", i));
      static_cast<void>(service->image(key));
      REQUIRE(service->waitForTableForTesting(
        [&](const MaterialTable& t) { return t.findImage(key) != nullptr; }, Timeout));
      const auto taken = service->image(key);
      REQUIRE(taken.image);
      bytes = taken.image->data.size();
    }
    const auto stats = service->stats();
    CHECK(stats.imageBytes <= 70000 + bytes);
    CHECK(stats.evictedImages >= 6u);
    // the first one was dropped: asked again, it is read again
    const auto first = diffuseKey("m0");
    CHECK_FALSE(service->table()->findImage(first));
    CHECK(service->image(first).state == ImageLookup::State::Pending);
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(first) != nullptr; }, Timeout));
    CHECK(service->image(first).state == ImageLookup::State::Source);
  }

  SECTION("S2.5: a texture written after its material, a deleted one, a resized one")
  {
    const auto game = ImageServiceGame{};
    game.image("kept", 32, 16);
    auto service = start(game.options(20ms));

    // written later: Default until the poll sees the file, then its size; the image slot
    // is read again too
    const auto later =
      waitForSize(*service, "textures/fix/later", [](const auto&) { return true; });
    CHECK(later->entry->editorImageSize.state == State::Default);
    static_cast<void>(service->image(diffuseKey("later")));
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(diffuseKey("later")) != nullptr; },
      Timeout));
    CHECK(service->image(diffuseKey("later")).state == ImageLookup::State::Default);
    game.image("later", 16, 8);
    CHECK(
      waitForSize(
        *service,
        "textures/fix/later",
        [](const auto& size) { return size.state == State::Ready; })
        ->entry->editorImageSize
      == EditorImageSize{16, 8, State::Ready, {}});
    REQUIRE(service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto slot = t.findImage(diffuseKey("later"));
        return slot && slot->image;
      },
      Timeout));

    // resized: Ready to another Ready size
    CHECK(
      waitForSize(*service, "textures/fix/kept", [](const auto&) { return true; })
        ->entry->editorImageSize
      == EditorImageSize{32, 16, State::Ready, {}});
    game.image("kept", 64, 16);
    bumpModificationTime(game.base() / "textures" / "fix" / "kept.tga");
    CHECK(
      waitForSize(
        *service, "textures/fix/kept", [](const auto& size) { return size.width == 64; })
        ->entry->editorImageSize
      == EditorImageSize{64, 16, State::Ready, {}});

    // deleted: back to Default, also across a reload (the kept Ready size goes with it)
    game.remove("kept");
    CHECK(
      waitForSize(
        *service,
        "textures/fix/kept",
        [](const auto& size) { return size.state == State::Default; })
        ->entry->editorImageSize.state
      == State::Default);
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u; }, Timeout));
    CHECK(
      waitForSize(*service, "textures/fix/kept", [](const auto&) { return true; })
        ->entry->editorImageSize.state
      == State::Default);
  }

  SECTION("X.3: a reload keeps the images whose files did not change")
  {
    const auto game = ImageServiceGame{2};
    game.image("m0", 16, 16);
    game.image("m1", 16, 16);
    auto service = start(game.options(0ms));
    const auto first = diffuseKey("m0");
    const auto second = diffuseKey("m1");
    static_cast<void>(service->image(first));
    static_cast<void>(service->image(second));
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(first) && t.findImage(second); },
      Timeout);

    // one source saved anew, then a reload (an .mtr saved)
    game.image("m1", 32, 16);
    bumpModificationTime(game.base() / "textures" / "fix" / "m1.tga");
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u && s.idle; }, Timeout));
    const auto reloaded = service->table();
    CHECK(reloaded->findImage(first) == table->findImage(first));
    CHECK_FALSE(reloaded->findImage(second));
    static_cast<void>(service->image(second));
    const auto again = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(second) != nullptr; }, Timeout);
    REQUIRE(again->findImage(second)->image);
    CHECK(again->findImage(second)->image->width == 32);
  }

  SECTION("Y.2: a container rebuilt in place drops its images at a reload")
  {
    const auto game = ImageServiceGame{1};
    const auto pack = game.base() / "pack.resources";
    writeTextFile(
      pack,
      resourcesBytes(
        {{"textures/fix/m0.tga", test::tgaFile(test::gradient(16, 16), 32)}}));
    auto service = start(game.options(0ms));
    const auto key = diffuseKey("m0");
    static_cast<void>(service->image(key));
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(key) != nullptr; }, Timeout);
    REQUIRE(table->findImage(key)->image);
    CHECK(table->findImage(key)->image->width == 16);

    // the container rebuilt with the image changed, in place, then a reload
    writeTextFile(
      pack,
      resourcesBytes(
        {{"textures/fix/m0.tga", test::tgaFile(test::gradient(32, 16), 32)}}));
    bumpModificationTime(pack);
    service->reload();
    REQUIRE(service->waitForStatsForTesting(
      [](const auto& s) { return s.builds == 2u && s.idle; }, Timeout));
    CHECK_FALSE(service->table()->findImage(key));
    static_cast<void>(service->image(key));
    const auto again = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.findImage(key) != nullptr; }, Timeout);
    REQUIRE(again->findImage(key)->image);
    CHECK(again->findImage(key)->image->width == 32);
  }

  SECTION("S3.13: a bake read that throws answers its ticket with a failed slot")
  {
    class ThrowingBakeSource : public MemoryDeclSource
    {
    public:
      ThrowingBakeSource()
        : MemoryDeclSource{materialDeclsFixture()}
      {
      }
      std::optional<std::string> gameRelativePath(const std::string&) const override
      {
        return std::string{"maps/thrown.map"};
      }
      GameFileProbe probe(const std::string& path) override
      {
        if (path.rfind("maps/", 0) == 0 || path.rfind("generated/maps/", 0) == 0)
        {
          throw std::runtime_error{"the bake cannot be probed"};
        }
        return MemoryDeclSource::probe(path);
      }
    };
    auto service = start(options([](const std::atomic<bool>&) {
      return std::unique_ptr<DeclSource>{std::make_unique<ThrowingBakeSource>()};
    }));
    const auto ticket = service->loadMapBake("thrown.map");
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto slot = t.findMapBake("thrown.map");
        return slot && slot->ticket >= ticket;
      },
      Timeout);
    const auto slot = table->findMapBake("thrown.map");
    REQUIRE(slot);
    CHECK_FALSE(slot->tree);
    CHECK_THAT(slot->error, Catch::Matchers::ContainsSubstring("could not be read"));
    CHECK_THAT(slot->error, Catch::Matchers::ContainsSubstring("cannot be probed"));
    CHECK(service->stats().failedWorkItems == 0u);
  }
}

namespace
{

// LPF.0b: holds the parse thread before it takes its next request, and tells when it is
// held (beforeTakeWorkForTesting calls pass)
struct ReparseHold
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

struct OpenReparseHoldAtExit
{
  std::shared_ptr<ReparseHold> hold;
  ~OpenReparseHoldAtExit() { hold->open(); }
};

// the work items the parse thread ran, and the publications each made
struct ReparseItems
{
  std::mutex mutex;
  std::vector<std::string> kinds;
  std::string current;
  std::map<std::string, int> publications;

  void begin(const std::string_view kind)
  {
    const auto lock = std::scoped_lock{mutex};
    kinds.emplace_back(kind);
    current = std::string{kind};
  }

  void published()
  {
    const auto lock = std::scoped_lock{mutex};
    ++publications[current];
  }

  // the kinds run and the publications made since the last call
  std::pair<std::vector<std::string>, std::map<std::string, int>> taken()
  {
    const auto lock = std::scoped_lock{mutex};
    return {std::exchange(kinds, {}), std::exchange(publications, {})};
  }
};

// a service over the fixture whose thread passes the hold and reports its items
std::unique_ptr<MaterialService> reparseService(
  const std::shared_ptr<ReparseHold>& hold,
  const std::shared_ptr<ReparseItems>& items,
  SourceFactory makeSource = fixtureSource)
{
  auto serviceOptions = options(std::move(makeSource), [items](const auto& event) {
    if (event.kind == MaterialServiceEvent::Kind::Published)
    {
      items->published();
    }
  });
  serviceOptions.beforeTakeWorkForTesting = [hold] { hold->pass(); };
  serviceOptions.beforeWorkItemForTesting = [items](const std::string_view kind) {
    items->begin(kind);
  };
  return start(std::move(serviceOptions));
}

std::shared_ptr<const MaterialTable> waitForHandOvers(MaterialService& service)
{
  const auto requested = service.feedRequested();
  return service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.feedApplied >= requested; }, Timeout);
}

bool waitUntilServiceIdle(MaterialService& service)
{
  return service.waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout);
}

// holds the thread: an evaluation the table lacks wakes it, and it is held when it comes
// back for its next request
void holdAfterEvaluation(
  MaterialService& service, ReparseHold& hold, const MaterialEntry& entry)
{
  static auto parm = 100.0f;
  hold.close();
  auto parms = NoParms;
  parm += 1.0f;
  parms[5] = parm;
  static_cast<void>(service.evaluate(MaterialId::of(entry), parms, 0.0f));
  REQUIRE(hold.waitUntilHeld());
}

const auto ReparsedWall = std::string{
  "textures/lp1a/wall\n{\n\tnoshadows\n\tqer_editorimage textures/lp1a/wall_ed.tga\n"
  "\tdiffusemap textures/lp1a/wall_d.tga\n\tbumpmap textures/lp1a/wall_local.tga\n"
  "\tspecularmap textures/lp1a/wall_s.tga\n}\n"};

constexpr int NoShadows = 1 << 2;

} // namespace

TEST_CASE("MaterialService re-parses one material (LPF.0b, C42, D96, D99, D105)")
{
  auto hold = std::make_shared<ReparseHold>();
  auto items = std::make_shared<ReparseItems>();
  auto service = reparseService(hold, items);
  const auto openAtExit = OpenReparseHoldAtExit{hold};
  const auto wall = waitForEntry(*service, "textures/lp1a/wall");
  const auto otherSpelling = waitForEntry(*service, "Textures/LP1A/Wall");
  REQUIRE(otherSpelling == wall);
  REQUIRE(waitUntilServiceIdle(*service));
  static_cast<void>(items->taken());

  SECTION("one publication: every spelling, the registers gone, the measure, the counts")
  {
    auto parms = NoParms;
    parms[0] = 1.0f;
    REQUIRE(waitForRegisters(*service, *wall, parms, 0.0f));
    REQUIRE(waitUntilServiceIdle(*service));
    static_cast<void>(items->taken());
    const auto before = service->table();
    const auto key = RegisterKey{MaterialId::of(*wall), parms, 0.0f};
    REQUIRE(before->findRegisters(key));

    REQUIRE(service->reparseMaterial("textures/lp1a/wall", ReparsedWall).is_success());
    CHECK(service->feedRequested() == before->feedApplied + 1);
    const auto after = waitForHandOvers(*service);
    REQUIRE(waitUntilServiceIdle(*service));
    const auto [kinds, publications] = items->taken();
    REQUIRE_FALSE(kinds.empty());
    CHECK(kinds.front() == "reparse");
    CHECK(publications.at("reparse") == 1);
    // C21: the text may name other images, so the size is measured again
    CHECK(std::ranges::find(kinds, "measure") != kinds.end());

    CHECK(after->feedApplied == before->feedApplied + 1);
    CHECK(after->materialsRevision == before->materialsRevision + 1);
    // the registers of the material's id are gone (and from the FIFO: an evaluation is
    // made again)
    CHECK_FALSE(after->findRegisters(key));
    for (const auto* name : {"textures/lp1a/wall", "Textures/LP1A/Wall"})
    {
      CAPTURE(name);
      const auto slot = after->find(name);
      REQUIRE(slot);
      REQUIRE(slot->entry);
      CHECK(slot->entry->revision == 1u);
      CHECK((slot->entry->materialFlags & NoShadows) != 0);
      CHECK(slot->entry->index == wall->index);
    }
    // a spelling looked up only now carries the revision (the per-index store)
    const auto later = waitForEntry(*service, "TEXTURES/lp1a/wall.tga");
    CHECK(later->revision == 1u);
    CHECK((later->materialFlags & NoShadows) != 0);
    // the measure published it measured, its size kept
    const auto measured = service->table()->find("textures/lp1a/wall");
    CHECK(measured->sizeMeasured);
    // an evaluation of the new parse
    CHECK(waitForRegisters(*service, *later, parms, 0.0f)->error.empty());

    SECTION("clear: the file's text again, one publication")
    {
      REQUIRE(waitUntilServiceIdle(*service));
      static_cast<void>(items->taken());
      const auto cleared = service->table();
      service->clearMaterialReparse("Textures/LP1A/Wall");
      const auto restored = waitForHandOvers(*service);
      REQUIRE(waitUntilServiceIdle(*service));
      CHECK(items->taken().second.at("reparse") == 1);
      CHECK(restored->materialsRevision == cleared->materialsRevision + 1);
      for (const auto* name : {"textures/lp1a/wall", "Textures/LP1A/Wall"})
      {
        const auto slot = restored->find(name);
        REQUIRE(slot);
        REQUIRE(slot->entry);
        CHECK(slot->entry->revision == 2u);
        CHECK((slot->entry->materialFlags & NoShadows) == 0);
      }

      // a clear of a name with no re-parse: one publication, nothing else
      const auto unchanged = service->table();
      service->clearMaterialReparse("textures/lp1a/grate");
      const auto same = waitForHandOvers(*service);
      CHECK(same->feedApplied == unchanged->feedApplied + 1);
      CHECK(same->materialsRevision == unchanged->materialsRevision);
      CHECK(same->find("textures/lp1a/wall") == unchanged->find("textures/lp1a/wall"));
    }

    SECTION("reload: the text applied again in the first table of the new registry")
    {
      REQUIRE(waitUntilServiceIdle(*service));
      hold->close();
      service->reload();
      // the build publishes once, then the thread is held before its next request
      REQUIRE(hold->waitUntilHeld());
      const auto first = service->table();
      CHECK(first->registry != after->registry);
      const auto slot = first->find("textures/lp1a/wall");
      REQUIRE(slot);
      REQUIRE(slot->entry);
      // counted from the new registry's 0
      CHECK(slot->entry->revision == 1u);
      CHECK((slot->entry->materialFlags & NoShadows) != 0);
      CHECK(first->find("Textures/LP1A/Wall")->entry == slot->entry);
      hold->open();
    }
  }

  SECTION("a refused text publishes once, Failed with the reason")
  {
    const auto before = service->table();
    REQUIRE(service
              ->reparseMaterial(
                "textures/lp1a/wall",
                "{" + std::string(MaterialDecls::MaxReparseTextBytes, ' ') + "}")
              .is_success());
    const auto after = waitForHandOvers(*service);
    REQUIRE(waitUntilServiceIdle(*service));
    CHECK(items->taken().second.at("reparse") == 1);
    CHECK(after->feedApplied == before->feedApplied + 1);
    CHECK(after->materialsRevision == before->materialsRevision + 1);
    for (const auto* name : {"textures/lp1a/wall", "Textures/LP1A/Wall"})
    {
      const auto slot = after->find(name);
      REQUIRE(slot);
      CHECK_FALSE(slot->entry);
      CHECK_THAT(slot->error, Catch::Matchers::ContainsSubstring("a re-parse takes"));
    }

    // a text of exactly the bound parses on the parse thread's 8 MB stack, and the
    // material is published again under the asked name
    const auto head = std::string{"{ diffusemap textures/lp1a/big "};
    auto text =
      head + std::string(MaterialDecls::MaxReparseTextBytes - head.size() - 1, ' ');
    text += "}";
    REQUIRE(text.size() == MaterialDecls::MaxReparseTextBytes);
    REQUIRE(service->reparseMaterial("Textures/LP1A/Wall", text).is_success());
    const auto parsed = waitForHandOvers(*service);
    const auto slot = parsed->find("Textures/LP1A/Wall");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(diffuseImage(*slot->entry) == "textures/lp1a/big");
    CHECK(parsed->find("textures/lp1a/wall")->entry == slot->entry);
  }

  SECTION("two texts of a name before the item runs: one re-parse, the newest")
  {
    holdAfterEvaluation(*service, *hold, *wall);
    static_cast<void>(items->taken());
    REQUIRE(
      service->reparseMaterial(
               "textures/lp1a/grate", "{ diffusemap textures/lp1a/first }")
        .is_success());
    REQUIRE(
      service
        ->reparseMaterial("textures/lp1a/grate", "{ diffusemap textures/lp1a/second }")
        .is_success());
    hold->open();
    const auto after = waitForHandOvers(*service);
    REQUIRE(waitUntilServiceIdle(*service));
    CHECK(after->feedApplied == service->feedRequested());
    CHECK(service->stats().droppedReparses == 1u);
    const auto kinds = items->taken().first;
    CHECK(std::ranges::count(kinds, "reparse") == 1);
    const auto slot = after->find("textures/lp1a/grate");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(diffuseImage(*slot->entry) == "textures/lp1a/second");
    CHECK(slot->entry->revision == 1u);
  }

  SECTION("served after the evaluations and ahead of the lookups")
  {
    holdAfterEvaluation(*service, *hold, *wall);
    static_cast<void>(items->taken());
    static_cast<void>(service->lookup("textures/lp1a/expr"));
    REQUIRE(service->reparseMaterial("textures/lp1a/wall", ReparsedWall).is_success());
    auto parms = NoParms;
    parms[1] = 0.5f;
    static_cast<void>(service->evaluate(MaterialId::of(*wall), parms, 0.0f));
    hold->open();
    waitForHandOvers(*service);
    REQUIRE(waitUntilServiceIdle(*service));
    const auto kinds = items->taken().first;
    REQUIRE(kinds.size() >= 3u);
    CHECK(kinds[0] == "evaluate");
    CHECK(kinds[1] == "reparse");
    CHECK(kinds[2] == "lookup");
  }

  SECTION("an evaluation after a re-parse uses the new expressions")
  {
    REQUIRE(service
              ->reparseMaterial(
                "textures/lp1a/expr",
                "{\n\t{\n\t\tblend add\n\t\tmap textures/lp1a/glow\n"
                "\t\trgb 0.25 * parm0\n\t}\n}\n")
              .is_success());
    waitForHandOvers(*service);
    const auto expr = waitForEntry(*service, "textures/lp1a/expr");
    CHECK(expr->revision == 1u);
    auto parms = NoParms;
    parms[0] = 2.0f;
    const auto registers = waitForRegisters(*service, *expr, parms, 0.0f);
    REQUIRE(registers->error.empty());
    CHECK(registers->registers[std::size_t(expr->stages[0].colorRegisters[0])] == 0.5f);
  }

  SECTION("a name without a decl: an implicit decl re-parsed, then the default")
  {
    REQUIRE(
      service->reparseMaterial(
               "textures/lpf/fresh", "{ diffusemap textures/lpf/fresh_d }")
        .is_success());
    waitForHandOvers(*service);
    const auto fresh = waitForEntry(*service, "textures/lpf/fresh");
    CHECK(fresh->implicit);
    CHECK(diffuseImage(*fresh) == "textures/lpf/fresh_d");
    service->clearMaterialReparse("textures/lpf/fresh");
    const auto cleared = waitForHandOvers(*service);
    const auto slot = cleared->find("textures/lpf/fresh");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->entry->defaulted);
    CHECK(slot->entry->revision == 2u);
  }
}

TEST_CASE("MaterialService re-parses a Failed material (LPF.0b, P5.8)")
{
  auto hold = std::make_shared<ReparseHold>();
  auto items = std::make_shared<ReparseItems>();
  auto service = reparseService(hold, items, [](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
        "materials/conflict.mtr",
        "textures/lpf/flat2d { { blend add map env/conflict } }\n"
        "textures/lpf/cube { { blend add cubeMap env/conflict } }\n",
        0}})};
  });
  const auto openAtExit = OpenReparseHoldAtExit{hold};
  static_cast<void>(waitForEntry(*service, "textures/lpf/flat2d"));
  // the engine Error of ImageManager.cpp:441: Failed under both spellings
  for (const auto* name : {"textures/lpf/cube", "Textures/LPF/Cube"})
  {
    static_cast<void>(service->lookup(name));
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.find(name) != nullptr; }, Timeout);
    REQUIRE_FALSE(table->find(name)->entry);
  }

  REQUIRE(service->reparseMaterial("textures/lpf/cube", "{ { blend add map env/other } }")
            .is_success());
  const auto after = waitForHandOvers(*service);
  for (const auto* name : {"textures/lpf/cube", "Textures/LPF/Cube"})
  {
    CAPTURE(name);
    const auto slot = after->find(name);
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->entry->revision == 1u);
    CHECK(slot->entry->stages.size() == 1u);
  }

  // a text that reaches the Error again: Failed with the reason, the revision moved
  REQUIRE(
    service->reparseMaterial(
             "Textures/LPF/Cube", "{ { blend add cubeMap env/conflict } }")
      .is_success());
  const auto failed = waitForHandOvers(*service);
  CHECK(failed->materialsRevision == after->materialsRevision + 1);
  for (const auto* name : {"textures/lpf/cube", "Textures/LPF/Cube"})
  {
    const auto slot = failed->find(name);
    REQUIRE(slot);
    CHECK_FALSE(slot->entry);
    CHECK(
      slot->error
      == "Image 'env/conflict' has been referenced with conflicting cube map states");
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
namespace
{

// LP7.1a: a quad of two triangles on lp7/test, and one triangle
const auto ServiceQuadObj = std::string{
  "v 0 0 0\nv 32 0 0\nv 32 32 0\nv 0 32 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
  "usemtl lp7/test\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n"};
const auto ServiceTriangleObj = std::string{
  "v 0 0 0\nv 32 0 0\nv 32 32 0\nvt 0 0\nvt 1 0\nvt 1 1\nvn 0 0 1\n"
  "usemtl lp7/test\nf 1/1/1 2/2/1 3/3/1\n"};
constexpr auto Stamp2015 = std::int64_t{1451520000};

std::vector<DeclFile> modelServiceFiles()
{
  return {
    DeclFile{"materials/lp7.mtr", materialText("lp7/test", "textures/lp7/test"), 0},
    DeclFile{"models/test/quad.obj", ServiceQuadObj, Stamp2015},
  };
}

std::unique_ptr<DeclSource> modelSource(const std::atomic<bool>&)
{
  return std::make_unique<MemoryDeclSource>(modelServiceFiles());
}

// the fixture's decls with the quad: the leak test's model path
std::unique_ptr<DeclSource> fixtureWithModelSource(const std::atomic<bool>&)
{
  auto files = materialDeclsFixture();
  files.push_back(
    DeclFile{"materials/lp7.mtr", materialText("lp7/test", "textures/lp7/test"), 0});
  files.push_back(DeclFile{"models/test/quad.obj", ServiceQuadObj, Stamp2015});
  return std::make_unique<MemoryDeclSource>(std::move(files));
}

// requests a model and waits until a table holds it in a final state
std::shared_ptr<const ModelRecord> waitForModel(
  MaterialService& service,
  const std::string& name,
  const std::function<bool(const ModelRecord&)>& done = {})
{
  static_cast<void>(service.requestModel(name));
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) {
      const auto record = t.findModel(name);
      return record && record->state != ModelRecord::State::Pending
             && (!done || done(*record));
    },
    Timeout);
  return table->findModel(name);
}

// a game folder of the model fixture, its sources stamped through the file system
class ModelServiceGame
{
private:
  fs::TestEnvironment m_env;

public:
  ModelServiceGame()
  {
    writeTextFile(
      base() / "materials" / "lp7.mtr", materialText("lp7/test", "textures/lp7/test"));
    writeTextFile(sourcePath(), ServiceQuadObj);
    REQUIRE(setUnixModificationTime(sourcePath(), Stamp2015));
  }

  std::filesystem::path base() const { return m_env.dir() / "base"; }
  std::filesystem::path sourcePath() const
  {
    return base() / "models" / "test" / "quad.obj";
  }
  std::filesystem::path binaryPath() const
  {
    return base() / "generated" / "rendermodels" / "models" / "test" / "quad.bobj";
  }

  SourceFactory source() const
  {
    return [dir = m_env.dir()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameFileSource>(dir, &cancel)};
    };
  }
};

} // namespace

TEST_CASE("MaterialService loads models (LP7.1a, C51, D125, D126, D137)")
{
  SECTION("a request publishes a Ready record from an OBJ, its time the source's")
  {
    auto service = start(options(modelSource));
    CHECK_FALSE(service->requestModel(""));
    const auto record = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(record);
    CHECK(record->state == ModelRecord::State::Ready);
    CHECK(record->source == ModelRecord::SourceKind::Source);
    CHECK(record->sourceTime == Stamp2015);
    REQUIRE(record->surfaces.size() == 1u);
    CHECK(record->surfaces[0].verts.size() == 4u);
    CHECK(record->surfaces[0].material == "lp7/test");
    CHECK(record->sourceMaterialNames == std::vector<std::string>{"lp7/test"});
    REQUIRE(record->sourceMaterials.size() == 1u);
    CHECK(record->sourceMaterials[0].name == "lp7/test");
    CHECK(record->hasDrawingSurfaces);
    // any spelling finds the canonical record
    CHECK(service->requestModel("Models/Test/Quad.OBJ") == record);
    const auto stats = service->stats();
    CHECK(stats.models == 1u);
    CHECK(stats.modelFinishes == 1u);
    CHECK(stats.modelBytes > 4 * sizeof(EngineDrawVert));
    CHECK(stats.longestModelFinishMs > 0.0);
  }

  SECTION("a missing file: Missing, the default cube")
  {
    auto service = start(options(modelSource));
    const auto record = waitForModel(*service, "models/test/none.obj");
    REQUIRE(record);
    CHECK(record->state == ModelRecord::State::Missing);
    CHECK(record->source == ModelRecord::SourceKind::Default);
    CHECK_THAT(record->reason, Catch::Matchers::ContainsSubstring("no file"));
    REQUIRE(record->surfaces.size() == 1u);
    CHECK(record->surfaces[0].verts.size() == 24u);
  }

  SECTION("the dispatch: an .md5mesh Skinned, an .fbx and an .ase the cube")
  {
    auto service = start(options(modelSource));
    const auto skinned = waitForModel(*service, "models/test/a.md5mesh");
    REQUIRE(skinned);
    CHECK(skinned->state == ModelRecord::State::Skinned);
    CHECK(skinned->surfaces.empty());
    const auto fbx = waitForModel(*service, "models/test/a.fbx");
    REQUIRE(fbx);
    CHECK(fbx->state == ModelRecord::State::Default);
    CHECK(fbx->surfaces.size() == 1u);
    const auto ase = waitForModel(*service, "models/test/a.ase");
    REQUIRE(ase);
    CHECK(ase->state == ModelRecord::State::Missing);
    CHECK(ase->pendingLoader);
  }

  SECTION("the engine's binary first, when it is there and its time test passes")
  {
    const auto game = ModelServiceGame{};
    writeTextFile(
      game.binaryPath(),
      minimalBinaryModel(Stamp2015, "lp7/test", "models/test/quad.obj"));
    auto service = start(options(game.source()));
    const auto record = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(record);
    CHECK(record->state == ModelRecord::State::Ready);
    CHECK(record->source == ModelRecord::SourceKind::Binary);
    CHECK(record->binaryTime == Stamp2015);
    CHECK(record->sourceTime == Stamp2015);
    REQUIRE(record->surfaces.size() == 1u);
    CHECK(record->surfaces[0].verts.size() == 3u);
    CHECK(service->stats().modelFinishes == 0u);
  }

  SECTION("a stale binary: the source is read")
  {
    const auto game = ModelServiceGame{};
    writeTextFile(
      game.binaryPath(),
      minimalBinaryModel(Stamp2015 - 60, "lp7/test", "models/test/quad.obj"));
    auto service = start(options(game.source()));
    const auto record = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(record);
    CHECK(record->source == ModelRecord::SourceKind::Source);
    CHECK(record->surfaces[0].verts.size() == 4u);
  }

  SECTION("the poll reads a rewritten source again and counts the drop")
  {
    const auto game = ModelServiceGame{};
    auto serviceOptions = options(game.source());
    serviceOptions.pollInterval = 20ms;
    auto service = start(std::move(serviceOptions));
    const auto before = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(before);
    REQUIRE(before->surfaces.size() == 1u);
    CHECK(before->surfaces[0].verts.size() == 4u);
    const auto changes = service->table()->modelChanges;

    // the LP5.G addendum's way: the file rewritten, its time moved on by 2 s
    writeTextFile(game.sourcePath(), ServiceTriangleObj);
    REQUIRE(setUnixModificationTime(game.sourcePath(), Stamp2015 + 2));
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto record = t.findModel("models/test/quad.obj");
        return t.modelChanges > changes && record
               && record->state == ModelRecord::State::Ready
               && record->surfaces.size() == 1u && record->surfaces[0].verts.size() == 3u;
      },
      Timeout);
    const auto after = table->findModel("models/test/quad.obj");
    REQUIRE(after);
    CHECK(table->modelChanges == changes + 1);
    const auto* changed = table->changedModels.find("models/test/quad.obj");
    REQUIRE(changed);
    CHECK(*changed == table->modelChanges);
    CHECK(after->sourceTime == Stamp2015 + 2);
    CHECK(after->version > before->version);
  }

  SECTION("a re-parse of its material finishes a Source record again (P7.22)")
  {
    auto service = start(options(modelSource));
    const auto before = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(before);
    REQUIRE(before->sourceMaterials.size() == 1u);
    CHECK(before->sourceMaterials[0].deform == 0);
    const auto changes = service->table()->modelChanges;
    REQUIRE(
      service
        ->reparseMaterial(
          "lp7/test",
          "{\n\tdeform sprite\n\t{\n\t\tblend add\n\t\tmap textures/lp7/test\n\t}\n}")
        .is_success());
    const auto table = service->waitForTableForTesting(
      [&](const MaterialTable& t) {
        const auto record = t.findModel("models/test/quad.obj");
        return t.modelChanges > changes && record
               && record->state == ModelRecord::State::Ready
               && record->sourceMaterials.size() == 1u
               && record->sourceMaterials[0].deform == 1;
      },
      Timeout);
    const auto after = table->findModel("models/test/quad.obj");
    REQUIRE(after);
    CHECK(after->sourceMaterials[0].discrete);
    // the deform's bounds: centre +- (radius + 20)
    CHECK(after->bounds[0][2] < -20.0f);
  }
}

TEST_CASE("MaterialService serves Model items after Image items (LP7.1a, D126, D52)")
{
  auto hold = std::make_shared<ReparseHold>();
  auto items = std::make_shared<ReparseItems>();
  auto service = reparseService(hold, items, modelSource);
  const auto material = waitForEntry(*service, "lp7/test");
  REQUIRE(material);
  REQUIRE(waitUntilServiceIdle(*service));
  holdAfterEvaluation(*service, *hold, *material);
  static_cast<void>(items->taken());

  static_cast<void>(service->requestModel("models/test/quad.obj"));
  auto key = MaterialImageKey{};
  key.name = "textures/lp7/test";
  key.usage = 1;
  static_cast<void>(service->image(key));
  hold->open();
  const auto record = waitForModel(*service, "models/test/quad.obj");
  REQUIRE(record);
  REQUIRE(waitUntilServiceIdle(*service));
  const auto kinds = items->taken().first;
  const auto image = std::ranges::find(kinds, "image");
  const auto model = std::ranges::find(kinds, "model");
  REQUIRE(image != kinds.end());
  REQUIRE(model != kinds.end());
  CHECK(image < model);
}

namespace
{

// S2.6: holds a polling parse thread before it takes its next request (its last work item
// done and freed), and tells when it is held there
struct Parking
{
  std::mutex mutex;
  std::condition_variable changed;
  bool closed = false;
  bool parked = false;

  void pass()
  {
    auto lock = std::unique_lock{mutex};
    parked = true;
    changed.notify_all();
    changed.wait(lock, [&] { return !closed; });
    parked = false;
  }

  // closes, and waits until the thread is held
  void park()
  {
    auto lock = std::unique_lock{mutex};
    closed = true;
    changed.wait(lock, [&] { return parked; });
  }

  void open()
  {
    {
      auto lock = std::scoped_lock{mutex};
      closed = false;
    }
    changed.notify_all();
  }
};

struct UnparkAtExit
{
  std::shared_ptr<Parking> parking;
  ~UnparkAtExit() { parking->open(); }
};

const auto LeakNames = std::vector<std::string>{
  "textures/lp1a/wall",
  "textures/lp1a/expr",
  "textures/lp1a/screen",
  "textures/lp1a/soundviz",
  "textures/lp1a/guiscreen",
  "textures/lp1a/broken",
  "textures/lp1a/nodecl",
};

// looks up and evaluates every name, the videomap material included
void useService(MaterialService& service, const std::vector<std::string>& names)
{
  for (const auto& name : names)
  {
    const auto entry = waitForEntry(service, name);
    waitForRegisters(service, *entry, NoParms, 0.5f);
  }
}

// a source that leaks one allocation whenever it is made: the leak tests' control
class LeakingSource : public MemoryDeclSource
{
public:
  LeakingSource()
    : MemoryDeclSource{materialDeclsFixture()}
  {
    static_cast<void>(new std::string(256, 'x'));
  }
};

} // namespace

TEST_CASE("MaterialService start, stop and reload leak nothing")
{
  auto messages = std::make_shared<std::atomic<int>>(0);
  const auto counting = [messages](const MaterialServiceEvent&) { ++*messages; };

  // G16: started, used, bulk-parsed and stopped; one in three is stopped in its build
  // while it reads its second decl file, one in three in its bulk parse after one step
  const auto cycle = [&](const int i) {
    if (i % 3 == 1)
    {
      auto gate = std::make_shared<Gate>();
      auto reads = std::make_shared<std::atomic<int>>(0);
      auto service = start(options(
        [gate, reads](const std::atomic<bool>&) {
          return std::unique_ptr<DeclSource>{std::make_unique<GatedSource>(gate, reads)};
        },
        counting,
        true));
      const auto opener = GateOpener{gate};
      REQUIRE(gate->waitUntilEntered());
      service->requestStop();
      gate->open();
      service->join();
      CHECK(service->stats().buildCancelled);
      return;
    }
    if (i % 3 == 2)
    {
      auto gate = std::make_shared<Gate>();
      auto steps = std::make_shared<std::atomic<int>>(0);
      auto serviceOptions = options(bulkSourceOf(2000), counting, true);
      serviceOptions.beforeBulkStepForTesting = [gate, steps] {
        if (++*steps == 2)
        {
          gate->enterAndWait();
        }
      };
      auto service = start(std::move(serviceOptions));
      const auto opener = GateOpener{gate};
      REQUIRE(gate->waitUntilEntered());
      service->requestStop();
      gate->open();
      service->join();
      CHECK_FALSE(service->stats().parseAllDone);
      return;
    }
    auto service = start(options(fixtureWithModelSource, counting, true));
    useService(*service, LeakNames);
    // LP7.1a: the model path, its one-worker pool made and stopped with the service
    const auto model = waitForModel(*service, "models/test/quad.obj");
    REQUIRE(model);
    CHECK(model->state == ModelRecord::State::Ready);
    service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.complete; }, Timeout);
  };

  SECTION("start and stop 100 times")
  {
    cycle(0);
    cycle(1);
    cycle(2);
    const auto grew = heapGrows([&] {
      for (int i = 0; i < 100; ++i)
      {
        cycle(i);
      }
    });
    CHECK_FALSE(grew);
  }

  SECTION("reload 100 times")
  {
    auto service = start(options(fixtureSource, counting, true));
    const auto reloadAndUse = [&](const std::uint64_t builds) {
      service->reload();
      REQUIRE(service->waitForStatsForTesting(
        [&](const auto& s) { return s.builds == builds && s.parseAllDone; }, Timeout));
      useService(*service, LeakNames);
    };
    // the heap is looked at only while the parse thread waits, its last work freed
    const auto waitUntilIdle = [&] {
      REQUIRE(
        service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    };
    useService(*service, LeakNames);
    reloadAndUse(2);
    waitUntilIdle();

    const auto grew = heapGrows([&] {
      for (std::uint64_t i = 0; i < 100; ++i)
      {
        reloadAndUse(i + 3);
      }
      waitUntilIdle();
    });
    CHECK_FALSE(grew);
  }

  SECTION("reload a game folder 30 times")
  {
    // G16: the real GameFileSource, with a .pk4 and .resources files, under the
    // checkpoint
    const auto folder = fs::TestEnvironment{};
    const auto base = folder.dir() / "base";
    writeTextFile(
      base / "materials" / "loose.mtr",
      materialText("textures/leak/loose", "textures/leak/loose"));
    writePk4(
      base / "leak.pk4",
      {{"materials/pk4.mtr", materialText("textures/leak/pk4", "textures/leak/pk4")}});
    writeTextFile(
      base / "leak.resources",
      resourcesBytes(
        {{"materials/res.mtr", materialText("textures/leak/res", "textures/leak/res")}}));
    writeTextFile(
      base / "maps" / "leakmap.resources",
      resourcesBytes(
        {{"materials/map.mtr", materialText("textures/leak/map", "textures/leak/map")}}));
    const auto names = std::vector<std::string>{
      "textures/leak/loose",
      "textures/leak/pk4",
      "textures/leak/res",
      "textures/leak/map",
      "textures/leak/nodecl",
    };

    auto service = start(options(
      [path = folder.dir()](const std::atomic<bool>& cancel) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<GameFileSource>(path, &cancel)};
      },
      counting,
      true));
    const auto reloadAndUse = [&](const std::uint64_t builds) {
      service->reload();
      REQUIRE(service->waitForStatsForTesting(
        [&](const auto& s) { return s.builds == builds && s.parseAllDone; }, Timeout));
      useService(*service, names);
    };
    const auto waitUntilIdle = [&] {
      REQUIRE(
        service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    };
    useService(*service, names);
    reloadAndUse(2);
    waitUntilIdle();

    const auto grew = heapGrows([&] {
      for (std::uint64_t i = 0; i < 30; ++i)
      {
        reloadAndUse(i + 3);
      }
      waitUntilIdle();
    });
    CHECK_FALSE(grew);
  }

  SECTION("S2.6: image lookups, the fallback, sizes, re-probes and bakes, 50 times")
  {
    // images from caches, from sources, a refused file and a cube missing a side; the
    // sizes measured; a map's bake; the poll's re-probe of a changed and an appearing
    // file
    const auto folder = fs::TestEnvironment{};
    const auto base = folder.dir() / "base";
    auto text = std::string{};
    for (const auto* name : {"plain", "refused", "later", "changing"})
    {
      text += materialText(
        std::string{"textures/heap/"} + name, std::string{"textures/heap/"} + name);
    }
    writeTextFile(base / "materials" / "heap.mtr", text);
    const auto tga = [&](const std::string& name, const int size) {
      writeTextFile(
        base / "textures" / "heap" / (name + ".tga"),
        test::tgaFile(test::gradient(size, size), 32));
    };
    tga("plain", 16);
    tga("changing", 8);
    auto refused = test::tgaFile(test::gradient(4, 4), 32);
    refused[16] = 16;
    writeTextFile(base / "textures" / "heap" / "refused.tga", refused);
    for (const auto* side : {"_px", "_nx", "_py", "_ny", "_pz"})
    {
      tga(std::string{"sky"} + side, 8);
    }
    writeTextFile(base / "maps" / "heap.proc", "mapProcFile003\n");

    auto serviceOptions = options(
      [path = folder.dir()](const std::atomic<bool>& cancel) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<GameFileSource>(path, &cancel)};
      },
      counting);
    serviceOptions.pollInterval = 10ms;
    // the heap is looked at only while the polling parse thread waits before its next
    // request, its last work item freed
    auto parking = std::make_shared<Parking>();
    serviceOptions.beforeTakeWorkForTesting = [parking] { parking->pass(); };
    auto service = start(std::move(serviceOptions));
    const auto unparkAtExit = UnparkAtExit{parking};
    const auto key = [](const std::string& name, const int cube = 0) {
      auto image = MaterialImageKey{};
      image.name = "textures/heap/" + name;
      image.usage = 1;
      image.filter = 3;
      image.cube = cube;
      return image;
    };
    const auto names = std::vector<std::string>{
      "textures/heap/plain",
      "textures/heap/refused",
      "textures/heap/later",
      "textures/heap/changing"};
    const auto mapPath = (base / "maps" / "heap.map").string();
    auto round = 0;
    const auto heapCycle = [&] {
      ++round;
      service->reload();
      REQUIRE(service->waitForStatsForTesting(
        [&](const auto& s) { return s.builds == std::uint64_t(round + 1); }, Timeout));
      for (const auto& name : names)
      {
        static_cast<void>(waitForSize(*service, name, [](const auto&) { return true; }));
      }
      const auto images =
        std::vector<MaterialImageKey>{key("plain"), key("refused"), key("sky", 1)};
      for (const auto& image : images)
      {
        static_cast<void>(service->image(image));
      }
      REQUIRE(service->waitForTableForTesting(
        [&](const MaterialTable& t) {
          return std::all_of(images.begin(), images.end(), [&](const auto& image) {
            return t.findImage(image) != nullptr;
          });
        },
        Timeout));
      // a file that changes and one that appears, seen by the poll
      tga("changing", round % 2 == 0 ? 8 : 16);
      bumpModificationTime(base / "textures" / "heap" / "changing.tga");
      tga("later", 8);
      const auto expected = round % 2 == 0 ? 8 : 16;
      static_cast<void>(
        waitForSize(*service, "textures/heap/changing", [&](const auto& size) {
          return size.width == expected;
        }));
      static_cast<void>(
        waitForSize(*service, "textures/heap/later", [](const auto& size) {
          return size.state == EditorImageSize::State::Ready;
        }));
      // a deleted one, back to Default
      std::filesystem::remove(base / "textures" / "heap" / "later.tga");
      static_cast<void>(
        waitForSize(*service, "textures/heap/later", [](const auto& size) {
          return size.state == EditorImageSize::State::Default;
        }));
      // the bake, and once its file changed, read again by the poll
      const auto ticket = service->loadMapBake(mapPath);
      const auto read = service->waitForTableForTesting(
        [&](const MaterialTable& t) {
          const auto slot = t.findMapBake(mapPath);
          return slot && slot->ticket >= ticket;
        },
        Timeout);
      const auto first = read->findMapBake(mapPath);
      REQUIRE(first);
      bumpModificationTime(base / "maps" / "heap.proc");
      REQUIRE(service->waitForTableForTesting(
        [&](const MaterialTable& t) { return t.findMapBake(mapPath) != first; },
        Timeout));
      REQUIRE(
        service->waitForStatsForTesting([](const auto& s) { return s.idle; }, Timeout));
    };
    heapCycle();
    heapCycle();
    parking->park();
    // the polling thread's folder listings and kept slots may hold a block or two more or
    // less at either checkpoint; a leak in any of the 50 cycles shows 50 times
    const auto growth = heapGrowth([&] {
      parking->open();
      for (auto i = 0; i < 50; ++i)
      {
        heapCycle();
      }
      parking->park();
    });
    parking->open();
    CAPTURE(growth.blocks, growth.bytes);
    CHECK(growth.blocks <= 8);
    CHECK(growth.bytes <= 8192);
  }
}

namespace
{

// the allocations of one thread while the hook is set
auto allocations = std::atomic<int>{0};
auto countedThread = DWORD{0};

int countAllocations(
  const int allocType,
  void*,
  const size_t,
  const int,
  const long,
  const unsigned char*,
  const int)
{
  if (allocType == _HOOK_ALLOC && GetCurrentThreadId() == countedThread)
  {
    ++allocations;
  }
  return TRUE;
}

} // namespace

TEST_CASE("MaterialTable's finds allocate nothing")
{
  // longer than any small-string buffer, so that a key made for the call would allocate
  const auto name =
    std::string{"textures/lp1b/a_material_name_well_beyond_the_small_string_buffer"};
  auto service = start(options(fixtureSource));
  waitForEntry(*service, name);
  service->entityDef("lp1a_lamp");
  const auto table = service->waitForTableForTesting(
    [](const MaterialTable& t) { return t.findEntityDef("lp1a_lamp") != nullptr; },
    Timeout);

  const auto view = std::string_view{name};
  // LP7.0 (C51, C53): the model and skin finds, of names no table holds
  const auto model =
    std::string{"models/mapobjects/a_model_name_well_beyond_the_small_string_buffer.lwo"};
  const auto modelView = std::string_view{model};
  auto found = true;
  countedThread = GetCurrentThreadId();
  allocations = 0;
  const auto previous = _CrtSetAllocHook(&countAllocations);
  for (auto i = 0; i < 100; ++i)
  {
    found = found && table->find(view) != nullptr
            && table->findEntityDef("lp1a_lamp") != nullptr
            && table->findModel(modelView) == nullptr
            && table->findSkin(modelView) == nullptr;
  }
  _CrtSetAllocHook(previous);
  CHECK(found);
  CHECK(allocations.load() == 0);
}

// LP7.0 (C51, C53, D126): the model table's types with no loader: the canonical name, the
// empty maps, and a reload keeping the records and their change counts (the images'
// rule) while the skins, per registry as the entityDefs, go
TEST_CASE("MaterialTable: the model records and the skins (LP7.0, C51, C53)")
{
  SECTION("the canonical name (ModelManager.cpp:313-329)")
  {
    CHECK(canonicalModelName("").empty());
    CHECK(
      canonicalModelName("Models/MapObjects/Chairs/Office.LWO")
      == "models/mapobjects/chairs/office.lwo");
    // the separators and the extension stay; only 'A'-'Z' are lowered
    CHECK(canonicalModelName("MODELS\\A.Obj") == "models\\a.obj");
    CHECK(canonicalModelName("\xC0\xDF.ASE") == "\xC0\xDF.ase");
    // MAX_OSPATH does not cut it
    CHECK(
      canonicalModelName(std::string(300, 'Q') + ".obj")
      == std::string(300, 'q') + ".obj");
  }

  SECTION("nothing requests a model: the maps are empty")
  {
    auto service = start(options(fixtureSource));
    const auto table = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.registry != 0; }, Timeout);
    CHECK(table->models.empty());
    CHECK(table->modelChanges == 0u);
    CHECK(table->changedModels.empty());
    CHECK(table->skins.empty());
    CHECK_FALSE(table->findModel("models/a.obj"));
    CHECK_FALSE(table->findSkin("skins/a"));
  }

  SECTION("a reload keeps the model records and their changes; the skins go")
  {
    auto record = std::make_shared<ModelRecord>();
    record->name = canonicalModelName("Models/A.obj");
    record->state = ModelRecord::State::Ready;
    record->source = ModelRecord::SourceKind::Source;
    record->surfaces.push_back(
      ModelSurface{"textures/a", {}, {0, 1, 2}, true, false, {}});
    auto skin = std::make_shared<SkinSlot>();
    skin->entry = std::make_shared<SkinRecord>(
      SkinRecord{"skins/a", {{"textures/a", "textures/b"}, {"", "textures/c"}}});
    auto builds = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options(fixtureSource);
    serviceOptions.registryBuiltForTesting = [=](MaterialTable& t) {
      if (++*builds == 1)
      {
        t.models = t.models.set(record->name, record);
        t.modelChanges = 1;
        t.changedModels = t.changedModels.set(record->name, 1u);
        t.skins = t.skins.set("skins/a", skin);
      }
    };
    auto service = start(std::move(serviceOptions));
    const auto first = service->waitForTableForTesting(
      [](const MaterialTable& t) { return t.registry != 0; }, Timeout);
    CHECK(first->findModel("models/a.obj") == record);
    CHECK(first->findSkin("skins/a") == skin);

    service->reload();
    const auto reloaded = service->waitForTableForTesting(
      [&](const MaterialTable& t) { return t.registry != first->registry; }, Timeout);
    CHECK(builds->load() == 2);
    CHECK(reloaded->findModel("models/a.obj") == record);
    CHECK(reloaded->modelChanges == 1u);
    const auto* change = reloaded->changedModels.find(std::string_view{"models/a.obj"});
    REQUIRE(change);
    CHECK(*change == 1u);
    CHECK_FALSE(reloaded->findSkin("skins/a"));
    CHECK(reloaded->skins.empty());
  }
}

// The control (G16): a source that leaks on every build must make the check fail, so that
// a passing leak test means the checkpoint sees the parse thread's heap. Catch2's
// [!shouldfail] reports it as passed when its check fails.
TEST_CASE("MaterialService re-parses leak nothing (LPF.0b)")
{
  auto service = start(options(fixtureSource));
  static_cast<void>(waitForEntry(*service, "textures/lp1a/wall"));
  const auto cycle = [&](const int i) {
    REQUIRE(service
              ->reparseMaterial(
                "textures/lp1a/wall",
                fmt::format("{{ diffusemap textures/lp1a/wall_{} }}", i % 3))
              .is_success());
    static_cast<void>(waitForHandOvers(*service));
    service->clearMaterialReparse("textures/lp1a/wall");
    static_cast<void>(waitForHandOvers(*service));
    REQUIRE(service->reparseMaterial("textures/lp1a/wall", ReparsedWall).is_success());
    static_cast<void>(waitForHandOvers(*service));
    REQUIRE(waitUntilServiceIdle(*service));
  };
  // LPF.1 (C44): the warm-up fills the tables' feed records to their bound
  // (MaterialTable::FeedRecordsKept), three items a cycle
  for (auto i = 0; i < int(MaterialTable::FeedRecordsKept / 3) + 2; ++i)
  {
    cycle(i);
  }
  // the tables' tries, the queues' deques and the kept slots may hold a few dozen blocks
  // more or less at either checkpoint (seen: up to 39 blocks, 1984 bytes); a leak in any
  // of the 300 re-parses or clears shows 100 times or more
  const auto growth = heapGrowth([&] {
    for (auto i = 0; i < 100; ++i)
    {
      cycle(i);
    }
  });
  CAPTURE(growth.blocks, growth.bytes);
  CHECK(growth.blocks <= 64);
  CHECK(growth.bytes <= 8192);
}

TEST_CASE("MaterialService leak check sees an injected leak", "[!shouldfail]")
{
  auto service = start(options([](const std::atomic<bool>&) {
    return std::unique_ptr<DeclSource>{std::make_unique<LeakingSource>()};
  }));
  useService(*service, {"textures/lp1a/wall"});
  service->reload();
  REQUIRE(service->waitForStatsForTesting(
    [](const auto& s) { return s.builds == 2u && s.idle; }, Timeout));

  const auto grew = heapGrows([&] {
    for (std::uint64_t i = 0; i < 5; ++i)
    {
      service->reload();
      REQUIRE(service->waitForStatsForTesting(
        [&](const auto& s) { return s.builds == i + 3 && s.idle; }, Timeout));
    }
  });
  CHECK_FALSE(grew);
}
#endif

} // namespace tb::lit
