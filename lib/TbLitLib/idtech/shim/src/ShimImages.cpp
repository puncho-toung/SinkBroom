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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_load.cpp and
// renderer/BinaryImage.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c.1: the image cache's name (C8): ActuallyLoadImage up to the generated name, behind
// the facade's MaterialDecls::imageCacheName.

// clang-format off: the std-only facade, then precompiled.h, then the registry
#include "lit/MaterialDecls.h"
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

namespace tb::lit
{
namespace
{

// Image.h:34-43 textureType_t, which the copied Image.h ranges leave out
constexpr int DTT_2D = 1;
constexpr int DTT_CUBIC = 2;
constexpr int DTT_2D_ARRAY = 3;

// Image_load.cpp:525-537 idImage::GetGeneratedName (an idStr for the engine's
// idStrStatic< 64 >, which holds every extension the loaders know)
void getGeneratedName(
  idStr& _name, const textureUsage_t& _usage, const cubeFiles_t& _cube)
{
  idStr extension;

  _name.ExtractFileExtension(extension);
  _name.StripFileExtension();

  _name += va("#__%02d%02d", (int)_usage, (int)_cube);
  if (extension.Length() > 0)
  {
    _name.SetFileExtension(extension);
  }
}

// BinaryImage.cpp:1144-1151 idBinaryImage::GetGeneratedFileName
void getGeneratedFileName(idStr& gfn, const char* name)
{
  gfn.Format("generated/images/%s.bimage", name);
  gfn.Replace("(", "/");
  gfn.Replace(",", "/");
  gfn.Replace(")", "");
  gfn.Replace(" ", "");
}

// Image_load.cpp:629-671, the BFHACK names: the first rule whose name the generated name
// holds (Find without case) renames it (Replace, with case); false when none applies
bool bfhackName(idStr& generatedName)
{
  struct Rule
  {
    const char* find;
    const char* from;
    const char* to;
  };
  constexpr Rule rules[] = {
    {"guis/assets/white#__0000", "white#__0000", "white#__0200"},
    {"guis/assets/white#__0100", "white#__0100", "white#__0200"},
    {"textures/black#__0100", "black#__0100", "black#__0200"},
    {"textures/decals/bulletglass1_d#__0100",
     "bulletglass1_d#__0100",
     "bulletglass1_d#__0200"},
    {"models/monsters/skeleton/skeleton01_d#__1000",
     "skeleton01_d#__1000",
     "skeleton01_d#__0100"},
  };
  for (const auto& rule : rules)
  {
    if (generatedName.Find(rule.find, false) >= 0)
    {
      generatedName.Replace(rule.from, rule.to);
      return true;
    }
  }
  return false;
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
  return "an engine error was reported";
}

} // namespace

DeclResult<ImageCacheName> MaterialDecls::imageCacheName(const MaterialImageKey& key)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // the copied parser hands out names shorter than MAX_IMAGE_NAME; a longer one would be
  // cut by R_LoadImageProgram's bounded buffer, so it is refused
  if (key.name.empty() || key.name.size() >= std::size_t(MAX_IMAGE_NAME))
  {
    return {std::nullopt, "image name too long or empty"};
  }

  auto result = ImageCacheName{};
  result.imageName = key.name;
  result.usage = key.usage;
  result.cube = key.cube;
  // this is the ONLY place generatorFunction will ever be called: an intrinsic and the
  // default image are made by the engine, without a cache
  if (key.intrinsic || key.isDefault)
  {
    result.generated = true;
    return {result, {}};
  }

  const auto errorsBefore = m_registry->errorCount();

  // Image_load.cpp:571-617, com_productionMode 0; sourceFileTime starts as the image
  // constructor sets it (Image_NVRHI.cpp:133)
  const auto name = idStr{key.name.c_str()};
  const auto cubeFiles = cubeFiles_t(key.cube);
  auto usage = textureUsage_t(key.usage);
  ID_TIME_T sourceFileTime = FILE_NOT_FOUND_TIMESTAMP;
  int textureType;

  // RB: added CF_2D_ARRAY
  if (cubeFiles == CF_2D_ARRAY)
  {
    textureType = DTT_2D_ARRAY;
  }
  else if (
    cubeFiles == CF_NATIVE || cubeFiles == CF_NATIVE_HDR || cubeFiles == CF_CAMERA
    || cubeFiles == CF_QUAKE1 || cubeFiles == CF_SINGLE || cubeFiles == CF_PANORAMA)
  {
    textureType = DTT_CUBIC;
    R_LoadCubeImages(
      name.c_str(), cubeFiles, NULL, NULL, &sourceFileTime, key.cubeMapSize);
  }
  else
  {
    textureType = DTT_2D;
    R_LoadImageProgram(name.c_str(), NULL, NULL, NULL, &sourceFileTime, &usage);
  }

  // RB: PBR HACK - RMAO maps should end with _rmao insted of _s
  auto imgName = name;
  if (usage == TD_SPECULAR_PBR_RMAO)
  {
    idStr baseName = imgName;
    baseName.StripFileExtension();

    if (baseName.StripTrailingOnce("_s"))
    {
      imgName = baseName + "_rmao";
    }
  }
  // RB end

  idStr generatedName = imgName;
  getGeneratedName(generatedName, usage, cubeFiles);
  idStr file;
  getGeneratedFileName(file, generatedName.c_str());

  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastErrorOf(m_registry->messages())};
  }

  result.imageName = imgName.c_str();
  result.usage = int(usage);
  result.textureType = textureType;
  result.sourceTime = std::int64_t(sourceFileTime);
  result.generatedName = generatedName.c_str();
  result.file = file.c_str();
  if (bfhackName(generatedName))
  {
    idStr bfhackFile;
    getGeneratedFileName(bfhackFile, generatedName.c_str());
    result.bfhackFile = bfhackFile.c_str();
  }
  return {result, {}};
}

} // namespace tb::lit
