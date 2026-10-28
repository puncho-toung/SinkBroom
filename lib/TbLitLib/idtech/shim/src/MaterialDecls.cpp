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

// Reads idMaterial's private editorImageName without changing the copied class, as
// DeclManagerShim.cpp reads the decl manager's lists: an explicit instantiation may name
// a private member, and the friend function it defines hands the member pointer out.
struct EditorImageNameTag
{
  using type = idStr idMaterial::*;
  friend type get(EditorImageNameTag);
};

template <typename Tag, typename Tag::type Member>
struct PrivateMember
{
  friend typename Tag::type get(Tag) { return Member; }
};

template struct PrivateMember<EditorImageNameTag, &idMaterial::editorImageName>;

// LP7.2 (C53): idDeclSkin's private mappings the same way (RemapShaderBySkin is the
// class's only lookup of them, DeclSkin.h)
struct SkinMappingsTag
{
  using type = idList<skinMapping_t, TAG_IDLIB_LIST_DECL> idDeclSkin::*;
  friend type get(SkinMappingsTag);
};

template struct PrivateMember<SkinMappingsTag, &idDeclSkin::mappings>;

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

std::string fmt_material_refused(const char* name)
{
  return std::string{"material '"} + name + "' exceeded "
         + std::to_string(MAX_SHADER_STAGES)
         + " stages; it is not parsed (the engine's parser writes past its stage array "
           "there)";
}

std::string lastError(const std::vector<DeclMessage>& messages)
{
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
  {
    if (it->kind == DeclMessageKind::Error || it->kind == DeclMessageKind::FatalError)
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
  if (const auto* stencil = stage.stencilStage)
  {
    entry.stencil.ref = int(stencil->ref);
    entry.stencil.readMask = int(stencil->readMask);
    entry.stencil.writeMask = int(stencil->writeMask);
    entry.stencil.comp = int(stencil->comp);
    entry.stencil.pass = int(stencil->pass);
    entry.stencil.fail = int(stencil->fail);
    entry.stencil.zFail = int(stencil->zFail);
  }

  return entry;
}

std::shared_ptr<const MaterialEntry> materialEntry(
  const idMaterial& material, const std::uint64_t registry, const std::uint32_t revision)
{
  auto entry = std::make_shared<MaterialEntry>();
  entry->registry = registry;
  entry->name = material.GetName();
  entry->sourceFile = material.GetFileName();
  entry->sourceLine = material.GetLineNum();
  entry->index = material.Index();
  entry->revision = revision;
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
  entry->suppressInSubview = material.SuppressInSubview();
  entry->entityGui = material.GetEntityGui();
  entry->editorImageName = (material.*get(EditorImageNameTag{})).c_str();
  entry->mikktspace = material.UseMikkTSpace();
  entry->unsmoothedTangents = material.UseUnsmoothedTangents();
  entry->createBackSides = material.ShouldCreateBackSides();
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
    // the registers are the material's, set by a wobbleSky stage alone
    if (material.GetStage(i)->texture.texgen == TG_WOBBLESKY_CUBE)
    {
      const auto* registers = material.GetTexGenRegisters();
      for (std::size_t r = 0; r < entry->texGenRegisters.size(); ++r)
      {
        entry->texGenRegisters[r] = registers[r];
      }
    }
  }

  return entry;
}

// the process's registry ids; 0 is never handed out, so a default MaterialEntry belongs
// to no registry
auto nextRegistryId = std::atomic<std::uint64_t>{1};

} // namespace

bool DeclFileTime::operator==(const DeclFileTime& other) const
{
  return path == other.path && time == other.time && size == other.size;
}

bool DeclFileTime::operator!=(const DeclFileTime& other) const
{
  return !(*this == other);
}

bool GameFileProbe::exists() const
{
  return kind != Kind::Missing;
}

bool GameFileProbe::operator==(const GameFileProbe& other) const
{
  return kind == other.kind && container == other.container && time == other.time
         && size == other.size && changeStamp == other.changeStamp;
}

bool GameFileProbe::operator!=(const GameFileProbe& other) const
{
  return !(*this == other);
}

DeclSource::~DeclSource() = default;

GameFileProbe DeclSource::probe(const std::string& path)
{
  auto found = GameFileProbe{};
  static_cast<void>(readGameFile(path, &found));
  return found;
}

std::optional<std::string> DeclSource::readGameFile(
  const std::string& path, GameFileProbe* found)
{
  auto time = std::int64_t{0};
  auto text = readFile(path, &time);
  if (found)
  {
    *found = GameFileProbe{};
    if (text)
    {
      found->kind = GameFileProbe::Kind::Loose;
      found->time = time;
      found->size = text->size();
    }
  }
  return text;
}

std::optional<std::string> DeclSource::readGameFileHead(
  const std::string& path, const std::size_t maxBytes, GameFileProbe* found)
{
  auto bytes = readGameFile(path, found);
  if (bytes && bytes->size() > maxBytes)
  {
    bytes->resize(maxBytes);
  }
  return bytes;
}

std::optional<std::string> DeclSource::gameRelativePath(const std::string&) const
{
  return std::nullopt;
}

bool DeclSource::usingResourceFiles() const
{
  return false;
}

std::vector<DeclFileTime> DeclSource::looseFileTimes(
  const std::string&, const std::string&) const
{
  return {};
}

std::vector<std::string> DeclSource::takeWarnings()
{
  return {};
}

void DeclSource::closeFiles() {}

void DeclSource::looseFilesMayHaveChanged() {}

bool DeclSource::changedWhileRead() const
{
  return false;
}

std::size_t DeclSource::containerCount() const
{
  return 0;
}

ContainerTablesOrigin DeclSource::containerTablesOrigin() const
{
  return {};
}

int compareEnginePaths(const std::string& lhs, const std::string& rhs)
{
  return idStr::IcmpPath(lhs.c_str(), rhs.c_str());
}

MemoryDeclSource::MemoryDeclSource(std::vector<DeclFile> files)
  : m_files{std::move(files)}
{
}

std::vector<std::string> MemoryDeclSource::listFiles(
  const std::string& folder, const std::string& extension)
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
  const std::string& path, std::int64_t* timestamp)
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

bool EditorImageSize::operator==(const EditorImageSize& other) const
{
  return width == other.width && height == other.height && state == other.state;
}

bool EditorImageSize::operator!=(const EditorImageSize& other) const
{
  return !(*this == other);
}

const std::string* findKey(const EntityKeys& keys, const std::string_view name)
{
  // idStr::Icmp folds 'A'-'Z' only; the engine's keys are compared to their end
  const auto fold = [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  };
  const auto equal = [&](const std::string& key) {
    if (key.size() != name.size())
    {
      return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i)
    {
      if (fold(key[i]) != fold(name[i]))
      {
        return false;
      }
    }
    return true;
  };

  if (name.empty())
  {
    return nullptr;
  }
  // the last one given wins (idDict::Set replaces the value)
  for (auto it = keys.rbegin(); it != keys.rend(); ++it)
  {
    if (equal(it->first))
    {
      return &it->second;
    }
  }
  return nullptr;
}

MaterialDecls::Claim::Claim()
  : m_held{shim::Registry::claim()}
{
}

MaterialDecls::Claim::~Claim()
{
  release();
}

bool MaterialDecls::Claim::held() const
{
  return m_held;
}

void MaterialDecls::Claim::release()
{
  if (m_held)
  {
    shim::Registry::release();
    m_held = false;
  }
}

MaterialDecls::MaterialDecls(
  DeclSource& source,
  const std::atomic<bool>* cancel,
  const std::function<void()>& afterBuildForTesting)
  : m_id{nextRegistryId++}
{
  // C17, A2.6: one registry at a time, claimed atomically by m_claim
  if (!m_claim.held())
  {
    m_constructionError = "another material registry exists";
    m_constructionMessages.push_back(
      DeclMessage{DeclMessageKind::Error, m_constructionError});
    return;
  }

  try
  {
    m_registry = std::make_unique<shim::Registry>(source, cancel);
  }
  catch (const std::exception& e)
  {
    // the registry's bindings have unbound the globals
    m_constructionError =
      std::string{"the material registry could not be built: "} + e.what();
  }
  catch (...)
  {
    m_constructionError = "the material registry could not be built";
  }
  if (!m_registry)
  {
    m_constructionMessages.push_back(
      DeclMessage{DeclMessageKind::Error, m_constructionError});
    m_claim.release();
    return;
  }

  // M-ID-GUARD-05: a material with too many stages never reaches the copied parser
  const auto refused = shim::refuseOversizedMaterials(m_registry->declManager());
  for (const auto index : refused)
  {
    coverIndex(std::size_t(index));
    m_failures[std::size_t(index)] = fmt_material_refused(
      declManager->DeclByIndex(DECL_MATERIAL, index, false)->GetName());
  }

  if (afterBuildForTesting)
  {
    afterBuildForTesting();
  }
}

MaterialDecls::~MaterialDecls()
{
  // the entries hold no id pointer, so they may outlive the registry; m_claim is released
  // after the registry is destroyed
  m_materials.clear();
  m_failures.clear();
  m_originals.clear();
  m_registry.reset();
}

std::vector<DeclFolderInfo> MaterialDecls::engineDeclFolders()
{
  // DeclManager.cpp:936 and :995-996, then d3xp/Game_local.cpp:327-331 (M-ID-FN-11)
  return {
    {"materials", ".mtr"},
    {"skins", ".skin"},
    {"sound", ".sndshd"},
    {"def", ".def"},
    {"fx", ".fx"},
    {"particles", ".prt"},
    {"af", ".af"},
    {"newpdas", ".pda"},
  };
}

const std::string& MaterialDecls::constructionError() const
{
  return m_constructionError;
}

std::uint64_t MaterialDecls::id() const
{
  return m_id;
}

bool MaterialDecls::cancelled() const
{
  return m_registry && m_registry->cancelled();
}

DeclResult<std::shared_ptr<const MaterialEntry>> MaterialDecls::lookup(
  const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }

  // M-ID-GUARD-01: FindTypeWithoutParsing canonicalises into MAX_STRING_CHARS bytes and
  // writes one past them for a name of MAX_STRING_CHARS or more characters without a '.'
  // among them. A SinkBroom face can name one, so every name that long is refused here;
  // the decl files' own tokens are guarded when they are read (M-ID-GUARD-06)
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
  coverIndex(index);
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
    m_materials[index] = materialEntry(material, m_id, m_revisions[index]);
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
  if (
    entry.registry != m_id || entry.index < 0
    || entry.index >= declManager->GetNumDecls(DECL_MATERIAL))
  {
    return {std::nullopt, "material '" + entry.name + "' is not in this registry"};
  }

  const auto* decl = declManager->DeclByIndex(DECL_MATERIAL, entry.index, false);
  if (!decl || entry.name != decl->GetName() || decl->GetState() == DS_UNPARSED)
  {
    return {std::nullopt, "material '" + entry.name + "' is not in this registry"};
  }
  // LPF.0b (D105, SC427): an entry of an earlier parse of the decl describes another
  // material than the one EvaluateRegisters would run
  if (entry.revision != revision(entry.index))
  {
    return {
      std::nullopt,
      "material '" + entry.name + "' was parsed again since the entry was made"};
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

std::string MaterialDecls::reparseNameRefusal(const std::string_view name)
{
  if (name.empty())
  {
    return "a material re-parse needs a material name";
  }
  // lookup's guard (M-ID-GUARD-01)
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return "material name too long";
  }
  return {};
}

std::optional<std::string> MaterialDecls::feedTextGuards(
  const std::string& text, const std::string_view name)
{
  const auto nameText = std::string{name};
  // M-ID-GUARD-06: what the file system's ReadFile refuses a decl file for
  if (shim::holdsOverlongDeclToken(text, nameText))
  {
    return "the text of material '" + nameText
           + "' holds a token of 1024 or more characters with no '.' among the first "
             "1024, which the engine's MakeNameCanonical cannot take; it is not parsed";
  }
  // M-ID-GUARD-05: what refuseOversizedMaterials makes the default material
  if (shim::materialStageBound(text, nameText.c_str()) > MAX_SHADER_STAGES)
  {
    return fmt_material_refused(nameText.c_str());
  }
  return std::nullopt;
}

void MaterialDecls::coverIndex(const std::size_t index)
{
  if (index >= m_materials.size())
  {
    m_materials.resize(index + 1);
    m_failures.resize(index + 1);
    m_revisions.resize(index + 1);
    m_originals.resize(index + 1);
  }
}

std::uint32_t MaterialDecls::revision(const int materialIndex) const
{
  return materialIndex >= 0 && std::size_t(materialIndex) < m_revisions.size()
           ? m_revisions[std::size_t(materialIndex)]
           : 0;
}

MaterialReparse MaterialDecls::reparseResult(
  const int index, const std::string& name, const bool changed)
{
  auto result = MaterialReparse{};
  result.name = name;
  result.index = index;
  result.changed = changed;
  result.revision = revision(index);
  // the entry made with the revision, or the failure the parse reached
  auto found = lookup(name);
  if (found)
  {
    result.state = MaterialReparse::State::Parsed;
    result.entry = std::move(*found.value);
  }
  else
  {
    result.state = MaterialReparse::State::Failed;
    result.error = std::move(found.error);
  }
  return result;
}

MaterialReparse MaterialDecls::parseText(
  const int index, const std::string& name, const std::string& text)
{
  // DeclByIndex without a parse allocates the decl's self (AllocateSelf, as FindType
  // would): the idDecl whose SetText and Invalidate the editors call (DeclManager.h:181,
  // :224)
  auto* decl = const_cast<idDecl*>(declManager->DeclByIndex(DECL_MATERIAL, index, false));
  const auto errorsBefore = m_registry->errorCount();
  // SetTextLocal frees the old text and keeps the new one (DeclManager.cpp:5098-5135);
  // Invalidate sets DS_UNPARSED (:5011-5014); FindType's ParseLocal frees the material's
  // data and parses the text into the same decl (:1325-1370, :5396-5441)
  decl->SetText(text.c_str());
  decl->Invalidate();
  static_cast<void>(declManager->FindType(DECL_MATERIAL, name.c_str(), true));
  const auto failed = m_registry->errorCount() != errorsBefore;

  const auto at = std::size_t(index);
  coverIndex(at);
  ++m_revisions[at];
  m_materials[at].reset();
  m_failures[at] = failed ? lastError(m_registry->messages()) : std::string{};
  return reparseResult(index, name, true);
}

MaterialReparse MaterialDecls::refusedText(
  const std::string_view name, MaterialReparse result)
{
  // the decl the name has, if it passes reparseNameRefusal; nothing is made for a name
  // without one
  if (reparseNameRefusal(name).empty())
  {
    const auto nameText = std::string{name};
    result.name = canonicalName(nameText);
    result.index =
      shim::materialDeclIndex(m_registry->declManager(), nameText.c_str(), false);
    result.revision = revision(result.index);
  }
  return result;
}

MaterialReparse MaterialDecls::reparse(
  const std::string_view name, const std::string& text)
{
  auto result = MaterialReparse{};
  if (!m_registry)
  {
    result.error = m_constructionError;
    return result;
  }
  // M-ID-GUARD-07: ParseLocal _alloca's the text on the parse thread's stack
  if (text.size() > MaxReparseTextBytes)
  {
    result.error = "the text of material '" + std::string{name.substr(0, 256)} + "' is "
                   + std::to_string(text.size()) + " bytes, more than the "
                   + std::to_string(MaxReparseTextBytes)
                   + " a re-parse takes (the engine's ParseLocal copies a decl's text "
                     "onto the parse thread's stack)";
    return refusedText(name, std::move(result));
  }
  if (auto refused = reparseNameRefusal(name); !refused.empty())
  {
    result.error = std::move(refused);
    return result;
  }
  const auto nameText = std::string{name};
  if (auto refused = feedTextGuards(text, nameText))
  {
    result.error = std::move(*refused);
    return refusedText(name, std::move(result));
  }

  // FindDeclWithoutParsing( DECL_MATERIAL, name, true )'s decl (P5.18): an existing one,
  // or a new implicit one, without the default parse's file probes
  const auto index =
    shim::materialDeclIndex(m_registry->declManager(), nameText.c_str(), true);
  const auto at = std::size_t(index);
  coverIndex(at);
  const auto* decl = declManager->DeclByIndex(DECL_MATERIAL, index, false);
  // P5.8: the text restore puts back, taken before the first SetText frees it; an
  // implicit decl's generated text was freed at its parse (ParseLocal), and a new one
  // has none
  if (!m_originals[at])
  {
    auto original = OriginalText{};
    if (!decl->IsImplicit())
    {
      auto kept = std::string(std::size_t(decl->GetTextLength()) + 1, '\0');
      decl->GetText(kept.data());
      kept.resize(std::size_t(decl->GetTextLength()));
      original.text = std::move(kept);
    }
    m_originals[at] = std::move(original);
  }
  return parseText(index, decl->GetName(), text);
}

MaterialReparse MaterialDecls::restore(const std::string_view name)
{
  auto result = MaterialReparse{};
  if (!m_registry)
  {
    result.error = m_constructionError;
    return result;
  }
  if (auto refused = reparseNameRefusal(name); !refused.empty())
  {
    result.error = std::move(refused);
    return result;
  }
  const auto nameText = std::string{name};
  const auto index =
    shim::materialDeclIndex(m_registry->declManager(), nameText.c_str(), false);
  if (index < 0)
  {
    result.error = "no material '" + nameText + "'";
    return result;
  }
  const auto at = std::size_t(index);
  coverIndex(at);
  auto* decl = const_cast<idDecl*>(declManager->DeclByIndex(DECL_MATERIAL, index, false));
  const auto canonical = std::string{decl->GetName()};
  if (!m_originals[at])
  {
    return reparseResult(index, canonical, false);
  }
  auto original = std::move(*m_originals[at]);
  m_originals[at].reset();
  if (original.text)
  {
    // the file's text, which passed the token guard when its file was read
    if (auto refused = feedTextGuards(*original.text, canonical))
    {
      // as refuseOversizedMaterials left it: the text kept, the decl the default
      // material, every lookup failing with the reason
      decl->SetText(original.text->c_str());
      decl->MakeDefault();
      ++m_revisions[at];
      m_materials[at].reset();
      m_failures[at] = std::move(*refused);
      return reparseResult(index, canonical, true);
    }
    return parseText(index, canonical, *original.text);
  }

  // an implicit decl: idDeclLocal::MakeDefault (DeclManager.cpp:5272-5301), the
  // material's DefaultDefinition parsed in place
  const auto errorsBefore = m_registry->errorCount();
  decl->MakeDefault();
  const auto failed = m_registry->errorCount() != errorsBefore;
  ++m_revisions[at];
  m_materials[at].reset();
  m_failures[at] = failed ? lastError(m_registry->messages()) : std::string{};
  return reparseResult(index, canonical, true);
}

int MaterialDecls::declsInFile(const std::string_view fileName) const
{
  return m_registry
           ? shim::declsInFile(m_registry->declManager(), std::string{fileName}.c_str())
           : 0;
}

DeclResult<std::shared_ptr<const MaterialEntry>> MaterialDecls::lookupIndex(
  const int materialIndex)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  if (materialIndex < 0 || materialIndex >= declManager->GetNumDecls(DECL_MATERIAL))
  {
    return {std::nullopt, "no material at index " + std::to_string(materialIndex)};
  }

  // the name only: lookup parses, with its error handling
  const auto* decl = declManager->DeclByIndex(DECL_MATERIAL, materialIndex, false);
  return lookup(decl->GetName());
}

std::string MaterialDecls::materialName(const int materialIndex) const
{
  if (
    !m_registry || materialIndex < 0
    || materialIndex >= declManager->GetNumDecls(DECL_MATERIAL))
  {
    return {};
  }
  return declManager->DeclByIndex(DECL_MATERIAL, materialIndex, false)->GetName();
}

DeclResult<std::string> MaterialDecls::parseTable(const int tableIndex)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  if (tableIndex < 0 || tableIndex >= declManager->GetNumDecls(DECL_TABLE))
  {
    return {std::nullopt, "no table at index " + std::to_string(tableIndex)};
  }

  const auto errorsBefore = m_registry->errorCount();
  const auto* decl = declManager->DeclByIndex(DECL_TABLE, tableIndex, true);
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }
  return {std::string{decl->GetName()}, {}};
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

std::vector<DeclTypeCount> MaterialDecls::declCounts() const
{
  return m_registry ? shim::declCounts(m_registry->declManager())
                    : std::vector<DeclTypeCount>{};
}

std::vector<std::string> MaterialDecls::declNames(const std::string_view typeName) const
{
  auto names = std::vector<std::string>{};
  if (!m_registry)
  {
    return names;
  }

  const auto typeText = std::string{typeName};
  const auto type = declManager->GetDeclTypeFromName(typeText.c_str());
  if (type == DECL_MAX_TYPES)
  {
    return names;
  }
  for (int i = 0; i < declManager->GetNumDecls(type); ++i)
  {
    names.emplace_back(declManager->DeclByIndex(type, i, false)->GetName());
  }
  return names;
}

std::vector<MaterialImageKey> MaterialDecls::imageKeys() const
{
  auto keys = std::vector<MaterialImageKey>{};
  if (!m_registry)
  {
    return keys;
  }
  for (int i = 0; i < globalImages->NumImages(); ++i)
  {
    if (auto key = imageKey(globalImages->ImageByIndex(i)))
    {
      keys.push_back(std::move(*key));
    }
  }
  return keys;
}

int MaterialDecls::defaultedCount(const std::string_view typeName) const
{
  if (!m_registry)
  {
    return 0;
  }

  const auto typeText = std::string{typeName};
  const auto type = declManager->GetDeclTypeFromName(typeText.c_str());
  if (type == DECL_MAX_TYPES)
  {
    return 0;
  }

  auto count = 0;
  for (int i = 0; i < declManager->GetNumDecls(type); ++i)
  {
    // the state only; nothing is parsed
    if (declManager->DeclByIndex(type, i, false)->GetState() == DS_DEFAULTED)
    {
      ++count;
    }
  }
  return count;
}

int MaterialDecls::declFileCount() const
{
  return m_registry ? shim::declFileCount(m_registry->declManager()) : 0;
}

std::size_t MaterialDecls::declFileBytes() const
{
  return m_registry ? shim::declFileBytes(m_registry->declManager()) : 0;
}

std::vector<DeclFolderInfo> MaterialDecls::declFolders() const
{
  return m_registry ? shim::declFolders(m_registry->declManager())
                    : std::vector<DeclFolderInfo>{};
}

DeclResult<std::shared_ptr<const EntityDefEntry>> MaterialDecls::entityDef(
  const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // the guard of lookup (M-ID-GUARD-01)
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

  const auto& def = static_cast<const idDeclEntityDef&>(*decl);
  auto entry = std::make_shared<EntityDefEntry>();
  entry->registry = m_id;
  entry->name = def.GetName();
  for (int i = 0; i < def.dict.GetNumKeyVals(); ++i)
  {
    const auto* keyValue = def.dict.GetKeyVal(i);
    entry->keys.emplace_back(keyValue->GetKey().c_str(), keyValue->GetValue().c_str());
  }
  // the copied rule, which finds the def parsed above
  entry->dmapOnly = idDeclEntityDef::IsMapOnlyClass(nameText.c_str());
  return {std::move(entry), {}};
}

DeclResult<EngineSkin> MaterialDecls::skin(const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // the guard of lookup (M-ID-GUARD-01)
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "skin name too long"};
  }

  const auto nameText = std::string{name};
  const auto errorsBefore = m_registry->errorCount();
  // Entity.cpp:302: FindSkin( temp ), makeDefault true (DeclManager.cpp:944-947)
  const auto* decl = declManager->FindSkin(nameText.c_str());
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }
  if (!decl)
  {
    return {std::nullopt, "no skin '" + nameText + "'"};
  }

  auto skin = EngineSkin{};
  skin.name = decl->GetName();
  const auto& mappings = decl->*get(SkinMappingsTag{});
  for (int i = 0; i < mappings.Num(); ++i)
  {
    const auto& mapping = mappings[i];
    skin.mappings.emplace_back(
      mapping.from ? mapping.from->GetName() : "",
      mapping.to ? mapping.to->GetName() : "");
  }
  return {std::move(skin), {}};
}

DeclResult<bool> MaterialDecls::touch(
  const std::string_view typeName, const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // the guard of lookup (M-ID-GUARD-01)
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "decl name too long"};
  }

  const auto typeText = std::string{typeName};
  const auto type = declManager->GetDeclTypeFromName(typeText.c_str());
  if (type == DECL_MAX_TYPES)
  {
    return {std::nullopt, "no decl type '" + typeText + "'"};
  }

  // DeclManager.cpp's TouchDecl_f
  const auto nameText = std::string{name};
  const auto errorsBefore = m_registry->errorCount();
  const auto* decl = declManager->FindType(type, nameText.c_str(), false);
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }
  return {decl != nullptr, {}};
}

DeclResult<std::string> MaterialDecls::printDecl(
  const std::string_view typeName, const std::string_view name)
{
  if (!m_registry)
  {
    return {std::nullopt, m_constructionError};
  }
  // the guard of lookup (M-ID-GUARD-01)
  if (name.size() >= size_t(MAX_STRING_CHARS))
  {
    return {std::nullopt, "decl name too long"};
  }

  const auto typeText = std::string{typeName};
  const auto type = declManager->GetDeclTypeFromName(typeText.c_str());
  if (type == DECL_MAX_TYPES)
  {
    return {std::nullopt, "no decl type '" + typeText + "'"};
  }

  // the console's arguments, as the engine's command line appends them
  auto args = idCmdArgs{};
  args.AppendArg(("print" + typeText).c_str());
  args.AppendArg(std::string{name}.c_str());

  const auto errorsBefore = m_registry->errorCount();
  m_registry->beginCapture();
  declManager->PrintType(args, type);
  auto text = m_registry->endCapture();
  if (m_registry->errorCount() != errorsBefore)
  {
    return {std::nullopt, lastError(m_registry->messages())};
  }
  return {std::move(text), {}};
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

std::vector<DeclMessage> MaterialDecls::takeMessagesFrom(const std::size_t first)
{
  return m_registry ? m_registry->takeMessagesFrom(first) : std::vector<DeclMessage>{};
}

} // namespace tb::lit
