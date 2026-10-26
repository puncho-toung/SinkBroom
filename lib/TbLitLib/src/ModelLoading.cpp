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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.cpp, renderer/Model_gltf.cpp,
// renderer/ModelManager.cpp and framework/FileSystem.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ModelLoading.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <span>
#include <utility>

namespace tb::lit
{
namespace
{

// FileSystem.h:53
constexpr auto FileNotFoundTimestamp = std::int64_t{-1};

std::span<const std::byte> bytesOf(const std::string& text)
{
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

template <typename T>
std::size_t bytesOfVector(const std::vector<T>& v)
{
  return v.size() * sizeof(T);
}

} // namespace

bool binaryModelTimeAccepted(const std::int64_t sourceTime, const std::int64_t storedTime)
{
  // Model.cpp:403: !InProductionMode() && source != FILE_NOT_FOUND_TIMESTAMP && source !=
  // 0
  // && source != stored rejects; com_productionMode 0 (FileSystem.cpp:841-845)
  constexpr auto inProductionMode = false;
  return !(
    !inProductionMode && sourceTime != FileNotFoundTimestamp && sourceTime != 0
    && sourceTime != storedTime);
}

ModelBinaryLookup lookUpModelBinary(
  DeclSource& source, const std::string_view canonical, const std::int64_t sourceTime)
{
  auto lookup = ModelBinaryLookup{};
  lookup.path = engineGeneratedModelPath(canonical);
  const auto extension = engineFileExtension(canonical);
  if (extension == "md5mesh")
  {
    // P7.19: idRenderModelMD5 writes MD5B (another format); nothing of it is drawn (K41)
    lookup.result = ModelBinaryLookup::Result::Md5b;
    lookup.reason = "an .md5mesh's binary is MD5B, not a BRM stream (K41)";
    return lookup;
  }

  const auto bytes = source.readGameFile(lookup.path, &lookup.probe);
  if (!bytes)
  {
    lookup.result = ModelBinaryLookup::Result::None;
    lookup.reason = "no binary " + lookup.path;
    return lookup;
  }

  auto offset = std::size_t{0};
  readBinaryModel(bytesOf(*bytes), offset) | kdl::transform([&](BprocModel model) {
    if (!binaryModelTimeAccepted(sourceTime, model.timeStamp))
    {
      lookup.result = ModelBinaryLookup::Result::Stale;
      lookup.reason = fmt::format(
        "{} is stale: it was written for a source of time {}, the source's is {}",
        lookup.path,
        model.timeStamp,
        sourceTime);
      return;
    }
    lookup.result = ModelBinaryLookup::Result::Accepted;
    lookup.model = std::move(model);
  }) | kdl::transform_error([&](auto e) {
    lookup.result = ModelBinaryLookup::Result::Unreadable;
    lookup.reason = lookup.path + ": " + e.msg;
  });

  if (
    lookup.result == ModelBinaryLookup::Result::Accepted
    && (extension == "glb" || extension == "gltf"))
  {
    // Model_gltf.cpp:756-787: the GLMB tail after the stream; :773's modelDef time test
    // with declSourceTimeStamp 0 (the preview has no import options) accepts
    readGlmbTail(bytesOf(*bytes), offset)
      | kdl::transform([&](GlmbTail tail) { lookup.glmb = tail; })
      | kdl::transform_error([&](auto e) {
          lookup.result = ModelBinaryLookup::Result::Unreadable;
          lookup.model.reset();
          lookup.reason = lookup.path + ": " + e.msg;
        });
  }
  return lookup;
}

ModelRecord modelRecordFromBinary(
  std::string canonical,
  const BprocModel& model,
  const std::optional<GlmbTail>& glmb,
  const std::int64_t sourceTime)
{
  auto record = ModelRecord{};
  record.name = std::move(canonical);
  record.state = ModelRecord::State::Ready;
  record.source = ModelRecord::SourceKind::Binary;
  record.surfaces.reserve(model.surfaces.size());
  for (const auto& stored : model.surfaces)
  {
    auto surface = ModelSurface{};
    surface.id = stored.id;
    surface.material = stored.material;
    surface.verts = stored.verts;
    surface.indexes = stored.indexes;
    surface.generateNormals = stored.generateNormals;
    surface.tangentsCalculated = stored.tangentsCalculated;
    surface.bounds = stored.bounds;
    surface.mirroredVerts = stored.mirroredVerts;
    surface.dupVerts = stored.dupVerts;
    record.surfaces.push_back(std::move(surface));
  }
  record.bounds = model.bounds;
  record.hasDrawingSurfaces = model.hasDrawingSurfaces;
  record.hasInteractingSurfaces = model.hasInteractingSurfaces;
  record.hasShadowCastingSurfaces = model.hasShadowCastingSurfaces;
  record.sourceTime = sourceTime;
  record.binaryTime = model.timeStamp;
  if (glmb)
  {
    record.glbModelState = glmb->modelState;
    record.glbAnimations = glmb->animationCount;
  }
  return record;
}

std::size_t modelRecordBytes(const ModelRecord& record)
{
  auto bytes = sizeof(ModelRecord);
  for (const auto& surface : record.surfaces)
  {
    bytes += sizeof(ModelSurface) + bytesOfVector(surface.verts)
             + bytesOfVector(surface.indexes) + bytesOfVector(surface.mirroredVerts)
             + bytesOfVector(surface.dupVerts) + surface.material.size();
  }
  return bytes;
}

} // namespace tb::lit
