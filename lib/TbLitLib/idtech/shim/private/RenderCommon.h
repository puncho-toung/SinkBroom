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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderProgs.h,
// renderer/tr_frontend_main.cpp, renderer/VertexCache.h and renderer/RenderCommon.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2023 Robert Beckebans
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// Stands in for the engine's renderer/RenderCommon.h, which the copied Material.cpp and
// Image_program.cpp include by this name, so that those files stay byte-identical (S23).
// It holds the image records and image manager, the render program indices, the static
// allocators and the view fields the copied code reads.

// included at the end of precompiled.h, as the engine includes it
#include "renderer/GLState.h"
#include "renderer/Image.h"

// renderer/Image.h's idImage, reduced to what a material keeps: the record ImageFromFile
// keys on. No pixels are ever loaded.
class idImage
{
public:
  explicit idImage(const char* name);

  const char* GetName() const { return imgName; }

  // whether loading fell back to the default image; nothing is loaded here. Only
  // idMaterial::GetEditorImage asks, and only when fs_copyfiles is set, which it never
  // is.
  bool IsDefaulted() const { return false; }

  idStr imgName;
  textureFilter_t filter = TF_DEFAULT;
  textureRepeat_t repeat = TR_REPEAT;
  textureUsage_t usage = TD_DEFAULT;
  cubeFiles_t cubeFiles = CF_2D;
  int cubeMapSize = 0;

  // created by CreateIntrinsicImages through ImageFromFunction
  bool intrinsic = false;
};

// renderer/Image.h's idImageManager, reduced to the image registry: ImageFromFile and
// AllocImage mirror ImageManager.cpp, and the intrinsic records mirror
// Image_intrinsic.cpp's CreateIntrinsicImages (see mirror.json).
class idImageManager
{
public:
  idImageManager();
  ~idImageManager();

  idImageManager(const idImageManager&) = delete;
  idImageManager& operator=(const idImageManager&) = delete;

  idImage* ImageFromFile(
    const char* name,
    textureFilter_t filter,
    textureRepeat_t repeat,
    textureUsage_t usage,
    cubeFiles_t cubeMap = CF_2D,
    int cubeMapSize = 0);

  // LP1c.2 (C21): ImageFromFile's naming and lookup without making a record: the record
  // it would return (the default image, a built-in of the name, a record of the name and
  // options), or null when it would make one; name and usage are what that record would
  // get. The cube-state conflict ImageFromFile reports is not reported here.
  const idImage* FindImageFromFile(
    const char* name,
    textureFilter_t filter,
    textureRepeat_t repeat,
    textureUsage_t& usage,
    cubeFiles_t cubeMap,
    idStr& foundName) const;

  idImage* ImageFromFunction(const char* name);
  idImage* AllocImage(const char* name);

  // creates the intrinsic records, as CreateIntrinsicImages
  void Init();

  // frees every record (R5.11: after all decls, before the GUI dummies)
  void Shutdown();

  int NumImages() const { return images.Num(); }
  const idImage* ImageByIndex(int index) const { return images[index]; }

  // the intrinsics the copied code names
  idImage* defaultImage = nullptr;
  idImage* whiteImage = nullptr;
  idImage* blackImage = nullptr;
  idImage* flatNormalMap = nullptr;
  idImage* currentRenderImage = nullptr;
  idImage* originalCurrentRenderImage = nullptr;

  idList<idImage*, TAG_IDLIB_LIST_IMAGE> images;
  idHashIndex imageHash;

  // never set: the registry is never inside a level load
  bool insideLevelLoad = false;
  bool preloadingMapImages = false;

private:
  void CreateIntrinsicImages();
};

extern idImageManager* globalImages;

// renderer/RenderProgs.h, as far as the copied stage parser reaches it

// RenderProgs.h:320-327
enum rpStage_t
{
  SHADER_STAGE_VERTEX = BIT(0),
  SHADER_STAGE_FRAGMENT = BIT(1),
  SHADER_STAGE_COMPUTE = BIT(2), // RB: for future use

  SHADER_STAGE_DEFAULT = SHADER_STAGE_VERTEX | SHADER_STAGE_FRAGMENT
};

// only the layouts the copied code names; their values are indices into the shim's
// attribute table, not the engine's
enum vertexLayoutType_t
{
  LAYOUT_UNKNOWN = 0,
  LAYOUT_DRAW_VERT,
};

enum bindingLayoutType_t
{
  BINDING_LAYOUT_POST_PROCESS_INGAME,
  NUM_BINDING_LAYOUTS
};

// RenderProgs.h:368-383
struct shaderMacro_t
{
  idStr name;
  idStr definition;

  shaderMacro_t()
    : name()
    , definition()
  {
  }

  shaderMacro_t(const idStr& _name, const idStr& _definition)
    : name(_name)
    , definition(_definition)
  {
  }
};

// The shader and program lists of renderer/RenderProgs.cpp: FindShader and FindProgram
// search and append as the engine does, starting from the builtin table, so a user shader
// gets the non-zero index the engine gives it. Nothing is compiled or loaded.
class idRenderProgManager
{
public:
  idRenderProgManager();

  int FindShader(
    const char* name,
    rpStage_t stage,
    const char* nameOutSuffix,
    const idList<shaderMacro_t>& macros,
    bool builtin,
    vertexLayoutType_t vertexLayout = LAYOUT_DRAW_VERT);
  int FindProgram(
    const char* name, int vIndex, int fIndex, bindingLayoutType_t bindingType);

  // the shader name FindShader stored at this index, or "" for an index out of range
  const char* ShaderName(int index) const;

  struct layoutTypeAttributes_t
  {
    bool pcEnabled = false;
  };

  layoutTypeAttributes_t layoutTypeAttributes[NUM_BINDING_LAYOUTS];

  struct shader_t
  {
    idStr name;
    idStr nameOutSuffix;
    rpStage_t stage = SHADER_STAGE_DEFAULT;
    bool builtin = false;
  };

  struct renderProg_t
  {
    idStr name;
    int vertexShaderIndex = -1;
    int fragmentShaderIndex = -1;
    int computeShaderIndex = -1;
    bindingLayoutType_t bindingLayoutType = BINDING_LAYOUT_POST_PROCESS_INGAME;
    bool builtin = false;
  };

  idList<shader_t, TAG_RENDER> shaders;
  idList<renderProg_t, TAG_RENDER> renderProgs;
};

extern idRenderProgManager* tbLitRenderProgManager;

// The engine's renderProgManager is an object; the copied code names it with a dot, so
// the name refers to the registry's instance (C17: globals bound to the one registry).
#define renderProgManager (*tbLitRenderProgManager)

// RenderCommon.h:1556-1558, on Mem_Alloc and Mem_Free (S25)
void* R_StaticAlloc(int bytes, const memTag_t tag = TAG_RENDER_STATIC);
void* R_ClearedStaticAlloc(int bytes);
void R_StaticFree(void* data);

// renderer/RenderWorld.h and RenderCommon.h: the only view field the copied code reads is
// renderView.shaderParms, in CheckForConstantRegisters, after a memset to zero
struct renderView_t
{
  float shaderParms[MAX_GLOBAL_SHADER_PARMS];
};

struct viewDef_t
{
  renderView_t renderView;
};

// ----- the geometry copies (LP1b.2, C24; LP7.1a) -----

// renderer/VertexCache.h's handle type, which srfTriangles_t's cache fields have; nothing
// here allocates a cache, so they stay 0
typedef uint64 vertCacheHandle_t;

// renderer/Model.h, copied in part: dominantTri_t and, LP7.1a, srfTriangles_t itself
// (Model.h:61-115): R_AllocStaticTriSurf clears it with Mem_ClearedAlloc, as in the
// engine, and the copied R_FreeStaticTriSurf frees its Mem_Alloc16 arrays
#include "renderer/Model.h"

// the copied ranges' prototypes, which the engine declares in RenderCommon.h
srfTriangles_t* R_AllocStaticTriSurf();
void R_AllocStaticTriSurfVerts(srfTriangles_t* tri, int numVerts);
void R_AllocStaticTriSurfIndexes(srfTriangles_t* tri, int numIndexes);
void R_AllocStaticTriSurfSilIndexes(srfTriangles_t* tri, int numIndexes);
void R_AllocStaticTriSurfDominantTris(srfTriangles_t* tri, int numVerts);
void R_AllocStaticTriSurfMirroredVerts(srfTriangles_t* tri, int numMirroredVerts);
void R_AllocStaticTriSurfDupVerts(srfTriangles_t* tri, int numDupVerts);
void R_AllocStaticTriSurfMocIndexes(srfTriangles_t* tri, int numIndexes);
void R_AllocStaticTriSurfMocVerts(srfTriangles_t* tri, int numVerts);
srfTriangles_t* R_CopyStaticTriSurf(const srfTriangles_t* tri);
void R_FreeStaticTriSurfVertexCaches(srfTriangles_t* tri);
void R_FreeStaticTriSurf(srfTriangles_t* tri);
void R_ResizeStaticTriSurfVerts(srfTriangles_t* tri, int numVerts);
void R_RangeCheckIndexes(const srfTriangles_t* tri);
void R_BoundTriSurf(srfTriangles_t* tri);
void R_CreateSilIndexes(srfTriangles_t* tri);
void R_CreateDupVerts(srfTriangles_t* tri);
void R_RemoveDegenerateTriangles(srfTriangles_t* tri);
void R_TestDegenerateTextureSpace(srfTriangles_t* tri);
void R_ReverseTriangles(srfTriangles_t* tri);
srfTriangles_t* R_MergeSurfaceList(const srfTriangles_t** surfaces, int numSurfaces);
srfTriangles_t* R_MergeTriangles(const srfTriangles_t* tri1, const srfTriangles_t* tri2);
void R_DeriveTangents(srfTriangles_t* tri);
void R_DeriveTangentsWithoutNormals(srfTriangles_t* tri, bool useMikktspace);
void R_BuildDominantTris(srfTriangles_t* tri);
void R_CleanupTriangles(
  srfTriangles_t* tri,
  bool createNormals,
  bool identifySilEdges,
  bool useUnsmoothedTangents,
  bool useMikktspace);
void R_CreateMaskedOcclusionCullingTris(srfTriangles_t* tri);

/**
 * RenderCommon.h's idRenderSystemLocal tr, reduced to the one counter the copied
 * R_DeriveTangents adds to (tr.pc.c_tangentIndexes, under !DMAP). It is atomic, so pool
 * threads may add to it at once (C24); nothing reads it. Constant-initialised.
 */
struct tbLitPerformanceCounters
{
  std::atomic<int> c_tangentIndexes{0};
};

class idRenderSystemLocal
{
public:
  tbLitPerformanceCounters pc;
};

extern idRenderSystemLocal tr;
