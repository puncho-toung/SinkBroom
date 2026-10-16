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

#include "mdl/EntityRotation.h"

#include "base/Macros.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/reflection_impl.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <ostream>
#include <ranges>
#include <string_view>

namespace tb::mdl
{

std::ostream& operator<<(std::ostream& lhs, const EntityRotationType& rhs)
{
  switch (rhs)
  {
  case EntityRotationType::None:
    lhs << "None";
    break;
  case EntityRotationType::Angle:
    lhs << "Angle";
    break;
  case EntityRotationType::AngleUpDown:
    lhs << "AngleUpDown";
    break;
  case EntityRotationType::Euler:
    lhs << "Euler";
    break;
  case EntityRotationType::Euler_PositivePitchDown:
    lhs << "Euler_PositivePitchDown";
    break;
  case EntityRotationType::Mangle:
    lhs << "Mangle";
    break;
  case EntityRotationType::Matrix3x3:
    lhs << "Matrix3x3";
    break;
  case EntityRotationType::IdTech4Angles:
    lhs << "IdTech4Angles";
    break;
    switchDefault();
  }
  return lhs;
}

std::ostream& operator<<(std::ostream& lhs, const EntityRotationUsage& rhs)
{
  switch (rhs)
  {
  case EntityRotationUsage::Allowed:
    lhs << "Allowed";
    break;
  case EntityRotationUsage::BlockRotation:
    lhs << "BlockRotation";
    break;
    switchDefault();
  }
  return lhs;
}

kdl_reflect_impl(EntityRotationInfo);

kdl_reflect_impl(EntityRotationUpdate);

namespace
{
std::optional<std::tuple<std::string, EntityRotationType>> selectEntityRotationType(
  const Entity& entity,
  const std::vector<std::tuple<std::string, EntityRotationType>>&
    propertyToEntityRotationTypeMapping)
{
  for (const auto& [propertyKey, entityRotationType] :
       propertyToEntityRotationTypeMapping)
  {
    if (entity.hasProperty(propertyKey))
    {
      return {{propertyKey, entityRotationType}};
    }
  }

  for (const auto& [propertyKey, entityRotationType] :
       propertyToEntityRotationTypeMapping)
  {
    if (const auto* definition = entity.definition();
        definition && getPropertyDefinition(definition, propertyKey))
    {
      return {{propertyKey, entityRotationType}};
    }
  }

  return std::nullopt;
}

EntityRotationInfo quakeEntityRotationInfo(const Entity& entity)
{
  auto type = EntityRotationType::None;
  std::string propertyKey;
  EntityRotationUsage usage = EntityRotationUsage::Allowed;

  const auto* model = entity.model();
  const auto* modelData = model ? model->data() : nullptr;
  const auto pitchType = modelData ? modelData->pitchType() : PitchType::Normal;
  const EntityRotationType eulerType =
    (pitchType == PitchType::MdlInverted ? EntityRotationType::Euler
                                         : EntityRotationType::Euler_PositivePitchDown);

  const auto hasTarget = [&]() {
    const auto targetPropertyKeys =
      getLinkSourcePropertyDefinitions(entity.definition())
      | std::views::transform(
        [](const auto& linkSourceProperty) { return linkSourceProperty->key; });

    return std::ranges::any_of(targetPropertyKeys, [&](const auto& targetPropertyKey) {
      return entity.hasProperty(targetPropertyKey);
    });
  };


  // determine the type of rotation to apply to this entity
  const auto classname = entity.classname();
  if (classname != EntityPropertyValues::NoClassname)
  {
    if (kdl::cs::str_is_prefix(classname, "light"))
    {
      if (entity.hasProperty(EntityPropertyKeys::Mangle))
      {
        // spotlight without a target, update mangle
        type = EntityRotationType::Mangle;
        propertyKey = EntityPropertyKeys::Mangle;
      }
      else if (!hasTarget())
      {
        // not a spotlight, but might have a rotatable model, so change angle or angles
        if (entity.hasProperty(EntityPropertyKeys::Angles))
        {
          type = eulerType;
          propertyKey = EntityPropertyKeys::Angles;
        }
        else if (entity.hasProperty(EntityPropertyKeys::Angle))
        {
          type = EntityRotationType::Angle;
          propertyKey = EntityPropertyKeys::Angle;
        }
        else
        {
          // not a spotlight, don't modify
        }
      }
      else
      {
        // spotlight with target, don't modify
      }
    }
    else
    {
      // non-light

      if (!entity.pointEntity())
      {
        // brush entity
        std::tie(propertyKey, type) =
          selectEntityRotationType(
            entity,
            {{EntityPropertyKeys::Angles, eulerType},
             {EntityPropertyKeys::Mangle, eulerType},
             {EntityPropertyKeys::Angle, EntityRotationType::AngleUpDown}})
            .value_or(std::make_tuple(propertyKey, type));
      }
      else
      {
        // point entity

        // if the origin of the definition's bounding box is not in its center, don't
        // apply the rotation
        const auto offset = entity.definitionBounds().center();
        if (!vm::is_zero(offset.xy(), vm::Cd::almost_zero()))
        {
          // TODO: this only makes sense for Quake
          usage = EntityRotationUsage::BlockRotation;
        }

        std::tie(propertyKey, type) =
          selectEntityRotationType(
            entity,
            {{EntityPropertyKeys::Angles, eulerType},
             {EntityPropertyKeys::Mangle, eulerType},
             {EntityPropertyKeys::Angle, EntityRotationType::AngleUpDown}})
            .value_or(std::make_tuple(
              EntityPropertyKeys::Angle, EntityRotationType::AngleUpDown));
      }
    }
  }

  return EntityRotationInfo{type, propertyKey, usage};
}

/**
 * Whether the id Tech 4 policy treats the given entity as a light: its classname starts
 * with "light", without regard to case. The prefix is the editor's choice (package 05,
 * section 4.6). The engine has no such rule: the game makes an idLight of every class
 * whose spawnclass is idLight, while dmap, the map converter and the FGD exporter compare
 * the whole classname with "light" using idStr::Icmp (tools/compilers/dmap/map.cpp,
 * idlib/MapFile.cpp, framework/DeclManager.cpp).
 */
bool isIdTech4Light(const Entity& entity)
{
  return kdl::ci::str_is_prefix(entity.classname(), "light");
}

/**
 * The properties that hold the rotation of a point entity under the id Tech 4 policy, in
 * the order in which the engine reads them: idLight and dmap for lights (d3xp/Light.cpp,
 * tools/compilers/dmap/map.cpp), idEntity::ParseSpawnArgsToRenderEntity for every other
 * entity (d3xp/Entity.cpp). The engine normalizes the angles of "light_angles" and
 * "angles" to 0..360 for lights, which does not change the rotation, and reads "angle" as
 * a plain yaw, without -1 for up and -2 for down.
 */
const std::vector<std::tuple<std::string, EntityRotationType>>& idTech4RotationProperties(
  const Entity& entity)
{
  static const auto lightProperties =
    std::vector<std::tuple<std::string, EntityRotationType>>{
      {EntityPropertyKeys::LightRotation, EntityRotationType::Matrix3x3},
      {EntityPropertyKeys::Rotation, EntityRotationType::Matrix3x3},
      {EntityPropertyKeys::LightAngles, EntityRotationType::IdTech4Angles},
      {EntityPropertyKeys::Angles, EntityRotationType::IdTech4Angles},
      {EntityPropertyKeys::Angle, EntityRotationType::Angle},
    };
  static const auto otherProperties =
    std::vector<std::tuple<std::string, EntityRotationType>>{
      {EntityPropertyKeys::Rotation, EntityRotationType::Matrix3x3},
      {EntityPropertyKeys::Angles, EntityRotationType::IdTech4Angles},
      {EntityPropertyKeys::Angle, EntityRotationType::Angle},
    };

  return isIdTech4Light(entity) ? lightProperties : otherProperties;
}

/**
 * Whether the id Tech 4 policy writes only the yaw to "angle" for the given point entity:
 * it is not a light, it has neither "rotation" nor "angles", and its definition declares
 * "angle" but not "angles". For some of these classes the engine reads nothing but
 * "angle", e.g. idTeleporter (d3xp/Fx.cpp) and idEntity::Event_RestorePosition
 * (d3xp/Entity.cpp).
 */
bool idTech4KeepsAngle(const Entity& entity)
{
  const auto* definition = entity.definition();
  return !isIdTech4Light(entity) && !entity.hasProperty(EntityPropertyKeys::Rotation)
         && !entity.hasProperty(EntityPropertyKeys::Angles)
         && getPropertyDefinition(definition, EntityPropertyKeys::Angle)
         && !getPropertyDefinition(definition, EntityPropertyKeys::Angles);
}

/**
 * Unlike the Quake rules, the id Tech 4 rules ignore the model's pitch type, a light's
 * target and the definition's bounds.
 */
EntityRotationInfo idTech4EntityRotationInfo(const Entity& entity)
{
  if (entity.classname() == EntityPropertyValues::NoClassname)
  {
    return {EntityRotationType::None, "", EntityRotationUsage::Allowed};
  }

  if (!entity.pointEntity())
  {
    // The brushes are in world space, so nothing rotates them. "angle" is the direction
    // of a mover, with -1 for up and -2 for down (idMover_Binary::GetMovedir), which the
    // entity's arrow shows.
    if (entity.hasProperty(EntityPropertyKeys::Angle))
    {
      return {
        EntityRotationType::AngleUpDown,
        EntityPropertyKeys::Angle,
        EntityRotationUsage::Allowed};
    }
    return {EntityRotationType::None, "", EntityRotationUsage::Allowed};
  }

  // the first present property wins even if its value is empty (idDict::GetMatrix and
  // idDict::GetAngles)
  for (const auto& [propertyKey, type] : idTech4RotationProperties(entity))
  {
    if (entity.hasProperty(propertyKey))
    {
      return {type, propertyKey, EntityRotationUsage::Allowed};
    }
  }

  // an entity without a rotation property gets one when it is rotated
  if (idTech4KeepsAngle(entity))
  {
    return {
      EntityRotationType::Angle, EntityPropertyKeys::Angle, EntityRotationUsage::Allowed};
  }
  return {
    EntityRotationType::IdTech4Angles,
    EntityPropertyKeys::Angles,
    EntityRotationUsage::Allowed};
}

/**
 * Reads up to N numbers from the start of the given text the way the engine's sscanf of
 * "%f %f ..." reads them: white space before each number is skipped, and reading stops at
 * the first text that is not a number. Returns the numbers read.
 */
template <std::size_t N>
std::vector<double> parseLeadingNumbers(const std::string_view text)
{
  auto result = std::vector<double>{};
  auto pos = std::size_t{0};
  while (result.size() < N)
  {
    pos = text.find_first_not_of(" \t\n\v\f\r", pos);
    if (pos == std::string_view::npos)
    {
      break;
    }

    // sscanf reads a plus sign, std::from_chars does not
    if (text[pos] == '+')
    {
      ++pos;
    }

    auto number = 0.0;
    const auto [end, error] =
      std::from_chars(text.data() + pos, text.data() + text.size(), number);
    if (error != std::errc{})
    {
      break;
    }

    result.push_back(number);
    pos = static_cast<std::size_t>(end - text.data());
  }
  return result;
}

/**
 * Reads up to N leading numbers of the given property over the given default values, as
 * idDict::GetMatrix and idDict::GetAngles start from a default and let sscanf set the
 * numbers it finds. A property that is missing, has no leading number or has a number
 * that is not finite gives the default values.
 */
template <std::size_t N>
std::array<double, N> parseIdTech4Numbers(
  const std::vector<EntityProperty>& properties,
  const std::string& propertyKey,
  std::array<double, N> result)
{
  const auto it = findEntityProperty(properties, propertyKey);
  if (it != std::end(properties))
  {
    const auto numbers = parseLeadingNumbers<N>(it->value());
    if (std::ranges::all_of(numbers, [](const auto n) { return vm::is_finite(n); }))
    {
      std::ranges::copy(numbers, std::begin(result));
    }
  }
  return result;
}

} // namespace

EntityRotationInfo entityRotationInfo(
  const Entity& entity, const EntityRotationPolicy policy)
{
  switch (policy)
  {
  case EntityRotationPolicy::Quake:
    return quakeEntityRotationInfo(entity);
  case EntityRotationPolicy::IdTech4:
    return idTech4EntityRotationInfo(entity);
    switchDefault();
  }
}

vm::mat4x4d entityRotation(
  const std::vector<EntityProperty>& properties, const EntityRotationInfo& info)
{
  switch (info.type)
  {
  case EntityRotationType::Angle: {
    const auto it = findEntityProperty(properties, info.propertyKey);
    if (it == std::end(properties) || it->value().empty())
    {
      return vm::mat4x4d::identity();
    }
    else
    {
      const auto angle = kdl::str_to_double(it->value()).value_or(0.0);
      return vm::rotation_matrix(vm::vec3d{0, 0, 1}, vm::to_radians(angle));
    }
  }
  case EntityRotationType::AngleUpDown: {
    const auto it = findEntityProperty(properties, info.propertyKey);
    if (it == std::end(properties) || it->value().empty())
    {
      return vm::mat4x4d::identity();
    }
    const auto angle = kdl::str_to_double(it->value()).value_or(0.0);
    if (angle == -1.0)
    {
      return vm::mat4x4d::rot_90_y_cw();
    }
    else if (angle == -2.0)
    {
      return vm::mat4x4d::rot_90_y_ccw();
    }
    else
    {
      return vm::rotation_matrix(vm::vec3d{0, 0, 1}, vm::to_radians(angle));
    }
  }
  case EntityRotationType::Euler: {
    const auto it = findEntityProperty(properties, info.propertyKey);
    const auto angles = it != std::end(properties)
                          ? vm::parse<double, 3>(it->value()).value_or(vm::vec3d{0, 0, 0})
                          : vm::vec3d{0, 0, 0};

    // x = -pitch
    // y =  yaw
    // z =  roll
    // pitch is applied with an inverted sign
    // see QuakeSpasm sources gl_rmain R_RotateForEntity function
    const auto roll = +vm::to_radians(angles.z());
    const auto pitch = -vm::to_radians(angles.x());
    const auto yaw = +vm::to_radians(angles.y());
    return vm::rotation_matrix(roll, pitch, yaw);
  }
  case EntityRotationType::Euler_PositivePitchDown: {
    const auto it = findEntityProperty(properties, info.propertyKey);
    const auto angles = it != std::end(properties)
                          ? vm::parse<double, 3>(it->value()).value_or(vm::vec3d{0, 0, 0})
                          : vm::vec3d{0, 0, 0};

    // x = pitch
    // y = yaw
    // z = roll
    const auto roll = +vm::to_radians(angles.z());
    const auto pitch = +vm::to_radians(angles.x());
    const auto yaw = +vm::to_radians(angles.y());
    return vm::rotation_matrix(roll, pitch, yaw);
  }
  case EntityRotationType::Mangle: {
    const auto it = findEntityProperty(properties, info.propertyKey);
    const auto angles = it != std::end(properties)
                          ? vm::parse<double, 3>(it->value()).value_or(vm::vec3d{0, 0, 0})
                          : vm::vec3d{0, 0, 0};

    // x = yaw
    // y = -pitch
    // z = roll
    const auto roll = +vm::to_radians(angles.z());
    const auto pitch = -vm::to_radians(angles.y());
    const auto yaw = +vm::to_radians(angles.x());
    return vm::rotation_matrix(roll, pitch, yaw);
  }
  case EntityRotationType::Matrix3x3: {
    // idDict::GetMatrix reads up to nine numbers over the identity as the rows of an
    // idMat3, and row i is the world direction of the entity's local axis i (forward,
    // left, up), i.e. column i of a rotation matrix here; "0 1 0 -1 0 0 0 0 1" and
    // "0 1 0 -1 0 0" turn the entity by 90 degrees
    const auto values = parseIdTech4Numbers<9>(
      properties, info.propertyKey, {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0});

    auto rotation = vm::mat4x4d::identity();
    for (size_t i = 0; i < values.size(); ++i)
    {
      rotation[i / 3][i % 3] = values[i];
    }
    return rotation;
  }
  case EntityRotationType::IdTech4Angles: {
    // idDict::GetAngles reads up to three numbers over 0 0 0
    const auto angles =
      parseIdTech4Numbers<3>(properties, info.propertyKey, {0.0, 0.0, 0.0});

    // pitch yaw roll, positive pitch points down, like Euler_PositivePitchDown
    const auto roll = +vm::to_radians(angles[2]);
    const auto pitch = +vm::to_radians(angles[0]);
    const auto yaw = +vm::to_radians(angles[1]);
    return vm::rotation_matrix(roll, pitch, yaw);
  }
  case EntityRotationType::None:
    return vm::mat4x4d::identity();
    switchDefault();
  }
}

vm::mat4x4d entityRotation(const Entity& entity, const EntityRotationPolicy policy)
{
  return entityRotation(entity.properties(), entityRotationInfo(entity, policy));
}

vm::vec3d entityYawPitchRoll(
  const vm::mat4x4d& transformation, const vm::mat4x4d& rotation)
{
  const auto M = vm::strip_translation(transformation) * vm::strip_translation(rotation);

  const auto newPosX = vm::normalize(M * vm::vec3d{1, 0, 0});
  const auto newPosY = vm::normalize(vm::cross(M * vm::vec3d{0, 0, 1}, newPosX));
  const auto newPosZ = vm::normalize(vm::cross(newPosX, newPosY));

  // Build a new rotation matrix from the three transformed unit vectors
  vm::mat4x4d rotMat;
  rotMat[0] = vm::vec4d(newPosX, 0.0);
  rotMat[1] = vm::vec4d(newPosY, 0.0);
  rotMat[2] = vm::vec4d(newPosZ, 0.0);

  const auto rollPitchYaw = vm::rotation_matrix_to_euler_angles(rotMat);

  return vm::vec3d(
    vm::to_degrees(rollPitchYaw[2]),
    vm::to_degrees(rollPitchYaw[1]),
    vm::to_degrees(rollPitchYaw[0]));
}

std::string formatIdTech4Float(const double value, const int decimals)
{
  // with no decimals, stripping the trailing zeros would turn 300 into 3
  contract_pre(decimals >= 1);

  auto result = fmt::format("{:.{}f}", value, decimals);
  result.erase(result.find_last_not_of('0') + 1);
  if (result.ends_with('.'))
  {
    result.pop_back();
  }
  return result == "-0" ? "0" : result;
}

namespace
{
double getEntityRotationAngle(vm::vec3d direction)
{
  direction[2] = 0.0;
  direction = normalize(direction);

  auto angle = vm::round(vm::to_degrees(std::acos(direction.x())));
  if (direction.y() < double(0.0))
  {
    angle = 360.0 - angle;
  }
  angle = vm::normalize_degrees(angle);
  return angle;
}

EntityProperty setEntityRotationAngle(
  const std::string& propertyKey, const vm::vec3d& direction)
{
  const auto angle = getEntityRotationAngle(direction);
  return {propertyKey, kdl::str_to_string(vm::round(angle))};
}

/**
 * Formats an angle the way the engine's idDict::SetAngles writes each angle
 * (idStr::FloatArrayToString with two decimals): with two decimals, then without trailing
 * zeros and a trailing dot, e.g. 90, 33.3 or 0.05. An angle that comes out as -0 is
 * written as 0.
 */
std::string formatIdTech4Angle(const double angle)
{
  return formatIdTech4Float(angle, 2);
}

/**
 * Returns the given rotation, transformed, as pitch, yaw and roll, with a positive pitch
 * pointing down, written the way the engine writes angles. Returns nothing for a
 * degenerate rotation, which has no angles.
 */
std::optional<EntityProperty> idTech4AnglesProperty(
  const std::string& propertyKey,
  const vm::mat4x4d& transformation,
  const vm::mat4x4d& rotation)
{
  const auto yawPitchRoll = entityYawPitchRoll(transformation, rotation);
  if (vm::is_nan(yawPitchRoll))
  {
    return std::nullopt;
  }

  return EntityProperty{
    propertyKey,
    formatIdTech4Angle(yawPitchRoll.y()) + " " + formatIdTech4Angle(yawPitchRoll.x())
      + " " + formatIdTech4Angle(yawPitchRoll.z())};
}

/**
 * Returns the yaw of the given direction as "angle", from 0 to 360 degrees and written
 * the way the engine writes angles. Returns nothing for a direction straight up or down,
 * which has no yaw.
 */
std::optional<EntityProperty> idTech4AngleProperty(const vm::vec3d& direction)
{
  if (vm::is_zero(direction.xy(), vm::Cd::almost_zero()))
  {
    return std::nullopt;
  }

  const auto yaw =
    vm::normalize_degrees(vm::to_degrees(std::atan2(direction.y(), direction.x())));
  return EntityProperty{EntityPropertyKeys::Angle, formatIdTech4Angle(yaw)};
}

} // namespace

std::optional<EntityProperty> applyEntityRotation(
  const std::vector<EntityProperty>& properties,
  const EntityRotationInfo& info,
  const vm::mat4x4d& transformation)
{
  if (info.usage == EntityRotationUsage::BlockRotation)
  {
    return std::nullopt;
  }

  const auto rotation = entityRotation(properties, info);
  switch (info.type)
  {
  case EntityRotationType::Angle: {
    const auto direction = normalize(transformation * rotation * vm::vec3d{1, 0, 0});
    return setEntityRotationAngle(info.propertyKey, direction);
  }
  case EntityRotationType::AngleUpDown: {
    const auto direction = normalize(transformation * rotation * vm::vec3d{1, 0, 0});
    if (direction.z() > 0.9)
    {
      return {{info.propertyKey, "-1"}};
    }
    if (direction.z() < -0.9)
    {
      return {{info.propertyKey, "-2"}};
    }
    return setEntityRotationAngle(info.propertyKey, direction);
  }
  case EntityRotationType::Euler: {
    const auto yawPitchRoll = entityYawPitchRoll(transformation, rotation);
    const auto nPitchYawRoll =
      vm::vec3d(-yawPitchRoll.y(), yawPitchRoll.x(), yawPitchRoll.z());
    return {{info.propertyKey, kdl::str_to_string(vm::round(nPitchYawRoll))}};
  }
  case EntityRotationType::Euler_PositivePitchDown: {
    const auto yawPitchRoll = entityYawPitchRoll(transformation, rotation);
    const auto nPitchYawRoll =
      vm::vec3d(yawPitchRoll.y(), yawPitchRoll.x(), yawPitchRoll.z());
    return {{info.propertyKey, kdl::str_to_string(vm::round(nPitchYawRoll))}};
  }
  case EntityRotationType::Mangle: {
    const auto yawPitchRoll = entityYawPitchRoll(transformation, rotation);
    const auto yawNPitchRoll =
      vm::vec3d(yawPitchRoll.x(), -yawPitchRoll.y(), yawPitchRoll.z());
    return {{info.propertyKey, kdl::str_to_string(vm::round(yawNPitchRoll))}};
  }
  case EntityRotationType::IdTech4Angles:
    return idTech4AnglesProperty(info.propertyKey, transformation, rotation);
  case EntityRotationType::Matrix3x3:
    // the id Tech 4 policy replaces a rotation matrix by "angles"
  case EntityRotationType::None:
    return std::nullopt;
    switchDefault();
  }
}

std::vector<std::string> idTech4ConvertedRotationKeys(const Entity& entity)
{
  static const auto lightKeys = std::vector<std::string>{
    EntityPropertyKeys::LightRotation,
    EntityPropertyKeys::Rotation,
    EntityPropertyKeys::LightAngles,
  };
  static const auto otherKeys = std::vector<std::string>{
    EntityPropertyKeys::Rotation,
  };

  return (isIdTech4Light(entity) ? lightKeys : otherKeys)
         | std::views::filter([&](const auto& key) { return entity.hasProperty(key); })
         | kdl::ranges::to<std::vector>();
}

EntityRotationUpdate idTech4AnglesUpdate(
  const Entity& entity, const vm::vec3d& pitchYawRoll)
{
  // the engine reads "angles" only if none of these is present, and "angle" only if
  // "angles" is not present
  auto remove = idTech4ConvertedRotationKeys(entity);
  if (entity.hasProperty(EntityPropertyKeys::Angle))
  {
    remove.push_back(EntityPropertyKeys::Angle);
  }

  return {
    EntityProperty{
      EntityPropertyKeys::Angles,
      formatIdTech4Angle(pitchYawRoll.x()) + " " + formatIdTech4Angle(pitchYawRoll.y())
        + " " + formatIdTech4Angle(pitchYawRoll.z())},
    std::move(remove),
  };
}

namespace
{

EntityRotationUpdate applyIdTech4EntityRotation(
  const Entity& entity, const vm::mat4x4d& transformation)
{
  if (entity.classname() == EntityPropertyValues::NoClassname)
  {
    return {};
  }

  const auto info = idTech4EntityRotationInfo(entity);
  if (!entity.pointEntity())
  {
    // The brushes are transformed themselves, so the entity never gets "angles" or
    // "rotation", which would turn them a second time in the game. Only "angle", the
    // direction of a mover, is turned, in whole degrees as upstream.
    return {applyEntityRotation(entity.properties(), info, transformation), {}};
  }

  const auto rotation = entityRotation(entity.properties(), info);

  // a degenerate rotation, e.g. of a "rotation" with only three numbers, has no angles
  const auto forward = vm::vec3d{rotation[0].xyz()};
  const auto left = vm::vec3d{rotation[1].xyz()};
  const auto up = vm::vec3d{rotation[2].xyz()};
  if (vm::is_zero(vm::dot(forward, vm::cross(left, up)), vm::Cd::almost_zero()))
  {
    return {};
  }

  if (idTech4KeepsAngle(entity))
  {
    // a direction straight up or down has no yaw, so keep the old one
    if (auto angle = idTech4AngleProperty(transformation * rotation * vm::vec3d{1, 0, 0}))
    {
      return {std::move(angle), {}};
    }
    return {};
  }

  auto angles =
    idTech4AnglesProperty(EntityPropertyKeys::Angles, transformation, rotation);
  if (!angles)
  {
    return {};
  }

  // the engine reads "angles" only if none of these is present, and "angle" only if
  // "angles" is not present
  auto remove = idTech4ConvertedRotationKeys(entity);
  if (entity.hasProperty(EntityPropertyKeys::Angle))
  {
    remove.push_back(EntityPropertyKeys::Angle);
  }

  return {std::move(angles), std::move(remove)};
}

} // namespace

EntityRotationUpdate applyEntityRotation(
  const Entity& entity,
  const vm::mat4x4d& transformation,
  const EntityRotationPolicy policy)
{
  switch (policy)
  {
  case EntityRotationPolicy::Quake:
    return {
      applyEntityRotation(
        entity.properties(), entityRotationInfo(entity), transformation),
      {},
    };
  case EntityRotationPolicy::IdTech4:
    return applyIdTech4EntityRotation(entity, transformation);
    switchDefault();
  }
}

void applyEntityRotationUpdate(Entity& entity, const EntityRotationUpdate& update)
{
  if (update.set)
  {
    entity.addOrUpdateProperty(update.set->key(), update.set->value());
  }

  for (const auto& key : update.remove)
  {
    entity.removeProperty(key);
  }
}

} // namespace tb::mdl
