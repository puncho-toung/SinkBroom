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
#include "mdl/UtilityMaterialConfig.h"
#include "ui/CatchConfig.h"
#include "ui/MaterialBrowserScope.h"
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

    // the scope sorts out the game's utility materials
    auto caulk =
      gl::Material{"common/caulk", gl::createTextureResource(gl::Texture{4, 4})};
    caulk.incUsageCount();
    const auto nodraw =
      gl::Material{"common/nodraw", gl::createTextureResource(gl::Texture{4, 4})};
    const auto withUtility =
      std::vector<const gl::Material*>{&used, &caulk, &plain, &nodraw};
    const auto utilityMaterials = std::vector<mdl::UtilityMaterialConfig>{
      {.pattern = "common/caulk", .description = "Seals the map."},
      {.pattern = "common/*", .description = ""},
    };

    // All, the default, ignores the list
    CHECK(filterMaterials(withUtility, false, false, "") == withUtility);
    CHECK(
      filterMaterials(
        withUtility, false, false, "", MaterialBrowserScope::All, utilityMaterials)
      == withUtility);

    // Regular hides the utility materials, Utility lists them alone
    CHECK(
      filterMaterials(
        withUtility, false, false, "", MaterialBrowserScope::Regular, utilityMaterials)
      == std::vector<const gl::Material*>{&used, &plain});
    CHECK(
      filterMaterials(
        withUtility, false, false, "", MaterialBrowserScope::Utility, utilityMaterials)
      == std::vector<const gl::Material*>{&caulk, &nodraw});

    // the other filters apply within the scope
    CHECK(
      filterMaterials(
        withUtility, true, false, "", MaterialBrowserScope::Utility, utilityMaterials)
      == std::vector<const gl::Material*>{&caulk});
    CHECK(
      filterMaterials(
        withUtility,
        false,
        false,
        "NODRAW",
        MaterialBrowserScope::Utility,
        utilityMaterials)
      == std::vector<const gl::Material*>{&nodraw});
    CHECK(filterMaterials(
            withUtility,
            false,
            false,
            "common",
            MaterialBrowserScope::Regular,
            utilityMaterials)
            .empty());

    // a game without utility materials: the regular browser lists everything, the
    // utility browser nothing
    CHECK(
      filterMaterials(withUtility, false, false, "", MaterialBrowserScope::Regular, {})
      == withUtility);
    CHECK(
      filterMaterials(withUtility, false, false, "", MaterialBrowserScope::Utility, {})
        .empty());
  }

  SECTION("wrapWords")
  {
    // every character 7 units wide
    const auto measureWidth = [](const std::string& text) {
      return float(text.size()) * 7.0f;
    };

    // a text that fits stays on one line
    CHECK(wrapWords("Seals the map.", 252.0f, measureWidth) == "Seals the map.");
    CHECK(wrapWords("Seals the map.", 98.0f, measureWidth) == "Seals the map.");

    // a long text breaks between words, each line as long as it can be
    CHECK(
      wrapWords("Invisible and solid; seals the map.", 70.0f, measureWidth)
      == "Invisible\nand solid;\nseals the\nmap.");
    CHECK(
      wrapWords("Invisible and solid; seals the map.", 140.0f, measureWidth)
      == "Invisible and solid;\nseals the map.");

    // a word wider than a line stands alone on its own line
    CHECK(
      wrapWords("a trigger_once_entityname b", 35.0f, measureWidth)
      == "a\ntrigger_once_entityname\nb");
    CHECK(
      wrapWords("trigger_once_entityname", 35.0f, measureWidth)
      == "trigger_once_entityname");

    // an empty text has no lines
    CHECK(wrapWords("", 70.0f, measureWidth).empty());
    CHECK(wrapWords("   ", 70.0f, measureWidth).empty());

    // repeated, leading and trailing spaces do not make empty words
    CHECK(wrapWords("  Seals   the  map.  ", 252.0f, measureWidth) == "Seals the map.");
    CHECK(wrapWords("Seals    the map.", 63.0f, measureWidth) == "Seals the\nmap.");
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

    // an alpha tested material is perforated, which doesn't seal either
    auto grate = gl::Material{"base/grate", gl::createTextureResource(gl::Texture{4, 4})};
    grate.setSurfaceParms({"coverage_perforated", "metal"});

    // other surface parms don't matter
    auto opaque =
      gl::Material{"base/opaque", gl::createTextureResource(gl::Texture{4, 4})};
    opaque.setSurfaceParms({"coverage_opaque", "metal"});

    // only the coverage counts, not the alpha or blend function of the preview
    auto blended =
      gl::Material{"base/blended", gl::createTextureResource(gl::Texture{4, 4})};
    blended.setAlphaFunc(gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.5f);
    blended.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    CHECK(materialBorder(plain, false) == MaterialBorder::Default);
    CHECK(materialBorder(opaque, false) == MaterialBorder::Default);
    CHECK(materialBorder(blended, false) == MaterialBorder::Default);
    CHECK(materialBorder(used, false) == MaterialBorder::Used);
    CHECK(materialBorder(translucent, false) == MaterialBorder::NonSealing);
    CHECK(materialBorder(grate, false) == MaterialBorder::NonSealing);

    blended.incUsageCount();
    CHECK(materialBorder(blended, false) == MaterialBorder::Used);

    // the leak warning wins over the usage
    CHECK(materialBorder(usedTranslucent, false) == MaterialBorder::NonSealing);

    // the selection wins over everything
    CHECK(materialBorder(plain, true) == MaterialBorder::Selected);
    CHECK(materialBorder(usedTranslucent, true) == MaterialBorder::Selected);
  }
}

} // namespace tb::ui
