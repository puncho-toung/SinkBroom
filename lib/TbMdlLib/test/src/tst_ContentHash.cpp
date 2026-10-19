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

#include "mdl/CatchConfig.h"
#include "mdl/ContentHash.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("ContentHash")
{
  SECTION("fnv1a64Hex")
  {
    // reference vectors of the 64-bit FNV-1a hash
    CHECK(fnv1a64Hex("") == "cbf29ce484222325");
    CHECK(fnv1a64Hex("a") == "af63dc4c8601ec8c");
    CHECK(fnv1a64Hex("foobar") == "85944171f73967e8");

    // every byte counts, including bytes above 0x7f and embedded zeros
    CHECK(fnv1a64Hex(std::string{"a\0b", 3}) != fnv1a64Hex("ab"));
    CHECK(fnv1a64Hex("\xff") != fnv1a64Hex("\x7f"));

    // always 16 lower-case hex digits
    const auto hash = fnv1a64Hex("// Game: Doom3BFG\n");
    CHECK(hash.size() == 16);
    CHECK(hash.find_first_not_of("0123456789abcdef") == std::string::npos);
  }
}

} // namespace tb::mdl
