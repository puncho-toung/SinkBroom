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
#include "mdl/TagMatcher.h"
#include "mdl/UtilityMaterialConfig.h"
#include "mdl/UtilityMaterials.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("UtilityMaterials")
{
  SECTION("materialNameMatchesPattern")
  {
    // a pattern with a slash is matched against the whole root-relative name
    CHECK(materialNameMatchesPattern("common/caulk", "common/caulk"));
    CHECK(!materialNameMatchesPattern("common/caulk", "editor/caulk"));
    CHECK(!materialNameMatchesPattern("caulk", "common/caulk"));
    CHECK(!materialNameMatchesPattern("base_wall/common/caulk", "common/caulk"));

    // a pattern without a slash is matched against the last component only
    CHECK(materialNameMatchesPattern("common/caulk", "caulk"));
    CHECK(materialNameMatchesPattern("caulk", "caulk"));
    CHECK(!materialNameMatchesPattern("common/caulk", "common"));

    // case is ignored, on both sides
    CHECK(materialNameMatchesPattern("common/entityGui2", "common/entitygui2"));
    CHECK(materialNameMatchesPattern("common/caulk", "COMMON/CAULK"));

    // globs
    CHECK(materialNameMatchesPattern("common/trigmulti", "common/trig*"));
    CHECK(materialNameMatchesPattern("common/player_clip", "common/*clip*"));
    CHECK(materialNameMatchesPattern("common/shadow2", "common/shadow?"));
    CHECK(!materialNameMatchesPattern("common/shadow", "common/shadow?"));
    CHECK(materialNameMatchesPattern("editor/visportal", "*/visportal"));
    CHECK(materialNameMatchesPattern("common/nodraw", "nodraw*"));
    CHECK(!materialNameMatchesPattern("base_wall/caulk", "common/*"));
  }

  SECTION("findUtilityMaterial")
  {
    const auto utilityMaterials = std::vector<UtilityMaterialConfig>{
      {.pattern = "common/caulk", .description = "Seals the map."},
      {.pattern = "common/*", .description = "Any common texture."},
      {.pattern = "editor/*", .description = ""},
    };

    // the first matching entry wins, so a material listed before the catch-all of its
    // folder gets its own entry
    CHECK(findUtilityMaterial(utilityMaterials, "common/caulk") == &utilityMaterials[0]);
    CHECK(findUtilityMaterial(utilityMaterials, "COMMON/Caulk") == &utilityMaterials[0]);
    CHECK(findUtilityMaterial(utilityMaterials, "common/nodraw") == &utilityMaterials[1]);
    CHECK(
      findUtilityMaterial(utilityMaterials, "editor/visportal") == &utilityMaterials[2]);

    // no match
    CHECK(findUtilityMaterial(utilityMaterials, "base_wall/caulk") == nullptr);
    CHECK(findUtilityMaterial(utilityMaterials, "caulk") == nullptr);

    // an empty list declares nothing
    CHECK(findUtilityMaterial({}, "common/caulk") == nullptr);

    // a catch-all listed first hides the entries after it
    const auto catchAllFirst = std::vector<UtilityMaterialConfig>{
      {.pattern = "common/*", .description = "Any common texture."},
      {.pattern = "common/caulk", .description = "Seals the map."},
    };
    CHECK(findUtilityMaterial(catchAllFirst, "common/caulk") == &catchAllFirst[0]);
  }

  SECTION("isUtilityMaterial")
  {
    const auto utilityMaterials = std::vector<UtilityMaterialConfig>{
      {.pattern = "common/caulk", .description = "Seals the map."},
      {.pattern = "editor/*", .description = ""},
    };

    CHECK(isUtilityMaterial(utilityMaterials, "common/caulk"));
    CHECK(isUtilityMaterial(utilityMaterials, "editor/visportal"));
    CHECK(!isUtilityMaterial(utilityMaterials, "common/nodraw"));
    CHECK(!isUtilityMaterial(utilityMaterials, "base_wall/lfwall13f3"));
    CHECK(!isUtilityMaterial({}, "common/caulk"));
  }
}

} // namespace tb::mdl
