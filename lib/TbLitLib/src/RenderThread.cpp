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
#include "lit/SwapchainRules.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <utility>

namespace tb::lit
{
namespace
{

constexpr auto AcquireTimeoutNs = std::uint64_t(
  std::chrono::duration_cast<std::chrono::nanoseconds>(AcquireTimeout).count());

// how often the render thread looks at a frame's event query while it waits for the GPU;
// a stop or a park ends the wait at once
constexpr auto EventPollInterval = std::chrono::milliseconds{1};

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

/**
 * LP0's frame content: a clear colour from the camera's direction, direction * 0.5 + 0.5,
 * so that a viewer sees the preview follow the camera.
 */
std::array<float, 4> clearColorFor(const vm::vec3f& direction)
{
  const auto channel = [](const float d) {
    return std::clamp(d * 0.5f + 0.5f, 0.0f, 1.0f);
  };
  return {channel(direction.x()), channel(direction.y()), channel(direction.z()), 1.0f};
}

} // namespace

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
    m_headlessTarget =
      device.createTexture(nvrhi::TextureDesc{}
                             .setWidth(m_headlessExtent.width)
                             .setHeight(m_headlessExtent.height)
                             .setFormat(nvrhi::Format::RGBA8_UNORM)
                             .setDebugName("Lit preview headless target")
                             .setIsRenderTarget(true)
                             .setInitialState(nvrhi::ResourceStates::RenderTarget)
                             .setKeepInitialState(true));
    if (!m_headlessTarget)
    {
      return Error{"Cannot create the headless target"};
    }
  }

  // step 8, LP0's part: the frame resources
  m_commandList = device.createCommandList();
  for (auto& event : m_frameEvents)
  {
    event = device.createEventQuery();
  }
  m_timers.createQueries(device, m_device.gpu().timerQueries);
  if (!m_commandList || m_events.lost())
  {
    return Error{"Cannot create the frame resources"};
  }
  return kdl::void_success;
}

bool RenderThread::headless() const
{
  return m_headless;
}

void RenderThread::start()
{
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
  m_commandList = nullptr;
  for (auto& event : m_frameEvents)
  {
    event = nullptr;
  }
  m_timers.releaseQueries();
  m_headlessTarget = nullptr;
}

std::optional<std::string> RenderThread::frameLogError() const
{
  return m_frameLogError;
}

void RenderThread::injectDeviceLost()
{
  m_injectDeviceLost = true;
}

void RenderThread::setBeforePresentHook(std::function<void()> hook)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_beforePresentHook = std::move(hook);
}

void RenderThread::main() noexcept
{
  try
  {
    if (const auto error = m_timers.openLog(m_options.frameLogPath))
    {
      m_events.post(SessionEvent::Warning, *error);
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
        m_events.post(SessionEvent::Parked, {});
        lock.lock();
      }
      m_wake.wait(lock, [&] { return m_stopRequested || !m_parkRequested; });
      if (!m_parkRequested)
      {
        m_parked = false;
      }
      continue;
    }

    if (m_events.lost() && !m_events.lostReported())
    {
      lock.unlock();
      m_events.markLost("NVRHI reported an error");
      lock.lock();
      continue;
    }

    const auto now = FrameClock::now();
    const auto next = m_pacer.nextFrameTime(canDraw());
    if (next && *next <= now)
    {
      m_pacer.frameStarted(now);
      const auto frameCamera = m_camera;
      lock.unlock();
      const auto outcome = drawFrame(frameCamera);
      lock.lock();
      if (outcome == FrameOutcome::Skipped)
      {
        m_pacer.frameSkipped();
      }
      continue;
    }

    auto deadline = next.value_or(FrameClock::time_point::max());
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
  return m_headless ? bool(m_headlessTarget) : m_swapchain && !m_swapchain->stale;
}

RenderThread::FrameOutcome RenderThread::drawFrame(const CameraState& camera)
{
  try
  {
    if (m_injectDeviceLost.exchange(false))
    {
      throw vk::DeviceLostError{"injected by debugInjectDeviceLost"};
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

    // the pass boundary before LP0's one pass: park and stop are honoured before an
    // image is acquired
    if (stopOrParkRequested())
    {
      return FrameOutcome::Skipped;
    }

    auto* const target = m_swapchain.get();
    if (!target && !m_headless)
    {
      return FrameOutcome::Skipped;
    }
    auto imageIndex = std::uint32_t{0};
    nvrhi::ITexture* texture = m_headlessTarget.Get();
    auto extent = m_headlessExtent;
    if (target)
    {
      // the vk::Result overload: OUT_OF_DATE is a value, not an exception
      const auto result = m_device.vkDevice().acquireNextImageKHR(
        target->swapchain,
        AcquireTimeoutNs,
        target->acquireSemaphores[slot],
        nullptr,
        &imageIndex);
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
    }

    const auto recordStart = FrameClock::now();
    auto* const timer = m_timers.query(slot);
    const auto color = clearColorFor(camera.direction);

    {
      auto recording = OpenCommandList{device, *m_commandList};
      auto& commandList = recording.list();
      if (timer)
      {
        commandList.beginTimerQuery(timer);
      }
      commandList.clearTextureFloat(
        texture,
        nvrhi::AllSubresources,
        nvrhi::Color{color[0], color[1], color[2], color[3]});
      if (timer)
      {
        commandList.endTimerQuery(timer);
      }
      recording.close();

      if (target)
      {
        m_device.vulkanDevice().queueWaitForSemaphore(
          nvrhi::CommandQueue::Graphics, target->acquireSemaphores[slot], 0);
        m_device.vulkanDevice().queueSignalSemaphore(
          nvrhi::CommandQueue::Graphics, target->presentSemaphores[imageIndex], 0);
      }
      recording.submit();
    }
    // NVRHI's submit reports a lost device only through the message callback (R5.19)
    if (m_events.lost())
    {
      throw vk::DeviceLostError{"NVRHI reported an error in executeCommandLists"};
    }
    device.setEventQuery(m_frameEvents[slot], nvrhi::CommandQueue::Graphics);
    m_frameEventSet[slot] = true;
    m_timers.frameSubmitted(slot);
    const auto cpuRecordMs = millisecondsSince(recordStart);
    const auto frame = m_frameIndex++;
    m_idleWorkPending = true;

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
      const auto result = m_device.queue().presentKHR(&present);
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
      row.frameIndex = frame;
      row.cpuRecordMs = cpuRecordMs;
      row.presentMode = presentModeName(
        target ? std::optional{VkPresentModeKHR(target->presentMode)} : std::nullopt);
      row.swapchainGeneration = target ? target->number : 0;
      row.framesSkipped = m_framesSkipped;
      row.width = extent.width;
      row.height = extent.height;
      m_timers.frameShown(slot, row);
      ++m_framesPresented;
    }
    else
    {
      ++m_framesSkipped;
    }

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

bool RenderThread::waitForFrameEvent(const std::size_t slot)
{
  // NVRHI's waitEventQuery would wait without a bound
  while (!m_device.device().pollEventQuery(m_frameEvents[slot]))
  {
    auto lock = std::unique_lock{m_mutex};
    if (m_wake.wait_for(
          lock, EventPollInterval, [&] { return m_stopRequested || m_parkRequested; }))
    {
      return false;
    }
  }
  return true;
}

void RenderThread::runBeforePresentHook()
{
  auto hook = std::function<void()>{};
  {
    const auto lock = std::scoped_lock{m_mutex};
    hook = m_beforePresentHook;
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
    device.runGarbageCollection();
    if (allResolved)
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
    }
    if (m_swapchain)
    {
      releaseSwapchainWrappers(*m_swapchain);
    }
    if (!m_events.lost())
    {
      m_device.device().runGarbageCollection();
    }
    m_idleWorkPending = false;
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
    // rows whose GPU time never arrived keep an empty field
    m_frameLogError = m_timers.closeLog(m_frameIndex);
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
