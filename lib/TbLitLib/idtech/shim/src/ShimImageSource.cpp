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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Image_load.cpp,
// renderer/BinaryImage.cpp and renderer/Image.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c.2: an image's source through the copied loaders (S14, D51), behind the facade's
// MaterialDecls::loadImagePixels, and its levels as the engine's binarizer lays them out
// before its encoder (D24), MaterialDecls::sourceImageLevels. Parse thread only (C20).

// clang-format off: the std-only facade, then precompiled.h, then the registry
#include "lit/MaterialDecls.h"
#include "precompiled.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
#include "renderer/Color/ColorSpace.h"
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

// Image.h:34-139, the enum values the copied Image.h ranges leave out
constexpr int DTT_2D = 1;
constexpr int DTT_CUBIC = 2;
constexpr int FMT_RGBA8 = 1;
constexpr int FMT_ALPHA = 3;
constexpr int FMT_L8A8 = 4;
constexpr int FMT_LUM8 = 5;
constexpr int FMT_INT8 = 6;
constexpr int FMT_DXT1 = 7;
constexpr int FMT_DXT5 = 8;
constexpr int FMT_RGB565 = 12;
constexpr int FMT_RG16F = 15;
constexpr int FMT_R11G11B10F = 19;
constexpr int FMT_BC6H = 20;
constexpr int FMT_BC7 = 21;
constexpr int CFM_DEFAULT = 0;
constexpr int CFM_NORMAL_DXT5 = 1;
constexpr int CFM_YCOCG_DXT5 = 2;
constexpr int CFM_GREEN_ALPHA = 3;

// BinaryImage.cpp:49, pinned at its default: the fast path, whose normal-map swizzle runs
// before the encoder (the HQ encoders swizzle inside themselves)
constexpr bool image_highQualityCompression = false;

void addLevel(
  CachedImage& image,
  const int level,
  const int destZ,
  const int width,
  const int height,
  std::vector<std::uint8_t> bytes)
{
  auto record = CachedImageLevel{};
  record.level = level;
  record.destZ = destZ;
  record.width = width;
  record.height = height;
  record.offset = image.data.size();
  record.size = bytes.size();
  image.data.insert(image.data.end(), bytes.begin(), bytes.end());
  image.levels.push_back(record);
}

// BinaryImage.cpp:309-324 and :807-820, the next level
byte* shrink(
  const byte* pic,
  const int width,
  const int height,
  const int textureFormat,
  const bool gammaMips)
{
  if (textureFormat == FMT_R11G11B10F || textureFormat == FMT_BC6H)
  {
    return R_MipMapR11G11B10F(pic, width, height);
  }
  if (gammaMips)
  {
    return R_MipMapWithGamma(pic, width, height);
  }
  return R_MipMap(pic, width, height);
}

/**
 * BinaryImage.cpp:56-331 idBinaryImage::Load2DFromMemory, the encoder skipped: the colour
 * conversion, then per level the bytes the engine stores, RGBA8 for DXT1 and DXT5 (the
 * encoder's input, unpadded) and R11G11B10F for BC6H (the SSE build encodes it from those
 * bytes), then the next level.
 */
void load2DFromMemory(
  CachedImage& image,
  const int width,
  const int height,
  const byte* pic_const,
  const int numLevels,
  int textureFormat,
  int colorFormat,
  const bool gammaMips)
{
  image.textureType = DTT_2D;
  image.storedFormat = textureFormat;
  image.colorFormat = colorFormat;
  image.width = width;
  image.height = height;
  image.numLevels = numLevels;

  byte* pic = (byte*)Mem_Alloc(size_t(width) * size_t(height) * 4, TAG_TEMP);
  memcpy(pic, pic_const, size_t(width) * size_t(height) * 4);

  if (colorFormat == CFM_YCOCG_DXT5)
  {
    // convert the image data to YCoCg and use the YCoCgDXT5 compressor
    idColorSpace::ConvertRGBToCoCg_Y(pic, pic, width, height);
  }
  else if (colorFormat == CFM_NORMAL_DXT5)
  {
    // Blah, HQ swizzles automatically, Fast doesn't
    if (!image_highQualityCompression)
    {
      for (int i = 0; i < width * height; i++)
      {
        pic[i * 4 + 3] = pic[i * 4 + 0];
        pic[i * 4 + 0] = 0;
        pic[i * 4 + 2] = 0;
      }
    }
  }
  else if (colorFormat == CFM_GREEN_ALPHA)
  {
    for (int i = 0; i < width * height; i++)
    {
      pic[i * 4 + 1] = pic[i * 4 + 3];
      pic[i * 4 + 0] = 0;
      pic[i * 4 + 2] = 0;
      pic[i * 4 + 3] = 0;
    }
  }

  int scaledWidth = width;
  int scaledHeight = height;
  for (int level = 0; level < numLevels && pic; level++)
  {
    const auto pixels = size_t(scaledWidth) * size_t(scaledHeight);
    auto bytes = std::vector<std::uint8_t>{};
    if (textureFormat == FMT_DXT1 || textureFormat == FMT_DXT5)
    {
      if (
        textureFormat == FMT_DXT5 && colorFormat != CFM_NORMAL_DXT5
        && colorFormat != CFM_YCOCG_DXT5)
      {
        image.colorFormat = colorFormat = CFM_DEFAULT;
      }
      bytes.assign(pic, pic + pixels * 4);
      image.format = FMT_RGBA8;
    }
    else if (textureFormat == FMT_BC6H || textureFormat == FMT_R11G11B10F)
    {
      // RB: store as R11G11B10, the bytes the BC6H encoder takes
      bytes.assign(pic, pic + pixels * 4);
      image.format = FMT_R11G11B10F;
    }
    else if (textureFormat == FMT_LUM8 || textureFormat == FMT_INT8)
    {
      // LUM8 and INT8 just read the red channel
      for (size_t i = 0; i < pixels; i++)
      {
        bytes.push_back(pic[i * 4]);
      }
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_ALPHA)
    {
      // ALPHA reads the alpha channel
      for (size_t i = 0; i < pixels; i++)
      {
        bytes.push_back(pic[i * 4 + 3]);
      }
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_L8A8)
    {
      // L8A8 reads the alpha and red channels
      for (size_t i = 0; i < pixels; i++)
      {
        bytes.push_back(pic[i * 4 + 0]);
        bytes.push_back(pic[i * 4 + 3]);
      }
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_RGB565)
    {
      for (size_t i = 0; i < pixels; i++)
      {
        unsigned short color = ((pic[i * 4 + 0] >> 3) << 11)
                               | ((pic[i * 4 + 1] >> 2) << 5) | (pic[i * 4 + 2] >> 3);
        bytes.push_back(std::uint8_t((color >> 8) & 0xFF));
        bytes.push_back(std::uint8_t(color & 0xFF));
      }
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_RG16F)
    {
      // RB: copy it as it was a RGBA8 because of the same size
      bytes.assign(pic, pic + pixels * 4);
      image.format = textureFormat;
    }
    else
    {
      image.storedFormat = textureFormat = FMT_RGBA8;
      bytes.assign(pic, pic + pixels * 4);
      image.format = FMT_RGBA8;
    }
    addLevel(image, level, 0, scaledWidth, scaledHeight, std::move(bytes));

    // downsample for the next level
    byte* shrunk = shrink(pic, scaledWidth, scaledHeight, textureFormat, gammaMips);
    Mem_Free(pic);
    pic = shrunk;

    scaledWidth = Max(1, scaledWidth >> 1);
    scaledHeight = Max(1, scaledHeight >> 1);
  }

  Mem_Free(pic);
}

/**
 * BinaryImage.cpp:679-837 idBinaryImage::LoadCubeFromMemory, the encoder skipped: no
 * colour conversion, the colour format CFM_DEFAULT; per side and level the bytes the
 * engine stores, RGBA8 for DXT1 and DXT5 and R11G11B10F for BC6H (unpadded: the engine
 * replicates a level below 4 × 4 into a whole block for its encoder), R11G11B10F as is,
 * RGBA8 for anything else. The records go in the engine's order, images[ level * 6 + side
 * ].
 */
void loadCubeFromMemory(
  CachedImage& image,
  const int width,
  const byte* const pics[6],
  const int numLevels,
  int textureFormat,
  const bool gammaMips)
{
  image.textureType = DTT_CUBIC;
  image.storedFormat = textureFormat;
  image.colorFormat = CFM_DEFAULT;
  image.width = image.height = width;
  image.numLevels = numLevels;

  // the levels of each side, made side by side as the engine makes them
  auto levels = std::vector<std::vector<std::vector<std::uint8_t>>>(6);
  auto format = FMT_RGBA8;
  for (int side = 0; side < 6; side++)
  {
    const byte* orig = pics[side];
    const byte* pic = orig;
    int scaledWidth = width;

    for (int level = 0; level < numLevels && pic; level++)
    {
      const auto bytes = size_t(scaledWidth) * size_t(scaledWidth) * 4;
      if (
        textureFormat == FMT_DXT1 || textureFormat == FMT_DXT5
        || textureFormat == FMT_BC7)
      {
        format = FMT_RGBA8;
      }
      else if (textureFormat == FMT_BC6H || textureFormat == FMT_R11G11B10F)
      {
        format = FMT_R11G11B10F;
      }
      else
      {
        image.storedFormat = textureFormat = FMT_RGBA8;
        format = FMT_RGBA8;
      }
      levels[size_t(side)].emplace_back(pic, pic + bytes);

      // downsample for the next level
      byte* shrunk = shrink(pic, scaledWidth, scaledWidth, textureFormat, gammaMips);
      if (pic != orig)
      {
        Mem_Free((void*)pic);
        pic = NULL;
      }
      pic = shrunk;

      scaledWidth = Max(1, scaledWidth >> 1);
    }
    if (pic != orig)
    {
      // free the down sampled version
      Mem_Free((void*)pic);
      pic = NULL;
    }
  }
  image.format = format;

  for (int level = 0; level < numLevels; level++)
  {
    for (int side = 0; side < 6; side++)
    {
      if (size_t(level) < levels[size_t(side)].size())
      {
        const auto levelWidth = Max(1, width >> level);
        addLevel(
          image,
          level,
          side,
          levelWidth,
          levelWidth,
          std::move(levels[size_t(side)][size_t(level)]));
      }
    }
  }
}

} // namespace

CachedImage MaterialDecls::sourceImageLevels(
  const SourcePixels& source,
  const int format,
  const int colorFormat,
  const int numLevels,
  const bool gammaMips)
{
  auto image = CachedImage{};
  image.fromSource = true;
  image.sourceTime = source.sourceTime;
  const auto faceBytes = size_t(source.width) * size_t(source.height) * 4;
  if (
    source.width < 1 || source.height < 1 || numLevels < 1
    || source.pixels.size() != faceBytes * size_t(source.faces))
  {
    return image;
  }
  if (source.faces == 6)
  {
    const byte* pics[6];
    for (size_t side = 0; side < 6; ++side)
    {
      pics[side] = source.pixels.data() + side * faceBytes;
    }
    loadCubeFromMemory(image, source.width, pics, numLevels, format, gammaMips);
  }
  else
  {
    load2DFromMemory(
      image,
      source.width,
      source.height,
      source.pixels.data(),
      numLevels,
      format,
      colorFormat,
      gammaMips);
  }
  return image;
}

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
