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

#include "mdl/UtilityMaterials.h"

#include "mdl/TagMatcher.h"

#include <algorithm>

namespace tb::mdl
{

const UtilityMaterialConfig* findUtilityMaterial(
  const std::vector<UtilityMaterialConfig>& utilityMaterials,
  const std::string_view materialName)
{
  const auto iUtilityMaterial =
    std::ranges::find_if(utilityMaterials, [&](const auto& utilityMaterial) {
      return materialNameMatchesPattern(materialName, utilityMaterial.pattern);
    });
  return iUtilityMaterial != utilityMaterials.end() ? &*iUtilityMaterial : nullptr;
}

bool isUtilityMaterial(
  const std::vector<UtilityMaterialConfig>& utilityMaterials,
  const std::string_view materialName)
{
  return findUtilityMaterial(utilityMaterials, materialName) != nullptr;
}

} // namespace tb::mdl
