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

/**
 * The frame log (C16) and the timer queries that time each frame's pass (R4-2.20): one
 * query per frame in flight, because LP0 draws one pass. Frame N reuses the query of
 * frame N - FramesInFlight, which the render thread has waited for before it records
 * frame N. A frame's row is written once its GPU time is known, or at stop. Render thread
 * only.
 */
class FrameTimers
{
private:
  struct Slot
  {
    nvrhi::TimerQueryHandle query;
    // ended in a submitted command list and not read yet
    bool pending = false;
    // the shown frame's row, waiting for the time
    std::optional<FrameLogRow> row;
  };

  std::array<Slot, FramesInFlight> m_slots;
  FrameLogWriter m_log;

public:
  // the queries, when the device has timestamps
  void createQueries(nvrhi::IDevice& device, bool timerQueries);
  // an empty path means no log; a log that cannot be opened is an error message
  std::optional<std::string> openLog(const std::filesystem::path& path);

  // the query of a frame slot, or null without timestamps
  nvrhi::ITimerQuery* query(std::size_t slot) const;

  // a frame that used the slot's query was submitted
  void frameSubmitted(std::size_t slot);
  // the submitted frame was shown; its row is written once its time is known
  void frameShown(std::size_t slot, const FrameLogRow& row);

  bool pending(std::size_t slot) const;
  // reads the time of the slot's finished frame and writes its row
  void resolve(nvrhi::IDevice& device, std::size_t slot);
  // every slot, oldest frame first, so that rows stay in frame order
  void resolveAll(nvrhi::IDevice& device, std::uint64_t nextFrameIndex);

  // At stop: writes the rows whose time never arrived, with an empty field, and closes
  // the log; the first write that failed, if any.
  std::optional<std::string> closeLog(std::uint64_t nextFrameIndex);

  // teardown step 2
  void releaseQueries();
};

} // namespace tb::lit
