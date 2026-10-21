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

#include "lit/NvrhiSupport.h"

#include <vulkan/vulkan.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("NvrhiSupport")
{
  SECTION("nvrhiHeaderVersionMatches")
  {
    CHECK(nvrhiHeaderVersionMatches());
  }

  SECTION("Vulkan dispatcher")
  {
    // the storage links, and nothing has loaded Vulkan or initialised it
    CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr == nullptr);
#ifdef _WIN32
    CHECK(GetModuleHandleW(L"vulkan-1.dll") == nullptr);
#endif
  }
}

} // namespace tb::lit
