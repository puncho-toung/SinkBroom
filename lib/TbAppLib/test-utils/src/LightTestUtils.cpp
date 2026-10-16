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

#include "ui/LightTestUtils.h"

#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"

#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

double legacyVisibleT(const double level)
{
  return 0.5 - std::sin(std::asin(1.0 - 2.0 * (1.0 - level)) / 3.0);
}

mdl::EntityNode* createLight(
  mdl::Map& map,
  const std::string& origin,
  const std::vector<std::pair<std::string, std::string>>& properties)
{
  const auto* definition = map.entityDefinitionManager().definition("light");
  REQUIRE(definition != nullptr);

  const auto position = vm::parse<double, 3>(origin);
  REQUIRE(position);

  // a light created at 0 0 0 has no origin property until it is set
  auto* entityNode = mdl::createPointEntity(map, *definition, *position);
  REQUIRE(entityNode != nullptr);
  REQUIRE(mdl::setEntityProperty(map, "origin", origin));
  for (const auto& [key, value] : properties)
  {
    REQUIRE(mdl::setEntityProperty(map, key, value));
  }
  return entityNode;
}

std::string valueOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  REQUIRE(value != nullptr);
  return *value;
}

vm::vec3d vectorOf(const mdl::EntityNode& entityNode, const std::string& key)
{
  const auto value = vm::parse<double, 3>(valueOf(entityNode, key));
  REQUIRE(value);
  return *value;
}

void checkVector(
  const mdl::EntityNode& entityNode, const std::string& key, const vm::vec3d& expected)
{
  const auto actual = vectorOf(entityNode, key);
  CAPTURE(key, actual, expected);
  CHECK(vm::is_equal(actual, expected, 0.005));
}

mdl::Hit brushHit(mdl::BrushNode* brushNode, const double distance)
{
  return mdl::Hit{
    mdl::BrushNode::BrushHitType, distance, vm::vec3d{distance, 0, 0}, brushNode};
}

} // namespace tb::ui
