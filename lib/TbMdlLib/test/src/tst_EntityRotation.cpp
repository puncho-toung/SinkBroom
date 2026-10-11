/*
 Copyright (C) 2020 Kristian Duske

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

#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityRotation.h"
#include "mdl/PropertyDefinition.h"

#include "kd/string_utils.h"

#include "vm/approx.h"
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/mat_io.h" // IWYU pragma: keep
#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
namespace
{

struct EntityDefinitionInfo
{
  std::vector<PropertyDefinition> propertyDefinitions;
  vm::bbox3d bounds = vm::bbox3d{16.0};
};

std::optional<EntityDefinition> createEntityDefinition(
  const std::optional<EntityDefinitionInfo>& info)
{
  if (info)
  {
    return EntityDefinition{
      "some_name",
      Color{},
      "",
      info->propertyDefinitions,
      PointEntityDefinition{
        info->bounds,
        {},
        {},
      },
    };
  }

  return std::nullopt;
}

} // namespace

TEST_CASE("EntityRotation")
{
  SECTION("entityRotationInfo")
  {
    using namespace mdl;

    auto manglePropertyDef =
      PropertyDefinition{"mangle", PropertyValueTypes::String{}, "", ""};
    auto targetPropertyDef =
      PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""};

    auto normalPitch = EntityModel{
      "",
      createEntityModelDataResource(
        EntityModelData{PitchType::Normal, Orientation::Oriented})};
    auto invertedPitch = EntityModel{
      "",
      createEntityModelDataResource(
        EntityModelData{PitchType::MdlInverted, Orientation::Oriented})};

    using T = std::tuple<
      std::vector<EntityProperty>,
      bool,
      std::optional<EntityDefinitionInfo>,
      EntityModel*,
      EntityRotationInfo>;

    // clang-format off
    const auto
    [entityProperties,              point, entityDefinitionInfo,            entityModel,    expectedRotationInfo] = GENERATE_REF(values<T>({
    {{},                            false, std::nullopt,                    nullptr,        {EntityRotationType::None, "", EntityRotationUsage::Allowed}},
    // a light with a mangle key
    {{{"classname", "light"},
      {"mangle",    "0 0 0"}},      true,  std::nullopt,                    nullptr,        {EntityRotationType::Mangle, "mangle", EntityRotationUsage::Allowed}},

    // a light without a target key and with an angles key, type is controlled by the model's pitch type (default is normal)
    {{{"classname", "light"},
      {"angles",    "0 0 0"}},      true,  std::nullopt,                    nullptr,        {EntityRotationType::Euler_PositivePitchDown, "angles", EntityRotationUsage::Allowed}},

    // a light without a target key and with an angle key
    {{{"classname", "light"},
      {"angle",    "0"}},           true,  std::nullopt,                    nullptr,        {EntityRotationType::Angle, "angle", EntityRotationUsage::Allowed}},

    // a light without a target key and with an angles key, type is controlled by the model's pitch type (normal)
    {{{"classname", "light"},
      {"angles",    "0 0 0"}},      true,  std::nullopt,                    &normalPitch,   {EntityRotationType::Euler_PositivePitchDown, "angles", EntityRotationUsage::Allowed}},

    // a light without a target key and with an angles key, type is controlled by the model's pitch type (inverted)
    {{{"classname", "light"},
      {"angles",    "0 0 0"}},      true,  std::nullopt,                    &invertedPitch, {EntityRotationType::Euler, "angles", EntityRotationUsage::Allowed}},

    // a light without a target key and without an angles key
    {{{"classname", "light"}},      true,  std::nullopt,                    nullptr,        {EntityRotationType::None, "", EntityRotationUsage::Allowed}},

    // a light with a target key
    {{{"classname", "light"},
      {"angles",    "0 0 0"},
      {"target", "xyz"}},           true,  {{{targetPropertyDef}}},         nullptr,        {EntityRotationType::None, "", EntityRotationUsage::Allowed}},

    // non-light brush entity without additional keys
    {{{"classname", "other"}},      false, std::nullopt,                    nullptr,        {EntityRotationType::None, "", EntityRotationUsage::Allowed}},

    // non-light brush entity with angles key
    {{{"classname", "other"},
      {"angles", "0 0 0"}},         false, std::nullopt,                    nullptr,        {EntityRotationType::Euler_PositivePitchDown, "angles", EntityRotationUsage::Allowed}},

    // non-light brush entity with mangle key
    {{{"classname", "other"},
      {"mangle", "0 0 0"}},         false, std::nullopt,                    &invertedPitch, {EntityRotationType::Euler, "mangle", EntityRotationUsage::Allowed}},

    // non-light brush entity with mangle key (model controls the euler type)
    {{{"classname", "other"},
      {"mangle", "0 0 0"}},         false, std::nullopt,                    nullptr,        {EntityRotationType::Euler_PositivePitchDown, "mangle", EntityRotationUsage::Allowed}},

    // non-light brush entity with angle key
    {{{"classname", "other"},
      {"angle", "0"}},              false, std::nullopt,                    nullptr,        {EntityRotationType::AngleUpDown, "angle", EntityRotationUsage::Allowed}},

    // non-light point entity without additional keys
    {{{"classname", "other"}},      true,  std::nullopt,                    nullptr,        {EntityRotationType::AngleUpDown, "angle", EntityRotationUsage::Allowed}},

    // non-light point entity with angles key
    {{{"classname", "other"},
      {"angles", "0 0 0"}},         true,  std::nullopt,                    nullptr,        {EntityRotationType::Euler_PositivePitchDown, "angles", EntityRotationUsage::Allowed}},

    // non-light point entity with angles key (model controls the euler type)
    {{{"classname", "other"},
      {"angles", "0 0 0"}},         true,  std::nullopt,                    &invertedPitch, {EntityRotationType::Euler, "angles", EntityRotationUsage::Allowed}},

    // non-light point entity with mangle key
    {{{"classname", "other"},
      {"mangle", "0 0 0"}},         true,  std::nullopt,                    nullptr,        {EntityRotationType::Euler_PositivePitchDown, "mangle", EntityRotationUsage::Allowed}},

    // non-light point entity with mangle key and off-center definition bounds
    {{{"classname", "other"},
      {"mangle", "0 0 0"}},         true,  {{{},
                                             {{0, 0, -16}, {16, 16, 16}}}}, nullptr,        {EntityRotationType::Euler_PositivePitchDown, "mangle", EntityRotationUsage::BlockRotation}},

    // a property definition counts as a property even if the property isn't present
    {{{"classname", "other"}},      true,  {{{manglePropertyDef}}},         nullptr,        {EntityRotationType::Euler_PositivePitchDown, "mangle", EntityRotationUsage::Allowed}},

    // prefer actual existing properties over definitions when determining the rotation type
    {{{"classname", "other"},
      {"angle", "45"}},             true,  {{{manglePropertyDef}}},         nullptr,        {EntityRotationType::AngleUpDown, "angle", EntityRotationUsage::Allowed}},

    // but not for light entities
    {{{"classname", "light"}},      true,  {{{manglePropertyDef}}},         nullptr,        {EntityRotationType::None, "", EntityRotationUsage::Allowed}},
    }));
    // clang-format on

    CAPTURE(entityProperties, point, entityDefinitionInfo, entityModel);

    const auto entityDefinition = createEntityDefinition(entityDefinitionInfo);

    auto entity = Entity{entityProperties};
    if (entityDefinition)
    {
      entity.setDefinition(&*entityDefinition);
    }
    entity.setModel(entityModel);
    entity.setPointEntity(point);

    CHECK(entityRotationInfo(entity) == expectedRotationInfo);
  }

  SECTION("entityRotationInfo with the idtech4 policy")
  {
    using ERT = EntityRotationType;
    using ERU = EntityRotationUsage;

    auto anglePropertyDef =
      PropertyDefinition{"angle", PropertyValueTypes::String{}, "", ""};
    auto anglesPropertyDef =
      PropertyDefinition{"angles", PropertyValueTypes::String{}, "", ""};
    auto targetPropertyDef =
      PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""};

    auto invertedPitch = EntityModel{
      "",
      createEntityModelDataResource(
        EntityModelData{PitchType::MdlInverted, Orientation::Oriented})};

    using T = std::tuple<
      std::vector<EntityProperty>,
      bool,
      std::optional<EntityDefinitionInfo>,
      EntityModel*,
      EntityRotationInfo>;

    // clang-format off
    const auto
    [entityProperties,                         point, entityDefinitionInfo,                              entityModel,    expectedRotationInfo] = GENERATE_REF(values<T>({
    // an entity without a classname is not rotated
    {{},                                       true,  std::nullopt,                                      nullptr,        {ERT::None, "", ERU::Allowed}},

    // a light reads light_rotation, rotation, light_angles, angles and angle in this order
    {{{"classname", "light"},
      {"light_rotation", "1 0 0 0 1 0 0 0 1"},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::Matrix3x3, "light_rotation", ERU::Allowed}},
    {{{"classname", "light"},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::Matrix3x3, "rotation", ERU::Allowed}},
    {{{"classname", "light"},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "light_angles", ERU::Allowed}},
    {{{"classname", "light"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},
    {{{"classname", "light"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::Angle, "angle", ERU::Allowed}},

    // a present property wins even if its value is empty
    {{{"classname", "light"},
      {"light_rotation", ""},
      {"angles", "0 90 0"}},                   true,  std::nullopt,                                      nullptr,        {ERT::Matrix3x3, "light_rotation", ERU::Allowed}},

    // a light without a rotation property gets angles, even if its definition only declares angle
    {{{"classname", "light"}},                 true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},
    {{{"classname", "light"}},                 true,  {{{anglePropertyDef}}},                            nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // mangle is not read, and a light's classname is compared without regard to case
    {{{"classname", "Light_Flicker"},
      {"mangle", "0 0 0"}},                    true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // a light with a target is rotated, and the model's pitch type does not matter
    {{{"classname", "light"},
      {"angles", "0 0 0"},
      {"target", "xyz"}},                      true,  {{{targetPropertyDef}}},                           &invertedPitch, {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // other point entities read rotation, angles and angle in this order
    {{{"classname", "other"},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::Matrix3x3, "rotation", ERU::Allowed}},
    {{{"classname", "other"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},
    {{{"classname", "other"},
      {"angle", "0"}},                         true,  std::nullopt,                                      nullptr,        {ERT::Angle, "angle", ERU::Allowed}},

    // a present property wins even if its value is empty
    {{{"classname", "other"},
      {"rotation", ""},
      {"angles", "0 90 0"}},                   true,  std::nullopt,                                      nullptr,        {ERT::Matrix3x3, "rotation", ERU::Allowed}},
    {{{"classname", "other"},
      {"angles", ""},
      {"angle", "90"}},                        true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // only lights read light_rotation and light_angles
    {{{"classname", "other"},
      {"light_rotation", "1 0 0 0 1 0 0 0 1"},
      {"light_angles", "0 0 0"},
      {"angle", "90"}},                        true,  std::nullopt,                                      nullptr,        {ERT::Angle, "angle", ERU::Allowed}},

    // without a rotation property, an entity gets angles, or angle if its definition declares angle but not angles
    {{{"classname", "other"}},                 true,  std::nullopt,                                      nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},
    {{{"classname", "other"}},                 true,  {{{anglePropertyDef}}},                            nullptr,        {ERT::Angle, "angle", ERU::Allowed}},
    {{{"classname", "other"}},                 true,  {{{anglePropertyDef, anglesPropertyDef}}},         nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},
    {{{"classname", "other"},
      {"angle", "90"}},                        true,  {{{anglePropertyDef}}},                            nullptr,        {ERT::Angle, "angle", ERU::Allowed}},
    {{{"classname", "other"},
      {"angles", "0 90 0"}},                   true,  {{{anglePropertyDef}}},                            nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // off-center definition bounds don't block the rotation
    {{{"classname", "other"},
      {"angles", "0 0 0"}},                    true,  {{{},
                                                         {{0, 0, -16}, {16, 16, 16}}}},             nullptr,        {ERT::IdTech4Angles, "angles", ERU::Allowed}},

    // an entity with brushes shows only the direction of its angle, whatever its other properties
    {{{"classname", "other"},
      {"rotation", "0 1 0 -1 0 0 0 0 1"},
      {"angles", "0 90 0"},
      {"angle", "90"}},                        false, std::nullopt,                                      nullptr,        {ERT::AngleUpDown, "angle", ERU::Allowed}},
    {{{"classname", "other"},
      {"rotation", "0 1 0 -1 0 0 0 0 1"},
      {"angles", "0 90 0"}},                   false, std::nullopt,                                      nullptr,        {ERT::None, "", ERU::Allowed}},
    {{{"classname", "light"},
      {"angles", "0 90 0"}},                   false, std::nullopt,                                      nullptr,        {ERT::None, "", ERU::Allowed}},
    }));
    // clang-format on

    CAPTURE(entityProperties, point, entityDefinitionInfo, entityModel);

    const auto entityDefinition = createEntityDefinition(entityDefinitionInfo);

    auto entity = Entity{entityProperties};
    if (entityDefinition)
    {
      entity.setDefinition(&*entityDefinition);
    }
    entity.setModel(entityModel);
    entity.setPointEntity(point);

    CHECK(
      entityRotationInfo(entity, EntityRotationPolicy::IdTech4) == expectedRotationInfo);
  }

  SECTION("entityRotation")
  {
    using T = std::tuple<std::vector<EntityProperty>, EntityRotationInfo, vm::mat4x4d>;
    using ERT = EntityRotationType;
    const auto usage =
      GENERATE(EntityRotationUsage::Allowed, EntityRotationUsage::BlockRotation);

    // clang-format off
    const auto
    [properties,              info,                          expectedTransformation] = GENERATE_COPY(values<T>({
    {{},                      {ERT::Angle,                   "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "90"}},       {ERT::Angle,                   "angle", usage}, vm::mat4x4d::rot_90_z_ccw()},
    {{},                      {ERT::AngleUpDown,             "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "90"}},       {ERT::AngleUpDown,             "angle", usage}, vm::mat4x4d::rot_90_z_ccw()},
    {{{"angle", "-1"}},       {ERT::AngleUpDown,             "angle", usage}, vm::mat4x4d::rot_90_y_cw()},
    {{{"angle", "-2"}},       {ERT::AngleUpDown,             "angle", usage}, vm::mat4x4d::rot_90_y_ccw()},
    {{},                      {ERT::Euler,                   "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "30 60 90"}}, {ERT::Euler,                   "angle", usage}, vm::rotation_matrix(vm::to_radians(90.0), vm::to_radians(-30.0), vm::to_radians(60.0))},
    {{},                      {ERT::Euler_PositivePitchDown, "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "30 60 90"}}, {ERT::Euler_PositivePitchDown, "angle", usage}, vm::rotation_matrix(vm::to_radians(90.0), vm::to_radians(30.0), vm::to_radians(60.0))},
    {{},                      {ERT::Mangle,                  "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "30 60 90"}}, {ERT::Mangle,                  "angle", usage}, vm::rotation_matrix(vm::to_radians(90.0), vm::to_radians(-60.0), vm::to_radians(30.0))},
    {{},                      {ERT::None,                    "angle", usage}, vm::mat4x4d::identity()},
    {{{"angle", "30 60 90"}}, {ERT::None,                    "angle", usage}, vm::mat4x4d::identity()},
    }));
    // clang-format on

    CAPTURE(properties, info);

    CHECK(entityRotation(properties, info) == vm::approx{expectedTransformation});
  }

  SECTION("entityRotation with a rotation matrix")
  {
    using T = std::tuple<std::vector<EntityProperty>, vm::mat4x4d>;

    const auto info = EntityRotationInfo{
      EntityRotationType::Matrix3x3, "rotation", EntityRotationUsage::Allowed};

    // clang-format off
    const auto
    [properties,                                  expectedTransformation] = GENERATE(values<T>({
    {{},                                          vm::mat4x4d::identity()},
    {{{"rotation", ""}},                          vm::mat4x4d::identity()},
    // the columns of the rotation are the forward, left and up axes: yaw 90
    {{{"rotation", "0 1 0 -1 0 0 0 0 1"}},        vm::mat4x4d::rot_90_z_ccw()},
    // pitch 90, positive pitch points down
    {{{"rotation", "0 0 -1 0 1 0 1 0 0"}},        vm::rotation_matrix(0.0, vm::to_radians(90.0), 0.0)},
    // like the engine, read the first nine numbers
    {{{"rotation", "0 1 0 -1 0 0 0 0 1 5"}},      vm::mat4x4d::rot_90_z_ccw()},
    // like the engine, fewer than nine numbers are read over the identity
    {{{"rotation", "0 1 0 -1 0 0 0 0"}},          vm::mat4x4d::rot_90_z_ccw()},
    {{{"rotation", "0 1 0 -1 0 0 0 0 x"}},        vm::mat4x4d::rot_90_z_ccw()},
    {{{"rotation", "0 1 0 -1 0 0"}},              vm::mat4x4d::rot_90_z_ccw()},
    {{{"rotation", "+0 +1 0 -1 0 0"}},            vm::mat4x4d::rot_90_z_ccw()},
    // a degenerate matrix: forward and left are both +Y
    {{{"rotation", "0 1 0"}},                     vm::mat4x4d{0, 0, 0, 0,
                                                              1, 1, 0, 0,
                                                              0, 0, 1, 0,
                                                              0, 0, 0, 1}},
    // no number, or a number that is not finite
    {{{"rotation", "x 1 0 -1 0 0"}},              vm::mat4x4d::identity()},
    {{{"rotation", "nan 0 0 0 1 0 0 0 1"}},       vm::mat4x4d::identity()},
    {{{"rotation", "inf 0 0 0 1 0 0 0 1"}},       vm::mat4x4d::identity()},
    {{{"rotation", "0 1 0 -1 0 0 0 0 -inf"}},     vm::mat4x4d::identity()},
    {{{"rotation", "nan nan nan nan nan nan nan nan nan"}},
                                                  vm::mat4x4d::identity()},
    }));
    // clang-format on

    CAPTURE(properties);

    CHECK(entityRotation(properties, info) == vm::approx{expectedTransformation});
  }

  SECTION("entityRotation with id Tech 4 angles")
  {
    using T = std::tuple<std::vector<EntityProperty>, vm::mat4x4d>;

    const auto info = EntityRotationInfo{
      EntityRotationType::IdTech4Angles, "angles", EntityRotationUsage::Allowed};
    const auto yaw90 = vm::mat4x4d::rot_90_z_ccw();
    const auto pitch30 = vm::rotation_matrix(0.0, vm::to_radians(30.0), 0.0);

    // clang-format off
    const auto
    [properties,                                  expectedTransformation] = GENERATE_COPY(values<T>({
    {{},                                          vm::mat4x4d::identity()},
    {{{"angles", ""}},                            vm::mat4x4d::identity()},
    {{{"angles", "0 90 0"}},                      yaw90},
    // like idDict::GetAngles, fewer than three numbers are read over 0 0 0
    {{{"angles", "0 90"}},                        yaw90},
    {{{"angles", "30"}},                          pitch30},
    {{{"angles", "0 90 x"}},                      yaw90},
    {{{"angles", "0 90 0 5"}},                    yaw90},
    // no number, or a number that is not finite
    {{{"angles", "x 90 0"}},                      vm::mat4x4d::identity()},
    {{{"angles", "nan 90 0"}},                    vm::mat4x4d::identity()},
    }));
    // clang-format on

    CAPTURE(properties);

    CHECK(entityRotation(properties, info) == vm::approx{expectedTransformation});
  }

  SECTION("entityRotation with the idtech4 policy")
  {
    const auto rotation =
      [](std::vector<EntityProperty> properties, const bool point = true) {
        auto entity = Entity{std::move(properties)};
        entity.setPointEntity(point);
        return entityRotation(entity, EntityRotationPolicy::IdTech4);
      };

    SECTION("angles follow idAngles::ToMat3")
    {
      // idAngles::ToMat3 (idlib/math/Angles.cpp) for pitch 30, yaw 45 and roll 60:
      //   sp = 1/2, cp = sqrt(3)/2, sy = cy = sqrt(2)/2, sr = sqrt(3)/2, cr = 1/2
      //   forward = mat[0] = (cp*cy,               cp*sy,               -sp)
      //                    = (0.612372,            0.612372,            -0.5)
      //   left    = mat[1] = (sr*sp*cy - cr*sy,    sr*sp*sy + cr*cy,    sr*cp)
      //                    = (0.306186 - 0.353553, 0.306186 + 0.353553, 0.75)
      //   up      = mat[2] = (cr*sp*cy + sr*sy,    cr*sp*sy - sr*cy,    cr*cp)
      //                    = (0.176777 + 0.612372, 0.176777 - 0.612372, 0.433013)
      const auto forward = vm::vec3d{0.612372, 0.612372, -0.5};
      const auto left = vm::vec3d{-0.047367, 0.659740, 0.75};
      const auto up = vm::vec3d{0.789149, -0.435596, 0.433013};

      const auto checkAxes = [&](const vm::mat4x4d& actual) {
        CHECK(actual * vm::vec3d{1, 0, 0} == vm::approx{forward, 1e-6});
        CHECK(actual * vm::vec3d{0, 1, 0} == vm::approx{left, 1e-6});
        CHECK(actual * vm::vec3d{0, 0, 1} == vm::approx{up, 1e-6});
      };

      checkAxes(rotation({{"classname", "other"}, {"angles", "30 45 60"}}));
      checkAxes(rotation({{"classname", "light"}, {"angles", "30 45 60"}}));

      // a light normalizes light_angles to 0..360, which changes nothing
      checkAxes(rotation({{"classname", "light"}, {"light_angles", "-330 405 420"}}));

      // the same rotation as a matrix
      checkAxes(rotation({
        {"classname", "other"},
        {"rotation",
         "0.612372 0.612372 -0.5 -0.047367 0.659740 0.75 0.789149 -0.435596 0.433013"},
      }));
    }

    SECTION("rotation, angles and angle agree on yaw 90")
    {
      const auto yaw90 = vm::mat4x4d::rot_90_z_ccw();
      CHECK(
        rotation({{"classname", "other"}, {"rotation", "0 1 0 -1 0 0 0 0 1"}})
        == vm::approx{yaw90});
      CHECK(
        rotation({{"classname", "other"}, {"angles", "0 90 0"}}) == vm::approx{yaw90});
      CHECK(rotation({{"classname", "other"}, {"angle", "90"}}) == vm::approx{yaw90});
      CHECK(
        rotation({{"classname", "light"}, {"light_rotation", "0 1 0 -1 0 0 0 0 1"}})
        == vm::approx{yaw90});
    }

    SECTION("angle is a plain yaw")
    {
      CHECK(
        rotation({{"classname", "light"}, {"angle", "-1"}})
        == vm::approx{vm::rotation_matrix(0.0, 0.0, vm::to_radians(-1.0))});
      CHECK(
        rotation({{"classname", "other"}, {"angle", "-2"}})
        == vm::approx{vm::rotation_matrix(0.0, 0.0, vm::to_radians(-2.0))});
    }

    SECTION("mangle is not read")
    {
      const auto properties =
        std::vector<EntityProperty>{{"classname", "light"}, {"mangle", "90 0 0"}};
      REQUIRE(
        entityRotation(Entity{properties}, EntityRotationPolicy::Quake)
        == vm::approx{vm::mat4x4d::rot_90_z_ccw()});
      CHECK(rotation(properties) == vm::mat4x4d::identity());
    }

    SECTION("An entity with brushes shows only the direction of its angle")
    {
      CHECK(
        rotation({{"classname", "other"}, {"angles", "0 90 0"}}, false)
        == vm::mat4x4d::identity());
      CHECK(
        rotation({{"classname", "other"}, {"rotation", "0 1 0 -1 0 0 0 0 1"}}, false)
        == vm::mat4x4d::identity());

      // like idMover_Binary::GetMovedir, -1 is up and -2 is down
      CHECK(
        rotation({{"classname", "func_door"}, {"angle", "90"}}, false)
        == vm::approx{vm::mat4x4d::rot_90_z_ccw()});
      CHECK(
        rotation({{"classname", "func_door"}, {"angle", "-1"}}, false)
          * vm::vec3d{1, 0, 0}
        == vm::approx{vm::vec3d{0, 0, 1}});
      CHECK(
        rotation({{"classname", "func_door"}, {"angle", "-2"}}, false)
          * vm::vec3d{1, 0, 0}
        == vm::approx{vm::vec3d{0, 0, -1}});
    }
  }

  SECTION("entityYawPitchRoll")
  {
    using T = std::tuple<double, double, double, vm::mat4x4d, vm::vec3d>;

    // clang-format off
    const auto
    [roll, pitch, yaw, transformation, expectedYawPitchRoll] = GENERATE(values<T>({
    {12.0, 13.0, 14.0, vm::mat4x4d::identity(),                 {14, 13, 12}},
    {12.0, 13.0, 14.0, vm::scaling_matrix(vm::vec3d{ 2, 2, 2}), {14, 13, 12}},
    {0.0,  45.0,  0.0, vm::scaling_matrix(vm::vec3d{ 2, 1, 1}), {0, vm::to_degrees(std::atan(0.5)), 0}},
    {10.0, 45.0,  0.0, vm::scaling_matrix(vm::vec3d{-1, 1, 1}), {180, 45, -10}},
    }));
    // clang-format on

    CAPTURE(roll, pitch, yaw, transformation);

    const auto rotation = vm::rotation_matrix(
      vm::to_radians(roll), vm::to_radians(pitch), vm::to_radians(yaw));

    CHECK(
      entityYawPitchRoll(transformation, rotation) == vm::approx{expectedYawPitchRoll});
  }

  SECTION("applyEntityRotation")
  {
    using T = std::tuple<
      std::vector<EntityProperty>,
      EntityRotationInfo,
      vm::mat4x4d,
      std::optional<EntityProperty>>;
    using ERT = EntityRotationType;
    using ERU = EntityRotationUsage;

    // clang-format off
    const auto
    [properties,        info,                                      transform, expectedProperty] = GENERATE_COPY(values<T>({
    {{{"angle", "45"}},         {ERT::Angle,                   "angle", ERU::Allowed},       vm::mat4x4d::rot_90_z_ccw(),                           {{"angle", "135"}}},
    {{{"angle", "45"}},         {ERT::Angle,                   "angle", ERU::BlockRotation}, vm::mat4x4d::rot_90_z_ccw(),                           {}},

    {{{"angle", "45"}},         {ERT::AngleUpDown,             "angle", ERU::Allowed},       vm::mat4x4d::rot_90_z_ccw(),                           {{"angle", "135"}}},
    {{{"angle",  "0"}},         {ERT::AngleUpDown,             "angle", ERU::Allowed},       vm::rotation_matrix(0.0, vm::to_radians(-90.0), 0.0), {{"angle", "-1"}}},
    {{{"angle",  "0"}},         {ERT::AngleUpDown,             "angle", ERU::Allowed},       vm::rotation_matrix(0.0, vm::to_radians(90.0), 0.0),  {{"angle", "-2"}}},
  
    {{{"angle",  "30 60 90"}},  {ERT::Euler,                   "angle", ERU::Allowed},       vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {{"angle", "0 0 0"}}},
    {{{"angle",  "30 60 90"}},  {ERT::Euler,                   "angle", ERU::BlockRotation}, vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {}},

    {{{"angle",  "-30 60 90"}}, {ERT::Euler_PositivePitchDown, "angle", ERU::Allowed},       vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {{"angle", "0 0 0"}}},
    {{{"angle",  "-30 60 90"}}, {ERT::Euler_PositivePitchDown, "angle", ERU::BlockRotation}, vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {}},

    {{{"angle",  "60 30 90"}}, {ERT::Mangle,                  "angle", ERU::Allowed},       vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {{"angle", "0 0 0"}}},
    {{{"angle",  "60 30 90"}}, {ERT::Mangle,                  "angle", ERU::BlockRotation}, vm::rotation_matrix(vm::to_radians(-90.0), vm::to_radians(-60.0), vm::to_radians(-30.0)), {}},
    }));
    // clang-format on

    CAPTURE(properties, info, transform);

    CHECK(applyEntityRotation(properties, info, transform) == expectedProperty);
  }

  SECTION("applyEntityRotation with an entity")
  {
    // the Quake policy updates the one property its rotation info names and removes none
    const auto entity = Entity{{
      {"classname", "other"},
      {"angle", "45"},
      {"rotation", "0 1 0 -1 0 0 0 0 1"},
    }};

    const auto expectedUpdate = EntityRotationUpdate{EntityProperty{"angle", "135"}, {}};
    CHECK(applyEntityRotation(entity, vm::mat4x4d::rot_90_z_ccw()) == expectedUpdate);
    CHECK(
      applyEntityRotation(
        entity, vm::mat4x4d::rot_90_z_ccw(), EntityRotationPolicy::Quake)
      == expectedUpdate);
  }

  SECTION("applyEntityRotation with an entity and the idtech4 policy")
  {
    using ERU = EntityRotationUpdate;

    auto anglePropertyDef =
      PropertyDefinition{"angle", PropertyValueTypes::String{}, "", ""};
    auto targetPropertyDef =
      PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""};

    const auto yaw90 = std::string{"0 1 0 -1 0 0 0 0 1"};
    const auto turnZ45cw = vm::rotation_matrix(0.0, 0.0, vm::to_radians(-45.0));
    const auto turnZ90ccw = vm::mat4x4d::rot_90_z_ccw();
    const auto pitchDown30 = vm::rotation_matrix(0.0, vm::to_radians(30.0), 0.0);
    const auto pitchUp90 = vm::rotation_matrix(0.0, vm::to_radians(-90.0), 0.0);

    using T = std::tuple<
      std::vector<EntityProperty>,
      bool,
      std::optional<EntityDefinitionInfo>,
      vm::mat4x4d,
      EntityRotationUpdate>;

    // clang-format off
    const auto
    [entityProperties,                         point, entityDefinitionInfo,     transformation, expectedUpdate] = GENERATE_COPY(values<T>({
    // a point entity's rotation is written to angles, and the properties the engine would read instead are removed
    {{{"classname", "other"},
      {"rotation", yaw90},
      {"angle", "45"}},                        true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"rotation", "angle"}}},
    {{{"classname", "other"},
      {"angles", "0 90 0"},
      {"angle", "45"}},                        true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"angle"}}},
    {{{"classname", "other"},
      {"angle", "90"}},                        true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"angle"}}},
    {{{"classname", "other"}},                 true,  std::nullopt,             turnZ90ccw,     ERU{EntityProperty{"angles", "0 90 0"}, {}}},
    {{{"classname", "other"}},                 true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 -45 0"}, {}}},
    {{{"classname", "other"}},                 true,  std::nullopt,             pitchDown30,    ERU{EntityProperty{"angles", "30 0 0"}, {}}},

    // a light also loses light_rotation and light_angles, even if it has a target
    {{{"classname", "light"},
      {"light_rotation", yaw90},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
      {"angle", "0"}},                         true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"light_rotation", "rotation", "light_angles", "angle"}}},
    {{{"classname", "light"},
      {"light_angles", "0 90 0"}},             true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"light_angles"}}},
    {{{"classname", "light"},
      {"angle", "90"},
      {"target", "xyz"}},                      true,  {{{targetPropertyDef}}},  turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"angle"}}},

    // other entities keep light_rotation and light_angles, which the engine does not read
    {{{"classname", "other"},
      {"light_rotation", yaw90},
      {"light_angles", "0 0 0"}},              true,  std::nullopt,             turnZ90ccw,     ERU{EntityProperty{"angles", "0 90 0"}, {}}},

    // a class whose definition declares angle but not angles keeps its yaw in angle
    {{{"classname", "other"}},                 true,  {{{anglePropertyDef}}},   turnZ90ccw,     ERU{EntityProperty{"angle", "90"}, {}}},
    {{{"classname", "other"},
      {"angle", "90"}},                        true,  {{{anglePropertyDef}}},   turnZ45cw,      ERU{EntityProperty{"angle", "45"}, {}}},
    // unless the entity has angles or rotation
    {{{"classname", "other"},
      {"angles", "0 90 0"},
      {"angle", "90"}},                        true,  {{{anglePropertyDef}}},   turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"angle"}}},
    {{{"classname", "other"},
      {"rotation", yaw90}},                    true,  {{{anglePropertyDef}}},   turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"rotation"}}},
    // a direction straight up has no yaw, so the old one is kept
    {{{"classname", "other"},
      {"angle", "0"}},                         true,  {{{anglePropertyDef}}},   pitchUp90,      ERU{}},
    // the yaw is written like the engine writes angles
    {{{"classname", "other"},
      {"angle", "30.6"}},                      true,  {{{anglePropertyDef}}},   vm::mat4x4d::identity(), ERU{EntityProperty{"angle", "30.6"}, {}}},
    {{{"classname", "other"},
      {"angle", "10.25"}},                     true,  {{{anglePropertyDef}}},   turnZ90ccw,     ERU{EntityProperty{"angle", "100.25"}, {}}},

    // angles are written like the engine's idDict::SetAngles writes them: two decimals, without trailing zeros, and
    // -0 as 0: the angles of the rotation 30 45 60, yaw 30.6, yaw -30.6 and pitch 30.6
    {{{"classname", "other"},
      {"rotation", "0.612372 0.612372 -0.5 -0.047367 0.659740 0.75 0.789149 -0.435596 0.433013"}},
                                               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "30 45 60"}, {"rotation"}}},
    {{{"classname", "other"},
      {"rotation", "0.860742 0.509041 0 -0.509041 0.860742 0 0 0 1"}},
                                               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "0 30.6 0"}, {"rotation"}}},
    {{{"classname", "other"},
      {"rotation", "0.860742 -0.509041 0 0.509041 0.860742 0 0 0 1"}},
                                               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "0 -30.6 0"}, {"rotation"}}},
    {{{"classname", "other"},
      {"rotation", "0.860742 0 -0.509041 0 1 0 0.509041 0 0.860742"}},
                                               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "30.6 0 0"}, {"rotation"}}},
    {{{"classname", "other"},
      {"angles", "-0 33.33 0"}},               true,  std::nullopt,             turnZ90ccw,     ERU{EntityProperty{"angles", "0 123.33 0"}, {}}},
    {{{"classname", "other"},
      {"angles", "0 0.05 0"}},                 true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "0 0.05 0"}, {}}},
    {{{"classname", "other"},
      {"angles", "0 -0.004 0"}},               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "0 0 0"}, {}}},
    {{{"classname", "other"},
      {"angles", "0 89.999 0"}},               true,  std::nullopt,             vm::mat4x4d::identity(), ERU{EntityProperty{"angles", "0 90 0"}, {}}},
    // a degenerate rotation matrix has no angles, so the entity is left as it is
    {{{"classname", "other"},
      {"rotation", "0 0 0 0 0 0 0 0 0"}},      true,  std::nullopt,             turnZ90ccw,     ERU{}},
    {{{"classname", "other"},
      {"rotation", "0 1 0"}},                  true,  std::nullopt,             turnZ90ccw,     ERU{}},
    // partial values are read like the engine reads them
    {{{"classname", "other"},
      {"rotation", "0 1 0 -1 0 0"}},           true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {"rotation"}}},
    {{{"classname", "other"},
      {"angles", "0 90"}},                     true,  std::nullopt,             turnZ45cw,      ERU{EntityProperty{"angles", "0 45 0"}, {}}},

    // an entity with brushes never gets angles or rotation, its angle is a mover's direction
    {{{"classname", "other"},
      {"angle", "90"}},                        false, std::nullopt,             turnZ90ccw,     ERU{EntityProperty{"angle", "180"}, {}}},
    {{{"classname", "other"},
      {"angle", "0"}},                         false, std::nullopt,             pitchUp90,      ERU{EntityProperty{"angle", "-1"}, {}}},
    {{{"classname", "other"},
      {"rotation", yaw90},
      {"angles", "0 90 0"}},                   false, std::nullopt,             turnZ90ccw,     ERU{}},
    {{{"classname", "other"}},                 false, std::nullopt,             turnZ90ccw,     ERU{}},

    // an entity without a classname is not rotated
    {{},                                       true,  std::nullopt,             turnZ90ccw,     ERU{}},
    }));
    // clang-format on

    CAPTURE(entityProperties, point, entityDefinitionInfo, transformation);

    const auto entityDefinition = createEntityDefinition(entityDefinitionInfo);

    auto entity = Entity{entityProperties};
    if (entityDefinition)
    {
      entity.setDefinition(&*entityDefinition);
    }
    entity.setPointEntity(point);

    CHECK(
      applyEntityRotation(entity, transformation, EntityRotationPolicy::IdTech4)
      == expectedUpdate);
  }

  SECTION("Round trips under the idtech4 policy")
  {
    const auto rotationOf = [](std::vector<EntityProperty> properties) {
      return entityRotation(Entity{std::move(properties)}, EntityRotationPolicy::IdTech4);
    };

    SECTION("Negative angles survive a rotation matrix")
    {
      // write the rotation of the angles as a rotation matrix, then convert it back
      const auto rotation =
        rotationOf({{"classname", "other"}, {"angles", "-30 -45 -60"}});
      auto numbers = std::vector<std::string>{};
      for (size_t column = 0; column < 3; ++column)
      {
        for (size_t row = 0; row < 3; ++row)
        {
          numbers.push_back(kdl::str_to_string(rotation[column][row]));
        }
      }

      const auto entity =
        Entity{{{"classname", "other"}, {"rotation", kdl::str_join(numbers, " ")}}};
      CHECK(
        applyEntityRotation(
          entity, vm::mat4x4d::identity(), EntityRotationPolicy::IdTech4)
        == EntityRotationUpdate{EntityProperty{"angles", "-30 -45 -60"}, {"rotation"}});
    }

    SECTION("Rewriting angles at a pitch of 90 keeps the rotation")
    {
      // at a pitch of 90, yaw and roll turn about the same axis, so the angles may change
      // while the rotation does not
      auto entity = Entity{{{"classname", "other"}, {"angles", "90 30 0"}}};
      const auto rotation = entityRotation(entity, EntityRotationPolicy::IdTech4);

      applyEntityRotationUpdate(
        entity,
        applyEntityRotation(
          entity, vm::mat4x4d::identity(), EntityRotationPolicy::IdTech4));
      CHECK(
        entityRotation(entity, EntityRotationPolicy::IdTech4)
        == vm::approx{rotation, 0.001});
    }
  }

  SECTION("idTech4ConvertedRotationKeys")
  {
    const auto allKeys = std::vector<EntityProperty>{
      {"light_rotation", "1 0 0 0 1 0 0 0 1"},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
      {"light_angles", "0 0 0"},
      {"angles", "0 0 0"},
      {"angle", "0"},
    };

    auto light = Entity{allKeys};
    light.setClassname("light");
    CHECK(
      idTech4ConvertedRotationKeys(light)
      == std::vector<std::string>{"light_rotation", "rotation", "light_angles"});

    auto other = Entity{allKeys};
    other.setClassname("other");
    CHECK(idTech4ConvertedRotationKeys(other) == std::vector<std::string>{"rotation"});

    CHECK(
      idTech4ConvertedRotationKeys(Entity{{{"classname", "light"}, {"angles", "0 0 0"}}})
        .empty());
  }

  SECTION("applyEntityRotationUpdate")
  {
    auto entity = Entity{{
      {"classname", "some_class"},
      {"angle", "45"},
      {"rotation", "1 0 0 0 1 0 0 0 1"},
    }};

    SECTION("Sets a property and removes others")
    {
      applyEntityRotationUpdate(
        entity,
        EntityRotationUpdate{EntityProperty{"angles", "0 90 0"}, {"rotation", "angle"}});
      CHECK(
        entity.properties()
        == std::vector<EntityProperty>{
          {"classname", "some_class"},
          {"angles", "0 90 0"},
        });
    }

    SECTION("Updates an existing property in place")
    {
      applyEntityRotationUpdate(
        entity, EntityRotationUpdate{EntityProperty{"angle", "90"}, {}});
      CHECK(
        entity.properties()
        == std::vector<EntityProperty>{
          {"classname", "some_class"},
          {"angle", "90"},
          {"rotation", "1 0 0 0 1 0 0 0 1"},
        });
    }

    SECTION("Removing a missing property does nothing")
    {
      applyEntityRotationUpdate(entity, EntityRotationUpdate{std::nullopt, {"angles"}});
      CHECK(entity.properties().size() == 3u);
    }
  }
}

} // namespace tb::mdl
