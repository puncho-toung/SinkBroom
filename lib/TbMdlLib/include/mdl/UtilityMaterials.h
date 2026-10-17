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

#include "mdl/UtilityMaterialConfig.h"

#include <string_view>
#include <vector>

namespace tb::mdl
{

/**
 * Returns the first of the given utility materials whose pattern matches the given
 * root-relative material name (see materialNameMatchesPattern), or null if none does.
 * The first match wins, so a game config lists a material before the catch-all pattern
 * of its folder.
 */
const UtilityMaterialConfig* findUtilityMaterial(
  const std::vector<UtilityMaterialConfig>& utilityMaterials,
  std::string_view materialName);

/**
 * Whether the given root-relative material name matches the pattern of any of the given
 * utility materials.
 */
bool isUtilityMaterial(
  const std::vector<UtilityMaterialConfig>& utilityMaterials,
  std::string_view materialName);

} // namespace tb::mdl
