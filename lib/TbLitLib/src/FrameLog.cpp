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

#include <fmt/format.h>

#include <cstdlib>
#include <system_error>
#include <utility>

namespace tb::lit
{
namespace
{

// rows are written out once this much is buffered (about 600 frames)
constexpr auto WriteThreshold = std::size_t{64 * 1024};


std::string optionalField(
  const std::optional<double>& value, const std::string_view format)
{
  return value ? fmt::format(fmt::runtime(format), *value) : std::string{};
}

template <typename T>
std::string optionalCount(const std::optional<T>& value)
{
  return value ? fmt::format("{}", *value) : std::string{};
}

// LP2.1's columns: alloc_count, bytes_uploaded, textures_resident, arena_bytes
std::string resourceFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{}",
    optionalCount(row.allocCount),
    optionalCount(row.bytesUploaded),
    optionalCount(row.texturesResident),
    optionalCount(row.arenaBytes));
}

// LP2.2b's columns: the passes' GPU times, the draws, the lights, the constant buffers'
// writes and whether the frame is settled; empty in a start or stop row
std::string passFields(const FrameLogRow& row)
{
  const auto settled =
    row.settled ? std::string{*row.settled ? "1" : "0"} : std::string{};
  return fmt::format(
    "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
    optionalField(row.gpuPrepassMs, "{:.3f}"),
    optionalField(row.gpuInteractionsMs, "{:.3f}"),
    optionalField(row.gpuTonemapMs, "{:.3f}"),
    optionalField(row.gpuBlitMs, "{:.3f}"),
    optionalCount(row.draws),
    optionalCount(row.prepassDraws),
    optionalCount(row.interactionDraws),
    optionalCount(row.skippedDraws),
    optionalCount(row.pendingPipelines),
    optionalCount(row.lightsVisible),
    optionalCount(row.lightsDrawn),
    optionalCount(row.lightsOverflowed),
    optionalCount(row.constantBytes),
    optionalCount(row.volatileWrites),
    optionalCount(row.volatileWriteBudget),
    optionalCount(row.pushConstantBytes),
    settled);
}

// U2.3's column, the allocation headroom's refusals, and V2.2's, the triangles the static
// interaction cull took out; empty in a start or stop row
std::string refusalFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{}", optionalCount(row.allocRefused), optionalCount(row.culledTriangles));
}

// LP4.1's columns, the view's portal flow, and LP4.2a's and LP4.3's, the ambient pass's;
// empty in a start or stop row
std::string flowFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{},{},{},{},{},{}",
    optionalCount(row.viewArea),
    optionalCount(row.areasFlowed),
    optionalCount(row.lightsInView),
    optionalCount(row.portalChains),
    optionalField(row.flowMs, "{:.3f}"),
    optionalCount(row.ambientDraws),
    optionalField(row.gpuAmbientMs, "{:.3f}"),
    optionalCount(row.ambientWrites),
    optionalCount(row.gridSurfaces));
}

// LPF.1's columns, the live feed's: the hand-overs the frame's table includes, the
// landing frame's latency and the override store's bytes; empty in a start or stop row
std::string feedFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalCount(row.feedApplied),
    optionalField(row.feedMs, "{:.3f}"),
    optionalCount(row.overrideBytes));
}

// LP5.1's columns, the interaction phases': the math lights and the ambientLight lights;
// empty in a start or stop row
std::string phaseFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{}", optionalCount(row.mathLights), optionalCount(row.ambientLights));
}

// LP5.2a's columns, the stage pass's: its draws, its skipped stages and its GPU time;
// empty in a start or stop row
std::string stageFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalCount(row.stageDraws),
    optionalCount(row.stageSkipped),
    optionalField(row.gpuStagesMs, "{:.3f}"));
}

// LP5.3's columns, pass 9's: its fog draws, its blend draws and its GPU time; empty in a
// start or stop row
std::string fogFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalCount(row.fogDraws),
    optionalCount(row.blendDraws),
    optionalField(row.gpuFogMs, "{:.3f}"));
}

// LP7.0's columns (C52, C54, C55): the models', the dress decals' and the adopted
// triangles' counts; LP7.3 (C54, K42): and the inlined copies'; empty in a start or stop
// row
std::string lp7Fields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{},{},{},{},{},{},{},{},{}",
    optionalCount(row.modelEntities),
    optionalCount(row.modelInstances),
    optionalCount(row.modelDraws),
    optionalCount(row.modelsPending),
    optionalCount(row.modelsSkinned),
    optionalCount(row.dressSurfaces),
    optionalCount(row.dressUnmatched),
    optionalCount(row.adoptedIslands),
    optionalCount(row.adoptedTris),
    optionalCount(row.unmatchedIslands),
    optionalCount(row.curvedIslands),
    optionalCount(row.inlinedSurfaces));
}

// LP3.1's columns (D157): pass 6's counts and times, set 11's writes, LP3.2's per-list
// shadow-atlas interaction draws, LP3.0's caster requests, and LP3.4's perforated draws;
// empty in a start or stop row
std::string shadowFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
    optionalCount(row.shadowDraws),
    optionalCount(row.tilesDrawn),
    optionalCount(row.tilesCleared),
    optionalCount(row.tilesRelocated),
    optionalCount(row.tilesCached),
    optionalCount(row.tilesProvisional),
    optionalCount(row.lightsCasting),
    optionalCount(row.lightsPlaced),
    optionalCount(row.lightsUnplaced),
    optionalCount(row.perforatedSkipped),
    optionalCount(row.atlasBytesWritten),
    optionalField(row.gpuShadowMs, "{:.3f}"),
    optionalField(row.shadowCpuMs, "{:.3f}"),
    optionalCount(row.set11Writes),
    optionalCount(row.smDrawsLocal),
    optionalCount(row.smDrawsGlobal),
    optionalCount(row.smDrawsTranslucent),
    optionalCount(row.casterLights),
    optionalCount(row.casterPairs),
    optionalCount(row.casterPairsResident),
    optionalCount(row.casterPairsPending),
    optionalCount(row.casters),
    optionalField(row.casterMs, "{:.3f}"),
    optionalCount(row.lightTriangleEntries),
    optionalCount(row.perforatedDraws));
}

// LP6.0's columns (C65): the CPU record's split inside cpu_record_ms's span: lists_ms
// (rebuildLists whole; flow_ms and caster_ms are inside it), the record of passes 1, 5,
// 7, 8 and 9 (pass 6's is shadow_cpu_ms), the tonemap passes', the uploads, the submit,
// and record_other_ms, the span less lists_ms, the seven pass columns, upload_ms and
// submit_ms; empty in a start or stop row
std::string cpuSplitFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{},{},{},{},{},{},{}",
    optionalField(row.listsMs, "{:.3f}"),
    optionalField(row.prepassCpuMs, "{:.3f}"),
    optionalField(row.ambientCpuMs, "{:.3f}"),
    optionalField(row.interactionCpuMs, "{:.3f}"),
    optionalField(row.stageCpuMs, "{:.3f}"),
    optionalField(row.fogCpuMs, "{:.3f}"),
    optionalField(row.tonemapCpuMs, "{:.3f}"),
    optionalField(row.uploadMs, "{:.3f}"),
    optionalField(row.submitMs, "{:.3f}"),
    optionalField(row.recordOtherMs, "{:.3f}"));
}

// LP6.0's reserved GPU columns (C65): the G-buffer's, SSAO's and TAA's times, empty until
// their passes land (LP6.5a, LP6.6); empty in a start or stop row
std::string lp6GpuFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalField(row.gpuGbufferMs, "{:.3f}"),
    optionalField(row.gpuSsaoMs, "{:.3f}"),
    optionalField(row.gpuTaaMs, "{:.3f}"));
}

// LP6.1's columns (C64): the draw records held at the frame's end and their resolutions
// this frame; empty in a start or stop row
std::string recordFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{}", optionalCount(row.recordsHeld), optionalCount(row.recordResolves));
}

// LP6.2's columns (C65): lists_ms's parts, the frontend's surfaces and lights and what
// follows it; empty in a start or stop row
std::string listsSplitFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalField(row.surfacesMs, "{:.3f}"),
    optionalField(row.lightsMs, "{:.3f}"),
    optionalField(row.linksMs, "{:.3f}"));
}

// LP6.2's last two (C66, D169): the caster sets made and the tiles drawn again for an
// image; empty in a start or stop row
std::string keptSetFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{}", optionalCount(row.casterSetsBuilt), optionalCount(row.tilesImageRedrawn));
}

// LP6.4a's three (C68 (a)): the texture heaps, their ranges' bytes and the slots'
// dedicated textures; empty in a start row
std::string heapFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{}",
    optionalCount(row.textureHeaps),
    optionalCount(row.heapBytes),
    optionalCount(row.texturesDedicated));
}

// LP6.4b's four (C68 (b)-(d)): the evictions, the floored slots, the in-place feeds and
// the arena bytes moved; an event row's where it has them
std::string budgetFields(const FrameLogRow& row)
{
  return fmt::format(
    "{},{},{},{}",
    optionalCount(row.texturesEvicted),
    optionalCount(row.texturesFloored),
    optionalCount(row.feedsInPlace),
    optionalCount(row.arenaMovedBytes));
}

std::string_view eventName(const FrameLogEvent event)
{
  switch (event)
  {
  case FrameLogEvent::Start:
    return "start";
  case FrameLogEvent::Frame:
    return "frame";
  case FrameLogEvent::Stop:
    return "stop";
  case FrameLogEvent::Compaction:
    return "compaction";
  }
  return "";
}

} // namespace

std::string_view frameLogHeader()
{
  return "session,event,frame,cpu_record_ms,acquire_ms,present_ms,gpu_frame_ms,present_"
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
         "islands,inlined_surfaces,shadow_draws,tiles_drawn,tiles_cleared,tiles_"
         "relocated,tiles_cached,tiles_provisional,lights_casting,lights_placed,lights_"
         "unplaced,perforated_skipped,atlas_bytes_written,gpu_shadow_ms,shadow_cpu_ms,"
         "set11_writes,sm_draws_local,sm_draws_global,sm_draws_translucent,caster_"
         "lights,caster_pairs,caster_pairs_resident,caster_pairs_pending,casters,caster_"
         "ms,light_triangle_entries,perforated_draws,lists_ms,prepass_cpu_ms,ambient_cpu_"
         "ms,interaction_cpu_ms,stage_cpu_ms,fog_cpu_ms,tonemap_cpu_ms,upload_ms,submit_"
         "ms,record_other_ms,gpu_gbuffer_ms,gpu_ssao_ms,gpu_taa_ms,records_held,record_"
         "resolves,surfaces_ms,lights_ms,links_ms,caster_sets_built,tiles_image_"
         "redrawn,texture_heaps,heap_bytes,textures_dedicated,textures_evicted,"
         "textures_floored,feeds_in_place,arena_moved_bytes\n";
}

std::string formatFrameLogRow(const FrameLogRow& row)
{
  const auto vram = fmt::format(
    "{},{}",
    optionalField(row.vramUsedMb, "{:.1f}"),
    optionalField(row.vramBudgetMb, "{:.1f}"));
  if (row.event != FrameLogEvent::Frame)
  {
    // the frame's columns empty (gpu_prepass_ms ... gpu_fog_ms), then proc_bytes, then
    // LP7's twelve counts, LP3.1's twenty-one, LP3.2's three, LP3.4's one, LP6.0's ten
    // and three, LP6.1's two and LP6.2's five empty; then LP6.4a's three (the stop row's)
    // and LP6.4b's four (an event row's own)
    return fmt::format(
      "{},{},,,,,,,,,,,{},{},,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,{},,,,,,,,,,,,"
      ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,{},{}\n",
      row.session,
      eventName(row.event),
      vram,
      resourceFields(row),
      optionalCount(row.procBytes),
      heapFields(row),
      budgetFields(row));
  }
  return fmt::format(
    "{},{},{},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
    "{},{},{},{},{},{},{}\n",
    row.session,
    eventName(row.event),
    row.frameIndex,
    row.cpuRecordMs,
    row.acquireMs,
    row.presentMs,
    optionalField(row.gpuFrameMs, "{:.3f}"),
    row.presentMode,
    row.swapchainGeneration,
    row.framesSkipped,
    row.width,
    row.height,
    vram,
    resourceFields(row),
    passFields(row),
    refusalFields(row),
    flowFields(row),
    feedFields(row),
    phaseFields(row),
    stageFields(row),
    fogFields(row),
    optionalCount(row.procBytes),
    lp7Fields(row),
    shadowFields(row),
    cpuSplitFields(row),
    lp6GpuFields(row),
    recordFields(row),
    listsSplitFields(row),
    keptSetFields(row),
    heapFields(row),
    budgetFields(row));
}

std::optional<std::string> FrameLogWriter::open(const std::filesystem::path& path)
{
  close();
  if (path.empty())
  {
    return std::nullopt;
  }

  // appended to, so that a log holds every session written into it (review L3.16)
  auto existing = std::error_code{};
  const auto empty = !std::filesystem::exists(path, existing)
                     || std::filesystem::file_size(path, existing) == 0;

  if (!empty)
  {
    // rows of another format would sit under the old header (review V.8)
    auto existingFile = std::ifstream{path, std::ios::binary};
    auto firstLine = std::string{};
    std::getline(existingFile, firstLine);
    if (firstLine + "\n" != frameLogHeader())
    {
      return fmt::format(
        "The frame log {} has another format; delete it or choose another path",
        path.string());
    }
  }

  m_path = path;
  m_file.open(path, std::ios::out | std::ios::app | std::ios::binary);
  if (!m_file)
  {
    return fmt::format("Cannot open the frame log {}", path.string());
  }

  m_buffer = empty ? std::string{frameLogHeader()} : std::string{};
  return std::nullopt;
}

bool FrameLogWriter::isOpen() const
{
  return m_file.is_open();
}

void FrameLogWriter::append(const FrameLogRow& row)
{
  if (!m_file.is_open())
  {
    return;
  }

  m_buffer += formatFrameLogRow(row);
  if (m_buffer.size() >= WriteThreshold)
  {
    writeBuffer();
  }
}

std::optional<std::string> FrameLogWriter::close()
{
  if (m_file.is_open())
  {
    writeBuffer();
    m_file.flush();
    m_file.close();
    if (!m_file && !m_error)
    {
      m_error = fmt::format("Cannot write the frame log {}", m_path.string());
    }
  }
  m_buffer.clear();
  return std::exchange(m_error, std::nullopt);
}

void FrameLogWriter::writeBuffer()
{
  m_file.write(m_buffer.data(), std::streamsize(m_buffer.size()));
  m_buffer.clear();
  if (!m_file && !m_error)
  {
    m_error = fmt::format("Cannot write the frame log {}", m_path.string());
  }
}

} // namespace tb::lit
