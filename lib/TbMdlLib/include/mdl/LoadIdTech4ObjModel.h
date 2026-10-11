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

#include "base/Result.h"
#include "mdl/EntityModel.h"

#include "kd/reflection_decl.h"

#include "vm/vec.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb
{
class Logger;

namespace gl
{
class Material;
}

namespace mdl
{

/**
 * Finds the material that a model names by its canonical id Tech 4 decl name. Returns
 * nothing if the name resolves to neither a material decl nor an image, so that the
 * caller can fall back to a default material that keeps the name.
 */
using FindMaterialFunc =
  std::function<std::optional<gl::Material>(const std::filesystem::path&)>;

/**
 * A corner of a triangle read from an id Tech 4 OBJ file.
 */
struct IdTech4ObjCorner
{
  // as in the file: Doom 3 units, Z up, no axis change and no scale
  vm::vec3f position;
  // (u, 1 - v): the file stores 1 - t, so this is the engine's (s, t)
  vm::vec2f uv;

  kdl_reflect_decl(IdTech4ObjCorner, position, uv);
};

struct IdTech4ObjTriangle
{
  // into IdTech4ObjData::materialNames; none for faces before any usemtl
  std::optional<size_t> materialIndex;
  // in the reverse of the file's order: the file is counter-clockwise, as OBJ requires,
  // and TrenchBroom's front faces are clockwise
  std::array<IdTech4ObjCorner, 3> corners;

  kdl_reflect_decl(IdTech4ObjTriangle, materialIndex, corners);
};

struct IdTech4ObjData
{
  // canonical decl names, in the order of their first usemtl
  std::vector<std::filesystem::path> materialNames;
  std::vector<IdTech4ObjTriangle> triangles;

  kdl_reflect_decl(IdTech4ObjData, materialNames, triangles);
};

/**
 * Parses an OBJ file the way RBDOOM-3-BFG's OBJ importer reads it, which is how the
 * engine's ExportOBJ writes the _tb/models proxies for TrenchBroom.
 *
 * Positions are kept as they are. A texture coordinate (u, v) becomes (u, 1 - v). The
 * rest of a usemtl line is a material decl name, which is canonicalized with
 * canonicalIdTech4MaterialName. A face may have three or more corners, written as p,
 * p/t, p//n or p/t/n with 1-based or negative (relative) indices; it is split into a
 * fan, and the corners of each triangle are reversed. Normals, smoothing groups,
 * objects, groups, lines, mtllib and comments are skipped.
 *
 * Returns an error that names the line for an unknown statement, a malformed number or
 * an index out of range, and an error if the file has no vertices or no faces.
 */
Result<IdTech4ObjData> parseIdTech4Obj(std::string_view str);

/**
 * Whether the given path has the extension .obj, ignoring case.
 */
bool canLoadIdTech4ObjModel(const std::filesystem::path& path);

/**
 * Loads an id Tech 4 OBJ file (see parseIdTech4Obj) as a model with one surface and one
 * frame.
 *
 * The skins of the surface are the materials that the usemtl names resolve to with
 * findMaterial, in the order of their first use, followed by a default skin for the faces
 * before the first usemtl. A name that doesn't resolve gets loadDefaultMaterial(name),
 * which keeps the name, and one warning lists all such names.
 */
Result<EntityModelData> loadIdTech4ObjModel(
  std::string name,
  std::string_view str,
  const FindMaterialFunc& findMaterial,
  const std::function<gl::Material(std::string)>& loadDefaultMaterial,
  Logger& logger);

} // namespace mdl
} // namespace tb
