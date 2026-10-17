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

#include "kd/reflection_decl.h"

#include <string>

namespace tb::mdl
{

/**
 * A material that the game config declares as a utility material, such as caulk, clip or
 * a visportal: one entry of materials.utilityMaterials. Kept apart from GameConfig.h so
 * that the UI can name it without the rest of the game config.
 */
struct UtilityMaterialConfig
{
  // glob over the root-relative material name, matched as a material name tag matches
  // (see materialNameMatchesPattern)
  std::string pattern;
  // what the material does; may be empty
  std::string description;

  kdl_reflect_decl(UtilityMaterialConfig, pattern, description);
};

} // namespace tb::mdl
