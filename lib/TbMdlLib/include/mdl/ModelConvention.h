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

#include <iosfwd>

namespace tb::mdl
{

/**
 * How the entity models of a game are read, as set by the optional game config key
 * entities.modelConvention.
 */
enum class ModelConvention
{
  /** Every model format is read the way TrenchBroom reads it for any game. */
  Default,
  /**
   * The id Tech 4 engines (Doom 3, RBDOOM-3-BFG): a .obj file is read the way the engine
   * reads it, with raw Z-up coordinates and a material decl name after usemtl; a glTF
   * model takes its axes and, where they resolve, its materials from the engine's glTF
   * importer.
   */
  IdTech4,
};

std::ostream& operator<<(std::ostream& lhs, ModelConvention rhs);

} // namespace tb::mdl
