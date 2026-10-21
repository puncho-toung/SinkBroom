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

// The decl manager: the copied ranges of framework/DeclManager.cpp, compiled here because
// idDeclLocal, idDeclFile and idDeclManagerLocal are defined inside that file (R5.10),
// and the shim's definitions of the members the copy leaves out. Each of those is listed
// in mirror.json ("virtuals").

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

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

template <typename Tag, typename Tag::type Member>
struct PrivateMember
{
  friend typename Tag::type get(Tag) { return Member; }
};

template struct PrivateMember<LinearListsTag, &idDeclManagerLocal::linearLists>;

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

std::string canonicalDeclName(const char* name)
{
  char canonicalName[MAX_STRING_CHARS + 1];
  idDeclManagerLocal::MakeNameCanonical(name, canonicalName, MAX_STRING_CHARS);
  return canonicalName;
}

} // namespace tb::lit::shim
