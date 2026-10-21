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
// S27). It holds at most MaxQueuedEngineMessages (B0.1). Pushing never throws (C17): a
// message that cannot be kept, for want of memory or room, is counted instead, the next
// drain says how many were dropped, and a lost Error or FatalError counts as an error for
// the registry.

#include "lit/MaterialDecls.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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
// drops the messages of one thread; any thread
void discardEngineMessages(std::thread::id thread) noexcept;
// the Error and FatalError messages that could not be kept since the program started
std::uint64_t lostEngineErrors() noexcept;

// B0.1: the queue keeps at most this many messages of all threads; beyond it a message is
// dropped and counted (a dropped Error or FatalError also as a lost error)
inline constexpr std::size_t MaxQueuedEngineMessages = 4096;
// the messages dropped since the last call; any thread
std::uint64_t takeDroppedEngineMessages() noexcept;

// a message's text as DeclMessage carries it: without its trailing newlines
std::string messageText(std::string text);

} // namespace tb::lit::shim
