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

#include "lit/FrameLog.h"
#include "lit/LitConfig.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tb::lit
{

// C16 (LP2.2b): what a frame times, each with a query of its own per frame slot: the
// whole command list, and the passes (the tonemap's covers histogram, exposure and
// tonemapping; LP4.2a adds the ambient pass's, LP5.2a the stage pass's, LP5.3 pass 9's,
// LP3.1 pass 6's). LP6.0 (C65) reserves LP6's: the G-buffer normals (pass 3) and SSAO
// (pass 4), filled by LP6.5a, and TAA (pass 11), by LP6.6; until then no pass begins
// them, and their columns stay empty
enum class TimedPass
{
  Frame,
  Prepass,
  Interactions,
  Tonemap,
  Blit,
  Ambient,
  Stages,
  Fog,
  Shadows,
  Gbuffer,
  Ssao,
  Taa,
};
inline constexpr auto TimedPassCount = std::size_t{12};

/**
 * The frame log (C16) and the timer queries that time each frame and its passes
 * (R4-2.20, §3.10): a fixed ring of TimedPassCount queries per frame in flight. Frame N
 * reuses the queries of frame N - FramesInFlight, which the render thread has waited for
 * before it records frame N; a query is read only when its pass was begun and ended in a
 * submitted frame. A frame's row is written once its GPU times are known (at the slot's
 * reuse, or from the idle tick), or at stop. Render thread only.
 */
class FrameTimers
{
private:
  struct Slot
  {
    std::array<nvrhi::TimerQueryHandle, TimedPassCount> queries;
    // begun and ended in the frame being recorded
    std::array<bool, TimedPassCount> timed = {};
    // ended in a submitted command list and not read yet
    std::array<bool, TimedPassCount> pending = {};
    // the shown frame's row, waiting for the times
    std::optional<FrameLogRow> row;
  };

  std::array<Slot, FramesInFlight> m_slots;
  FrameLogWriter m_log;

public:
  // the queries, when the device has timestamps
  void createQueries(nvrhi::IDevice& device, bool timerQueries);
  // an empty path means no log; a log that cannot be opened is an error message
  std::optional<std::string> openLog(const std::filesystem::path& path);

  // the query of a pass in a frame slot, or null without timestamps
  nvrhi::ITimerQuery* query(std::size_t slot, TimedPass pass) const;

  // a frame starts recording into the slot: no pass of it is timed yet
  void frameStarted(std::size_t slot);
  // the pass's query was begun and ended in the frame being recorded
  void passTimed(std::size_t slot, TimedPass pass);
  // the frame recorded into the slot was submitted: its timed passes are pending
  void frameSubmitted(std::size_t slot);
  // the submitted frame was shown; its row is written once its time is known
  void frameShown(std::size_t slot, const FrameLogRow& row);
  // a row that needs no GPU time, such as a session's start and stop
  void append(const FrameLogRow& row);

  // a query of the slot is pending
  bool pending(std::size_t slot) const;
  // reads the times of the slot's finished frame and writes its row
  void resolve(nvrhi::IDevice& device, std::size_t slot);
  // every slot, oldest frame first, so that rows stay in frame order
  void resolveAll(nvrhi::IDevice& device, std::uint64_t nextFrameIndex);

  // at stop: writes the rows whose time never arrived, with an empty field
  void flushUnresolved(std::uint64_t nextFrameIndex);
  // closes the log; the first write that failed, if any
  std::optional<std::string> closeLog();

  // teardown step 2
  void releaseQueries();
};

} // namespace tb::lit
