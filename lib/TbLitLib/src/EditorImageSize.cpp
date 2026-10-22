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
// renderer/Image_load.cpp and renderer/Image_intrinsic.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
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

bool isCube(const int cube)
{
  // Image_load.cpp:716 (CF_NATIVE, CF_CAMERA, CF_QUAKE1, CF_SINGLE, CF_PANORAMA)
  return cube == 1 || cube == 2 || cube == 7 || cube == 6 || cube == 3;
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
  auto files = name.value->sources;
  if (const auto header = cacheHeader(source, *name.value, files))
  {
    return ready(header->width, header->height, std::move(files));
  }

  // Image_load.cpp:716-865: the source
  if (name.value->cube == Cube2DArray || name.value->cube == CubePackedMipChain)
  {
    return defaulted(
      "an image array or packed mip chain without its cache: the engine's bake makes it",
      std::move(files));
  }
  const auto& imageName = name.value->imageName;
  if (isCube(name.value->cube) || imageName.find('(') != std::string::npos)
  {
    // a cube or an image program: the copied loaders, quietly, as the engine sizes them
    auto load = decls.loadImagePixels(*name.value, key.cubeMapSize, true);
    if (load.pixels)
    {
      files.insert(files.end(), load.pixels->files.begin(), load.pixels->files.end());
      return ready(load.pixels->width, load.pixels->height, std::move(files));
    }
    return defaulted(load.error, std::move(files));
  }

  // one image: the file the time call's leaf resolution found, by its header
  if (name.value->sources.empty())
  {
    return defaulted(
      fmt::format("Couldn't load image: {} : {}", imageName, name.value->generatedName),
      std::move(files));
  }
  const auto& leaf = name.value->sources.back();
  auto found = GameFileProbe{};
  const auto bytes = source.readGameFile(leaf.path, &found);
  if (!bytes)
  {
    return defaulted(leaf.path + " cannot be read", std::move(files));
  }
  const auto header = imageFileHeader(leaf.path, *bytes);
  if (header.problem)
  {
    return defaulted(
      "the image file " + leaf.path + ": " + *header.problem, std::move(files));
  }
  return ready(header.width, header.height, std::move(files));
}

EditorImageSizing editorImageSize(
  MaterialDecls& decls, DeclSource& source, const MaterialEntry& material)
{
  auto sizing = EditorImageSizing{};
  const auto use = [&](ImageUploadSize size) {
    sizing.size = EditorImageSize{size.width, size.height, size.state, size.reason};
    sizing.files.insert(sizing.files.end(), size.files.begin(), size.files.end());
  };
  const auto sizeOf = [&](const std::optional<MaterialImageKey>& image) {
    return image ? imageUploadSize(decls, source, *image)
                 : ready(DefaultImageSize, DefaultImageSize, {});
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
      use(std::move(editor));
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
      use(std::move(editor));
      return sizing;
    }
    sizing.files = std::move(editor.files);
    use(sizeOf(stageImage()));
    return sizing;
  }

  // if we don't have an editorImageName, use the first stage image
  use(
    material.stages.empty() ? ready(DefaultImageSize, DefaultImageSize, {})
                            : sizeOf(stageImage()));
  return sizing;
}

} // namespace tb::lit
