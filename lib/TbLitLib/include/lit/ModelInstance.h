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

#include "lit/ChunkBuilder.h"
#include "lit/ModelRecord.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tb::lit
{

/**
 * LP7.2 (C52, D129, P7.7): what a shared model surface is placed by in the arenas: the
 * record's canonical name, the surface's index in it and the record's version. The
 * record is immutable per version (MaterialTable::models publishes a new version when
 * its file is read again), so a key's geometry never changes; a new version is a new
 * key.
 */
struct SharedGeometryKey
{
  std::string record;
  std::size_t surface = 0;
  std::uint64_t version = 0;

  auto operator<=>(const SharedGeometryKey&) const = default;
};

/**
 * LP7.2 (C52, D129): one surface of a model record, shared by every instance of its key
 * (the engine's vertexCache shares the model's tris among its entities, SC621):
 * immutable, by shared_ptr. The record holds its EngineDrawVerts (model-local) and
 * 16-bit indexes, which the arenas upload once per key; positions is one copy of the
 * vertices' xyz for LightTriangles (its stamp's geometry, shared by every instance and
 * light that asks).
 */
struct SharedSurface
{
  SharedGeometryKey key;
  std::shared_ptr<const ModelRecord> record;
  std::vector<EngineVec3> positions;

  const ModelSurface& surface() const;
  std::size_t vertexCount() const;
  std::size_t indexCount() const;
};

// the shared surface of a record's surface: its key, the record, its positions; null for
// an index past the record's surfaces. Pure
std::shared_ptr<const SharedSurface> makeSharedSurface(
  std::shared_ptr<const ModelRecord> record, std::size_t surface);

/**
 * LP7.2 (C52, D129, P7.7): one surface of a model entity in the scene, apart from Chunk
 * (SceneGeometry::instances, by its chunk key): the entity, the surface's index, its own
 * material (the record's) and its draw material (C53: the entity's shader or skin), the
 * model matrix (A7's axis and the pivot, the entity state's), the shaderParms (D52), the
 * surface's bounds in model space (the record's, deforms grown, Model.cpp:1346-1360) and
 * through the matrix (worldBounds), the entity's areas (C52: references from the model's
 * bounds, never clipped), the caster and appearance versions, and the placement key of
 * its shared geometry.
 *
 * header is the instance as a chunk without vertices (its key, the surface bounds, the
 * cast class, the versions, provisional, sourceMaterial the own material): the frontend's
 * surfaces point at it (LitSurface::chunk) beside the instance (LitSurface::instance), so
 * the culls, the stage sort (K37) and the draws read an instance as they read a chunk;
 * the draw's placement is the shared one (GeometryArenas::placement of the key). The
 * chunk key is (area: the pivot's pointInArea as a key only, entity, draw material, cell:
 * the origin's 1024-unit cell, part: the surface index).
 */
struct ModelInstance
{
  Chunk header;
  PreviewId entity = NoPreviewId;
  std::size_t surface = 0;
  std::shared_ptr<const SharedSurface> geometry;
  EngineVec3 modelOrigin = {0, 0, 0};
  EngineMat3 axis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<float, MaterialDecls::ShaderParmCount> shaderParms = {};
  EngineBounds worldBounds = {};
  std::vector<int> areas;

  const ChunkKey& key() const { return header.key; }
  const SharedGeometryKey& placement() const { return geometry->key; }
};

} // namespace tb::lit
