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

// The renderer, sound, GUI and game layer of the engine's precompiled.h, as far as the
// copied material parser reaches it: dummies for cinematics, sound shaders, GUIs and the
// game, and the copied Material.h.

// idlib/precompiled.h:87-88, shared by materials, sound shaders and GUIs
const int MAX_EXPRESSION_OPS = 4096;
const int MAX_EXPRESSION_REGISTERS = 4096;

namespace nvrhi
{
class ICommandList;
}

// renderer/Cinematic.h. A videomap or soundmap stage owns one of these;
// idMaterial::FreeData deletes it through the virtual destructor, and nothing else tracks
// it (R5.11).
class idCinematic
{
public:
  static idCinematic* Alloc();

  virtual ~idCinematic() = default;

  // records the file the stage names; nothing is opened
  virtual bool InitFromFile(
    const char* qpath, bool looping, nvrhi::ICommandList* commandList);

  const char* GetFileName() const { return m_fileName.c_str(); }
  bool IsLooping() const { return m_looping; }
  virtual bool IsSoundWindow() const { return false; }

private:
  idStr m_fileName;
  bool m_looping = false;
};

// renderer/Cinematic.h's idSndWindow. Its own InitFromFile( qpath, looping ) hides the
// base overload without overriding it, so the material's three-argument call reaches
// idCinematic::InitFromFile, as in the engine.
class idSndWindow : public idCinematic
{
public:
  bool IsSoundWindow() const override { return true; }
};

#include "renderer/Material.h"

// sound/sound.h: only the amplitude EvaluateRegisters asks for; no emitter is ever passed
class idSoundEmitter
{
public:
  virtual ~idSoundEmitter() = default;
  virtual float CurrentAmplitude() = 0;
};

// sound/sound.h's sound shader decl, a stub type: its text is kept and skipped, as a
// plain idDecl skips it. idDeclManagerLocal::FindSound casts to it.
class idSoundShader : public idDecl
{
};

// ui/UserInterface.h: a GUI dummy that remembers the file a material's "gui" keyword
// names
class idUserInterface
{
public:
  explicit idUserInterface(const char* sourceFile)
    : m_sourceFile{sourceFile}
  {
  }

  const char* GetSourceFile() const { return m_sourceFile.c_str(); }

private:
  idStr m_sourceFile;
};

class idUserInterfaceManager
{
public:
  virtual ~idUserInterfaceManager() = default;

  virtual idUserInterface* FindGui(
    const char* qpath,
    bool autoLoad = false,
    bool needUnique = false,
    bool forceUnique = false) = 0;
};

extern idUserInterfaceManager* uiManager;

// d3xp/Game.h: the entityDef parser asks the game to precache a dict's media; the preview
// loads no media through the game
class idGame
{
public:
  virtual ~idGame() = default;
  virtual void CacheDictionaryMedia(const idDict* dict) = 0;
};

extern idGame* game;
