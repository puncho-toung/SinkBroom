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
#include "lit/EngineTables.h"
#include "lit/NvrhiSupport.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <nvrhi/validation.h>

#include <algorithm>
#include <array>
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

// C30, D72: the allocation counter. The dispatcher's own two functions, kept while the
// counting ones stand in for them, and the live count; constant-initialised (C9) and
// process-wide like the dispatcher, which one session at a time owns (LiveSession.h)
constinit auto s_allocateMemory = std::atomic<PFN_vkAllocateMemory>{nullptr};
constinit auto s_freeMemory = std::atomic<PFN_vkFreeMemory>{nullptr};
constinit auto s_liveAllocations = std::atomic<std::int64_t>{0};
// U2.3's test hook: while set, an allocation in a memory type that is host-visible and
// not device-local fails as the driver's would (NVRHI's upload chunks, staging); the
// types found when the counter was installed
constinit auto s_failHostAllocations = std::atomic<bool>{false};
constinit auto s_hostOnlyMemoryTypes = std::atomic<std::uint32_t>{0};

// U2.6: the host-visible allocations live now and their bytes (NVRHI's upload chunks, the
// volatile constant buffers, a readback's staging), for the session's account; a table
// under a spin lock, zero-initialised (C9). Past MaxHostAllocations an allocation is
// counted apart and not in the bytes (never reached: a handful of chunks and buffers)
struct HostAllocation
{
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize bytes = 0;
};
constexpr auto MaxHostAllocations = std::size_t{1024};
constinit auto s_hostVisibleMemoryTypes = std::atomic<std::uint32_t>{0};
constinit auto s_hostAllocationsLock = std::atomic<bool>{false};
constinit auto s_hostAllocations = std::array<HostAllocation, MaxHostAllocations>{};
constinit auto s_hostAllocationsLive = std::atomic<std::uint32_t>{0};
constinit auto s_hostVisibleBytes = std::atomic<std::uint64_t>{0};
constinit auto s_hostAllocationsUntracked = std::atomic<std::uint64_t>{0};

class HostAllocationsLock
{
public:
  HostAllocationsLock()
  {
    while (s_hostAllocationsLock.exchange(true, std::memory_order_acquire))
    {
    }
  }
  ~HostAllocationsLock()
  {
    s_hostAllocationsLock.store(false, std::memory_order_release);
  }

  deleteCopyAndMove(HostAllocationsLock);
};

bool hostVisibleType(const std::uint32_t memoryTypeIndex)
{
  return memoryTypeIndex < 32
         && ((s_hostVisibleMemoryTypes.load() >> memoryTypeIndex) & 1u) != 0;
}

void trackHostAllocation(const VkDeviceMemory memory, const VkDeviceSize bytes)
{
  const auto lock = HostAllocationsLock{};
  for (auto& entry : s_hostAllocations)
  {
    if (entry.memory == VK_NULL_HANDLE)
    {
      entry = HostAllocation{memory, bytes};
      ++s_hostAllocationsLive;
      s_hostVisibleBytes += bytes;
      return;
    }
  }
  ++s_hostAllocationsUntracked;
}

void untrackHostAllocation(const VkDeviceMemory memory)
{
  if (s_hostAllocationsLive == 0)
  {
    return;
  }
  const auto lock = HostAllocationsLock{};
  for (auto& entry : s_hostAllocations)
  {
    if (entry.memory == memory)
    {
      s_hostVisibleBytes -= entry.bytes;
      --s_hostAllocationsLive;
      entry = HostAllocation{};
      return;
    }
  }
}

// Any thread that allocates through NVRHI: the render thread, the compile thread, the
// GUI thread in teardown.
VKAPI_ATTR VkResult VKAPI_CALL countingAllocateMemory(
  VkDevice device,
  const VkMemoryAllocateInfo* info,
  const VkAllocationCallbacks* allocator,
  VkDeviceMemory* memory)
{
  if (
    s_failHostAllocations && info && info->memoryTypeIndex < 32
    && ((s_hostOnlyMemoryTypes.load() >> info->memoryTypeIndex) & 1u) != 0)
  {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const auto result = s_allocateMemory.load()(device, info, allocator, memory);
  if (result == VK_SUCCESS)
  {
    ++s_liveAllocations;
    if (info && memory && hostVisibleType(info->memoryTypeIndex))
    {
      trackHostAllocation(*memory, info->allocationSize);
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL countingFreeMemory(
  VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
{
  if (memory != VK_NULL_HANDLE)
  {
    --s_liveAllocations;
    untrackHostAllocation(memory);
  }
  s_freeMemory.load()(device, memory, allocator);
}

// GUI thread, phase B, right after createVkDevice loaded the device's functions and
// before NVRHI makes its first allocation
void installAllocationCounter(const vk::PhysicalDevice physicalDevice)
{
  auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
  if (dispatcher.vkAllocateMemory == &countingAllocateMemory)
  {
    return;
  }
  auto hostOnly = std::uint32_t{0};
  auto hostVisible = std::uint32_t{0};
  const auto properties = physicalDevice.getMemoryProperties();
  for (std::uint32_t i = 0; i < properties.memoryTypeCount && i < 32; ++i)
  {
    const auto flags = properties.memoryTypes[i].propertyFlags;
    if (flags & vk::MemoryPropertyFlagBits::eHostVisible)
    {
      hostVisible |= 1u << i;
      if (!(flags & vk::MemoryPropertyFlagBits::eDeviceLocal))
      {
        hostOnly |= 1u << i;
      }
    }
  }
  s_hostOnlyMemoryTypes = hostOnly;
  {
    const auto lock = HostAllocationsLock{};
    s_hostAllocations = {};
    s_hostAllocationsLive = 0;
    s_hostVisibleBytes = 0;
    s_hostAllocationsUntracked = 0;
  }
  s_hostVisibleMemoryTypes = hostVisible;
  s_failHostAllocations = false;
  s_allocateMemory = dispatcher.vkAllocateMemory;
  s_freeMemory = dispatcher.vkFreeMemory;
  s_liveAllocations = 0;
  if (dispatcher.vkAllocateMemory && dispatcher.vkFreeMemory)
  {
    dispatcher.vkAllocateMemory = &countingAllocateMemory;
    dispatcher.vkFreeMemory = &countingFreeMemory;
  }
}

// GUI thread, teardown step 7, before the VkDevice goes: the dispatcher's own functions
// again
void removeAllocationCounter()
{
  auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
  if (dispatcher.vkAllocateMemory == &countingAllocateMemory)
  {
    dispatcher.vkAllocateMemory = s_allocateMemory.load();
    dispatcher.vkFreeMemory = s_freeMemory.load();
  }
}

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
             // C30: every allocation NVRHI makes on this device is counted
             installAllocationCounter(d.m_gpu.physicalDevice);

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

std::uint32_t GpuDevice::pushConstantLimit() const
{
  return enginePushConstantLimit(m_gpu.maxPushConstantsSize);
}

bool GpuDevice::pushConstantsEnabled(const EngineBindingLayout layout) const
{
  return enginePushConstantsEnabled(layout, pushConstantLimit(), m_gpu.vendorID);
}

std::int64_t GpuDevice::deviceMemoryAllocations() const
{
  return s_liveAllocations;
}

std::int64_t liveDeviceMemoryAllocations()
{
  return s_liveAllocations;
}

std::uint64_t liveHostVisibleBytes()
{
  return s_hostVisibleBytes;
}

void failHostAllocationsForTesting(const bool fail)
{
  s_failHostAllocations = fail;
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
    removeAllocationCounter();
    m_vkDevice.destroy();
    m_vkDevice = nullptr;
  }
}

} // namespace tb::lit
