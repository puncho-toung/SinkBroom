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

// D63: the queue behind the permanent sink (shim/EngineMessages.h) and the facade's
// functions that drain it (lit/MaterialDecls.h).

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/EngineMessages.h"
// clang-format on

#include <array>
#include <atomic>
#include <functional>

namespace tb::lit::shim
{
namespace
{

struct Node
{
  std::thread::id thread;
  EngineMessage message;
  Node* next = nullptr;
};

void freeChain(Node* node) noexcept
{
  while (node)
  {
    auto* next = node->next;
    delete node;
    node = next;
  }
}

struct ChainDeleter
{
  void operator()(Node* node) const noexcept { freeChain(node); }
};

using Chain = std::unique_ptr<Node, ChainDeleter>;

/**
 * The messages of every thread in the order they were pushed, as a list of nodes under a
 * mutex. Its constructor is constexpr and neither member allocates, so the process's one
 * queue is constant-initialised (C9, S27); a node is allocated only when a message is
 * pushed. Nodes still queued when the process ends are not freed.
 */
class MessageQueue
{
private:
  mutable std::mutex m_mutex;
  Node* m_head = nullptr;
  Node* m_tail = nullptr;
  std::size_t m_size = 0;

public:
  constexpr MessageQueue() = default;

  // B0.1: false, and the node is not taken, when the queue already holds bound nodes
  bool push(Node* node, const std::size_t bound) noexcept
  {
    const auto lock = std::lock_guard{m_mutex};
    if (m_size >= bound)
    {
      return false;
    }
    if (m_tail)
    {
      m_tail->next = node;
    }
    else
    {
      m_head = node;
    }
    m_tail = node;
    ++m_size;
    return true;
  }

  // unlinks the thread's nodes into a chain of their own, in their order
  Node* take(const std::thread::id thread) noexcept
  {
    const auto lock = std::lock_guard{m_mutex};
    Node* taken = nullptr;
    Node** takenTail = &taken;
    Node* previous = nullptr;
    for (auto* node = m_head; node;)
    {
      auto* next = node->next;
      if (node->thread == thread)
      {
        if (previous)
        {
          previous->next = next;
        }
        else
        {
          m_head = next;
        }
        if (m_tail == node)
        {
          m_tail = previous;
        }
        node->next = nullptr;
        *takenTail = node;
        takenTail = &node->next;
        --m_size;
      }
      else
      {
        previous = node;
      }
      node = next;
    }
    return taken;
  }

  std::size_t size() const noexcept
  {
    const auto lock = std::lock_guard{m_mutex};
    return m_size;
  }
};

TB_LIT_CONSTINIT MessageQueue engineMessages;

/**
 * B0.5, R2.5, R2.6: what a thread lost (the queue full, or no memory for a node), kept
 * for that thread in a fixed table, so that counting allocates nothing and a thread that
 * cannot allocate still counts. A thread claims a slot by its id's hash at its first
 * loss; the slot is freed when the thread's messages are discarded (its owner does that
 * when it starts and when it ends), so a later thread with a recycled id inherits
 * nothing.
 */
struct LossSlot
{
  // the owning thread's id hash, 0 for a free slot
  std::atomic<std::uint64_t> owner{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> errors{0};
};

constexpr auto LossSlots = std::size_t{64};
TB_LIT_CONSTINIT std::array<LossSlot, LossSlots> losses;
TB_LIT_CONSTINIT std::atomic<std::uint64_t> unattributedLosses{0};

std::uint64_t threadKey(const std::thread::id thread) noexcept
{
  const auto key = std::uint64_t(std::hash<std::thread::id>{}(thread));
  return key == 0 ? 1 : key;
}

LossSlot* lossSlot(const std::thread::id thread, const bool claim) noexcept
{
  const auto key = threadKey(thread);
  // V.7: the thread's own slot first, wherever it is, so that it never claims a second
  for (auto i = std::size_t{0}; i < LossSlots; ++i)
  {
    auto& slot = losses[(std::size_t(key) + i) % LossSlots];
    if (slot.owner.load() == key)
    {
      return &slot;
    }
  }
  for (auto i = std::size_t{0}; claim && i < LossSlots; ++i)
  {
    auto& slot = losses[(std::size_t(key) + i) % LossSlots];
    auto owner = std::uint64_t{0};
    if (slot.owner.compare_exchange_strong(owner, key))
    {
      return &slot;
    }
  }
  return nullptr;
}

void lose(const DeclMessageKind kind) noexcept
{
  auto* slot = lossSlot(std::this_thread::get_id(), true);
  if (!slot)
  {
    ++unattributedLosses;
    return;
  }
  ++slot->dropped;
  // an error that is lost still counts, for this thread
  if (kind == DeclMessageKind::Error || kind == DeclMessageKind::FatalError)
  {
    ++slot->errors;
  }
}

void push(const DeclMessageKind kind, const bool consolePrint, const char* text) noexcept
{
  try
  {
    auto node = std::make_unique<Node>();
    node->thread = std::this_thread::get_id();
    node->message = EngineMessage{kind, consolePrint, std::string{text}};
    if (engineMessages.push(node.get(), MaxQueuedEngineMessages))
    {
      static_cast<void>(node.release());
      return;
    }
  }
  catch (...)
  {
    // C17: nothing is thrown through the copied code
  }
  lose(kind);
}

} // namespace

void reportV(
  const DeclMessageKind kind,
  const char* fmt,
  va_list argptr,
  const bool consolePrint) noexcept
{
  // the copied idStr::vsnPrintf reads only its arguments (M-ID-PURE-02)
  char text[MAX_PRINT_MSG];
  idStr::vsnPrintf(text, sizeof(text), fmt, argptr);
  push(kind, consolePrint, text);
}

void report(const DeclMessageKind kind, const char* text) noexcept
{
  push(kind, false, text);
}

void reportWarning(const std::string& text) noexcept
{
  push(DeclMessageKind::Warning, false, text.c_str());
}

std::vector<EngineMessage> takeEngineMessages(const std::thread::id thread)
{
  // the chain is freed also when the copy below runs out of memory
  const auto chain = Chain{engineMessages.take(thread)};
  auto messages = std::vector<EngineMessage>{};
  for (auto* node = chain.get(); node; node = node->next)
  {
    messages.push_back(std::move(node->message));
  }
  return messages;
}

void discardEngineMessages(const std::thread::id thread) noexcept
{
  freeChain(engineMessages.take(thread));
  if (auto* slot = lossSlot(thread, false))
  {
    slot->dropped = 0;
    slot->errors = 0;
    slot->owner = 0;
  }
}

EngineLosses takeEngineLosses(const std::thread::id thread) noexcept
{
  auto* slot = lossSlot(thread, false);
  return slot ? EngineLosses{slot->dropped.exchange(0), slot->errors.exchange(0)}
              : EngineLosses{};
}

std::vector<DeclMessage> lossMessages(const EngineLosses& lost)
{
  auto messages = std::vector<DeclMessage>{};
  if (lost.dropped > 0)
  {
    messages.push_back(DeclMessage{
      DeclMessageKind::Warning,
      std::to_string(lost.dropped)
        + " engine messages of this thread were dropped: the queue holds at most "
        + std::to_string(MaxQueuedEngineMessages)});
  }
  if (lost.errors > 0)
  {
    messages.push_back(DeclMessage{
      DeclMessageKind::Error, std::to_string(lost.errors) + " of them were errors"});
  }
  return messages;
}

std::uint64_t unattributedEngineLossesForTesting() noexcept
{
  return unattributedLosses.load();
}

std::string messageText(std::string text)
{
  // the engine's prints end in a newline; the messages do not
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
  {
    text.pop_back();
  }
  return text;
}

} // namespace tb::lit::shim

namespace tb::lit
{

std::vector<DeclMessage> drainEngineMessagesForThread(const std::thread::id thread)
{
  auto messages = std::vector<DeclMessage>{};
  for (auto& message : shim::takeEngineMessages(thread))
  {
    messages.push_back(
      DeclMessage{message.kind, shim::messageText(std::move(message.text))});
  }
  // B0.1, R2.5: the thread's first drain after it lost messages says how many
  for (auto& message : shim::lossMessages(shim::takeEngineLosses(thread)))
  {
    messages.push_back(std::move(message));
  }
  return messages;
}

void discardEngineMessagesForThread(const std::thread::id thread) noexcept
{
  shim::discardEngineMessages(thread);
}

std::size_t pendingEngineMessagesForTesting()
{
  return shim::engineMessages.size();
}

void reportEngineMessageForTesting(const DeclMessageKind kind, const std::string& text)
{
  // through both of the copied code's pointers, as it calls them
  switch (kind)
  {
  case DeclMessageKind::Info:
    idLib::common->Printf("%s\n", text.c_str());
    break;
  case DeclMessageKind::Warning:
    idLib::common->Warning("%s", text.c_str());
    break;
  case DeclMessageKind::Error:
    common->Error("%s", text.c_str());
    break;
  case DeclMessageKind::FatalError:
    common->FatalError("%s", text.c_str());
    break;
  case DeclMessageKind::Assert:
    AssertFailed("EngineMessages.cpp", 0, text.c_str());
    break;
  }
}

} // namespace tb::lit
