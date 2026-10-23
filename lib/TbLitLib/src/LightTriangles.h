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
#include "lit/ChunkBuilder.h"
#include "lit/CpuPool.h"
#include "lit/EngineLight.h"
#include "lit/Snapshot.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tb::lit
{

// DX.2: the (light, chunk) pairs the light triangles' cache keeps before it drops the
// ones the last build did not ask for
inline constexpr auto LightTriangleCacheSize = std::size_t{65536};

// a run of a chunk's triangles, as indexes into its index list
struct IndexRun
{
  std::uint32_t firstIndex = 0;
  std::uint32_t indexCount = 0;

  bool operator==(const IndexRun&) const = default;
};

/**
 * DX.2 (31-LP2-FIXES.md): a static interaction's light triangles of one chunk for one
 * light: all of them, none (the engine makes no interaction surface), runs of them, or
 * not known yet (the chunk is drawn whole until they are; the frame is unconverged).
 */
struct LightTriangles
{
  enum class State
  {
    All,
    None,
    Runs,
    Pending,
  };

  State state = State::All;
  std::vector<IndexRun> runs;
  // the triangles culled (statistics)
  std::size_t culledTriangles = 0;
};

/**
 * The runs of kept within all, the triangles of a chunk's index list: kept is a
 * subsequence of all's triangles in their order (engineInteractionLightTris' answer), so
 * each kept triangle is the next equal one of all. All when every triangle is kept, None
 * when none is. Pure.
 */
LightTriangles lightTrianglesOf(
  const std::vector<std::uint16_t>& all, const std::vector<std::uint16_t>& kept);

/**
 * DX.2: the light triangles of every (light, chunk) pair the frontend lights, as the
 * engine makes them once for its static interactions (GenerateAllInteractions: every
 * static-model entity, the world's areas, brush entities and patches alike), cached per
 * (light version, chunk caster version, the chunk's model matrix) and replaced when any
 * of them changes; a pair a camera move leaves out of the lists is kept for when it comes
 * back, and once the cache holds more than LightTriangleCacheSize pairs the ones the last
 * build did not ask for go. A miss is answered Pending and queued; endBuild hands the
 * misses to the CPU pool as one job (one copy of a chunk's positions and indexes, shared
 * by every light that asks for it (V2.5), the light's projection, the model matrix),
 * which runs the copied code (engineInteractionLightTris) and sees a Stop before each
 * pair; collect takes a finished job's answers and bumps version, which the lists' key
 * holds. Without a pool (tests) the misses are answered at endBuild. Render thread.
 */
class LightTriangleCache
{
public:
  struct Key
  {
    PreviewId light = NoPreviewId;
    ChunkKey chunk;

    bool operator<(const Key& other) const;
  };

private:
  struct Stamp
  {
    std::uint64_t lightVersion = 0;
    std::uint64_t casterVersion = 0;
    EngineGlMatrix modelMatrix = {};

    bool operator==(const Stamp&) const = default;
  };

  struct Entry
  {
    Stamp stamp;
    std::shared_ptr<const LightTriangles> triangles;
    bool used = false;
    // a request for stamp is queued or in a job
    bool asked = false;
  };

  // V2.5: a chunk's positions and indexes, copied once per build for every light
  struct ChunkGeometry
  {
    std::uint64_t casterVersion = 0;
    std::vector<EngineVec3> positions;
    std::vector<std::uint16_t> indexes;
  };

  struct Request
  {
    Key key;
    Stamp stamp;
    EngineMat4 baseLightProject = {};
    EngineBounds bounds = {};
    std::shared_ptr<const ChunkGeometry> geometry;
  };

  struct Answer
  {
    Key key;
    Stamp stamp;
    LightTriangles triangles;
  };

  std::map<Key, Entry> m_entries;
  std::vector<Request> m_requests;
  // the build's copies of the chunks its misses name (V2.5)
  std::map<ChunkKey, std::shared_ptr<const ChunkGeometry>> m_buildGeometry;
  std::optional<std::future<CpuJobResult<std::vector<Answer>>>> m_job;
  // what the job in flight was asked
  std::vector<std::pair<Key, Stamp>> m_jobAsked;
  // the answer of every pair not known yet
  std::shared_ptr<const LightTriangles> m_pending;
  std::uint64_t m_version = 0;
  std::size_t m_capacity = LightTriangleCacheSize;
  std::uint64_t m_jobs = 0;
  std::uint64_t m_answered = 0;

  // a Stop (context's) is seen before each pair; without a context (no pool) none comes
  static Result<std::vector<Answer>> answer(
    const std::vector<Request>& requests, const CpuJobContext* context);
  void store(std::vector<Answer> answers);

public:
  LightTriangleCache() = default;

  // a build of the lists begins: every entry is unused until asked for
  void beginBuild();
  // tests: the bound instead of LightTriangleCacheSize
  void setCapacityForTesting(std::size_t capacity);
  /**
   * The light triangles of chunk lit by the light of record (its derived projection),
   * the chunk's vertices in the space of modelMatrix; Pending (and queued) when not
   * known for these versions.
   */
  std::shared_ptr<const LightTriangles> triangles(
    PreviewId light,
    std::uint64_t lightVersion,
    const EngineMat4& baseLightProject,
    const Chunk& chunk,
    const EngineGlMatrix& modelMatrix);
  // the build ended: beyond the bound the entries not asked for go; the misses go to pool
  // (none: they are answered at once)
  void endBuild(CpuPool* pool);
  // a finished job's answers are taken; whether any was (the lists must be built again)
  bool collect();
  // a job runs or misses wait for one
  bool busy() const;
  // bumped by every answer taken
  std::uint64_t version() const;
  std::size_t size() const;
  // the jobs handed to the pool, and the answers taken (statistics)
  std::uint64_t jobs() const;
  std::uint64_t answered() const;
  void clear();

  deleteCopyAndMove(LightTriangleCache);
};

} // namespace tb::lit
