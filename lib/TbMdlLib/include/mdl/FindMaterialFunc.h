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

#include <filesystem>
#include <functional>
#include <optional>

namespace tb
{
namespace gl
{
class Material;
}

namespace mdl
{

/**
 * Finds the material that a model names by its canonical id Tech 4 decl name. Returns
 * nothing if the name resolves to neither a material decl nor an image, so that the
 * model loader can fall back to a default material that keeps the name, or to the
 * model's own textures.
 */
using FindMaterialFunc =
  std::function<std::optional<gl::Material>(const std::filesystem::path&)>;

} // namespace mdl
} // namespace tb
