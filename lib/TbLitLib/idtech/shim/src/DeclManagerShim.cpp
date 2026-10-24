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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/DeclManager.cpp and
// d3xp/Game_local.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2023 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The decl manager: the copied ranges of framework/DeclManager.cpp, compiled here because
// idDeclLocal, idDeclFile and idDeclManagerLocal are defined inside that file (R5.10),
// and the shim's definitions of the members the copy leaves out. Each of those is listed
// in mirror.json ("virtuals").

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

#include <string>
#include <string_view>

// The engine's declManagerLocal is a global object, which the copied code names with a
// dot; here the name refers to the registry's object (C17).
#define declManagerLocal (*tbLitDeclManagerLocal)

#include "framework/DeclManager.cpp"

idDeclManagerLocal* tbLitDeclManagerLocal = nullptr;
idDeclManager* declManager = nullptr;

// DeclManager.cpp:904-991 without the console commands and the Huffman tables: the decl
// types the engine registers, in its order. Types SinkBroom does not parse get a plain
// idDecl, whose parse skips the braced text, as the engine's "export" type does (S26).
void idDeclManagerLocal::Init()
{
  checksum = 0;

  // decls used throughout the engine
  RegisterDeclType("table", DECL_TABLE, idDeclAllocator<idDeclTable>);
  RegisterDeclType("material", DECL_MATERIAL, idDeclAllocator<idMaterial>);
  RegisterDeclType("skin", DECL_SKIN, idDeclAllocator<idDeclSkin>);
  RegisterDeclType("entityDef", DECL_ENTITYDEF, idDeclAllocator<idDeclEntityDef>);

  RegisterDeclType("sound", DECL_SOUND, idDeclAllocator<idSoundShader>);
  RegisterDeclType("mapDef", DECL_MAPDEF, idDeclAllocator<idDeclEntityDef>);
  RegisterDeclType("fx", DECL_FX, idDeclAllocator<idDecl>);
  RegisterDeclType("particle", DECL_PARTICLE, idDeclAllocator<idDecl>);
  RegisterDeclType("articulatedFigure", DECL_AF, idDeclAllocator<idDecl>);
  RegisterDeclType("pda", DECL_PDA, idDeclAllocator<idDecl>);
  RegisterDeclType("email", DECL_EMAIL, idDeclAllocator<idDecl>);
  RegisterDeclType("video", DECL_VIDEO, idDeclAllocator<idDecl>);
  RegisterDeclType("audio", DECL_AUDIO, idDeclAllocator<idDecl>);

  RegisterDeclFolder("materials", ".mtr", DECL_MATERIAL);
}

// DeclManager.cpp:993-997
void idDeclManagerLocal::Init2()
{
  RegisterDeclFolder("skins", ".skin", DECL_SKIN);
  RegisterDeclFolder("sound", ".sndshd", DECL_SOUND);
}

// rbdmap's registration; never called by SinkBroom
void idDeclManagerLocal::InitTool()
{
  common->Warning("idDeclManager::InitTool: not available in SinkBroom");
}

// DeclManager.cpp:1044-1081, in R5.11's order instead of type by type: material and table
// decls, then skins, entityDefs and the other decls, then the image records, then the GUI
// dummies, then the decl text, the decls and their files. A material's cinematics are
// freed by its FreeData, never here.
void idDeclManagerLocal::Shutdown()
{
  const auto freeDeclSelf = [](idDeclLocal* decl) {
    if (decl->self != NULL)
    {
      decl->self->FreeData();
      delete decl->self;
      decl->self = NULL;
    }
  };

  for (const auto type : {DECL_MATERIAL, DECL_TABLE})
  {
    for (int j = 0; j < linearLists[type].Num(); j++)
    {
      freeDeclSelf(linearLists[type][j]);
    }
  }

  for (int i = 0; i < DECL_MAX_TYPES; i++)
  {
    if (i == DECL_MATERIAL || i == DECL_TABLE)
    {
      continue;
    }
    for (int j = 0; j < linearLists[i].Num(); j++)
    {
      freeDeclSelf(linearLists[i][j]);
    }
  }

  if (globalImages)
  {
    globalImages->Shutdown();
  }

  tb::lit::shim::freeGuiDummies();

  for (int i = 0; i < DECL_MAX_TYPES; i++)
  {
    for (int j = 0; j < linearLists[i].Num(); j++)
    {
      idDeclLocal* decl = linearLists[i][j];
      if (decl->textSource)
      {
        Mem_Free(decl->textSource);
        decl->textSource = NULL;
      }
      delete decl;
    }
    linearLists[i].Clear();
    hashTables[i].Free();
  }

  // free decl files
  loadedFiles.DeleteContents(true);

  // free the decl types and folders
  declTypes.DeleteContents(true);
  declFolders.DeleteContents(true);
}

// the decl checksums are unused, and MD5_BlockChecksum is a stub (R5.10)
int idDeclManagerLocal::GetChecksum() const
{
  return 0;
}

// console command: nothing is listed (PrintType is copied, for the goldens' printMaterial
// and printTable)
void idDeclManagerLocal::ListType(const idCmdArgs&, declType_t) {}

// the material editor's functions; SinkBroom never writes decls
idDecl* idDeclManagerLocal::CreateNewDecl(declType_t, const char* name, const char*)
{
  common->Warning("idDeclManager::CreateNewDecl: not available in SinkBroom (%s)", name);
  return NULL;
}

bool idDeclManagerLocal::RenameDecl(declType_t, const char* oldName, const char*)
{
  common->Warning("idDeclManager::RenameDecl: not available in SinkBroom (%s)", oldName);
  return false;
}

void idDeclManagerLocal::WritePrecacheCommands(idFile*) {}

bool idDeclLocal::ReplaceSourceFileText()
{
  common->Warning("Can't save declaration %s: SinkBroom never writes decls.", GetName());
  return false;
}

namespace tb::lit::shim
{
namespace
{

// Reads idDeclManagerLocal's private decl lists without changing the copied class: an
// explicit instantiation may name a private member ([temp.explicit]), and the friend
// function it defines hands the member pointer out.
struct LinearListsTag
{
  using type =
    idList<idDeclLocal*, TAG_IDLIB_LIST_DECL> (idDeclManagerLocal::*)[DECL_MAX_TYPES];
  friend type get(LinearListsTag);
};

struct DeclFoldersTag
{
  using type = idList<idDeclFolder*, TAG_IDLIB_LIST_DECL> idDeclManagerLocal::*;
  friend type get(DeclFoldersTag);
};

struct LoadedFilesTag
{
  using type = idList<idDeclFile*, TAG_IDLIB_LIST_DECL> idDeclManagerLocal::*;
  friend type get(LoadedFilesTag);
};

// LPF.0b: idDeclLocal's private link to the next decl of its file
struct NextInFileTag
{
  using type = idDeclLocal* idDeclLocal::*;
  friend type get(NextInFileTag);
};

template <typename Tag, typename Tag::type Member>
struct PrivateMember
{
  friend typename Tag::type get(Tag) { return Member; }
};

template struct PrivateMember<LinearListsTag, &idDeclManagerLocal::linearLists>;
template struct PrivateMember<DeclFoldersTag, &idDeclManagerLocal::declFolders>;
template struct PrivateMember<LoadedFilesTag, &idDeclManagerLocal::loadedFiles>;
template struct PrivateMember<NextInFileTag, &idDeclLocal::nextInFile>;

} // namespace

idDeclManagerLocal* createDeclManager()
{
  // value-initialised: zeroed like the engine's global before Init
  auto* manager = new idDeclManagerLocal();
  tbLitDeclManagerLocal = manager;
  declManager = manager;
  return manager;
}

void initDeclManager(idDeclManagerLocal& manager)
{
  // Common.cpp:1261 and :1345
  manager.Init();
  manager.Init2();

  // d3xp/Game_local.cpp:323-331, the game's registrations
  manager.RegisterDeclType("model", DECL_MODELDEF, idDeclAllocator<idDecl>);
  manager.RegisterDeclType("export", DECL_MODELEXPORT, idDeclAllocator<idDecl>);

  manager.RegisterDeclFolder("def", ".def", DECL_ENTITYDEF);
  manager.RegisterDeclFolder("fx", ".fx", DECL_FX);
  manager.RegisterDeclFolder("particles", ".prt", DECL_PARTICLE);
  manager.RegisterDeclFolder("af", ".af", DECL_AF);
  manager.RegisterDeclFolder("newpdas", ".pda", DECL_PDA);
}

void destroyDeclManager(idDeclManagerLocal* manager)
{
  if (manager)
  {
    manager->Shutdown();
    delete manager;
  }
  declManager = nullptr;
  tbLitDeclManagerLocal = nullptr;
}

std::size_t declTextBytes(const idDeclManagerLocal& manager)
{
  const auto& linearLists = manager.*get(LinearListsTag{});

  auto bytes = std::size_t{0};
  for (int i = 0; i < DECL_MAX_TYPES; ++i)
  {
    for (int j = 0; j < linearLists[i].Num(); ++j)
    {
      const auto length = linearLists[i][j]->GetTextLength();
      // SetTextLocal keeps length + 1 bytes (USE_COMPRESSED_DECLS is off)
      bytes += length > 0 ? std::size_t(length) + 1 : 0;
    }
  }
  return bytes;
}

std::vector<DeclFolderInfo> declFolders(const idDeclManagerLocal& manager)
{
  const auto& folders = manager.*get(DeclFoldersTag{});

  auto result = std::vector<DeclFolderInfo>{};
  for (int i = 0; i < folders.Num(); ++i)
  {
    result.push_back(
      DeclFolderInfo{folders[i]->folder.c_str(), folders[i]->extension.c_str()});
  }
  return result;
}

int declFileCount(const idDeclManagerLocal& manager)
{
  return (manager.*get(LoadedFilesTag{})).Num();
}

std::size_t declFileBytes(const idDeclManagerLocal& manager)
{
  const auto& files = manager.*get(LoadedFilesTag{});
  auto bytes = std::size_t{0};
  for (int i = 0; i < files.Num(); ++i)
  {
    bytes += std::size_t(std::max(files[i]->fileSize, 0));
  }
  return bytes;
}

namespace
{

// the keywords idMaterial::ParseMaterial turns into one ParseStage call each (the
// engine's Material.cpp:2883-2924)
constexpr const char* StageShortcuts[] = {
  "diffusemap",
  "basecolormap",
  "specularmap",
  "rmaomap",
  "reflectionmap",
  "pbrmap",
  "bumpmap",
  "normalmap",
};

bool isStageShortcut(const idToken& token)
{
  for (const auto* keyword : StageShortcuts)
  {
    // ParseMaterial's !token.Icmp( keyword ), whatever the token's type
    if (token.Icmp(keyword) == 0)
    {
      return true;
    }
  }
  return false;
}

/**
 * An upper bound of the stage slots a material decl's text makes the copied parser write
 * (M-ID-GUARD-05), from the tokens the copied lexer reads in it (forEachDeclToken).
 *
 * - idMaterial::Parse skips to the first "{" (SkipUntilString); after it, ParseMaterial
 *   calls ParseStage only for a token equal to "{" or for a stage shortcut keyword, once
 *   per token, whatever the token's type.
 * - A ParseStage call writes one slot, and, after numStages++, a second one without a
 *   limit check, the coverage stage of a diffuse stage with an alpha test (the copy's
 *   Material.cpp:2299-2303, engine :2298-2302).
 * - A stage is diffuse only through ParseBlend's "diffusemap" or "basecolormap" (the
 *   copy's :1109-1112). In a "{" block that is a token of the material's own text, which
 *   this counts. A shortcut's stage is parsed from text made of its image name without
 * the quotes (Image_program.cpp:439-447), so diffusemap "_white alphaTest 0.5" makes both
 *   slots with no further token here.
 *
 * So a "{" counts 1 and a shortcut keyword 2: a block's coverage slot is paid for by its
 * blend keyword, a shortcut's by its second count. AddImplicitStages adds at most one
 * stage, below the limit, without an alpha test. Counting every such token also counts
 * arguments ("diffusemap {", "qer_editorimage {"), a block's own keyword and text after a
 * parse error, so the bound can be higher than the engine's slots, never lower
 * (M-ID-DIFF-03).
 */
int stageBound(const std::string& text, const char* name)
{
  auto opened = false;
  auto slots = 0;
  tb::lit::shim::forEachDeclToken(text, name, [&](const idToken& token) {
    const auto brace = token == "{";
    if (!opened)
    {
      opened = brace;
      return;
    }
    slots += brace ? 1 : isStageShortcut(token) ? 2 : 0;
  });
  return slots;
}

} // namespace

std::vector<int> refuseOversizedMaterials(idDeclManagerLocal& manager)
{
  const auto& materials = (manager.*get(LinearListsTag{}))[DECL_MATERIAL];

  auto refused = std::vector<int>{};
  auto text = std::string{};
  for (int i = 0; i < materials.Num(); ++i)
  {
    auto* decl = materials[i];
    if (decl->GetState() != DS_UNPARSED)
    {
      continue;
    }

    text.resize(std::size_t(decl->GetTextLength()) + 1);
    decl->GetText(text.data());
    text.resize(std::size_t(decl->GetTextLength()));
    if (stageBound(text, decl->GetFileName()) > MAX_SHADER_STAGES)
    {
      // the warning of the engine's Material.cpp:1641-1645, which it reports before it
      // overruns
      common->Warning(
        "material '%s' exceeded %i stages", decl->GetName(), MAX_SHADER_STAGES);
      decl->MakeDefault();
      refused.push_back(decl->Index());
    }
  }
  return refused;
}

std::vector<DeclTypeCount> declCounts(idDeclManagerLocal& manager)
{
  auto counts = std::vector<DeclTypeCount>{};
  for (int i = 0; i < manager.GetNumDeclTypes(); ++i)
  {
    if (const auto* declType = manager.GetDeclType(i))
    {
      counts.push_back(
        DeclTypeCount{declType->typeName.c_str(), manager.GetNumDecls(declType->type)});
    }
  }
  return counts;
}

std::string canonicalDeclName(const char* name)
{
  char canonicalName[MAX_STRING_CHARS + 1];
  idDeclManagerLocal::MakeNameCanonical(name, canonicalName, MAX_STRING_CHARS);
  return canonicalName;
}

int materialStageBound(const std::string& text, const char* name)
{
  return stageBound(text, name);
}

int materialDeclIndex(
  idDeclManagerLocal& manager, const char* name, const bool makeDefault)
{
  // FindDeclWithoutParsing's call (DeclManager.cpp:1379), keeping the idDeclLocal
  const auto* decl = manager.FindTypeWithoutParsing(DECL_MATERIAL, name, makeDefault);
  return decl ? decl->Index() : -1;
}

int declsInFile(const idDeclManagerLocal& manager, const char* fileName)
{
  const auto& files = manager.*get(LoadedFilesTag{});
  auto count = 0;
  for (int i = 0; i < files.Num(); ++i)
  {
    if (files[i]->fileName.Icmp(fileName) == 0)
    {
      for (const auto* decl = files[i]->decls; decl; decl = decl->*get(NextInFileTag{}))
      {
        ++count;
      }
    }
  }
  return count;
}

} // namespace tb::lit::shim
