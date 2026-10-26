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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/Heap.cpp, idlib/math/Simd_Generic.cpp and
// renderer/tr_frontend_main.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2014 Robert Beckebans
//   Copyright (C) 2012 Daniel Gibson
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// idLib's services for the copied code: the idLib wrappers, memory, the scalar
// SIMDProcessor, the MD5 stub, asserts and the localisation stubs. idLib::common is the
// permanent sink of D63; the other pointers here are bound by the registry (C17, S6).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/EngineMessages.h"
// clang-format on

using tb::lit::shim::reportV;

idSys* idLib::sys = nullptr;
idCommon* idLib::common = &tbLitCommonSink;
idCVarSystem* idLib::cvarSystem = nullptr;
idFileSystem* idLib::fileSystem = nullptr;
int idLib::frameNumber = 0;

void idLib::Printf(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Info, fmt, argptr);
  va_end(argptr);
}

void idLib::PrintfIf(const bool test, const char* fmt, ...)
{
  if (test)
  {
    va_list argptr;
    va_start(argptr, fmt);
    reportV(tb::lit::DeclMessageKind::Info, fmt, argptr);
    va_end(argptr);
  }
}

void idLib::Error(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Error, fmt, argptr);
  va_end(argptr);
}

void idLib::FatalError(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::FatalError, fmt, argptr);
  va_end(argptr);
}

void idLib::Warning(const char* fmt, ...)
{
  va_list argptr;
  va_start(argptr, fmt);
  reportV(tb::lit::DeclMessageKind::Warning, fmt, argptr);
  va_end(argptr);
}

void idLib::WarningIf(const bool test, const char* fmt, ...)
{
  if (test)
  {
    va_list argptr;
    va_start(argptr, fmt);
    reportV(tb::lit::DeclMessageKind::Warning, fmt, argptr);
    va_end(argptr);
  }
}

// Heap.cpp's Mem_Alloc16 returns NULL for a zero size and pads to 16 bytes; malloc
// already aligns to 16 on the 64-bit targets (S25)
void* Mem_Alloc16(const size_t size, const memTag_t)
{
  if (!size)
  {
    return nullptr;
  }
  return malloc((size + 15) & ~size_t{15});
}

void Mem_Free16(void* ptr)
{
  free(ptr);
}

void* Mem_ClearedAlloc(const size_t size, const memTag_t tag)
{
  void* mem = Mem_Alloc(size, tag);
  if (mem)
  {
    memset(mem, 0, size);
  }
  return mem;
}

char* Mem_CopyString(const char* in)
{
  auto* out = static_cast<char*>(Mem_Alloc(strlen(in) + 1, TAG_STRING));
  memcpy(out, in, strlen(in) + 1);
  return out;
}

unsigned int MD5_BlockChecksum(const void*, size_t)
{
  return 0;
}

// sys_assert.h: an assert in the copied code is reported to the sink of D63, from any
// thread, and execution continues; it never changes a parse's outcome, so Debug and
// Release parse alike (an Assert message is not an error, Registry::report). The copied
// va keeps its buffers per thread.
bool AssertFailed(const char* file, int line, const char* expression)
{
  tb::lit::shim::report(
    tb::lit::DeclMessageKind::Assert, va("%s(%d): %s", file, line, expression));
  return false;
}

// LangDict.h's idStrId: only the never-registered testStrId command of Str.cpp names it
void idStrId::Set(const char*)
{
  m_index = -1;
}

const char* idStrId::GetLocalizedString() const
{
  return "";
}

// Lib.cpp:364-367 LittleShort, through the function Swap_Init picks: ShortNoSwap on a
// little-endian host, ShortSwap on a big-endian one. Pure; any thread.
short LittleShort(const short l)
{
  const auto one = std::uint16_t{1};
  auto first = std::uint8_t{0};
  memcpy(&first, &one, 1);
  if (first == 1)
  {
    return l;
  }
  const auto bits = std::uint16_t(l);
  return short(std::uint16_t((bits << 8) | (bits >> 8)));
}

// RenderCommon.h's static allocators (tr_frontend_main.cpp:205-240)
void* R_StaticAlloc(int bytes, const memTag_t tag)
{
  void* buf = Mem_Alloc(size_t(bytes), tag);

  // as the engine: no failure on zero length allocations
  if (buf == nullptr && bytes != 0)
  {
    common->FatalError("R_StaticAlloc failed on %i bytes", bytes);
  }
  return buf;
}

void* R_ClearedStaticAlloc(int bytes)
{
  void* buf = R_StaticAlloc(bytes);
  if (buf)
  {
    memset(buf, 0, size_t(bytes));
  }
  return buf;
}

void R_StaticFree(void* data)
{
  Mem_Free(data);
}

/**
 * LP7.1a: math/Simd_Generic.h's idSIMD_Generic, reduced to the two MinMax overloads the
 * copied ranges below define (Simd_Generic.cpp:81-88, :109-116), without the virtual
 * base; idSIMDProcessor calls them. Stateless, so any thread may call them (C24 (3)).
 */
class idSIMD_Generic
{
public:
  void VPCALL MinMax(idVec2& min, idVec2& max, const idVec2* src, const int count);
  void VPCALL MinMax(idVec3& min, idVec3& max, const idDrawVert* src, const int count);
};

// the copied ranges (engine/idlib/math/Simd_Generic.cpp is HEADER_FILE_ONLY in the build)
#include "idlib/math/Simd_Generic.cpp"

void idSIMDProcessor::MinMax(
  idVec2& min, idVec2& max, const idVec2* src, const int count) const
{
  idSIMD_Generic{}.MinMax(min, max, src, count);
}

void idSIMDProcessor::MinMax(
  idVec3& min, idVec3& max, const idDrawVert* src, const int count) const
{
  idSIMD_Generic{}.MinMax(min, max, src, count);
}

// Simd_Generic.cpp:95-102 idSIMD_Generic::MinMax (idVec3 points), with UNROLL1 written
// out
void idSIMDProcessor::MinMax(
  idVec3& min, idVec3& max, const idVec3* src, const int count) const
{
  min[0] = min[1] = min[2] = idMath::INFINITUM;
  max[0] = max[1] = max[2] = -idMath::INFINITUM;
  for (int i = 0; i < count; i++)
  {
    const idVec3& v = src[i];
    if (v[0] < min[0])
    {
      min[0] = v[0];
    }
    if (v[0] > max[0])
    {
      max[0] = v[0];
    }
    if (v[1] < min[1])
    {
      min[1] = v[1];
    }
    if (v[1] > max[1])
    {
      max[1] = v[1];
    }
    if (v[2] < min[2])
    {
      min[2] = v[2];
    }
    if (v[2] > max[2])
    {
      max[2] = v[2];
    }
  }
}

namespace
{
// C24 (3): the one scalar SIMDProcessor, constant-initialised, never written
constexpr idSIMDProcessor tbLitScalarSimd;
} // namespace

const idSIMDProcessor* const SIMDProcessor = &tbLitScalarSimd;
