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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

// A physical device as the device choice sees it: usable when it has no problems.
struct DeviceCandidate
{
  std::string name;
  bool discrete = false;
  std::vector<std::string> problems;
};

/**
 * The device a session runs on (§3.1 step 4): the first usable discrete GPU, else the
 * first usable other device, in enumeration order as the engine picks
 * (DeviceManager_VK.cpp:779-794).
 */
std::optional<std::size_t> preferredDevice(
  const std::vector<DeviceCandidate>& candidates);

// The error text when no candidate is usable: every device with its problems.
std::string describeUnusableDevices(const std::vector<DeviceCandidate>& candidates);

} // namespace tb::lit
