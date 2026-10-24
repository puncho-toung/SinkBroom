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
// renderer/RenderWorld_defs.cpp and idlib/math/Vector.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ProbeRecord.h"

#include "lit/AreaData.h"
#include "lit/EntityState.h"
#include "lit/LightGridFile.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace tb::lit
{
namespace
{

// LP4.2b step 0 (setProbeBuildsHeldForTesting): tests only
constinit auto s_buildsHeld = std::atomic<bool>{false};

// SHADERPARM_RED .. SHADERPARM_BLUE, SHADERPARM_TIMESCALE, SHADERPARM_TIMEOFFSET and 5-7
// (RenderWorld.h)
std::array<float, EnvprobeShaderParmCount> envprobeShaderParms(const EntityKeys& args)
{
  // ParseSpawnArgsToRenderEnvprobe (EnvironmentProbe.cpp:83-101), at game time 0
  auto parms = std::array<float, EnvprobeShaderParmCount>{};
  auto color = EngineVec3{0, 0, 0};
  spawnVector(args, "_color", "1 1 1", color);
  parms[0] = color[0];
  parms[1] = color[1];
  parms[2] = color[2];
  spawnFloat(args, "shaderParm3", "1", parms[3]);
  if (!spawnFloat(args, "shaderParm4", "0", parms[4]))
  {
    // offset the start time of the shader to sync it to the game time: -MS2SEC( 0 )
    parms[4] = -(0.0f * 0.001f);
  }
  spawnFloat(args, "shaderParm5", "0", parms[5]);
  spawnFloat(args, "shaderParm6", "0", parms[6]);
  spawnFloat(args, "shaderParm7", "0", parms[7]);
  return parms;
}

} // namespace

ProbeTablePublication::ProbeTablePublication()
  : m_table{std::make_shared<const ProbeTable>()}
{
}

std::shared_ptr<const ProbeTable> ProbeTablePublication::table() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_table;
}

void ProbeTablePublication::publish(std::shared_ptr<const ProbeTable> table)
{
  auto previous = std::shared_ptr<const ProbeTable>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    previous = std::exchange(m_table, std::move(table));
  }
  // the table replaced is freed outside the lock
}

EngineVec3 engineSnapInt(const EngineVec3& v)
{
  // Vector.h:860-864: x = float( int( x ) ), and y, z alike
  auto snapped = v;
  for (auto& c : snapped)
  {
    // a guard: float( int( c ) ) of a value beyond an int is undefined in the engine
    if (
      c > -float(std::numeric_limits<int>::max())
      && c < float(std::numeric_limits<int>::max()))
    {
      c = float(int(c));
    }
  }
  return snapped;
}

ProbeRecord deriveEnvprobe(
  const PreviewId id,
  const bool automatic,
  const int spawnArea,
  const EngineVec3& origin,
  const std::array<float, EnvprobeShaderParmCount>& shaderParms,
  const ProcTree* tree,
  const AreaData* areas,
  const std::vector<ShProbe>* shProbes,
  const std::string_view bakeBase)
{
  auto probe = ProbeRecord{};
  probe.id = id;
  probe.automatic = automatic;
  probe.spawnArea = spawnArea;
  probe.origin = origin;
  probe.shaderParms = shaderParms;

  // R_DeriveEnvprobeData (RenderWorld_defs.cpp:808-924): the area of the origin, which
  // may let the view cull the probe behind a closed door (K30: none here)
  const auto areaNum = tree ? pointInArea(*tree, origin) : -1;
  if (areaNum != -1 && areas && std::size_t(areaNum) < areas->areas.size())
  {
    // AreaBounds( areaNum ): area->globalBounds (RenderWorld.cpp:1220-1230)
    probe.globalProbeBounds = areas->areas[std::size_t(areaNum)].globalBounds;
  }
  else
  {
    probe.globalProbeBounds = engineClearedBounds();
  }

  // the snapped world position keys the light data, not the probe index or entity name
  const auto point = engineSnapInt(origin);
  probe.point = {int(point[0]), int(point[1]), int(point[2])};

  // LoadSHProbesFile's entries (:743-798): the first of the same area and int x, y, z
  if (shProbes)
  {
    for (const auto& entry : *shProbes)
    {
      if (
        entry.area == areaNum && int(entry.origin[0]) == int(point[0])
        && int(entry.origin[1]) == int(point[1]) && int(entry.origin[2]) == int(point[2]))
      {
        for (std::size_t j = 0; j < 9; ++j)
        {
          probe.shIrradiance[j] =
            EngineVec3{entry.sh[j * 3 + 0], entry.sh[j * 3 + 1], entry.sh[j * 3 + 2]};
        }
        probe.shFound = true;
        break;
      }
    }
  }

  // the three images (:853-867), '-' made '_' over the name
  probe.images = envProbeImageKeys(bakeBase, areaNum, probe.point);

  // the probe projection (:869-922): the unit cube onto the bounds doubled, from the
  // identity at the origin
  auto localBounds = probe.globalProbeBounds;
  for (std::size_t i = 0; i < 3; ++i)
  {
    localBounds[0][i] = probe.globalProbeBounds[0][i] * 2;
    localBounds[1][i] = probe.globalProbeBounds[1][i] * 2;
  }
  const auto identity = EngineMat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
  probe.inverseBaseProbeProject = engineOffsetScaleForBounds(
    engineMatrixFromOriginAxis({0, 0, 0}, identity), localBounds);

  // R_CreateEnvprobeRefs (:926-941): areaNum again, the same PointInArea
  probe.areaNum = areaNum;
  return probe;
}

ProbeJobOutput buildProbeRecords(const ProbeJobInput& input, const CpuJobContext* context)
{
  auto output = ProbeJobOutput{};
  const auto* tree = input.tree.get();
  const auto* areas = input.areas.get();
  const auto* shProbes = input.shProbes.get();

  // PopulateEnvironmentProbes (Game_local.cpp:1279-1293): an artist's probes switch the
  // automatic ones off
  if (!input.placed.empty())
  {
    for (const auto& entity : input.placed)
    {
      // R2.1: a cancelled pool's job stops here; its output is dropped
      if (context && context->stale())
      {
        break;
      }
      // ParseSpawnArgsToRenderEnvprobe (EnvironmentProbe.cpp:83-85)
      auto origin = EngineVec3{0, 0, 0};
      spawnVector(entity.spawnArgs, "origin", "", origin);
      output.records.push_back(deriveEnvprobe(
        entity.id,
        false,
        -1,
        origin,
        envprobeShaderParms(entity.spawnArgs),
        tree,
        areas,
        shProbes,
        input.bakeBase));
    }
    return output;
  }

  // :1295-1299: without the entityDef the game spawns none (and Printfs it)
  if (input.envProbeDef != EnvProbeDefState::Found || !tree || !areas)
  {
    return output;
  }

  // :1302-1335: a probe into the centre of each BSP area
  for (std::size_t i = 0; i < areas->areas.size(); ++i)
  {
    if (context && context->stale())
    {
      break;
    }
    const auto point = engineSnapInt(engineBoundsCenter(areas->areas[i].globalBounds));
    if (pointInArea(*tree, point) < 0)
    {
      output.warnings.push_back(fmt::format(
        "PopulateEnvironmentProbes: location '{}' is not in a valid area", i));
      ++output.automaticSkipped;
      continue;
    }
    // EnvironmentProbe::Spawn of { classname env_probe, origin point }: the entityDef's
    // keys add no origin, and its shaderParms are the parser's defaults
    output.records.push_back(deriveEnvprobe(
      automaticProbeId(int(i)),
      true,
      int(i),
      point,
      envprobeShaderParms(EntityKeys{}),
      tree,
      areas,
      shProbes,
      input.bakeBase));
  }
  return output;
}

ProbeRegistry::ProbeRegistry(
  CpuPool* pool, std::shared_ptr<ProbeTablePublication> publication)
  : m_pool{pool}
  , m_publication{
      publication ? std::move(publication) : std::make_shared<ProbeTablePublication>()}
{
}

ProbeRegistry::~ProbeRegistry()
{
  // R5.22: the future of a job in flight is only waited for
  if (m_job && m_job->valid())
  {
    m_job->wait();
  }
}

bool ProbeRegistry::busy() const
{
  return m_job.has_value() || m_waiting.has_value();
}

void ProbeRegistry::update(ProbeJobInput input)
{
  if (m_job)
  {
    // one build at a time: the latest input waits, unless it is the one in flight
    if (m_started && *m_started == input)
    {
      m_waiting.reset();
    }
    else
    {
      m_waiting = std::move(input);
    }
    return;
  }
  if (m_started && *m_started == input)
  {
    m_waiting.reset();
    return;
  }
  if (s_buildsHeld.load())
  {
    // LP4.2b step 0: held for a test; collect() starts it once the hold ends
    m_waiting = std::move(input);
    return;
  }
  start(std::move(input));
}

void ProbeRegistry::start(ProbeJobInput input)
{
  m_started = input;
  m_jobInput = std::make_shared<const ProbeJobInput>(std::move(input));
  auto job = [jobInput =
                m_jobInput](const CpuJobContext& context) -> Result<ProbeJobOutput> {
    // R2.1: the job of a cancelled pool returns at its next probe
    auto output = buildProbeRecords(*jobInput, &context);
    if (context.stale())
    {
      return Error{"The probe job was abandoned"};
    }
    return output;
  };
  if (m_pool && !m_pool->stopped())
  {
    m_job = m_pool->run(std::move(job));
  }
  else
  {
    // no pool: the job runs here, and its messages are drained here
    auto promise = std::promise<CpuJobResult<ProbeJobOutput>>{};
    m_job = promise.get_future();
    auto output = CpuJobResult<ProbeJobOutput>{buildProbeRecords(*m_jobInput), {}};
    output.messages = drainEngineMessagesForThread(std::this_thread::get_id());
    promise.set_value(std::move(output));
  }
}

bool ProbeRegistry::collect()
{
  if (!m_job)
  {
    // LP4.2b step 0: an input held for a test is built once the hold ends
    if (m_waiting && !s_buildsHeld.load())
    {
      auto waiting = std::move(*m_waiting);
      m_waiting.reset();
      start(std::move(waiting));
    }
    return false;
  }
  if (m_job->wait_for(std::chrono::seconds{0}) != std::future_status::ready)
  {
    return false;
  }
  auto job = std::move(*m_job);
  m_job.reset();
  const auto input = std::exchange(m_jobInput, nullptr);
  try
  {
    auto result = job.get();
    for (const auto& message : result.messages)
    {
      if (message.kind != DeclMessageKind::Info)
      {
        m_warnings.push_back(message.text);
      }
    }
    std::move(result.result) | kdl::transform([&](ProbeJobOutput output) {
      // D87: the version moves only when a record changed
      if (!m_published || output.records != m_records)
      {
        m_records = std::move(output.records);
        ++m_version;
      }
      m_published = true;
      auto table = std::make_shared<ProbeTable>();
      table->version = m_version;
      table->records = m_records;
      table->provisional = input->provisional;
      table->envProbeDef = input->envProbeDef;
      table->placed = input->placed.size();
      table->automaticSkipped = output.automaticSkipped;
      m_publication->publish(std::move(table));
      for (auto& warning : output.warnings)
      {
        m_warnings.push_back(std::move(warning));
      }
    }) | kdl::transform_error([&](auto e) {
      m_warnings.push_back(std::move(e.msg));
      // the build did not happen: the same input is built again at the next update
      m_started.reset();
    });
  }
  catch (const std::exception& e)
  {
    m_warnings.push_back(fmt::format("A probe job failed: {}", e.what()));
    m_started.reset();
  }
  if (m_waiting && !s_buildsHeld.load())
  {
    auto waiting = std::move(*m_waiting);
    m_waiting.reset();
    if (!m_started || !(*m_started == waiting))
    {
      start(std::move(waiting));
    }
  }
  return true;
}

std::shared_ptr<const ProbeTable> ProbeRegistry::table() const
{
  return m_publication->table();
}

std::vector<std::string> ProbeRegistry::takeWarnings()
{
  return std::exchange(m_warnings, {});
}

void ProbeRegistry::finishForTesting()
{
  while (m_job)
  {
    m_job->wait();
    collect();
  }
}

void setProbeBuildsHeldForTesting(const bool held)
{
  s_buildsHeld = held;
}

} // namespace tb::lit
