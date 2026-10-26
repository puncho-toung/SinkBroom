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
 * - LP7.2 (step 1, aligned with the engine): at most MaxObjSourceCorners face corners in
 *   the file and MaxObjObjects objects with faces (OBJ_Parse makes one at each o or g
 *   after a face, and one at the end): the conversion's _alloca of one int per object
 *   stays small on the pool's 8 MB stack, and its per-corner tables stay within memory;
 *   both guards are the preview's own, far above any model the engine can draw. The
 *   engine's own limit is not a corner count: ConvertOBJToModelSurfaces stores each
 *   welded vertex's number in a 16-bit triIndex_t (Model.cpp:1676, sys_types.h:145) and
 *   R_MergeTriangles and R_DuplicateMirroredVertexes add to them, so a finished surface
 *   of more than MaxModelSurfaceVerts vertices has wrapped indexes; that is
 *   modelSurfaceProblem's test after the finish (lit/ModelLoading.h), where the
 *   counts are known (LP7.1a's 32767 corners per file refused retail models the engine
 *   loads, material-orb.obj among them).
 *
 * nullopt when the file passes, else the reason with its line. Pure; any thread.
 */
std::optional<std::string> objFileProblem(std::string_view bytes);

// the corner bound the ASE and LWO validators apply (LP7.1b; their conversions' 16-bit
// indexes, Model.cpp:2033, :2477)
inline constexpr std::size_t MaxObjCorners = 32767;
// LP7.2: objFileProblem's memory guards (the preview's own)
inline constexpr std::size_t MaxObjSourceCorners = std::size_t{1} << 22;
inline constexpr std::size_t MaxObjObjects = 65536;
// LP7.2: a finished surface's vertices that a 16-bit triIndex_t can number (0-65535)
inline constexpr std::size_t MaxModelSurfaceVerts = 65536;

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

/**
 * LP7.1b (C51 (3), D136, P7.2): the preview's own check of an LWO file before the copied
 * lwGetObject (Model_lwo.cpp:1775-2067) and ConvertLWOToModelSurfaces
 * (Model.cpp:2109-2529) see it. It walks the IFF chunks by the sizes the file declares,
 * as the reader walks them, and accepts a subset of what the copies read without a read
 * or write out of range, a NULL dereference, a loop that never ends or a leak (the
 * reader's own failure returns, each a NULL object freed by lwFreeObject and the engine's
 * default cube, are safe; the validator refuses most of them too):
 *
 * - a FORM of type LWO2 within the file, its chunks exactly filling it (:1854-2010);
 *   LWOB (LightWave 5, lwGetObject5) refused: "LightWave 5 objects (LWOB) are not read
 *   by the preview" (the director's decision of 2026-10-26);
 * - the chunks the reader parses of even size: PNTS points of 12 bytes, finite and at
 *   most 1e30 (:3031-3078); POLS polygons exactly filling it, at least one (:3153 writes
 *   pol[0] of an empty list), every vertex index within the layer's points (:3214-3253
 *   read past the chunk; :3295, :3330, :3353 and Model.cpp:2381 index unchecked);
 *   non-triangles pass (the conversion skips them, Model.cpp:2372-2376);
 * - VMAP and VMAD: a terminated name, records exactly filling the chunk (:5017-5074),
 *   at least one (:4957), the point and polygon indexes within the layer's at the end
 *   (:5110, :5133, :5174, :5207), a TXUV of two values or more, finite, an RGBA of four
 *   or more in 0..1 (Model.cpp:2252, :2412, :2430);
 * - PTAG: records exactly filling the chunk, each polygon within the layer's (:3647), a
 *   SURF tag below the tag count (lwResolvePolySurfaces' off-by-one at :3411); TAGS:
 *   strings terminated in the chunk (:3565, :1689), none empty (NULL: :3397, :3422),
 *   under 256 characters; polygons only with a TAGS chunk (else a NULL surface in
 *   lwGetVertNormals, :3468);
 * - LAYR with its fields and a terminated name; BBOX of six floats; ENVL and CLIP their
 *   sub-chunks exactly filling them (ENVL's NAME once); SURF a non-empty name under 256
 *   characters (strcmp at :3397, FindMaterial), its sub-chunks exactly filling it, COLR
 *   in 0..1 (Model.cpp:2392-2394); a texture BLOK's header and body sub-chunks exactly
 *   filling them, its ordinal non-empty (compare_textures' strcmp, :4538), its
 *   allocating sub-chunks once (VMAP, FUNC, PNAM, INAM, FKEY, IKEY, TMAP's OREF: a second
 *   leaks the first); a shader BLOK's header loop reaching ENAB without a zero-size
 *   sub-chunk (:4444-4458 never end on one), its ordinal non-empty (:4544), FUNC once;
 * - what the conversion reads: points in the first layer when there are surfaces
 *   (Model.cpp:2212-2216 return after adding them, FinishSurfaces' NULL geometry, :1241),
 *   at most 32,767 polygons and 32,767 triangle corners in the first layer (the 16-bit
 *   indexes, :2477), 256 SURF chunks and 256 tags (the _alloca of :2155), 256 layers.
 *
 * nullopt when the file passes, else the reason with its byte offset. Pure; any thread.
 */
std::optional<std::string> lwoFileProblem(std::string_view bytes);

} // namespace tb::lit
