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

#include "FrameTimers.h"

#include <algorithm>

namespace tb::lit
{
namespace
{

// the row's field of a timed pass
std::optional<double>& rowTime(FrameLogRow& row, const TimedPass pass)
{
  switch (pass)
  {
  case TimedPass::Prepass:
    return row.gpuPrepassMs;
  case TimedPass::Interactions:
    return row.gpuInteractionsMs;
  case TimedPass::Tonemap:
    return row.gpuTonemapMs;
  case TimedPass::Blit:
    return row.gpuBlitMs;
  case TimedPass::Ambient:
    return row.gpuAmbientMs;
  case TimedPass::Stages:
    return row.gpuStagesMs;
  case TimedPass::Frame:
    break;
  }
  return row.gpuFrameMs;
}

} // namespace

void FrameTimers::createQueries(nvrhi::IDevice& device, const bool timerQueries)
{
  if (timerQueries)
  {
    for (auto& slot : m_slots)
    {
      for (auto& query : slot.queries)
      {
        query = device.createTimerQuery();
      }
    }
  }
}

std::optional<std::string> FrameTimers::openLog(const std::filesystem::path& path)
{
  return m_log.open(path);
}

nvrhi::ITimerQuery* FrameTimers::query(const std::size_t slot, const TimedPass pass) const
{
  return m_slots[slot].queries[std::size_t(pass)].Get();
}

void FrameTimers::frameStarted(const std::size_t slot)
{
  m_slots[slot].timed = {};
}

void FrameTimers::passTimed(const std::size_t slot, const TimedPass pass)
{
  m_slots[slot].timed[std::size_t(pass)] = true;
}

void FrameTimers::frameSubmitted(const std::size_t slot)
{
  auto& s = m_slots[slot];
  for (std::size_t i = 0; i < TimedPassCount; ++i)
  {
    s.pending[i] = s.timed[i] && s.queries[i];
  }
  s.timed = {};
}

void FrameTimers::frameShown(const std::size_t slot, const FrameLogRow& row)
{
  if (pending(slot))
  {
    m_slots[slot].row = row;
  }
  else
  {
    m_log.append(row);
  }
}

void FrameTimers::append(const FrameLogRow& row)
{
  m_log.append(row);
}

bool FrameTimers::pending(const std::size_t slot) const
{
  const auto& s = m_slots[slot];
  return std::ranges::any_of(s.pending, [](const bool p) { return p; });
}

void FrameTimers::resolve(nvrhi::IDevice& device, const std::size_t slot)
{
  auto& s = m_slots[slot];
  if (!pending(slot))
  {
    return;
  }
  for (std::size_t i = 0; i < TimedPassCount; ++i)
  {
    if (!s.pending[i])
    {
      continue;
    }
    const auto seconds = device.getTimerQueryTime(s.queries[i]);
    s.pending[i] = false;
    if (s.row)
    {
      rowTime(*s.row, TimedPass(i)) = double(seconds) * 1000.0;
    }
  }
  if (s.row)
  {
    m_log.append(*s.row);
    s.row.reset();
  }
}

void FrameTimers::resolveAll(nvrhi::IDevice& device, const std::uint64_t nextFrameIndex)
{
  for (std::size_t i = 0; i < FramesInFlight; ++i)
  {
    resolve(device, std::size_t((nextFrameIndex + i) % FramesInFlight));
  }
}

void FrameTimers::flushUnresolved(const std::uint64_t nextFrameIndex)
{
  for (std::size_t i = 0; i < FramesInFlight; ++i)
  {
    auto& s = m_slots[std::size_t((nextFrameIndex + i) % FramesInFlight)];
    if (s.row)
    {
      m_log.append(*s.row);
      s.row.reset();
    }
  }
}

std::optional<std::string> FrameTimers::closeLog()
{
  return m_log.close();
}

void FrameTimers::releaseQueries()
{
  for (auto& slot : m_slots)
  {
    slot = Slot{};
  }
}

} // namespace tb::lit
