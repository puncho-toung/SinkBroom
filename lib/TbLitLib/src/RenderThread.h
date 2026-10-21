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

#include "FrameTimers.h"
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

namespace tb::lit
{
class GpuDevice;
class SessionEvents;
struct SwapchainGeneration;

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
  vk::Extent2D m_headlessExtent;

  // ----- the swapchain generation, replaced by the GUI thread only while parked -----
  std::unique_ptr<SwapchainGeneration> m_swapchain;

  // ----- render thread while it runs, then the GUI thread -----
  // the frame's one command list, immediate; any second list that may be open alongside
  // it sets enableImmediateExecution = false (R5.22)
  nvrhi::CommandListHandle m_commandList;
  std::array<nvrhi::EventQueryHandle, FramesInFlight> m_frameEvents;
  // the slot's event query was set by a submitted frame and not waited for yet
  std::array<bool, FramesInFlight> m_frameEventSet = {};
  FrameTimers m_timers;
  std::uint64_t m_frameIndex = 0;
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
  FramePacer m_pacer;
  std::function<void()> m_beforePresentHook;
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
  void start();

  // Any thread.

  void setCamera(const CameraState& camera);
  CameraState camera() const;
  void setExposed(bool exposed);
  void requestFrame();
  std::uint64_t framesPresented() const;

  // GUI thread: the surface handshake.

  bool park(std::chrono::milliseconds timeout);
  bool isParked() const;
  void unpark();
  // parked, or not running
  bool mayReplaceSwapchain() const;
  bool hasSwapchain() const;
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
  void setBeforePresentHook(std::function<void()> hook);

private:
  void main() noexcept;
  void runLoop();
  bool stopOrParkRequested() const;
  // under m_mutex
  bool canDraw() const;

  FrameOutcome drawFrame(const CameraState& camera);
  // whether the given fault was injected; it is taken
  bool takeFault(DebugFault fault);
  bool waitForFrameEvent(std::size_t slot);
  void runBeforePresentHook();
  void requestRecreate(
    SwapchainGeneration& generation, std::string_view reason, bool suboptimal = false);

  void idleTick();
  bool waitForIdle(std::string_view when);
  void parkGpuWork();
  void finish() noexcept;
  // a start or stop row with the VRAM in use
  void appendSessionRow(FrameLogEvent event);

  deleteCopyAndMove(RenderThread);
};

} // namespace tb::lit
