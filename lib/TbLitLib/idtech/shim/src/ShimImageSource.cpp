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
// renderer/BinaryImage.cpp, renderer/Image.h and renderer/Image_files.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP1c.2: an image's source through the copied loaders (S14, D51), behind the facade's
// MaterialDecls::loadImagePixels, and its levels as the engine's binarizer lays them out
// before its encoder (D24), MaterialDecls::sourceImageLevels; LPF.0a: LoadEXR's packing
// of an override's floats, packEngineR11G11B10F. Parse thread only (C20).

// clang-format off: the std-only facade, then precompiled.h, then the registry
#include "lit/MaterialDecls.h"
#include "precompiled.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
#include "renderer/Color/ColorSpace.h"
#include "libs/mesa/format_r11g11b10f.h"
// clang-format on

#include <cstring>

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

// BinaryImage.cpp:49, pinned at its default: the fast path, whose normal-map swizzle runs
// before the encoder (the HQ encoders swizzle inside themselves)
constexpr bool image_highQualityCompression = false;

// the bytes per pixel of a 2D level as Load2DFromMemory stores it below (the encoders'
// input for DXT1 and DXT5, R11G11B10F for BC6H)
std::size_t storedPixelBytes(const int textureFormat)
{
  switch (textureFormat)
  {
  case FMT_LUM8:
  case FMT_INT8:
  case FMT_ALPHA:
    return 1;
  case FMT_L8A8:
  case FMT_RGB565:
    return 2;
  default:
    return 4;
  }
}

// LP1c fix (S2.4): the bytes of every level, so that the image's data is allocated once
std::size_t levelBytes(
  const int width, const int height, const int numLevels, const std::size_t pixelBytes)
{
  auto total = std::size_t{0};
  auto w = width;
  auto h = height;
  for (int level = 0; level < numLevels; level++)
  {
    total += std::size_t(w) * std::size_t(h) * pixelBytes;
    w = Max(1, w >> 1);
    h = Max(1, h >> 1);
  }
  return total;
}

// one level's record, its bytes appended to the image's data by append (LP1c fix S2.4:
// written in place, no level buffer)
template <typename Append>
void addLevel(
  CachedImage& image,
  const int level,
  const int destZ,
  const int width,
  const int height,
  Append&& append)
{
  auto record = CachedImageLevel{};
  record.level = level;
  record.destZ = destZ;
  record.width = width;
  record.height = height;
  record.offset = image.data.size();
  append(image.data);
  record.size = image.data.size() - record.offset;
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
 * bytes), then the next level. The engine copies the picture into a Mem_Alloc'ed working
 * buffer; here the source's pixels are that buffer (moved from, LP1c fix S2.4). A stop
 * between levels (cancelled) leaves no level (LP1c fix S2.2).
 */
template <typename Cancelled>
void load2DFromMemory(
  CachedImage& image,
  const int width,
  const int height,
  std::vector<std::uint8_t> work,
  const int numLevels,
  int textureFormat,
  int colorFormat,
  const bool gammaMips,
  Cancelled&& cancelled)
{
  image.textureType = DTT_2D;
  image.storedFormat = textureFormat;
  image.colorFormat = colorFormat;
  image.width = width;
  image.height = height;
  image.numLevels = numLevels;

  byte* pic = work.data();

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

  image.data.reserve(
    levelBytes(width, height, numLevels, storedPixelBytes(textureFormat)));

  // the working buffer is the source's vector for level 0, then the mips' Mem_Alloc'ed
  // pictures, freed as the engine frees them
  const auto release = [&]() {
    if (pic == work.data())
    {
      work = {};
    }
    else
    {
      Mem_Free(pic);
    }
    pic = nullptr;
  };

  int scaledWidth = width;
  int scaledHeight = height;
  for (int level = 0; level < numLevels && pic; level++)
  {
    if (cancelled())
    {
      release();
      image.levels.clear();
      image.data = {};
      return;
    }
    const auto pixels = size_t(scaledWidth) * size_t(scaledHeight);
    const auto copy = [&](std::vector<std::uint8_t>& out) {
      out.insert(out.end(), pic, pic + pixels * 4);
    };
    if (textureFormat == FMT_DXT1 || textureFormat == FMT_DXT5)
    {
      if (
        textureFormat == FMT_DXT5 && colorFormat != CFM_NORMAL_DXT5
        && colorFormat != CFM_YCOCG_DXT5)
      {
        image.colorFormat = colorFormat = CFM_DEFAULT;
      }
      addLevel(image, level, 0, scaledWidth, scaledHeight, copy);
      image.format = FMT_RGBA8;
    }
    else if (textureFormat == FMT_BC6H || textureFormat == FMT_R11G11B10F)
    {
      // RB: store as R11G11B10, the bytes the BC6H encoder takes
      addLevel(image, level, 0, scaledWidth, scaledHeight, copy);
      image.format = FMT_R11G11B10F;
    }
    else if (textureFormat == FMT_LUM8 || textureFormat == FMT_INT8)
    {
      // LUM8 and INT8 just read the red channel
      addLevel(image, level, 0, scaledWidth, scaledHeight, [&](auto& out) {
        for (size_t i = 0; i < pixels; i++)
        {
          out.push_back(pic[i * 4]);
        }
      });
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_ALPHA)
    {
      // ALPHA reads the alpha channel
      addLevel(image, level, 0, scaledWidth, scaledHeight, [&](auto& out) {
        for (size_t i = 0; i < pixels; i++)
        {
          out.push_back(pic[i * 4 + 3]);
        }
      });
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_L8A8)
    {
      // L8A8 reads the alpha and red channels
      addLevel(image, level, 0, scaledWidth, scaledHeight, [&](auto& out) {
        for (size_t i = 0; i < pixels; i++)
        {
          out.push_back(pic[i * 4 + 0]);
          out.push_back(pic[i * 4 + 3]);
        }
      });
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_RGB565)
    {
      addLevel(image, level, 0, scaledWidth, scaledHeight, [&](auto& out) {
        for (size_t i = 0; i < pixels; i++)
        {
          unsigned short color = ((pic[i * 4 + 0] >> 3) << 11)
                                 | ((pic[i * 4 + 1] >> 2) << 5) | (pic[i * 4 + 2] >> 3);
          out.push_back(std::uint8_t((color >> 8) & 0xFF));
          out.push_back(std::uint8_t(color & 0xFF));
        }
      });
      image.format = textureFormat;
    }
    else if (textureFormat == FMT_RG16F)
    {
      // RB: copy it as it was a RGBA8 because of the same size
      addLevel(image, level, 0, scaledWidth, scaledHeight, copy);
      image.format = textureFormat;
    }
    else
    {
      image.storedFormat = textureFormat = FMT_RGBA8;
      addLevel(image, level, 0, scaledWidth, scaledHeight, copy);
      image.format = FMT_RGBA8;
    }

    // downsample for the next level
    byte* shrunk = shrink(pic, scaledWidth, scaledHeight, textureFormat, gammaMips);
    release();
    pic = shrunk;

    scaledWidth = Max(1, scaledWidth >> 1);
    scaledHeight = Max(1, scaledHeight >> 1);
  }

  if (pic)
  {
    release();
  }
}

/**
 * BinaryImage.cpp:679-837 idBinaryImage::LoadCubeFromMemory, the encoder skipped: no
 * colour conversion, the colour format CFM_DEFAULT; per side and level the bytes the
 * engine stores, RGBA8 for DXT1 and DXT5 and R11G11B10F for BC6H (unpadded: the engine
 * replicates a level below 4 × 4 into a whole block for its encoder), R11G11B10F as is,
 * RGBA8 for anything else. The records go in the engine's order, images[ level * 6 + side
 * ]. A stop between levels (cancelled) leaves no level (LP1c fix S2.2).
 */
template <typename Cancelled>
void loadCubeFromMemory(
  CachedImage& image,
  const int width,
  const byte* const pics[6],
  const int numLevels,
  int textureFormat,
  const bool gammaMips,
  Cancelled&& cancelled)
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
      if (cancelled())
      {
        if (pic != orig)
        {
          Mem_Free((void*)pic);
        }
        image.levels.clear();
        image.data = {};
        return;
      }
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

  image.data.reserve(6 * levelBytes(width, width, numLevels, 4));
  for (int level = 0; level < numLevels; level++)
  {
    for (int side = 0; side < 6; side++)
    {
      if (size_t(level) < levels[size_t(side)].size())
      {
        const auto levelWidth = Max(1, width >> level);
        auto& bytes = levels[size_t(side)][size_t(level)];
        addLevel(image, level, side, levelWidth, levelWidth, [&](auto& out) {
          out.insert(out.end(), bytes.begin(), bytes.end());
        });
        bytes = {};
      }
    }
  }
}

// R_LoadCubeImages' side names (Image_files.cpp:1155-1167); CF_SINGLE without a cube
// size takes the axis sides, as its else branch does
const char* const* cubeSideNames(const cubeFiles_t extensions)
{
  static const char* const quakeSides[6] = {
    "_ft.tga", "_bk.tga", "_lf.tga", "_rt.tga", "_up.tga", "_dn.tga"};
  static const char* const cameraSides[6] = {
    "_forward.tga", "_back.tga", "_left.tga", "_right.tga", "_up.tga", "_down.tga"};
  static const char* const axisSides[6] = {
    "_px.tga", "_nx.tga", "_py.tga", "_ny.tga", "_pz.tga", "_nz.tga"};
  if (extensions == CF_CAMERA)
  {
    return cameraSides;
  }
  if (extensions == CF_QUAKE1)
  {
    return quakeSides;
  }
  return axisSides;
}

// a cube R_LoadCubeImages loads from six files
bool isSixFileCube(const cubeFiles_t cubeFiles, const int cubeMapSize)
{
  return cubeFiles == CF_NATIVE || cubeFiles == CF_CAMERA || cubeFiles == CF_QUAKE1
         || (cubeFiles == CF_SINGLE && cubeMapSize == 0);
}

// Image_files.cpp:959-966 imageLoaders, the order R_LoadImage tries the extensions in
constexpr const char* LoaderExtensions[] = {"png", "tga", "jpg", "exr", "hdr"};

/**
 * LP1c fix round 2 (X.9): a cube side's header, as readImageFileHeader reads it; a .png
 * or .jpg whose header is refused gives way to the next extension of the loader order
 * that passes, as R_LoadImage's loop goes on after stb_image's failure (M-IMG-DIFF-04); a
 * refused TGA, EXR or HDR does not. Every file looked at is added to files.
 */
ImageFileHeader sideHeader(
  tb::lit::DeclSource& source, const std::string& path, std::vector<FoundGameFile>& files)
{
  auto probe = GameFileProbe{};
  auto header = readImageFileHeader(source, path, &probe);
  if (!header.problem || !isStbLoaderFile(path))
  {
    return header;
  }
  const auto base = path.substr(0, path.find_last_of('.'));
  for (const auto* extension : LoaderExtensions)
  {
    const auto next = base + "." + extension;
    if (next == path)
    {
      continue;
    }
    auto nextProbe = GameFileProbe{};
    auto nextHeader = readImageFileHeader(source, next, &nextProbe);
    files.push_back(FoundGameFile{next, nextProbe});
    if (!nextProbe.exists() || (nextHeader.problem && isStbLoaderFile(next)))
    {
      continue;
    }
    return nextHeader;
  }
  return header;
}

/**
 * LP1c fix round 2 (X.1): Image_files.cpp:1203-1315, R_LoadCubeImages' two branches that
 * cut six sides out of one picture (CF_SINGLE with a cube size, CF_PANORAMA), transcribed
 * with the check the engine lacks: the copied R_LoadImageProgram leaves no picture and a
 * time of 0 (never FILE_NOT_FOUND_TIMESTAMP) when the picture is missing, refused by the
 * pre-validation, cannot be decoded, or is read after a stop, and the engine's
 * generators read that NULL picture (M-IMG-DEFECT-04). Here no picture fails the load, as
 * does a picture smaller than the 4 x 3 sides R_GenerateCubeMapSideFromSingleImage copies
 * (M-IMG-GUARD-02). The sides go to pics; false with why when there are none.
 */
bool loadSinglePictureCube(
  const char* imgName,
  const cubeFiles_t extensions,
  byte* pics[6],
  int* outSize,
  ID_TIME_T* timestamp,
  int cubeMapSize,
  std::string& why)
{
  int width = 0;
  int height = 0;
  memset(pics, 0, 6 * sizeof(pics[0]));
  *timestamp = 0;

  ID_TIME_T thisTime;
  byte* thisPic[1];
  thisPic[0] = nullptr;
  R_LoadImageProgram(imgName, thisPic, &width, &height, &thisTime);
  if (thisTime == FILE_NOT_FOUND_TIMESTAMP || !thisPic[0])
  {
    if (thisPic[0])
    {
      R_StaticFree(thisPic[0]);
    }
    why = std::string{"its picture "} + imgName
          + " is missing, refused or cannot be decoded (or the session stopped)";
    return false;
  }
  if (thisTime > *timestamp)
  {
    *timestamp = thisTime;
  }

  if (extensions == CF_SINGLE)
  {
    if (
      cubeMapSize < 0 || std::int64_t(width) < 4 * std::int64_t(cubeMapSize)
      || std::int64_t(height) < 3 * std::int64_t(cubeMapSize))
    {
      R_StaticFree(thisPic[0]);
      why = "the single-image cube " + std::string{imgName} + " is "
            + std::to_string(width) + " x " + std::to_string(height)
            + ", smaller than the 4 x 3 sides of " + std::to_string(cubeMapSize)
            + " that the engine copies out of it";
      return false;
    }
    *outSize = cubeMapSize;

    for (int i = 0; i < 6; i++)
    {
      pics[i] =
        R_GenerateCubeMapSideFromSingleImage(thisPic[0], width, height, cubeMapSize, i);
      switch (i)
      {
      case 0: // forward
        R_RotatePic(pics[i], cubeMapSize);
        break;
      case 1: // back
        R_RotatePic(pics[i], cubeMapSize);
        R_HorizontalFlip(pics[i], cubeMapSize, cubeMapSize);
        R_VerticalFlip(pics[i], cubeMapSize, cubeMapSize);
        break;
      case 2: // left
        R_VerticalFlip(pics[i], cubeMapSize, cubeMapSize);
        break;
      case 3: // right
        R_HorizontalFlip(pics[i], cubeMapSize, cubeMapSize);
        break;
      case 4: // up
        R_RotatePic(pics[i], cubeMapSize);
        break;
      case 5: // down
        R_RotatePic(pics[i], cubeMapSize);
        break;
      }
    }
  }
  else
  {
    cubeMapSize = 1024;
    *outSize = cubeMapSize;

    for (int i = 0; i < 6; i++)
    {
      pics[i] =
        R_GenerateCubeMapSideFromPanoramaImage(thisPic[0], width, height, cubeMapSize, i);
    }
  }

  R_StaticFree(thisPic[0]);
  return true;
}

/**
 * LP1c fix round 3 (Y.1): Image_files.cpp:1156-1201 and :1317-1403, R_LoadCubeImages for
 * a cube of six files (CF_NATIVE, CF_CAMERA, CF_QUAKE1, CF_SINGLE without a cube size),
 * transcribed with one change: the side that stops the loop on its size is freed as well,
 * where the engine leaks it (M-IMG-DEFECT-03). A side's load can find another file than
 * the one whose header the pre-check read (a refused .tga, then a sibling .png of another
 * size), so only freeing it here keeps every path free of leaks. The sides go to pics;
 * false, with nothing left allocated, when a side is missing or of another size.
 */
bool loadSixFileCube(
  const char* imgName,
  const cubeFiles_t extensions,
  byte* pics[6],
  int* outSize,
  ID_TIME_T* timestamp)
{
  int i, j;
  const char* const* sides = cubeSideNames(extensions);
  char fullName[MAX_IMAGE_NAME];
  int width = 0, height = 0, size = 0;

  memset(pics, 0, 6 * sizeof(pics[0]));
  *timestamp = 0;

  for (i = 0; i < 6; i++)
  {
    idStr::snPrintf(fullName, sizeof(fullName), "%s%s", imgName, sides[i]);

    ID_TIME_T thisTime;
    R_LoadImageProgram(fullName, &pics[i], &width, &height, &thisTime);

    if (thisTime == FILE_NOT_FOUND_TIMESTAMP)
    {
      break;
    }
    if (i == 0)
    {
      size = width;
    }
    if (width != size || height != size)
    {
      common->Warning("Mismatched sizes on cube map '%s'", imgName);
      break;
    }
    if (thisTime > *timestamp)
    {
      *timestamp = thisTime;
    }
    if (extensions == CF_CAMERA)
    {
      // convert from "camera" images to native cube map images
      switch (i)
      {
      case 0: // forward
        R_RotatePic(pics[i], width);
        break;
      case 1: // back
        R_RotatePic(pics[i], width);
        R_HorizontalFlip(pics[i], width, height);
        R_VerticalFlip(pics[i], width, height);
        break;
      case 2: // left
        R_VerticalFlip(pics[i], width, height);
        break;
      case 3: // right
        R_HorizontalFlip(pics[i], width, height);
        break;
      case 4: // up
        R_RotatePic(pics[i], width);
        break;
      case 5: // down
        R_RotatePic(pics[i], width);
        break;
      }
    }
  }

  if (i != 6)
  {
    // we had an error, so free everything: the sides before i, and (Y.1) the side i that
    // stopped the loop, which the engine leaks
    for (j = 0; j < i; j++)
    {
      R_StaticFree(pics[j]);
      pics[j] = nullptr;
    }
    if (pics[i])
    {
      R_StaticFree(pics[i]);
      pics[i] = nullptr;
    }
    *timestamp = 0;
    return false;
  }

  *outSize = size;
  return true;
}

} // namespace

// LPF.0a: Image_files.cpp:604-621, LoadEXR's loop, over a caller's floats
std::vector<std::uint8_t> packEngineR11G11B10F(const std::vector<std::uint8_t>& rgba32f)
{
  const auto pixelCount = rgba32f.size() / (4 * sizeof(float));
  auto out = std::vector<std::uint8_t>(pixelCount * 4);

  // convert to packed R11G11B10F as uint32 for each pixel

  const std::uint8_t* src = rgba32f.data();
  std::uint8_t* dst = out.data();
  for (size_t i = 0; i < pixelCount; i++)
  {
    // read 3 floats and ignore the alpha channel
    float p[3];
    std::memcpy(p, src, sizeof(p));

    // convert
    uint32_t value = float3_to_r11g11b10f(p);
    std::memcpy(dst, &value, sizeof(value));

    src += 4 * sizeof(float);
    dst += 4;
  }
  return out;
}

CachedImage MaterialDecls::sourceImageLevels(
  SourcePixels&& source,
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
  auto* registry = m_registry.get();
  const auto cancelled = [registry]() {
    return registry && registry->files().cancelling();
  };
  if (source.faces == 6)
  {
    const byte* pics[6];
    for (size_t side = 0; side < 6; ++side)
    {
      pics[side] = source.pixels.data() + side * faceBytes;
    }
    loadCubeFromMemory(
      image, source.width, pics, numLevels, format, gammaMips, cancelled);
  }
  else
  {
    load2DFromMemory(
      image,
      source.width,
      source.height,
      std::move(source.pixels),
      numLevels,
      format,
      colorFormat,
      gammaMips,
      cancelled);
  }
  source.pixels = {};
  return image;
}

CubeSideSizes MaterialDecls::cubeSideSizes(
  const ImageCacheName& name, const int cubeMapSize)
{
  auto result = CubeSideSizes{};
  if (!m_registry)
  {
    result.problem = m_constructionError;
    return result;
  }
  const auto cubeFiles = cubeFiles_t(name.cube);
  if (!isSixFileCube(cubeFiles, cubeMapSize))
  {
    result.problem = "the cube " + name.imageName + " is not made of six files";
    return result;
  }
  // R_LoadCubeImages copies the name into a MAX_IMAGE_NAME buffer
  if (name.imageName.empty() || name.imageName.size() >= std::size_t(MAX_IMAGE_NAME))
  {
    result.problem = "image name too long or empty";
    return result;
  }

  auto& files = m_registry->files();
  const auto messagesBefore = m_registry->messages().size();
  const auto* const* sides = cubeSideNames(cubeFiles);
  auto size = 0;
  for (int i = 0; i < 6; i++)
  {
    // the side's name as R_LoadCubeImages makes it, resolved by the copied time call
    char fullName[MAX_IMAGE_NAME];
    idStr::snPrintf(fullName, sizeof(fullName), "%s%s", name.imageName.c_str(), sides[i]);
    files.beginRecording();
    int width = 0;
    int height = 0;
    ID_TIME_T time = 0;
    R_LoadImageProgram(fullName, NULL, &width, &height, &time);
    auto found = std::vector<FoundGameFile>{};
    shim::splitRecorded(files.endRecording(), found, result.files);
    if (found.empty())
    {
      result.problem = "its side " + std::string{fullName} + " is missing";
      break;
    }
    // the file a side's leaf resolution found last is the one it loads (X.9: or the
    // next extension's, when a .png or .jpg is refused)
    const auto sideFile = found.back();
    result.files.insert(result.files.end(), found.begin(), found.end());
    const auto header = sideHeader(files.source(), sideFile.path, result.files);
    if (header.problem)
    {
      result.problem = "its side " + sideFile.path + ": " + *header.problem;
      break;
    }
    if (i == 0)
    {
      size = header.width;
    }
    // R_LoadCubeImages: "Mismatched sizes on cube map"
    if (header.width != size || header.height != size)
    {
      result.problem = "Mismatched sizes on cube map '" + name.imageName + "'";
      break;
    }
  }
  static_cast<void>(m_registry->takeMessagesFrom(messagesBefore));
  if (result.problem.empty())
  {
    result.size = size;
  }
  return result;
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

  const auto cubeFiles = cubeFiles_t(name.cube);
  // LP1c fix (S2.1): a six-file cube's sides by their headers first, before the copy
  // reads any pixels, so that it never stops part-way on a missing or mismatched side
  auto sides = std::optional<CubeSideSizes>{};
  if (isSixFileCube(cubeFiles, cubeMapSize))
  {
    sides = cubeSideSizes(name, cubeMapSize);
  }

  auto& files = m_registry->files();
  const auto messagesBefore = m_registry->messages().size();
  auto reads = shim::ImageReads{files};

  // Image_load.cpp:716-865, the source of an image whose cache the engine would not use;
  // GetName() is the name after the PBR rename, usage the time call's
  auto usage = textureUsage_t(name.usage);
  ID_TIME_T sourceFileTime = ID_TIME_T(name.sourceTime);
  auto pixels = SourcePixels{};
  if (
    cubeFiles == CF_NATIVE || cubeFiles == CF_CAMERA || cubeFiles == CF_QUAKE1
    || cubeFiles == CF_SINGLE || cubeFiles == CF_PANORAMA)
  {
    int size = 0;
    byte* pics[6] = {};
    auto loaded = false;
    if (sides && !sides->problem.empty())
    {
      // the engine's own warning for a mismatched side, then its failure
      if (sides->problem.rfind("Mismatched sizes", 0) == 0)
      {
        idLib::Warning("%s", sides->problem.c_str());
      }
      idLib::Warning("Couldn't load cube image: %s", name.imageName.c_str());
      load.error =
        "Couldn't load cube image: " + name.imageName + " (" + sides->problem + ")";
    }
    else if (!sides)
    {
      // X.1: one picture cut into six sides, with the check the engine lacks
      auto why = std::string{};
      loaded = loadSinglePictureCube(
        name.imageName.c_str(),
        cubeFiles,
        pics,
        &size,
        &sourceFileTime,
        cubeMapSize,
        why);
      if (!loaded)
      {
        idLib::Warning("Couldn't load cube image: %s", name.imageName.c_str());
        load.error = "Couldn't load cube image: " + name.imageName + " (" + why + ")";
      }
    }
    else
    {
      // Y.1: the six-file loop, transcribed: a side that stops it is freed too
      loaded =
        loadSixFileCube(name.imageName.c_str(), cubeFiles, pics, &size, &sourceFileTime);
      if (!loaded)
      {
        idLib::Warning("Couldn't load cube image: %s", name.imageName.c_str());
        load.error = "Couldn't load cube image: " + name.imageName;
      }
    }
    if (loaded && size == 0)
    {
      // six sides of size 0: the engine keeps them; they are freed here
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
    else if (loaded)
    {
      pixels.width = size;
      pixels.height = size;
      pixels.faces = 6;
      pixels.pixels.reserve(std::size_t(size) * std::size_t(size) * 4 * 6);
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

  auto forcing = std::vector<std::string>{};
  auto [rejections, recorded] = reads.take();
  for (auto& rejection : rejections)
  {
    const auto stb = isStbLoaderFile(rejection.path);
    load.rejected.push_back(rejection.path + ": " + rejection.reason);
    if (!stb)
    {
      forcing.push_back(load.rejected.back());
    }
  }
  shim::splitRecorded(std::move(recorded), pixels.files, pixels.missing);

  if (!forcing.empty())
  {
    // D51: the engine would have read past a buffer or stopped on a refused TGA, EXR or
    // HDR file, so the image is the default, whatever else loaded; one warning per
    // refused file, and not the engine's own for the failed load
    static_cast<void>(m_registry->takeMessagesFrom(messagesBefore));
    for (const auto& rejection : load.rejected)
    {
      shim::reportWarning(
        "the image file " + rejection + "; " + name.imageName + " is the default image");
    }
    load.error = "the image file " + forcing.front();
  }
  else
  {
    // LP1c fix (S3.3): a refused .png or .jpg is stb_image's failure in the engine, which
    // warns and goes on to the next extension; the image is what that found
    for (const auto& rejection : load.rejected)
    {
      shim::reportWarning(
        "the image file " + rejection + "; stb_image would not load it either, so the "
        "next extension is tried");
    }
    if (load.error.empty())
    {
      load.pixels = std::move(pixels);
    }
  }

  if (quiet)
  {
    load.messages = m_registry->takeMessagesFrom(messagesBefore);
  }
  return load;
}

} // namespace tb::lit
