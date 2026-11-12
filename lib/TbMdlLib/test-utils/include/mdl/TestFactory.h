/*
 Copyright (C) 2025 Kristian Duske

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

#include <functional>
#include <string>

namespace tb::mdl
{
class Brush;
class BrushNode;
class Map;
class PatchNode;
struct LightProperties;

BrushNode* createBrushNode(
  const Map& map,
  const std::string& materialName = "material",
  const std::function<void(mdl::Brush&)>& brushFunc = [](mdl::Brush&) {});

PatchNode* createPatchNode(const std::string& materialName = "material");

/**
 * An emitter omni at the defaults of its keys (seededEmitterProperties), radius 288, its
 * bulb at its origin.
 */
LightProperties makeEmitterOmni();

/**
 * An emitter spot at the defaults of its keys (seededEmitterProperties) with the given
 * range, a new spot's cone of 60 degrees, its forward axis the frame's x.
 */
LightProperties makeEmitterSpot(double range = 288.0);

} // namespace tb::mdl
