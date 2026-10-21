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

#include "LiveSession.h"

#include <vulkan/vulkan.hpp>

#include <atomic>

namespace tb::lit
{
namespace
{

// constant-initialised; no session exists before the first Start (C9)
constinit auto liveSession = std::atomic<bool>{false};

} // namespace

bool claimLiveSession()
{
  auto expected = false;
  return liveSession.compare_exchange_strong(expected, true);
}

void releaseLiveSession()
{
  liveSession = false;
}

bool liveSessionExists()
{
  return liveSession;
}

bool initInstanceDispatcher(const InstanceHandles& handles)
{
  if (liveSessionExists())
  {
    return false;
  }
  if (handles.instance != VK_NULL_HANDLE && handles.getInstanceProcAddr != nullptr)
  {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(handles.instance, handles.getInstanceProcAddr);
  }
  return true;
}

bool resetDispatcher()
{
  if (liveSessionExists())
  {
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic{};
  return true;
}

} // namespace tb::lit
