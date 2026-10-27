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
         "bytes,settled,alloc_refused,culled_triangles,view_area,areas_flowed,lights_in_"
         "view,portal_chains,flow_ms,ambient_draws,gpu_ambient_ms,ambient_writes,grid_"
         "surfaces,feed_applied,feed_ms,override_bytes,math_lights,ambient_lights,stage_"
         "draws,stage_skipped,gpu_stages_ms,fog_draws,blend_draws,gpu_fog_ms,proc_bytes,"
         "model_entities,model_instances,model_draws,models_pending,models_skinned,dress_"
         "surfaces,dress_unmatched,adopted_islands,adopted_tris,unmatched_islands,curved_"
         "islands,inlined_surfaces\n");
  }

  SECTION("formatFrameLogRow")
  {
    CHECK(
      formatFrameLogRow(exampleRow())
      == "7,frame,12,0.153,0.051,16.205,0.021,mailbox,2,3,800,600,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"
         ",,,,,,,,,,,,,,,,,,,,,\n");

    // LP2.1: the allocations, the frame's uploads, the textures and the arenas
    auto withResources = exampleRow();
    withResources.allocCount = 211;
    withResources.bytesUploaded = 33554432;
    withResources.texturesResident = 205;
    withResources.arenaBytes = 134217728;
    CHECK(
      formatFrameLogRow(withResources)
      == "7,frame,12,0.153,0.051,16.205,0.021,mailbox,2,3,800,600,,,211,33554432,205,"
         "134217728,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n");

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
         "046,0.008,30,11,17,0,0,2,2,0,800,2,2730,660,1,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n");
    withPasses.settled = false;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",660,0,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    // U2.3: the allocation headroom's refusals; V2.2: the triangles the cull took out
    withPasses.allocRefused = 3;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",660,0,3,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    withPasses.culledTriangles = 12;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",660,0,3,12,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    // LP4.1: the view's portal flow: the view's area (-1 outside the world), the areas
    // flowed, the lights in view, the chains and the flow's time
    withPasses.viewArea = -1;
    withPasses.areasFlowed = 8;
    withPasses.lightsInView = 6;
    withPasses.portalChains = 8;
    withPasses.flowMs = 0.04271;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",660,0,3,12,-1,8,6,8,0.043,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    withPasses.viewArea = 3;
    withPasses.flowMs = 0.0;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",12,3,8,6,8,0.000,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    // LP4.2a: the ambient pass's draws and its GPU time
    withPasses.ambientDraws = 9;
    withPasses.gpuAmbientMs = 0.01849;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",12,3,8,6,8,0.000,9,0.018,,,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    // LP4.3: set 9's writes and the surfaces on the light-grid path
    withPasses.ambientWrites = 4;
    withPasses.gridSurfaces = 7;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",8,0.000,9,0.018,4,7,,,,,,,,,,,,,,,,,,,,,,,,\n"));
    // LPF.1: the frame's table's feed counter, the landing frame's feed time and the
    // override store's bytes; a frame on which no feed item lands has no feed time
    withPasses.feedApplied = 12;
    withPasses.overrideBytes = 16384;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",9,0.018,4,7,12,,16384,,,,,,,,,,,,,,,,,,,,,\n"));
    withPasses.feedMs = 21.34567;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",9,0.018,4,7,12,21.346,16384,,,,,,,,,,,,,,,,,,,,,\n"));
    // LP5.1: the math phase's lights and the ambientLight lights
    withPasses.mathLights = 2;
    withPasses.ambientLights = 1;
    CHECK(formatFrameLogRow(withPasses)
            .ends_with(",12,21.346,16384,2,1,,,,,,,,,,,,,,,,,,,\n"));
    // LP5.2a: the stage pass's draws, its skipped stages and its GPU time
    withPasses.stageDraws = 5;
    withPasses.stageSkipped = 2;
    CHECK(formatFrameLogRow(withPasses).ends_with(",16384,2,1,5,2,,,,,,,,,,,,,,,,,\n"));
    withPasses.gpuStagesMs = 0.03149;
    CHECK(
      formatFrameLogRow(withPasses).ends_with(",16384,2,1,5,2,0.031,,,,,,,,,,,,,,,,\n"));
    // LP5.3: pass 9's fog draws, blend draws and its GPU time
    withPasses.fogDraws = 3;
    withPasses.blendDraws = 1;
    CHECK(formatFrameLogRow(withPasses).ends_with(",5,2,0.031,3,1,,,,,,,,,,,,,,\n"));
    withPasses.gpuFogMs = 0.01234;
    CHECK(formatFrameLogRow(withPasses).ends_with(",5,2,0.031,3,1,0.012,,,,,,,,,,,,,\n"));

    // a row without its GPU time has an empty field
    auto row = exampleRow();
    row.gpuFrameMs.reset();
    row.acquireMs = 0.0;
    row.presentMs = 0.0;
    row.presentMode = "headless";
    row.swapchainGeneration = 0;
    CHECK(
      formatFrameLogRow(row)
      == "7,frame,12,0.153,0.000,0.000,,headless,0,3,800,600,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"
         ",,,,,,,,,,,,,,,,,,,,,\n");

    // a session's start and stop carry the VRAM in use, and nothing of a frame
    CHECK(
      formatFrameLogRow(sessionRow(FrameLogEvent::Start))
      == "7,start,,,,,,,,,,,812.3,6912.0,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n");
    auto stop = sessionRow(FrameLogEvent::Stop);
    stop.vramUsedMb.reset();
    stop.vramBudgetMb.reset();
    stop.allocCount = 9;
    CHECK(
      formatFrameLogRow(stop)
      == "7,stop,,,,,,,,,,,,,9,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n");

    // LP7.0 (C52, C54, C55, P7.20): LP7's eleven counts, 0 until their sub-packages
    // fill them, and the stop row's proc_bytes; a frame row leaves proc_bytes empty
    auto withLp7 = withPasses;
    withLp7.modelEntities = 0u;
    withLp7.modelInstances = 0u;
    withLp7.modelDraws = 0u;
    withLp7.modelsPending = 0u;
    withLp7.modelsSkinned = 0u;
    withLp7.dressSurfaces = 0u;
    withLp7.dressUnmatched = 0u;
    withLp7.adoptedIslands = 0u;
    withLp7.adoptedTris = 0u;
    withLp7.unmatchedIslands = 0u;
    withLp7.curvedIslands = 0u;
    // LP7.3 (C54, K42): and the inlined copies, the twelfth
    withLp7.inlinedSurfaces = 0u;
    CHECK(formatFrameLogRow(withLp7).ends_with(",3,1,0.012,,0,0,0,0,0,0,0,0,0,0,0,0\n"));
    withLp7.modelEntities = 3u;
    withLp7.curvedIslands = 2u;
    withLp7.inlinedSurfaces = 4u;
    CHECK(formatFrameLogRow(withLp7).ends_with(",0.012,,3,0,0,0,0,0,0,0,0,0,2,4\n"));
    auto stopWithTree = stop;
    stopWithTree.procBytes = 8368u;
    CHECK(
      formatFrameLogRow(stopWithTree)
      == "7,stop,,,,,,,,,,,,,9,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,8368,,,,,,,,,,,,\n");

    // as many fields as the header
    const auto header = fieldCount(frameLogHeader());
    // LP4.2a: 44 with the ambient pass's two; LP4.3: 46 with set 9's writes and the grid
    // surfaces; LPF.1: 49 with the feed's three; LP5.1: 51 with the math and ambient
    // lights; LP5.2a: 54 with the stage pass's three; LP5.3: 57 with pass 9's three;
    // LP7.0: 69 with proc_bytes and LP7's eleven counts; LP7.3: 70 with inlined_surfaces
    CHECK(header == 70u);
    CHECK(fieldCount(formatFrameLogRow(exampleRow())) == header);
    CHECK(fieldCount(formatFrameLogRow(withResources)) == header);
    CHECK(fieldCount(formatFrameLogRow(withPasses)) == header);
    CHECK(fieldCount(formatFrameLogRow(sessionRow(FrameLogEvent::Start))) == header);
    CHECK(fieldCount(formatFrameLogRow(stop)) == header);
    CHECK(fieldCount(formatFrameLogRow(withLp7)) == header);
    CHECK(fieldCount(formatFrameLogRow(stopWithTree)) == header);
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

TEST_CASE("FrameLog: the casters' companion log (LP3.0, P3.14)")
{
  // beside the frame log, its stem with -casters; none without a frame log
  CHECK(
    casterLogPath(std::filesystem::path{"out"} / "frame-log.csv")
    == std::filesystem::path{"out"} / "frame-log-casters.csv");
  CHECK(casterLogPath({}).empty());
  // a row has the header's fields
  const auto header = std::string{casterLogHeader()};
  auto row = CasterLogRow{};
  row.session = 2;
  row.frameIndex = 47;
  row.settled = true;
  row.casterLights = 11;
  row.casterPairs = 927;
  row.casterPairsResident = 927;
  row.casters = 698;
  row.casterMs = 1.5;
  const auto text = formatCasterLogRow(row);
  CHECK(std::ranges::count(header, ',') == std::ranges::count(text, ','));
  CHECK(header.back() == '\n');
  CHECK(text == "2,47,1,0,0,11,927,927,0,698,1.500,0,0\n");
}

} // namespace tb::lit
