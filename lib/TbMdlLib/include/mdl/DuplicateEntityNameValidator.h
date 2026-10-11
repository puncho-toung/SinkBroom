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
 * Reports entities that share their name with another entity, which the id Tech 4
 * engines refuse to spawn. Names are compared exactly, as the engine does when it spawns
 * the entities.
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
