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

#include "vm/vec.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
struct PrefabEntry;
struct PrefabPlacement;

struct PrefabRefreshReport
{
  // the entities of the first member that have no counterpart in the new file, so that
  // they, and their copies in the other members, lost their per-copy overrides
  std::size_t entitiesLostOverrides = 0;
  // their names, or their classnames if they have none, in the order of the old member
  std::vector<std::string> lostEntities;
  // the translation that was added to every new child so that the children that did not
  // move in the file stay where they were in the map (DD-23); zero if none was needed
  vm::vec3d referenceShift = vm::vec3d{0, 0, 0};
};

/**
 * Replaces the children of the placement's first member with the body of its prefab
 * file, and sets the prefab hash of every member to the file's, in one transaction named
 * "Refresh Prefab <name>". The linked group update carries the new children to the other
 * members when the transaction is committed; one member of the link set changes, so one
 * placement is one transaction (F9).
 *
 * The new children are transformed into the first member by the member's transformation
 * and the inverse of the file group's, as a linked group update would. A new child that
 * stands at the same position as an old child of the same kind (and, for an entity, of
 * the same classname) takes the old child's link ID, recursively, so that the protected
 * properties of every member, and the names of the entities, survive (F29, D18). A
 * nested group takes the old group's link ID only if no group outside the placement's
 * members shares it; otherwise it keeps its own, and the entities below it count as
 * lost. With naming rules, an entity that took a link ID keeps the old entity's name,
 * which is reserved first; the other entities keep their names from the file if they are
 * free, else they get new ones, and the link sources among the new children follow the
 * names.
 *
 * A file whose group carries no transformation was saved from loose nodes, so its
 * reference point (D10) moves whenever the bounds of its body change, which moves every
 * child against the placement. For such a file, the new children are translated by the
 * negated most common displacement of the matched children (old to new reference point:
 * an entity's origin, a brush's or patch's bounds centre), rounded to the map's grid,
 * with ties going to no displacement, else to the first; see referenceShift (DD-23).
 *
 * While the open group lies strictly below a member, it is closed first, inside the
 * transaction, since the refresh replaces the children of every member (DD-35).
 *
 * The placement's first member must be in the map.
 *
 * Returns an error and leaves the map untouched if the file is missing, cannot be read,
 * is not one non-empty group, or if the transaction fails.
 */
Result<PrefabRefreshReport> refreshPlacement(Map& map, const PrefabPlacement& placement);

/**
 * Makes every selected entity of the entry's class match the entity snapshot (C6, D14),
 * in one transaction named "Apply Prefab": its properties become the snapshot's, except
 * that the naming key, origin, the link keys (see isPrefabLinkKey), the brush entity
 * model key of a brush entity and every protected property keep the entity's own values
 * (or stay absent); a key that the entity has and the snapshot lacks is removed.
 * Brushes, patches and the protected properties are not touched.
 *
 * When the selected entities lie in two or more members of one link set, a selected
 * entity in a member other than the first of them, in the order of the world, is changed
 * through its linked copy in that first member, once, since two changed members of one
 * set cancel the commit (F9); the linked group update carries the change to the others.
 *
 * Returns the number of entities that were changed, which may be zero, in which case the
 * map is not changed. Returns an error if the entry is not a readable entity snapshot or
 * if the transaction fails.
 */
Result<std::size_t> applyPrefabToSelection(Map& map, const PrefabEntry& entry);

/**
 * Whether applyPrefabToSelection would change anything: the entry is an entity snapshot
 * whose world the library keeps, and the selection holds an entity of its class.
 */
bool canApplyPrefabToSelection(const Map& map, const PrefabEntry& entry);

} // namespace tb::mdl
