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

namespace tb::lit
{

std::optional<std::size_t> preferredDevice(const std::vector<DeviceCandidate>& candidates)
{
  auto firstOther = std::optional<std::size_t>{};
  for (std::size_t i = 0; i < candidates.size(); ++i)
  {
    const auto& candidate = candidates[i];
    if (!candidate.problems.empty())
    {
      continue;
    }
    if (candidate.discrete)
    {
      return i;
    }
    if (!firstOther)
    {
      firstOther = i;
    }
  }
  return firstOther;
}

std::string describeUnusableDevices(const std::vector<DeviceCandidate>& candidates)
{
  auto text = std::string{"No Vulkan device can run the lit preview."};
  if (candidates.empty())
  {
    text += " The Vulkan driver lists no device.";
  }
  for (const auto& candidate : candidates)
  {
    text += "\n" + candidate.name + ":";
    for (const auto& problem : candidate.problems)
    {
      text += "\n  - " + problem;
    }
  }
  return text;
}

} // namespace tb::lit
