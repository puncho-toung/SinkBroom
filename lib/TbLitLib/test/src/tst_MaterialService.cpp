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

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

constexpr auto Timeout = 30s;
constexpr auto NoParms = ShaderParms{};

std::unique_ptr<DeclSource> fixtureSource()
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

// a one-shot gate: a thread waits in it until the test opens it
class Gate
{
private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_entered = false;
  bool m_open = false;

public:
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
    const std::string& folder, const std::string& extension) const override
  {
    return m_source.listFiles(folder, extension);
  }

  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) const override
  {
    if (++*m_reads == 2)
    {
      m_gate->enterAndWait();
    }
    return m_source.readFile(path, timestamp);
  }
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
};

MaterialServiceOptions options(
  std::function<std::unique_ptr<DeclSource>()> makeSource,
  MaterialListener listener = {},
  const bool parseAll = false)
{
  auto result = MaterialServiceOptions{};
  result.makeSource = std::move(makeSource);
  result.listener = std::move(listener);
  result.pollInterval = 0ms;
  result.parseAll = parseAll;
  return result;
}

std::shared_ptr<const MaterialEntry> waitForEntry(
  MaterialService& service, const std::string& name)
{
  if (const auto found = service.lookup(name);
      found.state == MaterialLookup::State::Ready)
  {
    return found.entry;
  }
  const auto table = service.waitForTable(
    [&](const MaterialTable& t) { return t.find(name) != nullptr; }, Timeout);
  const auto* slot = table->find(name);
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
  const auto table = service.waitForTable(
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

} // namespace

TEST_CASE("MaterialService")
{
  SECTION("a lookup is pending, then a later table has it")
  {
    auto service = MaterialService{options(fixtureSource)};
    const auto first = service.lookup("textures/lp1a/wall");
    CHECK(
      (first.state == MaterialLookup::State::Pending
       || first.state == MaterialLookup::State::Ready));

    const auto entry = waitForEntry(service, "textures/lp1a/wall");
    CHECK(entry->name == "textures/lp1a/wall");
    CHECK(entry->registry == service.table()->registry);
    CHECK(service.lookup("textures/lp1a/wall").state == MaterialLookup::State::Ready);

    // the name as asked and the decl's canonical name lead to the same entry
    const auto spelled = waitForEntry(service, "Textures\\LP1A\\Wall.tga");
    CHECK(spelled == entry);
    CHECK(service.table()->find("textures/lp1a/wall")->entry == entry);
  }

  SECTION("a parse that reached an engine error is published as failed")
  {
    auto service = MaterialService{options([] {
      return std::unique_ptr<DeclSource>{
        std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{DeclFile{
          "materials/conflict.mtr",
          "textures/lp1a/flat2d { { blend add map env/conflict } }"
          "textures/lp1a/cube { { blend add cubeMap env/conflict } }",
          0}})};
    })};
    waitForEntry(service, "textures/lp1a/flat2d");
    service.lookup("textures/lp1a/cube");
    const auto table = service.waitForTable(
      [](const MaterialTable& t) { return t.find("textures/lp1a/cube") != nullptr; },
      Timeout);
    const auto* slot = table->find("textures/lp1a/cube");
    REQUIRE(slot);
    CHECK_FALSE(slot->entry);
    CHECK(
      slot->error
      == "Image 'env/conflict' has been referenced with conflicting cube map states");
    CHECK(service.lookup("textures/lp1a/cube").state == MaterialLookup::State::Failed);
  }

  SECTION("an evaluation publishes a delta keyed by material, parms and time")
  {
    auto service = MaterialService{options(fixtureSource)};
    const auto entry = waitForEntry(service, "textures/lp1a/expr");

    auto parms = NoParms;
    parms[0] = 0.5f;
    const auto atHalf = waitForRegisters(service, *entry, parms, 0.0f);
    const auto atZero = waitForRegisters(service, *entry, NoParms, 0.0f);
    CHECK(atHalf != atZero);
    CHECK(service.evaluate(MaterialId::of(*entry), parms, 0.0f) == atHalf);

    // a delta is shared by later tables until it is dropped
    const auto table = service.table();
    CHECK(
      table->findRegisters(RegisterKey{MaterialId::of(*entry), parms, 0.0f}) == atHalf);
    CHECK(shaderParmsHash(parms, 0.0f) != shaderParmsHash(NoParms, 0.0f));
  }

  SECTION("a reload builds a new registry, keeps the looked-up names, refuses old ids")
  {
    auto service = MaterialService{options(fixtureSource)};
    const auto before = waitForEntry(service, "textures/lp1a/screen");
    const auto firstRegistry = service.table()->registry;

    service.reload();
    REQUIRE(service.waitForStats([](const auto& s) { return s.builds == 2u; }, Timeout));
    const auto table = service.table();
    CHECK(table->registry != firstRegistry);
    // published at the switch, with the name looked up again
    const auto* slot = table->find("textures/lp1a/screen");
    REQUIRE(slot);
    REQUIRE(slot->entry);
    CHECK(slot->entry->registry == table->registry);
    CHECK(slot->entry != before);
    CHECK(slot->entry->stages.front().cinematic == before->stages.front().cinematic);

    // an evaluation of the old entry is refused
    CHECK_FALSE(service.evaluate(MaterialId::of(*before), NoParms, 0.0f));
    REQUIRE(service.waitForStats(
      [](const auto& s) { return s.refusedEvaluations == 1u; }, Timeout));
    CHECK(waitForRegisters(service, *slot->entry, NoParms, 0.0f));
  }

  SECTION("the bulk parse publishes every material and table")
  {
    auto collected = Collected{};
    auto service = MaterialService{options(fixtureSource, collected.listener(), true)};
    const auto table =
      service.waitForTable([](const MaterialTable& t) { return t.complete; }, Timeout);
    REQUIRE(table->complete);

    const auto stats = service.stats();
    CHECK(stats.parseAllDone);
    CHECK(stats.materials > 15);
    CHECK(stats.tables == 5);
    CHECK(stats.defaultedMaterials == 1);
    CHECK(int(table->materials.size()) >= stats.materials);
    CHECK(stats.declTextBytes > 0u);
    CHECK(stats.declCounts.size() == 15u);

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
    auto serviceOptions = options([] { return bulkSource(20000); }, {}, true);
    serviceOptions.beforeBulkStep = [gate, steps] {
      if (++*steps == 1)
      {
        gate->enterAndWait();
      }
    };
    auto service = MaterialService{std::move(serviceOptions)};

    // queued during the build, so served before the bulk's first step
    const auto last = waitForEntry(service, "textures/bulk/m19999");
    REQUIRE(gate->waitUntilEntered());
    CHECK_FALSE(service.table()->complete);

    // queued while the bulk waits before its first step
    CHECK_FALSE(service.evaluate(MaterialId::of(*last), NoParms, 1.0f));
    gate->open();
    const auto registers = waitForRegisters(service, *last, NoParms, 1.0f);
    CHECK(registers);
    // one 20 ms step cannot have parsed 20000 materials
    CHECK_FALSE(service.table()->complete);
  }

  SECTION("a stop during the bulk parse ends after the current step")
  {
    auto gate = std::make_shared<Gate>();
    auto steps = std::make_shared<std::atomic<int>>(0);
    auto serviceOptions = options([] { return bulkSource(20000); }, {}, true);
    serviceOptions.beforeBulkStep = [gate, steps] {
      if (++*steps == 3)
      {
        gate->enterAndWait();
      }
    };
    auto service = MaterialService{std::move(serviceOptions)};
    REQUIRE(gate->waitUntilEntered());

    service.requestStop();
    CHECK(service.stopRequested());
    const auto start = std::chrono::steady_clock::now();
    gate->open();
    service.join();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK_FALSE(service.stats().parseAllDone);
    CHECK(service.lookup("textures/bulk/m19999").state == MaterialLookup::State::Pending);
  }

  SECTION("a stop during the build ends after the current decl file")
  {
    auto gate = std::make_shared<Gate>();
    auto reads = std::make_shared<std::atomic<int>>(0);
    auto service = MaterialService{options([gate, reads] {
      return std::unique_ptr<DeclSource>{std::make_unique<GatedSource>(gate, reads)};
    })};
    REQUIRE(gate->waitUntilEntered());

    service.requestStop();
    gate->open();
    service.join();
    // the fixture has 9 decl files; the files after the second were never read
    CHECK(reads->load() == 2);
    CHECK(service.stats().buildCancelled);
  }

  SECTION("a changed loose decl file reloads the registry")
  {
    const auto folder = TempFolder{"tb-lit-poll"};
    const auto file = folder.path() / "base" / "materials" / "poll.mtr";
    writeTextFile(file, materialText("textures/poll/a", "textures/poll/first"));

    auto serviceOptions = options([path = folder.path()] {
      return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(path)};
    });
    serviceOptions.pollInterval = 20ms;
    auto service = MaterialService{std::move(serviceOptions)};
    CHECK(
      diffuseImage(*waitForEntry(service, "textures/poll/a")) == "textures/poll/first");

    writeTextFile(file, materialText("textures/poll/a", "textures/poll/second"));
    bumpModificationTime(file);
    REQUIRE(
      service.waitForStats([](const auto& s) { return s.pollReloads >= 1u; }, Timeout));
    const auto table = service.waitForTable(
      [](const MaterialTable& t) {
        const auto* slot = t.find("textures/poll/a");
        return slot && slot->entry
               && diffuseImage(*slot->entry) == "textures/poll/second";
      },
      Timeout);
    CHECK(diffuseImage(*table->find("textures/poll/a")->entry) == "textures/poll/second");
  }
}

TEST_CASE("PersistentMap")
{
  auto map = PersistentMap<std::string, int>{};
  for (int i = 0; i < 5000; ++i)
  {
    map = map.set(fmt::format("key{}", i), i);
  }
  CHECK(map.size() == 5000u);
  REQUIRE(map.find("key4321"));
  CHECK(*map.find("key4321") == 4321);

  // a new version shares the old, which is unchanged
  const auto older = map;
  const auto newer = map.set("key4321", -1).erase("key0");
  CHECK(*older.find("key4321") == 4321);
  CHECK(older.find("key0"));
  CHECK(*newer.find("key4321") == -1);
  CHECK_FALSE(newer.find("key0"));
  CHECK(newer.size() == 4999u);
  CHECK(newer.erase("absent").sharesRootWith(newer));

  auto sum = 0ll;
  newer.forEach([&](const auto&, const int value) { sum += value; });
  CHECK(sum == 4999ll * 5000 / 2 - 4321 - 1);
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("MaterialService start, stop and reload leak nothing")
{
  const auto names = std::vector<std::string>{
    "textures/lp1a/wall",
    "textures/lp1a/expr",
    "textures/lp1a/screen",
    "textures/lp1a/soundviz",
    "textures/lp1a/guiscreen",
    "textures/lp1a/broken",
    "textures/lp1a/nodecl",
  };
  auto messages = std::make_shared<std::atomic<int>>(0);
  const auto counting = [messages](const MaterialServiceEvent&) { ++*messages; };

  // looks up and evaluates every name, the videomap material included
  const auto use = [&](MaterialService& service) {
    for (const auto& name : names)
    {
      const auto entry = waitForEntry(service, name);
      waitForRegisters(service, *entry, NoParms, 0.5f);
    }
  };

  // started, used, bulk-parsed and stopped; every third one is stopped at once, while it
  // builds or bulk-parses
  const auto cycle = [&](const int i) {
    auto service = MaterialService{options(fixtureSource, counting, true)};
    if (i % 3 == 2)
    {
      service.requestStop();
      service.join();
      return;
    }
    use(service);
    service.waitForTable([](const MaterialTable& t) { return t.complete; }, Timeout);
  };

  SECTION("start and stop 100 times")
  {
    cycle(0);
    cycle(2);

    _CrtMemState before;
    _CrtMemCheckpoint(&before);
    for (int i = 0; i < 100; ++i)
    {
      cycle(i);
    }
    _CrtMemState after;
    _CrtMemCheckpoint(&after);
    _CrtMemState difference;
    const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
    if (grew)
    {
      _CrtMemDumpStatistics(&difference);
    }
    CHECK_FALSE(grew);
  }

  SECTION("reload 100 times")
  {
    auto service = MaterialService{options(fixtureSource, counting, true)};
    const auto reloadAndUse = [&](const std::uint64_t builds) {
      service.reload();
      REQUIRE(service.waitForStats(
        [&](const auto& s) { return s.builds == builds && s.parseAllDone; }, Timeout));
      use(service);
    };
    // the heap is looked at only while the parse thread waits, its last work freed
    const auto waitUntilIdle = [&] {
      REQUIRE(service.waitForStats([](const auto& s) { return s.idle; }, Timeout));
    };
    use(service);
    reloadAndUse(2);
    waitUntilIdle();

    _CrtMemState before;
    _CrtMemCheckpoint(&before);
    for (std::uint64_t i = 0; i < 100; ++i)
    {
      reloadAndUse(i + 3);
    }
    waitUntilIdle();
    _CrtMemState after;
    _CrtMemCheckpoint(&after);
    _CrtMemState difference;
    const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
    if (grew)
    {
      _CrtMemDumpStatistics(&difference);
    }
    CHECK_FALSE(grew);
  }
}
#endif

} // namespace tb::lit
