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

#include "lit/BprocModel.h"
#include "lit/EngineModels.h"
#include "lit/MaterialDecls.h"
#include "lit/ModelRecord.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{

/**
 * LP7.1a (C51, D125): the engine's binary time test (Model.cpp:400-406, LoadBinaryModel):
 * a binary whose stored time is not the source's is rejected, unless the source's time is
 * FILE_NOT_FOUND_TIMESTAMP (-1: no source) or 0 (a container entry, A4), or the file
 * system is in production mode, which the preview never is (com_productionMode 0,
 * FileSystem.cpp:841-845). Pure.
 */
bool binaryModelTimeAccepted(std::int64_t sourceTime, std::int64_t storedTime);

/**
 * LP7.1a (C51, D125, SC601, SC602, P7.19): what the lookup of a model's engine binary
 * found: generated/rendermodels/<canonical> with the extension b<ext>
 * (engineGeneratedModelPath, ModelManager.cpp:446-450), read through the C8 search
 * (loose, .pk4, .resources: DeclSource::readGameFile), its BRM 108/110 stream read by
 * readBinaryModel and the time test applied; a .bglb's GLMB tail read after the stream
 * (its modelDef time tested as :773 tests it, with the preview's declSourceTimeStamp 0:
 * no import options, so never refused). An .md5mesh's binary (.bmd5mesh) is MD5B, not a
 * BRM stream: it is never read (Md5b).
 */
struct ModelBinaryLookup
{
  enum class Result
  {
    // no binary was found
    None,
    // read and accepted: the record is made from it
    Accepted,
    // read, refused by the time test: the source is read instead
    Stale,
    // found, but its stream cannot be read
    Unreadable,
    // an .md5mesh's .bmd5mesh: not read (K41)
    Md5b,
  };

  Result result = Result::None;
  // the binary's game-relative path and where the search found it
  std::string path;
  GameFileProbe probe;
  std::optional<BprocModel> model;
  std::optional<GlmbTail> glmb;
  // why it was not accepted
  std::string reason;
};

/**
 * Looks the binary of canonical up and tests it against sourceTime (the source's time as
 * the file source's probe reports it, A4: -1 none, 0 a container entry). Parse thread
 * (the source is the parse thread's, C20).
 */
ModelBinaryLookup lookUpModelBinary(
  DeclSource& source, std::string_view canonical, std::int64_t sourceTime);

/**
 * The record of an accepted binary (D125, SC602): the stored vertices, indexes,
 * tangents, colours, mirrored and dup vertices, surface and model bounds and the
 * has-surfaces flags as they are, with no FinishSurfaces; state Ready, source Binary,
 * the source's time and the stored one, a .bglb's model_state and animation count.
 * Pure.
 */
ModelRecord modelRecordFromBinary(
  std::string canonical,
  const BprocModel& model,
  const std::optional<GlmbTail>& glmb,
  std::int64_t sourceTime);

// the bytes a record holds (the stats' model bytes): its vertices, indexes and derived
// arrays
std::size_t modelRecordBytes(const ModelRecord& record);

} // namespace tb::lit
