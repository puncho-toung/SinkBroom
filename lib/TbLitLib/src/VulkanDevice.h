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

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace tb::lit
{

// The engine's swapchain format: RGBA8_UNORM, swapped to BGRA8 for Vulkan
// (neo/sys/DeviceManager.h:58, DeviceManager_VK.cpp:1307-1310).
constexpr auto SwapchainFormat = vk::Format::eB8G8R8A8Unorm;

// The physical device a session runs on, and what its VkDevice enables.
struct GpuChoice
{
  vk::PhysicalDevice physicalDevice;
  // the one family that does graphics and, with a surface, present
  std::uint32_t queueFamily = 0;
  std::string name;
  bool timerQueries = false;
  // the required device extensions and the supported optional ones
  std::vector<std::string> deviceExtensions;
  bool synchronization2 = false;
  // what the device supports of the engine's "if supported" features
  vk::PhysicalDeviceFeatures supportedFeatures;
  // C30 (P2.10, P2.13): the limits and vendor the engine's push-constant rule reads
  // (DeviceManager_VK.cpp:1377-1382, RenderProgs.cpp:403-413), and the driver's limit on
  // live device memory allocations (D72)
  std::uint32_t maxPushConstantsSize = 0;
  std::uint32_t vendorID = 0;
  std::uint32_t maxMemoryAllocationCount = 0;
  // DeviceManager_VK.cpp:1056-1064: whether D24S8 can be a depth-stencil attachment; the
  // depth target is D32S8 otherwise (Image_NVRHI.cpp:404-414)
  bool d24s8Supported = false;
  // the other devices whose queries failed, one line each, for the log
  std::vector<std::string> skippedDevices;
};

/**
 * §3.1 step 4: a device that presents to the surface, runs Vulkan 1.2 or newer and has
 * the engine's required extensions and features, discrete preferred. A null surface is
 * the headless test path: presentation is not checked and the swapchain extension is not
 * required. A device is asked for the surface's formats and present modes only once one
 * of its queue families presents to the surface, and a device whose queries fail is
 * skipped and listed in skippedDevices. Vulkan errors are returned, never thrown.
 */
Result<GpuChoice> pickPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface);

/**
 * §3.1 step 5 (C19): the VkDevice with the engine's extension list and feature chain, as
 * far as NVRHI and the engine's shaders need them (mirror.json). It loads the default
 * dispatcher's device functions for it.
 */
Result<vk::Device> createVkDevice(const GpuChoice& gpu);

} // namespace tb::lit
