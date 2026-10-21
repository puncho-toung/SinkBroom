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
#include "lit/ResourceContainer.h"

#include <fmt/format.h>

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

// R4-1.1, R4-3.25: the parse thread's stack where the platform does not give the
// executable's
constexpr auto PosixStackBytes = std::size_t{2} * 1024 * 1024;

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

/**
 * Forwards to the source and records the time each read returned, so that a build can
 * tell whether a loose decl file changed while it was reading.
 */
class RecordingSource : public DeclSource
{
private:
  DeclSource& m_source;
  std::unordered_map<std::string, std::int64_t> m_reads;

public:
  explicit RecordingSource(DeclSource& source)
    : m_source{source}
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
    auto time = std::int64_t{0};
    auto text = m_source.readFile(path, &time);
    if (timestamp)
    {
      *timestamp = time;
    }
    if (text)
    {
      const_cast<RecordingSource*>(this)->m_reads[canonicalResourceName(path)] = time;
    }
    return text;
  }

  std::vector<DeclFileTime> looseFileTimes(
    const std::string& folder, const std::string& extension) const override
  {
    return m_source.looseFileTimes(folder, extension);
  }

  std::vector<std::string> takeWarnings() override { return m_source.takeWarnings(); }

  void closeFiles() override { m_source.closeFiles(); }

  // whether stamp shows a loose file that the build did not read as it is now
  bool changedSinceRead(const std::vector<DeclFileTime>& stamp) const
  {
    for (const auto& file : stamp)
    {
      const auto read = m_reads.find(canonicalResourceName(file.path));
      if (read == m_reads.end() || read->second != file.time)
      {
        return true;
      }
    }
    return false;
  }
};

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

const MaterialSlot* MaterialTable::find(const std::string_view name) const
{
  const auto* slot = materials.find(std::string{name});
  return slot ? slot->get() : nullptr;
}

std::shared_ptr<const EvaluatedRegisters> MaterialTable::findRegisters(
  const RegisterKey& key) const
{
  const auto* found = registers.find(key);
  return found ? *found : nullptr;
}

// What every thread shares: the requests, the published table and the stats, under one
// mutex; the stop flag is also the registry's cancel flag.
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

  // the parse thread's state; the source outlives the recorder, which outlives the
  // registry that reads through it
  std::unique_ptr<DeclSource> m_source;
  std::unique_ptr<RecordingSource> m_recorder;
  std::unique_ptr<MaterialDecls> m_decls;
  MaterialTable m_working;
  std::uint64_t m_version = 0;
  std::deque<RegisterKey> m_deltaOrder;
  // the names lookups were asked for, looked up again after a reload
  std::unordered_set<std::string> m_requestedNames;
  std::vector<DeclFileTime> m_stamp;
  Clock::time_point m_nextPoll;
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

  bool start(std::string* error)
  {
    return m_thread.start([this] { run(); }, PosixStackBytes, error);
  }

  void join() { m_thread.join(); }

private:
  bool stopping() const { return m_shared.stop.load(); }

  void run()
  {
    build({});
    for (;;)
    {
      auto work = takeWork();
      if (work.kind == WorkKind::Stop)
      {
        break;
      }

      switch (work.kind)
      {
      case WorkKind::Stop:
        break;
      case WorkKind::Reload:
        rebuild();
        break;
      case WorkKind::Evaluate:
        evaluate(work.evaluations);
        break;
      case WorkKind::Lookup:
        lookUp(work.lookups);
        break;
      case WorkKind::Poll:
        poll();
        break;
      case WorkKind::Bulk:
        bulkStep();
        break;
      }

      // every work item is one read batch (R4-3.26)
      if (m_source)
      {
        m_source->closeFiles();
      }
      drainMessages();
    }

    // teardown step 8: the registry is destroyed on its own thread
    m_decls.reset();
    m_recorder.reset();
    m_source.reset();
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
      if (m_shared.options.pollInterval.count() > 0)
      {
        m_shared.requested.wait_until(lock, m_nextPoll);
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
      return Work{WorkKind::Reload, {}, {}};
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
    if (m_shared.options.pollInterval.count() > 0 && Clock::now() >= m_nextPoll)
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
    if (!stopping() && m_shared.options.listener)
    {
      m_shared.options.listener(MaterialServiceEvent{
        MaterialServiceEvent::Kind::Message, 0, level, std::move(text)});
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

    if (!stopping() && m_shared.options.listener)
    {
      m_shared.options.listener(MaterialServiceEvent{
        MaterialServiceEvent::Kind::Published, m_version, DeclMessageKind::Info, {}});
    }
  }

  void updateStats(const std::function<void(MaterialServiceStats&)>& update)
  {
    {
      auto lock = std::scoped_lock{m_shared.mutex};
      update(m_shared.stats);
    }
    m_shared.changed.notify_all();
  }

  std::vector<DeclFileTime> looseStamp() const
  {
    auto stamp = std::vector<DeclFileTime>{};
    if (!m_decls || !m_source)
    {
      return stamp;
    }
    for (const auto& folder : m_decls->declFolders())
    {
      for (auto& file : m_source->looseFileTimes(folder.folder, folder.extension))
      {
        stamp.push_back(std::move(file));
      }
    }
    return stamp;
  }

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

  void build(const std::vector<std::string>& previousNames)
  {
    const auto started = Clock::now();
    m_warnings = 0;

    m_source = m_shared.options.makeSource ? m_shared.options.makeSource() : nullptr;
    if (!m_source)
    {
      m_source = std::make_unique<MemoryDeclSource>(std::vector<DeclFile>{});
    }
    const auto sourceSeconds = secondsSince(started);
    m_recorder = std::make_unique<RecordingSource>(*m_source);
    // the decl files are read here; the stop flag cuts them short (R5.20)
    m_decls = std::make_unique<MaterialDecls>(*m_recorder, &m_shared.stop);
    // the read batch ends: no container stays open (R4-3.26)
    m_source->closeFiles();
    const auto buildSeconds = secondsSince(started);

    if (!m_decls->constructionError().empty())
    {
      message(DeclMessageKind::Error, m_decls->constructionError());
    }

    m_stamp = looseStamp();
    const auto cancelled = m_decls->cancelled();
    const auto changedWhileReading = !cancelled && m_recorder->changedSinceRead(m_stamp);

    // a new table for the new registry
    m_working = MaterialTable{};
    m_working.registry = m_decls->id();
    m_deltaOrder.clear();
    m_bulkMaterial = 0;
    m_bulkTable = 0;
    m_bulkDone = false;
    m_bulkStarted.reset();

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
    updateStats([&](MaterialServiceStats& stats) {
      ++stats.builds;
      stats.registry = m_decls->id();
      stats.sourceSeconds = sourceSeconds;
      stats.buildSeconds = buildSeconds;
      stats.parseAllSeconds = 0.0;
      stats.parseAllDone = false;
      stats.buildCancelled = cancelled;
      stats.materials = m_decls->materialCount();
      stats.tables = m_decls->tableCount();
      stats.defaultedMaterials = 0;
      stats.declTextBytes = m_decls->declTextBytes();
      stats.declFiles = m_decls->declFileCount();
      stats.declFileBytes = m_decls->declFileBytes();
      stats.declCountsAtBuild = counts;
      stats.declCounts = counts;
    });

    if (!cancelled)
    {
      message(
        DeclMessageKind::Info,
        fmt::format(
          "Read {} materials and {} tables ({:.1f} MB of decl text) in {:.0f} ms",
          m_decls->materialCount(),
          m_decls->tableCount(),
          double(m_decls->declTextBytes()) / (1024.0 * 1024.0),
          buildSeconds * 1000.0));
    }

    if (changedWhileReading)
    {
      // a loose decl file changed while the build read it: read it again
      auto lock = std::scoped_lock{m_shared.mutex};
      m_shared.reloadRequested = true;
    }
    m_nextPoll = Clock::now() + m_shared.options.pollInterval;
  }

  void rebuild()
  {
    auto previousNames =
      std::vector<std::string>{m_requestedNames.begin(), m_requestedNames.end()};

    // one registry at a time (C17)
    m_decls.reset();
    m_recorder.reset();
    m_source.reset();
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
      m_requestedNames.insert(name);
      changed = lookUpOne(name) || changed;
    }
    if (changed)
    {
      publish();
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
      if (key.material.registry != m_decls->id())
      {
        ++refused;
        continue;
      }
      if (m_working.registers.find(key))
      {
        continue;
      }

      const auto entry = m_decls->lookupIndex(key.material.index);
      if (!entry)
      {
        ++refused;
        continue;
      }
      auto registers = m_decls->evaluate(**entry.value, key.shaderParms, key.time);
      if (!registers)
      {
        continue;
      }

      m_working.registers = m_working.registers.set(
        key, std::make_shared<const EvaluatedRegisters>(std::move(*registers.value)));
      m_deltaOrder.push_back(key);
      while (m_deltaOrder.size() > MaterialTable::MaxRegisterDeltas)
      {
        m_working.registers = m_working.registers.erase(m_deltaOrder.front());
        m_deltaOrder.pop_front();
      }
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
    if (!m_decls)
    {
      return;
    }
    if (looseStamp() != m_stamp)
    {
      updateStats([](MaterialServiceStats& stats) { ++stats.pollReloads; });
      rebuild();
    }
  }

  void bulkStep()
  {
    if (m_shared.options.beforeBulkStep)
    {
      m_shared.options.beforeBulkStep();
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
        changed = true;
        break;
      }
    }

    if (changed)
    {
      publish();
    }
  }

  void finishBulk()
  {
    m_bulkDone = true;
    m_working.complete = true;

    const auto seconds = m_bulkStarted ? secondsSince(*m_bulkStarted) : 0.0;
    const auto defaulted = m_decls->defaultedCount("material");
    drainMessages();
    updateStats([&](MaterialServiceStats& stats) {
      stats.parseAllDone = true;
      stats.parseAllSeconds = seconds;
      stats.materials = m_decls->materialCount();
      stats.tables = m_decls->tableCount();
      stats.defaultedMaterials = defaulted;
      stats.declTextBytes = m_decls->declTextBytes();
      stats.declCounts = m_decls->declCounts();
    });
    message(
      DeclMessageKind::Info,
      fmt::format(
        "Parsed {} materials ({} defaulted) and {} tables in {:.0f} ms, {} warnings",
        m_decls->materialCount(),
        defaulted,
        m_decls->tableCount(),
        seconds * 1000.0,
        m_warnings));
  }
};

MaterialService::MaterialService(MaterialServiceOptions options)
  : m_shared{std::make_unique<Shared>(std::move(options))}
  , m_worker{std::make_unique<Worker>(*m_shared)}
{
  m_shared->parseAllRequested = m_shared->options.parseAll;

  auto error = std::string{};
  if (!m_worker->start(&error))
  {
    m_shared->stop = true;
    if (m_shared->options.listener)
    {
      m_shared->options.listener(MaterialServiceEvent{
        MaterialServiceEvent::Kind::Message,
        0,
        DeclMessageKind::Error,
        fmt::format("The material parse thread could not be started: {}", error)});
    }
  }
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
  const auto latest = table();
  if (const auto* slot = latest->find(name))
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

std::shared_ptr<const MaterialTable> MaterialService::waitForTable(
  const std::function<bool(const MaterialTable&)>& predicate,
  const std::chrono::milliseconds timeout) const
{
  auto lock = std::unique_lock{m_shared->mutex};
  m_shared->changed.wait_for(lock, timeout, [&] { return predicate(*m_shared->table); });
  return m_shared->table;
}

bool MaterialService::waitForStats(
  const std::function<bool(const MaterialServiceStats&)>& predicate,
  const std::chrono::milliseconds timeout) const
{
  auto lock = std::unique_lock{m_shared->mutex};
  return m_shared->changed.wait_for(
    lock, timeout, [&] { return predicate(m_shared->stats); });
}

} // namespace tb::lit
