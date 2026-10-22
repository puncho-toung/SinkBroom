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
// reaches through its process globals, and binds those globals while it lives (C17):
// cvarSystem, fileSystem, declManager, declManagerLocal, globalImages,
// renderProgManager, uiManager and game. common and idLib::common are not among them:
// they are the permanent sink of D63 (shim/EngineMessages.h), whose messages of the
// registry's thread the registry drains into its own. Only one registry exists at a
// time.

#include "lit/MaterialDecls.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

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
//
// It is the exception boundary between the source and the copied code (C17): a source
// call that throws is reported as a warning, and the copied caller sees an unreadable
// file or an empty folder. It also refuses, with a warning, a file the copied code cannot
// take: one of 2 GB or more (the engine's lengths are ints), and one holding a token, as
// the copied idLexer reads it, of MAX_STRING_CHARS or more characters without a dot among
// the first MAX_STRING_CHARS, which the copied MakeNameCanonical writes past its buffer
// for (M-ID-GUARD-06).
//
// A5: ReadFile with no buffer (the loaders' and GetTimestamp's time-only calls) asks the
// source's probe, which reads no file body: the time is the engine's (A4) and the length
// the probe's. OpenFileRead opens nothing (LP1c fix S1.7): no copied code that runs calls
// it.
//
// LP1c.2: ReadFile with a buffer reads decl text, with the decl guards, except between
// beginImageReads and endImageReads, where the copied image loaders read: there it reads
// the game file through the source's readGameFile (no decl bookkeeping, no token scan)
// and validates it with imageFileProblem before the loader sees it (D51). A file that
// fails is read as missing, and kept, once per path, for endImageReads.
class idFileSystemLocal : public idFileSystem
{
public:
  // a file the loader pre-validation refused (D51)
  struct ImageRejection
  {
    std::string path;
    std::string reason;
  };

  // a file a probe or an image read found (the time call's leaves, a loader's file)
  struct FoundFile
  {
    std::string path;
    tb::lit::GameFileProbe probe;
  };

  idFileSystemLocal(
    tb::lit::DeclSource& source, const std::atomic<bool>* cancel = nullptr);

  // files left empty because the cancel flag was set
  int cancelledReads() const { return m_cancelledReads; }
  // LP1c fix (S2.2): the cancel flag is set (the registry is stopping)
  bool cancelling() const { return m_cancel && m_cancel->load(); }
  // LP1c fix (S2.1): the source, for the shim's own header reads (parse thread)
  tb::lit::DeclSource& source() { return m_source; }

  // the copied loaders read between these (parse thread); not nested. LP1c fix (S2.8):
  // ImageReads begins them and ends them however its scope is left
  void beginImageReads();
  std::vector<ImageRejection> endImageReads();

  // every file a probe or an image read looks for between these, in order, found or not
  // (LP1c fix S2.5: a missing one has a Missing probe) (parse thread); not nested
  void beginRecording();
  std::vector<FoundFile> endRecording();

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
  int readImageFile(const std::string& path, void** buffer, ID_TIME_T* timestamp);
  int readImageFileUnguarded(
    const std::string& path, void** buffer, ID_TIME_T* timestamp);
  void record(const std::string& path, const tb::lit::GameFileProbe& probe);

  tb::lit::DeclSource& m_source;
  const std::atomic<bool>* m_cancel;
  int m_cancelledReads = 0;
  bool m_imageReads = false;
  std::vector<ImageRejection> m_rejections;
  bool m_recording = false;
  std::vector<FoundFile> m_found;
};

namespace tb::lit::shim
{

/**
 * LP1c fix (S2.8): the file system's image reads and recording for one load, begun when
 * it is made and ended when it goes, also when the load throws, so that later decl reads
 * never skip their guards (M-ID-GUARD-06). take ends them and hands over what they kept.
 */
class ImageReads
{
public:
  explicit ImageReads(idFileSystemLocal& files)
    : m_files{files}
  {
    m_files.beginRecording();
    m_files.beginImageReads();
  }

  ~ImageReads()
  {
    if (!m_taken)
    {
      static_cast<void>(m_files.endImageReads());
      static_cast<void>(m_files.endRecording());
    }
  }

  ImageReads(const ImageReads&) = delete;
  ImageReads& operator=(const ImageReads&) = delete;

  std::pair<
    std::vector<idFileSystemLocal::ImageRejection>,
    std::vector<idFileSystemLocal::FoundFile>>
  take()
  {
    m_taken = true;
    auto rejections = m_files.endImageReads();
    return {std::move(rejections), m_files.endRecording()};
  }

private:
  idFileSystemLocal& m_files;
  bool m_taken = false;
};

// LP1c fix (S2.5): what a recording (idFileSystemLocal::endRecording) looked for: the
// files found, in order, and the names missed, once each
void splitRecorded(
  std::vector<idFileSystemLocal::FoundFile> recorded,
  std::vector<FoundGameFile>& found,
  std::vector<FoundGameFile>& missing);

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

  // claims the process's one registry slot, atomically (C17); false when it is held
  static bool claim();
  static void release();

  // binds the globals, creates the intrinsic images and registers every decl type and
  // folder the engine registers, in the engine's order (D33); cancel, when set, cuts the
  // decl files short (idFileSystemLocal). The caller holds the slot (claim). The calling
  // thread is the registry's thread: every call into it runs there (C20).
  explicit Registry(DeclSource& source, const std::atomic<bool>* cancel = nullptr);

  // frees in R5.11's order and unbinds the globals
  ~Registry();

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // The messages the registry's thread reported to the sink of D63, oldest first; each of
  // these takes the thread's new messages out of the sink first.
  const std::vector<DeclMessage>& messages() const;
  std::vector<DeclMessage> takeMessages();
  // LP1c.2: the messages from index first on, taken out (a quiet image load keeps them);
  // the errors among them stay counted
  std::vector<DeclMessage> takeMessagesFrom(std::size_t first);

  // Error and FatalError messages so far, and those any thread could not keep for want of
  // memory; a call that raises the count reached a site where the engine would not have
  // returned. An Assert is reported but never counted, so that Debug and Release parse
  // alike (asserts are compiled out of Release).
  std::size_t errorCount() const;

  // Between these, the registry's thread's idCommon::Printf text goes into a buffer as
  // the engine's console gets it, newlines included, instead of into the messages (the
  // print commands).
  void beginCapture();
  std::string endCapture();

  bool cancelled() const { return m_fileSystem.cancelledReads() > 0; }

  idDeclManagerLocal& declManager() { return *m_declManager.manager; }
  ::idFileSystemLocal& files() { return m_fileSystem; }
  idImageManager& images() { return m_images; }
  idRenderProgManager& renderProgs() { return m_renderProgs; }

private:
  // Binds the globals when it is made and unbinds them when it goes, so that a registry
  // whose construction throws leaves no global bound to its members (A2.1).
  class Bindings
  {
  public:
    explicit Bindings(Registry& registry);
    ~Bindings();

    Bindings(const Bindings&) = delete;
    Bindings& operator=(const Bindings&) = delete;
  };

  // owns the decl manager, which it shuts down while the globals are still bound
  struct DeclManagerOwner
  {
    idDeclManagerLocal* manager = nullptr;

    DeclManagerOwner();
    ~DeclManagerOwner();

    DeclManagerOwner(const DeclManagerOwner&) = delete;
    DeclManagerOwner& operator=(const DeclManagerOwner&) = delete;
  };

  /**
   * The registry's thread, whose messages it drains from the sink (D63). Made first and
   * gone last: what the thread reported before the registry existed, and what it reports
   * while the registry is destroyed, is dropped, as no registry received it before.
   */
  class OwnThread
  {
  public:
    OwnThread();
    ~OwnThread();

    OwnThread(const OwnThread&) = delete;
    OwnThread& operator=(const OwnThread&) = delete;

    std::thread::id id() const { return m_id; }

  private:
    std::thread::id m_id;
  };

  OwnThread m_thread;
  // the thread's messages as the registry has drained them; draining is how a const
  // accessor brings them up to date
  mutable std::vector<DeclMessage> m_messages;
  mutable std::size_t m_errorCount = 0;
  bool m_capturing = false;
  mutable std::string m_capture;

  idCVarSystem m_cvarSystem;
  ::idFileSystemLocal m_fileSystem;
  idImageManager m_images;
  idRenderProgManager m_renderProgs;
  UserInterfaceManagerShim m_guis;
  GameShim m_game;
  // made after the objects it binds and gone before them
  Bindings m_bindings;
  // made after the bindings and gone before them
  DeclManagerOwner m_declManager;

  // takes the registry's thread's messages out of the sink: console text into the capture
  // while one runs, the rest into the messages
  void drain() const;
  void report(DeclMessageKind kind, std::string text) const;

  friend void freeGuiDummies();
};

// Every token the copied idLexer reads from decl text with DECL_LEXER_FLAGS, as the
// copied decl parsers read it (LoadAndParse, idMaterial::Parse), in order, without the
// lexer's messages; name is the lexer's file name. After a token the lexer cannot read,
// the scan goes on wherever the lexer moved forward, so it sees every token a copied
// parser can see (M-ID-GUARD-05, -06).
void forEachDeclToken(
  const std::string& text,
  const char* name,
  const std::function<void(const idToken&)>& visit);

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

// M-ID-GUARD-05: every material decl whose text can make the copied parser write more
// than MAX_SHADER_STAGES stage slots is made the default material before anything can
// parse it, because the copied ParseStage writes past parseStages for the stage after the
// last (and its coverage stage of an alpha-tested diffuse stage, unchecked); the engine's
// warning is reported. The bound is taken from the copied lexer's tokens: every "{" after
// the material's first, plus twice every stage shortcut keyword, so it refuses some
// materials the engine parses (M-ID-DIFF-03). Returns their indices.
std::vector<int> refuseOversizedMaterials(idDeclManagerLocal& declManager);

// idDeclManagerLocal::MakeNameCanonical into a buffer of MAX_STRING_CHARS + 1, so a name
// of MAX_STRING_CHARS characters cannot write past it
std::string canonicalDeclName(const char* name);

// frees the GUI dummies (called by idDeclManagerLocal::Shutdown)
void freeGuiDummies();

} // namespace tb::lit::shim
