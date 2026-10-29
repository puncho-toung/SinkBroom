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

#include "RenderThread.h"

#include "GpuDevice.h"
#include "SessionEvents.h"
#include "StackThread.h"
#include "Swapchain.h"
#include "lit/EngineTables.h"
#include "lit/FrameLog.h"
#include "lit/MapBake.h"
#include "lit/SceneGeometry.h"
#include "lit/Snapshot.h"
#include "lit/SwapchainRules.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace tb::lit
{
namespace
{
// how often the render thread looks at the chunk jobs of a batch in flight
constexpr auto GeometryPollInterval = std::chrono::milliseconds{2};
} // namespace

namespace
{

constexpr auto AcquireTimeoutNs = std::uint64_t(
  std::chrono::duration_cast<std::chrono::nanoseconds>(AcquireTimeout).count());

// how often the render thread looks at a frame's event query while it waits for the GPU;
// a stop, a park or a lost device ends the wait at once
constexpr auto EventPollInterval = std::chrono::milliseconds{1};
// U2.7: a frame's event that has not completed after this long is said once (a GPU that
// hangs without the driver reporting the device lost); the wait goes on until a stop
constexpr auto GpuAnswerBound = std::chrono::seconds{2};

double millisecondsSince(const FrameClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(FrameClock::now() - start).count();
}

/**
 * An open command list that is closed and executed when its scope ends without submit(),
 * as when recording throws. NVRHI breaks the reference cycle between a list and its
 * tracked command buffer only when the list is executed, so a list left open would leak
 * with its command pool (review L2.5). NVRHI's submit swallows a lost device itself.
 */
class OpenCommandList
{
private:
  nvrhi::IDevice& m_device;
  nvrhi::ICommandList& m_list;
  bool m_closed = false;
  bool m_executed = false;

public:
  OpenCommandList(nvrhi::IDevice& device, nvrhi::ICommandList& list)
    : m_device{device}
    , m_list{list}
  {
    m_list.open();
  }

  ~OpenCommandList()
  {
    if (!m_executed)
    {
      try
      {
        submit();
      }
      catch (...)
      {
        // the frame has failed already; this only hands the list back to NVRHI
      }
    }
  }

  OpenCommandList(const OpenCommandList&) = delete;
  OpenCommandList& operator=(const OpenCommandList&) = delete;

  nvrhi::ICommandList& list() { return m_list; }

  void close()
  {
    if (!m_closed)
    {
      m_closed = true;
      m_list.close();
    }
  }

  void submit()
  {
    close();
    m_executed = true;
    m_device.executeCommandList(&m_list);
  }
};

} // namespace

void fillBakeStatus(SessionStatus& status, const SceneGeometry& geometry)
{
  using Bake = SessionStatus::Bake;
  status.solidPieces = geometry.solidPieces();
  if (!geometry.bakeAnswered())
  {
    return;
  }
  const auto bake = geometry.adoptedBake();
  if (!bake || bake->geometry == MapBake::Geometry::None)
  {
    status.bake = Bake::None;
    return;
  }
  status.bakeTime = mapBakeGeometryTime(*bake);
  if (!bake->dropReason.empty())
  {
    status.bake = Bake::Dropped;
    status.bakeDropReason = bake->dropReason;
    return;
  }
  status.bake = bake->geometry == MapBake::Geometry::Proc ? Bake::Proc : Bake::Bproc;
}

bool FrameConvergence::settled() const
{
  return !pipelinesPending && !materialsPending && !drawsSkipped && !uploadsLeft
         && !imagesPending && !provisionalChunks && !lutPending && !entityDefsPending
         && !modelsPending && !lightsProvisional && !geometryInFlight && !drawsOverflowed
         && !lightTrianglesPending && !probeImagesPending && !gridImagesPending
         && !probesInFlight && !feedBehind && !shadowsPending && !budgetLeft;
}

bool FrameConvergence::retriggers() const
{
  // what the next frame converges by itself: the uploads it records and the draws they
  // let it make; LP6.4b: the evictions and floors its budget pass left
  return drawsSkipped || uploadsLeft || budgetLeft;
}

std::uint64_t GpuMemoryAccount::total() const
{
  return arenaBytes + textureBytes + renderTargetBytes + releasedBytes;
}

std::uint64_t GpuMemoryAccount::hostVisible() const
{
  return volatileBufferBytes + uploadPoolBytes;
}

std::uint64_t GpuMemoryAccount::live() const
{
  return arenaUsedBytes + heapUsedBytes + dedicatedTextureBytes + renderTargetBytes
         + releasedBytes;
}

GpuMemoryAccount accountGpuMemory(
  const FrameResources& frame,
  const GeometryArenas& arenas,
  const ImageSlots& images,
  const LitPasses& passes,
  const TonemapPasses& tonemap,
  const TextureHeaps& heaps)
{
  auto account = GpuMemoryAccount{};
  account.arenaBytes = arenas.capacityBytes();
  // LP6.4a (C68 (a)): the heaps' capacity and the dedicated slot textures; a texture in
  // a heap, live or retired, is inside its heap's bytes
  account.textureBytes = heaps.capacityBytes() + images.dedicatedTextureBytes();
  // LP3.1 (A49): the shadow atlas on the render-target line once it is made (256 MB);
  // LP3.3 (D146): and its relocation scratch (16 MB)
  account.renderTargetBytes = frame.renderTargetBytes() + frame.bufferBytes()
                              + passes.shadowAtlasBytes() + passes.shadowScratchBytes();
  // LP6.4a: the slots' retired textures count only the dedicated ones' bytes
  account.releasedBytes = frame.retiredBytes() + images.retiredBytes();
  // LP6.4b (C68 (b), the director's reading (1)): the live figure's lines
  account.arenaUsedBytes = arenas.usedBytes();
  account.heapUsedBytes = heaps.usedBytes();
  account.dedicatedTextureBytes = images.dedicatedTextureBytes();
  // U2.6: the volatile buffers are host-visible (NVRHI allocates them for CPU writes);
  // the rest of the host-visible bytes is the upload pool and a readback's staging
  account.volatileBufferBytes =
    passes.volatileBufferBytes() + tonemap.volatileBufferBytes();
  const auto hostVisible = liveHostVisibleBytes();
  account.uploadPoolBytes = hostVisible > account.volatileBufferBytes
                              ? hostVisible - account.volatileBufferBytes
                              : 0;
  return account;
}

RenderThread::RenderThread(
  GpuDevice& device, SessionEvents& events, SessionOptions options)
  : m_device{device}
  , m_events{events}
  , m_options{std::move(options)}
  , m_vramBudget{m_options.vramBudgetBytes}
{
}

RenderThread::~RenderThread()
{
  requestStop();
  join();
}

Result<void> RenderThread::createFrameResources(
  const std::optional<vk::Extent2D> headlessExtent)
{
  auto& device = m_device.device();
  if (headlessExtent)
  {
    m_headless = true;
    m_headlessExtent = *headlessExtent;
    if (!makeHeadlessTarget())
    {
      return Error{"Cannot create the headless target"};
    }
  }

  // step 8, LP0's part: the frame resources; D78: the list's upload chunks are 32 MB
  m_commandList = device.createCommandList(
    nvrhi::CommandListParameters{}.setUploadChunkSize(UploadChunkBytes));
  // LP6.4b: ArenaBytes, or a test's smaller arenas
  m_arenas.setDevice(device, geometryArenaBytes());
  m_arenas.setAllocationRoom([this] { return allocationRoom(); });
  // LP6.4a (C68 (a), D171, Q47): the slots' textures in heaps of HeapBytes, a new heap
  // under U2.3's room (the director's reading (1))
  m_textureHeaps.setDevice(device, textureHeapBytes());
  m_textureHeaps.setAllocationRoom([this] { return allocationRoom(); });
  m_images.setHeaps(&m_textureHeaps);
  m_textureUploads.setD24s8Supported(m_device.gpu().d24s8Supported);
  // LP6.4b (C68 (c), D173): a feed of an equal desc whose levels fit a frame's uploads is
  // written in place
  m_images.setInPlaceRule(
    [this](const CachedImage& image, const nvrhi::ITexture& texture) {
      return m_textureUploads.fitsInPlace(image, texture.getDesc(), m_uploadCap.load());
    });
  for (auto& event : m_frameEvents)
  {
    event = device.createEventQuery();
  }
  m_timers.createQueries(device, m_device.gpu().timerQueries);
  if (!m_commandList || m_events.lost())
  {
    return Error{"Cannot create the frame resources"};
  }

  // LP2.1 (C14, C6): the registry's targets at the surface's extent (headless: the
  // headless extent); a later frame of another extent makes them again
  const auto extent = m_headless    ? m_headlessExtent
                      : m_swapchain ? m_swapchain->extent
                                    : vk::Extent2D{1, 1};
  // LP6.5a (C69, C70): a session with SSAO has the G-buffer's and SSAO's targets too
  return m_frameResources.create(
           device,
           m_device.gpu().d24s8Supported,
           vk::Extent2D{std::max(extent.width, 1u), std::max(extent.height, 1u)},
           m_options.ssaoForTesting)
         | kdl::transform([&]() {
             m_renderTargetGeneration = m_frameResources.renderTargetGeneration();
           });
}

bool RenderThread::headless() const
{
  return m_headless;
}

bool RenderThread::makeHeadlessTarget()
{
  // SC278: an RGBA8 offscreen target stands in for the swapchain image, with a
  // framebuffer of it alone for the blit (LP2.2b)
  auto& device = m_device.device();
  m_headlessTarget =
    device.createTexture(nvrhi::TextureDesc{}
                           .setWidth(m_headlessExtent.width)
                           .setHeight(m_headlessExtent.height)
                           .setFormat(nvrhi::Format::RGBA8_UNORM)
                           .setDebugName("Lit preview headless target")
                           .setIsRenderTarget(true)
                           .setInitialState(nvrhi::ResourceStates::RenderTarget)
                           .setKeepInitialState(true));
  m_headlessFramebuffer =
    m_headlessTarget ? device.createFramebuffer(
                         nvrhi::FramebufferDesc{}.addColorAttachment(m_headlessTarget))
                     : nullptr;
  return m_headlessTarget && m_headlessFramebuffer;
}

nvrhi::IFramebuffer* RenderThread::imageFramebuffer(
  SwapchainGeneration& generation, const std::uint32_t imageIndex)
{
  // made on the render thread at the image's first frame; released with the wrappers
  // (releaseSwapchainWrappers), before them
  if (generation.imageFramebuffers.size() < generation.imageTextures.size())
  {
    generation.imageFramebuffers.resize(generation.imageTextures.size());
  }
  auto& framebuffer = generation.imageFramebuffers[imageIndex];
  if (!framebuffer)
  {
    framebuffer = m_device.device().createFramebuffer(
      nvrhi::FramebufferDesc{}.addColorAttachment(generation.imageTextures[imageIndex]));
  }
  return framebuffer.Get();
}

void RenderThread::start(
  CpuPool* pool,
  MaterialService* materials_,
  std::shared_ptr<LightTablePublication> lights,
  PipelineCompiler* compiler,
  std::shared_ptr<ProbeTablePublication> probes,
  std::shared_ptr<GridTablePublication> grids)
{
  m_pool = pool;
  m_materials = materials_;
  m_compiler = compiler;
  m_pipelines.setCompiler(compiler);
  // C28: the slots ask the service (any thread may)
  if (m_materials)
  {
    m_images.setRequest([materials = m_materials](const MaterialImageKey& key) {
      return materials->image(key);
    });
  }
  m_geometry = std::make_unique<SceneGeometry>(
    m_pool, m_materials, std::move(lights), std::move(probes), std::move(grids));
  m_thread = std::thread{[this] { main(); }};
  libraryThreadStarted();
}

void RenderThread::setCamera(const CameraState& camera)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (m_cameraSet && m_camera == camera)
    {
      return;
    }
    m_camera = camera;
    m_cameraSet = true;
    m_pacer.trigger();
    // LP3.0 (D151): the still timer counts from the last move
    m_pacer.cameraMoved(FrameClock::now());
  }
  m_wake.notify_all();
}

CameraState RenderThread::camera() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_camera;
}

void RenderThread::setExposed(const bool exposed)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (exposed && !m_exposed)
    {
      // the window shows again: draw it once
      m_pacer.trigger();
    }
    m_exposed = exposed;
  }
  m_wake.notify_all();
}

void RenderThread::requestFrame()
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

std::uint64_t RenderThread::framesPresented() const
{
  return m_framesPresented;
}

bool RenderThread::offerSnapshot(std::shared_ptr<const Snapshot> snapshot)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    // S31: one in flight; a thread that stops takes no more
    if (m_snapshotInbox || m_stopRequested || m_exited)
    {
      return false;
    }
    m_snapshotInbox = std::move(snapshot);
    m_snapshotInboxFull = true;
    // the next frame's start takes it
    m_pacer.trigger();
  }
  m_wake.notify_all();
  return true;
}

bool RenderThread::snapshotSlotFree() const
{
  return !m_snapshotInboxFull;
}

std::uint64_t RenderThread::snapshotsTaken() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_snapshotsTaken;
}

std::shared_ptr<const Snapshot> RenderThread::lastSnapshot() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_snapshot;
}

std::uint64_t RenderThread::appliedSnapshotSequence() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_appliedSequence;
}

std::size_t RenderThread::chunkCount() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_chunkCount;
}

void RenderThread::tableChanged()
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_tableChanged = true;
  }
  m_wake.notify_all();
}

std::uint64_t RenderThread::batchesApplied() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_batchesApplied;
}

FirstBatchCounts RenderThread::firstBatch() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_firstBatch;
}

ModelTotals RenderThread::modelTotals() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_modelTotals;
}

ModelTotals RenderThread::currentModelTotals() const
{
  auto totals = ModelTotals{};
  if (m_geometry && !m_arenaTestChunks)
  {
    const auto counts = m_geometry->modelCounts();
    totals.entities = counts.entities;
    totals.instances = counts.instances;
    totals.pending = counts.pending;
    totals.skinned = counts.skinned;
  }
  totals.records = m_arenas.sharedRecords();
  totals.placements = m_arenas.placedShared();
  totals.placementBytes = m_arenas.placedSharedBytes();
  return totals;
}

void RenderThread::updateModelTotals()
{
  const auto totals = currentModelTotals();
  m_modelTotalsUploads = m_arenas.uploadedShared();
  const auto lock = std::scoped_lock{m_mutex};
  m_modelTotals = totals;
}

bool RenderThread::park(const std::chrono::milliseconds timeout)
{
  auto lock = std::unique_lock{m_mutex};
  if (!m_thread.joinable() || m_exited)
  {
    return true;
  }
  m_parkRequested = true;
  m_wake.notify_all();
  return m_stateChanged.wait_for(lock, timeout, [&] { return m_parked || m_exited; });
}

bool RenderThread::isParked() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return !m_thread.joinable() || m_exited || m_parked;
}

void RenderThread::unpark()
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (!m_parkRequested)
    {
      return;
    }
    m_parkRequested = false;
    // the new generation's first frame
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

bool RenderThread::mayReplaceSwapchain() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return !m_thread.joinable() || m_exited || m_parked;
}

bool RenderThread::hasSwapchain() const
{
  return m_swapchain != nullptr;
}

bool RenderThread::swapchainReplacementRequested() const
{
  // the GUI thread is the only one that replaces m_swapchain, and a number never changes
  return !m_swapchain || m_recreateRequestedFor == m_swapchain->number;
}

void RenderThread::setSwapchain(std::unique_ptr<SwapchainGeneration> swapchain)
{
  m_swapchain = std::move(swapchain);
}

std::unique_ptr<SwapchainGeneration> RenderThread::takeSwapchain()
{
  return std::move(m_swapchain);
}

void RenderThread::surfaceChanged()
{
  const auto lock = std::scoped_lock{m_mutex};
  m_suboptimalExtent = std::nullopt;
}

void RenderThread::requestStop()
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_stopRequested = true;
  }
  m_wake.notify_all();
}

bool RenderThread::waitForExit(const std::chrono::milliseconds timeout)
{
  auto lock = std::unique_lock{m_mutex};
  return !m_thread.joinable()
         || m_stateChanged.wait_for(lock, timeout, [&] { return m_exited; });
}

void RenderThread::join()
{
  // never detached: the thread uses this object until it exits
  if (m_thread.joinable())
  {
    m_thread.join();
    libraryThreadJoined();
  }
}

void RenderThread::releaseFrameResources()
{
  // §3.1 step 2: every handle the registry and the arenas hold
  m_passes.releaseAll();
  m_tonemap.releaseAll();
  m_ldrStaging = nullptr;
  m_ldrCopied = false;
  m_ldrReadbacksInFrame.clear();
  m_frameResources.releaseAll();
  m_arenas.releaseAll();
  m_pipelines.clear();
  m_bindingSets.clear();
  m_images.releaseAll();
  // LP6.4a (C68 (a)): a heap outlives every texture bound into it, so after the slots
  m_textureHeaps.releaseAll();
  m_samplers.releaseAll();
  m_arenaTestChunks.reset();
  m_commandList = nullptr;
  for (auto& event : m_frameEvents)
  {
    event = nullptr;
  }
  m_timers.releaseQueries();
  m_headlessFramebuffer = nullptr;
  m_headlessTarget = nullptr;
}

std::optional<std::string> RenderThread::frameLogError() const
{
  return m_frameLogError;
}

void RenderThread::injectFault(const DebugFault fault)
{
  m_injectedFault = int(fault) + 1;
}

bool RenderThread::takeFault(const DebugFault fault)
{
  auto expected = int(fault) + 1;
  return m_injectedFault.compare_exchange_strong(expected, 0);
}

void RenderThread::setDebugHook(const DebugHook point, std::function<void()> hook)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_debugHooks[std::size_t(point)] = std::move(hook);
}

void RenderThread::resizeHeadless(const vk::Extent2D extent)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_headlessResize =
      vk::Extent2D{std::max(extent.width, 1u), std::max(extent.height, 1u)};
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::runInFrame(std::function<void(RenderFrameAccess&)> hook)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_frameHooks.push_back(std::move(hook));
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

std::uint64_t RenderThread::renderTargetGeneration() const
{
  return m_renderTargetGeneration;
}

bool RenderThread::lastFrameSettled() const
{
  return m_lastSettled;
}

std::uint64_t RenderThread::stillTimerFirings() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_pacer.stillTimerFirings();
}

SessionStatus RenderThread::status() const
{
  const auto lock = std::scoped_lock{m_statusMutex};
  return m_status;
}

std::string RenderThread::formatClassLine() const
{
  const auto lock = std::scoped_lock{m_statusMutex};
  return m_formatClassLine;
}

void RenderThread::converge(const LitFrameStats& lit, const std::uint64_t tableVersion)
{
  // D77, §3.10
  const auto& tonemap = m_tonemap.lastStats();
  auto convergence = FrameConvergence{};
  convergence.pipelinesPending = lit.pendingPipelines > 0 || tonemap.pendingPipelines > 0;
  convergence.materialsPending = lit.waitsForTable;
  convergence.drawsSkipped = lit.retrigger;
  // a slot whose image arrived during the frame is written in the next
  convergence.uploadsLeft = m_uploadsLeft || m_images.hasLoading();
  convergence.imagesPending = m_images.pending() > 0;
  // the scene's chunks, not a test's
  const auto scene = m_geometry && !m_arenaTestChunks;
  convergence.provisionalChunks = scene && m_geometry->waitsForMaterials();
  // DX.4: what the scene still waits for, and the snapshot not taken yet
  convergence.entityDefsPending = scene && m_geometry->waitsForEntityDefs();
  // LP7.2 (C52, D124): a model entity's record is Pending, or its instances wait for a
  // material or a skin
  convergence.modelsPending = scene && m_geometry->waitsForModels();
  // LPF.1 (C43, C44): also while the scene has not scanned the materialsRevision of the
  // frame's table (a re-parse published after the idle pass's geometryTick and before
  // its feedTick): its cells are built from the table before it; the scan, at the next
  // geometryTick, moves sceneWaitState and so triggers the frame that settles; LP5.F
  // (D111): likewise a slot published again unmeasured (an override's new editorSize)
  // the scene has not scanned yet
  convergence.geometryInFlight =
    scene
    && (m_geometry->busy() || m_geometry->waitsForBake() || m_snapshotInboxFull
        || m_geometry->scannedMaterialsRevision() < m_frameMaterialsRevision
        || m_geometry->scannedSizesRevision() < m_frameSizesRevision);
  convergence.lightsProvisional =
    std::ranges::any_of(m_passes.lists().lights, [](const ViewLight& light) {
      return light.record
             && (light.record->provisional || light.record->pendingEntityDef
                 || !light.record->pendingMaterials.empty());
    });
  // U2.11
  convergence.drawsOverflowed = lit.lightsOverflowed > 0 || lit.overflowedDraws > 0;
  // DX.2
  convergence.lightTrianglesPending = lit.lightTrianglesPending;
  // LP3.0 (D151), LP3.3 (C62): the shadows' term
  convergence.shadowsPending = lit.shadowsPending;
  // LP4.2a (D88)
  convergence.probeImagesPending = lit.probeImagesPending;
  // LP4.3 (D88)
  convergence.gridImagesPending = lit.gridImagesPending;
  // LP4.2b step 0
  convergence.probesInFlight = scene && m_geometry->probesInFlight();
  convergence.lutPending = tonemap.lutPending;
  // LPF.1 (C44, D100, P5.2): the counters, not a flag: a hand-over the frame's table does
  // not hold yet keeps it from being settled, whatever the slots say; feedRequested is
  // read after feedApplied was, so a table published between them only makes this true
  convergence.feedBehind =
    m_materials && m_frameFeedApplied < m_materials->feedRequested();
  convergence.passesDisabled =
    lit.prepassDisabled || lit.interactionsDisabled || tonemap.disabled;
  // LP6.4b (C68 (b)): the budget pass left evictions or floors for the next frame
  convergence.budgetLeft = m_budgetLeft;
  m_convergence = convergence;
  m_lastSettled = convergence.settled();
  m_sceneWaitSeen = m_geometry ? std::optional{sceneWaitState()} : std::nullopt;
  // LP6.4b (C68 (d), D174, A60): a settled frame marks a compaction (a kind's least-used
  // arena whose ranges fit the others), run in the next frame drawn for another reason
  // or in an uploads-only idle submission
  if (m_lastSettled && !m_compactionWanted && !m_compactionHeldForTesting)
  {
    m_compactionWanted = m_arenas.planCompaction();
    m_compactionWantedAt = FrameClock::now();
  }

  // the livelock guard: a frame that re-triggers itself without uploading anything or
  // changing what it waits for has made no progress; after MaxStalledFrames of them the
  // thread waits for another trigger
  const auto progress = FrameProgress{
    m_images.resident(),
    m_images.pending(),
    m_images.hasLoading(),
    lit.skippedDraws,
    m_chunksVersion,
    tableVersion,
    m_renderTargetGeneration,
    lit.camera};
  if (!convergence.retriggers())
  {
    m_stalledFrames = 0;
    m_retrigger = false;
  }
  else
  {
    const auto progressed = m_frameUploadBytes > 0 || m_lastProgress != progress;
    m_stalledFrames = progressed ? 0 : m_stalledFrames + 1;
    m_retrigger = m_stalledFrames < MaxStalledFrames;
  }
  m_lastProgress = progress;
}

void RenderThread::setEveryFrameHook(std::function<void(RenderFrameAccess&)> hook)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_everyFrameHook = std::move(hook);
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::runAfterPasses(std::function<void(RenderFrameAccess&)> hook)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_afterPassHooks.push_back(std::move(hook));
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::runAfterTonemap(std::function<void(RenderFrameAccess&)> hook)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_afterTonemapHooks.push_back(std::move(hook));
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::setHdrClear(std::optional<std::array<float, 4>> colour)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_hdrClearForTesting = colour;
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::setTonemapDefaults(const EngineTonemapDefaults& tonemap)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_tonemapForTesting = tonemap;
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::readbackLdr(std::function<void(LdrImage)> done)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_ldrReadbacks.push_back(std::move(done));
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

std::uint64_t RenderThread::ldrStagingCreations() const
{
  return m_ldrStagingCreations;
}

void RenderThread::recordLdrReadback(nvrhi::ICommandList& commandList)
{
  // C29, P2.25: only a settled frame whose tonemap passes ran is copied; LPF.1's settled
  // includes the feed's counters, so a hand-over made before the request is in it
  const auto& tonemap = m_tonemap.lastStats();
  if (!m_convergence.settled() || !tonemap.tonemapped)
  {
    return;
  }
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (m_ldrReadbacks.empty())
    {
      return;
    }
    m_ldrReadbacksInFrame.swap(m_ldrReadbacks);
  }
  const auto ldr = m_frameResources.find(frame_resource::CurrentRenderLdr);
  if (!ldr)
  {
    m_ldrReadbacksInFrame.clear();
    return;
  }
  // LPF.2 (P5.24): one staging texture per extent and format, not one per readback; the
  // last readback has mapped and unmapped it after its frame finished on the GPU
  // (finishLdrReadback), so this frame's copy writes it with nothing else reading it
  // (nvrhi rule 17's CPU side)
  const auto& desc = ldr->texture->getDesc();
  if (
    !m_ldrStaging || m_ldrStaging->getDesc().width != desc.width
    || m_ldrStaging->getDesc().height != desc.height
    || m_ldrStaging->getDesc().format != desc.format)
  {
    m_ldrStaging = m_device.device().createStagingTexture(
      nvrhi::TextureDesc{}
        .setWidth(desc.width)
        .setHeight(desc.height)
        .setFormat(desc.format)
        .setDebugName("Lit preview LDR readback"),
      nvrhi::CpuAccessMode::Read);
    ++m_ldrStagingCreations;
  }
  if (!m_ldrStaging)
  {
    m_ldrReadbacksInFrame.clear();
    return;
  }
  commandList.copyTexture(
    m_ldrStaging, nvrhi::TextureSlice{}, ldr->texture, nvrhi::TextureSlice{});
  m_ldrCopied = true;
}

void RenderThread::finishLdrReadback(const std::size_t slot)
{
  if (!std::exchange(m_ldrCopied, false) || !m_ldrStaging)
  {
    return;
  }
  auto requests = std::exchange(m_ldrReadbacksInFrame, {});
  // the frame's copy is read once the frame has finished; a stop or a park drops it
  if (!waitForFrameEvent(slot))
  {
    return;
  }
  auto& device = m_device.device();
  const auto& desc = m_ldrStaging->getDesc();
  auto image = LdrImage{};
  image.width = desc.width;
  image.height = desc.height;
  auto pitch = std::size_t{0};
  const auto* bytes = static_cast<const std::uint8_t*>(device.mapStagingTexture(
    m_ldrStaging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
  if (!bytes)
  {
    return;
  }
  // rows top first, as the target holds them
  const auto rowBytes = std::size_t(desc.width) * 4;
  image.rgba.resize(rowBytes * desc.height);
  for (std::uint32_t y = 0; y < desc.height; ++y)
  {
    std::copy_n(bytes + y * pitch, rowBytes, image.rgba.data() + y * rowBytes);
  }
  device.unmapStagingTexture(m_ldrStaging);
  for (auto& done : requests)
  {
    done(image);
  }
}

void RenderThread::setTestChunks(std::shared_ptr<const std::map<ChunkKey, Chunk>> chunks)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_testChunks = std::move(chunks);
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::setBeforeChunkJob(
  std::function<void(const std::vector<CellRef>&)> hook)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_beforeChunkJob = std::move(hook);
}

void RenderThread::setUploadCap(const std::uint64_t bytes)
{
  m_uploadCap = bytes;
}

void RenderThread::holdCompletion(const bool hold)
{
  m_holdCompletion = hold;
  if (!hold)
  {
    requestFrame();
  }
}

const std::map<ChunkKey, Chunk>& RenderThread::arenaChunks() const
{
  if (m_arenaTestChunks)
  {
    return *m_arenaTestChunks;
  }
  return m_geometry ? m_geometry->chunks() : m_noChunks;
}

const std::map<ChunkKey, ModelInstance>& RenderThread::arenaInstances() const
{
  // LP7.2 (D129): the scene's instances; a test's chunks replace the scene's whole
  return m_geometry && !m_arenaTestChunks ? m_geometry->instances() : m_noInstances;
}

bool RenderThread::syncChunks()
{
  {
    // the tests' chunks replace the scene's while they are set; their versions count
    // apart from the scene's batches
    const auto lock = std::scoped_lock{m_mutex};
    if (m_arenaTestChunks != m_testChunks)
    {
      m_arenaTestChunks = m_testChunks;
      ++m_chunksVersion;
    }
  }
  if (m_chunksVersion != m_syncedChunksVersion)
  {
    m_syncedChunksVersion = m_chunksVersion;
    // D36: freed in the next frame to be recorded, reused once it has finished
    m_arenas.sync(arenaChunks(), m_frameIndex);
    // LP7.2 (D129, D35): the shared model surfaces, freed so when no instance names them
    m_arenas.syncInstances(arenaInstances(), m_frameIndex);
    updateModelTotals();
  }
  return m_arenas.pending();
}

bool RenderThread::recordUploads(nvrhi::ICommandList& commandList)
{
  // §3.10, D78: before the frame's first pass, within the frame's cap: the chunks first,
  // then the textures, low mips first (P2.14c); LP6.4b (C68 (c), D173): the fed images
  // written in place before them all, whole
  const auto cap = m_uploadCap.load();
  const auto inPlaceLeft =
    m_textureUploads.recordInPlace(commandList, m_images, cap, m_frameUploadBytes);
  // LP6.4b (C68 (d), D174): a compaction marked by a settled frame, before the arenas'
  // uploads (the copies precede every pass, D78)
  if (m_compactionWanted)
  {
    runCompaction(commandList);
  }
  const auto recorded =
    m_arenas.recordUploads(
      commandList, arenaChunks(), cap - std::min(cap, m_frameUploadBytes))
    | kdl::transform([&](const std::uint64_t bytes) {
        m_frameUploadBytes += bytes;
        return true;
      })
    | kdl::if_error([&](const auto& e) { m_events.markFailed(e.msg); })
    | kdl::value_or(false);
  if (!recorded)
  {
    return false;
  }
  // LP7.2 (P7.14): a shared surface placed
  if (m_arenas.uploadedShared() != m_modelTotalsUploads)
  {
    updateModelTotals();
  }
  const auto texturesLeft = m_textureUploads.record(
    m_device.device(),
    commandList,
    m_images,
    cap,
    m_frameUploadBytes,
    [this] { return allocationRoom(); },
    &m_textureHeaps);
  // LP6.4a (P8.3): NVRHI refused to bind a texture into a heap (the probe's assumption
  // is wrong on this device: the texture was made dedicated); said once
  if (!m_bindRefusalWarned && m_textureUploads.dedicated().bindRefused > 0)
  {
    m_bindRefusalWarned = true;
    m_events.post(
      SessionEvent::Warning,
      "NVRHI refused to bind a texture into the lit preview's texture heaps; such "
      "textures are made dedicated");
  }
  // U2.3: the headroom refused a texture or an arena; said once
  if (
    !m_headroomWarned
    && m_arenas.headroomRefusals() + m_textureUploads.headroomRefusals() > 0)
  {
    m_headroomWarned = true;
    m_events.post(
      SessionEvent::Warning,
      fmt::format(
        "The lit preview's device memory allocations reached {} of the device's {}: no "
        "more textures or geometry arenas are made, and the images left bind their "
        "stand-ins (a replaced image its old texture)",
        m_device.deviceMemoryAllocations(),
        m_allocationLimitForTesting.load() != 0
          ? m_allocationLimitForTesting.load()
          : m_device.gpu().maxMemoryAllocationCount));
  }
  // D77: what is left for the next frame makes this one unconverged
  m_uploadsLeft = m_arenas.pending() || texturesLeft || inPlaceLeft;
  // C28: the slots whose written range grew lose their sets before any pass binds them
  eraseChangedSets();
  return true;
}

bool RenderThread::allocationRoom() const
{
  const auto set = m_allocationLimitForTesting.load();
  const auto limit =
    std::uint64_t(set != 0 ? set : m_device.gpu().maxMemoryAllocationCount);
  if (limit == 0)
  {
    return true;
  }
  const auto live =
    std::uint64_t(std::max(m_device.deviceMemoryAllocations(), std::int64_t{0}));
  return live + AllocationHeadroom < limit;
}

void RenderThread::setAllocationLimit(const std::uint32_t limit)
{
  m_allocationLimitForTesting = limit;
}

void RenderThread::setSkipShaderPasses(const bool skip)
{
  m_skipShaderPassesForTesting = skip;
}

bool RenderThread::skipShaderPasses() const
{
  return m_skipShaderPassesForTesting.load();
}

void RenderThread::setDebugView(const DebugView view)
{
  if (m_debugView.exchange(view) == view)
  {
    return;
  }
  // a frame shows it (or takes it away)
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

DebugView RenderThread::debugView() const
{
  return m_debugView.load();
}

void RenderThread::setSkipInteractions(const bool skip)
{
  m_skipInteractionsForTesting = skip;
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_pacer.trigger();
  }
  m_wake.notify_all();
}

void RenderThread::setVramBudget(const std::uint64_t bytes)
{
  m_vramBudget = bytes != 0 ? bytes : m_options.vramBudgetBytes;
}

std::uint64_t RenderThread::vramBudget() const
{
  return m_vramBudget.load();
}

void RenderThread::setBudgetPassHook(std::function<void(const BudgetPassReport&)> hook)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_budgetPassHook = std::move(hook);
}

std::vector<int> RenderThread::startComputePrograms() const
{
  auto programs =
    std::vector<int>{TonemapComputePrograms.begin(), TonemapComputePrograms.end()};
  if (m_options.ssaoForTesting)
  {
    // LP6.5a (C70, P8.12): pass 4's three named programs
    for (const auto& named : EngineNamedPrograms)
    {
      programs.push_back(named.row.index);
    }
  }
  return programs;
}

std::vector<PipelineKey> RenderThread::startPipelineKeys() const
{
  const auto depth = m_frameResources.depthFormat();
  // LP6.5a (C69, P8.12): pass 3's two when the session's SSAO is on
  auto keys = litStartPipelineKeys(depth, m_options.ssaoForTesting);
  keys.push_back(tonemapPipelineKey(depth));
  keys.push_back(blitPipelineKey(
    m_headless && m_headlessTarget ? m_headlessTarget->getDesc().format
                                   : SwapchainImageFormat));
  return keys;
}

void RenderThread::applyHeadlessResize()
{
  auto resize = std::optional<vk::Extent2D>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    resize = std::exchange(m_headlessResize, std::nullopt);
  }
  if (!m_headless || !resize || *resize == m_headlessExtent)
  {
    return;
  }
  // D35: the old target and its framebuffer are held until the frames in flight that
  // drew into it have finished (a clear holds no reference of its own)
  m_frameResources.retire(m_headlessFramebuffer.Get(), 0, m_frameIndex);
  m_frameResources.retire(
    m_headlessTarget.Get(),
    std::uint64_t(m_headlessExtent.width) * m_headlessExtent.height * 4,
    m_frameIndex);
  m_headlessExtent = *resize;
  if (!makeHeadlessTarget())
  {
    m_events.markFailed("Cannot create the headless target");
  }
}

bool RenderThread::prepareFrameResources(const vk::Extent2D extent)
{
  // C28: a new extent makes the targets again and bumps the generation once, and the
  // binding sets of the old targets go
  return m_frameResources.ensureExtent(extent, m_frameIndex)
         | kdl::transform([&](const bool changed) {
             m_renderTargetGeneration = m_frameResources.renderTargetGeneration();
             if (changed)
             {
               // LP6.1 (C64): the draw records are cleared before the old targets' sets
               // go
               m_passes.renderTargetsChanged(m_renderTargetGeneration);
               m_bindingSets.renderTargetsChanged(m_renderTargetGeneration);
             }
             return true;
           })
         | kdl::if_error([&](const auto& e) { m_events.markFailed(e.msg); })
         | kdl::value_or(false);
}

TextureHeapStatus RenderThread::textureHeapStatus() const
{
  auto status = TextureHeapStatus{};
  status.heaps = m_textureHeaps.heaps();
  status.capacityBytes = m_textureHeaps.capacityBytes();
  status.usedBytes = m_textureHeaps.usedBytes();
  status.placed = m_textureHeaps.placedTextures();
  status.placedImageBytes = m_images.textureBytes() - m_images.dedicatedTextureBytes();
  status.freeRanges = m_textureHeaps.freeRanges();
  status.largestFreeBytes = m_textureHeaps.largestFree();
  status.dedicated = m_images.dedicatedTextures(&m_textureHeaps);
  status.dedicatedBytes = m_images.dedicatedTextureBytes();
  status.texturesPlaced = m_textureUploads.texturesPlaced();
  const auto& dedicated = m_textureUploads.dedicated();
  status.dedicatedNoProbe = dedicated.noHeaps;
  status.dedicatedRefusedClass = dedicated.refusedClass;
  status.dedicatedTooLarge = dedicated.tooLarge;
  status.dedicatedNoHeap = dedicated.noHeap;
  status.dedicatedBindRefused = dedicated.bindRefused;
  status.largestPlacedBytes = m_textureHeaps.largestPlaced();
  status.smallestPlacedBytes = m_textureHeaps.smallestPlaced();
  status.largestDedicatedBytes = m_textureUploads.largestDedicatedBytes();
  if (const auto& classes = m_textureHeaps.formatClassesForTesting())
  {
    status.probeMs = classes->milliseconds;
  }
  return status;
}

void RenderThread::probeFormatClassesAtStart()
{
  // LP6.4a (P8.3, D171): NVRHI places a texture in a heap without checking its memory
  // type, alignment or dedicated requirement, so the session asks the driver itself for
  // every class it can make, once; the classes kept dedicated (none expected) are said
  // in the one line
  const auto& gpu = m_device.gpu();
  auto table = probeFormatClasses(
    m_device.device(), m_device.vkDevice(), gpu.physicalDevice, gpu.d24s8Supported);
  auto line = lit::formatClassLine(table);
  m_events.post(SessionEvent::MaterialInfo, line);
  m_textureHeaps.setFormatClasses(std::move(table));
  const auto lock = std::scoped_lock{m_statusMutex};
  m_formatClassLine = std::move(line);
}

GeometryArenas::CompactionResult RenderThread::runCompaction(
  nvrhi::ICommandList& commandList)
{
  const auto compaction = *std::exchange(m_compactionWanted, std::nullopt);
  auto result = m_arenas.compact(&commandList, compaction, m_frameIndex);
  if (result.released)
  {
    // D35: the emptied arena goes once the frames that drew from it have finished;
    // released bytes until then
    m_frameResources.retire(result.released.Get(), result.releasedBytes, m_frameIndex);
    result.released = nullptr;
  }
  return result;
}

void RenderThread::holdCompaction(const bool hold)
{
  m_compactionHeldForTesting = hold;
}

void RenderThread::compactionSubmission()
{
  try
  {
    auto& device = m_device.device();
    // a frame slot as a frame takes it (D35): the frame that last used it has finished
    const auto slot = std::size_t(m_frameIndex % FramesInFlight);
    if (m_frameEventSet[slot])
    {
      if (!waitForFrameEvent(slot))
      {
        return;
      }
      device.resetEventQuery(m_frameEvents[slot]);
      m_frameEventSet[slot] = false;
    }
    m_timers.resolve(device, slot);
    if (m_frameIndex >= FramesInFlight)
    {
      framesCompleted(m_frameIndex - FramesInFlight + 1);
    }
    auto result = GeometryArenas::CompactionResult{};
    {
      auto recording = OpenCommandList{device, *m_commandList};
      result = runCompaction(recording.list());
      recording.submit();
    }
    if (m_events.lost())
    {
      throw vk::DeviceLostError{
        "NVRHI reported a lost device in the compaction's submit"};
    }
    device.setEventQuery(m_frameEvents[slot], nvrhi::CommandQueue::Graphics);
    m_frameEventSet[slot] = true;
    // one ring with the frames: the submission has a frame's number (every retire stamp
    // and framesCompleted count it) and presents nothing (framesPresented unmoved)
    ++m_frameIndex;
    ++m_compactionSubmissions;
    m_idleWorkPending = true;
    m_lastFrameEnd = FrameClock::now();
    // D77: idle is zero frames: an event row, no frame row
    auto row = FrameLogRow{};
    row.session = m_events.generation();
    row.event = FrameLogEvent::Compaction;
    row.allocCount = m_device.deviceMemoryAllocations();
    row.arenaBytes = m_arenas.capacityBytes();
    row.arenaMovedBytes = result.movedBytes;
    m_timers.append(row);
    device.runGarbageCollection();
  }
  catch (const vk::DeviceLostError& e)
  {
    m_events.markLost(e.what());
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(e.what());
  }
}

void RenderThread::warnVramBudget(const std::uint64_t used, const std::uint64_t budget)
{
  // §3.10, C11 (U2.6, V2.6): once per session, what eviction cannot bring under
  if (std::exchange(m_budgetWarned, true))
  {
    return;
  }
  m_events.post(
    SessionEvent::Warning,
    fmt::format(
      "The lit preview's GPU memory ({} MB) passes the device's budget ({} MB)",
      used / (1024 * 1024),
      budget / (1024 * 1024)));
}

void RenderThread::budgetPass()
{
  const auto frame = m_frameIndex;
  auto hook = std::function<void(const BudgetPassReport&)>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hook = m_budgetPassHook;
  }
  auto report = BudgetPassReport{};
  report.frame = frame;
  const auto finish = kdl::invoke_later{[&] {
    report.left = m_budgetLeft;
    if (hook)
    {
      // the stamp: the slots a set of this frame bound, before the bumps' erase
      for (ImageSlotId id = 0; id < ImageSlotId(m_images.size()); ++id)
      {
        if (const auto last = m_bindingSets.lastBound(id); last && *last == frame)
        {
          report.drawn.push_back(id);
        }
      }
      hook(report);
    }
  }};

  // D172 read the other way (the director's reading (3)): an Evicted slot a set of this
  // frame bound (its intrinsic standing in for it) or a draw asked for by its state is
  // asked again, as a first request is
  for (const auto id : m_images.evictedSlots())
  {
    const auto last = m_bindingSets.lastBound(id);
    if (m_images.wanted(id) || (last && *last == frame))
    {
      m_images.reRequestEvicted(id, frame);
      report.reRequested.push_back(id);
    }
  }
  m_images.clearWanted();
  m_budgetLeft = false;

  // Q48: the session's budget against the live figure, and VK_EXT_memory_budget's
  // budget against its usage (read at the first frame, after a settled one, and while
  // it says over: a driver call)
  const auto account = accountGpuMemory(
    m_frameResources, m_arenas, m_images, m_passes, m_tonemap, m_textureHeaps);
  const auto live = account.live();
  const auto budget = vramBudget();
  if (!m_budgetChecked || m_lastSettled || m_extensionOver)
  {
    m_budgetChecked = true;
    m_extensionVram = m_device.vram();
  }
  m_extensionOver =
    m_extensionVram && m_extensionVram->usedBytes > m_extensionVram->budgetBytes;
  report.liveBytes = live;
  report.budgetBytes = budget;
  report.over = live > budget || m_extensionOver;
  if (!report.over)
  {
    m_budgetExhaustedLive.reset();
    return;
  }
  // nothing was left to let go at this live figure: no further pass until it changes
  if (m_budgetExhaustedLive == live)
  {
    return;
  }

  // what is let go already and waits for its frames, and the complete textures the
  // floors in flight let go at their swaps, are not counted again
  const auto freeing =
    account.releasedBytes + m_textureHeaps.pendingBytes() + m_images.flooringBytes();
  // what to let go: to the budget less a sixteenth (the hysteresis), and to the budget
  // itself (the floor's trigger)
  const auto excess = [&](const std::uint64_t used, const std::uint64_t limit) {
    const auto net = used > freeing ? used - freeing : 0;
    return net > limit ? net - limit : std::uint64_t{0};
  };
  auto needed = std::uint64_t{0};
  auto passing = std::uint64_t{0};
  if (live > budget)
  {
    needed = excess(live, budget - budget / 16);
    passing = excess(live, budget);
  }
  if (m_extensionOver)
  {
    const auto extension = m_extensionVram->budgetBytes;
    needed =
      std::max(needed, excess(m_extensionVram->usedBytes, extension - extension / 16));
    passing = std::max(passing, excess(m_extensionVram->usedBytes, extension));
  }
  if (needed == 0)
  {
    return;
  }

  // the candidates: Resident slots, no intrinsic, their own complete texture bound, no
  // frame has bound for EvictionIdleFrames frames (the max lastUsed over their sets, the
  // stamp of C64's record path, A54; 0 when no set holds them); never one of this frame
  struct Candidate
  {
    std::uint64_t lastBound = 0;
    ImageSlotId id = NoImageSlot;
  };
  auto candidates = std::vector<Candidate>{};
  const auto complete = [&](const ImageSlots::Slot& slot) {
    return slot.state == ImageSlots::State::Resident && slot.intrinsic != NoImageSlot
           && slot.texture && slot.firstWrittenMip == 0 && slot.mipLevels > 0;
  };
  for (ImageSlotId id = 0; id < ImageSlotId(m_images.size()); ++id)
  {
    if (!complete(*m_images.slot(id)))
    {
      continue;
    }
    const auto last = m_bindingSets.lastBound(id).value_or(0);
    if (last + EvictionIdleFrames <= frame)
    {
      candidates.push_back(Candidate{last, id});
    }
  }
  // least recently bound first
  std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
    return std::tie(a.lastBound, a.id) < std::tie(b.lastBound, b.id);
  });
  auto freed = std::uint64_t{0};
  auto done = std::size_t{0};
  for (const auto& candidate : candidates)
  {
    if (freed >= needed)
    {
      break;
    }
    if (done == EvictionsPerFrame)
    {
      m_budgetLeft = true;
      break;
    }
    if (const auto bytes = m_images.evict(candidate.id, frame); bytes > 0)
    {
      freed += bytes;
      ++done;
      report.evicted.emplace_back(candidate.id, candidate.lastBound);
    }
  }

  // the mip floor (the director's reading (4)): no unbound candidate left and the budget
  // itself still passed: the bound slots not at their floor, the largest first, through
  // the Replacing path, to the budget less a sixteenth; a floor's saving as its levels'
  // share of the texture (a level a quarter of the one before)
  if (freed < passing && !m_budgetLeft)
  {
    struct FloorCandidate
    {
      std::uint64_t bytes = 0;
      std::uint64_t saving = 0;
      ImageSlotId id = NoImageSlot;
    };
    auto floors = std::vector<FloorCandidate>{};
    for (ImageSlotId id = 0; id < ImageSlotId(m_images.size()); ++id)
    {
      const auto& slot = *m_images.slot(id);
      if (!complete(slot) || slot.floored)
      {
        continue;
      }
      const auto& desc = slot.texture->getDesc();
      const auto level = mipFloorLevel(desc.width, desc.height, int(desc.mipLevels));
      if (level == 0)
      {
        continue;
      }
      const auto kept = slot.textureBytes >> std::min(2 * level, 62);
      floors.push_back(FloorCandidate{slot.textureBytes, slot.textureBytes - kept, id});
    }
    std::ranges::sort(floors, [](const FloorCandidate& a, const FloorCandidate& b) {
      return a.bytes != b.bytes ? a.bytes > b.bytes : a.id < b.id;
    });
    for (const auto& candidate : floors)
    {
      if (freed >= needed)
      {
        break;
      }
      if (done == EvictionsPerFrame)
      {
        m_budgetLeft = true;
        break;
      }
      if (m_images.floor(candidate.id, frame))
      {
        freed += candidate.saving;
        ++done;
        report.floored.push_back(candidate.id);
      }
    }
  }

  // every bound slot at its floor and the budget still passed: said once (D77 accepts
  // the floor)
  if (done == 0 && passing > 0)
  {
    m_budgetExhaustedLive = live;
    if (m_extensionOver)
    {
      warnVramBudget(m_extensionVram->usedBytes, m_extensionVram->budgetBytes);
    }
    else
    {
      warnVramBudget(live, budget);
    }
  }
}

void RenderThread::runFrameHooks(nvrhi::ICommandList& commandList)
{
  auto hooks = std::vector<std::function<void(RenderFrameAccess&)>>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hooks.swap(m_frameHooks);
  }
  auto everyFrame = std::function<void(RenderFrameAccess&)>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    everyFrame = m_everyFrameHook;
  }
  if (hooks.empty() && !everyFrame)
  {
    return;
  }
  auto access = frameAccess(commandList);
  if (everyFrame)
  {
    everyFrame(access);
  }
  for (auto& hook : hooks)
  {
    hook(access);
  }
}

RenderFrameAccess RenderThread::frameAccess(nvrhi::ICommandList& commandList)
{
  const auto models = currentModelTotals();
  auto dress = DressTotals{};
  if (m_geometry && !m_arenaTestChunks)
  {
    const auto counts = m_geometry->dressCounts();
    dress = DressTotals{counts.surfaces, counts.unmatched, counts.inlined, counts.ties};
  }
  return RenderFrameAccess{
    m_device.device(),
    commandList,
    m_frameIndex,
    m_frameResources,
    m_arenas,
    m_pipelines,
    m_images,
    m_bindingSets,
    m_textureUploads,
    m_samplers,
    m_frameUploadBytes,
    m_passes,
    m_tonemap,
    m_convergence,
    m_frameHdrClear,
    arenaChunks(),
    arenaInstances(),
    models,
    dress,
    m_frameStart,
    m_textureHeaps};
}

void RenderThread::runAfterPassHooks(nvrhi::ICommandList& commandList)
{
  auto hooks = std::vector<std::function<void(RenderFrameAccess&)>>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hooks.swap(m_afterPassHooks);
  }
  if (hooks.empty())
  {
    return;
  }
  auto access = frameAccess(commandList);
  for (auto& hook : hooks)
  {
    hook(access);
  }
}

void RenderThread::runAfterTonemapHooks(nvrhi::ICommandList& commandList)
{
  auto hooks = std::vector<std::function<void(RenderFrameAccess&)>>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hooks.swap(m_afterTonemapHooks);
  }
  if (hooks.empty())
  {
    return;
  }
  auto access = frameAccess(commandList);
  for (auto& hook : hooks)
  {
    hook(access);
  }
}

void RenderThread::recordHdrClear(
  nvrhi::ICommandList& commandList, const std::array<float, 4>& colour)
{
  // as LitPasses::recordClears, with the HDR target's colour the test's
  namespace fr = frame_resource;
  const auto hdr = m_frameResources.find(fr::CurrentRenderHdr);
  const auto ldr = m_frameResources.find(fr::CurrentRenderLdr);
  const auto depth = m_frameResources.find(fr::CurrentDepth);
  if (!hdr || !ldr || !depth)
  {
    return;
  }
  commandList.clearTextureFloat(
    hdr->texture,
    nvrhi::AllSubresources,
    nvrhi::Color{colour[0], colour[1], colour[2], colour[3]});
  commandList.clearTextureFloat(
    ldr->texture, nvrhi::AllSubresources, nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
  commandList.clearDepthStencilTexture(
    depth->texture, nvrhi::AllSubresources, true, 1.0f, true, EngineFrame.stencilClear);
}

void RenderThread::recordTonemap(
  nvrhi::ICommandList& commandList,
  nvrhi::ITexture* target,
  nvrhi::IFramebuffer* targetFramebuffer,
  const bool hdrDrawn)
{
  // LP2.2b (§3.8 pass 10): histogram, exposure and tonemapping into ldrFBO, then the
  // blit into the frame's target (SC201, SC203)
  auto inputs = TonemapFrameInputs{};
  inputs.device = &m_device.device();
  inputs.commandList = &commandList;
  inputs.frameIndex = m_frameIndex;
  inputs.frame = &m_frameResources;
  inputs.pipelines = &m_pipelines;
  inputs.images = &m_images;
  inputs.bindingSets = &m_bindingSets;
  inputs.samplers = &m_samplers;
  inputs.target = target;
  inputs.targetFramebuffer = targetFramebuffer;
  inputs.hdrDrawn = hdrDrawn;
  const auto slot = std::size_t(m_frameIndex % FramesInFlight);
  inputs.tonemapTimer = m_timers.query(slot, TimedPass::Tonemap);
  inputs.blitTimer = m_timers.query(slot, TimedPass::Blit);
  inputs.stopRequested = [this] { return stopOrParkRequested(); };
  // LP3.4 (D158): the selected debug view into the LDR target, after the tonemap pass and
  // before the blit, with what the lit passes bind with
  const auto debugView = m_debugView.load();
  if (debugView != DebugView::None)
  {
    inputs.beforeBlit = [&](nvrhi::IFramebuffer* ldr) {
      auto lit = LitFrameInputs{};
      lit.device = &m_device.device();
      lit.commandList = &commandList;
      lit.frameIndex = m_frameIndex;
      lit.frame = &m_frameResources;
      lit.arenas = &m_arenas;
      lit.pipelines = &m_pipelines;
      lit.images = &m_images;
      lit.bindingSets = &m_bindingSets;
      lit.samplers = &m_samplers;
      m_passes.recordDebugView(
        lit,
        ldr,
        FramebufferFormats{nvrhi::Format::RGBA8_UNORM, m_frameResources.depthFormat()},
        debugView);
    };
  }
  const auto stats = m_tonemap.record(inputs);
  if (stats.tonemapTimed)
  {
    m_timers.passTimed(slot, TimedPass::Tonemap);
  }
  if (stats.blitTimed)
  {
    m_timers.passTimed(slot, TimedPass::Blit);
  }
  // C28: the LUT's slot keeps the set it made
  eraseChangedSets();
}

bool RenderThread::recordLitPasses(
  nvrhi::ICommandList& commandList, const CameraState& camera)
{
  // LP2.2a (§3.8 passes 1 and 7): the clears, the prepass and the interactions into
  // hdrFBO, with the scene the arenas hold (the scene's chunks or a test's)
  auto inputs = LitFrameInputs{};
  inputs.device = &m_device.device();
  inputs.commandList = &commandList;
  inputs.frameIndex = m_frameIndex;
  inputs.frame = &m_frameResources;
  inputs.arenas = &m_arenas;
  inputs.pipelines = &m_pipelines;
  inputs.images = &m_images;
  inputs.bindingSets = &m_bindingSets;
  inputs.samplers = &m_samplers;
  inputs.camera = camera;
  inputs.chunks = &arenaChunks();
  inputs.chunksVersion = m_chunksVersion;
  inputs.instances = &arenaInstances();
  if (m_geometry && !m_arenaTestChunks)
  {
    inputs.entities = &m_geometry->entityStates();
    // LP4.1 (D79): the bake's tree and AreaData for the view's portal flow; a test's
    // chunks are not the scene's, so they are drawn without it (D94)
    inputs.procTree = m_geometry->procTree();
    inputs.areas = m_geometry->areaData();
    // LP5.3 (C49, D123): the fog portals of that tree's areas
    inputs.portalFog = m_geometry->portalFogTable();
    // LP4.3 (C36): the bake's light grid, read with its tree
    inputs.grid = m_geometry->gridTable();
  }
  if (m_geometry)
  {
    inputs.lights = m_geometry->lightTable();
    // LP4.2a (C37): the probes, which the view chooses from every frame
    inputs.probes = m_geometry->probeTable();
    // LP6.4a step 0b (C67 (b), D152): the first batch's early cells are no scene pass 6
    // holds its shadows across
    const auto& batch = m_geometry->firstBatch();
    inputs.sceneWhole =
      m_arenaTestChunks || batch.whole || batch.worldJobs + batch.entityJobs == 0;
  }
  if (m_materials)
  {
    inputs.materials = m_materials->table();
  }
  const auto tableVersion = inputs.materials ? inputs.materials->version : 0;
  inputs.service = m_materials;
  inputs.pool = m_pool;
  inputs.stopRequested = [this] { return stopOrParkRequested(); };
  inputs.warn = [this](std::string message) {
    m_events.post(SessionEvent::Warning, std::move(message));
  };
  const auto slot = std::size_t(m_frameIndex % FramesInFlight);
  inputs.prepassTimer = m_timers.query(slot, TimedPass::Prepass);
  inputs.interactionTimer = m_timers.query(slot, TimedPass::Interactions);
  inputs.ambientTimer = m_timers.query(slot, TimedPass::Ambient);
  // LP6.5a (C69, C70, C65): passes 3 and 4's, reserved by LP6.0
  inputs.gbufferTimer = m_timers.query(slot, TimedPass::Gbuffer);
  inputs.ssaoTimer = m_timers.query(slot, TimedPass::Ssao);
  // LP5.2a (C46, C47): the stage pass's timer, and the game folder of its material
  // programs' blobs
  inputs.stageTimer = m_timers.query(slot, TimedPass::Stages);
  // LP5.3 (C49): pass 9's timer
  inputs.fogTimer = m_timers.query(slot, TimedPass::Fog);
  // LP3.1 (D157): pass 6's
  inputs.shadowTimer = m_timers.query(slot, TimedPass::Shadows);
  // LP3.3 (D146, P3.4a): relocated tiles are copied when the device copies D32 images
  inputs.depthCopy = m_device.gpu().depthCopySupported;
  // LP3.3 (C62, D152)
  inputs.shadowsMoving = m_frameShadowsMoving;
  inputs.gamePath = m_options.gamePath;
  // LP6.5a (C69, C70, D177): passes 3 and 4 when the session's SSAO is on
  inputs.ssao = m_options.ssaoForTesting;
  inputs.afterPrepass = [this] { runDebugHook(DebugHook::AfterPrepass); };
  // C38 (LP4.2b): r_skipInteractions 1's stand-in, a test's switch
  m_passes.setSkipInteractionsForTesting(m_skipInteractionsForTesting.load());
  // LP5.0 (D118): r_skipAmbient's stand-in, which the stage pass reads (LP5.2a)
  m_passes.setSkipShaderPassesForTesting(m_skipShaderPassesForTesting.load());
  // LP3.1 (C57): r_skipShadows 1's stand-in, a test's session option
  m_passes.setSkipShadowsForTesting(m_options.skipShadowsForTesting);
  const auto stats = m_passes.record(inputs);
  if (stats.prepassTimed)
  {
    m_timers.passTimed(slot, TimedPass::Prepass);
  }
  if (stats.ambientTimed)
  {
    m_timers.passTimed(slot, TimedPass::Ambient);
  }
  if (stats.gbufferTimed)
  {
    m_timers.passTimed(slot, TimedPass::Gbuffer);
  }
  if (stats.ssaoTimed)
  {
    m_timers.passTimed(slot, TimedPass::Ssao);
  }
  if (stats.interactionsTimed)
  {
    m_timers.passTimed(slot, TimedPass::Interactions);
  }
  if (stats.stagesTimed)
  {
    m_timers.passTimed(slot, TimedPass::Stages);
  }
  if (stats.fogTimed)
  {
    m_timers.passTimed(slot, TimedPass::Fog);
  }
  if (stats.shadowAtlasTimed)
  {
    m_timers.passTimed(slot, TimedPass::Shadows);
  }
  // C28: the slots asked for by the passes' draws lose no set they made
  eraseChangedSets();
  // D77: what a table publication resolves waits for one (litTableTick)
  m_litWaitsForTable = stats.waitsForTable;
  m_litTableVersion = tableVersion;
  // LPF.1 (C44): the lists are the ones of this table's materialsRevision (ListsTableKey)
  m_frameListsRevision = inputs.materials ? inputs.materials->materialsRevision : 0;
  return stats.retrigger;
}

void RenderThread::litTableTick()
{
  if (!m_litWaitsForTable || !m_materials)
  {
    return;
  }
  if (m_materials->table()->version == m_litTableVersion)
  {
    return;
  }
  // D77: a table published while the lit passes waited for one is a frame trigger;
  // U2.5: through the arrival clock, so a start's stream of publications (one a texture)
  // draws at most 10 frames a second, each a rebuild of the lists
  m_litWaitsForTable = false;
  const auto now = FrameClock::now();
  const auto lock = std::scoped_lock{m_mutex};
  arrivalTrigger(now);
}

void RenderThread::lightTrianglesTick()
{
  if (!m_passes.collectLightTriangles())
  {
    return;
  }
  const auto lock = std::scoped_lock{m_mutex};
  m_pacer.trigger();
}

void RenderThread::feedTick()
{
  if (!m_materials)
  {
    return;
  }
  const auto table = m_materials->table();
  if (table->feedApplied == m_frameFeedApplied)
  {
    return;
  }
  // C44, D101: a feed publication the last frame did not refresh with is a frame trigger
  // at once, not through the arrival clock (the pacer still spaces frame starts by
  // MinFrameInterval), when it brings the render side something to draw (a slot changed:
  // imageChanges; a re-parse: materialsRevision) or when the last frame was behind the
  // feed (feedBehind) and must settle. A refusal, or an override of a name no slot uses,
  // changes nothing drawn: a settled session draws no frame for it. H.1
  // (19-LITGPU-HANG.md): every publication's tableChanged wakes the loop for another
  // idle pass, which reads the moved counter here whatever the slots' states, so no
  // publication is waited past
  const auto changes = table->imageChanges != m_frameImageChanges
                       || table->materialsRevision != m_frameMaterialsRevision;
  if (!changes && !m_convergence.feedBehind)
  {
    return;
  }
  const auto lock = std::scoped_lock{m_mutex};
  m_pacer.trigger();
}

void RenderThread::tableTick()
{
  // the scene's chunks, not a test's, and a last frame to compare with
  if (!m_materials || !m_geometry || m_arenaTestChunks || !m_sceneWaitSeen)
  {
    return;
  }
  const auto table = m_materials->table();
  if (
    table->registry == m_frameRegistry
    && table->materialsRevision == m_frameMaterialsRevision
    && table->sizesRevision == m_frameSizesRevision)
  {
    return;
  }
  // LP5.G (D18; C44's rule generalised): a table with another registry (a build: reload,
  // the poll's changed decl file) or materialsRevision or sizesRevision than the last
  // frame's is a frame trigger at once (the pacer still spaces frame starts) when the
  // scene's scan of it gave the scene work: a batch, a wait for a measure, an entityDef
  // or the probes (the wait state's terms before the scanned revisions; a scan that
  // only took the revisions changes nothing drawn), whose frame is unconverged until the
  // work lands (geometryTick's trigger then). A build's chunks are byte-identical when
  // nothing moved them, so no upload triggers it; a lookup or the poll moves no key and
  // draws none. H.1 (19-LITGPU-HANG.md): each publication's tableChanged wakes the loop;
  // the idle pass cleared m_tableChanged before geometryTick adopted and scanned the
  // newest table (or a batch in flight adopts it at its end, polled while building),
  // this reads the newest table after it, and a publication meanwhile makes the pass
  // run again before any wait: none is waited past
  const auto work = [](const SceneWaitState& state) {
    return std::tuple{
      std::get<0>(state),
      std::get<1>(state),
      std::get<2>(state),
      std::get<3>(state),
      std::get<4>(state),
      std::get<5>(state),
      std::get<6>(state),
      std::get<9>(state),
      std::get<10>(state)};
  };
  if (work(sceneWaitState()) == work(*m_sceneWaitSeen))
  {
    return;
  }
  const auto lock = std::scoped_lock{m_mutex};
  m_pacer.trigger();
}

void RenderThread::arrivalTrigger(const FrameClock::time_point now)
{
  const auto at = m_lastArrivalTrigger + TextureArrivalInterval;
  if (now >= at)
  {
    m_lastArrivalTrigger = now;
    m_arrivalTriggerAt.reset();
    m_pacer.trigger();
  }
  else if (!m_arrivalTriggerAt)
  {
    m_arrivalTriggerAt = at;
  }
}

void RenderThread::main() noexcept
{
  try
  {
    if (const auto error = m_timers.openLog(m_options.frameLogPath))
    {
      m_events.post(SessionEvent::Warning, *error);
    }
    appendSessionRow(FrameLogEvent::Start);
    // S31: the inbox is empty
    m_events.post(SessionEvent::ReadyForSnapshot, {});
    // D69: LP2's intrinsic images, which also stand in for pending slots (P2.14d)
    if (m_materials)
    {
      for (const auto& key : lp2IntrinsicKeys())
      {
        m_images.request(key);
      }
      // LP3.1 (C60): _blueNoise256, asked for at Start always (the _brdfLut pattern: the
      // parse thread makes it, 512² RGBA8, 1 MB), so that no frame waits for it when a
      // light first casts
      m_images.request(intrinsicImageKey("_blueNoise256"));
    }
    runLoop();
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(fmt::format("the render thread failed: {}", e.what()));
  }
  catch (...)
  {
    m_events.markFailed("the render thread failed");
  }
  finish();

  {
    const auto lock = std::scoped_lock{m_mutex};
    m_exited = true;
  }
  m_stateChanged.notify_all();
}

void RenderThread::runLoop()
{
  auto lock = std::unique_lock{m_mutex};
  while (!m_stopRequested)
  {
    if (m_parkRequested)
    {
      if (!m_parked)
      {
        lock.unlock();
        parkGpuWork();
        lock.lock();
        m_parked = true;
        m_stateChanged.notify_all();
        lock.unlock();
        runDebugHook(DebugHook::AfterParking);
        m_events.post(SessionEvent::Parked, {});
        lock.lock();
      }
      m_wake.wait(lock, [&] { return m_stopRequested || !m_parkRequested; });
      if (!m_parkRequested)
      {
        m_parked = false;
        // Review V.1: the park released the generation's image wrappers. Resumed without
        // a new generation, for example by an unpark that came while it parked, it draws
        // nothing on this one and asks the GUI thread for the next.
        if (m_swapchain && m_swapchain->imageTextures.empty() && !m_stopRequested)
        {
          lock.unlock();
          requestRecreate(*m_swapchain, "resumed after a park without a new generation");
          lock.lock();
        }
      }
      continue;
    }

    if (m_events.lost() && !m_events.lostReported())
    {
      lock.unlock();
      m_events.markLost("NVRHI reported a lost device");
      lock.lock();
      continue;
    }

    const auto now = FrameClock::now();
    const auto next = m_pacer.nextFrameTime(canDraw());
    if (next && *next <= now)
    {
      // LP3.3 (C62): the frame's pass 6 holds or settles (the still timer's settles)
      m_frameShadowsMoving = m_pacer.cameraMoving(now);
      m_pacer.frameStarted(now);
      m_frameStart = now;
      const auto frameCamera = m_camera;
      const auto cameraSet = m_cameraSet;
      // S31, B0.3: the frame takes the snapshot offered once the last one's chunks are
      // applied; ReadyForSnapshot then says the inbox is free again
      auto snapshot = std::shared_ptr<const Snapshot>{};
      auto previous = std::shared_ptr<const Snapshot>{};
      auto beforeChunkJob =
        std::optional<std::function<void(const std::vector<CellRef>&)>>{};
      if (m_snapshotInbox && (!m_geometry || m_geometry->readyForSnapshot()))
      {
        snapshot = std::move(m_snapshotInbox);
        previous = std::exchange(m_snapshot, snapshot);
        m_snapshotInboxFull = false;
        ++m_snapshotsTaken;
        beforeChunkJob = std::exchange(m_beforeChunkJob, std::nullopt);
      }
      lock.unlock();
      // B0.9: the snapshot taken before is freed here, outside m_mutex, which the GUI
      // thread's setCamera and offerSnapshot take
      previous.reset();
      if (snapshot)
      {
        m_events.post(SessionEvent::ReadyForSnapshot, {});
        if (m_geometry)
        {
          // LP6.3 (C67 (b)): the batch the snapshot starts puts the view's cells on the
          // pool first, the frame's view at the target's size; without a camera (a
          // headless session that set none) in the dirty set's order
          if (cameraSet)
          {
            const auto extent = m_swapchain ? m_swapchain->extent : m_headlessExtent;
            m_geometry->setView(frameCamera, extent.width, extent.height);
          }
          if (beforeChunkJob)
          {
            m_geometry->setBeforeJobForTesting(std::move(*beforeChunkJob));
          }
          m_geometry->takeSnapshot(*snapshot);
        }
      }
      const auto outcome = drawFrame(frameCamera);
      lock.lock();
      if (outcome == FrameOutcome::Skipped)
      {
        m_pacer.frameSkipped();
      }
      else if (outcome == FrameOutcome::Presented && m_retrigger)
      {
        // D77: an unconverged pass is a frame trigger
        m_pacer.trigger();
      }
      // LP3.0 (D151, C62): whether a settle wants the still timer (LP3.3: a held LOD, a
      // pending cascade budget, a held layout that is not the engine's)
      m_pacer.setSettleWanted(m_passes.lastStats().shadowSettleWanted);
      continue;
    }

    auto deadline = next.value_or(FrameClock::time_point::max());

    // LP3.0 (D151, C62, P3.13): the still timer, a frame trigger once the camera has been
    // still for StillTimerDelay while a settle is wanted (LP3.3); its frame settles
    if (const auto still = m_pacer.stillTimerTime(); still && canDraw())
    {
      if (*still <= now)
      {
        m_pacer.stillTimerFired();
        continue;
      }
      deadline = std::min(deadline, *still);
    }

    // the chunk jobs: a finished batch is applied, the inbox may be taken again, and a
    // newer material table is adopted (R2.2: tableChanged wakes the thread for it);
    // outside m_mutex, because they take the material service's mutex
    m_tableChanged = false;
    lock.unlock();
    runDebugHook(DebugHook::IdleUnlocked);
    const auto building = geometryTick();
    arrivalsTick();
    litTableTick();
    lightTrianglesTick();
    feedTick();
    tableTick();
    lock.lock();
    // D77: a texture arrival that came within TextureArrivalInterval of the last one
    if (m_arrivalTriggerAt && *m_arrivalTriggerAt <= FrameClock::now())
    {
      m_arrivalTriggerAt.reset();
      m_lastArrivalTrigger = FrameClock::now();
      m_pacer.trigger();
    }
    // the mutex was released for the chunk jobs: a stop, park, trigger, exposure or table
    // that came meanwhile, whose notify found no waiter, or the trigger geometryTick set
    // itself, is acted on, not waited past (plans/lit-preview/19-LITGPU-HANG.md)
    if (
      m_stopRequested || m_parkRequested || m_tableChanged
      || m_pacer.nextFrameTime(canDraw()) != next)
    {
      continue;
    }
    if (building || m_passes.lightTrianglesBusy())
    {
      deadline = std::min(deadline, now + GeometryPollInterval);
    }
    if (m_arrivalTriggerAt)
    {
      deadline = std::min(deadline, *m_arrivalTriggerAt);
    }

    // LP6.4b (C68 (d), D174, the director's reading (6)): a compaction no frame took
    // within CompactionIdleDelay of the settled frame that marked it: an uploads-only
    // submission, outside m_mutex as idleTick runs
    if (m_compactionWanted && !m_events.lost() && !m_events.failed())
    {
      const auto compactAt = m_compactionWantedAt + CompactionIdleDelay;
      if (compactAt <= now)
      {
        lock.unlock();
        compactionSubmission();
        lock.lock();
        continue;
      }
      deadline = std::min(deadline, compactAt);
    }

    if (m_idleWorkPending && !m_events.lost())
    {
      const auto idleAt = m_lastFrameEnd + IdleCollectInterval;
      if (idleAt <= now)
      {
        lock.unlock();
        idleTick();
        lock.lock();
        continue;
      }
      deadline = std::min(deadline, idleAt);
    }

    if (deadline == FrameClock::time_point::max())
    {
      m_wake.wait(lock);
    }
    else
    {
      m_wake.wait_until(lock, deadline);
    }
  }
}

bool RenderThread::stopOrParkRequested() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_stopRequested || m_parkRequested;
}

bool RenderThread::canDraw() const
{
  if (m_events.lost() || m_events.failed() || !m_exposed)
  {
    return false;
  }
  // a generation that a park left without its image wrappers is never drawn again
  return m_headless
           ? bool(m_headlessTarget)
           : m_swapchain && !m_swapchain->stale && !m_swapchain->imageTextures.empty();
}

RenderThread::FrameOutcome RenderThread::drawFrame(const CameraState& camera)
{
  try
  {
    if (takeFault(DebugFault::DeviceLostBeforeFrame))
    {
      throw vk::DeviceLostError{"injected before the frame"};
    }
    // a pass boundary
    if (stopOrParkRequested())
    {
      return FrameOutcome::Skipped;
    }

    auto& device = m_device.device();

    // frame N - FramesInFlight has finished: its event query, acquire semaphore and
    // timer query are free again (D35)
    const auto slot = std::size_t(m_frameIndex % FramesInFlight);
    if (m_frameEventSet[slot])
    {
      if (!waitForFrameEvent(slot))
      {
        return FrameOutcome::Skipped;
      }
      device.resetEventQuery(m_frameEvents[slot]);
      m_frameEventSet[slot] = false;
    }
    m_timers.resolve(device, slot);
    // frames 0 to N - FramesInFlight have finished
    if (m_frameIndex >= FramesInFlight)
    {
      framesCompleted(m_frameIndex - FramesInFlight + 1);
    }

    // the pass boundary before LP0's one pass: park and stop are honoured before an
    // image is acquired
    if (stopOrParkRequested())
    {
      return FrameOutcome::Skipped;
    }

    applyHeadlessResize();
    auto* const target = m_swapchain.get();
    if (!target && !m_headless)
    {
      return FrameOutcome::Skipped;
    }
    if (target && target->imageTextures.empty())
    {
      // never indexed: a park released them, and only a new generation brings them back
      target->stale = true;
      requestRecreate(*target, "the generation has no image wrappers");
      return FrameOutcome::Skipped;
    }
    // C14, C28: the frame's targets at the surface's extent, before an image is acquired
    if (!prepareFrameResources(target ? target->extent : m_headlessExtent))
    {
      return FrameOutcome::Stopped;
    }
    auto imageIndex = std::uint32_t{0};
    auto acquireMs = 0.0;
    auto presentMs = 0.0;
    nvrhi::ITexture* texture = m_headlessTarget.Get();
    auto extent = m_headlessExtent;
    if (target)
    {
      // the vk::Result overload: OUT_OF_DATE is a value, not an exception
      const auto acquireStart = FrameClock::now();
      const auto result = m_device.vkDevice().acquireNextImageKHR(
        target->swapchain,
        AcquireTimeoutNs,
        target->acquireSemaphores[slot],
        nullptr,
        &imageIndex);
      acquireMs = millisecondsSince(acquireStart);
      switch (result)
      {
      case vk::Result::eSuccess:
        break;
      case vk::Result::eSuboptimalKHR:
        // the image is acquired and its semaphore will be signalled: draw and present
        requestRecreate(*target, "acquire returned SUBOPTIMAL", true);
        break;
      case vk::Result::eTimeout:
      case vk::Result::eNotReady:
        // D48: skip; the trigger stays pending, so it is retried
        ++m_framesSkipped;
        return FrameOutcome::Skipped;
      case vk::Result::eErrorOutOfDateKHR:
        ++m_framesSkipped;
        target->stale = true;
        requestRecreate(*target, "acquire returned OUT_OF_DATE");
        return FrameOutcome::Skipped;
      case vk::Result::eErrorSurfaceLostKHR:
        ++m_framesSkipped;
        target->stale = true;
        m_events.post(
          SessionEvent::Error,
          "The window's Vulkan surface was lost. Stop and Start the preview.");
        return FrameOutcome::Skipped;
      case vk::Result::eErrorDeviceLost:
        throw vk::DeviceLostError{"vkAcquireNextImageKHR"};
      default:
        m_events.markFailed(
          fmt::format("vkAcquireNextImageKHR returned {}", vk::to_string(result)));
        return FrameOutcome::Stopped;
      }
      texture = target->imageTextures[imageIndex].Get();
      extent = target->extent;
      // every submit from here on waits for the image, also the one an exception's unwind
      // makes (review V.3)
      m_device.vulkanDevice().queueWaitForSemaphore(
        nvrhi::CommandQueue::Graphics, target->acquireSemaphores[slot], 0);
    }
    if (takeFault(DebugFault::DeviceLostAfterAcquire))
    {
      throw vk::DeviceLostError{"injected after the acquire"};
    }

    const auto recordStart = FrameClock::now();
    // C16: the slot's queries time the frame and its passes (R4-2.20)
    m_timers.frameStarted(slot);
    auto* const timer = m_timers.query(slot, TimedPass::Frame);
    m_frameResources.setFrameTarget(texture);
    // U2.8 (nvrhi rule 1): the swapchain's texture is the frame's only; it is not kept
    // past the frame, whose end or park may let it go
    const auto clearFrameTarget =
      kdl::invoke_later{[&] { m_frameResources.setFrameTarget(nullptr); }};
    m_frameUploadBytes = 0;
    syncChunks();
    refreshImages();
    requestProbeImages();
    requestGridImages();
    // LP2.2b: the blit's target, the frame's presentation target alone
    auto* const targetFramebuffer =
      target ? imageFramebuffer(*target, imageIndex) : m_headlessFramebuffer.Get();
    auto hdrClear = std::optional<std::array<float, 4>>{};
    {
      const auto lock = std::scoped_lock{m_mutex};
      hdrClear = m_hdrClearForTesting;
      if (m_tonemapForTesting)
      {
        m_tonemap.setDefaults(*m_tonemapForTesting);
        m_tonemapForTesting.reset();
      }
    }
    m_frameHdrClear = hdrClear;

    // the frame's lit passes' counts (none in the HDR clear test mode)
    auto litStats = LitFrameStats{};
    // LP6.0 (C65): the CPU record's splits the render thread times (the lit passes' are
    // in litStats)
    auto uploadMs = 0.0;
    auto tonemapMs = 0.0;
    auto submitStart = recordStart;

    {
      auto recording = OpenCommandList{device, *m_commandList};
      auto& commandList = recording.list();
      if (timer)
      {
        commandList.beginTimerQuery(timer);
      }
      // D70, D78: what the session's start clears and the frame's uploads come before its
      // first pass
      m_frameResources.recordStartClears(commandList);
      // D68: the common samplers, once per session
      const auto samplers =
        m_samplers.common(device) | kdl::transform([](const auto*) { return true; })
        | kdl::if_error([&](const auto& e) { m_events.markFailed(e.msg); })
        | kdl::value_or(false);
      if (!samplers)
      {
        return FrameOutcome::Stopped;
      }
      // LP6.4a (P8.3, the director's reading (3)): at Start, before the first upload,
      // every format class the slots can make is probed once for the session
      if (!m_textureHeaps.formatClassesForTesting())
      {
        probeFormatClassesAtStart();
      }
      const auto uploadStart = FrameClock::now();
      const auto uploaded = recordUploads(commandList);
      uploadMs = millisecondsSince(uploadStart);
      if (!uploaded)
      {
        return FrameOutcome::Stopped;
      }
      runFrameHooks(commandList);
      // LP2.2a: the lit passes into hdrFBO (D77: a draw skipped for a texture without
      // levels or a chunk not placed yet is retried at once; a pending material or
      // evaluation waits for a table, a pending pipeline for the compile thread)
      auto hdrDrawn = false;
      if (hdrClear)
      {
        recordHdrClear(commandList, *hdrClear);
        hdrDrawn = true;
      }
      else
      {
        recordLitPasses(commandList, camera);
        litStats = m_passes.lastStats();
        hdrDrawn = litStats.prepassDraws + litStats.ambientDraws
                     + litStats.interactionDraws + litStats.stageDraws + litStats.fogDraws
                     + litStats.blendDraws
                   > 0;
      }
      runAfterPassHooks(commandList);
      // LP2.2b: the tonemap passes into the LDR target and its blit into the frame's
      // target, which replace LP0's clear of it; LP6.0 (C65): their CPU record time, the
      // debug view's (LP3.4) included
      const auto tonemapStart = FrameClock::now();
      recordTonemap(commandList, texture, targetFramebuffer, hdrDrawn);
      tonemapMs = millisecondsSince(tonemapStart);
      // LP3.4 (D158): a debug view's draw skipped for a pending pipeline keeps the frame
      // unconverged (the compile thread's onReady triggers the next)
      if (!hdrClear && m_debugView.load() != DebugView::None)
      {
        const auto& after = m_passes.lastStats();
        litStats.pendingPipelines = after.pendingPipelines;
        litStats.skippedDraws = after.skippedDraws;
        litStats.debugViewDraws = after.debugViewDraws;
      }
      // LP6.4b (C68 (b), D172): the frame's stamps are final: the evicted slots it drew
      // are asked again, and the budget lets go what no frame binds
      budgetPass();
      // D77: what the frame waits for, whether it is settled, whether it re-triggers
      converge(litStats, m_materials ? m_materials->table()->version : std::uint64_t{0});
      // C29: a settled frame's LDR target for the readbacks asked for
      recordLdrReadback(commandList);
      runAfterTonemapHooks(commandList);
      if (takeFault(DebugFault::ThrowWhileRecording))
      {
        throw std::runtime_error{"injected while recording"};
      }
      if (timer)
      {
        commandList.endTimerQuery(timer);
        m_timers.passTimed(slot, TimedPass::Frame);
      }
      // LP6.0 (C65): the submit's CPU time, from the close to the event query
      submitStart = FrameClock::now();
      recording.close();

      if (target)
      {
        m_device.vulkanDevice().queueSignalSemaphore(
          nvrhi::CommandQueue::Graphics, target->presentSemaphores[imageIndex], 0);
      }
      recording.submit();
    }
    if (takeFault(DebugFault::DeviceRemovedInSubmit))
    {
      m_device.messageCallback().message(
        nvrhi::MessageSeverity::Error, "Device Removed!");
    }
    if (takeFault(DebugFault::NvrhiError))
    {
      m_device.messageCallback().message(
        nvrhi::MessageSeverity::Error, "injected by debugInjectFault");
    }
    // NVRHI's submit reports a lost device only through the message callback (R5.19)
    if (m_events.lost())
    {
      throw vk::DeviceLostError{"NVRHI reported a lost device in executeCommandLists"};
    }
    device.setEventQuery(m_frameEvents[slot], nvrhi::CommandQueue::Graphics);
    m_frameEventSet[slot] = true;
    m_timers.frameSubmitted(slot);
    const auto submitMs = millisecondsSince(submitStart);
    const auto cpuRecordMs = millisecondsSince(recordStart);
    // C29: a readback's wait for the GPU is not the frame's record time
    finishLdrReadback(slot);
    const auto frame = m_frameIndex++;
    m_idleWorkPending = true;

    runDebugHook(DebugHook::BeforePresent);

    auto shown = true;
    if (target && stopOrParkRequested())
    {
      // a pass boundary: a stop or park asked for during the frame is honoured before
      // the present, which may wait for messages that the GUI thread answers only
      // between its waits (R5.18). The acquired image is not shown, so the generation
      // is replaced before anything is drawn again.
      shown = false;
      target->stale = true;
      requestRecreate(*target, "a stop or park came before the present");
    }
    else if (target)
    {
      const auto present = vk::PresentInfoKHR{}
                             .setWaitSemaphores(target->presentSemaphores[imageIndex])
                             .setSwapchains(target->swapchain)
                             .setImageIndices(imageIndex);
      const auto presentStart = FrameClock::now();
      const auto result = m_device.queue().presentKHR(&present);
      presentMs = millisecondsSince(presentStart);
      switch (result)
      {
      case vk::Result::eSuccess:
        break;
      case vk::Result::eSuboptimalKHR:
        requestRecreate(*target, "present returned SUBOPTIMAL", true);
        break;
      case vk::Result::eErrorOutOfDateKHR:
        shown = false;
        target->stale = true;
        requestRecreate(*target, "present returned OUT_OF_DATE");
        break;
      case vk::Result::eErrorSurfaceLostKHR:
        shown = false;
        target->stale = true;
        m_events.post(
          SessionEvent::Error,
          "The window's Vulkan surface was lost. Stop and Start the preview.");
        break;
      case vk::Result::eErrorDeviceLost:
        throw vk::DeviceLostError{"vkQueuePresentKHR"};
      default:
        m_events.markFailed(
          fmt::format("vkQueuePresentKHR returned {}", vk::to_string(result)));
        return FrameOutcome::Stopped;
      }
    }

    if (shown)
    {
      auto row = FrameLogRow{};
      row.session = m_events.generation();
      row.frameIndex = frame;
      row.cpuRecordMs = cpuRecordMs;
      row.acquireMs = acquireMs;
      row.presentMs = presentMs;
      row.presentMode = presentModeName(
        target ? std::optional{VkPresentModeKHR(target->presentMode)} : std::nullopt);
      row.swapchainGeneration = target ? target->number : 0;
      row.framesSkipped = m_framesSkipped;
      row.width = extent.width;
      row.height = extent.height;
      // C16, C30 (LP2.1): the allocations, the frame's uploads, the textures, the arenas
      row.allocCount = m_device.deviceMemoryAllocations();
      row.bytesUploaded = m_frameUploadBytes;
      row.texturesResident = m_images.resident();
      row.arenaBytes = m_arenas.capacityBytes();
      // U2.3: the textures and arenas the allocation headroom refused so far
      row.allocRefused =
        m_arenas.headroomRefusals() + m_textureUploads.headroomRefusals();
      // V2.2: the static interaction cull's count for the frame (DX.2)
      row.culledTriangles = litStats.culledTriangles;
      // LP4.1 (C16): the view's portal flow behind the frame's lists (the view's area
      // only while a tree exists)
      if (litStats.areas > 0)
      {
        row.viewArea = litStats.viewArea;
      }
      row.areasFlowed = litStats.areasFlowed;
      row.lightsInView = litStats.lightsInView;
      row.portalChains = litStats.portalChains;
      row.flowMs = litStats.flowMs;
      // LP4.2a (C34): the ambient pass's draws (its GPU time from the timers)
      row.ambientDraws = litStats.ambientDraws;
      // LP4.3 (D86, C36): set 9's writes and the surfaces on the light-grid path
      row.ambientWrites = litStats.ambientConstantWrites;
      row.gridSurfaces = litStats.gridSurfaces;
      // LPF.1 (C44): the frame's table's feed counter and override bytes, and the time
      // of the feed items whose change landed in this presented frame
      if (m_materials)
      {
        row.feedApplied = m_frameFeedApplied;
        row.overrideBytes = m_frameOverrideBytes;
      }
      row.feedMs = landFeeds(FrameClock::now());
      // C16 (LP2.2b): the lit passes' counts (LitFrameStats) and the tonemap passes'
      const auto& tonemap = m_tonemap.lastStats();
      // LP6.5a (C69): pass 3's draws among the lit passes'
      row.draws = litStats.prepassDraws + litStats.gbufferDraws + litStats.ambientDraws
                  + litStats.interactionDraws + litStats.stageDraws + litStats.fogDraws
                  + litStats.blendDraws + tonemap.draws;
      row.prepassDraws = litStats.prepassDraws;
      row.interactionDraws = litStats.interactionDraws;
      row.skippedDraws = litStats.skippedDraws;
      row.pendingPipelines = litStats.pendingPipelines + tonemap.pendingPipelines;
      row.lightsVisible = litStats.lightsVisible;
      row.lightsDrawn = litStats.lightsDrawn;
      row.lightsOverflowed = litStats.lightsOverflowed;
      // LP5.1 (C45): the math phase's lights and the ambientLight lights
      row.mathLights = litStats.mathLights;
      row.ambientLights = litStats.ambientLights;
      // LP5.2a (C46): the stage pass's draws and skipped stages (its GPU time from the
      // timers)
      row.stageDraws = litStats.stageDraws;
      row.stageSkipped = litStats.stageSkipped;
      // LP5.3 (C49): pass 9's fog and blend draws (its GPU time from the timers)
      row.fogDraws = litStats.fogDraws;
      row.blendDraws = litStats.blendDraws;
      // LP7.2 (C52): the scene's model totals when the frame was drawn, and its draws of
      // instances
      {
        const auto models = m_geometry && !m_arenaTestChunks
                              ? m_geometry->modelCounts()
                              : SceneGeometry::ModelCounts{};
        row.modelEntities = models.entities;
        row.modelInstances = models.instances;
        row.modelDraws = std::uint64_t(litStats.modelDraws);
        row.modelsPending = models.pending;
        row.modelsSkinned = models.skinned;
      }
      // LP7.3 (C54): the dress surfaces the frame drew, and the scene's func_decals
      // without a fragment surface and inlined copies when the frame was drawn
      {
        const auto dress = m_geometry && !m_arenaTestChunks
                             ? m_geometry->dressCounts()
                             : SceneGeometry::DressCounts{};
        row.dressSurfaces = std::uint64_t(litStats.dressSurfaces);
        row.dressUnmatched = dress.unmatched;
        row.inlinedSurfaces = dress.inlined;
      }
      // LP7.4 (C55): the scene's adoption totals when the frame was drawn
      {
        const auto adoption =
          m_geometry ? m_geometry->adoptionCounts() : SceneGeometry::AdoptionCounts{};
        row.adoptedIslands = adoption.adoptedIslands;
        row.adoptedTris = adoption.adoptedTris;
        row.unmatchedIslands = adoption.unmatchedIslands;
        row.curvedIslands = adoption.curvedIslands;
      }
      row.constantBytes = litStats.constantBytes + tonemap.constantBytes;
      row.volatileWrites = litStats.constantWrites + tonemap.constantWrites;
      row.volatileWriteBudget = litStats.constantWriteBudget;
      row.pushConstantBytes = litStats.pushConstantBytes + tonemap.pushConstantBytes;
      row.settled = m_convergence.settled();
      // LP3.1 (D157): pass 6's counts (its GPU time from the timers), set 11's writes,
      // and LP3.0's caster light-triangle requests behind the frame's lists (P3.14)
      row.shadowDraws = litStats.shadowDraws;
      row.tilesDrawn = litStats.tilesDrawn;
      row.tilesCleared = litStats.tilesVacated;
      row.tilesRelocated = litStats.tilesRelocated;
      row.tilesCached = litStats.tilesCached;
      row.tilesProvisional = litStats.tilesProvisional;
      row.lightsCasting = litStats.lightsCasting;
      row.lightsPlaced = litStats.lightsPlaced;
      row.lightsUnplaced = litStats.lightsUnplaced;
      row.perforatedSkipped = litStats.perforatedCastersSkipped;
      row.atlasBytesWritten = litStats.atlasBytesWritten;
      row.shadowCpuMs = litStats.shadowRecordMs;
      row.set11Writes = litStats.interactionSmWrites;
      // LP3.2 (D157 extended, SC709): the shadow-atlas interaction draws per list
      row.smDrawsLocal = litStats.smDrawsLocal;
      row.smDrawsGlobal = litStats.smDrawsGlobal;
      row.smDrawsTranslucent = litStats.smDrawsTranslucent;
      row.casterLights = litStats.casterLights;
      row.casterPairs = litStats.casterPairs;
      row.casterPairsResident = litStats.casterPairsResident;
      row.casterPairsPending = litStats.casterPairsPending;
      row.casters = litStats.casters;
      row.casterMs = litStats.casterMs;
      row.lightTriangleEntries = litStats.lightTriangleEntries;
      // LP3.4 (D155): the perforated casters' shadow draws
      row.perforatedDraws = litStats.perforatedShadowDraws;
      // LP6.0 (C65): the CPU record's split inside cpu_record_ms's span, and the rest of
      // the span (never below 0: the splits are disjoint intervals of it)
      row.listsMs = litStats.listsMs;
      row.prepassCpuMs = litStats.prepassRecordMs;
      row.ambientCpuMs = litStats.ambientRecordMs;
      row.interactionCpuMs = litStats.interactionRecordMs;
      row.stageCpuMs = litStats.stageRecordMs;
      row.fogCpuMs = litStats.fogRecordMs;
      row.tonemapCpuMs = tonemapMs;
      row.uploadMs = uploadMs;
      row.submitMs = submitMs;
      row.recordOtherMs = std::max(
        0.0,
        cpuRecordMs
          - (litStats.listsMs + litStats.prepassRecordMs + litStats.ambientRecordMs
             + litStats.shadowRecordMs + litStats.interactionRecordMs
             + litStats.stageRecordMs + litStats.fogRecordMs + tonemapMs + uploadMs
             + submitMs));
      // LP6.1 (C64): the draw records held at the frame's end and resolved this frame
      row.recordsHeld = litStats.recordsHeld;
      row.recordResolves = litStats.recordResolves;
      // LP6.2 (C65): lists_ms's parts beside flow_ms and caster_ms
      row.surfacesMs = litStats.surfacesMs;
      row.lightsMs = litStats.lightsMs;
      row.linksMs = litStats.linksMs;
      // LP6.2 (C66, D169): the caster sets made, the tiles drawn again for an image
      row.casterSetsBuilt = litStats.casterSetsBuilt;
      row.tilesImageRedrawn = litStats.tilesImageRedrawn;
      // LP6.4a (C68 (a)): the texture heaps, their ranges' bytes, the slots' textures
      // outside them
      row.textureHeaps = m_textureHeaps.heaps();
      row.heapBytes = m_textureHeaps.usedBytes();
      row.texturesDedicated = m_images.dedicatedTextures(&m_textureHeaps);
      // LP6.4b (C68 (b), (c)): the evictions so far, the slots at the floor now, the fed
      // images written in place so far
      row.texturesEvicted = m_images.evictions();
      row.texturesFloored = m_images.floored();
      row.feedsInPlace = m_textureUploads.feedsInPlace();
      // LP6.4b (C68 (d)): the arena bytes the compactions moved so far
      row.arenaMovedBytes = m_arenas.movedBytes();
      m_timers.frameShown(slot, row);
      ++m_framesPresented;
      // LP2.3 (D9): the panel's status line reads the frame's counts
      {
        auto status = SessionStatus{};
        status.frames = m_framesPresented;
        status.settled = m_convergence.settled();
        status.lightsVisible = litStats.lightsVisible;
        status.lightsDrawn = litStats.lightsDrawn;
        status.lightsOverflowed = litStats.lightsOverflowed;
        // LP3.4 (D158): the lights the atlas left unshadowed, by name
        status.lightsUnshadowed = litStats.lightsUnplaced;
        status.unshadowedLights = litStats.unplacedLights;
        status.draws = litStats.prepassDraws + litStats.gbufferDraws
                       + litStats.ambientDraws + litStats.interactionDraws
                       + litStats.stageDraws + litStats.fogDraws + litStats.blendDraws
                       + tonemap.draws;
        status.gbufferDraws = litStats.gbufferDraws;
        status.ssaoRan = litStats.ssaoRan;
        // LP6.5b (C71): the copy for the second stage call
        status.stageCopies = litStats.stageCopies;
        // LP6.5b (C72): bumpyenvironment2's draws and its stages listed
        status.bumpyEnvironment2Draws = litStats.bumpyEnvironment2Draws;
        status.bumpyEnvironment2Listed = litStats.bumpyEnvironment2Listed;
        status.skippedDraws = litStats.skippedDraws;
        status.pendingPipelines = litStats.pendingPipelines + tonemap.pendingPipelines;
        const auto account = accountGpuMemory(
          m_frameResources, m_arenas, m_images, m_passes, m_tonemap, m_textureHeaps);
        status.vramBytes = account.total();
        // LP6.4a (C68 (a), (e)): the account's lines and the heaps
        status.vramHeapBytes = m_textureHeaps.capacityBytes();
        status.vramDedicatedTextureBytes = account.textureBytes - status.vramHeapBytes;
        status.vramArenaBytes = account.arenaBytes;
        status.vramRenderTargetBytes = account.renderTargetBytes;
        status.vramReleasedBytes = account.releasedBytes;
        // LP6.4b (C68 (b)): the live figure, its used lines, the budget, the evictions
        // and the floors
        status.vramLiveBytes = account.live();
        status.vramArenaUsedBytes = account.arenaUsedBytes;
        status.vramHeapUsedBytes = account.heapUsedBytes;
        status.vramBudgetBytes = vramBudget();
        status.texturesEvicted = m_images.evictions();
        status.texturesFloored = m_images.floored();
        // LP6.4b (C68 (d)): the arenas per kind, the compactions run, the bytes moved
        status.vertexArenas = m_arenas.vertexArenas();
        status.indexArenas = m_arenas.indexArenas();
        status.compactions = m_arenas.compactions();
        status.compactionSubmissions = m_compactionSubmissions;
        status.arenaMovedBytes = m_arenas.movedBytes();
        status.heaps = textureHeapStatus();
        // D90 (LP4.1): the view's portal flow
        status.viewArea = litStats.viewArea;
        status.areasFlowed = litStats.areasFlowed;
        status.areas = litStats.areas;
        status.lightsInView = litStats.lightsInView;
        // D90 (LP4.3, K8, K29): the scene's bake
        if (m_geometry)
        {
          fillBakeStatus(status, *m_geometry);
        }
        const auto lock = std::scoped_lock{m_statusMutex};
        m_status = status;
      }
    }
    else
    {
      ++m_framesSkipped;
    }

    // D37: the binding-set cache's bound, never a set of this frame
    m_bindingSets.trim(frame);
    device.runGarbageCollection();
    m_lastFrameEnd = FrameClock::now();

    if (shown && !m_readyPosted)
    {
      m_readyPosted = true;
      m_events.post(
        SessionEvent::Ready,
        fmt::format("Lit preview running on {}", m_device.gpu().name));
    }
    return shown ? FrameOutcome::Presented : FrameOutcome::Skipped;
  }
  catch (const vk::DeviceLostError& e)
  {
    m_events.markLost(e.what());
    return FrameOutcome::Stopped;
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(e.what());
    return FrameOutcome::Stopped;
  }
}

void RenderThread::framesCompleted(const std::uint64_t completedFrames)
{
  if (completedFrames > m_completedFrames && !m_holdCompletion)
  {
    m_completedFrames = completedFrames;
  }
  // D35, D36: what only finished frames used is let go, and their freed arena ranges may
  // be handed out again
  m_frameResources.collectRetired(m_completedFrames);
  m_arenas.reclaim(m_completedFrames);
  m_images.collectRetired(m_completedFrames);
  // LP6.4a (C68 (a), D35): the heap ranges freed in those frames, after their textures
  m_textureHeaps.reclaim(m_completedFrames);
  // LP6.4b (the director's reading (7)): a heap left with no range is released
  m_textureHeaps.releaseEmpty();
}

void RenderThread::refreshImages()
{
  if (m_materials)
  {
    const auto table = m_materials->table();
    m_images.refresh(*table, m_frameIndex);
    // LPF.1 (C44): the feed counters of the table the frame refreshed with, and its
    // feed items
    m_frameFeedApplied = table->feedApplied;
    m_frameImageChanges = table->imageChanges;
    m_frameMaterialsRevision = table->materialsRevision;
    m_frameSizesRevision = table->sizesRevision;
    m_frameRegistry = table->registry;
    m_frameOverrideBytes = table->overrideBytes;
    takeFeedRecords(*table);
  }
  eraseChangedSets();
}

void RenderThread::takeFeedRecords(const MaterialTable& table)
{
  if (!table.feedRecords || table.feedApplied <= m_feedRecordsTaken)
  {
    return;
  }
  for (const auto& record : *table.feedRecords)
  {
    if (record.feedApplied <= m_feedRecordsTaken)
    {
      continue;
    }
    // the slots this thread has for the keys the item changed (refresh asked them again)
    auto feed = PendingFeed{record.handOver, {}, record.materialsRevision};
    for (const auto& key : record.keys)
    {
      if (const auto id = m_images.find(key))
      {
        feed.slots.push_back(*id);
      }
    }
    // a refusal, an override of a name no slot uses, a clear of nothing: nothing lands
    if (feed.slots.empty() && feed.materialsRevision == 0)
    {
      continue;
    }
    // the newest item of a slot wins: the older one is never shown as itself
    for (auto& pending : m_pendingFeeds)
    {
      std::erase_if(pending.slots, [&](const ImageSlotId id) {
        return std::ranges::find(feed.slots, id) != feed.slots.end();
      });
    }
    std::erase_if(m_pendingFeeds, [](const PendingFeed& pending) {
      return pending.slots.empty() && pending.materialsRevision == 0;
    });
    m_pendingFeeds.push_back(std::move(feed));
    // bounded as the service's records are: an item that never lands (no frame draws its
    // lists) does not pile up
    if (m_pendingFeeds.size() > MaterialTable::FeedRecordsKept)
    {
      m_pendingFeeds.erase(m_pendingFeeds.begin());
    }
  }
  m_feedRecordsTaken = table.feedApplied;
}

std::optional<double> RenderThread::landFeeds(const FrameClock::time_point now)
{
  if (m_pendingFeeds.empty())
  {
    return std::nullopt;
  }
  // C43: the scene's cells of a re-parsed material are built once the scene scanned its
  // revision and the batch it started then has applied (a test's chunks are no scene's)
  const auto cellsAt = [&](const std::uint64_t revision) {
    return !m_geometry || m_arenaTestChunks
           || (m_geometry->scannedMaterialsRevision() >= revision && !m_geometry->busy()
               && !m_geometry->waitsForMaterials());
  };
  auto largest = std::optional<double>{};
  std::erase_if(m_pendingFeeds, [&](const PendingFeed& feed) {
    // LP6.4b: an in-place write waiting has not landed
    const auto slotsLanded = std::ranges::all_of(feed.slots, [&](const ImageSlotId id) {
      const auto* slot = m_images.slot(id);
      return !slot || (slot->state == ImageSlots::State::Resident && !slot->writeInPlace)
             || slot->state == ImageSlots::State::Default;
    });
    const auto revisionLanded =
      feed.materialsRevision == 0
      || (m_frameListsRevision >= feed.materialsRevision && cellsAt(feed.materialsRevision));
    if (!slotsLanded || !revisionLanded)
    {
      return false;
    }
    const auto ms =
      std::chrono::duration<double, std::milli>(now - feed.handOver).count();
    largest = std::max(largest.value_or(ms), ms);
    return true;
  });
  return largest;
}

void RenderThread::requestProbeImages()
{
  if (!m_geometry || !m_materials)
  {
    return;
  }
  auto table = m_geometry->probeTable();
  if (table == m_probeImagesTable)
  {
    return;
  }
  // before the frame's uploads, so that an answered slot is written in this frame (D78)
  for (const auto& probe : table->records)
  {
    m_images.request(probe.images[1]);
    m_images.request(probe.images[2]);
  }
  m_probeImagesTable = std::move(table);
}

void RenderThread::requestGridImages()
{
  if (!m_geometry || !m_materials)
  {
    return;
  }
  auto table = m_geometry->gridTable();
  if (table == m_gridImagesTable)
  {
    return;
  }
  // LoadLightGridImages (RenderWorld_lightgrid.cpp:415-447): every area's atlas, also an
  // area without points; before the frame's uploads (D78)
  for (const auto& area : table->areas)
  {
    m_images.request(area.atlas);
  }
  m_gridImagesTable = std::move(table);
}

void RenderThread::eraseChangedSets()
{
  // LP6.1 (C64, P8.1): the draw records resolved with a slot whose adopted usage changed
  // (their program can change) or whose version was bumped are released first, so that no
  // record holds a set the cache erases; C28: then exactly the sets that hold a bumped
  // slot
  for (const auto slot : m_images.takeUsageChangedSlots())
  {
    m_passes.recordsSlotChanged(slot);
  }
  for (const auto slot : m_images.takeChangedSlots())
  {
    m_passes.recordsSlotChanged(slot);
    m_bindingSets.slotChanged(slot);
  }
}

void RenderThread::arrivalsTick()
{
  if (!m_materials)
  {
    return;
  }
  const auto table = m_materials->table();
  // LP5.G addendum (D18): a slot's file changed since the table the last frame refreshed
  // with (the poll's re-probe: a texture saved again, imageChanges) is a frame trigger at
  // once, whatever the scene: that frame's refreshImages asks the slot again, which keeps
  // its texture bound (Replacing, pending: unconverged), and the new image's publication
  // is an arrival below. Only a key the render side has a slot for, so a change of an
  // image nothing draws draws nothing. H.1: the drop's tableChanged wakes the loop, and
  // this reads the newest table on every idle pass until a frame has refreshed with it
  if (table->imageChanges != m_frameImageChanges)
  {
    auto changed = false;
    table->changedImages.forEach(
      [&](const MaterialImageKey& key, const std::uint64_t change) {
        changed = changed || (change > m_frameImageChanges && m_images.find(key));
      });
    if (changed)
    {
      const auto lock = std::scoped_lock{m_mutex};
      m_pacer.trigger();
    }
  }
  if (m_images.pending() == 0)
  {
    return;
  }
  if (table->version == m_arrivalsCheckedVersion)
  {
    return;
  }
  m_arrivalsCheckedVersion = table->version;
  if (!m_images.anyArrivals(*table))
  {
    return;
  }
  // D77: a texture arrival is a frame trigger, at most every TextureArrivalInterval
  const auto now = FrameClock::now();
  const auto lock = std::scoped_lock{m_mutex};
  arrivalTrigger(now);
}

bool RenderThread::waitForFrameEvent(const std::size_t slot)
{
  // NVRHI's waitEventQuery would wait without a bound. U2.7: a lost device ends the wait
  // (D38), and a GPU that does not answer within GpuAnswerBound is said once
  const auto start = FrameClock::now();
  auto said = false;
  while (!m_device.device().pollEventQuery(m_frameEvents[slot]))
  {
    if (m_events.lost())
    {
      return false;
    }
    if (!said && FrameClock::now() - start >= GpuAnswerBound)
    {
      said = true;
      m_events.post(
        SessionEvent::Warning,
        fmt::format(
          "The GPU does not answer: a frame has waited {} s for it",
          std::chrono::duration_cast<std::chrono::seconds>(GpuAnswerBound).count()));
    }
    auto lock = std::unique_lock{m_mutex};
    if (m_wake.wait_for(
          lock, EventPollInterval, [&] { return m_stopRequested || m_parkRequested; }))
    {
      return false;
    }
  }
  return true;
}

void RenderThread::runDebugHook(const DebugHook point)
{
  auto hook = std::function<void()>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hook = m_debugHooks[std::size_t(point)];
  }
  if (hook)
  {
    hook();
  }
}

void RenderThread::requestRecreate(
  SwapchainGeneration& generation, const std::string_view reason, const bool suboptimal)
{
  if (generation.recreateRequested || (suboptimal && generation.suboptimalIgnored))
  {
    return;
  }

  if (suboptimal)
  {
    // A new generation of the same size that is SUBOPTIMAL again would be replaced on
    // every frame, so it presents on as it is until the size, DPR or screen changes
    // (review L2.6, L3.14).
    const auto lock = std::scoped_lock{m_mutex};
    if (m_suboptimalExtent == generation.extent)
    {
      generation.suboptimalIgnored = true;
      return;
    }
    m_suboptimalExtent = generation.extent;
  }

  generation.recreateRequested = true;
  m_recreateRequestedFor = generation.number;
  m_events.post(
    SessionEvent::SwapchainOutOfDate,
    fmt::format("Swapchain generation {}: {}", generation.number, reason));
}

void RenderThread::idleTick()
{
  // §3.1, §3.10: garbage collection and timer polling about a second after the last frame
  try
  {
    auto& device = m_device.device();
    auto allResolved = true;
    for (std::size_t i = 0; i < FramesInFlight; ++i)
    {
      const auto slot = std::size_t((m_frameIndex + i) % FramesInFlight);
      if (!m_timers.pending(slot))
      {
        continue;
      }
      // a slot's query is read only once its frame has finished
      if (!device.pollEventQuery(m_frameEvents[slot]))
      {
        allResolved = false;
        break;
      }
      m_timers.resolve(device, slot);
    }
    // every submitted frame has finished once each set event query has
    auto allFinished = true;
    for (std::size_t slot = 0; slot < FramesInFlight; ++slot)
    {
      allFinished =
        allFinished
        && (!m_frameEventSet[slot] || device.pollEventQuery(m_frameEvents[slot]));
    }
    if (allFinished)
    {
      framesCompleted(m_frameIndex);
    }
    device.runGarbageCollection();
    if (allResolved && allFinished)
    {
      m_idleWorkPending = false;
    }
    else
    {
      m_lastFrameEnd = FrameClock::now();
    }
  }
  catch (const vk::DeviceLostError& e)
  {
    m_events.markLost(e.what());
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(e.what());
  }
}

void RenderThread::appendSessionRow(const FrameLogEvent event)
{
  auto row = FrameLogRow{};
  row.session = m_events.generation();
  row.event = event;
  if (const auto vram = m_device.vram())
  {
    row.vramUsedMb = double(vram->usedBytes) / (1024.0 * 1024.0);
    row.vramBudgetMb = double(vram->budgetBytes) / (1024.0 * 1024.0);
  }
  // C30, D72: the allocations at the start and at the stop, before step 2 releases what
  // the render thread holds; the stop row also says what it holds
  row.allocCount = m_device.deviceMemoryAllocations();
  if (event == FrameLogEvent::Stop)
  {
    row.texturesResident = m_images.resident();
    row.arenaBytes = m_arenas.capacityBytes();
    // LP7.0 (C54): the adopted bake's tree, taken before the scene went
    row.procBytes = m_stopProcBytes;
    // LP6.4a (C68 (a)): what the heaps hold at the stop, before step 2 releases them
    row.textureHeaps = m_textureHeaps.heaps();
    row.heapBytes = m_textureHeaps.usedBytes();
    row.texturesDedicated = m_images.dedicatedTextures(&m_textureHeaps);
  }
  m_timers.append(row);
}

bool RenderThread::waitForIdle(const std::string_view when)
{
  if (m_device.device().waitForIdle())
  {
    return true;
  }
  m_events.markLost(fmt::format("waitForIdle {}", when));
  return false;
}

void RenderThread::parkGpuWork()
{
  // the render thread's half of the surface handshake (§3.1, R5.18, D49)
  try
  {
    // also on a lost device, whose wait returns at once: the GUI thread destroys the
    // swapchain next, which no submitted command may still use
    waitForIdle("while parking");
    if (!m_events.lost())
    {
      m_timers.resolveAll(m_device.device(), m_frameIndex);
      framesCompleted(m_frameIndex);
    }
    // U2.8 (nvrhi rule 1): no raw pointer to a swapchain texture outlives its wrappers
    m_frameResources.setFrameTarget(nullptr);
    if (m_swapchain)
    {
      // the generation is replaced before anything is drawn again (review V.1)
      releaseSwapchainWrappers(*m_swapchain);
      m_swapchain->stale = true;
    }
    if (!m_events.lost())
    {
      m_device.device().runGarbageCollection();
    }
    m_idleWorkPending = false;
    runDebugHook(DebugHook::WhileParking);
  }
  catch (const vk::DeviceLostError& e)
  {
    m_events.markLost(e.what());
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(e.what());
  }
}

SceneWaitState RenderThread::sceneWaitState() const
{
  return {
    m_geometry->appliedSequence(),
    m_geometry->busy(),
    m_geometry->waitsForBake(),
    m_geometry->waitsForEntityDefs(),
    m_geometry->waitsForMaterials(),
    m_geometry->probesInFlight(),
    m_geometry->probeTable()->version,
    m_geometry->scannedMaterialsRevision(),
    m_geometry->scannedSizesRevision(),
    m_geometry->waitsForModels(),
    m_geometry->modelCounts().instances};
}

bool RenderThread::geometryTick()
{
  if (!m_geometry)
  {
    return false;
  }
  const auto applied = m_geometry->collect();
  // LP6.3 (C67 (b)): world cells of the scene's first batch applied before it is whole:
  // their chunks are synced and drawn, the batch is not applied (batchesApplied, the
  // applied sequence and D77's geometryInFlight wait for the whole batch)
  const auto early = m_geometry->takeEarlyApplied();
  // DX.4: a frame that waited for the scene (a batch, the bake, an entityDef, a light
  // rebuilt; LP4.2b: a probe build) is drawn again once the scene's state moves, also
  // when nothing was uploaded
  if (
    (m_convergence.geometryInFlight || m_convergence.entityDefsPending
     || m_convergence.lightsProvisional || m_convergence.provisionalChunks
     || m_convergence.probesInFlight || m_convergence.modelsPending)
    && m_sceneWaitSeen && *m_sceneWaitSeen != sceneWaitState())
  {
    m_sceneWaitSeen = sceneWaitState();
    const auto lock = std::scoped_lock{m_mutex};
    m_pacer.trigger();
  }
  if (applied || early)
  {
    for (auto& warning : m_geometry->takeWarnings())
    {
      m_events.post(SessionEvent::Warning, std::move(warning));
    }
    // D36: the applied chunks' uploads; a frame records them (D77: new chunks are a
    // snapshot's arrival)
    ++m_chunksVersion;
    const auto uploads = syncChunks();
    const auto lock = std::scoped_lock{m_mutex};
    m_chunkCount = m_geometry->chunks().size();
    m_firstBatch = m_geometry->firstBatch();
    if (applied)
    {
      m_appliedSequence = m_geometry->appliedSequence();
      ++m_batchesApplied;
    }
    // a snapshot waiting in the inbox is taken at the next frame
    if (m_snapshotInbox || uploads)
    {
      m_pacer.trigger();
    }
  }
  // the probe build's end is polled for as a batch's is
  return m_geometry->busy() || m_geometry->probesBuilding();
}

void RenderThread::finish() noexcept
{
  // at exit: the outstanding GPU times, then the frame log (C16: flushed at stop)
  try
  {
    // also on a lost device: teardown step 2 releases textures that only clears used,
    // and clears hold no reference to them (NVRHI vulkan-texture.cpp:609-698)
    if (waitForIdle("at stop") && !m_events.lost())
    {
      m_timers.resolveAll(m_device.device(), m_frameIndex);
      framesCompleted(m_frameIndex);
    }
  }
  catch (const vk::DeviceLostError& e)
  {
    m_events.markLost(e.what());
  }
  catch (const std::exception& e)
  {
    m_events.markFailed(e.what());
  }

  try
  {
    // LP7.0 (C54): the stop row's proc_bytes, while the scene still holds its bake
    if (m_geometry)
    {
      const auto bake = m_geometry->adoptedBake();
      m_stopProcBytes = bake ? bake->procBytes : 0u;
    }
    // the chunk jobs in flight are waited for (R5.22) before the pool stops
    m_geometry.reset();

    // rows whose GPU time never arrived keep an empty field
    m_timers.flushUnresolved(m_frameIndex);
    appendSessionRow(FrameLogEvent::Stop);
    m_frameLogError = m_timers.closeLog();
  }
  catch (const std::exception& e)
  {
    try
    {
      m_frameLogError = fmt::format("Cannot write the frame log: {}", e.what());
    }
    catch (...)
    {
      m_frameLogError = std::string{};
    }
  }
}

} // namespace tb::lit
