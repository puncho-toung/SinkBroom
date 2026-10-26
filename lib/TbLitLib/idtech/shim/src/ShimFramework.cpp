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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/FileSystem.cpp, framework/File.cpp and
// framework/CVarSystem.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// The framework services for the copied code: cvars at engine defaults, the permanent
// message sink behind idCommon (D63), and the file system over a DeclSource.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
// clang-format on

// D63: the process's one sink; common and idLib::common point to it from the start and
// are never rebound. cvarSystem and fileSystem are bound by the registry (C17, S6).
TB_LIT_CONSTINIT idCommon tbLitCommonSink;
idCommon* common = &tbLitCommonSink;
idCVarSystem* cvarSystem = nullptr;
idFileSystem* fileSystem = nullptr;

// Common.h: no editor is open; the entityDef parser reads it
int com_editors = 0;

namespace
{

using tb::lit::shim::reportV;

// the cvars the copied code reads through cvarSystem by name, at their engine defaults
struct NamedCVar
{
  const char* name;
  const char* value;
  int flags;
};

// FileSystem.cpp:336
constexpr NamedCVar NamedCVars[] = {
  {"fs_copyfiles", "0", CVAR_SYSTEM | CVAR_INIT | CVAR_BOOL},
};

int integerValue(const char* value, const int flags)
{
  // idInternalCVar::UpdateValue
  if (flags & CVAR_BOOL)
  {
    return atoi(value) != 0 ? 1 : 0;
  }
  if (flags & CVAR_INTEGER)
  {
    return atoi(value);
  }
  return int(float(atof(value)));
}

} // namespace

int idCVar::GetInteger() const
{
  return integerValue(m_value, m_flags);
}

float idCVar::GetFloat() const
{
  if (m_flags & (CVAR_BOOL | CVAR_INTEGER))
  {
    return float(GetInteger());
  }
  return float(atof(m_value));
}

int idCVarSystem::GetCVarInteger(const char* name) const
{
  for (const auto& cvar : NamedCVars)
  {
    if (idStr::Icmp(cvar.name, name) == 0)
    {
      return integerValue(cvar.value, cvar.flags);
    }
  }
  return 0;
}

const char* idCVarSystem::GetCVarString(const char* name) const
{
  for (const auto& cvar : NamedCVars)
  {
    if (idStr::Icmp(cvar.name, name) == 0)
    {
      return cvar.value;
    }
  }
  return "";
}

void idCommon::Printf(const char* fmt, ...)
{
  // console text: a registry that captures a print command keeps it as the engine's
  // console gets it, newlines included (Registry::beginCapture)
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Info, fmt, argptr, true);
  va_end(argptr);
}

void idCommon::DPrintf(const char*, ...)
{
  // developer prints (com_developer 0)
}

void idCommon::Warning(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Warning, fmt, argptr);
  va_end(argptr);
}

void idCommon::DWarning(const char*, ...)
{
  // developer warnings (com_developer 0)
}

void idCommon::Error(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Error, fmt, argptr);
  va_end(argptr);
}

void idCommon::FatalError(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::FatalError, fmt, argptr);
  va_end(argptr);
}

namespace tb::lit::shim
{

void forEachDeclToken(
  const std::string& text,
  const char* name,
  const std::function<void(const idToken&)>& visit)
{
  // the copied idLexer::LoadMemory takes an int length, and the lexer stops at the NUL
  // that std::string keeps after the text, as it stops at the one ReadFile appends
  auto lexer = idLexer{};
  if (!lexer.LoadMemory(text.c_str(), int(text.size()), name))
  {
    return;
  }
  // LEXFL_NOERRORS and LEXFL_NOWARNINGS change only whether the lexer prints
  // (idLexer::Error and idLexer::Warning return before printing), so the scan reads the
  // tokens the copied parsers read and reports nothing they would not
  lexer.SetFlags(DECL_LEXER_FLAGS | LEXFL_NOERRORS | LEXFL_NOWARNINGS);

  auto token = idToken{};
  for (;;)
  {
    const auto offset = lexer.GetFileOffset();
    if (lexer.ReadToken(&token))
    {
      visit(token);
      continue;
    }
    // A token the lexer cannot read: LoadAndParse ignores a failed SkipBracedSection and
    // reads on from where the lexer stopped, and so does a material parse after a failed
    // ReadToken, so the scan goes on too while the lexer moves forward. Where it cannot
    // move (the text's end or NUL, an unknown punctuation), every parser stops as well.
    if (lexer.EndOfFile() || lexer.GetFileOffset() == offset)
    {
      return;
    }
  }
}

} // namespace tb::lit::shim

namespace
{

/**
 * M-ID-GUARD-06: whether the text holds a token that the copied MakeNameCanonical writes
 * one byte past its MAX_STRING_CHARS buffer for: a token of MAX_STRING_CHARS or more
 * characters with no '.' among the first MAX_STRING_CHARS (a dot there ends the canonical
 * name before the buffer does). Any token can reach it as a decl name, an inherit value,
 * a table or a material a skin names, so a file holding one is never parsed. The tokens
 * are the copied idLexer's with DECL_LEXER_FLAGS (forEachDeclToken), so a string joined
 * across a backslash, a '...' literal with spaces, a name after a number and the lexer's
 * comment rules are read exactly as the engine reads them.
 */
bool holdsOverlongToken(const std::string& text, const std::string& path)
{
  const auto limit = std::size_t(MAX_STRING_CHARS);
  auto found = false;
  tb::lit::shim::forEachDeclToken(text, path.c_str(), [&](const idToken& token) {
    found =
      found
      || (std::size_t(token.Length()) >= limit && memchr(token.c_str(), '.', limit) == nullptr);
  });
  return found;
}

} // namespace

bool tb::lit::shim::holdsOverlongDeclToken(
  const std::string& text, const std::string& path)
{
  return holdsOverlongToken(text, path);
}

idFileSystemLocal::idFileSystemLocal(
  tb::lit::DeclSource& source, const std::atomic<bool>* cancel)
  : m_source{source}
  , m_cancel{cancel}
{
}

// FileSystem.cpp:2537-2555: the list of the folder's files, sorted with idSort_PathStr;
// the source plays GetFileList's part (flat, merged, unique)
idFileList* idFileSystemLocal::ListFiles(
  const char* relativePath,
  const char* extension,
  bool sort,
  bool fullRelativePath,
  const char*)
{
  auto* fileList = new (TAG_IDFILE) idFileList;
  fileList->basePath = relativePath;

  const auto folder = std::string{relativePath};
  auto names = std::vector<std::string>{};
  // the exception boundary (C17): a source that throws lists nothing
  try
  {
    names = m_source.listFiles(folder, std::string{extension});
  }
  catch (const std::exception& e)
  {
    tb::lit::shim::reportWarning(
      "The decl files of " + folder + " cannot be listed: " + e.what());
  }
  catch (...)
  {
    tb::lit::shim::reportWarning("The decl files of " + folder + " cannot be listed");
  }

  for (const auto& name : names)
  {
    auto entry = idStr{name.c_str()};
    if (fullRelativePath)
    {
      entry = idStr{(folder + "/" + name).c_str()};
    }

    // GetFileList adds each name once, case-insensitively
    auto found = false;
    for (int i = 0; i < fileList->list.Num(); ++i)
    {
      if (fileList->list[i].Icmp(entry) == 0)
      {
        found = true;
        break;
      }
    }
    if (!found)
    {
      fileList->list.Append(entry);
    }
  }

  if (sort)
  {
    fileList->list.SortWithTemplate(idSort_PathStr());
  }

  return fileList;
}

void idFileSystemLocal::FreeFileList(idFileList* fileList)
{
  delete fileList;
}

// FileSystem.cpp's ReadFile: a Mem_Alloc'ed buffer of length + 1 bytes, NUL-terminated;
// -1 when the file cannot be read. A5: with no buffer only the time is wanted, which the
// source's probe answers without reading the file (the length is the probe's)
int idFileSystemLocal::ReadFile(
  const char* relativePath, void** buffer, ID_TIME_T* timestamp)
{
  if (buffer)
  {
    *buffer = nullptr;
  }

  // LP7.1a: a model read serves the bytes it was handed, and nothing else
  if (m_modelRead)
  {
    return readModelFile(relativePath ? relativePath : "", buffer, timestamp);
  }

  if (m_cancel && m_cancel->load() && m_imageReads && buffer)
  {
    // the registry is stopping: an image file reads as missing, never as an empty buffer,
    // which a loader would read its header from (D51)
    if (timestamp)
    {
      *timestamp = FILE_NOT_FOUND_TIMESTAMP;
    }
    return -1;
  }
  if (m_cancel && m_cancel->load())
  {
    // R5.20: the registry is stopping; the file reads as empty, so LoadAndParse finds no
    // decl in it and the folder's loop goes on to its end without reading anything
    ++m_cancelledReads;
    if (timestamp)
    {
      *timestamp = 0;
    }
    if (buffer)
    {
      auto* bytes = static_cast<char*>(Mem_Alloc(1, TAG_IDFILE));
      bytes[0] = '\0';
      *buffer = bytes;
    }
    return 0;
  }

  if (timestamp)
  {
    *timestamp = FILE_NOT_FOUND_TIMESTAMP;
  }

  // LP1c fix (S2.8): the name, the probe and its record allocate; a failure there makes
  // the file missing, never an exception through the copied caller
  auto path = std::string{};
  try
  {
    path = relativePath ? relativePath : "";
  }
  catch (...)
  {
    return -1;
  }
  if (!buffer)
  {
    auto found = tb::lit::GameFileProbe{};
    // the exception boundary (C17): a source that throws makes the file missing
    try
    {
      found = m_source.probe(path);
      // LP1c fix (S2.5, S3.4): a miss is recorded too, so that a file that appears
      // later is seen by the re-probe
      record(path, found);
    }
    catch (const std::exception& e)
    {
      tb::lit::shim::reportWarning(path + " cannot be probed: " + e.what());
      found = tb::lit::GameFileProbe{};
    }
    catch (...)
    {
      tb::lit::shim::reportWarning(path + " cannot be probed");
      found = tb::lit::GameFileProbe{};
    }
    if (!found.exists())
    {
      return -1;
    }
    if (timestamp)
    {
      *timestamp = ID_TIME_T(found.time);
    }
    return int(std::min(found.size, std::uint64_t(INT_MAX)));
  }

  if (m_imageReads)
  {
    return readImageFile(path, buffer, timestamp);
  }

  auto time = std::int64_t{0};
  auto text = std::optional<std::string>{};
  // the exception boundary (C17): a source that throws makes the file unreadable
  try
  {
    text = m_source.readFile(path, &time);
  }
  catch (const std::exception& e)
  {
    tb::lit::shim::reportWarning(path + " cannot be read: " + e.what());
  }
  catch (...)
  {
    tb::lit::shim::reportWarning(path + " cannot be read");
  }
  if (!text)
  {
    return -1;
  }

  // the engine's lengths are ints
  if (text->size() >= std::size_t(INT_MAX))
  {
    tb::lit::shim::reportWarning(path + " is 2 GB or larger; it is not read");
    return -1;
  }
  if (holdsOverlongToken(*text, path))
  {
    tb::lit::shim::reportWarning(
      path
      + " holds a token of 1024 or more characters with no '.' among the first 1024, "
        "which the engine's MakeNameCanonical cannot take; it is not read");
    return -1;
  }

  if (timestamp)
  {
    *timestamp = ID_TIME_T(time);
  }
  const auto length = int(text->size());
  if (buffer)
  {
    auto* bytes = static_cast<char*>(Mem_Alloc(size_t(length) + 1, TAG_IDFILE));
    memcpy(bytes, text->data(), size_t(length));
    bytes[length] = '\0';
    *buffer = bytes;
  }
  return length;
}

void idFileSystemLocal::FreeFile(void* buffer)
{
  Mem_Free(buffer);
}

void idFileSystemLocal::beginModelRead(
  std::string path, std::string bytes, const std::int64_t time)
{
  m_modelRead = ModelFile{std::move(path), std::move(bytes), time};
}

void idFileSystemLocal::endModelRead()
{
  m_modelRead.reset();
}

// LP7.1a: ReadFile during a model read: the handed file as FileSystem.cpp's ReadFile
// returns one (a Mem_Alloc'ed, NUL-terminated buffer of length + 1 bytes) with the handed
// time; another name, and a file the engine's int lengths cannot hold, are missing
int idFileSystemLocal::readModelFile(
  const std::string& path, void** buffer, ID_TIME_T* timestamp) const
{
  if (timestamp)
  {
    *timestamp = FILE_NOT_FOUND_TIMESTAMP;
  }
  if (path != m_modelRead->path || m_modelRead->bytes.size() >= std::size_t(INT_MAX))
  {
    return -1;
  }
  const auto length = int(m_modelRead->bytes.size());
  if (timestamp)
  {
    *timestamp = ID_TIME_T(m_modelRead->time);
  }
  if (buffer)
  {
    auto* bytes = static_cast<char*>(Mem_Alloc(size_t(length) + 1, TAG_IDFILE));
    memcpy(bytes, m_modelRead->bytes.data(), size_t(length));
    bytes[length] = '\0';
    *buffer = bytes;
  }
  return length;
}

void idFileSystemLocal::beginImageReads()
{
  m_imageReads = true;
  m_rejections.clear();
}

std::vector<idFileSystemLocal::ImageRejection> idFileSystemLocal::endImageReads()
{
  m_imageReads = false;
  return std::exchange(m_rejections, {});
}

void idFileSystemLocal::beginRecording()
{
  m_recording = true;
  m_found.clear();
}

std::vector<idFileSystemLocal::FoundFile> idFileSystemLocal::endRecording()
{
  m_recording = false;
  return std::exchange(m_found, {});
}

void idFileSystemLocal::record(
  const std::string& path, const tb::lit::GameFileProbe& probe)
{
  if (m_recording)
  {
    m_found.push_back(FoundFile{path, probe});
  }
}

/**
 * LP1c.2 (D51, follow-up 3 of LP1c.1): an image file for a copied loader, read through
 * the source's game-file search, with no decl bookkeeping and no decl token scan, then
 * validated outside the copied code (imageFileProblem). A file that fails is read as
 * missing (-1, no buffer, FILE_NOT_FOUND_TIMESTAMP), so the loader returns without a
 * picture and reads nothing; the refusal is kept once per path for endImageReads.
 * Otherwise the buffer is ReadFile's: Mem_Alloc'ed, NUL-terminated, length + 1 bytes.
 */
int idFileSystemLocal::readImageFile(
  const std::string& path, void** buffer, ID_TIME_T* timestamp)
{
  // LP1c fix (S2.8): nothing it does throws through the copied loader that called it (an
  // allocation failure in the pre-validation, the record or the refusal list): the file
  // reads as missing
  try
  {
    return readImageFileUnguarded(path, buffer, timestamp);
  }
  catch (...)
  {
    if (timestamp)
    {
      *timestamp = FILE_NOT_FOUND_TIMESTAMP;
    }
    return -1;
  }
}

int idFileSystemLocal::readImageFileUnguarded(
  const std::string& path, void** buffer, ID_TIME_T* timestamp)
{
  auto found = tb::lit::GameFileProbe{};
  auto bytes = std::optional<std::string>{};
  // the exception boundary (C17): a source that throws makes the file unreadable
  try
  {
    bytes = m_source.readGameFile(path, &found);
  }
  catch (const std::exception& e)
  {
    tb::lit::shim::reportWarning(path + " cannot be read: " + e.what());
  }
  catch (...)
  {
    tb::lit::shim::reportWarning(path + " cannot be read");
  }
  // LP1c fix (S2.5): a miss is recorded too, with its Missing probe
  record(path, bytes ? found : tb::lit::GameFileProbe{});
  if (!bytes)
  {
    return -1;
  }

  auto problem = std::optional<std::string>{};
  if (bytes->size() >= std::size_t(INT_MAX))
  {
    problem = "it is 2 GB or larger";
  }
  else
  {
    problem = tb::lit::imageFileProblem(path, *bytes);
  }
  if (problem)
  {
    const auto known =
      std::any_of(m_rejections.begin(), m_rejections.end(), [&](const auto& rejection) {
        return rejection.path == path;
      });
    if (!known)
    {
      m_rejections.push_back(ImageRejection{path, std::move(*problem)});
    }
    return -1;
  }

  if (timestamp)
  {
    *timestamp = ID_TIME_T(found.time);
  }
  const auto length = int(bytes->size());
  auto* data = static_cast<char*>(Mem_Alloc(size_t(length) + 1, TAG_IDFILE));
  memcpy(data, bytes->data(), size_t(length));
  data[length] = '\0';
  *buffer = data;
  return length;
}

// LP1c fix (S1.7): no copied code that runs opens a file (the loaders call ReadFile, and
// idLexer::LoadFile, the copy's only caller, is never reached), so nothing is opened, as
// LP1a's stub did (M-ID-FN-27). LP7.1b (P7.13): except the model read's one file, which
// the copied LWO reader opens (Model_lwo.cpp:1784): a memory file over the bytes and the
// time handed in; a file the engine's int lengths cannot hold opens nothing, as ReadFile
// reads it missing
idFile* idFileSystemLocal::OpenFileRead(const char* relativePath, bool, const char*)
{
  if (
    m_modelRead && relativePath && m_modelRead->path == relativePath
    && m_modelRead->bytes.size() < std::size_t(INT_MAX))
  {
    return new tb::lit::shim::MemoryFile{
      m_modelRead->path, m_modelRead->bytes, m_modelRead->time};
  }
  return nullptr;
}

idFile* idFileSystemLocal::OpenExplicitFileRead(const char*)
{
  return nullptr;
}

void idFileSystemLocal::CloseFile(idFile* f)
{
  delete f;
}

const char* idFileSystemLocal::OSPathToRelativePath(const char* OSPath)
{
  return tb::lit::shim::osPathToRelativePath(OSPath);
}

int idFileSystemLocal::GetFileLength(const char* relativePath)
{
  // FileSystem.cpp:455-459: an empty name warns and is -1
  if (!relativePath || !relativePath[0])
  {
    idLib::Warning("idFileSystemLocal::GetFileLength with empty name");
    return -1;
  }
  if (
    m_modelRead && m_modelRead->path == relativePath
    && m_modelRead->bytes.size() < std::size_t(INT_MAX))
  {
    return int(m_modelRead->bytes.size());
  }
  return -1;
}

int idFile::ReadUnsignedInt(unsigned int& value)
{
  // File.cpp:351-356
  int result = Read(&value, sizeof(value));
  value = static_cast<unsigned int>(LittleLong(static_cast<int>(value)));
  return result;
}

namespace tb::lit::shim
{

MemoryFile::MemoryFile(std::string name, std::string bytes, const std::int64_t time)
  : m_name{std::move(name)}
  , m_bytes{std::move(bytes)}
  , m_time{time}
{
}

const char* MemoryFile::GetFullPath() const
{
  return m_name.c_str();
}

int MemoryFile::Read(void* buffer, int len)
{
  // File.cpp:780-796: what is left when asked for more
  if (len <= 0)
  {
    return 0;
  }
  const auto left = m_bytes.size() - m_at;
  const auto count = std::min(std::size_t(len), left);
  memcpy(buffer, m_bytes.data() + m_at, count);
  m_at += count;
  return int(count);
}

int MemoryFile::Length() const
{
  return int(m_bytes.size());
}

ID_TIME_T MemoryFile::Timestamp() const
{
  return ID_TIME_T(m_time);
}

int MemoryFile::Tell() const
{
  return int(m_at);
}

int MemoryFile::Seek(const long offset, const fsOrigin_t origin)
{
  // File.cpp:1000-1036: from the current place, the end or the start; clamped to the
  // file, -1 when it had to be
  const auto size = static_cast<long long>(m_bytes.size());
  auto at = static_cast<long long>(m_at);
  switch (origin)
  {
  case FS_SEEK_CUR:
    at += offset;
    break;
  case FS_SEEK_END:
    at = size - offset;
    break;
  case FS_SEEK_SET:
    at = offset;
    break;
  default:
    common->FatalError("idFile_Memory::Seek: bad origin for %s\n", m_name.c_str());
    return -1;
  }
  if (at < 0)
  {
    m_at = 0;
    return -1;
  }
  if (at > size)
  {
    m_at = m_bytes.size();
    return -1;
  }
  m_at = std::size_t(at);
  return 0;
}

const char* osPathToRelativePath(const char* OSPath)
{
  // FileSystem.cpp:1843-1847: no colon and no leading slash: already relative
  if ((OSPath[0] != '/') && (OSPath[0] != '\\') && (idStr::FindChar(OSPath, ':') < 0))
  {
    return OSPath;
  }
  // :1849-1860: the base names; fs_game and fs_game_base are empty in the preview (C8)
  static constexpr const char* basePaths[] = {"base", "d3xp", "d3le"};
  const auto baseLength = [](const char* base) { return int(strlen(base)); };

  // :1861-1868: the slashes, as many as an idStaticList< int, MAX_OSPATH > keeps
  auto slashes = std::vector<int>{};
  for (const char* s = OSPath; *s != 0; s++)
  {
    if ((*s == '/' || *s == '\\') && slashes.size() < std::size_t(MAX_OSPATH))
    {
      slashes.push_back(int(s - OSPath));
    }
  }
  const auto count = int(slashes.size());
  for (int n = 0; n < count - 1; n++)
  {
    const char* start = OSPath + slashes[std::size_t(n)] + 1;
    const char* end = OSPath + slashes[std::size_t(n + 1)];
    const int componentLength = int(end - start);
    if (componentLength == 0)
    {
      continue;
    }
    for (const auto* base : basePaths)
    {
      if (componentLength != baseLength(base))
      {
        continue;
      }
      if (idStr::Icmpn(base, start, componentLength) == 0)
      {
        // :1884-1906: two base names in a row: after the second
        if (n < count - 2)
        {
          const char* start2 = OSPath + slashes[std::size_t(n + 1)] + 1;
          const char* end2 = OSPath + slashes[std::size_t(n + 2)];
          const int componentLength2 = int(end2 - start2);
          if (componentLength2 > 0)
          {
            for (const auto* base2 : basePaths)
            {
              if (componentLength2 != baseLength(base2))
              {
                continue;
              }
              if (idStr::Icmpn(base2, start2, baseLength(base2)) == 0)
              {
                return end2 + 1;
              }
            }
          }
        }
        return end + 1;
      }
    }
  }
  idLib::Warning("OSPathToRelativePath failed on %s", OSPath);
  return OSPath;
}

} // namespace tb::lit::shim
