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
#include <string_view>

namespace tb::mdl
{

/**
 * Returns the canonical form of an id Tech 4 (Doom 3, RBDOOM-3-BFG) decl name, such as a
 * material name in a .mtr file or a usemtl name in an exported .obj file.
 *
 * This is what idDeclManagerLocal::MakeNameCanonical does: ASCII letters are lowercased,
 * every backslash becomes a forward slash, and the name is cut at its last '.', wherever
 * that dot is, so "Textures\Base_Wall\Foo.tga" and "textures/base_wall/foo" name the
 * same decl. Any prefix such as "textures/" or "models/" is kept.
 */
std::filesystem::path canonicalIdTech4MaterialName(std::string_view name);

} // namespace tb::mdl
