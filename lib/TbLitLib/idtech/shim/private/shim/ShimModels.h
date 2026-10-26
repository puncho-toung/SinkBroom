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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.h, renderer/Model_local.h and
// renderer/Material.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// LP7.1a (C51, D137): the reduced idRenderModelStatic that renderer/Model.cpp's copied
// ranges are member functions of (shim/src/ShimModels.cpp compiles them), with the
// stand-ins at its class scope that keep them off the registry (ShimModels.cpp's notes).
// LP7.5: in a header of its own, so that shim/src/ShimGltf.cpp derives the reduced
// idRenderModelGLTF of renderer/Model_gltf.cpp's ranges from it; the member functions
// stay compiled in ShimModels.cpp alone.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineModels.h"
// clang-format on

// the OBJ model, which ConvertOBJToModelSurfaces' declaration names (copied whole)
#include "renderer/Model_obj.h"

class idRenderModelStatic
{
public:
  /**
   * The stand-in of idMaterial the copied ranges see (Material.h): the ten accessors
   * they call, over the values the parse thread captured (tb::lit::EngineModelMaterial),
   * and AddToSurfaceArea, which FinishSurfaces calls through a const_cast
   * (Model.cpp:1301): it adds to this object, which the finish owns, and nothing reads
   * it (the engine's only reader is the listSurfaceAreas console command).
   */
  class idMaterial
  {
  public:
    explicit idMaterial(tb::lit::EngineModelMaterial values)
      : m_values{std::move(values)}
    {
    }

    // Material.h:148-151 (idDecl's GetName)
    const char* GetName() const { return m_values.name.c_str(); }
    // Material.h:671
    bool IsDiscrete() const { return m_values.discrete; }
    // Material.h:745-748
    const char* GetRenderBump() const { return m_values.renderBump.c_str(); }
    // Material.h:616-619
    bool ShouldCreateBackSides() const { return m_values.createBackSides; }
    // Material.h:632-635
    bool UseMikkTSpace() const { return m_values.mikktspace; }
    // Material.h:624-627
    bool UseUnsmoothedTangents() const { return m_values.unsmoothedTangents; }
    // Material.h:584
    bool SurfaceCastsShadow() const { return m_values.castsShadow; }
    // Material.h:548
    bool IsDrawn() const { return m_values.drawn; }
    // Material.h:599
    bool ReceivesLighting() const { return m_values.receivesLighting; }
    // Material.h:819
    deform_t Deform() const { return deform_t(m_values.deform); }
    // Material.h:882-885
    void AddToSurfaceArea(float area) { m_surfaceArea += area; }

    const tb::lit::EngineModelMaterial& values() const { return m_values; }

  private:
    tb::lit::EngineModelMaterial m_values;
    float m_surfaceArea = 0.0f;
  };

  // Model.h:118-123's modelSurface_t, its shader the stand-in material
  struct modelSurface_t
  {
    int id;
    const idMaterial* shader;
    srfTriangles_t* geometry;
  };

  /**
   * The stand-in of declManager the copied conversion calls (Model.cpp:1458, :1484,
   * :1502): FindMaterial of a name the parse thread resolved returns that name's
   * material, the same object for two names of one decl, as the registry returns one
   * pointer; a name it was not handed returns the default material (never null, so
   * FinishSurfaces' NULL shader site stays unreachable) and is counted.
   */
  class ModelDecls
  {
  public:
    explicit ModelDecls(const tb::lit::EngineModelMaterials& materials)
      : m_default{materials.defaultMaterial}
    {
      m_materials.reserve(materials.materials.size());
      for (const auto& material : materials.materials)
      {
        m_materials.emplace_back(material);
      }
      for (const auto& [name, index] : materials.names)
      {
        if (index < m_materials.size())
        {
          m_names.emplace_back(name, index);
        }
      }
    }

    const idMaterial* FindMaterial(const char* name, bool makeDefault = true) const
    {
      (void)makeDefault;
      const auto text = std::string_view{name ? name : ""};
      for (const auto& [known, index] : m_names)
      {
        if (known == text)
        {
          return &m_materials[index];
        }
      }
      ++m_misses;
      return &m_default;
    }

    const idMaterial* defaultMaterial() const { return &m_default; }

    // the index of a material FindMaterial returned, -1 for the default material
    int indexOf(const idMaterial* material) const
    {
      for (std::size_t i = 0; i < m_materials.size(); ++i)
      {
        if (&m_materials[i] == material)
        {
          return int(i);
        }
      }
      return -1;
    }

    int misses() const { return m_misses; }

  private:
    // never resized after construction: the surfaces point into it
    std::vector<idMaterial> m_materials;
    std::vector<std::pair<std::string, std::size_t>> m_names;
    idMaterial m_default;
    mutable int m_misses = 0;
  };

  // the stand-in of tr, as far as MakeDefaultModel reads it (Model.cpp:276)
  struct ModelRenderSystem
  {
    const idMaterial* defaultMaterial = nullptr;
  };

  // Model_local.h:52-53, 68, 77-79, 93-95, 127-130 (the members the ranges call, without
  // virtual)
  idRenderModelStatic();
  ~idRenderModelStatic();

  void PurgeModel();
  void InitEmpty(const char* name);
  void AddSurface(modelSurface_t surface);
  void FinishSurfaces(bool useMikktspace);
  int NumSurfaces() const;

  void MakeDefaultModel();
  bool ConvertOBJToModelSurfaces(const objModel_t* obj);
  // LP7.1b: Model_local.h:127-128
  bool ConvertASEToModelSurfaces(const struct aseModel_s* ase);
  bool ConvertLWOToModelSurfaces(const struct st_lwObject* lwo);

  idRenderModelStatic(const idRenderModelStatic&) = delete;
  idRenderModelStatic& operator=(const idRenderModelStatic&) = delete;

  // Model_local.h:137-174
  idList<modelSurface_t, TAG_MODEL> surfaces;
  idBounds bounds;
  int overlaysAdded;

  int numInvertedJoints;
  idJointMat* jointsInverted;
  vertCacheHandle_t jointsInvertedBuffer;

  int lastModifiedFrame;
  int lastArchivedFrame;

  idStr name;
  bool isStaticWorldModel;
  bool defaulted;
  bool purged;
  bool fastLoad;
  bool reloadable;
  bool levelLoadReferenced;
  bool hasDrawingSurfaces;
  bool hasInteractingSurfaces;
  bool hasShadowCastingSurfaces;
  ID_TIME_T timeStamp;
  ID_TIME_T declTimeStamp;

  static idCVar r_mergeModelSurfaces;
  static idCVar r_slopVertex;
  static idCVar r_slopTexCoord;
  static idCVar r_slopNormal;

  // the stand-ins the copied member functions find at class scope
  const ModelDecls* declManager = nullptr;
  ModelRenderSystem tr;
};

namespace tb::lit::shim
{

// the plain data of a model after the copied ranges made it (its surfaces' vertices,
// indexes and derived data, its bounds and flags), and the materials' names and indexes
// in decls (ShimModels.cpp)
EngineModel modelOf(
  const idRenderModelStatic& model, const idRenderModelStatic::ModelDecls& decls);

// LP7.1a: the ten accessor values the copied ranges call, of the registry's material
// (parse thread); LP7.5: and GetContentFlags and Coverage, which the copied
// MapPolygonMesh::SetContents reads (MapFile.cpp:2966-3007)
EngineModelMaterial capture(const idMaterial& material);

} // namespace tb::lit::shim
