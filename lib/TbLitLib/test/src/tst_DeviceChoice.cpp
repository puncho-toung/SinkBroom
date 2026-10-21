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

#include "lit/DeviceChoice.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{

TEST_CASE("DeviceChoice")
{
  SECTION("preferredDevice")
  {
    const auto integrated = DeviceCandidate{"Intel(R) UHD Graphics", false, {}};
    const auto discrete = DeviceCandidate{"NVIDIA GeForce RTX 2070", true, {}};
    const auto broken = DeviceCandidate{"Old GPU", true, {"runs Vulkan 1.1.0"}};

    SECTION("a discrete GPU wins, wherever it is listed")
    {
      CHECK(preferredDevice({integrated, discrete}) == 1u);
      CHECK(preferredDevice({discrete, integrated}) == 0u);
    }

    SECTION("a device with problems is never picked")
    {
      CHECK(preferredDevice({broken, integrated}) == 1u);
      CHECK_FALSE(preferredDevice({broken}));
    }

    SECTION("the first of several others, in enumeration order")
    {
      auto second = integrated;
      second.name = "second";
      CHECK(preferredDevice({integrated, second}) == 0u);
    }

    SECTION("no device")
    {
      CHECK_FALSE(preferredDevice({}));
    }
  }

  SECTION("describeUnusableDevices")
  {
    const auto text = describeUnusableDevices(
      {{"Old GPU",
        true,
        {"runs Vulkan 1.1.0, the preview needs 1.2", "missing VK_KHR_swapchain"}},
       {"Other", false, {"has no graphics queue family"}}});
    CHECK(
      text
      == "No Vulkan device can run the lit preview.\n"
         "Old GPU:\n  - runs Vulkan 1.1.0, the preview needs 1.2\n  - missing VK_KHR_swapchain\n"
         "Other:\n  - has no graphics queue family");

    CHECK_THAT(
      describeUnusableDevices({}), Catch::Matchers::ContainsSubstring("lists no device"));
  }
}

} // namespace tb::lit
