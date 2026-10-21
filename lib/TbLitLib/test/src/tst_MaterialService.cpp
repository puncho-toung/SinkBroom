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

#include "LitTestUtils.h"
#include "lit/GameDeclSource.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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

std::shared_ptr<const MaterialEntry> waitForEntry(
  MaterialService& service, const std::string& name)
{
  if (const auto found = service.lookup(name);
      found.state == MaterialLookup::State::Ready)
  {
    return found.entry;
  }
  const auto table = service.waitForTableForTesting(
    [&](const MaterialTable& t) { return t.find(name) != nullptr; }, Timeout);
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
    const auto folder = TempFolder{"tb-lit-scan"};
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
      writeScanHeavyGame(folder.path(), containers, wanted, 4000);
      containers = wanted;

      const auto fullStarted = Clock::now();
      {
        const auto full = GameDeclSource{folder.path()};
        CHECK(full.searchOrder().size() == std::size_t(containers));
        CHECK(full.containerCount() == std::size_t(containers));
      }
      fullScan = Clock::now() - fullStarted;
      std::cout << "container scan: " << containers << " containers in "
                << std::chrono::duration<double, std::milli>(fullScan).count() << " ms\n";
    }

    auto entered = std::make_shared<Gate>();
    auto service = start(options([path = folder.path(),
                                  entered](const std::atomic<bool>& cancel) {
      // the test goes on when the scan begins
      entered->enter();
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(path, &cancel)};
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
    const auto folder = TempFolder{"tb-lit-poll"};
    const auto file = folder.path() / "base" / "materials" / "poll.mtr";
    writeTextFile(file, materialText("textures/poll/a", "textures/poll/first"));

    auto serviceOptions = options([path =
                                     folder.path()](const std::atomic<bool>& cancel) {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(path, &cancel)};
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
    const auto folder = TempFolder{"tb-lit-locked"};
    const auto materials = folder.path() / "base" / "materials";
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
      [path = folder.path()](const std::atomic<bool>& cancel) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<GameDeclSource>(path, &cancel)};
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

#if defined(_MSC_VER) && defined(_DEBUG)
namespace
{

// runs f between two heap checkpoints; true when the heap grew, with its statistics
template <typename F>
bool heapGrows(F&& f)
{
  _CrtMemState before;
  _CrtMemCheckpoint(&before);
  f();
  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  _CrtMemState difference;
  const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
  if (grew)
  {
    _CrtMemDumpStatistics(&difference);
  }
  return grew;
}

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
    auto service = start(options(fixtureSource, counting, true));
    useService(*service, LeakNames);
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
    // G16: the real GameDeclSource, with a .pk4 and .resources files, under the
    // checkpoint
    const auto folder = TempFolder{"tb-lit-leak-game"};
    const auto base = folder.path() / "base";
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
      [path = folder.path()](const std::atomic<bool>& cancel) {
        return std::unique_ptr<DeclSource>{
          std::make_unique<GameDeclSource>(path, &cancel)};
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
}

// The control (G16): a source that leaks on every build must make the check fail, so that
// a passing leak test means the checkpoint sees the parse thread's heap. Catch2's
// [!shouldfail] reports it as passed when its check fails.
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
