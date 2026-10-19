/*
 Copyright (C) 2025 Kristian Duske

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

#include "kd/flat_map.h"

#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;

enum class PasteType;

std::string serializeSelectedNodes(Map& map);
std::string serializeSelectedBrushFaces(Map& map);

PasteType paste(Map& map, const std::string& str);

/**
 * Prepares caller-owned nodes (for example, the result of NodeReader::read) for adding to
 * the given map, the way paste does:
 *
 * - world nodes, layer nodes and worldspawn entities are dissolved: their children are
 *   moved to parentForNodes(map), and the dissolved nodes are deleted;
 * - group persistent IDs that are already in the map or repeated among the nodes are
 *   reset;
 * - groups that would be added into their own link set are unlinked;
 * - link IDs follow the paste rule: a group whose link ID exists on exactly one object in
 *   the map is unlinked, on two or more it joins that link set, on none it keeps its ID.
 *
 * Entity names are not touched; see assignUniqueEntityNamesBeforeAdding.
 *
 * The returned nodes are detached, owned by the caller and keyed by their new parent,
 * ready for addNodes. The given vector must not be used afterwards, since some of its
 * nodes may have been deleted.
 */
kdl::flat_map<Node*, std::vector<Node*>> prepareNodesForAdding(
  Map& map, const std::vector<Node*>& nodes);


} // namespace tb::mdl
