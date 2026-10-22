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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/FileSystem.cpp and
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

namespace tb::lit::shim
{

BufferedFile::BufferedFile(std::string path, std::string bytes, const ID_TIME_T time)
  : m_path{std::move(path)}
  , m_bytes{std::move(bytes)}
  , m_time{time}
{
}

const char* BufferedFile::GetFullPath() const
{
  return m_path.c_str();
}

// idFile_Memory::Read: at most len bytes from the position on, which moves past them
int BufferedFile::Read(void* buffer, const int len)
{
  if (len <= 0 || !buffer)
  {
    return 0;
  }
  const auto count = std::min(std::size_t(len), m_bytes.size() - m_position);
  memcpy(buffer, m_bytes.data() + m_position, count);
  m_position += count;
  return int(count);
}

int BufferedFile::Length() const
{
  return int(m_bytes.size());
}

ID_TIME_T BufferedFile::Timestamp() const
{
  return m_time;
}

} // namespace tb::lit::shim

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

  const auto path = std::string{relativePath ? relativePath : ""};
  if (!buffer)
  {
    auto found = tb::lit::GameFileProbe{};
    // the exception boundary (C17): a source that throws makes the file missing
    try
    {
      found = m_source.probe(path);
    }
    catch (const std::exception& e)
    {
      tb::lit::shim::reportWarning(path + " cannot be probed: " + e.what());
    }
    catch (...)
    {
      tb::lit::shim::reportWarning(path + " cannot be probed");
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

// OpenFileRead for the copied loaders (LP1c.2): the whole file read through the source's
// C8 search into a BufferedFile, closed by CloseFile; null when it cannot be read. The
// decl parsers parse decl text in memory and never call it.
idFile* idFileSystemLocal::OpenFileRead(const char* relativePath, bool, const char*)
{
  if (!relativePath || !relativePath[0] || (m_cancel && m_cancel->load()))
  {
    return nullptr;
  }
  const auto path = std::string{relativePath};
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
  if (!bytes || bytes->size() >= std::size_t(INT_MAX))
  {
    return nullptr;
  }
  return new (TAG_IDFILE)
    tb::lit::shim::BufferedFile{path, std::move(*bytes), ID_TIME_T(found.time)};
}

idFile* idFileSystemLocal::OpenExplicitFileRead(const char*)
{
  return nullptr;
}

void idFileSystemLocal::CloseFile(idFile* f)
{
  delete f;
}
