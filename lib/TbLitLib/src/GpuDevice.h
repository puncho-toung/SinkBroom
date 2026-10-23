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

#include "VulkanDevice.h"
#include "base/Macros.h"
#include "base/Result.h"
#include "lit/EngineBuiltins.h"
#include "lit/LitSession.h"
#include "lit/VulkanInstance.h"

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace tb::lit
{
class SessionEvents;

/**
 * NVRHI's message callback (R5.19). NVRHI's submit catches DeviceLostError itself and
 * reports it only here, as "Device Removed!", which marks the device lost; the render
 * thread checks after every executeCommandLists. Any other Error-severity message, for
 * example from NVRHI's validation layer, is counted and fails the session once, so that
 * the device is not reported lost and garbage is still collected at teardown. Warnings go
 * to the log. It is called on whichever thread made the NVRHI call, and never throws.
 */
class MessageCallback : public nvrhi::IMessageCallback
{
private:
  SessionEvents& m_events;
  std::atomic<std::size_t> m_errorCount = 0;

public:
  explicit MessageCallback(SessionEvents& events);

  void message(
    nvrhi::MessageSeverity severity, const char* messageText) noexcept override;

  std::size_t errorCount() const;
};

/**
 * The GPU a session runs on (§3.1 phase B steps 4-6): the physical device, its VkDevice
 * and queue, and the NVRHI device on them, with NVRHI's validation layer in Debug when
 * the session validates. The teardown releases the NVRHI device (step 5) and destroys the
 * VkDevice (step 7) in explicit calls; the destructor runs those that have not run.
 */
class GpuDevice
{
private:
  // declared before the device it serves (R5.19)
  MessageCallback m_messageCallback;
  GpuChoice m_gpu;
  vk::Device m_vkDevice;
  vk::Queue m_queue;
  nvrhi::vulkan::DeviceHandle m_nvrhiDevice;
  // Debug with validation only
  nvrhi::DeviceHandle m_validationDevice;
  // m_validationDevice, else m_nvrhiDevice
  nvrhi::IDevice* m_device = nullptr;

public:
  static Result<std::unique_ptr<GpuDevice>> create(
    const InstanceHandles& instance,
    vk::SurfaceKHR surface,
    bool validation,
    SessionEvents& events);

  ~GpuDevice();

  const GpuChoice& gpu() const;
  vk::Device vkDevice() const;
  vk::Queue queue() const;

  // the device to record with; valid until releaseNvrhiDevice
  nvrhi::IDevice& device() const;
  // NVRHI's Vulkan device under it, for the queue's semaphore calls
  nvrhi::vulkan::IDevice& vulkanDevice() const;

  std::size_t nvrhiErrors() const;
  // the device-local heaps' usage and budget, when the device has VK_EXT_memory_budget
  std::optional<VramUsage> vram() const;

  // C30: the engine's push-constant budget on this device, min(maxPushConstantsSize,
  // NVRHI's c_MaxPushConstantSize), and whether a binding layout uses push constants
  // (pcEnabled with this device's limit and vendor). Any thread.
  std::uint32_t pushConstantLimit() const;
  bool pushConstantsEnabled(EngineBindingLayout layout) const;

  /**
   * C30, D72 (P2.13): the device memory allocations live now, counted by wrapping the
   * dispatcher's vkAllocateMemory and vkFreeMemory from just after the VkDevice's
   * functions are loaded until it is destroyed, so every NVRHI allocation is seen
   * (textures, buffers, upload chunks, staging). The counter is process-wide like the
   * dispatcher, which one session at a time owns. Any thread; for the frame log.
   */
  std::int64_t deviceMemoryAllocations() const;
  // for the tests' faults, which arrive as NVRHI's would
  MessageCallback& messageCallback();

  // step 5: drops the NVRHI device, its validation wrapper first, and returns what its
  // last Release() returned; 0 expected
  std::uint32_t releaseNvrhiDevice();
  // step 7, the device part
  void destroyVkDevice();

private:
  explicit GpuDevice(SessionEvents& events);

  deleteCopyAndMove(GpuDevice);
};

// LP2.4, any thread: the process-wide counter of deviceMemoryAllocations, also once its
// session has gone: what that session's teardown left; tests and measurements only
std::int64_t liveDeviceMemoryAllocations();

// U2.3, any thread: while set, every device memory allocation in a type that is
// host-visible and not device-local fails (NVRHI's upload chunks and staging); tests only
void failHostAllocationsForTesting(bool fail);

} // namespace tb::lit
