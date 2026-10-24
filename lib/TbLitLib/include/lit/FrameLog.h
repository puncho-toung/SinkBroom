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
  // the frame's GPU time, from the command list's start to its end (LP0's column, which
  // timed LP0's clear: LP2.2b renamed it)
  std::optional<double> gpuFrameMs;
  std::string_view presentMode; // a presentModeName literal
  std::uint64_t swapchainGeneration = 0;
  std::uint64_t framesSkipped = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  // start and stop rows: the device-local heaps' usage and budget from
  // VK_EXT_memory_budget, when the device reports them
  std::optional<double> vramUsedMb;
  std::optional<double> vramBudgetMb;

  // LP2.1 (C16, C30, D72): every row: the device memory allocations live then (lit's
  // counter); frame rows, and the stop row: the bytes the frame uploaded, the textures
  // with a written level and the geometry arenas' bytes
  std::optional<std::int64_t> allocCount;
  std::optional<std::uint64_t> bytesUploaded;
  std::optional<std::uint64_t> texturesResident;
  std::optional<std::uint64_t> arenaBytes;

  // LP2.2b (C16), frame rows: the GPU time of each pass that ran (the tonemap's is
  // histogram, exposure and tonemapping; empty for a pass that did not run or whose time
  // never arrived)
  std::optional<double> gpuPrepassMs;
  std::optional<double> gpuInteractionsMs;
  std::optional<double> gpuTonemapMs;
  std::optional<double> gpuBlitMs;
  // draw calls (prepass, interactions, tonemap, blit) and the lit passes' own counts
  // (LitFrameStats): their draws, the draws skipped (D77, D76) and the pipelines pending
  std::optional<std::uint64_t> draws;
  std::optional<std::uint64_t> prepassDraws;
  std::optional<std::uint64_t> interactionDraws;
  std::optional<std::uint64_t> skippedDraws;
  std::optional<std::uint64_t> pendingPipelines;
  // lights with interactions in the view, drawn, and not drawn for the constant
  // buffer's frame budget (D76)
  std::optional<std::uint64_t> lightsVisible;
  std::optional<std::uint64_t> lightsDrawn;
  std::optional<std::uint64_t> lightsOverflowed;
  // D76: the volatile constant buffers' bytes and writes, the writes a frame may make,
  // and the push-constant bytes
  std::optional<std::uint64_t> constantBytes;
  std::optional<std::uint64_t> volatileWrites;
  std::optional<std::uint64_t> volatileWriteBudget;
  std::optional<std::uint64_t> pushConstantBytes;
  // D77: the frame is settled
  std::optional<bool> settled;
  // U2.3 (31-LP2-FIXES.md), frame rows: the textures and geometry arenas not made so far
  // because the device's allocations reached AllocationHeadroom below its limit
  std::optional<std::uint64_t> allocRefused;
  // V2.2 (32-LP2-VERIFY.md), frame rows: the triangles the static interaction cull took
  // out of the frame's interactions (surfaces left out whole and the culled part of the
  // ones drawn as runs, M-INTER-01)
  std::optional<std::uint64_t> culledTriangles;
  // LP4.1 (C16, C32), frame rows: the view's portal flow behind the frame's lists: the
  // view's area (-1 outside the world; empty without a tree), the areas flowed, the view
  // lights that went on to the pairing, the chains (AddAreaToView calls) and flowView's
  // time this frame (0 when the lists were kept)
  std::optional<std::int64_t> viewArea;
  std::optional<std::uint64_t> areasFlowed;
  std::optional<std::uint64_t> lightsInView;
  std::optional<std::uint64_t> portalChains;
  std::optional<double> flowMs;
  // LP4.2a (C34, C16), frame rows: the ambient pass's draws and its GPU time (empty when
  // it did not run or its time never arrived)
  std::optional<std::uint64_t> ambientDraws;
  std::optional<double> gpuAmbientMs;
  // LP4.3 (D86, D92, C36), frame rows: the ambient pass's writes of set 9's volatile
  // constant buffer (against volatile_write_budget, the per-layout frame budget) and the
  // drawn surfaces with a grid area
  std::optional<std::uint64_t> ambientWrites;
  std::optional<std::uint64_t> gridSurfaces;
  // LPF.1 (C44, P5.13), frame rows: the feed hand-overs the frame's table includes
  // (MaterialTable::feedApplied); the time in ms from the newest hand-over of a feed item
  // to this presented frame, the first whose table includes the item and in which its
  // change has landed (its slots Resident with every level written; a re-parse: the lists
  // built with its materialsRevision and its cells built), the largest when several land
  // in one frame, empty on other frames; and the bytes the service's override store holds
  // (MaterialTable::overrideBytes)
  std::optional<std::uint64_t> feedApplied;
  std::optional<double> feedMs;
  std::optional<std::uint64_t> overrideBytes;
  // LP5.1 (C45, C46), frame rows: the lights with interactions that entered the math
  // phase (pc.c_mathLights, RenderBackend.cpp:4022-4029) and the ambientLight lights
  // among the lights of both phases (the preview's own count)
  std::optional<std::uint64_t> mathLights;
  std::optional<std::uint64_t> ambientLights;
  // LP5.2a (C46), frame rows: the stage pass's draws and its SL_AMBIENT stages that drew
  // nothing (LitFrameStats::stageDraws, stageSkipped), and its GPU time
  std::optional<std::uint64_t> stageDraws;
  std::optional<std::uint64_t> stageSkipped;
  std::optional<double> gpuStagesMs;
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
