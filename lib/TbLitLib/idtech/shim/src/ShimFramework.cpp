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

// The framework services for the copied code: cvars at engine defaults, the message
// collector behind idCommon, and the file system over a DeclSource.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/Registry.h"
// clang-format on

idCommon* common = nullptr;
idCVarSystem* cvarSystem = nullptr;
idFileSystem* fileSystem = nullptr;

// Common.h: no editor is open; the entityDef parser reads it
int com_editors = 0;

namespace
{

void reportV(const tb::lit::DeclMessageKind kind, const char* fmt, va_list argptr)
{
  char text[MAX_PRINT_MSG];
  idStr::vsnPrintf(text, sizeof(text), fmt, argptr);
  if (auto* registry = tb::lit::shim::Registry::current())
  {
    registry->report(kind, text);
  }
}

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
  va_list argptr;
  va_start(argptr, fmt);
  auto* registry = tb::lit::shim::Registry::current();
  if (registry && registry->capturing())
  {
    // the print commands: the text as the engine's console gets it
    char text[MAX_PRINT_MSG];
    idStr::vsnPrintf(text, sizeof(text), fmt, argptr);
    registry->capture(text);
  }
  else
  {
    reportV(tb::lit::DeclMessageKind::Info, fmt, argptr);
  }
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

idFileSystemLocal::idFileSystemLocal(
  const tb::lit::DeclSource& source, const std::atomic<bool>* cancel)
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
  for (const auto& name : m_source.listFiles(folder, std::string{extension}))
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
// -1 when the file cannot be read
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

  auto time = std::int64_t{0};
  const auto text = m_source.readFile(std::string{relativePath}, &time);
  if (timestamp)
  {
    *timestamp = text ? ID_TIME_T(time) : FILE_NOT_FOUND_TIMESTAMP;
  }
  if (!text)
  {
    return -1;
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

// the lexer's LoadFile is never used by the decl parsers, which parse decl text in memory
idFile* idFileSystemLocal::OpenFileRead(const char*, bool, const char*)
{
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
