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

// LP7.5: stands in for the engine's idlib/Lib.h, which the copied idlib/gltfProperties.h
// includes by that name (its quotes find no Lib.h beside it, then this folder, as
// precompiled.h and RenderCommon.h are found): the shim's ShimLib.h declares what the
// copied code calls of it.
#include "shim/ShimLib.h"
