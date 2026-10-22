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
#include "lit/MapBake.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
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

// Y.1: what a request is answered with after the thread ended, when not even the reason
// could be made
constexpr auto ThreadEndedText = "The material parse thread stopped by itself";

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

std::size_t MaterialImageKeyHash::operator()(const MaterialImageKey& key) const
{
  auto hash = std::uint64_t(std::hash<std::string>{}(key.name));
  for (const auto value :
       {key.usage,
        key.filter,
        key.repeat,
        key.cube,
        key.cubeMapSize,
        int(key.intrinsic),
        int(key.isDefault)})
  {
    hash = mixHash(hash, std::uint64_t(std::uint32_t(value)));
  }
  return std::size_t(hash);
}

std::shared_ptr<const ImageCacheSlot> MaterialTable::findImage(
  const MaterialImageKey& key) const
{
  const auto* slot = images.find(key);
  return slot ? *slot : nullptr;
}

std::shared_ptr<const MaterialSlot> MaterialTable::find(const std::string_view name) const
{
  // the transparent hash takes the view as it is
  const auto* slot = materials.find(name);
  return slot ? *slot : nullptr;
}

std::shared_ptr<const MapBakeSlot> MaterialTable::findMapBake(
  const std::string_view path) const
{
  const auto* slot = mapBakes.find(path);
  return slot ? *slot : nullptr;
}

std::shared_ptr<const EntityDefSlot> MaterialTable::findEntityDef(
  const std::string_view classname) const
{
  const auto* slot = entityDefs.find(classname);
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
  // the lookup queue: material names, and entityDef names (C22)
  std::deque<std::string> lookups;
  std::unordered_set<std::string> queuedLookups;
  std::deque<std::string> entityDefLookups;
  std::unordered_set<std::string> queuedEntityDefLookups;
  std::deque<RegisterKey> evaluations;
  std::unordered_set<RegisterKey, RegisterKeyHash> queuedEvaluations;
  // maps whose bakes to read (C10), with the lookups, each with its latest ticket
  std::deque<std::string> mapBakes;
  std::unordered_map<std::string, std::uint64_t> queuedMapBakes;
  std::uint64_t lastTicket = 0;
  // LP1c.1: image caches to read, with the lookups
  std::deque<MaterialImageKey> images;
  std::unordered_set<MaterialImageKey, MaterialImageKeyHash> queuedImages;

  std::shared_ptr<const MaterialTable> table = std::make_shared<const MaterialTable>();
  MaterialServiceStats stats;
  // X.2: once the thread has ended without a stop (stats.threadEnded), the refusal every
  // later evaluation gets; Y.1: the one made with the service stands in until the
  // detailed one is made, and for good when it cannot be
  const std::shared_ptr<const EvaluatedRegisters> threadEndedRefusal =
    refusal(ThreadEndedText);
  std::shared_ptr<const EvaluatedRegisters> threadRefusal;

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
    std::vector<std::string> entityDefLookups;
    std::vector<RegisterKey> evaluations;
    std::vector<std::pair<std::string, std::uint64_t>> mapBakes;
    std::vector<MaterialImageKey> images;
  };

  // R1.2: default-constructed, then the kind, so that a new member needs no initialiser
  // here (-Wmissing-field-initializers)
  static Work workOf(const WorkKind kind)
  {
    auto work = Work{};
    work.kind = kind;
    return work;
  }

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
  // the names lookups and entityDef queries were asked for, asked again after a reload
  std::unordered_set<std::string> m_requestedNames;
  std::unordered_set<std::string> m_requestedEntityDefs;
  // C21, D52: the last Ready editor-image size of each material, by canonical name, kept
  // across reloads, so that a Default size never replaces it
  std::unordered_map<std::string, EditorImageSize> m_readySizes;
  // the loose files' times when the last build ended
  std::vector<DeclFileTime> m_stamp;
  Clock::time_point m_nextPoll;
  // a rebuild that a change of the loose files asks for, and when it may run
  bool m_autoReloadPending = false;
  std::optional<Clock::time_point> m_lastAutoReload;
  std::size_t m_warnings = 0;
  // the last failure a guard logged
  std::string m_lastFailure;

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
   * build succeeds (W.4). False when f threw.
   */
  template <typename F>
  bool guarded(const char* what, F&& f)
  {
    try
    {
      f();
      return true;
    }
    catch (const std::exception& e)
    {
      failed(what, e.what());
    }
    catch (...)
    {
      failed(what, "an unknown exception");
    }
    return false;
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
      m_lastFailure =
        fmt::format("The material parse thread's {} failed: {}", what, reason);
      message(DeclMessageKind::Error, m_lastFailure);
    }
    catch (...)
    {
    }
  }

  /**
   * X.2: the thread ends without a stop, so nothing will take a request again. Every
   * later lookup and entityDef query is answered Failed and every later evaluation
   * refused, by the calling thread (MaterialService::lookup, entityDef, evaluate), and
   * the requests still queued are dropped: their callers get the same answer when they
   * ask again.
   *
   * Y.1: it may run because memory ran out, so it first sets stats.threadEnded, which
   * needs no allocation, and clears the queues; the answers then use the refusal made
   * with the service. Only after that does it try to say why, which may fail.
   */
  void endWithoutStop() noexcept
  {
    if (stopping())
    {
      return;
    }
    try
    {
      {
        auto lock = std::scoped_lock{m_shared.mutex};
        m_shared.stats.threadEnded = true;
        m_shared.lookups.clear();
        m_shared.queuedLookups.clear();
        m_shared.entityDefLookups.clear();
        m_shared.queuedEntityDefLookups.clear();
        m_shared.evaluations.clear();
        m_shared.queuedEvaluations.clear();
        m_shared.images.clear();
        m_shared.queuedImages.clear();
      }
      m_shared.changed.notify_all();
    }
    catch (...)
    {
    }

    try
    {
      if (m_shared.options.beforeEndDescriptionForTesting)
      {
        m_shared.options.beforeEndDescriptionForTesting();
      }
      auto reason = m_lastFailure.empty() ? std::string{ThreadEndedText} : m_lastFailure;
      auto refused = refusal(reason);
      {
        auto lock = std::scoped_lock{m_shared.mutex};
        m_shared.stats.threadError = std::move(reason);
        m_shared.threadRefusal = std::move(refused);
      }
      m_shared.changed.notify_all();
    }
    catch (...)
    {
    }
  }

  /**
   * W.5: what run lets escape (only an allocation failure outside every guard) ends the
   * thread, never the process. The registry is still destroyed on its own thread (C20),
   * the reason reaches the listener, and every later request fails with it (X.2).
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
    endWithoutStop();
  }

  void run()
  {
    guarded("build", [&] { build({}, {}); });
    for (;;)
    {
      // W.5, X.2: a request the thread cannot take (an allocation failure while it copies
      // the queue) ends the loop rather than trying it again at once, and every later
      // request fails
      auto work = Work{};
      if (!guarded("request", [&] { work = takeWork(); }))
      {
        endWithoutStop();
        break;
      }
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
        guarded("lookup", [&] {
          lookUp(work.lookups, work.entityDefLookups, work.mapBakes, work.images);
        });
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

    // teardown step 8: the registry is destroyed on its own thread, and what the thread
    // reported to the sink of D63 after it is dropped
    guarded("teardown", [&] {
      m_decls.reset();
      m_source.reset();
      static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    });
  }

  Clock::time_point autoReloadDue() const
  {
    return m_lastAutoReload ? *m_lastAutoReload + m_shared.options.autoReloadInterval
                            : Clock::time_point{};
  }

  Work takeWork()
  {
    if (m_shared.options.beforeTakeWorkForTesting)
    {
      m_shared.options.beforeTakeWorkForTesting();
    }
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
      return workOf(WorkKind::Stop);
    }
    if (m_shared.reloadRequested)
    {
      m_shared.reloadRequested = false;
      m_autoReloadPending = false;
      return workOf(WorkKind::Reload);
    }
    if (m_autoReloadPending && Clock::now() >= autoReloadDue())
    {
      m_autoReloadPending = false;
      return workOf(WorkKind::AutoReload);
    }
    if (!m_shared.evaluations.empty())
    {
      auto work = workOf(WorkKind::Evaluate);
      work.evaluations.assign(m_shared.evaluations.begin(), m_shared.evaluations.end());
      m_shared.evaluations.clear();
      m_shared.queuedEvaluations.clear();
      return work;
    }
    if (
      !m_shared.lookups.empty() || !m_shared.entityDefLookups.empty()
      || !m_shared.mapBakes.empty() || !m_shared.images.empty())
    {
      auto work = workOf(WorkKind::Lookup);
      work.lookups.assign(m_shared.lookups.begin(), m_shared.lookups.end());
      work.entityDefLookups.assign(
        m_shared.entityDefLookups.begin(), m_shared.entityDefLookups.end());
      for (auto& path : m_shared.mapBakes)
      {
        work.mapBakes.emplace_back(path, m_shared.queuedMapBakes.at(path));
      }
      work.images.assign(m_shared.images.begin(), m_shared.images.end());
      m_shared.lookups.clear();
      m_shared.queuedLookups.clear();
      m_shared.entityDefLookups.clear();
      m_shared.queuedEntityDefLookups.clear();
      m_shared.mapBakes.clear();
      m_shared.queuedMapBakes.clear();
      m_shared.images.clear();
      m_shared.queuedImages.clear();
      return work;
    }
    if (
      !m_autoReloadPending && m_shared.options.pollInterval.count() > 0
      && Clock::now() >= m_nextPoll)
    {
      return workOf(WorkKind::Poll);
    }
    if (m_shared.parseAllRequested && bulkPending())
    {
      return workOf(WorkKind::Bulk);
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

  /**
   * C21, D52: the entry as it is published, with its editor-image size: the registry's
   * (or the test hook's); a Default size gives way to the last Ready one of the same
   * material, kept across reloads, and a Ready one is kept for later.
   */
  std::shared_ptr<const MaterialEntry> sized(std::shared_ptr<const MaterialEntry> entry)
  {
    auto size = m_shared.options.editorImageSizeForTesting
                  ? m_shared.options.editorImageSizeForTesting(*entry)
                  : entry->editorImageSize;
    if (size.state == EditorImageSize::State::Ready)
    {
      m_readySizes.insert_or_assign(entry->name, size);
    }
    else if (const auto kept = m_readySizes.find(entry->name); kept != m_readySizes.end())
    {
      size = kept->second;
    }
    if (size == entry->editorImageSize)
    {
      return entry;
    }

    auto copy = std::make_shared<MaterialEntry>(*entry);
    copy->editorImageSize = size;
    return copy;
  }

  // one lookup into the working table, under the asked name and the decl's own
  bool lookUpOne(const std::string& name)
  {
    if (m_working.materials.find(name))
    {
      return false;
    }

    auto result = m_decls->lookup(name);
    auto slot = std::shared_ptr<const MaterialSlot>{};
    if (result)
    {
      // another spelling of a material that is published already shares its entry
      const auto* published = m_working.materials.find((*result.value)->name);
      if (published && (*published)->entry)
      {
        slot = *published;
      }
      else
      {
        auto made = std::make_shared<MaterialSlot>();
        made->entry = sized(*result.value);
        slot = std::move(made);
      }
    }
    else
    {
      auto made = std::make_shared<MaterialSlot>();
      made->error = result.error;
      slot = std::move(made);
    }
    const auto canonical = slot->entry ? slot->entry->name : std::string{};
    auto changed = setSlot(name, slot);
    if (!canonical.empty() && canonical != name)
    {
      changed = setSlot(canonical, slot) || changed;
    }
    return changed;
  }

  bool setEntityDefSlot(
    const std::string& name, std::shared_ptr<const EntityDefSlot> slot)
  {
    if (m_working.entityDefs.find(name))
    {
      return false;
    }
    m_working.entityDefs = m_working.entityDefs.set(name, std::move(slot));
    return true;
  }

  // C22: one entityDef query into the working table, under the asked name and the decl's
  bool lookUpEntityDef(const std::string& name)
  {
    if (m_working.entityDefs.find(name))
    {
      return false;
    }

    auto result = m_decls->entityDef(name);
    auto slot = std::shared_ptr<const EntityDefSlot>{};
    if (result)
    {
      // another spelling of an entityDef that is published already shares its entry
      const auto* published = m_working.entityDefs.find((*result.value)->name);
      if (published && (*published)->entry)
      {
        slot = *published;
      }
      else
      {
        auto made = std::make_shared<EntityDefSlot>();
        made->entry = std::move(*result.value);
        slot = std::move(made);
      }
    }
    else
    {
      auto made = std::make_shared<EntityDefSlot>();
      made->error = result.error;
      slot = std::move(made);
    }
    const auto canonical = slot->entry ? slot->entry->name : std::string{};
    auto changed = setEntityDefSlot(name, slot);
    if (!canonical.empty() && canonical != name)
    {
      changed = setEntityDefSlot(canonical, slot) || changed;
    }
    return changed;
  }

  void resetWorkingTable(const std::uint64_t registry)
  {
    // the bakes and the image caches do not depend on the registry
    auto mapBakes = std::move(m_working.mapBakes);
    auto images = std::move(m_working.images);
    m_working = MaterialTable{};
    m_working.registry = registry;
    m_working.mapBakes = std::move(mapBakes);
    m_working.images = std::move(images);
    m_deltaOrder.clear();
    m_bulkMaterial = 0;
    m_bulkTable = 0;
    m_bulkDone = false;
    m_bulkStarted.reset();
  }

  void build(
    const std::vector<std::string>& previousNames,
    const std::vector<std::string>& previousEntityDefs)
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
      buildRegistry(previousNames, previousEntityDefs, started);
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

  std::shared_ptr<const EntityDefSlot> failedEntityDefSlot() const
  {
    auto slot = std::make_shared<EntityDefSlot>();
    slot->error = buildFailure();
    return slot;
  }

  void buildRegistry(
    const std::vector<std::string>& previousNames,
    const std::vector<std::string>& previousEntityDefs,
    const Clock::time_point started)
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
    for (const auto& name : previousEntityDefs)
    {
      if (stopping())
      {
        break;
      }
      lookUpEntityDef(name);
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
    const auto containers = m_source->containerCount();
    updateStats([&](MaterialServiceStats& stats) {
      ++stats.builds;
      stats.registry = registry;
      stats.sourceSeconds = sourceSeconds;
      stats.buildSeconds = buildSeconds;
      stats.parseAllSeconds = 0.0;
      stats.parseAllDone = false;
      stats.buildCancelled = cancelled;
      stats.sourceContainers = containers;
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
    auto previousEntityDefs = std::vector<std::string>{
      m_requestedEntityDefs.begin(), m_requestedEntityDefs.end()};
    build(previousNames, previousEntityDefs);
  }

  /**
   * C10 (LP1c.1, D64): a map's bake into the working table, under its document path,
   * answering ticket: the C10 search through the game files (readMapBake), the text files
   * with the copied lexer, on this thread (C20). A bake whose files did not change keeps
   * its tree and data (the same pointers), so that a consumer that asked again rebuilds
   * nothing; a path outside the game folder gets a slot without a tree.
   */
  void loadMapBake(const std::string& path, const std::uint64_t ticket)
  {
    auto slot = std::make_shared<MapBakeSlot>();
    slot->path = path;
    if (!m_source)
    {
      slot->error = buildFailure();
    }
    else if (const auto name = engineMapName(*m_source, path))
    {
      const auto* kept = m_working.mapBakes.find(path);
      if (kept && (*kept)->bake && !mapBakeChanged(*m_source, *(*kept)->bake))
      {
        *slot = **kept;
      }
      else
      {
        auto bake = std::make_shared<const MapBake>(readMapBake(*m_source, *name));
        slot->tree = bake->tree;
        slot->error = bake->geometryError;
        slot->bake = std::move(bake);
      }
    }
    else
    {
      slot->error = path
                    + " is not below the game's base folder, so the engine cannot have "
                      "baked it (C10)";
    }
    slot->ticket = ticket;
    m_working.mapBakes =
      m_working.mapBakes.set(path, std::shared_ptr<const MapBakeSlot>{std::move(slot)});
  }

  // the bakes whose files changed since they were read are read again (a new dmap run)
  bool pollMapBakes()
  {
    if (!m_source)
    {
      return false;
    }
    auto changed = std::vector<std::pair<std::string, std::uint64_t>>{};
    m_working.mapBakes.forEach([&](const std::string& path, const auto& slot) {
      if (slot->bake && mapBakeChanged(*m_source, *slot->bake))
      {
        changed.emplace_back(path, slot->ticket);
      }
    });
    for (const auto& [path, ticket] : changed)
    {
      loadMapBake(path, ticket);
    }
    return !changed.empty();
  }

  /**
   * LP1c.1 (C8): one image's cache into the working table: the copied timestamp call
   * names it, the reader reads it through the game-file search. Without a registry (a
   * build failed) the image is Missing with the build's failure.
   */
  bool lookUpImage(const MaterialImageKey& key)
  {
    if (m_working.images.find(key))
    {
      return false;
    }
    auto slot = std::make_shared<ImageCacheSlot>();
    if (!m_decls || !m_source)
    {
      slot->reason = buildFailure();
    }
    else if (auto name = m_decls->imageCacheName(key))
    {
      *slot = resolveImageCache(*m_source, *name.value);
    }
    else
    {
      slot->reason = std::move(name.error);
    }
    m_working.images =
      m_working.images.set(key, std::shared_ptr<const ImageCacheSlot>{std::move(slot)});
    return true;
  }

  void lookUp(
    const std::vector<std::string>& names,
    const std::vector<std::string>& entityDefNames,
    const std::vector<std::pair<std::string, std::uint64_t>>& mapBakes = {},
    const std::vector<MaterialImageKey>& images = {})
  {
    auto changed = false;
    for (const auto& [path, ticket] : mapBakes)
    {
      if (stopping())
      {
        return;
      }
      loadMapBake(path, ticket);
      changed = true;
    }
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
    for (const auto& name : entityDefNames)
    {
      if (stopping())
      {
        return;
      }
      m_requestedEntityDefs.insert(name);
      changed =
        (m_decls ? lookUpEntityDef(name) : setEntityDefSlot(name, failedEntityDefSlot()))
        || changed;
    }
    for (const auto& key : images)
    {
      if (stopping())
      {
        return;
      }
      changed = lookUpImage(key) || changed;
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
    // C10, §3.7: a bake file that changed (or failed to parse and changed since) is read
    // again
    if (pollMapBakes())
    {
      publish();
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
          slot->entry = sized(*result.value);
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

  auto queued = false;
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    // X.2, Y.1: no thread will take the lookup
    if (m_shared->stats.threadEnded)
    {
      return MaterialLookup{
        MaterialLookup::State::Failed,
        nullptr,
        m_shared->stats.threadError.empty() ? std::string{ThreadEndedText}
                                            : m_shared->stats.threadError};
    }
    auto text = std::string{name};
    if (!m_shared->stop && m_shared->queuedLookups.insert(text).second)
    {
      m_shared->lookups.push_back(std::move(text));
      queued = true;
    }
  }
  // B0.13: a name already queued wakes nobody
  if (queued)
  {
    m_shared->requested.notify_one();
  }
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

  auto queued = false;
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    // X.2, Y.1: no thread will take the evaluation
    if (m_shared->stats.threadEnded)
    {
      return m_shared->threadRefusal ? m_shared->threadRefusal
                                     : m_shared->threadEndedRefusal;
    }
    if (!m_shared->stop && m_shared->queuedEvaluations.insert(key).second)
    {
      m_shared->evaluations.push_back(key);
      queued = true;
    }
  }
  if (queued)
  {
    m_shared->requested.notify_one();
  }
  return nullptr;
}

EntityDefLookup MaterialService::entityDef(const std::string_view classname)
{
  if (const auto slot = table()->findEntityDef(classname))
  {
    return slot->entry
             ? EntityDefLookup{EntityDefLookup::State::Ready, slot->entry, {}}
             : EntityDefLookup{EntityDefLookup::State::Failed, nullptr, slot->error};
  }

  auto queued = false;
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    // X.2, Y.1: no thread will take the query
    if (m_shared->stats.threadEnded)
    {
      return EntityDefLookup{
        EntityDefLookup::State::Failed,
        nullptr,
        m_shared->stats.threadError.empty() ? std::string{ThreadEndedText}
                                            : m_shared->stats.threadError};
    }
    auto text = std::string{classname};
    if (!m_shared->stop && m_shared->queuedEntityDefLookups.insert(text).second)
    {
      m_shared->entityDefLookups.push_back(std::move(text));
      queued = true;
    }
  }
  if (queued)
  {
    m_shared->requested.notify_one();
  }
  return EntityDefLookup{};
}

std::uint64_t MaterialService::loadMapBake(const std::string_view mapPath)
{
  auto queued = false;
  auto ticket = std::uint64_t{0};
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    ticket = ++m_shared->lastTicket;
    auto text = std::string{mapPath};
    if (!m_shared->stop && !m_shared->stats.threadEnded)
    {
      if (m_shared->queuedMapBakes.insert_or_assign(text, ticket).second)
      {
        m_shared->mapBakes.push_back(std::move(text));
      }
      queued = true;
    }
  }
  if (queued)
  {
    m_shared->requested.notify_one();
  }
  return ticket;
}

ImageLookup MaterialService::image(const MaterialImageKey& key)
{
  if (const auto slot = table()->findImage(key))
  {
    switch (slot->state)
    {
    case ImageCacheSlot::State::Ready:
      return ImageLookup{ImageLookup::State::Ready, slot->image, slot};
    case ImageCacheSlot::State::Missing:
      return ImageLookup{ImageLookup::State::Missing, nullptr, slot};
    case ImageCacheSlot::State::Stale:
      return ImageLookup{ImageLookup::State::Stale, nullptr, slot};
    }
  }

  auto queued = false;
  {
    auto lock = std::scoped_lock{m_shared->mutex};
    // X.2, Y.1: no thread will read it, and the fallback has no registry either
    if (m_shared->stats.threadEnded)
    {
      auto slot = std::make_shared<ImageCacheSlot>();
      slot->reason = m_shared->stats.threadError.empty() ? std::string{ThreadEndedText}
                                                         : m_shared->stats.threadError;
      return ImageLookup{ImageLookup::State::Missing, nullptr, std::move(slot)};
    }
    if (!m_shared->stop && m_shared->queuedImages.insert(key).second)
    {
      m_shared->images.push_back(key);
      queued = true;
    }
  }
  if (queued)
  {
    m_shared->requested.notify_one();
  }
  return ImageLookup{};
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
