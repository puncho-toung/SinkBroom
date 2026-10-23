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
#include "Swapchain.h"
#include "lit/EngineTables.h"
#include "lit/SceneGeometry.h"
#include "lit/Snapshot.h"
#include "lit/SwapchainRules.h"

#include "kd/invoke.h"
#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
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

bool FrameConvergence::settled() const
{
  return !pipelinesPending && !materialsPending && !drawsSkipped && !uploadsLeft
         && !imagesPending && !provisionalChunks && !lutPending && !entityDefsPending
         && !lightsProvisional && !geometryInFlight && !drawsOverflowed
         && !lightTrianglesPending;
}

bool FrameConvergence::retriggers() const
{
  // what the next frame converges by itself: the uploads it records and the draws they
  // let it make
  return drawsSkipped || uploadsLeft;
}

std::uint64_t GpuMemoryAccount::total() const
{
  return arenaBytes + textureBytes + renderTargetBytes + releasedBytes
         + volatileBufferBytes + uploadPoolBytes;
}

GpuMemoryAccount accountGpuMemory(
  const FrameResources& frame,
  const GeometryArenas& arenas,
  const ImageSlots& images,
  const LitPasses& passes,
  const TonemapPasses& tonemap)
{
  auto account = GpuMemoryAccount{};
  account.arenaBytes = arenas.capacityBytes();
  account.textureBytes = images.textureBytes();
  account.renderTargetBytes = frame.renderTargetBytes() + frame.bufferBytes();
  account.releasedBytes = frame.retiredBytes() + images.retiredBytes();
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
  m_arenas.setDevice(device, ArenaBytes);
  m_arenas.setAllocationRoom([this] { return allocationRoom(); });
  m_textureUploads.setD24s8Supported(m_device.gpu().d24s8Supported);
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
  return m_frameResources.create(
           device,
           m_device.gpu().d24s8Supported,
           vk::Extent2D{std::max(extent.width, 1u), std::max(extent.height, 1u)})
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
  PipelineCompiler* compiler)
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
  m_geometry = std::make_unique<SceneGeometry>(m_pool, m_materials, std::move(lights));
  m_thread = std::thread{[this] { main(); }};
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
  }
}

void RenderThread::releaseFrameResources()
{
  // §3.1 step 2: every handle the registry and the arenas hold
  m_passes.releaseAll();
  m_tonemap.releaseAll();
  m_ldrStaging = nullptr;
  m_ldrReadbacksInFrame.clear();
  m_frameResources.releaseAll();
  m_arenas.releaseAll();
  m_pipelines.clear();
  m_bindingSets.clear();
  m_images.releaseAll();
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

SessionStatus RenderThread::status() const
{
  const auto lock = std::scoped_lock{m_statusMutex};
  return m_status;
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
  convergence.geometryInFlight =
    scene && (m_geometry->busy() || m_geometry->waitsForBake() || m_snapshotInboxFull);
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
  convergence.lutPending = tonemap.lutPending;
  convergence.passesDisabled =
    lit.prepassDisabled || lit.interactionsDisabled || tonemap.disabled;
  m_convergence = convergence;
  m_lastSettled = convergence.settled();
  m_sceneWaitSeen = m_geometry ? std::optional{sceneWaitState()} : std::nullopt;
  if (m_lastSettled)
  {
    // U2.6: what a settled frame holds against the device's budget
    checkVramBudget();
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

void RenderThread::recordLdrReadback(nvrhi::ICommandList& commandList)
{
  // C29, P2.25: only a settled frame whose tonemap passes ran is copied
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
  const auto& desc = ldr->texture->getDesc();
  m_ldrStaging = m_device.device().createStagingTexture(
    nvrhi::TextureDesc{}
      .setWidth(desc.width)
      .setHeight(desc.height)
      .setFormat(desc.format)
      .setDebugName("Lit preview LDR readback"),
    nvrhi::CpuAccessMode::Read);
  if (!m_ldrStaging)
  {
    m_ldrReadbacksInFrame.clear();
    return;
  }
  commandList.copyTexture(
    m_ldrStaging, nvrhi::TextureSlice{}, ldr->texture, nvrhi::TextureSlice{});
}

void RenderThread::finishLdrReadback(const std::size_t slot)
{
  if (!m_ldrStaging)
  {
    return;
  }
  auto staging = std::exchange(m_ldrStaging, nullptr);
  auto requests = std::exchange(m_ldrReadbacksInFrame, {});
  // the frame's copy is read once the frame has finished; a stop or a park drops it
  if (!waitForFrameEvent(slot))
  {
    return;
  }
  auto& device = m_device.device();
  const auto& desc = staging->getDesc();
  auto image = LdrImage{};
  image.width = desc.width;
  image.height = desc.height;
  auto pitch = std::size_t{0};
  const auto* bytes = static_cast<const std::uint8_t*>(device.mapStagingTexture(
    staging, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &pitch));
  if (!bytes)
  {
    return;
  }
  const auto rowBytes = std::size_t(desc.width) * 4;
  image.rgba.resize(rowBytes * desc.height);
  for (std::uint32_t y = 0; y < desc.height; ++y)
  {
    std::copy_n(bytes + y * pitch, rowBytes, image.rgba.data() + y * rowBytes);
  }
  device.unmapStagingTexture(staging);
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
  }
  return m_arenas.pending();
}

bool RenderThread::recordUploads(nvrhi::ICommandList& commandList)
{
  // §3.10, D78: before the frame's first pass, within the frame's cap: the chunks first,
  // then the textures, low mips first (P2.14c)
  const auto cap = m_uploadCap.load();
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
  const auto texturesLeft = m_textureUploads.record(
    m_device.device(), commandList, m_images, cap, m_frameUploadBytes, [this] {
      return allocationRoom();
    });
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
        "stand-ins",
        m_device.deviceMemoryAllocations(),
        m_allocationLimitForTesting.load() != 0
          ? m_allocationLimitForTesting.load()
          : m_device.gpu().maxMemoryAllocationCount));
  }
  // D77: what is left for the next frame makes this one unconverged
  m_uploadsLeft = m_arenas.pending() || texturesLeft;
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

void RenderThread::setVramBudget(const std::uint64_t bytes)
{
  m_vramBudgetForTesting = bytes;
}

std::vector<PipelineKey> RenderThread::startPipelineKeys() const
{
  const auto depth = m_frameResources.depthFormat();
  auto keys = litStartPipelineKeys(depth);
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
               m_bindingSets.renderTargetsChanged(m_renderTargetGeneration);
             }
             return true;
           })
         | kdl::if_error([&](const auto& e) { m_events.markFailed(e.msg); })
         | kdl::value_or(false);
}

void RenderThread::checkVramBudget()
{
  // §3.10, C11 (U2.6): the account and the device's budget at the session's first frame
  // and at every settled frame (the caller's); LP6 evicts against it, until then a
  // session past it says so once
  if (m_budgetWarned)
  {
    return;
  }
  m_budgetChecked = true;
  const auto set = m_vramBudgetForTesting.load();
  const auto vram = set == 0 ? m_device.vram() : std::nullopt;
  const auto budget = set != 0 ? std::optional{set}
                      : vram   ? std::optional{vram->budgetBytes}
                               : std::nullopt;
  const auto account =
    accountGpuMemory(m_frameResources, m_arenas, m_images, m_passes, m_tonemap);
  if (budget && account.total() > *budget)
  {
    m_budgetWarned = true;
    m_events.post(
      SessionEvent::Warning,
      fmt::format(
        "The lit preview's GPU memory ({} MB) passes the device's budget ({} MB)",
        account.total() / (1024 * 1024),
        *budget / (1024 * 1024)));
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
    m_frameHdrClear};
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
  if (m_geometry && !m_arenaTestChunks)
  {
    inputs.entities = &m_geometry->entityStates();
  }
  if (m_geometry)
  {
    inputs.lights = m_geometry->lightTable();
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
  const auto stats = m_passes.record(inputs);
  if (stats.prepassTimed)
  {
    m_timers.passTimed(slot, TimedPass::Prepass);
  }
  if (stats.interactionsTimed)
  {
    m_timers.passTimed(slot, TimedPass::Interactions);
  }
  // C28: the slots asked for by the passes' draws lose no set they made
  eraseChangedSets();
  // D77: what a table publication resolves waits for one (litTableTick)
  m_litWaitsForTable = stats.waitsForTable;
  m_litTableVersion = tableVersion;
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
      m_pacer.frameStarted(now);
      const auto frameCamera = m_camera;
      // S31, B0.3: the frame takes the snapshot offered once the last one's chunks are
      // applied; ReadyForSnapshot then says the inbox is free again
      auto snapshot = std::shared_ptr<const Snapshot>{};
      auto previous = std::shared_ptr<const Snapshot>{};
      if (m_snapshotInbox && (!m_geometry || m_geometry->readyForSnapshot()))
      {
        snapshot = std::move(m_snapshotInbox);
        previous = std::exchange(m_snapshot, snapshot);
        m_snapshotInboxFull = false;
        ++m_snapshotsTaken;
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
      continue;
    }

    auto deadline = next.value_or(FrameClock::time_point::max());

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
      if (!samplers || !recordUploads(commandList))
      {
        return FrameOutcome::Stopped;
      }
      if (!m_budgetChecked)
      {
        checkVramBudget();
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
        hdrDrawn = litStats.prepassDraws + litStats.interactionDraws > 0;
      }
      runAfterPassHooks(commandList);
      // LP2.2b: the tonemap passes into the LDR target and its blit into the frame's
      // target, which replace LP0's clear of it
      recordTonemap(commandList, texture, targetFramebuffer, hdrDrawn);
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
      // C16 (LP2.2b): the lit passes' counts (LitFrameStats) and the tonemap passes'
      const auto& tonemap = m_tonemap.lastStats();
      row.draws = litStats.prepassDraws + litStats.interactionDraws + tonemap.draws;
      row.prepassDraws = litStats.prepassDraws;
      row.interactionDraws = litStats.interactionDraws;
      row.skippedDraws = litStats.skippedDraws;
      row.pendingPipelines = litStats.pendingPipelines + tonemap.pendingPipelines;
      row.lightsVisible = litStats.lightsVisible;
      row.lightsDrawn = litStats.lightsDrawn;
      row.lightsOverflowed = litStats.lightsOverflowed;
      row.constantBytes = litStats.constantBytes + tonemap.constantBytes;
      row.volatileWrites = litStats.constantWrites + tonemap.constantWrites;
      row.volatileWriteBudget = litStats.constantWriteBudget;
      row.pushConstantBytes = litStats.pushConstantBytes + tonemap.pushConstantBytes;
      row.settled = m_convergence.settled();
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
        status.draws = litStats.prepassDraws + litStats.interactionDraws + tonemap.draws;
        status.skippedDraws = litStats.skippedDraws;
        status.pendingPipelines = litStats.pendingPipelines + tonemap.pendingPipelines;
        status.vramBytes =
          accountGpuMemory(m_frameResources, m_arenas, m_images, m_passes, m_tonemap)
            .total();
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
}

void RenderThread::refreshImages()
{
  if (m_materials)
  {
    m_images.refresh(*m_materials->table(), m_frameIndex);
  }
  eraseChangedSets();
}

void RenderThread::eraseChangedSets()
{
  // C28: exactly the sets that hold a bumped slot
  for (const auto slot : m_images.takeChangedSlots())
  {
    m_bindingSets.slotChanged(slot);
  }
}

void RenderThread::arrivalsTick()
{
  if (!m_materials || m_images.pending() == 0)
  {
    return;
  }
  const auto table = m_materials->table();
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

std::tuple<std::uint64_t, bool, bool, bool, bool> RenderThread::sceneWaitState() const
{
  return {
    m_geometry->appliedSequence(),
    m_geometry->busy(),
    m_geometry->waitsForBake(),
    m_geometry->waitsForEntityDefs(),
    m_geometry->waitsForMaterials()};
}

bool RenderThread::geometryTick()
{
  if (!m_geometry)
  {
    return false;
  }
  const auto applied = m_geometry->collect();
  // DX.4: a frame that waited for the scene (a batch, the bake, an entityDef, a light
  // rebuilt) is drawn again once the scene's state moves, also when nothing was uploaded
  if (
    (m_convergence.geometryInFlight || m_convergence.entityDefsPending
     || m_convergence.lightsProvisional || m_convergence.provisionalChunks)
    && m_sceneWaitSeen && *m_sceneWaitSeen != sceneWaitState())
  {
    m_sceneWaitSeen = sceneWaitState();
    const auto lock = std::scoped_lock{m_mutex};
    m_pacer.trigger();
  }
  if (applied)
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
    m_appliedSequence = m_geometry->appliedSequence();
    m_chunkCount = m_geometry->chunks().size();
    ++m_batchesApplied;
    // a snapshot waiting in the inbox is taken at the next frame
    if (m_snapshotInbox || uploads)
    {
      m_pacer.trigger();
    }
  }
  return m_geometry->busy();
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
