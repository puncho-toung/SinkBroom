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

#include "base/Macros.h"
#include "base/Result.h"
#include "lit/VulkanInstance.h"

#include "vm/vec.h"

#include <vulkan/vulkan_core.h>

namespace nvrhi
{
class IDevice;
}

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

struct SessionOptions
{
  // the validation layer is on the instance; in Debug, NVRHI's validation layer as well
  bool validation = false;
  // FIFO whatever the surface offers (D48, the forced FIFO-only test)
  bool forceFifo = false;
  // the frame log (C16); empty means no log
  std::filesystem::path frameLogPath;
  // C1: the game folder (Map::gamePath()) whose base/renderprogs2/spirv holds the
  // engine's shader blobs; empty: no shader loads, every program is disabled (D9)
  std::filesystem::path gamePath;
};

// D9: a program the compile thread was asked for, and whether the preview can draw with
// it ("disabled: <field>" otherwise); the panel's status line shows the disabled ones
struct ProgramReport
{
  // a BUILTIN_ index (lit/EngineBuiltins.h)
  int program = 0;
  // the builtin's name and suffix
  std::string name;
  bool enabled = true;
  std::string reason;

  bool operator==(const ProgramReport&) const = default;
};

// A copy of the 3D view's camera (§3.2); never a pointer to it.
struct CameraState
{
  vm::vec3f position = vm::vec3f{0, 0, 0};
  vm::vec3f direction = vm::vec3f{1, 0, 0};
  vm::vec3f up = vm::vec3f{0, 0, 1};
  float fovDegrees = 90.0f;
  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;

  bool operator==(const CameraState&) const = default;
};

enum class SessionEvent
{
  // the first frame of the session was presented (headless: finished); the message
  // names the GPU
  Ready,
  // the render thread parked; sent also when park() timed out and it parked later
  Parked,
  // the device is lost: nothing more is drawn, press Start
  DeviceLost,
  // a session error, also an NVRHI error that is not a lost device; nothing more is
  // drawn, Stop and Start
  Error,
  // a warning to log (an NVRHI warning, an unwritable frame log); the session goes on
  Warning,
  // a line of the material parse thread's log (lit/MaterialService.h): MaterialInfo for
  // its summaries, MaterialWarning for the copied parser's warnings and errors and for
  // the game files it could not read; the session goes on (R4-1.17)
  MaterialInfo,
  MaterialWarning,
  // the material service published a new table (MaterialService::table()); the owner
  // posts it from the parse thread's listener, at most one at a time until it is handled
  MaterialsPublished,
  // acquire or present returned OUT_OF_DATE or SUBOPTIMAL: the GUI thread should park,
  // destroySwapchain, createSwapchain and unpark; posted once per swapchain generation
  SwapchainOutOfDate,
  // S31, B0.3: the inbox is empty: posted when the render thread starts and whenever it
  // has taken the snapshot offered (LitSession::offerSnapshot); it does not mean that
  // the last snapshot's chunks are applied
  ReadyForSnapshot,
};

/**
 * Called from the render thread, and from the GUI thread inside create, createSwapchain
 * and the teardown steps; the Qt side marshals to the GUI thread and drops a message
 * whose generation is not the live session's (R5.17). It must not block and must be safe
 * to call from any thread.
 */
using EventSink =
  std::function<void(SessionEvent, std::string message, std::uint64_t generation)>;

// The device-local heaps' VRAM, from VK_EXT_memory_budget.
struct VramUsage
{
  std::uint64_t usedBytes = 0;
  std::uint64_t budgetBytes = 0;
};

// C30: what the session's device tells the engine's rules (P2.10, P2.13)
struct DeviceFacts
{
  std::uint32_t maxPushConstantsSize = 0;
  std::uint32_t vendorID = 0;
  std::uint32_t maxMemoryAllocationCount = 0;
  // min(maxPushConstantsSize, NVRHI's c_MaxPushConstantSize)
  std::uint32_t pushConstantLimit = 0;
  bool d24s8Supported = false;
};

/**
 * What a session's teardown found, for its owner, which logs and shows a report that is
 * not clean. Complete once releaseGpuObjects has run. Messages that the session would
 * otherwise post while it stops land here, because no owner acts on them any more.
 */
struct TeardownReport
{
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t deviceReleaseCount = 0;
  std::size_t nvrhiErrors = 0;
  // lost during the session, or found lost while stopping
  bool deviceLost = false;
  // the frame log could not be written in full
  std::optional<std::string> frameLogError;
  // the warnings and errors raised while stopping
  std::vector<std::string> messages;
  std::uint64_t framesPresented = 0;
  std::uint64_t swapchainGenerations = 0;
  // when the session had its device, and once its objects were released, before the
  // device went; empty when the device does not report its budget
  std::optional<VramUsage> vramAtStart;
  std::optional<VramUsage> vramAtStop;
};

// Test hooks: faults the render thread raises in its next frame.
enum class DebugFault
{
  // vk::DeviceLostError at the start of the frame, before any GPU work
  DeviceLostBeforeFrame,
  // vk::DeviceLostError once the frame has acquired its image, before it records
  DeviceLostAfterAcquire,
  // NVRHI's "Device Removed!" through its message callback inside the submit, the only
  // way NVRHI reports a loss in executeCommandLists (R5.19)
  DeviceRemovedInSubmit,
  // an Error-severity NVRHI message, as from NVRHI's validation layer, after the submit
  NvrhiError,
  // an exception while the frame records its commands
  ThrowWhileRecording,
};

// Test hooks: where the render thread runs a test's function.
enum class DebugHook
{
  // in every frame after its commands are submitted and before it presents: a frame held
  // there is in flight, as one waiting inside a present would be
  BeforePresent,
  // in every park, once the generation's image wrappers are released, before the thread
  // counts as parked
  WhileParking,
  // in every park, once the thread counts as parked, before it posts Parked
  AfterParking,
  // in every pass that draws no frame, while the render thread's mutex is released for
  // the chunk jobs, before the pass chooses its wait
  IdleUnlocked,
};

// §3.1: the longest park() waits; the plan's bound is the current pass plus two frames in
// flight, about 250 ms in a settled frame
constexpr auto ParkTimeout = std::chrono::milliseconds{500};

class CpuPool;
class GpuDevice;
class LightTablePublication;
class MaterialService;
class PipelineCompiler;
class RenderThread;
class SessionEvents;
struct LightTable;
struct Snapshot;

/**
 * Everything a running preview owns below Qt (C18, D34): the VkDevice, the NVRHI device,
 * the swapchain generations, the render thread, the CPU pool and the frame log. The
 * instance, the surface and the material service belong to the caller. Nothing in it is
 * static. Its parts live in TbLitLib's sources, so that vulkan.hpp and NVRHI stay out of
 * TbUiLib.
 *
 * Threads (C20): the render thread alone opens, records and executes command lists and
 * collects garbage, and never waits on the GUI thread. The compile thread (D71) only
 * creates shaders, layouts, its format-only framebuffers and pipelines. The CPU pool's
 * jobs only fill CPU memory. The GUI thread creates and destroys swapchains only while
 * the render thread is parked, and runs the teardown steps once it is joined.
 *
 * Snapshots are pulled (§3.3, S31): the render thread posts ReadyForSnapshot when it
 * starts and whenever it has taken the snapshot offered; the GUI thread then offers one
 * (offerSnapshot), and at most one waits in the session's one-slot inbox. B0.3: the
 * render thread takes it at the start of a frame once the chunk jobs of the snapshot
 * before it are applied (lit/SceneGeometry.h), so one snapshot is in flight (taken, its
 * chunks not yet applied) besides the one in the inbox, and the chunks apply in the
 * snapshots' sequence order.
 */
class LitSession
{
private:
  SessionOptions m_options;
  // declared before the parts that post through it
  std::unique_ptr<SessionEvents> m_events;
  std::unique_ptr<GpuDevice> m_device;
  std::unique_ptr<RenderThread> m_renderThread;
  // S3: made at phase B, stopped in teardown step 1
  std::unique_ptr<CpuPool> m_pool;
  // D71: the compile thread, started at phase B, joined in teardown step 1 beside the
  // pool, its handles released in step 2
  std::unique_ptr<PipelineCompiler> m_compiler;
  // the owner's, which outlives the session (LitSessionHost); null after step 1
  MaterialService* m_materials = nullptr;
  // R2.9: the render thread's light registry publishes here; it outlives the thread
  std::shared_ptr<LightTablePublication> m_lightTables;
  std::uint64_t m_swapchainCount = 0;

  bool m_renderThreadStopped = false;
  bool m_gpuReleased = false;
  bool m_vkDeviceDestroyed = false;
  TeardownReport m_report;
  // the process's one live session slot, released when the destructor has torn down
  bool m_claimedLiveSession = false;

public:
  /**
   * Phase B of Start (§3.1 steps 4-7): picks the physical device that presents to
   * surface, creates the VkDevice, the NVRHI device and the first swapchain generation,
   * and starts the render thread. initInstanceDispatcher must have been called for the
   * instance. A null surface is the headless test path: no presentation, no swapchain;
   * the render thread clears an offscreen texture of the given size instead. Only one
   * session lives in a process at a time, because the Vulkan dispatcher is process-wide:
   * a second create fails while one exists. Step 8 also starts the CPU pool. materials,
   * the owner's material service or null, must outlive the session's teardown step 1,
   * which forgets it.
   */
  static Result<std::unique_ptr<LitSession>> create(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height,
    SessionOptions options,
    EventSink eventSink,
    std::uint64_t generation,
    MaterialService* materials = nullptr);

  // Runs every teardown step that has not run.
  ~LitSession();

  std::uint64_t generation() const;
  // the physical device's name
  const std::string& deviceName() const;
  // C30: the device's limits and vendor; any thread
  DeviceFacts deviceFacts() const;
  // C30, D72: the device memory allocations live now (lit's GpuDevice); any thread
  std::int64_t deviceMemoryAllocations() const;
  // the GPU and its one graphics and present queue family, for the GUI thread's present
  // check after a screen change (R5.33)
  VkPhysicalDevice physicalDevice() const;
  std::uint32_t queueFamily() const;

  // latest-wins camera slot, any thread; a changed camera triggers a frame
  void setCamera(const CameraState& camera);
  // gates every frame trigger (R5.25); the GUI thread mirrors QWindow::isExposed()
  void setExposed(bool exposed);
  // any thread
  void requestFrame();

  /**
   * S31, GUI thread: puts snapshot into the one-slot inbox and triggers a frame, whose
   * start hands it to the render thread. False, and the snapshot is not taken, when the
   * slot still holds the last one, when the snapshot is null or of another session
   * generation (D53), or when the render thread has stopped; the caller keeps its changes
   * and offers again on the next ReadyForSnapshot.
   */
  bool offerSnapshot(std::shared_ptr<const Snapshot> snapshot);
  /**
   * B0.7, GUI thread: the one-slot inbox is empty and the render thread has not been
   * stopped, so that an offer now is taken unless the thread failed meanwhile; an atomic
   * read. The owner asks whenever it has changes, so that one lost ReadyForSnapshot never
   * stops the snapshots of the session.
   */
  bool snapshotSlotFree() const;
  /**
   * R2.2, any thread: the material service published a table (the owner calls it on
   * SessionEvent::MaterialsPublished). The render thread wakes and adopts it at its next
   * pass that draws no frame, so that chunks and lights built while a material, an
   * entityDef or the .proc was pending are built again without a camera move. It sets a
   * flag under the render thread's mutex and notifies.
   */
  void tableChanged();
  /**
   * C15, R2.9, any thread: the latest light records the render thread published (never
   * null; empty before the first snapshot's lights are built), also after teardown step
   * 1, when the render thread has ended.
   */
  std::shared_ptr<const LightTable> lightTable() const;

  // S3: the CPU pool from phase B until teardown step 1, else null; GUI thread, tests
  // only (B0.10, R1.11: the render thread gets the pool and the service at its start)
  CpuPool* cpuPoolForTesting() const;
  // the owner's material service, from create until teardown step 1, else null; GUI
  // thread, tests only
  MaterialService* materialServiceForTesting() const;

  // Surface handshake (§3.1, D49), GUI thread only.

  /**
   * Asks the render thread to park and waits up to timeout. True: it is parked, and
   * destroySwapchain and createSwapchain may run. False: it has not parked yet, because
   * it is still inside a present that may need this thread's message loop; the request
   * stays, and SessionEvent::Parked arrives when it parks (check isParked()).
   */
  bool park(std::chrono::milliseconds timeout = ParkTimeout);
  bool isParked() const;
  // Destroys the swapchain and its semaphores; the render thread has released the
  // wrappers. Does nothing unless parked or stopped.
  void destroySwapchain();
  /**
   * Creates the next generation on the window's (new) surface; destroys the current one
   * first if there is one. A 0×0 extent (a minimised window) makes no swapchain: the
   * render thread draws nothing until the next one. An error, for example a GPU that no
   * longer presents to the surface after a screen change (R5.33), means Stop.
   */
  Result<void> createSwapchain(
    VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);
  bool hasSwapchain() const;
  /**
   * Ends a park. A park that completed released the generation's image wrappers, so a
   * thread that parked is resumed only after createSwapchain; one resumed without a new
   * generation draws nothing and asks for one with SwapchainOutOfDate (review V.1).
   */
  void unpark();
  /**
   * Whether a SwapchainOutOfDate still concerns the current generation: false once the
   * generation it was posted for has been replaced, so that a replacement is not
   * replaced again (review V.2). True without a swapchain.
   */
  bool swapchainReplacementRequested() const;
  /**
   * The window's DPR or screen changed. A generation that the surface reports SUBOPTIMAL
   * is replaced once; a new one of the same size that is SUBOPTIMAL again presents on as
   * it is, until the size changes or this is called.
   */
  void surfaceChanged();

  // Teardown (§3.1 steps), GUI thread; the Qt side deletes the window container between
  // releaseGpuObjects and destroyVkDevice (step 6). Each step runs once and runs the
  // earlier ones first if they have not run.

  // Step 1 comes in parts, so that a GUI thread can keep answering the messages that a
  // present may send to its window while it waits (R5.18): requestStop sets the stop
  // flag, cancels the CPU pool (R2.1: its generation bumped without a join, so its queued
  // and running jobs return at once) and returns; the render thread stops at the next
  // pass boundary, which includes the one before the present, waits for the GPU (also
  // when lost, ignoring the result) and for its jobs, writes the frame log and exits;
  // waitForRenderThread waits up to timeout and returns whether it has exited.
  // stopRenderThread requests the stop and joins without servicing anything, for callers
  // without a window, and after waitForRenderThread returned true; then it stops the CPU
  // pool (its generation bumped again, its threads joined, R5.22) and forgets the
  // material service.
  void requestStop();
  // from here on, warnings, errors and a loss go into the teardown report instead of the
  // event sink; requestStop calls it, and a caller that parks first calls it before that
  void beginStop();
  bool waitForRenderThread(std::chrono::milliseconds timeout);
  void stopRenderThread();
  // steps 2-5: release every handle, waitForIdle, runGarbageCollection (skipped when
  // lost), the swapchain wrappers, vkDestroySwapchainKHR, the semaphores, the NVRHI
  // device (its last Release() count is kept)
  void releaseGpuObjects();
  // step 7, the device part: vkDestroyDevice; the instance belongs to the caller
  void destroyVkDevice();
  // what the teardown found; complete after releaseGpuObjects
  TeardownReport teardownReport() const;

  // Test hooks

  // the render thread raises the fault in its next frame
  void debugInjectFault(DebugFault fault);
  // runs hook on the render thread at point until an empty function replaces it
  void debugSetHook(DebugHook point, std::function<void()> hook);
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t lastDeviceReleaseCount() const;
  std::size_t nvrhiErrorCount() const;
  // frames presented (headless: finished recording and submitted)
  std::uint64_t framesPresented() const;
  // the swapchain generations made so far; GUI thread
  std::uint64_t swapchainGenerations() const;
  // the camera the next frame draws with
  CameraState camera() const;
  bool deviceLost() const;
  // the snapshots the render thread took so far, and the last one; any thread
  std::uint64_t snapshotsTakenForTesting() const;
  std::shared_ptr<const Snapshot> lastSnapshotForTesting() const;
  // the batches of chunk or light jobs the render thread has applied; any thread
  std::uint64_t geometryBatchesForTesting() const;
  // the device to make resources with, from create until teardown step 5; tests only
  nvrhi::IDevice* nvrhiDeviceForTesting() const;
  // the compile thread, from create until teardown step 2; tests only (LP2.1 hands it to
  // the render thread)
  PipelineCompiler* pipelineCompilerForTesting() const;
  /**
   * D9, any thread: every program the compile thread was asked for, enabled or disabled
   * with the field that disabled it; the session's report of its passes.
   */
  std::vector<ProgramReport> programReport() const;

private:
  LitSession(
    SessionOptions options,
    EventSink eventSink,
    std::uint64_t generation,
    MaterialService* materials);

  Result<void> start(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height);

  deleteCopyAndMove(LitSession);
};

} // namespace tb::lit
