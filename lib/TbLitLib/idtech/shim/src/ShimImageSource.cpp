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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c.2: an image's source through the copied loaders (S14, D51), behind the facade's
// MaterialDecls::loadImagePixels. Parse thread only (C20).

// clang-format off: the std-only facade, then precompiled.h, then the registry
#include "lit/MaterialDecls.h"
#include "precompiled.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
// clang-format on

namespace tb::lit
{
namespace
{

// width × height × 4 bytes of a loader's picture, which the copied code allocated with
// R_StaticAlloc (Mem_Alloc), appended and freed as ActuallyLoadImage frees it (Mem_Free)
void takePicture(
  byte* pic, const int width, const int height, std::vector<std::uint8_t>& out)
{
  const auto size = std::size_t(width) * std::size_t(height) * 4;
  out.insert(out.end(), pic, pic + size);
  Mem_Free(pic);
}

/**
 * M-IMG-GUARD-02: the copied R_LoadCubeImages turns a CF_SINGLE image with a cube size
 * into six sides with R_GenerateCubeMapSideFromSingleImage, which copies a cross of 4 × 3
 * sides of that size out of the picture without looking at the picture's size. The
 * picture is loaded first, as R_LoadCubeImages loads it (R_LoadImageProgram, no usage),
 * and a picture smaller than the cross is refused: an empty text means it fits (or there
 * is no picture, which R_LoadCubeImages reports itself).
 */
std::string singleImageCubeFits(
  const std::string& imageName, const cubeFiles_t cubeFiles, const int cubeMapSize)
{
  if (cubeFiles != CF_SINGLE || cubeMapSize == 0)
  {
    return {};
  }
  byte* pic = nullptr;
  int width = 0;
  int height = 0;
  ID_TIME_T time = 0;
  R_LoadImageProgram(imageName.c_str(), &pic, &width, &height, &time);
  if (!pic)
  {
    return {};
  }
  R_StaticFree(pic);
  if (
    cubeMapSize < 0 || std::int64_t(width) < 4 * std::int64_t(cubeMapSize)
    || std::int64_t(height) < 3 * std::int64_t(cubeMapSize))
  {
    return "the single-image cube " + imageName + " is " + std::to_string(width) + " x "
           + std::to_string(height) + ", smaller than the 4 x 3 sides of "
           + std::to_string(cubeMapSize)
           + " that the engine copies out of it; it is the default image";
  }
  return {};
}

} // namespace

SourceLoad MaterialDecls::loadImagePixels(
  const ImageCacheName& name, const int cubeMapSize, const bool quiet)
{
  auto load = SourceLoad{};
  if (!m_registry)
  {
    load.error = m_constructionError;
    return load;
  }
  if (name.generated)
  {
    load.error = "the engine generates " + name.imageName + "; it has no source";
    return load;
  }
  // R_LoadCubeImages copies the name into a MAX_IMAGE_NAME buffer
  if (name.imageName.empty() || name.imageName.size() >= std::size_t(MAX_IMAGE_NAME))
  {
    load.error = "image name too long or empty";
    return load;
  }

  auto& files = m_registry->files();
  const auto messagesBefore = m_registry->messages().size();
  files.beginRecording();
  files.beginImageReads();

  // Image_load.cpp:716-865, the source of an image whose cache the engine would not use;
  // GetName() is the name after the PBR rename, usage the time call's
  const auto cubeFiles = cubeFiles_t(name.cube);
  auto usage = textureUsage_t(name.usage);
  ID_TIME_T sourceFileTime = ID_TIME_T(name.sourceTime);
  auto pixels = SourcePixels{};
  if (
    cubeFiles == CF_NATIVE || cubeFiles == CF_CAMERA || cubeFiles == CF_QUAKE1
    || cubeFiles == CF_SINGLE || cubeFiles == CF_PANORAMA)
  {
    int size = 0;
    byte* pics[6] = {};
    const auto fits = singleImageCubeFits(name.imageName, cubeFiles, cubeMapSize);
    const auto loaded =
      fits.empty()
      && R_LoadCubeImages(
        name.imageName.c_str(), cubeFiles, pics, &size, &sourceFileTime, cubeMapSize);
    if (!fits.empty())
    {
      shim::reportWarning(fits);
      load.error = fits;
    }
    else if (!loaded || size == 0)
    {
      // a cube that loaded six sides of size 0 keeps them in the engine; they are freed
      for (auto* pic : pics)
      {
        if (pic)
        {
          Mem_Free(pic);
        }
      }
      idLib::Warning("Couldn't load cube image: %s", name.imageName.c_str());
      load.error = "Couldn't load cube image: " + name.imageName;
    }
    else
    {
      pixels.width = size;
      pixels.height = size;
      pixels.faces = 6;
      for (auto* pic : pics)
      {
        takePicture(pic, size, size, pixels.pixels);
      }
    }
  }
  else
  {
    byte* pic = nullptr;
    int width = 0;
    int height = 0;

    // load the full specification, and perform any image program calculations
    R_LoadImageProgram(
      name.imageName.c_str(), &pic, &width, &height, &sourceFileTime, &usage);

    if (pic == NULL)
    {
      idLib::Warning(
        "Couldn't load image: %s : %s",
        name.imageName.c_str(),
        name.generatedName.c_str());
      load.error = "Couldn't load image: " + name.imageName + " : " + name.generatedName;
    }
    else
    {
      pixels.width = width;
      pixels.height = height;
      takePicture(pic, width, height, pixels.pixels);
    }
  }
  pixels.usage = int(usage);
  pixels.sourceTime = std::int64_t(sourceFileTime);

  for (auto& rejection : files.endImageReads())
  {
    load.rejected.push_back(rejection.path + ": " + rejection.reason);
  }
  for (auto& file : files.endRecording())
  {
    pixels.files.push_back(FoundGameFile{std::move(file.path), std::move(file.probe)});
  }

  if (!load.rejected.empty())
  {
    // D51: the engine would have read past a buffer or crashed on a refused file, so the
    // image is the default, whatever else loaded; one warning per refused file, and not
    // the engine's own for the failed load
    static_cast<void>(m_registry->takeMessagesFrom(messagesBefore));
    for (const auto& rejection : load.rejected)
    {
      shim::reportWarning(
        "the image file " + rejection + "; " + name.imageName + " is the default image");
    }
    load.error = "the image file " + load.rejected.front();
  }
  else if (load.error.empty())
  {
    load.pixels = std::move(pixels);
  }

  if (quiet)
  {
    load.messages = m_registry->takeMessagesFrom(messagesBefore);
  }
  return load;
}

} // namespace tb::lit
