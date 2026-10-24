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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/d3xp/Game_local.cpp, d3xp/EnvironmentProbe.cpp,
// renderer/RenderWorld_defs.cpp and renderer/RenderCommon.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "base/Macros.h"
#include "lit/CpuPool.h"
#include "lit/EngineLight.h"
#include "lit/MaterialDecls.h"
#include "lit/Snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// LP4.2a (C37, D87, P4.2, P4.10, P4.18): the map's environment probes as the game spawns
// them at map load and the renderer derives them, engine-neutral and immutable: the
// placed ones (entities whose merged entityDef has spawnclass EnvironmentProbe) or, when
// there is none, one automatic probe per area; each with R_DeriveEnvprobeData's area,
// bounds, snapped point, SH irradiance from the .shprobes and image keys. The per-view
// choice of three of them is lit/ViewProbes.h's (render thread, every frame).

namespace tb::lit
{
class CpuJobContext;
struct AreaData;
struct ProcTree;
struct ShProbe;

// K32's tie key of an automatic probe, which has no entity: the area it was spawned for
// above every entity's preview id (an automatic and a placed probe never coexist,
// GAME:1279-1293)
inline constexpr auto AutomaticProbeIdBase = PreviewId{1} << 63;
constexpr PreviewId automaticProbeId(const int area)
{
  return AutomaticProbeIdBase | PreviewId(std::uint32_t(area));
}

// the entityDef the automatic probes are spawned from (Game_local.cpp:1295, the retail
// base/def/_rbdoom_env_probe.def), and the spawnclass that makes an entity a probe
inline constexpr auto EnvProbeClassname = std::string_view{"env_probe"};
inline constexpr auto EnvProbeSpawnclass = std::string_view{"EnvironmentProbe"};

// the shaderParms ParseSpawnArgsToRenderEnvprobe fills (MAX_ENTITY_SHADER_PARMS 12,
// RenderWorld.h renderEnvironmentProbe_t)
inline constexpr auto EnvprobeShaderParmCount = std::size_t{12};

/**
 * One RenderEnvprobeLocal (RenderCommon.h:223-263) as R_CreateEnvprobeRefs leaves it
 * (RenderWorld_defs.cpp:808-941):
 *
 * - origin: renderEnvironmentProbe_t::origin, the parsed "origin" of a placed probe
 *   (ParseSpawnArgsToRenderEnvprobe, EnvironmentProbe.cpp:79-102; the engine presents
 *   the physics pose, EnvironmentProbe.cpp:495, the lights' M-LIGHT-DIFF-02 class:
 *   mirror.json M-PROBE-DIFF-01), PopulateEnvironmentProbes' snapped area centre for an
 *   automatic one (Game_local.cpp:1308-1322);
 * - shaderParms: the "_color" and shaderParm3-7 keys, kept, read by nothing;
 * - areaNum: PointInArea( origin ) (:817, :933); globalProbeBounds: the area's
 *   globalBounds (AreaBounds), cleared at -1 (:819-827);
 * - point: SnapInt( origin ) as ints (:831-832), the .shprobes match's and the image
 *   names' key;
 * - shIrradiance: the first .shprobes entry of the same area and int x, y, z (:836-851),
 *   else zero (shFound false);
 * - images: envProbeImageKeys( base, areaNum, point ): _amb, _spec, _depth (:853-867);
 *   the preview requests _spec and _depth only (D85);
 * - inverseBaseProbeProject: OffsetScaleForBounds( identity, globalProbeBounds × 2 )
 *   (:869-922), tabled for LP6's debug draws, unused by the pass.
 */
struct ProbeRecord
{
  // a placed probe's entity preview id, or automaticProbeId( area )
  PreviewId id = NoPreviewId;
  bool automatic = false;
  // an automatic probe's area i (env_probe_area_%i), -1 for a placed one
  int spawnArea = -1;
  EngineVec3 origin = {0, 0, 0};
  std::array<float, EnvprobeShaderParmCount> shaderParms = {};
  int areaNum = -1;
  EngineBounds globalProbeBounds = {};
  std::array<int, 3> point = {0, 0, 0};
  std::array<EngineVec3, 9> shIrradiance = {};
  bool shFound = false;
  std::array<MaterialImageKey, 3> images = {};
  EngineMat4 inverseBaseProbeProject = {};

  bool operator==(const ProbeRecord&) const = default;
};

// what the env_probe entityDef query found: not answered yet, found, or none
// (Game_local.cpp:1295-1299)
enum class EnvProbeDefState
{
  Pending,
  Found,
  Missing,
};

/**
 * The published probe records (D87, like LightTable): immutable once published, the
 * records in the engine's envprobeDefs order (the placed probes in preview-id order, the
 * automatic ones in area order). version grows only when a record changes; the other
 * fields say how the records were made.
 */
struct ProbeTable
{
  std::uint64_t version = 0;
  std::vector<ProbeRecord> records;
  // an entityDef the probes depend on was pending (env_probe's, or a classname's that
  // may be a probe's): a later table rebuilds them
  bool provisional = false;
  EnvProbeDefState envProbeDef = EnvProbeDefState::Pending;
  // the placed probes, which switch the automatic ones off (Game_local.cpp:1279-1293)
  std::size_t placed = 0;
  // the areas whose snapped centre is in no area (skipped with a warning, :1313-1318)
  std::size_t automaticSkipped = 0;
};

/**
 * R2.9: where a probe registry publishes its tables, as LightTablePublication: the
 * session owns it, the render thread's registry publishes into it, any thread reads the
 * latest table, also after the render thread has ended. Never null.
 */
class ProbeTablePublication
{
private:
  mutable std::mutex m_mutex;
  std::shared_ptr<const ProbeTable> m_table;

public:
  ProbeTablePublication();

  // the latest table; any thread
  std::shared_ptr<const ProbeTable> table() const;
  void publish(std::shared_ptr<const ProbeTable> table);

  deleteCopyAndMove(ProbeTablePublication);
};

// a placed probe: its entity and merged spawn args (mergeSpawnArgs)
struct ProbeEntity
{
  PreviewId id = NoPreviewId;
  EntityKeys spawnArgs;

  bool operator==(const ProbeEntity&) const = default;
};

// one build's input (held by shared_ptr, immutable): the bake's tree, AreaData and SH
// probes and the name the bake files are named after (MapBake::bakeBase), the placed
// probes, the env_probe entityDef's state, and whether an entityDef was pending
struct ProbeJobInput
{
  std::shared_ptr<const ProcTree> tree;
  std::shared_ptr<const AreaData> areas;
  std::shared_ptr<const std::vector<ShProbe>> shProbes;
  std::string bakeBase;
  std::vector<ProbeEntity> placed;
  EnvProbeDefState envProbeDef = EnvProbeDefState::Pending;
  bool provisional = false;

  bool operator==(const ProbeJobInput&) const = default;
};

struct ProbeJobOutput
{
  std::vector<ProbeRecord> records;
  std::size_t automaticSkipped = 0;
  std::vector<std::string> warnings;
};

/**
 * idVec3::SnapInt (Vector.h:860-864): each component float( int( v ) ), truncation
 * toward zero. A component beyond an int's range (the engine's undefined conversion) is
 * left as it is here, a guard; no area bound reaches it. Pure.
 */
EngineVec3 engineSnapInt(const EngineVec3& v);

/**
 * R_DeriveEnvprobeData and R_CreateEnvprobeRefs (RenderWorld_defs.cpp:808-941) for a
 * probe at origin: the record's derived fields from the tree (PointInArea), the areas'
 * globalBounds, the first matching .shprobes entry and the bake's base name. Without a
 * tree the area is -1. Pure; any thread, the CPU pool included (mirror.json
 * M-PROBE-POOL-01).
 */
ProbeRecord deriveEnvprobe(
  PreviewId id,
  bool automatic,
  int spawnArea,
  const EngineVec3& origin,
  const std::array<float, EnvprobeShaderParmCount>& shaderParms,
  const ProcTree* tree,
  const AreaData* areas,
  const std::vector<ShProbe>* shProbes,
  std::string_view bakeBase);

/**
 * The probes of a map (C37): with a placed probe, every placed probe (origin and
 * shaderParms as ParseSpawnArgsToRenderEnvprobe parses them, EnvironmentProbe.cpp:79-102)
 * and no automatic one (Game_local.cpp:1279-1293); else, when the env_probe entityDef
 * exists and the bake has a tree, PopulateEnvironmentProbes (:1302-1335): per area i of
 * the AreaData the point SnapInt( globalBounds[i]'s centre ) (an area without surfaces
 * has Zero() bounds: the origin, P4.10), skipped with the engine's warning when
 * PointInArea( point ) < 0; with the entityDef missing, none (the engine's Printf, :1298,
 * not a warning: the output carries no message for it). Each through deriveEnvprobe.
 * Pure: it reads the immutable input; any thread, the CPU pool included. With a job
 * context it stops at the first probe after the pool was cancelled.
 */
ProbeJobOutput buildProbeRecords(
  const ProbeJobInput& input, const CpuJobContext* context = nullptr);

/**
 * The render thread's probe records (D87, R2.9): SceneGeometry hands it the inputs
 * (update) whenever the bake's tree or AreaData, its .shprobes, the probe entities or the
 * env_probe entityDef may have changed; a build runs as one CPU pool job at a time
 * (inputs by shared_ptr, C24) when they differ from the last build's, and every probe is
 * built again. A collected build is published as a ProbeTable whose version moves only
 * when a record changed. Without a pool (or once it is stopped) the job runs on the
 * calling thread. While setProbeBuildsHeldForTesting holds the builds, an input waits
 * (busy() is true) and its build starts at the first collect() after the hold ends.
 *
 * Threads: the render thread (its owner, SceneGeometry) for everything except table(),
 * which any thread may call; the jobs see only their input.
 */
class ProbeRegistry
{
private:
  CpuPool* m_pool;
  std::shared_ptr<ProbeTablePublication> m_publication;
  // the input of the last build started, and one that waits for the job in flight
  std::optional<ProbeJobInput> m_started;
  std::optional<ProbeJobInput> m_waiting;
  std::shared_ptr<const ProbeJobInput> m_jobInput;
  std::optional<std::future<CpuJobResult<ProbeJobOutput>>> m_job;
  std::vector<ProbeRecord> m_records;
  std::uint64_t m_version = 0;
  bool m_published = false;
  std::vector<std::string> m_warnings;

public:
  // publication may be null: the registry then publishes into one of its own
  explicit ProbeRegistry(
    CpuPool* pool, std::shared_ptr<ProbeTablePublication> publication = nullptr);
  // waits for the job in flight (R5.22: only waited for)
  ~ProbeRegistry();

  bool busy() const;

  // render thread: the inputs now; a build when they differ from the last one's
  void update(ProbeJobInput input);

  /**
   * Collects the finished build and publishes its table (the version bumped when a
   * record changed), and true is returned; then starts the build that waited. Render
   * thread.
   */
  bool collect();

  // the latest published table (never null); any thread
  std::shared_ptr<const ProbeTable> table() const;

  // what the builds reported, oldest first; render thread
  std::vector<std::string> takeWarnings();

  // waits for the builds in flight and collects them (tests)
  void finishForTesting();

  deleteCopyAndMove(ProbeRegistry);

private:
  void start(ProbeJobInput input);
};

/**
 * LP4.2b step 0, any thread: while held, every ProbeRegistry keeps the input it is given
 * waiting instead of building it (busy() stays true: a frame is not settled meanwhile,
 * FrameConvergence::probesInFlight); once released, the waiting build starts at the
 * registry's next collect(). Process-wide; tests only (LitSession::
 * holdProbeBuildsForTesting).
 */
void setProbeBuildsHeldForTesting(bool held);

} // namespace tb::lit
