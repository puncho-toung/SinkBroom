/*
 Copyright (C) 2010 Kristian Duske

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

#include "mdl/LoadMaterialCollections.h"

#include "base/Logger.h"
#include "fs/FileSystem.h"
#include "fs/PathInfo.h"
#include "fs/PathMatcher.h"
#include "fs/TraversalMode.h"
#include "gl/MaterialCollection.h"
#include "gl/Resource.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/GameConfig.h"
#include "mdl/LoadMipTexture.h"
#include "mdl/LoadShaders.h"
#include "mdl/LoadTexture.h"
#include "mdl/MaterialUtils.h"
#include "mdl/Palette.h"
#include "mdl/Quake3Shader.h"

#include "kd/contracts.h"
#include "kd/functional.h"
#include "kd/path_hash.h"
#include "kd/path_utils.h"
#include "kd/ranges/as_rvalue_view.h"
#include "kd/ranges/chunk_by_view.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/result_fold.h"
#include "kd/string_compare.h"
#include "kd/string_compare_natural.h"
#include "kd/vector_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tb::mdl
{

namespace
{

Result<std::optional<Palette>> loadPalette(
  const fs::FileSystem& fs, const MaterialConfig& materialConfig)
{
  if (materialConfig.palette.empty())
  {
    return Result<std::optional<Palette>>{std::nullopt};
  }

  return fs.openFile(materialConfig.palette) | kdl::and_then([&](auto file) {
           return mdl::loadPalette(*file, materialConfig.palette);
         })
         | kdl::transform([](auto palette) { return std::optional{std::move(palette)}; });
}

bool shouldExclude(
  const std::string& materialName, const std::vector<std::string>& patterns)
{
  return std::ranges::any_of(patterns, [&](const auto& pattern) {
    return kdl::ci::str_matches_glob(materialName, pattern);
  });
}

Result<std::vector<std::filesystem::path>> findTexturePaths(
  const fs::FileSystem& fs, const MaterialConfig& materialConfig)
{
  return fs.find(
           materialConfig.root,
           fs::TraversalMode::Recursive,
           fs::makeExtensionPathMatcher(materialConfig.extensions))
         | kdl::transform([&](auto paths) {
             return paths | std::views::filter([&](const auto& path) {
                      return !shouldExclude(
                        path.stem().string(), materialConfig.excludes);
                    })
                    | kdl::views::as_rvalue | kdl::ranges::to<std::vector>();
           });
}

Result<std::vector<std::filesystem::path>> findAllMaterialPaths(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::vector<Quake3Shader>& shaders)
{
  return findTexturePaths(fs, materialConfig)
         | kdl::transform([&](const auto& texturePaths) {
             // The keys ignore case like the file system and the material manager do, so
             // that a shader replaces an image of the same name in another case instead
             // of yielding a second material that the manager can't tell apart.
             auto pathStemToPath = std::unordered_map<
               std::filesystem::path,
               std::filesystem::path,
               kdl::path_hash>{};
             for (const auto& texturePath : texturePaths)
             {
               pathStemToPath[kdl::path_to_lower(
                 kdl::path_remove_extension(texturePath))] = texturePath;
             }
             for (const auto& shader : shaders)
             {
               pathStemToPath[kdl::path_to_lower(shader.shaderPath)] = shader.shaderPath;
             }
             return kdl::vec_sort(
               pathStemToPath | std::views::values | kdl::ranges::to<std::vector>());
           });
}

Result<std::filesystem::path> findShaderTexture(
  const std::filesystem::path& texturePath,
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig)
{
  if (texturePath.empty())
  {
    return Error{"Empty texture path"};
  }

  if (
    kdl::ranges::contains(
      materialConfig.extensions, kdl::path_to_lower(texturePath.extension()))
    && fs.pathInfo(texturePath) == fs::PathInfo::File)
  {
    return texturePath;
  }

  const auto directoryPath = texturePath.parent_path();
  const auto basename = texturePath.stem().string();
  return fs.find(
           texturePath.parent_path(),
           fs::TraversalMode::Flat,
           kdl::logical_and(
             fs::makeFilenamePathMatcher(basename + ".*"),
             fs::makeExtensionPathMatcher(materialConfig.extensions)))
         | kdl::and_then([&](auto candidates) -> Result<std::filesystem::path> {
             if (!candidates.empty())
             {
               return candidates.front();
             }
             return Error{fmt::format("File not found: {}", texturePath)};
           });
}

Result<std::filesystem::path> findShaderTexture(
  const std::vector<Quake3ShaderStage>& stages,
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig)
{
  auto path = stages | kdl::first([&](const auto& stage) {
                return findShaderTexture(stage.map, fs, materialConfig);
              });
  if (path)
  {
    return std::move(*path);
  }
  return Error{"Could not find texture file"};
}

/**
 * The image a shader material is previewed with, and whether it is the placeholder
 * because none of the images that the shader names was found.
 */
struct PreviewImage
{
  std::filesystem::path path;
  bool missing = false;
};

PreviewImage findShaderTexture(
  const Quake3Shader& shader,
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig)
{
  return findShaderTexture(shader.editorImage, fs, materialConfig)
         | kdl::or_else(
           [&](auto) { return findShaderTexture(shader.shaderPath, fs, materialConfig); })
         | kdl::or_else(
           [&](auto) { return findShaderTexture(shader.lightImage, fs, materialConfig); })
         | kdl::or_else(
           [&](auto) { return findShaderTexture(shader.stages, fs, materialConfig); })
         | kdl::transform([](auto path) { return PreviewImage{std::move(path), false}; })
         | kdl::transform_error(
           [](auto) { return PreviewImage{DefaultTexturePath, true}; })
         | kdl::value();
}

/**
 * Lists the image files of a directory once, on its first lookup, and answers every
 * later lookup in that directory from the listing. One index serves one load of the
 * material collections, so it never sees a stale file system.
 */
class DirectoryIndex
{
private:
  // lowercase stem -> files with that stem, in file system order
  using StemIndex = std::unordered_map<
    std::filesystem::path,
    std::vector<std::filesystem::path>,
    kdl::path_hash>;

  const fs::FileSystem& m_fs;
  fs::PathMatcher m_extensionMatcher;
  // lowercase directory path -> its files
  std::unordered_map<std::filesystem::path, StemIndex, kdl::path_hash> m_directories;

public:
  DirectoryIndex(
    const fs::FileSystem& fs, const std::vector<std::filesystem::path>& extensions)
    : m_fs{fs}
    , m_extensionMatcher{fs::makeExtensionPathMatcher(extensions)}
  {
  }

  /**
   * Returns the image files in the given directory whose stem equals the given stem,
   * ignoring case.
   */
  const std::vector<std::filesystem::path>& find(
    const std::filesystem::path& directory, const std::filesystem::path& stem)
  {
    static const auto noFiles = std::vector<std::filesystem::path>{};

    const auto& stemIndex = index(kdl::path_to_lower(directory));
    const auto iFiles = stemIndex.find(kdl::path_to_lower(stem));
    return iFiles != stemIndex.end() ? iFiles->second : noFiles;
  }

private:
  const StemIndex& index(const std::filesystem::path& directory)
  {
    if (const auto iStemIndex = m_directories.find(directory);
        iStemIndex != m_directories.end())
    {
      return iStemIndex->second;
    }

    auto stemIndex = StemIndex{};
    // a directory that doesn't exist has no files
    m_fs.find(directory, fs::TraversalMode::Flat, m_extensionMatcher)
      | kdl::transform([&](auto paths) {
          for (auto& path : paths)
          {
            stemIndex[kdl::path_to_lower(path.stem())].push_back(std::move(path));
          }
        })
      | kdl::transform_error([](auto) {});
    return m_directories.emplace(directory, std::move(stemIndex)).first->second;
  }
};

/**
 * Turns an image reference in a Doom 3 material into the path of the image file without
 * a root, or nothing for an image that the engine generates.
 */
std::optional<std::filesystem::path> normalizeScriptImageReference(
  const std::filesystem::path& reference)
{
  // The parser already turned backslashes into separators and removed quotes. The file
  // system ignores case anyway, but the directory index wants one spelling.
  auto path = kdl::path_to_lower(reference.relative_path());

  // idImageManager maps these two generated images to files (ImageFromFile)
  if (path == "_black")
  {
    return "textures/black";
  }
  if (path == "_white")
  {
    return "guis/assets/white";
  }

  // the other names starting with '_' name generated images, such as _flat or _default
  if (const auto& native = path.native(); native.empty() || native.front() == '_')
  {
    return std::nullopt;
  }

  return path;
}

using ImageSet = std::unordered_set<std::filesystem::path, kdl::path_hash>;

/**
 * Returns the name under which the engine's image manager knows the image that a
 * reference names: lowercase and without extension (idImageManager::ImageFromFile).
 */
std::optional<std::filesystem::path> engineImageName(
  const std::filesystem::path& reference)
{
  if (const auto path = normalizeScriptImageReference(reference))
  {
    return kdl::path_remove_extension(*path);
  }
  return std::nullopt;
}

/**
 * Returns the engine names of the images that the given shaders use in diffuse stages.
 */
ImageSet findDiffuseImages(const std::vector<Quake3Shader>& shaders)
{
  auto diffuseImages = ImageSet{};
  for (const auto& shader : shaders)
  {
    for (const auto& stage : shader.stages)
    {
      if (stage.lighting == Quake3ShaderStage::Lighting::Diffuse)
      {
        if (auto name = engineImageName(stage.map))
        {
          diffuseImages.insert(std::move(*name));
        }
      }
    }
  }
  return diffuseImages;
}

/**
 * The state that the materials of one load share.
 */
struct MaterialLoadContext
{
  DirectoryIndex directoryIndex;
  // the engine names of the images that some material uses in a diffuse stage, from all
  // shaders including those outside of the material root; set up front, or built from
  // diffuseImageShaders on first use
  std::optional<ImageSet> diffuseImages = std::nullopt;
  const std::vector<Quake3Shader>* diffuseImageShaders = nullptr;
  // lowercase image path -> the texture resource of every material that shows it
  std::unordered_map<
    std::filesystem::path,
    std::shared_ptr<gl::TextureResource>,
    kdl::path_hash>
    textureResources = {};

  bool isDiffuseImage(const std::filesystem::path& reference)
  {
    if (!diffuseImages)
    {
      diffuseImages =
        diffuseImageShaders ? findDiffuseImages(*diffuseImageShaders) : ImageSet{};
    }

    const auto name = engineImageName(reference);
    return name && diffuseImages->contains(*name);
  }
};

std::optional<std::filesystem::path> findScriptImageFile(
  const std::filesystem::path& path,
  const MaterialConfig& materialConfig,
  DirectoryIndex& directoryIndex)
{
  const auto& candidates = directoryIndex.find(path.parent_path(), path.stem());
  if (candidates.empty())
  {
    return std::nullopt;
  }

  const auto isConfigured = [&](const std::filesystem::path& extension) {
    return std::ranges::any_of(materialConfig.extensions, [&](const auto& configured) {
      return kdl::path_to_lower(configured) == extension;
    });
  };

  const auto findCandidate =
    [&](const std::filesystem::path& extension) -> std::optional<std::filesystem::path> {
    const auto iCandidate = std::ranges::find_if(candidates, [&](const auto& candidate) {
      return kdl::path_to_lower(candidate.extension()) == extension;
    });
    return iCandidate != candidates.end() ? std::optional{*iCandidate} : std::nullopt;
  };

  // A .dds copy keeps its compression and mips on the GPU, so it needs much less memory
  // than the image it stands in for. The engine itself never reads .dds files.
  if (isConfigured(".dds"))
  {
    if (auto candidate = findCandidate(".dds"))
    {
      return candidate;
    }
  }

  // the file as named, then the same name with each configured extension in the order
  // of the config, as the engine tries its image loaders in its own order
  if (const auto extension = kdl::path_to_lower(path.extension());
      isConfigured(extension))
  {
    if (auto candidate = findCandidate(extension))
    {
      return candidate;
    }
  }

  for (const auto& extension : materialConfig.extensions)
  {
    if (auto candidate = findCandidate(kdl::path_to_lower(extension)))
    {
      return candidate;
    }
  }

  return std::nullopt;
}

struct ResolvedScriptImage
{
  std::filesystem::path path;
  // found only under one of materialConfig.shaderImageSearchPaths, not as a loose file
  bool underSearchPath = false;
};

/**
 * Finds the image file that an image reference in a Doom 3 material names, as written and
 * then under each of materialConfig.shaderImageSearchPaths.
 */
std::optional<ResolvedScriptImage> resolveScriptImage(
  const std::filesystem::path& reference,
  const MaterialConfig& materialConfig,
  DirectoryIndex& directoryIndex)
{
  const auto path = normalizeScriptImageReference(reference);
  if (!path)
  {
    return std::nullopt;
  }

  if (auto file = findScriptImageFile(*path, materialConfig, directoryIndex))
  {
    return ResolvedScriptImage{std::move(*file), false};
  }

  for (const auto& searchPath : materialConfig.shaderImageSearchPaths)
  {
    if (
      auto file = findScriptImageFile(searchPath / *path, materialConfig, directoryIndex))
    {
      return ResolvedScriptImage{std::move(*file), true};
    }
  }

  return std::nullopt;
}

bool isEditorImage(const Quake3Shader& shader, const std::filesystem::path& reference)
{
  return !shader.editorImage.empty()
         && kdl::path_to_lower(shader.editorImage) == kdl::path_to_lower(reference);
}

/**
 * Finds the image that a Doom 3 material is previewed with. This follows
 * idMaterial::GetEditorImage, whose choice the parser recorded in previewImages: dmap
 * scales the UVs of a Valve 220 face by the size of that image, so a preview of another
 * size would show the texture misaligned.
 */
PreviewImage findDoom3PreviewImage(
  const Quake3Shader& shader,
  const MaterialConfig& materialConfig,
  MaterialLoadContext& context)
{
  for (const auto& reference : shader.previewImages)
  {
    if (
      auto image = resolveScriptImage(reference, materialConfig, context.directoryIndex))
    {
      // GetEditorImage loads the editor image as a diffuse map (renderer/Material.cpp:258
      // at rbdoom/neo b19c3f76). The image manager keeps one image per usage
      // (idImageManager::ImageFromFile, ImageManager.cpp:453), and retail Doom 3 BFG has
      // no source images, only bimages generated per usage (idImage::GetGeneratedName,
      // Image_load.cpp:525-532). An editor image that exists only as an export of, say,
      // a bump map can't be loaded as a diffuse map, so the engine falls back to the
      // first diffuse stage, whose image may have another size. An image found only
      // under a search path, which is where such exports live, therefore counts as the
      // editor image only if some material uses it in a diffuse stage, so that a diffuse
      // bimage of it exists. A loose source image loads for any usage.
      if (
        image->underSearchPath && isEditorImage(shader, reference)
        && !context.isDiffuseImage(reference))
      {
        continue;
      }
      return {std::move(image->path), false};
    }
  }

  // A material that names no image at all, such as a tool material without stages and
  // editor image, is invisible in game and its UVs don't matter, so an image named like
  // the material is a better preview than the placeholder. The engine shows its default
  // image. A material that names images is never previewed by its own name, because the
  // engine doesn't do that either.
  if (shader.previewImages.empty())
  {
    if (
      auto image =
        resolveScriptImage(shader.shaderPath, materialConfig, context.directoryIndex))
    {
      return {std::move(image->path), false};
    }
  }

  return {DefaultTexturePath, true};
}

void setCulling(gl::Material& material, const Quake3Shader::Culling culling)
{
  // Note that Quake 3 has a different understanding of front and back, so we need to
  // invert them. The Doom 3 parser uses the Quake 3 terms.
  switch (culling)
  {
  case Quake3Shader::Culling::Front:
    material.setCulling(gl::MaterialCulling::Back);
    break;
  case Quake3Shader::Culling::Back:
    material.setCulling(gl::MaterialCulling::Front);
    break;
  case Quake3Shader::Culling::None:
    material.setCulling(gl::MaterialCulling::None);
    break;
  }
}

void setQuake3RenderState(gl::Material& material, const Quake3Shader& shader)
{
  if (!shader.stages.empty())
  {
    const auto& stage = shader.stages.front();
    if (stage.blendFunc.enable())
    {
      material.setBlendFunc(
        gl::getEnum(stage.blendFunc.srcFactor), gl::getEnum(stage.blendFunc.destFactor));
    }
    else
    {
      material.disableBlend();
    }
  }

  const auto& alphaFunc = !shader.stages.empty() && shader.stages.front().alphaFunc
                            ? shader.stages.front().alphaFunc
                            : shader.qerAlphaFunc;
  if (alphaFunc)
  {
    material.setAlphaFunc(alphaFunc->compare, alphaFunc->threshold);
  }
}

bool drawsWithBlendFunc(const Quake3ShaderStage::BlendFunc& blendFunc)
{
  using BlendFunc = Quake3ShaderStage::BlendFunc;

  // "blend none" (GL_ZERO, GL_ONE) draws nothing; hiding the face would only make it
  // impossible to edit
  return blendFunc.enable() && blendFunc.validateSrcFactor()
         && blendFunc.validateDestFactor()
         && !(
           blendFunc.srcFactor == BlendFunc::Zero
           && blendFunc.destFactor == BlendFunc::One);
}

void setDoom3RenderState(gl::Material& material, const Quake3Shader& shader)
{
  using Lighting = Quake3ShaderStage::Lighting;

  // the stage that the preview image most likely comes from
  const auto* governingStage = [&]() -> const Quake3ShaderStage* {
    if (const auto iDiffuse = std::ranges::find(
          shader.stages, Lighting::Diffuse, &Quake3ShaderStage::lighting);
        iDiffuse != shader.stages.end())
    {
      return &*iDiffuse;
    }
    return !shader.stages.empty() ? &shader.stages.front() : nullptr;
  }();

  switch (shader.coverage)
  {
  case Quake3Shader::Coverage::Opaque:
    // the engine draws opaque materials without blending, whatever alpha the image has
    material.disableBlend();
    break;
  case Quake3Shader::Coverage::Perforated: {
    // an explicit alpha function also suppresses the default blending
    const auto iAlphaTested = std::ranges::find_if(
      shader.stages, [](const auto& stage) { return stage.alphaFunc.has_value(); });
    const auto threshold =
      governingStage && governingStage->alphaFunc ? governingStage->alphaFunc->threshold
      : iAlphaTested != shader.stages.end()       ? iAlphaTested->alphaFunc->threshold
                                                  : 0.5f;
    material.setAlphaFunc(gl::MaterialAlphaFunc::Compare::GreaterEqual, threshold);
    break;
  }
  case Quake3Shader::Coverage::Translucent:
    // An unlit stage blends as its script says, such as glass with "blend filter".
    // Otherwise, an image with graduated alpha blends by itself. Translucent materials
    // that should be drawn see-through no matter what are left to a game config tag on
    // coverage_translucent; blending an opaque image would only turn off depth writes.
    if (
      governingStage && governingStage->lighting == Lighting::None
      && drawsWithBlendFunc(governingStage->blendFunc))
    {
      material.setBlendFunc(
        gl::getEnum(governingStage->blendFunc.srcFactor),
        gl::getEnum(governingStage->blendFunc.destFactor));
    }
    break;
  }
}

gl::ResourceLoader<gl::Texture> makeShaderTextureLoader(
  std::filesystem::path path,
  std::string name,
  const fs::FileSystem& fs,
  std::optional<Palette> palette)
{
  // the file system outlives every material and its texture resource
  return
    [&fs, path = std::move(path), name = std::move(name), palette = std::move(palette)]()
      -> Result<gl::Texture> { return loadTexture(path, name, fs, palette); };
}

Result<gl::Material> loadShaderMaterial(
  const Quake3Shader& shader,
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const gl::CreateTextureResource& createResource,
  const std::optional<Palette>& palette,
  MaterialLoadContext& context)
{
  const auto isDoom3 = shader.format == Quake3Shader::Format::Doom3;
  auto previewImage = isDoom3 ? findDoom3PreviewImage(shader, materialConfig, context)
                              : findShaderTexture(shader, fs, materialConfig);

  const auto prefixLength = kdl::path_length(materialConfig.root);
  auto shaderName = getMaterialNameFromPathSuffix(shader.shaderPath, prefixLength);

  // Materials that show the same image share its texture resource, which is decoded and
  // uploaded once. loadTexture picks the decoder by extension, so .dds images keep their
  // compression and mips. Doom 3 images need no palette.
  auto& textureResource = context.textureResources[kdl::path_to_lower(previewImage.path)];
  if (!textureResource)
  {
    textureResource = createResource(makeShaderTextureLoader(
      previewImage.path, shaderName, fs, isDoom3 ? std::nullopt : palette));
  }

  auto material = gl::Material{std::move(shaderName), textureResource};
  material.setSurfaceParms(shader.surfaceParms);
  material.setPreviewImageMissing(previewImage.missing);
  setCulling(material, shader.culling);
  if (isDoom3)
  {
    setDoom3RenderState(material, shader);
  }
  else
  {
    setQuake3RenderState(material, shader);
  }

  return material;
}

Result<gl::Texture> findAndLoadTexture(
  const std::filesystem::path& path,
  const std::string& name,
  const std::vector<std::filesystem::path>& extensions,
  const fs::FileSystem& fs,
  const std::optional<Palette>& palette)
{
  return findMaterialFile(fs, path, extensions)
    .and_then([&](const auto& actualPath) -> Result<gl::Texture> {
      return loadTexture(actualPath, name, fs, palette);
    });
}

gl::ResourceLoader<gl::Texture> makeTextureResourceLoader(
  const std::filesystem::path& path,
  const std::string& name,
  const std::vector<std::filesystem::path>& extensions,
  const fs::FileSystem& fs,
  const std::optional<Palette>& palette)
{
  return [&, path, name, palette]() -> Result<gl::Texture> {
    return findAndLoadTexture(path, name, extensions, fs, palette)
           | kdl::or_else([&](auto e) -> Result<gl::Texture> {
               return Error{fmt::format("Could not load texture '{}': {}", path, e.msg)};
             });
  };
}

Result<gl::Material> loadTextureMaterial(
  const std::filesystem::path& texturePath,
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const gl::CreateTextureResource& createResource,
  const std::optional<Palette>& palette)
{
  const auto prefixLength = kdl::path_length(materialConfig.root);
  const auto pathMatcher = !materialConfig.extensions.empty()
                             ? fs::makeExtensionPathMatcher(materialConfig.extensions)
                             : fs::matchAnyPath;
  auto name = getMaterialNameFromPathSuffix(texturePath, prefixLength);

  auto textureLoader =
    makeTextureResourceLoader(texturePath, name, materialConfig.extensions, fs, palette);
  auto textureResource = createResource(std::move(textureLoader));

  // For the classic id-tech miptex formats, the `{`-prefixed fence-texture naming
  // convention is known from the name alone, so the alpha-test decision can be set here
  // rather than waiting for the (lazily loaded) texture to become ready.
  const auto isMasked = isMipTexture(texturePath) && isMaskedTextureName(name);

  auto material = gl::Material{std::move(name), std::move(textureResource)};
  if (isMasked)
  {
    material.setAlphaFunc(gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.5f);
  }
  return material;
}

std::string materialCollectionName(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath)
{
  const auto prefixLength = kdl::path_length(materialConfig.root);
  if (const auto collectionPath =
        kdl::path_clip(materialPath, prefixLength).parent_path();
      !collectionPath.empty())
  {
    // If the file was loaded from a subdirectory of the root, use the parent path.
    return materialPath.parent_path().generic_string();
  }

  if (const auto* metadata =
        fs.metadata(materialPath, fs::FileSystemMetadataKeys::ImageFilePath);
      metadata && std::holds_alternative<std::filesystem::path>(*metadata))
  {
    if (const auto imageFileName = std::get<std::filesystem::path>(*metadata).filename();
        kdl::path_has_extension(kdl::path_to_lower(imageFileName), ".wad"))
    {
      // If the texture was loaded from a WAD file, use the WAD file name as the
      // collection name.
      return imageFileName.filename().generic_string();
    }
  }

  // Otherwise, just use the root directory name.
  return materialConfig.root.generic_string();
}

std::vector<gl::MaterialCollection> groupMaterialsIntoCollections(
  std::vector<gl::Material> materials)
{
  // Natural comparison folds case and skips whitespace, so distinct names such as
  // "base 1.wad" and "base1.wad" can compare equal. string_less_natural breaks such
  // ties by exact name, which keeps equal collection names adjacent. Otherwise, the
  // chunk_by below could split one collection into several.
  std::ranges::sort(
    materials, kdl::ci::string_less_natural{}, &gl::Material::collectionName);

  return materials | kdl::views::chunk_by([&](const auto& lhs, const auto& rhs) {
           return lhs.collectionName() == rhs.collectionName();
         })
         | std::views::transform([&](auto groupedMaterials) {
             contract_assert(!groupedMaterials.empty());

             auto materialCollectionName = groupedMaterials.front().collectionName();

             auto materialsForCollection = std::vector<gl::Material>(
               std::move_iterator{groupedMaterials.begin()},
               std::move_iterator{groupedMaterials.end()});

             materialsForCollection = kdl::vec_sort(
               std::move(materialsForCollection), [&](const auto& lhs, const auto& rhs) {
                 return lhs.relativePath() < rhs.relativePath();
               });

             return gl::MaterialCollection{
               std::move(materialCollectionName), std::move(materialsForCollection)};
           })
         | kdl::ranges::to<std::vector>();
}

Result<gl::Material> loadMaterial(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath,
  const gl::CreateTextureResource& createResource,
  const Quake3Shader* shader,
  const std::optional<Palette>& palette,
  MaterialLoadContext& context)
{
  return (shader ? loadShaderMaterial(
                     *shader, fs, materialConfig, createResource, palette, context)
                 : loadTextureMaterial(
                     materialPath, fs, materialConfig, createResource, palette))
         | kdl::transform([&](auto material) {
             fs.makeAbsolute(materialPath)
               | kdl::transform([&](auto absPath) { material.setAbsolutePath(absPath); })
               | kdl::or_else([](auto) { return kdl::void_success; });
             material.setRelativePath(materialPath);
             material.setCollectionName(
               materialCollectionName(fs, materialConfig, materialPath));
             return material;
           });
}

const Quake3Shader* findShader(
  const std::vector<Quake3Shader>& shaders, const std::filesystem::path& shaderPath)
{
  // Doom 3 shader paths are lowercase canonical decl names, and an exact match is
  // cheapest
  if (const auto iShader =
        std::ranges::find(shaders, shaderPath, &Quake3Shader::shaderPath);
      iShader != shaders.end())
  {
    return &*iShader;
  }

  // like the file system and the material manager, ignore case
  const auto lowerShaderPath = kdl::path_to_lower(shaderPath);
  const auto iShader = std::ranges::find_if(shaders, [&](const auto& shader) {
    return kdl::path_to_lower(shader.shaderPath) == lowerShaderPath;
  });
  return iShader != shaders.end() ? &*iShader : nullptr;
}

using ShaderIndex =
  std::unordered_map<std::filesystem::path, const Quake3Shader*, kdl::path_hash>;

ShaderIndex makeShaderIndex(const std::vector<Quake3Shader>& shaders)
{
  // keyed by lowercase shader path; of two shaders whose paths differ only in case, the
  // first one wins, as in findShader
  auto shaderIndex = ShaderIndex{};
  for (const auto& shader : shaders)
  {
    shaderIndex.try_emplace(kdl::path_to_lower(shader.shaderPath), &shader);
  }
  return shaderIndex;
}

const Quake3Shader* findShader(
  const ShaderIndex& shaderIndex, const std::filesystem::path& shaderPath)
{
  const auto iShader = shaderIndex.find(kdl::path_to_lower(shaderPath));
  return iShader != shaderIndex.end() ? iShader->second : nullptr;
}

/**
 * What the one line that a load logs reports about the material scripts.
 */
struct ShaderSummary
{
  size_t fileCount = 0;
  size_t shaderCount = 0;
  size_t defaultedCount = 0;
  size_t unterminatedCount = 0;
  size_t shadersUnderRootCount = 0;
};

void logLoadSummary(
  const ShaderSummary& summary,
  const std::vector<gl::Material>& materials,
  const MaterialConfig& materialConfig,
  Logger& logger)
{
  if (summary.fileCount == 0)
  {
    return;
  }

  // missing images are not logged one by one; the material browser can show them
  const auto missingCount = std::ranges::count_if(
    materials, [](const auto& material) { return material.previewImageMissing(); });

  logger.info() << fmt::format(
    "Loaded {} materials from {} files ({} defaulted, {} unterminated); {} under {}/, {} "
    "without a preview image",
    summary.shaderCount,
    summary.fileCount,
    summary.defaultedCount,
    summary.unterminatedCount,
    summary.shadersUnderRootCount,
    materialConfig.root.generic_string(),
    missingCount);
}

} // namespace


Result<gl::Material> loadMaterial(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const std::filesystem::path& materialPath,
  const gl::CreateTextureResource& createResource,
  const std::vector<Quake3Shader>& shaders,
  const std::optional<Palette>& palette)
{
  auto context = MaterialLoadContext{
    .directoryIndex = DirectoryIndex{fs, materialConfig.extensions},
    .diffuseImageShaders = &shaders,
  };
  return loadMaterial(
    fs,
    materialConfig,
    materialPath,
    createResource,
    findShader(shaders, kdl::path_remove_extension(materialPath)),
    palette,
    context);
}

Result<std::vector<gl::MaterialCollection>> loadMaterialCollections(
  const fs::FileSystem& fs,
  const MaterialConfig& materialConfig,
  const gl::CreateTextureResource& createResource,
  kdl::task_manager& taskManager,
  Logger& logger)
{
  return loadShaderFiles(fs, materialConfig, taskManager, logger)
         | kdl::join(loadPalette(fs, materialConfig))
         | kdl::and_then([&](auto loadedShaders, auto palette) {
             auto summary = ShaderSummary{
               .fileCount = loadedShaders.fileCount,
               .shaderCount = loadedShaders.shaders.size(),
               .defaultedCount = size_t(std::ranges::count_if(
                 loadedShaders.shaders,
                 [](const auto& shader) { return shader.engineDefaulted; })),
               .unterminatedCount = loadedShaders.unterminatedCount,
             };

             // from every shader, before those outside of the root are dropped
             auto diffuseImages = findDiffuseImages(loadedShaders.shaders);

             // Shaders outside of the root can't be used on brush faces, but they stay
             // available to entity models, which load their own.
             const auto shaders =
               loadedShaders.shaders | std::views::filter([&](const auto& shader) {
                 return kdl::path_has_prefix(shader.shaderPath, materialConfig.root);
               })
               | kdl::views::as_rvalue | kdl::ranges::to<std::vector>();
             summary.shadersUnderRootCount = shaders.size();

             return findAllMaterialPaths(fs, materialConfig, shaders)
                    | kdl::and_then([&](const auto& materialPaths) {
                        const auto shaderIndex = makeShaderIndex(shaders);
                        auto context = MaterialLoadContext{
                          .directoryIndex = DirectoryIndex{fs, materialConfig.extensions},
                          .diffuseImages = std::move(diffuseImages),
                        };

                        return materialPaths
                               | std::views::transform([&](const auto& materialPath) {
                                   return loadMaterial(
                                     fs,
                                     materialConfig,
                                     materialPath,
                                     createResource,
                                     findShader(
                                       shaderIndex,
                                       kdl::path_remove_extension(materialPath)),
                                     palette,
                                     context);
                                 })
                               | kdl::fold;
                      })
                    | kdl::transform([&](auto materials) {
                        logLoadSummary(summary, materials, materialConfig, logger);
                        return materials;
                      });
           })
         | kdl::transform([&](auto materials) {
             return groupMaterialsIntoCollections(std::move(materials));
           });
}

} // namespace tb::mdl
