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

#include "mdl/IdTech4MaterialName.h"

#include <filesystem>
#include <string>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{

TEST_CASE("IdTech4MaterialName")
{
  SECTION("idTech4Path")
  {
    using T = std::tuple<std::string, std::string>;

    // clang-format off
    const auto
    [text,                                 expected] = GENERATE(values<T>({
    {"",                                   ""},
    {"textures/base_wall/foo.tga",         "textures/base_wall/foo.tga"},
    // the case and the extension are kept
    {"Textures/Base_Wall/Foo.TGA",         "Textures/Base_Wall/Foo.TGA"},
    {"textures\\base_wall\\foo.tga",       "textures/base_wall/foo.tga"},
    // bytes the narrow encoding may not convert become '_'
    {"textures/e\x81.tga",                 "textures/e_.tga"},
    {"textures/caf\xC3\xA9",               "textures/caf__"},
    }));
    // clang-format on

    CAPTURE(text);

    const auto path = idTech4Path(text);
    CHECK(path == std::filesystem::path{expected});
    CHECK(path.generic_string() == expected);
  }

  SECTION("canonicalIdTech4DeclName")
  {
    using T = std::tuple<std::string, std::string>;

    // clang-format off
    const auto
    [name,                                 expected] = GENERATE(values<T>({
    {"",                                   ""},
    {"textures/base_wall/foo",             "textures/base_wall/foo"},
    {"Textures/Base_Wall/Foo",             "textures/base_wall/foo"},
    {"Textures\\Base_Wall\\Foo.tga",       "textures/base_wall/foo"},
    {"models/mapobjects/x/y",              "models/mapobjects/x/y"},
    {"models/mapobjects/x/y.lwo",          "models/mapobjects/x/y"},
    {"fixture_Table",                      "fixture_table"},
    // the name is cut at its last dot, even when that dot is in a directory name
    {"textures/a.b/c",                     "textures/a"},
    {"textures/a.b/c.d",                   "textures/a.b/c"},
    {"textures/foo.",                      "textures/foo"},
    {".tga",                               ""},
    // only ASCII letters are lowercased; other ASCII characters stay as they are
    {"textures/X_1-2:3",                   "textures/x_1-2:3"},
    // bytes from 0x80 up become '_'
    {"textures/\xC3\x89" "cole.tga",       "textures/__cole"},
    }));
    // clang-format on

    CAPTURE(name);

    CHECK(canonicalIdTech4DeclName(name) == expected);
  }

  SECTION("canonicalIdTech4MaterialName")
  {
    using T = std::tuple<std::string, std::string>;

    // clang-format off
    const auto
    [name,                                 expected] = GENERATE(values<T>({
    {"",                                   ""},
    {"Textures\\Base_Wall\\Foo.tga",       "textures/base_wall/foo"},
    {"models/mapobjects/x/y.lwo",          "models/mapobjects/x/y"},
    {"textures/\xC3\x89" "cole.tga",       "textures/__cole"},
    }));
    // clang-format on

    CAPTURE(name);

    const auto canonical = canonicalIdTech4MaterialName(name);
    CHECK(canonical == std::filesystem::path{expected});
    CHECK(canonical.generic_string() == expected);
  }
}

} // namespace tb::mdl
