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

#include "mdl/NearlyCoplanarFacesValidator.h"

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushGeometry.h"
#include "mdl/BrushNode.h"
#include "mdl/Issue.h"

#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>

namespace tb::mdl
{
namespace
{

const auto Type = freeIssueType();

constexpr auto MaxCreaseAngle = 2.0; // degrees

} // namespace

NearlyCoplanarFacesValidator::NearlyCoplanarFacesValidator()
  : Validator{Type, "Nearly coplanar faces"}
{
}

void NearlyCoplanarFacesValidator::doValidate(
  BrushNode& brushNode, std::vector<std::unique_ptr<Issue>>& issues) const
{
  // the faces of a convex brush point outward, so the angle between the normals of two
  // adjacent faces is the angle of the crease between them
  static const auto minDot = std::cos(vm::to_radians(MaxCreaseAngle));

  const auto& brush = brushNode.brush();
  if (std::ranges::any_of(brush.edges(), [&](const auto* edge) {
        const auto firstFaceIndex = edge->firstFace()->payload();
        const auto secondFaceIndex = edge->secondFace()->payload();
        return firstFaceIndex && secondFaceIndex
               && vm::dot(
                    brush.face(*firstFaceIndex).normal(),
                    brush.face(*secondFaceIndex).normal())
                    > minDot;
      }))
  {
    issues.push_back(std::make_unique<Issue>(
      Type,
      brushNode,
      "Brush has adjacent faces within 2 degrees of each other; id Tech 4's dmap moves "
      "the vertices on such a crease and leaves cracks, so rebuild it with planar sides "
      "or split it"));
  }
}

} // namespace tb::mdl
