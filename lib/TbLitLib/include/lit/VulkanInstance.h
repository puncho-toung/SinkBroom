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

#include <string>
#include <vector>

namespace tb::lit
{

/**
 * The Vulkan instance a session runs on. The instance belongs to its creator (Qt's
 * QVulkanInstance, or a test), which loaded the Vulkan library at runtime (C9) and
 * destroys the instance after the session's last teardown step.
 */
struct InstanceHandles
{
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  // the instance extensions it was created with (QVulkanInstance::extensions()); NVRHI
  // uses VK_EXT_debug_utils for object names and markers only when it is listed here
  std::vector<std::string> enabledExtensions;
};

/**
 * Loads vulkan.hpp's default dispatcher for this instance; done on every Start
 * (R4-2.22). False, and nothing loaded, while a session is live: the dispatcher is
 * process-wide, so only one session exists at a time.
 */
bool initInstanceDispatcher(const InstanceHandles& handles);

/**
 * Clears the default dispatcher back to null pointers, after the instance is destroyed.
 * False, and nothing cleared, while a session is live.
 */
bool resetDispatcher();

} // namespace tb::lit
