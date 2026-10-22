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
#include <vector>

namespace tb::lit
{
class CpuPool;
class GpuDevice;
class LightTablePublication;
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
 */
struct GpuMemoryAccount
{
  std::uint64_t arenaBytes = 0;
  std::uint64_t textureBytes = 0;
  std::uint64_t renderTargetBytes = 0;
  std::uint64_t releasedBytes = 0;

  std::uint64_t total() const;
};

// render thread
GpuMemoryAccount accountGpuMemory(
  const FrameResources& frame, const GeometryArenas& arenas, const ImageSlots& images);

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
  // D77: a texture arrival triggers a frame at most every TextureArrivalInterval
  FrameClock::time_point m_lastArrivalTrigger;
  std::optional<FrameClock::time_point> m_arrivalTriggerAt;
  // D77: the last frame left uploads for the next one
  bool m_unconverged = false;
  // D77 (LP2.2a): the last frame's lit passes wait for a material table newer than this
  // version (a pending material or evaluation); a newer one triggers a frame
  bool m_litWaitsForTable = false;
  std::uint64_t m_litTableVersion = 0;
  // C11: the account was compared with the device's budget at the session's first frame
  bool m_budgetChecked = false;
  // the bytes the frame being recorded has uploaded
  std::uint64_t m_frameUploadBytes = 0;
  // §3.10: what a frame uploads at most (UploadBytesPerFrame; tests set less)
  std::atomic<std::uint64_t> m_uploadCap = UploadBytesPerFrame;
  // LP2.1 test hook: while set, no frame counts as finished, so freed ranges and retired
  // resources stay held
  std::atomic<bool> m_holdCompletion = false;
  // C28: m_frameResources' render-target generation, for any thread (tests)
  std::atomic<std::uint64_t> m_renderTargetGeneration = 0;
  std::array<nvrhi::EventQueryHandle, FramesInFlight> m_frameEvents;
  // the slot's event query was set by a submitted frame and not waited for yet
  std::array<bool, FramesInFlight> m_frameEventSet = {};
  FrameTimers m_timers;
  std::uint64_t m_frameIndex = 0;
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
  std::array<std::function<void()>, 4> m_debugHooks;
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
  // LP2.1 test hook: the chunks the arenas hold instead of the scene's
  std::shared_ptr<const std::map<ChunkKey, Chunk>> m_testChunks;
  // the render thread's copy of m_testChunks, and the chunks of a thread without a scene
  std::shared_ptr<const std::map<ChunkKey, Chunk>> m_arenaTestChunks;
  std::map<ChunkKey, Chunk> m_noChunks;
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
  // layouts the passes draw with; it outlives the thread too
  void start(
    CpuPool* pool,
    MaterialService* materials,
    std::shared_ptr<LightTablePublication> lights,
    PipelineCompiler* compiler);

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
  // any thread: the arenas hold these chunks instead of the scene's until null replaces
  // them; a frame is triggered
  void setTestChunks(std::shared_ptr<const std::map<ChunkKey, Chunk>> chunks);
  // any thread: what a frame uploads at most (§3.10's cap, UploadBytesPerFrame)
  void setUploadCap(std::uint64_t bytes);
  // any thread: while held, no frame counts as finished (freed arena ranges and retired
  // resources stay held)
  void holdCompletion(bool hold);

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
  // D77 (LP2.2a): an idle pass's look for the material table the lit passes wait for;
  // whether a frame is due
  void litTableTick();
  RenderFrameAccess frameAccess(nvrhi::ICommandList& commandList);
  // the chunks the arenas follow (the scene's or the tests'), synchronised when their set
  // changed; whether uploads are queued
  bool syncChunks();
  // C28: the slots look at the latest material table, and the sets of bumped slots go
  void refreshImages();
  void eraseChangedSets();
  // D77: an idle pass's look for texture arrivals in a new table; whether a frame is due
  void arrivalsTick();
  const std::map<ChunkKey, Chunk>& arenaChunks() const;
  // D78: the frame's uploads, before its first pass; false: failed
  bool recordUploads(nvrhi::ICommandList& commandList);
  // whether the given fault was injected; it is taken
  bool takeFault(DebugFault fault);
  bool waitForFrameEvent(std::size_t slot);
  // frames 0 to completedFrames - 1 have finished: what only they used is let go (D35)
  void framesCompleted(std::uint64_t completedFrames);
  void runDebugHook(DebugHook point);
  void requestRecreate(
    SwapchainGeneration& generation, std::string_view reason, bool suboptimal = false);

  void idleTick();
  // collects the chunk jobs and applies a finished batch; whether a batch is in flight
  bool geometryTick();
  bool waitForIdle(std::string_view when);
  void parkGpuWork();
  void finish() noexcept;
  // a start or stop row with the VRAM in use
  void appendSessionRow(FrameLogEvent event);

  deleteCopyAndMove(RenderThread);
};

} // namespace tb::lit
