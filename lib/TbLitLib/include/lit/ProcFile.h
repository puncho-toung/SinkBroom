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
#include "lit/EngineGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

// a node of a .proc's area BSP (RenderWorld.h areaNode_t): the plane as the copied
// idLexer parsed it, and the children: > 0 a node, 0 solid, < 0 the area -1 - child
struct ProcNode
{
  std::array<float, 4> plane = {0, 0, 0, 0};
  std::array<int, 2> children = {0, 0};
};

// an inter-area portal: its winding, the area on its positive side and the other one
struct ProcPortal
{
  int positiveArea = 0;
  int negativeArea = 0;
  std::vector<std::array<float, 3>> points;
};

// one surface of a .proc model; the geometry only when the reader was asked for it
// (LP7.0, C54, D138: the product's bake read asks for it, MapBake; the goldens)
struct ProcSurface
{
  std::string material;
  int numVerts = 0;
  int numIndexes = 0;
  // 0 PROC_VERTEX_FORMAT_BASIC, 1 PROC_VERTEX_FORMAT_COLOR (mapProcFile004); -1 none: a
  // .bproc's finished surface (ProcModel::finished), whose binary keeps no format
  int vertexFormat = 0;
  // a .proc's: 8 or 12 floats per vertex (xyz st normal [rgba]), as the lexer parsed them
  std::vector<float> verts;
  // the triangles' vertex indexes: a .proc's as parsed, a .bproc's as readBinaryModel
  // read them
  std::vector<int> indexes;
  // LP7.0 (D138, P7.12): a .bproc's finished surface as readBinaryModel reads it
  // (lit/BprocModel.h): the stored draw vertices, tangents and packed colour included;
  // empty for a .proc's surface. tangentsCalculated is the stored flag
  std::vector<EngineDrawVert> finishedVerts;
  bool tangentsCalculated = false;
  // C31 (LP4.0): the bounds R_BoundTriSurf gives the surface (tr_trisurf.cpp:606-609,
  // SIMDProcessor->MinMax over every vertex: the shim's scalar MinMax, mirror.json
  // M-GEO-FN-01), recorded from the parsed positions whether or not the reader keeps the
  // geometry; cleared (+INFINITUM, -INFINITUM) for a surface without vertices. Read from
  // a .bproc: the stored bounds, which FinishSurfaces grew for a material with a deform
  std::array<EngineVec3, 2> bounds = {};
};

struct ProcModel
{
  std::string name;
  // _areaN: N; any other model: -1
  int area = -1;
  std::vector<ProcSurface> surfaces;
  // C31: the model's bounds as a .bproc stores them (LoadBinaryModel, Model.cpp:607-608:
  // FinishSurfaces' bounds, already grown for the surfaces whose material has a deform);
  // a .proc's model bounds need the materials (lit/AreaData.h), so boundsKnown is false
  // for every model the .proc reader makes
  std::array<EngineVec3, 2> bounds = {};
  bool boundsKnown = false;
  // LP7.0 (D138, P7.12): the model's surfaces are finished: a .bproc's (FinishSurfaces'
  // back-side copies, mirrored duplicates, tangents, the colour packed, no vertex format:
  // ProcSurface::finishedVerts); false for a .proc's, whose surfaces are ParseModel's
  // input (ProcSurface::verts). Nothing draws from either in LP7.0; C54 and C55 read a
  // .proc's only
  bool finished = false;
};

/**
 * A .proc file as the engine's InitFromMap reads it (D64, SC70): the id
 * (mapProcFile004 or 003), the models with their names and areas, the inter-area portals
 * and the area BSP; shadowModel sections are skipped as the engine skips them. Plain
 * data: it is published on the parse thread and shared, immutable, by every thread.
 */
struct ProcTree
{
  // the .proc's id: 4 (mapProcFile004) or 3; 0 for a tree read from a .bproc
  int version = 0;
  int numAreas = 0;
  std::vector<ProcModel> models;
  std::vector<ProcPortal> portals;
  std::vector<ProcNode> nodes;
};

/**
 * Reads a .proc's text with the copied idLexer (RenderWorld_load.cpp InitFromMap,
 * ParseModel, ParseInterAreaPortals, ParseNodes, with their bounds checks); geometry
 * keeps the models' vertices and indexes. Parse thread only (C20: the copied lexer); the
 * lexer's messages go to the calling thread's sink (D63).
 */
Result<ProcTree> readProcFile(std::string_view text, bool geometry = false);

/**
 * LP7.0 (C54): the bytes a tree's arrays hold: per surface its vertex floats (4 B each),
 * its finished draw vertices (32 B each) and its indexes (4 B each), the portals' points
 * (12 B each) and the nodes (24 B each); the strings and the containers' own sizes are
 * not counted. The service's stats and the frame log's stop row report it as proc_bytes.
 * Pure; any thread.
 */
std::size_t procTreeBytes(const ProcTree& tree);

// a model's area: N for "_areaN", -1 for any other name (InitFromMap's area models).
// Pure; any thread.
int procModelArea(std::string_view name);

/**
 * RenderWorld.cpp:1240-1275 PointInArea: from node 0, d = p·n + plane[3]; d > 0 goes to
 * child 0, otherwise (the plane included) to child 1; a child of 0 is solid (-1), a
 * negative one the area -1 - child; a tree without nodes gives -1, as the engine's
 * without areaNodes. Pure; any thread.
 */
int pointInArea(const ProcTree& tree, const std::array<float, 3>& point);

/**
 * The WriteFloat/idLexer mirror (D60 golden (a)): the float a .proc carries for value, as
 * dmap writes it (dmap/output.cpp WriteFloat through FS_WriteFloatString) and the copied
 * idLexer reads it back. Parse thread (C20: the copied lexer).
 */
float procFloat(float value);

/**
 * R3.9: procFloat's round trip without the copied lexer: engineProcFloatText, then
 * lit/NumberText.h's lexerFloat, the plain C++ mirror of idLexer's ParseFloat (equal to
 * procFloat, tst_NumberText.cpp). The chunker reads its st and positions back with it, as
 * ParseModel reads what dmap wrote (WriteFloat's snap to an integer within 0.001
 * included). Pure; any thread.
 */
float procReadBack(float value);

/**
 * RenderWorld_load.cpp:224-321, the ParseModel mirror of one surface: st recentred per
 * island of triangles sharing a vertex by Ftoi((min + max) / 2) per component, then
 * SetTexCoord (half floats) and SetNormal (bytes); the colour (LP7.0, C4 extended: the
 * file's) opaque white for the basic format and, for PROC_VERTEX_FORMAT_COLOR, the rgba
 * floats through PackColorRound (enginePackColorRound), color2 zero (idDrawVert::Clear).
 * Empty for a surface without its .proc geometry (read without it, or a .bproc's finished
 * surface). Pure; any thread.
 */
std::vector<EngineDrawVert> loadProcSurface(const ProcSurface& surface);

/**
 * The same recentring on surface vertices already through SetTexCoord (D59): the decoded
 * st, read back as ParseModel reads what dmap wrote (procReadBack, R3.9), of each island
 * moved by Ftoi((min + max) / 2), then through SetTexCoord again. Vertices no triangle
 * uses are left alone, as in ParseModel. Pure; any thread.
 */
void recentreTexCoords(
  std::vector<EngineDrawVert>& verts, const std::vector<std::uint16_t>& indexes);

} // namespace tb::lit
