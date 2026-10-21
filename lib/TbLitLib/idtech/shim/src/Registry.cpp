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

  common = &registry.m_common;
  idLib::common = &registry.m_common;
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
  idLib::common = nullptr;
  common = nullptr;

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

void Registry::report(const DeclMessageKind kind, const char* text)
{
  auto message = std::string{text};

  // the engine's prints end in a newline; the messages do not
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
  {
    message.pop_back();
  }

  if (kind == DeclMessageKind::Error || kind == DeclMessageKind::FatalError)
  {
    ++m_errorCount;
  }
  m_messages.push_back(DeclMessage{kind, std::move(message)});
}

std::vector<DeclMessage> Registry::takeMessages()
{
  return std::exchange(m_messages, {});
}

void Registry::beginCapture()
{
  m_capturing = true;
  m_capture.clear();
}

std::string Registry::endCapture()
{
  m_capturing = false;
  return std::exchange(m_capture, {});
}

void Registry::capture(const char* text)
{
  m_capture += text;
}

void reportV(const DeclMessageKind kind, const char* fmt, va_list argptr)
{
  char text[MAX_PRINT_MSG];
  idStr::vsnPrintf(text, sizeof(text), fmt, argptr);
  if (auto* registry = Registry::current())
  {
    registry->report(kind, text);
  }
}

void reportWarning(const std::string& text)
{
  if (auto* registry = Registry::current())
  {
    registry->report(DeclMessageKind::Warning, text.c_str());
  }
}

void freeGuiDummies()
{
  if (currentRegistry)
  {
    currentRegistry->m_guis.Shutdown();
  }
}

} // namespace tb::lit::shim
