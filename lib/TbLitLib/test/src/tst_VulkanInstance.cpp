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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{

TEST_CASE("VulkanInstance")
{
  SECTION("initInstanceDispatcher")
  {
    // without an instance it loads nothing and calls nothing
    CHECK(initInstanceDispatcher({}));
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr == nullptr);
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices == nullptr);
  }

  SECTION("resetDispatcher")
  {
    CHECK(resetDispatcher());
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr == nullptr);
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice == nullptr);
  }

  SECTION("create: a loader that cannot be loaded (LPF.2, D107)")
  {
    // nothing of that name exists: the reason names the loader (no GPU needed)
    auto options = VulkanInstanceOptions{};
    options.loaderName = "tb-lit-no-such-vulkan-loader.dll";
    options.validation = true;
    auto result = VulkanInstance::create(std::move(options));
    REQUIRE(result.is_error());
    CHECK_THAT(
      std::get<Error>(result.error()).msg,
      Catch::Matchers::StartsWith(
        "Cannot load the Vulkan loader tb-lit-no-such-vulkan-loader.dll"));
  }
}

} // namespace tb::lit
