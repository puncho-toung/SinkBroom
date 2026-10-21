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

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

// The storage of vulkan.hpp's default dynamic dispatcher, through which NVRHI and
// SinkBroom make every Vulkan call. It must be defined in exactly one file of the
// program. NVRHI defines it only when it is built as a shared library
// (src/vulkan/vulkan-device.cpp), and the engine, which links it statically as SinkBroom
// does, defines it itself (neo/sys/DeviceManager_VK.cpp). It is constant-initialised to
// null function pointers until a session initialises it (C9).
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace tb::lit
{

bool nvrhiHeaderVersionMatches()
{
  return nvrhi::verifyHeaderVersion(nvrhi::c_HeaderVersion);
}

} // namespace tb::lit
