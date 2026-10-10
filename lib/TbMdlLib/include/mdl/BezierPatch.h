/*
 Copyright (C) 2021 Kristian Duske

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

#include "base/Result.h"
#include "mdl/AssetReference.h"

#include "kd/flat_set.h"
#include "kd/reflection_decl.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace gl
{
class Material;
}

namespace mdl
{

/**
 * The explicit subdivisions of a Doom 3 patchDef3, in the order of the patch header.
 * RBDOOM-3-BFG applies the horizontal subdivisions along its patch width, which is the
 * first header number and our point row count, and the vertical subdivisions along its
 * height, our point column count. TrenchBroom only keeps them so that they can be written
 * back; it does not use them for rendering.
 */
struct PatchSubdivisions
{
  size_t horizontal;
  size_t vertical;

  kdl_reflect_decl(PatchSubdivisions, horizontal, vertical);
};

class BezierPatch
{
public:
  using Point = vm::vec<double, 5>;

private:
  size_t m_pointRowCount;
  size_t m_pointColumnCount;
  std::vector<Point> m_controlPoints;
  vm::bbox3d m_bounds;

  std::string m_materialName;
  AssetReference<gl::Material> m_materialReference;

  std::optional<PatchSubdivisions> m_explicitSubdivisions;

  kdl_reflect_decl(
    BezierPatch,
    m_pointRowCount,
    m_pointColumnCount,
    m_bounds,
    m_controlPoints,
    m_materialName,
    m_explicitSubdivisions);

public:
  /**
   * Creates a patch from trusted data. The point row and column counts must be odd and at
   * least 3, and the number of control points must be their product.
   */
  BezierPatch(
    size_t pointRowCount,
    size_t pointColumnCount,
    std::vector<Point> controlPoints,
    std::string materialName,
    std::optional<PatchSubdivisions> explicitSubdivisions = std::nullopt);

  /**
   * Creates a patch from untrusted data, such as a map file. Returns an error instead of
   * violating the constructor's preconditions.
   */
  static Result<BezierPatch> create(
    size_t pointRowCount,
    size_t pointColumnCount,
    std::vector<Point> controlPoints,
    std::string materialName,
    std::optional<PatchSubdivisions> explicitSubdivisions = std::nullopt);

  ~BezierPatch();

  BezierPatch(const BezierPatch& other);
  BezierPatch(BezierPatch&& other) noexcept;

  BezierPatch& operator=(const BezierPatch& other);
  BezierPatch& operator=(BezierPatch&& other) noexcept;

public: // control points:
  size_t pointRowCount() const;
  size_t pointColumnCount() const;

  size_t quadRowCount() const;
  size_t quadColumnCount() const;

  size_t surfaceRowCount() const;
  size_t surfaceColumnCount() const;

  const std::vector<Point>& controlPoints() const;
  Point& controlPoint(size_t row, size_t col);
  const Point& controlPoint(size_t row, size_t col) const;
  void setControlPoint(size_t row, size_t col, Point controlPoint);

  const vm::bbox3d& bounds() const;

  const std::string& materialName() const;
  void setMaterialName(std::string materialName);

  const gl::Material* material() const;
  bool setMaterial(gl::Material* material);

  const std::optional<PatchSubdivisions>& explicitSubdivisions() const;

  void transform(const vm::mat4x4d& transformation);
  void transformControlPoints(
    const kdl::flat_set<vm::vec3d>& positions, const vm::mat4x4d& transformation);

  std::vector<Point> evaluate(size_t subdivisionsPerSurface) const;

  /**
   * Evaluates the surface at the given normalized parameters, which range over the
   * whole patch (as opposed to a single 3x3 surface).
   *
   * @param u the column parameter, must be in [0, 1]
   * @param v the row parameter, must be in [0, 1]
   * @return the point on the surface at (u, v)
   */
  Point evaluateAt(double u, double v) const;
};

} // namespace mdl
} // namespace tb
