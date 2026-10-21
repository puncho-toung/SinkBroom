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

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tb::lit
{

/**
 * The present mode of a swapchain generation (D48, R5.4): MAILBOX when the surface offers
 * it, else FIFO, the only mode Vulkan guarantees. forceFifo picks FIFO whatever is
 * offered, for the forced FIFO-only exit test of LP0.
 */
VkPresentModeKHR choosePresentMode(
  std::span<const VkPresentModeKHR> offered, bool forceFifo);

/**
 * The extent of a swapchain generation (§3.1). It is the surface's currentExtent, except
 * that currentExtent 0xFFFFFFFF (Wayland) means the surface takes the window's size, so
 * the passed size is used, clamped to the surface's limits. An empty result means a 0×0
 * extent (a minimised window): make no swapchain and wait for the next expose.
 */
std::optional<VkExtent2D> chooseSwapchainExtent(
  const VkSurfaceCapabilitiesKHR& capabilities,
  std::uint32_t width,
  std::uint32_t height);

/**
 * The number of swapchain images to ask for: the engine's NUM_FRAME_DATA (3), clamped to
 * the surface's range; a maxImageCount of 0 means no upper limit.
 */
std::uint32_t chooseSwapchainImageCount(const VkSurfaceCapabilitiesKHR& capabilities);

/**
 * The frame log's name for a present mode; "headless" stands for a session without a
 * surface.
 */
std::string_view presentModeName(std::optional<VkPresentModeKHR> presentMode);

} // namespace tb::lit
