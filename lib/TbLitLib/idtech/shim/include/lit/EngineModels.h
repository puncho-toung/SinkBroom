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

// LP7.1a (C51, D135, D137): the engine's static model code copied into TbLitIdLib
// (renderer/Model.cpp's ranges, renderer/Model_obj.cpp, the cleanup and MikkTSpace of
// renderer/tr_trisurf.cpp) behind a std-only facade. It compiles as C++17 and C++20.
//
// Threading (D137, C20, C24): the parse thread reads a model's file with the copied
// OBJ_Load and resolves its materials with the registry's FindMaterial
// (MaterialDecls::parseObjModel, modelMaterial, defaultModelMaterial); what it hands on
// is plain data: the parsed OBJ (read-only once made) and each material's ten accessor
// values. The conversion and FinishSurfaces (engineFinishObjModel, engineDefaultModel)
// read nothing else: the copied member functions are compiled against a stand-in
// idRenderModelStatic whose class-scope idMaterial, declManager and tr serve those values
// (shim/src/ShimModels.cpp), so they touch no registry object and may run on a pool
// thread (mirror.json M-MODEL-POOL-01). FinishSurfaces writes one function-local static
// (Model.cpp:1323-1324), so at most one finish runs at a time: the caller serialises
// them.

#include "lit/EngineGeometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct objModel_t;
struct aseModel_s;
struct st_lwObject;
class idFile;
class gltfData;

namespace tb::lit
{

/**
 * The ten idMaterial accessors the copied model ranges call (ConvertOBJToModelSurfaces,
 * FinishSurfaces), their values taken on the parse thread from the registry's material
 * (Material.h); a stand-in material hands them to the copies on the pool.
 */
struct EngineModelMaterial
{
  // the registry's material index: two names of one decl are one material, as the
  // engine's FindMaterial returns one pointer (ConvertOBJToModelSurfaces' im1 == im2)
  int index = -1;
  // GetName()
  std::string name;
  // IsDiscrete()
  bool discrete = false;
  // GetRenderBump(), "" for none
  std::string renderBump;
  // ShouldCreateBackSides()
  bool createBackSides = false;
  // UseMikkTSpace()
  bool mikktspace = false;
  // UseUnsmoothedTangents()
  bool unsmoothedTangents = false;
  // SurfaceCastsShadow()
  bool castsShadow = false;
  // IsDrawn()
  bool drawn = false;
  // ReceivesLighting()
  bool receivesLighting = false;
  // Deform(), a deform_t (0 is DFRM_NONE)
  int deform = 0;
  // LP7.5 (the director's S3): GetContentFlags() and Coverage() (a materialCoverage_t),
  // which the copied MapPolygonMesh::SetContents reads (MapFile.cpp:2966-3007)
  int contentFlags = 0;
  int coverage = 0;

  bool operator==(const EngineModelMaterial& other) const;
  bool operator!=(const EngineModelMaterial& other) const;
};

/**
 * The materials a model's conversion asks for: each name its objects use (the usemtl
 * text, as declManager->FindMaterial gets it) with the material it resolved to, and the
 * stand-in of tr.defaultMaterial (MakeDefaultModel, Model.cpp:276).
 */
struct EngineModelMaterials
{
  std::vector<EngineModelMaterial> materials;
  // a name and its index in materials
  std::vector<std::pair<std::string, std::size_t>> names;
  EngineModelMaterial defaultMaterial;
};

// One surface of idRenderModelStatic after FinishSurfaces, its srfTriangles_t as plain
// data.
struct EngineModelSurface
{
  // modelSurface_t::id
  int id = 0;
  // the material's name (GetName), and its index in EngineModelMaterials::materials (-1
  // for tr.defaultMaterial)
  std::string material;
  int materialIndex = -1;
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
  // the duplicated mirrored vertices at the end of verts (R_DuplicateMirroredVertexes)
  int numMirroredVerts = 0;
  std::vector<int> mirroredVerts;
  // R_CreateDupVerts' pairs (the duplicate, then the first vertex of its position)
  std::vector<int> dupVerts;
  bool generateNormals = false;
  bool tangentsCalculated = false;
  std::array<EngineVec3, 2> bounds = {};
};

// idRenderModelStatic after InitFromFile or MakeDefaultModel, as plain data
struct EngineModel
{
  std::vector<EngineModelSurface> surfaces;
  std::array<EngineVec3, 2> bounds = {};
  bool defaulted = false;
  bool isStaticWorldModel = false;
  bool hasDrawingSurfaces = false;
  bool hasInteractingSurfaces = false;
  bool hasShadowCastingSurfaces = false;
  // the model's timeStamp (ConvertOBJToModelSurfaces: the OBJ's)
  std::int64_t timeStamp = 0;
  // the names the conversion asked the stand-in declManager for that it was not handed
  // (they took the default material); 0 when the parse thread resolved every usemtl
  int materialMisses = 0;
};

/**
 * A parsed OBJ: the copied OBJ_Load's objModel_t (Model_obj.cpp:243-260), owned and never
 * changed after the parse, so any thread may read it (engineFinishObjModel).
 */
class EngineObjModel
{
private:
  objModel_t* m_model = nullptr;

public:
  explicit EngineObjModel(objModel_t* model);
  ~EngineObjModel();

  EngineObjModel(const EngineObjModel&) = delete;
  EngineObjModel& operator=(const EngineObjModel&) = delete;

  const objModel_t* model() const { return m_model; }
  // the objects' usemtl texts, in order
  std::vector<std::string> materialNames() const;
  // its objects and their face corners (the indexes of all objects)
  std::size_t objectCount() const;
  std::size_t cornerCount() const;
  // obj->timeStamp: the time OBJ_Load's ReadFile reported
  std::int64_t timeStamp() const;
};

// what MaterialDecls::parseObjModel read: the model (null when OBJ_Load returned NULL: an
// empty or unreadable file, the engine's "Couldn't load model"), or the engine error it
// reached
struct EngineObjParse
{
  std::shared_ptr<const EngineObjModel> model;
  std::string error;
};

/**
 * InitFromFile for an OBJ (Model.cpp:314-379) from LoadOBJ on (:3211-3229): InitEmpty,
 * the model's timeStamp from the OBJ (LoadOBJ's *sourceTimeStamp = obj->timeStamp),
 * ConvertOBJToModelSurfaces (its return value ignored, as LoadOBJ ignores it), purged
 * false, then FinishSurfaces( useMikktspace ). name is the model's (InitEmpty's: names
 * of the form _area* are static world models). Pool-safe (D137): materials are the parse
 * thread's captures; one call at a time (the header's note).
 */
EngineModel engineFinishObjModel(
  const EngineObjModel& obj,
  const EngineModelMaterials& materials,
  std::string_view name,
  bool useMikktspace);

/**
 * LP7.1b (C51 (3), D137): a parsed ASE, the copied ASE_Load's aseModel_t
 * (Model_ase.cpp:939-957), owned and never changed after the parse, so any thread may
 * read it (engineFinishAseModel); ASE_Free (:964-1032) frees it.
 */
class EngineAseModel
{
private:
  aseModel_s* m_model = nullptr;

public:
  explicit EngineAseModel(aseModel_s* model);
  ~EngineAseModel();

  EngineAseModel(const EngineAseModel&) = delete;
  EngineAseModel& operator=(const EngineAseModel&) = delete;

  const aseModel_s* model() const { return m_model; }
  // the names ConvertASEToModelSurfaces asks the decl manager for, one per object in
  // order (Model.cpp:1803, :1815, :1860): its material's name as the parse kept it (the
  // *BITMAP through OSPathToRelativePath, else the *MATERIAL_NAME); "" for an object of a
  // model without materials (FindMaterial( NULL ), the engine's "_emptyName",
  // DeclManager.cpp:712-715)
  std::vector<std::string> materialNames() const;
  std::size_t objectCount() const;
  std::size_t materialCount() const;
  // the face corners of the objects' meshes (3 per *MESH_FACE)
  std::size_t cornerCount() const;
  // ase->timeStamp: the time ASE_Load's ReadFile reported
  std::int64_t timeStamp() const;
  // an object's mesh with vertex colours (colorsParsed, Model_ase.cpp:522): its corners
  // take the *MESH_CFACE colours x 255 in bytes 0-2, the alpha left as the faces'
  // allocation has it (E-LIT-4: uninitialised in the engine, 0 in the preview)
  bool hasVertexColours() const;
};

// what MaterialDecls::parseAseModel read: the model (null when ASE_Load returned NULL),
// or the engine error it reached
struct EngineAseParse
{
  std::shared_ptr<const EngineAseModel> model;
  std::string error;
};

/**
 * InitFromFile for an ASE (Model.cpp:314-379) from LoadASE on (:3134-3152): InitEmpty,
 * ConvertASEToModelSurfaces (its result ignored, as LoadASE ignores it; it takes the
 * model's timeStamp from the ASE), purged false, then FinishSurfaces( useMikktspace ).
 * Pool-safe (D137): materials are the parse thread's captures; one call at a time.
 */
EngineModel engineFinishAseModel(
  const EngineAseModel& ase,
  const EngineModelMaterials& materials,
  std::string_view name,
  bool useMikktspace);

// the conversion alone, without FinishSurfaces: the surfaces as
// ConvertASEToModelSurfaces leaves them, for the goldens' mutants. Pool-safe
EngineModel engineConvertAseModelForTesting(
  const EngineAseModel& ase,
  const EngineModelMaterials& materials,
  std::string_view name);

/**
 * LP7.1b (C51 (3), D137): a parsed LWO2, the copied lwGetObject's lwObject
 * (Model_lwo.cpp:1775-2067), owned; lwFreeObject (:1740-1751) frees it. The conversion
 * writes each texture vmap's offset into it (Model.cpp:2248), which only the one
 * finish that holds it reads: one finish at a time.
 */
class EngineLwoModel
{
private:
  st_lwObject* m_model = nullptr;

public:
  explicit EngineLwoModel(st_lwObject* model);
  ~EngineLwoModel();

  EngineLwoModel(const EngineLwoModel&) = delete;
  EngineLwoModel& operator=(const EngineLwoModel&) = delete;

  const st_lwObject* model() const { return m_model; }
  // the names ConvertLWOToModelSurfaces asks the decl manager for: each surface's, in
  // the object's list order (Model.cpp:2164, :2174, :2326)
  std::vector<std::string> materialNames() const;
  // the object's surfaces, and the first layer's points, polygons and triangles (the
  // conversion reads that layer only; it skips a polygon of other than three
  // vertices, :2372-2376; the layer, :2209)
  std::size_t surfaceCount() const;
  std::size_t pointCount() const;
  std::size_t polygonCount() const;
  std::size_t triangleCount() const;
  // lwo->timeStamp: what the opened file's Timestamp() gave (:1846)
  std::int64_t timeStamp() const;
};

// what MaterialDecls::parseLwoModel read: the model, or (null) the engine error it
// reached or why lwGetObject returned NULL (the IFF chunk and offset it failed at)
struct EngineLwoParse
{
  std::shared_ptr<const EngineLwoModel> model;
  std::string error;
};

/**
 * InitFromFile for an LWO (Model.cpp:314-379) from LoadLWO on (:3159-3179): InitEmpty,
 * ConvertLWOToModelSurfaces (its result ignored, as LoadLWO ignores it; it takes the
 * model's timeStamp from the object), purged false, then FinishSurfaces( useMikktspace ).
 * Pool-safe (D137); one call at a time.
 */
EngineModel engineFinishLwoModel(
  const EngineLwoModel& lwo,
  const EngineModelMaterials& materials,
  std::string_view name,
  bool useMikktspace);

/**
 * LP7.5 (C51 (3), D137; the director's S4, S8): a parsed glTF, the copied GLTF_Parser's
 * gltfData (gltfParser.cpp:2100-2186 Load), owned. The parse leaves it out of the
 * parser's file cache (gltfData::Data's list and hash, emptied after each parse: no
 * cache in the preview, a difference) and hands it on; the finish's ProcessNode_r
 * resolves its nodes' matrices into it (ResolveNodeMatrix, a write), so only the one
 * finish that holds it reads it, one finish at a time. Its destructor frees every
 * element the parser made with new (idListNewElement: the buffers, views, accessors and
 * their views, meshes, primitives, attributes, nodes, scenes, materials, textures,
 * images, samplers, skins, animations, channels and samplers, cameras, the extensions'
 * objects), which the engine leaks on every load (~gltfData frees the chunks alone,
 * gltfParser.cpp:2320-2341: E-LIT-8, a difference in memory only), then the asset, on
 * any thread (the shim's lock around the parser's static cache).
 */
class EngineGltfModel
{
private:
  gltfData* m_data = nullptr;

public:
  explicit EngineGltfModel(gltfData* data);
  ~EngineGltfModel();

  EngineGltfModel(const EngineGltfModel&) = delete;
  EngineGltfModel& operator=(const EngineGltfModel&) = delete;

  gltfData* data() const { return m_data; }
};

// what MaterialDecls::parseGltfModel read: the asset, or (null) the engine error the
// parse reached (an Error or FatalError of the copied parser or its lexer: the engine's
// map load would have stopped there, a drop), or why Load returned false
struct EngineGltfParse
{
  std::shared_ptr<const EngineGltfModel> model;
  std::string error;
};

/**
 * LP7.5: gltfManager::ExtractIdentifier (gltfParser.cpp:2377-2427) transcribed with the
 * copied idLexer in place of idParser (one token of a short text: the same token): a
 * name of the form [file].[node name or mesh id].[glb or gltf] names its file and the
 * node or id; any other name its file alone. Pure; any thread.
 */
struct EngineGltfIdentifier
{
  // the file the name reads (ExtractIdentifier's filename after the call)
  std::string fileName;
  // the integer identifier (-1 for none) and the node name ("" for none)
  int id = -1;
  std::string name;
};

EngineGltfIdentifier engineGltfIdentifier(std::string_view name);

/**
 * LP7.5 (C51 (3), D137): idRenderModelGLTF::InitFromFile's decisions without import
 * options (Model_gltf.cpp:472-628, transcribed; a plain func_static's .glb has none,
 * D139), made on the parse thread over the parsed asset: FindModelRoot (copied, its
 * prints the parse thread's), the mesh nodes (MeshNodeIds), the animations that make the
 * model DM_CACHED, and the names ProcessNode_r and MapPolygonMesh::SetContents will ask
 * the decl manager for. Also the asset's second check: the validator's covers of the
 * conversion's sites, on what the copied parser made (an index in range, a window inside
 * the BIN chunk of binLength bytes), so that a parse that read the JSON otherwise than
 * the validator never reaches the conversion.
 */
struct EngineGltfPlan
{
  // the second check's refusal ("" when it passes)
  std::string problem;
  // the engine's own default model: "Can't find meshes in static glTF2 model" (:551-562)
  // or a NULL skin the engine would dereference; "" when it converts
  std::string defaultReason;
  // the nodes ProcessNode_r starts from (MeshNodeIds, :534-560)
  std::vector<int> meshNodes;
  // hasAnimations (:564-628): an animation of a skinned mesh node's subtree or of its
  // skin's first joint's, which makes the model DM_CACHED (:628): nothing drawn on a
  // static entity (K41); an unskinned mesh node's animations are never counted (:571)
  bool animated = false;
  int animations = 0;
  // a mesh node with a skin among the meshNodes (its JOINTS_0 in the vertex colours)
  bool skinned = false;
  // per surface ProcessNode_r will make, in order: the material name it asks for (the
  // glTF material's, or textures/base_wall/snpanel2rust without one, :85-96), and the
  // primitive's vertices
  std::vector<std::string> materialNames;
  std::vector<std::size_t> surfaceVertices;
  // a surface's primitive lacks NORMAL, TEXCOORD_0, COLOR_0 (or JOINTS_0) or WEIGHTS_0:
  // the engine leaves those fields of its vertices uninitialised heap (E-LIT-7,
  // MapFile_gltf.cpp:103-107)
  bool heapVertexFields = false;
};

EngineGltfPlan engineGltfPlan(
  const EngineGltfModel& model,
  const EngineGltfIdentifier& identifier,
  std::size_t binLength);

/**
 * LP7.5: InitFromFile's conversion and finish without import options (Model_gltf.cpp:
 * 629-747, transcribed): globalTransform = blenderToDoomTransform (Y up to Z up,
 * idAngles( 0, 0, 90 ), :55), useMikktspace as given (true for every glTF, :645), each
 * of plan.meshNodes through the copied ProcessNode_r from the identity (one surface per
 * primitive through the copied MapPolygonMesh::ConvertFromMeshGltf, the material
 * FindMaterial of the glTF material's name, else textures/base_wall/snpanel2rust), then
 * no surface: "Couldn't load model" and MakeDefaultModel (:713-722); then the copied
 * FinishSurfaces( useMikktspace ). LoadModel (:1727-1801), which changes no surface of a
 * static model, is not run. Its allocations cleared (E-LIT-7: the engine's vertices hold
 * heap where no attribute writes, MapFile_gltf.cpp:103-107; the preview's 0). Pool-safe
 * (D137): materials are the parse thread's captures (ProcessNode_r's through the reduced
 * idRenderModelStatic's class-scope declManager, SetContents' through the stand-in of
 * the namespace its MapFile ranges compile in); one call at a time.
 */
EngineModel engineFinishGltfModel(
  const EngineGltfModel& model,
  const EngineGltfPlan& plan,
  const EngineModelMaterials& materials,
  std::string_view name,
  bool useMikktspace);

/**
 * GetModel's default model (ModelManager.cpp:520-523, Model.cpp:356-361): InitEmpty, then
 * MakeDefaultModel (Model.cpp:226-296): 24 vertices, 36 indexes, a cube of +-8 with st
 * 0..1 per face, generateNormals, then FinishSurfaces( false ), with defaultMaterial
 * standing in for tr.defaultMaterial. Pool-safe; one call at a time.
 */
EngineModel engineDefaultModel(
  std::string_view name, const EngineModelMaterial& defaultMaterial);

/**
 * InitFromFile's 2016 rule (Model.cpp:366-375): "assume this is a newer asset not by id
 * Software and its normalmaps are baked using the mikktspace standard" for a source time
 * strictly after 1451638800 (2016-01-01 10:00 UTC). Pure.
 */
bool engineUseMikktspace(std::int64_t sourceTimeStamp);

/**
 * idStr::ExtractFileExtension of a name (Str.cpp:1424-1443): what follows the last '.',
 * or "" when there is none past the first character. Pure; any thread.
 */
std::string engineFileExtension(std::string_view name);

/**
 * GetModel's binary name (ModelManager.cpp:446-450): "generated/rendermodels/" with the
 * canonical name appended by idStr::AppendPath, its extension set to "b<ext>" by
 * SetFileExtension. Pure; any thread.
 */
std::string engineGeneratedModelPath(std::string_view canonical);

// the copied conversion's own cvars (Model.cpp:42-45), as it reads them
struct EngineModelCvars
{
  bool mergeModelSurfaces = false;
  float slopVertex = 0.0f;
  float slopTexCoord = 0.0f;
  float slopNormal = 0.0f;
};

// r_mergeModelSurfaces, r_slopVertex, r_slopTexCoord and r_slopNormal at their defaults
// (the shim's constexpr idCVar, D21); pure
EngineModelCvars engineModelCvars();

// the triangles of R_CleanupTriangles, its derived data kept
// (engineCleanupModelTriangles)
struct EngineModelTriangles
{
  EngineTriangles triangles;
  std::vector<int> dupVerts;
  bool tangentsCalculated = false;
  std::array<EngineVec3, 2> bounds = {};
};

/**
 * R_CleanupTriangles( tri, generateNormals, true, unsmoothedTangents, mikktspace )
 * (tr_trisurf.cpp:1810-1847) whole, as FinishSurfaces calls it for a surface: the range
 * check, sil indexes, degenerates, R_TestDegenerateTextureSpace,
 * R_DuplicateMirroredVertexes, R_CreateDupVerts, the bounds, then the tangents (the
 * unsmoothed path, R_DeriveTangentsWithoutNormals( tri, mikktspace ), or
 * R_DeriveNormalsAndTangents over the dupVerts). Null on indexes the copy could not take
 * (lit/EngineGeometry.h's checks). Pure; any thread.
 */
std::optional<EngineModelTriangles> engineCleanupModelTriangles(
  std::vector<EngineDrawVert> verts,
  std::vector<std::uint16_t> indexes,
  bool generateNormals,
  bool unsmoothedTangents,
  bool mikktspace);

/**
 * LP7.1b (P7.13): the shim's OSPathToRelativePath (FileSystem.cpp:1841-1917 transcribed,
 * the preview's base names: base, d3xp, d3le; fs_game and fs_game_base empty, C8), which
 * the copied ASE reader calls on a *BITMAP path. Its warning goes to the calling
 * thread's engine messages. Any thread.
 */
std::string engineOSPathToRelativePath(std::string_view osPath);

/**
 * LP7.1b (P7.13): the shim's memory-backed idFile, the one a model read's OpenFileRead
 * opens (Read, Tell, Seek as framework/File.cpp's idFile_Memory; Timestamp the time
 * handed in), for its tests. Seek's origin is fsOrigin_t's value: 0 FS_SEEK_CUR, 1
 * FS_SEEK_END, 2 FS_SEEK_SET.
 */
class EngineMemoryFileForTesting
{
public:
  EngineMemoryFileForTesting(std::string name, std::string bytes, std::int64_t time);
  ~EngineMemoryFileForTesting();

  EngineMemoryFileForTesting(const EngineMemoryFileForTesting&) = delete;
  EngineMemoryFileForTesting& operator=(const EngineMemoryFileForTesting&) = delete;

  // idFile::Read of length bytes; what it read is in bytes, its count returned
  int read(std::string& bytes, int length);
  int seek(long offset, int origin);
  int tell() const;
  int length() const;
  std::int64_t timestamp() const;
  std::string fullPath() const;

private:
  std::unique_ptr<idFile> m_file;
};

} // namespace tb::lit
