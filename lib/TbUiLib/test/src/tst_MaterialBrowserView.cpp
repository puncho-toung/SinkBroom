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

#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "ui/CatchConfig.h"
#include "ui/MaterialBrowserView.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MaterialBrowserView")
{
  SECTION("filterMaterials")
  {
    auto used = gl::Material{"base/used", gl::createTextureResource(gl::Texture{4, 4})};
    used.incUsageCount();

    auto missing =
      gl::Material{"base/missing", gl::createTextureResource(gl::Texture{4, 4})};
    missing.setPreviewImageMissing(true);

    const auto plain =
      gl::Material{"other/plain", gl::createTextureResource(gl::Texture{4, 4})};

    const auto all = std::vector<const gl::Material*>{&used, &missing, &plain};

    CHECK(filterMaterials(all, false, false, "") == all);
    CHECK(
      filterMaterials(all, true, false, "") == std::vector<const gl::Material*>{&used});
    CHECK(
      filterMaterials(all, false, true, "")
      == std::vector<const gl::Material*>{&used, &plain});
    CHECK(
      filterMaterials(all, true, true, "") == std::vector<const gl::Material*>{&used});

    // a name must contain one of the words, ignoring case
    CHECK(
      filterMaterials(all, false, false, "BASE")
      == std::vector<const gl::Material*>{&used, &missing});
    CHECK(
      filterMaterials(all, false, true, "base plain")
      == std::vector<const gl::Material*>{&used, &plain});
  }

  SECTION("materialBorder")
  {
    const auto plain =
      gl::Material{"base/plain", gl::createTextureResource(gl::Texture{4, 4})};

    auto used = gl::Material{"base/used", gl::createTextureResource(gl::Texture{4, 4})};
    used.incUsageCount();

    auto translucent =
      gl::Material{"base/glass", gl::createTextureResource(gl::Texture{4, 4})};
    translucent.setSurfaceParms({"coverage_translucent"});

    auto usedTranslucent =
      gl::Material{"base/decal", gl::createTextureResource(gl::Texture{4, 4})};
    usedTranslucent.setSurfaceParms({"coverage_translucent", "nonsolid"});
    usedTranslucent.incUsageCount();

    // other surface parms don't matter, including the coverage of an alpha tested one
    auto grate = gl::Material{"base/grate", gl::createTextureResource(gl::Texture{4, 4})};
    grate.setSurfaceParms({"coverage_perforated", "metal"});

    CHECK(materialBorder(plain, false) == MaterialBorder::Default);
    CHECK(materialBorder(grate, false) == MaterialBorder::Default);
    CHECK(materialBorder(used, false) == MaterialBorder::Used);
    CHECK(materialBorder(translucent, false) == MaterialBorder::Translucent);

    // the leak warning wins over the usage
    CHECK(materialBorder(usedTranslucent, false) == MaterialBorder::Translucent);

    // the selection wins over everything
    CHECK(materialBorder(plain, true) == MaterialBorder::Selected);
    CHECK(materialBorder(usedTranslucent, true) == MaterialBorder::Selected);
  }
}

} // namespace tb::ui
