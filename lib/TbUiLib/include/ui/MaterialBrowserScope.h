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

namespace tb::ui
{

/**
 * Which materials a material browser lists, by the game config's utility materials
 * (materials.utilityMaterials): all of them, all but the utility materials, or the
 * utility materials only. Kept apart from MaterialBrowserView.h so that a browser's
 * users need not include the view.
 */
enum class MaterialBrowserScope
{
  All,
  Regular,
  Utility
};

} // namespace tb::ui
