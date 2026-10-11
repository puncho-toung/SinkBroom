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
 * Returns the property changes that make the given entity's rotation properties
 * reflect the given transformation, which must not contain a translation.
 */
EntityRotationUpdate applyEntityRotation(
  const Entity& entity,
  const vm::mat4x4d& transformation,
  EntityRotationPolicy policy = EntityRotationPolicy::Quake);

void applyEntityRotationUpdate(Entity& entity, const EntityRotationUpdate& update);

} // namespace tb::mdl
