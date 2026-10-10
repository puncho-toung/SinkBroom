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
#include <string>
#include <string_view>

namespace tb::mdl
{

/**
 * Builds a path from id Tech 4 script text without ever throwing.
 *
 * std::filesystem::path throws when the platform cannot convert the text from its narrow
 * encoding, for example byte 0x81 in code page 1252. id Tech 4 names are ASCII, so every
 * byte from 0x80 up is replaced with '_' first. Slashes and backslashes both become path
 * separators.
 */
std::filesystem::path idTech4Path(std::string_view text);

/**
 * Returns the canonical form of an id Tech 4 (Doom 3, RBDOOM-3-BFG) decl name, such as a
 * material or table name in a .mtr file or a usemtl name in an exported .obj file.
 *
 * This is what idDeclManagerLocal::MakeNameCanonical does: ASCII letters are lowercased,
 * every backslash becomes a forward slash, and the name is cut at its last '.', wherever
 * that dot is, so "Textures\Base_Wall\Foo.tga" and "textures/base_wall/foo" name the
 * same decl. Any prefix such as "textures/" or "models/" is kept. Bytes from 0x80 up
 * become '_', as in idTech4Path, so the result is always ASCII.
 */
std::string canonicalIdTech4DeclName(std::string_view name);

/**
 * Returns canonicalIdTech4DeclName(name) as a path built by idTech4Path, so it never
 * throws.
 */
std::filesystem::path canonicalIdTech4MaterialName(std::string_view name);

} // namespace tb::mdl
