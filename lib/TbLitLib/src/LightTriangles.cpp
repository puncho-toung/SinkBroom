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

#include "LightTriangles.h"

#include "kd/result.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace tb::lit
{

LightTriangles lightTrianglesOf(
  const std::vector<std::uint16_t>& all, const std::vector<std::uint16_t>& kept)
{
  auto result = LightTriangles{};
  const auto triangles = all.size() / 3;
  if (kept.size() / 3 == triangles)
  {
    result.state = LightTriangles::State::All;
    return result;
  }
  result.culledTriangles = triangles - kept.size() / 3;
  if (kept.empty())
  {
    result.state = LightTriangles::State::None;
    return result;
  }
  result.state = LightTriangles::State::Runs;
  auto next = std::size_t{0};
  for (std::size_t t = 0; t < triangles && next + 2 < kept.size(); ++t)
  {
    const auto i = t * 3;
    if (
      all[i] != kept[next] || all[i + 1] != kept[next + 1]
      || all[i + 2] != kept[next + 2])
    {
      continue;
    }
    next += 3;
    if (
      !result.runs.empty()
      && result.runs.back().firstIndex + result.runs.back().indexCount == i)
    {
      result.runs.back().indexCount += 3;
    }
    else
    {
      result.runs.push_back(IndexRun{std::uint32_t(i), 3});
    }
  }
  return result;
}

bool LightTriangleCache::Key::operator<(const Key& other) const
{
  return std::tie(light, chunk) < std::tie(other.light, other.chunk);
}

std::vector<LightTriangleCache::Answer> LightTriangleCache::answer(
  const std::vector<Request>& requests)
{
  auto answers = std::vector<Answer>{};
  answers.reserve(requests.size());
  for (const auto& request : requests)
  {
    const auto kept = engineInteractionLightTris(
      request.baseLightProject,
      request.stamp.modelMatrix,
      request.bounds,
      request.positions,
      request.indexes);
    answers.push_back(
      Answer{request.key, request.stamp, lightTrianglesOf(request.indexes, kept)});
  }
  return answers;
}

void LightTriangleCache::store(std::vector<Answer> answers)
{
  for (auto& answer : answers)
  {
    // an answer for versions that have changed since is dropped
    if (const auto it = m_entries.find(answer.key);
        it != m_entries.end() && it->second.stamp == answer.stamp)
    {
      it->second.triangles =
        std::make_shared<const LightTriangles>(std::move(answer.triangles));
      it->second.asked = false;
      ++m_answered;
    }
  }
  ++m_version;
}

void LightTriangleCache::setCapacityForTesting(const std::size_t capacity)
{
  m_capacity = capacity;
}

void LightTriangleCache::beginBuild()
{
  for (auto& [key, entry] : m_entries)
  {
    entry.used = false;
  }
}

std::shared_ptr<const LightTriangles> LightTriangleCache::triangles(
  const PreviewId light,
  const std::uint64_t lightVersion,
  const EngineMat4& baseLightProject,
  const Chunk& chunk,
  const EngineGlMatrix& modelMatrix)
{
  if (!m_pending)
  {
    m_pending = std::make_shared<const LightTriangles>(
      LightTriangles{LightTriangles::State::Pending, {}, 0});
  }

  const auto key = Key{light, chunk.key};
  const auto stamp = Stamp{lightVersion, chunk.casterVersion, modelMatrix};
  auto& entry = m_entries[key];
  entry.used = true;
  if (entry.stamp == stamp && (entry.triangles || entry.asked))
  {
    // known, or asked for these versions already (queued, or in a job)
    return entry.triangles ? entry.triangles : m_pending;
  }
  entry.stamp = stamp;
  entry.triangles = nullptr;
  entry.asked = true;
  auto request = Request{};
  request.key = key;
  request.stamp = stamp;
  request.baseLightProject = baseLightProject;
  request.bounds = chunk.bounds;
  request.positions.reserve(chunk.verts.size());
  for (const auto& vert : chunk.verts)
  {
    request.positions.push_back(vert.xyz);
  }
  request.indexes = chunk.indexes;
  m_requests.push_back(std::move(request));
  return m_pending;
}

void LightTriangleCache::endBuild(CpuPool* pool)
{
  if (m_entries.size() > m_capacity)
  {
    std::erase_if(m_entries, [](const auto& entry) { return !entry.second.used; });
  }
  if (m_requests.empty() || m_job)
  {
    return;
  }
  auto requests = std::exchange(m_requests, {});
  if (!pool)
  {
    store(answer(requests));
    return;
  }
  m_jobAsked.clear();
  for (const auto& request : requests)
  {
    m_jobAsked.emplace_back(request.key, request.stamp);
  }
  // C20, C24: the job holds copies only, never the cache or the session
  m_job = pool->run(
    [requests =
       std::move(requests)](const CpuJobContext& context) -> Result<std::vector<Answer>> {
      if (context.stale())
      {
        return Error{"The light triangles' job was abandoned"};
      }
      return answer(requests);
    });
  ++m_jobs;
}

bool LightTriangleCache::collect()
{
  if (!m_job || m_job->wait_for(std::chrono::seconds{0}) != std::future_status::ready)
  {
    return false;
  }
  auto job = std::move(*m_job);
  m_job.reset();
  auto answers = std::vector<Answer>{};
  try
  {
    job.get().result
      | kdl::transform([&](std::vector<Answer> value) { answers = std::move(value); })
      | kdl::ignore();
  }
  catch (const std::exception&)
  {
    // a job the stopping pool dropped: its pairs are asked again by the next build
  }
  // a pair the job left unanswered is asked again when a build next asks for it
  for (const auto& [key, stamp] : std::exchange(m_jobAsked, {}))
  {
    if (const auto it = m_entries.find(key);
        it != m_entries.end() && it->second.stamp == stamp && !it->second.triangles)
    {
      it->second.asked = false;
    }
  }
  store(std::move(answers));
  return true;
}

bool LightTriangleCache::busy() const
{
  return m_job.has_value() || !m_requests.empty();
}

std::uint64_t LightTriangleCache::version() const
{
  return m_version;
}

std::size_t LightTriangleCache::size() const
{
  return m_entries.size();
}

std::uint64_t LightTriangleCache::jobs() const
{
  return m_jobs;
}

std::uint64_t LightTriangleCache::answered() const
{
  return m_answered;
}

void LightTriangleCache::clear()
{
  m_entries.clear();
  m_requests.clear();
  m_jobAsked.clear();
  m_job.reset();
}

} // namespace tb::lit
