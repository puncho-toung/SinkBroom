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

#include <string>
#include <string_view>

namespace tb::mdl
{

/**
 * Returns the 64-bit FNV-1a hash of the given bytes as 16 lower-case hexadecimal digits.
 *
 * Unlike std::hash, the result is the same on every platform and in every build, so it
 * can be stored in a file and compared later, for example to tell whether a prefab file
 * has changed since it was placed (_tb_prefab_hash).
 */
std::string fnv1a64Hex(std::string_view bytes);

} // namespace tb::mdl
