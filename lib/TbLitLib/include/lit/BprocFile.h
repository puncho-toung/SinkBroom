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

#include "base/Result.h"
#include "lit/ProcFile.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace tb::lit
{

// the area BSP of a .bproc and what its header says
struct BprocTree
{
  // the map name the .bproc stores (InitFromMap's mapName, which names the bake files)
  std::string mapName;
  // the .proc time it was made from (Unix seconds, A4)
  std::int64_t timeStamp = 0;
  // the version of its magic: 3, or the BFG's 1 or 2
  int version = 0;
  ProcTree tree;
};

/**
 * C10, A9, D64: a .bproc's area BSP as InitFromMap's binary branch reads it
 * (RenderWorld_load.cpp:818-879), into the ProcTree the .proc reader makes: the models
 * skipped through the LoadBinaryModel mirror (lit/BprocModel.h), kept by name and area
 * with their surfaces' materials and counts; the inter-area portals and the nodes, which
 * the engine wrote from the copied idLexer's values, so they equal the .proc's exactly.
 *
 * As the engine with com_productionMode 0: a version-3 file is never time-checked against
 * the .proc (A9); a version 1 or 2 file is taken only when procExists is false (no .proc
 * at all, FILE_NOT_FOUND_TIMESTAMP); a model whose stored time differs from the header's,
 * when that is neither -1 nor 0, fails the whole file (LoadBinaryModel's test). Not in
 * the engine: a portal whose areas are not below the area count, or a node whose child
 * names no node, fails the file, as the .proc reader's bounds checks do. Pure; any
 * thread.
 */
Result<BprocTree> readBprocTree(std::span<const std::byte> data, bool procExists);

} // namespace tb::lit
