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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/ModelManager.cpp and idlib/Str.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ModelRecord.h"

namespace tb::lit
{

std::string canonicalModelName(const std::string_view name)
{
  // ModelManager.cpp:313-319: no name, no model; canonical = _modelName; ToLower()
  auto canonical = std::string{name};
  for (auto& c : canonical)
  {
    // Str.h:1101-1110, :1404-1408 through a signed char: 'A'-'Z' only
    if (c >= 'A' && c <= 'Z')
    {
      c = char(c + ('a' - 'A'));
    }
  }
  return canonical;
}

} // namespace tb::lit
