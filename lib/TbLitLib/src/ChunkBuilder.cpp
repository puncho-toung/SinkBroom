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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/MapFile.cpp, renderer/Model.cpp,
// renderer/Material.h, tools/compilers/dmap/map.cpp, dmap/usurface.cpp,
// dmap/output.cpp and d3xp/Game_local.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
//   Copyright (C) 2020 Admer
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ChunkBuilder.h"

#include "lit/AreaTree.h"
#include "lit/CpuPool.h"
#include "lit/MaterialService.h"
#include "lit/ProcFile.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace tb::lit
{
namespace
{

// renderer/Material.h:349, :392-394, :295-297
constexpr auto ContentsOrigin = 1 << 22;
constexpr auto CoverageOpaque = 1;
constexpr auto CoveragePerforated = 2;

bool equalsNoCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::equal(
           lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             const auto lower = [](const char c) {
               return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
             };
             return lower(a) == lower(b);
           });
}

bool startsWithNoCase(const std::string_view text, const std::string_view prefix)
{
  return text.size() >= prefix.size()
         && equalsNoCase(text.substr(0, prefix.size()), prefix);
}

std::uint64_t fnv(std::uint64_t h, const void* data, const std::size_t size)
{
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (auto i = std::size_t{0}; i < size; ++i)
  {
    h ^= bytes[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

constexpr auto FnvBasis = 0xCBF29CE484222325ull;

EngineVec3 toFloat(const vm::vec3d& v)
{
  return {float(v.x()), float(v.y()), float(v.z())};
}

EngineVec3 sub(const EngineVec3& a, const EngineVec3& b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

// R3.9: a vertex position as ParseModel reads back what dmap wrote (WriteFloat, idLexer)
EngineVec3 readBack(const EngineVec3& xyz)
{
  return {procReadBack(xyz[0]), procReadBack(xyz[1]), procReadBack(xyz[2])};
}

// what a face needs of its material
struct FaceMaterial
{
  std::string name;
  const MaterialEntry* entry = nullptr;
  bool drawn = true;
  bool castsShadow = true;
  float width = 8;
  float height = 8;
  bool provisional = true;
};

// Material.h IsDrawn: stages, an entity gui or a gui
bool isDrawn(const MaterialEntry& entry)
{
  return !entry.stages.empty() || entry.entityGui != 0 || !entry.gui.empty();
}

struct MaterialCache
{
  const MaterialTable* table;
  std::map<std::string, FaceMaterial> cache;
  std::set<std::string> pending;

  const FaceMaterial& get(const std::string& name)
  {
    if (const auto it = cache.find(name); it != cache.end())
    {
      return it->second;
    }
    auto material = FaceMaterial{};
    material.name = name;
    const auto slot = table ? table->find(name) : nullptr;
    if (slot && slot->entry)
    {
      material.entry = slot->entry.get();
      material.drawn = isDrawn(*slot->entry);
      material.castsShadow = slot->entry->surfaceCastsShadow;
      material.width = float(slot->entry->editorImageSize.width);
      material.height = float(slot->entry->editorImageSize.height);
      material.provisional =
        slot->entry->editorImageSize.state == EditorImageSize::State::Default;
    }
    else if (!slot)
    {
      // not looked up yet: the chunk is built with the defaults, provisionally
      pending.insert(name);
    }
    return cache.emplace(name, std::move(material)).first->second;
  }
};

// an entity as its primitives are built
struct EntityContext
{
  const SceneEntity* entity = nullptr;
  ChunkEntityState state;
  // the engine's coordinates of the primitives are the world's minus this (origin, or a
  // func_group's origin when its primitives join worldspawn)
  EngineVec3 parseOrigin = {0, 0, 0};
  // and the vertices are relative to the parse origin plus the origin offset
  EngineVec3 originOffset = {0, 0, 0};
  // the chunk key's entity: worldspawn for its own and a func_group's primitives
  PreviewId chunkEntity = NoPreviewId;
  bool world = false;
  int area = 0;
  // LP7.2 (C53): a brush entity's customShader name, else its customSkin (null: none, or
  // its slot is pending: skinPending)
  std::string shader;
  const SkinRecord* skin = nullptr;
  bool skinPending = false;
};

// a triangle of a chunk under construction
struct Triangle
{
  std::array<EngineDrawVert, 3> verts;
};

// the triangles of one (area, entity, draw material, cell) and one own material (LP7.2,
// C53: material is the faces' own, drawMaterial the key's)
struct Bucket
{
  ChunkKey key;
  const FaceMaterial* material = nullptr;
  const FaceMaterial* drawMaterial = nullptr;
  const EntityContext* entity = nullptr;
  std::vector<Triangle> triangles;
};

// LP7.4 (C55): the adopted islands of one (area, entity, material, cell)
struct AdoptedBucket
{
  ChunkKey key;
  const FaceMaterial* material = nullptr;
  const FaceMaterial* drawMaterial = nullptr;
  const EntityContext* entity = nullptr;
  const EntityAdoption* adoption = nullptr;
  std::vector<const AdoptedIsland*> islands;
};

// MapFile.cpp:1171-1181: the origin key applies to the primitives after it (the last one
// given, as TrenchBroom writes the keys first)
EngineVec3 parseOriginOf(const SceneEntity& entity)
{
  return mapOrigin(entity.keys);
}

class Builder
{
private:
  const ChunkJobInput& m_input;
  const CpuJobContext* m_context;
  ChunkJobOutput m_output;
  MaterialCache m_materials;
  std::string m_overrideMaterial;
  bool m_override = false;
  std::vector<std::string> m_removedClasses;
  std::unordered_map<PreviewId, EntityContext> m_contexts;
  // LP7.2 (C53): by the key (its draw material) and the faces' own material, so that two
  // own materials a skin maps to one are separate chunks (the engine's separate surfaces)
  std::map<std::pair<ChunkKey, std::string>, Bucket> m_buckets;
  std::set<CellRef> m_cells;
  // LP7.4 (C55): the adopted islands of the job's cells per (area, entity, material,
  // cell), and the first part after the editor triangulation's of each key
  std::map<ChunkKey, AdoptedBucket> m_adopted;
  std::map<ChunkKey, int> m_nextPart;

public:
  Builder(const ChunkJobInput& input, const CpuJobContext* context)
    : m_input{input}
    , m_context{context}
    , m_materials{input.materials.get(), {}, {}}
  {
    m_cells.insert(input.cells.begin(), input.cells.end());
    readWorldspawn();
  }

  ChunkJobOutput build()
  {
    m_output.cells = m_input.cells;
    for (const auto& node : m_input.nodes)
    {
      // R2.1: a cancelled pool's job stops here; its output is dropped
      if (m_context && m_context->stale())
      {
        return std::move(m_output);
      }
      if (!node || !node->visible)
      {
        continue;
      }
      const auto* context = contextOf(node->entity);
      if (!context || !context->state.visible)
      {
        continue;
      }
      const auto ref = CellRef{context->chunkEntity, cellOfNode(*node)};
      if (!m_cells.contains(ref))
      {
        continue;
      }
      if (node->kind == SnapshotNodeKind::Brush)
      {
        addBrush(*node, *context, ref.cell);
      }
      else if (node->kind == SnapshotNodeKind::Patch && node->patch)
      {
        addPatch(*node->patch, *context, ref.cell);
      }
    }
    addAdoptedIslands();
    for (auto& [key, bucket] : m_buckets)
    {
      finishBucket(bucket);
    }
    for (auto& [key, adopted] : m_adopted)
    {
      finishAdopted(adopted);
    }
    for (const auto& [id, context] : m_contexts)
    {
      if (!context.world)
      {
        m_output.entities.push_back(context.state);
      }
    }
    m_output.pendingMaterials.assign(
      m_materials.pending.begin(), m_materials.pending.end());
    for (const auto& [name, material] : m_materials.cache)
    {
      m_output.materials.push_back(name);
    }
    return std::move(m_output);
  }

private:
  // MapFile.cpp:1759-1796: removeEntities* and overrideMaterial from worldspawn's map
  // keys
  void readWorldspawn()
  {
    if (!m_input.worldspawn)
    {
      return;
    }
    m_removedClasses = removedClassnames(m_input.worldspawn->keys);
    const auto merged = mergeSpawnArgs(m_input.worldspawn->keys, nullptr);
    if (const auto* material = findKey(merged, "overrideMaterial"))
    {
      m_override = true;
      m_overrideMaterial = *material;
    }
  }

  bool removed(const std::string& classname) const
  {
    return isRemovedClassname(m_removedClasses, classname);
  }

  const EntityContext* contextOf(const PreviewId id)
  {
    if (const auto it = m_contexts.find(id); it != m_contexts.end())
    {
      return &it->second;
    }
    const auto it = m_input.entities.find(id);
    if (it == m_input.entities.end() || !it->second)
    {
      return nullptr;
    }
    return &m_contexts.emplace(id, makeContext(*it->second)).first->second;
  }

  EntityContext makeContext(const SceneEntity& entity)
  {
    auto context = EntityContext{};
    context.entity = &entity;
    context.state.id = entity.id;
    const auto isWorld = equalsNoCase(entity.classname, "worldspawn");
    const auto isFuncGroup = equalsNoCase(entity.classname, "func_group");

    // MapFile.cpp:2270-2282: removed entities lose everything
    if (removed(entity.classname))
    {
      context.state.visible = false;
      return context;
    }

    // C22, A10: the map keys over the entityDef's
    const auto def = m_input.entityDefs.find(entity.classname);
    const auto* defEntry = def != m_input.entityDefs.end() ? def->second.get() : nullptr;
    const auto spawnArgs =
      mergeSpawnArgs(entity.keys, defEntry ? &defEntry->keys : nullptr);
    // Game_local.cpp:978-985, dmap/map.cpp:601-606: a map-only class has no primitives
    if (defEntry && defEntry->dmapOnly)
    {
      context.state.visible = false;
      return context;
    }

    if (isWorld || isFuncGroup)
    {
      // MapFile.cpp:1109-1112 worldspawn's origin is forced to zero; :1811-1826 a
      // func_group's primitives join worldspawn, relative to the func_group's origin
      context.world = true;
      context.parseOrigin = isWorld ? EngineVec3{0, 0, 0} : parseOriginOf(entity);
      context.chunkEntity =
        isWorld || !m_input.worldspawn ? entity.id : m_input.worldspawn->id;
      context.state.id = context.chunkEntity;
      context.state.hash = 0;
      return context;
    }

    // K17, A14: drawn only through its model key, which names the brush model dmap makes
    // from name
    const auto name = spawnString(spawnArgs, "name");
    const auto model = spawnString(spawnArgs, "model");
    context.state.visible = !name.empty() && !model.empty() && equalsNoCase(name, model);

    context.state.parms = parseSpawnArgsToRenderEntity(spawnArgs);
    // LP7.0 (D128, P7.17): Entity.cpp:651-656, a hidden entity's EV_Hide removes its
    // render entity: its pieces are skipped here, as a noshadows or _color key is read
    if (context.state.parms.hidden)
    {
      context.state.visible = false;
    }
    context.parseOrigin = parseOriginOf(entity);
    context.originOffset = originOffsetOf(entity.id);
    context.state.originOffset = context.originOffset;
    // Entity.cpp:701-707, Game_local.cpp:4141-4149: drawn at origin + __brushOrigin
    context.state.modelOrigin = {
      context.state.parms.origin[0] + context.originOffset[0],
      context.state.parms.origin[1] + context.originOffset[1],
      context.state.parms.origin[2] + context.originOffset[2]};
    context.chunkEntity = entity.id;
    context.area =
      m_input.proc ? pointInArea(*m_input.proc, context.state.modelOrigin) : 0;
    context.state.hash = renderEntityHash(context.state.parms)
                         ^ fnv(FnvBasis, context.originOffset.data(), sizeof(EngineVec3));
    // LP7.2 (C53): ParseSpawnArgsToRenderEntity's customShader, else its customSkin
    // (Entity.cpp:299-315; the frontend takes the shader first, :585-607)
    if (!context.state.parms.shader.empty())
    {
      context.shader = context.state.parms.shader;
    }
    else if (!context.state.parms.skin.empty())
    {
      const auto slot = m_input.skins.find(context.state.parms.skin);
      if (slot == m_input.skins.end() || !slot->second)
      {
        context.skinPending = true;
      }
      else
      {
        context.skin = slot->second->entry.get();
      }
    }
    return context;
  }

  // MapFile.cpp:1556-1590 CalculateBrushOrigin: the mean bounds centre of the entity's
  // CONTENTS_ORIGIN brushes, relative to its origin
  EngineVec3 originOffsetOf(const PreviewId entity)
  {
    const auto it = m_input.entities.find(entity);
    const auto origin = it != m_input.entities.end() && it->second
                          ? parseOriginOf(*it->second)
                          : EngineVec3{0, 0, 0};
    auto sum = EngineVec3{0, 0, 0};
    auto count = 0;
    for (const auto& node : m_input.nodes)
    {
      if (!node || node->entity != entity || node->kind != SnapshotNodeKind::Brush)
      {
        continue;
      }
      if (!isOriginBrush(*node))
      {
        continue;
      }
      auto points = std::vector<EngineVec3>{};
      for (const auto& face : node->faces)
      {
        for (const auto& p : face.polygon)
        {
          points.push_back(sub(toFloat(p), origin));
        }
      }
      if (points.empty())
      {
        continue;
      }
      const auto bounds = engineBoundsOfPoints(points);
      // idBounds::GetCenter
      for (auto k = 0; k < 3; ++k)
      {
        sum[std::size_t(k)] +=
          (bounds[1][std::size_t(k)] + bounds[0][std::size_t(k)]) * 0.5f;
      }
      ++count;
    }
    if (count == 0)
    {
      return {0, 0, 0};
    }
    // originOffset /= static_cast<float>( originBrushes.Num() )
    for (auto& c : sum)
    {
      c /= float(count);
    }
    return sum;
  }

  // MapFile.cpp:1036-1048 IsOriginBrush: a side whose material has CONTENTS_ORIGIN;
  // textures/common/origin counts before the table has it
  bool isOriginBrush(const SceneNode& node)
  {
    for (const auto& face : node.faces)
    {
      const auto name = engineSideMaterial(face.material);
      if (equalsNoCase(name, "textures/common/origin"))
      {
        return true;
      }
      const auto& material = m_materials.get(name);
      if (material.entry && (material.entry->contentFlags & ContentsOrigin) != 0)
      {
        return true;
      }
    }
    return false;
  }

  const FaceMaterial& materialOf(const std::string& engineName)
  {
    // MapFile.cpp:1767-1796: overrideMaterial replaces brush sides and patches
    return m_materials.get(m_override ? m_overrideMaterial : engineName);
  }

  /**
   * LP7.2 (C53, D131, P7.6): the material a face of its own material is drawn with under
   * its entity's shader or skin, in R_AddSingleModel's order (tr_frontend_addmodels.cpp:
   * 579-607; the caller skipped a face neither drawn nor casting shadows, :579): a
   * customShader replaces it and a deforming surface maps to nothing (:588-594); else a
   * customSkin's RemapShaderBySkin (DeclSkin.cpp: a surface whose own material is not
   * drawn is never remapped, the first mapping of the material or the "*" wildcard
   * wins), and a remapped material neither drawn nor casting shadows is skipped
   * (:601-605). Null: skipped. A material the table does not have yet is not remapped by
   * a skin (its IsDrawn is not known): the chunk is provisional and built again when it
   * arrives
   */
  const FaceMaterial* drawMaterialOf(
    const FaceMaterial& original, const EntityContext& context)
  {
    if (context.world)
    {
      return &original;
    }
    if (!context.shader.empty())
    {
      if (original.entry && original.entry->deform != 0)
      {
        return nullptr;
      }
      return &m_materials.get(context.shader);
    }
    if (!context.skin || !original.entry || !original.drawn)
    {
      return &original;
    }
    for (const auto& mapping : context.skin->mappings)
    {
      if (mapping.from.empty() || mapping.from == original.entry->name)
      {
        const auto& remapped = m_materials.get(mapping.to);
        if (remapped.entry && !remapped.drawn && !remapped.castsShadow)
        {
          return nullptr;
        }
        return &remapped;
      }
    }
    return &original;
  }

  Bucket& bucket(
    const int area,
    const EntityContext& context,
    const FaceMaterial& material,
    const FaceMaterial& drawMaterial,
    const CellCoord& cell)
  {
    auto key = ChunkKey{area, context.chunkEntity, drawMaterial.name, cell, 0};
    auto [it, inserted] = m_buckets.try_emplace(std::pair{key, material.name});
    if (inserted)
    {
      it->second.key = key;
      it->second.material = &material;
      it->second.drawMaterial = &drawMaterial;
      it->second.entity = &context;
    }
    return it->second;
  }

  // the vertex of a side as dmap makes it (usurface.cpp:233-252): st from the position
  // relative to the parse origin, then the vertex relative to the origin offset
  EngineDrawVert sideVert(
    const EngineVec3& parsed,
    const SceneFace& face,
    const FaceMaterial& material,
    const EnginePlane& plane,
    const EntityContext& context)
  {
    auto vert = engineClearedVert();
    const auto st =
      engineValveTexCoord(parsed, face.texture, material.width, material.height);
    engineSetTexCoord(vert, st[0], st[1]);
    engineSetNormal(vert, {plane.a, plane.b, plane.c});
    // dmap/output.cpp:589 WriteUTriangles( ..., entity->originOffset ), read back
    vert.xyz = readBack(sub(parsed, context.originOffset));
    return vert;
  }

  // LP7.4 (C55): the record of a face piece for the adoption pass, in the frame the
  // .proc's positions are in (sideVert's: the parse frame less the origin offset, read
  // back), the side plane moved with it and st from the parse frame (stOrigin)
  void addPiece(
    const SceneNode& node,
    const std::size_t faceIndex,
    const std::size_t pieceIndex,
    const AreaPiece& piece,
    const SceneFace& face,
    const FaceMaterial& material,
    const EnginePlane& plane,
    const EntityContext& context,
    const CellCoord& cell)
  {
    auto record = ChunkPiece{};
    record.cell = CellRef{context.chunkEntity, cell};
    auto& adoption = record.piece;
    adoption.key =
      AdoptionPieceKey{node.id, std::uint32_t(faceIndex), std::uint32_t(pieceIndex)};
    // a world piece's model is its area's; a brush entity's its own (K17)
    adoption.area = context.world ? piece.area : -1;
    adoption.material = material.name;
    const auto& o = context.originOffset;
    adoption.plane = EnginePlane{
      plane.a,
      plane.b,
      plane.c,
      plane.d + plane.a * o[0] + plane.b * o[1] + plane.c * o[2]};
    adoption.texVec = engineValveTextureVectors(face.texture);
    adoption.textureWidth = material.width;
    adoption.textureHeight = material.height;
    adoption.stOrigin = o;
    adoption.points.reserve(piece.points.size());
    for (const auto& p : piece.points)
    {
      adoption.points.push_back(readBack(sub(p, o)));
    }
    m_output.pieces.push_back(std::move(record));
  }

  void addBrush(
    const SceneNode& node, const EntityContext& context, const CellCoord& cell)
  {
    for (auto faceIndex = std::size_t{0}; faceIndex < node.faces.size(); ++faceIndex)
    {
      const auto& face = node.faces[faceIndex];
      const auto& material = materialOf(engineSideMaterial(face.material));
      // usurface.cpp:192: no faces for sides neither drawn nor casting shadows
      if (!material.drawn && !material.castsShadow)
      {
        continue;
      }
      // LP7.2 (C53): the entity's shader or skin, at chunk build
      const auto* drawMaterial = drawMaterialOf(material, context);
      // ParseValve220: the plane from the three points relative to the origin;
      // FindFloatPlane fixes it
      const auto plane = engineSidePlane(
        sub(toFloat(face.points[0]), context.parseOrigin),
        sub(toFloat(face.points[1]), context.parseOrigin),
        sub(toFloat(face.points[2]), context.parseOrigin));
      if (!plane || face.polygon.size() < 3)
      {
        continue;
      }
      // the engine's winding order: TrenchBroom's polygon reversed
      auto winding = std::vector<EngineVec3>{};
      winding.reserve(face.polygon.size());
      for (auto it = face.polygon.rbegin(); it != face.polygon.rend(); ++it)
      {
        winding.push_back(sub(toFloat(*it), context.parseOrigin));
      }
      if (winding.size() > std::size_t(engineBounds::MaxWindingPoints))
      {
        warnOnce("a face with more than 1024 vertices is not drawn");
        continue;
      }

      auto pieces = context.world
                      ? clipSideIntoAreas(m_input.proc.get(), winding, *plane)
                      : std::vector<AreaPiece>{AreaPiece{context.area, winding}};
      for (auto pieceIndex = std::size_t{0}; pieceIndex < pieces.size(); ++pieceIndex)
      {
        const auto& piece = pieces[pieceIndex];
        if (piece.area == SolidArea && m_input.proc && context.world)
        {
          ++m_output.solidPieces;
        }
        // a piece in solid is in no area model: never a candidate (K14)
        if (m_input.collectPieces && !(context.world && piece.area == SolidArea))
        {
          addPiece(
            node, faceIndex, pieceIndex, piece, face, material, *plane, context, cell);
        }
        // LP7.4 (C55): an adopted island draws this piece's part of the face; its other
        // roles (the references through the island's chunk, the solid count) stay
        if (replaced(
              context.chunkEntity,
              AdoptionPieceKey{
                node.id, std::uint32_t(faceIndex), std::uint32_t(pieceIndex)}))
        {
          continue;
        }
        // LP7.2 (C53): a surface the shader or skin skips draws nothing; its piece is
        // still the adoption's (the own material's group)
        if (!drawMaterial)
        {
          continue;
        }
        auto& target = bucket(piece.area, context, material, *drawMaterial, cell);
        // TriListForSide's fan: w[0], w[i - 1], w[i]
        const auto& w = piece.points;
        for (auto i = std::size_t{2}; i < w.size(); ++i)
        {
          target.triangles.push_back(Triangle{{
            sideVert(w[0], face, material, *plane, context),
            sideVert(w[i - 1], face, material, *plane, context),
            sideVert(w[i], face, material, *plane, context),
          }});
        }
      }
    }
  }

  void addPatch(
    const ScenePatch& patch, const EntityContext& context, const CellCoord& cell)
  {
    const auto& material = materialOf(enginePatchMaterial(patch.material));
    if (!material.drawn && !material.castsShadow)
    {
      return;
    }
    // LP7.2 (C53): the entity's shader or skin
    const auto* drawMaterial = drawMaterialOf(material, context);
    if (!drawMaterial)
    {
      return;
    }
    // MapFile.cpp:330-352: the engine's width is TrenchBroom's row count; the control
    // points go to [column · width + row], relative to the origin, st through
    // SetTexCoord
    auto engine = EnginePatch{};
    engine.width = int(patch.rows);
    engine.height = int(patch.columns);
    engine.explicitSubdivisions = patch.explicitSubdivisions;
    engine.horizontalSubdivisions = int(patch.horizontalSubdivisions);
    engine.verticalSubdivisions = int(patch.verticalSubdivisions);
    if (patch.controlPoints.size() != std::size_t(patch.rows) * patch.columns)
    {
      warnOnce("a patch whose control points do not match its size is not drawn");
      return;
    }
    engine.controlPoints.resize(patch.controlPoints.size());
    for (auto row = std::size_t{0}; row < patch.rows; ++row)
    {
      for (auto column = std::size_t{0}; column < patch.columns; ++column)
      {
        const auto& p = patch.controlPoints[row * patch.columns + column];
        auto vert = engineClearedVert();
        vert.xyz = sub({float(p[0]), float(p[1]), float(p[2])}, context.parseOrigin);
        engineSetTexCoord(vert, float(p[3]), float(p[4]));
        engine.controlPoints[column * patch.rows + row] = vert;
      }
    }
    auto error = std::string{};
    const auto surface = engineSubdividePatch(engine, &error);
    if (!surface)
    {
      // K19
      warnOnce(fmt::format("a patch is not drawn: {}", error));
      return;
    }
    for (auto i = std::size_t{0}; i + 2 < surface->indexes.size(); i += 3)
    {
      auto original = std::array<EngineDrawVert, 3>{
        surface->verts[std::size_t(surface->indexes[i])],
        surface->verts[std::size_t(surface->indexes[i + 1])],
        surface->verts[std::size_t(surface->indexes[i + 2])]};
      // usurface.cpp:657-661: skip degenerate triangles from pinched curves
      if (engineTriangleArea(original[0].xyz, original[1].xyz, original[2].xyz) <= 0)
      {
        continue;
      }
      const auto corners =
        std::array<EngineVec3, 3>{original[0].xyz, original[1].xyz, original[2].xyz};
      auto pieces = context.world ? clipTriangleIntoAreas(m_input.proc.get(), corners)
                                  : std::vector<AreaPiece>{AreaPiece{
                                      context.area, {corners.begin(), corners.end()}}};
      for (auto& piece : pieces)
      {
        if (piece.area == SolidArea && m_input.proc && context.world)
        {
          ++m_output.solidPieces;
        }
        auto& target = bucket(piece.area, context, material, *drawMaterial, cell);
        const auto& w = piece.points;
        for (auto k = std::size_t{2}; k < w.size(); ++k)
        {
          auto tri = Triangle{};
          const auto corner = [&](const EngineVec3& xyz) {
            // R3.5: usurface.cpp:649-663 sends every patch triangle, split or not,
            // through WindingToTriList, which takes its corners from the original's
            // (TriVertsFromOriginal)
            auto vert = engineVertFromOriginal(xyz, original);
            vert.xyz = readBack(sub(xyz, context.originOffset));
            vert.color = {255, 255, 255, 255};
            vert.color2 = {0, 0, 0, 0};
            // 39-PATCH-NORMAL-FLAKE.md F2 (M-GEO-DIFF-01): the original's tangent and
            // fourth normal byte were never written (Subdivide's stack vertices,
            // SubdivideExplicit's heap block); the weld compares all 32 bytes, so they
            // are the cleared vertex's, as dmap's MatchVert ignores them
            const auto cleared = engineClearedVert();
            vert.tangent = cleared.tangent;
            vert.normal[3] = cleared.normal[3];
            return vert;
          };
          tri.verts = {corner(w[0]), corner(w[k - 1]), corner(w[k])};
          target.triangles.push_back(tri);
        }
      }
    }
  }

  void warnOnce(std::string warning)
  {
    if (
      std::find(m_output.warnings.begin(), m_output.warnings.end(), warning)
      == m_output.warnings.end())
    {
      m_output.warnings.push_back(std::move(warning));
    }
  }

  struct WeldKey
  {
    EngineDrawVert vert;
    bool operator==(const WeldKey& other) const { return vert == other.vert; }
  };

  struct WeldHash
  {
    std::size_t operator()(const WeldKey& key) const
    {
      return std::size_t(fnv(FnvBasis, &key.vert, sizeof(EngineDrawVert)));
    }
  };

  void finishBucket(Bucket& bucket)
  {
    // the bucket's triangles in parts of at most MaxChunkWeldedVerts welded vertices;
    // LP7.2 (C53): the parts of a key go on across its own materials
    auto part = m_nextPart[bucket.key];
    auto verts = std::vector<EngineDrawVert>{};
    auto indexes = std::vector<std::uint16_t>{};
    auto weld = std::unordered_map<WeldKey, std::uint16_t, WeldHash>{};
    const auto flush = [&]() {
      if (!indexes.empty())
      {
        auto key = bucket.key;
        key.part = part++;
        finishChunk(
          key,
          *bucket.material,
          *bucket.drawMaterial,
          *bucket.entity,
          std::move(verts),
          std::move(indexes),
          true);
      }
      verts.clear();
      indexes.clear();
      weld.clear();
    };
    for (const auto& triangle : bucket.triangles)
    {
      if (verts.size() + 3 > MaxChunkWeldedVerts)
      {
        flush();
      }
      for (auto vert : triangle.verts)
      {
        // dmap/output.cpp:250 ShareMapTriVerts stores SetNormal( GetNormal() ), which
        // moves a -1 component's byte from 0 to 1
        engineSetNormal(vert, engineNormal(vert));
        const auto [it, inserted] =
          weld.try_emplace(WeldKey{vert}, std::uint16_t(verts.size()));
        if (inserted)
        {
          // LP2.3 (found by the parity rows): dmap/output.cpp:318-338 writes the welded
          // vertex's xyz, st and GetNormal() (normalised) as text (WriteFloat, :95-105),
          // and RenderWorld_load.cpp ParseModel (:304-312) loads a cleared vertex with
          // what it reads back: SetNormal() moves a -1 component's byte from 1 back to 0
          // and the zero components' 128 to 129 on faces facing -x, -y or -z (a tilt of
          // about a degree), and the normal's fourth byte is 0. The st are read back and
          // recentred in finishChunk (recentreTexCoords), the xyz in sideVert
          auto loaded = engineClearedVert();
          loaded.xyz = vert.xyz;
          loaded.st = vert.st;
          engineSetNormal(loaded, readBack(engineNormal(vert)));
          verts.push_back(loaded);
        }
        indexes.push_back(it->second);
      }
    }
    flush();
    m_nextPart[bucket.key] = part;
  }

  // LP7.4 (C55): whether an adopted island of the entity replaces the face piece
  bool replaced(const PreviewId entity, const AdoptionPieceKey& key) const
  {
    const auto it = m_input.adoptions.find(entity);
    return it != m_input.adoptions.end() && it->second
           && it->second->replaced.contains(key);
  }

  // LP7.4 (C55, P7.11): the adopted islands whose cell is one of the job's, per key
  void addAdoptedIslands()
  {
    for (const auto& [entity, adoption] : m_input.adoptions)
    {
      if (!adoption || !adoption->tree || !adoption->islands)
      {
        continue;
      }
      const auto* context = contextOf(entity);
      if (!context || !context->state.visible)
      {
        continue;
      }
      for (const auto& island : adoption->adopted)
      {
        if (!m_cells.contains(island.cell))
        {
          continue;
        }
        const auto& material = m_materials.get(island.material);
        // LP7.2 (C53): drawn with its entity's shader or skin, as the faces it replaces
        const auto* drawMaterial = drawMaterialOf(material, *context);
        if (!drawMaterial)
        {
          continue;
        }
        // a world island's chunk is of its model's area; a brush entity's of its pivot's
        // area, as every chunk of the entity (P4.9)
        const auto area = context->world ? island.area : context->area;
        auto key =
          ChunkKey{area, context->chunkEntity, drawMaterial->name, island.cell.cell, 0};
        auto [it, inserted] = m_adopted.try_emplace(key);
        if (inserted)
        {
          it->second.key = key;
          it->second.material = &material;
          it->second.drawMaterial = drawMaterial;
          it->second.entity = context;
          it->second.adoption = adoption.get();
        }
        it->second.islands.push_back(&island);
      }
    }
  }

  /**
   * LP7.4 (C55, SC642): the adopted islands of one key as chunks, islands merged while
   * they stay within MaxChunkWeldedVerts, their parts after the editor triangulation's:
   * each island's vertices and triangles as ParseModel loads them (loadProcIsland), then
   * finishChunk without the weld and the recentring (they are the engine's already)
   */
  void finishAdopted(const AdoptedBucket& adopted)
  {
    auto part = m_nextPart[adopted.key];
    auto verts = std::vector<EngineDrawVert>{};
    auto indexes = std::vector<std::uint16_t>{};
    const auto flush = [&]() {
      if (!indexes.empty())
      {
        auto key = adopted.key;
        key.part = part++;
        finishChunk(
          key,
          *adopted.material,
          *adopted.drawMaterial,
          *adopted.entity,
          std::move(verts),
          std::move(indexes),
          false);
      }
      verts.clear();
      indexes.clear();
    };
    const auto& tree = *adopted.adoption->tree;
    const auto& islands = *adopted.adoption->islands;
    for (const auto* island : adopted.islands)
    {
      if (
        island->model >= tree.models.size() || island->model >= islands.models.size()
        || island->surface >= tree.models[island->model].surfaces.size()
        || island->surface >= islands.models[island->model].surfaces.size()
        || island->island
             >= islands.models[island->model].surfaces[island->surface].size())
      {
        warnOnce("an adopted island is not in its tree");
        continue;
      }
      const auto geometry = loadProcIsland(
        tree.models[island->model].surfaces[island->surface],
        islands.models[island->model].surfaces[island->surface][island->island]);
      if (geometry.verts.empty() || geometry.verts.size() > MaxChunkWeldedVerts)
      {
        warnOnce(fmt::format("an adopted island of {} is not drawn", island->material));
        continue;
      }
      if (verts.size() + geometry.verts.size() > MaxChunkWeldedVerts)
      {
        flush();
      }
      const auto offset = verts.size();
      verts.insert(verts.end(), geometry.verts.begin(), geometry.verts.end());
      for (const auto index : geometry.indexes)
      {
        indexes.push_back(std::uint16_t(offset + index));
      }
    }
    flush();
  }

  // LP7.2 (C53, D131): material is the faces' own (the back sides, the tangent path;
  // the st were made with its editor-image size), drawMaterial the key's (the cast class)
  void finishChunk(
    ChunkKey key,
    const FaceMaterial& material,
    const FaceMaterial& drawMaterial,
    const EntityContext& context,
    std::vector<EngineDrawVert> verts,
    std::vector<std::uint16_t> indexes,
    const bool editorTriangles)
  {
    // D59: st read back as ParseModel reads them (R3.9) and recentred per island,
    // through SetTexCoord again (RenderWorld_load.cpp ParseModel); an adopted island's
    // are the .proc's, recentred already
    if (editorTriangles)
    {
      recentreTexCoords(verts, indexes);
    }

    // Model.cpp:1252-1283 FinishSurfaces: a reversed copy for back sides, then each
    // surface through R_CleanupTriangles' steps
    const auto path = tangentPath(material.entry);
    const auto back = material.entry && material.entry->createBackSides
                        ? std::optional{engineReverseTriangles(verts, indexes)}
                        : std::nullopt;
    auto front = engineCleanupTriangles(std::move(verts), std::move(indexes), path);
    if (!front)
    {
      warnOnce(fmt::format("a chunk of {} could not be cleaned up", material.name));
      return;
    }
    auto chunk = Chunk{};
    chunk.key = std::move(key);
    chunk.verts = std::move(front->verts);
    chunk.indexes = std::move(front->indexes);
    if (back)
    {
      if (auto cleaned = engineCleanupTriangles(back->verts, back->indexes, path))
      {
        const auto offset = chunk.verts.size();
        if (offset + cleaned->verts.size() <= 65535)
        {
          chunk.verts.insert(
            chunk.verts.end(), cleaned->verts.begin(), cleaned->verts.end());
          for (const auto index : cleaned->indexes)
          {
            chunk.indexes.push_back(std::uint16_t(offset + index));
          }
        }
        else
        {
          warnOnce(
            fmt::format("the back sides of a chunk of {} do not fit", material.name));
        }
      }
    }
    if (chunk.indexes.empty())
    {
      return;
    }

    orderBySubCell(chunk);
    auto positions = std::vector<EngineVec3>{};
    positions.reserve(chunk.verts.size());
    for (const auto& vert : chunk.verts)
    {
      positions.push_back(vert.xyz);
    }
    chunk.bounds = engineBoundsOfPoints(positions);

    // S36, R5.28: the cast class (LP7.2: the draw material's) and the two versions
    const auto& parms = context.state.parms;
    const auto noShadows = !context.world && parms.noShadows;
    if (!drawMaterial.castsShadow || noShadows)
    {
      chunk.castClass = CastClass::None;
    }
    else if (!drawMaterial.entry)
    {
      chunk.castClass = CastClass::Opaque;
    }
    else
    {
      chunk.castClass = drawMaterial.entry->coverage == CoverageOpaque ? CastClass::Opaque
                        : drawMaterial.entry->coverage == CoveragePerforated
                          ? CastClass::Perforated
                          : CastClass::None;
    }
    // C21: the faces' own editor-image size; LP7.2 (A41): a skin not in the table yet
    chunk.provisional = material.provisional || (!context.world && context.skinPending);
    chunk.adopted = !editorTriangles;
    chunk.sourceMaterial = material.name;

    auto caster = fnv(FnvBasis, &chunk.castClass, sizeof(chunk.castClass));
    const auto flags = std::array<bool, 2>{parms.noShadows, parms.noSelfShadows};
    caster = fnv(caster, flags.data(), sizeof(flags));
    for (const auto index : chunk.indexes)
    {
      caster = fnv(caster, chunk.verts[index].xyz.data(), sizeof(EngineVec3));
      if (chunk.castClass == CastClass::Perforated)
      {
        caster = fnv(caster, chunk.verts[index].st.data(), sizeof(chunk.verts[index].st));
      }
    }
    if (chunk.castClass == CastClass::Perforated)
    {
      caster = fnv(caster, drawMaterial.name.data(), drawMaterial.name.size());
    }
    chunk.casterVersion = caster;

    auto appearance = fnv(FnvBasis, drawMaterial.name.data(), drawMaterial.name.size());
    appearance = fnv(appearance, material.name.data(), material.name.size());
    appearance =
      fnv(appearance, chunk.verts.data(), chunk.verts.size() * sizeof(EngineDrawVert));
    appearance =
      fnv(appearance, chunk.indexes.data(), chunk.indexes.size() * sizeof(std::uint16_t));
    appearance = fnv(appearance, &context.state.hash, sizeof(context.state.hash));
    chunk.appearanceVersion = appearance;

    m_output.chunks.push_back(std::move(chunk));
  }

  // R-C4: the triangles by 256-unit sub-cell of their centroids, at most 32 ranges
  static void orderBySubCell(Chunk& chunk)
  {
    const auto count = chunk.indexes.size() / 3;
    struct Entry
    {
      std::array<int, 3> subCell;
      std::size_t triangle;
    };
    auto entries = std::vector<Entry>{};
    entries.reserve(count);
    for (auto t = std::size_t{0}; t < count; ++t)
    {
      auto subCell = std::array<int, 3>{};
      for (auto k = std::size_t{0}; k < 3; ++k)
      {
        const auto c = (chunk.verts[chunk.indexes[t * 3]].xyz[k]
                        + chunk.verts[chunk.indexes[t * 3 + 1]].xyz[k]
                        + chunk.verts[chunk.indexes[t * 3 + 2]].xyz[k])
                       / 3.0f;
        subCell[k] = int(std::floor(c / SubCellSize));
      }
      entries.push_back(Entry{subCell, t});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      return a.subCell < b.subCell;
    });

    auto sorted = std::vector<std::uint16_t>{};
    sorted.reserve(chunk.indexes.size());
    auto groups = std::vector<std::pair<std::size_t, std::size_t>>{};
    for (auto i = std::size_t{0}; i < entries.size(); ++i)
    {
      if (i == 0 || entries[i].subCell != entries[i - 1].subCell)
      {
        groups.emplace_back(i, 0);
      }
      ++groups.back().second;
      for (auto k = std::size_t{0}; k < 3; ++k)
      {
        sorted.push_back(chunk.indexes[entries[i].triangle * 3 + k]);
      }
    }
    chunk.indexes = std::move(sorted);

    // more sub-cells than ranges: neighbours in the order merge, evenly
    const auto perRange = (groups.size() + MaxChunkRanges - 1) / MaxChunkRanges;
    chunk.ranges.clear();
    for (auto g = std::size_t{0}; g < groups.size(); g += perRange)
    {
      const auto first = groups[g].first;
      auto triangles = std::size_t{0};
      for (auto h = g; h < std::min(groups.size(), g + perRange); ++h)
      {
        triangles += groups[h].second;
      }
      auto range = ChunkRange{};
      range.firstIndex = std::uint32_t(first * 3);
      range.indexCount = std::uint32_t(triangles * 3);
      auto points = std::vector<EngineVec3>{};
      for (auto i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i)
      {
        points.push_back(chunk.verts[chunk.indexes[i]].xyz);
      }
      range.bounds = engineBoundsOfPoints(points);
      chunk.ranges.push_back(range);
    }
  }
};

} // namespace

const std::string& chunkSourceMaterial(const Chunk& chunk)
{
  return chunk.sourceMaterial.empty() ? chunk.key.material : chunk.sourceMaterial;
}

bool ChunkKey::operator==(const ChunkKey& other) const
{
  return std::tie(area, entity, material, cell, part)
         == std::tie(other.area, other.entity, other.material, other.cell, other.part);
}

bool ChunkKey::operator<(const ChunkKey& other) const
{
  return std::tie(entity, cell, area, material, part)
         < std::tie(other.entity, other.cell, other.area, other.material, other.part);
}

bool CellRef::operator==(const CellRef& other) const
{
  return entity == other.entity && cell == other.cell;
}

bool CellRef::operator<(const CellRef& other) const
{
  return std::tie(entity, cell) < std::tie(other.entity, other.cell);
}

std::vector<std::shared_ptr<const SceneNode>> sceneNodesOf(const Snapshot& snapshot)
{
  auto nodes = std::vector<std::shared_ptr<const SceneNode>>{};
  nodes.reserve(snapshot.nodes.size());
  for (const auto& source : snapshot.nodes)
  {
    auto node = std::make_shared<SceneNode>();
    node->id = source.id;
    node->entity = source.entity;
    node->kind = source.kind;
    node->visible = source.visible;
    if (source.kind == SnapshotNodeKind::Brush)
    {
      for (auto f = source.elements.first;
           f < source.elements.first + source.elements.count;
           ++f)
      {
        if (f >= snapshot.faces.size())
        {
          break;
        }
        const auto& sf = snapshot.faces[f];
        auto face = SceneFace{};
        for (auto v = sf.polygon.first; v < sf.polygon.first + sf.polygon.count; ++v)
        {
          if (v < snapshot.vertices.size())
          {
            face.polygon.push_back(snapshot.vertices[v]);
          }
        }
        face.points = sf.points;
        face.material = std::string{snapshot.string(sf.material)};
        face.texture = EngineValveTexture{
          {sf.uAxis.x(), sf.uAxis.y(), sf.uAxis.z()},
          sf.offset.x(),
          {sf.vAxis.x(), sf.vAxis.y(), sf.vAxis.z()},
          sf.offset.y(),
          sf.scale.x(),
          sf.scale.y()};
        node->faces.push_back(std::move(face));
      }
    }
    else if (
      source.kind == SnapshotNodeKind::Patch && source.elements.count > 0
      && source.elements.first < snapshot.patches.size())
    {
      const auto& sp = snapshot.patches[source.elements.first];
      auto patch = ScenePatch{};
      patch.material = std::string{snapshot.string(sp.material)};
      patch.rows = sp.rows;
      patch.columns = sp.columns;
      for (auto c = sp.controlPoints.first;
           c < sp.controlPoints.first + sp.controlPoints.count
           && c < snapshot.controlPoints.size();
           ++c)
      {
        patch.controlPoints.push_back(snapshot.controlPoints[c]);
      }
      patch.explicitSubdivisions = sp.explicitSubdivisions;
      patch.horizontalSubdivisions = sp.horizontalSubdivisions;
      patch.verticalSubdivisions = sp.verticalSubdivisions;
      node->patch = std::move(patch);
    }
    nodes.push_back(std::move(node));
  }
  return nodes;
}

std::vector<std::shared_ptr<const SceneEntity>> sceneEntitiesOf(const Snapshot& snapshot)
{
  auto entities = std::vector<std::shared_ptr<const SceneEntity>>{};
  entities.reserve(snapshot.entities.size());
  for (const auto& source : snapshot.entities)
  {
    auto entity = std::make_shared<SceneEntity>();
    entity->id = source.id;
    entity->classname = std::string{snapshot.string(source.classname)};
    for (auto k = source.keys.first;
         k < source.keys.first + source.keys.count && k < snapshot.keyValues.size();
         ++k)
    {
      entity->keys.emplace_back(
        std::string{snapshot.string(snapshot.keyValues[k].key)},
        std::string{snapshot.string(snapshot.keyValues[k].value)});
    }
    entities.push_back(std::move(entity));
  }
  return entities;
}

CellCoord cellOfNode(const SceneNode& node)
{
  auto sum = vm::vec3d{0, 0, 0};
  auto count = std::size_t{0};
  for (const auto& face : node.faces)
  {
    for (const auto& p : face.polygon)
    {
      sum = sum + p;
      ++count;
    }
  }
  if (node.patch)
  {
    for (const auto& p : node.patch->controlPoints)
    {
      sum = sum + vm::vec3d{p[0], p[1], p[2]};
      ++count;
    }
  }
  if (count == 0)
  {
    return {0, 0, 0};
  }
  const auto c = sum / double(count);
  return {
    int(std::floor(c.x() / CellSize)),
    int(std::floor(c.y() / CellSize)),
    int(std::floor(c.z() / CellSize))};
}

EngineTangentPath tangentPath(const MaterialEntry* material)
{
  // C25, tr_trisurf.cpp:1831-1839: unsmoothedTangents first; R3.11: mikktspace takes the
  // engine's failure path, since MikkTSpace is not copied (Q18, M-ID-DIFF-04)
  if (material && material->unsmoothedTangents)
  {
    return EngineTangentPath::Unsmoothed;
  }
  return material && material->mikktspace ? EngineTangentPath::Mikktspace
                                          : EngineTangentPath::WithoutNormals;
}

std::string engineSideMaterial(const std::string& tbName)
{
  // ParseValve220: always "textures/" + name
  return "textures/" + tbName;
}

std::string enginePatchMaterial(const std::string& tbName)
{
  // version 220: the prefix only when it is missing
  return startsWithNoCase(tbName, "textures/") ? tbName : "textures/" + tbName;
}

ChunkJobOutput buildChunks(const ChunkJobInput& input, const CpuJobContext* context)
{
  return Builder{input, context}.build();
}

} // namespace tb::lit
