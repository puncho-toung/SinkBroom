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

#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tb::lit
{

// LPF.0a (C39 (a)): the texel format of an override's raw pixels
enum class ImagePixelFormat
{
  // 4 bytes a texel, for every usage but the HDR ones
  RGBA8,
  // four floats a texel, for TD_HDRI and TD_HDR_LIGHTPROBE only: packed to R11G11B10F
  // by the copied float3_to_r11g11b10f, the alpha dropped, as LoadEXR packs
  // (Image_files.cpp:604-621)
  RGBA32F,
};

/**
 * LPF.0a (C39 (a)): an override's raw pixels, as the engine's loaders hand a picture to
 * the binarizer: width × height texels a face, rows top first (the picture LoadTGA
 * returns after its flip), alpha 255 where the source has none; 1 face, or 6 for a cube
 * key in R_LoadCubeImages' side order and orientation (Image_files.cpp:1156-1201, the
 * six-file loop :1317-1403: _px, _nx, _py, _ny, _pz, _nz, CF_CAMERA's sides already
 * turned into those), the order LoadCubeFromMemory takes them in. bytes holds faces ×
 * height × width texels.
 */
struct RawImagePixels
{
  int width = 0;
  int height = 0;
  int faces = 1;
  ImagePixelFormat format = ImagePixelFormat::RGBA8;
  std::vector<std::uint8_t> bytes;
};

/**
 * LPF.0a (C39): an image handed to the material service under a game image name, in
 * place of its files: (a) raw pixels, converted on the parse thread as the source
 * fallback converts a source (the engine's layouts and mip rule for the usage the copied
 * time call gives the key, no encoder: K34, M-IMG-DIFF-05), or (b) GPU-ready levels in
 * the shape a .bimage reads into (CachedImage, as rawIntrinsicImage hands them: the
 * usage's format or its uncompressed twin, its colour format, 6 faces exactly for a cube
 * key; D24: a compressed image is compressed by the caller). editorSize (D111) stands for
 * the files' size as C21's editor-image size of the name.
 */
struct ImageOverride
{
  std::variant<RawImagePixels, std::shared_ptr<const CachedImage>> image;
  std::optional<EditorImageSize> editorSize;
};

/**
 * LPF.0a (C39, D97): the name an image override is stored and matched under: as
 * idImageManager::ImageFromFile folds a name before it compares it with its records'
 * (ImageManager.cpp:417-423: every ".tga", ".jpg", ".png", ".exr" and ".hdr" removed
 * wherever it stands, case-sensitively as idStr::Replace compares, Str.cpp:937-981, then
 * backslashes made slashes), then lowercased, since the records' names are compared
 * without case (ImageManager.cpp:432, idStr::Icmp). An image key matches an override
 * when its own name lowercased (feedMatchName: a record's name is folded already) equals
 * this: the whole name, so a program key (addnormals( … ), heightmap( … )) matches only
 * by its whole program text, never by one of its leaves. Pure; any thread.
 */
std::string feedKeyName(const std::string& name);

// LPF.0a: the name an image key is matched under: its name, lowercased. Pure.
std::string feedMatchName(const MaterialImageKey& key);

/**
 * LPF.0a (P5.16): why an override of this name is refused at hand-over, or "" when it is
 * not: an empty name and "default" are the default image (ImageManager.cpp:403-407), and
 * a name that starts with '_' is one of the engine's built-ins, which match any options
 * and are never read from files (:436-439), the preview's intrinsic stand-ins among them.
 * name is feedKeyName's. Pure.
 */
std::string feedNameRefusal(const std::string& name);

/**
 * LPF.0a (C39, D103, P5.5, P5.6, P5.25): an override's image for one key, as its slot
 * holds it, given the key's cache name from the copied time call
 * (MaterialDecls::imageCacheName: the usage after the _s -> _rmao rename and a program's
 * usage, the texture type). (a) Raw pixels are converted by sourceLevels with name.usage,
 * as the source fallback converts a loaded source (sourceImage): RGBA8 for every usage
 * but TD_HDRI and TD_HDR_LIGHTPROBE, whose RGBA32F is packed by packEngineR11G11B10F
 * first; the cross pairs are refused (the engine has no such source). (b) A GPU-ready
 * image is taken as it is when it passes the cache's own test (engineCacheMatches,
 * Image_load.cpp:674-680: the usage's colour format, its format or that format's
 * uncompressed twin, the key's texture type), its records are the binarizer's (numLevels
 * x faces, level-major, 6 faces exactly for a cube) and the upload takes them
 * (engineTextureDesc, levelUploadBytes). Either is refused when a level, its bytes summed
 * over its faces, passes UploadBytesPerFrame, the render side's constant (mirror.json
 * M-TEX-DIFF-02): a raw RGBA8 2D side above 2896, a raw RGBA8 cube side above 1182. An
 * image the engine generates or bakes (an intrinsic, an array, a packed mip chain) is
 * refused. Every refusal names its rule. Parse thread (C20: sourceImageLevels and the
 * packing are copied code).
 */
Result<std::shared_ptr<const CachedImage>> overrideImage(
  MaterialDecls& decls,
  const ImageCacheName& name,
  const MaterialImageKey& key,
  const ImageOverride& image);

} // namespace tb::lit
