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

namespace tb::lit
{

std::vector<const char*> requiredInstanceExtensions(const bool validation)
{
  // The engine also asks for VK_KHR_get_physical_device_properties2 (core since 1.1),
  // VK_EXT_sampler_filter_minmax (a device extension) and VK_EXT_debug_report (replaced
  // by debug utils); see mirror.json.
  if (validation)
  {
    return {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  }
  return {};
}

void initInstanceDispatcher(const InstanceHandles& handles)
{
  if (handles.instance != VK_NULL_HANDLE && handles.getInstanceProcAddr != nullptr)
  {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(handles.instance, handles.getInstanceProcAddr);
  }
}

void resetDispatcher()
{
  VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic{};
}

} // namespace tb::lit
