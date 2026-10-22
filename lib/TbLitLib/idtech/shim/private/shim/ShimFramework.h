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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/CmdSystem.h and
// framework/CVarSystem.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// The framework layer of the engine's precompiled.h: the command, cvar, common and file
// system interfaces the copied files call, then the copied decl headers. The enums,
// typedefs and macros come from verbatim header ranges; the classes are SinkBroom's and
// declare only the members the copied code calls.

// command system (framework/CmdSystem.h)
class idCommandLink;
#include "framework/CmdSystem.h"

// constexpr, so the copied CONSOLE_COMMANDs are constant-initialised and register nothing
// when the program starts (S27, C9); nothing reads what they name
class idCommandLink
{
public:
  constexpr idCommandLink(
    const char* cmdName,
    cmdFunction_t function,
    const char* description,
    argCompletion_t argCompletion = NULL)
    : m_cmdName{cmdName}
  {
    (void)function;
    (void)description;
    (void)argCompletion;
  }

private:
  const char* m_cmdName;
};

class idCmdSystem
{
public:
  // CmdSystem.h:218-224; named by idDeclManagerLocal::decl_show, never called
  template <int min, int max>
  static void ArgCompletion_Integer(
    const idCmdArgs& args, void (*callback)(const char* s))
  {
    for (int i = min; i <= max; i++)
    {
      callback(va("%s %d", args.Argv(0), i));
    }
  }
};

// cvars (framework/CVarSystem.h)
#include "framework/CVarSystem.h"

// A cvar at its engine default (D21). The constructors are constexpr, so the copied
// globals (r_useConstantMaterials, r_forceSoundOpAmplitude, decl_show) allocate and
// register nothing (S27). The getters follow idInternalCVar::UpdateValue's conversions.
class idCVar
{
public:
  constexpr idCVar(
    const char* name,
    const char* value,
    int flags,
    const char* description,
    argCompletion_t valueCompletion = NULL)
    : m_name{name}
    , m_value{value}
    , m_flags{flags}
  {
    (void)description;
    (void)valueCompletion;
  }

  constexpr idCVar(
    const char* name,
    const char* value,
    int flags,
    const char* description,
    float valueMin,
    float valueMax,
    argCompletion_t valueCompletion = NULL)
    : m_name{name}
    , m_value{value}
    , m_flags{flags}
  {
    (void)description;
    (void)valueMin;
    (void)valueMax;
    (void)valueCompletion;
  }

  const char* GetName() const { return m_name; }
  const char* GetString() const { return m_value; }
  int GetFlags() const { return m_flags; }

  bool GetBool() const { return GetInteger() != 0; }
  int GetInteger() const;
  float GetFloat() const;

private:
  const char* m_name;
  const char* m_value;
  int m_flags;
};

class idCVarSystem
{
public:
  // the integer value of a cvar the copied code names but does not declare, at its engine
  // default; 0 for an unknown name, as the engine's GetCVarInteger
  int GetCVarInteger(const char* name) const;
  // the same as text; "" for an unknown name, as the engine's GetCVarString (idCmdArgs'
  // cvar expansion, which the shim never feeds)
  const char* GetCVarString(const char* name) const;
};

extern idCVarSystem* cvarSystem;

// common (framework/Common.h)
#include "framework/Common.h"

// D63: the permanent, thread-safe sink. Its one object, tbLitCommonSink, lives for the
// process, and common and idLib::common point to it from the start (constant-initialised,
// C9, S27). Each call formats its message on the calling thread and queues it, tagged
// with that thread (shim/EngineMessages.h); the registry drains its own thread's, the
// owner of any other thread drains that thread's. Callable from any thread. Error and
// FatalError log and return (C17); FatalError and Error sites and what the copied code
// does after them are listed in mirror.json.
class idCommon
{
public:
  void Printf(const char* fmt, ...);
  void DPrintf(const char* fmt, ...);
  void Warning(const char* fmt, ...);
  void DWarning(const char* fmt, ...);
  void Error(const char* fmt, ...);
  void FatalError(const char* fmt, ...);

  // LP1c fix (S3.2): the binarizer's progress, which the copied DXT encoders report;
  // declared only, and defined by the test-only TbLitIdDxtTestLib (the product copies no
  // encoder, D24)
  void LoadPacifierBinarizeProgressIncrement(int count);
};

extern idCommon tbLitCommonSink;
extern idCommon* common;

// file system (framework/FileSystem.h)
#include "framework/FileSystem.h"

// the members of framework/File.h's idFile that the copied code calls
class idFile
{
public:
  virtual ~idFile() = default;

  virtual const char* GetFullPath() const = 0;
  virtual int Read(void* buffer, int len) = 0;
  virtual int Length() const = 0;
  virtual ID_TIME_T Timestamp() const = 0;
};

// the members of framework/FileSystem.h's idFileSystem that the copied code calls
class idFileSystem
{
public:
  virtual ~idFileSystem() = default;

  virtual idFileList* ListFiles(
    const char* relativePath,
    const char* extension,
    bool sort = false,
    bool fullRelativePath = false,
    const char* gamedir = NULL) = 0;
  virtual void FreeFileList(idFileList* fileList) = 0;
  virtual int ReadFile(
    const char* relativePath, void** buffer, ID_TIME_T* timestamp = NULL) = 0;
  virtual void FreeFile(void* buffer) = 0;
  virtual idFile* OpenFileRead(
    const char* relativePath, bool allowCopyFiles = true, const char* gamedir = NULL) = 0;
  virtual idFile* OpenExplicitFileRead(const char* OSPath) = 0;
  virtual void CloseFile(idFile* f) = 0;
};

extern idFileSystem* fileSystem;

// decls
// clang-format off: the order is the engine's precompiled.h order
#include "framework/DeclManager.h"
#include "framework/DeclTable.h"
#include "framework/DeclSkin.h"
#include "framework/DeclEntityDef.h"
// clang-format on
