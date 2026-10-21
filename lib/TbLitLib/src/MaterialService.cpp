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

#include "lit/MaterialService.h"

#include "StackThread.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tb::lit
{
namespace
{

using Clock = std::chrono::steady_clock;

// R4-1.1, R4-3.25, G13: the parse thread's stack on every platform. The copied parser
// keeps about 120 KB of parse data on the stack, twice when MakeDefault re-parses inside
// Parse, and _alloca's each decl's text (A2.9).
constexpr auto ParseThreadStackBytes = std::size_t{8} * 1024 * 1024;

// a bulk step parses for at most this long, then looks at the queue again (D52)
constexpr auto BulkStepTime = std::chrono::milliseconds{20};

std::uint32_t floatBits(const float value)
{
  auto bits = std::uint32_t{0};
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

std::uint64_t mixHash(std::uint64_t hash, const std::uint64_t value)
{
  hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
  return hash;
}

double secondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double>(Clock::now() - start).count();
}

bool isProblem(const DeclMessageKind kind)
{
  return kind != DeclMessageKind::Info;
}

// the loose decl files of the engine's decl folders and their times
std::vector<DeclFileTime> looseStamp(const DeclSource& source)
{
  auto stamp = std::vector<DeclFileTime>{};
  for (const auto& folder : MaterialDecls::engineDeclFolders())
  {
    for (auto& file : source.looseFileTimes(folder.folder, folder.extension))
    {
      stamp.push_back(std::move(file));
    }
  }
  return stamp;
}

std::shared_ptr<const EvaluatedRegisters> refusal(std::string reason)
{
  auto registers = std::make_shared<EvaluatedRegisters>();
  registers->error = std::move(reason);
  return registers;
}

} // namespace

MaterialId MaterialId::of(const MaterialEntry& entry)
{
  return MaterialId{entry.registry, entry.index};
}

bool MaterialId::operator==(const MaterialId& other) const
{
  return registry == other.registry && index == other.index;
}

bool RegisterKey::operator==(const RegisterKey& other) const
{
  if (!(material == other.material) || floatBits(time) != floatBits(other.time))
  {
    return false;
  }
  for (std::size_t i = 0; i < shaderParms.size(); ++i)
  {
    if (floatBits(shaderParms[i]) != floatBits(other.shaderParms[i]))
    {
      return false;
    }
  }
  return true;
}

std::uint64_t shaderParmsHash(const ShaderParms& shaderParms, const float time)
{
  auto hash = std::uint64_t{floatBits(time)};
  for (const auto parm : shaderParms)
  {
    hash = mixHash(hash, floatBits(parm));
  }
  return hash;
}

std::size_t RegisterKeyHash::operator()(const RegisterKey& key) const
{
  // without the registry: a table holds the deltas of one registry only, so its keys
  // hash alike from one build to the next
  auto hash = shaderParmsHash(key.shaderParms, key.time);
  hash = mixHash(hash, std::uint64_t(std::uint32_t(key.material.index)));
  return std::size_t(hash);
}

std::shared_ptr<const MaterialSlot> MaterialTable::find(const std::string_view name) const
{
  const auto* slot = materials.find(std::string{name});
  return slot ? *slot : nullptr;
}

std::shared_ptr<const EvaluatedRegisters> MaterialTable::findRegisters(
  const RegisterKey& key) const
{
  const auto* found = registers.find(key);
  return found ? *found : nullptr;
}

// What every thread shares: the requests, the published table and the stats, under one
// mutex; the stop flag is also the registry's and the source's cancel flag.
struct MaterialService::Shared
{
  MaterialServiceOptions options;

  mutable std::mutex mutex;
  // requests for the parse thread
  std::condition_variable requested;
  // a table or the stats changed (tests wait on it)
  mutable std::condition_variable changed;

  std::atomic<bool> stop = false;
  bool reloadRequested = false;
  bool parseAllRequested = false;
  std::deque<std::string> lookups;
  std::unordered_set<std::string> queuedLookups;
  std::deque<RegisterKey> evaluations;
  std::unordered_set<RegisterKey, RegisterKeyHash> queuedEvaluations;

  std::shared_ptr<const MaterialTable> table = std::make_shared<const MaterialTable>();
  MaterialServiceStats stats;

  explicit Shared(MaterialServiceOptions options_)
    : options{std::move(options_)}
  {
  }
};

// The parse thread and everything only it touches.
class MaterialService::Worker
{
private:
  enum class WorkKind
  {
    Stop,
    Reload,
    AutoReload,
    Evaluate,
    Lookup,
    Poll,
    Bulk,
  };

  struct Work
  {
    WorkKind kind = WorkKind::Stop;
    std::vector<std::string> lookups;
    std::vector<RegisterKey> evaluations;
  };

  Shared& m_shared;
  StackThread m_thread;

  // the parse thread's state; the source outlives the registry that reads through it
  std::unique_ptr<DeclSource> m_source;
  std::unique_ptr<MaterialDecls> m_decls;
  // why the last build left no registry (W.4); every request fails with it until a build
  // succeeds
  std::string m_buildError;
  MaterialTable m_working;
  std::uint64_t m_version = 0;
  std::deque<RegisterKey> m_deltaOrder;
  // the names lookups were asked for, looked up again after a reload
  std::unordered_set<std::string> m_requestedNames;
  // the loose files' times when the last build ended
  std::vector<DeclFileTime> m_stamp;
  Clock::time_point m_nextPoll;
  // a rebuild that a change of the loose files asks for, and when it may run
  bool m_autoReloadPending = false;
  std::optional<Clock::time_point> m_lastAutoReload;
  std::size_t m_warnings = 0;

  int m_bulkMaterial = 0;
  int m_bulkTable = 0;
  bool m_bulkDone = false;
  std::optional<Clock::time_point> m_bulkStarted;

public:
  explicit Worker(Shared& shared)
    : m_shared{shared}
  {
  }

  ~Worker() { join(); }

  Result<void> start()
  {
    return m_thread.start(
      [this] { run(); },
      ParseThreadStackBytes,
      [this](const char* reason) { escaped(reason); });
  }

  void join() { m_thread.join(); }

private:
  bool stopping() const { return m_shared.stop.load(); }

  /**
   * A2.1: nothing a work item throws reaches the thread's top. The item is logged and
   * dropped; a build that throws leaves no registry, and every request fails until a
   * build succeeds (W.4).
   */
  template <typename F>
  void guarded(const char* what, F&& f)
  {
    try
    {
      f();
      return;
    }
    catch (const std::exception& e)
    {
      failed(what, e.what());
    }
    catch (...)
    {
      failed(what, "an unknown exception");
    }
  }

  // W.5: called from the guards' handlers, so it throws nothing itself
  void failed(const char* what, const char* reason) noexcept
  {
    try
    {
      updateStats([](MaterialServiceStats& stats) { ++stats.failedWorkItems; });
    }
    catch (...)
    {
    }
    try
    {
      message(
        DeclMessageKind::Error,
        fmt::format("The material parse thread's {} failed: {}", what, reason));
    }
    catch (...)
    {
    }
  }

  /**
   * W.5: what run lets escape (only an allocation failure outside every guard) ends the
   * thread, never the process. The registry is still destroyed on its own thread (C20),
   * and the reason reaches the listener.
   */
  void escaped(const char* reason) noexcept
  {
    try
    {
      m_decls.reset();
      m_source.reset();
    }
    catch (...)
    {
    }
    failed("loop", reason);
  }

  void run()
  {
    guarded("build", [&] { build({}); });
    for (;;)
    {
      // W.5: a request the thread cannot take (an allocation failure while it copies the
      // queue) ends the loop as a stop does, rather than trying it again at once
      auto work = Work{};
      guarded("request", [&] { work = takeWork(); });
      if (work.kind == WorkKind::Stop)
      {
        break;
      }

      switch (work.kind)
      {
      case WorkKind::Stop:
        break;
      case WorkKind::Reload:
        guarded("reload", [&] { rebuild(); });
        break;
      case WorkKind::AutoReload:
        m_lastAutoReload = Clock::now();
        updateStats([](MaterialServiceStats& stats) { ++stats.pollReloads; });
        guarded("reload", [&] { rebuild(); });
        break;
      case WorkKind::Evaluate:
        guarded("evaluation", [&] { evaluate(work.evaluations); });
        break;
      case WorkKind::Lookup:
        guarded("lookup", [&] { lookUp(work.lookups); });
        break;
      case WorkKind::Poll:
        guarded("poll", [&] { poll(); });
        break;
      case WorkKind::Bulk:
        guarded("bulk parse", [&] { bulkStep(); });
        break;
      }

      // every work item is one read batch (R4-3.26)
      guarded("message drain", [&] {
        if (m_source)
        {
          m_source->closeFiles();
        }
        drainMessages();
      });
    }

    // teardown step 8: the registry is destroyed on its own thread
    guarded("teardown", [&] {
      m_decls.reset();
      m_source.reset();
    });
  }

  Clock::time_point autoReloadDue() const
  {
    return m_lastAutoReload ? *m_lastAutoReload + m_shared.options.autoReloadInterval
                            : Clock::time_point{};
  }

  Work takeWork()
  {
    auto lock = std::unique_lock{m_shared.mutex};
    for (;;)
    {
      if (auto work = nextWork())
      {
        m_shared.stats.idle = false;
        return std::move(*work);
      }

      // nothing to do, and the last work item's memory is freed
      if (!m_shared.stats.idle)
      {
        m_shared.stats.idle = true;
        m_shared.changed.notify_all();
      }

      auto wakeUp = std::optional<Clock::time_point>{};
      if (m_shared.options.pollInterval.count() > 0)
      {
        wakeUp = m_nextPoll;
      }
      if (m_autoReloadPending)
      {
        wakeUp = wakeUp ? std::min(*wakeUp, autoReloadDue()) : autoReloadDue();
      }
      if (wakeUp)
      {
        m_shared.requested.wait_until(lock, *wakeUp);
      }
      else
      {
        m_shared.requested.wait(lock);
      }
    }
  }

  // the most urgent request, with the mutex held
  std::optional<Work> nextWork()
  {
    if (stopping())
    {
      return Work{WorkKind::Stop, {}, {}};
    }
    if (m_shared.reloadRequested)
    {
      m_shared.reloadRequested = false;
      m_autoReloadPending = false;
      return Work{WorkKind::Reload, {}, {}};
    }
    if (m_autoReloadPending && Clock::now() >= autoReloadDue())
    {
      m_autoReloadPending = false;
      return Work{WorkKind::AutoReload, {}, {}};
    }
    if (!m_shared.evaluations.empty())
    {
      auto work = Work{WorkKind::Evaluate, {}, {}};
      work.evaluations.assign(m_shared.evaluations.begin(), m_shared.evaluations.end());
      m_shared.evaluations.clear();
      m_shared.queuedEvaluations.clear();
      return work;
    }
    if (!m_shared.lookups.empty())
    {
      auto work = Work{WorkKind::Lookup, {}, {}};
      work.lookups.assign(m_shared.lookups.begin(), m_shared.lookups.end());
      m_shared.lookups.clear();
      m_shared.queuedLookups.clear();
      return work;
    }
    if (
      !m_autoReloadPending && m_shared.options.pollInterval.count() > 0
      && Clock::now() >= m_nextPoll)
    {
      return Work{WorkKind::Poll, {}, {}};
    }
    if (m_shared.parseAllRequested && bulkPending())
    {
      return Work{WorkKind::Bulk, {}, {}};
    }
    return std::nullopt;
  }

  bool bulkPending() const { return m_decls && !m_bulkDone; }

  void message(const DeclMessageKind level, std::string text)
  {
    if (isProblem(level))
    {
      ++m_warnings;
    }
    notify(MaterialServiceEvent{
      MaterialServiceEvent::Kind::Message, 0, level, std::move(text)});
  }

  // the listener must not throw; if it does, the event is dropped, so that a failure's
  // own message cannot throw out of its catch (A2.1)
  void notify(MaterialServiceEvent event)
  {
    if (stopping() || !m_shared.options.listener)
    {
      return;
    }
    try
    {
      m_shared.options.listener(event);
    }
    catch (...)
    {
    }
  }

  void drainMessages()
  {
    auto messages = m_decls ? m_decls->takeMessages() : std::vector<DeclMessage>{};
    auto warnings = m_source ? m_source->takeWarnings() : std::vector<std::string>{};
    for (auto& entry : messages)
    {
      message(entry.kind, std::move(entry.text));
    }
    for (auto& warning : warnings)
    {
      message(DeclMessageKind::Warning, std::move(warning));
    }

    auto lock = std::scoped_lock{m_shared.mutex};
    m_shared.stats.warnings = m_warnings;
  }

  void publish()
  {
    m_working.version = ++m_version;
    auto table = std::make_shared<const MaterialTable>(m_working);
    {
      auto lock = std::scoped_lock{m_shared.mutex};
      m_shared.table = std::move(table);
    }
    m_shared.changed.notify_all();

    notify(MaterialServiceEvent{
      MaterialServiceEvent::Kind::Published, m_version, DeclMessageKind::Info, {}});
  }

  // A2.12: the caller computes what it stores outside the lock
  void updateStats(const std::function<void(MaterialServiceStats&)>& update)
  {
    {
      auto lock = std::scoped_lock{m_shared.mutex};
      update(m_shared.stats);
    }
    m_shared.changed.notify_all();
  }

  void requestAutoReload() { m_autoReloadPending = true; }

  bool setSlot(const std::string& name, std::shared_ptr<const MaterialSlot> slot)
  {
    if (m_working.materials.find(name))
    {
      return false;
    }
    m_working.materials = m_working.materials.set(name, std::move(slot));
    return true;
  }

  // one lookup into the working table, under the asked name and the decl's own
  bool lookUpOne(const std::string& name)
  {
    if (m_working.materials.find(name))
    {
      return false;
    }

    auto result = m_decls->lookup(name);
    auto slot = std::make_shared<MaterialSlot>();
    if (result)
    {
      slot->entry = *result.value;
    }
    else
    {
      slot->error = result.error;
    }
    const auto canonical = slot->entry ? slot->entry->name : std::string{};
    auto changed = setSlot(name, slot);
    if (!canonical.empty() && canonical != name)
    {
      changed = setSlot(canonical, slot) || changed;
    }
    return changed;
  }

  void resetWorkingTable(const std::uint64_t registry)
  {
    m_working = MaterialTable{};
    m_working.registry = registry;
    m_deltaOrder.clear();
    m_bulkMaterial = 0;
    m_bulkTable = 0;
    m_bulkDone = false;
    m_bulkStarted.reset();
  }

  void build(const std::vector<std::string>& previousNames)
  {
    const auto started = Clock::now();
    m_warnings = 0;
    m_buildError.clear();

    // a build that throws leaves no registry and publishes an empty table
    resetWorkingTable(0);
    m_decls.reset();
    m_source.reset();
    try
    {
      buildRegistry(previousNames, started);
    }
    catch (const std::exception& e)
    {
      buildFailed(e.what());
      throw;
    }
    catch (...)
    {
      buildFailed("an unknown exception");
      throw;
    }
  }

  /**
   * W.4: after a build that threw, every lookup and evaluation is answered with the
   * failure, and the loose files are still polled through the source, when there is one,
   * so that a changed file builds again (at most once per autoReloadInterval). A source
   * that could not be made leaves nothing to poll; reload() builds again.
   */
  void buildFailed(const char* reason)
  {
    m_buildError = fmt::format("The material registry could not be built: {}", reason);
    m_decls.reset();
    resetWorkingTable(0);
    m_bulkDone = true;
    publish();

    if (m_source)
    {
      try
      {
        m_source->closeFiles();
        m_stamp = looseStamp(*m_source);
      }
      catch (...)
      {
        // a source that cannot stamp its files cannot be polled
        m_source.reset();
      }
    }
    m_nextPoll = Clock::now() + m_shared.options.pollInterval;
  }

  std::string buildFailure() const
  {
    return m_buildError.empty() ? "There is no material registry." : m_buildError;
  }

  std::shared_ptr<const MaterialSlot> failedSlot() const
  {
    auto slot = std::make_shared<MaterialSlot>();
    slot->error = buildFailure();
    return slot;
  }

  void buildRegistry(
    const std::vector<std::string>& previousNames, const Clock::time_point started)
  {
    m_source =
      m_shared.options.makeSource ? m_shared.options.makeSource(m_shared.stop) : nullptr;
    if (!m_source)
    {
      m_source = std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{});
    }
    const auto sourceSeconds = secondsSince(started);

    // A2.3: what the loose files were before the read and after it; a change between
    // them, or a file that kept changing while it was read, asks for one more build
    const auto stampBefore = looseStamp(*m_source);
    // the decl files are read here; the stop flag cuts them short (R5.20)
    m_decls = std::make_unique<MaterialDecls>(*m_source, &m_shared.stop);
    // the read batch ends: no container stays open (R4-3.26)
    m_source->closeFiles();
    const auto buildSeconds = secondsSince(started);
    m_stamp = looseStamp(*m_source);

    if (!m_decls->constructionError().empty())
    {
      message(DeclMessageKind::Error, m_decls->constructionError());
    }

    const auto cancelled = m_decls->cancelled() || stopping();
    if (!cancelled && (stampBefore != m_stamp || m_source->changedWhileRead()))
    {
      requestAutoReload();
    }

    // a new table for the new registry
    resetWorkingTable(m_decls->id());

    // what the consumers asked for before is there at the switch
    for (const auto& name : previousNames)
    {
      if (stopping())
      {
        break;
      }
      lookUpOne(name);
    }
    drainMessages();
    publish();

    const auto counts = m_decls->declCounts();
    const auto materials = m_decls->materialCount();
    const auto tables = m_decls->tableCount();
    const auto textBytes = m_decls->declTextBytes();
    const auto files = m_decls->declFileCount();
    const auto fileBytes = m_decls->declFileBytes();
    const auto registry = m_decls->id();
    updateStats([&](MaterialServiceStats& stats) {
      ++stats.builds;
      stats.registry = registry;
      stats.sourceSeconds = sourceSeconds;
      stats.buildSeconds = buildSeconds;
      stats.parseAllSeconds = 0.0;
      stats.parseAllDone = false;
      stats.buildCancelled = cancelled;
      stats.materials = materials;
      stats.tables = tables;
      stats.defaultedMaterials = 0;
      stats.declTextBytes = textBytes;
      stats.declFiles = files;
      stats.declFileBytes = fileBytes;
      stats.declCountsAtBuild = counts;
      stats.declCounts = counts;
    });

    if (!cancelled)
    {
      message(
        DeclMessageKind::Info,
        fmt::format(
          "Read {} materials and {} tables ({:.1f} MB of decl text) in {:.0f} ms",
          materials,
          tables,
          double(textBytes) / (1024.0 * 1024.0),
          buildSeconds * 1000.0));
    }
    m_nextPoll = Clock::now() + m_shared.options.pollInterval;
  }

  void rebuild()
  {
    auto previousNames =
      std::vector<std::string>{m_requestedNames.begin(), m_requestedNames.end()};
    build(previousNames);
  }

  void lookUp(const std::vector<std::string>& names)
  {
    auto changed = false;
    for (const auto& name : names)
    {
      if (stopping())
      {
        return;
      }
      // the next build looks it up again, also after a failed one
      m_requestedNames.insert(name);
      // W.4: without a registry the lookup fails at once, so no caller waits for it
      changed = (m_decls ? lookUpOne(name) : setSlot(name, failedSlot())) || changed;
    }
    if (changed)
    {
      publish();
    }
  }

  void setRegisters(
    const RegisterKey& key, std::shared_ptr<const EvaluatedRegisters> registers)
  {
    m_working.registers = m_working.registers.set(key, std::move(registers));
    m_deltaOrder.push_back(key);
    while (m_deltaOrder.size() > MaterialTable::MaxRegisterDeltas)
    {
      m_working.registers = m_working.registers.erase(m_deltaOrder.front());
      m_deltaOrder.pop_front();
    }
  }

  void evaluate(const std::vector<RegisterKey>& keys)
  {
    auto changed = false;
    auto refused = std::uint64_t{0};
    for (const auto& key : keys)
    {
      if (stopping())
      {
        return;
      }
      if (m_working.registers.find(key))
      {
        continue;
      }

      // W.4: without a registry the evaluation is refused with the build's failure
      if (!m_decls)
      {
        ++refused;
        setRegisters(key, refusal(buildFailure()));
        changed = true;
        continue;
      }
      // A2.13: a refusal is published too, so the caller stops asking
      if (key.material.registry != m_decls->id())
      {
        ++refused;
        setRegisters(key, refusal("the material belongs to an earlier registry"));
        changed = true;
        continue;
      }
      const auto entry = m_decls->lookupIndex(key.material.index);
      if (!entry)
      {
        ++refused;
        setRegisters(key, refusal(entry.error));
        changed = true;
        continue;
      }
      auto registers = m_decls->evaluate(**entry.value, key.shaderParms, key.time);
      if (!registers)
      {
        ++refused;
        setRegisters(key, refusal(registers.error));
        changed = true;
        continue;
      }

      setRegisters(
        key, std::make_shared<const EvaluatedRegisters>(std::move(*registers.value)));
      changed = true;
    }

    if (refused)
    {
      updateStats(
        [&](MaterialServiceStats& stats) { stats.refusedEvaluations += refused; });
    }
    if (changed)
    {
      publish();
    }
  }

  void poll()
  {
    m_nextPoll = Clock::now() + m_shared.options.pollInterval;
    if (m_source && looseStamp(*m_source) != m_stamp)
    {
      // a changed loose file: one build, at most once per autoReloadInterval
      requestAutoReload();
    }
  }

  void bulkStep()
  {
    if (m_shared.options.beforeBulkStepForTesting)
    {
      m_shared.options.beforeBulkStepForTesting();
    }
    if (!m_bulkStarted)
    {
      m_bulkStarted = Clock::now();
    }

    const auto deadline = Clock::now() + BulkStepTime;
    auto changed = false;
    while (!stopping() && Clock::now() < deadline)
    {
      if (m_bulkMaterial < m_decls->materialCount())
      {
        const auto index = m_bulkMaterial++;
        const auto result = m_decls->lookupIndex(index);
        auto slot = std::make_shared<MaterialSlot>();
        if (result)
        {
          slot->entry = *result.value;
        }
        else
        {
          slot->error = result.error;
        }
        changed = setSlot(m_decls->materialName(index), std::move(slot)) || changed;
      }
      else if (m_bulkTable < m_decls->tableCount())
      {
        // a failed parse is reported by its messages
        static_cast<void>(m_decls->parseTable(m_bulkTable++));
      }
      else
      {
        finishBulk();
        return;
      }
    }

    if (changed)
    {
      publish();
    }
  }

  // G17: the complete table and the summary come before parseAllDone, so a waiter that
  // stops on parseAllDone has both
  void finishBulk()
  {
    m_bulkDone = true;
    m_working.complete = true;
    publish();

    const auto seconds = m_bulkStarted ? secondsSince(*m_bulkStarted) : 0.0;
    const auto defaulted = m_decls->defaultedCount("material");
    const auto materials = m_decls->materialCount();
    const auto tables = m_decls->tableCount();
    const auto textBytes = m_decls->declTextBytes();
    const auto counts = m_decls->declCounts();
    drainMessages();
    message(
      DeclMessageKind::Info,
      fmt::format(
        "Parsed {} materials ({} defaulted) and {} tables in {:.0f} ms, {} warnings",
        materials,
        defaulted,
        tables,
        seconds * 1000.0,
        m_warnings));
    updateStats([&](MaterialServiceStats& stats) {
      stats.parseAllDone = true;
      stats.parseAllSeconds = seconds;
      stats.materials = materials;
      stats.tables = tables;
      stats.defaultedMaterials = defaulted;
      stats.declTextBytes = textBytes;
      stats.declCounts = counts;
      stats.warnings = m_warnings;
    });
  }
};

Result<std::unique_ptr<MaterialService>> MaterialService::create(
  MaterialServiceOptions options)
{
  auto service =
    std::unique_ptr<MaterialService>{new MaterialService{std::move(options)}};
  return service->m_worker->start() | kdl::transform([&]() { return std::move(service); })
         | kdl::or_else([](const auto& e) -> Result<std::unique_ptr<MaterialService>> {
             return Error{"The material parse thread could not be started: " + e.msg};
           });
}

MaterialService::MaterialService(MaterialServiceOptions options)
  : m_shared{std::make_unique<Shared>(std::move(options))}
  , m_worker{std::make_unique<Worker>(*m_shared)}
{
  m_shared->parseAllRequested = m_shared->options.parseAll;
}

MaterialService::~MaterialService()
{
  requestStop();
  join();
}

std::shared_ptr<const MaterialTable> MaterialService::table() const
{
  auto lock = std::scoped_lock{m_shared->mutex};
  return m_shared->table;
}

MaterialLookup MaterialService::lookup(const std::string_view name)
{
  if (const auto slot = table()->find(name))
  {
    return slot->entry
             ? MaterialLookup{MaterialLookup::State::Ready, slot->entry, {}}
             : MaterialLookup{MaterialLookup::State::Failed, nullptr, slot->error};
  }

  {
    auto lock = std::scoped_lock{m_shared->mutex};
    auto text = std::string{name};
    if (!m_shared->stop && m_shared->queuedLookups.insert(text).second)
    {
      m_shared->lookups.push_back(std::move(text));
    }
  }
  m_shared->requested.notify_one();
  return MaterialLookup{};
}

std::shared_ptr<const EvaluatedRegisters> MaterialService::evaluate(
  const MaterialId material, const ShaderParms& shaderParms, const float time)
{
  const auto key = RegisterKey{material, shaderParms, time};
  if (auto registers = table()->findRegisters(key))
  {
    return registers;
  }

  {
    auto lock = std::scoped_lock{m_shared->mutex};
    if (!m_shared->stop && m_shared->queuedEvaluations.insert(key).second)
    {
      m_shared->evaluations.push_back(key);
    }
  }
  m_shared->requested.notify_one();
  return nullptr;
}

void MaterialService::parseAll()
{
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    m_shared->parseAllRequested = true;
  }
  m_shared->requested.notify_one();
}

void MaterialService::reload()
{
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    m_shared->reloadRequested = true;
  }
  m_shared->requested.notify_one();
}

void MaterialService::requestStop()
{
  {
    // under the lock, so that the thread cannot miss it between its check and its wait
    auto lock = std::scoped_lock{m_shared->mutex};
    m_shared->stop = true;
  }
  m_shared->requested.notify_all();
}

bool MaterialService::stopRequested() const
{
  return m_shared->stop;
}

void MaterialService::join()
{
  m_worker->join();
}

MaterialServiceStats MaterialService::stats() const
{
  auto lock = std::scoped_lock{m_shared->mutex};
  return m_shared->stats;
}

std::shared_ptr<const MaterialTable> MaterialService::waitForTableForTesting(
  const std::function<bool(const MaterialTable&)>& predicate,
  const std::chrono::milliseconds timeout) const
{
  auto lock = std::unique_lock{m_shared->mutex};
  m_shared->changed.wait_for(lock, timeout, [&] { return predicate(*m_shared->table); });
  return m_shared->table;
}

bool MaterialService::waitForStatsForTesting(
  const std::function<bool(const MaterialServiceStats&)>& predicate,
  const std::chrono::milliseconds timeout) const
{
  auto lock = std::unique_lock{m_shared->mutex};
  return m_shared->changed.wait_for(
    lock, timeout, [&] { return predicate(m_shared->stats); });
}

} // namespace tb::lit
