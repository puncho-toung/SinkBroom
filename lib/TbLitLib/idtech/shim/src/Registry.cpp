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

// the one bound registry (C17); written only by the registry's constructor and destructor
Registry* currentRegistry = nullptr;

} // namespace

Registry* Registry::current()
{
  return currentRegistry;
}

Registry::Registry(const DeclSource& source)
  : m_fileSystem{source}
{
  assert(currentRegistry == nullptr);
  currentRegistry = this;

  // binds declManager and declManagerLocal
  m_declManager = createDeclManager();

  common = &m_common;
  idLib::common = &m_common;
  cvarSystem = &m_cvarSystem;
  idLib::cvarSystem = &m_cvarSystem;
  fileSystem = &m_fileSystem;
  idLib::fileSystem = &m_fileSystem;
  globalImages = &m_images;
  tbLitRenderProgManager = &m_renderProgs;
  uiManager = &m_guis;
  game = &m_game;

  // idLib::Init: the dict pools' case rules
  idDict::Init();

  // the renderer creates its intrinsic images before any material is parsed
  m_images.Init();

  initDeclManager(*m_declManager);
}

Registry::~Registry()
{
  // decls, image records, GUI dummies, decl text (R5.11), then the decl manager itself;
  // this unbinds declManager and declManagerLocal
  destroyDeclManager(m_declManager);
  m_declManager = nullptr;

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

void Registry::report(const DeclMessageKind kind, const char* text)
{
  auto message = std::string{text};

  // the engine's prints end in a newline; the messages do not
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
  {
    message.pop_back();
  }

  if (
    kind == DeclMessageKind::Error || kind == DeclMessageKind::FatalError
    || kind == DeclMessageKind::Assert)
  {
    ++m_errorCount;
  }
  m_messages.push_back(DeclMessage{kind, std::move(message)});
}

std::vector<DeclMessage> Registry::takeMessages()
{
  return std::exchange(m_messages, {});
}

void freeGuiDummies()
{
  if (currentRegistry)
  {
    currentRegistry->m_guis.Shutdown();
  }
}

} // namespace tb::lit::shim
