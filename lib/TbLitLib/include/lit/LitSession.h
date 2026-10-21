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
#include <string>

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

// §3.1: the longest park() waits; the plan's bound is the current pass plus two frames in
// flight, about 250 ms in a settled frame
constexpr auto kParkTimeout = std::chrono::milliseconds{500};

/**
 * Everything a running preview owns below Qt (C18, D34): the VkDevice, the NVRHI device,
 * the swapchain generations, the render thread and the frame log. The instance and the
 * surface belong to the caller. Nothing in it is static.
 *
 * Threads (C20): the render thread alone opens, records and executes command lists and
 * collects garbage. The GUI thread creates and destroys swapchains only while the render
 * thread is parked, and runs the teardown steps once it is joined.
 */
class LitSession
{
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

  // Runs every teardown step that has not run yet.
  ~LitSession();

  LitSession(const LitSession&) = delete;
  LitSession& operator=(const LitSession&) = delete;

  std::uint64_t generation() const;
  // the physical device's name
  const std::string& deviceName() const;

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
  bool park(std::chrono::milliseconds timeout = kParkTimeout);
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

  // Teardown (§3.1 steps), GUI thread; the Qt side deletes the window container between
  // releaseGpuObjects and destroyVkDevice (step 6). Each step runs once and runs the
  // earlier ones first if they have not run.

  // step 1: the stop flag; the render thread stops at the next pass boundary, waits for
  // the GPU (also when lost, ignoring the result), writes the frame log and is joined
  void stopRenderThread();
  // steps 2-5: release every handle, waitForIdle, runGarbageCollection (skipped when
  // lost), the swapchain wrappers, vkDestroySwapchainKHR, the semaphores, the NVRHI
  // device (its last Release() count is kept)
  void releaseGpuObjects();
  // step 7, the device part: vkDestroyDevice; the instance belongs to the caller
  void destroyVkDevice();

  // Test hooks

  // the render thread throws vk::DeviceLostError at its next frame
  void debugInjectDeviceLost();
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t lastDeviceReleaseCount() const;
  std::size_t nvrhiErrorCount() const;
  // frames presented (headless: finished recording and submitted)
  std::uint64_t framesPresented() const;
  bool deviceLost() const;

private:
  struct Impl;
  explicit LitSession(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> m_impl;
};

} // namespace tb::lit
