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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Material.cpp,
// renderer/Image_load.cpp, renderer/Image_intrinsic.cpp and renderer/Image_files.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
//   Copyright (C) 2016-2017 Dustin Land
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/EditorImageSize.h"

#include "lit/GameFile.h"
#include "lit/ImageCache.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <array>
#include <cstdint>

namespace tb::lit
{
namespace
{

using namespace engine_image;
using State = EditorImageSize::State;

// materialStage lighting: SL_DIFFUSE
constexpr int StageDiffuse = 2;
// cubeFiles_t: the image arrays and packed mip chains the engine makes itself
constexpr int Cube2DArray = 4;
// a .bimage header: int64 time, then seven int32s
constexpr std::size_t BimageHeaderBytes = 36;

// Image_intrinsic.cpp: DEFAULT_SIZE, the default image's generator size
constexpr int DefaultImageSize = 16;

ImageUploadSize ready(const int width, const int height, std::vector<FoundGameFile> files)
{
  return ImageUploadSize{width, height, State::Ready, false, {}, std::move(files)};
}

// Image_load.cpp:775-819: a load without a picture makes an 8 × 8 defaulted image
ImageUploadSize defaulted(std::string reason, std::vector<FoundGameFile> files)
{
  return ImageUploadSize{8, 8, State::Default, true, std::move(reason), std::move(files)};
}

// cubeFiles_t: CF_NATIVE, CF_CAMERA, CF_PANORAMA, CF_SINGLE, CF_QUAKE1
constexpr int CubeNative = 1;
constexpr int CubeCamera = 2;
constexpr int CubePanorama = 3;
constexpr int CubeSingle = 6;
constexpr int CubeQuake1 = 7;
// R_LoadCubeImages' side size for a panorama
constexpr int PanoramaSideSize = 1024;

bool isCube(const int cube)
{
  // Image_load.cpp:716 (CF_NATIVE, CF_CAMERA, CF_QUAKE1, CF_SINGLE, CF_PANORAMA)
  return cube == CubeNative || cube == CubeCamera || cube == CubeQuake1
         || cube == CubeSingle || cube == CubePanorama;
}

// R_LoadCubeImages' loop over six files (CF_SINGLE without a cube size takes it too)
bool isSixFileCube(const int cube, const int cubeMapSize)
{
  return cube == CubeNative || cube == CubeCamera || cube == CubeQuake1
         || (cube == CubeSingle && cubeMapSize == 0);
}

// Image_files.cpp:959-966 imageLoaders, the order R_LoadImage tries the extensions in
constexpr std::array<std::string_view, 5> LoaderExtensions = {
  "png", "tga", "jpg", "exr", "hdr"};

/**
 * LP1c fix (S2.2, S3.3): a leaf's size by its header, read without its body
 * (readImageFileHeader). A .png or .jpg that the pre-validation refuses is stb_image's
 * failure in the engine, whose R_LoadImage goes on to the next extension: the next file
 * of the loader order is sized instead; a refused TGA, EXR or HDR file is the default.
 */
ImageFileHeader leafHeader(
  DeclSource& source, const std::string& path, std::vector<FoundGameFile>& files)
{
  auto probe = GameFileProbe{};
  auto header = readImageFileHeader(source, path, &probe);
  if (!header.problem || !isStbLoaderFile(path))
  {
    if (header.problem)
    {
      header.problem = "the image file " + path + ": " + *header.problem;
    }
    return header;
  }
  const auto refusal = "the image file " + path + ": " + *header.problem;
  const auto dot = path.find_last_of('.');
  const auto base = path.substr(0, dot);
  for (const auto extension : LoaderExtensions)
  {
    const auto next = base + "." + std::string{extension};
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
    if (nextHeader.problem)
    {
      nextHeader.problem =
        refusal + "; the image file " + next + ": " + *nextHeader.problem;
    }
    return nextHeader;
  }
  header.problem = refusal;
  return header;
}

// the usable cache's header, read without the body (BinaryImage.cpp:994-1036 and
// Image_load.cpp:674-689), or nullopt
std::optional<CachedImage> cacheHeader(
  DeclSource& source, const ImageCacheName& name, std::vector<FoundGameFile>& files)
{
  const auto read = [&](const std::string& file) -> std::optional<CachedImage> {
    auto origin = GameFileProbe{};
    const auto bytes = source.readGameFileHead(file, BimageHeaderBytes, &origin);
    if (!bytes)
    {
      return std::nullopt;
    }
    files.push_back(FoundGameFile{file, origin});
    return readBimageHeader(*bytes) | kdl::transform([](CachedImage image) {
             return std::optional{std::move(image)};
           })
           | kdl::transform_error(
             [](const auto&) { return std::optional<CachedImage>{}; })
           | kdl::value();
  };
  auto header = read(name.file);
  if (!header && source.usingResourceFiles() && !name.bfhackFile.empty())
  {
    header = read(name.bfhackFile);
  }
  if (
    !header || engineCacheIsStale(name.sourceTime, header->sourceTime)
    || !engineCacheMatches(*header, name.usage, name.textureType))
  {
    return std::nullopt;
  }
  return header;
}

} // namespace

std::optional<std::pair<int, int>> intrinsicImageSize(const std::string_view name)
{
  // Image_intrinsic.cpp:1060-1161 CreateIntrinsicImages and each generator's size
  struct Intrinsic
  {
    std::string_view name;
    int width;
    int height;
  };
  static constexpr auto Sizes = std::array<Intrinsic, 31>{{
    {"_default", 16, 16},
    {"_white", 16, 16},
    {"_black", 16, 16},
    {"_blackDiffuse", 16, 16},
    {"_cyan", 16, 16},
    {"_redClay", 16, 16},
    {"_flat", 16, 16},
    {"_alphaNotch", 2, 1},
    // FOG_SIZE, FOG_ENTER_SIZE, FALLOFF_TEXTURE_SIZE × 16, QUADRATIC_WIDTH × HEIGHT
    {"_fog", 128, 128},
    {"_fogEnter", 64, 64},
    {"_noFalloff", 64, 16},
    {"_quadratic", 32, 4},
    // JITTER_SIZE
    {"_jitter1", 128, 128},
    {"_jitter4", 512, 128},
    {"_jitter16", 2048, 128},
    {"_random256", 256, 256},
    // Image_blueNoiseVC_2.h
    {"_blueNoise256", 512, 512},
    // ENVPROBE_CAPTURE_SIZE
    {"_envprobeHDR", 256, 256},
    {"_envprobeDepth", 256, 256},
    {"_heatmap5", 64, 16},
    {"_heatmap7", 64, 16},
    {"_grain1", 128, 128},
    // SMAA/AreaTex.h, SearchTex.h
    {"_smaaArea", 160, 560},
    {"_smaaSearch", 64, 16},
    {"_chromeSpec", 16, 16},
    {"_plasticSpec", 16, 16},
    // Image_brdfLut.h
    {"_brdfLut", 256, 256},
    {"_scratch", 16, 16},
    {"_scratch2", 16, 16},
    {"_accum", 512, 512},
    {"_guiEdit", 640, 480},
  }};
  for (const auto& intrinsic : Sizes)
  {
    if (equalsNoCaseAscii(intrinsic.name, name))
    {
      return std::pair{intrinsic.width, intrinsic.height};
    }
  }
  if (equalsNoCaseAscii(name, "_defaultUACRadiance"))
  {
    return std::pair{8, 8};
  }
  if (equalsNoCaseAscii(name, "_guiEditDepthStencil"))
  {
    return std::pair{640, 480};
  }
  return std::nullopt;
}

ImageUploadSize imageUploadSize(
  MaterialDecls& decls, DeclSource& source, const MaterialImageKey& key)
{
  // the generators (ActuallyLoadImage: "this is the ONLY place generatorFunction will
  // ever be called")
  if (key.isDefault)
  {
    return ready(DefaultImageSize, DefaultImageSize, {});
  }
  if (key.intrinsic)
  {
    if (const auto size = intrinsicImageSize(key.name))
    {
      return ready(size->first, size->second, {});
    }
    auto size = ImageUploadSize{};
    size.defaulted = false;
    size.reason = "the engine sizes " + key.name + " by its window, a cvar or a font";
    return size;
  }

  auto name = decls.imageCacheName(key);
  if (!name)
  {
    return defaulted("the image's name cannot be made: " + name.error, {});
  }
  // LP1c fix (S2.5): the names the time call missed too, so that a file that appears
  // later is measured
  auto files = name.value->sources;
  files.insert(files.end(), name.value->missing.begin(), name.value->missing.end());
  if (const auto header = cacheHeader(source, *name.value, files))
  {
    return ready(header->width, header->height, std::move(files));
  }

  // Image_load.cpp:716-865: the source, sized by headers without decoding (LP1c fix S2.2)
  const auto cube = name.value->cube;
  if (cube == Cube2DArray || cube == CubePackedMipChain)
  {
    return defaulted(
      "an image array or packed mip chain without its cache: the engine's bake makes it",
      std::move(files));
  }
  const auto& imageName = name.value->imageName;
  if (isSixFileCube(cube, key.cubeMapSize))
  {
    // R_LoadCubeImages' loop: six sides of one square size, side 0's
    auto sides = decls.cubeSideSizes(*name.value, key.cubeMapSize);
    files.insert(files.end(), sides.files.begin(), sides.files.end());
    if (!sides.problem.empty())
    {
      return defaulted(
        fmt::format("Couldn't load cube image: {} ({})", imageName, sides.problem),
        std::move(files));
    }
    return ready(sides.size, sides.size, std::move(files));
  }

  // one picture: a 2D image or program by its first leaf (addnormals resamples its
  // second to the first's size; no program changes the size), a CF_SINGLE cube or a
  // panorama by the picture R_LoadCubeImages cuts its sides from. A missing leaf ends the
  // time call, and the engine's load then has no picture
  if (name.value->sources.empty() || (!isCube(cube) && !name.value->resolved))
  {
    return defaulted(
      isCube(cube)
        ? fmt::format("Couldn't load cube image: {}", imageName)
        : fmt::format(
            "Couldn't load image: {} : {}", imageName, name.value->generatedName),
      std::move(files));
  }
  const auto header = leafHeader(source, name.value->sources.front().path, files);
  if (header.problem)
  {
    return defaulted(*header.problem, std::move(files));
  }
  if (cube == CubeSingle)
  {
    // M-IMG-GUARD-02: the cross of 4 × 3 sides must fit the picture
    if (
      key.cubeMapSize < 0
      || std::int64_t(header.width) < 4 * std::int64_t(key.cubeMapSize)
      || std::int64_t(header.height) < 3 * std::int64_t(key.cubeMapSize))
    {
      return defaulted(
        fmt::format(
          "the single-image cube {} is {} x {}, smaller than the 4 x 3 sides of {}",
          imageName,
          header.width,
          header.height,
          key.cubeMapSize),
        std::move(files));
    }
    return ready(key.cubeMapSize, key.cubeMapSize, std::move(files));
  }
  if (cube == CubePanorama)
  {
    // R_LoadCubeImages: cubeMapSize = 1024 for a panorama
    return ready(PanoramaSideSize, PanoramaSideSize, std::move(files));
  }
  return ready(header.width, header.height, std::move(files));
}

EditorImageSizing editorImageSize(
  MaterialDecls& decls, DeclSource& source, const MaterialEntry& material)
{
  auto sizing = EditorImageSizing{};
  const auto use = [&](ImageUploadSize size, std::string image) {
    sizing.size = EditorImageSize{size.width, size.height, size.state, size.reason};
    sizing.image = std::move(image);
    sizing.files.insert(sizing.files.end(), size.files.begin(), size.files.end());
  };
  const auto sizeOf = [&](const std::optional<MaterialImageKey>& image) {
    if (image)
    {
      use(imageUploadSize(decls, source, *image), image->name);
    }
    else
    {
      use(ready(DefaultImageSize, DefaultImageSize, {}), {});
    }
  };
  // Material.cpp:236-251: the first diffuse stage's image, else stage 0's; null when
  // neither has one
  const auto stageImage = [&]() -> std::optional<MaterialImageKey> {
    auto image = std::optional<MaterialImageKey>{};
    for (const auto& stage : material.stages)
    {
      if (stage.lighting == StageDiffuse)
      {
        image = stage.image;
        break;
      }
    }
    if (!image && !material.stages.empty())
    {
      image = material.stages.front().image;
    }
    return image;
  };

  if (const auto key = decls.editorImageKey(material))
  {
    // look for an explicit one, as TD_DIFFUSE
    auto editor = imageUploadSize(decls, source, *key);
    if (!editor.defaulted || material.stages.empty())
    {
      use(std::move(editor), key->name);
      return sizing;
    }
    // look for the diffusemap alternative like TrenchBroom does: the first diffuse
    // stage's image, or stage 0's when that one has none; without a diffuse stage the
    // defaulted editor image stays
    auto diffuse = false;
    for (const auto& stage : material.stages)
    {
      diffuse = diffuse || stage.lighting == StageDiffuse;
    }
    if (!diffuse)
    {
      use(std::move(editor), key->name);
      return sizing;
    }
    sizing.files = std::move(editor.files);
    sizeOf(stageImage());
    return sizing;
  }

  // if we don't have an editorImageName, use the first stage image
  if (material.stages.empty())
  {
    use(ready(DefaultImageSize, DefaultImageSize, {}), {});
  }
  else
  {
    sizeOf(stageImage());
  }
  return sizing;
}

} // namespace tb::lit
