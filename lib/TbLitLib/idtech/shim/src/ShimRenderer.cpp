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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderProgs.cpp,
// renderer/NVRHI/RenderProgs_NVRHI.cpp, renderer/ImageManager.cpp and
// renderer/Image_intrinsic.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The renderer services for the copied code: image records keyed as the engine keys them,
// the render program lists, the R_LoadImage stub, cinematic and GUI dummies, and the
// game.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

idImageManager* globalImages = nullptr;
idRenderProgManager* tbLitRenderProgManager = nullptr;
idUserInterfaceManager* uiManager = nullptr;
idGame* game = nullptr;

namespace
{

// RenderProgs.cpp:791-953: name, nameOutSuffix and stages of every builtin, in table
// order
struct BuiltinShader
{
  const char* name;
  const char* nameOutSuffix;
  rpStage_t stages;
};

constexpr BuiltinShader BuiltinShaders[] = {
  {"builtin/gui", "", SHADER_STAGE_DEFAULT},
  {"builtin/color", "", SHADER_STAGE_DEFAULT},
  {"builtin/color", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/vertex_color", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lighting_IBL", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lighting_IBL", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lighting_IBL", "_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lighting_IBL", "_PBR_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lightgrid_IBL", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lightgrid_IBL", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lightgrid_IBL", "_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/ambient_lightgrid_IBL", "_PBR_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/gbuffer", "", SHADER_STAGE_DEFAULT},
  {"builtin/gbuffer", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/texture", "", SHADER_STAGE_DEFAULT},
  {"builtin/texture_color", "", SHADER_STAGE_DEFAULT},
  {"builtin/texture_color", "_sRGB", SHADER_STAGE_DEFAULT},
  {"builtin/texture_color", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/texture_color_texgen", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interaction", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interaction", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionAmbient", "", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionAmbient", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interaction", "_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interaction", "_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionAmbient", "_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionAmbient", "_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_spot", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_spot_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_point", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_point_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_parallel", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_parallel_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_spot_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_spot_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_point_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_point_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_parallel_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_parallel_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_spot", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_spot_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_point", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_point_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_parallel", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_parallel_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_spot_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_spot_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_point_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_point_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_parallel_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/lighting/interactionSM", "_atlas_parallel_skinned_PBR", SHADER_STAGE_DEFAULT},
  {"builtin/debug/lightgrid", "", SHADER_STAGE_DEFAULT},
  {"builtin/debug/lightgrid", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/debug/octahedron", "", SHADER_STAGE_DEFAULT},
  {"builtin/debug/octahedron", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/environment", "", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/environment", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment", "", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment2", "", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment2", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment2", "_SSR", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/bumpyenvironment2", "_SSR_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/depth", "", SHADER_STAGE_DEFAULT},
  {"builtin/depth", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/fog/blendlight", "", SHADER_STAGE_DEFAULT},
  {"builtin/fog/blendlight", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/fog/fog", "", SHADER_STAGE_DEFAULT},
  {"builtin/fog/fog", "_skinned", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/skybox", "", SHADER_STAGE_DEFAULT},
  {"builtin/legacy/wobblesky", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/postprocess", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/retro_2bit", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/retro_c64", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/retro_cpc", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/retro_genesis", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/retro_ps1", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/crt_mattias", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/crt_newpixie", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/crt_advanced", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/screen", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/tonemap", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/tonemap", "_brightpass", SHADER_STAGE_DEFAULT},
  {"builtin/post/hdr_glare_chromatic", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/tonemap", "_debug", SHADER_STAGE_DEFAULT},
  {"builtin/post/SMAA_edge_detection", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/SMAA_blending_weight_calc", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/SMAA_final", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/motionBlur", "_vectors", SHADER_STAGE_DEFAULT},
  {"builtin/post/taa", "", SHADER_STAGE_COMPUTE},
  {"builtin/post/taa", "_msaa2x", SHADER_STAGE_COMPUTE},
  {"builtin/post/taa", "_msaa4x", SHADER_STAGE_COMPUTE},
  {"builtin/post/taa", "_msaa8x", SHADER_STAGE_COMPUTE},
  {"builtin/SSAO/AmbientOcclusion_AO", "", SHADER_STAGE_DEFAULT},
  {"builtin/SSAO/AmbientOcclusion_AO", "_write", SHADER_STAGE_DEFAULT},
  {"builtin/SSAO/AmbientOcclusion_blur", "", SHADER_STAGE_DEFAULT},
  {"builtin/SSAO/AmbientOcclusion_blur", "_write", SHADER_STAGE_DEFAULT},
  {"builtin/SSGI/DeepGBufferRadiosity_radiosity", "", SHADER_STAGE_DEFAULT},
  {"builtin/SSGI/DeepGBufferRadiosity_blur", "", SHADER_STAGE_DEFAULT},
  {"builtin/SSGI/DeepGBufferRadiosity_blur", "_write", SHADER_STAGE_DEFAULT},
  {"builtin/VR/stereoDeGhost", "", SHADER_STAGE_DEFAULT},
  {"builtin/VR/stereoWarp", "", SHADER_STAGE_DEFAULT},
  {"builtin/video/bink", "", SHADER_STAGE_DEFAULT},
  {"builtin/video/bink", "_sRGB", SHADER_STAGE_DEFAULT},
  {"builtin/video/bink_gui", "", SHADER_STAGE_DEFAULT},
  {"builtin/VR/stereoInterlace", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/motionBlur", "", SHADER_STAGE_DEFAULT},
  {"builtin/debug/debug_shadowmap", "", SHADER_STAGE_DEFAULT},
  {"builtin/blit", "", SHADER_STAGE_FRAGMENT},
  {"builtin/rect", "", SHADER_STAGE_VERTEX},
  {"builtin/mipmapgen", "", SHADER_STAGE_COMPUTE},
  {"builtin/post/tonemapping", "", SHADER_STAGE_DEFAULT},
  {"builtin/post/tonemapping", "_tex_array", SHADER_STAGE_DEFAULT},
  {"builtin/post/histogram", "", SHADER_STAGE_COMPUTE},
  {"builtin/post/histogram", "_tex_array", SHADER_STAGE_COMPUTE},
  {"builtin/post/exposure", "", SHADER_STAGE_COMPUTE},
};

// Image_intrinsic.cpp:1060-1161, CreateIntrinsicImages: the names ImageFromFunction
// registers, in order; shadowMapResolutions (Framebuffer.h:48) completes the shadow names
constexpr const char* IntrinsicImagesBeforeShadows[] = {
  "_default",
  "_white",
  "_black",
  "_blackDiffuse",
  "_cyan",
  "_redClay",
  "_flat",
  "_alphaNotch",
  "_fog",
  "_fogEnter",
  "_noFalloff",
  "_quadratic",
  "_shadowMapAtlas",
};

constexpr int ShadowMapResolutions[] = {1024, 512, 256, 256, 128};

constexpr const char* IntrinsicImagesAfterShadows[] = {
  "_jitter1",
  "_jitter4",
  "_jitter16",
  "_random256",
  "_blueNoise256",
  "_currentRenderHDR",
  "_currentRenderLDR",
  "_taaMotionVectors",
  "_taaResolved",
  "_taaFeedback1",
  "_taaFeedback2",
  "_envprobeHDR",
  "_envprobeDepth",
  "_bloomRender0",
  "_bloomRender1",
  "_heatmap5",
  "_heatmap7",
  "_grain1",
  "_smaaInput",
  "_smaaArea",
  "_smaaSearch",
  "_smaaEdges",
  "_smaaBlend",
  "_currentNormals",
  "_ao0",
  "_ao1",
  "_cszBuffer",
  "_imguiFont",
  "_chromeSpec",
  "_plasticSpec",
  "_brdfLut",
  "_defaultUACIrradiance",
  "_defaultUACRadiance",
  "_scratch",
  "_scratch2",
  "_accum",
  "_currentRender",
  "_currentDepth",
};

constexpr const char* IntrinsicImagesAfterLoadingIcons[] = {
  "_guiEdit",
  "_guiEditDepthStencil",
};

// ImageManager.cpp:359 and :420
const char* const ImageFormats[] = {".tga", ".jpg", ".png", ".exr", ".hdr"};

} // namespace

idImage::idImage(const char* name)
  : imgName{name}
{
}

idImageManager::idImageManager() = default;

idImageManager::~idImageManager()
{
  Shutdown();
}

// ImageManager.cpp:400-499, without the loading: the record is keyed exactly as the
// engine keys it, and no pixels are read
idImage* idImageManager::ImageFromFile(
  const char* _name,
  textureFilter_t filter,
  textureRepeat_t repeat,
  textureUsage_t usage,
  cubeFiles_t cubeMap,
  int cubeMapSize)
{
  if (
    !_name || !_name[0] || idStr::Icmp(_name, "default") == 0
    || idStr::Icmp(_name, "_default") == 0)
  {
    declManager->MediaPrint("DEFAULTED\n");
    return globalImages->defaultImage;
  }
  if (idStr::Icmpn(_name, "fonts", 5) == 0 || idStr::Icmpn(_name, "newfonts", 8) == 0)
  {
    usage = TD_FONT;
  }
  if (idStr::Icmpn(_name, "lights", 6) == 0)
  {
    usage = TD_LIGHT;
  }

  // strip any .tga file extensions from anywhere in the _name, including image program
  // parameters. The engine uses an idStrStatic<MAX_OSPATH>, which would free its static
  // buffer if a name reached 1024 characters; idStr gives the same name for every shorter
  // one, and the copied parser passes at most MAX_IMAGE_NAME - 1.
  idStr name = _name;
  for (const auto* ext : ImageFormats)
  {
    name.Replace(ext, "");
  }
  name.BackSlashesToSlashes();

  // see if the image is already loaded
  const int hash = name.FileNameHash();
  for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i))
  {
    idImage* image = images[i];
    if (name.Icmp(image->GetName()) == 0)
    {
      // the built in's, like _white and _flat always match the other options
      if (name[0] == '_')
      {
        return image;
      }
      if (image->cubeFiles != cubeMap)
      {
        common->Error(
          "Image '%s' has been referenced with conflicting cube map states", _name);
      }

      if (image->filter != filter || image->repeat != repeat)
      {
        // we might want to have the system reset these parameters on every bind and
        // share the image data
        continue;
      }
      if (image->usage != usage)
      {
        // If an image is used differently then we need 2 copies of it because usage
        // affects the way it's compressed and swizzled
        continue;
      }

      image->usage = usage;
      return image;
    }
  }

  // create a new image
  idImage* image = AllocImage(name);
  image->cubeFiles = cubeMap;
  image->cubeMapSize = cubeMapSize;
  image->usage = usage;
  image->filter = filter;
  image->repeat = repeat;

  declManager->MediaPrint("%s\n", image->GetName());

  return image;
}

// ImageManager.cpp:355-390, without the generator: the record of an intrinsic name
idImage* idImageManager::ImageFromFunction(const char* _name)
{
  idStr name = _name;
  for (const auto* ext : ImageFormats)
  {
    name.Replace(ext, "");
  }
  name.BackSlashesToSlashes();

  const int hash = name.FileNameHash();
  for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i))
  {
    idImage* image = images[i];
    if (name.Icmp(image->GetName()) == 0)
    {
      return image;
    }
  }

  idImage* image = AllocImage(name);
  image->intrinsic = true;
  return image;
}

// ImageManager.cpp:310-324
idImage* idImageManager::AllocImage(const char* name)
{
  if (strlen(name) >= MAX_IMAGE_NAME)
  {
    common->Error("idImageManager::AllocImage: \"%s\" is too long\n", name);
  }

  const int hash = idStr(name).FileNameHash();

  auto* image = new (TAG_IMAGE) idImage(name);

  imageHash.Add(hash, images.Append(image));

  return image;
}

void idImageManager::Init()
{
  CreateIntrinsicImages();
}

void idImageManager::CreateIntrinsicImages()
{
  for (const auto* name : IntrinsicImagesBeforeShadows)
  {
    ImageFromFunction(name);
  }
  for (int i = 0; i < int(sizeof(ShadowMapResolutions) / sizeof(ShadowMapResolutions[0]));
       ++i)
  {
    ImageFromFunction(va("_shadowMapArray%i_%i", i, ShadowMapResolutions[i]));
  }
  for (const auto* name : IntrinsicImagesAfterShadows)
  {
    ImageFromFunction(name);
  }

  defaultImage = ImageFromFunction("_default");
  whiteImage = ImageFromFunction("_white");
  blackImage = ImageFromFunction("_black");
  flatNormalMap = ImageFromFunction("_flat");
  currentRenderImage = ImageFromFunction("_currentRender");

  // save a copy of this for material comparison, because currentRenderImage may get
  // reassigned during stereo rendering
  originalCurrentRenderImage = currentRenderImage;

  ImageFromFile("textures/loadingicon2", TF_DEFAULT, TR_CLAMP, TD_DEFAULT, CF_2D);
  ImageFromFile("textures/loadingicon3", TF_DEFAULT, TR_CLAMP, TD_DEFAULT, CF_2D);

  for (const auto* name : IntrinsicImagesAfterLoadingIcons)
  {
    ImageFromFunction(name);
  }
}

void idImageManager::Shutdown()
{
  images.DeleteContents(true);
  imageHash.Free();
  defaultImage = nullptr;
  whiteImage = nullptr;
  blackImage = nullptr;
  flatNormalMap = nullptr;
  currentRenderImage = nullptr;
  originalCurrentRenderImage = nullptr;
}

// RenderProgs.cpp:955-1010 (the builtin loop of idRenderProgManager::Init): every
// builtin's shaders are found in table order, so FindShader indices match the engine's
idRenderProgManager::idRenderProgManager()
{
  // RenderProgs.cpp:393 for the one layout the copied stage parser names: rpNominalSet8
  // (RenderProgs.h:286) holds 13 vec4s, 208 bytes, against the push-constant limit
  // DeviceManager_VK.cpp:1381 sets, min(device limit, nvrhi::c_MaxPushConstantSize). The
  // device is unknown here, so the limit is NVRHI's own, 256 outside Apple. The value
  // only reaches the macro list the copied code passes to FindShader, which FindShader
  // ignores; LP2 applies the engine's rule per device when it picks blob permutations.
  constexpr auto rpBufSize = 13 * 16;
  constexpr auto maxPushConstantSize = 256;
  layoutTypeAttributes[BINDING_LAYOUT_POST_PROCESS_INGAME].pcEnabled =
    rpBufSize <= maxPushConstantSize;

  const auto noMacros = idList<shaderMacro_t>{};
  for (const auto& builtin : BuiltinShaders)
  {
    renderProg_t prog;
    prog.name = builtin.name;
    prog.builtin = true;
    const int index = renderProgs.Append(prog);

    if (builtin.stages & SHADER_STAGE_VERTEX)
    {
      renderProgs[index].vertexShaderIndex = FindShader(
        builtin.name, SHADER_STAGE_VERTEX, builtin.nameOutSuffix, noMacros, true);
    }
    if (builtin.stages & SHADER_STAGE_FRAGMENT)
    {
      renderProgs[index].fragmentShaderIndex = FindShader(
        builtin.name, SHADER_STAGE_FRAGMENT, builtin.nameOutSuffix, noMacros, true);
    }
    if (builtin.stages & SHADER_STAGE_COMPUTE)
    {
      renderProgs[index].computeShaderIndex = FindShader(
        builtin.name, SHADER_STAGE_COMPUTE, builtin.nameOutSuffix, noMacros, true);
    }
  }
}

// RenderProgs.cpp:1157-1185, without loading the shader
int idRenderProgManager::FindShader(
  const char* name,
  rpStage_t stage,
  const char* nameOutSuffix,
  const idList<shaderMacro_t>&,
  bool builtin,
  vertexLayoutType_t)
{
  idStr shaderName(name);
  shaderName.StripFileExtension();

  for (int i = 0; i < shaders.Num(); i++)
  {
    shader_t& shader = shaders[i];
    if (
      shader.name.Icmp(shaderName) == 0 && shader.stage == stage
      && shader.nameOutSuffix.Icmp(nameOutSuffix) == 0)
    {
      return i;
    }
  }

  shader_t shader;
  shader.name = shaderName;
  shader.nameOutSuffix = nameOutSuffix;
  shader.builtin = builtin;
  shader.stage = stage;

  return shaders.Append(shader);
}

// NVRHI/RenderProgs_NVRHI.cpp:285-302, without loading the program
int idRenderProgManager::FindProgram(
  const char* name, int vIndex, int fIndex, bindingLayoutType_t bindingType)
{
  for (int i = 0; i < renderProgs.Num(); ++i)
  {
    if (
      (renderProgs[i].vertexShaderIndex == vIndex)
      && (renderProgs[i].fragmentShaderIndex == fIndex))
    {
      return i;
    }
  }

  renderProg_t program;
  program.name = name;
  program.bindingLayoutType = bindingType;
  program.vertexShaderIndex = vIndex;
  program.fragmentShaderIndex = fIndex;
  return renderProgs.Append(program);
}

const char* idRenderProgManager::ShaderName(const int index) const
{
  return index >= 0 && index < shaders.Num() ? shaders[index].name.c_str() : "";
}

// TEMPORARY until LP1c copies Image_files.cpp: no image file is ever read. The image
// programs' pixel path (R_LoadImageProgram with a pic) gets "no image" and the
// file-not-found timestamp, as for a missing file; the parse path never calls it.
void R_LoadImage(
  const char*,
  byte** pic,
  int* width,
  int* height,
  ID_TIME_T* timestamp,
  bool,
  textureUsage_t*)
{
  if (pic)
  {
    *pic = nullptr;
  }
  if (width)
  {
    *width = 0;
  }
  if (height)
  {
    *height = 0;
  }
  if (timestamp)
  {
    *timestamp = FILE_NOT_FOUND_TIMESTAMP;
  }
}

idCinematic* idCinematic::Alloc()
{
  return new idCinematic;
}

bool idCinematic::InitFromFile(const char* qpath, bool looping, nvrhi::ICommandList*)
{
  m_fileName = qpath;
  m_looping = looping;
  return true;
}

namespace tb::lit::shim
{

UserInterfaceManagerShim::~UserInterfaceManagerShim()
{
  Shutdown();
}

// UserInterface.cpp:268-310: a GUI of this source file is shared unless a unique one is
// asked for; loading always succeeds for a non-empty name (InitFromFile returns true and
// falls back to an "Invalid GUI" desktop)
idUserInterface* UserInterfaceManagerShim::FindGui(
  const char* qpath, bool autoLoad, bool needUnique, bool forceUnique)
{
  for (int i = 0; i < m_guis.Num(); ++i)
  {
    if (!idStr::Icmp(m_guis[i]->GetSourceFile(), qpath))
    {
      if (!forceUnique && needUnique)
      {
        break;
      }
      return m_guis[i];
    }
  }

  if (autoLoad && qpath && *qpath)
  {
    auto* gui = new (TAG_OLD_UI) idUserInterface(qpath);
    m_guis.Append(gui);
    return gui;
  }
  return nullptr;
}

void UserInterfaceManagerShim::Shutdown()
{
  m_guis.DeleteContents(true);
}

void GameShim::CacheDictionaryMedia(const idDict*) {}

} // namespace tb::lit::shim
