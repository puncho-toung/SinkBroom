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

#include <vector>

namespace tb::mdl
{

/**
 * Reports brushes with two faces that share an edge and whose normals are within 2
 * degrees of each other. Such a crease appears when a non-planar side, for example one
 * whose vertices were rounded to integers, is built as two nearly coplanar faces. id Tech
 * 4's dmap computes the vertices on the crease from the face planes and moves them far
 * enough to leave cracks.
 *
 * A rounding error of half a unit over edges about 30 units long makes creases of about
 * 1 degree, while hand-built brushwork rarely puts adjacent faces within 2 degrees.
 */
class NearlyCoplanarFacesValidator : public Validator
{
public:
  NearlyCoplanarFacesValidator();

private:
  void doValidate(
    BrushNode& brushNode, std::vector<std::unique_ptr<Issue>>& issues) const override;
};

} // namespace tb::mdl
