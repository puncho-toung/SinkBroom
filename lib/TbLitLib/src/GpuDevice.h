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
#include "lit/VulkanInstance.h"

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace tb::lit
{
class SessionEvents;

/**
 * NVRHI's message callback (R5.19). NVRHI's submit catches DeviceLostError itself and
 * reports it only here, so an Error-severity message marks the device lost, which the
 * render thread checks after every executeCommandLists. Warnings go to the log. It is
 * called on whichever thread made the NVRHI call.
 */
class MessageCallback : public nvrhi::IMessageCallback
{
private:
  SessionEvents& m_events;
  std::atomic<std::size_t> m_errorCount = 0;

public:
  explicit MessageCallback(SessionEvents& events);

  void message(nvrhi::MessageSeverity severity, const char* messageText) override;

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

  // step 5: drops the NVRHI device, its validation wrapper first, and returns what its
  // last Release() returned; 0 expected
  std::uint32_t releaseNvrhiDevice();
  // step 7, the device part
  void destroyVkDevice();

private:
  explicit GpuDevice(SessionEvents& events);

  deleteCopyAndMove(GpuDevice);
};

} // namespace tb::lit
