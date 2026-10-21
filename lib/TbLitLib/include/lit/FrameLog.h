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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{

enum class FrameLogEvent
{
  // a session started; the row has the device-local VRAM in use then
  Start,
  // a presented frame
  Frame,
  // a session stopped; the row has the device-local VRAM in use then
  Stop,
};

/**
 * One row of the frame log (C16, LP0 subset). Every row names its session, so that one
 * log can hold several sessions, for example Start and Stop ten times. A frame's GPU time
 * comes from the timer-query ring once that frame has finished on the GPU, so a frame row
 * is complete only FramesInFlight frames later, or at stop.
 */
struct FrameLogRow
{
  // the session's generation
  std::uint64_t session = 0;
  FrameLogEvent event = FrameLogEvent::Frame;

  // frame rows
  std::uint64_t frameIndex = 0;
  double cpuRecordMs = 0.0;
  // how long the acquire waited for an image, and how long the present call took, which
  // is where FIFO blocks; both 0 without a swapchain
  double acquireMs = 0.0;
  double presentMs = 0.0;
  std::optional<double> gpuClearMs;
  std::string_view presentMode; // a presentModeName literal
  std::uint64_t swapchainGeneration = 0;
  std::uint64_t framesSkipped = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  // start and stop rows: the device-local heaps' usage and budget from
  // VK_EXT_memory_budget, when the device reports them
  std::optional<double> vramUsedMb;
  std::optional<double> vramBudgetMb;
};

// the CSV header line, ending in a newline
std::string_view frameLogHeader();

// one CSV line, ending in a newline; a field that does not apply or is unknown is empty
std::string formatFrameLogRow(const FrameLogRow& row);

/**
 * Writes the frame log on the render thread: rows are buffered in memory, written out in
 * large blocks, and flushed when the log is closed at stop. A log that exists is appended
 * to; the header is written only into an empty file.
 */
class FrameLogWriter
{
public:
  // an empty path means no log; a file that cannot be opened is an error message
  std::optional<std::string> open(const std::filesystem::path& path);
  bool isOpen() const;

  void append(const FrameLogRow& row);
  // writes what is buffered and closes the file; the first write that failed, if any
  std::optional<std::string> close();

private:
  void writeBuffer();

  std::filesystem::path m_path;
  std::ofstream m_file;
  std::string m_buffer;
  std::optional<std::string> m_error;
};

} // namespace tb::lit
