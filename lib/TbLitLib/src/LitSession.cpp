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

#include "lit/LitSession.h"

#include "GpuDevice.h"
#include "LiveSession.h"
#include "PipelineCompiler.h"
#include "RenderThread.h"
#include "SessionEvents.h"
#include "StackThread.h"
#include "Swapchain.h"
#include "lit/CpuPool.h"
#include "lit/EngineBuiltins.h"
#include "lit/GridTable.h"
#include "lit/LightRecord.h"
#include "lit/ProbeRecord.h"
#include "lit/SceneGeometry.h"
#include "lit/Snapshot.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <cassert>
#include <future>
#include <utility>

namespace tb::lit
{

LitSession::LitSession(
  SessionOptions options,
  EventSink eventSink,
  const std::uint64_t generation,
  MaterialService* materials)
  : m_options{std::move(options)}
  , m_events{std::make_unique<SessionEvents>(std::move(eventSink), generation)}
  , m_materials{materials}
  , m_lightTables{std::make_shared<LightTablePublication>()}
  , m_probeTables{std::make_shared<ProbeTablePublication>()}
  , m_gridTables{std::make_shared<GridTablePublication>()}
{
}

Result<std::unique_ptr<LitSession>> LitSession::create(
  const InstanceHandles& instance,
  const VkSurfaceKHR surface,
  const std::uint32_t width,
  const std::uint32_t height,
  SessionOptions options,
  EventSink eventSink,
  const std::uint64_t generation,
  MaterialService* materials)
{
  if (instance.instance == VK_NULL_HANDLE || instance.getInstanceProcAddr == nullptr)
  {
    return Error{"No Vulkan instance"};
  }
  if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices == nullptr)
  {
    return Error{"The Vulkan dispatcher is not initialised for the instance"};
  }

  if (!claimLiveSession())
  {
    return Error{"Another lit preview session runs in this process"};
  }

  try
  {
    auto session = std::unique_ptr<LitSession>{};
    try
    {
      session.reset(
        new LitSession{std::move(options), std::move(eventSink), generation, materials});
    }
    catch (...)
    {
      releaseLiveSession();
      throw;
    }
    session->m_claimedLiveSession = true;
    return session->start(instance, surface, width, height)
           | kdl::transform([&]() { return std::move(session); })
           | kdl::or_else([&](Error e) -> Result<std::unique_ptr<LitSession>> {
               // whatever exists is torn down (§3.1), and what that found goes with the
               // error, since no report outlives this session (review V.4)
               session->releaseGpuObjects();
               for (const auto& message : session->m_report.messages)
               {
                 e.msg += " " + message;
               }
               return e;
             });
  }
  catch (const std::exception& e)
  {
    return Error{fmt::format("Cannot start the lit preview: {}", e.what())};
  }
}

Result<void> LitSession::start(
  const InstanceHandles& instance,
  const VkSurfaceKHR surface,
  const std::uint32_t width,
  const std::uint32_t height)
{
  // steps 4-6
  return GpuDevice::create(
           instance, vk::SurfaceKHR{surface}, m_options.validation, *m_events)
         | kdl::and_then([&](std::unique_ptr<GpuDevice> device) {
             m_device = std::move(device);
             m_report.vramAtStart = m_device->vram();
             m_renderThread =
               std::make_unique<RenderThread>(*m_device, *m_events, m_options);
             // step 7
             return surface != VK_NULL_HANDLE ? createSwapchain(surface, width, height)
                                              : Result<void>{kdl::void_success};
           })
         | kdl::and_then([&]() {
             // step 8, LP0's part: the frame resources, then the render thread
             const auto headlessExtent =
               surface == VK_NULL_HANDLE
                 ? std::optional{vk::Extent2D{std::max(width, 1u), std::max(height, 1u)}}
                 : std::nullopt;
             return m_renderThread->createFrameResources(headlessExtent);
           })
         // step 8: the CPU pool before the render thread, which may use it from its
         // first frame (S3)
         | kdl::and_then([&]() { return CpuPool::create(); })
         | kdl::and_then([&](std::unique_ptr<CpuPool> pool) {
             m_pool = std::move(pool);
             // step 8 (D71): the compile thread; a pipeline it makes triggers a frame
             // (D77)
             m_compiler = std::make_unique<PipelineCompiler>(
               *m_device, *m_events, m_options.gamePath);
             auto* renderThread = m_renderThread.get();
             return m_compiler->start([renderThread] { renderThread->requestFrame(); });
           })
         | kdl::transform([&]() {
             // DX.5: the pipelines every lit scene draws with, made while the scene
             // loads; not for a session whose game path disables every program (T0.4)
             if (m_options.requestStartPipelines && m_options.gamePath.is_absolute())
             {
               m_compiler->request(
                 m_renderThread->startPipelineKeys(),
                 {TonemapComputePrograms.begin(), TonemapComputePrograms.end()});
             }
           })
         | kdl::transform([&]() {
             // B0.10: the render thread gets the pool, the service and (LP2.1) the
             // compile thread at start; each outlives it
             m_renderThread->start(
               m_pool.get(),
               m_materials,
               m_lightTables,
               m_compiler.get(),
               m_probeTables,
               m_gridTables);
           });
}

LitSession::~LitSession()
{
  destroyVkDevice();
  if (m_claimedLiveSession)
  {
    releaseLiveSession();
  }
}

std::uint64_t LitSession::generation() const
{
  return m_events->generation();
}

const std::string& LitSession::deviceName() const
{
  return m_device->gpu().name;
}

DeviceFacts LitSession::deviceFacts() const
{
  const auto& gpu = m_device->gpu();
  return DeviceFacts{
    gpu.maxPushConstantsSize,
    gpu.vendorID,
    gpu.maxMemoryAllocationCount,
    m_device->pushConstantLimit(),
    gpu.d24s8Supported,
  };
}

std::int64_t LitSession::deviceMemoryAllocations() const
{
  return m_device ? m_device->deviceMemoryAllocations() : 0;
}

VkPhysicalDevice LitSession::physicalDevice() const
{
  return m_device->gpu().physicalDevice;
}

std::uint32_t LitSession::queueFamily() const
{
  return m_device->gpu().queueFamily;
}

void LitSession::setCamera(const CameraState& camera)
{
  m_renderThread->setCamera(camera);
}

void LitSession::setExposed(const bool exposed)
{
  m_renderThread->setExposed(exposed);
}

void LitSession::requestFrame()
{
  m_renderThread->requestFrame();
}

bool LitSession::offerSnapshot(std::shared_ptr<const Snapshot> snapshot)
{
  // D53: a snapshot made for another session is dropped
  if (!snapshot || snapshot->generation != generation() || m_renderThreadStopped)
  {
    return false;
  }
  return m_renderThread->offerSnapshot(std::move(snapshot));
}

bool LitSession::snapshotSlotFree() const
{
  return !m_renderThreadStopped && m_renderThread->snapshotSlotFree();
}

std::shared_ptr<const LightTable> LitSession::lightTable() const
{
  return m_lightTables->table();
}

std::shared_ptr<const ProbeTable> LitSession::probeTable() const
{
  return m_probeTables->table();
}

std::shared_ptr<const GridTable> LitSession::gridTable() const
{
  return m_gridTables->table();
}

void LitSession::tableChanged()
{
  if (!m_renderThreadStopped && m_renderThread)
  {
    m_renderThread->tableChanged();
  }
}

CpuPool* LitSession::cpuPoolForTesting() const
{
  return m_pool && !m_pool->stopped() ? m_pool.get() : nullptr;
}

MaterialService* LitSession::materialServiceForTesting() const
{
  return m_materials;
}

bool LitSession::park(const std::chrono::milliseconds timeout)
{
  return m_renderThread->park(timeout);
}

bool LitSession::isParked() const
{
  return m_renderThread->isParked();
}

void LitSession::destroySwapchain()
{
  if (!m_renderThread->hasSwapchain())
  {
    return;
  }
  if (!m_renderThread->mayReplaceSwapchain())
  {
    m_events->post(
      SessionEvent::Error,
      "destroySwapchain was called while the render thread was not parked");
    return;
  }
  auto generation = m_renderThread->takeSwapchain();
  destroySwapchainGeneration(m_device->vkDevice(), *generation);
}

Result<void> LitSession::createSwapchain(
  const VkSurfaceKHR surface, const std::uint32_t width, const std::uint32_t height)
{
  if (m_renderThread->headless())
  {
    return Error{"A headless session has no swapchain"};
  }
  if (surface == VK_NULL_HANDLE)
  {
    return Error{"No surface for the swapchain"};
  }
  if (!m_renderThread->mayReplaceSwapchain())
  {
    return Error{"The swapchain can only be replaced while the render thread is parked"};
  }
  if (m_events->lost())
  {
    return Error{"The GPU device is lost"};
  }

  try
  {
    destroySwapchain();
    const auto& gpu = m_device->gpu();
    return createSwapchainGeneration(SwapchainSpec{
             gpu.physicalDevice,
             gpu.queueFamily,
             m_device->vkDevice(),
             &m_device->device(),
             vk::SurfaceKHR{surface},
             width,
             height,
             m_options.forceFifo,
             m_swapchainCount + 1,
           })
           | kdl::transform([&](std::unique_ptr<SwapchainGeneration> generation) {
               // a 0×0 extent makes none
               if (generation)
               {
                 ++m_swapchainCount;
               }
               m_renderThread->setSwapchain(std::move(generation));
             });
  }
  catch (const std::exception& e)
  {
    return Error{fmt::format("Cannot create the swapchain: {}", e.what())};
  }
}

bool LitSession::hasSwapchain() const
{
  return m_renderThread->hasSwapchain();
}

void LitSession::unpark()
{
  m_renderThread->unpark();
}

bool LitSession::swapchainReplacementRequested() const
{
  return m_renderThread->swapchainReplacementRequested();
}

void LitSession::surfaceChanged()
{
  m_renderThread->surfaceChanged();
}

void LitSession::beginStop()
{
  m_events->beginStop();
}

void LitSession::requestStop()
{
  beginStop();
  if (m_renderThread)
  {
    m_renderThread->requestStop();
  }
  // step 1 (R2.1): the jobs queued and running return at once, so the render thread's
  // exit, which waits for its batch, and this thread's join wait for no batch
  if (m_pool)
  {
    m_pool->cancel();
  }
  // D71: the compile thread ends after the pipeline it is making
  if (m_compiler)
  {
    m_compiler->requestStop();
  }
}

bool LitSession::waitForRenderThread(const std::chrono::milliseconds timeout)
{
  return !m_renderThread || m_renderThread->waitForExit(timeout);
}

void LitSession::stopRenderThread()
{
  if (m_renderThreadStopped)
  {
    return;
  }
  m_renderThreadStopped = true;
  requestStop();
  if (m_renderThread)
  {
    m_renderThread->join();
  }
  // step 1 (D71): the compile thread, whose last pipeline may still be in the driver
  if (m_compiler)
  {
    m_compiler->join();
  }
  // step 1, after the render thread that queued the jobs: the pool, cancelled when the
  // stop was requested, is stopped (its generation bumped again, its threads joined);
  // abandoned futures are only waited for (R5.22)
  if (m_pool)
  {
    m_pool->stop();
  }
  // nothing that runs from here on reads the owner's material service
  m_materials = nullptr;
}

void LitSession::releaseGpuObjects()
{
  stopRenderThread();
  if (m_gpuReleased)
  {
    return;
  }
  m_gpuReleased = true;

  if (m_device)
  {
    try
    {
      // step 2: every handle the session holds
      if (m_renderThread)
      {
        m_renderThread->releaseFrameResources();
      }
      if (m_compiler)
      {
        m_compiler->releaseAll();
      }

      // step 3 (S13): on a lost device the wait's result is ignored and garbage is not
      // collected
      auto& device = m_device->device();
      if (!device.waitForIdle())
      {
        m_events->markLost("waitForIdle at the release");
      }
      if (!m_events->lost())
      {
        device.runGarbageCollection();
      }
    }
    catch (const vk::DeviceLostError& e)
    {
      m_events->markLost(e.what());
    }
    catch (const std::exception& e)
    {
      m_events->markFailed(e.what());
    }

    // step 4: the wrappers, vkDestroySwapchainKHR, the semaphores
    if (m_renderThread)
    {
      if (auto generation = m_renderThread->takeSwapchain())
      {
        destroySwapchainGeneration(m_device->vkDevice(), *generation);
      }
    }

    // what the session still holds once its objects are released
    m_report.vramAtStop = m_device->vram();

    // step 5: the NVRHI device, wrapper first; its last Release() must return 0
    m_report.deviceReleaseCount = m_device->releaseNvrhiDevice();
    if (m_report.deviceReleaseCount != 0)
    {
      m_events->post(
        SessionEvent::Error,
        fmt::format(
          "The NVRHI device is still referenced {} times after its release",
          m_report.deviceReleaseCount));
    }
    // plan step 5: a reference left is a handle the session forgot to release
    assert(m_report.deviceReleaseCount == 0);

    m_report.nvrhiErrors = m_device->nvrhiErrors();
  }

  // a loss that NVRHI's callback set but no one reported reaches the report (review V.4)
  if (m_events->lost() && !m_events->lostReported())
  {
    m_events->markLost("found at the stop");
  }
  m_report.deviceLost = m_events->lost();
  if (m_renderThread)
  {
    m_report.frameLogError = m_renderThread->frameLogError();
    m_report.framesPresented = m_renderThread->framesPresented();
  }
  m_report.swapchainGenerations = m_swapchainCount;
  m_report.messages = m_events->takeTeardownMessages();
}

void LitSession::destroyVkDevice()
{
  releaseGpuObjects();
  if (m_vkDeviceDestroyed)
  {
    return;
  }
  m_vkDeviceDestroyed = true;
  if (m_device)
  {
    m_device->destroyVkDevice();
  }
}

TeardownReport LitSession::teardownReport() const
{
  return m_report;
}

void LitSession::debugInjectFault(const DebugFault fault)
{
  m_renderThread->injectFault(fault);
}

void LitSession::debugSetHook(const DebugHook point, std::function<void()> hook)
{
  m_renderThread->setDebugHook(point, std::move(hook));
}

std::uint32_t LitSession::lastDeviceReleaseCount() const
{
  return m_report.deviceReleaseCount;
}

std::size_t LitSession::nvrhiErrorCount() const
{
  return m_device ? m_device->nvrhiErrors() : 0u;
}

std::uint64_t LitSession::framesPresented() const
{
  return m_renderThread ? m_renderThread->framesPresented() : 0u;
}

bool LitSession::lastFrameSettledForTesting() const
{
  return m_renderThread && m_renderThread->lastFrameSettled();
}

std::uint64_t LitSession::swapchainGenerations() const
{
  return m_swapchainCount;
}

CameraState LitSession::camera() const
{
  return m_renderThread->camera();
}

bool LitSession::deviceLost() const
{
  return m_events->lost();
}

std::uint64_t LitSession::snapshotsTakenForTesting() const
{
  return m_renderThread ? m_renderThread->snapshotsTaken() : 0u;
}

std::shared_ptr<const Snapshot> LitSession::lastSnapshotForTesting() const
{
  return m_renderThread ? m_renderThread->lastSnapshot() : nullptr;
}

std::uint64_t LitSession::geometryBatchesForTesting() const
{
  return m_renderThread ? m_renderThread->batchesApplied() : 0u;
}

ModelTotals LitSession::modelTotalsForTesting() const
{
  return m_renderThread ? m_renderThread->modelTotals() : ModelTotals{};
}

PipelineCompiler* LitSession::pipelineCompilerForTesting() const
{
  return m_gpuReleased ? nullptr : m_compiler.get();
}

const SessionOptions& LitSession::optionsForTesting() const
{
  return m_options;
}

void LitSession::resizeHeadlessForTesting(
  const std::uint32_t width, const std::uint32_t height)
{
  m_renderThread->resizeHeadless(vk::Extent2D{width, height});
}

std::uint64_t LitSession::renderTargetGenerationForTesting() const
{
  return m_renderThread ? m_renderThread->renderTargetGeneration() : 0u;
}

std::optional<VramUsage> LitSession::vramForTesting() const
{
  return m_device && !m_vkDeviceDestroyed ? m_device->vram() : std::nullopt;
}

void LitSession::runInFrameForTesting(std::function<void(RenderFrameAccess&)> hook)
{
  m_renderThread->runInFrame(std::move(hook));
}

void LitSession::setEveryFrameHookForTesting(std::function<void(RenderFrameAccess&)> hook)
{
  m_renderThread->setEveryFrameHook(std::move(hook));
}

void LitSession::runAfterPassesForTesting(std::function<void(RenderFrameAccess&)> hook)
{
  m_renderThread->runAfterPasses(std::move(hook));
}

void LitSession::runAfterTonemapForTesting(std::function<void(RenderFrameAccess&)> hook)
{
  m_renderThread->runAfterTonemap(std::move(hook));
}

void LitSession::setHdrClearForTesting(std::optional<std::array<float, 4>> colour)
{
  m_renderThread->setHdrClear(colour);
}

void LitSession::setTonemapForTesting(const EngineTonemapDefaults& tonemap)
{
  m_renderThread->setTonemapDefaults(tonemap);
}

Result<LdrImage> LitSession::readbackLdr(const std::chrono::milliseconds timeout)
{
  if (!m_renderThread || m_renderThreadStopped)
  {
    return Error{"The lit preview session has stopped; there is no frame to read back."};
  }
  auto promise = std::make_shared<std::promise<LdrImage>>();
  auto future = promise->get_future();
  m_renderThread->readbackLdr(
    [promise](LdrImage image) { promise->set_value(std::move(image)); });
  if (future.wait_for(timeout) != std::future_status::ready)
  {
    return Error{fmt::format(
      "No settled frame within {} ms (a session that is fed continuously, or not "
      "exposed, "
      "never settles).",
      timeout.count())};
  }
  try
  {
    return future.get();
  }
  catch (const std::future_error&)
  {
    // the render thread ended with the request
    return Error{"The lit preview session stopped before a settled frame."};
  }
}

std::optional<LdrImage> LitSession::readbackLdrForTesting(
  const std::chrono::milliseconds timeout)
{
  return readbackLdr(timeout) | kdl::transform([](LdrImage image) {
           return std::optional<LdrImage>{std::move(image)};
         })
         | kdl::value_or(std::optional<LdrImage>{});
}

std::uint64_t LitSession::ldrStagingCreationsForTesting() const
{
  return m_renderThread ? m_renderThread->ldrStagingCreations() : 0;
}

void LitSession::setChunksForTesting(
  std::shared_ptr<const std::map<ChunkKey, Chunk>> chunks)
{
  m_renderThread->setTestChunks(std::move(chunks));
}

void LitSession::setUploadCapForTesting(const std::uint64_t bytes)
{
  m_renderThread->setUploadCap(bytes);
}

void LitSession::holdFrameCompletionForTesting(const bool hold)
{
  m_renderThread->holdCompletion(hold);
}

void LitSession::setAllocationLimitForTesting(const std::uint32_t limit)
{
  m_renderThread->setAllocationLimit(limit);
}

void LitSession::setVramBudgetForTesting(const std::uint64_t bytes)
{
  m_renderThread->setVramBudget(bytes);
}

void LitSession::skipInteractionsForTesting(const bool skip)
{
  m_renderThread->setSkipInteractions(skip);
}

void LitSession::skipShaderPassesForTesting(const bool skip)
{
  m_renderThread->setSkipShaderPasses(skip);
}

bool LitSession::shaderPassesSkippedForTesting() const
{
  return m_renderThread->skipShaderPasses();
}

void LitSession::failHostAllocationsForTesting(const bool fail)
{
  tb::lit::failHostAllocationsForTesting(fail);
}

std::vector<ProgramReport> LitSession::programReport() const
{
  return m_compiler ? m_compiler->programs() : std::vector<ProgramReport>{};
}

SessionStatus LitSession::status() const
{
  return m_renderThread ? m_renderThread->status() : SessionStatus{};
}

CompileTimings LitSession::compileTimingsForTesting() const
{
  return m_compiler ? m_compiler->timingsForTesting() : CompileTimings{};
}

void LitSession::setMapPathOverrideForTesting(const char* gameRelativeMapPath)
{
  setSceneMapPathOverrideForTesting(gameRelativeMapPath);
}

void LitSession::holdProbeBuildsForTesting(const bool hold)
{
  setProbeBuildsHeldForTesting(hold);
}

std::int64_t LitSession::liveDeviceMemoryAllocationsForTesting()
{
  return liveDeviceMemoryAllocations();
}

std::int64_t LitSession::liveThreadsForTesting()
{
  return liveLibraryThreads();
}

std::string sessionStatusLine(
  const SessionStatus& status, const std::vector<ProgramReport>& programs)
{
  auto line = std::string{};
  if (status.frames == 0)
  {
    line = "starting";
  }
  else
  {
    line = fmt::format(
      "{} | {} of {} {} drawn",
      status.settled ? "settled" : "drawing",
      status.lightsDrawn,
      status.lightsVisible,
      status.lightsVisible == 1 ? "light" : "lights");
    if (status.lightsOverflowed > 0)
    {
      line += fmt::format(", {} overflowed", status.lightsOverflowed);
    }
    line += fmt::format(" | {} draws", status.draws);
    if (status.skippedDraws > 0)
    {
      line += fmt::format(", {} skipped", status.skippedDraws);
    }
    if (status.pendingPipelines > 0)
    {
      line += fmt::format(" | {} pipelines pending", status.pendingPipelines);
    }
    line +=
      fmt::format(" | VRAM {:.1f} MB", double(status.vramBytes) / (1024.0 * 1024.0));
    // D90 (LP4.1): the view's portal flow while a tree exists
    if (status.areas > 0)
    {
      line += fmt::format(
        " | area {}, {} of {} areas", status.viewArea, status.areasFlowed, status.areas);
    }
    // D90 (LP4.3, K8, K29): the bake, once known
    using Bake = SessionStatus::Bake;
    if (status.bake != Bake::Unknown)
    {
      line += status.bake == Bake::None   ? " | bake: none"
              : status.bake == Bake::Proc ? " | bake: proc"
              : status.bake == Bake::Bproc
                ? " | bake: bproc"
                : fmt::format(" | bake: dropped ({})", status.bakeDropReason);
      const auto stale =
        status.bake != Bake::None
        && (status.documentModified || (status.bakeTime > 0 && status.mapFileTime > status.bakeTime));
      if (stale)
      {
        line += ", stale";
      }
      if (status.solidPieces > 0)
      {
        line += fmt::format(
          ", {} {} outside the baked hull",
          status.solidPieces,
          status.solidPieces == 1 ? "piece" : "pieces");
      }
    }
  }
  for (const auto& program : programs)
  {
    if (!program.enabled)
    {
      // LP4.2a (D9, C34): an ambient program disables the ambient pass, said as such
      const auto ambient = program.program >= EngineBuiltinAmbientLightingIbl
                           && program.program <= EngineBuiltinAmbientLightgridIblPbr + 1;
      line += fmt::format(
        " | {}disabled: {} ({})",
        ambient ? "ambient " : "",
        program.name,
        program.reason);
    }
  }
  return line;
}

nvrhi::IDevice* LitSession::nvrhiDeviceForTesting() const
{
  return m_device && !m_gpuReleased ? &m_device->device() : nullptr;
}

} // namespace tb::lit
