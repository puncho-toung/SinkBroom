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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/Thread.h and idlib/sys/sys_includes.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012 Robert Beckebans
//   Copyright (C) 2012 Daniel Gibson
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// The system layer the copied engine files expect from idlib/sys: the C and C++ headers
// sys_includes.h pulls in (without windows.h, DirectInput and DirectSound, which nothing
// copied uses), the copied sys_defines.h, sys_assert.h, sys_types.h and sys_intrinsics.h,
// a mutex for idDeclManagerLocal and the ID_TIME_T define of the engine's precompiled.h.
//
// Every standard header the shim and the facade need is included here, before any copied
// header, because idlib/Str.h #defines strcmp, stricmp and friends to idStr functions.

// sys_includes.h: "don't generate asserts" when neither _DEBUG nor NDEBUG is set
#if !defined(_DEBUG) && !defined(NDEBUG)
#define NDEBUG
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// clang-format off: the order is the engine's precompiled.h order
#include "idlib/sys/sys_defines.h"
#include "idlib/sys/sys_assert.h"
#include "idlib/sys/sys_types.h"
#include "idlib/sys/sys_intrinsics.h"
// clang-format on

// idlib/Thread.h's idSysMutex and idScopedCriticalSection, on a recursive mutex: the
// engine's Windows critical section is recursive, and FindType locks again when a
// material parse looks up a table. Every call into the copied code runs on one thread
// (D32).
class idSysMutex
{
public:
  idSysMutex() = default;
  idSysMutex(const idSysMutex&) = delete;
  idSysMutex& operator=(const idSysMutex&) = delete;

  bool Lock(bool blocking = true)
  {
    if (blocking)
    {
      m_mutex.lock();
      return true;
    }
    return m_mutex.try_lock();
  }

  void Unlock() { m_mutex.unlock(); }

private:
  std::recursive_mutex m_mutex;
};

class idScopedCriticalSection
{
public:
  explicit idScopedCriticalSection(idSysMutex& mutex)
    : m_mutex{&mutex}
  {
    m_mutex->Lock();
  }

  ~idScopedCriticalSection() { m_mutex->Unlock(); }

  idScopedCriticalSection(const idScopedCriticalSection&) = delete;
  idScopedCriticalSection& operator=(const idScopedCriticalSection&) = delete;

private:
  idSysMutex* m_mutex;
};

// engine precompiled.h: signed because -1 means "file not found"
#define ID_TIME_T int64
