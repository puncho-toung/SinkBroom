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

MessageQueue engineMessages;

// Error and FatalError messages that could not be kept
std::atomic<std::uint64_t> lostErrors{0};
// B0.1: messages dropped because the queue was full or memory ran out, and how many of
// them the drains have reported so far
std::atomic<std::uint64_t> droppedMessages{0};
std::atomic<std::uint64_t> reportedDrops{0};

void lose(const DeclMessageKind kind) noexcept
{
  ++droppedMessages;
  // an error that is lost still counts
  if (kind == DeclMessageKind::Error || kind == DeclMessageKind::FatalError)
  {
    ++lostErrors;
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
}

std::uint64_t takeDroppedEngineMessages() noexcept
{
  const auto dropped = droppedMessages.load();
  const auto reported = reportedDrops.exchange(dropped);
  return dropped > reported ? dropped - reported : 0;
}

std::uint64_t lostEngineErrors() noexcept
{
  return lostErrors.load();
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
  // B0.1: the first drain after messages were dropped says how many
  if (const auto dropped = shim::takeDroppedEngineMessages())
  {
    messages.push_back(DeclMessage{
      DeclMessageKind::Warning,
      std::to_string(dropped) + " engine messages were dropped: the queue holds at most "
        + std::to_string(shim::MaxQueuedEngineMessages)});
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
