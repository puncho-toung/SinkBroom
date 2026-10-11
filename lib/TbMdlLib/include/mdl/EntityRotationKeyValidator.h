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

#include "mdl/Validator.h"

#include <memory>
#include <vector>

namespace tb::mdl
{
class EntityNode;
class Issue;

/**
 * Reports the rotation properties that the id Tech 4 rotation policy never writes:
 *
 * - "angles" or "rotation" on an entity with brushes or patches. TrenchBroom keeps the
 *   brushes in world space, so the engine would rotate them a second time.
 * - "rotation" on a point entity, and "light_rotation" or "light_angles" on a light. The
 *   editor shows them, but replaces them by "angles" when the entity is rotated.
 *
 * Quick fixes: "Delete Property" deletes the reported property; "Convert to Angles" turns
 * the rotation of each reported point entity into "angles" without changing it, and
 * leaves entities with brushes or patches alone.
 */
class EntityRotationKeyValidator : public Validator
{
public:
  EntityRotationKeyValidator();

private:
  void doValidate(
    EntityNode& entityNode, std::vector<std::unique_ptr<Issue>>& issues) const override;
};

} // namespace tb::mdl
