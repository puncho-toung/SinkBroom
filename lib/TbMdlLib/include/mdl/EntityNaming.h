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

#include "mdl/EntityProperties.h"
#include "mdl/IssueType.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::mdl
{
class Entity;
class EntityDefinitionManager;
class EntityNodeBase;
class GroupNode;
class Issue;
class Map;
class Node;

/**
 * The entities of a map by their name, compared without regard to case, which is how the
 * id Tech 4 engines find entities by name (idMapFile::FindEntity, and in practice the
 * game's entity hash, which compares with Icmp). A map whose game has naming rules keeps
 * one up to date along with its node index.
 */
class EntityNameIndex
{
private:
  std::string m_nameKey;
  // lowercase name -> the entities with that name
  std::unordered_map<std::string, std::vector<EntityNodeBase*>> m_entityNodes;

public:
  explicit EntityNameIndex(std::string nameKey);

  void addEntityNode(EntityNodeBase& entityNode);
  void removeEntityNode(EntityNodeBase& entityNode);

  std::vector<EntityNodeBase*> findEntityNodes(std::string_view name) const;
};

/**
 * Hands out unique entity names for one editing operation, in the scheme the id Tech 4
 * engines use themselves: <classname>_<N> with the lowest free N from 0.
 *
 * A name is taken if an entity in the map holds it in the naming key, compared without
 * regard to case, unless that entity is in one of the excluded subtrees, or if it was
 * reserved during this operation. The holders are found through the map's entity name
 * index, so the map's game must have naming rules.
 *
 * An allocator keeps no state beyond the operation it is made for, so a cancelled
 * transaction leaves nothing behind.
 */
class EntityNameAllocator
{
private:
  const Map& m_map;
  const EntityNamingConfig& m_config;
  std::vector<const Node*> m_excluded;
  // lowercase names reserved during this operation
  std::unordered_set<std::string> m_reserved;
  // lowercase classname -> the lowest N that may still be free
  std::unordered_map<std::string, size_t> m_nextIndices;

public:
  explicit EntityNameAllocator(const Map& map, std::vector<const Node*> excluded = {});

  const EntityNamingConfig& config() const;

  /**
   * Whether the given name is taken by an entity in the map or by a reservation, or
   * is one of the names the engine reserves for itself.
   */
  bool isTaken(std::string_view name) const;

  /**
   * Returns <classname>_<N> with the lowest N from 0 such that the name is not taken,
   * and reserves it.
   */
  std::string allocate(std::string_view classname);

  /**
   * Reserves the given name for the rest of the operation.
   */
  void reserve(std::string_view name);

private:
  bool isExcluded(const Node& node) const;
};

/**
 * Whether the engine refuses to spawn an entity with the given name, which it reserves
 * for itself (NULL and null_entity, compared exactly as the engine compares them).
 */
bool isReservedEntityName(std::string_view name);

enum class EntityNamingMode
{
  // every entity gets a new name
  Create,
  // an entity keeps its name if no other entity holds it
  KeepIfFree,
};

struct EntityNamingResult
{
  // old name -> new name, in source order; the old name may be empty
  std::vector<std::pair<std::string, std::string>> renamed;
  // the names that entities kept
  std::vector<std::string> kept;
};

/**
 * Whether an entity of the given class with brushes or patches must have its brush entity
 * model key set to its name. This is the case for every class but worldspawn, whose
 * brushes make up the world, and func_group, whose brushes the map compiler merges into
 * the world.
 */
bool isBrushEntityModelClass(std::string_view classname);

/**
 * Gives the given entity, a single entity that is not in the map or about to change, a
 * unique name according to the given mode, and sets its brush entity model key to that
 * name if it has brushes or patches. The worldspawn entity is never named.
 *
 * Returns true if the entity was changed.
 */
bool allocateEntityName(
  Entity& entity,
  bool hasBrushesOrPatches,
  EntityNameAllocator& allocator,
  EntityNamingMode mode,
  EntityNamingResult& result);

/**
 * Names every entity in the given subtrees, which must not be part of the map yet,
 * depth first and in the given order, which must be the order of their source (the
 * clipboard or the selection) so that the result does not depend on memory addresses.
 */
EntityNamingResult assignUniqueEntityNames(
  const std::vector<Node*>& rootsInSourceOrder,
  EntityNameAllocator& allocator,
  EntityNamingMode mode);

/**
 * Changes the values of the link source properties of the entities in the given subtrees
 * that refer to a renamed entity to its new name, compared without regard to case like
 * the engine compares them. Only references among the given entities are changed, so a
 * copied trigger points at the copied light, but one that was copied alone still points
 * at the original light. A name that one of the entities kept is not changed, since the
 * reference is ambiguous.
 *
 * The entities in the given skipped groups are not changed. These are groups that stay
 * linked to groups in the map, whose next update would overwrite the link source values.
 *
 * The entity definitions are looked up by classname, because nodes that are not part of
 * the map yet may carry none.
 */
void remapLinkSourceValues(
  const EntityDefinitionManager& entityDefinitionManager,
  const std::vector<Node*>& rootsInSourceOrder,
  const EntityNamingResult& namingResult,
  const std::vector<const GroupNode*>& skippedGroups = {});

/**
 * Names the entities in the given subtrees, which are about to be added to the map, in
 * KeepIfFree mode, and if remapLinkSources is true, changes the references among them to
 * their new names, except inside groups that stay linked to groups in the map.
 *
 * The map's game must have naming rules; callers check that before they collect the
 * nodes, so that games without them do no work.
 */
void assignUniqueEntityNamesBeforeAdding(
  const Map& map, const std::vector<Node*>& rootsInSourceOrder, bool remapLinkSources);

/**
 * Sorts the given entity nodes of the map in the order of a depth first traversal of the
 * map, so that operations on them do not depend on the order of a selection.
 */
std::vector<EntityNodeBase*> sortInTreeOrder(
  const Map& map, std::vector<EntityNodeBase*> entityNodes);

/**
 * Returns the entity nodes of the given issues of the given type, without duplicates and
 * in tree order. A quick fix calls this before it changes anything, because a change
 * destroys the issues of the changed entity and of every entity with the same name.
 */
std::vector<EntityNodeBase*> collectEntityNodes(
  const Map& map, const std::vector<const Issue*>& issues, IssueType type);

/**
 * Returns the entities in the map whose name equals the given name without regard to
 * case. Returns nothing if the map's game has no naming rules.
 */
std::vector<EntityNodeBase*> findEntitiesWithName(const Map& map, std::string_view name);

} // namespace tb::mdl
