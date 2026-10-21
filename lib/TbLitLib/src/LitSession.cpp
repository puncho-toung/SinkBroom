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
#include "RenderThread.h"
#include "SessionEvents.h"
#include "Swapchain.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <cassert>
#include <utility>

namespace tb::lit
{

LitSession::LitSession(
  SessionOptions options, EventSink eventSink, const std::uint64_t generation)
  : m_options{std::move(options)}
  , m_events{std::make_unique<SessionEvents>(std::move(eventSink), generation)}
{
}

Result<std::unique_ptr<LitSession>> LitSession::create(
  const InstanceHandles& instance,
  const VkSurfaceKHR surface,
  const std::uint32_t width,
  const std::uint32_t height,
  SessionOptions options,
  EventSink eventSink,
  const std::uint64_t generation)
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
      session.reset(new LitSession{std::move(options), std::move(eventSink), generation});
    }
    catch (...)
    {
      releaseLiveSession();
      throw;
    }
    session->m_claimedLiveSession = true;
    // on an error the destructor tears down whatever exists (§3.1)
    return session->start(instance, surface, width, height)
           | kdl::transform([&]() { return std::move(session); });
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
         | kdl::transform([&]() { m_renderThread->start(); });
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

void LitSession::surfaceChanged()
{
  m_renderThread->surfaceChanged();
}

void LitSession::requestStop()
{
  m_events->beginStop();
  if (m_renderThread)
  {
    m_renderThread->requestStop();
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

      // step 3 (S13): on a lost device the wait's result is ignored and garbage is not
      // collected
      auto& device = m_device->device();
      if (!device.waitForIdle())
      {
        m_events->setLost();
      }
      if (!m_events->lost())
      {
        device.runGarbageCollection();
      }
    }
    catch (const vk::DeviceLostError&)
    {
      m_events->setLost();
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

void LitSession::debugSetBeforePresentHook(std::function<void()> hook)
{
  m_renderThread->setBeforePresentHook(std::move(hook));
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

bool LitSession::deviceLost() const
{
  return m_events->lost();
}

} // namespace tb::lit
