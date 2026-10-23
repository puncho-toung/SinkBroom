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

#include "lit/FrameLog.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

FrameLogRow exampleRow()
{
  auto row = FrameLogRow{};
  row.session = 7;
  row.frameIndex = 12;
  row.cpuRecordMs = 0.15349;
  row.acquireMs = 0.0512;
  row.presentMs = 16.2049;
  row.gpuFrameMs = 0.0214;
  row.presentMode = "mailbox";
  row.swapchainGeneration = 2;
  row.framesSkipped = 3;
  row.width = 800;
  row.height = 600;
  return row;
}

FrameLogRow sessionRow(const FrameLogEvent event)
{
  auto row = FrameLogRow{};
  row.session = 7;
  row.event = event;
  row.vramUsedMb = 812.34;
  row.vramBudgetMb = 6912.0;
  return row;
}

std::size_t fieldCount(const std::string_view line)
{
  return std::size_t(std::count(line.begin(), line.end(), ',')) + 1;
}

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

} // namespace

TEST_CASE("FrameLog")
{
  SECTION("frameLogHeader")
  {
    CHECK(
      frameLogHeader()
      == "session,event,frame,cpu_record_ms,acquire_ms,present_ms,gpu_frame_ms,present_"
         "mode,swapchain_generation,frames_skipped,width,height,vram_used_mb,vram_budget_"
         "mb,alloc_count,bytes_uploaded,textures_resident,arena_bytes,gpu_prepass_ms,gpu_"
         "interactions_ms,gpu_tonemap_ms,gpu_blit_ms,draws,prepass_draws,interaction_"
         "draws,skipped_draws,pending_pipelines,lights_visible,lights_drawn,lights_"
         "overflowed,constant_bytes,volatile_writes,volatile_write_budget,push_constant_"
         "bytes,settled,alloc_refused,culled_triangles\n");
  }

  SECTION("formatFrameLogRow")
  {
    CHECK(
      formatFrameLogRow(exampleRow())
      == "7,frame,12,0.153,0.051,16.205,0.021,mailbox,2,3,800,600,,,,,,,,,,,,,,,,,,,,,,,,,\n");

    // LP2.1: the allocations, the frame's uploads, the textures and the arenas
    auto withResources = exampleRow();
    withResources.allocCount = 211;
    withResources.bytesUploaded = 33554432;
    withResources.texturesResident = 205;
    withResources.arenaBytes = 134217728;
    CHECK(
      formatFrameLogRow(withResources)
      == "7,frame,12,0.153,0.051,16.205,0.021,mailbox,2,3,800,600,,,211,33554432,205,"
         "134217728,,,,,,,,,,,,,,,,,,,\n");

    // LP2.2b: the passes' GPU times, the draws, the lights, the constant buffers' writes
    // and whether the frame is settled
    auto withPasses = exampleRow();
    withPasses.gpuPrepassMs = 0.0123;
    withPasses.gpuInteractionsMs = 0.2501;
    withPasses.gpuTonemapMs = 0.0456;
    withPasses.gpuBlitMs = 0.0077;
    withPasses.draws = 30;
    withPasses.prepassDraws = 11;
    withPasses.interactionDraws = 17;
    withPasses.skippedDraws = 0;
    withPasses.pendingPipelines = 0;
    withPasses.lightsVisible = 2;
    withPasses.lightsDrawn = 2;
    withPasses.lightsOverflowed = 0;
    withPasses.constantBytes = 800;
    withPasses.volatileWrites = 2;
    withPasses.volatileWriteBudget = 2730;
    withPasses.pushConstantBytes = 660;
    withPasses.settled = true;
    CHECK(
      formatFrameLogRow(withPasses)
      == "7,frame,12,0.153,0.051,16.205,0.021,mailbox,2,3,800,600,,,,,,,0.012,0.250,0."
         "046,0.008,30,11,17,0,0,2,2,0,800,2,2730,660,1,,\n");
    withPasses.settled = false;
    CHECK(formatFrameLogRow(withPasses).ends_with(",660,0,,\n"));
    // U2.3: the allocation headroom's refusals; V2.2: the triangles the cull took out
    withPasses.allocRefused = 3;
    CHECK(formatFrameLogRow(withPasses).ends_with(",660,0,3,\n"));
    withPasses.culledTriangles = 12;
    CHECK(formatFrameLogRow(withPasses).ends_with(",660,0,3,12\n"));

    // a row without its GPU time has an empty field
    auto row = exampleRow();
    row.gpuFrameMs.reset();
    row.acquireMs = 0.0;
    row.presentMs = 0.0;
    row.presentMode = "headless";
    row.swapchainGeneration = 0;
    CHECK(
      formatFrameLogRow(row)
      == "7,frame,12,0.153,0.000,0.000,,headless,0,3,800,600,,,,,,,,,,,,,,,,,,,,,,,,,\n");

    // a session's start and stop carry the VRAM in use, and nothing of a frame
    CHECK(
      formatFrameLogRow(sessionRow(FrameLogEvent::Start))
      == "7,start,,,,,,,,,,,812.3,6912.0,,,,,,,,,,,,,,,,,,,,,,,\n");
    auto stop = sessionRow(FrameLogEvent::Stop);
    stop.vramUsedMb.reset();
    stop.vramBudgetMb.reset();
    stop.allocCount = 9;
    CHECK(formatFrameLogRow(stop) == "7,stop,,,,,,,,,,,,,9,,,,,,,,,,,,,,,,,,,,,,\n");

    // as many fields as the header
    const auto header = fieldCount(frameLogHeader());
    CHECK(header == 37u);
    CHECK(fieldCount(formatFrameLogRow(exampleRow())) == header);
    CHECK(fieldCount(formatFrameLogRow(withResources)) == header);
    CHECK(fieldCount(formatFrameLogRow(withPasses)) == header);
    CHECK(fieldCount(formatFrameLogRow(sessionRow(FrameLogEvent::Start))) == header);
    CHECK(fieldCount(formatFrameLogRow(stop)) == header);
  }
}

TEST_CASE("FrameLogWriter")
{
  SECTION("open, append and close")
  {
    const auto path =
      std::filesystem::temp_directory_path() / "TbLitLibTest-frame-log.csv";
    std::filesystem::remove(path);

    auto writer = FrameLogWriter{};
    CHECK_FALSE(writer.open(path));
    CHECK(writer.isOpen());
    writer.append(exampleRow());
    auto second = exampleRow();
    second.frameIndex = 13;
    writer.append(second);
    CHECK_FALSE(writer.close());
    CHECK_FALSE(writer.isOpen());

    const auto firstSession = std::string{frameLogHeader()}
                              + formatFrameLogRow(exampleRow())
                              + formatFrameLogRow(second);
    CHECK(readFile(path) == firstSession);

    // appending after close writes nothing
    writer.append(exampleRow());
    CHECK(readFile(path) == firstSession);

    // a log that exists is appended to, without a second header (review L3.16)
    CHECK_FALSE(writer.open(path));
    writer.append(sessionRow(FrameLogEvent::Start));
    CHECK_FALSE(writer.close());
    CHECK(
      readFile(path)
      == firstSession + formatFrameLogRow(sessionRow(FrameLogEvent::Start)));
    std::filesystem::remove(path);
  }

  SECTION("a log of another format is not appended to")
  {
    // review V.8: rows of the new format under an older header
    const auto path =
      std::filesystem::temp_directory_path() / "TbLitLibTest-old-frame-log.csv";
    const auto oldLog = std::string{"frame,cpu_record_ms,gpu_clear_ms\n0,1.000,0.500\n"};
    {
      auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
      file << oldLog;
    }

    auto writer = FrameLogWriter{};
    const auto error = writer.open(path);
    REQUIRE(error);
    CHECK(error->find("another format") != std::string::npos);
    CHECK_FALSE(writer.isOpen());
    writer.append(exampleRow());
    CHECK_FALSE(writer.close());
    CHECK(readFile(path) == oldLog);
    std::filesystem::remove(path);
  }

  SECTION("open with an empty path is no log")
  {
    auto writer = FrameLogWriter{};
    CHECK_FALSE(writer.open({}));
    CHECK_FALSE(writer.isOpen());
    writer.append(exampleRow());
    CHECK_FALSE(writer.close());
  }

  SECTION("open with an unopenable path is an error message")
  {
    auto writer = FrameLogWriter{};
    const auto error = writer.open(
      std::filesystem::temp_directory_path() / "no-such-directory-TbLitLibTest"
      / "log.csv");
    CHECK(error);
    CHECK_FALSE(writer.isOpen());
  }
}

} // namespace tb::lit
