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

// The idLib layer of the engine's precompiled.h (idlib/Lib.h): the idLib class, the
// memory functions and the copied idLib headers in Lib.h's order. Only what the copied
// files use is here.

class idSys;
class idCommon;
class idCVarSystem;
class idFileSystem;

// idlib/Lib.h's idLib. Error and FatalError log and return: they are not NO_RETURN here,
// because nothing may throw or jump through copied code (C17).
class idLib
{
public:
  static idSys* sys;
  static idCommon* common;
  static idCVarSystem* cvarSystem;
  static idFileSystem* fileSystem;
  static int frameNumber;

  static void Printf(const char* fmt, ...);
  static void PrintfIf(const bool test, const char* fmt, ...);
  static void Error(const char* fmt, ...);
  static void FatalError(const char* fmt, ...);
  static void Warning(const char* fmt, ...);
  static void WarningIf(const bool test, const char* fmt, ...);

  // every call into the copied code runs on the parse thread
  static bool IsMainThread() { return true; }
};

typedef int qhandle_t;

class idFile;
class idVec3;
class idVec4;

// the engine's math/Simd.h declares it before Str.h names it
class idCmdArgs;

// the memory tags (memTag_t); Heap.h's global new and delete replacement is never copied
#include "idlib/Heap.h"

// Heap.h's Mem_* functions, on malloc and free
void* Mem_Alloc16(const size_t size, const memTag_t tag);
void Mem_Free16(void* ptr);

inline void* Mem_Alloc(const size_t size, const memTag_t tag)
{
  return Mem_Alloc16(size, tag);
}

inline void Mem_Free(void* ptr)
{
  Mem_Free16(ptr);
}

void* Mem_ClearedAlloc(const size_t size, const memTag_t tag);
char* Mem_CopyString(const char* in);

// Heap.h's tagged new and delete, forwarded to the global operators. The copied code
// pairs a tagged new with a plain delete (new( TAG_STRING ) char[] with delete [], for
// example), which matches only because both reach the same global allocator (R4-2.10).
inline void* operator new(size_t size, memTag_t)
{
  return ::operator new(size);
}

inline void operator delete(void* p, memTag_t) noexcept
{
  ::operator delete(p);
}

inline void* operator new[](size_t size, memTag_t)
{
  return ::operator new[](size);
}

inline void operator delete[](void* p, memTag_t) noexcept
{
  ::operator delete[](p);
}

// clang-format off: the copied headers follow idlib/Lib.h's order, which they depend on

// memory management and arrays
#include "idlib/containers/Sort.h"
#include "idlib/containers/List.h"

// math
#include "idlib/math/Math.h"
#include "idlib/math/Vector.h"
#include "idlib/math/Matrix.h"
#include "idlib/math/Angles.h"

// text manipulation
#include "idlib/Str.h"
#include "idlib/StrStatic.h"
#include "idlib/Token.h"
#include "idlib/Lexer.h"
#include "idlib/CmdArgs.h"

// containers
#include "idlib/containers/HashIndex.h"
#include "idlib/containers/StrList.h"
#include "idlib/containers/StrPool.h"

// clang-format on

// hashing: idlib/hashing/MD5.h. Only the decl checksums call it, and they are unused
// here, so the shim's stub returns 0 (R5.10, R4-3.10).
unsigned int MD5_BlockChecksum(const void* data, size_t length);

// misc
class idRandom;
class idSerializer;
#include "idlib/Dict.h"

// idlib/LangDict.h's idStrId, which only Str.cpp's testStrId console command names; that
// command is never registered or run (the header itself needs idDynamicBlockAlloc)
class idStrId
{
public:
  explicit idStrId(const char* key) { Set(key); }

  void Set(const char* key);
  const char* GetLocalizedString() const;

private:
  int index = -1;
};
