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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/File.cpp, idlib/gltfParser.cpp,
// idlib/gltfProperties.h, idlib/MapFile.h, renderer/Material.h, renderer/Model_gltf.cpp,
// renderer/Model_local.h and renderer/ModelManager.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2022-2023 Harrie van Ginneken
//   Copyright (C) 2012-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.5 (C51 (3), D137; the director's decisions S1-S8 of 2026-10-26): the glTF facade
// (lit/EngineModels.h) and MaterialDecls::parseGltfModel. It compiles, in this order:
//
// - S1: idFile_Memory, a stand-in of framework/File.cpp's class under its name, read only
//   and over memory it does not own, which the copied parser and conversion construct by
//   value over an accessor's bytes (gltfParser.cpp:2205-2213, MapFile_gltf.cpp:51-52,
//   :98-99);
// - the copied idlib/gltfProperties.h and gltfParser.h whole and gltfParser.cpp's range
//   (HEADER_FILE_ONLY in the build), and gltfManager::ExtractIdentifier transcribed
//   (the copy's needs idParser, which is not copied: S2);
// - S3: idlib/MapFile.h's, MapFile.cpp's and MapFile_gltf.cpp's ranges inside the
//   namespace tbLitGltfMesh, which declares a stand-in idMaterial (GetContentFlags,
//   Coverage) and declManager before them: MapPolygonMesh::SetContents' unqualified
//   declManager->FindMaterial (MapFile.cpp:2981, :2993) finds the namespace's before the
//   global one, so it reads the parse thread's captures, never the registry, and no
//   copied line changes (C7, the LP7.1a pattern);
// - S7: idRenderModelGLTF reduced to what renderer/Model_gltf.cpp's ranges touch, derived
//   from shim/ShimModels.h's reduced idRenderModelStatic, whose class-scope stand-ins
//   ProcessNode_r's FindMaterial and modelSurface_t find; FindModelRoot's idImportOptions
//   reduced to its one member read; then the ranges;
// - the facade: the parse (S6's error-counting scope, S8's emptied cache), the plan, the
//   finish, and the asset's destructor (S4's walk).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineModels.h"
#include "lit/MaterialDecls.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
#include "shim/ShimModels.h"
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

/**
 * S1 (the director's decision): framework/File.cpp's idFile_Memory as the copied glTF
 * code uses it: the constructor over a name, data and a length (File.cpp:732-743: read
 * only, the data not owned), Read (:780-796: what is left when asked for more), Seek
 * (:1000-1036: from the current place, the end or the start, clamped, -1 when it had to
 * be), Length, Tell and Timestamp (:960-963: 0). Written, not copied: the class's range
 * carries a dynamically initialised global hash table, two console commands and a cvar
 * (M-MODEL-NC-01). A non-positive Read length reads nothing (the engine's memcpy of a
 * negative length is undefined). The thread that made it (the parse thread's parse, the
 * pool's conversion).
 */
class idFile_Memory : public idFile
{
public:
  idFile_Memory(const char* name, const char* data, const int length)
    : m_name{name}
    , m_data{data}
    , m_length{length}
  {
  }

  const char* GetFullPath() const override { return m_name.c_str(); }

  int Read(void* buffer, int len) override
  {
    if (len <= 0)
    {
      return 0;
    }
    if (m_at + len > m_length)
    {
      len = m_length - m_at;
    }
    memcpy(buffer, m_data + m_at, size_t(len));
    m_at += len;
    return len;
  }

  int Length() const override { return m_length; }
  ID_TIME_T Timestamp() const override { return 0; }
  int Tell() const override { return m_at; }

  int Seek(const long offset, const fsOrigin_t origin) override
  {
    auto at = static_cast<long long>(m_at);
    switch (origin)
    {
    case FS_SEEK_CUR:
      at += offset;
      break;
    case FS_SEEK_END:
      at = m_length - offset;
      break;
    case FS_SEEK_SET:
      at = offset;
      break;
    default:
      common->FatalError("idFile_Memory::Seek: bad origin for %s\n", m_name.c_str());
      return -1;
    }
    if (at < 0)
    {
      m_at = 0;
      return -1;
    }
    if (at > m_length)
    {
      m_at = m_length;
      return -1;
    }
    m_at = int(at);
    return 0;
  }

private:
  idStr m_name;
  const char* m_data;
  int m_length;
  int m_at = 0;
};

// the copied parser (idlib/gltfProperties.h and gltfParser.h whole)
// clang-format off: in the engine's order
#include "idlib/gltfProperties.h"
#include "idlib/gltfParser.h"
// clang-format on

/**
 * S4 (E-LIT-8): gltfPropertyArray's constructor allocates 32 items and counts none
 * (gltfParser.cpp:236-243: AssureSizeAlloc( 32, ... ) then SetNum( 0 )), takes them one
 * by one, and its destructor's DeleteContents deletes the Num() it took (:230-234,
 * List.h:386-407): the engine leaks the rest of every array it parses. The explicit
 * specialization of that one list type's DeleteContents deletes every allocated item, as
 * the template does the counted ones (a difference in memory only; nothing else of the
 * list's behaviour changes). Declared before the parser's range uses it.
 */
template <>
inline void idList<gltfPropertyItem*, TAG_IDLIB_LIST>::DeleteContents(const bool clear)
{
  for (int i = 0; i < size; i++)
  {
    if (list[i])
    {
      delete list[i];
    }
    list[i] = NULL;
  }
  if (clear)
  {
    Clear();
  }
  else
  {
    memset(list, 0, size * sizeof(gltfPropertyItem*));
  }
}

// the copied range of idlib/gltfParser.cpp (its LoadGLTF command is constexpr: registered
// nowhere, run never)
#include "idlib/gltfParser.cpp"

/**
 * S2: gltfManager::ExtractIdentifier (gltfParser.cpp:2378-2427) transcribed: the copy
 * reads the identifier's one token with an idParser (LEXFL_ALLOWPATHNAMES |
 * LEXFL_NOSTRINGESCAPECHARS), here the copied idLexer with those flags, which reads the
 * same token of a text without directives; the parser's Warnings are the lexer's.
 * Parse thread (the copied lexer, C20); engineGltfIdentifier's tests too.
 */
bool gltfManager::ExtractIdentifier(idStr& filename, int& id, idStr& name)
{
  idStr extension;
  idStr targetStr;
  filename.ExtractFileExtension(extension);
  idStr file = filename;
  file = file.StripFileExtension();
  file.ExtractFileExtension(targetStr);
  id = -1;

  if (!extension.Length())
  {
    idLib::Warning("no gltf identifier");
    return false;
  }

  if (targetStr.Length())
  {
    filename = file.Left(file.Length() - targetStr.Length() - 1) + "." + extension;

    idLexer parser(LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS);
    parser.LoadMemory(targetStr.c_str(), targetStr.Size(), "model:GltfID");

    idToken token;
    if (parser.ExpectAnyToken(&token))
    {
      if ((token.type == TT_NUMBER) && (token.subtype & TT_INTEGER))
      {
        id = token.GetIntValue();
      }
      else if (token.type == TT_NAME)
      {
        name = token;
      }
      else
      {
        parser.Warning("malformed gltf mesh identifier");
        return false;
      }
      return true;
    }
    else
    {
      parser.Warning("malformed gltf mesh identifier");
    }
  }

  return false;
}

/**
 * S3 (the director's decision): the namespace the MapFile ranges compile in, with the
 * stand-ins they find before the global names. Only MapPolygonMesh::SetContents names
 * them: declManager->FindMaterial of each polygon's material, its GetContentFlags and
 * Coverage (MapFile.cpp:2966-3007); its contents and opaque are never read by the model
 * path. The finish sets the tables it serves for its duration (MeshDeclsScope); one
 * finish at a time (FinishSurfaces' static already requires it, M-MODEL-POOL-01).
 */
namespace tbLitGltfMesh
{

// Material.h's idMaterial, as far as SetContents reads it: the parse thread's capture
class idMaterial
{
public:
  explicit idMaterial(const tb::lit::EngineModelMaterial& values)
    : m_contents{values.contentFlags}
    , m_coverage{materialCoverage_t(values.coverage)}
  {
  }

  constexpr idMaterial(const int contents, const materialCoverage_t coverage)
    : m_contents{contents}
    , m_coverage{coverage}
  {
  }

  // Material.h's GetContentFlags and Coverage
  int GetContentFlags() const { return m_contents; }
  materialCoverage_t Coverage() const { return m_coverage; }

private:
  int m_contents;
  materialCoverage_t m_coverage;
};

// declManager's FindMaterial over a finish's captures: a name the parse thread resolved
// gives its material, another the default material's values (and is counted)
class MeshDecls
{
public:
  explicit MeshDecls(const tb::lit::EngineModelMaterials& materials)
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

  int misses() const { return m_misses; }

private:
  std::vector<idMaterial> m_materials;
  std::vector<std::pair<std::string, std::size_t>> m_names;
  idMaterial m_default;
  mutable int m_misses = 0;
};

// the finish's tables, while it runs (constant-initialised; per-call working state)
TB_LIT_CONSTINIT std::atomic<const MeshDecls*> s_meshDecls{nullptr};

// a material of no contents and MC_BAD, for a FindMaterial outside a finish (never)
constexpr auto s_noMaterial = idMaterial{0, MC_BAD};

// what declManager-> reaches: FindMaterial of the running finish's tables, else
// s_noMaterial
struct MeshDeclsAccess
{
  const MeshDeclsAccess* operator->() const { return this; }

  const idMaterial* FindMaterial(const char* name, bool makeDefault = true) const
  {
    const auto* decls = s_meshDecls.load();
    return decls ? decls->FindMaterial(name, makeDefault) : &s_noMaterial;
  }
};

constexpr auto declManager = MeshDeclsAccess{};

// the finish's scope for its tables
class MeshDeclsScope
{
public:
  explicit MeshDeclsScope(const MeshDecls& decls) { s_meshDecls.store(&decls); }
  ~MeshDeclsScope() { s_meshDecls.store(nullptr); }

  MeshDeclsScope(const MeshDeclsScope&) = delete;
  MeshDeclsScope& operator=(const MeshDeclsScope&) = delete;
};

// MapPolygonMesh's ConvertFromBrush and ConvertFromPatch, declared in its range, name
// these (MapFile.h:84-289, not copied)
class idMapBrush;
class idMapPatch;

// the copied ranges (idlib/MapFile.h, MapFile.cpp, MapFile_gltf.cpp)
// clang-format off: the header before its ranges
#include "idlib/MapFile.h"
#include "idlib/MapFile.cpp"
#include "idlib/MapFile_gltf.cpp"
// clang-format on

} // namespace tbLitGltfMesh

using tbLitGltfMesh::MapPolygonMesh;

/**
 * S7: ModelManager.h's idImportOptions as far as the copied FindModelRoot reads it
 * (options->armature, Model_gltf.cpp:330); the preview always passes NULL (D139: no
 * import options, the director's decision).
 */
class idImportOptions
{
public:
  idStr armature;
};

/**
 * S7: Model_gltf.h's idRenderModelGLTF reduced to what the copied ProcessNode_r touches
 * (Model_gltf.cpp:64-136): the base's surfaces, bounds, NumSurfaces, AddSurface and its
 * class-scope stand-ins of declManager and modelSurface_t (shim/ShimModels.h).
 */
class idRenderModelGLTF : public idRenderModelStatic
{
public:
  void ProcessNode_r(
    gltfNode* modelNode,
    const idMat4& parentTransform,
    const idMat4& globalTransform,
    gltfData* data);
};

// the copied ranges of renderer/Model_gltf.cpp
#include "renderer/Model_gltf.cpp"

namespace tb::lit
{
namespace
{

// S6, S8: the parser's file cache (gltfData::dataList, fileDataHash, gltfParser.cpp:
// 164-165, written by Load and by ~gltfData's ClearData) between the parse thread's parse
// and an asset's destructor on any thread (constant-initialised)
TB_LIT_CONSTINIT std::mutex s_gltfCache;

// S4: every element the parser made with new, whose pointers gltfData's lists and its
// elements' lists hold (gltfParser.cpp:388-411 AddData's chunks excepted, which ~gltfData
// frees), and the accessor views of GetAccessorView (:2197-2318) with their elements.
// Each list grew by AssureSizeAlloc (List.h:689-713), which allocates whole granules (16)
// of elements and counts only those asked for: the elements past Num() are allocated too,
// and deleted here with the others (the engine leaks both kinds on every load: E-LIT-8).
// gltfData's lists are private: their const accessors give the pointers
template <typename T, typename F>
void forAllocated(const idList<T*>& list, F&& f)
{
  const auto* items = list.Ptr();
  for (auto i = 0; i < list.NumAllocated(); ++i)
  {
    if (items[i])
    {
      f(items[i]);
    }
  }
}

template <typename T>
void deleteAll(const idList<T*>& list)
{
  forAllocated(list, [](T* item) { delete item; });
}

void deleteTextureInfo(const gltfTexture_Info_Extensions& extensions)
{
  delete extensions.KHR_texture_transform;
}

void freeElements(gltfData& data)
{
  forAllocated(data.AccessorList(), [](gltfAccessor* accessor) {
    delete accessor->floatView;
    delete accessor->matView;
    if (accessor->vecView)
    {
      deleteAll(*accessor->vecView);
      delete accessor->vecView;
    }
    if (accessor->quatView)
    {
      deleteAll(*accessor->quatView);
      delete accessor->quatView;
    }
  });
  forAllocated(data.MeshList(), [](gltfMesh* mesh) {
    forAllocated(mesh->primitives, [](gltfMesh_Primitive* primitive) {
      deleteAll(primitive->attributes);
    });
    deleteAll(mesh->primitives);
  });
  forAllocated(
    data.NodeList(), [](gltfNode* node) { delete node->extensions.KHR_lights_punctual; });
  forAllocated(data.MaterialList(), [](gltfMaterial* material) {
    deleteTextureInfo(material->pbrMetallicRoughness.baseColorTexture.extensions);
    deleteTextureInfo(material->pbrMetallicRoughness.metallicRoughnessTexture.extensions);
    deleteTextureInfo(material->normalTexture.extensions);
    deleteTextureInfo(material->occlusionTexture.extensions);
    deleteTextureInfo(material->emissiveTexture.extensions);
    if (auto* gloss = material->extensions.KHR_materials_pbrSpecularGlossiness)
    {
      deleteTextureInfo(gloss->diffuseTexture.extensions);
      deleteTextureInfo(gloss->specularGlossinessTexture.extensions);
      delete gloss;
    }
  });
  forAllocated(data.TextureList(), [](gltfTexture* texture) {
    deleteTextureInfo(texture->extensions);
  });
  forAllocated(data.ExtensionsList(), [](gltfExtensions* extensions) {
    deleteAll(extensions->KHR_lights_punctual);
  });
  forAllocated(data.AnimationList(), [](gltfAnimation* animation) {
    deleteAll(animation->channels);
    deleteAll(animation->samplers);
  });
  deleteAll(data.BufferList());
  deleteAll(data.ImageList());
  deleteAll(data.SamplerList());
  deleteAll(data.BufferViewList());
  deleteAll(data.TextureList());
  deleteAll(data.AccessorList());
  deleteAll(data.ExtensionsUsedList());
  deleteAll(data.MeshList());
  deleteAll(data.SceneList());
  deleteAll(data.NodeList());
  deleteAll(data.CameraList());
  deleteAll(data.MaterialList());
  deleteAll(data.ExtensionsList());
  deleteAll(data.AnimationList());
  deleteAll(data.SkinList());
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

// the engine's read of an accessor in the BIN chunk (MapFile_gltf.cpp:51-61, :93-358) for
// the plan's second check: read bytes an element, stepping by the view's stride - back +
// read (back the conversion's seek: s_meshAttributeMap's elementSize times the
// component's bytes); within the view and the chunk
std::optional<std::string> accessorWindow(
  gltfData& data,
  const int accessorIndex,
  const std::size_t read,
  const std::size_t back,
  const std::size_t binLength,
  const std::string& what)
{
  const auto& accessors = data.AccessorList();
  if (accessorIndex < 0 || accessorIndex >= accessors.Num())
  {
    return what + ": accessor " + std::to_string(accessorIndex) + " out of range";
  }
  const auto* accessor = accessors[accessorIndex];
  const auto& views = data.BufferViewList();
  if (accessor->bufferView < 0 || accessor->bufferView >= views.Num())
  {
    return what + ": view " + std::to_string(accessor->bufferView) + " out of range";
  }
  const auto* view = views[accessor->bufferView];
  if (view->buffer != 0 || data.BufferList().Num() != 1 || binLength == 0)
  {
    return what + ": buffer " + std::to_string(view->buffer) + " is not the BIN chunk";
  }
  if (
    accessor->count < 0 || accessor->byteOffset < 0 || view->byteOffset < 0
    || view->byteLength < 0 || view->byteStride < 0)
  {
    return what + ": a negative count or offset";
  }
  const auto stride = std::size_t(view->byteStride);
  if (stride != 0 && stride < read)
  {
    return what + ": its stride is below its element";
  }
  const auto step = stride == 0 ? read : stride - back + read;
  const auto count = std::size_t(accessor->count);
  const auto extent = count == 0 ? 0 : (count - 1) * step + read;
  if (
    std::size_t(accessor->byteOffset) + extent > std::size_t(view->byteLength)
    || std::size_t(view->byteOffset) + std::size_t(view->byteLength) > binLength)
  {
    return what + ": its elements pass its view or the BIN chunk";
  }
  return std::nullopt;
}

} // namespace

EngineGltfModel::EngineGltfModel(gltfData* data)
  : m_data{data}
{
}

EngineGltfModel::~EngineGltfModel()
{
  if (!m_data)
  {
    return;
  }
  // S4: the elements, then the asset: ~gltfData frees its chunks and calls ClearData,
  // which reads the cache (emptied after the parse: it finds nothing)
  const auto lock = std::scoped_lock{s_gltfCache};
  freeElements(*m_data);
  delete m_data;
}

EngineGltfIdentifier engineGltfIdentifier(const std::string_view name)
{
  auto filename = idStr{std::string{name}.c_str()};
  auto id = -1;
  auto nodeName = idStr{};
  gltfManager::ExtractIdentifier(filename, id, nodeName);
  return {filename.c_str(), id, nodeName.c_str()};
}

EngineGltfParse MaterialDecls::parseGltfModel(
  const std::string& name, const std::string& bytes, const std::int64_t time)
{
  if (!m_registry)
  {
    return {nullptr, m_constructionError};
  }
  const auto errorsBefore = m_registry->errorCount();
  auto* data = static_cast<gltfData*>(nullptr);
  auto loaded = false;
  {
    // gltfParser.cpp:1929-2011: loadGLB's GetFileLength, OpenFileRead and reads get the
    // bytes and the time handed in
    const auto read = shim::ModelRead{m_registry->files(), name, bytes, time};
    const auto lock = std::scoped_lock{s_gltfCache};
    {
      GLTF_Parser gltf;
      loaded = gltf.Load(idStr{name.c_str()});
      // the asset is the preview's, not the parser's (whose Shutdown would delete it)
      data = gltf.currentAsset;
      gltf.currentAsset = nullptr;
    }
    // S8: no file cache: the asset leaves the parser's list and hash, and the hash's
    // table is freed, so that the next parse of any name reads its file again
    if (data)
    {
      auto fileName = data->FileName();
      gltfData::ClearData(fileName);
    }
    gltfData::fileDataHash.Free();
  }
  auto model = std::shared_ptr<const EngineGltfModel>{};
  if (data)
  {
    model = std::make_shared<const EngineGltfModel>(data);
  }
  // S6: an Error or FatalError of the parse (the parser's or its lexer's) is the engine's
  // drop: no model
  if (m_registry->errorCount() != errorsBefore)
  {
    return {nullptr, lastErrorOf(m_registry->messages())};
  }
  if (!loaded || !model)
  {
    return {nullptr, "GLTF_Parser::Load returned false"};
  }
  return {std::move(model), {}};
}

EngineGltfPlan engineGltfPlan(
  const EngineGltfModel& model,
  const EngineGltfIdentifier& identifier,
  const std::size_t binLength)
{
  auto plan = EngineGltfPlan{};
  auto* data = model.data();
  if (!data)
  {
    plan.problem = "no asset";
    return plan;
  }
  auto& nodes = data->NodeList();
  auto& meshes = data->MeshList();

  // the second check: the indexes ProcessNode_r, GetAllMeshes, GetAnimationIds and the
  // conversion read, and the windows, on what the copied parser made
  for (auto i = 0; i < nodes.Num(); ++i)
  {
    const auto* node = nodes[i];
    if (
      node->mesh >= meshes.Num() || node->mesh < -1 || node->skin < -1
      || node->skin >= data->SkinList().Num())
    {
      plan.problem = "node " + std::to_string(i) + ": its mesh or skin is out of range";
      return plan;
    }
    for (const auto child : node->children)
    {
      if (child < 0 || child >= nodes.Num())
      {
        plan.problem = "node " + std::to_string(i) + ": a child out of range";
        return plan;
      }
    }
  }
  for (const auto* node : nodes)
  {
    // FindModelRoot indexes nodes[ skin->skeleton ] (Model_gltf.cpp:409-411)
    if (node->skin >= 0)
    {
      const auto skeleton = data->SkinList()[node->skin]->skeleton;
      if (skeleton < 0 || skeleton >= nodes.Num())
      {
        plan.problem = "a skinned node's skin has no skeleton node";
        return plan;
      }
    }
  }
  for (const auto* skin : data->SkinList())
  {
    for (const auto joint : skin->joints)
    {
      if (joint < 0 || joint >= nodes.Num())
      {
        plan.problem = "a skin's joint out of range";
        return plan;
      }
    }
  }
  for (const auto* animation : data->AnimationList())
  {
    for (const auto* channel : animation->channels)
    {
      if (channel->target.node >= nodes.Num())
      {
        plan.problem = "an animation's target out of range";
        return plan;
      }
    }
  }
  for (auto m = 0; m < meshes.Num(); ++m)
  {
    for (const auto* primitive : meshes[m]->primitives)
    {
      if (primitive->material >= data->MaterialList().Num() || primitive->material < -1)
      {
        plan.problem = "mesh " + std::to_string(m) + ": a material out of range";
        return plan;
      }
      const auto* indices =
        primitive->indices >= 0 && primitive->indices < data->AccessorList().Num()
          ? data->AccessorList()[primitive->indices]
          : nullptr;
      if (!indices || indices->count % 3 != 0)
      {
        plan.problem =
          "mesh " + std::to_string(m) + ": the indices are missing or not triangles";
        return plan;
      }
      if (
        auto problem = accessorWindow(
          *data,
          primitive->indices,
          indices->typeSize,
          indices->typeSize,
          binLength,
          "the indices"))
      {
        plan.problem = std::move(*problem);
        return plan;
      }
      auto vertices = -1;
      for (const auto* attribute : primitive->attributes)
      {
        if (
          attribute->accessorIndex < 0
          || attribute->accessorIndex >= data->AccessorList().Num())
        {
          plan.problem = "mesh " + std::to_string(m) + ": an attribute out of range";
          return plan;
        }
        const auto* accessor = data->AccessorList()[attribute->accessorIndex];
        if (vertices >= 0 && accessor->count != vertices)
        {
          plan.problem = "mesh " + std::to_string(m) + ": the attributes' counts differ";
          return plan;
        }
        vertices = accessor->count;
        using Type = gltfMesh_Primitive_Attribute::Type;
        auto read = std::size_t{0};
        switch (attribute->type)
        {
        case Type::Position:
        case Type::Normal:
          read = 3;
          break;
        case Type::TexCoord0:
          read = 2;
          break;
        case Type::Tangent:
        case Type::Weight:
        case Type::Color0:
        case Type::Joints:
          read = 4;
          break;
        default:
          break;
        }
        if (read > 0)
        {
          if (
            auto problem = accessorWindow(
              *data,
              attribute->accessorIndex,
              read * accessor->typeSize,
              attribute->elementSize * accessor->typeSize,
              binLength,
              "an attribute"))
          {
            plan.problem = std::move(*problem);
            return plan;
          }
        }
      }
    }
  }
  for (auto i = 0; i < data->SceneList().Num(); ++i)
  {
    for (const auto root : data->SceneList()[i]->nodes)
    {
      if (root < 0 || root >= nodes.Num())
      {
        plan.problem = "scene " + std::to_string(i) + ": a node out of range";
        return plan;
      }
    }
  }
  if (data->DefaultScene() < 0 || data->DefaultScene() >= data->SceneList().Num())
  {
    plan.problem = "the default scene is out of range";
    return plan;
  }

  // InitFromFile (Model_gltf.cpp:472-628), options NULL: the root, the mesh nodes and the
  // animations
  auto rootName = idStr{identifier.name.c_str()};
  auto rootID = -1;
  auto* currentSkin = static_cast<gltfSkin*>(nullptr);
  auto* root = FindModelRoot(data, nullptr, rootName, &rootID, &currentSkin);
  auto meshNodeIds = idList<int>{};
  if (rootID != -1)
  {
    if (currentSkin)
    {
      // also collect meshes above armature but they need to be bound to the skeleton
      data->GetAllSkinnedMeshes(currentSkin, meshNodeIds);
    }
    else
    {
      // get all meshes in hierachy, starting at root
      data->GetAllMeshes(root, meshNodeIds);
    }
  }
  else
  {
    // collect all meshes without root
    data->GetAllMeshes(meshNodeIds);
    if (meshNodeIds.Num() == 0)
    {
      plan.defaultReason = "Can't find meshes in static glTF2 model (Model_gltf.cpp:555)";
      return plan;
    }
  }

  // :564-626: the animations of the first skinned mesh node and of its skin's first joint
  auto totalAnims = 0;
  auto lastSkin = -1;
  auto animIds = idList<int>{};
  for (const auto meshID : meshNodeIds)
  {
    auto* meshNode = nodes[meshID];
    auto animCount = 0;
    if (meshNode->skin != -1 && meshNode->skin != lastSkin && lastSkin == -1)
    {
      animCount = data->GetAnimationIds(meshNode, animIds);
      // check if this model has a skeleton/bones
      if (meshNode->skin >= 0)
      {
        lastSkin = meshNode->skin;
        if (!currentSkin)
        {
          // :584: currentSkin->joints with no skin FindModelRoot found (a named root
          // without one): the engine dereferences NULL
          plan.defaultReason =
            "a skinned mesh under a root without a skin: the engine dereferences a "
            "NULL skin (Model_gltf.cpp:584)";
          return plan;
        }
        if (currentSkin->joints.Num())
        {
          animCount = data->GetAnimationIds(nodes[currentSkin->joints[0]], animIds);
        }
      }
    }
    totalAnims += animCount;
  }
  plan.animated = totalAnims > 0;
  plan.animations = totalAnims;
  for (const auto meshID : meshNodeIds)
  {
    plan.meshNodes.push_back(meshID);
  }

  // ProcessNode_r's walk (:64-136), its surfaces and their materials in its order
  auto walk = [&](auto& self, const gltfNode* node) -> void {
    if (node->mesh >= 0)
    {
      for (const auto* primitive : meshes[node->mesh]->primitives)
      {
        const auto* material =
          primitive->material != -1 ? data->MaterialList()[primitive->material] : nullptr;
        plan.materialNames.emplace_back(
          material && !gltf_forceBspMeshTexture.GetBool()
            ? material->name.c_str()
            : "textures/base_wall/snpanel2rust");
        plan.surfaceVertices.push_back(
          primitive->attributes.Num() > 0
            ? std::size_t(
                data->AccessorList()[primitive->attributes[0]->accessorIndex]->count)
            : 0);
        // MapFile_gltf.cpp:103-358: the fields no attribute of it writes
        using Type = gltfMesh_Primitive_Attribute::Type;
        auto written = std::array<bool, 4>{};
        for (const auto* attribute : primitive->attributes)
        {
          written[0] = written[0] || attribute->type == Type::Normal;
          written[1] = written[1] || attribute->type == Type::TexCoord0;
          written[2] = written[2] || attribute->type == Type::Color0
                       || attribute->type == Type::Joints;
          written[3] = written[3] || attribute->type == Type::Weight;
        }
        plan.heapVertexFields =
          plan.heapVertexFields
          || !std::all_of(written.begin(), written.end(), [](bool w) { return w; });
      }
    }
    plan.skinned = plan.skinned || node->skin >= 0;
    for (const auto child : node->children)
    {
      self(self, nodes[child]);
    }
  };
  for (const auto meshID : plan.meshNodes)
  {
    walk(walk, nodes[meshID]);
  }
  return plan;
}

EngineModel engineFinishGltfModel(
  const EngineGltfModel& model,
  const EngineGltfPlan& plan,
  const EngineModelMaterials& materials,
  const std::string_view name,
  const bool useMikktspace)
{
  const auto decls = idRenderModelStatic::ModelDecls{materials};
  const auto meshDecls = tbLitGltfMesh::MeshDecls{materials};
  const auto meshScope = tbLitGltfMesh::MeshDeclsScope{meshDecls};
  // E-LIT-7: the vertices MapPolygonMesh sizes with Mem_Alloc are cleared
  const auto cleared = shim::ClearedAllocations{};
  auto glTF = idRenderModelGLTF{};
  glTF.declManager = &decls;
  glTF.tr.defaultMaterial = decls.defaultMaterial();

  // Model_gltf.cpp:472-747 InitFromFile without options, transcribed: the name, the
  // bounds cleared, globalTransform = blenderToDoomTransform, useMikktspace as given,
  // ProcessNode_r over the mesh nodes from the identity
  const auto fileName = std::string{name};
  glTF.name = fileName.c_str();
  glTF.PurgeModel();
  glTF.bounds.Clear();
  auto* data = model.data();
  const auto& nodes = data->NodeList();
  const auto globalTransform = blenderToDoomTransform;
  for (const auto meshID : plan.meshNodes)
  {
    gltfNode* meshNode = nodes[meshID];
    glTF.ProcessNode_r(meshNode, mat4_identity, globalTransform, data);
  }
  if (glTF.surfaces.Num() <= 0)
  {
    // :713-722
    common->Warning("Couldn't load model: '%s'", glTF.name.c_str());
    glTF.MakeDefaultModel();
    return shim::modelOf(glTF, decls);
  }
  // derive mikktspace tangents from normals
  glTF.FinishSurfaces(useMikktspace);
  if (meshDecls.misses() > 0)
  {
    common->Warning(
      "%s: %d material names of its polygons were not resolved",
      fileName.c_str(),
      meshDecls.misses());
  }
  return shim::modelOf(glTF, decls);
}

} // namespace tb::lit
