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

#include "lit/VulkanInstance.h"

#include <vulkan/vulkan.hpp>

#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("VulkanInstance")
{
  SECTION("requiredInstanceExtensions")
  {
    CHECK(requiredInstanceExtensions(false).empty());

    const auto withValidation = requiredInstanceExtensions(true);
    REQUIRE(withValidation.size() == 1u);
    CHECK(std::string_view{withValidation[0]} == VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  SECTION("initInstanceDispatcher")
  {
    // without an instance it loads nothing and calls nothing
    initInstanceDispatcher({});
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr == nullptr);
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices == nullptr);
  }

  SECTION("resetDispatcher")
  {
    resetDispatcher();
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr == nullptr);
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice == nullptr);
  }
}

} // namespace tb::lit
