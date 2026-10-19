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
 * properties of every member, and the names of the entities, survive (F29, D18). With
 * naming rules, the other entities keep their names from the file if they are free,
 * else they get new ones, and the link sources among the new children follow the names.
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
 * Returns the number of entities that match the class, which may be zero, in which case
 * the map is not changed. Returns an error if the entry is not a readable entity
 * snapshot or if the transaction fails.
 */
Result<std::size_t> applyPrefabToSelection(Map& map, const PrefabEntry& entry);

/**
 * Whether applyPrefabToSelection would change anything: the entry is an entity snapshot
 * whose world the library keeps, and the selection holds an entity of its class.
 */
bool canApplyPrefabToSelection(const Map& map, const PrefabEntry& entry);

} // namespace tb::mdl
