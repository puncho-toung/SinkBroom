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
#include "lit/CpuPool.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/MaterialService.h"
#include "lit/PersistentMap.h"
#include "lit/Snapshot.h"

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
class CpuJobContext;
struct ProcTree;

/**
 * C15, R-C10 (§3.6): one light as the game would have it at time 0, engine-neutral and
 * immutable, keyed by its preview id and version. The version grows with every build of
 * the light, and a light is built again whenever its keys, classname or visibility
 * change, its entityDef or a material it waits for arrives, or the .proc changes; a
 * reader that kept a record knows by the version whether it still holds.
 *
 * - light: the parsed render light with idLight::Spawn's effects at time 0
 *   (noShadows, noSpecular, parallel, the math type and keys, levels, start_off, the
 *   style values);
 * - derived: the light shader and its falloff image, the projection (lightProject,
 *   baseLightProject, its inverse, globalLightBounds, globalLightOrigin), areaNum and the
 *   area references (LP4 reads areaNum and the areas);
 * - material: the light shader's MaterialId, none while it is pending.
 */
struct LightRecord
{
  PreviewId id = NoPreviewId;
  std::uint64_t version = 0;
  std::string classname;
  // EditorContext::visible (D65)
  bool visible = true;
  // the merged spawn args the light was parsed from: the map's keys over its entityDef's
  EntityKeys spawnArgs;
  ParsedLight light;
  DerivedLight derived;
  std::optional<MaterialId> material;
  // LPF.0b (C43, D106): the revision stamps (MaterialTable::revisionStamp) of the
  // materials the light was derived with (lightMaterialNames: its texture's and the
  // default light material's), combined; a re-parse of either builds it again
  std::uint64_t materialStamp = 0;
  // built while its entityDef or a material was pending (C22, A15): built again once a
  // later material table has them
  bool provisional = false;
  // what it waits for
  std::vector<std::string> pendingMaterials;
  bool pendingEntityDef = false;
};

/**
 * The published light records (§3.6, like the material table, A18): immutable once
 * published, shared by every thread that holds it, a new version sharing every record
 * that did not change (copy-on-write); the registry publishes it with one pointer swap.
 */
struct LightTable
{
  using Records = PersistentMap<PreviewId, std::shared_ptr<const LightRecord>>;

  // increases with every publication
  std::uint64_t version = 0;
  Records records;

  // null when there is no such light
  std::shared_ptr<const LightRecord> find(PreviewId id) const;
};

/**
 * R2.9: where a light registry publishes its tables. The session owns it and the render
 * thread's registry publishes into it, so that any thread, the GUI thread and LP2's
 * readers included, reads the latest table also once the render thread has ended (its
 * scene is destroyed at its exit). One pointer swap under a mutex; never null.
 */
class LightTablePublication
{
private:
  mutable std::mutex m_mutex;
  std::shared_ptr<const LightTable> m_table;

public:
  LightTablePublication();

  // the latest table; any thread
  std::shared_ptr<const LightTable> table() const;
  void publish(std::shared_ptr<const LightTable> table);

  deleteCopyAndMove(LightTablePublication);
};

// one batch's input (held by shared_ptr, immutable): the lights to build, the material
// table (null without a service), the .proc's tree (null without one) and worldspawn's
// map keys (removeEntities*, R3.12)
struct LightJobInput
{
  struct Light
  {
    PreviewId id = NoPreviewId;
    std::uint64_t version = 0;
    std::string classname;
    EntityKeys mapKeys;
    bool visible = true;
  };

  std::vector<Light> lights;
  std::shared_ptr<const MaterialTable> materials;
  std::shared_ptr<const ProcTree> proc;
  EntityKeys worldspawn;
};

struct LightJobOutput
{
  std::vector<LightRecord> records;
  // R3.12: the lights (id, version) of a class worldspawn's removeEntities* names: the
  // engine's map loader deletes them, so they have no record
  std::vector<std::pair<PreviewId, std::uint64_t>> removed;
  // what the records' warnings said, with the light they are about
  std::vector<std::string> warnings;
};

/**
 * Builds the records of the input's lights: the entityDef's keys from the table
 * (findEntityDef: pending while the table has no answer, none when there is no such
 * def), mergeSpawnArgs, parseLight, deriveLight over the tree with its
 * commonChildrenArea computed once; a light whose class worldspawn's removeEntities*
 * names is removed instead (R3.12). Pure: it reads the immutable input and calls only
 * the light code, so any thread, the CPU pool included (C24, mirror.json
 * M-LIGHT-POOL-02); the copied code's messages go to the calling thread's sink. With a
 * job context it stops at the first light after the pool was cancelled (R2.1).
 */
LightJobOutput buildLightRecords(
  const LightJobInput& input, const CpuJobContext* context = nullptr);

/**
 * The render thread's light records (§3.6, C15, R-C10): it keeps each light's map keys
 * from the snapshots (the entities with a Light node, B0.4's rules), builds the changed
 * ones as one CPU pool job at a time, inputs by shared_ptr (C24), and publishes the
 * records in a LightTable. Like SceneGeometry it queues the entityDefs and materials the
 * lights need with the material service; every table a job adopts is scanned for them
 * (R3.1), and a light that waited is built again in that job. A new .proc tree, and a
 * change of worldspawn's keys (removeEntities*, R3.12), rebuild every light.
 *
 * Without a pool (or once it is stopped) the job runs on the calling thread. Without a
 * material service every record stays provisional.
 *
 * Threads: the render thread (its owner, SceneGeometry) for everything except table(),
 * which any thread may call; the jobs on the pool see only their input.
 */
class LightRegistry
{
private:
  struct Light
  {
    std::string classname;
    EntityKeys mapKeys;
    bool visible = true;
    std::uint64_t version = 0;
  };

  CpuPool* m_pool;
  MaterialService* m_materials;

  std::unordered_map<PreviewId, Light> m_lights;
  std::shared_ptr<const ProcTree> m_proc;
  // worldspawn's map keys, for removeEntities* (R3.12)
  PreviewId m_worldspawn = NoPreviewId;
  EntityKeys m_worldspawnKeys;
  // the table the last job was built with; every table adopted is scanned once
  std::shared_ptr<const MaterialTable> m_table;
  // LPF.0b (C43): the materialsRevision of the last table scanned for re-parses
  std::uint64_t m_materialsRevision = 0;
  std::set<PreviewId> m_dirty;
  std::set<PreviewId> m_removed;
  // the provisional lights by the material or classname they wait for
  std::map<std::string, std::set<PreviewId>> m_waitingMaterials;
  std::map<std::string, std::set<PreviewId>> m_waitingEntityDefs;

  std::optional<std::future<CpuJobResult<LightJobOutput>>> m_job;
  LightTable::Records m_records;
  std::uint64_t m_tableVersion = 0;
  std::vector<std::string> m_warnings;

  std::shared_ptr<LightTablePublication> m_publication;

public:
  // publication may be null: the registry then publishes into one of its own
  LightRegistry(
    CpuPool* pool,
    MaterialService* materials,
    std::shared_ptr<LightTablePublication> publication = nullptr);
  // waits for the job in flight (R5.22: only waited for)
  ~LightRegistry();

  // no job is in flight
  bool readyForSnapshot() const;
  bool busy() const;

  // merges the snapshot's lights and the .proc tree it comes with (setProcTree) and
  // starts one job for the changed ones; render thread
  void takeSnapshot(const Snapshot& snapshot, std::shared_ptr<const ProcTree> proc);

  // the .proc's tree the areas are pushed into (null: none); a new one rebuilds every
  // light; render thread
  void setProcTree(std::shared_ptr<const ProcTree> proc);

  /**
   * Collects the finished job: its records replace the lights' and the table is
   * published, and true is returned. When nothing is in flight it starts a job for what a
   * newer material table made ready. Render thread.
   */
  bool collect();

  // the latest published table (never null); any thread
  std::shared_ptr<const LightTable> table() const;

  // what the jobs and the copied code reported, oldest first; render thread
  std::vector<std::string> takeWarnings();

  // waits for the job in flight and collects it (tests)
  void finishForTesting();

  deleteCopyAndMove(LightRegistry);

private:
  void startBatch();
  // adopts the latest material table and, when it is new, scans what the lights wait for
  void refreshTable();
  void publish();
  void apply(LightJobOutput output);
};

} // namespace tb::lit
