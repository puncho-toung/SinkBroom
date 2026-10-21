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

#include "GpuDevice.h"

#include "SessionEvents.h"
#include "lit/NvrhiSupport.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/validation.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{
namespace
{

// what NVRHI's submit reports when it catches vk::DeviceLostError
// (nvrhi src/vulkan/vulkan-queue.cpp:175)
constexpr auto DeviceRemovedMessage = std::string_view{"Device Removed!"};

} // namespace

MessageCallback::MessageCallback(SessionEvents& events)
  : m_events{events}
{
}

void MessageCallback::message(
  const nvrhi::MessageSeverity severity, const char* messageText) noexcept
{
  try
  {
    const auto text = std::string_view{messageText ? messageText : ""};
    switch (severity)
    {
    case nvrhi::MessageSeverity::Info:
      return;
    case nvrhi::MessageSeverity::Warning:
      m_events.post(SessionEvent::Warning, fmt::format("NVRHI warning: {}", text));
      return;
    case nvrhi::MessageSeverity::Error:
    case nvrhi::MessageSeverity::Fatal:
      ++m_errorCount;
      if (text == DeviceRemovedMessage)
      {
        // the render thread reports it once it sees the flag (review L2.9)
        m_events.setLost();
        return;
      }
      m_events.markFailed(fmt::format("NVRHI error: {}", text));
      return;
    }
  }
  catch (...)
  {
    // a message that cannot be formatted is lost; the count stands
  }
}

std::size_t MessageCallback::errorCount() const
{
  return m_errorCount;
}

GpuDevice::GpuDevice(SessionEvents& events)
  : m_messageCallback{events}
{
}

Result<std::unique_ptr<GpuDevice>> GpuDevice::create(
  const InstanceHandles& instance,
  const vk::SurfaceKHR surface,
  const bool validation,
  SessionEvents& events)
{
  if (!nvrhiHeaderVersionMatches())
  {
    return Error{"The NVRHI headers do not belong to the NVRHI library"};
  }

  auto gpuDevice = std::unique_ptr<GpuDevice>{new GpuDevice{events}};

  // step 4
  return pickPhysicalDevice(vk::Instance{instance.instance}, surface)
         | kdl::and_then([&](GpuChoice gpu) {
             for (const auto& skipped : gpu.skippedDevices)
             {
               events.post(SessionEvent::Warning, skipped);
             }
             gpuDevice->m_gpu = std::move(gpu);
             // step 5
             return createVkDevice(gpuDevice->m_gpu);
           })
         | kdl::and_then(
           [&](const vk::Device vkDevice) -> Result<std::unique_ptr<GpuDevice>> {
             auto& d = *gpuDevice;
             d.m_vkDevice = vkDevice;
             d.m_queue = vkDevice.getQueue(d.m_gpu.queueFamily, 0);

             // step 6, as the engine does it (DeviceManager_VK.cpp:1348-1375)
             auto instanceExtensions = std::vector<const char*>{};
             for (const auto& name : instance.enabledExtensions)
             {
               instanceExtensions.push_back(name.c_str());
             }
             auto deviceExtensions = std::vector<const char*>{};
             for (const auto& name : d.m_gpu.deviceExtensions)
             {
               deviceExtensions.push_back(name.c_str());
             }

             auto desc = nvrhi::vulkan::DeviceDesc{};
             desc.errorCB = &d.m_messageCallback;
             desc.instance = instance.instance;
             desc.physicalDevice = d.m_gpu.physicalDevice;
             desc.device = vkDevice;
             desc.graphicsQueue = d.m_queue;
             desc.graphicsQueueIndex = int(d.m_gpu.queueFamily);
             desc.transferQueue = VK_NULL_HANDLE;
             desc.computeQueue = VK_NULL_HANDLE;
             desc.instanceExtensions = instanceExtensions.data();
             desc.numInstanceExtensions = instanceExtensions.size();
             desc.deviceExtensions = deviceExtensions.data();
             desc.numDeviceExtensions = deviceExtensions.size();
             d.m_nvrhiDevice = nvrhi::vulkan::createDevice(desc);
             if (!d.m_nvrhiDevice)
             {
               return Error{"Cannot create the NVRHI device"};
             }
#ifndef NDEBUG
             if (validation)
             {
               d.m_validationDevice =
                 nvrhi::validation::createValidationLayer(d.m_nvrhiDevice);
             }
#else
             static_cast<void>(validation);
#endif
             d.m_device =
               d.m_validationDevice ? d.m_validationDevice.Get() : d.m_nvrhiDevice.Get();
             return std::move(gpuDevice);
           });
}

GpuDevice::~GpuDevice()
{
  releaseNvrhiDevice();
  destroyVkDevice();
}

const GpuChoice& GpuDevice::gpu() const
{
  return m_gpu;
}

vk::Device GpuDevice::vkDevice() const
{
  return m_vkDevice;
}

vk::Queue GpuDevice::queue() const
{
  return m_queue;
}

nvrhi::IDevice& GpuDevice::device() const
{
  return *m_device;
}

nvrhi::vulkan::IDevice& GpuDevice::vulkanDevice() const
{
  return *m_nvrhiDevice;
}

std::size_t GpuDevice::nvrhiErrors() const
{
  return m_messageCallback.errorCount();
}

MessageCallback& GpuDevice::messageCallback()
{
  return m_messageCallback;
}

std::optional<VramUsage> GpuDevice::vram() const
{
  if (
    std::ranges::find(
      m_gpu.deviceExtensions, std::string{VK_EXT_MEMORY_BUDGET_EXTENSION_NAME})
    == m_gpu.deviceExtensions.end())
  {
    return std::nullopt;
  }

  try
  {
    // as the engine reads it for its statistics (DeviceManager_VK.cpp:1477-1488)
    auto budget = vk::PhysicalDeviceMemoryBudgetPropertiesEXT{};
    auto properties = vk::PhysicalDeviceMemoryProperties2{};
    properties.pNext = &budget;
    m_gpu.physicalDevice.getMemoryProperties2(&properties);

    auto usage = VramUsage{};
    const auto& heaps = properties.memoryProperties;
    for (std::uint32_t i = 0; i < heaps.memoryHeapCount; ++i)
    {
      if (heaps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
      {
        usage.usedBytes += budget.heapUsage[i];
        usage.budgetBytes += budget.heapBudget[i];
      }
    }
    return usage;
  }
  catch (const std::exception&)
  {
    return std::nullopt;
  }
}

std::uint32_t GpuDevice::releaseNvrhiDevice()
{
  m_device = nullptr;
  m_validationDevice = nullptr;
  return m_nvrhiDevice ? std::uint32_t(m_nvrhiDevice.Reset()) : 0u;
}

void GpuDevice::destroyVkDevice()
{
  if (m_vkDevice)
  {
    m_vkDevice.destroy();
    m_vkDevice = nullptr;
  }
}

} // namespace tb::lit
