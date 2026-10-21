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

#include "lit/SwapchainRules.h"

#include <algorithm>

namespace tb::lit
{
namespace
{

// the engine's swapChainBufferCount default, NUM_FRAME_DATA (neo/idlib/precompiled.h:95)
constexpr auto kEngineSwapchainImageCount = std::uint32_t{3};

// Vulkan's value of currentExtent for a surface whose size the swapchain decides
constexpr auto kUndefinedExtent = std::uint32_t{0xFFFFFFFF};

} // namespace

VkPresentModeKHR choosePresentMode(
  const std::span<const VkPresentModeKHR> offered, const bool forceFifo)
{
  if (
    !forceFifo
    && std::ranges::find(offered, VK_PRESENT_MODE_MAILBOX_KHR) != offered.end())
  {
    return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

std::optional<VkExtent2D> chooseSwapchainExtent(
  const VkSurfaceCapabilitiesKHR& capabilities,
  const std::uint32_t width,
  const std::uint32_t height)
{
  auto extent = capabilities.currentExtent;
  if (extent.width == kUndefinedExtent || extent.height == kUndefinedExtent)
  {
    // clamp only a non-zero size: a zero size stays zero and means "wait"
    extent.width =
      width == 0
        ? 0
        : std::clamp(
            width,
            capabilities.minImageExtent.width,
            std::max(
              capabilities.minImageExtent.width, capabilities.maxImageExtent.width));
    extent.height =
      height == 0
        ? 0
        : std::clamp(
            height,
            capabilities.minImageExtent.height,
            std::max(
              capabilities.minImageExtent.height, capabilities.maxImageExtent.height));
  }

  if (extent.width == 0 || extent.height == 0)
  {
    return std::nullopt;
  }
  return extent;
}

std::uint32_t chooseSwapchainImageCount(const VkSurfaceCapabilitiesKHR& capabilities)
{
  auto count = std::max(kEngineSwapchainImageCount, capabilities.minImageCount);
  if (capabilities.maxImageCount > 0)
  {
    count = std::min(count, capabilities.maxImageCount);
  }
  return count;
}

std::string_view presentModeName(const std::optional<VkPresentModeKHR> presentMode)
{
  if (!presentMode)
  {
    return "headless";
  }

  switch (*presentMode)
  {
  case VK_PRESENT_MODE_MAILBOX_KHR:
    return "mailbox";
  case VK_PRESENT_MODE_FIFO_KHR:
    return "fifo";
  case VK_PRESENT_MODE_IMMEDIATE_KHR:
    return "immediate";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    return "fifo_relaxed";
  default:
    return "other";
  }
}

} // namespace tb::lit
