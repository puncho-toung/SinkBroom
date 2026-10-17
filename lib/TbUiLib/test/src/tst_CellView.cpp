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

#include "gl/AttrString.h"
#include "ui/CatchConfig.h"
#include "ui/CellView.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

gl::AttrString centred(const std::vector<std::string>& lines)
{
  auto result = gl::AttrString{};
  for (const auto& line : lines)
  {
    result.appendCentered(line);
  }
  return result;
}

} // namespace

TEST_CASE("CellView")
{
  SECTION("cellTitleString")
  {
    // a one-line title, which is what every cell had before, is one centred line
    CHECK(cellTitleString("caulk") == centred({"caulk"}));
    CHECK(cellTitleString("caulk") != gl::AttrString{"caulk"});

    // one centred line per line, in order
    CHECK(
      cellTitleString("caulk\nSeals the map.") == centred({"caulk", "Seals the map."}));
    CHECK(
      cellTitleString("visportal\nArea portal: seals the\nboundary")
      == centred({"visportal", "Area portal: seals the", "boundary"}));

    // empty lines count, as the layout's measure counts them
    CHECK(cellTitleString("a\n\nb") == centred({"a", "", "b"}));
    CHECK(cellTitleString("a\n") == centred({"a", ""}));

    // an empty title has no lines
    CHECK(cellTitleString("") == gl::AttrString{});
  }
}

} // namespace tb::ui
