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

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

VkSurfaceCapabilitiesKHR capabilities(
  const VkExtent2D current,
  const VkExtent2D min = {1, 1},
  const VkExtent2D max = {16384, 16384},
  const std::uint32_t minImages = 2,
  const std::uint32_t maxImages = 8)
{
  auto result = VkSurfaceCapabilitiesKHR{};
  result.currentExtent = current;
  result.minImageExtent = min;
  result.maxImageExtent = max;
  result.minImageCount = minImages;
  result.maxImageCount = maxImages;
  return result;
}

std::pair<std::uint32_t, std::uint32_t> size(const VkExtent2D& extent)
{
  return {extent.width, extent.height};
}

} // namespace

TEST_CASE("SwapchainRules")
{
  SECTION("choosePresentMode")
  {
    using Modes = std::vector<VkPresentModeKHR>;

    SECTION("MAILBOX when offered")
    {
      const auto offered = Modes{
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR};
      CHECK(choosePresentMode(offered, false) == VK_PRESENT_MODE_MAILBOX_KHR);
    }

    SECTION("FIFO otherwise, never IMMEDIATE (R5.4)")
    {
      CHECK(
        choosePresentMode(
          Modes{VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR}, false)
        == VK_PRESENT_MODE_FIFO_KHR);
      CHECK(
        choosePresentMode(
          Modes{VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR}, false)
        == VK_PRESENT_MODE_FIFO_KHR);
      CHECK(
        choosePresentMode(Modes{VK_PRESENT_MODE_FIFO_KHR}, false)
        == VK_PRESENT_MODE_FIFO_KHR);
    }

    SECTION("FIFO when an empty list was returned")
    {
      CHECK(choosePresentMode(Modes{}, false) == VK_PRESENT_MODE_FIFO_KHR);
    }

    SECTION("forceFifo ignores MAILBOX (D48)")
    {
      CHECK(
        choosePresentMode(
          Modes{VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR}, true)
        == VK_PRESENT_MODE_FIFO_KHR);
    }
  }

  SECTION("chooseSwapchainExtent")
  {
    SECTION("currentExtent wins over the passed size")
    {
      const auto extent = chooseSwapchainExtent(capabilities({800, 600}), 1024, 768);
      REQUIRE(extent);
      CHECK(size(*extent) == std::pair{800u, 600u});
    }

    SECTION("0xFFFFFFFF takes the passed size")
    {
      const auto extent =
        chooseSwapchainExtent(capabilities({0xFFFFFFFF, 0xFFFFFFFF}), 1024, 768);
      REQUIRE(extent);
      CHECK(size(*extent) == std::pair{1024u, 768u});
    }

    SECTION("0xFFFFFFFF clamps the passed size to the surface's limits")
    {
      const auto caps = capabilities({0xFFFFFFFF, 0xFFFFFFFF}, {64, 64}, {2048, 1024});
      const auto small = chooseSwapchainExtent(caps, 10, 20);
      REQUIRE(small);
      CHECK(size(*small) == std::pair{64u, 64u});

      const auto large = chooseSwapchainExtent(caps, 4000, 4000);
      REQUIRE(large);
      CHECK(size(*large) == std::pair{2048u, 1024u});
    }

    SECTION("0×0 means no swapchain")
    {
      CHECK_FALSE(chooseSwapchainExtent(capabilities({0, 0}, {0, 0}, {0, 0}), 800, 600));
      CHECK_FALSE(chooseSwapchainExtent(capabilities({800, 0}), 800, 600));
    }

    SECTION("0xFFFFFFFF with a zero passed size means no swapchain")
    {
      const auto caps = capabilities({0xFFFFFFFF, 0xFFFFFFFF}, {1, 1});
      CHECK_FALSE(chooseSwapchainExtent(caps, 0, 0));
      CHECK_FALSE(chooseSwapchainExtent(caps, 640, 0));
    }
  }

  SECTION("chooseSwapchainImageCount")
  {
    // the engine's NUM_FRAME_DATA, clamped
    CHECK(chooseSwapchainImageCount(capabilities({1, 1}, {1, 1}, {1, 1}, 2, 8)) == 3u);
    CHECK(chooseSwapchainImageCount(capabilities({1, 1}, {1, 1}, {1, 1}, 4, 8)) == 4u);
    CHECK(chooseSwapchainImageCount(capabilities({1, 1}, {1, 1}, {1, 1}, 1, 2)) == 2u);
    // no upper limit
    CHECK(chooseSwapchainImageCount(capabilities({1, 1}, {1, 1}, {1, 1}, 2, 0)) == 3u);
  }

  SECTION("presentModeName")
  {
    CHECK(presentModeName(VK_PRESENT_MODE_MAILBOX_KHR) == "mailbox");
    CHECK(presentModeName(VK_PRESENT_MODE_FIFO_KHR) == "fifo");
    CHECK(presentModeName(std::nullopt) == "headless");
  }
}

} // namespace tb::lit
