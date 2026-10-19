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

#include "mdl/ContentHash.h"

#include <fmt/format.h>

#include <cstdint>

namespace tb::mdl
{

std::string fnv1a64Hex(const std::string_view bytes)
{
  constexpr auto offsetBasis = std::uint64_t{14695981039346656037u};
  constexpr auto prime = std::uint64_t{1099511628211u};

  auto hash = offsetBasis;
  for (const auto c : bytes)
  {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= prime;
  }

  return fmt::format("{:016x}", hash);
}

} // namespace tb::mdl
