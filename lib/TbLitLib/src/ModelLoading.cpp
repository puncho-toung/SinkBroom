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

#include "ModelValidators.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
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

ModelDispatch modelDispatch(const std::string_view canonical)
{
  auto dispatch = ModelDispatch{};
  if (canonical.empty())
  {
    dispatch.path = ModelPath::None;
    return dispatch;
  }
  // ModelManager.cpp:270-291: the built-ins, found by the name hash before the dispatch
  if (canonical == "_default")
  {
    dispatch.path = ModelPath::DefaultModel;
    dispatch.reason = "the engine's default model";
    return dispatch;
  }
  if (canonical == "_beam" || canonical == "_sprite")
  {
    dispatch.path = ModelPath::Default;
    dispatch.reason = fmt::format(
      "{} is the engine's own model class: the preview draws the default cube (K47)",
      canonical);
    return dispatch;
  }

  dispatch.extension = engineFileExtension(canonical);
  const auto& ext = dispatch.extension;
  if (ext == "obj")
  {
    dispatch.path = ModelPath::Source;
  }
  else if (ext == "ase" || ext == "lwo")
  {
    dispatch.path = ModelPath::PendingLoader;
    dispatch.reason = fmt::format(
      "the .{} loader is not in the preview yet (LP7.1b): the default cube stands in",
      ext);
  }
  else if (ext == "ma")
  {
    dispatch.path = ModelPath::MayaBinaryOnly;
    dispatch.reason =
      "a Maya .ma model is drawn from the engine's .bma only: the default cube (K46)";
  }
  else if (ext == "glb" || ext == "gltf")
  {
    dispatch.path = ModelPath::GltfBinaryOnly;
    dispatch.reason =
      "a glTF model is drawn from the engine's .bglb only: the default cube (K40)";
  }
  else if (ext == "md5mesh")
  {
    dispatch.path = ModelPath::Skinned;
    dispatch.reason =
      "an animated .md5mesh model draws nothing on a static entity, as in the engine "
      "(K41)";
  }
  else if (ext == "md3" || ext == "prt" || ext == "liquid")
  {
    dispatch.path = ModelPath::Default;
    dispatch.reason = fmt::format(
      "a .{} model is the engine's own model class: the preview draws the default cube "
      "(K47)",
      ext);
  }
  else if (ext == "flt")
  {
    // dispatched to idRenderModelStatic (:417-422), but InitFromFile has no flt loader
    dispatch.path = ModelPath::Default;
    dispatch.reason =
      "the engine has no .flt loader (Model.cpp:350-361): the default cube, as in the "
      "engine";
  }
  else
  {
    dispatch.path = ModelPath::Default;
    dispatch.reason = ext.empty()
                        ? std::string{"a model name without an extension: the default "
                                      "cube, as in the engine"}
                        : fmt::format(
                            "unknown model type '.{}' (ModelManager.cpp:512): the "
                            "default cube, as in the engine",
                            ext);
  }
  return dispatch;
}

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

ModelRecord modelRecordFromEngine(
  std::string canonical,
  const EngineModel& model,
  const ModelRecord::State state,
  const ModelRecord::SourceKind source,
  const std::int64_t sourceTime,
  std::string reason)
{
  auto record = ModelRecord{};
  record.name = std::move(canonical);
  record.state = state;
  record.source = source;
  record.surfaces.reserve(model.surfaces.size());
  for (const auto& finished : model.surfaces)
  {
    auto surface = ModelSurface{};
    surface.id = finished.id;
    surface.material = finished.material;
    surface.verts = finished.verts;
    surface.indexes = finished.indexes;
    surface.generateNormals = finished.generateNormals;
    surface.tangentsCalculated = finished.tangentsCalculated;
    surface.bounds = finished.bounds;
    surface.mirroredVerts = finished.mirroredVerts;
    surface.dupVerts = finished.dupVerts;
    record.surfaces.push_back(std::move(surface));
  }
  record.bounds = model.bounds;
  record.hasDrawingSurfaces = model.hasDrawingSurfaces;
  record.hasInteractingSurfaces = model.hasInteractingSurfaces;
  record.hasShadowCastingSurfaces = model.hasShadowCastingSurfaces;
  record.sourceTime = sourceTime;
  record.reason = std::move(reason);
  return record;
}

EngineModel finishModel(const ModelFinishInput& input)
{
  if (input.obj)
  {
    return engineFinishObjModel(
      *input.obj, input.materials, input.name, input.useMikktspace);
  }
  return engineDefaultModel(input.name, input.materials.defaultMaterial);
}

std::optional<ModelFinishInput> defaultModelInput(
  MaterialDecls& decls, const std::string_view canonical, const std::int64_t sourceTime)
{
  auto fallback = decls.defaultModelMaterial();
  if (!fallback)
  {
    return std::nullopt;
  }
  auto input = ModelFinishInput{};
  input.name = std::string{canonical};
  input.materials.defaultMaterial = std::move(*fallback.value);
  input.sourceTime = sourceTime;
  return input;
}

ObjSourceRead readObjSource(
  MaterialDecls& decls,
  const std::string_view canonical,
  const std::string& bytes,
  const std::int64_t sourceTime)
{
  auto read = ObjSourceRead{};
  const auto refuse = [&](std::string reason) {
    read.reason = std::move(reason);
    read.input = defaultModelInput(decls, canonical, sourceTime);
    return std::move(read);
  };

  // D136: the copies never see a file the validator refuses
  if (const auto problem = objFileProblem(bytes))
  {
    return refuse(fmt::format("{} cannot be read: {}", canonical, *problem));
  }

  auto parsed = decls.parseObjModel(std::string{canonical}, bytes, sourceTime);
  if (!parsed.error.empty())
  {
    return refuse(fmt::format("{}: {}", canonical, parsed.error));
  }
  if (!parsed.model)
  {
    // Model.cpp:356-361: "Couldn't load model", MakeDefaultModel
    return refuse(fmt::format("{} could not be loaded", canonical));
  }

  auto input = ModelFinishInput{};
  input.name = std::string{canonical};
  input.obj = parsed.model;
  input.sourceTime = sourceTime;
  // Model.cpp:366-375 on the time the caller hands in (A4)
  input.useMikktspace = engineUseMikktspace(sourceTime);
  for (const auto& name : parsed.model->materialNames())
  {
    if (std::ranges::any_of(
          input.materials.names, [&](const auto& known) { return known.first == name; }))
    {
      continue;
    }
    auto material = decls.modelMaterial(name);
    if (!material)
    {
      return refuse(
        fmt::format("{}: its material '{}': {}", canonical, name, material.error));
    }
    // one entry per decl, as FindMaterial returns one pointer per decl
    const auto same = std::ranges::find_if(
      input.materials.materials,
      [&](const auto& known) { return known.index == material.value->index; });
    const auto index = std::size_t(same - input.materials.materials.begin());
    if (same == input.materials.materials.end())
    {
      input.materials.materials.push_back(std::move(*material.value));
    }
    input.materials.names.emplace_back(name, index);
  }
  auto fallback = decls.defaultModelMaterial();
  if (!fallback)
  {
    return refuse(fmt::format("{}: no default material: {}", canonical, fallback.error));
  }
  input.materials.defaultMaterial = std::move(*fallback.value);
  read.input = std::move(input);
  return read;
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
