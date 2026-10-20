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

#include <cstddef>
#include <cstdint>

namespace tb::mdl
{
namespace
{

constexpr auto OffsetBasis = std::uint64_t{14695981039346656037u};
constexpr auto Prime = std::uint64_t{1099511628211u};

constexpr auto CR = '\r';
constexpr auto LF = '\n';

std::uint64_t addByte(const std::uint64_t hash, const char c)
{
  return (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(c))) * Prime;
}

std::string toHex(const std::uint64_t hash)
{
  return fmt::format("{:016x}", hash);
}

} // namespace

std::string fnv1a64Hex(const std::string_view bytes)
{
  auto hash = OffsetBasis;
  for (const auto c : bytes)
  {
    hash = addByte(hash, c);
  }
  return toHex(hash);
}

std::string fnv1a64HexNormalizedLineEndings(const std::string_view text)
{
  auto hash = OffsetBasis;
  for (std::size_t i = 0; i < text.size(); ++i)
  {
    // the CR of a CR LF is skipped, without copying the text
    if (text[i] != CR || i + 1 == text.size() || text[i + 1] != LF)
    {
      hash = addByte(hash, text[i]);
    }
  }
  return toHex(hash);
}

} // namespace tb::mdl
