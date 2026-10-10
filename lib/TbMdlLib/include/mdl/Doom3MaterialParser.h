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

#include <string_view>
#include <vector>

namespace tb
{
class ParserStatus;

namespace mdl
{
class Quake3Shader;

/**
 * Parses a Doom 3 / RBDOOM-3-BFG material decl file (.mtr) into one Quake3Shader record
 * with format Doom3 per material decl, in file order.
 *
 * The parser follows the engine, not the Quake 3 shader grammar:
 *
 * - The file is split into decls by brace matching, as idDeclFile::LoadAndParse does.
 *   Typed decls other than materials (table, skin, particle, ...) are skipped. A decl
 *   that lacks its closing brace swallows what follows, as in the engine, and a warning
 *   points at the swallowed material.
 * - Every material decl becomes a record, whatever its root (textures/, models/, lights/,
 *   guis/, ...). Its shaderPath is the canonical decl name (see IdTech4MaterialName.h).
 *   The first definition of a name in the file wins.
 * - Each material is parsed with the engine's keyword arities. Where the engine would
 *   default the material (an unknown keyword, an unexpected end, a bad expression term),
 *   the record is kept, flagged engineDefaulted, and parsing of that material stops; the
 *   other materials in the file are not affected.
 *
 * Problems are reported through the given status with their file location. The parser
 * never throws and never drops the rest of a file.
 */
class Doom3MaterialParser
{
private:
  std::string_view m_str;

public:
  explicit Doom3MaterialParser(std::string_view str);

  std::vector<Quake3Shader> parse(ParserStatus& status);
};

} // namespace mdl
} // namespace tb
