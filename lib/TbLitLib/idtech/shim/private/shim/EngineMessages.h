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

// Included after precompiled.h. D63: the permanent, thread-safe sink behind the copied
// code's common and idLib::common. Every message the copied code or the shim reports is
// formatted on the calling thread and kept, tagged with that thread, in one queue under a
// mutex, until the thread's owner takes it: the registry takes its own thread's messages
// (Registry::messages, errorCount), and the owner of any other thread that runs copied
// code takes that thread's (drainEngineMessagesForThread in lit/MaterialDecls.h).
//
// The queue is constant-initialised and allocates only when a message is pushed (C9,
// S27; B0.6: checked by the compiler where it can, TB_LIT_CONSTINIT). It holds at most
// MaxQueuedEngineMessages (B0.1). Pushing never throws (C17): a message that cannot be
// kept, for want of memory or room, is counted instead, for the thread that lost it
// (B0.5, R2.5, R2.6), and that thread's next drain says how many were dropped and how
// many of them were errors, so a lost Error or FatalError counts as an error for that
// thread's registry or job alone. The queue's mutex is destroyed at exit (its destructor
// is not trivial on MSVC and Apple); every thread that pushes is joined before then.

#include "lit/MaterialDecls.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// B0.6: constant initialisation the compiler checks, where it can (TbLitIdLib is C++17)
#if defined(__cpp_constinit)
#define TB_LIT_CONSTINIT constinit
#elif defined(__clang__)
#define TB_LIT_CONSTINIT [[clang::require_constant_initialization]]
#elif defined(__GNUC__)
#define TB_LIT_CONSTINIT __constinit
#else
#define TB_LIT_CONSTINIT
#endif

namespace tb::lit::shim
{

// one message as the calling thread reported it, its text as formatted, newlines kept
struct EngineMessage
{
  DeclMessageKind kind = DeclMessageKind::Info;
  // idCommon::Printf's console text, which a registry that captures a print command keeps
  // as the engine's console gets it (Registry::beginCapture)
  bool consolePrint = false;
  std::string text;
};

// formats the message with the copied idStr::vsnPrintf into MAX_PRINT_MSG bytes, as the
// engine's common does, and pushes it for the calling thread; any thread
void reportV(
  DeclMessageKind kind,
  const char* fmt,
  va_list argptr,
  bool consolePrint = false) noexcept;
// pushes text as it is for the calling thread; any thread
void report(DeclMessageKind kind, const char* text) noexcept;
void reportWarning(const std::string& text) noexcept;

// the messages of one thread, oldest first, taken out of the queue; any thread
std::vector<EngineMessage> takeEngineMessages(std::thread::id thread);
// drops the messages of one thread, and what it lost; any thread
void discardEngineMessages(std::thread::id thread) noexcept;

// B0.1: the queue keeps at most this many messages of all threads; beyond it a message is
// dropped and counted (lit/MaterialDecls.h)
inline constexpr std::size_t MaxQueuedEngineMessages =
  MaxQueuedEngineMessagesOfAllThreads;

// what one thread lost since its last take: every dropped message, and the Error and
// FatalError messages among them
struct EngineLosses
{
  std::uint64_t dropped = 0;
  std::uint64_t errors = 0;
};
// B0.5, R2.5, R2.6: what the thread lost, taken; any thread
EngineLosses takeEngineLosses(std::thread::id thread) noexcept;
// the lines a drain of the thread ends with for what it lost: a Warning with the count
// of dropped messages, and an Error with the count of errors among them
std::vector<DeclMessage> lossMessages(const EngineLosses& losses);
// what was lost by threads that found no slot of their own (more than LossSlots threads
// losing at once); never reported to any drain
std::uint64_t unattributedEngineLossesForTesting() noexcept;

// a message's text as DeclMessage carries it: without its trailing newlines
std::string messageText(std::string text);

} // namespace tb::lit::shim
