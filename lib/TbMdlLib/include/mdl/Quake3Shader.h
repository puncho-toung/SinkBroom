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

#pragma once

#include "gl/Material.h"

#include "kd/flat_set.h"
#include "kd/reflection_decl.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{

class Quake3ShaderStage
{
public:
  struct BlendFunc
  {
    std::string srcFactor;
    std::string destFactor;

    static const std::string One;
    static const std::string Zero;
    static const std::string SrcColor;
    static const std::string DestColor;
    static const std::string OneMinusSrcColor;
    static const std::string OneMinusDestColor;
    static const std::string SrcAlpha;
    static const std::string DestAlpha;
    static const std::string OneMinusSrcAlpha;
    static const std::string OneMinusDestAlpha;
    static const std::string SrcAlphaSaturate;

    bool enable() const;
    bool validateSrcFactor() const;
    bool validateDestFactor() const;
    void reset();

    kdl_reflect_decl(BlendFunc, srcFactor, destFactor);
  };

  /**
   * The light interaction a Doom 3 stage takes part in, set by "blend diffusemap",
   * "blend bumpmap", "blend specularmap" and their aliases, or by the diffusemap,
   * bumpmap and specularmap shorthands. Quake 3 stages are always None.
   */
  enum class Lighting
  {
    None,
    Diffuse,
    Bump,
    Specular
  };

public:
  std::filesystem::path map;
  BlendFunc blendFunc;
  std::optional<gl::MaterialAlphaFunc> alphaFunc;
  Lighting lighting = Lighting::None;

  kdl_reflect_decl(Quake3ShaderStage, map, blendFunc, alphaFunc, lighting);
};

class Quake3Shader
{
public:
  enum class Culling
  {
    Front,
    Back,
    None
  };

  /**
   * The script language a record was parsed from: a Quake 3 .shader file or a Doom 3
   * .mtr file.
   */
  enum class Format
  {
    Quake3,
    Doom3
  };

  /**
   * How a Doom 3 material covers what is behind it, as idMaterial::Coverage computes it.
   * Translucent surfaces do not seal a map, perforated (alpha tested) ones do.
   */
  enum class Coverage
  {
    Opaque,
    Perforated,
    Translucent
  };

public:
  std::filesystem::path shaderPath;
  std::filesystem::path editorImage;
  std::filesystem::path lightImage;
  Culling culling = Culling::Front;
  kdl::flat_set<std::string> surfaceParms;
  std::vector<Quake3ShaderStage> stages;
  std::optional<gl::MaterialAlphaFunc> qerAlphaFunc;
  Format format = Format::Quake3;
  // Doom 3 only.
  Coverage coverage = Coverage::Opaque;
  // Doom 3 only: the images idMaterial::GetEditorImage would try, in that order.
  std::vector<std::filesystem::path> previewImages;
  // Doom 3 only: the engine would replace this material with its default material.
  bool engineDefaulted = false;
  // Doom 3 only: the canonical names of the tables the material's expressions look up.
  // The engine defaults the material if one of them is declared in no decl file.
  std::vector<std::string> referencedTables;

public:
  Quake3ShaderStage& addStage();

  kdl_reflect_decl(
    Quake3Shader,
    shaderPath,
    editorImage,
    lightImage,
    culling,
    surfaceParms,
    stages,
    qerAlphaFunc,
    format,
    coverage,
    previewImages,
    engineDefaulted,
    referencedTables);
};

std::ostream& operator<<(std::ostream& lhs, Quake3ShaderStage::Lighting rhs);
std::ostream& operator<<(std::ostream& lhs, Quake3Shader::Culling rhs);
std::ostream& operator<<(std::ostream& lhs, Quake3Shader::Format rhs);
std::ostream& operator<<(std::ostream& lhs, Quake3Shader::Coverage rhs);

} // namespace tb::mdl
