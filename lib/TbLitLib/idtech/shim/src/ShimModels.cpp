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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.cpp, renderer/Model.h,
// renderer/Model_local.h, renderer/Material.h, renderer/ModelManager.cpp and
// idlib/Str.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.1a (C51, D137): the model facade (lit/EngineModels.h) and MaterialDecls' model
// calls. renderer/Model.cpp's copied ranges are member functions of idRenderModelStatic;
// Model_local.h is not copied (it declares the renderer's whole idRenderModel interface),
// so this file declares a reduced idRenderModelStatic, without a base class or virtuals,
// with exactly the members the ranges read and write (Model_local.h:44-174), and then
// compiles the ranges (renderer/Model.cpp is HEADER_FILE_ONLY in the build).
//
// The ranges name four things that would reach the registry: declManager->FindMaterial
// (ConvertOBJToModelSurfaces), idMaterial's accessors (both ranges) and AddToSurfaceArea
// (FinishSurfaces), modelSurface_t's const idMaterial* shader, and tr.defaultMaterial
// (MakeDefaultModel). The class declares a stand-in of each at class scope: inside the
// copied member functions unqualified lookup finds a class member before a global, so
// declManager, idMaterial, modelSurface_t and tr there are the stand-ins and no copied
// line changes (C7). The stand-in material holds the ten accessor values the parse
// thread took from the registry's material (MaterialDecls::modelMaterial); its
// AddToSurfaceArea adds to the stand-in and is dropped (M-MODEL-DIFF-03). So the
// conversion and FinishSurfaces read no registry object, and may run on a pool thread
// (M-MODEL-POOL-01).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineModels.h"
#include "lit/MaterialDecls.h"
#include "shim/Registry.h"
// clang-format on

// the conversion's slop merge (copied whole), and the OBJ model (copied whole)
#include "idlib/containers/VectorSet.h"
#include "renderer/Model_obj.h"

// OBJ_Load, copied whole in renderer/Model_obj.cpp, which declares it in Model_obj.h
// already; OBJ_Free too

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

// the copied ranges
#include "renderer/Model.cpp"

namespace tb::lit
{
namespace
{

EngineDrawVert fromIdVert(const idDrawVert& vert)
{
  static_assert(sizeof(idDrawVert) == sizeof(EngineDrawVert));
  auto result = EngineDrawVert{};
  std::memcpy(&result, &vert, sizeof(result));
  return result;
}

EngineVec3 fromIdVec(const idVec3& v)
{
  return {v.x, v.y, v.z};
}

EngineModel modelOf(
  const idRenderModelStatic& model, const idRenderModelStatic::ModelDecls& decls)
{
  auto result = EngineModel{};
  result.bounds = {fromIdVec(model.bounds[0]), fromIdVec(model.bounds[1])};
  result.defaulted = model.defaulted;
  result.isStaticWorldModel = model.isStaticWorldModel;
  result.hasDrawingSurfaces = model.hasDrawingSurfaces;
  result.hasInteractingSurfaces = model.hasInteractingSurfaces;
  result.hasShadowCastingSurfaces = model.hasShadowCastingSurfaces;
  result.timeStamp = std::int64_t(model.timeStamp);
  result.materialMisses = decls.misses();
  result.surfaces.reserve(std::size_t(model.surfaces.Num()));
  for (auto i = 0; i < model.surfaces.Num(); ++i)
  {
    const auto& surf = model.surfaces[i];
    auto surface = EngineModelSurface{};
    surface.id = surf.id;
    if (surf.shader)
    {
      surface.material = surf.shader->GetName();
      surface.materialIndex = decls.indexOf(surf.shader);
    }
    if (const auto* tri = surf.geometry)
    {
      surface.verts.reserve(std::size_t(tri->numVerts));
      for (auto v = 0; v < tri->numVerts; ++v)
      {
        surface.verts.push_back(fromIdVert(tri->verts[v]));
      }
      surface.indexes.assign(tri->indexes, tri->indexes + tri->numIndexes);
      surface.numMirroredVerts = tri->numMirroredVerts;
      if (tri->numMirroredVerts > 0)
      {
        surface.mirroredVerts.assign(
          tri->mirroredVerts, tri->mirroredVerts + tri->numMirroredVerts);
      }
      if (tri->numDupVerts > 0)
      {
        surface.dupVerts.assign(tri->dupVerts, tri->dupVerts + tri->numDupVerts * 2);
      }
      surface.generateNormals = tri->generateNormals;
      surface.tangentsCalculated = tri->tangentsCalculated;
      surface.bounds = {fromIdVec(tri->bounds[0]), fromIdVec(tri->bounds[1])};
    }
    result.surfaces.push_back(std::move(surface));
  }
  return result;
}

EngineModelMaterial capture(const idMaterial& material)
{
  auto values = EngineModelMaterial{};
  values.index = material.Index();
  values.name = material.GetName();
  values.discrete = material.IsDiscrete();
  const auto* renderBump = material.GetRenderBump();
  values.renderBump = renderBump ? renderBump : "";
  values.createBackSides = material.ShouldCreateBackSides();
  values.mikktspace = material.UseMikkTSpace();
  values.unsmoothedTangents = material.UseUnsmoothedTangents();
  values.castsShadow = material.SurfaceCastsShadow();
  values.drawn = material.IsDrawn();
  values.receivesLighting = material.ReceivesLighting();
  values.deform = int(material.Deform());
  return values;
}

std::string lastErrorOf(const std::vector<DeclMessage>& messages)
{
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
  {
    if (it->kind == DeclMessageKind::Error || it->kind == DeclMessageKind::FatalError)
    {
      return it->text;
    }
  }
  return "the engine reported an error";
}

} // namespace

bool EngineModelMaterial::operator==(const EngineModelMaterial& other) const
{
  return index == other.index && name == other.name && discrete == other.discrete
         && renderBump == other.renderBump && createBackSides == other.createBackSides
         && mikktspace == other.mikktspace
         && unsmoothedTangents == other.unsmoothedTangents
         && castsShadow == other.castsShadow && drawn == other.drawn
         && receivesLighting == other.receivesLighting && deform == other.deform;
}

bool EngineModelMaterial::operator!=(const EngineModelMaterial& other) const
{
  return !(*this == other);
}

EngineObjModel::EngineObjModel(objModel_t* model)
  : m_model{model}
{
}

EngineObjModel::~EngineObjModel()
{
  // Model_obj.cpp:267-279
  OBJ_Free(m_model);
}

std::vector<std::string> EngineObjModel::materialNames() const
{
  auto names = std::vector<std::string>{};
  if (m_model)
  {
    for (auto i = 0; i < m_model->objects.Num(); ++i)
    {
      names.emplace_back(m_model->objects[i]->material.c_str());
    }
  }
  return names;
}

std::size_t EngineObjModel::objectCount() const
{
  return m_model ? std::size_t(m_model->objects.Num()) : 0;
}

std::size_t EngineObjModel::cornerCount() const
{
  auto corners = std::size_t{0};
  if (m_model)
  {
    for (auto i = 0; i < m_model->objects.Num(); ++i)
    {
      corners += std::size_t(m_model->objects[i]->indexes.Num());
    }
  }
  return corners;
}

std::int64_t EngineObjModel::timeStamp() const
{
  return m_model ? std::int64_t(m_model->timeStamp) : 0;
}

EngineModel engineFinishObjModel(
  const EngineObjModel& obj,
  const EngineModelMaterials& materials,
  const std::string_view name,
  const bool useMikktspace)
{
  const auto decls = idRenderModelStatic::ModelDecls{materials};
  auto model = idRenderModelStatic{};
  model.declManager = &decls;
  model.tr.defaultMaterial = decls.defaultMaterial();

  // Model.cpp:314-379 InitFromFile with LoadOBJ (:3211-3229) for a model OBJ_Load read
  const auto fileName = std::string{name};
  model.InitEmpty(fileName.c_str());
  model.reloadable = true;
  // LoadOBJ: *sourceTimeStamp = obj->timeStamp; ConvertOBJToModelSurfaces( obj ), whose
  // result it ignores
  static_cast<void>(model.ConvertOBJToModelSurfaces(obj.model()));
  model.purged = false;
  model.FinishSurfaces(useMikktspace);
  return modelOf(model, decls);
}

EngineModel engineDefaultModel(
  const std::string_view name, const EngineModelMaterial& defaultMaterial)
{
  auto materials = EngineModelMaterials{};
  materials.defaultMaterial = defaultMaterial;
  const auto decls = idRenderModelStatic::ModelDecls{materials};
  auto model = idRenderModelStatic{};
  model.declManager = &decls;
  model.tr.defaultMaterial = decls.defaultMaterial();

  // ModelManager.cpp:520-523: InitEmpty( canonical ), then MakeDefaultModel
  const auto fileName = std::string{name};
  model.InitEmpty(fileName.c_str());
  model.MakeDefaultModel();
  return modelOf(model, decls);
}

EngineModelCvars engineModelCvars()
{
  auto cvars = EngineModelCvars{};
  cvars.mergeModelSurfaces = idRenderModelStatic::r_mergeModelSurfaces.GetBool();
  cvars.slopVertex = idRenderModelStatic::r_slopVertex.GetFloat();
  cvars.slopTexCoord = idRenderModelStatic::r_slopTexCoord.GetFloat();
  cvars.slopNormal = idRenderModelStatic::r_slopNormal.GetFloat();
  return cvars;
}

bool engineUseMikktspace(const std::int64_t sourceTimeStamp)
{
  // Model.cpp:366-375: "this is 1.1.2016 10:00 AM"
  const auto min2016 = ID_TIME_T(1451638800);
  return ID_TIME_T(sourceTimeStamp) > min2016;
}

std::string engineFileExtension(const std::string_view name)
{
  const auto text = idStr{std::string{name}.c_str()};
  auto extension = idStr{};
  text.ExtractFileExtension(extension);
  return extension.c_str();
}

std::string engineGeneratedModelPath(const std::string_view canonical)
{
  // ModelManager.cpp:446-450
  const auto canonicalText = std::string{canonical};
  auto extension = idStr{};
  idStr{canonicalText.c_str()}.ExtractFileExtension(extension);
  auto generatedFileName = idStr{"generated/rendermodels/"};
  generatedFileName.AppendPath(canonicalText.c_str());
  generatedFileName.SetFileExtension(va("b%s", extension.c_str()));
  return generatedFileName.c_str();
}

EngineObjParse MaterialDecls::parseObjModel(
  const std::string& name, const std::string& bytes, const std::int64_t time)
{
  if (!m_registry)
  {
    return {nullptr, m_constructionError};
  }
  const auto errorsBefore = m_registry->errorCount();
  auto* obj = static_cast<objModel_t*>(nullptr);
  {
    // Model_obj.cpp:243-250: OBJ_Load's ReadFile gets the bytes and the time handed in
    const auto read = shim::ModelRead{m_registry->files(), name, bytes, time};
    obj = OBJ_Load(name.c_str());
  }
  auto model = std::shared_ptr<const EngineObjModel>{};
  if (obj)
  {
    model = std::make_shared<const EngineObjModel>(obj);
  }
  if (m_registry->errorCount() != errorsBefore)
  {
    return {nullptr, lastErrorOf(m_registry->messages())};
  }
  return {std::move(model), {}};
}

DeclResult<EngineModelMaterial> MaterialDecls::modelMaterial(const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // M-ID-GUARD-01, as lookup
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "material name too long"};
  }

  // ConvertOBJToModelSurfaces' declManager->FindMaterial( mesh->material ), makeDefault
  // true (Model.cpp:1458)
  const auto nameText = std::string{name};
  const auto errorsBefore = m_registry->errorCount();
  const auto* material = declManager->FindMaterial(nameText.c_str());
  const auto failed = m_registry->errorCount() != errorsBefore;
  if (!material)
  {
    return {
      std::nullopt,
      failed ? lastErrorOf(m_registry->messages()) : "no material '" + nameText + "'"};
  }
  const auto index = size_t(material->Index());
  coverIndex(index);
  if (failed)
  {
    m_failures[index] = lastErrorOf(m_registry->messages());
  }
  if (!m_failures[index].empty())
  {
    return {std::nullopt, m_failures[index]};
  }
  return {capture(*material), {}};
}

DeclResult<EngineModelMaterial> MaterialDecls::defaultModelMaterial()
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // tr.defaultMaterial: the decl named _default, as MakeDefaultModel's DMAP branch finds
  // it (Model.cpp:274: FindMaterial( "_default", false )); a registry without that decl
  // (a test's) takes the implicit material of the name
  const auto errorsBefore = m_registry->errorCount();
  const auto* material = declManager->FindMaterial("_default", false);
  if (!material)
  {
    material = declManager->FindMaterial("_default");
  }
  if (!material || m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastErrorOf(m_registry->messages())};
  }
  return {capture(*material), {}};
}

} // namespace tb::lit
