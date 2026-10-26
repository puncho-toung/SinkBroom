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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{

/**
 * LP7.1a (C51 (3), D136, P7.2, R5.3's rule): the preview's own check of an OBJ file
 * before the copied OBJ_Parse (Model_obj.cpp:56-235) and ConvertOBJToModelSurfaces
 * (Model.cpp:1383-1733) see it, which continue past their FatalError and Error sites (the
 * shim's return): a file that fails is drawn as the default cube with the reason, and
 * the copies never read it. It accepts a subset of what the copies read without
 * reaching a fatal site, line by line:
 *
 * - no NUL byte, fewer than 2^31 bytes, at least one byte (OBJ_Load returns NULL for an
 *   empty file, the engine's "Couldn't load model");
 * - every line's first token is one of v, vt, vn, f, usemtl, o, g, mtllib, s, l (the
 *   tokens :88-211 take) or starts with a single #, or the line is blank: any other is
 *   :211's FatalError (a C or C++ comment, which the lexer would skip, is refused too);
 * - v and vn hold exactly three numbers, vt exactly two, each a number the copied lexer
 *   reads as one TT_NUMBER that ParseFloat takes (a '-' sign, decimal digits with no
 *   leading zero before a digit, which the lexer reads as octal, at most one '.', an
 *   exponent 'e' as Lexer.cpp:881-910 reads it): anything else reaches ParseFloat's
 *   Errors (Lexer.cpp:1696, :1702, :1714) or :211 for the stray token;
 * - f holds exactly three a/b/c corners of positive decimal integers, each at most the
 *   count of v, vt and vn lines before it (OBJ_Parse indexes the lists read so far):
 *   anything else is :186's FatalError, an index past the counts an idList read out of
 *   range;
 * - usemtl's text (ReadRestOfLine's, stripped) is under MAX_OSPATH (256) characters, so
 *   FindMaterial's canonicalisation stays in its buffer;
 * - at most 32767 face corners in the file: the conversion's 16-bit indexes
 *   (triIndex_t) hold every vertex of a merged surface and the mirrored duplicates
 *   R_DuplicateMirroredVertexes may add (at most as many again) only below that
 * (Model.cpp:1676, tr_trisurf.cpp:1742), and the conversion's _alloca of one int per
 * object stays small on the pool's stack.
 *
 * nullopt when the file passes, else the reason with its line. Pure; any thread.
 */
std::optional<std::string> objFileProblem(std::string_view bytes);

// the corner bound objFileProblem applies
inline constexpr std::size_t MaxObjCorners = 32767;

} // namespace tb::lit
