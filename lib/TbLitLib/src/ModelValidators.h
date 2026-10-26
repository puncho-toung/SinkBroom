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

/**
 * LP7.1b (C51 (3), D136, P7.2): the preview's own check of an ASE file before the copied
 * ASE_Load (Model_ase.cpp:874-957) and ConvertASEToModelSurfaces (Model.cpp:1736-2107)
 * see it. It walks the file as the copied reader does, writing nothing (the tokenizer of
 * :84-123, whose 0x80-0xFF bytes delimit as the engine's signed char does; the braced
 * blocks of :125-184; every key function of :191-932 in the same order), and accepts a
 * subset of what the copies read without reaching a fatal site, a write or read out of
 * range, an uninitialised value or a leak:
 *
 * - no NUL byte (ASE_Parse reads to the first), fewer than 2^31 bytes, every line at most
 *   1,000 characters (ASE_GetToken's 1,024-character buffer, :55, has no bound);
 * - every block closed and no '}' before its block opens (:144, :180), no brace outside a
 *   block (the reader prints it and goes on);
 * - each *MESH_NUM* count declared once, before its list, in 0..2^20; each list given
 *   once with exactly its count of entries (:355-368, :501-510, :529-541 write before
 *   their checks; :380-413, :438-440 and :465-467 have none), only its own keyword in it
 *   (:372, :417, :446, :474, :514, :546, :777); one *MESH per *GEOMOBJECT (:813 would
 * leak the first's lists);
 * - *MESH_NUMTVFACES equal to *MESH_NUMFACES, and *MESH_NUMCVFACES only then (the
 *   engine's Errors at :681-684 and :693-696, the second comparing numTVFaces);
 * - *MESH_TFACELIST, *MESH_CFACELIST and *MESH_NORMALS after *MESH_FACE_LIST
 *   (:728-750), *MESH_CFACELIST after *MESH_CVERTLIST with its colour indexes in range
 *   (:465 reads cvertexes[a] unchecked);
 * - *MESH_NORMALS: each face's *MESH_FACENORMAL in order (:566-574), then a
 *   *MESH_VERTEXNORMAL for each of its three corners (:599-618; :604 reads faces[-1]
 *   before the first face normal; a corner left out would be uninitialised), every normal
 *   of non-zero length after the mesh's *NODE_TM rows (:583-587, :627-631 normalise);
 * - what the conversion reads: a declared count's list present, every face's vertex
 *   index in range (Model.cpp:1959-1962, then vRemap[v]), a *MESH_TFACELIST with indexes
 *   in range when the coordinates are used (:1966-1974), a *MESH_CFACELIST when the mesh
 *   has colours (:1985-1989: else uninitialised), every *MATERIAL_REF naming a material
 *   (:1803, :1815, :2058 index unchecked; :1859 lets a negative through), at most 32,767
 *   face corners in the objects' meshes (the 16-bit indexes, :2033), at most 1,024
 *   objects (the _alloca of :1779) and 1,024 materials;
 * - every number atof or atoi reads a plain finite decimal (at most 1e30 in size: the
 *   conversion's bounds and idVectorSubset's boxes stay finite), *BITMAP and
 *   *MATERIAL_NAME under 256 characters (OSPathToRelativePath and the idStr buffers).
 *
 * A file with no *GEOMOBJECT passes: the engine loads it as a model without surfaces,
 * and so does the preview (the director's decision of 2026-10-26). nullopt when the file
 * passes, else the reason with its line. Pure; any thread.
 */
std::optional<std::string> aseFileProblem(std::string_view bytes);

} // namespace tb::lit
