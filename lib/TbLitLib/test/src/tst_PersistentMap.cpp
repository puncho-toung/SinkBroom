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

#include "lit/PersistentMap.h"

#include <fmt/format.h>

#include <functional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{

TEST_CASE("PersistentMap")
{
  auto map = PersistentMap<std::string, int>{};
  for (int i = 0; i < 5000; ++i)
  {
    map = map.set(fmt::format("key{}", i), i);
  }
  CHECK(map.size() == 5000u);
  REQUIRE(map.find("key4321"));
  CHECK(*map.find("key4321") == 4321);

  // a new version shares the old, which is unchanged
  const auto older = map;
  const auto newer = map.set("key4321", -1).erase("key0");
  CHECK(*older.find("key4321") == 4321);
  CHECK(older.find("key0"));
  CHECK(*newer.find("key4321") == -1);
  CHECK_FALSE(newer.find("key0"));
  CHECK(newer.size() == 4999u);
  CHECK(newer.erase("absent").sharesRootWith(newer));

  auto sum = 0ll;
  newer.forEach([&](const auto&, const int value) { sum += value; });
  CHECK(sum == 4999ll * 5000 / 2 - 4321 - 1);
}

TEST_CASE("PersistentMap with a transparent hash")
{
  auto map = PersistentMap<std::string, int, StringHash, std::equal_to<>>{};
  for (int i = 0; i < 500; ++i)
  {
    map = map.set(fmt::format("a_key_longer_than_a_small_string_{}", i), i);
  }

  // a view finds what its string finds, without a std::string made for the call
  const auto key = std::string{"a_key_longer_than_a_small_string_321"};
  const auto view = std::string_view{key};
  REQUIRE(map.find(view));
  CHECK(map.find(view) == map.find(key));
  CHECK(*map.find(view) == 321);
  CHECK_FALSE(map.find(std::string_view{"a_key_longer_than_a_small_string_"}));
  CHECK(StringHash{}(key) == std::hash<std::string>{}(key));

  const auto erased = map.erase(key);
  CHECK_FALSE(erased.find(view));
  CHECK(map.find(view));
}

} // namespace tb::lit
