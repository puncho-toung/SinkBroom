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

#include "Swapchain.h"
#include "VulkanDevice.h"
#include "lit/FrameLog.h"
#include "lit/FramePacing.h"
#include "lit/LitConfig.h"
#include "lit/SwapchainRules.h"

#include <fmt/format.h>
#include <nvrhi/nvrhi.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

namespace tb::lit
{
namespace
{

// LP0 draws one pass, the clear
constexpr auto kPassCount = std::size_t{1};
constexpr auto kClearPass = std::size_t{0};
constexpr auto kTimerQueryCount = kPassCount * kFramesInFlight;

constexpr auto kAcquireTimeoutNs = std::uint64_t(
  std::chrono::duration_cast<std::chrono::nanoseconds>(kAcquireTimeout).count());

// how often the render thread looks at a frame's event query while it waits for the GPU;
// a stop or a park ends the wait at once
constexpr auto kEventPollInterval = std::chrono::milliseconds{1};

void post(
  const EventSink& sink,
  const SessionEvent event,
  std::string message,
  const std::uint64_t generation) noexcept
{
  if (!sink)
  {
    return;
  }
  try
  {
    sink(event, std::move(message), generation);
  }
  catch (...)
  {
    // a sink that throws loses its message, not the session
  }
}

/**
 * NVRHI's message callback (R5.19). NVRHI's submit catches DeviceLostError itself and
 * reports it only here, so every Error-severity message sets the lost flag, which the
 * render thread checks after every executeCommandLists. Warnings go to the log. It is
 * called on whichever thread made the NVRHI call.
 */
class MessageCallback : public nvrhi::IMessageCallback
{
public:
  MessageCallback(
    const EventSink& sink, const std::uint64_t generation, std::atomic<bool>& lost)
    : m_sink{sink}
    , m_generation{generation}
    , m_lost{lost}
  {
  }

  void message(const nvrhi::MessageSeverity severity, const char* messageText) override
  {
    switch (severity)
    {
    case nvrhi::MessageSeverity::Info:
      return;
    case nvrhi::MessageSeverity::Warning:
      post(
        m_sink,
        SessionEvent::Warning,
        fmt::format("NVRHI warning: {}", messageText),
        m_generation);
      return;
    case nvrhi::MessageSeverity::Error:
    case nvrhi::MessageSeverity::Fatal:
      ++m_errorCount;
      m_lost = true;
      post(
        m_sink,
        SessionEvent::Error,
        fmt::format("NVRHI error: {}", messageText),
        m_generation);
      return;
    }
  }

  std::size_t errorCount() const { return m_errorCount; }

private:
  const EventSink& m_sink;
  std::uint64_t m_generation;
  std::atomic<bool>& m_lost;
  std::atomic<std::size_t> m_errorCount = 0;
};

// One query of the timer ring (R4-2.20) and the frame log row waiting for its time.
struct TimerSlot
{
  nvrhi::TimerQueryHandle query;
  // ended in a submitted command list and not read yet
  bool pending = false;
  std::optional<FrameLogRow> row;
};

enum class FrameOutcome
{
  Presented,
  // not drawn, or drawn but not shown: the trigger stays pending
  Skipped,
  // lost or failed: nothing more is drawn
  Stopped,
};

// the error of a failed result; Result has one error type
template <typename T>
Error errorOf(const Result<T>& result)
{
  return std::get<Error>(result.error());
}

double millisecondsSince(const FrameClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(FrameClock::now() - start).count();
}

} // namespace

struct LitSession::Impl
{
  Impl(
    InstanceHandles instance_,
    SessionOptions options_,
    EventSink sink_,
    const std::uint64_t generation_)
    : instance{std::move(instance_)}
    , options{std::move(options_)}
    , sink{std::move(sink_)}
    , generation{generation_}
    , messageCallback{sink, generation, lost}
  {
  }

  // ----- immutable after start -----
  InstanceHandles instance;
  SessionOptions options;
  EventSink sink;
  std::uint64_t generation;
  GpuChoice gpu;
  vk::Device vkDevice;
  vk::Queue queue;

  std::atomic<bool> lost = false;
  std::atomic<bool> lostReported = false;
  std::atomic<bool> failed = false;

  // declared before the device it serves (R5.19)
  MessageCallback messageCallback;
  nvrhi::vulkan::DeviceHandle nvrhiDevice;
  // Debug with options.validation only
  nvrhi::DeviceHandle validationDevice;
  // validationDevice, else nvrhiDevice
  nvrhi::IDevice* device = nullptr;

  // ----- the frame target: a swapchain generation, or the headless texture -----
  // replaced by the GUI thread only while the render thread is parked or not running
  std::unique_ptr<SwapchainGeneration> swapchain;
  std::uint64_t swapchainCount = 0;
  bool headless = false;
  nvrhi::TextureHandle headlessTarget;
  vk::Extent2D headlessExtent;

  // ----- frame resources, render thread only while it runs -----
  // the frame's one command list, immediate; any second list that may be open alongside
  // it sets enableImmediateExecution = false (R5.22). LP0 has no volatile constant
  // buffer; a later one sizes maxVersions with volatileBufferMaxVersions (R5.22).
  nvrhi::CommandListHandle commandList;
  std::array<nvrhi::EventQueryHandle, kFramesInFlight> frameEvents;
  // the slot's event query was set by a submitted frame and not waited for yet
  std::array<bool, kFramesInFlight> frameEventSet = {};
  std::array<TimerSlot, kTimerQueryCount> timers;
  std::uint64_t frameIndex = 0;
  std::uint64_t framesSkipped = 0;
  bool readyPosted = false;
  bool idleWorkPending = false;
  FrameClock::time_point lastFrameEnd;
  FrameLogWriter frameLog;
  std::atomic<std::uint64_t> framesPresented = 0;
  std::atomic<bool> injectDeviceLost = false;

  // ----- shared with the GUI thread, under mutex -----
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable parkedChanged;
  bool stopRequested = false;
  bool parkRequested = false;
  bool parked = false;
  bool threadExited = false;
  bool exposed = false;
  bool cameraSet = false;
  CameraState camera;
  FramePacer pacer;
  std::function<void()> beforePresentHook;

  // ----- GUI thread -----
  std::thread thread;
  bool threadStopped = false;
  bool gpuReleased = false;
  bool vkDeviceDestroyed = false;
  std::atomic<std::uint32_t> lastReleaseCount = 0;

  void postEvent(const SessionEvent event, std::string message) const
  {
    post(sink, event, std::move(message), generation);
  }

  void markLost(const std::string& reason)
  {
    lost = true;
    if (!lostReported.exchange(true))
    {
      postEvent(
        SessionEvent::DeviceLost,
        fmt::format(
          "The GPU device was lost ({}). Press Start to run the preview again.", reason));
    }
  }

  void markFailed(const std::string& reason)
  {
    if (!failed.exchange(true))
    {
      postEvent(
        SessionEvent::Error,
        fmt::format("The lit preview stopped: {}. Stop and Start it again.", reason));
    }
  }

  // ===== creation (§3.1 phase B) =====

  Result<void> start(
    const VkSurfaceKHR surface, const std::uint32_t width, const std::uint32_t height)
  {
    // step 4
    auto gpuResult =
      pickPhysicalDevice(vk::Instance{instance.instance}, vk::SurfaceKHR{surface});
    if (gpuResult.is_error())
    {
      return errorOf(gpuResult);
    }
    gpu = std::move(gpuResult).value();
    for (const auto& skipped : gpu.skippedDevices)
    {
      postEvent(SessionEvent::Warning, skipped);
    }

    // step 5
    auto deviceResult = createVkDevice(gpu);
    if (deviceResult.is_error())
    {
      return errorOf(deviceResult);
    }
    vkDevice = deviceResult.value();
    queue = vkDevice.getQueue(gpu.queueFamily, 0);

    // step 6, as the engine does it (DeviceManager_VK.cpp:1348-1375)
    auto instanceExtensions = std::vector<const char*>{};
    for (const auto& name : instance.enabledExtensions)
    {
      instanceExtensions.push_back(name.c_str());
    }
    auto deviceExtensions = std::vector<const char*>{};
    for (const auto& name : gpu.deviceExtensions)
    {
      deviceExtensions.push_back(name.c_str());
    }

    auto desc = nvrhi::vulkan::DeviceDesc{};
    desc.errorCB = &messageCallback;
    desc.instance = instance.instance;
    desc.physicalDevice = gpu.physicalDevice;
    desc.device = vkDevice;
    desc.graphicsQueue = queue;
    desc.graphicsQueueIndex = int(gpu.queueFamily);
    desc.transferQueue = VK_NULL_HANDLE;
    desc.computeQueue = VK_NULL_HANDLE;
    desc.instanceExtensions = instanceExtensions.data();
    desc.numInstanceExtensions = instanceExtensions.size();
    desc.deviceExtensions = deviceExtensions.data();
    desc.numDeviceExtensions = deviceExtensions.size();
    nvrhiDevice = nvrhi::vulkan::createDevice(desc);
    if (!nvrhiDevice)
    {
      return Error{"Cannot create the NVRHI device"};
    }
#ifndef NDEBUG
    if (options.validation)
    {
      validationDevice = nvrhi::validation::createValidationLayer(nvrhiDevice);
    }
#endif
    device = validationDevice ? validationDevice.Get() : nvrhiDevice.Get();

    // step 7
    if (surface != VK_NULL_HANDLE)
    {
      if (auto result = createSwapchain(surface, width, height); result.is_error())
      {
        return result;
      }
    }
    else
    {
      headless = true;
      headlessExtent = vk::Extent2D{std::max(width, 1u), std::max(height, 1u)};
      headlessTarget =
        device->createTexture(nvrhi::TextureDesc{}
                                .setWidth(headlessExtent.width)
                                .setHeight(headlessExtent.height)
                                .setFormat(nvrhi::Format::RGBA8_UNORM)
                                .setDebugName("Lit preview headless target")
                                .setIsRenderTarget(true)
                                .setInitialState(nvrhi::ResourceStates::RenderTarget)
                                .setKeepInitialState(true));
      if (!headlessTarget)
      {
        return Error{"Cannot create the headless target"};
      }
    }

    // step 8, LP0's part: the frame resources and the render thread
    commandList = device->createCommandList();
    for (auto& event : frameEvents)
    {
      event = device->createEventQuery();
    }
    if (gpu.timerQueries)
    {
      for (auto& timer : timers)
      {
        timer.query = device->createTimerQuery();
      }
    }
    if (!commandList || lost)
    {
      return Error{"Cannot create the frame resources"};
    }

    thread = std::thread{[this] { renderThreadMain(); }};
    return kdl::void_success;
  }

  // ===== swapchain generations (§3.1, D49), GUI thread =====

  bool mayReplaceSwapchain() const
  {
    const auto lock = std::scoped_lock{mutex};
    return !thread.joinable() || threadExited || parked;
  }

  Result<void> createSwapchain(
    const VkSurfaceKHR surface, const std::uint32_t width, const std::uint32_t height)
  {
    if (headless)
    {
      return Error{"A headless session has no swapchain"};
    }
    if (surface == VK_NULL_HANDLE)
    {
      return Error{"No surface for the swapchain"};
    }
    if (!mayReplaceSwapchain())
    {
      return Error{
        "The swapchain can only be replaced while the render thread is parked"};
    }
    if (lost)
    {
      return Error{"The GPU device is lost"};
    }

    destroySwapchain();

    auto result = createSwapchainGeneration(SwapchainSpec{
      gpu.physicalDevice,
      gpu.queueFamily,
      vkDevice,
      device,
      vk::SurfaceKHR{surface},
      width,
      height,
      options.forceFifo,
      swapchainCount + 1,
    });
    if (result.is_error())
    {
      return errorOf(result);
    }

    swapchain = std::move(result).value();
    if (swapchain)
    {
      ++swapchainCount;
    }
    return kdl::void_success;
  }

  void destroySwapchain()
  {
    if (!swapchain)
    {
      return;
    }
    if (!mayReplaceSwapchain())
    {
      postEvent(
        SessionEvent::Error,
        "destroySwapchain was called while the render thread was not parked");
      return;
    }
    destroySwapchainGeneration(vkDevice, *swapchain);
    swapchain.reset();
  }

  // ===== render thread (C20) =====

  bool stopOrParkRequested() const
  {
    const auto lock = std::scoped_lock{mutex};
    return stopRequested || parkRequested;
  }

  // under mutex; the swapchain is not replaced while the render thread runs unparked
  bool canDraw() const
  {
    if (lost || failed || !exposed)
    {
      return false;
    }
    return headless ? bool(headlessTarget) : swapchain && !swapchain->stale;
  }

  void renderThreadMain() noexcept
  {
    try
    {
      if (const auto error = frameLog.open(options.frameLogPath))
      {
        postEvent(SessionEvent::Warning, *error);
      }
      runLoop();
    }
    catch (const std::exception& e)
    {
      markFailed(fmt::format("the render thread failed: {}", e.what()));
    }
    catch (...)
    {
      markFailed("the render thread failed");
    }
    finish();

    {
      const auto lock = std::scoped_lock{mutex};
      threadExited = true;
    }
    parkedChanged.notify_all();
  }

  void runLoop()
  {
    auto lock = std::unique_lock{mutex};
    while (!stopRequested)
    {
      if (parkRequested)
      {
        if (!parked)
        {
          lock.unlock();
          parkGpuWork();
          lock.lock();
          parked = true;
          parkedChanged.notify_all();
          lock.unlock();
          postEvent(SessionEvent::Parked, {});
          lock.lock();
        }
        wake.wait(lock, [&] { return stopRequested || !parkRequested; });
        if (!parkRequested)
        {
          parked = false;
        }
        continue;
      }

      if (lost && !lostReported)
      {
        lock.unlock();
        markLost("NVRHI reported an error");
        lock.lock();
        continue;
      }

      const auto now = FrameClock::now();
      const auto next = pacer.nextFrameTime(canDraw());
      if (next && *next <= now)
      {
        pacer.frameStarted(now);
        const auto frameCamera = camera;
        lock.unlock();
        const auto outcome = drawFrame(frameCamera);
        lock.lock();
        if (outcome == FrameOutcome::Skipped)
        {
          pacer.frameSkipped();
        }
        continue;
      }

      auto deadline = next.value_or(FrameClock::time_point::max());
      if (idleWorkPending && !lost)
      {
        const auto idleAt = lastFrameEnd + kIdleCollectInterval;
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
        wake.wait(lock);
      }
      else
      {
        wake.wait_until(lock, deadline);
      }
    }
  }

  void requestRecreate(SwapchainGeneration& target, const std::string_view reason)
  {
    if (!target.recreateRequested)
    {
      target.recreateRequested = true;
      postEvent(
        SessionEvent::SwapchainOutOfDate,
        fmt::format("Swapchain generation {}: {}", target.number, reason));
    }
  }

  FrameOutcome drawFrame(const CameraState& frameCamera)
  {
    try
    {
      if (injectDeviceLost.exchange(false))
      {
        throw vk::DeviceLostError{"injected by debugInjectDeviceLost"};
      }
      // a pass boundary
      if (stopOrParkRequested())
      {
        return FrameOutcome::Skipped;
      }

      // frame N - kFramesInFlight has finished: its event query, acquire semaphore and
      // timer queries are free again (D35)
      const auto slot = std::size_t(frameIndex % kFramesInFlight);
      if (frameEventSet[slot])
      {
        if (!waitForFrameEvent(slot))
        {
          return FrameOutcome::Skipped;
        }
        device->resetEventQuery(frameEvents[slot]);
        frameEventSet[slot] = false;
      }
      resolveTimers(slot);

      // the pass boundary before LP0's one pass: park and stop are honoured before an
      // image is acquired, so an acquired image is always presented
      if (stopOrParkRequested())
      {
        return FrameOutcome::Skipped;
      }

      auto* const target = swapchain.get();
      if (!target && !headless)
      {
        return FrameOutcome::Skipped;
      }
      auto imageIndex = std::uint32_t{0};
      nvrhi::ITexture* texture = headlessTarget.Get();
      auto extent = headlessExtent;
      if (target)
      {
        // the vk::Result overload: OUT_OF_DATE is a value, not an exception
        const auto result = vkDevice.acquireNextImageKHR(
          target->swapchain,
          kAcquireTimeoutNs,
          target->acquireSemaphores[slot],
          nullptr,
          &imageIndex);
        switch (result)
        {
        case vk::Result::eSuccess:
          break;
        case vk::Result::eSuboptimalKHR:
          // the image is acquired and its semaphore will be signalled: draw and present
          requestRecreate(*target, "acquire returned SUBOPTIMAL");
          break;
        case vk::Result::eTimeout:
        case vk::Result::eNotReady:
          // D48: skip; the trigger stays pending, so it is retried
          ++framesSkipped;
          return FrameOutcome::Skipped;
        case vk::Result::eErrorOutOfDateKHR:
          ++framesSkipped;
          target->stale = true;
          requestRecreate(*target, "acquire returned OUT_OF_DATE");
          return FrameOutcome::Skipped;
        case vk::Result::eErrorSurfaceLostKHR:
          ++framesSkipped;
          target->stale = true;
          postEvent(
            SessionEvent::Error,
            "The window's Vulkan surface was lost. Stop and Start the preview.");
          return FrameOutcome::Skipped;
        case vk::Result::eErrorDeviceLost:
          throw vk::DeviceLostError{"vkAcquireNextImageKHR"};
        default:
          markFailed(
            fmt::format("vkAcquireNextImageKHR returned {}", vk::to_string(result)));
          return FrameOutcome::Stopped;
        }
        texture = target->imageTextures[imageIndex].Get();
        extent = target->extent;
      }

      const auto recordStart = FrameClock::now();
      auto& timer = timers[timerQuerySlot(frameIndex, kClearPass, kPassCount)];
      const auto color = clearColorFor(frameCamera.direction);

      commandList->open();
      if (timer.query)
      {
        commandList->beginTimerQuery(timer.query);
      }
      commandList->clearTextureFloat(
        texture,
        nvrhi::AllSubresources,
        nvrhi::Color{color[0], color[1], color[2], color[3]});
      if (timer.query)
      {
        commandList->endTimerQuery(timer.query);
      }
      commandList->close();

      if (target)
      {
        nvrhiDevice->queueWaitForSemaphore(
          nvrhi::CommandQueue::Graphics, target->acquireSemaphores[slot], 0);
        nvrhiDevice->queueSignalSemaphore(
          nvrhi::CommandQueue::Graphics, target->presentSemaphores[imageIndex], 0);
      }
      device->executeCommandList(commandList);
      // NVRHI's submit reports a lost device only through the message callback (R5.19)
      if (lost)
      {
        throw vk::DeviceLostError{"NVRHI reported an error in executeCommandLists"};
      }
      device->setEventQuery(frameEvents[slot], nvrhi::CommandQueue::Graphics);
      frameEventSet[slot] = true;
      timer.pending = bool(timer.query);
      const auto cpuRecordMs = millisecondsSince(recordStart);
      const auto frame = frameIndex++;
      idleWorkPending = true;

      runBeforePresentHook();

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
        const auto result = queue.presentKHR(&present);
        switch (result)
        {
        case vk::Result::eSuccess:
          break;
        case vk::Result::eSuboptimalKHR:
          requestRecreate(*target, "present returned SUBOPTIMAL");
          break;
        case vk::Result::eErrorOutOfDateKHR:
          shown = false;
          target->stale = true;
          requestRecreate(*target, "present returned OUT_OF_DATE");
          break;
        case vk::Result::eErrorSurfaceLostKHR:
          shown = false;
          target->stale = true;
          postEvent(
            SessionEvent::Error,
            "The window's Vulkan surface was lost. Stop and Start the preview.");
          break;
        case vk::Result::eErrorDeviceLost:
          throw vk::DeviceLostError{"vkQueuePresentKHR"};
        default:
          markFailed(fmt::format("vkQueuePresentKHR returned {}", vk::to_string(result)));
          return FrameOutcome::Stopped;
        }
      }

      if (shown)
      {
        auto row = FrameLogRow{};
        row.frameIndex = frame;
        row.cpuRecordMs = cpuRecordMs;
        row.presentMode = presentModeName(
          target ? std::optional{VkPresentModeKHR(target->presentMode)} : std::nullopt);
        row.swapchainGeneration = target ? target->number : 0;
        row.framesSkipped = framesSkipped;
        row.width = extent.width;
        row.height = extent.height;
        if (timer.pending)
        {
          timer.row = row;
        }
        else
        {
          frameLog.append(row);
        }
        ++framesPresented;
      }
      else
      {
        ++framesSkipped;
      }

      device->runGarbageCollection();
      lastFrameEnd = FrameClock::now();

      if (shown && !readyPosted)
      {
        readyPosted = true;
        postEvent(
          SessionEvent::Ready, fmt::format("Lit preview running on {}", gpu.name));
      }
      return shown ? FrameOutcome::Presented : FrameOutcome::Skipped;
    }
    catch (const vk::DeviceLostError& e)
    {
      markLost(e.what());
      return FrameOutcome::Stopped;
    }
    catch (const vk::SystemError& e)
    {
      markFailed(e.what());
      return FrameOutcome::Stopped;
    }
    catch (const std::exception& e)
    {
      markFailed(e.what());
      return FrameOutcome::Stopped;
    }
  }

  // Waits until the slot's frame has finished on the GPU; false when a stop or a park
  // came first. NVRHI's waitEventQuery would wait without a bound.
  bool waitForFrameEvent(const std::size_t slot)
  {
    while (!device->pollEventQuery(frameEvents[slot]))
    {
      auto lock = std::unique_lock{mutex};
      if (wake.wait_for(
            lock, kEventPollInterval, [&] { return stopRequested || parkRequested; }))
      {
        return false;
      }
    }
    return true;
  }

  void runBeforePresentHook()
  {
    auto hook = std::function<void()>{};
    {
      const auto lock = std::scoped_lock{mutex};
      hook = beforePresentHook;
    }
    if (hook)
    {
      hook();
    }
  }

  // Reads the slot's timer queries, whose frame has finished, and completes their rows.
  void resolveTimers(const std::size_t frameSlot)
  {
    for (std::size_t pass = 0; pass < kPassCount; ++pass)
    {
      auto& timer = timers[frameSlot * kPassCount + pass];
      if (!timer.pending)
      {
        continue;
      }
      const auto seconds = device->getTimerQueryTime(timer.query);
      timer.pending = false;
      if (timer.row)
      {
        timer.row->gpuClearMs = double(seconds) * 1000.0;
        frameLog.append(*timer.row);
        timer.row.reset();
      }
    }
  }

  bool timersPending(const std::size_t frameSlot) const
  {
    for (std::size_t pass = 0; pass < kPassCount; ++pass)
    {
      if (timers[frameSlot * kPassCount + pass].pending)
      {
        return true;
      }
    }
    return false;
  }

  // The frame slots oldest first, so that rows stay in frame order; f returns false to
  // stop.
  template <typename F>
  void forEachSlotOldestFirst(F&& f)
  {
    for (std::size_t i = 0; i < kFramesInFlight; ++i)
    {
      if (!f(std::size_t((frameIndex + i) % kFramesInFlight)))
      {
        return;
      }
    }
  }

  // §3.1, §3.10: garbage collection and timer polling about a second after the last frame
  void idleTick()
  {
    try
    {
      auto allResolved = true;
      forEachSlotOldestFirst([&](const std::size_t slot) {
        if (!timersPending(slot))
        {
          return true;
        }
        // a slot's queries are read only once its frame has finished
        if (!device->pollEventQuery(frameEvents[slot]))
        {
          allResolved = false;
          return false;
        }
        resolveTimers(slot);
        return true;
      });
      device->runGarbageCollection();
      if (allResolved)
      {
        idleWorkPending = false;
      }
      else
      {
        lastFrameEnd = FrameClock::now();
      }
    }
    catch (const vk::DeviceLostError& e)
    {
      markLost(e.what());
    }
    catch (const std::exception& e)
    {
      markFailed(e.what());
    }
  }

  // waitForIdle; false, and the device marked lost, when it reports a lost device
  bool waitForIdle(const std::string_view when)
  {
    if (device->waitForIdle())
    {
      return true;
    }
    markLost(fmt::format("waitForIdle {}", when));
    return false;
  }

  // The render thread's half of the surface handshake (§3.1, R5.18, D49).
  void parkGpuWork()
  {
    try
    {
      // also on a lost device, whose wait returns at once: the GUI thread destroys the
      // swapchain next, which no submitted command may still use
      waitForIdle("while parking");
      if (!lost)
      {
        forEachSlotOldestFirst([&](const std::size_t slot) {
          resolveTimers(slot);
          return true;
        });
      }
      if (swapchain)
      {
        releaseSwapchainWrappers(*swapchain);
      }
      if (!lost)
      {
        device->runGarbageCollection();
      }
      idleWorkPending = false;
    }
    catch (const vk::DeviceLostError& e)
    {
      markLost(e.what());
    }
    catch (const std::exception& e)
    {
      markFailed(e.what());
    }
  }

  // At exit: the outstanding GPU times, then the frame log (C16: flushed at stop).
  void finish() noexcept
  {
    try
    {
      // also on a lost device: teardown step 2 releases textures that only clears used,
      // and clears hold no reference to them (NVRHI vulkan-texture.cpp:609-698)
      if (waitForIdle("at stop") && !lost)
      {
        forEachSlotOldestFirst([&](const std::size_t slot) {
          resolveTimers(slot);
          return true;
        });
      }
    }
    catch (const vk::DeviceLostError& e)
    {
      markLost(e.what());
    }
    catch (const std::exception& e)
    {
      markFailed(e.what());
    }

    try
    {
      // rows whose GPU time never arrived keep an empty field
      forEachSlotOldestFirst([&](const std::size_t slot) {
        for (std::size_t pass = 0; pass < kPassCount; ++pass)
        {
          auto& timer = timers[slot * kPassCount + pass];
          if (timer.row)
          {
            frameLog.append(*timer.row);
            timer.row.reset();
          }
        }
        return true;
      });
      frameLog.close();
    }
    catch (const std::exception& e)
    {
      postEvent(
        SessionEvent::Warning, fmt::format("Cannot write the frame log: {}", e.what()));
    }
  }

  // ===== teardown (§3.1, C18), GUI thread =====

  // step 1
  void requestStop()
  {
    {
      const auto lock = std::scoped_lock{mutex};
      stopRequested = true;
    }
    wake.notify_all();
  }

  bool waitForRenderThread(const std::chrono::milliseconds timeout)
  {
    auto lock = std::unique_lock{mutex};
    return !thread.joinable()
           || parkedChanged.wait_for(lock, timeout, [&] { return threadExited; });
  }

  void stopRenderThread()
  {
    if (threadStopped)
    {
      return;
    }
    threadStopped = true;
    requestStop();
    // never detached: the thread uses this object until it exits
    if (thread.joinable())
    {
      thread.join();
    }
  }

  // steps 2-5
  void releaseGpuObjects()
  {
    stopRenderThread();
    if (gpuReleased)
    {
      return;
    }
    gpuReleased = true;

    try
    {
      // step 2: every handle the session holds
      commandList = nullptr;
      for (auto& event : frameEvents)
      {
        event = nullptr;
      }
      for (auto& timer : timers)
      {
        timer = TimerSlot{};
      }
      headlessTarget = nullptr;

      // step 3 (S13): on a lost device the wait's result is ignored and garbage is not
      // collected
      if (device)
      {
        if (!device->waitForIdle())
        {
          lost = true;
        }
        if (!lost)
        {
          device->runGarbageCollection();
        }
      }
    }
    catch (const vk::DeviceLostError&)
    {
      lost = true;
    }
    catch (const std::exception& e)
    {
      markFailed(e.what());
    }

    // step 4: the wrappers, vkDestroySwapchainKHR, the semaphores
    if (swapchain)
    {
      destroySwapchainGeneration(vkDevice, *swapchain);
      swapchain.reset();
    }

    // step 5: the NVRHI device, wrapper first; its last Release() must return 0
    device = nullptr;
    validationDevice = nullptr;
    if (nvrhiDevice)
    {
      lastReleaseCount = std::uint32_t(nvrhiDevice.Reset());
      if (lastReleaseCount != 0)
      {
        postEvent(
          SessionEvent::Error,
          fmt::format(
            "The NVRHI device is still referenced {} times after its release",
            std::uint32_t(lastReleaseCount)));
      }
    }
  }

  // step 7, the device part
  void destroyVkDevice()
  {
    releaseGpuObjects();
    if (vkDeviceDestroyed)
    {
      return;
    }
    vkDeviceDestroyed = true;
    if (vkDevice)
    {
      vkDevice.destroy();
      vkDevice = nullptr;
    }
  }
};

Result<std::unique_ptr<LitSession>> LitSession::create(
  const InstanceHandles& instance,
  const VkSurfaceKHR surface,
  const std::uint32_t width,
  const std::uint32_t height,
  SessionOptions options,
  EventSink eventSink,
  const std::uint64_t generation)
{
  try
  {
    if (instance.instance == VK_NULL_HANDLE || instance.getInstanceProcAddr == nullptr)
    {
      return Error{"No Vulkan instance"};
    }
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices == nullptr)
    {
      return Error{"The Vulkan dispatcher is not initialised for the instance"};
    }

    auto session = std::unique_ptr<LitSession>{new LitSession{std::make_unique<Impl>(
      instance, std::move(options), std::move(eventSink), generation)}};
    if (auto result = session->m_impl->start(surface, width, height); result.is_error())
    {
      // the destructor tears down whatever exists (§3.1)
      return errorOf(result);
    }
    return std::move(session);
  }
  catch (const vk::SystemError& e)
  {
    return Error{fmt::format("Cannot start the lit preview: {}", e.what())};
  }
  catch (const std::exception& e)
  {
    return Error{fmt::format("Cannot start the lit preview: {}", e.what())};
  }
}

LitSession::LitSession(std::unique_ptr<Impl> impl)
  : m_impl{std::move(impl)}
{
}

LitSession::~LitSession()
{
  m_impl->destroyVkDevice();
}

std::uint64_t LitSession::generation() const
{
  return m_impl->generation;
}

const std::string& LitSession::deviceName() const
{
  return m_impl->gpu.name;
}

VkPhysicalDevice LitSession::physicalDevice() const
{
  return m_impl->gpu.physicalDevice;
}

std::uint32_t LitSession::queueFamily() const
{
  return m_impl->gpu.queueFamily;
}

void LitSession::setCamera(const CameraState& camera)
{
  {
    const auto lock = std::scoped_lock{m_impl->mutex};
    if (m_impl->cameraSet && m_impl->camera == camera)
    {
      return;
    }
    m_impl->camera = camera;
    m_impl->cameraSet = true;
    m_impl->pacer.trigger();
  }
  m_impl->wake.notify_all();
}

void LitSession::setExposed(const bool exposed)
{
  {
    const auto lock = std::scoped_lock{m_impl->mutex};
    if (exposed && !m_impl->exposed)
    {
      // the window shows again: draw it once
      m_impl->pacer.trigger();
    }
    m_impl->exposed = exposed;
  }
  m_impl->wake.notify_all();
}

void LitSession::requestFrame()
{
  {
    const auto lock = std::scoped_lock{m_impl->mutex};
    m_impl->pacer.trigger();
  }
  m_impl->wake.notify_all();
}

bool LitSession::park(const std::chrono::milliseconds timeout)
{
  auto lock = std::unique_lock{m_impl->mutex};
  if (!m_impl->thread.joinable() || m_impl->threadExited)
  {
    return true;
  }
  m_impl->parkRequested = true;
  m_impl->wake.notify_all();
  return m_impl->parkedChanged.wait_for(
    lock, timeout, [&] { return m_impl->parked || m_impl->threadExited; });
}

bool LitSession::isParked() const
{
  const auto lock = std::scoped_lock{m_impl->mutex};
  return !m_impl->thread.joinable() || m_impl->threadExited || m_impl->parked;
}

void LitSession::destroySwapchain()
{
  m_impl->destroySwapchain();
}

Result<void> LitSession::createSwapchain(
  const VkSurfaceKHR surface, const std::uint32_t width, const std::uint32_t height)
{
  try
  {
    return m_impl->createSwapchain(surface, width, height);
  }
  catch (const std::exception& e)
  {
    return Error{fmt::format("Cannot create the swapchain: {}", e.what())};
  }
}

bool LitSession::hasSwapchain() const
{
  return m_impl->swapchain != nullptr;
}

void LitSession::unpark()
{
  {
    const auto lock = std::scoped_lock{m_impl->mutex};
    if (!m_impl->parkRequested)
    {
      return;
    }
    m_impl->parkRequested = false;
    // the new generation's first frame
    m_impl->pacer.trigger();
  }
  m_impl->wake.notify_all();
}

void LitSession::requestStop()
{
  m_impl->requestStop();
}

bool LitSession::waitForRenderThread(const std::chrono::milliseconds timeout)
{
  return m_impl->waitForRenderThread(timeout);
}

void LitSession::stopRenderThread()
{
  m_impl->stopRenderThread();
}

void LitSession::releaseGpuObjects()
{
  m_impl->releaseGpuObjects();
}

void LitSession::destroyVkDevice()
{
  m_impl->destroyVkDevice();
}

void LitSession::debugInjectDeviceLost()
{
  m_impl->injectDeviceLost = true;
}

void LitSession::debugSetBeforePresentHook(std::function<void()> hook)
{
  const auto lock = std::scoped_lock{m_impl->mutex};
  m_impl->beforePresentHook = std::move(hook);
}

std::uint32_t LitSession::lastDeviceReleaseCount() const
{
  return m_impl->lastReleaseCount;
}

std::size_t LitSession::nvrhiErrorCount() const
{
  return m_impl->messageCallback.errorCount();
}

std::uint64_t LitSession::framesPresented() const
{
  return m_impl->framesPresented;
}

bool LitSession::deviceLost() const
{
  return m_impl->lost;
}

} // namespace tb::lit
