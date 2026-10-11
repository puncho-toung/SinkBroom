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
class EntityNode;
class Issue;

/**
 * Reports entities with brushes or patches whose name is missing, whose brush entity
 * model key is missing, or whose brush entity model key differs from their name. The
 * id Tech 4 map compiler names the model of such an entity after its name and fails
 * without one, and the engine loads the model named by the model key.
 *
 * The brush entity model key of the given config must be set.
 */
class BrushEntityModelValidator : public Validator
{
private:
  EntityNamingConfig m_config;

public:
  explicit BrushEntityModelValidator(EntityNamingConfig config);

private:
  void doValidate(
    EntityNode& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const override;
};

} // namespace tb::mdl
