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

#pragma once

#include "base/Macros.h"
#include "base/Result.h"
#include "lit/MaterialDecls.h"
#include "lit/PersistentMap.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{
struct ProcTree;

using ShaderParms = std::array<float, MaterialDecls::ShaderParmCount>;

// a material of one registry: the registry's id and the material's index in it
struct MaterialId
{
  std::uint64_t registry = 0;
  int index = -1;

  static MaterialId of(const MaterialEntry& entry);

  bool operator==(const MaterialId& other) const;
};

// what a lookup has found for a name: an entry, or the engine error its parse reached
struct MaterialSlot
{
  std::shared_ptr<const MaterialEntry> entry;
  std::string error;
};

// C22: what an entityDef query has found for a classname: an entry, or why there is none
// (no such entityDef, or the engine error its parse reached)
struct EntityDefSlot
{
  std::shared_ptr<const EntityDefEntry> entry;
  std::string error;
};

// D64: what reading a .proc found: its tree, or why there is none (no file, or the
// error its parse reached); published with the materials
struct ProcFileSlot
{
  std::string path;
  std::shared_ptr<const ProcTree> tree;
  std::string error;
};

// the key of an evaluation: the material, its entity shaderParms and the time; equal
// keys have equal bits
struct RegisterKey
{
  MaterialId material;
  ShaderParms shaderParms{};
  float time = 0.0f;

  bool operator==(const RegisterKey& other) const;
};

struct RegisterKeyHash
{
  std::size_t operator()(const RegisterKey& key) const;
};

// the hash of one set of entity shaderParms at a time
std::uint64_t shaderParmsHash(const ShaderParms& shaderParms, float time);

/**
 * One published version of the materials (D52, R5.16, C7): immutable, shared by every
 * thread that holds it, and freed with its last holder. It never holds an id pointer.
 *
 * - materials: what each looked-up or bulk-parsed name found, keyed by the decl's
 *   canonical name and by each name a lookup was asked with; each entry carries its
 *   editor-image size (C21), and a Ready size is kept across reloads (D52);
 * - entityDefs: what each entityDef query found (C22), keyed the same way;
 * - registers: the evaluated registers, keyed by (material, shaderParms, time), published
 *   as deltas; the oldest are dropped beyond MaxRegisterDeltas, and a holder keeps its
 *   last registers until new ones arrive.
 *
 * A new version shares every material, entityDef and delta that did not change.
 */
struct MaterialTable
{
  using Materials = PersistentMap<
    std::string,
    std::shared_ptr<const MaterialSlot>,
    StringHash,
    std::equal_to<>>;
  using EntityDefs = PersistentMap<
    std::string,
    std::shared_ptr<const EntityDefSlot>,
    StringHash,
    std::equal_to<>>;
  using Registers = PersistentMap<
    RegisterKey,
    std::shared_ptr<const EvaluatedRegisters>,
    RegisterKeyHash>;
  using ProcFiles = PersistentMap<
    std::string,
    std::shared_ptr<const ProcFileSlot>,
    StringHash,
    std::equal_to<>>;

  static constexpr std::size_t MaxRegisterDeltas = 16384;

  // increases with every publication
  std::uint64_t version = 0;
  // the registry the entries and deltas belong to (MaterialDecls::id); 0 before the first
  // build
  std::uint64_t registry = 0;
  // the bulk parse of this registry has finished: every material and table is parsed
  bool complete = false;
  Materials materials;
  EntityDefs entityDefs;
  Registers registers;
  // the .proc files read so far (loadProcFile), by path; kept across reloads
  ProcFiles procFiles;

  // the slot of a name, shared with the table, so that it outlives a table held only for
  // the call; null when the name is not looked up yet. Neither find allocates.
  std::shared_ptr<const MaterialSlot> find(std::string_view name) const;
  std::shared_ptr<const EntityDefSlot> findEntityDef(std::string_view classname) const;
  std::shared_ptr<const ProcFileSlot> findProcFile(std::string_view path) const;
  // the registers of a key; an EvaluatedRegisters with an error when the evaluation was
  // refused, null when it is not done yet
  std::shared_ptr<const EvaluatedRegisters> findRegisters(const RegisterKey& key) const;
};

// what lookup returns
struct MaterialLookup
{
  enum class State
  {
    // queued: a later table has it
    Pending,
    Ready,
    // the parse reached an engine error (MaterialSlot::error)
    Failed,
  };

  State state = State::Pending;
  std::shared_ptr<const MaterialEntry> entry;
  std::string error;
};

// what entityDef returns: Pending (queued: a later table has it), Ready, or Failed with
// the slot's error
struct EntityDefLookup
{
  using State = MaterialLookup::State;

  State state = State::Pending;
  std::shared_ptr<const EntityDefEntry> entry;
  std::string error;
};

// what the parse thread reports to its owner
struct MaterialServiceEvent
{
  enum class Kind
  {
    // a new table version is published
    Published,
    // a line for the log: the copied code's messages, the source's warnings and the
    // service's own summaries
    Message,
  };

  Kind kind = Kind::Message;
  // Published: the new table's version
  std::uint64_t version = 0;
  // Message
  DeclMessageKind level = DeclMessageKind::Info;
  std::string text;
};

// called on the parse thread; it must not block, throw or call back into the service (an
// event it throws for is dropped)
using MaterialListener = std::function<void(const MaterialServiceEvent&)>;

struct MaterialServiceOptions
{
  // makes the decl source of a registry; called on the parse thread for every build, so a
  // reload rescans the game's containers. cancel is the stop flag: a source that scans
  // stops when it is set (G4).
  std::function<std::unique_ptr<DeclSource>(const std::atomic<bool>& cancel)> makeSource;
  MaterialListener listener;
  // how often the loose decl files are compared with the last build's; 0 never
  std::chrono::milliseconds pollInterval{1000};
  // the least time between two builds that a change of the loose files starts, rather
  // than reload() (A2.3, A3.6)
  std::chrono::milliseconds autoReloadInterval{5000};
  // bulk-parse every material and table after every build (parseAll)
  bool parseAll = false;
  // runs on the parse thread before each bulk step
  std::function<void()> beforeBulkStepForTesting;
  // runs on the parse thread each time it takes a request; what it throws is a request
  // the thread cannot take (X.2)
  std::function<void()> beforeTakeWorkForTesting;
  // runs on the parse thread once the thread that ends by itself has set its end flag,
  // before it describes why; what it throws stands for that description's allocation
  // failing (Y.1)
  std::function<void()> beforeEndDescriptionForTesting;
  // runs on the parse thread when an entry is published and replaces the editor-image
  // size the registry measured (C21; Default until LP1c.2 loads images)
  std::function<EditorImageSize(const MaterialEntry&)> editorImageSizeForTesting;
};

struct MaterialServiceStats
{
  // registries built so far, and the current one's id
  std::uint64_t builds = 0;
  std::uint64_t registry = 0;
  // the last build: making the source (a game source reads its containers' tables), then
  // the whole build with reading every decl file; and the last bulk parse
  double sourceSeconds = 0.0;
  double buildSeconds = 0.0;
  double parseAllSeconds = 0.0;
  bool parseAllDone = false;
  // a build that the stop cut short
  bool buildCancelled = false;
  // the containers the last build's source holds (DeclSource::containerCount): fewer
  // than the game has when the stop cut its scan short
  std::size_t sourceContainers = 0;
  int materials = 0;
  int tables = 0;
  int defaultedMaterials = 0;
  // warnings and errors reported since the current build began
  std::size_t warnings = 0;
  std::size_t declTextBytes = 0;
  // the decl files read and their bytes, as the engine's listDecls counts them
  int declFiles = 0;
  std::size_t declFileBytes = 0;
  // the decls of each type after the build, and now: a parse can add implicit decls, as a
  // material's particle deform does for a particle no file defines
  std::vector<DeclTypeCount> declCountsAtBuild;
  std::vector<DeclTypeCount> declCounts;
  // evaluations of a material of an earlier registry or of a failed parse, refused
  std::uint64_t refusedEvaluations = 0;
  // builds because a loose decl file changed, found by a poll or while a build read it
  std::uint64_t pollReloads = 0;
  // work items that threw; the item is dropped and the thread goes on (A2.1)
  std::uint64_t failedWorkItems = 0;
  // the thread waits for a request, and its last work item is done and freed
  bool idle = false;
  // the thread ended without a stop (X.2): set first, without allocating (Y.1)
  bool threadEnded = false;
  // why it ended; empty while it serves, and when not even that could be said (Y.1)
  std::string threadError;
};

/**
 * The parse thread (D32, C20, §3.5 Threading): it alone constructs, uses and destroys the
 * MaterialDecls, and every other thread sees only the published MaterialTable. Requests
 * queue in priority order: stop, reload, evaluations, lookups, then the bulk parse, which
 * works in short steps so that evaluations jump ahead of it (D52).
 *
 * The thread builds the first registry at once. A reload destroys the registry, builds a
 * new one, looks up again every name the consumers asked for, and publishes the new table
 * in one step; entries of the old registry are refused by evaluate. reload() rebuilds at
 * once. A change of the loose decl files' times, found by a poll or between the stamps
 * taken before and after a build, rebuilds at most once per autoReloadInterval, so a file
 * that cannot be read never loops (A2.3). A build ends its read batch with
 * DeclSource::closeFiles, so no game file stays open (R4-3.26).
 *
 * Exceptions (A2.1): the source's are caught by the registry's file system; anything else
 * a work item throws is logged, counted in failedWorkItems, and the thread goes on. A
 * build that throws publishes an empty table; until a build succeeds, every lookup is
 * published as Failed and every evaluation as refused, with the build's failure, and the
 * loose files are still polled through the source, so a changed file builds again (W.4).
 * A build whose makeSource threw has no source to poll: only reload() builds again.
 * A request the thread cannot take, or what escapes every guard (an allocation failure),
 * ends the thread, never the process: it is logged, stats().threadEnded is set first
 * without allocating, stats().threadError then says why, and every later lookup and
 * entityDef query answers Failed and every later evaluation a refusal, without queueing
 * anything (W.5, X.2). When even the reason cannot be made, the answers carry a fixed
 * text and the refusal made with the service (Y.1).
 *
 * Stop (§3.1 teardown steps 1 and 8, R5.20, G4): requestStop returns at once. The thread
 * stops within one directory entry, one container table entry or one .pk4 directory
 * while a game source scans, after the decl file it is reading while the registry is
 * built, or after the material it is parsing, and reports nothing more. join destroys the
 * registry on the thread, then joins it. The thread has an 8 MB stack on every platform,
 * whatever the executable reserves (G13).
 */
class MaterialService
{
private:
  struct Shared;
  class Worker;

  std::unique_ptr<Shared> m_shared;
  std::unique_ptr<Worker> m_worker;

  explicit MaterialService(MaterialServiceOptions options);

public:
  // starts the parse thread; fails when it cannot be created
  static Result<std::unique_ptr<MaterialService>> create(MaterialServiceOptions options);

  // requestStop, then join
  ~MaterialService();

  // the latest table; any thread
  std::shared_ptr<const MaterialTable> table() const;

  /**
   * The material of a name as the latest table has it, or Pending, and the name is
   * queued; any thread. The name is used as given: the parse thread canonicalises it and
   * publishes the slot under both spellings, so that no other thread calls copied code.
   */
  MaterialLookup lookup(std::string_view name);

  /**
   * The registers of a material at these shaderParms and this time, from the latest
   * table, or null, and the evaluation is queued ahead of lookups and the bulk parse; any
   * thread. A material of an earlier registry is refused: look it up again in the newer
   * table.
   */
  std::shared_ptr<const EvaluatedRegisters> evaluate(
    MaterialId material, const ShaderParms& shaderParms, float time);

  /**
   * C22: the entityDef of a classname as the latest table has it, or Pending, and the
   * name is queued with the lookups; any thread. The name is used as given and published
   * under it and the decl's canonical name. Every name asked for is asked again after a
   * reload, so a later table has it from the new registry.
   */
  EntityDefLookup entityDef(std::string_view classname);

  /**
   * D64: reads the .proc at path (a file path) on the parse thread with the copied lexer
   * (C20) and publishes what it found in the table's procFiles, under path; any thread.
   * Each call reads the file again, so a new dmap run is seen when the caller asks.
   */
  void loadProcFile(std::string_view path);

  // bulk-parses every material and table of the current registry and of every later one
  void parseAll();
  // rebuilds the registry from a new source
  void reload();

  // teardown step 1: the thread stops after its current decl file or material
  void requestStop();
  bool stopRequested() const;
  // teardown step 8: the registry is destroyed on the thread, then the thread is joined
  void join();

  MaterialServiceStats stats() const;

  // waits until the latest table satisfies predicate, or timeout
  std::shared_ptr<const MaterialTable> waitForTableForTesting(
    const std::function<bool(const MaterialTable&)>& predicate,
    std::chrono::milliseconds timeout) const;
  // waits until the stats satisfy predicate, or timeout
  bool waitForStatsForTesting(
    const std::function<bool(const MaterialServiceStats&)>& predicate,
    std::chrono::milliseconds timeout) const;

  deleteCopyAndMove(MaterialService);
};

} // namespace tb::lit
