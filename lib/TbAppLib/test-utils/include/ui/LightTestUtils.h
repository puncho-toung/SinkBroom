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

#include "mdl/Hit.h"

#include "vm/vec.h"

#include <string>
#include <utility>
#include <vector>

namespace tb
{
namespace mdl
{
class BrushNode;
class EntityNode;
class Map;
} // namespace mdl

namespace ui
{

/**
 * The default of the handle threshold (light editor plan, D53).
 */
inline constexpr auto DefaultThreshold = 0.10;

/**
 * The t at which the seeded math block (D29, linear, inner radius 0.1) is visible at
 * the default threshold: u = (t - 0.1) / 0.9 = 0.9.
 */
inline constexpr auto SeededVisibleT = 0.91;

/**
 * The t at which a light without math is visible at the given level, where
 * 1 - smoothstep(t) is the level: smoothstep(t) = 1 - level, whose inverse is
 * 1/2 - sin(asin(1 - 2 y) / 3) (D61). A point light's handle box is +-radius * t.
 */
double legacyVisibleT(double level = DefaultThreshold);

/**
 * Creates a light entity at the given origin with the given properties; the new light
 * is selected, and the properties are set on the selection.
 */
mdl::EntityNode* createLight(
  mdl::Map& map,
  const std::string& origin,
  const std::vector<std::pair<std::string, std::string>>& properties = {});

/**
 * The value of the given property, which the entity must have.
 */
std::string valueOf(const mdl::EntityNode& entityNode, const std::string& key);

/**
 * The value of the given vector property, which the entity must have.
 */
vm::vec3d vectorOf(const mdl::EntityNode& entityNode, const std::string& key);

/**
 * Checks the value of the given vector property against the expected one within the two
 * decimals that the light editor writes (D6).
 */
void checkVector(
  const mdl::EntityNode& entityNode, const std::string& key, const vm::vec3d& expected);

/**
 * A hit of the given brush at the given distance along +x.
 */
mdl::Hit brushHit(mdl::BrushNode* brushNode, double distance);

} // namespace ui
} // namespace tb
