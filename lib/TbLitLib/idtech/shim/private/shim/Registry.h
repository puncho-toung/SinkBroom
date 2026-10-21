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

// Included after precompiled.h. The registry owns every object the copied engine code
// reaches through its process globals, and binds those globals while it lives (C17). Only
// one registry exists at a time.

#include "lit/MaterialDecls.h"

#include <atomic>

class idDeclManagerLocal;

// the engine's declManagerLocal global, bound to the registry's decl manager
extern idDeclManagerLocal* tbLitDeclManagerLocal;

// framework/FileSystem.h's file system, over a DeclSource. It has the engine's name in
// the global namespace, so the copied idFileList's friend declaration lets it fill the
// list.
//
// Cancel (R5.20): the copied RegisterDeclFolder loops over a folder's files with no
// cancel point, so once the cancel flag is set every later ReadFile returns an empty
// file. The folder's loop then ends after the file it was reading, and nothing is
// reported.
class idFileSystemLocal : public idFileSystem
{
public:
  idFileSystemLocal(
    const tb::lit::DeclSource& source, const std::atomic<bool>* cancel = nullptr);

  // files left empty because the cancel flag was set
  int cancelledReads() const { return m_cancelledReads; }

  idFileList* ListFiles(
    const char* relativePath,
    const char* extension,
    bool sort = false,
    bool fullRelativePath = false,
    const char* gamedir = NULL) override;
  void FreeFileList(idFileList* fileList) override;
  int ReadFile(
    const char* relativePath, void** buffer, ID_TIME_T* timestamp = NULL) override;
  void FreeFile(void* buffer) override;
  idFile* OpenFileRead(
    const char* relativePath,
    bool allowCopyFiles = true,
    const char* gamedir = NULL) override;
  idFile* OpenExplicitFileRead(const char* OSPath) override;
  void CloseFile(idFile* f) override;

private:
  const tb::lit::DeclSource& m_source;
  const std::atomic<bool>* m_cancel;
  int m_cancelledReads = 0;
};

namespace tb::lit::shim
{

// the GUI dummies a material's "gui" keyword creates; freed after the image records
// (R5.11)
class UserInterfaceManagerShim : public idUserInterfaceManager
{
public:
  UserInterfaceManagerShim() = default;
  ~UserInterfaceManagerShim() override;

  UserInterfaceManagerShim(const UserInterfaceManagerShim&) = delete;
  UserInterfaceManagerShim& operator=(const UserInterfaceManagerShim&) = delete;

  idUserInterface* FindGui(
    const char* qpath,
    bool autoLoad = false,
    bool needUnique = false,
    bool forceUnique = false) override;

  void Shutdown();

private:
  idList<idUserInterface*, TAG_OLD_UI> m_guis;
};

class GameShim : public idGame
{
public:
  // the preview precaches nothing through the game
  void CacheDictionaryMedia(const idDict* dict) override;
};

class Registry
{
public:
  // the registry the globals are bound to, or nullptr
  static Registry* current();

  // binds the globals, creates the intrinsic images and registers every decl type and
  // folder the engine registers, in the engine's order (D33); cancel, when set, cuts the
  // decl files short (idFileSystemLocal)
  explicit Registry(const DeclSource& source, const std::atomic<bool>* cancel = nullptr);

  // frees in R5.11's order and unbinds the globals
  ~Registry();

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  void report(DeclMessageKind kind, const char* text);

  const std::vector<DeclMessage>& messages() const { return m_messages; }
  std::vector<DeclMessage> takeMessages();

  // Error, FatalError and Assert messages so far; a call that raises the count reached a
  // site where the engine would not have returned
  std::size_t errorCount() const { return m_errorCount; }

  // Between these, idCommon::Printf text goes into a buffer as the engine's console gets
  // it, newlines included, instead of into the messages (the print commands).
  void beginCapture();
  std::string endCapture();
  bool capturing() const { return m_capturing; }
  void capture(const char* text);

  bool cancelled() const { return m_fileSystem.cancelledReads() > 0; }

  idDeclManagerLocal& declManager() { return *m_declManager; }
  idImageManager& images() { return m_images; }
  idRenderProgManager& renderProgs() { return m_renderProgs; }

private:
  std::vector<DeclMessage> m_messages;
  std::size_t m_errorCount = 0;
  bool m_capturing = false;
  std::string m_capture;

  idCommon m_common;
  idCVarSystem m_cvarSystem;
  ::idFileSystemLocal m_fileSystem;
  idImageManager m_images;
  idRenderProgManager m_renderProgs;
  UserInterfaceManagerShim m_guis;
  GameShim m_game;
  idDeclManagerLocal* m_declManager = nullptr;

  friend void freeGuiDummies();
};

// The parts that need idDeclManagerLocal's definition, which lives in the copied
// DeclManager.cpp (shim/src/DeclManagerShim.cpp).

// a decl manager in the zeroed state the engine's global starts in, bound to declManager
// and declManagerLocal
idDeclManagerLocal* createDeclManager();

// Init, Init2 and the game's registrations (Game_local.cpp:323-331)
void initDeclManager(idDeclManagerLocal& declManager);

// Shutdown in R5.11's order (it frees the image records and GUI dummies in between), then
// the object itself; unbinds declManager and declManagerLocal
void destroyDeclManager(idDeclManagerLocal* declManager);

// the bytes of decl text every decl keeps
std::size_t declTextBytes(const idDeclManagerLocal& declManager);

// the registered decl folders, in registration order
std::vector<DeclFolderInfo> declFolders(const idDeclManagerLocal& declManager);

// the decl files read and the bytes they held (idDeclFile::fileSize)
int declFileCount(const idDeclManagerLocal& declManager);
std::size_t declFileBytes(const idDeclManagerLocal& declManager);

// every registered decl type with its number of decls, in type order
std::vector<DeclTypeCount> declCounts(idDeclManagerLocal& declManager);

// idDeclManagerLocal::MakeNameCanonical into a buffer of MAX_STRING_CHARS + 1, so a name
// of MAX_STRING_CHARS characters cannot write past it
std::string canonicalDeclName(const char* name);

// frees the GUI dummies (called by idDeclManagerLocal::Shutdown)
void freeGuiDummies();

} // namespace tb::lit::shim
