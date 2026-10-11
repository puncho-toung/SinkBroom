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
#include "mdl/Validator.h"

#include <memory>
#include <vector>

namespace tb::mdl
{
class EntityNodeBase;
class Issue;
class Map;

/**
 * Reports the entity names that the id Tech 4 engines refuse to spawn: a name that
 * another entity has, a name the engine reserves (NULL, null_entity), and a link source
 * property that refers to the entity itself.
 *
 * Names are compared without regard to case. The game's entity hash compares names with
 * Icmp within a bucket chosen by a case-sensitive hash, so two names that differ only in
 * case are fatal or not depending on their hash; the only rule a mapper can follow is to
 * treat them as equal, as idMapFile::FindEntity does.
 *
 * The issues of an entity depend on the names of other entities, so the map invalidates
 * them whenever an entity with the same name is added, removed or changed.
 */
class DuplicateEntityNameValidator : public Validator
{
private:
  const Map& m_map;
  EntityNamingConfig m_config;

public:
  DuplicateEntityNameValidator(const Map& map, EntityNamingConfig config);

private:
  void doValidate(EntityNodeBase& entityNode, std::vector<std::unique_ptr<Issue>>& issues)
    const override;
};

} // namespace tb::mdl
