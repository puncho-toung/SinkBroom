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

// LP1c.2 (§3.5, R5.6, D27): the translation unit of the copied image loaders. The copied
// engine/renderer/Image_files.cpp (R_LoadImage, the imageLoaders table, LoadTGA,
// LoadSTB_RGBA8, LoadEXR, LoadHDR, R_LoadCubeImages, with the stb_image and tinyexr
// implementations it compiles) is included here, after the names of tinyexr's embedded
// miniz are made its own (shim/TinyexrMinizNames.h), as DeclManagerShim.cpp includes the
// copied DeclManager.cpp. Nothing may run the loaders' pixel paths before the file system
// validates every image file outside the copy (D51). Parse thread only (C20).

// stb_image keeps its failure reason and three load flags thread-local, which would grow
// the executable's static TLS block; every load runs on the parse thread (C20), so plain
// statics serve (M-IMG-BUILD-01, M-IMG-STATIC-02)
#define STBI_NO_THREAD_LOCALS

// clang-format off: precompiled.h comes first in every shim file, the names before the copy
#include "precompiled.h"
#include "shim/TinyexrMinizNames.h"
#include "renderer/Image_files.cpp"
// clang-format on
