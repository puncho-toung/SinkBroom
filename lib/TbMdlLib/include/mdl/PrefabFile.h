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

#include <filesystem>
#include <string>
#include <vector>

namespace tb::mdl
{
class EntityNode;
class Map;
class Node;
class PrefabLibrary;
class WorldNode;

/**
 * The worldspawn keys of a prefab file. A prefab file is an ordinary map whose worldspawn
 * carries these keys and whose body is one entity (PrefabKind::Entity) or one top-level
 * group (PrefabKind::Assembly).
 */
namespace PrefabPropertyKeys
{
inline const std::string Kind = "_tb_prefab_kind";
inline const std::string Name = "_tb_prefab_name";
inline const std::string Class = "_tb_prefab_class";
inline const std::string Tags = "_tb_prefab_tags";
inline const std::string Saved = "_tb_prefab_saved";
inline const std::string Source = "_tb_prefab_source";
} // namespace PrefabPropertyKeys

namespace PrefabPropertyValues
{
inline const std::string KindEntity = "entity";
inline const std::string KindAssembly = "assembly";
} // namespace PrefabPropertyValues

enum class PrefabKind
{
  Entity,
  Assembly,
};

enum class PrefabRoot
{
  User,
  Mod,
};

/**
 * The worldspawn _tb_prefab_* keys of a prefab file.
 */
struct PrefabMetadata
{
  PrefabKind kind = PrefabKind::Entity;
  std::string name;
  // entity kind only, else empty
  std::string className;
  // split on ';'
  std::vector<std::string> tags;
  // ISO 8601 local, as written
  std::string saved;
  // map name it was saved from
  std::string source;
};

struct SavePrefabOptions
{
  std::string name;
  // relative subfolder, may be empty
  std::filesystem::path category;
  std::vector<std::string> tags;
  PrefabRoot root = PrefabRoot::User;
  // "Save as assembly anyway"
  bool forceAssembly = false;
  // remove link keys whose value names an entity outside the selection
  bool clearExternalLinks = true;
  // write every definition default the entity lacks as an own key
  bool includeDefaults = false;
};

/**
 * Exactly one entity (a point entity, or a brush entity with its brushes and patches)
 * and nothing else is an entity snapshot unless forceAssembly is set; anything else is an
 * assembly.
 *
 * Brushes and patches that belong to a brush entity stand for that entity, so selecting
 * all brushes of one brush entity counts as selecting the entity.
 */
PrefabKind derivePrefabKind(const std::vector<Node*>& selectedNodes, bool forceAssembly);

struct ExternalLink
{
  const EntityNode* node;
  std::string key;
  std::string value;
};

/**
 * Returns every link key of the selected entities (the keys their definitions type as
 * link sources, target and targetN, and bind) whose value names no entity in the
 * selection. These are the keys savePrefab removes when clearExternalLinks is set.
 */
std::vector<ExternalLink> findExternalLinks(const std::vector<Node*>& selectedNodes);

/**
 * The file that savePrefab writes for the given options:
 * writeRootFor(options.root) / options.category / <options.name>.map, where the name is
 * made a file name that every platform can hold (invalid characters replaced, trailing
 * dots and spaces removed, a reserved device name such as CON or NUL suffixed with '_').
 *
 * Returns an error if no character of the name is left, if the category is not a folder
 * inside the root (it has a root name or a root directory, or leaves the root through
 * ".."), or if the root is not available.
 */
Result<std::filesystem::path> prefabFilePath(
  const PrefabLibrary& library, const SavePrefabOptions& options);

/**
 * Writes the selected nodes to a new prefab file at prefabFilePath(options), overwriting
 * an existing file, and returns its absolute path. The map is not modified.
 *
 * The body is the one selected entity for the entity kind. For an assembly it is the one
 * selected group, renamed to options.name, if nothing else is selected, and otherwise a
 * new group named options.name that holds the selection.
 */
Result<std::filesystem::path> savePrefab(
  const Map& map,
  const std::vector<Node*>& selectedNodes,
  const SavePrefabOptions& options);

/**
 * Reads the _tb_prefab_* keys from the worldspawn of the given world. Returns an error if
 * the world is not a prefab, that is, if _tb_prefab_kind is missing or unknown.
 */
Result<PrefabMetadata> readPrefabMetadata(const WorldNode& world);

} // namespace tb::mdl
