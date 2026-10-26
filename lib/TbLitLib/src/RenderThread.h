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

#include "BindingSetCache.h"
#include "FrameResources.h"
#include "FrameTimers.h"
#include "GeometryArenas.h"
#include "ImageSlots.h"
#include "LitPasses.h"
#include "PipelineCompiler.h"
#include "SamplerSet.h"
#include "TextureUploads.h"
#include "TonemapPasses.h"
#include "base/Macros.h"
#include "base/Result.h"
#include "lit/FramePacer.h"
#include "lit/LitConfig.h"
#include "lit/LitSession.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace tb::lit
{
class CpuPool;
class GpuDevice;
class GridTablePublication;
class LightTablePublication;
class ProbeTablePublication;
class MaterialService;
class PipelineCompiler;
class SceneGeometry;
class SessionEvents;
struct Snapshot;
struct SwapchainGeneration;

/**
 * §3.10, C11 (LP2.1 step 9): what the session's GPU memory is spent on, as the render
 * thread counts it: the geometry arenas' buffers, the slots' textures, the frame's
 * targets and buffers, and what was released and waits for its frames to finish (D35).
 * U2.6: also, as their own column, the volatile constant buffers (every version) and the
 * rest of the host-visible memory the device holds: NVRHI's upload chunks
 * (UploadChunkBytes each, kept by each command list until teardown) and a readback's
 * staging; V2.6: total() is the device-local part alone, the budget is compared with the
 * driver's own figure (VramUsage::usedBytes). U2.9: arenas never shrink before teardown,
 * so a whole-map rebuild may leave up to twice the geometry plus one arena per kind in
 * arenaBytes (M-ARENA-DIFF-01; compaction in LP6).
 */
struct GpuMemoryAccount
{
  std::uint64_t arenaBytes = 0;
  std::uint64_t textureBytes = 0;
  std::uint64_t renderTargetBytes = 0;
  std::uint64_t releasedBytes = 0;
  std::uint64_t volatileBufferBytes = 0;
  std::uint64_t uploadPoolBytes = 0;

  // the device-local estimate: arenas, textures, targets and what waits for its frames
  std::uint64_t total() const;
  // V2.6: the host-visible column, the volatile buffers and the upload pool
  std::uint64_t hostVisible() const;
};

// render thread
GpuMemoryAccount accountGpuMemory(
  const FrameResources& frame,
  const GeometryArenas& arenas,
  const ImageSlots& images,
  const LitPasses& passes,
  const TonemapPasses& tonemap);

/**
 * D77, §3.10 (LP2.2b): what kept a frame from being settled, and which of it the frame
 * retries by triggering the next one itself. Everything else waits for its own trigger,
 * so that a frame waiting for another thread costs no frames: a pending pipeline for the
 * compile thread's onReady, a pending material or evaluation for a table publication
 * (litTableTick), a pending image for its arrival (arrivalsTick, at most 10 Hz), a
 * provisional chunk for its rebuild (geometryTick), a pending entityDef, a provisional
 * light, a batch, the bake or a snapshot for the scene's next change (geometryTick, the
 * snapshot's offer), a feed hand-over the frame's table does not hold for its
 * publication (LPF.1: feedTick, at once). A settled frame is drawn again for a table
 * publication that gives the scene work (LP5.G: tableTick, at once). The exposure is at
 * its target in
 * every frame the exposure pass runs (D70). A pass that cannot draw (a program disabled
 * by D9) counts as converged, its state reported (passesDisabled, programReport).
 */
// DX.4 (LP4.2b step 0): the scene's applied batches, busy(), waitsForBake(),
// waitsForEntityDefs(), waitsForMaterials(), probesInFlight() and the probe table's
// version; LPF.1 (C43, C44): the materialsRevision it scanned last; LP5.F (D111): the
// sizesRevision it scanned last; LP7.2 (C52): waitsForModels() and its instances
using SceneWaitState = std::tuple<
  std::uint64_t,
  bool,
  bool,
  bool,
  bool,
  bool,
  std::uint64_t,
  std::uint64_t,
  std::uint64_t,
  bool,
  std::uint64_t>;

/**
 * LP4.3 (C38, D90): the status line's bake fields of a scene: its adopted bake's state,
 * drop reason and tree file time once the map path's request is answered, and its pieces
 * in solid. Render thread.
 */
void fillBakeStatus(SessionStatus& status, const SceneGeometry& geometry);

struct FrameConvergence
{
  // a pipeline of the lit or the tonemap passes is pending
  bool pipelinesPending = false;
  // a material, an evaluation or a light's shader is pending
  bool materialsPending = false;
  // a draw was skipped for a texture without a written level or a chunk not placed yet
  bool drawsSkipped = false;
  // uploads are left for the next frame (arenas, textures), or a slot's image waits to
  // be written (C21's full mips not reached)
  bool uploadsLeft = false;
  // slots the material table has not answered yet
  bool imagesPending = false;
  // chunks built with the defaults of a material the table does not have yet, or (LP5.F,
  // A41) at a C21 Default size while the material's size was not measured (D77's
  // provisional chunks, SceneGeometry::waitsForMaterials); one of a Default size whose
  // slot is measured is final: the engine's 8 × 8 for an image it cannot load
  bool provisionalChunks = false;
  // the tonemap passes' colour LUT has no written level yet
  bool lutPending = false;
  // DX.4 (31-LP2-FIXES.md): entities drawn with default parms while their entityDef is
  // pending; a drawn light built while its entityDef or a material was pending
  // (LightRecord::provisional, pendingEntityDef, pendingMaterials)
  bool entityDefsPending = false;
  bool lightsProvisional = false;
  // LP7.2 (C52, D124): a model entity's record is Pending or its instances wait for a
  // material or a skin (SceneGeometry::waitsForModels); the table that answers is the
  // trigger (geometryTick)
  bool modelsPending = false;
  // DX.4: a geometry batch or the light job runs, the map's bake has not been answered,
  // or a snapshot waits in the inbox; each brings its own trigger (geometryTick, the
  // offer)
  bool geometryInFlight = false;
  // U2.11: a draw or a light was dropped for the volatile constant buffer's budget (D76);
  // it does not re-trigger (nothing the next frame does changes the budget)
  bool drawsOverflowed = false;
  // DX.2: a lit surface's light triangles are being made on the CPU pool (it is drawn
  // whole meanwhile); the job's end is the trigger (lightTrianglesTick)
  bool lightTrianglesPending = false;
  // LP4.2a (D88): an image the view's probe choice read is not loaded yet (the default
  // cube stands); its arrival is the trigger (arrivalsTick)
  bool probeImagesPending = false;
  // LP4.2b step 0 (D87): the probe build runs, its inputs wait, or its table is
  // provisional (SceneGeometry::probesInFlight): the view's probe choice may read the
  // defaults; the build's collection is the trigger (geometryTick), as the light job's
  bool probesInFlight = false;
  // LP4.3 (D88): the atlas of an area with grid points is not loaded yet (a surface that
  // would take that grid takes the probe path meanwhile); its arrival is the trigger
  // (arrivalsTick)
  bool gridImagesPending = false;
  // LPF.1 (C44, D100, P5.2): the table the frame refreshed with at its start holds fewer
  // feed hand-overs (image overrides, re-parses) than the service had counted when the
  // frame converged (MaterialTable::feedApplied < MaterialService::feedRequested); not a
  // re-trigger term: the publication that advances feedApplied is the trigger (feedTick)
  bool feedBehind = false;
  // D9: a pass is disabled (counts as converged)
  bool passesDisabled = false;

  bool settled() const;
  // the frame triggers the next one itself (D77's unconverged pass)
  bool retriggers() const;
};

/**
 * D77 (LP2.2b): the livelock guard's view of a frame: when a frame that re-triggers
 * itself uploaded nothing and leaves this unchanged, it made no progress; after
 * MaxStalledFrames such frames the thread stops re-triggering until another trigger.
 */
struct FrameProgress
{
  std::size_t texturesResident = 0;
  std::size_t imagesPending = 0;
  bool imagesLoading = false;
  std::size_t skippedDraws = 0;
  std::uint64_t chunksVersion = 0;
  std::uint64_t tableVersion = 0;
  std::uint64_t renderTargetGeneration = 0;
  CameraState camera;

  bool operator==(const FrameProgress&) const = default;
};

// D77: the frames a session re-triggers without progress before it waits for a trigger
inline constexpr auto MaxStalledFrames = std::size_t{3};

// LP7.3 (C54): the scene's dress totals when a frame was drawn
// (SceneGeometry::DressCounts; 0 for a test's chunks)
struct DressTotals
{
  std::uint64_t surfaces = 0;
  std::uint64_t unmatched = 0;
  std::uint64_t inlined = 0;
  std::uint64_t ties = 0;
};

/**
 * Test hooks (LP2.1): what a frame's hook may use, on the render thread, inside the
 * frame's open command list once its uploads are recorded and before its first pass.
 */
struct RenderFrameAccess
{
  nvrhi::IDevice& device;
  nvrhi::ICommandList& commandList;
  // the frame's index (m_frameIndex before it is submitted)
  std::uint64_t frameIndex;
  FrameResources& frame;
  GeometryArenas& arenas;
  // T0.9: the render thread's copy of the compile thread's pipelines
  RenderPipelines& pipelines;
  // C28: the image slots and the binding sets
  ImageSlots& images;
  BindingSetCache& bindingSets;
  TextureUploads& textures;
  // D68: the common samplers (made at the session's first frame) and the images'
  SamplerSet& samplers;
  // the bytes the frame has uploaded before the hook runs
  std::uint64_t bytesUploaded;
  // LP2.2a: the lit passes (their lists, stats and test hooks)
  LitPasses& passes;
  // LP2.2b: the tonemap passes and the blit (their stats)
  TonemapPasses& tonemap;
  // LP2.2b (D77): the frame's convergence in runAfterTonemap's hooks, the last frame's
  // in the others
  const FrameConvergence& convergence;
  // LP2.2b test hook: the colour the frame clears the HDR target to instead of its lit
  // passes (setHdrClear), if any
  std::optional<std::array<float, 4>> hdrClear;
  // LP5.F: the chunks the frame's lists are made from (the scene's or a test's)
  const std::map<ChunkKey, Chunk>& chunks;
  // LP7.2 (C52, D129): and the model instances (the scene's; none for a test's chunks)
  const std::map<ChunkKey, ModelInstance>& instances;
  // LP7.2 (C52): the scene's models when the frame was drawn (ModelTotals; the counts
  // 0 for a test's chunks)
  ModelTotals models;
  // LP7.3 (C54): and its dress decals
  DressTotals dress;
  // LP7.5 step 0: the pacer's stamp of the frame's start (FramePacer::frameStarted),
  // taken before the snapshot, the acquire and the uploads
  FrameClock::time_point frameStart;
};

/**
 * A session's render thread (§3.1, C20) and what it draws into. It alone opens, records
 * and executes command lists and collects garbage. It never waits on the GUI thread: it
 * acquires with a timeout, and it honours a stop or a park at every pass boundary, the
 * last one before the present.
 *
 * The GUI thread replaces the swapchain generation only while the thread is parked or
 * not running (D49), and runs the teardown steps once it has exited.
 */
class RenderThread
{
private:
  enum class FrameOutcome
  {
    Presented,
    // not drawn, or drawn but not shown: the trigger stays pending
    Skipped,
    // lost or failed: nothing more is drawn
    Stopped,
  };

  // ----- set before the thread starts -----
  GpuDevice& m_device;
  SessionEvents& m_events;
  SessionOptions m_options;
  bool m_headless = false;
  nvrhi::TextureHandle m_headlessTarget;
  // LP2.2b: a framebuffer of the headless target alone, the blit's target
  nvrhi::FramebufferHandle m_headlessFramebuffer;
  vk::Extent2D m_headlessExtent;

  // B0.10: the session's pool and material service, given at start; the render thread's
  // scene and chunks (B0.3), made at start and destroyed when the thread ends
  CpuPool* m_pool = nullptr;
  MaterialService* m_materials = nullptr;
  // LP2.1 (D71, B0.10): the session's compile thread, given at start; it outlives this
  // thread, which never starts or joins it, and its onReady is this thread's frame
  // trigger (D77)
  PipelineCompiler* m_compiler = nullptr;
  // T0.9: the compiler's Ready and Failed answers, kept for the draws
  RenderPipelines m_pipelines;
  std::unique_ptr<SceneGeometry> m_geometry;

  // ----- the swapchain generation, replaced by the GUI thread only while parked -----
  std::unique_ptr<SwapchainGeneration> m_swapchain;

  // ----- render thread while it runs, then the GUI thread -----
  // the frame's one command list, immediate; any second list that may be open alongside
  // it sets enableImmediateExecution = false (R5.22)
  nvrhi::CommandListHandle m_commandList;
  // LP2.1 (C14, C6): the frame's targets, framebuffers and tonemap buffers
  FrameResources m_frameResources;
  // LP2.1 (S8, D36, D78): the chunks' geometry in arena buffers, and the version of the
  // chunk set they were last synchronised with (scene batches and test chunks count)
  GeometryArenas m_arenas;
  std::uint64_t m_chunksVersion = 0;
  std::uint64_t m_syncedChunksVersion = 0;
  // LP2.1 (C28, D37): the image-slot table and the binding sets made against its slots;
  // the material table the slots last looked at for arrivals
  ImageSlots m_images;
  BindingSetCache m_bindingSets{BindingSetCacheSize};
  // LP2.1 (SC268, P2.14c): the textures made from the slots' images, low mips first
  TextureUploads m_textureUploads;
  // LP2.1 (D68): the engine's common samplers and the images' own
  SamplerSet m_samplers;
  // LP2.2a: the depth prepass and the interactions into hdrFBO
  LitPasses m_passes;
  // LP2.2b: histogram, exposure and tonemapping into the LDR target, and the blit
  TonemapPasses m_tonemap;
  std::uint64_t m_arrivalsCheckedVersion = 0;
  // LP4.2a (D87): the probe table whose _spec and _depth slots were asked for
  std::shared_ptr<const ProbeTable> m_probeImagesTable;
  // LP4.3 (C36, SC336): the grid table whose atlases were asked for last
  std::shared_ptr<const GridTable> m_gridImagesTable;
  // D77: a texture arrival triggers a frame at most every TextureArrivalInterval
  FrameClock::time_point m_lastArrivalTrigger;
  std::optional<FrameClock::time_point> m_arrivalTriggerAt;
  // D77: the frame being recorded leaves uploads for the next one
  bool m_uploadsLeft = false;
  // D77 (LP2.2b): the last frame's convergence; whether it re-triggers itself; the
  // livelock guard's count of frames that re-triggered without progress, and the last
  // frame's progress
  FrameConvergence m_convergence;
  // DX.4: what the scene waited for when the last frame looked (its applied batches,
  // busy, bake, entityDefs, provisional chunks; LP4.2b: the probes in flight and their
  // table's version), for geometryTick's trigger
  std::optional<SceneWaitState> m_sceneWaitSeen;
  bool m_retrigger = false;
  std::size_t m_stalledFrames = 0;
  std::optional<FrameProgress> m_lastProgress;
  // any thread (tests): the last frame drawn was settled
  std::atomic<bool> m_lastSettled = false;
  // LP2.3 (D9): the last frame shown, written by the render thread, read on any thread
  mutable std::mutex m_statusMutex;
  SessionStatus m_status;
  // D77 (LP2.2a): the last frame's lit passes wait for a material table newer than this
  // version (a pending material or evaluation); a newer one triggers a frame
  bool m_litWaitsForTable = false;
  std::uint64_t m_litTableVersion = 0;
  // LPF.1 (C44, D100, D101): the feed counters of the table the last frame refreshed with
  // at its start (refreshImages): its feedApplied (converge's feedBehind), and its
  // imageChanges and materialsRevision (feedTick: whether a feed publication brings the
  // render side anything to draw)
  std::uint64_t m_frameFeedApplied = 0;
  std::uint64_t m_frameImageChanges = 0;
  std::uint64_t m_frameMaterialsRevision = 0;
  // LP5.F (D111): its sizesRevision (converge's geometryInFlight)
  std::uint64_t m_frameSizesRevision = 0;
  // LP5.G: its registry (tableTick: a build since the last frame)
  std::uint64_t m_frameRegistry = 0;
  // and its override store's bytes (the frame log's override_bytes)
  std::uint64_t m_frameOverrideBytes = 0;
  // LP7.0 (C54): the bytes of the scene's bake's tree when the session stopped (the stop
  // row's proc_bytes); none without a scene
  std::optional<std::uint64_t> m_stopProcBytes;
  /**
   * LPF.1 (C44, P5.13): the feed items the frames' tables included whose change has not
   * landed yet, for the frame log's feed_ms: an image override's newest hand-over time
   * and the slots it changed (landed once each is Resident with every level written, or
   * Default), a re-parse's and the materialsRevision it published (landed once the lists
   * were built with it and the scene's cells of it are built). A newer item takes over
   * the slots of an older one, which then never lands; the feedApplied of the newest
   * record taken.
   */
  struct PendingFeed
  {
    FrameClock::time_point handOver;
    std::vector<ImageSlotId> slots;
    std::uint64_t materialsRevision = 0;
  };
  std::vector<PendingFeed> m_pendingFeeds;
  std::uint64_t m_feedRecordsTaken = 0;
  // the materialsRevision of the table the frame's lit passes built their lists with
  std::uint64_t m_frameListsRevision = 0;
  // C11 (U2.6): the account is compared with the device's budget at the session's first
  // frame and at every settled frame; whether the session said once that it passed it,
  // and the budget a test set instead of the device's (0: the device's)
  bool m_budgetChecked = false;
  bool m_budgetWarned = false;
  std::atomic<std::uint64_t> m_vramBudgetForTesting = 0;
  // C38 (LP4.2b): the interactions are not recorded (setSkipInteractions)
  std::atomic<bool> m_skipInteractionsForTesting = false;
  // LP5.0 (D118): r_skipAmbient's stand-in (setSkipShaderPasses), not read by a pass yet
  std::atomic<bool> m_skipShaderPassesForTesting = false;
  // the bytes the frame being recorded has uploaded
  std::uint64_t m_frameUploadBytes = 0;
  // §3.10: what a frame uploads at most (UploadBytesPerFrame; tests set less)
  std::atomic<std::uint64_t> m_uploadCap = UploadBytesPerFrame;
  // LP2.1 test hook: while set, no frame counts as finished, so freed ranges and retired
  // resources stay held
  std::atomic<bool> m_holdCompletion = false;
  // U2.3: the allocation limit a test set instead of the device's
  // maxMemoryAllocationCount (0: the device's), and whether the session said once that
  // the headroom refused a texture or an arena
  std::atomic<std::uint32_t> m_allocationLimitForTesting = 0;
  bool m_headroomWarned = false;
  // C28: m_frameResources' render-target generation, for any thread (tests)
  std::atomic<std::uint64_t> m_renderTargetGeneration = 0;
  std::array<nvrhi::EventQueryHandle, FramesInFlight> m_frameEvents;
  // the slot's event query was set by a submitted frame and not waited for yet
  std::array<bool, FramesInFlight> m_frameEventSet = {};
  FrameTimers m_timers;
  std::uint64_t m_frameIndex = 0;
  // the started frame's FramePacer::frameStarted stamp (RenderFrameAccess::frameStart)
  FrameClock::time_point m_frameStart;
  // D35: the frames known to have finished on the GPU (frames 0 to m_completedFrames - 1)
  std::uint64_t m_completedFrames = 0;
  std::uint64_t m_framesSkipped = 0;
  bool m_readyPosted = false;
  bool m_idleWorkPending = false;
  FrameClock::time_point m_lastFrameEnd;
  std::optional<std::string> m_frameLogError;
  std::atomic<std::uint64_t> m_framesPresented = 0;
  // the injected DebugFault plus one, 0 for none
  std::atomic<int> m_injectedFault = 0;

  // ----- shared with the GUI thread, under m_mutex -----
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::condition_variable m_stateChanged;
  bool m_stopRequested = false;
  bool m_parkRequested = false;
  bool m_parked = false;
  bool m_exited = false;
  bool m_exposed = false;
  bool m_cameraSet = false;
  CameraState m_camera;
  // S31: the one-slot inbox, which the thread empties at a frame's start once the last
  // snapshot's chunks are applied (B0.3), and the last snapshot it took (for the tests;
  // B0.9: the one it replaces is freed outside m_mutex)
  std::shared_ptr<const Snapshot> m_snapshotInbox;
  std::shared_ptr<const Snapshot> m_snapshot;
  std::uint64_t m_snapshotsTaken = 0;
  // B0.7: whether m_snapshotInbox holds a snapshot, written with it, read without the
  // lock
  std::atomic<bool> m_snapshotInboxFull = false;
  std::uint64_t m_appliedSequence = 0;
  std::size_t m_chunkCount = 0;
  // R2.2: a material table was published since the last idle pass looked
  bool m_tableChanged = false;
  // the batches of chunk or light jobs applied so far (tests)
  std::uint64_t m_batchesApplied = 0;
  FramePacer m_pacer;
  std::array<std::function<void()>, 5> m_debugHooks;
  // LP2.1 test hooks: the headless extent the next frame takes, and the functions the
  // next frame runs once (RenderFrameAccess)
  std::optional<vk::Extent2D> m_headlessResize;
  std::vector<std::function<void(RenderFrameAccess&)>> m_frameHooks;
  std::function<void(RenderFrameAccess&)> m_everyFrameHook;
  // LP2.2a test hook: the functions the next frame runs once after its lit passes
  std::vector<std::function<void(RenderFrameAccess&)>> m_afterPassHooks;
  // LP2.2b test hooks: the functions the next frame runs once after its tonemap passes
  // and blit; the colour every frame clears the HDR target to instead of its lit passes
  std::vector<std::function<void(RenderFrameAccess&)>> m_afterTonemapHooks;
  std::optional<std::array<float, 4>> m_hdrClearForTesting;
  // LP2.3 (Q21): the tonemap passes' values a test set, taken by the next frame (m_mutex)
  std::optional<EngineTonemapDefaults> m_tonemapForTesting;
  // the frame being recorded's copy of it
  std::optional<std::array<float, 4>> m_frameHdrClear;
  // LP2.2b (C29): the LDR readbacks asked for, and those the frame being recorded copies
  // into its staging texture
  std::vector<std::function<void(LdrImage)>> m_ldrReadbacks;
  std::vector<std::function<void(LdrImage)>> m_ldrReadbacksInFrame;
  // LPF.2 (P5.24): the readbacks' one staging texture, kept while the target's extent
  // and format stay (released in teardown step 2), and whether this frame copied into it
  nvrhi::StagingTextureHandle m_ldrStaging;
  bool m_ldrCopied = false;
  // the staging textures made so far; any thread (tests)
  std::atomic<std::uint64_t> m_ldrStagingCreations = 0;
  // LP2.1 test hook: the chunks the arenas hold instead of the scene's
  std::shared_ptr<const std::map<ChunkKey, Chunk>> m_testChunks;
  // the render thread's copy of m_testChunks, and the chunks of a thread without a scene
  std::shared_ptr<const std::map<ChunkKey, Chunk>> m_arenaTestChunks;
  std::map<ChunkKey, Chunk> m_noChunks;
  std::map<ChunkKey, ModelInstance> m_noInstances;
  // LP7.2 (P7.14): the scene's models as last synced and placed (under m_mutex), made
  // again when the instances were synced or a shared surface was written
  ModelTotals m_modelTotals;
  std::uint64_t m_modelTotalsUploads = 0;
  ModelTotals currentModelTotals() const;
  void updateModelTotals();
  // the number of the last generation whose replacement was requested; GUI thread reads
  std::atomic<std::uint64_t> m_recreateRequestedFor = 0;
  // the extent of the last generation replaced because it was SUBOPTIMAL
  std::optional<vk::Extent2D> m_suboptimalExtent;

  // ----- GUI thread -----
  std::thread m_thread;

public:
  RenderThread(GpuDevice& device, SessionEvents& events, SessionOptions options);
  // joins the thread if it still runs
  ~RenderThread();

  /**
   * The frame resources: the command list, the event queries, the timers, and the
   * offscreen target of a headless session (no surface), which has the given extent.
   */
  Result<void> createFrameResources(std::optional<vk::Extent2D> headlessExtent);
  bool headless() const;
  // B0.10: the session's CPU pool and material service (either may be null), which the
  // render thread's chunk jobs use; both outlive the thread. R2.9: lights, the session's
  // light table publication, which the thread's light registry publishes into. LP2.1:
  // compiler, the session's compile thread (may be null), whose pipelines and binding
  // layouts the passes draw with; it outlives the thread too. LP4.2a: probes, the
  // session's probe table publication (null: the registry's own). LP4.3: grids, the
  // session's grid table publication (null: the scene's own)
  void start(
    CpuPool* pool,
    MaterialService* materials,
    std::shared_ptr<LightTablePublication> lights,
    PipelineCompiler* compiler,
    std::shared_ptr<ProbeTablePublication> probes = nullptr,
    std::shared_ptr<GridTablePublication> grids = nullptr);

  // Any thread.

  void setCamera(const CameraState& camera);
  CameraState camera() const;
  void setExposed(bool exposed);
  void requestFrame();
  std::uint64_t framesPresented() const;
  // see LitSession::offerSnapshot
  bool offerSnapshot(std::shared_ptr<const Snapshot> snapshot);
  // see LitSession::snapshotSlotFree; an atomic read
  bool snapshotSlotFree() const;
  std::uint64_t snapshotsTaken() const;
  std::shared_ptr<const Snapshot> lastSnapshot() const;
  // the sequence of the last snapshot whose chunks are applied, and the chunk count
  std::uint64_t appliedSnapshotSequence() const;
  std::size_t chunkCount() const;
  // see LitSession::tableChanged
  void tableChanged();
  std::uint64_t batchesApplied() const;
  // LP7.2 (P7.14): see LitSession::modelTotalsForTesting
  ModelTotals modelTotals() const;

  // GUI thread: the surface handshake.

  bool park(std::chrono::milliseconds timeout);
  bool isParked() const;
  void unpark();
  // parked, or not running
  bool mayReplaceSwapchain() const;
  bool hasSwapchain() const;
  // see LitSession::swapchainReplacementRequested
  bool swapchainReplacementRequested() const;
  void setSwapchain(std::unique_ptr<SwapchainGeneration> swapchain);
  std::unique_ptr<SwapchainGeneration> takeSwapchain();
  // the window's DPR or screen changed: SUBOPTIMAL may mean something new again
  void surfaceChanged();

  // GUI thread: teardown step 1, and step 2's frame resources after the join.

  void requestStop();
  bool waitForExit(std::chrono::milliseconds timeout);
  void join();
  void releaseFrameResources();
  // the frame log's first failed write; after the join
  std::optional<std::string> frameLogError() const;

  // Test hooks.

  void injectFault(DebugFault fault);
  void setDebugHook(DebugHook point, std::function<void()> hook);
  // any thread: a headless session's next frame is drawn at extent (its target and the
  // frame resources made again); a frame is triggered
  void resizeHeadless(vk::Extent2D extent);
  // any thread: the next frame drawn runs hook once on the render thread (see
  // RenderFrameAccess); a frame is triggered
  void runInFrame(std::function<void(RenderFrameAccess&)> hook);
  // any thread: the frame resources' render-target generation (C28)
  std::uint64_t renderTargetGeneration() const;
  // LP2.2b, any thread: the last frame drawn was settled (D77)
  bool lastFrameSettled() const;
  // LP2.3 (D9), any thread: the last frame shown, for the panel's status line
  SessionStatus status() const;
  // any thread: every frame runs hook on the render thread (as runInFrame) until an
  // empty function replaces it
  void setEveryFrameHook(std::function<void(RenderFrameAccess&)> hook);
  // LP2.2a, any thread: the next frame drawn runs hook once on the render thread after
  // its lit passes, inside its command list (a readback of the HDR target); a frame is
  // triggered
  void runAfterPasses(std::function<void(RenderFrameAccess&)> hook);
  // LP2.2b, any thread: the next frame drawn runs hook once on the render thread after
  // its tonemap passes and its blit, inside its command list (a readback of the LDR or
  // the frame's target); a frame is triggered
  void runAfterTonemap(std::function<void(RenderFrameAccess&)> hook);
  // LP2.2b, any thread: while set, every frame clears the HDR target to colour instead
  // of recording its lit passes, and tonemaps it; a frame is triggered
  void setHdrClear(std::optional<std::array<float, 4>> colour);
  // LP2.3 (Q21), any thread: the tonemap passes' values from the next frame on; a frame
  // is triggered
  void setTonemapDefaults(const EngineTonemapDefaults& tonemap);
  // LP2.2b (C29), any thread: done gets a copy of the next settled frame's LDR target,
  // on the render thread once that frame has finished on the GPU; a frame is triggered.
  // A request the thread never serves (it stops first) is dropped
  void readbackLdr(std::function<void(LdrImage)> done);
  // LPF.2 (P5.24), any thread: the readbacks' staging textures made so far (one per
  // extent, not one per readback)
  std::uint64_t ldrStagingCreations() const;
  // any thread: the arenas hold these chunks instead of the scene's until null replaces
  // them; a frame is triggered
  void setTestChunks(std::shared_ptr<const std::map<ChunkKey, Chunk>> chunks);
  // any thread: what a frame uploads at most (§3.10's cap, UploadBytesPerFrame)
  void setUploadCap(std::uint64_t bytes);
  // any thread: while held, no frame counts as finished (freed arena ranges and retired
  // resources stay held)
  void holdCompletion(bool hold);
  // U2.3, any thread: the allocation limit the headroom counts from instead of the
  // device's maxMemoryAllocationCount (0: the device's)
  void setAllocationLimit(std::uint32_t limit);
  // U2.6, any thread: the budget the account is compared with instead of the device's
  // (VK_EXT_memory_budget); 0 for the device's
  void setVramBudget(std::uint64_t bytes);
  // C38 (LP4.2b), any thread: while set, the frames record no interactions
  // (LitPasses::setSkipInteractionsForTesting); a frame is triggered
  void setSkipInteractions(bool skip);
  // LP5.0 (D118), any thread: r_skipAmbient's stand-in, stored and handed to the lit
  // passes each frame (LitPasses::setSkipShaderPassesForTesting), which LP5.2a's stage
  // pass will read; no frame is triggered while nothing reads it
  void setSkipShaderPasses(bool skip);
  bool skipShaderPasses() const;
  // DX.5, GUI thread, phase B, once the frame resources exist and before start: the
  // pipelines the first lit frames ask for (litStartPipelineKeys, the tonemap pass's and
  // the blit's against this session's targets)
  std::vector<PipelineKey> startPipelineKeys() const;

private:
  void main() noexcept;
  void runLoop();
  bool stopOrParkRequested() const;
  // under m_mutex
  bool canDraw() const;

  FrameOutcome drawFrame(const CameraState& camera);
  // a headless resize asked for, applied before the frame's extent is known
  void applyHeadlessResize();
  // the frame's resources at its extent; false: failed
  bool prepareFrameResources(vk::Extent2D extent);
  // C11, §3.10: at the first frame, the account against VK_EXT_memory_budget's budget
  void checkVramBudget();
  void runFrameHooks(nvrhi::ICommandList& commandList);
  void runAfterPassHooks(nvrhi::ICommandList& commandList);
  void runAfterTonemapHooks(nvrhi::ICommandList& commandList);
  // LP2.2a: the frame's lit passes, after its uploads and hooks; whether the next frame
  // is due at once (D77: a skipped draw)
  bool recordLitPasses(nvrhi::ICommandList& commandList, const CameraState& camera);
  // LP2.2b test hook: the HDR target cleared to colour, the LDR and depth targets as the
  // lit passes clear them, instead of the lit passes
  void recordHdrClear(
    nvrhi::ICommandList& commandList, const std::array<float, 4>& colour);
  // LP2.2b: the tonemap passes and the blit into the frame's target (hdrDrawn: the HDR
  // target holds a picture)
  void recordTonemap(
    nvrhi::ICommandList& commandList,
    nvrhi::ITexture* target,
    nvrhi::IFramebuffer* targetFramebuffer,
    bool hdrDrawn);
  // LP2.2b: the framebuffer of a swapchain image alone, made at its first frame; null
  // when NVRHI cannot make it
  nvrhi::IFramebuffer* imageFramebuffer(
    SwapchainGeneration& generation, std::uint32_t imageIndex);
  // LP2.2b: the headless target and its framebuffer at m_headlessExtent; false: failed
  bool makeHeadlessTarget();
  // D77 (LP2.2b): the frame's convergence after its passes, and whether it re-triggers
  // itself (with the livelock guard)
  void converge(const LitFrameStats& lit, std::uint64_t tableVersion);
  // LP2.2b (C29): a settled frame copies the LDR target for the readbacks asked for
  void recordLdrReadback(nvrhi::ICommandList& commandList);
  // once the frame in slot is submitted: waits for it on the GPU and hands the copy over
  void finishLdrReadback(std::size_t slot);
  // D77 (LP2.2a): an idle pass's look for the material table the lit passes wait for;
  // whether a frame is due
  void litTableTick();
  RenderFrameAccess frameAccess(nvrhi::ICommandList& commandList);
  // the chunks the arenas follow (the scene's or the tests'), synchronised when their set
  // changed; whether uploads are queued
  bool syncChunks();
  // C28: the slots look at the latest material table, and the sets of bumped slots go
  void refreshImages();
  // LPF.1 (C44): the feed items of the frame's table not taken yet (m_pendingFeeds)
  void takeFeedRecords(const MaterialTable& table);
  // LPF.1 (C44): at a presented frame, the feed items whose change landed in it, and the
  // largest of their times since their hand-overs in ms (the frame log's feed_ms)
  std::optional<double> landFeeds(FrameClock::time_point now);
  // LP4.2a (D87, D85): a probe table not seen yet: every probe's _spec and _depth slots
  // are asked for (never _amb)
  void requestProbeImages();
  // LP4.3 (C36, SC336): every area's atlas of a grid table not asked for yet
  void requestGridImages();
  void eraseChangedSets();
  // D77: an idle pass's look for texture arrivals in a new table; whether a frame is due;
  // LP5.G addendum: and for a changed file of a slot the render side holds
  void arrivalsTick();
  // DX.2: an idle pass's look for the light triangles' job; its answers trigger a frame
  void lightTrianglesTick();
  // LPF.1 (C44, D101, P5.20): an idle pass's look for a feed publication the last frame
  // did not refresh with; a frame at once (no arrival clock), the pacer's interval kept
  void feedTick();
  // LP5.G: an idle pass's look for a table with another registry or revision than the
  // last frame's, whose scan gave the scene work; a frame at once, as feedTick's
  void tableTick();
  // D77, U2.5: a trigger at most every TextureArrivalInterval: now, or at the interval's
  // end; under m_mutex
  void arrivalTrigger(FrameClock::time_point now);
  const std::map<ChunkKey, Chunk>& arenaChunks() const;
  // LP7.2 (D129): the scene's model instances (none while a test's chunks stand)
  const std::map<ChunkKey, ModelInstance>& arenaInstances() const;
  // D78: the frame's uploads, before its first pass; false: failed
  bool recordUploads(nvrhi::ICommandList& commandList);
  // U2.3: one more device allocation keeps AllocationHeadroom free below the limit
  bool allocationRoom() const;
  // whether the given fault was injected; it is taken
  bool takeFault(DebugFault fault);
  bool waitForFrameEvent(std::size_t slot);
  // frames 0 to completedFrames - 1 have finished: what only they used is let go (D35)
  void framesCompleted(std::uint64_t completedFrames);
  void runDebugHook(DebugHook point);
  void requestRecreate(
    SwapchainGeneration& generation, std::string_view reason, bool suboptimal = false);

  void idleTick();
  // collects the chunk jobs and applies a finished batch; whether a batch, the light
  // job or a probe build is in flight
  bool geometryTick();
  // DX.4: the scene's state that a frame's geometry terms read
  SceneWaitState sceneWaitState() const;
  bool waitForIdle(std::string_view when);
  void parkGpuWork();
  void finish() noexcept;
  // a start or stop row with the VRAM in use
  void appendSessionRow(FrameLogEvent event);

  deleteCopyAndMove(RenderThread);
};

} // namespace tb::lit
