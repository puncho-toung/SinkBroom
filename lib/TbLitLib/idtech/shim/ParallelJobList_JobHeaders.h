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

// Stands in for the engine's idlib/ParallelJobList_JobHeaders.h, the one header the
// copied idlib/geometry/RenderMatrix.cpp names by a path of its own,
// "../ParallelJobList_JobHeaders.h" (LP1b.3, D58, C24). That path finds nothing beside
// the copy, so the compiler resolves it against TbLitIdLib's first include folder,
// shim/private, which leads here: this file lives in shim/ for that reason alone and is
// reached by no other name.
//
// The engine's GCC and Clang builds force-include idlib/precompiled.h into every engine
// file (neo/CMakeLists.txt:1318-1325), so RenderMatrix.cpp sees all of idLib before its
// own includes, which the include guards then skip; including the shim's precompiled.h
// here gives the copy the same view (mirror.json M-LIGHT-NC-01). The engine header's own
// list (the system headers, sys_assert.h, sys_types.h, sys_intrinsics.h, Math.h,
// ParallelJobList.h) is a subset of it, ParallelJobList.h aside, which the copy never
// uses.

#include "precompiled.h"
