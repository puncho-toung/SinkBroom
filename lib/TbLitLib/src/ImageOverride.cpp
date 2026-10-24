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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/ImageManager.cpp, idlib/Str.cpp and
// renderer/Image_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ImageOverride.h"

#include "TextureUploads.h"
#include "lit/EngineImageEnums.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/LitConfig.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tb::lit
{
namespace
{

using namespace engine_image;

// cubeFiles_t (Image.h): the images the engine makes itself in these layouts
constexpr int Cube2DArray = 4;

// ImageManager.cpp:420 (and the shim's ShimRenderer.cpp ImageFormats)
constexpr std::string_view ImageFormats[] = {".tga", ".jpg", ".png", ".exr", ".hdr"};

// Str.cpp:937-981 idStr::Replace( old, "" ): every occurrence, left to right, without
// overlap, compared with case (idStr::Cmpn)
std::string removeAll(const std::string& text, const std::string_view old)
{
  auto out = std::string{};
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();)
  {
    if (text.compare(i, old.size(), old) == 0)
    {
      i += old.size();
    }
    else
    {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

// idStr::Icmp's case folding: ASCII letters only
std::string lowercased(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  });
  return text;
}

// textureUsage_t: the usages DeriveOpts gives BC6H, whose sources are float pictures
bool hdrUsage(const int usage)
{
  return usage == UsageHdri || usage == UsageHdrLightProbe;
}

// the bytes a texel of level 0 takes once sourceImageLevels has stored it: one channel
// for LUM8, INT8 and ALPHA, two for L8A8, four for the rest (RGBA8 for DXT and RGBA8,
// R11G11B10F, RG16F, RGB565 expanded to RGBA8)
std::uint64_t storedTexelBytes(const int format)
{
  switch (format)
  {
  case FormatLum8:
  case FormatInt8:
  case FormatAlpha:
    return 1;
  case FormatL8a8:
    return 2;
  default:
    return 4;
  }
}

std::string capRefusal(const int level, const std::uint64_t bytes, const int faces)
{
  return fmt::format(
    "an override is refused: level {} is {} bytes over its {} face(s), more than the {} "
    "MB a frame uploads (UploadBytesPerFrame; the engine writes a level whole, "
    "mirror.json "
    "M-TEX-DIFF-02)",
    level,
    bytes,
    faces,
    UploadBytesPerFrame / (1024 * 1024));
}

/**
 * D103 (P5.6, P5.25): the render side's rule (TextureUploads::record, U3.16) at the
 * service: each level's bytes as writeTexture reads them (levelUploadBytes), summed over
 * its faces, within UploadBytesPerFrame; "" when the image passes.
 */
std::string levelCapProblem(const CachedImage& image)
{
  auto problem = std::string{};
  auto perLevel =
    std::vector<std::uint64_t>(std::size_t(std::max(image.numLevels, 0)), 0);
  for (const auto& level : image.levels)
  {
    levelUploadBytes(image, level) | kdl::transform([&](const std::uint64_t bytes) {
      if (level.level >= 0 && level.level < image.numLevels)
      {
        perLevel[std::size_t(level.level)] += bytes;
      }
    }) | kdl::if_error([&](const auto& e) {
      problem = "an override is refused: " + e.msg;
    }) | kdl::ignore();
    if (!problem.empty())
    {
      return problem;
    }
  }
  const auto faces = image.textureType == TypeCubic ? 6 : 1;
  for (std::size_t level = 0; level < perLevel.size(); ++level)
  {
    if (perLevel[level] > UploadBytesPerFrame)
    {
      return capRefusal(int(level), perLevel[level], faces);
    }
  }
  return {};
}

// C39 (a): raw pixels, converted as the source fallback converts a loaded source
Result<std::shared_ptr<const CachedImage>> rawOverride(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  const RawImagePixels& raw)
{
  const auto cube = name.textureType == TypeCubic;
  const auto faces = cube ? 6 : 1;
  if (raw.width < 1 || raw.height < 1)
  {
    return Error{fmt::format(
      "an override is refused: its pixels are {} x {}", raw.width, raw.height)};
  }
  if (raw.faces != faces)
  {
    return Error{fmt::format(
      "an override is refused: it has {} face(s), where the key's image is {} ({} "
      "face(s), R_LoadCubeImages' six sides for a cube)",
      raw.faces,
      cube ? "a cube" : "2D",
      faces)};
  }
  if (cube && raw.width != raw.height)
  {
    return Error{fmt::format(
      "an override is refused: a cube's sides are square (R_LoadCubeImages), its are {} "
      "x {}",
      raw.width,
      raw.height)};
  }
  const auto texelBytes = std::uint64_t(raw.format == ImagePixelFormat::RGBA32F ? 16 : 4);
  const auto texels =
    std::uint64_t(raw.width) * std::uint64_t(raw.height) * std::uint64_t(faces);
  if (std::uint64_t(raw.bytes.size()) != texels * texelBytes)
  {
    return Error{fmt::format(
      "an override is refused: it holds {} bytes, where {} x {} x {} face(s) of {} bytes "
      "a texel are {}",
      raw.bytes.size(),
      raw.width,
      raw.height,
      faces,
      texelBytes,
      texels * texelBytes)};
  }
  // P5.5: the engine reads the 8-bit usages from 8-bit sources and the HDR usages from
  // float ones (LoadEXR and LoadHDR pack them to R11G11B10F); never across
  if (raw.format == ImagePixelFormat::RGBA32F && !hdrUsage(name.usage))
  {
    return Error{fmt::format(
      "an RGBA32F override is refused for usage {}: only TD_HDRI and TD_HDR_LIGHTPROBE "
      "take floats (packed to R11G11B10F as LoadEXR packs them); the engine reads every "
      "other usage from an 8-bit source",
      name.usage)};
  }
  if (raw.format == ImagePixelFormat::RGBA8 && hdrUsage(name.usage))
  {
    return Error{fmt::format(
      "an RGBA8 override is refused for usage {} (TD_HDRI or TD_HDR_LIGHTPROBE): the "
      "engine reads an HDR usage's texels as R11G11B10F packed from an EXR or HDR "
      "source's floats, never from 8-bit pixels",
      name.usage)};
  }
  // D103: refused before the conversion when level 0 alone passes the cap
  const auto opts = engineImageOpts(name.usage);
  if (const auto level0 = texels * storedTexelBytes(opts.format);
      level0 > UploadBytesPerFrame)
  {
    return Error{capRefusal(0, level0, faces)};
  }

  auto pixels = SourcePixels{};
  pixels.width = raw.width;
  pixels.height = raw.height;
  pixels.faces = faces;
  pixels.usage = name.usage;
  pixels.sourceTime = 0;
  pixels.pixels =
    raw.format == ImagePixelFormat::RGBA32F ? packEngineR11G11B10F(raw.bytes) : raw.bytes;
  return sourceLevels(decls, name, key, std::move(pixels))
         | kdl::and_then(
           [](CachedImage image) -> Result<std::shared_ptr<const CachedImage>> {
             if (auto problem = levelCapProblem(image); !problem.empty())
             {
               return Error{std::move(problem)};
             }
             return std::make_shared<const CachedImage>(std::move(image));
           });
}

// C39 (b): GPU-ready levels, taken as they are once the cache's test and the upload's
// accept them
Result<std::shared_ptr<const CachedImage>> readyOverride(
  const ImageCacheName& name, const std::shared_ptr<const CachedImage>& ready)
{
  if (!ready)
  {
    return Error{"an override is refused: it holds no image"};
  }
  const auto& image = *ready;
  const auto opts = engineImageOpts(name.usage);
  // Image_load.cpp:674-680, the test a .bimage must pass for the usage (A3)
  if (!engineCacheMatches(image, name.usage, name.textureType))
  {
    if (image.colorFormat != opts.colorFormat)
    {
      return Error{fmt::format(
        "a GPU-ready override is refused: its colour format {} is not the {} DeriveOpts "
        "gives usage {} (Image_load.cpp:674-680: a .bimage of another colour format is "
        "binarized again, and the draw decodes the usage's layout)",
        image.colorFormat,
        opts.colorFormat,
        name.usage)};
    }
    if (image.textureType != name.textureType)
    {
      return Error{fmt::format(
        "a GPU-ready override is refused: its texture type {} is not the key's {} (6 "
        "faces exactly for a cube key; Image_load.cpp:674-680)",
        image.textureType,
        name.textureType)};
    }
    return Error{fmt::format(
      "a GPU-ready override is refused: its format {} is neither usage {}'s {} nor that "
      "format's uncompressed twin (RGBA8 for RGB565, R11G11B10F for BC6H: "
      "Image_load.cpp:676-678)",
      image.format,
      name.usage,
      opts.format)};
  }
  // the binarizer's records: level-major, each level's faces in side order
  const auto faces = image.textureType == TypeCubic ? 6 : 1;
  if (
    image.numLevels < 1
    || image.levels.size() != std::size_t(image.numLevels) * std::size_t(faces))
  {
    return Error{fmt::format(
      "a GPU-ready override is refused: its {} level records are not {} level(s) x {} "
      "face(s)",
      image.levels.size(),
      image.numLevels,
      faces)};
  }
  for (std::size_t k = 0; k < image.levels.size(); ++k)
  {
    const auto& level = image.levels[k];
    const auto at = int(k) / faces;
    const auto side = int(k) % faces;
    const auto width = std::max(1, image.width >> at);
    const auto height = std::max(1, image.height >> at);
    if (
      level.level != at || level.destZ != side || level.width != width
      || level.height != height)
    {
      return Error{fmt::format(
        "a GPU-ready override is refused: record {} is level {} side {} of {} x {}, "
        "where the binarizer writes level {} side {} of {} x {}",
        k,
        level.level,
        level.destZ,
        level.width,
        level.height,
        at,
        side,
        width,
        height)};
    }
  }
  auto problem = std::string{};
  engineTextureDesc(image, false, name.generatedName) | kdl::if_error([&](const auto& e) {
    problem = "a GPU-ready override is refused: " + e.msg;
  }) | kdl::ignore();
  if (!problem.empty())
  {
    return Error{std::move(problem)};
  }
  if (auto capped = levelCapProblem(image); !capped.empty())
  {
    return Error{std::move(capped)};
  }
  return ready;
}

} // namespace

std::string feedKeyName(const std::string& name)
{
  // ImageManager.cpp:417-423: strip any .tga file extensions from anywhere in the _name,
  // including image program parameters
  auto folded = name;
  for (const auto extension : ImageFormats)
  {
    folded = removeAll(folded, extension);
  }
  std::replace(folded.begin(), folded.end(), '\\', '/');
  // :432: the records' names are compared without case
  return lowercased(std::move(folded));
}

std::string feedMatchName(const MaterialImageKey& key)
{
  return lowercased(key.name);
}

std::string feedNameRefusal(const std::string& name)
{
  if (name.empty() || name == "default")
  {
    return fmt::format(
      "an override of \"{}\" is refused: the name is the engine's default image "
      "(ImageManager.cpp:403-407)",
      name);
  }
  if (name.front() == '_')
  {
    return fmt::format(
      "an override of \"{}\" is refused: a name that starts with '_' is one of the "
      "engine's built-in images, which match any options and are never read from files "
      "(ImageManager.cpp:436-439)",
      name);
  }
  return {};
}

Result<std::shared_ptr<const CachedImage>> overrideImage(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  const ImageOverride& image)
{
  if (name.generated)
  {
    return Error{"an override is refused: the engine generates this image itself"};
  }
  if (name.cube == Cube2DArray || name.cube == CubePackedMipChain)
  {
    return Error{
      "an override is refused: an image array or packed mip chain comes from the "
      "engine's bakes"};
  }
  return std::visit(
    [&](const auto& form) -> Result<std::shared_ptr<const CachedImage>> {
      if constexpr (std::is_same_v<std::decay_t<decltype(form)>, RawImagePixels>)
      {
        return rawOverride(decls, name, key, form);
      }
      else
      {
        return readyOverride(name, form);
      }
    },
    image.image);
}

} // namespace tb::lit
