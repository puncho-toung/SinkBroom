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

// Stands in for the engine's idlib/precompiled.h, which every copied engine file includes
// by this name, so that whole-file copies stay byte-identical (S23, C7). It follows the
// engine header's order: system, idLib, framework, decls, renderer, sound, GUI, game,
// then RenderCommon.h. Only what the copied files use is declared (C17). This header
// lives in TbLitIdLib's private include folder and never reaches a TrenchBroom
// translation unit.

// clang-format off: the order is the engine's
#include "shim/ShimSystem.h"
#include "shim/ShimLib.h"
#include "shim/ShimFramework.h"
#include "shim/ShimEngine.h"
#include "RenderCommon.h"
// clang-format on
