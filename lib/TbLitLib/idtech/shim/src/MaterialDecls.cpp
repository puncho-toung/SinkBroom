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

// clang-format off: the std-only facade, then precompiled.h, then the registry
#include "lit/MaterialDecls.h"
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

namespace tb::lit
{
namespace
{

bool endsWithNoCase(const std::string& text, const std::string& suffix)
{
  return text.size() >= suffix.size()
         && idStr::Icmp(text.c_str() + (text.size() - suffix.size()), suffix.c_str())
              == 0;
}

std::optional<MaterialImageKey> imageKey(const idImage* image)
{
  if (!image)
  {
    return std::nullopt;
  }

  auto key = MaterialImageKey{};
  key.name = image->GetName();
  key.usage = int(image->usage);
  key.filter = int(image->filter);
  key.repeat = int(image->repeat);
  key.cube = int(image->cubeFiles);
  key.cubeMapSize = image->cubeMapSize;
  key.intrinsic = image->intrinsic;
  key.isDefault = image == globalImages->defaultImage;
  return key;
}

std::string lastError(const std::vector<DeclMessage>& messages)
{
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
  {
    if (
      it->kind == DeclMessageKind::Error || it->kind == DeclMessageKind::FatalError
      || it->kind == DeclMessageKind::Assert)
    {
      return it->text;
    }
  }
  return "an engine error was reported";
}

MaterialStage stageEntry(const shaderStage_t& stage)
{
  auto entry = MaterialStage{};
  entry.lighting = int(stage.lighting);
  entry.drawStateBits = std::uint64_t(stage.drawStateBits);
  entry.conditionRegister = stage.conditionRegister;
  for (int i = 0; i < 4; ++i)
  {
    entry.colorRegisters[size_t(i)] = stage.color.registers[i];
  }
  entry.hasAlphaTest = stage.hasAlphaTest;
  entry.alphaTestRegister = stage.alphaTestRegister;
  entry.ignoreAlphaTest = stage.ignoreAlphaTest;
  entry.vertexColor = int(stage.vertexColor);
  entry.privatePolygonOffset = stage.privatePolygonOffset;

  const auto& texture = stage.texture;
  entry.image = imageKey(texture.image);
  entry.texgen = int(texture.texgen);
  entry.hasTextureMatrix = texture.hasMatrix;
  for (int row = 0; row < 2; ++row)
  {
    for (int column = 0; column < 3; ++column)
    {
      entry.textureMatrixRegisters[size_t(row)][size_t(column)] =
        texture.matrix[row][column];
    }
  }
  entry.dynamicImage = int(texture.dynamic);
  entry.dynamicWidth = texture.width;
  entry.dynamicHeight = texture.height;
  if (texture.cinematic)
  {
    entry.cinematic = texture.cinematic->GetFileName();
    entry.soundMap = texture.cinematic->IsSoundWindow();
  }

  if (const auto* newStage = stage.newStage)
  {
    entry.newStage = true;
    if (newStage->vertexProgram)
    {
      entry.vertexProgram = renderProgManager.ShaderName(newStage->vertexProgram);
    }
    if (newStage->fragmentProgram)
    {
      entry.fragmentProgram = renderProgManager.ShaderName(newStage->fragmentProgram);
    }
    for (int i = 0; i < newStage->numVertexParms; ++i)
    {
      entry.vertexParms.push_back(std::array<int, 4>{
        newStage->vertexParms[i][0],
        newStage->vertexParms[i][1],
        newStage->vertexParms[i][2],
        newStage->vertexParms[i][3]});
    }
    for (int i = 0; i < newStage->numFragmentProgramImages; ++i)
    {
      entry.fragmentImages.push_back(imageKey(newStage->fragmentProgramImages[i]));
    }
  }
  entry.hasStencil = stage.stencilStage != nullptr;

  return entry;
}

std::shared_ptr<const MaterialEntry> materialEntry(const idMaterial& material)
{
  auto entry = std::make_shared<MaterialEntry>();
  entry->name = material.GetName();
  entry->sourceFile = material.GetFileName();
  entry->sourceLine = material.GetLineNum();
  entry->index = material.Index();
  entry->implicit = material.base->IsImplicit();
  // MakeDefault re-parses the default definition after CommonInit cleared MF_DEFAULTED;
  // the decl state keeps the fact
  entry->defaulted = material.GetState() == DS_DEFAULTED;

  for (int bit = 0; bit < 31; ++bit)
  {
    if (material.TestMaterialFlag(1 << bit))
    {
      entry->materialFlags |= 1 << bit;
    }
  }
  entry->contentFlags = material.GetContentFlags();
  entry->surfaceFlags = material.GetSurfaceFlags();
  entry->sort = material.GetSort();
  entry->coverage = int(material.Coverage());
  entry->cullType = int(material.GetCullType());
  entry->polygonOffset = material.GetPolygonOffset();
  entry->deform = int(material.Deform());
  for (int i = 0; i < 4; ++i)
  {
    entry->deformRegisters[size_t(i)] = material.GetDeformRegister(i);
  }
  if (const auto* deformDecl = material.GetDeformDecl())
  {
    entry->deformDecl = deformDecl->GetName();
  }
  entry->spectrum = material.Spectrum();
  entry->fogLight = material.IsFogLight();
  entry->blendLight = material.IsBlendLight();
  entry->ambientLight = material.IsAmbientLight();
  entry->receivesFog = material.ReceivesFog();
  entry->portalSky = material.IsPortalSky();
  entry->hasSubview = material.HasSubview();
  entry->receivesLighting = material.ReceivesLighting();
  entry->surfaceCastsShadow = material.SurfaceCastsShadow();
  entry->entityGui = material.GetEntityGui();
  if (const auto* gui = material.GlobalGui())
  {
    entry->gui = gui->GetSourceFile();
  }
  entry->lightFalloffImage = imageKey(material.LightFalloffImage());
  entry->fastPathBumpImage = imageKey(material.GetFastPathBumpImage());
  entry->fastPathDiffuseImage = imageKey(material.GetFastPathDiffuseImage());
  entry->fastPathSpecularImage = imageKey(material.GetFastPathSpecularImage());

  entry->numRegisters = material.GetNumRegisters();
  entry->constantRegisters = material.ConstantRegisters() != nullptr;
  entry->hasAmbient = material.HasAmbient();
  for (int i = 0; i < material.GetNumStages(); ++i)
  {
    entry->stages.push_back(stageEntry(*material.GetStage(i)));
  }

  return entry;
}

} // namespace

DeclSource::~DeclSource() = default;

MemoryDeclSource::MemoryDeclSource(std::vector<DeclFile> files)
  : m_files{std::move(files)}
{
}

std::vector<std::string> MemoryDeclSource::listFiles(
  const std::string& folder, const std::string& extension) const
{
  // the files directly inside folder: the engine's ListFiles does not recurse
  auto names = std::vector<std::string>{};
  for (const auto& file : m_files)
  {
    const auto& path = file.path;
    if (
      path.size() <= folder.size() + 1
      || idStr::Icmpn(path.c_str(), folder.c_str(), int(folder.size())) != 0
      || path[folder.size()] != '/')
    {
      continue;
    }

    auto name = path.substr(folder.size() + 1);
    if (name.find('/') == std::string::npos && endsWithNoCase(name, extension))
    {
      names.push_back(std::move(name));
    }
  }
  return names;
}

std::optional<std::string> MemoryDeclSource::readFile(
  const std::string& path, std::int64_t* timestamp) const
{
  for (const auto& file : m_files)
  {
    if (idStr::Icmp(file.path.c_str(), path.c_str()) == 0)
    {
      if (timestamp)
      {
        *timestamp = file.timestamp;
      }
      return file.text;
    }
  }
  return std::nullopt;
}

bool MaterialImageKey::operator==(const MaterialImageKey& other) const
{
  return name == other.name && usage == other.usage && filter == other.filter
         && repeat == other.repeat && cube == other.cube
         && cubeMapSize == other.cubeMapSize && intrinsic == other.intrinsic
         && isDefault == other.isDefault;
}

bool MaterialImageKey::operator!=(const MaterialImageKey& other) const
{
  return !(*this == other);
}

MaterialDecls::MaterialDecls(const DeclSource& source)
{
  if (shim::Registry::current())
  {
    m_constructionError = "another material registry exists";
    m_constructionMessages.push_back(
      DeclMessage{DeclMessageKind::Error, m_constructionError});
    return;
  }
  m_registry = std::make_unique<shim::Registry>(source);
}

MaterialDecls::~MaterialDecls()
{
  // the entries hold no id pointer, so they may outlive the registry
  m_materials.clear();
  m_failures.clear();
  m_registry.reset();
}

const std::string& MaterialDecls::constructionError() const
{
  return m_constructionError;
}

DeclResult<std::shared_ptr<const MaterialEntry>> MaterialDecls::lookup(
  const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }

  // FindTypeWithoutParsing canonicalises into MAX_STRING_CHARS bytes and writes one past
  // them for a longer name; the engine never passes one, a SinkBroom face might
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "material name too long"};
  }

  const auto nameText = std::string{name};
  const auto errorsBefore = m_registry->errorCount();
  const auto* decl = declManager->FindType(DECL_MATERIAL, nameText.c_str(), true);
  const auto failed = m_registry->errorCount() != errorsBefore;
  if (!decl)
  {
    return {
      std::nullopt,
      failed ? lastError(m_registry->messages()) : "no material '" + nameText + "'"};
  }

  const auto& material = static_cast<const idMaterial&>(*decl);
  const auto index = size_t(material.Index());
  if (index >= m_materials.size())
  {
    m_materials.resize(index + 1);
    m_failures.resize(index + 1);
  }
  if (failed)
  {
    m_failures[index] = lastError(m_registry->messages());
  }
  if (!m_failures[index].empty())
  {
    return {std::nullopt, m_failures[index]};
  }
  if (!m_materials[index])
  {
    m_materials[index] = materialEntry(material);
  }
  return {m_materials[index], {}};
}

DeclResult<EvaluatedRegisters> MaterialDecls::evaluate(
  const MaterialEntry& entry,
  const std::array<float, ShaderParmCount>& shaderParms,
  const float time)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }

  // DeclByIndex reads past its list after reporting an out-of-range index
  if (entry.index < 0 || entry.index >= declManager->GetNumDecls(DECL_MATERIAL))
  {
    return {std::nullopt, "material '" + entry.name + "' is not in this registry"};
  }

  const auto* decl = declManager->DeclByIndex(DECL_MATERIAL, entry.index, false);
  if (!decl || entry.name != decl->GetName() || decl->GetState() == DS_UNPARSED)
  {
    return {std::nullopt, "material '" + entry.name + "' is not in this registry"};
  }

  const auto& material = static_cast<const idMaterial&>(*decl);

  float localShaderParms[MAX_ENTITY_SHADER_PARMS];
  for (int i = 0; i < MAX_ENTITY_SHADER_PARMS; ++i)
  {
    localShaderParms[i] = shaderParms[size_t(i)];
  }
  float globalShaderParms[MAX_GLOBAL_SHADER_PARMS];
  memset(globalShaderParms, 0, sizeof(globalShaderParms));

  // EvaluateRegisters always writes the predefined registers, even for a material that
  // has fewer
  auto registers = EvaluatedRegisters{};
  registers.registers.resize(
    size_t(std::max(material.GetNumRegisters(), int(EXP_REG_NUM_PREDEFINED))));

  const auto errorsBefore = m_registry->errorCount();
  material.EvaluateRegisters(
    registers.registers.data(), localShaderParms, globalShaderParms, time, nullptr);
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }

  return {std::move(registers), {}};
}

std::string MaterialDecls::canonicalName(const std::string_view name)
{
  const auto text = std::string{name.substr(0, size_t(MAX_STRING_CHARS))};
  return shim::canonicalDeclName(text.c_str());
}

int MaterialDecls::materialCount() const
{
  return m_registry ? declManager->GetNumDecls(DECL_MATERIAL) : 0;
}

int MaterialDecls::tableCount() const
{
  return m_registry ? declManager->GetNumDecls(DECL_TABLE) : 0;
}

int MaterialDecls::entityDefCount() const
{
  return m_registry ? declManager->GetNumDecls(DECL_ENTITYDEF) : 0;
}

DeclResult<std::vector<std::pair<std::string, std::string>>> MaterialDecls::entityDefKeys(
  const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "entityDef name too long"};
  }

  const auto nameText = std::string{name};
  const auto errorsBefore = m_registry->errorCount();
  const auto* decl = declManager->FindType(DECL_ENTITYDEF, nameText.c_str(), false);
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }
  if (!decl)
  {
    return {std::nullopt, "no entityDef '" + nameText + "'"};
  }

  const auto& entityDef = static_cast<const idDeclEntityDef&>(*decl);
  auto keys = std::vector<std::pair<std::string, std::string>>{};
  for (int i = 0; i < entityDef.dict.GetNumKeyVals(); ++i)
  {
    const auto* keyValue = entityDef.dict.GetKeyVal(i);
    keys.emplace_back(keyValue->GetKey().c_str(), keyValue->GetValue().c_str());
  }
  return {std::move(keys), {}};
}

std::size_t MaterialDecls::declTextBytes() const
{
  return m_registry ? shim::declTextBytes(m_registry->declManager()) : 0;
}

const std::vector<DeclMessage>& MaterialDecls::messages() const
{
  return m_registry ? m_registry->messages() : m_constructionMessages;
}

std::vector<DeclMessage> MaterialDecls::takeMessages()
{
  return m_registry ? m_registry->takeMessages()
                    : std::exchange(m_constructionMessages, {});
}

} // namespace tb::lit
