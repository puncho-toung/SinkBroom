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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ratio>
#include <string_view>

namespace tb::lit
{

/**
 * F1.20 (2026-11-05): the small helpers the library's sources each kept a copy of, once.
 * Internal (src/ only); pure; any thread.
 */

// idStr::Icmp == 0: the ASCII letters compared without case, every other byte as it is
// (no locale)
inline bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  const auto lower = [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  };
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [&](const char a, const char b) {
             return lower(a) == lower(b);
           });
}

// the hash combine of the binding-set and draw-record keys (boost's, 64-bit constant)
inline void combine(std::size_t& seed, const std::size_t value)
{
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

// the seconds and the milliseconds from start to its clock's now
template <typename TimePoint>
double secondsSince(const TimePoint start)
{
  return std::chrono::duration<double>(TimePoint::clock::now() - start).count();
}

template <typename TimePoint>
double millisecondsSince(const TimePoint start)
{
  return std::chrono::duration<double, std::milli>(TimePoint::clock::now() - start)
    .count();
}

} // namespace tb::lit
