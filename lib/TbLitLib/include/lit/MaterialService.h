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
#include "lit/ImageCache.h"
#include "lit/ImageOverride.h"
#include "lit/MapBake.h"
#include "lit/MaterialDecls.h"
#include "lit/ModelRecord.h"
#include "lit/PersistentMap.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
  // LP1c fix (S2.2): the entry's editor-image size was measured (C21); until then it is
  // the last Ready size of the material (D52) or the registry's Default, and a lookup of
  // the name queues the measure
  bool sizeMeasured = false;
  // LP5.F (D111, S2.5): published again unmeasured over a measured size (remeasure: an
  // override's new editorSize, a changed file the poll found): until it is measured, its
  // size is not final, whatever its state
  bool remeasured = false;
};

// C22: what an entityDef query has found for a classname: an entry, or why there is none
// (no such entityDef, or the engine error its parse reached)
struct EntityDefSlot
{
  std::shared_ptr<const EntityDefEntry> entry;
  std::string error;
};

// C10 (LP1c.1): what the bake search found for a map, asked by its document path
// (MaterialService::loadMapBake); published with the materials
struct MapBakeSlot
{
  // the document path asked with
  std::string path;
  // the latest request it answers (loadMapBake's ticket): an owner that asked again takes
  // only a slot of its own ticket or later, never one read before it asked
  std::uint64_t ticket = 0;
  // the area BSP from the .proc or the .bproc (null: none), and why there is none or why
  // the .proc was passed over for the .bproc
  std::shared_ptr<const ProcTree> tree;
  std::string error;
  // everything the search found (null for a path outside the game folder)
  std::shared_ptr<const MapBake> bake;
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

// equal image keys hash alike (every field of MaterialImageKey::operator==)
struct MaterialImageKeyHash
{
  std::size_t operator()(const MaterialImageKey& key) const;
};

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
 *   last registers until new ones arrive;
 * - images (LP1c.1, C8): what the image cache reader found for each image key asked for,
 *   published as deltas and kept across reloads (an image's cache does not depend on the
 *   decls);
 * - models (LP7.0, C51, D126): the model records by canonical name, kept across reloads
 *   as the images are, with modelChanges and changedModels for the poll; LP7.1a's
 *   MaterialService::requestModel fills it;
 * - skins (LP7.0, C53): the skin decls by name, per registry as the entityDefs are
 *   (LP7.2: MaterialService::skin fills it, lit/ModelRecord.h SkinRecord).
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
  using MapBakes = PersistentMap<
    std::string,
    std::shared_ptr<const MapBakeSlot>,
    StringHash,
    std::equal_to<>>;
  using Images = PersistentMap<
    MaterialImageKey,
    std::shared_ptr<const ImageCacheSlot>,
    MaterialImageKeyHash>;
  using ImageChanges =
    PersistentMap<MaterialImageKey, std::uint64_t, MaterialImageKeyHash>;
  using Models = PersistentMap<
    std::string,
    std::shared_ptr<const ModelRecord>,
    StringHash,
    std::equal_to<>>;
  using ModelChanges =
    PersistentMap<std::string, std::uint64_t, StringHash, std::equal_to<>>;
  using Skins = PersistentMap<
    std::string,
    std::shared_ptr<const SkinSlot>,
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
  // the maps' bakes read so far (loadMapBake), by document path; kept across reloads
  MapBakes mapBakes;
  // LP1c.1: the image caches read so far (image), by key; kept across reloads
  Images images;
  /**
   * LP2.1 (P2.14b): the image slots dropped because their files changed (a reload's
   * changed files, the poll's re-probe), each key with imageChanges' count at its last
   * drop; imageChanges counts the drops. The budget's drops (S2.4) are not changes. A
   * consumer that holds an image's texture asks for it again when its key's count is
   * newer than the count it took it at, and looks only when imageChanges moved. Kept
   * across reloads.
   */
  std::uint64_t imageChanges = 0;
  ImageChanges changedImages;
  /**
   * LP7.0 (C51, D126): the model records by canonical name (canonicalModelName), kept
   * across reloads; modelChanges counts the records dropped because their files changed
   * and changedModels holds each name with the count at its last drop, as imageChanges
   * and changedImages do for the images (a consumer holding a model's geometry asks
   * again when its name's count is newer than the one it took it at). LP7.1a fills it
   * (MaterialService::requestModel).
   */
  Models models;
  std::uint64_t modelChanges = 0;
  ModelChanges changedModels;
  // LP7.0 (C53): the skins by name, published per registry as the entityDefs are (a
  // reload drops them and asks again for every name asked before); LP7.2's skin() fills
  // it
  Skins skins;
  /**
   * LPF.0a (D100, C40): the image override hand-overs (setImageOverride,
   * clearImageOverride) and, LPF.0b, the material re-parse hand-overs (reparseMaterial,
   * clearMaterialReparse) this table includes, applied or refused: every Feed and Reparse
   * work item publishes once, and a table whose feedApplied equals
   * MaterialService::feedRequested() holds every hand-over made before that was read.
   * Kept across reloads.
   */
  std::uint64_t feedApplied = 0;
  /**
   * LPF.0b (D105, C43): moves once with every material re-parse, its restore and its
   * re-application after a build: a consumer that caches per material compares it to see
   * whether any entry's revision (MaterialEntry::revision) moved. An image override
   * moves it not. Kept across reloads.
   */
  std::uint64_t materialsRevision = 0;
  // LP5.F (D111): moves with every slot published again unmeasured (MaterialSlot::
  // remeasured), so that a consumer of the sizes sees a table it has not scanned. Kept
  // across reloads
  std::uint64_t sizesRevision = 0;
  // LPF.0a (C41): the bytes the override store holds (the callers' pixels or ready
  // levels), not the converted slots' (those count in the image budget)
  std::size_t overrideBytes = 0;

  /**
   * LPF.1 (C44, P5.13): one Feed or Reparse item the table includes: the name it was
   * handed over under, its newest hand-over time (steady_clock, the clock the render
   * thread's frame log times with), the table's feedApplied once it was applied, the
   * image keys whose slots it changed (an override's set or clear: the keys dropped and
   * published again; none for a refusal or a name no key uses) and, for a re-parse, the
   * materialsRevision it published (0 when it changed no material).
   */
  struct FeedRecord
  {
    enum class Kind
    {
      ImageOverride,
      Reparse,
    };

    Kind kind = Kind::ImageOverride;
    std::string name;
    std::chrono::steady_clock::time_point handOver;
    std::uint64_t feedApplied = 0;
    std::vector<MaterialImageKey> keys;
    std::uint64_t materialsRevision = 0;
  };
  // the newest items the records keep
  static constexpr std::size_t FeedRecordsKept = 32;
  // LPF.1: the newest FeedRecordsKept items, oldest first; shared by the tables that do
  // not add one; kept across reloads (null before the first item)
  std::shared_ptr<const std::vector<FeedRecord>> feedRecords;

  // the slot of a name, shared with the table, so that it outlives a table held only for
  // the call; null when the name is not looked up yet. Neither find allocates.
  std::shared_ptr<const MaterialSlot> find(std::string_view name) const;
  std::shared_ptr<const EntityDefSlot> findEntityDef(std::string_view classname) const;
  std::shared_ptr<const MapBakeSlot> findMapBake(std::string_view path) const;
  std::shared_ptr<const ImageCacheSlot> findImage(const MaterialImageKey& key) const;
  // LP7.0 (C51): the record of a canonical model name (canonicalModelName), null when it
  // was never requested or was dropped (a changed file, LP7.1a); it does not allocate
  std::shared_ptr<const ModelRecord> findModel(std::string_view canonicalName) const;
  // LP7.0 (C53): a skin's slot, null when it is not published yet (LP7.2: skin() asks)
  std::shared_ptr<const SkinSlot> findSkin(std::string_view name) const;
  // the registers of a key; an EvaluatedRegisters with an error when the evaluation was
  // refused, null when it is not done yet
  std::shared_ptr<const EvaluatedRegisters> findRegisters(const RegisterKey& key) const;
  // LPF.0b (C43, D106): what a consumer keeps of a material to see a re-parse: 0 when the
  // name is not looked up, 1 for a Failed slot, else 2 + the entry's revision. It does
  // not allocate
  std::uint64_t revisionStamp(std::string_view name) const;
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

// LP7.2 (C53): what skin returns: Pending (queued: a later table has it), Ready, or
// Failed with the slot's error
struct SkinLookup
{
  using State = MaterialLookup::State;

  State state = State::Pending;
  std::shared_ptr<const SkinRecord> entry;
  std::string error;
};

// LP1c.1, LP1c.2: what image returns: Pending (queued: a later table has it), or what the
// slot holds; the slot says why (its cache state and reasons)
struct ImageLookup
{
  enum class State
  {
    Pending,
    // the engine's cache, in the engine's GPU format
    Ready,
    // no cache the engine would use (Missing or Stale): the source fallback's image, in
    // the engine's channel layouts as RGBA8, R11G11B10F for HDR usages (D24)
    Source,
    // an image the engine generates (an intrinsic, the default image): no cache, no
    // source
    Generated,
    // neither a cache nor a source the preview can load: the default image
    Default,
  };

  State state = State::Pending;
  std::shared_ptr<const CachedImage> image;
  std::shared_ptr<const ImageCacheSlot> slot;
};

// LPF.0a (C39, C40): what became of the last hand-over of an override's name
struct ImageOverrideStatus
{
  enum class State
  {
    // no override of the name
    None,
    // handed over and stored, not converted yet: queued, no key uses the name yet, or no
    // registry (a failed build) can convert it
    Pending,
    // converted for every key the table held under the name
    Applied,
    // refused, with the reason; the slots stand as they were (a refused replacement
    // leaves the earlier override applied)
    Refused,
  };

  State state = State::None;
  std::string reason;
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
  // reload rescans the game's containers (LP6.3: a source from a container index checks
  // the key and scans only when it moved, lit/GameContainerIndex.h). cancel is the stop
  // flag: a source that scans stops when it is set (G4).
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
  // size the registry measured (C21; Default until LP1c.2 loads images); the entry is
  // then published measured
  std::function<EditorImageSize(const MaterialEntry&)> editorImageSizeForTesting;
  // LP1c fix (S2.4): the bytes of the published images' data the table keeps: beyond it
  // the images a consumer has taken (image() returned their data) are dropped from the
  // table, the least recently published first, and read again when asked for; 0 keeps
  // every image. LPF.0a (D99): an override's slots too, converted again from the store
  std::size_t imageBudgetBytes = std::size_t{512} * 1024 * 1024;
  // LPF.0a: runs on the parse thread before each work item with its kind ("evaluate",
  // "feed", "reparse", "lookup", "bake", "poll", "measure", "image", "model" (LP7.1a),
  // "bulk", "reload")
  std::function<void(std::string_view kind)> beforeWorkItemForTesting;
  // LP7.0: runs on the parse thread on every registry build, with the build's new working
  // table before anything is looked up in it, so a test can publish the model records and
  // skins that no request path makes yet (C51, C53)
  std::function<void(MaterialTable& table)> registryBuiltForTesting;
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
  // LP6.0 (C65): when the service published its first table (the first build's: the
  // containers' scan, the decl read and the lookups before it), on the steady clock; the
  // measurement's Start -> first table, which a later build or table does not move
  std::optional<std::chrono::steady_clock::time_point> firstTableAt;
  bool parseAllDone = false;
  // a build that the stop cut short
  bool buildCancelled = false;
  // the containers the last build's source holds (DeclSource::containerCount): fewer
  // than the game has when the stop cut its scan short
  std::size_t sourceContainers = 0;
  // LP6.3 (C67 (a)): where the last build's source took its container tables from (a
  // scan, the container index in memory or its file; None without an index), its key
  // check's seconds (inside sourceSeconds), the index file's write after a scan and its
  // bytes (written, or read on a file hit)
  ContainerTablesOrigin::Kind sourceTables = ContainerTablesOrigin::Kind::None;
  double keyCheckSeconds = 0.0;
  double indexWriteSeconds = 0.0;
  std::uint64_t indexFileBytes = 0;
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
  // LP1c fix (S2.4): the bytes of the image data the latest table holds, and the images
  // dropped for the budget so far
  std::size_t imageBytes = 0;
  std::uint64_t evictedImages = 0;
  // LP7.0 (C54): the bytes of the area trees the latest table's bakes hold, each tree
  // once (MapBake::procBytes, their geometry included)
  std::size_t procBytes = 0;
  // LP7.1a (C51, D126, D137, P7.14): the model records the latest table holds and their
  // bytes (modelRecordBytes), the finishes the model pool ran, the longest model work
  // item on the parse thread and the longest finish on the pool, in milliseconds
  std::size_t models = 0;
  std::size_t modelBytes = 0;
  std::uint64_t modelFinishes = 0;
  double longestModelItemMs = 0.0;
  double longestModelFinishMs = 0.0;
  // LPF.0a (D96): override hand-overs replaced by a newer one of the name before their
  // work item ran (their bytes dropped), and the slots converted from the override store
  std::uint64_t droppedFeeds = 0;
  std::uint64_t overrideConversions = 0;
  // LPF.0b (D96): re-parse hand-overs replaced by a newer one of the name before their
  // work item ran
  std::uint64_t droppedReparses = 0;
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
 * queue in priority order: stop, reload, evaluations, the image overrides (LPF.0a, D96:
 * one item per name), the material re-parses (LPF.0b, D96: one item per name), lookups
 * (materials and entityDefs), a bake, the poll, the
 * editor-image measures, the images, the models (LP7.1a, D126), then the bulk parse. LP1c
 * fix (S2.3): every work item is short, so that an evaluation (a light drag) waits for
 * one step at most: a lookup or measure item takes names until about 20 ms have passed
 * (or 256 names) and publishes what it did, leaving the rest queued; a bake is one item;
 * an image item (LP4.3) takes queued images for up to 100 ms (or 256), and ends after the
 * image in hand once more urgent work is queued, an override included; the bulk parse
 * works in 20 ms steps (D52).
 *
 * LP1c fix (S2.2): an entry is published at once with the last Ready editor-image size
 * of its material (D52) or the registry's Default, unmeasured; its measure (C21, by
 * headers, never a decode) is queued for a name a consumer looked up (lookup queues it
 * for a slot the bulk parse made), and publishes the entry measured. A kept Ready size
 * stands for a Default measure only while the files it was measured from are unchanged
 * and the material's image chain is the same (S3.15), so a deleted or renamed texture
 * returns to Default.
 *
 * The thread builds the first registry at once. A reload destroys the registry, builds a
 * new one, looks up again every name the consumers asked for, and publishes the new table
 * in one step; entries of the old registry are refused by evaluate. reload() rebuilds at
 * once. LP1c fix (S2.5): a reload also drops every image slot (a reload is the way to
 * see new containers and caches the engine wrote), and the kept Ready sizes whose files
 * changed. A change of the loose decl files' times, found by a poll or between the stamps
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
   * LP7.2 (C53, D131, P7.6): the skin of a name (an entity's skin key) as the latest
   * table has it, or Pending, and the name is queued with the lookups after the
   * entityDefs; any thread. On the parse thread MaterialDecls::skin (FindSkin with
   * makeDefault, as ParseSpawnArgsToRenderEntity calls it: a name without a decl is the
   * engine's default skin) publishes a SkinSlot under the asked name and the decl's
   * canonical name. Every name asked for is asked again after a reload, as the entityDefs
   * are.
   */
  SkinLookup skin(std::string_view name);

  /**
   * C10 (LP1c.1, D64): reads the bake of the map whose document is at mapPath on the
   * parse thread (C20): the map's engine name below the game's base folder (a relative
   * path is taken as one), then readMapBake's search: maps/<map>.proc, else the .bproc,
   * the light grid and the SH probes, validated (§3.7). Publishes the slot in the table's
   * mapBakes under mapPath with the returned ticket; any thread. Each call probes the
   * files again: a bake whose files did not change keeps its tree (the same pointer), a
   * changed one is read again. While pollInterval runs, the parse thread also probes the
   * bakes it holds and reads a changed one again with its ticket, so a new dmap run is
   * seen.
   */
  std::uint64_t loadMapBake(std::string_view mapPath);

  /**
   * LP1c.1 (C8, D23), LP1c.2 (S14, D24): an image as the latest table has it, or
   * Pending, and the key is queued with the lookups; any thread. On the parse thread the
   * copied timestamp call names the cache (MaterialDecls::imageCacheName), the image
   * cache reader reads it through the game-file search (resolveImageCache), and where the
   * engine would binarize, the source fallback makes the image from its source
   * (resolveImage); the slot is published once, in a batch of up to 100 ms of queued
   * images (LP4.3), which ends after the image in hand when more urgent work arrives (an
   * override among it, LPF.0a). LP1c fix: a reload drops the slots whose files changed
   * (S2.5, X.3), and the image budget drops a slot whose data a consumer has taken
   * through this call once the table's image data is beyond
   * MaterialServiceOptions::imageBudgetBytes (S2.4); asked for again, it is read again.
   * LPF.0a: a key whose name has a stored override (setImageOverride) is answered Ready
   * from the override, converted on the parse thread, never from its files.
   */
  ImageLookup image(const MaterialImageKey& key);

  /**
   * LPF.0a (C39, C40, C41, D96): hands the service an image under a game image name, in
   * place of its files; any thread. The name is folded (feedKeyName) and matched against
   * the whole name of every image key, so a program key (bumpmap addnormals( … )) takes
   * an override only of its whole program text, and the time call's _s → _rmao rename
   * needs the sibling file as in the engine (M-FEED-DIFF-01). Refused at once, with
   * nothing queued and no count moved: an empty name, "default", and a name that starts
   * with "_" (the engine's built-ins, ImageManager.cpp:404-408, :437-441). Otherwise one
   * Feed work item per name is queued, the newest hand-over of a name replacing an older
   * one not taken yet (its bytes dropped, stats().droppedFeeds), and feedRequested()
   * counts it. On the parse thread the item converts the image for every key the table
   * holds under the name, each with its own cache name and usage (the copied time call,
   * the one file probe an override makes) and publishes the keys' drops and the new slots
   * in ONE table with feedApplied advanced; a refusal (a cross pair of pixel format and
   * usage, a GPU-ready image of another format, colour format or face count, a level
   * beyond UploadBytesPerFrame over all its faces) publishes once too, the slots
   * unchanged, and imageOverrideStatus says why. A name no key uses yet is stored and
   * converted at the key's first lookup. The store is the truth: an override slot stays
   * across builds, the budget may drop it and a later request converts it again; the poll
   * never probes an overridden key's files. editorSize, when it changes a material's
   * editor-image size, remeasures every material whose image chain holds the name (D111).
   */
  Result<void> setImageOverride(std::string imageName, ImageOverride image);

  /**
   * LPF.0a (C40, P5.17): ends the override of a name; any thread. One Feed work item, as
   * setImageOverride's: the keys converted from the override are dropped and read from
   * their files again in the same item, then published once, so no table between shows
   * them pending; a name with no override publishes once, nothing else changed.
   */
  void clearImageOverride(std::string imageName);

  /**
   * LPF.0b (C42, D96, D99, D104): hands the service a material's decl text in place of
   * its decl file's; any thread. name is the material's name (any spelling; the parse
   * thread canonicalises it), declText the decl's body with or without the "material
   * <name>" head. One Reparse work item per name is queued, served after the image
   * overrides and ahead of the lookups, the newest text of a name replacing an older one
   * not taken yet (stats().droppedReparses), and feedRequested() counts it. On the parse
   * thread MaterialDecls::reparse parses the material's decl again in place, and ONE
   * table is published with feedApplied advanced and materialsRevision moved: the slot
   * replaced under the asked name, the canonical name and every name it was published
   * under, its register deltas gone (they are evaluated again when asked), its
   * editor-image size measured again; a refused text (over 1 MB, a guard, the name) or a
   * parse that reached an engine Error publishes the slot Failed with the reason, so its
   * consumers drop the material. The text is kept and applied again after every registry
   * build (a build reads the decl files again) until clearMaterialReparse. A re-parsed
   * name without a decl stays implicit (FindMaterial( name, false ) takes it for none:
   * M-FEED-DIFF-01). Fails only when no thread will take it.
   */
  Result<void> reparseMaterial(std::string name, std::string declText);

  /**
   * LPF.0b (C42, P5.8): ends a name's re-parse; any thread. One Reparse work item, as
   * reparseMaterial's: the decl's file text is parsed again (an implicit name is made the
   * default material: the engine's generated text is gone), the slot replaced as a
   * re-parse replaces it, and the kept text forgotten; ONE table is published. A name
   * with no re-parse publishes once, nothing else changed.
   */
  void clearMaterialReparse(std::string name);

  /**
   * LP7.1a (C51, D125, D126, D137): the model record of a name (any spelling; the record
   * is kept under canonicalModelName's), as the latest table has it, or null, and the
   * name is queued; any thread; an empty name is no model (null, nothing queued). On the
   * parse thread a Model work item (served after the images, ahead of the bulk parse)
   * takes one name: the dispatch (lit/ModelLoading.h modelDispatch), the engine's binary
   * through the C8 search with the time test (accepted: Ready at once, no
   * FinishSurfaces), else for an .obj the source (its bytes and the probe's time) through
   * the validator, the copied OBJ_Load and the materials' capture; what needs
   * FinishSurfaces is published Pending and finished on the service's one-worker pool
   * (made with the first model, stopped before the registry goes), one finish at a time,
   * and published when it answers (a queued work item). A missing file is Missing, a
   * refused one Default, an .md5mesh Skinned, every other kind the default cube as
   * modelDispatch says. While pollInterval runs, the poll probes each record's source and
   * binary within its slice and drops a changed one (modelChanges, changedModels),
   * reading it again. A re-parse or a reload that changes a material a Source record was
   * finished with drops and reads it again (P7.22). LP7.2's scene asks; tests ask today.
   */
  std::shared_ptr<const ModelRecord> requestModel(std::string_view name);

  // LPF.0a (D100): the override hand-overs and, LPF.0b, the re-parse hand-overs made so
  // far; any thread (LPF.1's render thread compares it with a table's feedApplied)
  std::uint64_t feedRequested() const;

  // LPF.0a: what became of a name's last hand-over (folded as feedKeyName); any thread
  ImageOverrideStatus imageOverrideStatus(const std::string& imageName) const;

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
  /**
   * LP5.F, any thread: while held, the parse thread takes none of the queued measures
   * (S2.2) and goes on with every other work item, the images among them, so that a
   * material published unmeasured keeps its Default or kept size meanwhile; a release
   * wakes the thread for the measures queued. Tests only.
   */
  void holdMeasuresForTesting(bool hold);

  deleteCopyAndMove(MaterialService);
};

} // namespace tb::lit
