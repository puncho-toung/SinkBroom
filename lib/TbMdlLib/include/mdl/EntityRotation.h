/*
 Copyright (C) 2010 Kristian Duske

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

#include "kd/reflection_decl.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Entity;
enum class PitchType;

enum class EntityRotationType
{
  None,
  Angle,
  AngleUpDown,
  Euler,
  Euler_PositivePitchDown,
  Mangle,
  /**
   * Nine numbers, the world directions of the entity's forward, left and up axes, as in
   * the id Tech 4 properties "rotation" and "light_rotation". Never written.
   */
  Matrix3x3,
  /**
   * Pitch, yaw and roll like Euler_PositivePitchDown, read like the id Tech 4 properties
   * "angles" and "light_angles": a value with fewer than three numbers sets the numbers
   * it has, and the others are 0.
   */
  IdTech4Angles,
};

std::ostream& operator<<(std::ostream& lhs, const EntityRotationType& rhs);

enum class EntityRotationUsage
{
  Allowed,
  BlockRotation
};

std::ostream& operator<<(std::ostream& lhs, const EntityRotationUsage& rhs);

struct EntityRotationInfo
{
  const EntityRotationType type;
  const std::string propertyKey;
  const EntityRotationUsage usage;

  kdl_reflect_decl(EntityRotationInfo, type, propertyKey, usage);
};

/**
 * The properties that change when an entity is transformed: one property to add or
 * update, and the properties to remove, in this order. Only the id Tech 4 rotation
 * policy removes properties.
 */
struct EntityRotationUpdate
{
  std::optional<EntityProperty> set;
  std::vector<std::string> remove;

  kdl_reflect_decl(EntityRotationUpdate, set, remove);
};

EntityRotationInfo entityRotationInfo(
  const Entity& entity, EntityRotationPolicy policy = EntityRotationPolicy::Quake);

vm::mat4x4d entityRotation(
  const std::vector<EntityProperty>& properties, const EntityRotationInfo& info);

vm::mat4x4d entityRotation(
  const Entity& entity, EntityRotationPolicy policy = EntityRotationPolicy::Quake);

vm::vec3d entityYawPitchRoll(
  const vm::mat4x4d& transformation, const vm::mat4x4d& rotation);

std::optional<EntityProperty> applyEntityRotation(
  const std::vector<EntityProperty>& properties,
  const EntityRotationInfo& info,
  const vm::mat4x4d& transformation);

/**
 * Returns the properties that the id Tech 4 rotation policy reads the rotation of the
 * given point entity from and replaces by "angles" when it rotates the entity:
 * "rotation", and for a light also "light_rotation" and "light_angles". Returns only the
 * present properties, in the order in which the engine reads them.
 */
std::vector<std::string> idTech4ConvertedRotationKeys(const Entity& entity);

/**
 * Returns the property changes that make the given entity's rotation properties
 * reflect the given transformation, which must not contain a translation.
 *
 * The id Tech 4 policy writes the rotation of a point entity to "angles" and removes the
 * properties that the engine would read instead, except for a class whose definition
 * declares "angle" but not "angles", which keeps its yaw in "angle". Of an entity with
 * brushes, it only turns "angle", the direction of a mover.
 */
EntityRotationUpdate applyEntityRotation(
  const Entity& entity,
  const vm::mat4x4d& transformation,
  EntityRotationPolicy policy = EntityRotationPolicy::Quake);

void applyEntityRotationUpdate(Entity& entity, const EntityRotationUpdate& update);

} // namespace tb::mdl
