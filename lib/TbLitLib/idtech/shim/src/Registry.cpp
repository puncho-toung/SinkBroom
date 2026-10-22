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

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "shim/EngineMessages.h"
#include "shim/Registry.h"
// clang-format on

namespace tb::lit::shim
{
namespace
{

// the one bound registry (C17); written only by the bindings
Registry* currentRegistry = nullptr;

// the process's one registry slot (C17, A2.6), claimed before a registry is made
auto registryClaimed = std::atomic<bool>{false};

} // namespace

Registry* Registry::current()
{
  return currentRegistry;
}

bool Registry::claim()
{
  auto expected = false;
  return registryClaimed.compare_exchange_strong(expected, true);
}

void Registry::release()
{
  registryClaimed = false;
}

Registry::Bindings::Bindings(Registry& registry)
{
  currentRegistry = &registry;

  // common and idLib::common stay the permanent sink of D63
  cvarSystem = &registry.m_cvarSystem;
  idLib::cvarSystem = &registry.m_cvarSystem;
  fileSystem = &registry.m_fileSystem;
  idLib::fileSystem = &registry.m_fileSystem;
  globalImages = &registry.m_images;
  tbLitRenderProgManager = &registry.m_renderProgs;
  uiManager = &registry.m_guis;
  game = &registry.m_game;

  // idLib::Init: the dict pools' case rules
  idDict::Init();
}

Registry::Bindings::~Bindings()
{
  // idLib::ShutDown: every entityDef dict is cleared by now, so the pools are empty
  idDict::Shutdown();

  game = nullptr;
  uiManager = nullptr;
  tbLitRenderProgManager = nullptr;
  globalImages = nullptr;
  idLib::fileSystem = nullptr;
  fileSystem = nullptr;
  idLib::cvarSystem = nullptr;
  cvarSystem = nullptr;

  currentRegistry = nullptr;
}

Registry::DeclManagerOwner::DeclManagerOwner()
  // binds declManager and declManagerLocal
  : manager{createDeclManager()}
{
}

Registry::DeclManagerOwner::~DeclManagerOwner()
{
  // decls, image records, GUI dummies, decl text (R5.11), then the decl manager itself;
  // this unbinds declManager and declManagerLocal
  destroyDeclManager(manager);
}

Registry::Registry(DeclSource& source, const std::atomic<bool>* cancel)
  : m_fileSystem{source, cancel}
  , m_bindings{*this}
{
  // the renderer creates its intrinsic images before any material is parsed
  m_images.Init();

  initDeclManager(*m_declManager.manager);
}

// the members go in reverse order: the decl manager, then the bindings, then the objects
Registry::~Registry() = default;

Registry::OwnThread::OwnThread()
  : m_id{std::this_thread::get_id()}
{
  discardEngineMessages(m_id);
}

Registry::OwnThread::~OwnThread()
{
  discardEngineMessages(m_id);
}

void Registry::drain() const
{
  for (auto& message : takeEngineMessages(m_thread.id()))
  {
    if (m_capturing && message.consolePrint)
    {
      m_capture += message.text;
    }
    else
    {
      report(message.kind, std::move(message.text));
    }
  }
  // R2.5, R2.6: what this thread lost: the drops said, every lost error counted
  const auto lost = takeEngineLosses(m_thread.id());
  for (auto& message : lossMessages(lost))
  {
    m_messages.push_back(std::move(message));
  }
  m_errorCount += std::size_t(lost.errors);
}

void Registry::report(const DeclMessageKind kind, std::string text) const
{
  if (kind == DeclMessageKind::Error || kind == DeclMessageKind::FatalError)
  {
    ++m_errorCount;
  }
  m_messages.push_back(DeclMessage{kind, messageText(std::move(text))});
}

const std::vector<DeclMessage>& Registry::messages() const
{
  drain();
  return m_messages;
}

std::vector<DeclMessage> Registry::takeMessages()
{
  drain();
  return std::exchange(m_messages, {});
}

std::vector<DeclMessage> Registry::takeMessagesFrom(const std::size_t first)
{
  drain();
  if (first >= m_messages.size())
  {
    return {};
  }
  auto taken = std::vector<DeclMessage>(
    std::make_move_iterator(m_messages.begin() + std::ptrdiff_t(first)),
    std::make_move_iterator(m_messages.end()));
  m_messages.resize(first);
  return taken;
}

std::size_t Registry::errorCount() const
{
  drain();
  return m_errorCount;
}

void Registry::beginCapture()
{
  // what the thread printed before goes into the messages
  drain();
  m_capturing = true;
  m_capture.clear();
}

std::string Registry::endCapture()
{
  drain();
  m_capturing = false;
  return std::exchange(m_capture, {});
}

void freeGuiDummies()
{
  if (currentRegistry)
  {
    currentRegistry->m_guis.Shutdown();
  }
}

} // namespace tb::lit::shim
