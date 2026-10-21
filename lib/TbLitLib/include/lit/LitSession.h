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
  // the device is lost or NVRHI reported an error: nothing more is drawn, press Start
  DeviceLost,
  // a session error; nothing more is drawn, Stop and Start
  Error,
  // a warning to log (an NVRHI warning, an unwritable frame log); the session goes on
  Warning,
  // acquire or present returned OUT_OF_DATE or SUBOPTIMAL: the GUI thread should park,
  // destroySwapchain, createSwapchain and unpark; posted once per swapchain generation
  SwapchainOutOfDate,
};

/**
 * Called from the render thread, and from the GUI thread inside create, createSwapchain
 * and the teardown steps; the Qt side marshals to the GUI thread and drops a message
 * whose generation is not the live session's (R5.17). It must not block and must be safe
 * to call from any thread.
 */
using EventSink =
  std::function<void(SessionEvent, std::string message, std::uint64_t generation)>;

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
};

// §3.1: the longest park() waits; the plan's bound is the current pass plus two frames in
// flight, about 250 ms in a settled frame
constexpr auto ParkTimeout = std::chrono::milliseconds{500};

class GpuDevice;
class RenderThread;
class SessionEvents;

/**
 * Everything a running preview owns below Qt (C18, D34): the VkDevice, the NVRHI device,
 * the swapchain generations, the render thread and the frame log. The instance and the
 * surface belong to the caller. Nothing in it is static. Its parts live in TbLitLib's
 * sources, so that vulkan.hpp and NVRHI stay out of TbUiLib.
 *
 * Threads (C20): the render thread alone opens, records and executes command lists and
 * collects garbage. The GUI thread creates and destroys swapchains only while the render
 * thread is parked, and runs the teardown steps once it is joined.
 */
class LitSession
{
private:
  SessionOptions m_options;
  // declared before the parts that post through it
  std::unique_ptr<SessionEvents> m_events;
  std::unique_ptr<GpuDevice> m_device;
  std::unique_ptr<RenderThread> m_renderThread;
  std::uint64_t m_swapchainCount = 0;

  bool m_renderThreadStopped = false;
  bool m_gpuReleased = false;
  bool m_vkDeviceDestroyed = false;
  TeardownReport m_report;

public:
  /**
   * Phase B of Start (§3.1 steps 4-7): picks the physical device that presents to
   * surface, creates the VkDevice, the NVRHI device and the first swapchain generation,
   * and starts the render thread. initInstanceDispatcher must have been called for the
   * instance. A null surface is the headless test path: no presentation, no swapchain;
   * the render thread clears an offscreen texture of the given size instead.
   */
  static Result<std::unique_ptr<LitSession>> create(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height,
    SessionOptions options,
    EventSink eventSink,
    std::uint64_t generation);

  // Runs every teardown step that has not run.
  ~LitSession();

  std::uint64_t generation() const;
  // the physical device's name
  const std::string& deviceName() const;
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
  void unpark();
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
  // present may send to its window while it waits (R5.18): requestStop sets the stop flag
  // and returns; the render thread stops at the next pass boundary, which includes the
  // one before the present, waits for the GPU (also when lost, ignoring the result),
  // writes the frame log and exits; waitForRenderThread waits up to timeout and returns
  // whether it has exited. stopRenderThread requests the stop and joins without servicing
  // anything, for callers without a window, and after waitForRenderThread returned true.
  void requestStop();
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

  // the render thread throws vk::DeviceLostError at its next frame
  void debugInjectDeviceLost();
  /**
   * Runs on the render thread in every frame after its commands are submitted and before
   * it presents, until an empty function replaces it: a frame held there is in flight, as
   * one waiting inside a present would be.
   */
  void debugSetBeforePresentHook(std::function<void()> hook);
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t lastDeviceReleaseCount() const;
  std::size_t nvrhiErrorCount() const;
  // frames presented (headless: finished recording and submitted)
  std::uint64_t framesPresented() const;
  bool deviceLost() const;

private:
  LitSession(SessionOptions options, EventSink eventSink, std::uint64_t generation);

  Result<void> start(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height);

  deleteCopyAndMove(LitSession);
};

} // namespace tb::lit
