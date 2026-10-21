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

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace tb::lit
{

/**
 * One swapchain generation (§3.1, S28): the swapchain, its image wrappers and its acquire
 * and present semaphores, created together and destroyed together. Destruction is split
 * (D49): the render thread releases the wrappers while it parks, the GUI thread destroys
 * the swapchain and the semaphores.
 */
struct SwapchainGeneration
{
  // 1, 2, ... within a session; the frame log's swapchain_generation
  std::uint64_t number = 0;
  vk::SwapchainKHR swapchain;
  vk::Extent2D extent;
  vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo;
  std::vector<vk::Image> images;
  // NVRHI handles of the images, released by the render thread while it parks
  std::vector<nvrhi::TextureHandle> imageTextures;
  // one per frame in flight: frame N reuses frame N - kFramesInFlight's, which the
  // render thread has waited for
  std::vector<vk::Semaphore> acquireSemaphores;
  // one per image: an image is acquired again only after its last present has waited
  std::vector<vk::Semaphore> presentSemaphores;

  // OUT_OF_DATE or SURFACE_LOST: nothing more is drawn until the GUI thread replaces it
  bool stale = false;
  // the recreate request was posted (once per generation)
  bool recreateRequested = false;
};

struct SwapchainSpec
{
  vk::PhysicalDevice physicalDevice;
  std::uint32_t queueFamily = 0;
  vk::Device device;
  nvrhi::IDevice* nvrhiDevice = nullptr;
  vk::SurfaceKHR surface;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool forceFifo = false;
  std::uint64_t number = 0;
};

/**
 * Creates a generation, on the GUI thread while the render thread is parked or not yet
 * running. A null result is a 0×0 extent: no swapchain, wait for the next expose. The
 * queue family's present support is checked first, because a screen change on a
 * hybrid-GPU laptop can take it away (R5.33).
 */
Result<std::unique_ptr<SwapchainGeneration>> createSwapchainGeneration(
  const SwapchainSpec& spec);

// The render thread's half (D49), after waitForIdle: drops the image wrappers.
void releaseSwapchainWrappers(SwapchainGeneration& generation);

// The GUI thread's half (D49): any wrappers left, vkDestroySwapchainKHR, the semaphores.
void destroySwapchainGeneration(vk::Device device, SwapchainGeneration& generation);

} // namespace tb::lit
