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

#include "lit/EngineGeometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tb::lit
{

/**
 * LP7.0 (C51, D126): the name a model is kept under, as
 * idRenderModelManagerLocal::GetModel makes it (renderer/ModelManager.cpp:313-329): an
 * empty name is no model (GetModel returns NULL; the result is empty), any other the
 * whole name with its letters lowered by idStr::ToLower. ToLower's CharIsUpper also takes
 * 0xC0-0xDF, which a signed char (MSVC, GCC and Clang on x86) never reaches, so only
 * 'A'-'Z' change; the idStrStatic< MAX_OSPATH > grows on the heap past its buffer, so a
 * long name is never cut. The extension stays in the name. Pure; any thread.
 */
std::string canonicalModelName(std::string_view name);

/**
 * LP7.0 (C51): one surface of a model record, after r_mergeModelSurfaces 1 (one surface
 * per material unless the material IsDiscrete, Model.cpp:42-45): its material's name (the
 * surface's own, before C53's skin and shader remaps), the engine's draw vertices and
 * 16-bit indexes, the two flags FinishSurfaces and the binary keep, and the surface's
 * bounds. Plain data.
 */
struct ModelSurface
{
  std::string material;
  std::vector<EngineDrawVert> verts;
  std::vector<std::uint16_t> indexes;
  bool generateNormals = false;
  bool tangentsCalculated = false;
  std::array<EngineVec3, 2> bounds = {};
};

/**
 * LP7.0 (C51, D125, D126, D127): what the material service's model table holds for a
 * canonical model name (MaterialTable::models), immutable once published and shared by
 * every holder. LP7.0 defines the record and the table only: nothing requests or loads a
 * model yet (LP7.1a), so the table has no entries. The states are the preview's own (the
 * engine loads synchronously and has none):
 * - Pending: requested, read or finishing (C52: its entities draw nothing and keep the
 *   frame unconverged, D124's rule);
 * - Ready: the surfaces of the binary (no FinishSurfaces) or of the source path;
 * - Missing: no file: the default cube's surfaces, flagged (reason);
 * - Default: an unknown type, a validator or loader failure, a modelDef name on an
 *   idEntity spawnclass: the default cube, flagged (reason).
 */
struct ModelRecord
{
  enum class State
  {
    Pending,
    Ready,
    Missing,
    Default,
  };

  // where the surfaces came from (C51's lookup order): the engine's
  // generated/rendermodels/<name>.b<ext> binary, the source file through the copied
  // loaders and FinishSurfaces, or the default cube; None while Pending
  enum class SourceKind
  {
    None,
    Binary,
    Source,
    Default,
  };

  // the canonical name (canonicalModelName), the table's key
  std::string name;
  State state = State::Pending;
  SourceKind source = SourceKind::None;
  // moves each time the name's record is published again (a changed file read again:
  // MaterialTable::modelChanges); with a surface index, D129's placement key
  std::uint64_t version = 0;
  std::vector<ModelSurface> surfaces;
  // the model's bounds (FinishSurfaces', the deforming surfaces grown,
  // Model.cpp:1346-1360; a binary's stored ones)
  std::array<EngineVec3, 2> bounds = {};
  // idRenderModelStatic's three has-surfaces flags
  bool hasDrawingSurfaces = false;
  bool hasInteractingSurfaces = false;
  bool hasShadowCastingSurfaces = false;
  // A4: the source file's time as the file source reports it, never the checkout's
  // (-1 FILE_NOT_FOUND_TIMESTAMP, 0 a container entry; P7.4); the binary's stored time
  // (LoadBinaryModel's timeStamp, Model.cpp:400), 0 without one
  std::int64_t sourceTime = -1;
  std::int64_t binaryTime = 0;
  // P7.19: a .bglb binary's GLMB tail, its model_state and animation count (an animated
  // one is K41's); 0 for every other kind
  int glbModelState = 0;
  int glbAnimations = 0;
  // Missing and Default: why, for the entity's once-notice (C52)
  std::string reason;
};

/**
 * LP7.0 (C53): a skin decl as the registry's idDeclSkin holds it (framework/DeclSkin.h:
 * its mappings, each a from and a to material; from empty is "*", any unmatched shader),
 * published by name in MaterialTable::skins as the entityDefs are. LP7.0 publishes none:
 * the facade has no skin accessor yet (lit/MaterialDecls.h; idDeclSkin's mappings are
 * private, RemapShaderBySkin its only lookup), LP7.2's to add.
 */
struct SkinRecord
{
  struct Mapping
  {
    std::string from;
    std::string to;
  };

  // the decl's canonical name
  std::string name;
  std::vector<Mapping> mappings;
};

// what a skin query found for a name: a record, or why there is none (as EntityDefSlot)
struct SkinSlot
{
  std::shared_ptr<const SkinRecord> entry;
  std::string error;
};

} // namespace tb::lit
