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
// This file includes, unaltered, the RBDOOM-3-BFG data header
// neo/renderer/Image_brdfLut.h (copied whole into lib/TbLitLib/idtech/engine/renderer):
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP4.2a (D85, P4.13): the facade of lit/EngineImageData.h over the copied data header.
// The header defines its array static and const, so this file holds the one copy of it;
// constant-initialised data that nothing writes, so any thread may read it (C9, C20).

#include "lit/EngineImageData.h"

#include "renderer/Image_brdfLut.h"

namespace tb::lit
{

static_assert(sizeof(brfLutTexBytes) == BRDFLUT_TEX_WIDTH * BRDFLUT_TEX_HEIGHT * 4);

const std::uint8_t* engineBrdfLutData()
{
  return reinterpret_cast<const std::uint8_t*>(brfLutTexBytes);
}

std::size_t engineBrdfLutSize()
{
  return sizeof(brfLutTexBytes);
}

int engineBrdfLutWidth()
{
  return BRDFLUT_TEX_WIDTH;
}

int engineBrdfLutHeight()
{
  return BRDFLUT_TEX_HEIGHT;
}

} // namespace tb::lit
